// Checks port/render/shaders/plane_z.h, the exact Z of a triangle's plane at a point that the PC renderer's fragment
// stage works out (port/gs/pc/shaders/pcr_exact_z.h), compiled here from the same source as C++, against the same
// value worked out in 128-bit integers and rounded half to even:
//   - random triangles and points over the whole range it is held to (PLANE_Z_MOST, Z in 256ths to 2^40), small and
//     large, thin and wide, points inside and around them;
//   - the three corners in every order give the same Z (the property exact Z is for);
//   - halves rounded to even, built on purpose;
//   - two meshes over one plane cut into different triangles give the same Z at every point (a decal, or the two map
//     records holding one wall: the case that showed the float's error at map 063);
//   - corners with no area give PLANE_Z_NONE;
//   - plane_fixed, the plane as the fragment stage draws it (three fixed-point numbers a triangle): at pixels' points,
//     the same Z as the exact one but within 2^-12 of a half (and there a Z apart at most), the same three numbers
//     whichever corner is first and whichever of two meshes over one plane worked them out.
//
//     out/<preset>/plane_z_test     prints the failures, and exits 1 if any
#include <stdio.h>

#include <algorithm>
#include <cstdint>

#include "../port/render/shaders/plane_z.h"

typedef __int128 i128;

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

// -most..most
static int64_t random_in(int64_t most) {
    return int64_t(rng() % uint64_t(2 * most + 1)) - most;
}

// floor(n / d) for d > 0
static i128 floor_div(i128 n, i128 d) {
    i128 q = n / d;
    if ((n % d != 0) && (n < 0)) {
        q -= 1;
    }
    return q;
}

// The reference: the plane's Z in 128 bits, rounded half to even; false when no area or past what plane_z resolves
static bool reference(const int64_t c[3][3], int64_t px, int64_t py, int64_t &out) {
    i128 ax = c[1][0] - c[0][0], ay = c[1][1] - c[0][1], bx = c[2][0] - c[0][0], by = c[2][1] - c[0][1];
    i128 dx = px - c[0][0], dy = py - c[0][1];
    i128 area = ax * by - ay * bx;
    if (area == 0) {
        return false;
    }
    i128 n = i128(c[0][2]) * area + i128(c[1][2] - c[0][2]) * (dx * by - dy * bx) +
             i128(c[2][2] - c[0][2]) * (ax * dy - ay * dx);
    i128 d = area << PLANE_Z_FRACTION;
    if (d < 0) {
        d = -d;
        n = -n;
    }
    i128 q = floor_div(n, d), r = n - q * d;
    if (2 * r > d || (2 * r == d && (q & 1) != 0)) {
        q += 1;
    }
    if (q > (i128(1) << 60) || q < -(i128(1) << 60)) {
        return false;
    }
    out = int64_t(q);
    return true;
}

static int64_t plane_at(const int64_t c[3][3], int64_t px, int64_t py) {
    return plane_z(px, py, c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2], c[2][0], c[2][1], c[2][2]);
}

static int failures = 0;
static long checked = 0;

static void check(const int64_t c[3][3], int64_t px, int64_t py, const char *what) {
    int64_t want = 0;
    bool known = reference(c, px, py, want);
    int64_t got = plane_at(c, px, py);
    if (!known) {
        i128 area = i128(c[1][0] - c[0][0]) * (c[2][1] - c[0][1]) - i128(c[1][1] - c[0][1]) * (c[2][0] - c[0][0]);
        if (area == 0 && got != PLANE_Z_NONE && failures++ < 20) {
            printf("%s: no area, got %lld\n", what, (long long)got);
        }
        return; // (else a quotient past 2^60: a point far outside a thin triangle, clamped by the caller)
    }
    checked++;
    if (got != want) {
        if (failures++ < 20) {
            printf("%s: corners (%lld %lld %lld) (%lld %lld %lld) (%lld %lld %lld) at (%lld %lld): %lld, want %lld\n", what,
                   (long long)c[0][0], (long long)c[0][1], (long long)c[0][2], (long long)c[1][0], (long long)c[1][1],
                   (long long)c[1][2], (long long)c[2][0], (long long)c[2][1], (long long)c[2][2], (long long)px,
                   (long long)py, (long long)got, (long long)want);
        }
    }
    // every order of the corners
    int order[3] = {0, 1, 2};
    while (std::next_permutation(order, order + 3)) {
        int64_t p[3][3];
        for (int i = 0; i < 3; i++) {
            for (int k = 0; k < 3; k++) {
                p[i][k] = c[order[i]][k];
            }
        }
        if (plane_at(p, px, py) != want && failures++ < 20) {
            printf("%s: the corners in order %d %d %d give %lld, want %lld\n", what, order[0], order[1], order[2],
                   (long long)plane_at(p, px, py), (long long)want);
        }
    }
}

