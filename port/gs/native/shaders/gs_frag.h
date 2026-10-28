// The native GS's pixels (port/gs/native/gsn.cpp): the GS's texture lookup, texture function, fog,
// alpha test and (for the draws that need it) blending, in the GS's integer arithmetic.
//
// Textures come in two kinds. A texture decoded from VRAM is an integer image of the raw texels (an
// index for PSMT8 and PSMT4, looked up in the CLUT here; the 32, 24 or 16-bit colour); a texture that
// is a render target is the target itself (RGBA8, the GS's bytes). Both are read texel by texel, and
// filtered here as the GS filters (4 bits of fraction), so a CLUT change needs no new texture.
//
// Two shaders are made of it: gs.frag, and gs_zclamp.frag (GSN_ZCLAMP), which writes the depth
// itself as the GS has it per pixel: Z interpolated as the vertices have it, rounded to a whole Z
// (ties to even) and kept within what the Z buffer's format holds. It is used for every draw with a Z
// buffer: a fraction left in would put a second draw over the same plane (ZTST GEQUAL) a hair behind
// the first where the GS has them equal. gs.frag is for the draws without one. gs_zclamp_sample.frag
// is gs_zclamp.frag shaded at every sample (GSN_SAMPLE, below). The gs_*_in*.frag ones read the
// destination from the render pass (GSN_DEST_INPUT, at u_dest below).
#include "gs_common.h"

// gs_zclamp_sample.frag (GSN_SAMPLE, --native-cutout-aa ssaa) is shaded once a sample, not once a
// pixel: its inputs are taken at each sample of a multisampled target
#ifdef GSN_SAMPLE
#define GSN_IN sample noperspective in
#else
#define GSN_IN noperspective in
#endif
layout(location = 0) GSN_IN vec4 v_color;
layout(location = 1) GSN_IN vec3 v_stq;
layout(location = 2) GSN_IN float v_fog;
#ifdef GSN_ZCLAMP
layout(location = 3) GSN_IN float v_z;
#endif

layout(set = 0, binding = 0) uniform usampler2D u_raw;
layout(set = 0, binding = 1) uniform sampler2D u_target;
// (a storage buffer, not a uniform one: a texel's CLUT entry is read at an index of its own, and a uniform
// buffer read at indices that differ across the pixels shaded together is read once for each, one after
// another: about 1.4 ms of the GPU's 5 a picture at the owner's settings, gsn_standalone.inc's "The CLUT")
layout(set = 0, binding = 2, std430) readonly buffer Clut {
    uvec4 entries[64];
    // GSN_M_RECT_2D (--native-2d smooth): after the CLUT, each triangle's texels (gsn.cpp, texel_rect_2d),
    // two a uvec4, the first and last across and down (bits 0-15 and 16-31), the triangle's number in
    // v_fog (gs.vert); only as many as the draw has are bound (the 64 KiB of a uniform buffer at most)
    uvec4 rects[];
} u_clut;
#ifdef GSN_HW_SAMPLER
// --native-sampler hardware (docs/port/PC_RENDERER.md, P1): the texture expanded to RGBA8 with its mip levels
// (gsn_rgba.inc: each texel as texel_f decodes it), the GS's byte values in each channel, read by the GPU's sampler:
// its filtering, mipmaps and anisotropy instead of sample_texture's own
layout(set = 0, binding = 6) uniform sampler2D u_rgba;
#endif
// The destination, for the draws that blend here: a copy of the target taken before the draw (binding
// 3), or (GSN_DEST_INPUT, the gs_*_in*.frag shaders) the copy kept in the render pass, an input
// attachment: the pass's second colour attachment, which holds the target's pixels (a multisampled
// target's resolved, in every sample) where the draw covers them (port/gs/native/gsn.cpp, "Shader
// blending in the render pass")
#if defined(GSN_DEST_INPUT) && GSN_DEST_INPUT == 2
layout(input_attachment_index = 0, set = 0, binding = 4) uniform subpassInputMS u_dest_in;
vec4 dest_pixel() {
    return subpassLoad(u_dest_in, 0);
}
#elif defined(GSN_DEST_INPUT)
layout(input_attachment_index = 1, set = 0, binding = 5) uniform subpassInput u_dest_in;
vec4 dest_pixel() {
    return subpassLoad(u_dest_in);
}
#else
layout(set = 0, binding = 3) uniform sampler2D u_dest;
vec4 dest_pixel() {
    return texelFetch(u_dest, ivec2(gl_FragCoord.xy) - pc.dst_offset, 0);
}
#endif

