// Checks port/render/ripple.cpp (the reflection's rippled water, the CPU's part) and the S and T arithmetic of
// port/render/shaders/ripple_maths.h against the code they stand for, Wobble_DrawRippledModel (gfx/wobble.c), as
// game/native_draw/nd_water.c's KF4_ND_WATER_CHECK writes it out:
//   - the record's vertex matrix: Matrix_Multiply(world_screen, model by part) without VU0's registers (nd_water.c's
//     vu_matrix_multiply, in VU arithmetic: port/include/port/vu_float.h), bit for bit, for random matrices;
//   - each vertex's place and ADC (check_strips: Vu0_ProjectToScreen3's FTOI4, the window of the vertex and the two
//     before it, the data's bit 15), for random strips seen through a camera-like matrix;
//   - S and T: (X - 0x6C00) / 10240 and (Y - 0x7200) / 7168 in the game's floats (the EE's FPU rounds toward zero:
//     the C here run with MXCSR so; and to nearest), against the shader's way (the nearest quotient by fused
//     multiply-adds, then toward zero where it went past), copied here in C++, for every X and Y of 16 bits.
//
//     out/<preset>/ripple_test     prints the failures, and exits 1 if any
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>

#include <vector>

#include "../port/render/ripple.hpp"

extern "C" {
#include "port/vu_float.h"
}

static uint32_t rng_state = 0x1234567u;

static uint32_t rng() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static float random_float() {
    if (rng() % 16 == 0) {
        return 0.0f;
    }
    uint32_t e = 110 + rng() % 40;
    uint32_t bits = (rng() & 0x807FFFFFu) | e << 23;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static int failures, checks;

static void fail(const char *what, int trial) {
    failures++;
    if (failures <= 20) {
        printf("FAIL trial %d: %s\n", trial, what);
    }
}

// ---- nd_water.c, as the game's side writes it ----------------------------------------------------------------------
static void vu_matrix_multiply(float *dst, const float *matrix, const float *src) {
    float out[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float acc = Vu_FMul(matrix[row], src[col * 4 + 0]);
            acc = Vu_FAdd(acc, Vu_FMul(matrix[4 + row], src[col * 4 + 1]));
            acc = Vu_FAdd(acc, Vu_FMul(matrix[8 + row], src[col * 4 + 2]));
            out[col * 4 + row] = Vu_FAdd(acc, Vu_FMul(matrix[12 + row], src[col * 4 + 3]));
        }
    }
    memcpy(dst, out, sizeof(out));
}

static void check_place(const float *vm, float x, float y, float z, int32_t out[3]) {
    float p[4] = {x, y, z, 1.0f};
    float c[4];
    for (int row = 0; row < 4; row++) {
        float acc = Vu_FMul(vm[row], p[0]);
        acc = Vu_FAdd(acc, Vu_FMul(vm[4 + row], p[1]));
        acc = Vu_FAdd(acc, Vu_FMul(vm[8 + row], p[2]));
        c[row] = Vu_FAdd(acc, Vu_FMul(vm[12 + row], p[3]));
    }
    uint32_t q = vu_float_div(0x3F800000u, vu_float_bits(c[3]));
    for (int row = 0; row < 3; row++) {
        out[row] = vu_float_ftoi(vu_float_mul(vu_float_bits(c[row]), q), 4);
    }
}

// ---- ripple_maths.h's S and T, in C++ ------------------------------------------------------------------------------
static float toward_zero(float r) {
    uint32_t b;
    memcpy(&b, &r, 4);
    b -= 1;
    float f;
    memcpy(&f, &b, 4);
    return r == 0.0f ? r : f;
}