static void random_triangles() {
    const int64_t z_most = (int64_t(1) << 40) - 1;
    for (int t = 0; t < 200000; t++) {
        // sizes from a pixel to the whole range, Z from flat to the whole range
        int64_t span = int64_t(1) << (rng() % 26);
        int64_t zspan = int64_t(1) << (rng() % 41);
        int64_t cx = random_in(PLANE_Z_MOST - span), cy = random_in(PLANE_Z_MOST - span);
        int64_t z0 = int64_t(rng() % uint64_t(z_most + 1));
        int64_t c[3][3];
        for (int i = 0; i < 3; i++) {
            c[i][0] = cx + random_in(span);
            c[i][1] = cy + random_in(span);
            c[i][2] = std::clamp<int64_t>(z0 + random_in(zspan), 0, z_most);
        }
        for (int k = 0; k < 4; k++) {
            // a point near the triangle (inside or a little out)
            int64_t px = cx + random_in(span + span / 8 + 1), py = cy + random_in(span + span / 8 + 1);
            px = std::clamp<int64_t>(px, -PLANE_Z_MOST, PLANE_Z_MOST);
            py = std::clamp<int64_t>(py, -PLANE_Z_MOST, PLANE_Z_MOST);
            check(c, px, py, "random");
        }
    }
}

// Z rising by a half Z (128 256ths) a step across: every other point a half, rounded to the even one
static void halves() {
    for (int t = 0; t < 2000; t++) {
        int64_t z0 = int64_t(rng() % (uint64_t(1) << 39)) << 8;
        int64_t c[3][3] = {{0, 0, z0}, {16, 0, z0 + 16 * 128}, {0, 16, z0}};
        for (int64_t x = 0; x <= 16; x++) {
            int64_t want = 0;
            reference(c, x, 0, want);
            int64_t exact2 = 2 * (z0 >> 8) + x; // (twice the exact Z: odd is a half)
            int64_t even = (exact2 & 1) == 0 ? exact2 / 2 : ((exact2 / 2) % 2 == 0 ? exact2 / 2 : exact2 / 2 + 1);
            if (want != even && failures++ < 20) {
                printf("halves: the reference rounds %lld/2 to %lld\n", (long long)exact2, (long long)want);
            }
            check(c, x, 0, "halves");
        }
    }
}

