// SPDX-License-Identifier: GPL-3.0-or-later
#include "generated_cases.h"
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {
unsigned checks = 0, failures = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }
float value(uint32_t input) { return std::bit_cast<float>(input); }
}
int main() {
    R5900Context ctx;
    ctx.f[0] = 16.0f;
    ctx.f[12] = 25.0f;
    sqrt_distance(&ctx);
    const float distance = ctx.f[0];
    ctx.f[20] = 6.0f; ctx.f[21] = 0.0f;
    edge_square_x(&ctx); edge_square_z(&ctx); edge_sum(&ctx);
    sqrt_distance(&ctx);
    const float edgeLength = ctx.f[0];
    edge_normalize_x(&ctx); edge_normalize_z(&ctx);
#if EXPECT_ORIGINAL_OPERAND_BUG
    check(distance == 4.0f, "original raw 0x460C0004 reads stale Fs=16 instead of Ft=25");
    check(edgeLength == 0.0f && std::isinf(ctx.f[2]), "original axis-aligned ground edge has zero length and infinite normal");
    ctx.f[4] = 6.0f; ctx.f[5] = 9.0f;
    rsqrt_distinct(&ctx);
    check(ctx.f[3] > 0.408f && ctx.f[3] < 0.409f, "original RSQRT ignores Ft and numerator");
#else
    check(distance == 5.0f, "raw 0x460C0004 computes full planar distance from Ft");
    check(edgeLength == 6.0f && ctx.f[2] == 1.0f && ctx.f[1] == 0.0f, "axis-aligned ground edge has finite unit normal");
    for (const auto function : {sqrt_distinct, sqrt_alias_ft, sqrt_alias_fs}) {
        ctx.f[7] = 9.0f; ctx.f[12] = 49.0f; ctx.f[21] = -99.0f;
        function(&ctx);
        const float result = function == sqrt_distinct ? ctx.f[21] :
            function == sqrt_alias_ft ? ctx.f[12] : ctx.f[7];
        check(result == 7.0f, "SQRT captures Ft correctly with distinct sources and destination aliases");
    }
    for (const uint32_t input : {0u, 0x80000000u, 1u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu}) {
        ctx.f[12] = value(input); ctx.f[7] = 81.0f; ctx.fcr31 = 0x00A3C070u;
        sqrt_distinct(&ctx);
        check(bits(ctx.f[21]) == (input & 0x80000000u), "SQRT preserves signed zero and flushes denormals");
        check(ctx.fcr31 == 0x00A0C070u, "SQRT clears live I/D while preserving condition and sticky flags");
    }
    ctx.f[12] = -9.0f; ctx.f[7] = 81.0f; ctx.fcr31 = 0x0081C010u;
    sqrt_distinct(&ctx);
    check(ctx.f[21] == 3.0f, "SQRT negative normal returns sqrt of absolute magnitude");
    check(ctx.fcr31 == 0x0082C050u, "SQRT negative sets invalid/sticky invalid and preserves unrelated flags");
    ctx.f[12] = 9.0f;
    sqrt_distinct(&ctx);
    check(ctx.f[21] == 3.0f && ctx.fcr31 == 0x0080C050u, "next positive SQRT clears invalid but retains sticky invalid");
    for (const uint32_t input : {0x7F800000u, 0x7FC00001u, 0xFF800000u, 0xFFC00001u}) {
        ctx.f[12] = value(input); ctx.fcr31 = 0;
        sqrt_distinct(&ctx);
        check(bits(ctx.f[21]) == bits(sqrtf(std::numeric_limits<float>::max())), "EE exponent-255 input is bounded before host sqrt");
        check(ctx.fcr31 == ((input & 0x80000000u) ? 0x00020040u : 0u), "exponent-255 sign controls invalid flag");
    }
    for (const auto function : {rsqrt_distinct, rsqrt_alias_ft, rsqrt_alias_fs}) {
        ctx.f[4] = 6.0f; ctx.f[5] = 9.0f; ctx.f[3] = -99.0f;
        function(&ctx);
        const float result = function == rsqrt_distinct ? ctx.f[3] :
            function == rsqrt_alias_ft ? ctx.f[5] : ctx.f[4];
        check(result == 2.0f, "RSQRT captures Fs numerator and Ft denominator before destination aliases");
    }
    ctx.f[4] = -6.0f; ctx.f[5] = -9.0f; ctx.fcr31 = 0x0081C010u;
    rsqrt_distinct(&ctx);
    check(ctx.f[3] == -2.0f && ctx.fcr31 == 0x0082C050u, "RSQRT negative denominator uses abs and invalid flags");
    for (const uint32_t input : {0u, 0x80000000u, 1u, 0x80000001u}) {
        ctx.f[4] = -6.0f; ctx.f[5] = value(input); ctx.fcr31 = 0x0082C050u;
        rsqrt_distinct(&ctx);
        check(bits(ctx.f[3]) == ((input & 0x80000000u) | 0x7F7FFFFFu), "RSQRT zero result saturates with denominator sign");
        check(ctx.fcr31 == 0x0081C070u, "RSQRT zero clears invalid and sets divide/sticky divide");
    }
    ctx.f[4] = value(0x80000001u); ctx.f[5] = 9.0f; ctx.fcr31 = 0;
    rsqrt_distinct(&ctx);
    check(bits(ctx.f[3]) == 0x80000000u, "RSQRT numerator denormal flushes to signed zero");
    ctx.f[4] = std::numeric_limits<float>::max(); ctx.f[5] = std::numeric_limits<float>::min();
    rsqrt_distinct(&ctx);
    check(bits(ctx.f[3]) == 0x7F7FFFFFu, "RSQRT overflow saturates without introducing host infinity");
    ctx.f[4] = std::numeric_limits<float>::min(); ctx.f[5] = std::numeric_limits<float>::max();
    rsqrt_distinct(&ctx);
    check(bits(ctx.f[3]) == 0u, "RSQRT underflow flushes to zero");
#endif
    ctx.f[7] = -3.0f; ctx.f[12] = 8.0f;
    add_unchanged(&ctx); check(ctx.f[21] == 5.0f, "ADD still reads Fs and Ft");
    sub_unchanged(&ctx); check(ctx.f[21] == -11.0f, "SUB operand order unchanged");
    mul_unchanged(&ctx); check(ctx.f[21] == -24.0f, "MUL operand order unchanged");
    abs_unchanged(&ctx); check(ctx.f[21] == 3.0f, "ABS still reads Fs");
    mov_unchanged(&ctx); check(ctx.f[21] == -3.0f, "MOV still reads Fs");
    neg_unchanged(&ctx); check(ctx.f[21] == 3.0f, "NEG still reads Fs");
    std::printf("%u checks, %u failures (%s)\n", checks, failures,
        EXPECT_ORIGINAL_OPERAND_BUG ? "original operand control" : "production generated execution");
    return failures ? 1 : 0;
}
