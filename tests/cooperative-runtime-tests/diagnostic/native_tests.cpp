// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
constexpr uint64_t physicalEvent=0x8000000000000000ull;
constexpr uint32_t csrAddress=0x12001000;
size_t checks=0,failures=0;
void check(bool yes,const char* why){++checks;if(!yes){++failures;std::fprintf(stderr,"FAIL: %s\n",why);}}
uint64_t cycles(Clock::duration time){return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(time).count())*EeScheduler::kEeClockHz/1000000000ull;}
}
struct PhysicalFieldProof {
    static void anchor(PS2Runtime& runtime,Clock::time_point epoch){
        auto& ee=runtime.eeScheduler();auto& gs=runtime.memory().gs();
        R5900Context initial{};ee.reset(runtime.memory().getRDRAM(),initial);
        ee.m_hostClockEpoch=epoch;
        ee.m_fieldPacer.reset(epoch,16683333ns,500us);
        ee.m_vsyncTick=0;gs.vsyncTick.store(0);gs.csr.fetch_and(~0x2000ull);
        ee.m_deadlines.clear();ee.m_events.clear();ee.m_pendingInvocations.clear();
        ee.m_vblankHandoffs.clear();ee.m_eeCycle=0;
        ee.m_guestExecuting.store(false);
        ee.updateNextDeadline();
    }
    static void run(PS2Runtime& runtime){
        auto& ee=runtime.eeScheduler();auto& memory=runtime.memory();auto& gs=memory.gs();
        R5900Context initial{};ee.reset(memory.getRDRAM(),initial);
        constexpr auto period=16683333ns;
        auto epoch=Clock::now()-period*5-4ms;
        anchor(runtime,epoch);
        ee.m_vblankHandoffs.push_back({EeScheduler::kMainThreadId,0});
        ee.m_vblankHandoffExpiry=Clock::now()+1s;
        const auto beforeCycle=ee.m_eeCycle;
        const auto beforeInvocations=ee.m_pendingInvocations.size();
        const uint32_t csr=memory.read32(csrAddress);
        check(gs.vsyncTick.load()==5&&((csr>>13)&1)==1,"actual CSR MMIO observes latest odd physical slot while notification is held");
        check(ee.m_vsyncTick==0,"physical sampling does not deliver a software wake");
        check(ee.m_eeCycle==beforeCycle&&ee.m_pendingInvocations.size()==beforeInvocations,"physical sampling invents neither cycles nor guest invocations");
        const auto physicalBoundary=ee.m_fieldPacer.physicalBoundary();
        check(physicalBoundary==epoch+period*5,"MMIO phase keeps the nominal fixed boundary");
        check(ee.m_fieldPacer.physicalBlankEnd()<Clock::now(),"late observation outside blank does not create an artificial new blank");
        memory.write32(csrAddress,0);
        check(gs.vsyncTick.load()==5,"CSR writes also sample phase without advancing it twice");

        // An old independent End remains serviceable while Start is held.
        const auto oldestStart=epoch+period;
        ee.m_deadlines.push_back({ee.wallClockTarget(oldestStart),oldestStart,{EeEventType::VBlankStart,0,physicalEvent},100});
        ee.m_deadlines.push_back({ee.wallClockTarget(oldestStart+500us),oldestStart+500us,{EeEventType::VBlankEnd,0,1},101});
        ee.m_eeCycle=ee.wallClockTarget(Clock::now());ee.m_nextWallClockCheck=Clock::now()+1h;
        ee.updateNextDeadline();ee.processDueDeadlines();
        const auto findEnd=[&](){return std::find_if(ee.m_deadlines.begin(),ee.m_deadlines.end(),[](const auto& item){return item.event.type==EeEventType::VBlankEnd;});};
        auto nextEnd=findEnd();
        check(ee.m_vsyncTick==0,"held software Start never stops the physical clock or delivers a wake");
        check(nextEnd!=ee.m_deadlines.end()&&nextEnd->hostDeadline==epoch+period*6+500us,"late independent End coalesces once and retains a future physical End");
        std::this_thread::sleep_for(20ms);
        ee.m_eeCycle=ee.wallClockTarget(Clock::now());ee.processDueDeadlines();nextEnd=findEnd();
        const auto slotAfterEnd=gs.vsyncTick.load();
        std::printf("DIAG second_end slot=%llu end_exists=%d next_slot=%llu end_future_ms=%.6f now_from_epoch_ms=%.6f\n",(unsigned long long)slotAfterEnd,nextEnd!=ee.m_deadlines.end(), nextEnd==ee.m_deadlines.end()?0:(unsigned long long)nextEnd->event.value, nextEnd==ee.m_deadlines.end()?0:std::chrono::duration<double,std::milli>(nextEnd->hostDeadline-Clock::now()).count(),std::chrono::duration<double,std::milli>(Clock::now()-epoch).count());
        check(nextEnd!=ee.m_deadlines.end()&&nextEnd->hostDeadline>Clock::now()&&nextEnd->event.value>slotAfterEnd,"a second End remains live while the original Start is still held");
        check(std::count_if(ee.m_deadlines.begin(),ee.m_deadlines.end(),[](const auto& item){return item.event.type==EeEventType::VBlankEnd;})==1,"missed End causes never produce a backlog of future callbacks");

        // Old pending notification: it can wake an old wait once, but cannot
        // satisfy a wait or one-shot flag installed after that physical edge.
        auto& oldMain=ee.m_threads.at(EeScheduler::kMainThreadId);
        oldMain.status=EeThreadStatus::Waiting;
        oldMain.wait={EeWaitReason::VSync,EeVSyncWait{slotAfterEnd-1,-1},{}};
        ee.m_vblankHandoffs.clear();
        ee.processEvent({EeEventType::VBlankStart,0,physicalEvent});
        check(ee.m_vsyncTick==slotAfterEnd&&oldMain.status==EeThreadStatus::Ready,"one coalesced Start wakes the older physical wait");
        const size_t ready=ee.m_readyQueues[0].size();
        ee.processEvent({EeEventType::VBlankStart,0,physicalEvent});
        check(ee.m_readyQueues[0].size()==ready,"repeated old pending Start never replays a wake");

        anchor(runtime,Clock::now()-period*3-2ms);
        auto& main=ee.m_threads.at(EeScheduler::kMainThreadId);
        ee.m_deadlines.push_back({1,Clock::now()-100ms,{EeEventType::VBlankStart,0,physicalEvent},1});
        constexpr uint32_t flag=0x1000,sample=0x1010;
        ee.setVSyncFlag(flag,sample);
        check(gs.vsyncTick.load()==3,"one-shot registration first samples the actual physical edge");
        const auto tagged=ee.m_deadlines.front().event;
        check((tagged.value&~physicalEvent)==3,"one-shot registration records its fresh physical identity");
        ee.processEvent(tagged);
        uint32_t flagValue=0;std::memcpy(&flagValue,memory.getRDRAM()+flag,sizeof(flagValue));
        check(flagValue==0&&ee.m_vsyncFlagAddress==flag,"stale coalesced Start cannot satisfy a newly registered one-shot flag");
        main.status=EeThreadStatus::Running;ee.m_currentThreadId=EeScheduler::kMainThreadId;
        try{ee.waitVSync(0,-1);}catch(const EeDispatcherTransfer&){}
        check(main.wait.reason==EeWaitReason::VSync&&std::get<EeVSyncWait>(main.wait.payload).afterTick==3,"fresh wait captures latest physical identity rather than stale delivered counter");
        ee.completeVSync(3);
        check(main.status==EeThreadStatus::Waiting,"stale edge cannot complete fresh VSync wait");

        // An indivisible native call can read current FIELD without reentering
        // any IRQ or callback. Its normal timer clock still follows elapsed time.
        anchor(runtime,Clock::now()-period*2-1ms);
        ee.m_guestExecuting.store(true);ee.m_guestEntryTime=Clock::now();ee.m_guestEntryCycle=0;
        memory.writeIORegister(0x10000020,60000);memory.writeIORegister(0x10000010,0x1C1);
        std::this_thread::sleep_for(35ms);
        const auto before=ee.m_pendingInvocations.size();
        const uint32_t observed=memory.read32(csrAddress);
        const uint64_t slot=gs.vsyncTick.load();
        check(slot>=4&&((observed>>13)&1)==(slot&1),"long native CSR read samples correct even/odd missed-slot phase");
        check(ee.m_pendingInvocations.size()==before&&ee.m_vsyncTick==0,"MMIO clock hook does not recursively execute or queue VBlank guest callbacks");
        check(ee.m_eeCycle>=cycles(35ms),"native timer accounting remains independent of physical slot sampling");
        ee.m_guestExecuting.store(false);

        // A mode change changes only the future segment, retaining an old End.
        anchor(runtime,Clock::now()-period*3-1ms);ee.synchronizePhysicalClock();
        const uint64_t oldSlot=gs.vsyncTick.load();const auto oldBoundary=ee.m_fieldPacer.physicalBoundary();
        const auto oldEnd=oldBoundary+500us;
        ee.m_deadlines.push_back({ee.wallClockTarget(oldEnd),oldEnd,{EeEventType::VBlankEnd,0,oldSlot},100});
        ee.m_deadlines.push_back({ee.wallClockTarget(oldBoundary+period),oldBoundary+period,{EeEventType::VBlankStart,0,physicalEvent},101});
        ee.setGsVideoMode(0,0);
        check(gs.vsyncTick.load()==oldSlot&&ee.m_fieldPacer.physicalBoundary()==oldBoundary,"mode transition does not reinterpret past slot identities");
        check(ee.m_deadlines[0].hostDeadline==oldEnd,"already queued End is preserved across mode change");
        check(ee.m_deadlines[1].hostDeadline==oldBoundary+16667us,"future Start uses the next piecewise physical phase");
        check(ee.m_deadlines[1].deadlineCycle==ee.wallClockTarget(oldBoundary+16667us),"future Start logical deadline matches its oscillator epoch");

        anchor(runtime,Clock::now());
        const auto future=ee.m_fieldPacer.nextPhysicalBoundary();
        ee.m_deadlines.push_back({ee.wallClockTarget(future),future,{EeEventType::VBlankStart,0,physicalEvent},1});
        ee.m_deadlines.push_back({ee.wallClockTarget(future+500us),future+500us,{EeEventType::VBlankEnd,0,1},2});
        ee.setGsVideoMode(0,0);
        check(ee.m_deadlines[1].hostDeadline==ee.m_deadlines[0].hostDeadline+500us,"mode transition retimes a not-yet-started slot's paired End");

        anchor(runtime,Clock::now());
        memory.writeIORegister(0x10000020,60000);memory.writeIORegister(0x10000010,0x1C1);
        ee.m_guestExecuting.store(true);ee.m_guestEntryTime=Clock::now();ee.m_guestEntryCycle=ee.m_eeCycle;
        std::this_thread::sleep_for(35ms);
        const auto beforeSdk=ee.m_eeCycle;
        ee.setVSyncFlag(flag,sample);
        check(ee.m_eeCycle==beforeSdk,"one-shot SDK registration samples phase without bulk timer credit");
        ee.setGsVideoMode(0,0);
        check(ee.m_eeCycle==beforeSdk,"video-mode SDK change samples phase without bulk timer credit");
        auto& waiter=ee.m_threads.at(EeScheduler::kMainThreadId);
        waiter.status=EeThreadStatus::Running;ee.m_currentThreadId=EeScheduler::kMainThreadId;
        try{ee.waitVSync(0,-1);}catch(const EeDispatcherTransfer&){}
        check(ee.m_eeCycle==beforeSdk,"wait registration leaves timer debt for chronological dispatcher service");
        ee.m_guestExecuting.store(false);
    }
};
int main(){auto runtime=std::make_unique<PS2Runtime>();check(runtime->memory().initialize(),"real PS2 RAM initializes");PhysicalFieldProof::run(*runtime);std::printf("physical native: %zu checks, %zu failures\n",checks,failures);return failures?EXIT_FAILURE:EXIT_SUCCESS;}
