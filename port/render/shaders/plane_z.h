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
typedef uint32_t pz_u32;
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
#define pz_u32 uint
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

// A plane as the fragment stage draws it: its Z at the target's pixel (kx, ky), worked out once a triangle (by the
// vertex stage) as three fixed-point numbers, each the plane's exact value rounded to 2^-PLANE_FIXED_BITS of a Z:
// base, the Z at pixel (0, 0); step_x and step_y, what a pixel across and down adds. Rounded from the plane's exact
// values, they depend on the plane alone (not on its triangle, nor on which corner is first): two meshes over one
// plane get the same three, so the same Z at every pixel. Each pixel's Z is then base + step_x kx + step_y ky
// (64-bit integers, kx and ky under 2^14), rounded half to even: within 2^-12 of a Z of the plane's exact Z, so it
// rounds as the exact one does but where that is within 2^-12 of a half.
// off: the plane's Z at pixel (0, 0) past 2^34 or a step past 2^20 of a Z (a triangle seen edge on, Z changing by more
// than a sixteenth of a 24-bit Z buffer a pixel), or no area: the caller's interpolated Z.
#define PLANE_FIXED_BITS 26
#define PLANE_FIXED_BASE_MOST (int64_t(1) << 60)
#define PLANE_FIXED_STEP_MOST (int64_t(1) << 46)
struct PzFixed {
    int64_t base;
    int64_t step_x;
    int64_t step_y;
    bool on;
};

// a << bits (0 < bits < 64), whole (the value under 2^126)
PZ_FN PzWide pz_shift_up(PzWide a, int bits) {
    return pz_wide((a.hi << bits) | (a.lo >> (64 - bits)), a.lo << bits);
}

// The plane's numerator at (px, py): Z0 area + dZ1 u + dZ2 v (plane_z), its Z times area 2^PLANE_Z_FRACTION
PZ_FN PzWide pz_numerator(int64_t px, int64_t py, int64_t x0, int64_t y0, int64_t z0, int64_t x1, int64_t y1,
                          int64_t z1, int64_t x2, int64_t y2, int64_t z2) {
    int64_t ax = x1 - x0, ay = y1 - y0, bx = x2 - x0, by = y2 - y0, dx = px - x0, dy = py - y0;
    int64_t area = ax * by - ay * bx;
    int64_t u = dx * by - dy * bx, v = ax * dy - ay * dx;
    return pz_add(pz_add(pz_mul(z0, area), pz_mul(z1 - z0, u)), pz_mul(z2 - z0, v));
}

// n / d in 2^-PLANE_FIXED_BITS, rounded half to even
PZ_FN int64_t pz_fixed_of(PzWide n, int64_t d) {
    return pz_divide_round(pz_shift_up(n, PLANE_FIXED_BITS), d);
}

// The plane through the corners (as plane_z), its pixel k at the point step k + first (step and first in the corners'
// unit: the pixels' points, whole)
PZ_FN PzFixed plane_fixed(int64_t step, int64_t first, int64_t x0, int64_t y0, int64_t z0, int64_t x1, int64_t y1,
                          int64_t z1, int64_t x2, int64_t y2, int64_t z2) {
    PzFixed f;
    f.base = int64_t(0);
    f.step_x = int64_t(0);
    f.step_y = int64_t(0);
    f.on = false;
    int64_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == int64_t(0)) {
        return f;
    }
    // the numerator at pixel (0, 0), and what a pixel across and down add: step times its X and Y coefficients,
    // dZ1 (y2 - y0) - dZ2 (y1 - y0) and dZ2 (x1 - x0) - dZ1 (x2 - x0)
    PzWide n0 = pz_numerator(first, first, x0, y0, z0, x1, y1, z1, x2, y2, z2);
    PzWide nx = pz_add(pz_mul(z1 - z0, (y2 - y0) * step), pz_neg(pz_mul(z2 - z0, (y1 - y0) * step)));
    PzWide ny = pz_add(pz_mul(z2 - z0, (x1 - x0) * step), pz_neg(pz_mul(z1 - z0, (x2 - x0) * step)));
    if (area < int64_t(0)) {
        area = -area;
        n0 = pz_neg(n0);
        nx = pz_neg(nx);
        ny = pz_neg(ny);
    }
    int64_t d = area << PLANE_Z_FRACTION;
    f.base = pz_fixed_of(n0, d);
    f.step_x = pz_fixed_of(nx, d);
    f.step_y = pz_fixed_of(ny, d);
    f.on = f.base < PLANE_FIXED_BASE_MOST && f.base > -PLANE_FIXED_BASE_MOST && f.step_x < PLANE_FIXED_STEP_MOST &&
           f.step_x > -PLANE_FIXED_STEP_MOST && f.step_y < PLANE_FIXED_STEP_MOST && f.step_y > -PLANE_FIXED_STEP_MOST;
    return f;
}

