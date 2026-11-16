#version 450
// SMAA T2x's third pass (pcr_smaa.h, port/gs/pc/pcr_antialias.cpp's SmaaT2x): pcr_smaa_blend.frag's, with SMAA's
// reprojection on: the motion (picture pixels to the previous picture, AntiAliasFrame::motion) blended as the colour
// is, and its length kept in alpha (sqrt(5 |v|), v in SMAA's texcoord units) for the resolve's velocity weighting
#extension GL_GOOGLE_include_directive : require
#define SMAA_REPROJECTION 1
// (SMAA's velocity is now less then in texcoords, ours then less now in pixels)
#define SMAA_DECODE_VELOCITY(sample) (-(sample).rg * SMAA_RT_METRICS.xy)
#include "pcr_smaa.h"
layout(set = 0, binding = 0) uniform sampler2D u_picture; // (a copy of it, read linear, clamped)
layout(set = 0, binding = 1) uniform sampler2D u_weights; // (linear, clamped)
layout(set = 0, binding = 2) uniform sampler2D u_motion;  // (RG16F, linear, clamped)
layout(location = 0) out vec4 o_color;

void main() {
    vec2 texcoord = smaa_texcoord();
    vec4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);
    o_color = SMAANeighborhoodBlendingPS(texcoord, offset, u_picture, u_weights, u_motion);
}
