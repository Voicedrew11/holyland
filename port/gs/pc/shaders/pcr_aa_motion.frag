#version 450
// The temporal anti-aliasing's motion from the cameras (port/gs/pc/pcr_antialias.cpp, AntiAliasing::Inputs): for each
// pixel of the picture, where its place in the world (from its depth and this picture's camera) was in the previous
// picture (by that picture's camera), in picture pixels from here. Right for what stays put in the world (most of the
// screen); not for a moving model, the 2D or an effect drawn over the 3D (they keep the depth under them). The jitter is
// not in it: each pixel is taken at its own middle, as the exact Z is (shaders/pcr_exact_z.h).
// The GS's projection (frame_interp.c's): X = sx x / z + cx, Y = sy y / z + cy, Z = cz + az / z, from the view
// R world + t. Where nothing was drawn (Z at cz or under: the depth's clear, the sky's far), the place is taken as far
// away: only the camera's turn moves it.
layout(set = 0, binding = 0) uniform sampler2D u_depth; // R32F: the GS's Z times zscale (Canvas::scanout_depth)
layout(set = 0, binding = 1, std140) uniform Cameras {
    vec4 now_view[3];  // this picture's world to view: rows of R, each with t in w
    vec4 then_view[3]; // the previous picture's
    vec4 now_screen;   // sx, sy, cx, cy
    vec4 then_screen;
    vec4 now_z;        // cz, az, zscale, 0
    vec4 then_z;       // cz, az, 0, 0
    vec4 now_place;    // the picture's origin on the GS's screen (x, y), its pixels a GS pixel (x, y)
    vec4 then_place;
} u;
layout(location = 0) out vec2 o_motion;

void main() {
    vec2 p = gl_FragCoord.xy;
    // (a target pixel's middle is the GS's point (p - 0.5) / scale: shaders/pcr_common.h's gs_to_clip)
    vec2 gs = u.now_place.xy + (p - 0.5) / u.now_place.zw;
    float z = texelFetch(u_depth, ivec2(p), 0).r / u.now_z.z;
    mat3 r_now = transpose(mat3(u.now_view[0].xyz, u.now_view[1].xyz, u.now_view[2].xyz)); // (its rows as columns)
    mat3 r_then = transpose(mat3(u.then_view[0].xyz, u.then_view[1].xyz, u.then_view[2].xyz));
    vec3 t_now = vec3(u.now_view[0].w, u.now_view[1].w, u.now_view[2].w);
    vec3 t_then = vec3(u.then_view[0].w, u.then_view[1].w, u.then_view[2].w);
    vec3 ray = vec3((gs.x - u.now_screen.z) / u.now_screen.x, (gs.y - u.now_screen.w) / u.now_screen.y, 1.0);
    vec3 then;
    if (z > u.now_z.x + 0.5) {
        vec3 view = ray * (u.now_z.y / (z - u.now_z.x));
        vec3 world = transpose(r_now) * (view - t_now); // (R's inverse: its transpose)
        then = r_then * world + t_then;
    } else {
        then = r_then * (transpose(r_now) * ray); // (far: the direction alone)
    }
    if (!(then.z > 0.0)) {
        o_motion = vec2(0.0); // (behind the previous camera: nowhere it was seen)
        return;
    }
    vec2 gs_then = vec2(u.then_screen.x * then.x / then.z + u.then_screen.z,
                        u.then_screen.y * then.y / then.z + u.then_screen.w);
    vec2 p_then = (gs_then - u.then_place.xy) * u.then_place.zw + 0.5;
    o_motion = p_then - p;
}
