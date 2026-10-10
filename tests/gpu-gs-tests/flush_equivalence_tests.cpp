// SPDX-License-Identifier: GPL-3.0-only
// Actual current and scratch prior-flush Vulkan backends, synthetic data only.
#include "runtime/gs/gs_vulkan_backend.h"
#include "gs_vulkan_backend_ref.h"
#include "gs_trace.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace GSTrace {
bool enabled() { return false; }
void imageTransfer(const GSBitBltBuf &, const GSTrxPos &, const GSTrxReg &, uint32_t) {}
}

namespace {
constexpr uint32_t vramBytes = 4u << 20u;
constexpr uint32_t sourceColor = 0x80804020u;
constexpr uint32_t secondColor = 0x80204060u;
constexpr uint32_t textureBase = 8192u;

GSPrimitiveBatch sprite(uint32_t rgba = sourceColor)
{
    GSPrimitiveBatch b{};
    b.vertexCount = 2u; b.state.prim.type = GS_PRIM_SPRITE;
    b.state.context.frame = {0u, 10u, GS_PSM_CT32, 0u};
    b.state.context.scissor = {0u, 639u, 0u, 447u};
    b.state.context.zbuf.zmask = true; b.state.colclamp = 1u;
    b.vertices[0].x = 2.0f; b.vertices[0].y = 2.0f;
    b.vertices[1].x = 18.0f; b.vertices[1].y = 18.0f;
    for (auto &v : b.vertices) {
        v.r = uint8_t(rgba); v.g = uint8_t(rgba >> 8u);
        v.b = uint8_t(rgba >> 16u); v.a = uint8_t(rgba >> 24u);
        v.q = 1.0f; v.fog = 255u; v.z = 20u;
    }
    return b;
}

GSPresentationRequest presentation(uint64_t tick = 2u)
{
    GSPresentationRequest r{};
    r.pmode = 2u; r.smode2 = 3u; r.dispfb2 = 10ull << 9u;
    r.display2 = 636ull | (50ull << 12u) | (3ull << 23u) |
                 (2559ull << 32u) | (895ull << 44u);
    r.vsyncTick = tick;
    return r;
}

void upload(GSRasterBackend &gs, uint32_t base, const void *data, uint32_t bytes,
            uint32_t width = 8u, uint32_t height = 8u, uint8_t psm = GS_PSM_CT32)
{
    GSTransferCommand t{};
    t.bitbltbuf.dbp = base; t.bitbltbuf.dbw = 1u; t.bitbltbuf.dpsm = psm;
    t.trxreg = {uint16_t(width), uint16_t(height)}; t.direction = 0u;
    gs.BeginTransfer(t); gs.UploadImage(static_cast<const uint8_t *>(data), bytes);
}

bool sameFrame(const PresentationFrame &a, const PresentationFrame &b)
{
    return a.width == b.width && a.height == b.height && a.displayFbp == b.displayFbp &&
           a.sourceFbp == b.sourceFbp && a.usedPreferred == b.usedPreferred && a.pixels == b.pixels;
}

bool sameTransfer(const GSTransferSnapshot &a, const GSTransferSnapshot &b)
{
    return a.x == b.x && a.y == b.y && a.totalPixels == b.totalPixels &&
           a.copiedPixels == b.copiedPixels && a.direction == b.direction &&
           a.localToHostPendingBytes == b.localToHostPendingBytes;
}

struct Fixture {
    std::array<std::unique_ptr<GSRasterBackend>, 2> gs{
        std::make_unique<GSVulkanBackend>(), std::make_unique<GSVulkanBackendRef>()};
    std::vector<uint8_t> initial = std::vector<uint8_t>(vramBytes, 0u);
    uint64_t cases = 0u, checks = 0u, failed = 0u, comparedVramBytes = 0u, frames = 0u;

    void check(bool condition, const char *caseName, const char *label)
    {
        ++checks;
        if (!condition) {
            ++failed;
            std::fprintf(stderr, "FAIL %s: %s\n", caseName, label);
        }
    }

    void run(const char *name, const std::function<void(GSRasterBackend &)> &commands)
    {
        runCaptured(name, [&](auto &backend, auto &) { commands(backend); });
    }

