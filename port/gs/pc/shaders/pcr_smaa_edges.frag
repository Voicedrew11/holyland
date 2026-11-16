#version 450
// SMAA's first pass (pcr_smaa.h): the picture's edges by luma, into the edges target (RG8, cleared first: a pixel with
// no edge is discarded)
#extension GL_GOOGLE_include_directive : require
#include "pcr_smaa.h"
layout(set = 0, binding = 0) uniform sampler2D u_picture; // (a copy of it, read linear, clamped)
layout(location = 0) out vec2 o_edges;

void main() {
    vec2 texcoord = smaa_texcoord();
    vec4 offset[3];
    SMAAEdgeDetectionVS(texcoord, offset);
    o_edges = SMAALumaEdgeDetectionPS(texcoord, offset, u_picture);
}