// One plane cut into triangles two ways (a quad split on either diagonal, and a fan from a point inside): every point
// covered by both gives the same Z
static void two_meshes() {
    for (int t = 0; t < 20000; t++) {
        int64_t span = int64_t(1) << (4 + rng() % 20);
        int64_t x0 = random_in(PLANE_Z_MOST - span), y0 = random_in(PLANE_Z_MOST - span);
        // a plane with whole 256ths at the quad's corners: Z = a + b x + c y (b, c in 256ths a unit)
        int64_t a = int64_t(rng() % (uint64_t(1) << 38)), b = random_in(1 << 12), cc = random_in(1 << 12);
        auto zof = [&](int64_t x, int64_t y) { return a + b * (x - x0) + cc * (y - y0); };
        int64_t q[4][2] = {{x0, y0}, {x0 + span, y0}, {x0 + span, y0 + span}, {x0, y0 + span}};
        bool on_plane = true; // (every corner's Z within 0..2^40: else not one plane once clamped)
        for (const auto &corner : q) {
            int64_t z = zof(corner[0], corner[1]);
            on_plane = on_plane && z >= 0 && z < (int64_t(1) << 40);
        }
        if (!on_plane) {
            continue;
        }
        int64_t first[3][3] = {{q[0][0], q[0][1], zof(q[0][0], q[0][1])},
                               {q[1][0], q[1][1], zof(q[1][0], q[1][1])},
                               {q[2][0], q[2][1], zof(q[2][0], q[2][1])}};
        int64_t second[3][3] = {{q[1][0], q[1][1], zof(q[1][0], q[1][1])},
                                {q[2][0], q[2][1], zof(q[2][0], q[2][1])},
                                {q[3][0], q[3][1], zof(q[3][0], q[3][1])}};
        // any point of the quad: both triangles lie on the one plane, so each gives its Z (inside it or not)
        for (int k = 0; k < 8; k++) {
            int64_t px = x0 + int64_t(rng() % uint64_t(span + 1)), py = y0 + int64_t(rng() % uint64_t(span + 1));
            int64_t za = plane_at(first, px, py), zb = plane_at(second, px, py);
            checked++;
            if (za != zb && failures++ < 20) {
                printf("two meshes: (%lld %lld) %lld against %lld\n", (long long)px, (long long)py, (long long)za,
                       (long long)zb);
            }
        }
    }
}

static void no_area() {
    int64_t line[3][3] = {{0, 0, 100}, {16, 16, 200}, {32, 32, 300}};
    if (plane_at(line, 5, 9) != PLANE_Z_NONE && failures++ < 20) {
        printf("no area: a line's corners gave a Z\n");
    }
    int64_t point[3][3] = {{7, 7, 1}, {7, 7, 2}, {7, 7, 3}};
    if (plane_at(point, 7, 7) != PLANE_Z_NONE && failures++ < 20) {
        printf("no area: a point's corners gave a Z\n");
    }
}

// The exact Z at (px, py) as reference does, and whether it is within 2^-12 of a Z of a half (where plane_fixed may round
// the other way)
static bool reference_near_half(const int64_t c[3][3], int64_t px, int64_t py) {
    i128 ax = c[1][0] - c[0][0], ay = c[1][1] - c[0][1], bx = c[2][0] - c[0][0], by = c[2][1] - c[0][1];
    i128 dx = px - c[0][0], dy = py - c[0][1];
    i128 area = ax * by - ay * bx;
    i128 n = i128(c[0][2]) * area + i128(c[1][2] - c[0][2]) * (dx * by - dy * bx) +
             i128(c[2][2] - c[0][2]) * (ax * dy - ay * dx);
    i128 d = area << PLANE_Z_FRACTION;
    if (d < 0) {
        d = -d;
        n = -n;
    }
    i128 r = n - floor_div(n, d) * d;
    i128 off = 2 * r - d;
    if (off < 0) {
        off = -off;
    }
    return off <= (d >> 11);
}

static PzFixed fixed_of(const int64_t c[3][3], int64_t step, int64_t first) {
    return plane_fixed(step, first, c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2], c[2][0], c[2][1], c[2][2]);
}

static bool same_fixed(PzFixed a, PzFixed b) {
    return a.on == b.on && (!a.on || (a.base == b.base && a.step_x == b.step_x && a.step_y == b.step_y));
}

static long fixed_off = 0, fixed_near_half = 0, quick_told = 0, quick_not_told = 0;
static long depth_checked = 0, depth_quick = 0;