    void runCaptured(const char *name,
                     const std::function<void(GSRasterBackend &, std::vector<PresentationFrame> &)> &commands)
    {
        ++cases;
        std::array<std::vector<PresentationFrame>, 2> frame;
        std::array<std::vector<uint8_t>, 2> vram;
        std::array<GSTransferSnapshot, 2> transfer;
        for (size_t i = 0; i < gs.size(); ++i) {
            gs[i]->Initialize(initial.data(), uint32_t(initial.size()));
            gs[i]->Reset(); commands(*gs[i], frame[i]);
            transfer[i] = gs[i]->GetTransferSnapshot();
            frame[i].push_back(gs[i]->Present(presentation()));
            gs[i]->Sync(GSSyncReason::DebugReadback); gs[i]->SnapshotVram(vram[i]);
        }
        check(bool(frame[0].back()) && frame[0].back().width == 640u && frame[0].back().height == 448u,
              name, "actual GPU presentation is valid");
        check(frame[0].size() == frame[1].size(), name, "presentation count matches");
        for (size_t i = 0; i < std::min(frame[0].size(), frame[1].size()); ++i) {
            ++frames;
            check(sameFrame(frame[0][i], frame[1][i]), name, "all RGBA bytes and presentation metadata match");
        }
        check(sameTransfer(transfer[0], transfer[1]), name, "transfer state matches");
        check(vram[0].size() == vramBytes && vram[1].size() == vramBytes,
              name, "both snapshots contain exactly 4 MiB");
        check(vram[0] == vram[1], name, "all 4 MiB VRAM bytes match");
        comparedVramBytes += vram[0].size();
    }
};
}

