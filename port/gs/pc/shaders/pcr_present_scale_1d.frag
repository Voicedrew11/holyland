#version 450
// The picture scaled along one direction for the presenter (port/gs/pc/pcr.cpp, PresentScaler; --present-scaling
// best), drawn over the whole output image with pcr_present.vert's triangle. Two passes make the scaled picture:
// across into an image as wide as the output and as tall as the picture (linear light, floats), then down into the
// output. pcr_present_scale.frag is the same filter in one pass, both directions at once: at 4x into a 1440-row
// window it reads about 35 picture pixels for each output pixel, these two passes 7 and 5 (0.48 ms a picture to
// about half, 009, 2026-11-14).
//
// The filter along the direction: a Catmull-Rom cubic (B 0, C 0.5), stretched by the ratio when the picture is made
// smaller so every picture pixel the output pixel covers counts. The sum is taken on linear light (the first pass
// decodes the picture's sRGB values, the second encodes), and each pass's result is clamped to what the pixels within
// the kernel's middle could give, against the cubic's overshoot at hard edges. (The one pass clamps once, to the
// pixels of both directions' middles; two clamps, each along its own direction, differ from it by a rounding here and
// there at hard corners.)
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform Push {
    ivec2 src_size;  // the source's pixels
    ivec2 along;     // (1, 0): across; (0, 1): down
    float ratio;     // source pixels an output pixel spans along the direction
    int decode;      // 1: the source is sRGB values, weighed on linear light
    int encode;      // 1: the output is sRGB values
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
    ivec2 here = ivec2(gl_FragCoord.xy);
    bool across = push.along.x != 0;
    // the output pixel's middle in the source's pixels, along the direction
    float at = (across ? gl_FragCoord.x : gl_FragCoord.y) * push.ratio - 0.5;
    float stretch = max(push.ratio, 1.0); // the kernel's width, in source pixels
    int last = across ? push.src_size.x - 1 : push.src_size.y - 1;
    int lo = clamp(int(ceil(at - 2.0 * stretch)), 0, last), hi = clamp(int(floor(at + 2.0 * stretch)), 0, last);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    vec3 mn = vec3(1e9), mx = vec3(-1e9);
    for (int i = lo; i <= hi; i++) {
        float w = cubic((float(i) - at) / stretch);
        vec3 c = texelFetch(u_src, across ? ivec2(i, here.y) : ivec2(here.x, i), 0).rgb;
        if (push.decode != 0) {
            c = to_linear(c);
        }
        sum += c * w;
        total += w;
        if (abs(float(i) - at) <= stretch) {
            mn = min(mn, c);
            mx = max(mx, c);
        }
    }
    vec3 c = total != 0.0 ? sum / total : vec3(0.0);
    if (mx.x >= mn.x) {
        c = clamp(c, mn, mx);
    }
    if (push.encode != 0) {
        c = clamp(to_srgb(max(c, vec3(0.0))), 0.0, 1.0);
    }
    o_color = vec4(c, 1.0);
}
