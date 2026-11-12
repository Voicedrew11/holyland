// A native draw's record for program 0x000 (vu1_record.hpp): VU1's set-up in its arithmetic
#include "vu1_record.hpp"

#include <string.h>

#include <algorithm>
#include <vector>

extern "C" {
#include "port/vu_float.h"
}

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define VU1_RECORD_FAST 1
#else
#define VU1_RECORD_FAST 0
#endif

bool vu1_record_fast = true;

namespace {

// VU1's arithmetic (port/include/port/vu_float.h: the interpreter's, bit for bit), on floats
float vu_mul(float a, float b) {
    return vu_float_from_bits(vu_float_mul(vu_float_bits(a), vu_float_bits(b)));
}
float vu_add(float a, float b) {
    return vu_float_from_bits(vu_float_add(vu_float_bits(a), vu_float_bits(b)));
}
float vu_sub(float a, float b) {
    return vu_float_from_bits(vu_float_sub(vu_float_bits(a), vu_float_bits(b)));
}

// The arithmetic in two forms (as port/platform/vu1_hle.c has them; its top comment): Exact, VU1's above, and Fast,
// plain SSE floats in FastMode. In FastMode (toward zero, denormals read as zero, results flushed to zero) an SSE
// operation is VU1's whenever no operand has exponent 255 (a number to the VU, an infinity or NaN to SSE) and it does
// not overflow (VU1's results past FLT_MAX are numbers, SSE's FLT_MAX or infinity), divide by zero or raise invalid:
// the operands are checked before, MXCSR's flags after (underflow too, to be safe), and when either fails the work is
// done again the exact way. This file is built with -frounding-math -ffp-exception-behavior=strict, so the compiler
// neither assumes the rounding mode nor moves an operation across the MXCSR reads and writes.
struct Exact {
    static float mul(float a, float b) {
        return vu_mul(a, b);
    }
    static float add(float a, float b) {
        return vu_add(a, b);
    }
    static float sub(float a, float b) {
        return vu_sub(a, b);
    }
};

struct Fast {
    static float mul(float a, float b) {
        return a * b;
    }
    static float add(float a, float b) {
        return a + b;
    }
    static float sub(float a, float b) {
        return a - b;
    }
};

// an exponent of 255: not for the fast way
bool huge(uint32_t bits) {
    return (bits & 0x7F800000u) == 0x7F800000u;
}

bool any_huge(const float *f, int n) {
    bool any = false;
    for (int i = 0; i < n; i++) {
        any |= huge(vu_float_bits(f[i]));
    }
    return any;
}

#if VU1_RECORD_FAST
// MXCSR for the fast way, set at the start (flags clear) and put back at the end: every exception masked, toward
// zero, FTZ, DAZ (port/platform.h's PORT_MXCSR_EE)
class FastMode {
public:
    static constexpr unsigned CSR = 0xFFC0u;
    static constexpr unsigned TROUBLE = 0x001Du; // invalid, divide by zero, overflow, underflow

    FastMode() : saved_(_mm_getcsr()) {
        _mm_setcsr(CSR);
    }
    ~FastMode() {
        _mm_setcsr(saved_);
    }
    FastMode(const FastMode &) = delete;
    FastMode &operator=(const FastMode &) = delete;

