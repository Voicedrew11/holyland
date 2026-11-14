// What every GS primitive the PC renderer draws passes its shaders (port/gs/pc/pcr_canvas.cpp, PrimPush: the two
// must agree). Colours are the GS's integers, 0-255, and alpha 0x80 is 1.0.
#ifndef PCR_RECORDS
layout(push_constant, std430) uniform Push {
    vec2 inv_size;   // 1 / the target's size in GS pixels
    float zscale;    // Z to depth: 1 / 2^24, or 1 / 2^32 for a 32-bit Z buffer
    uint zmax;       // the largest Z the Z buffer's format holds
    uint flags;      // PCR_F_* below
    uint tex_size;   // the texture's level 0: width (bits 0-15) and height (16-31) in texels
    uint clamp_lo;   // CLAMP: WMS 0-1, WMT 2-3, MINU 4-13, MAXU 14-23
    uint clamp_hi;   // CLAMP: MINV 0-9, MAXV 10-19
    uint test;       // TEST: ATST 0-2, AREF 3-10
    uint fogcol;     // FOGCOL: R 0-7, G 8-15, B 16-23
    uint texa;       // TEXA: TA0 0-7, AEM 15, TA1 16-23
    uint tex1;       // TEX1: MMAG 0, MMIN 1-3, MXL 4-6, LCM 7, L 8-9; log2 of the anisotropy 10-12, the texture
                     // detail's bias to the GS's level 16-31 (signed, 256ths; pcr_canvas.cpp's sampling_bits)
    float lod_k;     // TEX1's K
    uint levels;     // the texture's levels (mipmaps read: TEX1's MXL + 1 at most)
    ivec2 src_offset; // a texture from a surface: where in the surface's copy (GS pixels) it starts
    ivec2 src_scale;  // ... the surface's pixels a GS pixel is, across and down (the canvas's scale: gs_to_clip's too)
    ivec2 place;      // (native draws, pcr_nd000.vert) the frame's place in its surface, GS pixels
    ivec2 dest_offset; // (u_dest) the target's pixel the copy of the destination starts at
    uint alpha;        // (PCR_F_SHADER_BLEND) ALPHA: A 0-1, B 2-3, C 4-5, D 6-7, FIX 8-15; COLCLAMP 16, PABE 17,
                       // a 24-bit frame 18
    uint clut_at;      // (PCR_SRC_INDEX, a kept walk's batch) the CLUT's first colour in the walk's CLUTs (pcr_prim.h's
                       // u_walk_cluts); PCR_NO_WALK_CLUT: set 1's u_clut
    uint table;        // (a kept walk's batch) its texture's place in the walk's table, u_table (bits 0-15; PCR_NO_TABLE:
                       // set 1's u_texels and u_sampled), and its sampler's in u_samplers (16-19)
    uint record;       // (PCR_RECORDS) the run's first record in u_records: its draw's is this + gl_DrawID
    uint factors;      // (PCR_RECORDS) the blend's colour factors given by the shader (pcr_prim.h's o_factor)
    uint trace_xy;     // (developers' pixel trace, pcr_prim.h's trace) the target's pixel traced: x | y << 16;
                       // PCR_NO_TRACE none
    uint trace_draw;   // ... the draw its fragments are recorded as (the caller's number)
    uint unused;       // (a record's size a multiple of 8 bytes: pcr_canvas.cpp's PrimPush)
} pc;
#else
// (PCR_RECORDS: the programs a kept walk's runs are drawn with, pcr_canvas.cpp's draw_runs) The same fields, a draw's
// in a record of the picture's (u_records), one an indirect draw: its run's first (the push constants' record) plus
// gl_DrawID; the fragment stage has it from the vertex stage. The push constants as above, so that the layouts agree.
#define PCR_PUSH_FIELDS \
    vec2 inv_size; float zscale; uint zmax; uint flags; uint tex_size; uint clamp_lo; uint clamp_hi; uint test; \
    uint fogcol; uint texa; uint tex1; float lod_k; uint levels; ivec2 src_offset; ivec2 src_scale; ivec2 place; \
    ivec2 dest_offset; uint alpha; uint clut_at; uint table; uint record; \
    uint factors; uint trace_xy; uint trace_draw; uint unused;
