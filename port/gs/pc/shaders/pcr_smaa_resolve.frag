#version 450
// SMAA T2x's temporal resolve (port/gs/pc/pcr_antialias.cpp's SmaaT2x): this picture's SMAA (with the jitter of one of
// T2x's two) and the previous picture's (the other), reprojected by the motion, half and half where the two moved
// alike. SMAA.hlsl's SMAAResolvePS with SMAA_REPROJECTION (the velocity weighting: CryEngine 3's), and one thing more:
// the previous colour held within this picture's 3 x 3 around the pixel. The motion comes from the depth and the
// cameras, so the 2D over the 3D (the HUD) and the fire have the world's motion; both pictures' velocities agree there
// and the weighting alone would blend in what was under them. Two pictures only: no history of histories, so no trail
// longer than one picture.
layout(set = 0, binding = 0) uniform sampler2D u_now;    // this picture's SMAA, the velocity's length in alpha
layout(set = 0, binding = 1) uniform sampler2D u_then;   // the previous picture's (linear, clamped)
layout(set = 0, binding = 2) uniform sampler2D u_motion; // RG16F: picture pixels to the previous picture
layout(push_constant) uniform Push {
    vec2 texel;       // 1 / the picture's size
    uint has_then;    // 0: no history (reset): this picture's alone
    float weight_scale; // SMAA_REPROJECTION_WEIGHT_SCALE
} pc;
layout(location = 0) out vec4 o_color;

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 now = texelFetch(u_now, at, 0);
    if (pc.has_then == 0u) {
        o_color = vec4(now.rgb, 1.0);
        return;
    }
    vec2 then_uv = (gl_FragCoord.xy + texelFetch(u_motion, at, 0).rg) * pc.texel;
    if (any(lessThan(then_uv, vec2(0.0))) || any(greaterThan(then_uv, vec2(1.0)))) {
        o_color = vec4(now.rgb, 1.0); // (not on the previous picture: come into view)
        return;
    }
    vec4 then = textureLod(u_then, then_uv, 0.0);
    vec3 lo = now.rgb, hi = now.rgb;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec3 c = texelFetch(u_now, clamp(at + ivec2(x, y), ivec2(0), textureSize(u_now, 0) - 1), 0).rgb;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }
    vec3 held = clamp(then.rgb, lo, hi);
    float delta = abs(now.a * now.a - then.a * then.a) / 5.0;
    float weight = 0.5 * clamp(1.0 - sqrt(delta) * pc.weight_scale, 0.0, 1.0);
    o_color = vec4(mix(now.rgb, held, weight), 1.0);
}
