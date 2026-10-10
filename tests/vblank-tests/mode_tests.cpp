// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>
using namespace std::chrono_literals;
using Time=VBlankFieldPacer::TimePoint;
constexpr auto legacy=16667us;
constexpr auto ntsc=16683333ns;
constexpr auto blank=500us;
size_t checks{};
void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
struct FieldModeProof {
 struct State { std::vector<EeScheduler::ScheduledEvent> deadlines; uint64_t tick,cycle;Time epoch,expiry;std::vector<EeScheduler::VBlankHandoff> holds; };
 static State save(EeScheduler& ee){return {ee.m_deadlines,ee.m_vsyncTick,ee.m_eeCycle,ee.m_hostClockEpoch,ee.m_vblankHandoffExpiry,ee.m_vblankHandoffs};}
 static uint64_t cycles(VBlankFieldPacer::Duration d){return (uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count())*EeScheduler::kEeClockHz+999999999ull)/1000000000ull;}
 static void same(const State&a,const State&b){
  check(a.tick==b.tick&&a.cycle==b.cycle&&a.epoch==b.epoch,"mode change leaves ticks, elapsed cycles and wall epoch unchanged");
  check(a.deadlines.size()==b.deadlines.size(),"mode change creates no events");
  check(a.holds.size()==b.holds.size(),"mode change retains waiter holds");
  for(size_t i=0;i<a.holds.size();++i)check(a.holds[i].threadId==b.holds[i].threadId&&a.holds[i].continuationSequence==b.holds[i].continuationSequence,"mode change retains exact continuation identity");
  for(size_t i=0;i<a.deadlines.size();++i){
   check(a.deadlines[i].event.type==b.deadlines[i].event.type&&a.deadlines[i].sequence==b.deadlines[i].sequence,"mode change retains type and sequence");
   if(a.deadlines[i].event.type!=EeEventType::VBlankStart)
    check(a.deadlines[i].deadlineCycle==b.deadlines[i].deadlineCycle&&a.deadlines[i].hostDeadline==b.deadlines[i].hostDeadline,"mode change retains End/alarm/ordinary deadline");
  }
 }
 static void expect(EeScheduler& ee,VBlankFieldPacer::Duration d,const State& before){
  same(before,save(ee));check(ee.m_fieldPacer.period()==d,"selected field period matches mode");
  check(cycles(d)==(d==ntsc?4920116ull:4915299ull),"selected cycle offset is exact");
  for(size_t i=0;i<ee.m_deadlines.size();++i){auto& e=ee.m_deadlines[i];if(e.event.type==EeEventType::VBlankStart){
   auto host=ee.m_vsyncTick?ee.m_fieldPacer.nextStartAfter(ee.m_fieldPacer.lastStart(unsigned(ee.m_vsyncTick&1)),unsigned((ee.m_vsyncTick+1)&1)):ee.m_fieldPacer.firstStart();
   check(e.hostDeadline==host,"pending Start follows preserved physical history");
   check(e.deadlineCycle==before.deadlines[i].deadlineCycle-cycles(beforePeriod)+cycles(d),"pending Start adjusts only its prior cycle offset");
  }}
  if(!ee.m_vblankHandoffs.empty())check(ee.m_vblankHandoffExpiry==ee.m_fieldPacer.lastStart(unsigned(ee.m_vsyncTick&1))+d*2,"held Start timeout follows current mode");
 }
 static inline VBlankFieldPacer::Duration beforePeriod{};
 static void seed(EeScheduler& ee,unsigned tick,bool held){
  const auto epoch=std::chrono::steady_clock::now();ee.m_fieldPacer.reset(epoch,legacy,blank);ee.m_vsyncTick=tick;
  ee.m_eeCycle=1234567;ee.m_hostClockEpoch=epoch-1s;ee.m_deadlines.clear();ee.m_vblankHandoffs.clear();
  for(unsigned i=1;i<=tick;++i)ee.m_fieldPacer.recordStart(epoch+legacy*i,i&1);
  const auto last=epoch+legacy*tick;
  ee.m_deadlines.push_back({9000000+cycles(legacy),tick?ee.m_fieldPacer.nextStartAfter(last,(tick+1)&1):ee.m_fieldPacer.firstStart(),{EeEventType::VBlankStart,0,0},10});
  ee.m_deadlines.push_back({9000000+147456,last+blank,{EeEventType::VBlankEnd,0,tick},11});
  ee.m_deadlines.push_back({9018875,last+64us,{EeEventType::Alarm,4,33},12});
  if(held){ee.m_vblankHandoffs.push_back({1,99});ee.m_vblankHandoffExpiry=last+legacy*2;}
  ee.updateNextDeadline();
 }
 static void run(PS2Runtime& rt,uint8_t*rdram,R5900Context&ctx){auto& ee=rt.eeScheduler();
  for(unsigned tick:{0u,1u,2u,3u})for(bool held:{false,true}){if(held&&!tick)continue;seed(ee,tick,held);
   for(auto mode:std::vector<std::pair<uint32_t,uint32_t>>{{1,2},{1,2},{1,3},{0,2},{0,3},{1,0},{1,0x50},{1,2}}){
    beforePeriod=ee.m_fieldPacer.period();auto before=save(ee);ee.setGsVideoMode(mode.first,mode.second);auto d=(mode.first&1)&&mode.second==2?VBlankFieldPacer::Duration{ntsc}:VBlankFieldPacer::Duration{legacy};expect(ee,d,before);
   }
  }
  seed(ee,2,true);beforePeriod=ee.m_fieldPacer.period();auto before=save(ee);
  SET_GPR_U32(&ctx,4,1);SET_GPR_U32(&ctx,5,2);SET_GPR_U32(&ctx,6,1);ps2_syscalls::SetGsCrt(rdram,&ctx,&rt);expect(ee,ntsc,before);check(getRegU32(&ctx,2)==0,"actual SetGsCrt success unchanged");
  beforePeriod=ee.m_fieldPacer.period();before=save(ee);SET_GPR_U32(&ctx,4,0);SET_GPR_U32(&ctx,5,1);SET_GPR_U32(&ctx,6,3);SET_GPR_U32(&ctx,7,1);ps2_stubs::sceGsResetGraph(rdram,&ctx,&rt);expect(ee,legacy,before);check(getRegU32(&ctx,2)==0,"actual ResetGraph success unchanged");
  beforePeriod=ee.m_fieldPacer.period();before=save(ee);SET_GPR_U32(&ctx,4,0);SET_GPR_U32(&ctx,5,1);SET_GPR_U32(&ctx,6,2);SET_GPR_U32(&ctx,7,1);ps2_stubs::sceGsResetGraph(rdram,&ctx,&rt);expect(ee,ntsc,before);
  beforePeriod=ee.m_fieldPacer.period();before=save(ee);SET_GPR_U32(&ctx,4,1);SET_GPR_U32(&ctx,6,3);ps2_stubs::sceGsResetGraph(rdram,&ctx,&rt);expect(ee,ntsc,before);check(before.expiry==ee.m_vblankHandoffExpiry,"non-reset graph mode keeps current timeout");
 }
};
int main(){try{
 VBlankFieldPacer p;p.reset(Time{},legacy,blank);p.recordStart(Time{}+legacy,1);p.recordStart(Time{}+legacy*2,0);
 p.setPeriod(ntsc);check(p.firstStart()==Time{}+ntsc,"mode change preserves first epoch");check(p.lastStart(1)==Time{}+legacy&&p.lastStart(0)==Time{}+legacy*2,"mode change preserves field histories");
 std::mt19937 rng(0x12000000);auto now=Time{}+legacy*2;unsigned tick=2;
 for(unsigned i=0;i<100000;++i){auto period=(rng()&1)?VBlankFieldPacer::Duration{ntsc}:VBlankFieldPacer::Duration{legacy};p.setPeriod(period);auto old=p.lastStart((tick+1)&1);auto next=p.nextStartAfter(now,(tick+1)&1);check(next>=old+period*2,"mode transition preserves same-parity bound");check(next>now+blank,"mode transition preserves End order");now=std::max(next,now+std::chrono::microseconds(rng()%100001));p.recordStart(now,++tick&1);}
 auto rt=std::make_unique<PS2Runtime>();check(rt->memory().initialize(),"actual memory initializes");auto*rdram=rt->memory().getRDRAM();R5900Context ctx{};rt->eeScheduler().bindMainContextForSyscall(ctx,rdram);FieldModeProof::run(*rt,rdram,ctx);
 std::printf("PASS mode-aware field period checks=%zu transitions=100000\n",checks);return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
