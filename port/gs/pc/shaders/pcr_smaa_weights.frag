#version 450
// SMAA's second pass (pcr_smaa.h): each edge's blending weights from its length and shape, into the weights target
// (RGBA8, cleared first)
#extension GL_GOOGLE_include_directive : require
#include "pcr_smaa.h"
layout(set = 0, binding = 0) uniform sampler2D u_edges;  // (linear, clamped: SMAA reads two edges in one sample)
layout(set = 0, binding = 1) uniform sampler2D u_area;   // AreaTex (RG8, linear, clamped)
layout(set = 0, binding = 2) uniform sampler2D u_search; // SearchTex (R8, nearest, clamped)
layout(location = 0) out vec4 o_weights;

void main() {
    vec2 texcoord = smaa_texcoord();
    vec2 pixcoord;
    vec4 offset[3];
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
    o_weights = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, u_edges, u_area, u_search, pc.subsample);
}