    // whether every operation since the start gave VU1's result (its operands checked)
    bool clean() const {
        return (_mm_getcsr() & TROUBLE) == 0;
    }

private:
    unsigned saved_;
};
#endif

// out = m v, as MULAx ACC; MADDAy; MADDAz; MADDw (each product rounded, then the sum): m a qword a column
template <class A> void vu_transform4(float *out, const float *m, const float *v) {
    for (int r = 0; r < 4; r++) {
        float acc = A::mul(m[r], v[0]);
        acc = A::add(acc, A::mul(m[4 + r], v[1]));
        acc = A::add(acc, A::mul(m[8 + r], v[2]));
        out[r] = A::add(acc, A::mul(m[12 + r], v[3]));
    }
}

// VU1's clip judgement of a place (vu1_hle_ops.h's judge and clipped): the vertex matrix m by the place q (four words
// as VU1 memory has them: integers for 0x000's and 0x700's, floats for 0x280's, by VU1's bits either way), divided by
// w; past the guard band (2047 about 2048) in X or Y, or Z below 0
bool clipped(const int32_t *q, const float *m, bool integers) {
    uint32_t p[4];
    for (int i = 0; i < 4; i++) {
        p[i] = integers ? vu_float_bits(float(q[i])) : uint32_t(q[i]); // (ITOF0: exact for the meshes' 16 bits)
    }
    uint32_t c[4];
    for (int r = 0; r < 4; r++) {
        uint32_t acc = vu_float_mul(vu_float_bits(m[r]), p[0]);
        acc = vu_float_add(acc, vu_float_mul(vu_float_bits(m[4 + r]), p[1]));
        acc = vu_float_add(acc, vu_float_mul(vu_float_bits(m[8 + r]), p[2]));
        c[r] = vu_float_add(acc, vu_float_mul(vu_float_bits(m[12 + r]), p[3]));
    }
    uint32_t qq = vu_float_div(0x3F800000u, c[3]);
    uint32_t x = vu_float_mul(c[0], qq), y = vu_float_mul(c[1], qq), z = vu_float_mul(c[2], qq);
    uint32_t rx = vu_float_sub(x, 0x45000000u), ry = vu_float_sub(y, 0x45000000u); // (- 2048)
    return (rx & 0x7FFFFFFFu) > 0x44FFE000u || (ry & 0x7FFFFFFFu) > 0x44FFE000u || // (2047)
           ((z & 0x80000000u) && (z & 0x7FFFFFFFu) > 0x007FFFFFu);
}

#if VU1_RECORD_FAST
// clipped's maths the fast way (in FastMode; the place p as floats)
bool clipped_fast(const float *p, const float *m) {
    float c[4];
    for (int r = 0; r < 4; r++) {
        float acc = m[r] * p[0];
        acc = acc + m[4 + r] * p[1];
        acc = acc + m[8 + r] * p[2];
        c[r] = acc + m[12 + r] * p[3];
    }
    float qq = 1.0f / c[3];
    float x = c[0] * qq, y = c[1] * qq;
    uint32_t z = vu_float_bits(c[2] * qq);
    uint32_t rx = vu_float_bits(x - 2048.0f), ry = vu_float_bits(y - 2048.0f);
    return (rx & 0x7FFFFFFFu) > 0x44FFE000u || (ry & 0x7FFFFFFFu) > 0x44FFE000u ||
           ((z & 0x80000000u) && (z & 0x7FFFFFFFu) > 0x007FFFFFu);
}
#endif

// clipped of n places, the i-th at q + i * stride, each by the matrix matrix_of(place) gives: the fast way for all of
// them when it can be trusted with every one, else the exact way
template <class MatrixOf>
void clip_all(bool *clip, uint32_t n, const int32_t *q, size_t stride, bool integers, bool matrices_huge,
              MatrixOf &&matrix_of) {
#if VU1_RECORD_FAST
    if (vu1_record_fast && !matrices_huge) {
        bool done;
        {
            FastMode mode;
            bool fit = true;
            for (uint32_t i = 0; i < n; i++) {
                const int32_t *v = q + size_t(i) * stride;
                float p[4];
                for (int j = 0; j < 4; j++) {
                    if (integers) {
                        fit &= v[j] > -(1 << 24) && v[j] < (1 << 24); // (exact as a float in any rounding)
                        p[j] = float(v[j]);
                    } else {
                        fit &= !huge(uint32_t(v[j]));
                        memcpy(&p[j], &v[j], 4);
                    }
                }
                clip[i] = clipped_fast(p, matrix_of(v));
            }
            done = fit && mode.clean();
        }
        if (done) {
            return;
        }
    }
#else
    (void)matrices_huge;
#endif
    for (uint32_t i = 0; i < n; i++) {
        const int32_t *v = q + size_t(i) * stride;
        clip[i] = clipped(v, matrix_of(v), integers);
    }
}

// vu1_record_000's maths in one arithmetic: the vertex matrix, the light directions and the point lights
template <class A> void record_000_maths(Vu1Record &k, const RdDraw &d, const RdCamera &cam) {
    const float *m = &d.model[0][0];
    for (int col = 0; col < 4; col++) {
        vu_transform4<A>(&k.vertex[col * 4], &cam.world_screen[0][0], &m[col * 4]);
        vu_transform4<A>(&k.light_dir[col * 4], &d.light_dir[0][0], &m[col * 4]);
    }
    for (int l = 0; l < 4; l++) {
        // the point light into the part's space: less its translation, then by its rotation with the off-diagonal
        // signs turned (the set-up's own maths, to the sign of a 0: the translation as 0 + it, the turned signs as
        // 0 - them)
        float p[3] = {A::sub(d.point_pos[l][0], A::add(0.0f, m[12])), A::sub(d.point_pos[l][1], A::add(0.0f, m[13])),
                      A::sub(d.point_pos[l][2], A::add(0.0f, m[14]))};
        float r0[3] = {A::add(0.0f, m[0]), A::sub(0.0f, m[1]), A::sub(0.0f, m[2])};
        float r1[3] = {A::sub(0.0f, m[4]), A::add(0.0f, m[5]), A::sub(0.0f, m[6])};
        float r2[3] = {A::sub(0.0f, m[8]), A::sub(0.0f, m[9]), A::add(0.0f, m[10])};
        for (int i = 0; i < 3; i++) {
            float acc = A::mul(r0[i], p[0]);
            acc = A::add(acc, A::mul(r1[i], p[1]));
            k.point_pos[l * 4 + i] = A::add(acc, A::mul(r2[i], p[2]));
        }
    }
}

} // namespace

