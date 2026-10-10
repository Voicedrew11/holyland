// SPDX-License-Identifier: GPL-3.0-only
// Synthetic framebuffers, real host-to-local transfer and presentation backend.
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
constexpr unsigned stride = 640;
constexpr unsigned bufferHeight = 512;
unsigned checks = 0, failures = 0;

void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition)
    {
        if (failures < 12)
            std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// Encodes every column and row independently, including bits above 255.
// The 32-line HUD marker at rows416..447 makes loss of the bottom visible.
uint32_t pattern(unsigned x, unsigned y, unsigned tag = 0)
{
    const unsigned blue = 1u | ((x >> 8u) << 1u) | ((y >> 8u) << 3u) |
                          ((y >= 416u && y < 448u) ? 0x80u : 0u) | (tag << 5u);
    return 0x80000000u | (x & 255u) | ((y & 255u) << 8u) | (blue << 16u);
}

struct Backend
{
    std::vector<uint8_t> memory = std::vector<uint8_t>(4u << 20u);
    GSCpuBackend gs;
    Backend() { gs.Initialize(memory.data(), static_cast<uint32_t>(memory.size())); }

    void upload(unsigned page, unsigned tag = 0)
    {
        std::vector<uint8_t> source(stride * bufferHeight * 4u);
        for (unsigned y = 0; y < bufferHeight; ++y)
            for (unsigned x = 0; x < stride; ++x)
            {
                const uint32_t pixel = pattern(x, y, tag);
                const size_t offset = (static_cast<size_t>(y) * stride + x) * 4u;
                for (unsigned byte = 0; byte < 4; ++byte)
                    source[offset + byte] = static_cast<uint8_t>(pixel >> (byte * 8u));
            }
        GSTransferCommand transfer{};
        transfer.direction = 0; // Host-to-local, DISPFB pages become transfer blocks.
        transfer.bitbltbuf.dbp = page * 32u;
        transfer.bitbltbuf.dbw = 10;
        transfer.bitbltbuf.dpsm = GS_PSM_CT32;
        transfer.trxreg.rrw = stride;
        transfer.trxreg.rrh = bufferHeight;
        gs.BeginTransfer(transfer);
        gs.UploadImage(source.data(), static_cast<uint32_t>(source.size()));
    }
};

uint64_t display(unsigned timingWidth, unsigned timingHeight, unsigned magh = 0, unsigned magv = 0)
{
    return (static_cast<uint64_t>(timingWidth - 1u) << 32u) |
           (static_cast<uint64_t>(timingHeight - 1u) << 44u) |
           (static_cast<uint64_t>(magh) << 23u) | (static_cast<uint64_t>(magv) << 27u);
}

uint64_t framebuffer(unsigned page)
{
    return page | (10ull << 9u) | (static_cast<uint64_t>(GS_PSM_CT32) << 15u);
}

GSPresentationRequest request(unsigned circuit, unsigned page, uint64_t timing,
                              uint64_t mode = 3, uint64_t tick = 0)
{
    GSPresentationRequest r{};
    r.pmode = circuit == 1 ? 1 : 2;
    r.smode2 = mode;
    r.vsyncTick = tick;
    if (circuit == 1) { r.dispfb1 = framebuffer(page); r.display1 = timing; }
    else { r.dispfb2 = framebuffer(page); r.display2 = timing; }
    return r;
}

uint32_t pixelAt(const PresentationFrame &frame, unsigned x, unsigned y)
{
    const size_t offset = (static_cast<size_t>(y) * stride + x) * 4u;
    if (offset + 3 >= frame.pixels.size())
        return 0;
    const auto *p = frame.pixels.data() + offset;
    return p[0] | (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
}

enum class Rows { Identity, Even, Odd, Bob };
void verify(const PresentationFrame &frame, unsigned width, unsigned height,
            Rows rows = Rows::Identity, unsigned tag = 0)
{
    expect(static_cast<bool>(frame), "presentation produces an image");
    expect(frame.width == width, "logical width matches source geometry after MAGH");
    expect(frame.height == height, "logical height includes full source after mode division and clamping");
    expect(frame.pixels.size() >= stride * height * 4u, "fixed-stride output contains all requested rows");
    for (unsigned y = 0; y < height; ++y)
    {
        unsigned sourceY = y;
        if (rows == Rows::Even || rows == Rows::Odd)
            sourceY = std::min(height - 1u, (y / 2u) * 2u + (rows == Rows::Odd ? 1u : 0u));
        else if (rows == Rows::Bob)
            sourceY = y / 2u;
        for (unsigned x = 0; x < width; ++x)
            expect(pixelAt(frame, x, y) == (pattern(x, sourceY, tag) | 0xff000000u),
                   "row/column gradient and normalized alpha survive scanout exactly");
    }
    if (rows == Rows::Identity && height == 448)
    {
        expect(pixelAt(frame, width - 1u, 416) == (pattern(width - 1u, 416, tag) | 0xff000000u),
               "HUD begins at source row416 without scaling or cropping");
        expect(pixelAt(frame, width - 1u, 447) == (pattern(width - 1u, 447, tag) | 0xff000000u),
               "last HUD row447 and rightmost column are visible");
    }
}

void crt2Game()
{
    Backend b;
    // Captured KFIV CRT2: DW2559/MAGH3, DH895/MAGV0, INT1/FFMD1, FBW10/CT32.
    for (unsigned page : {0u, 140u})
    {
        b.upload(page);
        for (uint64_t parity = 0; parity < 2; ++parity)
            verify(b.gs.Present(request(2, page, display(2560, 896, 3), 3, parity)), 640, 448);
    }
}

void crt1Game()
{
    Backend b;
    b.upload(140);
    verify(b.gs.Present(request(1, 140, display(2560, 896, 3))), 640, 448);
}

void dualCrt()
{
    Backend b;
    b.upload(0, 1); b.upload(200, 2);
    auto r = request(1, 0, display(2560, 898, 3));
    r.dispfb2 = framebuffer(200); r.display2 = display(2560, 896, 3);
    r.pmode = 3 | (1ull << 5u) | (255ull << 8u); // Both CRTs, MMOD constant alpha255.
    verify(b.gs.Present(r), 640, 449, Rows::Identity, 1);
    // Equal heights and constant alpha0 select the independently decoded CRT2.
    r.display1 = r.display2; r.pmode = 3 | (1ull << 5u);
    verify(b.gs.Present(r), 640, 448, Rows::Identity, 2);
}

void magnification()
{
    Backend b;
    b.upload(140);
    verify(b.gs.Present(request(2, 140, display(2560, 1792, 3, 1))), 640, 448);
    verify(b.gs.Present(request(1, 140, display(4096, 1792, 7, 1))), 512, 448);
}

void oddRounding()
{
    Backend b;
    b.upload(140);
    verify(b.gs.Present(request(2, 140, display(2560, 897, 3))), 640, 449);
    verify(b.gs.Present(request(1, 140, display(2560, 1794, 3, 1))), 640, 449);
}

void frameClamp()
{
    Backend b;
    b.upload(140);
    for (unsigned timingHeight : {1024u, 1026u, 2048u})
        verify(b.gs.Present(request(2, 140, display(2560, timingHeight, 3))), 640, 512);
}

void progressive()
{
    Backend b;
    b.upload(140);
    for (uint64_t mode : {0ull, 2ull}) // FFMD alone does not halve progressive images.
        verify(b.gs.Present(request(2, 140, display(2560, 448, 3), mode)), 640, 448);
    verify(b.gs.Present(request(1, 140, display(2560, 896, 3, 1), 0)), 640, 448);
    verify(b.gs.Present(request(2, 140, display(641, 600), 0)), 640, 512);
}

void fieldRows()
{
    Backend b;
    b.upload(140);
    for (uint64_t parity = 0; parity < 2; ++parity)
        verify(b.gs.Present(request(2, 140, display(2560, 448, 3), 1, parity)), 640, 448,
               parity ? Rows::Odd : Rows::Even);
}

void smallFrame()
{
    Backend b;
    b.upload(140);
    // One first presentation only: preserve existing small FRAME-field bob,
    // without making assumptions about cross-frame history ownership.
    verify(b.gs.Present(request(2, 140, display(2560, 448, 3))), 640, 448, Rows::Bob);
}
}

int main(int argc, char **argv)
{
    const std::string name = argc > 1 ? argv[1] : "";
    if (name == "crt2_game") crt2Game(); else if (name == "crt1_game") crt1Game();
    else if (name == "dual_crt") dualCrt(); else if (name == "magnification") magnification();
    else if (name == "odd_rounding") oddRounding(); else if (name == "frame_clamp") frameClamp();
    else if (name == "progressive") progressive(); else if (name == "field_rows") fieldRows();
    else if (name == "small_frame") smallFrame();
    else { std::fprintf(stderr, "unknown case\n"); return 2; }
    std::printf("%s: %u checks, %u failures\n", name.c_str(), checks, failures);
    return failures ? 1 : 0;
}
