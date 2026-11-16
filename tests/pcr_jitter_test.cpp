// The temporal anti-aliasing's jitter (port/gs/pc/pcr_jitter.h): none is no move, packed to exactly 0 (the pictures
// with no temporal method stay as they were); every offset within half a pixel; SMAA T2x's two as SMAA.hlsl gives
// them (y turned down); Halton's 8 evenly about the middle; the packing as GLSL's unpackSnorm2x16 reads it.
#include "pcr_jitter.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

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

// GLSL's unpackSnorm2x16, one half
float unpack(uint32_t packed, int half) {
    int16_t v = int16_t(uint16_t(packed >> (16 * half)));
    return std::fmax(-1.0f, float(v) / 32767.0f);
}

void none_is_nothing() {
    CHECK(jitter_length(JitterSequence::none) == 1);
    for (uint32_t i = 0; i < 5; i++) {
        Jitter j = jitter_at(JitterSequence::none, i);
        CHECK(j.x_px == 0.0f && j.y_px == 0.0f);
        CHECK(jitter_packed(j) == 0u);
    }
    CHECK(jitter_length(JitterSequence::count) == 1);
    CHECK(jitter_at(JitterSequence::count, 3).x_px == 0.0f);
}

void within_half_a_pixel() {
    for (auto s : {JitterSequence::smaa_t2x, JitterSequence::halton8}) {
        for (uint32_t i = 0; i < 2 * jitter_length(s); i++) {
            Jitter j = jitter_at(s, i);
            CHECK(std::fabs(j.x_px) < 0.5f && std::fabs(j.y_px) < 0.5f);
            // (wraps)
            Jitter again = jitter_at(s, i + jitter_length(s));
            CHECK(again.x_px == j.x_px && again.y_px == j.y_px);
        }
    }
}

void smaa_t2x_as_smaa_has_it() {
    CHECK(jitter_length(JitterSequence::smaa_t2x) == 2);
    Jitter a = jitter_at(JitterSequence::smaa_t2x, 0), b = jitter_at(JitterSequence::smaa_t2x, 1);
    // (SMAA.hlsl: (0.25, -0.25) and (-0.25, 0.25) with y up)
    CHECK(a.x_px == 0.25f && a.y_px == 0.25f);
    CHECK(b.x_px == -0.25f && b.y_px == -0.25f);
}

void halton_about_the_middle() {
    CHECK(jitter_length(JitterSequence::halton8) == 8);
    float sx = 0.0f, sy = 0.0f;
    for (uint32_t i = 0; i < 8; i++) {
        Jitter j = jitter_at(JitterSequence::halton8, i);
        sx += j.x_px;
        sy += j.y_px;
        for (uint32_t k = 0; k < i; k++) {
            Jitter o = jitter_at(JitterSequence::halton8, k);
            CHECK(o.x_px != j.x_px || o.y_px != j.y_px); // (no offset twice)
        }
    }
    CHECK(std::fabs(sx / 8.0f) < 0.07f && std::fabs(sy / 8.0f) < 0.07f);
    // (its first: Halton's index 1, 1/2 and 1/3, less a half)
    Jitter first = jitter_at(JitterSequence::halton8, 0);
    CHECK(first.x_px == 0.0f && std::fabs(first.y_px - (1.0f / 3.0f - 0.5f)) < 1e-6f);
}

void packed_as_glsl_reads_it() {
    for (Jitter j : {Jitter{0.25f, -0.25f}, Jitter{-0.4375f, 0.3125f}, Jitter{0.0f, 0.5f}}) {
        uint32_t p = jitter_packed(j);
        CHECK(std::fabs(unpack(p, 0) - j.x_px) < 1.0f / 32767.0f);
        CHECK(std::fabs(unpack(p, 1) - j.y_px) < 1.0f / 32767.0f);
    }
}

} // namespace

int main() {
    none_is_nothing();
    within_half_a_pixel();
    smaa_t2x_as_smaa_has_it();
    halton_about_the_middle();
    packed_as_glsl_reads_it();
    if (failures == 0) {
        std::printf("pcr_jitter_test: ok\n");
    }
    return failures == 0 ? 0 : 1;
}
