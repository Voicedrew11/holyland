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

namespace {
unsigned checks = 0, pictures = 0, audioCalls = 0, services = 0, secondary = 0, retries = 0, finished = 0;
bool audioPhase = false, accepting = false;
unsigned operation = 0;
uint64_t firstServiceTick = 0;
constexpr uint32_t mpeg = 0x120000, work = 0x130000, source = 0x140000, image = 0x160000;
constexpr uint32_t beginPc = 0x110000, nextPc = beginPc + 4, attemptPc = beginPc + 8;
constexpr uint32_t resultPc = beginPc + 12, audioPc = beginPc + 16, updatePc = beginPc + 20;
constexpr uint32_t secondaryPc = beginPc + 24;
constexpr std::array<uint8_t, 18> packet{0,0,1,0xbd,0,12,0x80,0,0,0xff,0xa0,0,0,1,0x10,0x20,0x30,0x40};
void check(bool value, const char *message) { ++checks; if (!value) throw std::runtime_error(message); }
void reg(R5900Context &ctx, int index, uint32_t value) { SET_GPR_U32(&ctx, index, value); }
uint32_t word(uint8_t *ram, uint32_t addr) { uint32_t value; std::memcpy(&value, ram + addr, 4); return value; }
void picture(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    ctx->pc = nextPc;
    reg(*ctx, 4, mpeg); reg(*ctx, 5, image); reg(*ctx, 6, 0x460);
    ps2_stubs::sceMpegGetPicture(ram, ctx, runtime);
}
void next(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    if (operation == 5 && pictures == 1) {
        check(word(ram, work + 4) == 2, "guard case retains genuine queued pictures");
        secondary = 0; audioPhase = true; ctx->pc = attemptPc;
        runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick() + 1, 0);
        return;
    }
    if (pictures < 3) { picture(ram, ctx, runtime); return; }
    check(word(ram, work + 4) == 0, "real video reference queue has drained before audio backpressure");
    check(word(ram, mpeg + 8) == 2, "three genuine decoded pictures reached the client");
    check(secondary == 3, "all registered ordinary callbacks ran for real pictures");
    secondary = 0; audioPhase = true; ctx->pc = attemptPc;
    runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick() + 1, 0);
}
void attempt(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    ctx->pc = resultPc; reg(*ctx, 4, mpeg); reg(*ctx, 5, source); reg(*ctx, 6, static_cast<uint32_t>(packet.size()));
    ps2_stubs::sceMpegDemuxPss(ram, ctx, runtime);
}
void result(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    if (operation >= 4) {
        check(getRegU32(ctx, 2) == 0 && services == 0 && secondary == 0,
              "audio-only or queued-video clients do not receive synthetic output service");
        check(std::memcmp(ram + source, packet.data(), packet.size()) == 0,
              "guarded audio retries preserve their pending PES");
        if (++retries < 8) { attempt(ram, ctx, runtime); return; }
        ++finished; ctx->pc = 0; runtime->requestStop(); return;
    } else if (operation != 0) {
        check(static_cast<int32_t>(getRegU32(ctx, 2)) < 0, "reset/delete/new generation cancels stalled demux return");
        check(services == 1 && secondary == 0, "cancellation prevents later queued output callbacks");
    } else if (!accepting) {
        check(getRegU32(ctx, 2) == 0, "output service cannot commit rejected source bytes");
        check(services == 1, "audio starvation services the existing UPDATE boundary");
        check(secondary == 1, "registered output callbacks retain sequential execution");
        check(std::memcmp(ram + source, packet.data(), packet.size()) == 0, "retries preserve the original pending PES");
        if (++retries < 8) { attempt(ram, ctx, runtime); return; }
        check(runtime->eeScheduler().currentVSyncTick() == firstServiceTick, "eight rejected retries stayed in the same VSync");
        ctx->pc = attemptPc;
        runtime->eeScheduler().waitVSync(firstServiceTick + 1, 0);
        return;
    } else if (getRegU32(ctx, 2) == 0) {
        check(services == 2 && secondary == 2, "the next VSync pumps real consumer progress exactly once");
        attempt(ram, ctx, runtime); return;
    } else {
        check(getRegU32(ctx, 2) == packet.size(), "only explicit stream acceptance commits the complete PES");
        check(audioCalls == 10, "all rejected callbacks retain a single accepted retry");
        check(services == 2 && secondary == 2, "successful acceptance does not duplicate output service");
    }
    if (operation == 0)
        check(word(ram, mpeg + 8) == 2, "audio service never fabricates another picture");
    check(word(ram, work + 4) == 0, "audio service leaves the drained video reference count intact");
    ++finished; ctx->pc = 0; runtime->requestStop();
}
void audio(uint8_t *ram, R5900Context *ctx, PS2Runtime *) {
    ++audioCalls;
    const uint32_t tuple = getRegU32(ctx, 5);
    check(word(ram, tuple) == 2 && getRegU32(ctx, 6) == 0xaabbccdd, "actual stream callback preserves type and userdata");
    check(word(ram, tuple + 8) == source + 9 && word(ram, tuple + 12) == 9, "audio callback retains the original payload address and length");
    reg(*ctx, 2, accepting ? 1 : 0); ctx->pc = 0;
}
void update(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    const uint32_t tuple = getRegU32(ctx, 5);
    check(word(ram, tuple) == 4 && getRegU32(ctx, 6) == 0x11223344, "existing ordinary UPDATE ABI is used for audio service");
    check(word(ram, tuple + 8) == 0 && word(ram, tuple + 12) == 0, "output service does not invent a stream payload");
    if (!audioPhase) ++pictures;
    else {
        ++services;
        check(word(ram, mpeg + 8) == 2, "additional output service preserves the last genuine picture number");
        if (services == 1) firstServiceTick = runtime->eeScheduler().currentVSyncTick();
        if (services == 2) {
            check(runtime->eeScheduler().currentVSyncTick() > firstServiceTick, "additional output service is limited to another VSync");
            accepting = true;
        }
        if (operation) {
            R5900Context cancel{}; reg(cancel, 4, mpeg);
            if (operation == 1) ps2_stubs::sceMpegReset(ram, &cancel, runtime);
            if (operation == 2) ps2_stubs::sceMpegDelete(ram, &cancel, runtime);
            if (operation == 3) ps2_stubs::notifyMpegCdStreamStart(runtime);
        }
    }
    reg(*ctx, 2, 1); ctx->pc = 0;
}
void extra(uint8_t *, R5900Context *ctx, PS2Runtime *) { ++secondary; reg(*ctx, 2, 1); ctx->pc = 0; }
}
int main() {
    try {
        const std::vector<uint8_t> video(kSyntheticVideo.begin(), kSyntheticVideo.end());
        check(video.size() == 1065, "self-contained synthetic MPEG-2 input opens");
        for (operation = 0; operation < 6; ++operation) {
            pictures = audioCalls = services = secondary = retries = finished = 0;
            audioPhase = accepting = false;
            auto runtime = std::make_unique<PS2Runtime>();
            check(runtime->memory().initialize(), "real guest memory initializes");
            auto *ram = runtime->memory().getRDRAM();
            ps2_stubs::resetMpegStubState(); ps2_stubs::notifyMpegCdStreamStart(runtime.get());
            R5900Context create{}; reg(create,4,mpeg);reg(create,5,work);reg(create,6,0x2000);
            ps2_stubs::sceMpegCreate(ram,&create,runtime.get());
            if (operation != 4) {
                std::memcpy(ram + source, video.data(), video.size());
                R5900Context feed{};reg(feed,4,mpeg);reg(feed,5,source);reg(feed,6,static_cast<uint32_t>(video.size()));
                ps2_stubs::sceMpegAddBs(ram,&feed,runtime.get());
                reg(feed,4,mpeg);ps2_stubs::sceMpegFlush(ram,&feed,runtime.get());
                check(word(ram,work+4)==3,"actual FFmpeg decodes three synthetic pictures without producer EOF");
            }
            std::memcpy(ram + source, packet.data(), packet.size());
            runtime->registerFunction(beginPc,picture); runtime->registerFunction(nextPc,next);
            runtime->registerFunction(attemptPc,attempt);runtime->registerFunction(resultPc,result);
            runtime->registerFunction(audioPc,audio);runtime->registerFunction(updatePc,update);
            runtime->registerFunction(secondaryPc,extra);
            R5900Context add{};reg(add,4,mpeg);reg(add,5,2);reg(add,6,0);reg(add,7,audioPc);reg(add,8,0xaabbccdd);
            ps2_stubs::sceMpegAddStrCallback(ram,&add,runtime.get());
            for (uint32_t pc : {updatePc,secondaryPc}) {
                reg(add,4,mpeg);reg(add,5,4);reg(add,6,pc);reg(add,7,0x11223344);
                ps2_stubs::sceMpegAddCallback(ram,&add,runtime.get());
            }
            R5900Context ctx{};ctx.pc = operation == 4 ? attemptPc : beginPc;
            runtime->eeScheduler().reset(ram,ctx);runtime->eeScheduler().run();
            check(finished==1,"actual guest callbacks complete the stalled movie client");
        }
        std::cout << "PASS: " << checks << " real decoder/audio service, retry, VSync and cancellation checks\n";
#ifdef MPEG_AUDIO_SERVICE_OLD_CONTROL
        std::cerr << "FAIL: old control unexpectedly serviced the starved audio client\n";
        return 1;
#else
        return 0;
#endif
    } catch (const std::exception &e) {
#ifdef MPEG_AUDIO_SERVICE_OLD_CONTROL
        if (checks == 16 && std::strcmp(e.what(), "audio starvation services the existing UPDATE boundary") == 0) {
            std::cout << "PASS: old control reproduces audio starvation at check16, preserving a rejected PES without consumer service\n";
            return 0;
        }
#endif
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1;
    }
}
