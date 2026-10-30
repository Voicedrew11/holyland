#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's native draws of program 0x000 (port/gs/pc/pcr_native.cpp): a batch's triangles from its mesh,
// each corner what VU1's program would have kicked (port/render/shaders/vu1_000_maths.h: its maths and arithmetic,
// the native GS's own), given to pcr_prim.frag as a GS primitive's are: X and Y in the target's GS pixels (XYOFFSET
// off, the surface's place added), the GS's integer Z, RGBA, S T Q and fog. A triangle VU1's clipping would leave out
// (the ADC of its kick or of the two kicks before it) is not drawn.
#define PCR_ND
#include "pcr_common.h"

// the batch's triangles: x, y, z the arena's numbers of its corners; w the kick's (bits 0-27) and how many of the two
// kicks before it are the batch's (bits 28-29)
layout(set = 2, binding = 2, std430) readonly buffer Tris {
    uvec4 tris[];
};

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;

// a picture between's (zeros: the pictures between are not moved here yet)
layout(set = 1, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;
} pic;
// the meshes (the arena): four qwords a vertex (position, normal, ST, colour), as VU1 memory had them
layout(set = 2, binding = 0, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "vu1_000_draw.h"
// the draws' records (port/render/vu1_record.hpp)
layout(set = 2, binding = 1, std430) readonly buffer Draws {
    Draw draws[];
};
#include "vu1_000_maths.h"

void main() {
    uvec4 tri = tris[gl_VertexIndex / 3];
    uint which = uint(gl_VertexIndex) % 3u;
    uint at = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w & 0x0FFFFFFFu;
    uint befores = (tri.w >> 28) & 3u;
    uint instance = uint(gl_InstanceIndex);
    d = draws[instance & 0x3FFFFFFFu];
    bool gouraud = (instance >> 30) != 0u;
    // VU1's ADC: the kick's clip and the two kicks' before it
    bool gone = false;
    for (uint i = 0u; i <= befores && !gone; i++) {
        gone = vu_clipped(kick - i);
    }
    ivec4 N = mesh.q[at * 4u + 1u], T = mesh.q[at * 4u + 2u];
    Place p = vu_place(at);
    // the fog: ACC.w = 1 * fog.z, then + fog.w * q; clamped to 0..255
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, p.q)), 255.0), 0.0);
    if ((d.info.w & ND_C_WAVE) != 0) {
        // the wave UVs on the GPU: the vertex's own S and T moved by its wave's (ST's w) offsets in 16 bits, as
        // Wobble_ApplyWaveUvs writes them, read back signed as VIF1's UNPACK does
        uint w = uint(T.w) & 15u;
        uint o = d.wave[w >> 2][w & 3u];
        T.x = (int(uint(T.x) + (o & 0xFFFFu)) << 16) >> 16;
        T.y = (int(uint(T.y) + (o >> 16)) << 16) >> 16;
    }
    vec3 st = vec3(T.xyz) / 4096.0; // (ITOF12: exact)
    vec3 stq = vec3(vu_mul(st.x, p.q), vu_mul(st.y, p.q), vu_mul(st.z, p.q));
    uint z = (d.info.w & ND_C_FAR) != 0 ? 0u : gs_z(p); // (at the far plane: the modern sky)
    uint w16 = (uint(ftoi(fogf * 16.0)) | uint(N.w)) & 0xFFFFu;
    vec2 xy = vec2(float(int(uint(p.x) & 0xFFFFu) - d.info.y), float(int(uint(p.y) & 0xFFFFu) - d.info.z)) / 16.0;
    xy += vec2(pc.place);
    if (gone) {
        xy = vec2(1.0e9); // (far past the target's right edge, as are its two other corners: not drawn)
    }
    uvec4 rgba = lit_colour(gouraud ? at : kick);
    // as pcr_prim.vert
    vec2 ndc = (xy + 0.5) * pc.inv_size * 2.0 - 1.0;
    gl_Position = vec4(ndc, float(min(z, pc.zmax)) * pc.zscale, 1.0);
    v_color = vec4(rgba);
    v_stq = stq;
    v_fog = float((w16 >> 4) & 0xFFu);
}
