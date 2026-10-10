#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/MPEG.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
unsigned checks = 0;
void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void reg(R5900Context &ctx, int index, uint32_t value) { SET_GPR_U32(&ctx, index, value); }
uint32_t word(uint8_t *ram, uint32_t address)
{
    uint32_t value = 0;
    std::memcpy(&value, ram + address, sizeof(value));
    return value;
}
constexpr uint32_t mpeg = 0x00120000u;
constexpr uint32_t work = 0x00130000u;
constexpr uint32_t image = 0x00200000u;
constexpr uint32_t firstPc = 0x00011000u;
constexpr uint32_t lastPc = 0x00011004u;
constexpr uint32_t tailPc = 0x00011008u;
constexpr uint32_t updatePc = 0x0001100Cu;
constexpr uint32_t streamPc = 0x00011010u;
constexpr uint32_t emptyResumePc = 0x00011014u;
constexpr uint32_t ordinaryUser = 0x11223344u;
constexpr uint32_t streamUser = 0x55667788u;
unsigned handoffStage = 0;
unsigned ordinaryCalls = 0;
unsigned streamCalls = 0;

void create(PS2Runtime &runtime, uint8_t *ram)
{
    std::memset(ram + work, 0xA5, 0x2000u);
    R5900Context ctx{};
    reg(ctx, 4, mpeg);
    reg(ctx, 5, work);
    reg(ctx, 6, 0x2000u);
    ps2_stubs::sceMpegCreate(ram, &ctx, &runtime);
    check(getRegU32(&ctx, 2) != 0u, "create returns valid ring address");
    check(word(ram, mpeg + 0x40u) == work, "retail getter can resolve SDK work arena");
    check(word(ram, work) == 0u, "create clears poisoned SDK completion field");
    check(word(ram, work + 4u) == 0u, "create clears poisoned SDK reference count before any input");
}

bool retailReferencesEmpty(uint8_t *ram)
{
    // Actual unbound getter at 0x230DE8: work=*(mpeg+0x40),
    // return *(work+4)==0. Exercise that data ABI independently of host state.
    return word(ram, word(ram, mpeg + 0x40u) + 4u) == 0u;
}

void picture(PS2Runtime &runtime, uint8_t *ram)
{
    R5900Context ctx{};
    reg(ctx, 4, mpeg);
    reg(ctx, 5, image);
    reg(ctx, 6, 0x460u);
    ps2_stubs::sceMpegGetPicture(ram, &ctx, &runtime);
    check(static_cast<int32_t>(getRegU32(&ctx, 2)) == 0, "GetPicture succeeds");
}

int query(PS2Runtime &runtime, uint8_t *ram)
{
    R5900Context ctx{};
    reg(ctx, 4, mpeg);
    ps2_stubs::sceMpegIsEnd(ram, &ctx, &runtime);
    return static_cast<int32_t>(getRegU32(&ctx, 2));
}

void firstFrame(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    picture(*runtime, ram);
    check(word(ram, mpeg) == 16u && word(ram, mpeg + 4u) == 16u, "real decoder frame dimensions published");
    check(word(ram, work) == 0u, "handing off first picture does not truncate final picture");
    check(word(ram, work + 4u) == 1u && !retailReferencesEmpty(ram),
          "original SDK ref getter retains the final decoded picture after first pop");
    handoffStage = 1;
    ctx->pc = lastPc;
    reg(*ctx, 4, mpeg);
    reg(*ctx, 5, image);
    reg(*ctx, 6, 0x460u);
    ps2_stubs::sceMpegGetPicture(ram, ctx, runtime);
}

void lastFrame(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(ordinaryCalls == 2u, "ordinary callbacks completed before frame consumer continuation");
    check(streamCalls == 0u, "ordinary type4 dispatch does not invoke registered type4 stream callback");
    check(word(ram, mpeg + 8u) == 1u, "last genuine picture handed off exactly once");
    check(word(ram, work) == 1u, "raw retail getter observes completion after last picture handoff");
    check(word(ram, work + 4u) == 0u && retailReferencesEmpty(ram),
          "original SDK ref getter observes emptiness after final picture handoff");
    check(query(*runtime, ram) == 0, "native end query retains final presentation interval");
    handoffStage = 2;
    ctx->pc = tailPc;
    runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick() + 1u, 0);
}

void presentedTail(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(query(*runtime, ram) == 1, "native end query completes once final picture interval elapses");
    picture(*runtime, ram);
    check(ordinaryCalls == 2u, "empty/blank GetPicture does not synchronously invoke ordinary callback");
    handoffStage = 3;
    ctx->pc = emptyResumePc;
}

void emptyResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
{
    check(ordinaryCalls == 2u && streamCalls == 0u,
          "empty/blank GetPicture does not queue deferred ordinary or stream callback");
    handoffStage = 4;
    ctx->pc = 0u;
    runtime->requestStop();
}

