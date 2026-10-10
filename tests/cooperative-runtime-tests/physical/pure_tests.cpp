// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/vblank_field_pacer.h"
#include <cstdio>
#include <cstdlib>
#include <random>

using Clock = VBlankFieldPacer::Clock;
using namespace std::chrono_literals;
size_t checks=0, failures=0;
void check(bool yes,const char* why){++checks;if(!yes){++failures;std::fprintf(stderr,"FAIL: %s\n",why);}}
int main(){
    const auto epoch=Clock::time_point{1s};
    constexpr auto period=16683333ns;
    VBlankFieldPacer p;
    p.reset(epoch,period,500us);
    check(p.advancePhysicalPhase(epoch+period-1ns,0)==0,"no slot before first fixed boundary");
    uint64_t n=p.advancePhysicalPhase(epoch+period,0);
    check(n==1&&p.physicalBoundary()==epoch+period,"first slot on physical boundary");
    n=p.advancePhysicalPhase(epoch+period*7+400us,n);
    check(n==7,"late observation skips directly to the latest physical slot");
    check(p.physicalBoundary()==epoch+period*7,"late delivery never rebases the phase");
    check(p.physicalBlankEnd()==epoch+period*7+500us,"blank remains tied to physical edge");
    check(p.advancePhysicalPhase(epoch+period*7+900us,n)==n,"repeated sampling never replays a missed edge");
    n=p.advancePhysicalPhase(epoch+period*10+700us,n);
    check(n==10&&(n&1)==0,"even and odd skip counts retain correct FIELD");
    const auto anchor=p.physicalBoundary();
    p.setPeriod(16667us);
    check(p.advancePhysicalPhase(anchor+16667us-1ns,n)==n,"mode changes only the future phase segment");
    n=p.advancePhysicalPhase(anchor+16667us,n);
    check(n==11&&p.physicalBoundary()==anchor+16667us,"mode transition preserves monotonic slot identity");

    // A fast 32.5 ms body can reach the original limiter after FIELD1. The
    // following physical edges still occur on one common grid; it cannot be
    // trapped behind another 30 ms FIELD0 followed by a compressed FIELD1.
    p.reset(epoch,period,500us);
    n=p.advancePhysicalPhase(epoch+period,0);
    const auto reset=epoch+period+100us;
    const auto gate=reset+32500us;
    n=p.advancePhysicalPhase(gate,n);
    check(n==2,"32.5 ms main gate sees the expected intervening physical FIELD0");
    const auto wake=p.nextPhysicalBoundary();
    check(wake-gate<1ms,"next FIELD1 is nearby after a fast limiter gate");
    check(p.advancePhysicalPhase(wake,n)==3,"one fresh physical edge satisfies the subsequent wait");

    std::mt19937_64 rng(0x504859534943414cull);
    for(unsigned trial=0;trial<200;++trial){
        p.reset(epoch,period,500us);n=0;auto now=epoch;
        for(unsigned i=0;i<10000;++i){
            now+=std::chrono::nanoseconds(rng()%100000000);
            const auto before=n;
            n=p.advancePhysicalPhase(now,n);
            const uint64_t exact=static_cast<uint64_t>((now-epoch)/period);
            check(n==exact&&n>=before,"random work observes exactly one monotonic physical slot index");
            check(p.physicalBoundary()==epoch+period*static_cast<int64_t>(n),"random physical phase never drifts or compresses");
            check(p.nextPhysicalBoundary()>now,"next physical edge is strictly future after coalescing");
        }
    }
    std::printf("physical phase: %zu checks, %zu failures; sizeof=%zu\n",checks,failures,sizeof(p));
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
