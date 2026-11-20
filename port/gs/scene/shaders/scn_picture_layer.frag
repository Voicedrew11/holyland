#version 450
#extension GL_GOOGLE_include_directive : require
// The brightness kept as a layer of the frame (port/gs/scene/scn_build.cpp's Layer), drawn by each picture of the
// frame in place of its copy out: pcr_prim.frag's arithmetic for what such a layer is, and nothing else, so that a
// picture costs about what the copy did. The frame read at a point inside the layer's own rectangle (reads_inside: no
// wrap, no texel past the texture), UV (PCR_F_FST) rounded to 1/256 as texture_uv, nearest at the surface's scale as
// surface_pixel; the texture function (TFX, TCC); FBA. No fog, no alpha test, no blend: those layers are drawn by
// pcr_prim.frag. With pcr_prim.vert, whose corners and interpolants these are.
#include "pcr_common.h"

layout(location = 0) noperspective in vec4 v_color;
layout(location = 1) noperspective in vec3 v_stq;
layout(location = 0) out vec4 o_color;
layout(set = 1, binding = 1) uniform sampler2D u_surface;

void main() {
    vec2 uv = round(v_stq.xy * 256.0) / 256.0;
    // (surface_axis without its wrap: the texel's pixel at the scale, the surface's offset added)
    ivec2 at = ivec2(floor(uv * vec2(pc.src_scale))) + pc.src_offset * pc.src_scale;
    uvec4 t = uvec4(round(texelFetch(u_surface, at, 0) * 255.0));
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
