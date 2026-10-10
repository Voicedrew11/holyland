// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
constexpr uint32_t setupPc=0x170000, pollPc=setupPc+4, timerPc=setupPc+8;
constexpr uint32_t ackPc=setupPc+12, startPc=setupPc+16, endPc=setupPc+20;
constexpr uint32_t secondaryPc=setupPc+24, alarmPc=setupPc+28;
constexpr uint32_t gsWaitPc=setupPc+32, gsResumePc=setupPc+36, gsPollPc=setupPc+40;
constexpr uint32_t timerBase=0x10000000, compare=60000;
constexpr uint64_t fieldCycles=(16667ull*EeScheduler::kEeClockHz+999999ull)/1000000ull;
struct Sample { uint64_t tick,cycles,ticks,reconstructed,reconstructedSecond,secondTicks; uint32_t count,mode,secondMode; };
std::vector<Sample> samples;
Clock::time_point begin;
uint64_t epoch=0,rollovers=0;
uint64_t secondEpoch=0;
uint64_t secondRollovers=0;
std::vector<uint32_t> timerCauses;
size_t checks=0,failures=0,polls=0,endCalls=0,secondaryCalls=0,alarmCalls=0;
bool fragmented=false,masked=false,noHandler=false,watchdog=false;
bool simultaneous=false,multiple=false,blocking=false,withAlarm=false;
bool callbackWait=false,callbackPoll=false,callbackSeeded=false;
size_t callbackPolls=0;
void check(bool value,const char* message) {
    ++checks; if(!value) { ++failures;std::fprintf(stderr,"FAIL: %s\n",message); }
}
struct TimerSample { uint32_t mode,count; uint64_t cycle; };
TimerSample sampleTimer(PS2Runtime* runtime,uint32_t base,uint64_t timerEpoch) {
    TimerSample result{};
    // Both reads synchronize real elapsed time. Retry a compare-boundary
    // crossing so the sampled latch and COUNT describe the same interval.
    // Guest ISR software counters cannot change inside this native function.
    for(unsigned attempt=0;attempt<8;++attempt) {
        result.mode=runtime->memory().readIORegister(base+0x10);
        runtime->eeScheduler().publishSnapshot();
        const uint64_t modeCycle=runtime->eeScheduler().snapshot().eeCycle;
        result.count=runtime->memory().readIORegister(base);
        runtime->eeScheduler().publishSnapshot();
        result.cycle=runtime->eeScheduler().snapshot().eeCycle;
        if((modeCycle-timerEpoch)/32/compare==(result.cycle-timerEpoch)/32/compare)
            return result;
    }
    check(false,"timer MODE and COUNT sample resolves within eight attempts");
    return result;
}
void ack(uint8_t*,R5900Context* ctx,PS2Runtime* runtime) {
    const uint32_t cause=getRegU32(ctx,4);
    const uint32_t base=cause==10?timerBase+0x800:timerBase;
    const uint32_t mode=runtime->memory().readIORegister(base+0x10);
    check((mode&0x400)!=0,"normal timer ISR observes its real EQUF latch");
    if(cause==10)++secondRollovers;else ++rollovers;
    timerCauses.push_back(cause);
    if(withAlarm&&rollovers==1)check(alarmCalls==1,"earlier-cycle ordinary alarm executes before first timer handler");
    runtime->memory().writeIORegister(base+0x10,mode|0x400);
    check((runtime->memory().readIORegister(base+0x10)&0x400)==0,"authored ISR acknowledges EQUF through real MODE write");
#if !EXPECT_LOST_ROLLOVERS
    if(!blocking&&!callbackWait&&!callbackPoll){
        const uint64_t timerEpoch=cause==10?secondEpoch:epoch;
        const auto sample=sampleTimer(runtime,base,timerEpoch);
        const uint64_t elapsed=(sample.cycle-timerEpoch)/32;
        const uint64_t accounted=(cause==10?secondRollovers:rollovers)*compare+sample.count+
            ((sample.mode&0x400)?compare:0);
        check(accounted==elapsed,"prompt acknowledged ISR has no permanent lost timer period");
    }
#endif
    ctx->pc=0;
}
void timer(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    if(blocking) {
        ctx->pc=ackPc;
        ps2_syscalls::WaitVSyncTick(ram,ctx,runtime,-1);
        return;
    }
    if(!fragmented) { ack(ram,ctx,runtime);return; }
    ctx->r[31]=_mm_setzero_si128();
    if(!runtime->dispatchGuestBranch(ram,ctx,ackPc,timerPc,0,
        PS2Runtime::GuestBranchKind::DirectCall,"authored timer acknowledge"))return;
    ctx->pc=0;
}
void recordStart(R5900Context* ctx,PS2Runtime* runtime) {
    auto& ee=runtime->eeScheduler();ee.publishSnapshot();
    const auto first=sampleTimer(runtime,timerBase,epoch);
    const uint64_t cycles=first.cycle-epoch;
    const auto second=sampleTimer(runtime,timerBase+0x800,secondEpoch);
    const uint64_t secondTicks=(second.cycle-secondEpoch)/32;
    samples.push_back({ee.currentVSyncTick(),cycles,cycles/32,rollovers*compare+first.count,
        secondRollovers*compare+second.count,secondTicks,first.count,first.mode,second.mode});
    ctx->pc=0;
    if(samples.size()==8)runtime->requestStop();
}
void gsWait(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    ctx->pc=gsResumePc;
    ps2_syscalls::WaitVSyncTick(ram,ctx,runtime,-1);
}
void gsResume(uint8_t*,R5900Context* ctx,PS2Runtime* runtime) {
    check(runtime->eeScheduler().currentVSyncTick()>=2,"unrelated GS callback's physical wait completes with timer IRQ pending");
    recordStart(ctx,runtime);
}
void gsPoll(uint8_t*,R5900Context* ctx,PS2Runtime* runtime) {
    ++callbackPolls;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // An authored generated loop retains its normal instruction costs. The
    // synthetic-credit service gate must not suppress those cycle advances.
    (void)runtime->eeScheduler().checkpointDue(512000);
    if(runtime->eeScheduler().currentVSyncTick()>=2) {
        recordStart(ctx,runtime);return;
    }
    if(Clock::now()-begin>std::chrono::seconds(2)) {
        watchdog=true;runtime->requestStop();ctx->pc=0;return;
    }
    ctx->pc=gsPollPc;
}
void start(uint8_t*,R5900Context* ctx,PS2Runtime* runtime) {
    if((callbackWait||callbackPoll)&&!callbackSeeded) {
        callbackSeeded=true;
        // Queue a genuine timer interrupt while this unrelated GS invocation
        // is active. It cannot accept the pending invocation before it returns
        // or blocks. No timer status or IRQ queue is patched by the fixture.
        (void)runtime->eeScheduler().checkpointDue(compare*32);
        ctx->pc=callbackWait?gsWaitPc:gsPollPc;
        return;
    }
    recordStart(ctx,runtime);
}
void end(uint8_t*,R5900Context* ctx,PS2Runtime*) { ++endCalls;ctx->pc=0; }
void secondary(uint8_t*,R5900Context* ctx,PS2Runtime* runtime) {
    ++secondaryCalls;
    check((runtime->memory().readIORegister(timerBase+0x10)&0x400)==0,"ordered second handler sees first handler acknowledgement");
    ctx->pc=0;
}
void alarm(uint8_t*,R5900Context* ctx,PS2Runtime*) { ++alarmCalls;ctx->pc=0; }
void poll(uint8_t*,R5900Context* ctx,PS2Runtime* runtime) {
    ++polls;std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(Clock::now()-begin>std::chrono::seconds(2)) {
        watchdog=true;runtime->requestStop();ctx->pc=0;return;
    }
    ctx->pc=pollPc;
}
void setup(uint8_t*,R5900Context* ctx,PS2Runtime* runtime) {
    runtime->memory().writeIORegister(timerBase,0);
    runtime->memory().writeIORegister(timerBase+0x20,compare);
    runtime->memory().writeIORegister(timerBase+0x10,0x1C1); // BUS/16, ZRET, CUE, compare IRQ.
    if(simultaneous) {
        runtime->memory().writeIORegister(timerBase+0x800,0);
        runtime->memory().writeIORegister(timerBase+0x820,compare);
        runtime->memory().writeIORegister(timerBase+0x810,0x1C1);
    }
    if(withAlarm)check(runtime->eeScheduler().setAlarm(1,alarmPc,0,0,0)>0,"ordinary 64 microsecond alarm registers");
    runtime->memory().writeIORegister(timerBase,0);
    runtime->eeScheduler().publishSnapshot();epoch=runtime->eeScheduler().snapshot().eeCycle;
    if(simultaneous){
        runtime->memory().writeIORegister(timerBase+0x800,0);
        runtime->eeScheduler().publishSnapshot();secondEpoch=runtime->eeScheduler().snapshot().eeCycle;
    }
    // Both production and deliberately bulk-credit control receive the same
    // initial historical debt, sufficient to cross multiple real compares.
    std::this_thread::sleep_for(std::chrono::milliseconds(23));
    ctx->pc=pollPc;
}
}