struct PcrRecord {
    PCR_PUSH_FIELDS
};
layout(push_constant, std430) uniform Push {
    PCR_PUSH_FIELDS
} pcr_push;
layout(set = 1, binding = 7, std430) readonly buffer Records {
    PcrRecord r[];
} u_records;
#ifdef PCR_STAGE_VERTEX
#extension GL_ARB_shader_draw_parameters : require
#define PCR_RECORD (pcr_push.record + uint(gl_DrawIDARB))
#define pc u_records.r[PCR_RECORD]
// (the fields the fragment stage reads, flat: read there from the record, each a load a pixel, the pictures between
// took the GPU 6% longer)
layout(location = 4) flat out uvec4 v_record[6];
void pcr_pass_record() {
    v_record[0] = uvec4(pc.flags, pc.tex_size, pc.clamp_lo, pc.clamp_hi);
    v_record[1] = uvec4(pc.test, pc.fogcol, pc.texa, pc.tex1);
    v_record[2] = uvec4(floatBitsToUint(pc.lod_k), pc.levels, pc.alpha, pc.clut_at);
    v_record[3] = uvec4(pc.table, pc.factors, uvec2(pc.src_offset));
    v_record[4] = uvec4(uvec2(pc.src_scale), uvec2(pc.dest_offset));
    v_record[5] = uvec4(pc.trace_xy, pc.trace_draw, floatBitsToUint(pc.zscale), pc.zmax);
}
#else
layout(location = 4) flat in uvec4 v_record[6];
PcrRecord pcr_record;
void pcr_take_record() {
    pcr_record.flags = v_record[0].x;
    pcr_record.tex_size = v_record[0].y;
    pcr_record.clamp_lo = v_record[0].z;
    pcr_record.clamp_hi = v_record[0].w;
    pcr_record.test = v_record[1].x;
    pcr_record.fogcol = v_record[1].y;
    pcr_record.texa = v_record[1].z;
    pcr_record.tex1 = v_record[1].w;
    pcr_record.lod_k = uintBitsToFloat(v_record[2].x);
    pcr_record.levels = v_record[2].y;
    pcr_record.alpha = v_record[2].z;
    pcr_record.clut_at = v_record[2].w;
    pcr_record.table = v_record[3].x;
    pcr_record.factors = v_record[3].y;
    pcr_record.src_offset = ivec2(v_record[3].zw);
    pcr_record.src_scale = ivec2(v_record[4].xy);
    pcr_record.dest_offset = ivec2(v_record[4].zw);
    pcr_record.trace_xy = v_record[5].x;
    pcr_record.trace_draw = v_record[5].y;
    pcr_record.zscale = uintBitsToFloat(v_record[5].z);
    pcr_record.zmax = v_record[5].w;
    pcr_record.record = pcr_push.record; // (the push constants read here too, so that both stages have them, as the
                                         // other programs' do)
}
#define pc pcr_record
#endif
#endif

#define PCR_NO_TABLE 0xFFFFu

#define PCR_NO_WALK_CLUT 0xFFFFFFFFu

#define PCR_F_TME 0x1u
#define PCR_F_FST 0x2u
#define PCR_F_TFX_SHIFT 2u        // 2 bits
#define PCR_F_TCC 0x10u
#define PCR_F_FGE 0x20u
#define PCR_F_SRC_SHIFT 6u        // PCR_SRC_*, 3 bits
#define PCR_F_PASS_SHIFT 9u       // 0: every pixel; 1: those that pass the alpha test; 2: those that fail it
#define PCR_F_FBA 0x800u          // the alpha written has bit 7 set
#define PCR_F_PREMUL 0x1000u      // the colour out is Cs * C / 128 (Vulkan adds or takes it away)
#define PCR_F_C_FIX 0x2000u       // ... C is ALPHA's FIX (in clamp_hi's bits 24-31), else As
#define PCR_F_ALPHA_FACTOR 0x4000u // the alpha out is As / 128, the blend's factor (the blend constant puts it back)
#define PCR_F_DATE 0x8000u        // TEST's DATE: drawn only where the destination's alpha bit 7 (u_dest) is DATM
#define PCR_F_DATM 0x10000u
#define PCR_F_SHADER_BLEND 0x20000u // ALPHA's (A - B) * C + D here, from u_dest (a blend with no Vulkan form)
#define PCR_F_SURFACE_CT16 0x40000u // (PCR_SRC_SURFACE) a 16-bit surface's texture: 5 bits a colour, TEXA's alphas
#define PCR_F_MSAA 0x80000u        // the target is multisampled (gs_to_clip)
#define PCR_F_SMOOTH_2D 0x100000u  // the 2D mode's smooth: the 2D's magnified textures read with a cubic (pcr_prim.h)
#define PCR_F_HIGH 0x200000u       // colour high: the colours' arithmetic in floats (pcr_prim.h), 16-bit surfaces
#define PCR_F_DECAL 0x400000u      // (developers' views: the trace, the decal losses) a decal (pcr_canvas.cpp's is_decal)
#define PCR_F_DECAL_LOSS 0x800000u // (developers' decal losses) the colour out is 16 of magenta: where the decal lost
#define PCR_F_DECAL_LOSS_FAR 0x1000000u // ... and its depth PCR_DECAL_LOSS_MARGIN nearer: where it lost by more
#define PCR_F_EXACT_Z 0x2000000u   // the GS's whole Z at each pixel (pcr_prim.h's PCR_EXACT_Z): the corners' depth a bound
#define PCR_F_LAYERS 0x4000000u    // (with PCR_F_EXACT_Z) tolerant layers: Z's fraction kept (pcr_prim.h's pcr_exact_depth),
                                   // a native draw's corners in the order of their places (pcr_nd000.vert)