Vu1Record vu1_record_000(const RdDraw &d, const RdCamera &cam) {
    Vu1Record k;
    memset(&k, 0, sizeof(k));
    bool done = false;
#if VU1_RECORD_FAST
    if (vu1_record_fast && !any_huge(&cam.world_screen[0][0], 16) && !any_huge(&d.model[0][0], 16) &&
        !any_huge(&d.light_dir[0][0], 16) && !any_huge(&d.point_pos[0][0], 16)) {
        FastMode mode;
        record_000_maths<Fast>(k, d, cam);
        done = mode.clean();
    }
#endif
    if (!done) {
        record_000_maths<Exact>(k, d, cam);
    }
    memcpy(k.model, d.model, sizeof(k.model));
    memcpy(k.view, cam.view, sizeof(k.view));
    memcpy(k.screen, cam.screen, sizeof(k.screen));
    memcpy(k.light_col, d.light_col, sizeof(k.light_col));
    memcpy(k.fog, d.fog, sizeof(k.fog));
    for (int l = 0; l < 4; l++) {
        k.point_pos[l * 4 + 3] = d.point_pos[l][3];
        memcpy(&k.point_col[l * 4], d.point_col[l], 16);
    }
    k.info[0] = std::max(0, std::min(4, int32_t(int16_t(d.point_count))));
    if (d.flags & RD_DRAW_WAVE_UVS) {
        memcpy(k.wave, d.parent, sizeof(k.wave));
        k.info[3] |= VU1_WAVE;
    }
    if (d.flags & RD_DRAW_FAR_Z) {
        k.info[3] |= VU1_FAR;
    }
    return k;
}

Vu1Record vu1_record_5a0(const RdDraw &d, const RdCamera &cam) {
    Vu1Record k = vu1_record_000(d, cam);
    memset(k.light_dir, 0, sizeof(k.light_dir));
    memset(k.light_col, 0, sizeof(k.light_col));
    memset(k.fog, 0, sizeof(k.fog));
    memset(k.point_pos, 0, sizeof(k.point_pos));
    memset(k.point_col, 0, sizeof(k.point_col));
    k.info[0] = 0;
    return k;
}

Vu1Record280 vu1_record_280(const RdDraw &d, const RdCamera &cam) {
    Vu1Record280 k;
    memset(&k, 0, sizeof(k));
    RdDraw set = d;
    for (int s = 0; s < 2; s++) {
        memcpy(set.model, s ? d.parent : d.model, sizeof(set.model));
        Vu1Record c = vu1_record_000(set, cam);
        memcpy(k.vertex[s], c.vertex, 64);
        memcpy(k.light_dir[s], c.light_dir, 64);
        memcpy(k.point_pos[4 * s], c.point_pos, 64);
        k.info[0] = c.info[0];
        memcpy(k.sphere[s], d.turn[s], 64);
    }
    memcpy(k.light_col, d.light_col, 64);
    memcpy(k.fog, d.fog, 16);
    memcpy(k.scale, d.colour[0], 16);
    memcpy(k.bias, d.colour[1], 16);
    memcpy(k.point_col, d.point_col, 64);
    memcpy(k.screen, cam.screen, 64);
    return k;
}