// The depth rule's Z with its fraction (pz_pixels_depth, pz_pixels_depth_quick) at pixel (kx, ky) against the plane's
// fixed-point value, half a Z added, clamped to 0..most: within the float's rounding of it (2^-23 of it, and 2^-20
// of a Z); the quick way within its sum's error besides (pz_pixels_quick's bound, doubled)
static void check_depth(PzFixed f, PzPixels p, int64_t kx, int64_t ky, uint32_t most) {
    long double value = (long double)(f.base + f.step_x * kx + f.step_y * ky) / 67108864.0L + 0.5L;
    value = std::clamp<long double>(value, 0.0L, (long double)most);
    long double slack = 1.0L / 1048576.0L + value / 8388608.0L;
    float depth = pz_pixels_depth(p, int(kx), int(ky), most);
    depth_checked++;
    if (std::fabs((long double)depth - value) > slack && failures++ < 20) {
        printf("depth: pz_pixels_depth %.6f, want %.6Lf (most %u)\n", double(depth), value, most);
    }
    float quick = 0.0f;
    if (!pz_pixels_depth_quick(p, float(kx), float(ky), most, quick)) {
        return;
    }
    depth_quick++;
    long double a = std::fabs((long double)p.step_x_z * (long double)(kx - p.first_x));
    long double b = std::fabs((long double)p.step_y_z * (long double)(ky - p.first_y));
    long double quick_slack = slack + (a + b + 1.0L) / 524288.0L + value / 4194304.0L;
    if (std::fabs((long double)quick - value) > quick_slack && failures++ < 20) {
        printf("depth: pz_pixels_depth_quick %.6f, want %.6Lf (most %u)\n", double(quick), value, most);
    }
}

static void fixed_planes() {
    const int64_t z_most = (int64_t(1) << 40) - 1;
    for (int t = 0; t < 200000; t++) {
        bool msaa = (rng() & 1) != 0;
        int64_t step = msaa ? 32 : 16, first = msaa ? 16 : 0;
        // corners in the pixels' unit, about the target (16384 pixels across at most)
        int64_t span = int64_t(1) << (rng() % 20);
        int64_t cx = int64_t(rng() % uint64_t(step << 14)), cy = int64_t(rng() % uint64_t(step << 14));
        int64_t zspan = int64_t(1) << (rng() % 41);
        int64_t z0 = int64_t(rng() % uint64_t(z_most + 1));
        int64_t c[3][3];
        for (int i = 0; i < 3; i++) {
            c[i][0] = cx + random_in(span);
            c[i][1] = cy + random_in(span);
            c[i][2] = std::clamp<int64_t>(z0 + random_in(zspan), 0, z_most);
        }
        PzFixed f = fixed_of(c, step, first);
        int order[3] = {0, 1, 2};
        while (std::next_permutation(order, order + 3)) {
            int64_t p[3][3];
            for (int i = 0; i < 3; i++) {
                for (int k = 0; k < 3; k++) {
                    p[i][k] = c[order[i]][k];
                }
            }
            if (!same_fixed(fixed_of(p, step, first), f) && failures++ < 20) {
                printf("fixed: the corners in order %d %d %d give another plane\n", order[0], order[1], order[2]);
            }
        }
        if (!f.on) {
            fixed_off++;
            continue;
        }
        for (int k = 0; k < 4; k++) {
            int64_t kx = std::clamp<int64_t>((cx + random_in(span + 1)) / step, 0, (1 << 14) - 1);
            int64_t ky = std::clamp<int64_t>((cy + random_in(span + 1)) / step, 0, (1 << 14) - 1);
            int64_t px = step * kx + first, py = step * ky + first;
            int64_t want = 0;
            if (!reference(c, px, py, want)) {
                continue;
            }
            int64_t got = plane_fixed_z(f, kx, ky);
            checked++;
            // (the fragment stage's way: from a least pixel below the triangle's, -1 at least)
            int least_x = int(std::max<int64_t>(-1, (std::min({c[0][0], c[1][0], c[2][0]}) - first) / step - 1));
            int least_y = int(std::max<int64_t>(-1, (std::min({c[0][1], c[1][1], c[2][1]}) - first) / step - 1));
            least_x = std::min<int>(least_x, int(kx));
            least_y = std::min<int>(least_y, int(ky));
            for (uint32_t most : {0xFFFFFFu, 0xFFFFFFFFu, 0xFFFFu}) {
                PzPixels pixels_of = pz_pixels(f, least_x, least_y);
                pz_u32 fine = 0;
                int64_t pixels = pz_pixels_z(pixels_of, int(kx), int(ky), most, fine);
                // (the quick way: where it can tell, the same Z and fine part)
                pz_u32 quick = 0, quick_fine = 0;
                if (pz_pixels_quick(pixels_of, float(kx), float(ky), most, quick, quick_fine)) {
                    quick_told++;
                    if ((int64_t(quick) != pixels || quick_fine != fine) && failures++ < 20) {
                        printf("fixed: pz_pixels_quick %u fine %u, pz_pixels_z %lld fine %u (most %u)\n", quick,
                               quick_fine, (long long)pixels, fine, most);
                    }
                } else {
                    quick_not_told++;
                }
                if (pixels != std::clamp<int64_t>(got, 0, most) && failures++ < 20) {
                    printf("fixed: pz_pixels_z %lld, plane_fixed_z %lld (most %u)\n", (long long)pixels, (long long)got,
                           most);
                }
                check_depth(f, pixels_of, kx, ky, most);
            }
            if (reference_near_half(c, px, py)) {
                fixed_near_half++;
                if ((got - want > 1 || want - got > 1) && failures++ < 20) {
                    printf("fixed: near a half at pixel (%lld %lld): %lld, want %lld\n", (long long)kx, (long long)ky,
                           (long long)got, (long long)want);
                }
            } else if (got != want && failures++ < 20) {
                printf("fixed: corners (%lld %lld %lld) (%lld %lld %lld) (%lld %lld %lld) at pixel (%lld %lld): %lld, "
                       "want %lld\n",
                       (long long)c[0][0], (long long)c[0][1], (long long)c[0][2], (long long)c[1][0], (long long)c[1][1],
                       (long long)c[1][2], (long long)c[2][0], (long long)c[2][1], (long long)c[2][2], (long long)kx,
                       (long long)ky, (long long)got, (long long)want);
            }
        }
    }
}