int main(int argc,char** argv) {
    if(argc!=2)return EXIT_FAILURE;
    const std::string mode=argv[1];fragmented=mode=="fragmented";masked=mode=="masked";noHandler=mode=="no-handler";
    simultaneous=mode=="simultaneous";multiple=mode=="multiple";blocking=mode=="blocking";withAlarm=mode=="alarm";
    callbackWait=mode=="callback-wait";callbackPoll=mode=="callback-poll";
    if(mode!="simple"&&!fragmented&&!masked&&!noHandler&&!simultaneous&&!multiple&&!blocking&&!withAlarm&&!callbackWait&&!callbackPoll)return EXIT_FAILURE;
    auto runtime=std::make_unique<PS2Runtime>();check(runtime->memory().initialize(),"actual RAM initializes");
    runtime->registerFunction(setupPc,setup);runtime->registerFunction(pollPc,poll);
    runtime->registerFunction(timerPc,timer);runtime->registerFunction(ackPc,ack);
    runtime->registerFunction(startPc,start);runtime->registerFunction(endPc,end);
    runtime->registerFunction(secondaryPc,secondary);runtime->registerFunction(alarmPc,alarm);
    runtime->registerFunction(gsWaitPc,gsWait);runtime->registerFunction(gsResumePc,gsResume);runtime->registerFunction(gsPollPc,gsPoll);
    R5900Context ctx{};ctx.pc=setupPc;
    auto& ee=runtime->eeScheduler();ee.reset(runtime->memory().getRDRAM(),ctx);
    if(!noHandler)check(ee.addIrqHandler(false,9,timerPc,true,0,0,0)>0,"ordinary Timer0 IRQ handler registers");
    if(simultaneous)check(ee.addIrqHandler(false,10,timerPc,true,0,0,0)>0,"simultaneous Timer1 IRQ handler registers");
    if(multiple)check(ee.addIrqHandler(false,9,secondaryPc,true,0,0,0)>0,"second ordered Timer0 IRQ handler registers");
    ee.setIrqCauseEnabled(false,9,!masked);
    ee.setGsVSyncCallback(startPc,0,0);
    check(ee.addIrqHandler(false,3,endPc,true,0,0,0)>0,"ordinary End IRQ registers");
    begin=Clock::now();ee.run();
    check(!watchdog&&samples.size()==8,"eight physical fields complete with a continuously Runnable native thread");
    // A handler which blocks on each field can keep higher-priority IRQ work
    // runnable throughout this short capture. Its field/End/ISR checks below
    // establish progress without promising service to a lower-priority poller.
    if(!blocking)
        check(polls>0,"native polling makes progress beside hardware/IRQ service");
    check(endCalls==7,"seven field Ends are serviced before final Start callback");
    for(const auto& sample:samples) {
        check(sample.count<compare,"real ZRET counter stays below COMP");
        check(sample.cycles>=sample.tick*fieldCycles-epoch,"field credit reaches exact scheduled cycle offset");
#if EXPECT_LOST_ROLLOVERS
        check(sample.ticks>=sample.reconstructed+compare,"bulk-credit control loses at least one software compare interval");
#else
        if(masked||noHandler) {
            check(sample.reconstructed==sample.ticks%compare,"unserviced hardware retains real modulo counter and coalescing");
            check((sample.mode&0x400)!=0,"masked or missing handler leaves EQUF latched");
        } else if(blocking||callbackWait||callbackPoll) {
            check(sample.reconstructed<=sample.ticks,"blocking ISR receives no invented software time");
            check((sample.ticks-sample.reconstructed)%compare==0,"blocked ISR preserves legitimate latched-period coalescing");
        } else check(sample.reconstructed+((sample.mode&0x400)?compare:0)==sample.ticks,
            "software plus any one real newly latched IRQ conserves every timer tick");
        if(simultaneous)check(sample.reconstructedSecond+((sample.secondMode&0x400)?compare:0)==sample.secondTicks,
            "Timer1 conserves its independently sampled hardware ticks and pending latch");
#endif
        std::printf("field %llu: EE=%llu expected=%llu reconstructed=%llu count=%u mode=%03x\n",
            static_cast<unsigned long long>(sample.tick),static_cast<unsigned long long>(sample.cycles),
            static_cast<unsigned long long>(sample.ticks),static_cast<unsigned long long>(sample.reconstructed),sample.count,sample.mode);
    }
    if(masked||noHandler)check(rollovers==0,"masked or missing handler receives no invented IRQ");
    if(multiple)check(secondaryCalls==rollovers,"every ordered second handler services before later clock credit");
    if(simultaneous)for(size_t i=0;i<timerCauses.size();++i)check(timerCauses[i]==9u+(i&1u),"simultaneous timer causes retain ascending order");
    if(withAlarm)check(alarmCalls==1,"ordinary alarm executes exactly once");
    if(callbackWait||callbackPoll)check(callbackSeeded,"unrelated active GS callback queues the genuine timer interrupt");
    if(callbackPoll)check(callbackPolls>1,"unrelated GS callback actually polls through multiple normal instruction checkpoints");
    std::printf("%s: %zu checks, %zu failures; fields=%zu ISR=%llu polls=%zu ends=%zu watchdog=%d\n",
        mode.c_str(),checks,failures,samples.size(),static_cast<unsigned long long>(rollovers),polls,endCalls,watchdog);
    return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}
