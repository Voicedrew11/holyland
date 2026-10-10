// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr uint32_t kWait = 0x160000, kResume = 0x160010, kStart = 0x160020,
                   kEnd = 0x160030, kNestedResume = 0x160040, kPoll = 0x160050, kAlarm = 0x160060;
constexpr uint64_t kPeriodCycles = (16667ull * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
constexpr size_t kCount = 12;
std::string mode;
int workerId = 0;
size_t assertions = 0, failures = 0, resumes = 0, workerResumes = 0, completions = 0;
size_t nestedResumes = 0, pollTransfers = 0;
size_t alarmCalls = 0;
bool debtInjected = false, lifecycleDone = false;
std::vector<uint64_t> observed, workerObserved, starts;
std::vector<Clock::time_point> startTimes, gameplayTimes;
std::vector<char> events;

void check(bool value, const char *message)
{
    ++assertions;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}

void endIrq(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    events.push_back('E');
    ctx->pc = 0;
}

void alarm(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    ++alarmCalls; events.push_back('A');
    check(runtime->eeScheduler().currentVSyncTick() == 1 && resumes == 0,
          "ordinary due alarm runs during field hold before base continuation");
    ctx->pc = 0;
}

void nestedResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    ++nestedResumes;
    check(runtime->eeScheduler().currentVSyncTick() >= 2, "nested invocation-local wait consumes a later physical field");
    ctx->pc = 0;
}

void startCallback(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &ee = runtime->eeScheduler();
    const uint64_t tick = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
    starts.push_back(tick); startTimes.push_back(Clock::now()); events.push_back('S');
    if (mode != "no-waiters")
        check(completions != 0 || mode == "multiple" || mode == "suspended", "resume completion may precede queued callback without acknowledging base");
    if (tick == 1 && !debtInjected && mode != "work35")
    {
        debtInjected = true;
        if (mode == "alarm") check(ee.setAlarm(1, kAlarm, 0, 0, 0) > 0, "alarm registered while publication is held");
        // Enough wall and guest-cycle debt that the original drainer can
        // publish the next field before entering the awakened base function.
        const auto debtBegin=Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        std::printf("DIAG callback_sleep_ms=%.6f delivered=%llu physical=%llu csr=%llu\n", std::chrono::duration<double,std::milli>(Clock::now()-debtBegin).count(), (unsigned long long)ee.currentVSyncTick(), (unsigned long long)runtime->memory().gs().vsyncTick.load(), (unsigned long long)((runtime->memory().gs().csr.load()>>13)&1));
        ee.accountCycles(static_cast<uint32_t>(kPeriodCycles * 2));
    }
    if (tick == 1 && !lifecycleDone && workerId)
    {
        lifecycleDone = true;
        if (mode == "suspend-ready")
            check(ee.suspendThread(workerId, true) == 0, "Ready waiter can be suspended during callback");
        if (mode == "delete-ready")
        {
            uint32_t stack = 0;
            check(ee.terminateThread(workerId, stack, true) == 0, "Ready waiter can be terminated during callback");
            check(ee.deleteThread(workerId, stack) == 0, "terminated waiter can be deleted during callback");
        }
    }
    if (mode.starts_with("nested-") && tick == 1)
    {
        ee.setGsVSyncCallback(0, 0, 0);
        ctx->pc = mode == "nested-missing" ? 0x1600f0 : kNestedResume;
        ee.waitVSync(ee.currentVSyncTick(), -1, [runtime](R5900Context &resumed) {
            if (mode == "nested-completion") resumed.pc = 0;
            if (mode == "nested-cancel")
                runtime->eeScheduler().currentThread()->invocations.back().shouldExecute = [] { return false; };
        });
    }
    if (mode == "stop") runtime->requestStop();
    if (mode == "no-waiters" && tick >= kCount) runtime->requestStop();
    ctx->pc = 0;
}