layout(location = 0) out vec4 o_color;

uint field(uint v, uint shift, uint bits) {
    return (v >> shift) & ((1u << bits) - 1u);
}

ivec4 unpack32(uint v) {
    return ivec4(v & 0xFFu, (v >> 8) & 0xFFu, (v >> 16) & 0xFFu, v >> 24);
}

// TEXA: the alpha of a texel with none of its own (24-bit), or one bit of it (16-bit)
int texa_alpha(ivec3 rgb, bool bit, bool has_bit) {
    if (has_bit && bit) {
        return int(field(pc.texa, 16u, 8u));
    }
    if ((pc.texa & 0x8000u) != 0u && rgb == ivec3(0)) {
        return 0;
    }
    return int(field(pc.texa, 0u, 8u));
}

// A texture that is a render target drawn at a larger scale (--native-scale, --native-widescreen) is
// read at its own pixels: the coordinates are in them, `ts` to a texel (across and down), and wrap as
// the texel they are in does
ivec2 target_scale() {
    if (field(pc.flags, GSN_F_SRC_SHIFT, 4u) < GSN_SRC_TARGET_CT32) {
        return ivec2(1);
    }
    return max(ivec2(int(pc.tscale & 0xFFFFu), int(pc.tscale >> 16)), ivec2(1));
}

// --native-colour high (GSN_M_HIGH): the colours' arithmetic in floats, without the GS's roundings (alpha
// stays the GS's integers: the alpha test, DATE and the blend factors are as exact)
bool high;

// --native-colour high keeps the fractions the GS's shifts drop, but not the level: along a gradient the
// GS's floor takes half a step on average, and the picture's level is the PS2's when that is taken too
// (else a dark scene comes out about a step brighter). Whole numbers (to 1/128) are taken as whole.
bvec3 whole(vec3 x) {
    return lessThan(abs(x - round(x)), vec3(1.0 / 128.0));
}

// The GS's floor of a colour interpolated across a primitive: exact where it is a whole number (the same
// at every corner), else less half a step
vec3 gs_floor(vec3 x) {
    return mix(x - 0.5, round(x), whole(x));
}

// What the GS's p * k >> 7 drops on average, for p along a gradient: half a step, less what k's factors
// of two keep exact (k 128 drops nothing; 64 a quarter; 136, the brightness pass's, 15/32). For a k that
// is itself along a gradient, the mean over k.
float shift_loss(float k) {
    if (abs(k - round(k)) >= 1.0 / 128.0) {
        return 0.5 - 4.5 / 256.0;
    }
    int ki = int(round(k));
    return ki == 0 ? 0.0 : 0.5 - float(1 << min(findLSB(ki), 7)) / 256.0;
}

// p * k >> 7 as the GS has it where both are whole numbers; else p * k / 128 less what the shift drops
// on average (or, `up`, the shift rounded up: plus what it adds)
vec3 gs_shift7(vec3 p, vec3 k, bool up) {
    vec3 x = p * k / 128.0;
    vec3 loss = vec3(shift_loss(k.r), shift_loss(k.g), shift_loss(k.b));
    bvec3 exact = bvec3(uvec3(whole(p)) & uvec3(whole(k)));
    return up ? mix(x + loss, ceil(x - 1.0 / 256.0), exact) : mix(x - loss, floor(x + 1.0 / 256.0), exact);
}

