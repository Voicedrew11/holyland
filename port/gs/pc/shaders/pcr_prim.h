// The PC renderer's GS pixel (port/gs/pc/pcr_canvas.cpp): the texture read by the GPU's sampler where it can stand in
// (the texture's colours decoded once, the GS's filters and wrap modes as the sampler's, the level from Q), else the
// GS's read (its region modes, filters and mipmap level from Q, the CLUT, TEXA); texture function, fog and alpha
// test, in its integers. The blend is
// Vulkan's (the factors pcr_canvas.cpp picks for ALPHA), with the colour premultiplied here where it needs it; a blend
// with no Vulkan form is the GS's here, from a copy of the destination (PCR_F_SHADER_BLEND).
#include "pcr_common.h"

// (pcr_prim_sample.frag, PCR_SAMPLE: shaded once a sample of a multisampled target, its inputs taken at each sample:
// the cut-outs with cut-out AA, pcr_canvas.cpp)
#ifdef PCR_SAMPLE
#define PCR_IN sample noperspective in
#else
#define PCR_IN noperspective in
#endif
layout(location = 0) PCR_IN vec4 v_color;
layout(location = 1) PCR_IN vec3 v_stq;
layout(location = 2) PCR_IN float v_fog;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform usampler2D u_texels;  // RGBA8 raw (R8G8B8A8_UINT) or indices (R8_UINT)
layout(set = 0, binding = 1) uniform sampler2D u_surface;  // a texture that is a drawn surface (its copy)
layout(set = 0, binding = 3) uniform sampler2D u_dest;     // (PCR_F_DATE, PCR_F_SHADER_BLEND) the destination before
                                                           // the draw, from pc.dest_offset
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

// One axis of a surface's pixel (at the surface's scale, scale of them a GS texel): the texel it is in wrapped as
// CLAMP says, its place inside the texel kept, or the clamped texel's edge pixel when a clamp moved it
int surface_axis(int h, int scale, int offset, int size, uint mode, uint lo, uint hi) {
    int t = h >= 0 ? h / scale : -((scale - 1 - h) / scale);
    int sub = h - t * scale;
    int w = wrap(t, size, mode, lo, hi, 0u);
    if (w != t && (mode == 1u || mode == 2u)) {
        sub = w > t ? 0 : scale - 1;
    }
    return (w + offset) * scale + sub;
}

// One pixel of a texture that is a drawn surface, at the surface's own scale: the texels drawn finer than the GS's
// are read as they were drawn, not one GS texel's middle (a frame read back whole keeps the scale's detail)
uvec4 surface_pixel(ivec2 h, ivec2 size) {
    int x = surface_axis(h.x, pc.src_scale.x, pc.src_offset.x, size.x, pc.clamp_lo & 3u, (pc.clamp_lo >> 4) & 0x3FFu,
                         (pc.clamp_lo >> 14) & 0x3FFu);
    int y = surface_axis(h.y, pc.src_scale.y, pc.src_offset.y, size.y, (pc.clamp_lo >> 2) & 3u, pc.clamp_hi & 0x3FFu,
                         (pc.clamp_hi >> 10) & 0x3FFu);
    uvec4 c = uvec4(round(texelFetch(u_surface, ivec2(x, y), 0) * 255.0));
    if ((pc.flags & PCR_F_SURFACE_CT16) != 0u) {
        // (a 16-bit frame keeps a colour's top 5 bits and alpha's bit 7; read as a texture, the colours are those
        // bits and the alpha TEXA's, TA1 or TA0 by that bit, 0 for black with AEM: texel()'s PCR_SRC_CT16)
        c.rgb &= uvec3(0xF8u);
        bool black = (pc.texa & 0x8000u) != 0u && (c.r | c.g | c.b) == 0u;
        c.a = c.a >= 0x80u ? (pc.texa >> 16) & 0xFFu : black ? 0u : pc.texa & 0xFFu;
    }
    return c;
}

