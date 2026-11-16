// A triangle's Z at a point, exactly: the plane through its three corners, rounded to a whole Z, half to even; and
// for the fragment stage the fraction of a Z below it that the depth keeps (pz_fine_bits).
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
#define PZ_OUT(type) type &
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
// (GLSL's findMSB + 1: the bits v takes, 0 for 0)
PZ_FN int pz_bit_length(pz_u32 v) {
    return v == 0u ? 0 : 32 - __builtin_clz(v);
}
#else
#define PZ_FN
#define PZ_OUT(type) out type
#define pz_floor floor
#define pz_clamp clamp
#define pz_abs abs
#define pz_u32 uint
#define pz_bit_length(v) (findMSB(v) + 1)
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
// (64-bit integers, kx and ky under 2^14), rounded half up (any rule both meshes share ties them; half up is the
// fragment stage's cheapest): within 2^-12 of a Z of the plane's exact Z, so it rounds as the exact one does but where
// that is within 2^-12 of a half.
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

// The plane's Z at pixel (kx, ky) (0 <= kx, ky < 2^14), whole, rounded half up
PZ_FN int64_t plane_fixed_z(PzFixed f, int64_t kx, int64_t ky) {
    int64_t v = f.base + f.step_x * kx + f.step_y * ky;
    return (v + (int64_t(1) << (PLANE_FIXED_BITS - 1))) >> PLANE_FIXED_BITS;
}

// plane_fixed_z for the fragment stage, from what is handed down a triangle (pz_pixels): the plane's value at a pixel
// kept below and left of the triangle's (first), half a Z added (the rounding), and its steps. Two ways to the same
// value, clamped to 0..most:
// - pz_pixels_z, exactly: first's value plus each step, in pieces (bits 0-15 and 16-31 not negative, 32 up signed and
//   under 2^14: a step is under 2^46), times the pixel's place from first (under 2^15: each product under 2^31, the
//   GPU's fast 24-bit multiply), summed in two 32-bit words with their carries.
// - pz_pixels_quick, in floats: the value less first's whole Z, first's fraction plus each step (as a float, in Z) times
//   the place, with a bound on its error; where that is nearer a whole Z than the bound, it says it cannot tell (the
//   caller then takes pz_pixels_z). A few full-rate operations where the exact sum is a long chain of dependent ones:
//   at map 063 the exact sum at every pixel took the GPU about 0.19 ms a picture, 8% of it (2026-11-15). Where it can
//   tell, its Z is pz_pixels_z's exactly (plane_z_test checks it), so two meshes on one plane still tie whichever way
//   each pixel went.
// The fine part of a pixel's Z: below its whole Z (the plane's value with half a Z added, its whole part), the
// fraction of a Z in 256ths, of which the depth keeps the first pz_fine_bits (the rest 0). The depth (a 32-bit float,
// Z 2^-24) holds whole Z z and bits of fraction exactly while z 2^bits is under 2^24: all 8 bits to Z 65535, none
// from Z 2^23 up. Worked out from the plane's fixed-point sum, it is the same for two meshes on one plane, as the
// whole Z is: they still tie. (Far from the eye the game's Z is coarse: a log's strips, its front and the one behind
// it, round to one whole Z there, and on the GS the strip drawn last shows through the front in bands that move with
// the eye. The PC renderer sorts them by the fine part: map 000's rope ladder, 2026-11-15.)
#define PLANE_Z_FINE_BITS 8

PZ_FN int pz_fine_bits(pz_u32 z) {
    int bits = 24 - pz_bit_length(z);
    return bits < 0 ? 0 : bits > PLANE_Z_FINE_BITS ? PLANE_Z_FINE_BITS : bits;
}

// The fine part (256ths) at whole Z z from the fraction's 256ths: the first pz_fine_bits of them
PZ_FN pz_u32 pz_fine_of(pz_u32 z, pz_u32 fraction256) {
    return fraction256 & ((pz_u32(0xFFu) << (PLANE_Z_FINE_BITS - pz_fine_bits(z))) & pz_u32(0xFFu));
}

// The greatest fine part at whole Z z: a decal's (pcr_prim.h), so that it ties with every fine part of z, as on the GS
PZ_FN pz_u32 pz_fine_top(pz_u32 z) {
    return pz_u32(256) - (pz_u32(256) >> pz_fine_bits(z));
}

