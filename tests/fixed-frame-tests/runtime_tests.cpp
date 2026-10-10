// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "test_environment.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr uint32_t boundary = 0x20B780, continuation = 0x20B7EC, field = 0x180000;
constexpr uint32_t sp = 0x01010000, caller = 0x180004;
unsigned calls = 0, fields = 0, failures = 0;
unsigned timerCalls = 0;
std::vector<Clock::time_point> times;
std::vector<Clock::time_point> fieldTimes;
void check(bool value, const char *message)
{
    if (!value)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
void untouched(uint8_t *, R5900Context *, PS2Runtime *)
{
    check(false, "replaced limiter entry executed");
}
void originalTimer(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ++timerCalls;
    ctx->pc = getRegU32(ctx, 31);
}
void vsync(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ++fields;
    fieldTimes.push_back(Clock::now());
    ctx->pc = 0;
}
void resume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    times.push_back(Clock::now());
    auto &memory = runtime->memory();
    check(getRegU32(ctx, 29) == sp - 64, "original stack frame reconstructed");
    check(memory.read64(sp - 64) == 0x1111 && memory.read64(sp - 48) == 0x2222 && memory.read64(sp - 32) == 0x3333 &&
              memory.read64(sp - 16) == caller,
          "callee saved registers preserved");
    check(getRegU32(ctx, 17) == 0x430000 && getRegU32(ctx, 18) == 0x430000, "original swap arguments initialized");
    check(getRegU32(ctx, 4) == 0 && getRegU32(ctx, 2) == 0, "cache call has original arguments and result");
    SET_GPR_U32(ctx, 29, sp);
    SET_GPR_U32(ctx, 16, 0x1111);
    SET_GPR_U32(ctx, 17, 0x2222);
    SET_GPR_U32(ctx, 18, 0x3333);
    SET_GPR_U32(ctx, 31, caller);
    ctx->pc = boundary;
    if (++calls == 16)
        runtime->requestStop();
}
} // namespace
int main()
{
    setTestEnvironment("PS2X_GS_BACKEND", "cpu");
    setTestEnvironment("PS2X_KFIV_FIXED_FRAME", "1");
    setTestEnvironment("PS2X_FRAME_INTERPOLATION", "0");
    // Two consecutive runtimes also exercise teardown and fresh pacing state.
    for (unsigned instance = 0; instance < 2; ++instance)
    {
        calls = fields = 0;
        times.clear();
        fieldTimes.clear();
        auto runtime = std::make_unique<PS2Runtime>();
        check(runtime->memory().initialize(), "RAM allocated");
        runtime->registerFunction(boundary, untouched);
        runtime->registerFunction(continuation, resume);
        runtime->registerFunction(field, vsync);
        runtime->registerFunction(0x20D078, originalTimer);
        auto &memory = runtime->memory();
        memory.write32(boundary, 0x27BDFFC0);
        memory.write32(0x20B7A8, 0x3442B0C7);
        memory.write32(continuation, 0x8F85A408);
        memory.write32(0x20D078, 0x3C021000);
        memory.write32(0x20D07C, 0xAF80A414);
        memory.write32(0x20D080, 0x03E00008);
        memory.write32(0x20D084, 0xAC400000);
        setTestEnvironment("PS2X_FRAME_INTERPOLATION", "1");
        ps2_game_overrides::applyMatching(*runtime, "unrelated.elf", 0x100008, 0xBDA82D37, true);
        check(runtime->lookupFunction(boundary) == untouched, "unrelated ELF unchanged");
        ps2_game_overrides::applyMatching(*runtime, "SLUS_203.18", 0x100008, 0xBDA82D36, true);
        check(runtime->lookupFunction(boundary) == untouched, "wrong hash unchanged");
        ps2_game_overrides::applyMatching(*runtime, "SLUS_203.18", 0x100008, 0xBDA82D37, true);
        check(runtime->lookupFunction(boundary) != untouched, "verified game limiter bound");
        ps2_game_overrides::applyMatching(*runtime, "SLUS_203.18", 0x100008, 0xBDA82D37, true);
        R5900Context timerContext{};
        SET_GPR_U32(&timerContext, 31, 0x1002E8);
        timerCalls = 0;
        runtime->lookupFunction(0x20D078)(memory.getRDRAM(), &timerContext, runtime.get());
        check(timerCalls == 1 && timerContext.pc == 0x1002E8,
              "repeated registration invokes the original timer exactly once");
        setTestEnvironment("PS2X_FRAME_INTERPOLATION", "0");
        R5900Context context{};
        context.pc = boundary;
        SET_GPR_U32(&context, 29, sp);
        SET_GPR_U32(&context, 16, 0x1111);
        SET_GPR_U32(&context, 17, 0x2222);
        SET_GPR_U32(&context, 18, 0x3333);
        SET_GPR_U32(&context, 31, caller);
        auto &scheduler = runtime->eeScheduler();
        scheduler.reset(memory.getRDRAM(), context);
        scheduler.setGsVSyncCallback(field, 0, 0);
        const auto start = Clock::now();
        std::jthread watchdog([&](std::stop_token stop) {
            for (int n = 0; n < 300 && !stop.stop_requested(); ++n)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!stop.stop_requested())
                runtime->requestStop();
        });
        scheduler.run();
        watchdog.request_stop();
        watchdog.join();
        check(calls == 16, "host deadline wakes original continuation exactly once per update");
        const auto physicalFields=memory.gs().vsyncTick.load(std::memory_order_acquire);
        std::printf("instance=%u updates=%u physical-fields=%llu callbacks=%u elapsed=%.6f\n", instance, calls,
                    static_cast<unsigned long long>(physicalFields), fields,
                    std::chrono::duration<double>(Clock::now() - start).count());
        // The physical clock is independent of coalesced guest delivery. WSL
        // may deliver fewer callbacks when a wake misses a field boundary.
        check(physicalFields >= 25, "physical fields continue during native waits");
        check(fieldTimes.size() > 1 && fieldTimes.back()-fieldTimes.front() >= std::chrono::milliseconds(350),
              "guest field callbacks continue throughout native waits");
        if (times.size() == 16)
        {
            const auto span = std::chrono::duration<double>(times.back() - times.front()).count();
            check(span >= 0.49 && span < 1.0, "16 game updates retain the NTSC interval");
            check(times.front() - start < std::chrono::milliseconds(100), "new runtime has no stale frame deadline");
        }
    }
    std::printf("native frame integration: %u failures\n", failures);
    return failures ? 1 : 0;
}