void wait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &ee = runtime->eeScheduler();
    if (mode == "suspended" && workerId && !lifecycleDone && ee.currentThreadId() == EeScheduler::kMainThreadId)
    {
        lifecycleDone = true;
        check(ee.thread(workerId)->status == EeThreadStatus::Waiting, "higher-priority worker reached wait before suspension");
        check(ee.suspendThread(workerId, false) == 0, "waiting worker becomes WaitingSuspended");
    }
    if (mode == "work35" && resumes != 0 && (resumes & 1u))
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
    const bool isWorker = ee.currentThreadId() == workerId;
    ctx->pc = kResume;
    const uint64_t current = ee.currentVSyncTick();
    ee.waitVSync(current, -1, [isWorker](R5900Context &) { if (!isWorker) ++completions; });
    (void)rdram;
}

void resume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &ee = runtime->eeScheduler();
    const uint64_t tick = ee.currentVSyncTick();
    const bool isWorker = ee.currentThreadId() == workerId;
    if (isWorker)
    {
        ++workerResumes; workerObserved.push_back(tick);
        if (mode == "self-delete") ee.exitCurrent(true);
        if (workerResumes == kCount) { runtime->requestStop(); ctx->pc = 0; return; }
        ctx->pc = kWait; return;
    }
    if (mode == "priority" && resumes == 0)
    {
        ctx->pc = kPoll;
        return;
    }
    if (mode == "polling" && resumes == 0)
    {
        ctx->pc = kPoll;
        while (ee.currentVSyncTick() < 2)
        {
            if (ee.checkpointDue()) { ++pollTransfers; throw EeDispatcherTransfer{}; }
        }
    }
    ++resumes; observed.push_back(tick);
    std::printf("DIAG resume=%zu delivered=%llu physical=%llu v0=%d csr=%llu\n", resumes,(unsigned long long)tick,(unsigned long long)runtime->memory().gs().vsyncTick.load(),_mm_cvtsi128_si32(ctx->r[2]),(unsigned long long)((runtime->memory().gs().csr.load()>>13)&1));
    if ((resumes & 1u) != 0) gameplayTimes.push_back(Clock::now());
    if (mode.starts_with("nested-"))
    {
        check(nestedResumes == (mode == "nested-wait" ? 1u : 0u), "exact nested continuation executes or retires as requested");
        check(tick >= 2, "new wait retires parent barrier without inventing a physical slot");
        check(_mm_cvtsi128_si32(ctx->r[2]) == 1, "direct scheduler base wait retains its first published FIELD result");
        runtime->requestStop(); ctx->pc = 0; return;
    }
    if (mode != "polling")
        check(_mm_cvtsi128_si32(ctx->r[2]) == static_cast<int>((runtime->memory().gs().csr.load(std::memory_order_acquire) >> 13u) & 1u), "resumed SDK wait samples the current CSR FIELD");
    if (resumes == kCount && mode != "multiple") { runtime->requestStop(); ctx->pc = 0; return; }
    if (resumes == kCount && mode == "multiple") { ctx->pc = 0; return; }
    ctx->pc = kWait;
}

void poll(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &ee = runtime->eeScheduler();
    if (mode == "no-waiters")
    {
        // A polling guest makes no VSync wait and must never acquire a hold.
        if (ee.checkpointDue(100000)) throw EeDispatcherTransfer{};
        ctx->pc = kPoll; return;
    }
    while (ee.currentVSyncTick() < 2)
        if (ee.checkpointDue()) { ++pollTransfers; throw EeDispatcherTransfer{}; }
    if (mode != "priority")
        check(pollTransfers > 0, "polling continuation cooperatively transfers before later publication");
    runtime->requestStop(); ctx->pc = 0;
}
}

