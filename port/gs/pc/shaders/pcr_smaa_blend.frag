#version 450
// SMAA's third pass (pcr_smaa.h): each pixel blended with its neighbours by the weights, over the whole picture
#extension GL_GOOGLE_include_directive : require
#include "pcr_smaa.h"
layout(set = 0, binding = 0) uniform sampler2D u_picture; // (a copy of it, read linear, clamped)
layout(set = 0, binding = 1) uniform sampler2D u_weights; // (linear, clamped)
layout(location = 0) out vec4 o_color;

void main() {
    vec2 texcoord = smaa_texcoord();
    vec4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);
    o_color = SMAANeighborhoodBlendingPS(texcoord, offset, u_picture, u_weights);
}
