#version 450
// Colour high's scanout (port/gs/pc/pcr_canvas.cpp, scanout): a 16-bit surface's rectangle into the 8-bit picture,
// drawn over the picture (or the part a lower field's lines take) with pcr_cover.vert's triangle, each picture pixel
// the surface's pixel it falls on (nearest, as the blit at 8 bits). Colours are dithered to 8 bits: a channel v (in 8-bit steps, with its fraction) becomes
// floor(v + t), t a threshold in [0, 1) from interleaved gradient noise (fine-grained, no pattern to see, the same
// every picture), so a gradient keeps its mean level everywhere instead of stepping. A whole number stays as it is
// (the native GS's narrow.frag). Alpha is rounded.
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant, std430) uniform Push {
    ivec2 origin; // the rectangle's first pixel in the surface
    ivec2 extent; // its size there
    ivec2 size;   // the size of the part of the picture it is drawn over
    ivec2 at;     // where that part starts (a lower field's: a line down)
} push;
layout(location = 0) out vec4 o_color;

float threshold(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 from = push.origin + ((p - push.at) * push.extent) / push.size;
    vec4 v = texelFetch(u_src, from, 0) * 255.0;
    // (a whole number read back as a hair under or over it stays that number)
    vec3 c = floor(v.rgb + 1.0 / 512.0 + threshold(vec2(p)) * (1.0 - 1.0 / 256.0));
    o_color = vec4(clamp(c, 0.0, 255.0), round(v.a)) / 255.0;
}
