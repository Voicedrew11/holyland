#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
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
        uint64_t publishedTick;
        uint32_t flag;
        uint64_t field;
        int parity;
        uint64_t cycle;
    };

    std::vector<Sample> samples;
    std::vector<Clock::time_point> endTimes;
    bool slowFrame = true;
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
        samples.push_back({Clock::now(), runtime->eeScheduler().currentVSyncTick(),
            tick, flag, runtime->memory().gs().csr.load(std::memory_order_acquire) & 0x2000ull,
            _mm_cvtsi128_si32(context->r[2]), runtime->eeScheduler().snapshot().eeCycle});
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

int main()
{
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
    check(std::chrono::duration<double, std::milli>(samples.front().time - begin).count() >= 190.0,
        "the guest injected a real slow frame");
    for (size_t index = 0u; index < samples.size(); ++index)
    {
        const Sample &sample = samples[index];
        check(sample.tick == index + 1u, "VBlank ticks remain sequential");
        check(sample.publishedTick == sample.tick, "registered tick is published before resume");
        check(sample.flag == 1u, "registered VSync flag is set before resume");
        check(sample.parity == static_cast<int>(index & 1u), "WaitVSyncTick return parity alternates");
        check(sample.field == ((sample.tick & 1u) ? 0x2000ull : 0ull), "GS FIELD matches the VBlank tick");
        if (index != 0u)
        {
            const double gapMs = std::chrono::duration<double, std::milli>(sample.time - samples[index - 1u].time).count();
            std::printf("tick %llu: gap %.3f ms, parity %d, FIELD 0x%llx, cycle %llu\n",
                static_cast<unsigned long long>(sample.tick), gapMs, sample.parity,
                static_cast<unsigned long long>(sample.field), static_cast<unsigned long long>(sample.cycle));
#if !EXPECT_ORIGINAL_BURST
            check(gapMs >= 15.0, "late VBlank waits cannot catch up in a sub-15ms burst");
#endif
            check(sample.cycle > samples[index - 1u].cycle, "guest cycle time still advances");
            constexpr uint64_t fieldCycles = (16667ull * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
            check(sample.cycle - samples[index - 1u].cycle == fieldCycles,
                "guest-cycle VBlank deadlines retain their exact original field period");
        }
    }
    check(endTimes.size() == kWaitCount - 1u, "each completed field emits its VBlankEnd IRQ before the next field");
#if !EXPECT_ORIGINAL_BURST
    for (size_t index = 0u; index < endTimes.size(); ++index)
    {
        check(endTimes[index] >= samples[index].time, "VBlankEnd follows its emitted field");
        check(endTimes[index] < samples[index + 1u].time, "VBlankEnd precedes the next emitted field");
    }
#endif
    const double followupMs = std::chrono::duration<double, std::milli>(samples.back().time - samples.front().time).count();
#if EXPECT_ORIGINAL_BURST
    check(followupMs < 15.0, "original scheduler reproduces burst after the 200ms slow frame");
    std::printf("Original burst control: five follow-up waits completed in %.3f ms\n", followupMs);
#else
    check(followupMs >= 75.0, "five follow-up waits preserve their real field spacing");
#endif
    std::printf("%zu assertions, %zu failures, total %.3f ms, follow-up %.3f ms\n", assertions, failures,
        std::chrono::duration<double, std::milli>(finish - begin).count(), followupMs);
    std::fflush(stdout);
    std::fflush(stderr);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
