// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_interpolation.h"
#include "test_environment.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <thread>
#include <chrono>

static unsigned failures = 0;
static void check(bool value, const char *message)
{
    if (!value)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
int main()
{
    setTestEnvironment("PS2X_GS_BACKEND", "cpu");
    setTestEnvironment("PS2X_FRAME_INTERPOLATION", "0");
    GS owner;
    setTestEnvironment("PS2X_FRAME_INTERPOLATION", "1");
    auto backend = ps2_gs_interpolation::wrap(std::make_unique<GSCpuBackend>(), &owner);
    std::vector<uint8_t> vram(4u << 20);
    backend->Initialize(vram.data(), uint32_t(vram.size()));
    GSPrimitiveBatch triangle{};
    triangle.vertexCount = 3;
    triangle.state.prim.type = GS_PRIM_TRIANGLE;
    triangle.state.prim.tme = true;
    triangle.state.prim.fst = false;
    triangle.state.prim.iip = true;
    auto &context = triangle.state.context;
    context.frame = {0, 10, GS_PSM_CT32, 0};
    context.scissor = {0, 639, 0, 447};
    context.tex0 = {0x3000, 1, GS_PSM_CT32, 3, 3, 0, 1};
    triangle.state.textureWidth = triangle.state.textureHeight = 8;
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x)
            backend->WriteVram(GS_PSM_CT32, 0x3000, 1, x, y, 0x80808080);
    triangle.vertices[0].x = 10;
    triangle.vertices[0].y = 10;
    triangle.vertices[1].x = 40;
    triangle.vertices[1].y = 10;
    triangle.vertices[1].s = 1;
    triangle.vertices[2].x = 10;
    triangle.vertices[2].y = 40;
    triangle.vertices[2].t = 1;
    for (auto &vertex : triangle.vertices)
    {
        vertex.q = 1;
        vertex.r = vertex.g = vertex.b = vertex.a = 128;
    }
    GSPresentationRequest request{};
    request.pmode = 2;
    request.dispfb2 = 10u << 9;
    request.display2 = (639ull << 32) | (447ull << 44);
    ps2_gs_interpolation::sealFrame(owner, request); // Capture starts at a game boundary.
    backend->ClearFramebuffer(context, 0);
    backend->Submit(triangle);
    ps2_gs_interpolation::sealFrame(owner, request);
    const auto first = backend->Present(request);
    for (auto &vertex : triangle.vertices)
        vertex.x += 30;
    backend->ClearFramebuffer(context, 0);
    backend->Submit(triangle);
    ps2_gs_interpolation::sealFrame(owner, request);
    const auto second = backend->Present(request);
    std::vector<uint8_t> before, after;
    backend->SnapshotVram(before);
    const auto intermediate = ps2_gs_interpolation::present(owner, 0.5f);
    backend->SnapshotVram(after);
    check(bool(first) && bool(second) && bool(intermediate), "original and intermediate frames exist");
    check(first.pixels != second.pixels, "authored scene actually moves");
    check(intermediate.pixels != second.pixels, "presentation renders geometry between updates");
    check(intermediate.pixels != first.pixels, "extra presentation is not a repeated older frame");
    check(ps2_gs_interpolation::present(owner, 1).pixels == second.pixels, "phase one preserves the original endpoint");
    check(ps2_gs_interpolation::present(owner, 0.5f).pixels == intermediate.pixels,
          "repeated replays do not mutate the captured starting memory");
    check(before == after, "extra rendering never changes guest-visible VRAM");
    check(backend->Present(request).pixels == second.pixels, "game readback and scanout stay at original endpoint");
    ps2_gs_interpolation::suspend(owner);
    check(!ps2_gs_interpolation::present(owner), "non-game loop immediately returns to live presentation");
    backend->SnapshotVram(after);
    check(before == after, "suspension does not change guest video memory");
    ps2_gs_interpolation::sealFrame(owner, request);
    backend->ClearFramebuffer(context, 0);
    backend->Submit(triangle);
    ps2_gs_interpolation::sealFrame(owner, request);
    check(ps2_gs_interpolation::present(owner, 0.5f).pixels == second.pixels,
          "resumed capture checkpoints current memory and never matches pre-menu geometry");
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    check(!ps2_gs_interpolation::present(owner), "missing game boundaries fall back to live presentation");
    ps2_gs_interpolation::sealFrame(owner, request);
    backend->ClearFramebuffer(context, 0);
    backend->Submit(triangle);
    ps2_gs_interpolation::sealFrame(owner, request);
    check(bool(ps2_gs_interpolation::present(owner, 1)), "capture resumes after an unannounced scene transition");
    backend->Reset();
    check(!ps2_gs_interpolation::present(owner), "reset discards old presentation frames");
    std::printf("independent interpolation: %u failures\n", failures);
    return failures ? 1 : 0;
}