// One texel at integer coordinates, wrapped as CLAMP says, at mip level `level`. Whole numbers, but for a
// texture that is a 16-bit target at --native-colour high: its colours with their fraction.
vec4 texel_f(ivec2 c, int level) {
    ivec2 ts = target_scale();
    ivec2 logical = ivec2(floor(vec2(c) / vec2(ts)));
    ivec2 sub = c - logical * ts;
    c = logical;
    ivec2 size = max(ivec2(int(pc.tex_size & 0xFFFFu), int(pc.tex_size >> 16)) >> level, ivec2(1));
    uint wms = field(pc.clamp_lo, 0u, 2u);
    uint wmt = field(pc.clamp_lo, 2u, 2u);
    ivec2 lo = ivec2(int(field(pc.clamp_lo, 4u, 10u)), int(field(pc.clamp_hi, 0u, 10u)));
    ivec2 hi = ivec2(int(field(pc.clamp_lo, 14u, 10u)), int(field(pc.clamp_hi, 10u, 10u)));
    uvec2 mode = uvec2(wms, wmt);

    for (int i = 0; i < 2; i++) {
        if (mode[i] == 0u) {
            c[i] &= size[i] - 1;
        } else if (mode[i] == 1u) {
            c[i] = clamp(c[i], 0, size[i] - 1);
        } else if (mode[i] == 2u) {
            c[i] = clamp(c[i], lo[i] >> level, hi[i] >> level);
        } else {
            c[i] = (c[i] & lo[i]) | hi[i];
        }
    }

    uint src = field(pc.flags, GSN_F_SRC_SHIFT, 4u);
    if (src >= GSN_SRC_TARGET_CT32) {
        vec4 f = texelFetch(u_target, (c + pc.src_offset) * ts + sub, 0) * 255.0;
        ivec4 t = ivec4(round(f));
        if (src == GSN_SRC_TARGET_CT24) {
            t.a = texa_alpha(t.rgb, false, false);
        } else if (src == GSN_SRC_TARGET_CT16) {
            t.rgb &= 0xF8;
            t.a = texa_alpha(t.rgb, t.a >= 0x80, true);
            return vec4(t);
        }
        return high ? vec4(f.rgb, float(t.a)) : vec4(t);
    }
    uint raw = texelFetch(u_raw, c, level).r;
    if (src == GSN_SRC_CLUT) {
        return vec4(unpack32(u_clut.entries[raw >> 2][raw & 3u]));
    } else if (src == GSN_SRC_CT32) {
        return vec4(unpack32(raw));
    } else if (src == GSN_SRC_CT24) {
        ivec4 t = unpack32(raw);
        t.a = texa_alpha(t.rgb, false, false);
        return vec4(t);
    }
    ivec4 t = ivec4((raw & 0x1Fu) << 3, ((raw >> 5) & 0x1Fu) << 3, ((raw >> 10) & 0x1Fu) << 3, 0);
    t.a = texa_alpha(t.rgb, (raw & 0x8000u) != 0u, true);
    return vec4(t);
}

ivec4 texel(ivec2 c, int level) {
    return ivec4(texel_f(c, level));
}

// v / 2^bits, rounded to nearest, ties to even: the mipmap levels' weighted sum
ivec4 round_shift(ivec4 v, int bits) {
    ivec4 q = v >> bits;
    ivec4 r = v & ((1 << bits) - 1);
    int half_ = 1 << (bits - 1);
    return q + ivec4(greaterThan(r, ivec4(half_))) + ivec4(equal(r, ivec4(half_))) * (q & 1);
}

// At a larger --native-scale: the texture coordinates at the GS pixel's integer point (sample_texture),
// where the PS2 reads its texel, and whether the target is larger than the PS2's
vec2 tc_gs;
bool scaled;
// the texture coordinates' change a pixel across and down (sample_texture; --native-aniso)
vec2 tc_dx, tc_dy;

