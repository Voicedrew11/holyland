#version 450
// The temporal anti-aliasing's inputs shown over the picture (port/gs/pc/pcr_antialias.cpp, KF4_AA_DEBUG), a
// developer's view, never a player's:
// 1 depth: grey, log2 of the GS's Z (nearer lighter); blue where there is none
// 2 motion: red right / left, green down / up (0.5 none, a full step 64 pixels); blue the length (1: 128 pixels or
//   more); dark blue where there is none
// 3 reproject: the previous picture moved by the motion, less this one (4 times, grey: black where it holds); red
//   where there is no history or motion
// 4 jitter: the previous picture where it was, less this one (8 times): what the jitter alone changes, on a still
//   camera
layout(set = 0, binding = 0) uniform sampler2D u_now;      // this picture as made (the history's copy)
layout(set = 0, binding = 1) uniform sampler2D u_then;     // the previous picture's (or u_now)
layout(set = 0, binding = 2) uniform sampler2D u_depth;    // (or u_now)
layout(set = 0, binding = 3) uniform sampler2D u_motion;   // (or u_now)
layout(push_constant) uniform Push {
    vec2 texel;     // 1 / the picture's size
    float zscale;   // the depth's (0: no depth)
    uint view;      // 1-4 above
    uint has;       // bit 0 the previous picture, 1 the depth, 2 the motion
} pc;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 p = gl_FragCoord.xy;
    ivec2 at = ivec2(p);
    bool then_ok = (pc.has & 1u) != 0u, depth_ok = (pc.has & 2u) != 0u, motion_ok = (pc.has & 4u) != 0u;
    vec3 now = texelFetch(u_now, at, 0).rgb;
    if (pc.view == 1u) {
        if (!depth_ok) {
            o_color = vec4(0.0, 0.0, 0.6, 1.0);
            return;
        }
        float z = texelFetch(u_depth, at, 0).r / pc.zscale;
        o_color = vec4(vec3(clamp(log2(z + 1.0) / 24.0, 0.0, 1.0)), 1.0);
    } else if (pc.view == 2u) {
        if (!motion_ok) {
            o_color = vec4(0.0, 0.0, 0.6, 1.0);
            return;
        }
        vec2 m = texelFetch(u_motion, at, 0).rg;
        o_color = vec4(clamp(0.5 + m / 128.0, 0.0, 1.0), clamp(length(m) / 128.0, 0.0, 1.0), 1.0);
    } else if (pc.view == 3u) {
        if (!then_ok || !motion_ok) {
            o_color = vec4(0.6, 0.0, 0.0, 1.0);
            return;
        }
        vec2 m = texelFetch(u_motion, at, 0).rg;
        vec3 then = textureLod(u_then, (p + m) * pc.texel, 0.0).rgb;
        o_color = vec4(vec3(clamp(4.0 * length(then - now), 0.0, 1.0)), 1.0);
    } else {
        if (!then_ok) {
            o_color = vec4(0.6, 0.0, 0.0, 1.0);
            return;
        }
        vec3 then = texelFetch(u_then, at, 0).rgb;
        o_color = vec4(vec3(clamp(8.0 * length(then - now), 0.0, 1.0)), 1.0);
    }
}
