#version 450
#extension GL_GOOGLE_include_directive : require
// DATE's set-up (port/gs/pc/pcr_canvas.cpp, "DATE by the stencil"): over the rectangle a DATE batch draws in, the
// stencil set where the target's alpha bit 7 is TEST's DATM (the pixels the GS lets the batch draw), the target read in
// the render pass. No colour is written.
#include "pcr_common.h"

layout(input_attachment_index = 0, set = 0, binding = 5) uniform subpassInput u_target;

void main() {
    uint da = uint(round(subpassLoad(u_target).a * 255.0));
    if (((da >> 7) & 1u) != ((pc.flags & PCR_F_DATM) != 0u ? 1u : 0u)) {
        discard;
    }
}