// A surface's texture read at a point (in GS texels), nearest or bilinear among the surface's own pixels
uvec4 read_surface(vec2 uv, ivec2 size, bool linear) {
    vec2 h = uv * vec2(pc.src_scale);
    if (!linear) {
        return surface_pixel(ivec2(floor(h)), size);
    }
    vec2 p = h - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = floor(fract(p) * 16.0) / 16.0;
    vec4 a = vec4(surface_pixel(i, size)), b = vec4(surface_pixel(i + ivec2(1, 0), size));
    vec4 c = vec4(surface_pixel(i + ivec2(0, 1), size)), d = vec4(surface_pixel(i + ivec2(1, 1), size));
    return uvec4(floor(mix(mix(a, b, f.x), mix(c, d, f.x), f.y)));
}

// A level read at a point (in its texels), nearest or bilinear
uvec4 read_level(vec2 uv, uint level, bool linear) {
    ivec2 size = max(ivec2(int(pc.tex_size & 0xFFFFu), int(pc.tex_size >> 16)) >> int(level), ivec2(1));
    if (src_kind() == PCR_SRC_SURFACE) {
        return read_surface(uv, size, linear); // (a surface's copy has one level)
    }
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

// The texture's coordinates at this pixel, in level 0's texels
vec2 texture_uv() {
    vec2 size = vec2(float(pc.tex_size & 0xFFFFu), float(pc.tex_size >> 16));
    // (UV, FST: the GS's 1/16 texel at the corners, often a whole texel at a pixel, as a frame drawn back 1:1. The GPU's
    // interpolation lands a little either side of it, which side by vendor (NVIDIA's below, so floor took the texel
    // before on whole triangles of a frame drawn back); rounded to 1/256 texel it lands on it, 2026-11-11. S/T/Q the same:
    // S/Q times the size lands on a whole texel in the menus' brightness sprite and background, which draw the buffer
    // back over itself 1:1, and the menus at 1x sat a GS pixel left of gs's, 2026-11-11. The GS's own U and V are in
    // 1/16 texel.)
    vec2 uv = (pc.flags & PCR_F_FST) != 0u ? v_stq.xy : v_stq.xy / v_stq.z * size;
    return round(uv * 256.0) / 256.0;
}

// (with anisotropy) their change a pixel across and down, taken in main before anything is discarded
vec2 uv_dx, uv_dy;

uint aniso_log2() {
    return (pc.tex1 >> 10) & 7u;
}

// The sampled texture read with the sampler's anisotropy (aniso_log2 above 0: a mipmapped texture filtered bilinear).
// The GS picks the level from the distance alone, the same along a surface and across it, so a floor or wall seen at
// a grazing angle gets a level coarser than its texels across the pixel want. The pixel's footprint on the texture
// is long: the sampler reads n times along its long axis (n its length over its width, at most the sampler's), each
// at the level of the long axis over n, by textureGrad with the footprint as it is. Where that is no finer than the
// GS's level (a footprint about as long as wide, or the GS's level fine enough already) it is read as without it.
// (The footprint is never made longer: where its width is under a texel, the reads at level 0 are the sampler's.)
bool sampled_anisotropic(vec2 uv, float lod, out vec4 t) {
    float lx = length(uv_dx), ly = length(uv_dy);
    float lmax = max(lx, ly);
    float n = min(ceil(lmax / max(min(lx, ly), 1e-8) - 0.01), float(1u << aniso_log2()));
    float alod = log2(max(lmax / n, 1e-8));
    if (n < 2.0 || alod >= lod - 1.0 / 16.0) {
        return false;
    }
    vec2 size = vec2(textureSize(u_sampled, 0));
    t = textureGrad(u_sampled, uv / size, uv_dx / size, uv_dy / size);
    return true;
}

// The texture's colour at this pixel: the level from Q (TEX1's LCM, L, K), the filter MMAG or MMIN picks; above 0 the
// texture detail's bias moves which level (never the choice between MMAG and MMIN), within the texture's levels
uvec4 sample_texture() {
    vec2 uv = texture_uv();
    float lod = pc.lod_k;
    if ((pc.tex1 & 0x80u) == 0u) {
        lod += log2(1.0 / abs(v_stq.z)) * float(1u << ((pc.tex1 >> 8) & 3u));
    }
    float bias = float(int(pc.tex1) >> 16) / 256.0;
    if (src_kind() == PCR_SRC_SAMPLED) {
        // (the sampler's filters are MMAG's at a level of 0 or below and MMIN's above; its levels and the lod's
        // clamp are the texture's: pcr_canvas.cpp's sampler_for)
        if (lod > 0.0 && bias != 0.0) {
            lod = max(lod + bias, 1.0 / 256.0);
        }
        vec4 t;
        if (lod <= 0.0 || aniso_log2() == 0u || !sampled_anisotropic(uv, lod, t)) {
            t = textureLod(u_sampled, uv / vec2(textureSize(u_sampled, 0)), lod);
        }
        return uvec4(round(t * 255.0));
    }
    uint mmin = (pc.tex1 >> 1) & 7u;
    if (lod <= 0.0 || pc.levels <= 1u) {
        bool linear = lod <= 0.0 ? (pc.tex1 & 1u) != 0u : mmin == 1u || mmin == 4u || mmin == 5u;
        return read_level(uv, 0u, linear);
    }
    bool linear = mmin == 1u || mmin == 4u || mmin == 5u;
    float most = float(pc.levels - 1u);
    lod = max(lod + bias, 0.0);
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

// The destination as it was before the draw, in the GS's integers
uvec4 destination() {
    return uvec4(round(texelFetch(u_dest, ivec2(gl_FragCoord.xy) - pc.dest_offset, 0) * 255.0));
}

// ALPHA's (A - B) * C + D in the GS's integers (as gs_frag.h's), COLCLAMP's clamp or wrap; with PABE, Cs where As's
// bit 7 is clear
uvec3 blended(uvec4 cs) {
    if ((pc.alpha & 0x20000u) != 0u && cs.a < 0x80u) {
        return cs.rgb;
    }
    uvec4 cd = destination();
    ivec3 s = ivec3(cs.rgb), d = ivec3(cd.rgb);
    uint sel_a = pc.alpha & 3u, sel_b = (pc.alpha >> 2) & 3u, sel_c = (pc.alpha >> 4) & 3u, sel_d = (pc.alpha >> 6) & 3u;
    ivec3 a = sel_a == 0u ? s : sel_a == 1u ? d : ivec3(0);
    ivec3 b = sel_b == 0u ? s : sel_b == 1u ? d : ivec3(0);
    ivec3 dd = sel_d == 0u ? s : sel_d == 1u ? d : ivec3(0);
    // (a 24-bit frame has no alpha: its Ad is 1.0)
    int ad = (pc.alpha & 0x40000u) != 0u ? 0x80 : int(cd.a);
    int c = sel_c == 0u ? int(cs.a) : sel_c == 1u ? ad : int((pc.alpha >> 8) & 0xFFu);
    ivec3 r = (((a - b) * c) >> 7) + dd;
    return (pc.alpha & 0x10000u) != 0u ? uvec3(clamp(r, 0, 255)) : uvec3(r & 0xFF);
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
    if ((pc.flags & PCR_F_TME) != 0u && aniso_log2() != 0u) {
        vec2 uv = texture_uv();
        uv_dx = dFdx(uv);
        uv_dy = dFdy(uv);
    }
    if ((pc.flags & PCR_F_DATE) != 0u) {
        uint da = destination().a;
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
    uvec3 rgb = (pc.flags & PCR_F_SHADER_BLEND) != 0u ? blended(c) : c.rgb;
    if ((pc.flags & PCR_F_PREMUL) != 0u) {
        uint k = (pc.flags & PCR_F_C_FIX) != 0u ? pc.clamp_hi >> 24 : c.a;
        rgb = min((rgb * k) >> 7, uvec3(255u));
    }
    uint a = (pc.flags & PCR_F_FBA) != 0u ? c.a | 0x80u : c.a;
    float alpha_out = (pc.flags & PCR_F_ALPHA_FACTOR) != 0u ? float(c.a) / 128.0 : float(a) / 255.0;
    o_color = vec4(vec3(rgb) / 255.0, alpha_out);
}
