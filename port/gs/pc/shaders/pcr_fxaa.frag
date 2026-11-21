#version 450
#extension GL_GOOGLE_include_directive : require
// FXAA over the picture (port/gs/pc/pcr_antialias.cpp, PORT_AA_FXAA), drawn over the whole of it with pcr_present.vert's
// triangle, reading a copy of it: pcr_fxaa.h. (Where the picture is scaled for the window, the final pass in one does
// the same: pcr_present_fused.comp.)
layout(set = 0, binding = 0) uniform sampler2D u_src; // the picture's copy, read bilinear, clamped
layout(push_constant) uniform Push {
    vec2 texel; // 1 / the picture's size
} pc;
layout(location = 0) out vec4 o_color;

#define FXAA_SRC u_src
#define FXAA_TEXEL pc.texel
#include "pcr_fxaa.h"

void main() {
    o_color = vec4(fxaa_at(gl_FragCoord.xy * pc.texel), 1.0); // (the pixel's middle)
}