// The fine part pz_pixels_z gave at pixel (kx, ky) (whole Z z) against the plane's fixed-point value worked out here:
// below it by less than a step of the fine part, the steps it keeps at z (pz_fine_bits), none where z is clamped; the
// depth's float holds z and it exactly (z under 2^24), and z with the decal's top fine part (pz_fine_top), at least it
static void check_fine(PzFixed f, int kx, int ky, pz_u32 most, pz_u32 z, pz_u32 fine) {
    __int128 v = __int128(f.base) + __int128(f.step_x) * kx + __int128(f.step_y) * ky +
                 (__int128(1) << (PLANE_FIXED_BITS - 1)); // (2^-PLANE_FIXED_BITS, half a Z added)
    __int128 whole = v >> PLANE_FIXED_BITS;
    if (whole < 0 || whole >= __int128(most)) {
        if (fine != 0 && failures++ < 20) {
            printf("fine: %u where Z is clamped to %u\n", fine, z);
        }
        return;
    }
    int bits = pz_fine_bits(z);
    __int128 kept = (v - (whole << PLANE_FIXED_BITS)) >> (PLANE_FIXED_BITS - bits); // (the fraction's steps)
    pz_u32 want = pz_u32(kept) << (PLANE_Z_FINE_BITS - bits);
    pz_u32 top = pz_fine_top(z);
    // (from Z 2^24 up the float holds no whole Z exactly either: a 32-bit Z buffer's far end, as before fine parts)
    double depth = double(float(z) + float(fine) * (1.0f / 256.0f));
    double top_depth = double(float(z) + float(top) * (1.0f / 256.0f));
    bool held = z >= (1u << 24) || (depth == double(z) + fine / 256.0 && top_depth == double(z) + top / 256.0);
    if ((fine != want || !held || top < fine || top >= 256u) &&
        failures++ < 20) {
        printf("fine: Z %u fine %u (want %u, %d bits), top %u\n", z, fine, want, bits, top);
    }
}

