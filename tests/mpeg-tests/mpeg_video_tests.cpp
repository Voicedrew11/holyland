#include "fixtures/video_three_frames.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/MPEG.h"

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
    uint32_t value = 0u;
    std::memcpy(&value, ram + addr, sizeof(value));
    return value;
}
constexpr uint32_t mpeg = 0x120000u, work = 0x130000u, source = 0x140000u;
constexpr uint32_t dmaSource = 0x150000u, image = 0x160000u;
constexpr uint32_t mainPc = 0x110000u, retryPc = mainPc + 4u, decodedPc = mainPc + 8u;
constexpr uint32_t nextPicturePc = mainPc + 12u, endPc = mainPc + 16u;
constexpr uint32_t videoPc = mainPc + 20u, noDataPc = mainPc + 24u, picturePc = mainPc + 28u;
constexpr uint32_t d4 = 0x1000B400u;
std::vector<uint8_t> elementary, packet;
unsigned videoCalls = 0u, noDataCalls = 0u, pictureCalls = 0u, finished = 0u;
bool sequenceOnly = false;

void demux(R5900Context &ctx, uint8_t *ram, PS2Runtime *runtime)
{
    reg(ctx, 4, mpeg); reg(ctx, 5, source); reg(ctx, 6, static_cast<uint32_t>(packet.size()));
    ps2_stubs::sceMpegDemuxPss(ram, &ctx, runtime);
}
void getPicture(R5900Context &ctx, uint8_t *ram, PS2Runtime *runtime)
{
    reg(ctx, 4, mpeg); reg(ctx, 5, image); reg(ctx, 6, 0x460u);
    ps2_stubs::sceMpegGetPicture(ram, &ctx, runtime);
}

void mainEntry(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    ctx->pc = retryPc;
    demux(*ctx, ram, runtime);
}
void retry(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(getRegU32(ctx, 2) == 0u, "rejected video PES does not commit source bytes");
    check(videoCalls == 1u && noDataCalls == 0u, "rejection does not service input before acceptance");
    check(runtime->memory().read32(d4 + 0x10u) == 0u, "rejected video cannot advance input DMA");
    R5900Context empty{}; reg(empty, 4, mpeg);
    ps2_stubs::sceMpegIsRefBuffEmpty(ram, &empty, runtime);
    check(getRegU32(&empty, 2) == 1u, "rejected video never reaches FFmpeg decoded queue");
    check(std::memcmp(ram + source, packet.data(), packet.size()) == 0,
          "original guest video bytes survive the rejected callback");
    ctx->pc = decodedPc;
    demux(*ctx, ram, runtime);
}
void decoded(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(getRegU32(ctx, 2) == packet.size(), "accepted retry commits the complete PES and EOF marker");
    check(videoCalls == 2u && noDataCalls == 1u, "one accepted payload produces one input service");
    const uint32_t retired = static_cast<uint32_t>(elementary.size() / 16u * 16u);
    check(runtime->memory().read32(d4 + 0x10u) == dmaSource + retired,
          "actual decoder consumption retires exactly whole quadwords of DMA input");
    check(runtime->memory().read32(d4 + 0x20u) == 68u - retired / 16u,
          "DMA retains its unconsumed tail rather than fabricating completion");
    if (sequenceOnly)
    {
        R5900Context flush{}; reg(flush, 4, mpeg);
        ps2_stubs::sceMpegFlush(ram, &flush, runtime);
    }
    check(word(ram, work) == 0u, "producer EOF does not truncate decoded pictures");
    check(word(ram, work + 4u) == 3u, "actual FFmpeg decoded reference count reaches original SDK getter");
    ctx->pc = nextPicturePc;
    getPicture(*ctx, ram, runtime);
}
void nextPicture(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(pictureCalls >= 1u && pictureCalls <= 3u, "genuine FFmpeg pictures dispatch in bounded order");
    check(word(ram, mpeg) == 16u && word(ram, mpeg + 4u) == 16u,
          "actual synthetic MPEG-2 dimensions reach guest SDK state");
    if (pictureCalls < 3u)
    {
        ctx->pc = nextPicturePc;
        getPicture(*ctx, ram, runtime);
    }
    else
    {
        if (sequenceOnly)
        {
            R5900Context query{}; reg(query, 4, mpeg);
            ps2_stubs::sceMpegIsEnd(ram, &query, runtime);
            check(word(ram, work) == 0u && getRegU32(&query, 2) == 0u,
                  "B7 sequence end alone cannot complete a drained MPEG program");
            ps2_stubs::notifyMpegCdStreamEof(runtime);
        }
        check(word(ram, work) == 1u, "authoritative program/producer end follows all three genuine handoffs");
        check(word(ram, work + 4u) == 0u, "actual final FFmpeg handoff drains original SDK reference count");
        ctx->pc = endPc;
        runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick() + 3u, 0);
    }
}
void end(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    R5900Context query{}; reg(query, 4, mpeg);
    ps2_stubs::sceMpegIsEnd(ram, &query, runtime);
    check(getRegU32(&query, 2) == 1u, "actual decode and final presentation finish at EOF");
    check(pictureCalls == 3u && noDataCalls == 1u && videoCalls == 2u,
          "rejected retry neither duplicates video decode nor picture/input callbacks");
    R5900Context reset{}; reg(reset, 4, mpeg);
    ps2_stubs::sceMpegReset(ram, &reset, runtime);
    check((runtime->memory().read32(d4) & 0x100u) == 0u && runtime->memory().read32(d4 + 0x20u) == 0u,
          "MPEG reset clears outstanding native input DMA credits and transfer state");
    runtime->memory().write32(d4 + 0x10u, dmaSource);
    runtime->memory().write32(d4 + 0x20u, 1u);
    runtime->memory().write32(d4, 0x101u);
    // The previous 1,065-byte payload left nine bytes of sub-QW credit.
    // Seven new bytes would retire a quadword if reset leaked that credit.
    runtime->memory().creditIpuInputBytes(7u);
    check(runtime->memory().read32(d4 + 0x10u) == dmaSource,
          "reset does not leak prior sub-quadword credit into the next stream");
    ++finished;
    ctx->pc = 0u;
    runtime->requestStop();
}
void video(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    ++videoCalls;
    const uint32_t tuple = getRegU32(ctx, 5);
    check(getRegU32(ctx, 4) == mpeg && word(ram, tuple) == 0u,
          "video stream callback receives the exact handle/type ABI");
    check(getRegU32(ctx, 6) == 0x11223344u && getRegU32(ctx, 7) == 0u,
          "video stream callback preserves userdata and retail a3");
    check(word(ram, tuple + 8u) == source + 9u && word(ram, tuple + 12u) == elementary.size(),
          "video callback points to original accepted elementary payload");
    check(std::memcmp(ram + source + 9u, elementary.data(), elementary.size()) == 0,
          "real synthetic video data remains intact on rejection and retry");
    reg(*ctx, 2, videoCalls == 1u ? 0u : 1u);
    ctx->pc = 0u;
}
void noData(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    ++noDataCalls;
    check(getRegU32(ctx, 4) == mpeg && word(ram, getRegU32(ctx, 5)) == 1u,
          "decoder input service invokes ordinary NODATA type1 ABI");
    check(getRegU32(ctx, 6) == 0x55667788u && videoCalls == 2u,
          "ordinary input service follows stream acceptance and preserves userdata");
    runtime->memory().write32(0x1000E000u, 1u);
    runtime->memory().write32(d4 + 0x10u, dmaSource);
    runtime->memory().write32(d4 + 0x20u, 68u);
    runtime->memory().write32(d4, 0x101u);
    reg(*ctx, 2, 1u);
    ctx->pc = 0u;
}
void picture(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    ++pictureCalls;
    check(word(ram, getRegU32(ctx, 5)) == 4u && getRegU32(ctx, 6) == 0x99AABBCCu,
          "each actual decoded picture dispatches ordinary transfer callback metadata");
    check(word(ram, mpeg + 8u) == pictureCalls - 1u, "decoded picture index has no retry duplicates");
    reg(*ctx, 2, 1u);
    ctx->pc = 0u;
}
}