int main() try
{
    Fixture f;
    f.run("standalone Sync and Present", [](auto &gs) {
        gs.Submit(sprite()); gs.Sync(GSSyncReason::Finish);
        if (gs.ReadVram(GS_PSM_CT32, 0u, 10u, 4u, 4u) != sourceColor)
            throw std::runtime_error("Standalone Sync lost its pending draw");
    });
    f.run("adjacent Flush Sync Present", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush(); gs.Sync(GSSyncReason::Presentation);
    });
    f.run("repeated unchanged Flush and Sync", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush(); gs.Flush();
        gs.Sync(GSSyncReason::Finish); gs.Sync(GSSyncReason::Presentation);
    });
    f.run("intervening draw invalidates flush", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush(); gs.Submit(sprite(secondColor));
        gs.Sync(GSSyncReason::Presentation);
        if (gs.ReadVram(GS_PSM_CT32, 0u, 10u, 4u, 4u) != secondColor)
            throw std::runtime_error("Intervening draw was lost");
    });
    f.run("intervening texture upload and TEXFLUSH", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush();
        const std::array<uint32_t, 64> data = [] { std::array<uint32_t, 64> a{}; a.fill(secondColor); return a; }();
        upload(gs, textureBase, data.data(), uint32_t(sizeof(data))); gs.TextureFlush();
        auto b = sprite(); b.state.prim.tme = true; b.state.prim.fst = true;
        b.state.context.tex0 = {textureBase, 1u, GS_PSM_CT32, 3u, 3u, true, 1u};
        b.vertices[1].u = 8u * 16u; b.vertices[1].v = 8u * 16u;
        gs.Submit(b); gs.Sync(GSSyncReason::Finish);
    });
    f.run("partial upload after explicit flush", [](auto &gs) {
        const std::array<uint32_t, 16> data = [] { std::array<uint32_t, 16> a{}; a.fill(secondColor); return a; }();
        upload(gs, textureBase, data.data(), 8u, 4u, 4u);
        gs.Flush(); gs.UploadImage(reinterpret_cast<const uint8_t *>(data.data()) + 8u, sizeof(data) - 8u);
        gs.Sync(GSSyncReason::LocalToHost);
    });
    f.run("intervening indexed upload and CLUT", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush();
        std::array<uint32_t, 16> colors{}; colors.fill(secondColor);
        std::array<uint8_t, 32> texels{}; texels.fill(0x11u);
        constexpr uint32_t paletteBase = 12288u;
        upload(gs, paletteBase, colors.data(), sizeof(colors), 16u, 1u);
        upload(gs, textureBase, texels.data(), sizeof(texels), 8u, 8u, GS_PSM_T4);
        GSTex0Reg tex{}; tex.tbp0 = textureBase; tex.tbw = 1u; tex.psm = GS_PSM_T4;
        tex.tw = tex.th = 3u; tex.tcc = true; tex.tfx = 1u;
        tex.cbp = paletteBase; tex.cpsm = GS_PSM_CT32; tex.cld = 1u;
        gs.LoadClut(tex, {}); gs.TextureFlush();
        auto b = sprite(); b.state.prim.tme = true; b.state.prim.fst = true;
        b.state.context.tex0 = tex; b.vertices[1].u = b.vertices[1].v = 128u;
        gs.Submit(b); gs.Sync(GSSyncReason::Presentation);
    });
    f.run("host mapping invalidates flush", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush(); gs.WriteVram(GS_PSM_CT32, 0u, 10u, 4u, 4u, secondColor);
        gs.Sync(GSSyncReason::Finish);
        if (gs.ReadVram(GS_PSM_CT32, 0u, 10u, 4u, 4u) != secondColor)
            throw std::runtime_error("Host write was lost");
    });
    f.run("local transfer after flush", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush();
        GSTransferCommand t{}; t.bitbltbuf = {0u, 10u, GS_PSM_CT32, textureBase, 1u, GS_PSM_CT32};
        t.trxpos = {2u, 2u, 0u, 0u, 0u}; t.trxreg = {8u, 8u}; t.direction = 2u;
        gs.BeginTransfer(t); gs.Sync(GSSyncReason::Finish);
    });
    f.run("local-to-host FIFO after flush", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush();
        GSTransferCommand t{}; t.bitbltbuf = {0u, 10u, GS_PSM_CT32, 0u, 10u, GS_PSM_CT32};
        t.trxpos = {4u, 4u, 0u, 0u, 0u}; t.trxreg = {2u, 2u}; t.direction = 1u;
        gs.BeginTransfer(t); std::array<uint32_t, 4> pixels{};
        if (gs.ConsumeLocalToHostBytes(reinterpret_cast<uint8_t *>(pixels.data()), sizeof(pixels)) != sizeof(pixels))
            throw std::runtime_error("FIFO size was lost");
        for (auto pixel : pixels) if (pixel != sourceColor) throw std::runtime_error("FIFO color differs");
    });
    f.run("Reset still waits and accepts new draws", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush(); gs.Sync(GSSyncReason::Reset); gs.Reset();
        gs.Submit(sprite(secondColor)); gs.Sync(GSSyncReason::Finish);
    });
    f.run("clear framebuffer after flush", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush();
        if (!gs.ClearFramebuffer(sprite().state.context, secondColor))
            throw std::runtime_error("Supported framebuffer clear was rejected");
        gs.Sync(GSSyncReason::Presentation);
    });
    f.runCaptured("repeated and skipped presentation ticks", [](auto &gs, auto &captures) {
        gs.Submit(sprite()); gs.Flush(); gs.Sync(GSSyncReason::Presentation);
        for (uint64_t tick : {2u, 2u, 9u, 10u}) {
            const auto frame = gs.Present(presentation(tick));
            if (!frame) throw std::runtime_error("Repeated presentation became invalid");
            captures.push_back(frame);
        }
        gs.Submit(sprite(secondColor));
    });
    f.run("worker mutations between main flush and Sync", [](auto &gs) {
        gs.Submit(sprite()); gs.Flush(); std::exception_ptr error;
        std::jthread worker([&] { try { gs.Submit(sprite(secondColor)); gs.Flush(); }
                                  catch (...) { error = std::current_exception(); } });
        worker.join(); if (error) std::rethrow_exception(error);
        gs.Sync(GSSyncReason::Presentation);
    });
    std::printf("flush_equivalence cases=%llu checks=%llu full_rgba_frames=%llu compared_vram_bytes=%llu failed=%llu\n",
                (unsigned long long)f.cases, (unsigned long long)f.checks,
                (unsigned long long)f.frames, (unsigned long long)f.comparedVramBytes,
                (unsigned long long)f.failed);
    return f.failed ? 1 : 0;
}
catch (const std::exception &error) {
    std::fprintf(stderr, "flush_equivalence runtime error: %s\n", error.what());
    return 2;
}
