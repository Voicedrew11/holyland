// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixtures/video_three_frames.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/MPEG.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks = 0, operation = 0, updates = 0, inputs = 0, extras = 0, finished = 0;
std::vector<unsigned> order;
constexpr uint32_t mpeg = 0x120000, work = 0x130000, source = 0x140000, image = 0x160000;
constexpr uint32_t beginPc = 0x110000, resumePc = beginPc + 4, updatePc = beginPc + 8;
constexpr uint32_t inputPc = beginPc + 12, extraPc = beginPc + 16;
void check(bool value, const char *message) { ++checks; if (!value) throw std::runtime_error(message); }
void reg(R5900Context &ctx, int index, uint32_t value) { SET_GPR_U32(&ctx, index, value); }
uint32_t word(uint8_t *ram, uint32_t address) {
    uint32_t value; std::memcpy(&value, ram + address, sizeof(value)); return value;
}
void cancel(uint8_t *ram, PS2Runtime *runtime) {
    R5900Context ctx{}; reg(ctx, 4, mpeg);
    switch (operation) {
    case 1: case 4: ps2_stubs::sceMpegReset(ram, &ctx, runtime); break;
    case 2: case 5: ps2_stubs::sceMpegDelete(ram, &ctx, runtime); break;
    case 3: case 6: ps2_stubs::notifyMpegCdStreamStart(runtime); break;
    case 7:
        reg(ctx, 5, work); reg(ctx, 6, 0x2000);
        ps2_stubs::sceMpegCreate(ram, &ctx, runtime); break;
    }
}
void picture(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    ctx->pc = resumePc; reg(*ctx, 4, mpeg); reg(*ctx, 5, image); reg(*ctx, 6, 0x460);
    ps2_stubs::sceMpegGetPicture(ram, ctx, runtime);
}
void resume(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    check(getRegU32(ctx, 2) == 0, "the actual picture client resumes with the SDK return value");
    check(std::memcmp(ram + source, kSyntheticVideo.data(), kSyntheticVideo.size()) == 0,
          "picture input service preserves the original compressed bytes");
    if (operation == 0) {
        check(inputs == updates && extras == inputs, "each genuine picture receives both input callbacks");
        check(order.size() == updates * 3 && order[order.size() - 3] == 4 &&
              order[order.size() - 2] == 1 && order.back() == 2,
              "UPDATE precedes all registered NODATA callbacks for every real picture");
        check(word(ram, mpeg + 8) == updates - 1 && word(ram, work + 4) == 3 - updates,
              "input service preserves the genuine picture index and decoded reference count");
        if (updates < 3) { picture(ram, ctx, runtime); return; }
        ps2_stubs::notifyMpegCdStreamEof(runtime);
        R5900Context ended{}; reg(ended, 4, mpeg); reg(ended, 5, image);
        ps2_stubs::sceMpegGetPicture(ram, &ended, runtime);
        check(updates == 3 && inputs == 3 && extras == 3,
              "an empty completed decoder emits neither UPDATE nor NODATA");
    } else if (operation <= 3) {
        check(updates == 1 && inputs == 0 && extras == 0 && order.size() == 1,
              "reset/delete/new generation in UPDATE cancels every queued NODATA callback");
    } else {
        check(updates == 1 && inputs == 1 && extras == 0 && order.size() == 2,
              "reset/delete/new generation/recreate in NODATA cancels its queued successor");
    }
    ++finished; ctx->pc = 0; runtime->requestStop();
}
void tuple(uint8_t *ram, R5900Context *ctx, unsigned type, uint32_t userdata) {
    const uint32_t address = getRegU32(ctx, 5);
    check(getRegU32(ctx, 4) == mpeg && word(ram, address) == type,
          "ordinary callbacks receive the actual MPEG handle and type tuple");
    check(getRegU32(ctx, 6) == userdata && getRegU32(ctx, 7) == 0,
          "ordinary callbacks preserve registered userdata and the SDK fourth argument");
    check(word(ram, address + 8) == 0 && word(ram, address + 12) == 0,
          "picture input service does not fabricate a compressed payload");
}
void update(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    tuple(ram, ctx, 4, 0x11223344); ++updates; order.push_back(4);
    check(word(ram, mpeg + 8) == updates - 1, "UPDATE follows a genuine FFmpeg picture handoff");
    if (operation >= 1 && operation <= 3) cancel(ram, runtime);
    reg(*ctx, 2, 1); ctx->pc = 0;
}
void input(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    tuple(ram, ctx, 1, 0x55667788); ++inputs; order.push_back(1);
    check(inputs == updates && extras + 1 == inputs, "NODATA runs after UPDATE and before its successor");
    check(word(ram, mpeg + 8) == updates - 1, "NODATA preserves the actual decoded picture index");
    if (operation >= 4) cancel(ram, runtime);
    reg(*ctx, 2, 1); ctx->pc = 0;
}
void extra(uint8_t *ram, R5900Context *ctx, PS2Runtime *) {
    tuple(ram, ctx, 1, 0x99aabbcc); ++extras; order.push_back(2);
    check(extras == inputs, "multiple registered NODATA callbacks retain FIFO order");
    reg(*ctx, 2, 1); ctx->pc = 0;
}
}
int main() {
    try {
        for (operation = 0; operation < 8; ++operation) {
            updates = inputs = extras = finished = 0; order.clear();
            auto runtime = std::make_unique<PS2Runtime>();
            check(runtime->memory().initialize(), "real guest memory initializes");
            auto *ram = runtime->memory().getRDRAM();
            ps2_stubs::resetMpegStubState(); ps2_stubs::notifyMpegCdStreamStart(runtime.get());
            R5900Context ctx{}; reg(ctx, 4, mpeg); reg(ctx, 5, work); reg(ctx, 6, 0x2000);
            ps2_stubs::sceMpegCreate(ram, &ctx, runtime.get());
            std::memcpy(ram + source, kSyntheticVideo.data(), kSyntheticVideo.size());
            reg(ctx, 4, mpeg); reg(ctx, 5, source); reg(ctx, 6, static_cast<uint32_t>(kSyntheticVideo.size()));
            ps2_stubs::sceMpegAddBs(ram, &ctx, runtime.get());
            reg(ctx, 4, mpeg); ps2_stubs::sceMpegFlush(ram, &ctx, runtime.get());
            check(word(ram, work + 4) == 3, "actual FFmpeg decodes three synthetic pictures");
            runtime->registerFunction(beginPc, picture); runtime->registerFunction(resumePc, resume);
            runtime->registerFunction(updatePc, update); runtime->registerFunction(inputPc, input);
            runtime->registerFunction(extraPc, extra);
            for (const auto callback : {std::pair{4u, updatePc}, std::pair{1u, inputPc}, std::pair{1u, extraPc}}) {
                reg(ctx, 4, mpeg); reg(ctx, 5, callback.first); reg(ctx, 6, callback.second);
                reg(ctx, 7, callback.second == updatePc ? 0x11223344 : callback.second == inputPc ? 0x55667788 : 0x99aabbcc);
                ps2_stubs::sceMpegAddCallback(ram, &ctx, runtime.get());
            }
            ctx = {}; ctx.pc = beginPc;
            runtime->eeScheduler().reset(ram, ctx); runtime->eeScheduler().run();
            check(finished == 1, "actual callback service returns to the genuine picture client");
        }
        std::cout << "PASS: " << checks << " real picture input-service, ABI, order and cancellation checks\n";
#ifdef MPEG_PICTURE_INPUT_OLD_CONTROL
        std::cerr << "FAIL: old control unexpectedly serviced the picture input consumer\n";
        return 1;
#else
        return 0;
#endif
    } catch (const std::exception &error) {
#ifdef MPEG_PICTURE_INPUT_OLD_CONTROL
        if (checks == 9 && std::strcmp(error.what(), "each genuine picture receives both input callbacks") == 0) {
            std::cout << "PASS: old control omits picture input service at check9 after the genuine UPDATE callback\n";
            return 0;
        }
#endif
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