// The texture at texel coordinates `tc` (level 0's), one level, nearest or bilinear (whole numbers but
// at --native-colour high)
vec4 sample_level(vec2 tc, int level, bool linear) {
    ivec2 ts = target_scale();
    vec2 at = tc / float(1 << level) * vec2(ts);
    // the GS's texel coordinates have 4 bits of fraction; interpolated in floats, a coordinate that is
    // exactly on one comes out a hair under it, which the bias puts back
    if (!linear && scaled && level == 0 && (pc.misc & GSN_M_SMOOTH_2D) != 0u &&
        max(max(abs(tc_dx.x), abs(tc_dx.y)), max(abs(tc_dy.x), abs(tc_dy.y))) <= 1.0 + 1.0 / 64.0) {
        // --native-2d smooth: the 2D's texture (a font, the HUD, a picture) magnified at a larger scale,
        // read with a Catmull-Rom cubic (16 texels) at this pixel's own place: the same picture at every
        // scale, the texels' steps smooth and their edges still sharp. Nearest, the GS's texel k covers
        // the GS pixel whose integer point (the middle of its pixels of the target) is at k: half a GS
        // pixel on from where a filter puts its middle, so the place is moved by that, the letters
        // where the PS2 has them. The taps stay inside the texture (no wrap to its far side), and inside
        // the texels the game gave the triangle (GSN_M_RECT_2D, gsn.cpp's texel_rect_2d): a letter's
        // sprite covers its own cell of a sheet of letters, and taps past it read the letters beside and
        // below it (faint ticks, dots and specks at the letters' edges); at its edges the sprite has its
        // edge texels, as on the PS2. The result is kept within the four texels around the place (no
        // ring: no colour or alpha the texels there do not have, as Catmull-Rom's overshoot made).
        vec2 size = vec2(float(pc.tex_size & 0xFFFFu), float(pc.tex_size >> 16));
        vec2 scale = vec2(float(max(field(pc.misc, GSN_M_SCALE_X_SHIFT, 3u), 1u)), float(max(field(pc.misc, GSN_M_SCALE_SHIFT, 3u), 1u)));
        ivec2 first = ivec2(0), last = ivec2(size) - 1;
        if ((pc.misc & GSN_M_RECT_2D) != 0u) {
            uint t = uint(round(v_fog));
            uvec4 pair = u_clut.rects[t >> 1];
            uvec2 r = (t & 1u) != 0u ? pair.zw : pair.xy;
            first = ivec2(r & 0xFFFFu);
            last = ivec2(r >> 16);
        }
        vec2 q = clamp(tc + 0.5 * (tc_dx * scale.x + tc_dy * scale.y), vec2(first) + 0.5, vec2(last) + 0.5) - 0.5;
        vec2 i = floor(q);
        vec2 f = q - i;
        vec4 wx = vec4(f.x * (-0.5 + f.x * (1.0 - 0.5 * f.x)), 1.0 + f.x * f.x * (-2.5 + 1.5 * f.x),
                       f.x * (0.5 + f.x * (2.0 - 1.5 * f.x)), f.x * f.x * (-0.5 + 0.5 * f.x));
        vec4 wy = vec4(f.y * (-0.5 + f.y * (1.0 - 0.5 * f.y)), 1.0 + f.y * f.y * (-2.5 + 1.5 * f.y),
                       f.y * (0.5 + f.y * (2.0 - 1.5 * f.y)), f.y * f.y * (-0.5 + 0.5 * f.y));
        vec4 sum = vec4(0.0), lo = vec4(255.0), hi = vec4(0.0);
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                vec4 c = texel_f(clamp(ivec2(i) + ivec2(x - 1, y - 1), first, last), 0);
                sum += c * (wx[x] * wy[y]);
                if ((x == 1 || x == 2) && (y == 1 || y == 2)) {
                    lo = min(lo, c);
                    hi = max(hi, c);
                }
            }
        }
        sum = clamp(sum, lo, hi);
        return high ? sum : floor(sum + 0.5);
    }
    ivec2 p = ivec2(floor(at * 16.0 + 1.0 / 64.0));
    if (!linear && scaled) {
        // Read nearest at a larger scale, the texel is the one the PS2 reads at the GS pixel's integer
        // point, and of a texture that is a target at that scale too, of its pixels the one this pixel
        // is over, from where the GS pixel's middle is. So a picture copied a GS pixel to a GS pixel
        // reads its pixels exactly whether its coordinates are on the texels' middles (x + 0.5) or on
        // their edges (x, as the frame's passes have them), and a sprite's first column reads its first
        // texel. (From this pixel's own coordinates, a coordinate on an edge took the texel before for
        // the GS pixel's pixels left of its integer point: the frame's first column and row read the
        // other edge of the texture, and so did the title's pictures where their sprites meet.)
        ivec2 gs = ivec2(floor(tc_gs / float(1 << level) * 16.0 + 1.0 / 64.0)) >> 4;
        return texel_f(gs * ts + ivec2(floor((tc - tc_gs) / float(1 << level) * vec2(ts) + 0.5 * vec2(ts))), level);
    }
    if (!linear) {
        return texel_f(p >> 4, level);
    }
    if (high) {
        // the weights with all their fraction, not the GS's 4 bits, and nothing rounded
        vec2 q = at - 0.5 + 1.0 / 1024.0;
        ivec2 i = ivec2(floor(q));
        vec2 f = q - floor(q);
        vec4 top = mix(texel_f(i, level), texel_f(i + ivec2(1, 0), level), f.x);
        vec4 bottom = mix(texel_f(i + ivec2(0, 1), level), texel_f(i + ivec2(1, 1), level), f.x);
        return mix(top, bottom, f.y);
    }
    p -= 8;
    ivec2 i = p >> 4;
    ivec2 f = p & 15;
    ivec4 c00 = texel(i, level);
    ivec4 c10 = texel(i + ivec2(1, 0), level);
    ivec4 c01 = texel(i + ivec2(0, 1), level);
    ivec4 c11 = texel(i + ivec2(1, 1), level);
    // rounded to nearest, a half up (as paraLLEl-GS has it: a texel a pixel at the GS's half-texel
    // offset makes every pixel such a half, and ties to even left half of them 1 lower)
    return vec4((c00 * (16 - f.x) * (16 - f.y) + c10 * f.x * (16 - f.y) + c01 * (16 - f.x) * f.y + c11 * f.x * f.y + 128) >> 8);
}

