// SPDX-License-Identifier: GPL-3.0-only
#include "gs_field_reconstruction.h"
#include <cstdio>
#include <stdexcept>

namespace {
constexpr unsigned width = 8, height = 8, stride = 10;
std::vector<uint8_t> field(unsigned phase)
{
    std::vector<uint8_t> pixels(stride * height * 4, 0);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const auto i = (y * stride + x) * 4;
            pixels[i] = uint8_t((y / 2 * 2 + phase) * 20);
            pixels[i + 1] = uint8_t(x * 20);
            pixels[i + 2] = 90; pixels[i + 3] = 255;
        }
    return pixels;
}
void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void full(const std::vector<uint8_t> &p)
{
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            expect(p[(y * stride + x) * 4] == y * 20, "stationary complementary rows retain full detail");
}
}
int main()
{
    try {
        GSFieldReconstruction reconstruction;
        auto apply = [&](unsigned phase, uint64_t generation, unsigned base = 0) {
            auto p = field(phase);
            reconstruction.reconstruct(p, width, height, stride, phase, generation, base);
            return p;
        };
        expect(apply(0, 1) == field(0), "first field must not borrow uninitialized history");
        expect(apply(1, 19) == field(1), "a new phase needs a stationary comparison");
        full(apply(0, 20)); full(apply(1, 40));
        full(apply(1, 40)); // repeated presentation, not a new field
        full(apply(0, 400)); // skipped fields and arbitrary presentation rate

        auto changed = field(0);
        for (unsigned y = 2; y < 4; ++y) changed[(y * stride + 3) * 4] = 200;
        const auto source = changed;
        reconstruction.reconstruct(changed, width, height, stride, 0, 401, 0);
        expect(changed[(3 * stride + 3) * 4] == 200, "changed pixels must not borrow old opposite rows");
        auto repeat = source;
        reconstruction.reconstruct(repeat, width, height, stride, 0, 401, 0);
        expect(repeat == changed, "duplicate presentation must not turn motion into stationary detail");
        repeat = source;
        reconstruction.reconstruct(repeat, width, height, stride, 0, 450, 0);
        expect(repeat[(3 * stride + 3) * 4] == 200, "stale opposite pixels stay invalid until that field is refreshed");
        expect(apply(0, 451, 16) == field(0), "changed viewport offset resets history");
        reconstruction.reset();
        expect(apply(1, 1) == field(1), "device reset clears old fields");
        std::puts("field reconstruction: stationary detail, motion, repeated/skipped fields, and resets passed");
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
