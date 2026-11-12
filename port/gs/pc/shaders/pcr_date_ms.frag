#version 450
#extension GL_GOOGLE_include_directive : require
// DATE's set-up on a multisampled target (pcr_date.frag with MSAA): shaded at every sample, each sample's stencil set
// where that sample's alpha bit 7 is TEST's DATM
#include "pcr_common.h"

layout(input_attachment_index = 0, set = 0, binding = 5) uniform subpassInputMS u_target;

void main() {
    uint da = uint(round(subpassLoad(u_target, gl_SampleID).a * 255.0));
    if (((da >> 7) & 1u) != ((pc.flags & PCR_F_DATM) != 0u ? 1u : 0u)) {
        discard;
    }
}
