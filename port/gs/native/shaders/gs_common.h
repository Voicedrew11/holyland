// What every draw of the native GS passes its shaders (port/gs/native/gsn.cpp, GsnPush: the two
// must agree). Colours are the GS's integers, 0-255, and alpha 0x80 is 1.0.
layout(push_constant, std430) uniform Push {
    vec2 inv_size;    // 1 / the target's size in pixels
    float zscale;     // Z to depth: 1 / 2^24, or 1 / 2^32 for PSMZ32
    uint zmax;        // the largest Z the Z buffer's format holds
    uint flags;       // GSN_F_* below
    uint tex_size;    // the texture's width (bits 0-15) and height (16-31) in texels
    uint clamp_lo;    // CLAMP: WMS 0-1, WMT 2-3, MINU 4-13, MAXU 14-23
    uint clamp_hi;    // CLAMP: MINV 0-9, MAXV 10-19
    uint test;        // TEST: ATE 0, ATST 1-3, AREF 4-11, AFAIL 12-13, DATE 14, DATM 15
    uint fogcol;      // FOGCOL: R 0-7, G 8-15, B 16-23
    uint alpha;       // ALPHA: A 0-1, B 2-3, C 4-5, D 6-7, FIX 8-15
    uint texa;        // TEXA: TA0 0-7, AEM 15, TA1 16-23
    uint tex1;        // TEX1: MMAG 0, MMIN 1-3, MXL 4-6, LCM 7, L 8-9
    float lod_k;      // TEX1's K
    ivec2 src_offset; // a texture that is a render target: where in it the texture starts
    ivec2 dst_offset; // the destination copy (shader blending): the frame buffer pixel at its (0, 0)
    uint misc;        // GSN_M_* below
    uint fbmsk;       // FRAME's FBMSK: the bits not written
    uint tscale;      // a texture that is a render target: its pixels a GS pixel is, across (bits 0-15) and down (16-31)
    float lod_bias;   // --native-lod: added to the mipmap level the GS picks (only to which level)
    float zbias;      // --native-layer-z tolerant: added to the pixel's Z (a draw's colour pass)
    float zslope;     // ... and this times the Z's change across a GS pixel, at most zbias (a layer's colour pass)
} pc;

#ifdef ND_STANDALONE
#ifndef SA_GRAZE
#define SA_GRAZE 1.0
#endif
// The scene's slack for a tie's colour pass (gsn_standalone.inc, "Ties"), in Z: pc.zbias, plus
// - pc.zslope >= 0: that times the corner's alpha / 255 (--native-layer-z tolerant's slope term, 255 a Z), or
// - pc.zslope < 0 (a layer, by its real depth): -pc.zslope world units across the surface, as Z there. The corner's
//   alpha is the triangle's Z change across a GS pixel, g = 2^(a / 32) - 1. With Z = cz + az q (q = 1 / the depth),
//   a depth change is (Z - cz) q times as much Z, and a surface whose Z changes g a GS pixel lies at tan t =
//   g sx / (Z - cz) to the view (sx: the screen's GS pixels a unit across at depth 1). Two surfaces e apart across
//   are e / cos t apart in depth: e q sqrt((Z - cz)^2 + (g sx)^2) in Z, the same in the world at every distance.
//   (tan t at most SA_GRAZE: toward a silhouette e / cos t grows without end, and a layer passed over the faces in
//   front of it there: a seam along a tower's top edge against the sky with 3; 1: at most 1.4 times e)
float sa_slack(float zf, float q, float cz, float sx, uint a) {
    if (pc.zslope >= 0.0) {
        return pc.zbias + pc.zslope * float(a) / 255.0;
    }
    float g = exp2(float(a) / 32.0) - 1.0;
    float r = abs(zf - cz);
    float t = min(g * sx, SA_GRAZE * r);
    return pc.zbias - pc.zslope * abs(q) * sqrt(r * r + t * t);
}
#endif

#define GSN_F_TME 0x1u
#define GSN_F_FST 0x2u
#define GSN_F_TFX_SHIFT 2u
#define GSN_F_TCC 0x10u
#define GSN_F_FGE 0x20u
#define GSN_F_SHADER_BLEND 0x40u   // blend here, from a copy of the destination (no fixed-function blend)
#define GSN_F_SRC_SHIFT 8u         // GSN_SRC_*
#define GSN_F_ALPHA_FACTOR 0x1000u // fixed-function blend by As: alpha out is As / 128 (the blend constant puts it back)
#define GSN_F_PASS_SHIFT 13u       // 0: every pixel; 1: those that pass the alpha test; 2: those that fail it
#define GSN_F_DATE 0x8000u         // the destination alpha test, here (shader blending only)
#define GSN_F_PREMUL 0x10000u      // Cs * C >> 7 out (C: ALPHA's As or FIX), for Vulkan to add or take away
#define GSN_F_PREMUL_CEIL 0x20000u // ... rounded up (a subtraction)

#define GSN_SRC_CLUT 0u
#define GSN_SRC_CT32 1u
#define GSN_SRC_CT24 2u
#define GSN_SRC_CT16 3u
#define GSN_SRC_TARGET_CT32 4u
#define GSN_SRC_TARGET_CT24 5u
#define GSN_SRC_TARGET_CT16 6u

#define GSN_M_FBA 0x1u
#define GSN_M_COLCLAMP 0x2u
#define GSN_M_DST16 0x4u  // the frame buffer is 16-bit: colours keep their top 5 bits, alpha its top bit
#define GSN_M_DST24 0x8u  // the frame buffer has no alpha
#define GSN_M_PABE 0x10u
#define GSN_M_SCANMSK_SHIFT 5u // SCANMSK: 2 draws no even lines, 3 no odd ones
#define GSN_M_SCALE_SHIFT 7u   // the target's pixels a GS pixel is down (--native-scale), 3 bits
#define GSN_M_ZCLAMP 0x400u    // gs_zclamp.frag writes the depth (every draw with a Z buffer)
#define GSN_M_SCALE_X_SHIFT 11u // the target's pixels a GS pixel is across (--native-scale), 3 bits
#define GSN_M_ANISO_SHIFT 14u   // --native-aniso: log2 of the most taps along a mipmapped texture's footprint, 3 bits (0: off)
#define GSN_M_HIGH 0x20000u
#define GSN_M_SMOOTH_2D 0x80000u // --native-2d smooth: the 2D's magnified textures read with a cubic at a larger scale
#define GSN_M_RECT_2D 0x100000u  // ... the cubic's taps kept inside each triangle's texels (gs_frag.h's Clut; gs.vert: the triangle's number in v_fog)
#define GSN_M_JITTER 0x200000u  // the scene's frame: the picture's sub-pixel jitter added (Picture's counts.zw, sa_*_jitter.vert: upscaling)
#define GSN_M_ZFINE 0x40000u    // --native-layer-z tolerant: the pixel's Z keeps its fraction (and zbias is added)     // --native-colour high: colours in floats, the target 16-bit (alpha as the GS's)
