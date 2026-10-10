// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixtures/video_three_frames.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/MPEG.h"

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks = 0, operation = 0, videos = 0, inputs = 0, updates = 0, finished = 0;
unsigned otherVideos = 0, otherInputs = 0;
size_t copied = 0;
std::vector<uint32_t> videoUsers, inputUsers, updateUsers;
constexpr uint32_t mpeg = 0x120000, work = 0x130000, source = 0x140000;
constexpr uint32_t image = 0x160000, copiedSource = 0x170000;
constexpr uint32_t otherMpeg = 0x180000, otherWork = 0x190000;
constexpr uint32_t beginPc = 0x110000, demuxDonePc = beginPc + 4, pictureDonePc = beginPc + 8;
constexpr uint32_t otherDonePc = beginPc + 12, videoPc = beginPc + 16, inputPc = beginPc + 20;
constexpr uint32_t updatePc = beginPc + 24, otherVideoPc = beginPc + 28, otherInputPc = beginPc + 32;
std::vector<uint8_t> packet;
void check(bool value, const char *message) { ++checks; if (!value) throw std::runtime_error(message); }
void reg(R5900Context &ctx, int index, uint32_t value) { SET_GPR_U32(&ctx, index, value); }
uint32_t word(uint8_t *ram, uint32_t address) { uint32_t value; std::memcpy(&value, ram + address, 4); return value; }
void create(uint8_t *ram, PS2Runtime *runtime, uint32_t handle, uint32_t arena) {
    R5900Context ctx{}; reg(ctx, 4, handle); reg(ctx, 5, arena); reg(ctx, 6, 0x2000);
    ps2_stubs::sceMpegCreate(ram, &ctx, runtime);
}
void add(uint8_t *ram, PS2Runtime *runtime, uint32_t handle, uint32_t user, bool other = false) {
    R5900Context ctx{}; reg(ctx, 4, handle); reg(ctx, 5, 0); reg(ctx, 6, 0);
    reg(ctx, 7, other ? otherVideoPc : videoPc); reg(ctx, 8, user);
    ps2_stubs::sceMpegAddStrCallback(ram, &ctx, runtime);
    for (auto [type, pc] : {std::pair{1u, other ? otherInputPc : inputPc}, std::pair{4u, updatePc}}) {
        if (other && type == 4) continue;
        reg(ctx, 5, type); reg(ctx, 6, pc); reg(ctx, 7, user);
        ps2_stubs::sceMpegAddCallback(ram, &ctx, runtime);
    }
}
void demux(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime, uint32_t handle) {
    reg(*ctx, 4, handle); reg(*ctx, 5, source); reg(*ctx, 6, static_cast<uint32_t>(packet.size()));
    ps2_stubs::sceMpegDemuxPss(ram, ctx, runtime);
}
void begin(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    ctx->pc = demuxDonePc; demux(ram, ctx, runtime, mpeg);
}
void demuxDone(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    check(getRegU32(ctx, 2) == packet.size(), "the genuine native demux commits one complete PES");
    check(videos == 1 && copied == kSyntheticVideo.size(),
          "Create must clear prior stream registrations before re-registering one consumer");
    check(videoUsers == std::vector<uint32_t>{operation == 1 ? 0xaau : 0xbbu},
          "Create replaces old userdata while Reset preserves its existing consumer");
    check(inputs == 1 && inputUsers == videoUsers, "ordinary input service retains exactly one live registration");
    check(std::memcmp(ram + copiedSource, kSyntheticVideo.data(), kSyntheticVideo.size()) == 0,
          "the registered consumer copies the original compressed bytes exactly once");
    check(word(ram, work + 4) == 3, "one accepted stream produces three actual FFmpeg pictures");
    ctx->pc = pictureDonePc; reg(*ctx, 4, mpeg); reg(*ctx, 5, image); reg(*ctx, 6, 0x460);
    ps2_stubs::sceMpegGetPicture(ram, ctx, runtime);
}
void pictureDone(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    check(updates == 1 && inputs == 2 && updateUsers == videoUsers && inputUsers.back() == videoUsers.front(),
          "recreated picture handoff invokes only the current ordinary callback tuple");
    check(word(ram, work + 4) == 2 && word(ram, mpeg + 8) == 0,
          "recreation does not duplicate the genuine decoded picture accounting");
    ctx->pc = otherDonePc; demux(ram, ctx, runtime, otherMpeg);
}
void otherDone(uint8_t *, R5900Context *ctx, PS2Runtime *runtime) {
    check(getRegU32(ctx, 2) == packet.size() && otherVideos == 1 && otherInputs == 1,
          "Create, Reset and Delete leave another MPEG handle's callback registrations intact");
    check(videos == 1 && copied == kSyntheticVideo.size(), "another handle cannot replay the recreated consumer");
    ++finished; ctx->pc = 0; runtime->requestStop();
}
void video(uint8_t *ram, R5900Context *ctx, PS2Runtime *) {
    ++videos; videoUsers.push_back(getRegU32(ctx, 6));
    const uint32_t tuple = getRegU32(ctx, 5), address = word(ram, tuple + 8), bytes = word(ram, tuple + 12);
    check(getRegU32(ctx, 4) == mpeg && word(ram, tuple) == 0 && address == source + 9 && bytes == kSyntheticVideo.size(),
          "the actual stream callback preserves handle, type, original source and byte count");
    check(copied + bytes <= kSyntheticVideo.size() * 2, "synthetic duplicate-control copy stays within its test buffer");
    std::memcpy(ram + copiedSource + copied, ram + address, bytes); copied += bytes;
    reg(*ctx, 2, 1); ctx->pc = 0;
}
void input(uint8_t *ram, R5900Context *ctx, PS2Runtime *) {
    ++inputs; inputUsers.push_back(getRegU32(ctx, 6));
    check(getRegU32(ctx, 4) == mpeg && word(ram, getRegU32(ctx, 5)) == 1 && getRegU32(ctx, 7) == 0,
          "the actual ordinary input callback preserves its SDK tuple");
    reg(*ctx, 2, 1); ctx->pc = 0;
}
void update(uint8_t *ram, R5900Context *ctx, PS2Runtime *) {
    ++updates; updateUsers.push_back(getRegU32(ctx, 6));
    check(getRegU32(ctx, 4) == mpeg && word(ram, getRegU32(ctx, 5)) == 4 && getRegU32(ctx, 7) == 0,
          "the actual ordinary output callback preserves its SDK tuple");
    reg(*ctx, 2, 1); ctx->pc = 0;
}
void otherVideo(uint8_t *, R5900Context *ctx, PS2Runtime *) {
    ++otherVideos; check(getRegU32(ctx, 4) == otherMpeg && getRegU32(ctx, 6) == 0xcc,
                        "another handle retains its registered stream userdata");
    reg(*ctx, 2, 1); ctx->pc = 0;
}
void otherInput(uint8_t *, R5900Context *ctx, PS2Runtime *) {
    ++otherInputs; check(getRegU32(ctx, 4) == otherMpeg && getRegU32(ctx, 6) == 0xcc,
                        "another handle retains its registered ordinary userdata");
    reg(*ctx, 2, 1); ctx->pc = 0;
}
}
int main() {
    try {
        packet = {0, 0, 1, 0xe0, 0, 0, 0x80, 0, 0};
        const unsigned length = 3 + static_cast<unsigned>(kSyntheticVideo.size());
        packet[4] = static_cast<uint8_t>(length >> 8); packet[5] = static_cast<uint8_t>(length);
        packet.insert(packet.end(), kSyntheticVideo.begin(), kSyntheticVideo.end());
        packet.insert(packet.end(), {0, 0, 1, 0xb9});
        for (operation = 0; operation < 3; ++operation) {
            videos = inputs = updates = finished = otherVideos = otherInputs = 0; copied = 0;
            videoUsers.clear(); inputUsers.clear(); updateUsers.clear();
            auto runtime = std::make_unique<PS2Runtime>();
            check(runtime->memory().initialize(), "real guest memory initializes for callback lifetime checks");
            auto *ram = runtime->memory().getRDRAM();
            ps2_stubs::resetMpegStubState(); ps2_stubs::notifyMpegCdStreamStart(runtime.get());
            create(ram, runtime.get(), mpeg, work); create(ram, runtime.get(), otherMpeg, otherWork);
            add(ram, runtime.get(), mpeg, 0xaa); add(ram, runtime.get(), otherMpeg, 0xcc, true);
            R5900Context ctx{}; reg(ctx, 4, mpeg);
            if (operation == 1) ps2_stubs::sceMpegReset(ram, &ctx, runtime.get());
            else {
                if (operation == 2) ps2_stubs::sceMpegDelete(ram, &ctx, runtime.get());
                create(ram, runtime.get(), mpeg, work); add(ram, runtime.get(), mpeg, 0xbb);
            }
            std::memcpy(ram + source, packet.data(), packet.size());
            runtime->registerFunction(beginPc, begin); runtime->registerFunction(demuxDonePc, demuxDone);
            runtime->registerFunction(pictureDonePc, pictureDone); runtime->registerFunction(otherDonePc, otherDone);
            runtime->registerFunction(videoPc, video); runtime->registerFunction(inputPc, input);
            runtime->registerFunction(updatePc, update); runtime->registerFunction(otherVideoPc, otherVideo);
            runtime->registerFunction(otherInputPc, otherInput);
            ctx = {}; ctx.pc = beginPc;
            runtime->eeScheduler().reset(ram, ctx); runtime->eeScheduler().run();
            check(finished == 1, "the genuine client resumes after each lifecycle operation");
        }
        std::cout << "PASS: " << checks << " actual MPEG recreation, byte-copy and callback lifetime checks\n";
#ifdef MPEG_RECREATE_OLD_CONTROL
        std::cerr << "FAIL: old control unexpectedly cleared recreated callback registrations\n"; return 1;
#else
        return 0;
#endif
    } catch (const std::exception &error) {
#ifdef MPEG_RECREATE_OLD_CONTROL
        if (checks == 9 && std::strcmp(error.what(), "Create must clear prior stream registrations before re-registering one consumer") == 0) {
            std::cout << "PASS: old control duplicates an actual accepted video payload after Create at check" << checks << '\n';
            return 0;
        }
#endif
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