bool vu1_kicks_280(const int32_t *q, const RdBatch &b, const Vu1Record280 &k, Vu1Kicks280 &p) {
    if (b.count == 0 || b.count > VU1_BATCH_MOST) {
        return false;
    }
    p.n = b.count;
    bool clip[VU1_BATCH_MOST];
    // (the set: ST's w, as ILW loads it)
    clip_all(clip, p.n, q, 32, false, any_huge(k.vertex[0], 32),
             [&](const int32_t *v) { return k.vertex[(uint32_t(v[11]) & 0xFFFF) != 0 ? 1 : 0]; });
    uint32_t count = 0;
    for (uint32_t i = 0; i < p.n; i++) {
        count += clip[i];
    }
    // ("vertex -1": vertex 0's clip seen again before the loop, by the count, ADC and the tag's w)
    count += clip[0];
    bool adc_tag = (uint16_t(b.tag[1] >> 32) & 1) && clip[0];
    for (uint32_t i = 0; i < p.n; i++) {
        p.adc[i] = clip[i] || (i >= 1 ? clip[i - 1] : clip[0]) || (i >= 2 && clip[i - 2]) || adc_tag;
    }
    p.all_clipped = int16_t(uint16_t(count - p.n)) >= 0;
    return true;
}

Vu1Record vu1_record_700(const RdDraw &d, const RdCamera &cam) {
    RdDraw e = d;
    e.point_count = 0;
    e.flags = 0; // (0x000's wave UVs and far plane: not 0x700's)
    Vu1Record k = vu1_record_000(e, cam);
    memset(k.point_pos, 0, sizeof(k.point_pos));
    memset(k.point_col, 0, sizeof(k.point_col));
    memcpy(k.point_col, d.colour[0], 16);
    k.info[0] = 0;
    return k;
}

std::vector<uint64_t> vu1_ad_700(const RdBatch &b, const RdDraw &d) {
    std::vector<uint64_t> out = b.ad;
    if (b.ad.size() < 4) {
        return out; // (TOP+0xA2 is not in the packet: not kicked)
    }
    if (uint16_t(b.ad[3] & 0x3FFF) != uint16_t(vu_float_bits(d.colour[1][3]))) {
        return out;
    }
    uint32_t q[4];
    for (int i = 0; i < 4; i++) {
        q[i] = vu_float_bits(d.colour[2][i]);
    }
    out[2] = q[2] & 0xFF;
    out[3] = uint64_t(q[0]) | uint64_t(q[1]) << 32;
    return out;
}

bool vu1_kicks_700(const int32_t *q, const RdBatch &b, const Vu1Record &k, Vu1Kicks700 &p) {
    if (b.count == 0 || b.count > VU1_BATCH_MOST) {
        return false;
    }
    p.n = b.count;
    bool clip[VU1_BATCH_MOST];
    clip_all(clip, p.n, q, 16, true, any_huge(k.vertex, 16), [&](const int32_t *) { return k.vertex; });
    uint32_t unclipped = 0;
    for (uint32_t i = 0; i < p.n; i++) {
        unclipped += !clip[i];
    }
    // vertex n, past the strip: clipped when not known (render.h's RdBatch::past)
    bool clip_past = true;
    if (b.past_known) {
        int32_t past[4] = {b.past[0], b.past[1], b.past[2], b.past[3]};
        clip_past = clipped(past, k.vertex, true);
    }
    // (vi06: n less the clipped of the n + 1 vertices, kicked when above 0)
    p.kick = int32_t(unclipped) - int32_t(clip_past) > 0;
    for (uint32_t i = 0; i < p.n; i++) {
        p.adc[i] = clip[i] || (i >= 1 && clip[i - 1]) || (i >= 2 && clip[i - 2]);
    }
    return true;
}
