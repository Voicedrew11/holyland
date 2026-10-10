// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/vblank_field_pacer.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>
using namespace std::chrono_literals;
using Time=VBlankFieldPacer::TimePoint;
constexpr auto period=16683333ns;
constexpr auto blank=500us;
constexpr uint64_t cycles=(294912000ull*1001ull+59999ull)/60000ull;
uint64_t checks=0;
void check(bool yes,const char* message){++checks;if(!yes)throw std::runtime_error(message);}
struct Event{Time start,next;uint64_t cycle;};
std::vector<Event> run(const std::vector<int64_t>& work,unsigned presentationRate){
    VBlankFieldPacer pacer;pacer.reset(Time{},period,blank);
    Time now{},deadline=pacer.firstStart();std::array<Time,2> prior{};
    std::array<bool,2> have{};std::vector<Event> out;out.reserve(work.size());
    uint64_t tick=0;
    for(const auto busy:work){
        const unsigned reads=presentationRate?std::max(1u,presentationRate/60):64u;
        if(tick)for(unsigned i=0;i<reads;++i)
            check(pacer.nextStartAfter(now,unsigned((tick+1)&1))==deadline,"Presentation changes physical field deadline");
        now=std::max(now+std::chrono::microseconds(busy),deadline);
        const unsigned parity=unsigned(++tick&1);pacer.recordStart(now,parity);
        if(have[parity])check(now-prior[parity]>=2*period,"Same parity exceeds original NTSC cadence");
        prior[parity]=now;have[parity]=true;
        deadline=pacer.nextStartAfter(now,parity^1);
        check(deadline>now+blank,"Next Start overtakes prior End");
        check(deadline>now,"Backlog burst invents immediate field");
        check(cycles==4920116,"Exact ceil rational guest cycle offset");
        out.push_back({now,deadline,tick*cycles});
    }
    return out;
}
int main(){try{
    check(2*period.count()*9216000ull/1000000000ull>307399,"NTSC two fields exceed owned Timer0 limiter threshold");
    check(33334000ull*9216000ull/1000000000ull<307399,"Exact60 two fields expose threshold edge");
    VBlankFieldPacer pacer;pacer.reset(Time{},period,blank);
    check(pacer.firstStart()==Time{}+period,"First physical field uses NTSC duration");
    pacer.recordStart(Time{},1);pacer.recordStart(Time{}+25ms,0);
    check(pacer.nextStartAfter(Time{}+25ms,1)==Time{}+2*period,"Late opposite field retains NTSC same-parity phase");
    pacer.recordStart(Time{}+35ms,0);
    check(pacer.nextStartAfter(Time{}+35ms,1)==Time{}+35ms+blank+VBlankFieldPacer::Duration{1},"Overrun never adds a phase-grid idle slot");
    const auto zero=run(std::vector<int64_t>(2000,0),60);
    for(size_t i=0;i<zero.size();++i)
        check(zero[i].start==Time{}+period*int64_t(i+1),"Zero work retains rational NTSC phase");
    std::mt19937 random(0x10016000);std::vector<int64_t> work(200000);
    for(size_t i=0;i<work.size();++i)work[i]=i%2?random()%200001u:std::array<int64_t,10>{0,1,499,500,501,16683,16684,25000,35000,10000000}[random()%10];
    const auto expected=run(work,60);
    for(const unsigned rate:{300u,0u}){
        const auto actual=run(work,rate);check(actual.size()==expected.size(),"Presentation creates guest fields");
        for(size_t i=0;i<actual.size();++i)
            check(actual[i].start==expected[i].start&&actual[i].next==expected[i].next&&actual[i].cycle==expected[i].cycle,"Host presentation affects guest wall/cycle cadence");
    }
    std::printf("PASS NTSC pure checks=%llu random fields=200000 presentation=60/300/uncapped cycles=%llu\n",checks,cycles);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