int main()
{
    try
    {
        elementary.assign(kSyntheticVideo.begin(), kSyntheticVideo.end());
        check(elementary.size() == 1065u, "self-contained synthetic three-frame ES fixture opens");
        const std::vector<uint8_t> originalElementary = elementary;
        for (unsigned mode = 0u; mode < 2u; ++mode)
        {
        sequenceOnly = mode == 1u;
        videoCalls = noDataCalls = pictureCalls = finished = 0u;
        elementary = originalElementary;
        if (sequenceOnly) elementary.insert(elementary.end(), {0u, 0u, 1u, 0xB7u});
        packet = {0u, 0u, 1u, 0xE0u, static_cast<uint8_t>((elementary.size() + 3u) >> 8u),
            static_cast<uint8_t>(elementary.size() + 3u), 0x80u, 0u, 0u};
        packet.insert(packet.end(), elementary.begin(), elementary.end());
        if (!sequenceOnly) packet.insert(packet.end(), {0u, 0u, 1u, 0xB9u});
        auto runtime = std::make_unique<PS2Runtime>();
        check(runtime->memory().initialize(), "real guest memory initializes");
        auto *ram = runtime->memory().getRDRAM();
        ps2_stubs::resetMpegStubState();
        ps2_stubs::notifyMpegCdStreamStart(runtime.get());
        R5900Context create{}; reg(create, 4, mpeg); reg(create, 5, work); reg(create, 6, 0x2000u);
        ps2_stubs::sceMpegCreate(ram, &create, runtime.get());
        std::memcpy(ram + source, packet.data(), packet.size());
        std::memcpy(ram + dmaSource, elementary.data(), elementary.size());
        runtime->registerFunction(mainPc, mainEntry); runtime->registerFunction(retryPc, retry);
        runtime->registerFunction(decodedPc, decoded); runtime->registerFunction(nextPicturePc, nextPicture);
        runtime->registerFunction(endPc, end); runtime->registerFunction(videoPc, video);
        runtime->registerFunction(noDataPc, noData); runtime->registerFunction(picturePc, picture);
        R5900Context stream{}; reg(stream, 4, mpeg); reg(stream, 5, 0u); reg(stream, 6, 0u);
        reg(stream, 7, videoPc); reg(stream, 8, 0x11223344u);
        ps2_stubs::sceMpegAddStrCallback(ram, &stream, runtime.get());
        for (const auto [type, pc, user] : {std::array<uint32_t, 3>{1u, noDataPc, 0x55667788u},
                                          std::array<uint32_t, 3>{4u, picturePc, 0x99AABBCCu}})
        {
            R5900Context add{}; reg(add, 4, mpeg); reg(add, 5, type); reg(add, 6, pc); reg(add, 7, user);
            ps2_stubs::sceMpegAddCallback(ram, &add, runtime.get());
        }
        ps2_stubs::notifyMpegCdStreamDataProduced(
            static_cast<uint32_t>(packet.size() + (sequenceOnly ? 0u : 2044u)), !sequenceOnly);
        R5900Context main{}; main.pc = mainPc;
        runtime->eeScheduler().reset(ram, main);
        runtime->eeScheduler().run();
        check(finished == 1u, "actual video producer and presentation consumer both resume");
        }
        std::cout << "PASS: " << checks << " actual MPEG video, NODATA, and IPU credit checks\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
