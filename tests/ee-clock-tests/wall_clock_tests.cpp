// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "ps2_syscalls.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {
using Clock=std::chrono::steady_clock;
constexpr uint32_t setupPc=0x180000,workPc=setupPc+4,readPc=setupPc+8;
constexpr uint32_t timerPc=setupPc+12,ackPc=setupPc+16,finishPc=setupPc+20;
constexpr uint32_t base=0x10000000,compare=60000;
size_t checks=0,failures=0,isrs=0;
uint64_t timerEpoch=0,cyclesAtSample=0,represented=0;
uint32_t sampleCount=0,sampleMode=0;
Clock::time_point workBegin,workEnd,afterAck;
bool fragmented=false,longIsr=false,masked=false,noHandler=false,resetCycle=false,delayed=false,idle=false,instructionLedger=false;
bool nativeMmio=false;
void check(bool yes,const char* why){++checks;if(!yes){++failures;std::fprintf(stderr,"FAIL: %s\n",why);}}
uint64_t cycle(PS2Runtime* runtime){auto& ee=runtime->eeScheduler();ee.publishSnapshot();return ee.snapshot().eeCycle;}
void acknowledge(uint8_t*,R5900Context* ctx,PS2Runtime* runtime){
    auto& memory=runtime->memory();
    if(longIsr&&!delayed){delayed=true;std::this_thread::sleep_for(std::chrono::milliseconds(45));}
    const uint32_t mode=memory.readIORegister(base+0x10);
    check((mode&0x400)!=0,"normal Timer0 ISR sees real EQUF latch");
    ++isrs;memory.writeIORegister(base+0x10,mode|0x400);
    check((memory.readIORegister(base+0x10)&0x400)==0,"MODE acknowledgement clears EQUF after native elapsed accrual");
    afterAck=Clock::now();ctx->pc=0;
}
void timer(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime){
    if(!fragmented){acknowledge(ram,ctx,runtime);return;}
    ctx->r[31]=_mm_setzero_si128();
    if(!runtime->dispatchGuestBranch(ram,ctx,ackPc,timerPc,0,PS2Runtime::GuestBranchKind::DirectCall,"authored fragmented timer"))return;
    ctx->pc=0;
}
void finish(uint8_t*,R5900Context* ctx,PS2Runtime* runtime){
    auto& memory=runtime->memory();
    sampleMode=memory.readIORegister(base+0x10);
    sampleCount=memory.readIORegister(base);
    cyclesAtSample=cycle(runtime)-timerEpoch;
    represented=isrs*compare+sampleCount;
    ctx->pc=0;
    if(resetCycle){
        // Reset starts a fresh oscillator lifecycle; no stale native-entry
        // callback interval may be applied to the newly reset hardware clock.
        R5900Context next{};next.pc=0;
        runtime->eeScheduler().reset(memory.getRDRAM(),next);
        check(cycle(runtime)<1000,"scheduler reset clears old wall clock debt and virtual cycle ledger");
    }
    runtime->requestStop();
}
void read(uint8_t*,R5900Context* ctx,PS2Runtime* runtime){
    if(idle)workEnd=Clock::now();
    if(nativeMmio){
        check(isrs==1,"indivisible long native call latches only one deferred hardware interrupt");
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        check((runtime->memory().readIORegister(base+0x10)&0x400)!=0,"MODE acknowledgement re-arms timer only for subsequently elapsed time");
        check(isrs==1,"second native MMIO read queues IRQ without reentering guest handler");
    }
    ctx->pc=finishPc;
    // A real dispatch safepoint must reconcile native work before this later
    // timer-read/reset helper can execute, including all prompt ISR boundaries.
    runtime->memory().readIORegister(base);
}
void work(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime){
    if(idle){workBegin=Clock::now();ctx->pc=readPc;ps2_syscalls::WaitVSyncTick(ram,ctx,runtime,-1);return;}
    workBegin=Clock::now();std::this_thread::sleep_for(std::chrono::milliseconds(23));workEnd=Clock::now();
    if(nativeMmio){
        check(isrs==0,"native function cannot be retroactively interrupted before its first MMIO");
        check((runtime->memory().readIORegister(base+0x10)&0x400)!=0,"native MMIO accrues elapsed hardware time and latches EQUF");
        check((runtime->memory().readIORegister(base+0x10)&0x400)!=0,"repeated native MMIO preserves the same real latch");
        check(isrs==0,"repeated MMIO does not recursively dispatch or duplicate IRQ");
    }
    ctx->pc=readPc;
    (void)runtime;
}
void setup(uint8_t*,R5900Context* ctx,PS2Runtime* runtime){
    auto& memory=runtime->memory();
    if(instructionLedger)(void)runtime->eeScheduler().checkpointDue(64000000);
    memory.writeIORegister(base+0x20,compare);memory.writeIORegister(base+0x10,0x1C1);
    memory.writeIORegister(base,0);timerEpoch=cycle(runtime);ctx->pc=workPc;
}
}
int main(int argc,char** argv){
    if(argc!=2)return EXIT_FAILURE;const std::string mode=argv[1];
    fragmented=mode=="fragmented";longIsr=mode=="long-isr";masked=mode=="masked";
    noHandler=mode=="no-handler";resetCycle=mode=="reset-cycle";
    idle=mode=="idle";instructionLedger=mode=="instruction-ledger";
    nativeMmio=mode=="native-mmio";
    if(mode!="normal"&&!fragmented&&!longIsr&&!masked&&!noHandler&&!resetCycle&&!idle&&!instructionLedger&&!nativeMmio)return EXIT_FAILURE;
    auto runtime=std::make_unique<PS2Runtime>();check(runtime->memory().initialize(),"real PS2 RAM initializes");
    runtime->registerFunction(setupPc,setup);runtime->registerFunction(workPc,work);runtime->registerFunction(readPc,read);
    runtime->registerFunction(timerPc,timer);runtime->registerFunction(ackPc,acknowledge);runtime->registerFunction(finishPc,finish);
    R5900Context ctx{};ctx.pc=setupPc;auto& ee=runtime->eeScheduler();ee.reset(runtime->memory().getRDRAM(),ctx);
    if(!noHandler)check(ee.addIrqHandler(false,9,timerPc,true,0,0,0)>0,"real Timer0 handler registers");
    ee.setIrqCauseEnabled(false,9,!masked);ee.run();
    const double nativeMs=std::chrono::duration<double,std::milli>(workEnd-workBegin).count();
    const double clockMs=double(cyclesAtSample)*1000.0/EeScheduler::kEeClockHz;
    const uint64_t ticks=cyclesAtSample/32;
    check(nativeMs>=(idle?15:23),"native authored work consumes the requested wall time");
#if EXPECT_CLOCK_UNDERCREDIT
    check(clockMs+3<nativeMs,"cycle-driven control materially undercredits real native work");
#else
    if(instructionLedger){
        check(clockMs<1,"already accounted instruction cycles ahead of wall floor receive no duplicate native time");
        check(isrs==0,"no new timer IRQ is synthesized from an already credited instruction lead");
    }else{
    check(clockMs+1>=nativeMs,"native work advances the reset-anchored hardware clock floor before later timer helper");
    if(masked||noHandler){
        check(isrs==0,"masked or missing handler creates no synthetic ISR");
        check((sampleMode&0x400)!=0,"unserviced timer retains real latched EQUF");
        check(sampleCount==ticks%compare,"unserviced hardware retains true modulo count");
    }else if(nativeMmio){
        check(isrs==2,"two separately latched intervals produce exactly two actual deferred handlers");
        check(ticks>=represented+compare,"unpreemptible native call retains legitimate hardware coalescing");
        check((ticks-represented)%compare==0,"native-call coalescing loses only complete already latched intervals");
    }else if(longIsr){
        check(delayed,"long native timer handler executed");
        check(ticks>=represented+compare,"genuinely delayed acknowledgement retains hardware coalescing");
        check((ticks-represented)%compare==0,"long ISR loss is an integral number of legitimate latched periods");
        check(isrs<=5,"long acknowledgement does not replay every historical compare as fresh IRQ");
    }else{
        check(isrs>=(idle?2:3),"prompt normal ISR services multiple compare boundaries hidden by native work");
        check(represented==ticks,"prompt chronological ISR reconstruction has no lost rollovers");
    }
    }
#endif
    std::printf("native_ms=%.6f clock_ms=%.6f cycles=%llu ticks=%llu represented=%llu ISR=%zu mode=%03x checks=%zu failures=%zu\n",
        nativeMs,clockMs,(unsigned long long)cyclesAtSample,(unsigned long long)ticks,(unsigned long long)represented,isrs,sampleMode,checks,failures);
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