// pz_pixels_quick against pz_pixels_z on planes made directly (not from corners), many of them with first's value a
// hair from a whole Z (where the quick way must say it cannot tell), steps of every size
static void quick_planes() {
    for (int t = 0; t < 300000; t++) {
        PzFixed f;
        f.on = true;
        int64_t z = int64_t(rng() % (uint64_t(1) << 25));
        int64_t near = random_in(int64_t(1) << (rng() % 27));
        f.base = (z << PLANE_FIXED_BITS) + near - (int64_t(1) << (PLANE_FIXED_BITS - 1));
        f.step_x = random_in(int64_t(1) << (rng() % 47));
        f.step_y = random_in(int64_t(1) << (rng() % 47));
        int least_x = int(rng() % 16384) - 1, least_y = int(rng() % 16384) - 1;
        f.base -= f.step_x * least_x + f.step_y * least_y; // (so that first's value is the one made above)
        PzPixels p = pz_pixels(f, least_x, least_y);
        for (int k = 0; k < 8; k++) {
            int reach = 1 << (rng() % 15);
            int kx = std::min(least_x + int(rng() % uint64_t(reach)), 32766);
            int ky = std::min(least_y + int(rng() % uint64_t(reach)), 32766);
            for (uint32_t most : {0xFFFFFFu, 0xFFFFFFFFu}) {
                pz_u32 fine = 0, quick = 0, quick_fine = 0;
                pz_u32 exact = pz_pixels_z(p, kx, ky, most, fine);
                checked++;
                check_fine(f, kx, ky, most, exact, fine);
                if (!pz_pixels_quick(p, float(kx), float(ky), most, quick, quick_fine)) {
                    quick_not_told++;
                    continue;
                }
                quick_told++;
                if ((quick != exact || quick_fine != fine) && failures++ < 20) {
                    printf("quick: base %lld steps %lld %lld first (%d %d) pixel (%d %d): %u fine %u, exact %u fine %u\n",
                           (long long)f.base, (long long)f.step_x, (long long)f.step_y, least_x, least_y, kx, ky, quick,
                           quick_fine, exact, fine);
                }
            }
        }
    }
}

// two_meshes for plane_fixed: both triangles of a quad on one plane give the same three numbers
// The depth rule's Z (pcr_exact_z.h's pcr_plane_depth: the quick way, else the exact) of one plane drawn by two
// triangles, each from its own least pixel: within the rule's least tolerance of each other, 1/512 of a Z (the corners'
// Z step's half) and the float's step (2^-23 of the depth), so that they tie
static long depth_ties = 0;
static float shader_depth(PzPixels p, int64_t kx, int64_t ky, uint32_t most) {
    float depth = 0.0f;
    if (!pz_pixels_depth_quick(p, float(kx), float(ky), most, depth)) {
        depth = pz_pixels_depth(p, int(kx), int(ky), most);
    }
    return depth;
}

static PzPixels pixels_from_least(PzFixed f, const int64_t c[3][3], int64_t step, int64_t first) {
    int least_x = int(std::max<int64_t>(-1, (std::min({c[0][0], c[1][0], c[2][0]}) - first) / step - 1));
    int least_y = int(std::max<int64_t>(-1, (std::min({c[0][1], c[1][1], c[2][1]}) - first) / step - 1));
    return pz_pixels(f, least_x, least_y);
}

static void two_meshes_tie(PzFixed f, const int64_t a[3][3], const int64_t b[3][3], int64_t step, int64_t first) {
    PzPixels pa = pixels_from_least(f, a, step, first), pb = pixels_from_least(f, b, step, first);
    int64_t x_lo = std::max({a[0][0], a[1][0], a[2][0]}) / step, y_lo = std::max({a[0][1], a[1][1], a[2][1]}) / step;
    for (int k = 0; k < 4; k++) {
        // (a pixel both triangles' least pixels are below: inside their shared bounding box)
        int64_t kx = std::min<int64_t>(x_lo - int64_t(rng() % 4), (1 << 14) - 1);
        int64_t ky = std::min<int64_t>(y_lo - int64_t(rng() % 4), (1 << 14) - 1);
        if (kx < std::max(pa.first_x, pb.first_x) || ky < std::max(pa.first_y, pb.first_y)) {
            continue;
        }
        for (uint32_t most : {0xFFFFFFu, 0xFFFFFFFFu}) {
            float da = shader_depth(pa, kx, ky, most), db = shader_depth(pb, kx, ky, most);
            float tie = 1.0f / 512.0f + std::max(da, db) / 8388608.0f;
            depth_ties++;
            if (std::fabs(da - db) > tie && failures++ < 20) {
                printf("two meshes' depth: %.6f and %.6f at pixel (%lld %lld), apart by more than %.6f\n", double(da),
                       double(db), (long long)kx, (long long)ky, double(tie));
            }
        }
    }
}