// A mipmapped texture at level `lod` (clamped already): the nearer level or the two around it (whole
// numbers but at --native-colour high)
vec4 sample_mip(vec2 tc, float lod, int mxl, uint mmin, bool linear) {
    if (mmin == 2u || mmin == 4u) {
        return sample_level(tc, min(int(floor(lod + 0.5)), mxl), linear);
    }
    int l0 = int(floor(lod));
    int l1 = min(l0 + 1, mxl);
    if (high) {
        return mix(sample_level(tc, l0, linear), sample_level(tc, l1, linear), fract(lod));
    }
    int w = int(floor(fract(lod) * 16.0));
    return vec4(round_shift(ivec4(sample_level(tc, l0, linear)) * (16 - w) + ivec4(sample_level(tc, l1, linear)) * w, 4));
}

#ifdef GSN_HW_SAMPLER
vec4 sample_texture() {
    vec2 size = vec2(float(pc.tex_size & 0xFFFFu), float(pc.tex_size >> 16));
    vec2 uv = (pc.flags & GSN_F_FST) != 0u ? v_stq.xy / size : v_stq.xy / v_stq.z;
    vec4 t = texture(u_rgba, uv, pc.lod_bias) * 255.0;
    return high ? t : floor(t + 0.5);
}
#else
vec4 sample_texture() {
    vec2 size = vec2(float(pc.tex_size & 0xFFFFu), float(pc.tex_size >> 16));
    vec2 tc = (pc.flags & GSN_F_FST) != 0u ? v_stq.xy : v_stq.xy / v_stq.z * size;
    // the GS pixel's integer point: the middle of its pixels of the target, (x + 0.5) scale (gs.vert)
    vec2 scale = vec2(float(max(field(pc.misc, GSN_M_SCALE_X_SHIFT, 3u), 1u)), float(max(field(pc.misc, GSN_M_SCALE_SHIFT, 3u), 1u)));
    vec2 to_gs = (floor(floor(gl_FragCoord.xy) / scale) + 0.5) * scale - (floor(gl_FragCoord.xy) + 0.5);
    tc_dx = dFdxFine(tc);
    tc_dy = dFdyFine(tc);
    tc_gs = tc + tc_dx * to_gs.x + tc_dy * to_gs.y;
    scaled = scale != vec2(1.0);
    uint mmag = field(pc.tex1, 0u, 1u);
    uint mmin = field(pc.tex1, 1u, 3u);
    int mxl = int(field(pc.tex1, 4u, 3u));
    float lod = pc.lod_k;
    if (field(pc.tex1, 7u, 1u) == 0u) {
        lod += log2(1.0 / abs(v_stq.z)) * float(1u << field(pc.tex1, 8u, 2u));
    }
    if (lod <= 0.0 || mxl == 0 && mmin < 2u) {
        return sample_level(tc, 0, lod <= 0.0 ? mmag == 1u : mmin == 1u);
    }
    if (mmin < 2u) {
        return sample_level(tc, 0, mmin == 1u);
    }
    bool linear = mmin == 4u || mmin == 5u;
    // (--native-lod moves which level, never the choice between MMAG and MMIN above)
    lod = clamp(lod + pc.lod_bias, 0.0, float(mxl));
    uint aniso = field(pc.misc, GSN_M_ANISO_SHIFT, 3u);
    if (aniso != 0u) {
        // --native-aniso: the GS picks the level from the distance alone, the same along a surface and
        // across it, so a floor or wall seen at a grazing angle can get a level coarser than its texels
        // across the pixel want. The pixel's footprint on the texture (its change across and down, in
        // level 0's texels) is long: n reads spread along its long axis (n its length over its width, at
        // most the switch's), each at the level of the long axis over n (about the short axis's), never
        // coarser than the GS's level, averaged. Where that is no finer than the GS's level (a footprint
        // about as long as wide, or the GS's level fine enough already, as at a larger --native-scale
        // with --native-lod scale), the texture is read as without it.
        float lx = length(tc_dx);
        float ly = length(tc_dy);
        float lmax = max(lx, ly);
        int n = min(int(ceil(lmax / max(min(lx, ly), 1e-8) - 0.01)), 1 << aniso);
        float alod = clamp(log2(max(lmax / float(n), 1e-8)), 0.0, lod);
        if (n > 1 && alod < lod - 1.0 / 16.0) {
            vec2 major = lx > ly ? tc_dx : tc_dy;
            vec4 sum = vec4(0.0);
            for (int i = 0; i < n; i++) {
                sum += sample_mip(tc + major * ((float(i) + 0.5) / float(n) - 0.5), alod, mxl, mmin, linear);
            }
            // (the mean, rounded at the GS's precision; at --native-colour high with its fraction)
            return high ? sum / float(n) : vec4((ivec4(sum) + n / 2) / n);
        }
    }
    return sample_mip(tc, lod, mxl, mmin, linear);
}
#endif

