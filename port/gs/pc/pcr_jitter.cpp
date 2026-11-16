// The sub-pixel offsets of a temporal anti-aliasing method (pcr_jitter.h)
#include "pcr_jitter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace pcr {

namespace {

// SMAA T2x's, as SMAA.hlsl gives them (y up) turned to y down
constexpr std::array<Jitter, 2> smaa_t2x = {{{0.25f, 0.25f}, {-0.25f, -0.25f}}};

// The radical inverse of i in base, 0 to 1 (Halton's sequence)
constexpr float radical_inverse(uint32_t i, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= float(base);
        r += f * float(i % base);
        i /= base;
    }
    return r;
}

// Halton (2, 3) from index 1 (index 0 is the corner, 0 0), less a half so that it is about the pixel's middle
constexpr std::array<Jitter, 8> make_halton8() {
    std::array<Jitter, 8> h{};
    for (uint32_t i = 0; i < h.size(); i++) {
        h[i] = {radical_inverse(i + 1, 2) - 0.5f, radical_inverse(i + 1, 3) - 0.5f};
    }
    return h;
}
constexpr std::array<Jitter, 8> halton8 = make_halton8();

std::span<const Jitter> sequence_of(JitterSequence sequence) {
    switch (sequence) {
    case JitterSequence::smaa_t2x:
        return smaa_t2x;
    case JitterSequence::halton8:
        return halton8;
    default:
        return {};
    }
}

// x (-1 to 1) as GLSL's packSnorm2x16 packs it
uint32_t snorm16(float x) {
    float clamped = std::clamp(x, -1.0f, 1.0f);
    return uint32_t(uint16_t(int16_t(std::lround(clamped * 32767.0f))));
}

} // namespace

Jitter jitter_at(JitterSequence sequence, uint32_t index) {
    std::span<const Jitter> s = sequence_of(sequence);
    return s.empty() ? Jitter{} : s[index % s.size()];
}

uint32_t jitter_length(JitterSequence sequence) {
    std::span<const Jitter> s = sequence_of(sequence);
    return s.empty() ? 1 : uint32_t(s.size());
}

uint32_t jitter_packed(Jitter jitter) {
    return snorm16(jitter.x_px) | snorm16(jitter.y_px) << 16;
}

} // namespace pcr
