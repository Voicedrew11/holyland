#version 450
// The scene's motion from the last picture shown to this one, a pixel at a time (upscaling, docs/port/UPSCALING.md;
// gsn_composite.inc's sa_motion_pass), for a temporal upscaler: where each pixel's surface was in the last picture,
// less where it is now, in the scene's pixels (RG16F). Worked out from this picture's depth and the two pictures'
// cameras: the pixel's place in the GS's coordinates (its middle, this picture's jitter taken out) and its Z taken
// back through this picture's world_screen and forward through the last one's, one matrix m made of the two on the
// CPU. The camera's motion only: a model that moved by itself is given the camera's. A depth of 0 is the far plane
// (the sky, the clear): the Z the view's depth goes to at infinity, which m takes to the place that direction is seen
// at. With show, a picture to look at instead (KF4_UPSCALE_SHOW=motion): grey where nothing moved, red and green
// the motion across and down (a pixel is 1/16 of the way to full), blue how far. And the depth as FSR takes it
// (R32F, upscale_ffx.h): near / the view's depth, 1 at near and 0 far away (inverted, infinite), the view's depth
// from Z through the screen matrix's two rows that make Z and w (zview: its [2][2], [2][3], [3][2], [3][3]).
layout(set = 0, binding = 0) uniform sampler2D u_depth;
layout(push_constant, std430) uniform Push {
    mat4 m;         // this picture's X, Y, Z (and 1) to the last picture's, times its w
    vec2 inv_scale; // the GS's pixels a scene pixel is, across and down
    vec2 origin;    // XYOFFSET, in the GS's pixels
    vec2 jitter;    // this picture's jitter, in scene pixels (taken out)
    float zscale;   // the scene's depth to Z: Z = depth / zscale
    float zinf;     // Z where the view's depth goes to infinity
    float show;     // 1: a picture of the motion
    float near;     // the view's depth where FSR's depth is 1
    vec4 zview;     // the screen matrix's Z and w rows' z and 1 columns
} pc;
layout(location = 0) out vec4 o_out;
layout(location = 1) out float o_depth;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float d = texelFetch(u_depth, p, 0).r;
    vec2 x = (vec2(p) + 0.5 - pc.jitter) * pc.inv_scale + pc.origin;
    float z = d > 0.0 ? d / pc.zscale : pc.zinf;
    vec4 c = pc.m * vec4(x, z, 1.0);
    vec2 mv = abs(c.w) > 1.0e-20 ? (c.xy / c.w - x) / pc.inv_scale : vec2(0.0);
    // (Z = (zview.x * vz + zview.y) / (zview.z * vz + zview.w), for the view's depth vz)
    float below = pc.zview.z * z - pc.zview.x;
    float vz = abs(below) > 1.0e-20 ? (pc.zview.y - pc.zview.w * z) / below : 0.0;
    o_depth = d > 0.0 && vz > 0.0 ? clamp(pc.near / vz, 0.0, 1.0) : 0.0;
    if (pc.show != 0.0) {
        o_out = vec4(clamp(0.5 + mv / 16.0, 0.0, 1.0), clamp(length(mv) / 8.0, 0.0, 1.0), 1.0);
    } else {
        o_out = vec4(mv, 0.0, 0.0);
    }
}
