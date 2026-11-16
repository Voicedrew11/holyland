// SMAA's three passes over the picture (port/gs/pc/pcr_antialias.cpp, PORT_AA_SMAA): pcr_smaa_edges.frag,
// pcr_smaa_weights.frag and pcr_smaa_blend.frag, each drawn over the whole of its target with pcr_present.vert's
// triangle. The method is the reference implementation's (third_party/smaa/SMAA.hlsl, compiled here as GLSL), at its
// HIGH preset; SMAA's vertex functions are called in the fragment stage, from the pixel's place, so the passes share
// the one vertex shader with no outputs.

layout(push_constant) uniform SmaaPush {
    vec4 metrics;   // 1 / the picture's width, 1 / its height, its width, its height (SMAA_RT_METRICS)
    vec4 subsample; // the weights pass's subsampleIndices: 0 for SMAA 1x (SMAA.hlsl's @SUBSAMPLE_INDICES)
} pc;

#define SMAA_RT_METRICS pc.metrics
#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
#include "SMAA.hlsl"

// The pixel's middle, as SMAA's texcoord (0 to 1, down from the top: Vulkan's, as Direct3D's)
vec2 smaa_texcoord() {
    return gl_FragCoord.xy * SMAA_RT_METRICS.xy;
}
