#version 450
#extension GL_GOOGLE_include_directive : require
// The scene renderer's DATE (port/gs/scene/scn_draw.cpp, docs/plans/scene-renderer-design.md "DATE"): each shadow
// triangle writes its number in the frame's shadows (the draw's first, pc.record, plus the triangle's in the draw)
// into an R32F image the frame's size with a MIN blend, after its depth test, which the GPU makes before this shader.
// What is left at a pixel is the first triangle the GS would have drawn there; pcr_prim.frag built with SCN_FIRST_DRAW
// then draws each triangle only where it is that one. The number n is written as n / 2^24, exact in float below 2^24,
// so that the image is cleared to 1.0 (no triangle): a value the GPU clears fast, where 3e38 was cleared by a draw.
// (With exact Z the depth tested is the shadow's own exact Z, as its darkening pass tests it: pcr_exact_z.h.)
#include "pcr_common.h"
#ifdef PCR_EXACT_Z
#include "pcr_exact_z.h"
#endif

layout(location = 0) out float o_first;

void first() {
#ifdef PCR_EXACT_Z
    pcr_whole_depth();
#endif
    o_first = float(pc.record + uint(gl_PrimitiveID)) * (1.0 / 16777216.0);
}

void main() {
#if defined(PCR_RECORDS) && defined(PCR_DRAWS_APART)
    // (the draw's fields from its record, a record at a time: pcr_common.h)
    PCR_EACH_RECORD(first());
#else
    first();
#endif
}