static void fixed_two_meshes() {
    for (int t = 0; t < 20000; t++) {
        bool msaa = (rng() & 1) != 0;
        int64_t step = msaa ? 32 : 16, first = msaa ? 16 : 0;
        int64_t span = int64_t(1) << (4 + rng() % 16);
        int64_t x0 = int64_t(rng() % uint64_t(step << 14)), y0 = int64_t(rng() % uint64_t(step << 14));
        int64_t a = int64_t(rng() % (uint64_t(1) << 38)), b = random_in(1 << 12), cc = random_in(1 << 12);
        auto zof = [&](int64_t x, int64_t y) { return a + b * (x - x0) + cc * (y - y0); };
        int64_t q[4][2] = {{x0, y0}, {x0 + span, y0}, {x0 + span, y0 + span}, {x0, y0 + span}};
        bool on_plane = true;
        for (const auto &corner : q) {
            int64_t z = zof(corner[0], corner[1]);
            on_plane = on_plane && z >= 0 && z < (int64_t(1) << 40);
        }
        if (!on_plane) {
            continue;
        }
        int64_t first_tri[3][3] = {{q[0][0], q[0][1], zof(q[0][0], q[0][1])},
                                   {q[1][0], q[1][1], zof(q[1][0], q[1][1])},
                                   {q[2][0], q[2][1], zof(q[2][0], q[2][1])}};
        int64_t second_tri[3][3] = {{q[1][0], q[1][1], zof(q[1][0], q[1][1])},
                                    {q[2][0], q[2][1], zof(q[2][0], q[2][1])},
                                    {q[3][0], q[3][1], zof(q[3][0], q[3][1])}};
        checked++;
        PzFixed fa = fixed_of(first_tri, step, first), fb = fixed_of(second_tri, step, first);
        if (!same_fixed(fa, fb) && failures++ < 20) {
            printf("fixed two meshes: one plane gave two (quad at %lld %lld, %lld across)\n", (long long)x0,
                   (long long)y0, (long long)span);
        }
        if (fa.on) {
            two_meshes_tie(fa, first_tri, second_tri, step, first);
        }
    }
}

// pz_fine_bits: the steps the depth holds beside each whole Z
static void fine_bits() {
    const pz_u32 zs[] = {0u, 1u, 0xFFu, 0xFFFFu, 0x10000u, 0x7FFFFFu, 0x800000u, 0xFFFFFFu, 0xFFFFFFFFu};
    const int want[] = {8, 8, 8, 8, 7, 1, 0, 0, 0};
    for (int i = 0; i < 9; i++) {
        checked++;
        if (pz_fine_bits(zs[i]) != want[i] && failures++ < 20) {
            printf("fine bits: Z %u gave %d, want %d\n", zs[i], pz_fine_bits(zs[i]), want[i]);
        }
    }
}

int main() {
    fine_bits();
    random_triangles();
    halves();
    two_meshes();
    no_area();
    fixed_planes();
    fixed_two_meshes();
    quick_planes();
    printf("plane_z: %ld checked, %d failures (plane_fixed: %ld planes off, %ld points near a half; quick: %ld told, %ld "
           "not; depth: %ld checked, %ld the quick way, %ld ties of two meshes)\n",
           checked, failures, fixed_off, fixed_near_half, quick_told, quick_not_told, depth_checked, depth_quick,
           depth_ties);
    return failures != 0;
}
