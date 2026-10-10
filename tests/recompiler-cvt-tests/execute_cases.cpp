// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated_cases.h"
#include <array>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {
unsigned checks = 0, failures = 0;
void check(bool pass, const char* description) {
    ++checks;
    if (!pass) {
        ++failures;
        if (failures <= 16) std::fprintf(stderr, "FAIL: %s\n", description);
    }
}
float value(uint32_t input) { return std::bit_cast<float>(input); }
uint32_t bits(float input) { return std::bit_cast<uint32_t>(input); }
// Independent floating-point oracle: comparisons are against exactly
// representable double bounds; only values strictly inside the signed range
// reach the language's defined, truncating float-to-integer conversion.
uint32_t expected(uint32_t input) {
    const double number = static_cast<double>(value(input));
    if (std::isnan(number)) return input & 0x80000000u ? 0x80000000u : 0x7FFFFFFFu;
    if (number >= 2147483648.0) return 0x7FFFFFFFu;
    if (number <= -2147483648.0) return 0x80000000u;
    return static_cast<uint32_t>(static_cast<int32_t>(number));
}
// This reference polynomial is the mathematical ninth-order Taylor series,
// not a game asset or a substitute production VU interpreter. Range reduction
// is the actual emitted code above; the approximation demonstrates why its
// principal interval matters to skeletal rotations.
std::array<float, 2> approximate_trig(float angle) {
    constexpr float halfPi = 1.57079632679489661923f;
    const float reduced = angle < 0 ? halfPi + angle : halfPi - angle;
    const float square = reduced * reduced;
    const float cosine = reduced * (1.0f + square * (-1.0f / 6.0f + square *
        (1.0f / 120.0f + square * (-1.0f / 5040.0f + square / 362880.0f))));
    const float sine = std::copysign(std::sqrt(std::max(0.0f, 1.0f - cosine * cosine)), angle);
    return {sine, cosine};
}
}
int main() {
    constexpr std::array<uint32_t, 42> inputs{
        0u, 0x80000000u, 1u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
        0x00800000u, 0x80800000u, 0x3EFFFFFFu, 0xBEFFFFFFu,
        0x3F000000u, 0xBF000000u, 0x3F400000u, 0xBF400000u,
        0x3F7FFFFFu, 0xBF7FFFFFu, 0x3F800000u, 0xBF800000u,
        0x3FC00000u, 0xBFC00000u, 0x40200000u, 0xC0200000u,
        0x404FFFFFu, 0xC04FFFFFu, 0x4B000001u, 0xCB000001u,
        0x4EFFFFFFu, 0xCEFFFFFFu, 0x4F000000u, 0xCF000000u,
        0x4F000001u, 0xCF000001u, 0x7F7FFFFFu, 0xFF7FFFFFu,
        0x7F800000u, 0xFF800000u, 0x7FC00001u, 0xFFC00001u,
        0x7F800001u, 0xFF800001u, 0x7FFFFFFFu, 0xFFFFFFFFu};
    const int initialMode = std::fegetround();
    R5900Context ctx;
    for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        check(std::fesetround(mode) == 0 && std::fegetround() == mode,
            "host rounding mode is actually selected");
        for (uint32_t input : inputs) {
#if ORIGINAL_ROUNDING_CONTROL
            // The old host cast is undefined for NaNs/overflow. Exercise its
            // rounding defect only on defined in-range inputs.
            if ((input & 0x7F800000u) >= 0x4F000000u) continue;
#endif
            for (const auto& item : cases) {
                std::array<uint32_t, 32> before{};
                for (unsigned i = 0; i < 32; ++i) {
                    before[i] = 0x3F000000u + i * 0x10203u;
                    ctx.f[i] = value(before[i]);
                }
                ctx.f[item.fs] = value(input);
                before[item.fs] = input;
                ctx.fcr31 = 0xA5C3C07Fu;
                ctx.f_acc = -23.0f;
                ctx.vu0_q = 19.0f;
                item.function(&ctx);
                check(bits(ctx.f[item.fd]) == expected(input),
                    "generated CVT.W.S truncates and saturates raw bits");
                for (unsigned i = 0; i < 32; ++i)
                    if (i != item.fd) check(bits(ctx.f[i]) == before[i],
                        "distinct source and unrelated FPR bits are preserved");
                check(ctx.fcr31 == 0xA5C3C07Fu && ctx.f_acc == -23.0f && ctx.vu0_q == 19.0f,
                    "FCR31, COP1 accumulator and VU Q remain unchanged");
                check(std::fegetround() == mode, "conversion leaves host rounding mode unchanged");
            }
        }
    }
    check(std::fesetround(FE_TONEAREST) == 0, "restore nearest mode for arithmetic sequence");
    constexpr float pi = 3.14159265358979323846f;
    for (float angle : {-3.0f, -1.4f, -0.7f, -0.07f, -0.03f, 0.0f,
        0.03f, 0.07f, 0.7f, 1.4f, 3.0f}) {
        ctx.f[12] = angle; ctx.f[20] = 2.0f * pi; ctx.f[21] = pi;
        ctx.fcr31 = 0x0083C070u;
        wrap_angle(&ctx);
        check(std::fabs(ctx.f[12] - angle) < 5e-7f,
            "emitted principal-angle reduction preserves positive and negative rotations");
        check(ctx.fcr31 == 0x0083C070u, "emitted angle reduction preserves FCR31");
        if (std::fabs(angle) >= 0.03f && std::fabs(angle) <= 1.4f) {
            const auto actual = approximate_trig(ctx.f[12]);
            check(std::fabs(actual[0] - std::sin(angle)) < 0.003f &&
                std::fabs(actual[1] - std::cos(angle)) < 0.00002f,
                "reduced rotation stays within the polynomial's principal interval");
        }
    }
    // Single evaluation remains guaranteed even for an expression argument.
    int evaluations = 0;
    const auto argument = [&]() { ++evaluations; return 1.75f; };
    check(FPU_CVT_W_S(argument()) == 1 && evaluations == 1,
        "CVT helper evaluates its argument once");
    std::fesetround(initialMode);
    std::printf("%u checks, %u failures (%s)\n", checks, failures,
        ORIGINAL_ROUNDING_CONTROL ? "original rounding negative control" : "production generated execution");
    return failures ? 1 : 0;
}
