#version 450
#extension GL_GOOGLE_include_directive : require
#ifdef PCR_DATE_MARK
#extension GL_ARB_fragment_shader_interlock : require
#endif
// DATE's set-up on a multisampled target (pcr_date.frag with MSAA): shaded at every sample, each sample's stencil set
// where that sample's alpha bit 7 is TEST's DATM
#include "pcr_common.h"

layout(input_attachment_index = 0, set = 1, binding = 5) uniform subpassInputMS u_target;

// (PCR_DATE_MARK, the _mark variant: a batch that the depth rule draws with its own copy of the depth, pcr_prim.h's
// PCR_DEPTH_INTERLOCK) The copy's sign bit set where DATE fails and cleared where it passes, over the same rectangle,
// in the critical section: the rule's shader then fails what the stencil fails, so the copy stays the depth's
#ifdef PCR_DATE_MARK
layout(pixel_interlock_ordered) in;
layout(set = 1, binding = 11, r32f) uniform coherent image2DArray u_zstore;
#endif

void main() {
    uint da = uint(round(subpassLoad(u_target, gl_SampleID).a * 255.0));
    bool passes = ((da >> 7) & 1u) == ((pc.flags & PCR_F_DATM) != 0u ? 1u : 0u);
#ifdef PCR_DATE_MARK
    beginInvocationInterlockARB();
    ivec3 at = ivec3(ivec2(gl_FragCoord.xy), gl_SampleID);
    float depth = abs(imageLoad(u_zstore, at).r);
    imageStore(u_zstore, at, vec4(passes ? depth : -depth));
    endInvocationInterlockARB();
#endif
    if (!passes) {
        discard;
    }
}
