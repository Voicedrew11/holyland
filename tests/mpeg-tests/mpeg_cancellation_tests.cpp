#include "fixtures/video_three_frames.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/MPEG.h"
#include "Syscalls/Helpers/State.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
unsigned checks = 0u;
void check(bool value, const char *message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void reg(R5900Context &ctx, int index, uint32_t value) { SET_GPR_U32(&ctx, index, value); }
uint32_t word(uint8_t *ram, uint32_t addr)
{
    uint32_t result = 0u;
    std::memcpy(&result, ram + addr, sizeof(result));
    return result;
}
constexpr uint32_t mpeg = 0x120000u, work = 0x130000u, source = 0x140000u;
constexpr uint32_t sentinel = 0x150000u, image = 0x160000u;
constexpr uint32_t firstPc = 0x110000u, freshPc = firstPc + 4u, callbackPc = firstPc + 8u;
constexpr uint32_t videoPc = firstPc + 12u, controlPc = firstPc + 16u, noDataPc = firstPc + 20u;
constexpr uint32_t producerPc = firstPc + 24u, producerDonePc = firstPc + 28u;
enum class ResetOperation { Reset, Delete, Recreate, Reopen, Init };
unsigned calls = 0u, videoCalls = 0u, noDataCalls = 0u, completed = 0u;
uint32_t tupleAllocation = 0u;
bool pictureCase = false, cancelNoData = false;
std::vector<uint8_t> videoPacket;

void create(PS2Runtime *runtime, uint8_t *ram)
{
    R5900Context ctx{}; reg(ctx, 4, mpeg); reg(ctx, 5, work); reg(ctx, 6, 0x2000u);
    ps2_stubs::sceMpegCreate(ram, &ctx, runtime);
}
void add(PS2Runtime *runtime, uint8_t *ram, bool ordinary, uint32_t type, uint32_t pc, uint32_t data)
{
    R5900Context ctx{}; reg(ctx, 4, mpeg); reg(ctx, 5, type);
    if (ordinary)
    {
        reg(ctx, 6, pc); reg(ctx, 7, data);
        ps2_stubs::sceMpegAddCallback(ram, &ctx, runtime);
    }
    else
    {
        reg(ctx, 7, pc); reg(ctx, 8, data);
        ps2_stubs::sceMpegAddStrCallback(ram, &ctx, runtime);
    }
}
void reset(PS2Runtime *runtime, uint8_t *ram, ResetOperation operation)
{
    R5900Context ctx{}; reg(ctx, 4, mpeg);
    switch (operation)
    {
    case ResetOperation::Reset: ps2_stubs::sceMpegReset(ram, &ctx, runtime); break;
    case ResetOperation::Delete: ps2_stubs::sceMpegDelete(ram, &ctx, runtime); break;
    case ResetOperation::Recreate: create(runtime, ram); break;
    case ResetOperation::Reopen: ps2_stubs::notifyMpegCdStreamStart(runtime); break;
    case ResetOperation::Init: ps2_stubs::sceMpegInit(ram, &ctx, runtime); break;
    }
}
void reclaimTuple(PS2Runtime *runtime)
{
    const uint32_t reclaimed = runtime->guestMalloc(32u, 16u);
    check(reclaimed == tupleAllocation, "cancelled invocation completion releases its actual guest tuple");
    runtime->guestFree(reclaimed);
}
void picture(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    reg(*ctx, 4, mpeg); reg(*ctx, 5, image); reg(*ctx, 6, 0x460u);
    ps2_stubs::sceMpegGetPicture(ram, ctx, runtime);
}
void audio(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    reg(*ctx, 4, mpeg); reg(*ctx, 5, source); reg(*ctx, 6, 18u);
    ps2_stubs::sceMpegDemuxPss(ram, ctx, runtime);
}
void callback(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    ++calls;
    check(getRegU32(ctx, 6) == 0xBBu, "stale callback never reaches reused handle; fresh userdata does");
    check(word(ram, getRegU32(ctx, 5)) == (pictureCase ? 4u : 1u),
          "fresh callback retains the correct ordinary/stream type");
    ram[sentinel] = 0x66u;
    reg(*ctx, 2, 1u); ctx->pc = 0u;
}
void first(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(calls == 0u && ram[sentinel] == 0x55u,
          "queued callback cannot enter guest code after reset/delete/recreate/reopen/init");
    reclaimTuple(runtime);
    reset(runtime, ram, ResetOperation::Delete);
    create(runtime, ram);
    add(runtime, ram, pictureCase, pictureCase ? 4u : 1u, callbackPc, 0xBBu);
    ctx->pc = freshPc;
    if (pictureCase)
    {
        ps2_stubs::enqueueMpegDecodedFrameForTesting(mpeg);
        picture(ram, ctx, runtime);
    }
    else
        audio(ram, ctx, runtime);
}
void fresh(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(calls == 1u && ram[sentinel] == 0x66u, "new instance still executes its genuine callback exactly once");
    if (!pictureCase)
        check(getRegU32(ctx, 2) == 18u, "new stream producer resumes after fresh accepted callback");
    ++completed; ctx->pc = 0u; runtime->requestStop();
}
void control(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    reset(runtime, ram, ResetOperation::Reset);
    ctx->pc = 0u;
}
void video(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    ++videoCalls;
    check(word(ram, getRegU32(ctx, 5)) == 0u, "accepted video callback precedes NODATA service");
    if (cancelNoData)
    {
        // FIFO: this reset runs after video acceptance queues the NODATA call,
        // but immediately before that NODATA invocation first enters guest code.
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::RpcCallback;
        invocation.context.pc = controlPc;
        runtime->eeScheduler().queueInvocation(std::move(invocation));
    }
    reg(*ctx, 2, 1u); ctx->pc = 0u;
}
void noData(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    ++noDataCalls;
    check(word(ram, getRegU32(ctx, 5)) == 1u && getRegU32(ctx, 6) == 0xCCu,
          "uncancelled NODATA retains its registered tuple and userdata");
    ram[sentinel] = 0x66u;
    reg(*ctx, 2, 1u); ctx->pc = 0u;
}
void producer(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    reg(*ctx, 4, mpeg); reg(*ctx, 5, source); reg(*ctx, 6, static_cast<uint32_t>(videoPacket.size()));
    ctx->pc = producerDonePc;
    ps2_stubs::sceMpegDemuxPss(ram, ctx, runtime);
}
void producerDone(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(videoCalls == 1u, "accepted video callback is not replayed during cancellation");
    check(noDataCalls == (cancelNoData ? 0u : 1u), "NODATA guard cancels only a replaced transaction");
    check(ram[sentinel] == (cancelNoData ? 0x55u : 0x66u), "cancelled NODATA cannot mutate reused guest state");
    check(static_cast<int32_t>(getRegU32(ctx, 2)) ==
              (cancelNoData ? KE_WAIT_DELETE : static_cast<int32_t>(videoPacket.size())),
          "suspended producer resumes with cancellation or its genuine committed prefix");
    reclaimTuple(runtime);
    ++completed; ctx->pc = 0u; runtime->requestStop();
}
std::unique_ptr<PS2Runtime> runtimeWithMemory()
{
    auto runtime = std::make_unique<PS2Runtime>();
    check(runtime->memory().initialize(), "real guest memory initializes for cancellation case");
    ps2_stubs::resetMpegStubState();
    return runtime;
}
void runResetCase(ResetOperation operation, bool ordinaryPicture)
{
    calls = completed = 0u; pictureCase = ordinaryPicture;
    auto runtime = runtimeWithMemory();
    auto *ram = runtime->memory().getRDRAM();
    R5900Context main{}; main.pc = firstPc;
    runtime->eeScheduler().reset(ram, main);
    runtime->registerFunction(firstPc, first); runtime->registerFunction(freshPc, fresh);
    runtime->registerFunction(callbackPc, callback);
    ps2_stubs::notifyMpegCdStreamStart(runtime.get());
    create(runtime.get(), ram);
    add(runtime.get(), ram, pictureCase, pictureCase ? 4u : 1u, callbackPc, 0xAAu);
    tupleAllocation = runtime->guestMalloc(32u, 16u); runtime->guestFree(tupleAllocation);
    constexpr std::array<uint8_t, 18u> pes{0u, 0u, 1u, 0xBDu, 0u, 12u, 0x80u, 0u, 0u,
                                          0xFFu, 0xA0u, 0u, 0u, 1u, 2u, 3u, 4u, 5u};
    std::memcpy(ram + source, pes.data(), pes.size()); ram[sentinel] = 0x55u;
    R5900Context queue{};
    if (pictureCase)
    {
        ps2_stubs::enqueueMpegDecodedFrameForTesting(mpeg);
        picture(ram, &queue, runtime.get());
    }
    else
        audio(ram, &queue, runtime.get());
    reset(runtime.get(), ram, operation);
    runtime->eeScheduler().run();
    check(completed == 1u, "cancelled and fresh callback cases complete without stranding the scheduler");
}
void runNoDataCase(bool cancel)
{
    videoCalls = noDataCalls = completed = 0u; cancelNoData = cancel;
    auto runtime = runtimeWithMemory(); auto *ram = runtime->memory().getRDRAM();
    R5900Context main{}; main.pc = producerPc;
    runtime->eeScheduler().reset(ram, main);
    runtime->registerFunction(producerPc, producer); runtime->registerFunction(producerDonePc, producerDone);
    runtime->registerFunction(videoPc, video); runtime->registerFunction(controlPc, control);
    runtime->registerFunction(noDataPc, noData);
    ps2_stubs::notifyMpegCdStreamStart(runtime.get()); create(runtime.get(), ram);
    add(runtime.get(), ram, false, 0u, videoPc, 0u);
    add(runtime.get(), ram, true, 1u, noDataPc, 0xCCu);
    tupleAllocation = runtime->guestMalloc(32u, 16u); runtime->guestFree(tupleAllocation);
    std::memcpy(ram + source, videoPacket.data(), videoPacket.size()); ram[sentinel] = 0x55u;
    runtime->eeScheduler().run();
    check(completed == 1u, "NODATA cancellation resumes its actual suspended producer");
}
}

int main()
{
    try
    {
        for (auto operation : {ResetOperation::Reset, ResetOperation::Delete, ResetOperation::Recreate,
                               ResetOperation::Reopen, ResetOperation::Init})
            for (bool ordinary : {false, true}) runResetCase(operation, ordinary);
        std::vector<uint8_t> elementary(kSyntheticVideo.begin(), kSyntheticVideo.end());
        check(elementary.size() == 1065u, "actual synthetic video fixture opens for NODATA cancellation");
        videoPacket = {0u, 0u, 1u, 0xE0u, static_cast<uint8_t>((elementary.size() + 3u) >> 8u),
            static_cast<uint8_t>(elementary.size() + 3u), 0x80u, 0u, 0u};
        videoPacket.insert(videoPacket.end(), elementary.begin(), elementary.end());
        runNoDataCase(true); runNoDataCase(false);
        std::cout << "PASS: " << checks << " actual MPEG queued cancellation and reuse checks\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