int main(int argc, char **argv)
{
    if (argc != 2) return EXIT_FAILURE;
    mode = argv[1];
    auto runtime = std::make_unique<PS2Runtime>();
    check(runtime->memory().initialize(), "real PS2Memory initializes");
    runtime->registerFunction(kWait, wait); runtime->registerFunction(kResume, resume);
    runtime->registerFunction(kStart, startCallback); runtime->registerFunction(kEnd, endIrq);
    runtime->registerFunction(kNestedResume, nestedResume); runtime->registerFunction(kPoll, poll);
    runtime->registerFunction(kAlarm, alarm);
    R5900Context main{}; main.pc = mode == "no-waiters" ? kPoll : kWait;
    auto &ee = runtime->eeScheduler(); ee.reset(runtime->memory().getRDRAM(), main);
    ee.setGsVSyncCallback(kStart, 0, 0); ee.addIrqHandler(false, 3, kEnd, true, 0, 0, 0);
    if (mode == "multiple" || mode == "suspended" || mode == "suspend-ready" || mode == "delete-ready" || mode == "self-delete" || mode == "priority")
    {
        EeThreadCreateParams worker{}; worker.entry = kWait; worker.priority = 1;
        workerId = ee.createThread(worker); check(workerId > 1, "worker created");
        check(ee.startThread(workerId, 0, main, false) == 0, "worker started");
        if (mode == "suspended")
        {
            int old = 0; check(ee.changePriority(EeScheduler::kMainThreadId, 2, false, old) == 0, "main lowers priority for waiting-suspended test");
        }
    }
    const auto begin = Clock::now();
#if EXPECT_MISSED_FIELDS
    std::atomic<bool> completed=false;
    std::thread watchdog([&]{
        const auto limit=Clock::now()+std::chrono::milliseconds(500);
        while(!completed.load()&&Clock::now()<limit)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!completed.load())runtime->requestStop();
    });
#endif
    ee.run();
#if EXPECT_MISSED_FIELDS
    completed.store(true);watchdog.join();
#endif
    const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    if (mode == "stop") check(resumes == 0, "Stop remains effective while waiter handoff is held");
    else if (mode.starts_with("nested-")) check(resumes == 1, "original continuation finishes after nested lifecycle path");
    else if (mode == "polling") check(pollTransfers > 0 && ee.currentVSyncTick() >= 2, "poll loop observes the next genuine field");
    else if (mode == "priority") check(workerResumes == 0 && ee.currentVSyncTick() >= 2, "bounded hold expiry preserves higher-priority polling and genuine display progress");
    else if (mode == "no-waiters") check(starts.size() >= kCount, "fields advance without any VSync waiter barrier");
    else
    {
        check(resumes == kCount, "all twelve waiter continuations run");
        bool skipped = false;
        for (size_t i = 0; i < observed.size(); ++i) if (observed[i] != i + 1) skipped = true;
#if EXPECT_MISSED_FIELDS
        skipped|=resumes<kCount;
        check(skipped, "continuous-only negative control loses fields or starves continuation");
        // Parity mismatch and callback observation failures are expected here.
        failures = skipped ? 0 : 1;
        std::printf("Continuous-only control: skipped=%d, fields=%llu, resumes=%zu\n", skipped,
            static_cast<unsigned long long>(ee.currentVSyncTick()), resumes);
        return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
#else
        for(size_t i=1;i<observed.size();++i)
            check(observed[i]>observed[i-1], "each waiter consumes a distinct later physical publication");
        if(mode!="work35")check(!skipped, "normal no-overrun fixture retains consecutive physical publications");
#endif
        if (mode == "multiple")
        {
            check(workerResumes == kCount, "all lower-priority waiter continuations also run");
            check(workerObserved == observed, "multiple awakened waiters consume identical sequential fields");
        }
        if (mode == "suspended" || mode == "suspend-ready" || mode == "delete-ready")
            check(workerResumes == 0, "excluded or retired worker never blocks subsequent Starts");
        if (mode == "self-delete") check(workerResumes == 1 && ee.thread(workerId) == nullptr, "self-delete retires hold without stale-pointer access");
        if (mode == "alarm") check(alarmCalls == 1, "ordinary alarm fires exactly once without being held with Start");
    }
    for(size_t i=1;i<starts.size();++i)
        check(starts[i]>starts[i-1], "GS callbacks have unique monotonic physical identities without backlog replay");
    // End is independent of software Start holds. Its physical nominal order,
    // coalescing and current-blank handling are checked by the focused fixture.
    std::printf("%s: %zu assertions, %zu failures; fields=%llu, resumes=%zu/%zu, poll-transfers=%zu, elapsed=%.3fms\n",
        mode.c_str(), assertions, failures, static_cast<unsigned long long>(ee.currentVSyncTick()), resumes, workerResumes, pollTransfers, elapsed);
    if (mode == "work35" && gameplayTimes.size() >= 2)
        std::printf("35ms-work average gameplay interval %.3fms\n", std::chrono::duration<double, std::milli>(gameplayTimes.back() - gameplayTimes.front()).count() / (gameplayTimes.size() - 1));
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