void pictureUpdate(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    ++ordinaryCalls;
    check(getRegU32(ctx, 4) == mpeg, "SDK callback a0 identifies created MPEG handle");
    const uint32_t tuple = getRegU32(ctx, 5);
    check(tuple != 0u && tuple < PS2_RAM_SIZE - 32u, "SDK callback owns valid guest tuple storage");
    check(word(ram, tuple) == 4u, "SDK callback tuple publishes ordinary type4");
    check(getRegU32(ctx, 6) == ordinaryUser, "SDK callback preserves registered user data");
    check(getRegU32(ctx, 7) == 0u, "SDK callback a3 matches retail dispatcher ABI");
    check(word(ram, mpeg + 8u) == ordinaryCalls - 1u, "SDK callback follows each genuine image handoff in order");
    check(ram[image] == 0x80u, "decoded guest image copied before transfer callback");
    reg(*ctx, 2, 1u);
    ctx->pc = 0u;
}

void streamUpdate(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ++streamCalls;
    reg(*ctx, 2, 1u);
    ctx->pc = 0u;
}

void registerUpdateCallbacks(PS2Runtime &runtime, uint8_t *ram)
{
    runtime.registerFunction(updatePc, pictureUpdate);
    runtime.registerFunction(streamPc, streamUpdate);
    R5900Context ordinary{};
    reg(ordinary, 4, mpeg); reg(ordinary, 5, 4u);
    reg(ordinary, 6, updatePc); reg(ordinary, 7, ordinaryUser);
    ps2_stubs::sceMpegAddCallback(ram, &ordinary, &runtime);
    check(getRegU32(&ordinary, 2) != 0u, "ordinary callback registration succeeds");
    R5900Context stream{};
    reg(stream, 4, mpeg); reg(stream, 5, 4u); reg(stream, 6, 0u);
    reg(stream, 7, streamPc); reg(stream, 8, streamUser);
    ps2_stubs::sceMpegAddStrCallback(ram, &stream, &runtime);
    check(getRegU32(&stream, 2) == 0u, "same numeric stream callback registration succeeds");
}
}

int main()
{
    try
    {
        auto runtime = std::make_unique<PS2Runtime>();
        check(runtime->memory().initialize(), "real guest memory initializes");
        uint8_t *ram = runtime->memory().getRDRAM();
        check(ram != nullptr, "real runtime allocates guest memory");
        ps2_stubs::resetMpegStubState();
        R5900Context mainContext{};
        runtime->eeScheduler().reset(ram, mainContext);
        ps2_stubs::notifyMpegCdStreamStart(runtime.get());
        create(*runtime, ram);
        registerUpdateCallbacks(*runtime, ram);
        check(query(*runtime, ram) == 0, "new handle is not ended before producer EOF");

        ps2_stubs::enqueueMpegDecodedFrameForTesting(mpeg);
        ps2_stubs::enqueueMpegDecodedFrameForTesting(mpeg);
        check(word(ram, work + 4u) == 2u && !retailReferencesEmpty(ram),
              "original SDK ref getter follows the decoder queue before handoff");
        ps2_stubs::notifyMpegCdStreamEof(runtime.get());
        check(word(ram, work) == 0u, "EOF retains incomplete status while genuine decoded pictures remain");
        check(query(*runtime, ram) == 0, "public end query also waits for decoded queue");
        runtime->registerFunction(firstPc, firstFrame);
        runtime->registerFunction(lastPc, lastFrame);
        runtime->registerFunction(tailPc, presentedTail);
        runtime->registerFunction(emptyResumePc, emptyResume);
        mainContext.pc = firstPc;
        runtime->eeScheduler().reset(ram, mainContext);
        runtime->eeScheduler().run();
        check(handoffStage == 4u, "real scheduler resumed both frame handoff and final presentation waits");
        check(ordinaryCalls == 2u && streamCalls == 0u, "no blank-frame or double ordinary callback dispatch");

        R5900Context reset{};
        reg(reset, 4, mpeg);
        ps2_stubs::sceMpegReset(ram, &reset, runtime.get());
        check(word(ram, work) == 0u, "reset clears raw SDK completion status");
        check(retailReferencesEmpty(ram), "reset clears SDK reference state for arena reuse");
        ps2_stubs::notifyMpegCdStreamStart(runtime.get());
        check(word(ram, work) == 0u, "new CD generation preserves SDK arena and clears completion");
        check(query(*runtime, ram) == 0, "new generation does not inherit old EOF");
        ps2_stubs::enqueueMpegDecodedFrameForTesting(mpeg);
        picture(*runtime, ram);
        check(word(ram, work) == 0u, "empty decoded queue without producer EOF remains incomplete");
        ps2_stubs::notifyMpegCdStreamEof(runtime.get());
        check(word(ram, work) == 1u, "EOF updates SDK status when decoded queue already drained");

        create(*runtime, ram);
        check(word(ram, work) == 0u, "recreate clears completion even after finished stream");
        runtime->requestStop();
        std::cout << "PASS: " << checks << " actual MPEG SDK lifecycle checks\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
