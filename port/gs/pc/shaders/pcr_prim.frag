#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's GS pixel (port/gs/pc/pcr_canvas.cpp): the texture read by the GPU's sampler where it can stand in
// (the texture's colours decoded once, the GS's filters and wrap modes as the sampler's, the level from Q), else the
// GS's read (its region modes, filters and mipmap level from Q, the CLUT, TEXA); texture function, fog and alpha
// test, in its integers. The blend is
// Vulkan's (the factors pcr_canvas.cpp picks for ALPHA), with the colour premultiplied here where it needs it.
#include "pcr_common.h"

layout(location = 0) noperspective in vec4 v_color;
layout(location = 1) noperspective in vec3 v_stq;
layout(location = 2) noperspective in float v_fog;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform usampler2D u_texels;  // RGBA8 raw (R8G8B8A8_UINT) or indices (R8_UINT)
layout(set = 0, binding = 1) uniform sampler2D u_surface;  // a texture that is a drawn surface (its copy)
layout(set = 0, binding = 3) uniform sampler2D u_dest;     // (PCR_F_DATE) the destination before the draw
layout(set = 0, binding = 4) uniform sampler2D u_sampled;  // (PCR_SRC_SAMPLED) the texture's colours, RGBA8
layout(set = 0, binding = 2) uniform Clut {
    uvec4 entries[64]; // the 256 colours the indices look up: R | G << 8 | B << 16 | A << 24
} u_clut;

uint src_kind() {
    return (pc.flags >> PCR_F_SRC_SHIFT) & 7u;
}

uvec4 unpack_rgba(uint c) {
    return uvec4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24);
}

// A texel coordinate wrapped as CLAMP says (level 0's region modes; the levels below repeat or clamp)
int wrap(int x, int size, uint mode, uint lo, uint hi, uint level) {
    if (mode == 0u || (mode == 3u && level != 0u)) {
        return x & (size - 1);
    }
    if (mode == 1u || level != 0u) {
        return clamp(x, 0, size - 1);
    }
    if (mode == 2u) {
        return clamp(x, int(lo), int(hi));
    }
    return int((uint(x) & lo) | hi); // REGION_REPEAT: MINU is the mask, MAXU the fix
}

// One texel of a level, as RGBA in the GS's integers (TEXA's alphas given to the formats without their own)
uvec4 texel(ivec2 at, uint level, ivec2 size) {
    at.x = wrap(at.x, size.x, pc.clamp_lo & 3u, (pc.clamp_lo >> 4) & 0x3FFu, (pc.clamp_lo >> 14) & 0x3FFu, level);
    at.y = wrap(at.y, size.y, (pc.clamp_lo >> 2) & 3u, pc.clamp_hi & 0x3FFu, (pc.clamp_hi >> 10) & 0x3FFu, level);
    uint kind = src_kind();
    if (kind == PCR_SRC_SURFACE) {
        ivec2 p = (at + pc.src_offset) * pc.src_scale + pc.src_scale / 2;
        return uvec4(round(texelFetch(u_surface, p, 0) * 255.0));
    }
    if (kind == PCR_SRC_INDEX) {
        uint i = texelFetch(u_texels, at, int(level)).r;
        return unpack_rgba(u_clut.entries[i >> 2][i & 3u]);
    }
    uvec4 c = texelFetch(u_texels, at, int(level));
    if (kind == PCR_SRC_CT32) {
        return c;
    }
    bool black = (pc.texa & 0x8000u) != 0u && (c.r | c.g | c.b) == 0u;
    if (kind == PCR_SRC_CT16 && c.a != 0u) {
        return uvec4(c.rgb, (pc.texa >> 16) & 0xFFu);
    }
    return uvec4(c.rgb, black ? 0u : pc.texa & 0xFFu);
}

// A level read at a point (in its texels), nearest or bilinear
uvec4 read_level(vec2 uv, uint level, bool linear) {
    ivec2 size = max(ivec2(int(pc.tex_size & 0xFFFFu), int(pc.tex_size >> 16)) >> int(level), ivec2(1));
    if (!linear) {
        return texel(ivec2(floor(uv)), level, size);
    }
    vec2 p = uv - 0.5;
    ivec2 i = ivec2(floor(p));
    // (the GS's 4 bits of fraction)
    vec2 f = floor(fract(p) * 16.0) / 16.0;
    vec4 a = vec4(texel(i, level, size)), b = vec4(texel(i + ivec2(1, 0), level, size));
    vec4 c = vec4(texel(i + ivec2(0, 1), level, size)), d = vec4(texel(i + ivec2(1, 1), level, size));
    return uvec4(floor(mix(mix(a, b, f.x), mix(c, d, f.x), f.y)));
}