static float bits_float(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static float shader_game_float(int n, bool nearest) {
    uint32_t m = n < 0 ? 0u - uint32_t(n) : uint32_t(n);
    float f = float(m);
    if (!nearest && f > 16777216.0f && uint32_t(f) > m) {
        f = toward_zero(f);
    }
    return n < 0 ? -f : f;
}

static float shader_game_div(float n, float b, float y, bool nearest) {
    float q = n * y;
    float r = fmaf(-q, b, n);
    float s = fmaf(r, y, q);
    float e = fmaf(-s, b, n);
    return !nearest && e != 0.0f && (e < 0.0f) != (s < 0.0f) ? toward_zero(s) : s;
}

// The game's own: in the MXCSR rounding mode set (toward zero as the EE's FPU, or to nearest)
static volatile float divisor_s = 10240.0f, divisor_t = 7168.0f;

static float game_s(int32_t x) {
    volatile float n = (float)int32_t(uint32_t(x) - 0x6C00u); // (the EE's subtraction wraps)
    return n / divisor_s;
}

static float game_t(int32_t y) {
    volatile float n = (float)int32_t(uint32_t(y) - 0x7200u);
    return n / divisor_t;
}

int main() {
    const unsigned csr = _mm_getcsr();
    // the record's matrix
    for (int trial = 0; trial < 2000; trial++) {
        RdDraw d;
        RdCamera cam;
        memset(&d, 0, sizeof(d));
        memset(&cam, 0, sizeof(cam));
        for (int i = 0; i < 16; i++) {
            (&cam.world_screen[0][0])[i] = random_float();
            (&d.model[0][0])[i] = random_float();
            (&d.point_pos[0][0])[i] = random_float();
            (&d.point_col[0][0])[i] = random_float();
        }
        d.flags = trial & 1 ? RD_DRAW_ROUND_NEAREST : 0;
        RippleRecord k = ripple_record(d, cam);
        float want[16];
        vu_matrix_multiply(want, &cam.world_screen[0][0], &d.model[0][0]);
        checks++;
        if (memcmp(k.vertex, want, sizeof(want)) != 0) {
            fail("vertex matrix", trial);
        }
        checks++;
        if (memcmp(k.sines, d.point_pos, 64) != 0 || memcmp(k.sines + 16, d.point_col, 64) != 0 ||
            memcmp(k.screen, cam.screen, 64) != 0 || k.info[2] != ((trial & 1) ? RIPPLE_NEAREST : 0)) {
            fail("sines, screen or flags", trial);
        }
    }
    // the places and the ADC of strips through a camera: a projection to the screen's 12.4 middle (2048) and Z, with
    // the vertices about it, some off the 16 bits, behind the eye or past Z's bound
    for (int trial = 0; trial < 2000; trial++) {
        RippleRecord k;
        memset(&k, 0, sizeof(k));
        float s = 200.0f + float(rng() % 400);
        float zs = float(1 << (12 + rng() % 12));
        float m[16] = {s, 0, 0, 0, 0, s, 0, 0, 2048.0f, 2048.0f, zs, 1.0f, 0, 0, 0, 0};
        for (int i = 0; i < 16; i++) {
            m[i] = m[i] + (rng() % 4 == 0 ? random_float() * 1e-3f : 0.0f);
        }
        m[14] = float(int32_t(rng() % 4000) - 1000); // (w's offset: some vertices behind the eye)
        memcpy(k.vertex, m, sizeof(m));
        uint32_t n = 3 + rng() % 40;
        std::vector<int32_t> vertices(size_t(n) * 16, 0);
        for (uint32_t v = 0; v < n; v++) {
            int32_t *q = &vertices[size_t(v) * 16];
            q[0] = int32_t(rng() % 8000) - 4000;
            q[1] = int32_t(rng() % 8000) - 4000;
            q[2] = int32_t(rng() % 6000) - 1000;
            q[7] = rng() % 8 == 0 ? 0x8000 : int32_t(rng() & 0x7FFF);
        }
        std::vector<uint8_t> adc;
        ripple_adc(vertices.data(), n, k, adc);
        uint32_t window = 0;
        for (uint32_t v = 0; v < n; v++) {
            const int32_t *q = &vertices[size_t(v) * 16];
            int32_t p[3];
            check_place(k.vertex, float(q[0]), float(q[1]), float(q[2]), p);
            p[2] >>= 4;
            window |= (uint32_t)p[2] > 0xFFFF0 || ((p[0] | p[1]) & 0xFFFF0000);
            uint32_t flags = (window != 0 ? 0x8000u : 0) | uint32_t(q[7]);
            window = (window << 1) & 7;
            RipplePlace got = ripple_place(k, float(q[0]), float(q[1]), float(q[2]));
            checks++;
            if (got.x != p[0] || got.y != p[1] || got.z != p[2]) {
                fail("a vertex's place", trial);
            }
            checks++;
            if ((adc[v] != 0) != ((flags & 0x8000u) != 0)) {
                fail("a vertex's ADC", trial);
            }
        }
    }
    // S and T for every X and Y of 16 bits (and what the EE's subtraction makes of a place saturated to 0x80000000),
    // toward zero and to nearest
    for (int nearest = 0; nearest < 2; nearest++) {
        int bad = 0;
        for (int64_t x = -1; x <= 0xFFFF; x++) {
            int32_t xi = x < 0 ? int32_t(0x80000000u) : int32_t(x);
            _mm_setcsr((csr & ~_MM_ROUND_MASK) | (nearest ? _MM_ROUND_NEAREST : _MM_ROUND_TOWARD_ZERO));
            float ws = game_s(xi), wt = game_t(xi);
            _mm_setcsr(csr);
            float gs = shader_game_div(shader_game_float(int32_t(uint32_t(xi) - 0x6C00u), nearest), 10240.0f,
                                       bits_float(0x38CCCCCDu), nearest);
            float gt = shader_game_div(shader_game_float(int32_t(uint32_t(xi) - 0x7200u), nearest), 7168.0f,
                                       bits_float(0x39124925u), nearest);
            checks += 2;
            if (memcmp(&ws, &gs, 4) != 0 || memcmp(&wt, &gt, 4) != 0) {
                failures++;
                if (bad++ < 5) {
                    printf("FAIL S/T (%s) at X %d: game %.9g %.9g, shader %.9g %.9g\n", nearest ? "nearest" : "toward zero",
                           xi, double(ws), double(wt), double(gs), double(gt));
                }
            }
        }
    }
    printf("ripple_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
