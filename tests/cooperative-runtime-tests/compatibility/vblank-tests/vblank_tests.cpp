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

namespace
{
    using Clock = std::chrono::steady_clock;
// Physical slot identities may skip; only the fixed nominal clock is paced.
constexpr uint64_t physicalPeriodNs=16667000ull;
uint64_t physicalCycle(uint64_t slot,uint64_t offsetNs=0u) {
    const uint64_t ns=slot*physicalPeriodNs+offsetNs;
    return (ns/1000000000ull)*EeScheduler::kEeClockHz+
        ((ns%1000000000ull)*EeScheduler::kEeClockHz)/1000000000ull;
}
bool physicalDeadline(uint64_t value) {
    const uint64_t guess=value/physicalCycle(1);
    for(uint64_t slot=guess>1?guess-1:1;slot<=guess+2;++slot)
        if(value==physicalCycle(slot)||value==physicalCycle(slot,500000u))return true;
    return false;
}

    constexpr uint32_t kWaitPc = 0x00160000u;
    constexpr uint32_t kResumePc = 0x00160010u;
    constexpr uint32_t kEndIrqPc = 0x00160020u;
    constexpr uint32_t kFlagAddress = 0x00001800u;
    constexpr uint32_t kTickAddress = 0x00001810u;
    constexpr size_t kWaitCount = 6u;

    struct Sample
    {
        Clock::time_point time;
        uint64_t tick;
        uint64_t publishedCsr;
        uint32_t flag;
        uint64_t field;
        uint64_t csr;
        int parity;
        uint64_t cycle;
        uint64_t nextDeadline;
    };

    std::vector<Sample> samples;
    std::vector<Clock::time_point> endTimes;
    bool slowFrame = true;
    bool lateOppositeField = false;
    int lateOppositeWorkMs = 25;
    size_t assertions = 0u;
    size_t failures = 0u;

    void check(bool condition, const char *message)
    {
        ++assertions;
        if (!condition)
        {
            ++failures;
            std::fprintf(stderr, "FAIL: %s\n", message);
        }
    }

    void endIrq(uint8_t *, R5900Context *context, PS2Runtime *)
    {
        endTimes.push_back(Clock::now());
        context->pc = 0u;
    }

