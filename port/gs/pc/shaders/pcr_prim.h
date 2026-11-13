// The PC renderer's GS pixel (port/gs/pc/pcr_canvas.cpp): the texture read by the GPU's sampler where it can stand in
// (the texture's colours decoded once, the GS's filters and wrap modes as the sampler's, the level from Q), else the
// GS's read (its region modes, filters and mipmap level from Q, the CLUT, TEXA); texture function, fog and alpha
// test, in its integers. The blend is
// Vulkan's (the factors pcr_canvas.cpp picks for ALPHA), with the colour premultiplied here where it needs it; a blend
// with no Vulkan form is the GS's here, from a copy of the destination (PCR_F_SHADER_BLEND).
#extension GL_EXT_nonuniform_qualifier : require
#include "pcr_common.h"

// (pcr_prim_sample.frag, PCR_SAMPLE: shaded once a sample of a multisampled target, its inputs taken at each sample:
// the cut-outs with cut-out AA, pcr_canvas.cpp)
#ifdef PCR_SAMPLE
#define PCR_IN sample noperspective in
#else
#define PCR_IN noperspective in
#endif
// (pcr_prim_early.frag, PCR_EARLY: the draws that discard nothing, no alpha test's pass and no DATE from a copy:
// without a discard in the shader the depth test and write are made before it (the GPU's early Z), which a shader
// that may discard keeps until after it; the same pixels either way)
#ifdef PCR_EARLY
layout(early_fragment_tests) in;
#endif
layout(location = 0) PCR_IN vec4 v_color;
layout(location = 1) PCR_IN vec3 v_stq;
layout(location = 2) PCR_IN float v_fog;
layout(location = 3) flat in uvec2 v_rect; // (the 2D mode's smooth: pcr_prim.vert's a_rect)

layout(location = 0) out vec4 o_color;
#ifdef PCR_RECORDS
// (a run's draw, pc.factors' top bit: the blend's colour factors, the source's in rgb and one minus the destination's in
// alpha (the blend's SRC1_COLOR and ONE_MINUS_SRC1_ALPHA); the colour out's alpha then the alpha the blend would have
// written)
layout(location = 0, index = 1) out vec4 o_factor;
#endif

layout(set = 1, binding = 0) uniform usampler2D u_texels;  // RGBA8 raw (R8G8B8A8_UINT) or indices (R8_UINT)
layout(set = 1, binding = 1) uniform sampler2D u_surface;  // a texture that is a drawn surface (its copy)
layout(set = 1, binding = 3) uniform sampler2D u_dest;     // (PCR_F_DATE, PCR_F_SHADER_BLEND) the destination before
                                                           // the draw, from pc.dest_offset
layout(set = 1, binding = 4) uniform sampler2D u_sampled;  // (PCR_SRC_SAMPLED) the texture's colours, RGBA8
layout(set = 1, binding = 2) uniform Clut {
    uvec4 entries[64]; // the 256 colours the indices look up: R | G << 8 | B << 16 | A << 24
} u_clut;
// (pc.clut_at) a kept walk's CLUTs, 256 colours each, put on the GPU once when the walk is handed out (pcr_canvas.cpp's
// put_cluts_on_gpu): its pictures between bind no CLUT a batch
layout(set = 0, binding = 15, std430) readonly buffer WalkCluts {
    uint colours[];
} u_walk_cluts;
// (pc.table) a kept walk's textures, written once when the walk is handed out (pcr_canvas.cpp's put_table_on_gpu): its
// pictures between bind no texture a batch. One binding, the images alone (Granite's bindless set), read as integers
// (the texels: R8_UINT, R8G8B8A8_UINT) or as colours (a sampled texture's: RGBA8), with the samplers beside them; every
// draw's index is the same across it (a push constant), so no nonuniformEXT
layout(set = 2, binding = 0) uniform texture2D u_table[];
layout(set = 2, binding = 0) uniform utexture2D u_table_uint[];
layout(set = 0, binding = 16) uniform sampler u_samplers[16]; // (0: nearest, clamped: the texels' texelFetch)

