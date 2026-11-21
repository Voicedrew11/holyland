// A texture's smaller levels made on the PC (port/gs/shared/mip_chain.h): the levels' count and sizes, four texels
// averaged, transparent texels' colour kept out of the colour, odd and 1-wide sides.
#include "mip_chain.h"

#include <cstdio>
#include <vector>

using namespace pcr;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | g << 8 | b << 16 | a << 24;
}

void levels_counted() {
    CHECK(mip_chain_levels(1, 1) == 1);
    CHECK(mip_chain_levels(2, 2) == 2);
    CHECK(mip_chain_levels(256, 256) == 9);
    CHECK(mip_chain_levels(512, 32) == 10);
    CHECK(mip_chain_levels(1024, 1024) == 11);
}

void sizes_back_to_back() {
    std::vector<uint32_t> level0(256 * 64, rgba(10, 20, 30, 128));
    std::vector<uint32_t> below = mip_chain_below(level0, 256, 64);
    // 128x32 + 64x16 + 32x8 + 16x4 + 8x2 + 4x1 + 2x1 + 1x1
    CHECK(below.size() == 4096u + 1024 + 256 + 64 + 16 + 4 + 2 + 1);
    for (uint32_t t : below) {
        CHECK(t == rgba(10, 20, 30, 128));
    }
    CHECK(mip_chain_below(level0, 1, 1).empty());
    CHECK(mip_chain_below(std::vector<uint32_t>(3), 2, 2).empty());
}

void four_averaged() {
    std::vector<uint32_t> level0 = {rgba(0, 0, 0, 255), rgba(255, 255, 255, 255), rgba(100, 0, 0, 255),
                                    rgba(0, 100, 0, 255)};
    std::vector<uint32_t> below = mip_chain_below(level0, 2, 2);
    CHECK(below.size() == 1);
    CHECK(below[0] == rgba(89, 89, 64, 255));
}

void transparent_colour_left_out() {
    // a cut-out's edge: two opaque grey texels, two transparent black ones; the colour stays grey, the alpha halves
    std::vector<uint32_t> level0 = {rgba(200, 200, 200, 128), rgba(0, 0, 0, 0), rgba(200, 200, 200, 128),
                                    rgba(0, 0, 0, 0)};
    std::vector<uint32_t> below = mip_chain_below(level0, 2, 2);
    CHECK(below.size() == 1);
    CHECK(below[0] == rgba(200, 200, 200, 64));
    // all transparent: the plain average
    std::vector<uint32_t> clear = {rgba(8, 0, 0, 0), rgba(0, 0, 0, 0), rgba(0, 0, 0, 0), rgba(0, 0, 0, 0)};
    CHECK(mip_chain_below(clear, 2, 2)[0] == rgba(2, 0, 0, 0));
}

void one_wide() {
    std::vector<uint32_t> level0 = {rgba(0, 0, 0, 255), rgba(40, 0, 0, 255), rgba(80, 0, 0, 255),
                                    rgba(120, 0, 0, 255)};
    std::vector<uint32_t> below = mip_chain_below(level0, 1, 4);
    CHECK(below.size() == 3); // 1x2, 1x1
    CHECK(below[0] == rgba(20, 0, 0, 255));
    CHECK(below[1] == rgba(100, 0, 0, 255));
    CHECK(below[2] == rgba(60, 0, 0, 255));
}

} // namespace

int main() {
    levels_counted();
    sizes_back_to_back();
    four_averaged();
    transparent_colour_left_out();
    one_wide();
    if (failures) {
        std::printf("mip_chain: %d failures\n", failures);
        return 1;
    }
    std::printf("mip_chain: ok\n");
    return 0;
}
