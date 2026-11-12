// The reflection's rippled water on the GPU, the CPU's part (ripple.hpp): the EE's maths in VU0's arithmetic
#include "ripple.hpp"

#include <string.h>

extern "C" {
#include "port/vu_float.h"
}

namespace {

float vu_mul(float a, float b) {
    return vu_float_from_bits(vu_float_mul(vu_float_bits(a), vu_float_bits(b)));
}
float vu_add(float a, float b) {
    return vu_float_from_bits(vu_float_add(vu_float_bits(a), vu_float_bits(b)));
}

// out = m v, as MULAx ACC; MADDAy; MADDAz; MADDw: m a qword a column
void vu_transform4(float *out, const float *m, const float *v) {
    for (int r = 0; r < 4; r++) {
        float acc = vu_mul(m[r], v[0]);
        acc = vu_add(acc, vu_mul(m[4 + r], v[1]));
        acc = vu_add(acc, vu_mul(m[8 + r], v[2]));
        out[r] = vu_add(acc, vu_mul(m[12 + r], v[3]));
    }
}

} // namespace

RippleRecord ripple_record(const RdDraw &d, const RdCamera &cam) {
    RippleRecord k;
    memset(&k, 0, sizeof(k));
    // Matrix_Multiply(vm, world_screen_matrix, model by part): each of the part's columns by world_screen
    for (int col = 0; col < 4; col++) {
        vu_transform4(&k.vertex[col * 4], &cam.world_screen[0][0], &d.model[col][0]);
    }
    memcpy(k.screen, cam.screen, sizeof(k.screen));
    memcpy(k.sines, d.point_pos, 16 * sizeof(float));
    memcpy(k.sines + 16, d.point_col, 16 * sizeof(float));
    k.info[2] = (d.flags & RD_DRAW_ROUND_NEAREST) ? RIPPLE_NEAREST : 0;
    return k;
}

RipplePlace ripple_place(const RippleRecord &k, float x, float y, float z) {
    float p[4] = {x, y, z, 1.0f}, c[4];
    vu_transform4(c, k.vertex, p);
    uint32_t q = vu_float_div(0x3F800000u, vu_float_bits(c[3]));
    RipplePlace out;
    out.x = vu_float_ftoi(vu_float_mul(vu_float_bits(c[0]), q), 4);
    out.y = vu_float_ftoi(vu_float_mul(vu_float_bits(c[1]), q), 4);
    out.z = vu_float_ftoi(vu_float_mul(vu_float_bits(c[2]), q), 4) >> 4;
    return out;
}

void ripple_adc(const int32_t *vertices, uint32_t n, const RippleRecord &k, std::vector<uint8_t> &adc) {
    adc.resize(n);
    uint32_t window = 0;
    for (uint32_t v = 0; v < n; v++) {
        const int32_t *q = &vertices[size_t(v) * 16];
        RipplePlace p = ripple_place(k, float(q[0]), float(q[1]), float(q[2]));
        // (out of bounds: this vertex's kick and the next two get ADC)
        window |= uint32_t(p.z) > 0xFFFF0u || ((p.x | p.y) & int32_t(0xFFFF0000u)) != 0;
        adc[v] = window != 0 || (uint32_t(q[7]) & 0x8000u) != 0;
        window = (window << 1) & 7;
    }
}
