// The developers' pixel trace's probe (pcr_canvas.cpp's trace_probe; pcr.h's PcrTraceProbe): the traced pixel's samples
// as the draws so far left them, its Z (depth / the Z scale) and its colour, into the trace buffer's probe record
// pc.slot. One invocation a sample.
layout(local_size_x = 8) in;

#ifdef PCR_PROBE_MS
layout(set = 0, binding = 0) uniform sampler2DMS u_depth;  // the depth surface (the colour's again when none)
layout(set = 0, binding = 1) uniform sampler2DMS u_colour; // the colour surface's image drawn into
#else
layout(set = 0, binding = 0) uniform sampler2D u_depth;
layout(set = 0, binding = 1) uniform sampler2D u_colour;
#endif

// (pcr_prim.h's Trace: the head, the fragments' records, then the probes' at PCR_PROBE_FIRST, five uvec4 each)
#define PCR_PROBE_FIRST (1u + 4096u * 3u)
layout(set = 0, binding = 2, std430) buffer Trace {
    uvec4 rec[];
} u_trace;

layout(push_constant, std430) uniform Push {
    ivec2 xy;       // the target's pixel
    uint draw;      // the draw it is after
    uint slot;      // the probe's record
    uint samples;   // the surfaces' samples (1 to 8)
    uint has_depth; // 0: no depth surface
    float zscale;   // Z to depth
} pc;

void main() {
    uint s = gl_LocalInvocationID.x;
    if (s >= pc.samples) {
        return;
    }
#ifdef PCR_PROBE_MS
    float d = texelFetch(u_depth, pc.xy, int(s)).r;
    vec4 c = texelFetch(u_colour, pc.xy, int(s));
#else
    float d = texelFetch(u_depth, pc.xy, 0).r;
    vec4 c = texelFetch(u_colour, pc.xy, 0);
#endif
    uint at = PCR_PROBE_FIRST + pc.slot * 5u;
    if (s == 0u) {
        u_trace.rec[at] = uvec4(pc.draw, pc.samples, pc.has_depth, 0u);
    }
    float z = pc.has_depth != 0u ? d / pc.zscale : -1.0;
    uvec4 c8 = uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
    // (z and colour of sample s: words 4 + s and 12 + s of the record)
    uint zi = at + 1u + s / 4u, ci = at + 3u + s / 4u;
    u_trace.rec[zi][s % 4u] = floatBitsToUint(z);
    u_trace.rec[ci][s % 4u] = c8.r | c8.g << 8 | c8.b << 16 | c8.a << 24;
}
