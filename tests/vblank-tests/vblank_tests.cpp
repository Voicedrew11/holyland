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
        check(sample.tick == index + 1u, "VBlank ticks remain sequential");
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
            // Opposite fields may recover phase. Bound same-parity resume
            // observations, allowing 1.334ms of callback timestamp jitter.
            if (index >= 2u)
            {
                const double parityGapMs = std::chrono::duration<double, std::milli>(sample.time - samples[index - 2u].time).count();
                check(parityGapMs >= 32.0, "same-parity field waits preserve the original 30Hz gameplay bound");
            }
#endif
            check(sample.cycle > samples[index - 1u].cycle, "guest cycle time still advances");
            constexpr uint64_t fieldCycles = (16667ull * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
            constexpr uint64_t blankCycles = (500ull * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
            const auto scheduledStart = [](const Sample &value) {
                const uint64_t end = value.tick * fieldCycles + blankCycles;
                return value.nextDeadline == end ? value.nextDeadline - blankCycles
                    : value.nextDeadline - fieldCycles;
            };
            check(sample.nextDeadline == sample.tick * fieldCycles + blankCycles ||
                sample.nextDeadline == (sample.tick + 1u) * fieldCycles,
                "snapshot retains an original scheduled End or following Start offset");
            check(scheduledStart(sample) - scheduledStart(samples[index - 1u]) == fieldCycles,
                "scheduled field cycles retain exact original offsets independently of elapsed hardware time");
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
    check(followupMs >= 64.0, "five follow-up waits preserve two full same-parity intervals");
#endif
    std::printf("%zu assertions, %zu failures, total %.3f ms, follow-up %.3f ms\n", assertions, failures,
        std::chrono::duration<double, std::milli>(finish - begin).count(), followupMs);
    std::fflush(stdout);
    std::fflush(stderr);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
