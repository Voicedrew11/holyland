#version 450
// The picture scaled to the window's size for the presenter (port/gs/pc/pcr.cpp, PresentScaler;
// --present-scaling best), drawn over the whole output image with pcr_present.vert's triangle.
//
// Each output pixel is a weighted sum of the picture's pixels around the place it maps to, by a
// Catmull-Rom cubic (B 0, C 0.5) in each direction. Made larger (the output bigger than the picture) the
// cubic is used as it is: sharper than bilinear, with no blocks. Made smaller it is stretched by the ratio
// (its reach and its lobes ratio times as wide), so every picture pixel the output pixel covers counts
// and fine detail is averaged instead of skipped (bilinear reads 2 x 2 pixels whatever the ratio, which
// shimmers and aliases at 4x). The sum is taken on linear light (the picture's sRGB values decoded and
// encoded again), so a fine bright line on a dark wall keeps its brightness; the result is clamped to
// what the pixels around could give, against the cubic's overshoot at hard edges.
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform Push {
    ivec2 src_size;  // the picture's pixels
    vec2 ratio;      // picture pixels an output pixel spans, across and down
    int linear;      // 1: weighed on linear light
} push;
layout(location = 0) out vec4 o_color;

float cubic(float x) {
    // Catmull-Rom: B 0, C 0.5
    x = abs(x);
    if (x < 1.0) {
        return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    }
    if (x < 2.0) {
        return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    }
    return 0.0;
}

vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec2 at = (gl_FragCoord.xy) * push.ratio - 0.5; // the output pixel's middle in the picture's pixels
    vec2 stretch = max(push.ratio, vec2(1.0));       // the kernel's width, in picture pixels
    vec2 reach = 2.0 * stretch;
    ivec2 lo = ivec2(ceil(at - reach)), hi = ivec2(floor(at + reach));
    lo = clamp(lo, ivec2(0), push.src_size - 1);
    hi = clamp(hi, ivec2(0), push.src_size - 1);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    vec3 mn = vec3(1e9), mx = vec3(-1e9);
    for (int y = lo.y; y <= hi.y; y++) {
        float wy = cubic((float(y) - at.y) / stretch.y);
        if (wy == 0.0) {
            continue;
        }
        for (int x = lo.x; x <= hi.x; x++) {
            float w = cubic((float(x) - at.x) / stretch.x) * wy;
            vec3 c = texelFetch(u_src, ivec2(x, y), 0).rgb;
            if (push.linear != 0) {
                c = to_linear(c);
            }
            sum += c * w;
            total += w;
            if (abs(float(x) - at.x) <= stretch.x && abs(float(y) - at.y) <= stretch.y) {
                mn = min(mn, c);
                mx = max(mx, c);
            }
        }
    }
    vec3 c = total != 0.0 ? sum / total : vec3(0.0);
    if (mx.x >= mn.x) {
        c = clamp(c, mn, mx);
    }
    if (push.linear != 0) {
        c = to_srgb(max(c, vec3(0.0)));
    }
    o_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
