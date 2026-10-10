#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr uint32_t setupPc = 0x00170000u, mainPollPc = setupPc + 4u;
constexpr uint32_t peerPollPc = setupPc + 8u, waitPc = setupPc + 12u;
constexpr uint32_t resumePc = setupPc + 16u, startIrqPc = setupPc + 20u;
constexpr uint32_t endIrqPc = setupPc + 24u, timerIrqPc = setupPc + 28u;
constexpr uint32_t flagAddr = 0x1800u, tickAddr = 0x1810u;
constexpr uint32_t timerBase = 0x10000000u;
constexpr uint64_t fieldCycles = (16667ull * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
constexpr uint64_t blankCycles = (500ull * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
constexpr size_t wantedFields = 6u;

struct Field
{
    Clock::time_point time;
    uint64_t tick, cycle, nextDeadline;
    uint32_t field, timerCount;
};
struct Resume
{
    uint64_t tick, published, csr;
    uint32_t flag;
    int32_t parity;
};
std::vector<Field> starts, ends;
std::vector<Resume> resumes;
Clock::time_point begin;
uint64_t timerEpoch = 0u;
unsigned mainPolls = 0u, peerPolls = 0u, timerIrqs = 0u;
unsigned checks = 0u, failures = 0u;
bool lateNativeCall = false, lateCallDone = false, watchdog = false;
int peerId = 0, waiterId = 0;

void check(bool value, const char *message)
{
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void reg(R5900Context *ctx, unsigned index, uint32_t value)
{
    ctx->r[index] = _mm_set_epi32(0, 0, 0, static_cast<int>(value));
}
Field sample(PS2Runtime *runtime)
{
    auto &ee = runtime->eeScheduler();
    const uint32_t timerCount = runtime->memory().readIORegister(timerBase);
    ee.publishSnapshot();
    const auto snapshot = ee.snapshot();
    return {Clock::now(), ee.currentVSyncTick(), snapshot.eeCycle, snapshot.nextEventCycle,
        static_cast<uint32_t>(runtime->memory().gs().csr.load(std::memory_order_acquire) & 0x2000u),
        timerCount};
}
void startIrq(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    starts.push_back(sample(runtime));
    ctx->pc = 0u;
}
void endIrq(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    ends.push_back(sample(runtime));
    ctx->pc = 0u;
}
void timerIrq(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ++timerIrqs;
    ctx->pc = 0u;
}
void wait(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    std::memset(ram + flagAddr, 0, 4u);
    runtime->eeScheduler().setVSyncFlag(flagAddr, tickAddr);
    ctx->pc = resumePc;
    ps2_syscalls::WaitVSyncTick(ram, ctx, runtime, -1);
}
void resume(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    uint32_t flag = 0u;
    uint64_t published = 0u;
    std::memcpy(&flag, ram + flagAddr, sizeof(flag));
    std::memcpy(&published, ram + tickAddr, sizeof(published));
    resumes.push_back({runtime->eeScheduler().currentVSyncTick(), published,
        runtime->memory().gs().csr.load(std::memory_order_acquire), flag,
        static_cast<int32_t>(getRegU32(ctx, 2))});
    if (resumes.size() == wantedFields)
    {
        ctx->pc = 0u;
        runtime->requestStop();
    }
    else ctx->pc = waitPc;
}
void poll(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime, bool peer)
{
    if (peer) ++peerPolls; else ++mainPolls;
    // Deliberately expensive native HLE: real wall time elapses while the
    // dispatcher itself accounts only eight artificial instruction cycles.
    // Two priority-one threads remain Ready and rotate; no idle waiter shortcut.
    const auto *consumer = runtime->eeScheduler().thread(waiterId);
    if (lateNativeCall && !lateCallDone && resumes.size() == 1u && consumer &&
        consumer->status == EeThreadStatus::Waiting && consumer->wait.reason == EeWaitReason::VSync)
    {
        lateCallDone = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    const auto pollUntil = Clock::now() + std::chrono::milliseconds(1);
    while (Clock::now() < pollUntil) _mm_pause();
    const auto elapsed = Clock::now() - begin;
#if EXPECT_BOTH_CLOCKS_CONTROL
    if (elapsed >= std::chrono::milliseconds(100))
#else
    if (elapsed >= std::chrono::seconds(2))
#endif
    {
        watchdog = true;
        runtime->requestStop();
        ctx->pc = 0u;
        return;
    }
    ctx->pc = peer ? peerPollPc : mainPollPc;
    reg(ctx, 4u, 1u);
    ps2_syscalls::RotateThreadReadyQueue(ram, ctx, runtime);
}
void mainPoll(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) { poll(ram, ctx, runtime, false); }
void peerPoll(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) { poll(ram, ctx, runtime, true); }
void setup(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    auto &ee = runtime->eeScheduler();
    int previous = -1;
    check(ee.changePriority(EeScheduler::kMainThreadId, 1, true, previous) == 0,
        "main uses retail post-InitThread priority one");
    EeThreadCreateParams peer{};
    peer.entry = peerPollPc; peer.stack = 0x00180000u; peer.stackSize = 0x4000u; peer.priority = 1;
    peerId = ee.createThread(peer);
    check(peerId > 0 && ee.startThread(peerId, 0u, *ctx, true) == 0,
        "real Ready polling peer starts at priority one");
    peer.entry = waitPc; peer.stack = 0x00184000u;
    waiterId = ee.createThread(peer);
    check(waiterId > 0 && ee.startThread(waiterId, 0u, *ctx, true) == 0,
        "actual VSync consumer starts beside Ready pollers");
    runtime->memory().writeIORegister(timerBase, 0u);
    runtime->memory().writeIORegister(timerBase + 0x20u, 2000u);
    runtime->memory().writeIORegister(timerBase + 0x10u, 0x182u); // BUS/256, enabled, compare IRQ.
    ee.publishSnapshot(); timerEpoch = ee.snapshot().eeCycle;
    ctx->pc = mainPollPc;
}

void runCase(bool late)
{
    starts.clear(); ends.clear(); resumes.clear();
    mainPolls = peerPolls = timerIrqs = 0u;
    lateNativeCall = late; lateCallDone = false; watchdog = false;
    auto runtime = std::make_unique<PS2Runtime>();
    check(runtime->memory().initialize(), "actual runtime RAM initializes");
    runtime->registerFunction(setupPc, setup); runtime->registerFunction(mainPollPc, mainPoll);
    runtime->registerFunction(peerPollPc, peerPoll); runtime->registerFunction(waitPc, wait);
    runtime->registerFunction(resumePc, resume); runtime->registerFunction(startIrqPc, startIrq);
    runtime->registerFunction(endIrqPc, endIrq); runtime->registerFunction(timerIrqPc, timerIrq);
    R5900Context ctx{}; ctx.pc = setupPc;
    runtime->eeScheduler().reset(runtime->memory().getRDRAM(), ctx);
    auto &ee = runtime->eeScheduler();
    check(ee.addIrqHandler(false, 2u, startIrqPc, true, 0u, 0u, 0u) > 0, "real VBlankStart IRQ registered");
    check(ee.addIrqHandler(false, 3u, endIrqPc, true, 0u, 0u, 0u) > 0, "real VBlankEnd IRQ registered");
    check(ee.addIrqHandler(false, 9u, timerIrqPc, true, 0u, 0u, 0u) > 0, "real timer IRQ registered");
    ee.setIrqCauseEnabled(false, 2u, true); ee.setIrqCauseEnabled(false, 3u, true);
    ee.setIrqCauseEnabled(false, 9u, true);
    begin = Clock::now();
    ee.run();
    const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    check(mainPolls > 5u && peerPolls > 5u, "both Ready native polling threads ran without idle fast-forward");
    std::printf("%s: total %.3fms, fields %zu, ends %zu, waits %zu, polls %u/%u, timer IRQ %u\n",
#if EXPECT_BOTH_CLOCKS_CONTROL
        "prior BOTH-deadlines control",
#else
        late ? "late native call" : "busy native poll",
#endif
        totalMs, starts.size(), ends.size(), resumes.size(), mainPolls, peerPolls, timerIrqs);
#if EXPECT_BOTH_CLOCKS_CONTROL
    check(watchdog && totalMs >= 100.0 && totalMs < 500.0, "control runs its real 100ms busy-poll interval");
    check(starts.empty() && resumes.empty(), "old BOTH-deadlines rule produces fewer than one field in 100ms");
    check(ee.snapshot().eeCycle < fieldCycles, "low native dispatcher credits cannot reach one old virtual field");
    check(timerIrqs == 0u, "old missing elapsed credit also starves EE timer compare");
#else
    check(!watchdog && (late ? starts.size() >= wantedFields : starts.size() == wantedFields) && resumes.size() == wantedFields,
        "all six hardware fields and genuine VSync waits finish while peers remain Ready");
    check(ends.size() >= starts.size() - 1u && ends.size() <= starts.size(),
        "every completed field has one VBlankEnd before its successor");
    check(timerIrqs == 1u, "elapsed hardware clock propagates through accountCycles to real timer compare IRQ");
    check(totalMs >= (late ? 275.0 : 90.0) && totalMs < (late ? 500.0 : 300.0),
        "physical fields remain paced while elapsed native work advances the independent hardware clock");
    if (late)
    {
        // Busy equal-priority scheduling can legitimately miss the short
        // opposite field after consuming the previous publication. Preserve
        // exact observed parity/tick and accounting checks, not a 1:1 count.
        for (const auto &result : resumes)
        {
            check(result.published == result.csr && result.flag == 1u, "late sampled CSR publication remains consistent");
            check(result.parity == static_cast<int32_t>(result.tick & 1u), "late wait samples its resumed CSR FIELD parity");
        }
        std::printf("  late diagnostic: %zu physical fields, %zu wait returns\n", starts.size(), resumes.size());
    }
    if (starts.size() == wantedFields && resumes.size() == wantedFields)
    for (size_t i = 0u; i < wantedFields; ++i)
    {
        const auto &field = starts[i]; const auto &result = resumes[i];
        check(field.tick == i + 1u && result.tick == field.tick, "fields and waiter resumes stay consecutive");
        check(result.published == result.csr && result.flag == 1u, "full sampled CSR and one-shot flag publish before waiter continuation");
        check(result.parity == static_cast<int32_t>((field.field >> 13u) & 1u), "native WaitVSyncTick returns the resumed CSR FIELD");
        check(field.field == ((field.tick & 1u) ? 0x2000u : 0u), "CSR FIELD follows emitted tick parity");
        // Hardware emits before the queued guest IRQ dispatch. With real host
        // scheduling delay, the 500 us End can already have emitted by the time
        // this handler samples the public next-event snapshot. Both phases must
        // retain the exact original virtual offsets.
        const uint64_t endDeadline = field.tick * fieldCycles + blankCycles;
        const uint64_t nextStartDeadline = (field.tick + 1u) * fieldCycles;
        check(field.nextDeadline == endDeadline || field.nextDeadline == nextStartDeadline,
            "public snapshot retains an exact End or following Start virtual offset");
        const uint32_t expectedTimer = static_cast<uint32_t>(((field.cycle - timerEpoch) / 512u) & 0xFFFFu);
        check(field.timerCount == expectedTimer, "BUS/256 timer count receives all forwarded EE cycles exactly once");
        if (i > 0u)
        {
            const double gap = std::chrono::duration<double, std::milli>(field.time - starts[i - 1u].time).count();
            std::printf("  tick %llu: %.3fms, virtual next %llu, actual cycle %llu\n",
                static_cast<unsigned long long>(field.tick), gap,
                static_cast<unsigned long long>(field.nextDeadline), static_cast<unsigned long long>(field.cycle));
            if (late && i == 1u)
                check(lateCallDone && gap >= 190.0 && gap < 260.0,
                    "real native call holds an armed consumer across a 200ms overdue field");
            else
                check(gap >= 0.3 && gap < 60.0, "opposite field follows End without slow virtual-credit starvation");
            if(i>=2u)check(std::chrono::duration<double,std::milli>(field.time-starts[i-2u].time).count()>=32.0,
                "same-parity callbacks retain the original gameplay rate bound");
            const auto nextStart = [](const Field &sample) {
                return sample.nextDeadline == sample.tick * fieldCycles + blankCycles
                    ? sample.nextDeadline - blankCycles + fieldCycles : sample.nextDeadline;
            };
            check(nextStart(field) - nextStart(starts[i - 1u]) == fieldCycles,
                "consecutive virtual field deadlines retain their exact period");
            if (i - 1u < ends.size())
            {
                const auto &end = ends[i - 1u];
                check(end.tick == i && end.nextDeadline == (i + 1u) * fieldCycles,
                    "VBlankEnd precedes the next exact virtual field deadline");
                check(end.time > starts[i - 1u].time && end.time < field.time,
                    "actual IRQ delivery orders Start then End then next Start");
            }
        }
    }
    if (ends.size() == wantedFields)
        check(ends.back().tick == wantedFields && ends.back().time > starts.back().time,
            "optional final End still belongs to the final emitted field");
#endif
}
}

int main()
{
    runCase(false);
#if !EXPECT_BOTH_CLOCKS_CONTROL
    runCase(true);
#endif
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
