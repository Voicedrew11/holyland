// What every GS primitive the PC renderer draws passes its shaders (port/gs/pc/pcr_canvas.cpp, PrimPush: the two
// must agree). Colours are the GS's integers, 0-255, and alpha 0x80 is 1.0.
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
    uint trace_xy;     // (developers' pixel trace, pcr_prim.h's trace) the target's pixel traced: x | y << 16;
                       // PCR_NO_TRACE none
    uint trace_draw;   // ... the draw its fragments are recorded as (the caller's number)
    float ztie;        // Z added to a GEQUAL draw's: its place among its Z buffer's since its clear (pcr_canvas.cpp's
                       // number_tie), so that a later draw wins a tie, as the GS's whole Z has it
} pc;

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
#define PCR_DECAL_LOSS_MARGIN 16.0
#define PCR_NO_TRACE 0xFFFFFFFFu

// The depth of Z (clamped to the Z buffer's largest), pc.ztie nearer
float pcr_depth(uint z) {
    return (float(min(z, pc.zmax)) + pc.ztie +
            ((pc.flags & PCR_F_DECAL_LOSS_FAR) != 0u ? PCR_DECAL_LOSS_MARGIN : 0.0)) *
           pc.zscale;
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
