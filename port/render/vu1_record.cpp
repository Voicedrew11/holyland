// A native draw's record for program 0x000 (vu1_record.hpp): VU1's set-up in its arithmetic
#include "vu1_record.hpp"

#include <string.h>

#include <algorithm>
#include <vector>

extern "C" {
#include "port/vu_float.h"
}

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

// out = m v, as MULAx ACC; MADDAy; MADDAz; MADDw (each product rounded, then the sum): m a qword a column
void vu_transform4(float *out, const float *m, const float *v) {
    for (int r = 0; r < 4; r++) {
        float acc = vu_mul(m[r], v[0]);
        acc = vu_add(acc, vu_mul(m[4 + r], v[1]));
        acc = vu_add(acc, vu_mul(m[8 + r], v[2]));
        out[r] = vu_add(acc, vu_mul(m[12 + r], v[3]));
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

} // namespace

Vu1Record vu1_record_000(const RdDraw &d, const RdCamera &cam) {
    Vu1Record k;
    memset(&k, 0, sizeof(k));
    const float *m = &d.model[0][0];
    for (int col = 0; col < 4; col++) {
        vu_transform4(&k.vertex[col * 4], &cam.world_screen[0][0], &m[col * 4]);
        vu_transform4(&k.light_dir[col * 4], &d.light_dir[0][0], &m[col * 4]);
    }
    memcpy(k.model, d.model, sizeof(k.model));
    memcpy(k.view, cam.view, sizeof(k.view));
    memcpy(k.screen, cam.screen, sizeof(k.screen));
    memcpy(k.light_col, d.light_col, sizeof(k.light_col));
    memcpy(k.fog, d.fog, sizeof(k.fog));
    for (int l = 0; l < 4; l++) {
        // the point light into the part's space: less its translation, then by its rotation with the off-diagonal
        // signs turned (the set-up's own maths, to the sign of a 0: the translation as 0 + it, the turned signs as
        // 0 - them)
        float p[3] = {vu_sub(d.point_pos[l][0], vu_add(0.0f, m[12])), vu_sub(d.point_pos[l][1], vu_add(0.0f, m[13])),
                      vu_sub(d.point_pos[l][2], vu_add(0.0f, m[14]))};
        float r0[3] = {vu_add(0.0f, m[0]), vu_sub(0.0f, m[1]), vu_sub(0.0f, m[2])};
        float r1[3] = {vu_sub(0.0f, m[4]), vu_add(0.0f, m[5]), vu_sub(0.0f, m[6])};
        float r2[3] = {vu_sub(0.0f, m[8]), vu_sub(0.0f, m[9]), vu_add(0.0f, m[10])};
        for (int i = 0; i < 3; i++) {
            float acc = vu_mul(r0[i], p[0]);
            acc = vu_add(acc, vu_mul(r1[i], p[1]));
            k.point_pos[l * 4 + i] = vu_add(acc, vu_mul(r2[i], p[2]));
        }
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
    uint32_t count = 0;
    for (uint32_t i = 0; i < p.n; i++) {
        const int32_t *v = q + size_t(i) * 32;
        clip[i] = clipped(v, k.vertex[(uint32_t(v[11]) & 0xFFFF) != 0 ? 1 : 0], false); // (the set: ST's w, as ILW loads it)
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
    uint32_t unclipped = 0;
    for (uint32_t i = 0; i < p.n; i++) {
        clip[i] = clipped(q + size_t(i) * 16, k.vertex, true);
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