#define PCR_F_LAYER_TEST 0x8000000u // ... and a draw's colour tested half a Z nearer
#define PCR_F_LAYER_FINE 0x10000000u // ... a sixteenth of a Z (a cut-out's)
#define PCR_F_LAYER_WIDE 0x20000000u // ... a Z and half its slope across a GS pixel, up to a Z more (a decal's)
#define PCR_F_LAYER_Z 0x40000000u    // ... a layer's pass that writes its Z alone: a native draw's corners unlit
#define PCR_DECAL_LOSS_MARGIN 16u
#define PCR_NO_TRACE 0xFFFFFFFFu

// (the developers' decal losses: Z PCR_DECAL_LOSS_MARGIN nearer)
uint pcr_loss_margin() {
    return (pc.flags & PCR_F_DECAL_LOSS_FAR) != 0u ? PCR_DECAL_LOSS_MARGIN : 0u;
}

// Exact Z (PCR_F_EXACT_Z). The GS tests and writes Z as a whole number at each pixel, interpolated from its corners' Z
// and clamped to what the Z buffer holds:
// a mesh drawn over another (a decal: moss on a wall, a puddle on a floor, Z tested GEQUAL) has the same Z as it at
// most pixels, a tie, which the later draw wins. The GPU's depth, interpolated with its fraction, puts two meshes cut into
// different triangles a hair apart, and one of them loses over whole triangles. So the fragment stage (pcr_prim.h's
// pcr_exact_depth) works out the pixel's whole Z as the GS (and paraLLEl-GS) does, and writes it as its depth; ties are
// then ties, and the depth test's GEQUAL lets the later draw win, as on the GS.
// The GS's Z at a pixel is a corner's whole Z plus the rounded interpolation of the corners' differences from it. Each
// corner hands down a whole Z its triangle shares (zref: the first corner's as the frame has it, given to every corner
// by the canvas, or worked out by each corner of a native draw), flat, and its own Z less that, with its fraction
// (v_zrel), interpolated: small numbers, so the float keeps them as exactly as the GS's own differences. Any whole Z
// the corners share gives the same rounding, so a corner moved in a picture between needs no other corner's move.
// That is the barycentric extension's result without it: every GPU draws exact Z.
// In a picture between a corner's Z is moved (gif_moves.h, vu1_*_maths.h) and keeps its fraction (zfrac): rounded at
// each corner, the moss's corners and the wall's under it moved a Z apart here and there, and the moss lost over whole
// triangles. The depth the corners give the GPU is only a bound, a little nearer than any whole Z the triangle's pixels
// have (4 of 2^24 of the depth: more than the half Z of rounding and the interpolation's error), for the GPU's early
// tests (pcr_prim.h's layout(depth_less)); past 1 near the Z buffer's largest Z, clamped there (Granite's depth clamp:
// cmake/granite_depth_bounds.cmake). Without exact Z, the corners' Z clamped and interpolated by the GPU, its fraction
// kept.
#ifdef PCR_STAGE_VERTEX
layout(location = 10) flat out uint v_zref;
layout(location = 11) noperspective out float v_zrel;
#endif
float pcr_depth(uint z, float zfrac, uint zref) {
#ifdef PCR_STAGE_VERTEX
    v_zref = zref;
    v_zrel = float(int(z - zref)) + zfrac;
#endif
    if ((pc.flags & PCR_F_EXACT_Z) != 0u) {
        return (float(z) + zfrac + float(pcr_loss_margin())) * pc.zscale + 4.0 / 16777216.0;
    }
    return float(min(z, pc.zmax) + pcr_loss_margin()) * pc.zscale;
}

#define PCR_NO_RECT 0xFFFFFFFFu    // (v_rect: no texels known for the primitive)

#define PCR_SRC_CT32 0u
#define PCR_SRC_CT24 1u
#define PCR_SRC_CT16 2u
#define PCR_SRC_INDEX 3u
#define PCR_SRC_SURFACE 4u
#define PCR_SRC_SAMPLED 5u // the GPU's sampler on a texture of its colours (u_sampled)

// A place in the target (GS pixels, XYOFFSET taken off) in Vulkan's clip space. The GS draws a pixel when its integer
// point is inside a primitive, Vulkan when a pixel's middle is: every vertex moves half of one of the target's own
// pixels (1 / src_scale of a GS pixel), so the first of a GS pixel's pixels at the canvas's scale is tested at the GS's
// own point and a primitive covers the same GS pixels at any scale. (Moved half a GS pixel, as at 1x, a 4x primitive
// was drawn half a GS pixel up and left: a target drawn whole kept its first two columns and rows undrawn, and a
// 128-wide texture the game draws, repeated over a floor, showed lines where it repeats; 2026-11-08.)
// With MSAA (PCR_F_MSAA) a pixel is its samples' area, not its middle: nothing moves, so a GS pixel is its scale's
// pixels whole (from its integer point to the next) and a primitive on whole GS pixels covers every sample of them (moved
// half a target pixel, a sprite's edge cut each pixel on it in two: half its samples drawn, a line at every edge).
vec2 gs_to_clip(vec2 xy) {
    if ((pc.flags & PCR_F_MSAA) != 0u) {
        return xy * pc.inv_size * 2.0 - 1.0;
    }
    return (xy + 0.5 / vec2(pc.src_scale)) * pc.inv_size * 2.0 - 1.0;
}
