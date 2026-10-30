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
    uint tex1;       // TEX1: MMAG 0, MMIN 1-3, MXL 4-6, LCM 7, L 8-9
    float lod_k;     // TEX1's K
    uint levels;     // the texture's levels (mipmaps read: TEX1's MXL + 1 at most)
    ivec2 src_offset; // a texture from a surface: where in the surface's copy (GS pixels) it starts
    ivec2 src_scale;  // ... the surface's pixels a GS pixel is, across and down
    ivec2 place;      // (native draws, pcr_nd000.vert) the frame's place in its surface, GS pixels
} pc;

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

#define PCR_SRC_CT32 0u
#define PCR_SRC_CT24 1u
#define PCR_SRC_CT16 2u
#define PCR_SRC_INDEX 3u
#define PCR_SRC_SURFACE 4u
