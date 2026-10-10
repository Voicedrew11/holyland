// SPDX-License-Identifier: GPL-3.0-only
// Source-only synthetic patterns; no retail assets, recordings, or workspace paths.
// Synthetic logical viewport through ParallelGS GPU sampling/merge/readback.
#include "gs_vulkan_scanout.h"
#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "gs_util.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ParallelGS;
using namespace Vulkan;
namespace
{
constexpr unsigned stride = 640;
constexpr size_t vramBytes = 4u << 20u;
unsigned checks = 0, failures = 0;
void expect(bool condition, const char *message)
{
    ++checks;
    if (!condition) { if (failures < 10) std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
uint32_t pattern(unsigned x, unsigned y, unsigned tag = 0)
{
    unsigned blue = 1u | ((x >> 8u) << 1u) | ((y >> 8u) << 3u) | (tag << 5u);
    if (y >= 416 && y < 448) blue |= 0x80u;
    return 0x80000000u | (x & 255u) | ((y & 255u) << 8u) | (blue << 16u);
}
void upload(GSInterface &gs, unsigned page, unsigned height = 448, unsigned tag = 0)
{
    std::vector<uint32_t> input(stride * height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < stride; ++x) input[y * stride + x] = pattern(x, y, tag);
    auto *mapped = gs.map_vram_write(0, vramBytes);
    if (!mapped) throw std::runtime_error("VRAM mapping unavailable");
    vram_upload<PSMCT32>(mapped, input.data(), page * 32u, 10, 0, 0, stride, height, vramBytes - 1u);
    gs.end_vram_write(page * 8192u, stride * height * 4u);
}
GSPresentationRequest request(unsigned circuit, unsigned page, unsigned timingHeight = 896,
                              unsigned width = 640, unsigned magh = 3, unsigned magv = 0)
{
    GSPresentationRequest r{};
    r.pmode = circuit == 1 ? (1ull | (1ull << 5u) | (255ull << 8u)) : 2ull;
    r.smode2 = 3;
    const uint64_t fb = page | (10ull << 9u);
    const uint64_t display = (uint64_t(width * (magh + 1u) - 1u) << 32u) |
                             (uint64_t(timingHeight - 1u) << 44u) |
                             (uint64_t(magh) << 23u) | (uint64_t(magv) << 27u);
    if (circuit == 1) { r.dispfb1 = fb; r.display1 = display; }
    else { r.dispfb2 = fb; r.display2 = display; }
    return r;
}
uint32_t pixel(const PresentationFrame &frame, unsigned x, unsigned y)
{
    const size_t offset = (static_cast<size_t>(y) * stride + x) * 4u;
    if (offset + 3u >= frame.pixels.size()) return 0;
    const auto *p = frame.pixels.data() + offset;
    return p[0] | (uint32_t(p[1]) << 8u) | (uint32_t(p[2]) << 16u) | (uint32_t(p[3]) << 24u);
}
void verify(const PresentationFrame &frame, unsigned width = 640, unsigned height = 448,
            bool bob = false, unsigned tag = 0)
{
    expect(bool(frame), "GPU scanout returns image bytes");
    expect(frame.width == width && frame.height == height, "GPU logical output dimensions are uncropped");
    expect(frame.pixels.size() == stride * 512u * 4u, "fixed640 row-stride host buffer retained");
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            expect(pixel(frame, x, y) == (pattern(x, bob ? y / 2u : y, tag) | 0xff000000u),
                   "GPU-produced gradient includes every row/column and normalized alpha");
    if (!bob && height == 448)
        expect(pixel(frame, width - 1u, 447) == (pattern(width - 1u, 447, tag) | 0xff000000u),
               "last HUD row447 is visible at its original scale");
}

void fieldHistory(GSInterface &gs, Device &device, bool raw)
{
    struct Step { unsigned page, tag; uint64_t tick; };
    // Tags differ in blue, so an adaptive filter can regard fields as static
    // while still reusing visibly different previous-field pixels. Its edge
    // rule always reuses the previous field on an unmatched first/last row.
    // Include same-page uploads, changed pages, repeated parity, skipped
    // ticks, and a repeated tick: no history/parity assumption may blend text.
    constexpr Step steps[] = {
        {0, 0, 0}, {0, 1, 1}, {70, 2, 1}, {70, 3, 3}, {140, 0, 4},
        {0, 1, 8}, {140, 2, 9}, {140, 3, 9}, {70, 1, 10}, {0, 2, 13}
    };
    for (const auto &step : steps)
    {
        upload(gs, step.page, 224, step.tag);
        auto r = request(2, step.page, 448);
        r.vsyncTick = step.tick;
        const auto frame = presentVulkanGs(gs, device, r);
        const unsigned outputHeight = raw ? 224u : 448u;
        verify(frame, 640, outputHeight, !raw, step.tag);
        expect(pixel(frame, 639, outputHeight - 1u) == (pattern(639, 223, step.tag) | 0xff000000u),
               "bottom row always comes from the current field after page/timing changes");
        device.next_frame_context();
    }
}
}
int main(int argc, char **argv)
{
    try
    {
        const std::string name = argc > 1 ? argv[1] : "";
        if (!Context::init_loader(nullptr)) throw std::runtime_error("Vulkan loader init failed");
        Context context; context.set_num_thread_indices(1);
        if (!context.init_instance_and_device(nullptr, 0, nullptr, 0,
            CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT | CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
            CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT)) throw std::runtime_error("Vulkan context init failed");
        Device device; device.set_context(context); device.init_frame_contexts(4);
        const auto &props = device.get_gpu_properties();
        std::printf("GPU scanout adapter: %s type=%u\n", props.deviceName, props.deviceType);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            throw std::runtime_error("A hardware Vulkan device is required; software CPU devices are rejected");
        GSInterface gs;
        if (!gs.init(&device, GSOptions{})) throw std::runtime_error("GS init failed");
        if (name == "crt1" || name == "crt2")
        {
            for (unsigned page : {0u, 140u})
            {
                upload(gs, page);
                auto r = request(name == "crt1" ? 1 : 2, page);
                for (unsigned parity = 0; parity < 2; ++parity)
                { r.vsyncTick = parity; verify(presentVulkanGs(gs, device, r)); device.next_frame_context(); }
            }
        }
        else if (name == "dual")
        {
            upload(gs, 0, 448, 1); upload(gs, 200, 448, 2);
            auto r = request(1, 0); auto second = request(2, 200);
            r.dispfb2 = second.dispfb2; r.display2 = second.display2;
            r.pmode = 3 | (1ull << 5u) | (255ull << 8u);
            verify(presentVulkanGs(gs, device, r), 640, 448, false, 1); device.next_frame_context();
            r.pmode = 3 | (1ull << 5u);
            verify(presentVulkanGs(gs, device, r), 640, 448, false, 2); device.next_frame_context();
        }
        else if (name == "movie")
        {
            upload(gs, 140, 224);
            verify(presentVulkanGs(gs, device, request(2, 140, 448)), 640, 448, true);
            device.next_frame_context();
        }
        else if (name == "field_history" || name == "field_history_prior" || name == "raw_field")
        {
            fieldHistory(gs, device, name == "raw_field");
        }
        else if (name == "viewport")
        {
            upload(gs, 140);
            verify(presentVulkanGs(gs, device, request(2, 140, 1792, 640, 3, 1)));
            device.next_frame_context();
            verify(presentVulkanGs(gs, device, request(2, 140, 896, 320, 7)), 320, 448);
            device.next_frame_context();
        }
        else if (name == "unsupported")
        {
            bool rejected = false;
            try { (void)presentVulkanGs(gs, device, request(2, 140, 897)); }
            catch (const VulkanGsScanoutUnsupported &) { rejected = true; }
            expect(rejected, "449-row viewport is rejected before GPU work rather than cropped");
        }
        else throw std::runtime_error("Unknown test case");
        gs.flush(); device.wait_idle();
        std::printf("scanout %s: %u checks, %u failures\n", name.c_str(), checks, failures);
        // This mode is used only by the optional prior-source target. A
        // Vulkan initialization error still takes the exception path and
        // fails; only completed pixel comparisons can satisfy the control.
        if (name == "field_history_prior")
            return failures != 0u ? 0 : 1;
        return failures ? 1 : 0;
    }
    catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 2; }
}
