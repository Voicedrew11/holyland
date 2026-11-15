// The GS's blend as Vulkan's (port/gs/pc/pcr_blend.h)
#include "pcr_blend.h"

#include <cstdio>

using namespace pcr;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

// ALPHA's register: A, B, C, D (2 bits each) and FIX
uint64_t alpha(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t fix) {
    return uint64_t(a | b << 2 | c << 4 | d << 6) | uint64_t(fix) << 32;
}

// A blend that leaves the colour: drawn without the blend, the alpha alone written, the GS's result had
bool keeps_colour(const BlendChoice &b) {
    return !b.enable && !b.write_rgb && b.supported && b.flags == 0;
}

void test_blend_off() {
    BlendChoice b = choose_blend(alpha(0, 1, 0, 1, 0), false);
    CHECK(!b.enable && b.write_rgb && b.supported && b.flags == 0);
}

void test_a_same_as_b() {
    CHECK(keeps_colour(choose_blend(alpha(1, 1, 0, 1, 0), true)));  // (Cd - Cd) * As + Cd
    CHECK(keeps_colour(choose_blend(alpha(2, 2, 2, 1, 77), true))); // (0 - 0) * FIX + Cd
    BlendChoice cs = choose_blend(alpha(0, 0, 0, 0, 0), true);      // (Cs - Cs) * As + Cs
    CHECK(!cs.enable && cs.write_rgb && cs.flags == 0);
}

void test_fix_zero_keeps_colour() {
    // Cs * FIX + Cd and Cd - Cs * FIX with FIX 0: Cd (the game's pass that sets the alpha to 0, ALPHA 0x68)
    CHECK(keeps_colour(choose_blend(alpha(0, 2, 2, 1, 0), true)));
    CHECK(keeps_colour(choose_blend(alpha(2, 0, 2, 1, 0), true)));
    CHECK((alpha(0, 2, 2, 1, 0) & 0xFF) == 0x68);
}

void test_fix_not_zero_blends() {
    BlendChoice add = choose_blend(alpha(0, 2, 2, 1, 1), true); // Cs * FIX + Cd, FIX 1
    CHECK(add.enable && add.write_rgb && add.supported);
    CHECK(add.dst == VK_BLEND_FACTOR_ONE && add.op == VK_BLEND_OP_ADD && add.flags == (F_PREMUL | F_C_FIX));
    BlendChoice sub = choose_blend(alpha(2, 0, 2, 1, 0x80), true); // Cd - Cs * FIX
    CHECK(sub.enable && sub.write_rgb && sub.op == VK_BLEND_OP_REVERSE_SUBTRACT && sub.flags == (F_PREMUL | F_C_FIX));
}

void test_by_as_not_kept() {
    // the same forms by As: As is the pixel's, 0 only somewhere, so the blend stays
    BlendChoice add = choose_blend(alpha(0, 2, 0, 1, 0), true);
    CHECK(add.enable && add.write_rgb && add.flags == F_PREMUL);
    BlendChoice sub = choose_blend(alpha(2, 0, 0, 1, 0), true);
    CHECK(sub.enable && sub.write_rgb && sub.op == VK_BLEND_OP_REVERSE_SUBTRACT);
    // FIX 0 with D 0 (Cs * 0 + Cs): not Cd, not supported as before
    BlendChoice d0 = choose_blend(alpha(0, 2, 2, 0, 0), true);
    CHECK(d0.write_rgb && !d0.supported);
}

void test_usual_blend() {
    BlendChoice b = choose_blend(alpha(0, 1, 0, 1, 0), true); // (Cs - Cd) * As + Cd
    CHECK(b.enable && b.write_rgb && b.supported);
    CHECK(b.src == VK_BLEND_FACTOR_SRC_ALPHA && b.dst == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    CHECK(b.src_alpha == VK_BLEND_FACTOR_CONSTANT_ALPHA && b.flags == F_ALPHA_FACTOR);
    BlendChoice ad = choose_blend(alpha(0, 1, 1, 1, 0), true); // C = Ad: the shader's
    CHECK(!ad.supported);
}

} // namespace

int main() {
    test_blend_off();
    test_a_same_as_b();
    test_fix_zero_keeps_colour();
    test_fix_not_zero_blends();
    test_by_as_not_kept();
    test_usual_blend();
    if (failures) {
        std::printf("pcr_blend: %d failures\n", failures);
        return 1;
    }
    std::printf("pcr_blend: all passed\n");
    return 0;
}