    void wait(uint8_t *rdram, R5900Context *context, PS2Runtime *runtime)
    {
        if (slowFrame)
        {
            slowFrame = false;
            // Leave genuine host-deadline debt without advancing guest cycles.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (lateOppositeField && samples.size() == 1u)
        {
            lateOppositeField = false;
            // Delay only the opposite field. Bounded recovery may shorten
            // its next interval while preserving both same-parity limits.
            std::this_thread::sleep_for(std::chrono::milliseconds(lateOppositeWorkMs));
        }
        runtime->eeScheduler().setVSyncFlag(kFlagAddress, kTickAddress);
        context->pc = kResumePc;
        ps2_syscalls::WaitVSyncTick(rdram, context, runtime, -1);
    }

    void resume(uint8_t *rdram, R5900Context *context, PS2Runtime *runtime)
    {
        uint32_t flag = 0u;
        uint64_t tick = 0u;
        std::memcpy(&flag, rdram + kFlagAddress, sizeof(flag));
        std::memcpy(&tick, rdram + kTickAddress, sizeof(tick));
        runtime->eeScheduler().publishSnapshot();
        const auto clockSnapshot = runtime->eeScheduler().snapshot();
        samples.push_back({Clock::now(), runtime->eeScheduler().currentVSyncTick(),
            tick, flag, runtime->memory().gs().csr.load(std::memory_order_acquire) & 0x2000ull,
            runtime->memory().gs().csr.load(std::memory_order_acquire),
            _mm_cvtsi128_si32(context->r[2]), clockSnapshot.eeCycle, clockSnapshot.nextEventCycle});
        if (samples.size() == kWaitCount)
        {
            context->pc = 0u;
            runtime->requestStop();
        }
        else
        {
            context->pc = kWaitPc;
        }
    }
}

int main(int argc, char **argv)
{
    const bool lateOppositeCase = argc == 2 && std::string(argv[1]) == "--late-opposite";
    const bool lateOpposite35Case = argc == 2 && std::string(argv[1]) == "--late-opposite-35";
    if (argc != 1 && !lateOppositeCase && !lateOpposite35Case)
        return EXIT_FAILURE;
    if (lateOppositeCase || lateOpposite35Case)
    {
        slowFrame = false;
        lateOppositeField = true;
        lateOppositeWorkMs = lateOpposite35Case ? 35 : 25;
    }
    auto runtime = std::make_unique<PS2Runtime>();
    check(runtime->memory().initialize(), "actual runtime memory initializes");
    runtime->registerFunction(kWaitPc, wait);
    runtime->registerFunction(kResumePc, resume);
    runtime->registerFunction(kEndIrqPc, endIrq);
    R5900Context context{};
    context.pc = kWaitPc;
    runtime->eeScheduler().reset(runtime->memory().getRDRAM(), context);
    runtime->eeScheduler().addIrqHandler(false, 3u, kEndIrqPc, true, 0u, 0u, 0u);
    const auto begin = Clock::now();
    runtime->eeScheduler().run();
    const auto finish = Clock::now();

    check(samples.size() == kWaitCount, "all six actual VSync waits resume");
    if (samples.empty())
        return EXIT_FAILURE;
    if (lateOppositeCase || lateOpposite35Case)
        check(samples.size() >= 2u && std::chrono::duration<double, std::milli>(samples[1].time - samples[0].time).count() >= lateOppositeWorkMs - 1.0,
            "the guest injected its configured work before the opposite field");
    else
        check(std::chrono::duration<double, std::milli>(samples.front().time - begin).count() >= 190.0,
            "the guest injected a real slow frame");
    for (size_t index = 0u; index < samples.size(); ++index)
    {
        const Sample &sample = samples[index];
        check(sample.tick != 0u && (index == 0u || sample.tick > samples[index-1u].tick), "delivered physical identities are unique and monotonic");
#if EXPECT_ORIGINAL_BURST
        check(sample.publishedCsr == sample.tick, "original control publishes its software tick");
#else
        check(sample.publishedCsr == sample.csr, "registered 64-bit sampled CSR is published before resume");
#endif
        check(sample.flag == 1u, "registered VSync flag is set before resume");
        check(sample.parity == static_cast<int>((sample.field >> 13u) & 1u), "WaitVSyncTick returns the resumed CSR FIELD");
        check(sample.field == ((sample.tick & 1u) ? 0x2000ull : 0ull), "GS FIELD matches the VBlank tick");
        if (index != 0u)
        {
            const double gapMs = std::chrono::duration<double, std::milli>(sample.time - samples[index - 1u].time).count();
            std::printf("tick %llu: gap %.3f ms, parity %d, FIELD 0x%llx, cycle %llu\n",
                static_cast<unsigned long long>(sample.tick), gapMs, sample.parity,
                static_cast<unsigned long long>(sample.field), static_cast<unsigned long long>(sample.cycle));
#if !EXPECT_ORIGINAL_BURST
            // Observed emissions may recover a fixed physical phase after
            // lateness. The pure physical clock proves nominal slot spacing.
            check(sample.cycle > samples[index - 1u].cycle, "guest cycle time still advances");
            check(physicalDeadline(sample.nextDeadline),
                "public next event remains an exact fixed-epoch physical Start or End cause");
#endif
        }
    }
    check(!endTimes.empty(), "independent physical End IRQ stream makes progress");
    for(size_t index=1;index<endTimes.size();++index)
        check(endTimes[index]>endTimes[index-1], "independent End callbacks never replay the same delivery");
    const double followupMs = std::chrono::duration<double, std::milli>(samples.back().time - samples.front().time).count();
#if EXPECT_ORIGINAL_BURST
    check(followupMs < 15.0, "original scheduler reproduces burst after the 200ms slow frame");
    std::printf("Original burst control: five follow-up waits completed in %.3f ms\n", followupMs);
#else
    check(followupMs >= 64.0, "five follow-up waits preserve two full same-parity intervals");
#endif
    std::printf("%zu assertions, %zu failures, total %.3f ms, follow-up %.3f ms\n", assertions, failures,
        std::chrono::duration<double, std::milli>(finish - begin).count(), followupMs);
    std::fflush(stdout);
    std::fflush(stderr);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
