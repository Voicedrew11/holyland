#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's GS primitives (port/gs/pc/pcr_canvas.cpp): already in the target's GS pixels (the GS's X and Y
// less XYOFFSET), one vertex a triangle corner. A GS pixel is drawn when its integer point is inside a primitive;
// Vulkan tests a pixel's middle, so every vertex moves half a GS pixel (at any scale: the middle of its pixels).
// The GS interpolates everything linearly across the screen: noperspective throughout.
#include "pcr_common.h"

layout(location = 0) in vec2 a_pos;
layout(location = 1) in uint a_z;
layout(location = 2) in uvec4 a_color;
layout(location = 3) in vec3 a_stq;
layout(location = 4) in float a_fog;

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;

void main() {
    vec2 ndc = (a_pos + 0.5) * pc.inv_size * 2.0 - 1.0;
    // (the GS keeps Z within what the Z buffer's format holds; taken in at the corners)
    gl_Position = vec4(ndc, float(min(a_z, pc.zmax)) * pc.zscale, 1.0);
    v_color = vec4(a_color);
    v_stq = a_stq;
    v_fog = a_fog;
}
