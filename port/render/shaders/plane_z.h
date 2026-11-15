// A triangle's Z at a point, exactly: the plane through its three corners, rounded to a whole Z, half to even.
//
// Shared by the PC renderer's fragment stage (port/gs/pc/shaders/pcr_exact_z.h, GLSL with 64-bit integers) and its
// unit test (tests/plane_z_test.cpp, C++, against 128-bit integers): one source, written in what the two languages
// share, so the test runs the arithmetic the GPU runs.
//
// The corners and the point are integers in one unit (any: the caller's 16ths of a GS pixel times the target's
// scale), each within PLANE_Z_MOST of the origin; a corner's Z is an integer in 2^-PLANE_Z_FRACTION of a Z (a picture
// between's moved corner keeps a fraction), 0 to 2^40. Then the plane's numerator, Z0 area + dZ1 u + dZ2 v, is under
// 2^96 (worked out in 128 bits), and its divisor, area 2^PLANE_Z_FRACTION, under 2^62.
#ifndef PLANE_Z_H
#define PLANE_Z_H

#define PLANE_Z_FRACTION 8
#define PLANE_Z_MOST (1 << 25)

// (64-bit constants as 32-bit literals made 64-bit: glslang pastes no literal suffixes)
#ifdef __cplusplus
#include <cmath>
#include <cstdint>
#define PZ_FN static inline
PZ_FN float pz_floor(float v) {
    return std::floor(v);
}
PZ_FN float pz_clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
PZ_FN float pz_abs(float v) {
    return std::fabs(v);
}
#else
#define PZ_FN
#define pz_floor floor
#define pz_clamp clamp
#define pz_abs abs
#endif

// No plane: the corners have no area (the GPU draws none of such a triangle either)
#define PLANE_Z_NONE int64_t(uint64_t(1u) << 63)

// A 128-bit integer, two's complement
struct PzWide {
    uint64_t hi;
    uint64_t lo;
};

PZ_FN PzWide pz_wide(uint64_t hi, uint64_t lo) {
    PzWide w;
    w.hi = hi;
    w.lo = lo;
    return w;
}

PZ_FN PzWide pz_neg(PzWide a) {
    uint64_t lo = ~a.lo + uint64_t(1u);
    return pz_wide(~a.hi + (lo == uint64_t(0u) ? uint64_t(1u) : uint64_t(0u)), lo);
}

PZ_FN PzWide pz_add(PzWide a, PzWide b) {
    uint64_t lo = a.lo + b.lo;
    return pz_wide(a.hi + b.hi + (lo < a.lo ? uint64_t(1u) : uint64_t(0u)), lo);
}

// a b, whole (|a| and |b| under 2^63)
PZ_FN PzWide pz_mul(int64_t a, int64_t b) {
    uint64_t ua = uint64_t(a < int64_t(0) ? -a : a), ub = uint64_t(b < int64_t(0) ? -b : b);
    uint64_t a0 = ua & uint64_t(0xFFFFFFFFu), a1 = ua >> 32, b0 = ub & uint64_t(0xFFFFFFFFu), b1 = ub >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & uint64_t(0xFFFFFFFFu)) + (p10 & uint64_t(0xFFFFFFFFu));
    PzWide r = pz_wide(p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (p00 & uint64_t(0xFFFFFFFFu)) | (mid << 32));
    return (a < int64_t(0)) != (b < int64_t(0)) ? pz_neg(r) : r;
}

// About its value (a float's estimate: the steps of pz_divide_round need no more)
PZ_FN float pz_float(PzWide a) {
    bool fits = a.hi == (int64_t(a.lo) < int64_t(0) ? ~uint64_t(0u) : uint64_t(0u));
    if (fits) {
        return float(int64_t(a.lo));
    }
    return float(int64_t(a.hi)) * 18446744073709551616.0f + float(a.lo);
}

// n / d rounded to a whole number, half to even (0 < d < 2^62). Each step takes off the float's estimate of the
// remainder's quotient, which is right to about 2^-20 of it on any GPU, so every step shortens the remainder by 2^19 or
// more, and 0 <= r < d within a few steps; then r against d / 2 rounds. A quotient past 2^61 (a point far outside a
// thin triangle: its Z is clamped anyway) comes out about right, the steps stopped there.
PZ_FN int64_t pz_divide_round(PzWide n, int64_t d) {
    const float most = 2.0e18f;
    int64_t q = int64_t(0);
    PzWide r = n;
    float fd = float(d);
    for (int i = 0; i < 16; i++) {
        bool negative = int64_t(r.hi) < int64_t(0);
        if (!negative && r.hi == uint64_t(0u) && r.lo < uint64_t(d)) {
            break;
        }
        float step_f = pz_clamp(pz_floor(pz_float(r) / fd), -most, most);
        int64_t step = int64_t(step_f);
        if (step == int64_t(0)) {
            step = negative ? int64_t(-1) : int64_t(1);
        }
        if (pz_abs(float(q) + step_f) > most) {
            return q + step;
        }
        q += step;
        r = pz_add(r, pz_neg(pz_mul(step, d)));
    }
    uint64_t twice = r.lo << 1;
    if (twice > uint64_t(d) || (twice == uint64_t(d) && (q & int64_t(1)) != int64_t(0))) {
        q += int64_t(1);
    }
    return q;
}

// The plane through (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) at (px, py): its Z, whole (z in 2^-PLANE_Z_FRACTION),
// rounded half to even; PLANE_Z_NONE when the corners have no area. Z (P) = z0 + (dz1 cross (d, b) + dz2 cross (a, d))
// / cross (a, b), with a and b corners 1 and 2 less corner 0 and d the point less it: the same value whichever corner
// is first.
PZ_FN int64_t plane_z(int64_t px, int64_t py, int64_t x0, int64_t y0, int64_t z0, int64_t x1, int64_t y1, int64_t z1,
                      int64_t x2, int64_t y2, int64_t z2) {
    int64_t ax = x1 - x0, ay = y1 - y0, bx = x2 - x0, by = y2 - y0, dx = px - x0, dy = py - y0;
    int64_t area = ax * by - ay * bx;
    if (area == int64_t(0)) {
        return PLANE_Z_NONE;
    }
    int64_t u = dx * by - dy * bx, v = ax * dy - ay * dx;
    PzWide n = pz_add(pz_add(pz_mul(z0, area), pz_mul(z1 - z0, u)), pz_mul(z2 - z0, v));
    if (area < int64_t(0)) {
        area = -area;
        n = pz_neg(n);
    }
    return pz_divide_round(n, area << PLANE_Z_FRACTION);
}

#endif