struct PzPixels {
    pz_u32 first_low;  // the plane's value (2^-PLANE_FIXED_BITS) at pixel (first_x, first_y), half a Z added: bits 0-31
    pz_u32 first_high; // ... bits 32-63
    int first_x;
    int first_y;
    pz_u32 step_x_low; // step_x: bits 0-31
    pz_u32 step_y_low; // step_y: bits 0-31
    pz_u32 steps_high; // step_x's bits 32 up (bits 0-15, signed) and step_y's (16-31)
    float step_x_z;    // step_x in Z (within 2^-23 of it)
    float step_y_z;    // step_y in Z
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
    uint64_t first = uint64_t(f.base + f.step_x * int64_t(least_x) + f.step_y * int64_t(least_y) +
                              (int64_t(1) << (PLANE_FIXED_BITS - 1))); // (half a Z: rounded half up)
    p.first_low = pz_u32(first & uint64_t(0xFFFFFFFFu));
    p.first_high = pz_u32(first >> 32);
    p.step_x_low = pz_u32(uint64_t(f.step_x) & uint64_t(0xFFFFFFFFu));
    p.step_y_low = pz_u32(uint64_t(f.step_y) & uint64_t(0xFFFFFFFFu));
    p.steps_high = (pz_u32(f.step_x >> 32) & pz_u32(0xFFFFu)) | (pz_u32(f.step_y >> 32) << 16);
    p.step_x_z = float(f.step_x) * (1.0f / 67108864.0f); // (2^-PLANE_FIXED_BITS: a power of two, exact)
    p.step_y_z = float(f.step_y) * (1.0f / 67108864.0f);
    return p;
}

// The plane's Z at pixel (kx, ky) (from first_x and first_y up, under 2^15 from them), whole, rounded half up,
// clamped to 0..most: plane_fixed_z's; and its fine part (none where clamped)
// The plane's value at pixel (kx, ky), half a Z added, in 2^-PLANE_FIXED_BITS of a Z: bits 0-31 (low) and 32-63 (high)
PZ_FN void pz_pixels_sum(PzPixels p, int kx, int ky, PZ_OUT(pz_u32) low, PZ_OUT(pz_u32) high) {
    pz_u32 dx = pz_u32(kx - p.first_x) & pz_u32(0x7FFFu), dy = pz_u32(ky - p.first_y) & pz_u32(0x7FFFu);
    pz_u32 s0 = (p.step_x_low & pz_u32(0xFFFFu)) * dx + (p.step_y_low & pz_u32(0xFFFFu)) * dy;
    pz_u32 s1 = (p.step_x_low >> 16) * dx + (p.step_y_low >> 16) * dy;
    int s2 = pz_signed16(p.steps_high) * int(dx) + pz_signed16(p.steps_high >> 16) * int(dy);
    pz_u32 c0, c1;
    low = uaddCarry(p.first_low, s0, c0);
    low = uaddCarry(low, s1 << 16, c1);
    high = p.first_high + c0 + c1 + (s1 >> 16) + pz_u32(s2);
}

PZ_FN pz_u32 pz_pixels_z(PzPixels p, int kx, int ky, pz_u32 most, PZ_OUT(pz_u32) fine) {
    pz_u32 low, high;
    pz_pixels_sum(p, kx, ky, low, high);
    fine = 0u;
    if (int(high) < 0) {
        return 0u;
    }
    if ((high >> PLANE_FIXED_BITS) != 0u) {
        return most;
    }
    pz_u32 z = (low >> PLANE_FIXED_BITS) | (high << (32 - PLANE_FIXED_BITS));
    if (z >= most) {
        return most;
    }
    fine = pz_fine_of(z, (low >> (PLANE_FIXED_BITS - PLANE_Z_FINE_BITS)) & pz_u32(0xFFu));
    return z;
}

