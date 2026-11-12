#version 450
// A surface's pixels into every sample of its multisampled image (MSAA, port/gs/pc/pcr_canvas.cpp: what an upload, a
// copy or a new surface wrote into the surface's own image, before it is drawn into again)
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0);
}