// The texture's colour at this pixel: the level from Q (TEX1's LCM, L, K), the filter MMAG or MMIN picks
uvec4 sample_texture() {
    vec2 size = vec2(float(pc.tex_size & 0xFFFFu), float(pc.tex_size >> 16));
    vec2 uv = (pc.flags & PCR_F_FST) != 0u ? v_stq.xy : v_stq.xy / v_stq.z * size;
    float lod = pc.lod_k;
    if ((pc.tex1 & 0x80u) == 0u) {
        lod += log2(1.0 / abs(v_stq.z)) * float(1u << ((pc.tex1 >> 8) & 3u));
    }
    if (src_kind() == PCR_SRC_SAMPLED) {
        // (the sampler's filters are MMAG's at a level of 0 or below and MMIN's above; its levels and the lod's
        // clamp are the texture's: pcr_canvas.cpp's sampler_for)
        vec4 t = textureLod(u_sampled, uv / vec2(textureSize(u_sampled, 0)), lod);
        return uvec4(round(t * 255.0));
    }
    uint mmin = (pc.tex1 >> 1) & 7u;
    if (lod <= 0.0 || pc.levels <= 1u) {
        bool linear = lod <= 0.0 ? (pc.tex1 & 1u) != 0u : mmin == 1u || mmin == 4u || mmin == 5u;
        return read_level(uv, 0u, linear);
    }
    bool linear = mmin == 1u || mmin == 4u || mmin == 5u;
    float most = float(pc.levels - 1u);
    if (mmin == 3u || mmin == 5u) {
        float l = min(lod, most);
        uint l0 = uint(floor(l));
        uint l1 = min(l0 + 1u, pc.levels - 1u);
        float f = floor(fract(l) * 16.0) / 16.0;
        vec4 a = vec4(read_level(uv / float(1u << l0), l0, linear));
        vec4 b = vec4(read_level(uv / float(1u << l1), l1, linear));
        return uvec4(floor(mix(a, b, f)));
    }
    uint l = mmin >= 2u ? uint(min(floor(lod + 0.5), most)) : 0u;
    return read_level(uv / float(1u << l), l, linear);
}

bool alpha_passes(uint a) {
    uint aref = (pc.test >> 3) & 0xFFu;
    switch (pc.test & 7u) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return a < aref;
    case 3u: return a <= aref;
    case 4u: return a == aref;
    case 5u: return a >= aref;
    case 6u: return a > aref;
    default: return a != aref;
    }
}

void main() {
    if ((pc.flags & PCR_F_DATE) != 0u) {
        uint da = uint(round(texelFetch(u_dest, ivec2(gl_FragCoord.xy), 0).a * 255.0));
        if (((da >> 7) & 1u) != ((pc.flags & PCR_F_DATM) != 0u ? 1u : 0u)) {
            discard;
        }
    }
    uvec4 vc = uvec4(v_color + 0.001);
    uvec4 c = vc;
    if ((pc.flags & PCR_F_TME) != 0u) {
        uvec4 t = sample_texture();
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
    }
    if ((pc.flags & PCR_F_FGE) != 0u) {
        uint f = uint(clamp(v_fog, 0.0, 255.0));
        uvec3 fog = uvec3(pc.fogcol & 0xFFu, (pc.fogcol >> 8) & 0xFFu, (pc.fogcol >> 16) & 0xFFu);
        c.rgb = (f * c.rgb + (255u - f) * fog) >> 8;
    }
    uint pass = (pc.flags >> PCR_F_PASS_SHIFT) & 3u;
    if (pass != 0u && alpha_passes(c.a) != (pass == 1u)) {
        discard;
    }
    uvec3 rgb = c.rgb;
    if ((pc.flags & PCR_F_PREMUL) != 0u) {
        uint k = (pc.flags & PCR_F_C_FIX) != 0u ? pc.clamp_hi >> 24 : c.a;
        rgb = min((rgb * k) >> 7, uvec3(255u));
    }
    uint a = (pc.flags & PCR_F_FBA) != 0u ? c.a | 0x80u : c.a;
    float alpha_out = (pc.flags & PCR_F_ALPHA_FACTOR) != 0u ? float(c.a) / 128.0 : float(a) / 255.0;
    o_color = vec4(vec3(rgb) / 255.0, alpha_out);
}