bool from_table() {
    return (pc.table & 0xFFFFu) != PCR_NO_TABLE;
}

// The texels (u_texels or the table's)
uvec4 fetch_texels(ivec2 at, int level) {
    if (from_table()) {
        return texelFetch(usampler2D(u_table_uint[pc.table & 0xFFFFu], u_samplers[0]), at, level);
    }
    return texelFetch(u_texels, at, level);
}

// The sampled texture (u_sampled or the table's with its sampler)
ivec2 sampled_size() {
    if (from_table()) {
        return textureSize(sampler2D(u_table[pc.table & 0xFFFFu], u_samplers[(pc.table >> 16) & 15u]), 0);
    }
    return textureSize(u_sampled, 0);
}
vec4 sampled_lod(vec2 uv, float lod) {
    if (from_table()) {
        return textureLod(sampler2D(u_table[pc.table & 0xFFFFu], u_samplers[(pc.table >> 16) & 15u]), uv, lod);
    }
    return textureLod(u_sampled, uv, lod);
}
vec4 sampled_grad(vec2 uv, vec2 dx, vec2 dy) {
    if (from_table()) {
        return textureGrad(sampler2D(u_table[pc.table & 0xFFFFu], u_samplers[(pc.table >> 16) & 15u]), uv, dx, dy);
    }
    return textureGrad(u_sampled, uv, dx, dy);
}
vec4 sampled_fetch(ivec2 at) {
    if (from_table()) {
        return texelFetch(sampler2D(u_table[pc.table & 0xFFFFu], u_samplers[(pc.table >> 16) & 15u]), at, 0);
    }
    return texelFetch(u_sampled, at, 0);
}

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
        uint i = fetch_texels(at, int(level)).r;
        return unpack_rgba(pc.clut_at != PCR_NO_WALK_CLUT ? u_walk_cluts.colours[pc.clut_at + i]
                                                          : u_clut.entries[i >> 2][i & 3u]);
    }
    uvec4 c = fetch_texels(at, int(level));
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

// (with anisotropy or the 2D mode's smooth) their change a pixel across and down, taken in main before anything is
// discarded
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
    vec2 size = vec2(sampled_size());
    t = sampled_grad(uv / size, uv_dx / size, uv_dy / size);
    return true;
}

// (colour high: the texture's colour with its fraction where the read keeps one: the sampler's filter, the 2D mode's
// cubic; texture_f_kept says it was set, else the colour is sample_texture's whole numbers)
vec4 texture_f;
bool texture_f_kept = false;

// The 2D mode's smooth (pcr_canvas.cpp, PCR_F_SMOOTH_2D: at a scale above 1, a sprite's or a 2D triangle's texture
// read nearest): where the texture is magnified on the target (a texel at least a pixel across), it is read with a
// Catmull-Rom cubic (16 texels) at this pixel's own place, not nearest: the same picture at every scale, the texels'
// steps smooth and their edges still sharp (the native GS's --native-2d smooth). Read nearest, a texel covers the
// target's pixels from where its coordinate is reached; its middle, where the cubic puts it, is half a pixel's change
// on from this pixel's coordinate (taken at the GS's point: gs_to_clip; none with MSAA). The taps stay inside the
// texels the primitive covers (v_rect: a letter's cell of a sheet of letters; past it, the letters beside it bled in),
// and the result inside the four texels around the place (no ring: no colour or alpha they do not have).
bool smooth_2d_applies(float lod) {
    if ((pc.flags & PCR_F_SMOOTH_2D) == 0u || v_rect.x == PCR_NO_RECT || lod > 0.0 || (pc.tex1 & 1u) != 0u ||
        src_kind() == PCR_SRC_SURFACE) {
        return false;
    }
    vec2 m = max(abs(uv_dx), abs(uv_dy));
    return max(m.x, m.y) <= 1.0 + 1.0 / 64.0;
}

vec4 tap_2d(ivec2 at, ivec2 size) {
    if (src_kind() == PCR_SRC_SAMPLED) {
        return sampled_fetch(min(at, sampled_size() - 1)) * 255.0;
    }
    return vec4(texel(at, 0u, size));
}