// pz_pixels_z at pixel (kx, ky) given as floats (whole), in floats: true and z where it can tell (see above).
// Its error: first's fraction as a float within 2^-24; each step within 2^-23 of its value, and each product and sum
// rounded once (2^-24 of it), so within (|a| + |b| + 1) 2^-21 in all (a and b the steps times the place): within the
// bound e, twice that. It can tell where the sum is more than e from a whole number (rounding the exact value to a
// whole Z there is rounding the sum) and first's whole Z is under 2^30 (with the sum, whole in 32-bit integers). Its
// fine part likewise: where the sum is more than e from a step of the fine part's.
PZ_FN bool pz_pixels_quick(PzPixels p, float kx, float ky, pz_u32 most, PZ_OUT(pz_u32) z, PZ_OUT(pz_u32) fine) {
    float dx = kx - float(p.first_x), dy = ky - float(p.first_y);
    float a = p.step_x_z * dx, b = p.step_y_z * dy;
    float fraction = float(p.first_low & pz_u32(0x3FFFFFFu)) * (1.0f / 67108864.0f);
    float r = (fraction + a) + b;
    float e = (pz_abs(a) + pz_abs(b) + 1.0f) * (1.0f / 1048576.0f); // (2^-20)
    float n = pz_floor(r);
    float part = r - n; // (exact)
    z = 0u;
    fine = 0u;
    if (!(part > e && part < 1.0f - e && dx >= 0.0f && dy >= 0.0f && dx < 32768.0f && dy < 32768.0f &&
          p.first_high < pz_u32(0x1000000u))) {
        return false;
    }
    int whole = int((p.first_high << (32 - PLANE_FIXED_BITS)) | (p.first_low >> PLANE_FIXED_BITS)) + int(n);
    if (whole < 0) {
        return true;
    }
    pz_u32 w = pz_u32(whole);
    if (w >= most) {
        z = most;
        return true;
    }
    int bits = pz_fine_bits(w);
    float steps = float(1 << bits);
    float scaled = part * steps; // (exact: a power of two)
    float step = pz_floor(scaled);
    if (!(scaled - step > e * steps && step + 1.0f - scaled > e * steps)) {
        return false;
    }
    z = w;
    fine = pz_u32(step) << (PLANE_Z_FINE_BITS - bits);
    return true;
}

// The depth rule's Z (port/gs/pc/shaders/pcr_prim.h's PCR_DEPTH_RULE): the plane's value at pixel (kx, ky) with its
// fraction, not rounded (half a Z added, as pz_pixels_z's sum has it), clamped to 0..most, as a float: whole Z exactly
// below 2^24 and as much of the fraction as the float holds beside it. Two meshes on one plane get it within
// 2^-12 of a Z of each other (the sum's rounding) plus the float's, which the rule's tolerance covers.
// - pz_pixels_depth, from the exact sum;
// - pz_pixels_depth_quick, in floats (pz_pixels_quick's sum: within its bound e of the exact), where the pixel's place
//   from first and first's whole Z fit (false: the caller takes pz_pixels_depth). It need not tell which whole Z the
//   value rounds to, as pz_pixels_quick must: the depth runs on across it.
PZ_FN float pz_pixels_depth(PzPixels p, int kx, int ky, pz_u32 most) {
    pz_u32 low, high;
    pz_pixels_sum(p, kx, ky, low, high);
    if (int(high) < 0) {
        return 0.0f;
    }
    if ((high >> PLANE_FIXED_BITS) != 0u) {
        return float(most);
    }
    pz_u32 z = (low >> PLANE_FIXED_BITS) | (high << (32 - PLANE_FIXED_BITS));
    if (z >= most) {
        return float(most);
    }
    return float(z) + float(low & pz_u32(0x3FFFFFFu)) * (1.0f / 67108864.0f);
}

PZ_FN bool pz_pixels_depth_quick(PzPixels p, float kx, float ky, pz_u32 most, PZ_OUT(float) depth) {
    float dx = kx - float(p.first_x), dy = ky - float(p.first_y);
    depth = 0.0f;
    if (!(dx >= 0.0f && dy >= 0.0f && dx < 32768.0f && dy < 32768.0f && p.first_high < pz_u32(0x1000000u))) {
        return false;
    }
    float fraction = float(p.first_low & pz_u32(0x3FFFFFFu)) * (1.0f / 67108864.0f);
    float r = (fraction + p.step_x_z * dx) + p.step_y_z * dy;
    float whole = float((p.first_high << (32 - PLANE_FIXED_BITS)) | (p.first_low >> PLANE_FIXED_BITS));
    depth = pz_clamp(whole + r, 0.0f, float(most));
    return true;
}

#endif
