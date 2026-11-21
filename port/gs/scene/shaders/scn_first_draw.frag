#version 450
#extension GL_GOOGLE_include_directive : require
// The scene renderer's DATE (port/gs/scene/scn_draw.cpp, docs/plans/scene-renderer-design.md "DATE"): each shadow
// triangle writes its number in the frame's shadows (the draw's first, pc.record, plus the triangle's in the draw)
// into an R32F image the frame's size with a MIN blend, after its depth test, which the GPU makes before this shader.
// What is left at a pixel is the first triangle the GS would have drawn there; pcr_prim.frag built with SCN_FIRST_DRAW
// then draws each triangle only where it is that one. The number n is written as n / 2^24, exact in float below 2^24,
// so that the image is cleared to 1.0 (no triangle): a value the GPU clears fast, where 3e38 was cleared by a draw.
#include "pcr_common.h"

layout(location = 0) out float o_first;

void main() {
#ifdef PCR_RECORDS
    pcr_take_record(); // (the draw's fields from its record: pcr_common.h)
#endif
    o_first = float(pc.record + uint(gl_PrimitiveID)) * (1.0 / 16777216.0);
}
