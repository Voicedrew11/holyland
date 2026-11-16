#version 450
#extension GL_ARB_fragment_shader_interlock : require
// The depth rule's own copy of the depth (pcr_prim.h's PCR_DEPTH_INTERLOCK) cleared where the depth attachment is,
// in the pass (pcr_canvas.cpp's clear_depth_copy): drawn over the clear's rectangle (pcr_cover.vert, the scissor),
// each of the copy's layers (a sample each) given the clear's depth, in the critical section, so in order with the
// draws before and after it at each pixel
layout(pixel_interlock_ordered) in;
layout(set = 0, binding = 0, r32f) uniform coherent image2DArray u_zstore;
layout(push_constant) uniform Push {
    float depth;
    int layers;
} pc;

void main() {
    beginInvocationInterlockARB();
    for (int i = 0; i < pc.layers; i++) {
        imageStore(u_zstore, ivec3(ivec2(gl_FragCoord.xy), i), vec4(pc.depth));
    }
    endInvocationInterlockARB();
}
