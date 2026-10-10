// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
unsigned checks = 0u, failures = 0u;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition)
    {
        if (failures < 20u)
            std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

GSPrimitiveBatch point(uint64_t test, uint32_t z, bool maskDepth = false)
{
    GSPrimitiveBatch batch{};
    batch.vertexCount = 1u;
    batch.state.prim.type = GS_PRIM_POINT;
    batch.state.context.frame = {0u, 1u, GS_PSM_CT32, 0u};
    batch.state.context.zbuf = {32u, GS_PSM_Z32, maskDepth};
    batch.state.context.scissor = {0u, 15u, 0u, 15u};
    batch.state.context.test = test;
    batch.vertices[0].x = 3.0f;
    batch.vertices[0].y = 4.0f;
    batch.vertices[0].z = z;
    batch.vertices[0].r = 0x11u;
    batch.vertices[0].g = 0x22u;
    batch.vertices[0].b = 0x33u;
    batch.vertices[0].a = 0x80u;
    return batch;
}

void depthCase(GSCpuBackend &gs, bool enabled, uint32_t method, uint32_t incomingZ,
               bool shouldDraw, bool maskDepth = false)
{
    constexpr uint32_t initialColor = 0x80554433u, initialZ = 10u;
    constexpr uint32_t sourceColor = 0x80332211u;
    gs.WriteVram(GS_PSM_CT32, 0u, 1u, 3u, 4u, initialColor);
    gs.WriteVram(GS_PSM_Z32, 32u * 32u, 1u, 3u, 4u, initialZ);
    const uint64_t test = (static_cast<uint64_t>(enabled) << 16u) |
                          (static_cast<uint64_t>(method) << 17u);
    gs.Submit(point(test, incomingZ, maskDepth));
    gs.Sync(GSSyncReason::Presentation);
    expect(gs.ReadVram(GS_PSM_CT32, 0u, 1u, 3u, 4u) == (shouldDraw ? sourceColor : initialColor),
           enabled ? "enabled ZTST chooses the correct color result" : "disabled ZTE bypasses every ZTST value");
    const bool writesDepth = enabled && shouldDraw && !maskDepth;
    expect(gs.ReadVram(GS_PSM_Z32, 32u * 32u, 1u, 3u, 4u) == (writesDepth ? incomingZ : initialZ),
           enabled ? "enabled depth writes follow test result and ZMSK" : "disabled ZTE preserves the depth buffer");
}

void movieSpriteCase(GSCpuBackend &gs)
{
    // Synthetic content using the original movie's captured GS register values.
    // No game assets or private generated code are needed for the regression.
    constexpr uint32_t textureBlock = 4480u, textureColor = 0x80604020u;
    constexpr uint32_t zBlock = 140u * 32u, initialDepth = 0x01234567u;
    for (uint32_t y = 0u; y < 224u; ++y)
        for (uint32_t x = 0u; x < 640u; ++x)
        {
            gs.WriteVram(GS_PSM_CT32, 0u, 10u, x, y, 0x80000000u);
            // The captured movie texture and inherited depth base alias. The
            // upload below replaces these bytes with the synthetic movie.
            gs.WriteVram(GS_PSM_Z32, zBlock, 10u, x, y, initialDepth);
        }

    GSTransferCommand transfer{};
    transfer.bitbltbuf.dbp = textureBlock;
    transfer.bitbltbuf.dbw = 10u;
    transfer.bitbltbuf.dpsm = GS_PSM_CT32;
    transfer.trxreg = {640u, 448u};
    transfer.direction = 0u;
    std::vector<uint32_t> pixels(640u * 448u, textureColor);
    gs.BeginTransfer(transfer);
    gs.UploadImage(reinterpret_cast<const uint8_t *>(pixels.data()), static_cast<uint32_t>(pixels.size() * 4u));
    gs.TextureFlush();

    std::vector<uint8_t> before;
    gs.SnapshotVram(before);
    GSPrimitiveBatch sprite{};
    sprite.vertexCount = 2u;
    sprite.state.prim.type = GS_PRIM_SPRITE;
    sprite.state.prim.tme = true;
    sprite.state.prim.fst = true;
    sprite.state.context.frame = {0u, 10u, GS_PSM_CT32, 0u};
    sprite.state.context.zbuf = {140u, GS_PSM_Z32, false};
    sprite.state.context.scissor = {0u, 639u, 0u, 447u};
    sprite.state.context.test = 0u;
    sprite.state.context.tex0 = {textureBlock, 10u, GS_PSM_CT32, 10u, 10u, 0u, 1u};
    sprite.state.textureWidth = 1024u;
    sprite.state.textureHeight = 1024u;
    sprite.state.linearFilter = true;
    sprite.vertices[0].u = 8u;
    sprite.vertices[0].v = 24u;
    sprite.vertices[1].x = 640.0f;
    sprite.vertices[1].y = 224.0f;
    sprite.vertices[1].u = 10248u;
    sprite.vertices[1].v = 7192u;
    // DECAL must use texture RGB even though the original vertex RGBA is zero.
    gs.Submit(sprite);
    gs.Sync(GSSyncReason::Presentation);
    for (uint32_t y = 0u; y < 224u; ++y)
        for (uint32_t x = 0u; x < 640u; ++x)
            expect((gs.ReadVram(GS_PSM_CT32, 0u, 10u, x, y) & 0xffffffu) != 0u,
                   "movie TEST=0 textured DECAL sprite writes visible RGB");

    std::vector<uint8_t> after;
    gs.SnapshotVram(after);
    bool textureUnchanged = true;
    // TEST.ZTE=0 must not corrupt the aliased movie texture with depth writes.
    for (size_t offset = textureBlock * 256u; offset < before.size(); ++offset)
        textureUnchanged = textureUnchanged && before[offset] == after[offset];
    expect(textureUnchanged, "disabled movie depth writes preserve the movie texture");

    GSPresentationRequest request{};
    request.pmode = 0x66u; // Only CRT2, ALP=0: the movie's relevant output path.
    request.smode2 = 0u;
    request.dispfb2 = 10u << 9u;
    request.display2 = (639ull << 32u) | (223ull << 44u);
    const PresentationFrame frame = gs.Present(request);
    expect(frame.width == 640u && frame.height == 224u, "CRT2 movie scanout has the requested dimensions");
    expect(frame.pixels.size() >= 4u && frame.pixels[0] == 0x20u && frame.pixels[1] == 0x40u && frame.pixels[2] == 0x60u,
           "CRT2-only presentation keeps movie color with PMODE.ALP=0");
}
}

int main()
{
    std::vector<uint8_t> vram(4u * 1024u * 1024u);
    GSCpuBackend gs;
    gs.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
    for (uint32_t method = 0u; method < 4u; ++method)
    {
        depthCase(gs, false, method, 0u, true);
        depthCase(gs, false, method, 20u, true);
    }
    depthCase(gs, true, 0u, 20u, false);
    depthCase(gs, true, 1u, 0u, true);
    depthCase(gs, true, 2u, 9u, false);
    depthCase(gs, true, 2u, 10u, true);
    depthCase(gs, true, 2u, 11u, true);
    depthCase(gs, true, 3u, 9u, false);
    depthCase(gs, true, 3u, 10u, false);
    depthCase(gs, true, 3u, 11u, true);
    depthCase(gs, true, 1u, 20u, true, true);
    movieSpriteCase(gs);
    std::printf("GS depth regression: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
