// A native draw's record for program 0x000 (vu1_record.hpp): VU1's set-up in its arithmetic
#include "vu1_record.hpp"

#include <string.h>

#include <algorithm>

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