bool alpha_test(int a) {
    int ref = int(field(pc.test, 4u, 8u));
    switch (field(pc.test, 1u, 3u)) {
    case 0u: return false;
    case 1u: return true;
    case 2u: return a < ref;
    case 3u: return a <= ref;
    case 4u: return a == ref;
    case 5u: return a >= ref;
    case 6u: return a > ref;
    default: return a != ref;
    }
}

void main() {
#ifdef GSN_ZCLAMP
    {
        // (--native-layer-z tolerant, GSN_M_ZFINE: the fraction kept, and the colour pass's tolerance)
        float z = min(v_z, float(pc.zmax));
        if ((pc.misc & GSN_M_ZFINE) == 0u) {
            z = roundEven(z);
        }
        // (a layer's colour pass: also zslope times the Z's change across a GS pixel, the larger of
        // across and down, at most zbias more: its mesh's vertices are a fraction of a pixel off the
        // surface's)
        float sx = float(max(field(pc.misc, GSN_M_SCALE_X_SHIFT, 3u), 1u));
        float sy = float(max(field(pc.misc, GSN_M_SCALE_SHIFT, 3u), 1u));
        float slope = max(abs(dFdx(v_z)) * sx, abs(dFdy(v_z)) * sy);
        gl_FragDepth = min(z + pc.zbias + min(pc.zslope * slope, pc.zbias), float(pc.zmax)) * pc.zscale;
    }
#endif
    high = (pc.misc & GSN_M_HIGH) != 0u;
    // GS colours are integers; a colour that is the same at every corner comes through exactly
    ivec4 cf = ivec4(floor(v_color + 1.0 / 256.0));
    ivec4 c = cf;
    // --native-colour high: the colour as well in floats, hc (c's alpha is still the one used)
    vec3 hc = high ? gs_floor(v_color.rgb) : vec3(0.0);

    if ((pc.flags & GSN_F_TME) != 0u) {
        vec4 ctf = sample_texture();
        ivec4 ct = ivec4(round(ctf));
        uint tfx = field(pc.flags, GSN_F_TFX_SHIFT, 2u);
        bool tcc = (pc.flags & GSN_F_TCC) != 0u;
        if (tfx == 0u) { // MODULATE
            c.rgb = min((cf.rgb * ct.rgb) >> 7, ivec3(255));
            c.a = tcc ? min((cf.a * ct.a) >> 7, 255) : cf.a;
            hc = min(gs_shift7(ctf.rgb, hc, false), vec3(255.0));
        } else if (tfx == 1u) { // DECAL
            c.rgb = ct.rgb;
            c.a = tcc ? ct.a : cf.a;
            hc = ctf.rgb;
        } else if (tfx == 2u) { // HIGHLIGHT
            c.rgb = min(((cf.rgb * ct.rgb) >> 7) + cf.a, ivec3(255));
            c.a = tcc ? min(ct.a + cf.a, 255) : cf.a;
            hc = min(gs_shift7(ctf.rgb, hc, false) + float(cf.a), vec3(255.0));
        } else { // HIGHLIGHT2
            c.rgb = min(((cf.rgb * ct.rgb) >> 7) + cf.a, ivec3(255));
            c.a = tcc ? ct.a : cf.a;
            hc = min(gs_shift7(ctf.rgb, hc, false) + float(cf.a), vec3(255.0));
        }
    }
    if ((pc.flags & GSN_F_FGE) != 0u) {
        // F rounded, and the blend towards FOGCOL rounded too (as paraLLEl-GS has it, from tests on the
        // hardware: F 255, FOGCOL 128 and a colour of 128 give 128, not 127)
        int f = clamp(int(roundEven(v_fog)), 0, 255);
        ivec3 fog = ivec3(unpack32(pc.fogcol).rgb);
        c.rgb = ivec3(floor(vec3(fog) + vec3((c.rgb - fog) * f) / 256.0 + 0.5));
        hc = vec3(fog) + (hc - vec3(fog)) * clamp(v_fog, 0.0, 255.0) / 256.0;
    }

    uint scanmsk = field(pc.misc, GSN_M_SCANMSK_SHIFT, 2u);
    uint line = uint(gl_FragCoord.y) / max(field(pc.misc, GSN_M_SCALE_SHIFT, 3u), 1u);
    if (scanmsk >= 2u && (line & 1u) == scanmsk - 2u) {
        discard;
    }

    uint pass = field(pc.flags, GSN_F_PASS_SHIFT, 2u);
    if (pass != 0u && alpha_test(c.a) != (pass == 1u)) {
        discard;
    }

    if ((pc.flags & GSN_F_SHADER_BLEND) != 0u) {
        ivec4 d = ivec4(round(dest_pixel() * 255.0));
        if ((pc.misc & GSN_M_DST16) != 0u) {
            d.rgb &= 0xF8;
            d.a = d.a >= 0x80 ? 0x80 : 0;
        }
        if ((pc.flags & GSN_F_DATE) != 0u && ((d.a >> 7) & 1) != int(field(pc.test, 15u, 1u))) {
            discard;
        }
        ivec4 o = c;
        if ((pc.misc & GSN_M_PABE) == 0u || c.a >= 0x80) {
            uint sa = field(pc.alpha, 0u, 2u), sb = field(pc.alpha, 2u, 2u);
            uint sc = field(pc.alpha, 4u, 2u), sd = field(pc.alpha, 6u, 2u);
            ivec3 a = sa == 0u ? c.rgb : sa == 1u ? d.rgb : ivec3(0);
            ivec3 b = sb == 0u ? c.rgb : sb == 1u ? d.rgb : ivec3(0);
            // (a 24-bit frame buffer has no alpha: its Ad is 1.0)
            int ad = (pc.misc & GSN_M_DST24) != 0u ? 0x80 : d.a;
            int k = sc == 0u ? c.a : sc == 1u ? ad : int(field(pc.alpha, 8u, 8u));
            ivec3 dd = sd == 0u ? c.rgb : sd == 1u ? d.rgb : ivec3(0);
            o.rgb = (((a - b) * k) >> 7) + dd;
        }
        o.rgb = (pc.misc & GSN_M_COLCLAMP) != 0u ? clamp(o.rgb, 0, 255) : o.rgb & 0xFF;
        if ((pc.misc & GSN_M_FBA) != 0u) {
            o.a |= 0x80;
        }
        if ((pc.misc & GSN_M_DST16) != 0u) {
            o.rgb &= 0xF8;
            o.a &= 0x80;
        }
        if ((pc.misc & GSN_M_DST24) != 0u) {
            o.a = d.a;
        }
        uint m = pc.fbmsk;
        uint ou = uint(o.r) | (uint(o.g) << 8) | (uint(o.b) << 16) | (uint(o.a & 0xFF) << 24);
        uint du = uint(d.r) | (uint(d.g) << 8) | (uint(d.b) << 16) | (uint(d.a) << 24);
        o_color = vec4(unpack32((ou & ~m) | (du & m))) / 255.0;
        if (high && (pc.misc & GSN_M_COLCLAMP) != 0u && m == 0u) {
            // the same blend in floats, from the destination's colours with their fraction (with FBMSK
            // or without COLCLAMP, which wraps, the GS's integers above)
            vec3 dh = dest_pixel().rgb * 255.0;
            vec3 oh = hc;
            if ((pc.misc & GSN_M_PABE) == 0u || c.a >= 0x80) {
                uint sa = field(pc.alpha, 0u, 2u), sb = field(pc.alpha, 2u, 2u);
                uint sc = field(pc.alpha, 4u, 2u), sd = field(pc.alpha, 6u, 2u);
                vec3 a = sa == 0u ? hc : sa == 1u ? dh : vec3(0.0);
                vec3 b = sb == 0u ? hc : sb == 1u ? dh : vec3(0.0);
                int ad = (pc.misc & GSN_M_DST24) != 0u ? 0x80 : d.a;
                int k = sc == 0u ? c.a : sc == 1u ? ad : int(field(pc.alpha, 8u, 8u));
                vec3 dd = sd == 0u ? hc : sd == 1u ? dh : vec3(0.0);
                oh = gs_shift7(a - b, vec3(float(k)), false) + dd;
            }
            o_color.rgb = clamp(oh, 0.0, 255.0) / 255.0;
        }
        return;
    }

    int a = c.a;
    if ((pc.misc & GSN_M_FBA) != 0u) {
        a |= 0x80;
    }
    if ((pc.misc & GSN_M_DST16) != 0u) {
        c.rgb &= 0xF8;
        a &= 0x80;
    }
    if ((pc.flags & GSN_F_PREMUL) != 0u) {
        int k = field(pc.alpha, 4u, 2u) == 0u ? c.a : int(field(pc.alpha, 8u, 8u));
        c.rgb = min((c.rgb * k + ((pc.flags & GSN_F_PREMUL_CEIL) != 0u ? 127 : 0)) >> 7, ivec3(255));
        hc = min(gs_shift7(hc, vec3(float(k)), (pc.flags & GSN_F_PREMUL_CEIL) != 0u), vec3(255.0));
    }
    o_color.rgb = high ? hc / 255.0 : vec3(c.rgb) / 255.0;
    o_color.a = (pc.flags & GSN_F_ALPHA_FACTOR) != 0u ? float(c.a) / 128.0 : float(a) / 255.0;
}