// The plane's Z at pixel (kx, ky) (0 <= kx, ky < 2^14), whole, rounded half to even
PZ_FN int64_t plane_fixed_z(PzFixed f, int64_t kx, int64_t ky) {
    int64_t v = f.base + f.step_x * kx + f.step_y * ky;
    int64_t q = v >> PLANE_FIXED_BITS;
    int64_t r = v - (q << PLANE_FIXED_BITS);
    int64_t half_step = int64_t(1) << (PLANE_FIXED_BITS - 1);
    if (r > half_step || (r == half_step && (q & int64_t(1)) != int64_t(0))) {
        q += int64_t(1);
    }
    return q;
}

// plane_fixed_z for the fragment stage in 32-bit operations (the GPU's 64-bit ones are made of several, its 64-bit
// multiplies of slow ones): the same value, exactly, clamped to 0..most, from what the vertex stage hands down a
// triangle (pz_pixels): the plane's value at a pixel kept below and left of the triangle's (first), and each step in
// pieces, bits 0-15 and 16-31 (not negative) and 32 up (signed, under 2^14: a step is under 2^46). A pixel's value is
// first's plus each piece times the pixel's place from first (under 2^15: each product under 2^31, the GPU's fast
// 24-bit multiply), summed in two 32-bit words with their carries, then rounded half to even.
struct PzPixels {
    pz_u32 first_low;  // the plane's value (2^-PLANE_FIXED_BITS) at pixel (first_x, first_y): bits 0-31
    pz_u32 first_high; // ... bits 32-63
    int first_x;
    int first_y;
    pz_u32 step_x_low; // step_x: bits 0-31
    pz_u32 step_y_low; // step_y: bits 0-31
    pz_u32 steps_high; // step_x's bits 32 up (bits 0-15, signed) and step_y's (16-31)
};

#ifdef __cplusplus
// (GLSL's)
PZ_FN pz_u32 uaddCarry(pz_u32 a, pz_u32 b, pz_u32 &carry) {
    pz_u32 sum = a + b;
    carry = sum < a ? 1u : 0u;
    return sum;
}
#endif

// bits 0-15 of v as a signed number
PZ_FN int pz_signed16(pz_u32 v) {
    return int(v << 16) >> 16;
}

// From the plane (on) and the least pixel the triangle covers (its corners' least less one: a helper pixel of a quad
// may be past them; the target's are 0 up, so -1 at least)
PZ_FN PzPixels pz_pixels(PzFixed f, int least_x, int least_y) {
    PzPixels p;
    p.first_x = least_x;
    p.first_y = least_y;
    uint64_t first = uint64_t(f.base + f.step_x * int64_t(least_x) + f.step_y * int64_t(least_y));
    p.first_low = pz_u32(first & uint64_t(0xFFFFFFFFu));
    p.first_high = pz_u32(first >> 32);
    p.step_x_low = pz_u32(uint64_t(f.step_x) & uint64_t(0xFFFFFFFFu));
    p.step_y_low = pz_u32(uint64_t(f.step_y) & uint64_t(0xFFFFFFFFu));
    p.steps_high = (pz_u32(f.step_x >> 32) & pz_u32(0xFFFFu)) | (pz_u32(f.step_y >> 32) << 16);
    return p;
}

// The plane's Z at pixel (kx, ky) (from first_x and first_y up, under 2^15 from them), whole, rounded half to even,
// clamped to 0..most: plane_fixed_z's
PZ_FN pz_u32 pz_pixels_z(PzPixels p, int kx, int ky, pz_u32 most) {
    pz_u32 dx = pz_u32(kx - p.first_x) & pz_u32(0x7FFFu), dy = pz_u32(ky - p.first_y) & pz_u32(0x7FFFu);
    pz_u32 s0 = (p.step_x_low & pz_u32(0xFFFFu)) * dx + (p.step_y_low & pz_u32(0xFFFFu)) * dy;
    pz_u32 s1 = (p.step_x_low >> 16) * dx + (p.step_y_low >> 16) * dy;
    int s2 = pz_signed16(p.steps_high) * int(dx) + pz_signed16(p.steps_high >> 16) * int(dy);
    pz_u32 c0, c1, c2;
    pz_u32 low = uaddCarry(p.first_low, s0, c0);
    low = uaddCarry(low, s1 << 16, c1);
    pz_u32 high = p.first_high + c0 + c1 + (s1 >> 16) + pz_u32(s2);
    // half to even: add a half less one, and one more where the whole part is odd
    low = uaddCarry(low, pz_u32((1 << (PLANE_FIXED_BITS - 1)) - 1) + ((low >> PLANE_FIXED_BITS) & 1u), c2);
    high += c2;
    if (int(high) < 0) {
        return 0u;
    }
    if ((high >> PLANE_FIXED_BITS) != 0u) {
        return most;
    }
    pz_u32 z = (low >> PLANE_FIXED_BITS) | (high << (32 - PLANE_FIXED_BITS));
    return z < most ? z : most;
}

#endif
