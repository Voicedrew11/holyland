#version 450
// A layer's fragment: a texel of noise at half alpha (TEXTURED), or one colour. DISCARDS: a discard that never
// happens (the depth test then waits for the shader). OWN_DEPTH: the shader writes the depth (as exact Z does).
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_colour;
#ifdef OWN_DEPTH
layout(depth_less) out float gl_FragDepth;
#endif
void main() {
#ifdef TEXTURED
    vec4 c = texture(u_tex, v_uv);
#else
    vec4 c = vec4(0.25, 0.5, 0.75, 0.5);
#endif
#ifdef DISCARDS
    if (c.a < 0.001) {
        discard;
    }
#endif
#ifdef OWN_DEPTH
    gl_FragDepth = gl_FragCoord.z;
#endif
    o_colour = vec4(c.rgb, 0.5);
}
