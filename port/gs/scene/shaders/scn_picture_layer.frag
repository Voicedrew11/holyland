#version 450
#extension GL_GOOGLE_include_directive : require
// The brightness kept as a layer of the frame (port/gs/scene/scn_build.cpp's Layer), drawn by each picture of the
// frame in place of its copy out: pcr_prim.frag's arithmetic for what such a layer is, and nothing else, so that a
// picture costs about what the copy did. The frame read at a point inside the layer's own rectangle (reads_inside: no
// wrap, no texel past the texture), UV (PCR_F_FST) rounded to 1/256 as texture_uv, nearest at the surface's scale as
// surface_pixel; the texture function (TFX, TCC); FBA. No fog, no alpha test, no blend: those layers are drawn by
// pcr_prim.frag. With pcr_prim.vert, whose corners and interpolants these are.
// The fills after the HUD kept before it (scn_build.cpp's is_fill: one colour over the whole frame) are applied to the
// pixel read, in order, as the GS draws them into the frame before the brightness reads it: ((A - B) * C >> 7) + D a
// channel as pcr_prim.h's blended (the GS's arithmetic), or as slice 2's draw of the fill made it (gpu_blend).
#include "pcr_common.h"

layout(location = 0) noperspective in vec4 v_color;
layout(location = 1) noperspective in vec3 v_stq;
layout(location = 0) out vec4 o_color;
layout(set = 1, binding = 1) uniform sampler2D u_surface;
// (where a CLUT would be) entries[0].x the fills' count; fill i at entries[1 + i]: x ALPHA's word as pc.alpha (A 0-1,
// B 2-3, C 4-5, D 6-7, FIX 8-15; COLCLAMP 16, PABE 17, a 24-bit frame 18), y its RGBA (R 0-7 ... A 24-31), z bit 0
// blended (PRIM's ABE), bit 1 its alpha written, bit 2 FBA, bit 3 the GS's arithmetic
layout(set = 1, binding = 2) uniform Fills {
    uvec4 entries[64];
} u_fills;

// The fill as slice 2's draw of it into the frame made it (pcr_blend.h's choose_blend, the forms is_fill takes): by
// Vulkan's blend, its factor C / 128 (at most 1: a fixed-point target's source clamped); or C applied in the shader
// first (PCR_F_PREMUL: (Cs * C) >> 7) and added or taken away by the blend. The blend as the Vega's (RADV) makes it,
// fitted to slice 2's pictures (the frame under the fill read out, 1,251 pairs of a colour and what the blend made of
// it: black fills at alphas 0x21, 0x4e and 0x6e, red 128 at 0x40; no miss): each colour taken as a fraction of 255 held
// to 12 significant bits (to the nearest, a half to even), the blend's sum of them exact, the result rounded to the
// nearest 255th (a half up). Another GPU's blend may round otherwise: then a level here and there, as any two GPUs'.
vec3 held(vec3 x) {
    // (the float's 23 bits of fraction cut to 11, to the nearest, a half to even; 0 stays 0)
    uvec3 b = floatBitsToUint(x);
    b = (b + 0x7FFu + ((b >> 12u) & 1u)) & ~0xFFFu;
    return uintBitsToFloat(b);
}

vec3 nearest(vec3 v) {
    return clamp(floor(v * 255.0 + 0.5), 0.0, 255.0);
}

uvec3 gpu_blend(uvec3 cs, uvec3 cd, uint sel_a, uint sel_b, uint sel_d, int c) {
    vec3 s = held(vec3(cs) / 255.0), d = held(vec3(cd) / 255.0);
    float k = min(float(c) / 128.0, 1.0);
    uvec3 premul = min((cs * uint(c)) >> 7u, uvec3(255u));
    if (sel_a == sel_b) {
        return sel_d == 0u ? cs : sel_d == 1u ? cd : uvec3(0u);
    }
    if (sel_a == 0u && sel_b == 1u) { // Cs * C + Cd * (1 - C)
        return uvec3(nearest(s * k + d * (1.0 - k)));
    }
    if (sel_a == 1u && sel_b == 0u) { // Cd * C + Cs * (1 - C)
        return uvec3(nearest(d * k + s * (1.0 - k)));
    }
    if (sel_a == 0u && sel_b == 2u) { // Cs * C + D
        return sel_d == 1u ? min(premul + cd, uvec3(255u)) : premul;
    }
    if (sel_a == 2u && sel_b == 0u) { // D - Cs * C
        return sel_d == 1u ? uvec3(max(ivec3(cd) - ivec3(premul), ivec3(0))) : uvec3(0u);
    }
    // Cd * C + D (D Cs or 0)
    return uvec3(nearest(d * k + (sel_d == 0u ? s : vec3(0.0))));
}