uvec4 smooth_2d(vec2 uv) {
    ivec2 size = ivec2(int(pc.tex_size & 0xFFFFu), int(pc.tex_size >> 16));
    ivec2 first = ivec2(v_rect & 0xFFFFu), last = ivec2(v_rect >> 16);
    vec2 shift = (pc.flags & PCR_F_MSAA) != 0u ? vec2(0.0) : 0.5 * (uv_dx + uv_dy);
    vec2 q = clamp(uv + shift, vec2(first) + 0.5, vec2(last) + 0.5) - 0.5;
    vec2 i = floor(q);
    vec2 f = q - i;
    vec4 wx = vec4(f.x * (-0.5 + f.x * (1.0 - 0.5 * f.x)), 1.0 + f.x * f.x * (-2.5 + 1.5 * f.x),
                   f.x * (0.5 + f.x * (2.0 - 1.5 * f.x)), f.x * f.x * (-0.5 + 0.5 * f.x));
    vec4 wy = vec4(f.y * (-0.5 + f.y * (1.0 - 0.5 * f.y)), 1.0 + f.y * f.y * (-2.5 + 1.5 * f.y),
                   f.y * (0.5 + f.y * (2.0 - 1.5 * f.y)), f.y * f.y * (-0.5 + 0.5 * f.y));
    vec4 sum = vec4(0.0), lo = vec4(255.0), hi = vec4(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec4 c = tap_2d(clamp(ivec2(i) + ivec2(x - 1, y - 1), first, last), size);
            sum += c * (wx[x] * wy[y]);
            if ((x == 1 || x == 2) && (y == 1 || y == 2)) {
                lo = min(lo, c);
                hi = max(hi, c);
            }
        }
    }
    texture_f = clamp(sum, lo, hi);
    texture_f_kept = true;
    return uvec4(floor(texture_f + 0.5));
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
    if (smooth_2d_applies(lod)) {
        return smooth_2d(uv);
    }
    if (src_kind() == PCR_SRC_SAMPLED) {
        // (the sampler's filters are MMAG's at a level of 0 or below and MMIN's above; its levels and the lod's
        // clamp are the texture's: pcr_canvas.cpp's sampler_for)
        if (lod > 0.0 && bias != 0.0) {
            lod = max(lod + bias, 1.0 / 256.0);
        }
        vec4 t;
        if (lod <= 0.0 || aniso_log2() == 0u || !sampled_anisotropic(uv, lod, t)) {
            t = sampled_lod(uv / vec2(sampled_size()), lod);
        }
        texture_f = t * 255.0;
        texture_f_kept = true;
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
// Colour high (PCR_F_HIGH, pcr_canvas.cpp's set_colour: the colour surfaces 16 bits a channel): the colours' arithmetic
// in floats, without the GS's roundings, alpha still the GS's integers (the alpha test, DATE and the blend's factors as
// exact); the picture is dithered to 8 bits as it is scanned out (pcr_narrow.frag). As the native GS's --native-colour
// high: the fractions the GS's shifts drop are kept, but not the level. Along a gradient the GS's floor takes half a
// step on average, and the picture's level is the PS2's when that is taken too (else a dark scene comes out about a
// step brighter). Whole numbers (to 1/128) are taken as whole.
bvec3 whole(vec3 x) {
    return lessThan(abs(x - round(x)), vec3(1.0 / 128.0));
}

// The GS's floor of a colour interpolated across a primitive: exact where it is a whole number, else less half a step
vec3 gs_floor(vec3 x) {
    return mix(x - 0.5, round(x), whole(x));
}

// What the GS's p * k >> 7 drops on average, for p along a gradient: half a step, less what k's factors of two keep
// exact (k 128 drops nothing; 64 a quarter); for a k that is itself along a gradient, the mean over k
float shift_loss(float k) {
    if (abs(k - round(k)) >= 1.0 / 128.0) {
        return 0.5 - 4.5 / 256.0;
    }
    int ki = int(round(k));
    return ki == 0 ? 0.0 : 0.5 - float(1 << min(findLSB(ki), 7)) / 256.0;
}

// p * k >> 7 as the GS has it where both are whole numbers; else p * k / 128 less what the shift drops on average
vec3 gs_shift7(vec3 p, vec3 k) {
    vec3 x = p * k / 128.0;
    vec3 loss = vec3(shift_loss(k.r), shift_loss(k.g), shift_loss(k.b));
    bvec3 exact = bvec3(uvec3(whole(p)) & uvec3(whole(k)));
    return mix(x - loss, floor(x + 1.0 / 256.0), exact);
}

// blended() in floats, from the destination's colours with their fraction (COLCLAMP's clamp; its wrap stays the GS's
// integers, blended()'s)
vec3 blended_high(uvec4 cs, vec3 hc) {
    if ((pc.alpha & 0x20000u) != 0u && cs.a < 0x80u) {
        return hc;
    }
    vec4 dh = texelFetch(u_dest, ivec2(gl_FragCoord.xy) - pc.dest_offset, 0) * 255.0;
    uint sel_a = pc.alpha & 3u, sel_b = (pc.alpha >> 2) & 3u, sel_c = (pc.alpha >> 4) & 3u, sel_d = (pc.alpha >> 6) & 3u;
    vec3 a = sel_a == 0u ? hc : sel_a == 1u ? dh.rgb : vec3(0.0);
    vec3 b = sel_b == 0u ? hc : sel_b == 1u ? dh.rgb : vec3(0.0);
    vec3 dd = sel_d == 0u ? hc : sel_d == 1u ? dh.rgb : vec3(0.0);
    float ad = (pc.alpha & 0x40000u) != 0u ? 128.0 : round(dh.a);
    float c = sel_c == 0u ? float(cs.a) : sel_c == 1u ? ad : float((pc.alpha >> 8) & 0xFFu);
    return clamp(gs_shift7(a - b, vec3(c)) + dd, 0.0, 255.0);
}

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
#ifdef PCR_RECORDS
    pcr_take_record();
#endif
    if ((pc.flags & PCR_F_TME) != 0u && (aniso_log2() != 0u || (pc.flags & PCR_F_SMOOTH_2D) != 0u)) {
        vec2 uv = texture_uv();
        uv_dx = dFdx(uv);
        uv_dy = dFdy(uv);
    }
#ifndef PCR_EARLY
    if ((pc.flags & PCR_F_DATE) != 0u) {
        uint da = destination().a;
        if (((da >> 7) & 1u) != ((pc.flags & PCR_F_DATM) != 0u ? 1u : 0u)) {
            discard;
        }
    }
#endif
    uvec4 vc = uvec4(v_color + 0.001);
    uvec4 c = vc;
    bool high = (pc.flags & PCR_F_HIGH) != 0u;
    vec3 hc = high ? gs_floor(v_color.rgb) : vec3(0.0); // (colour high: c's colour in floats; c's alpha still used)
    if ((pc.flags & PCR_F_TME) != 0u) {
        uvec4 t = sample_texture();
        vec3 tf = texture_f_kept ? texture_f.rgb : vec3(t.rgb);
        bool tcc = (pc.flags & PCR_F_TCC) != 0u;
        switch ((pc.flags >> PCR_F_TFX_SHIFT) & 3u) {
        case 0u: // MODULATE
            c.rgb = min((t.rgb * vc.rgb) >> 7, uvec3(255u));
            c.a = tcc ? min((t.a * vc.a) >> 7, 255u) : vc.a;
            hc = min(gs_shift7(tf, hc), vec3(255.0));
            break;
        case 1u: // DECAL
            c.rgb = t.rgb;
            c.a = tcc ? t.a : vc.a;
            hc = tf;
            break;
        case 2u: // HIGHLIGHT
            c.rgb = min(((t.rgb * vc.rgb) >> 7) + vc.a, uvec3(255u));
            c.a = tcc ? min(t.a + vc.a, 255u) : vc.a;
            hc = min(gs_shift7(tf, hc) + float(vc.a), vec3(255.0));
            break;
        default: // HIGHLIGHT2
            c.rgb = min(((t.rgb * vc.rgb) >> 7) + vc.a, uvec3(255u));
            c.a = tcc ? t.a : vc.a;
            hc = min(gs_shift7(tf, hc) + float(vc.a), vec3(255.0));
            break;
        }
    }
    if ((pc.flags & PCR_F_FGE) != 0u) {
        uint f = uint(clamp(v_fog, 0.0, 255.0));
        uvec3 fog = uvec3(pc.fogcol & 0xFFu, (pc.fogcol >> 8) & 0xFFu, (pc.fogcol >> 16) & 0xFFu);
        c.rgb = (f * c.rgb + (255u - f) * fog) >> 8;
        float ff = clamp(v_fog, 0.0, 255.0);
        hc = (ff * hc + (255.0 - ff) * vec3(fog)) / 256.0;
    }
#ifndef PCR_EARLY
    uint pass = (pc.flags >> PCR_F_PASS_SHIFT) & 3u;
    if (pass != 0u && alpha_passes(c.a) != (pass == 1u)) {
        discard;
    }
#endif
    uvec3 rgb = (pc.flags & PCR_F_SHADER_BLEND) != 0u ? blended(c) : c.rgb;
    if (high && (pc.flags & PCR_F_SHADER_BLEND) != 0u) {
        // (COLCLAMP's wrap: the GS's integers)
        hc = (pc.alpha & 0x10000u) != 0u ? blended_high(c, hc) : vec3(rgb);
    }
    if ((pc.flags & PCR_F_PREMUL) != 0u) {
        uint k = (pc.flags & PCR_F_C_FIX) != 0u ? pc.clamp_hi >> 24 : c.a;
        rgb = min((rgb * k) >> 7, uvec3(255u));
        hc = min(gs_shift7(hc, vec3(float(k))), vec3(255.0));
    }
    uint a = (pc.flags & PCR_F_FBA) != 0u ? c.a | 0x80u : c.a;
    float alpha_out = (pc.flags & PCR_F_ALPHA_FACTOR) != 0u ? float(c.a) / 128.0 : float(a) / 255.0;
    o_color = vec4((high ? clamp(hc, 0.0, 255.0) : vec3(rgb)) / 255.0, alpha_out);
#ifdef PCR_RECORDS
    o_factor = vec4(0.0);
    if ((pc.factors & 0x80000000u) != 0u) {
        // Vulkan's factors as its blend has them: the source alpha and the constant (FIX / 128) clamped to [0, 1]
        // (a fixed-point target's), the constant alpha 128 / 255 (pcr_canvas.cpp's blend constants)
        float as = clamp(alpha_out, 0.0, 1.0);
        float fix = clamp(float(pc.clamp_hi >> 24) / 128.0, 0.0, 1.0);
        // (the source's factor as it is, f[]; the destination's as one minus it, g[], the blend taking it from one itself
        // (ONE_MINUS_SRC1_ALPHA). Where a one minus is made here, it is one minus the factor as the blend holds it: NVIDIA's
        // blend cuts a factor down to the target's precision, 8 or 16 bits (a source alpha or constant of k / 128 is
        // floor(255 k / 128) / 255 to it in an 8-bit target); given as 1 - As, its pictures came out a value or two off)
        float held = high ? 65535.0 : 255.0;
        float qa = floor(as * held) / held, qfix = floor(fix * held) / held;
        float f[6] = float[6](0.0, 1.0, as, 1.0 - qa, fix, 1.0 - qfix);
        float g[6] = float[6](1.0, 0.0, 1.0 - qa, as, 1.0 - qfix, fix);
        o_factor = vec4(vec3(f[pc.factors & 7u]), g[(pc.factors >> 4) & 7u]);
        if ((pc.flags & PCR_F_ALPHA_FACTOR) != 0u) {
            o_color.a = as * (128.0 / 255.0);
        }
    }
#endif
}
