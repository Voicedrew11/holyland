#version 450
// Sharpening of the scaled picture for the presenter (port/gs/pc/pcr.cpp, PresentScaler;
// --present-sharpen), drawn over the whole output image with pcr_present.vert's triangle.
//
// Contrast-adaptive, after the idea AMD published as CAS: each pixel is pushed away from its four
// neighbours (an unsharp mask with a negative weight w on them), by an amount that follows how much
// room the neighbourhood leaves: where its darkest and brightest are far from black and white it is
// sharpened fully, where it already spans nearly all of it (a hard edge) hardly at all, so edges do not
// ring and flat areas' noise is not raised much. Strength 0 to 1 sets the most w (1/8 to 1/5 of a
// neighbour). Written here from that description, per channel, on the sRGB values.
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform Push {
    ivec2 size;
    float strength; // 0 to 1
} push;
layout(location = 0) out vec4 o_color;

vec3 at(ivec2 p) {
    return texelFetch(u_src, clamp(p, ivec2(0), push.size - 1), 0).rgb;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 e = at(p);
    vec3 b = at(p + ivec2(0, -1)), d = at(p + ivec2(-1, 0)), f = at(p + ivec2(1, 0)), h = at(p + ivec2(0, 1));
    vec3 mn = min(e, min(min(b, d), min(f, h)));
    vec3 mx = max(e, max(max(b, d), max(f, h)));
    // the room left: how far the neighbourhood is from both ends, against its brightest
    vec3 room = clamp(min(mn, 1.0 - mx) / max(mx, vec3(1.0 / 1024.0)), 0.0, 1.0);
    vec3 amount = sqrt(room);
    float most = -mix(1.0 / 8.0, 1.0 / 5.0, clamp(push.strength, 0.0, 1.0));
    vec3 w = amount * most * clamp(push.strength * 4.0, 0.0, 1.0);
    vec3 c = (e + (b + d + f + h) * w) / (1.0 + 4.0 * w);
    o_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