uvec4 fill(uvec4 cd, uvec4 f) {
    uvec4 cs = uvec4(f.y & 0xFFu, (f.y >> 8) & 0xFFu, (f.y >> 16) & 0xFFu, f.y >> 24);
    uvec4 r = cd;
    if ((f.z & 1u) == 0u || ((f.x & 0x20000u) != 0u && cs.a < 0x80u)) {
        r.rgb = cs.rgb;
    } else {
        ivec3 s = ivec3(cs.rgb), d = ivec3(cd.rgb);
        uint sel_a = f.x & 3u, sel_b = (f.x >> 2) & 3u, sel_c = (f.x >> 4) & 3u, sel_d = (f.x >> 6) & 3u;
        ivec3 a = sel_a == 0u ? s : sel_a == 1u ? d : ivec3(0);
        ivec3 b = sel_b == 0u ? s : sel_b == 1u ? d : ivec3(0);
        ivec3 dd = sel_d == 0u ? s : sel_d == 1u ? d : ivec3(0);
        int ad = (f.x & 0x40000u) != 0u ? 0x80 : int(cd.a);
        int c = sel_c == 0u ? int(cs.a) : sel_c == 1u ? ad : int((f.x >> 8) & 0xFFu);
        if ((f.z & 8u) != 0u) {
            ivec3 v = (((a - b) * c) >> 7) + dd;
            r.rgb = (f.x & 0x10000u) != 0u ? uvec3(clamp(v, 0, 255)) : uvec3(v & 0xFF);
        } else {
            r.rgb = gpu_blend(cs.rgb, cd.rgb, sel_a, sel_b, sel_d, c);
        }
    }
    if ((f.z & 2u) != 0u) {
        r.a = (f.z & 4u) != 0u ? cs.a | 0x80u : cs.a;
    }
    return r;
}

void main() {
    // (pcr_prim.h's texture_uv and read_surface: rounded to 1/256 of the surface's pixel, not of a texel, which at a
    // scale of 3 or 6 read the pixel before every third pixel)
    vec2 h = round(v_stq.xy * vec2(pc.src_scale) * 256.0) / 256.0;
    // (surface_axis without its wrap: the texel's pixel at the scale, the surface's offset added)
    ivec2 at = ivec2(floor(h)) + pc.src_offset * pc.src_scale;
    uvec4 t = uvec4(round(texelFetch(u_surface, at, 0) * 255.0));
    uint fills = u_fills.entries[0].x;
    for (uint i = 0u; i < fills; i++) {
        t = fill(t, u_fills.entries[1u + i]);
    }
    uvec4 vc = uvec4(v_color + 0.001);
    uvec4 c;
    bool tcc = (pc.flags & PCR_F_TCC) != 0u;
    switch ((pc.flags >> PCR_F_TFX_SHIFT) & 3u) {
    case 0u: // MODULATE
        c.rgb = min((t.rgb * vc.rgb) >> 7, uvec3(255u));
        c.a = tcc ? min((t.a * vc.a) >> 7, 255u) : vc.a;
        break;
    case 1u: // DECAL
        c.rgb = t.rgb;
        c.a = tcc ? t.a : vc.a;
        break;
    case 2u: // HIGHLIGHT
        c.rgb = min(((t.rgb * vc.rgb) >> 7) + vc.a, uvec3(255u));
        c.a = tcc ? min(t.a + vc.a, 255u) : vc.a;
        break;
    default: // HIGHLIGHT2
        c.rgb = min(((t.rgb * vc.rgb) >> 7) + vc.a, uvec3(255u));
        c.a = tcc ? t.a : vc.a;
        break;
    }
    uint a = (pc.flags & PCR_F_FBA) != 0u ? c.a | 0x80u : c.a;
    o_color = vec4(vec3(c.rgb) / 255.0, float(a) / 255.0);
}
