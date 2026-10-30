#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's native draws of program 0x280, the skinned models (port/gs/pc/pcr_native.cpp): a pass of a batch,
// its triangles those VU1 kicks (the CPU's plan: port/render/vu1_record.cpp), each corner what VU1's program would have
// kicked (port/render/shaders/vu1_280_maths.h), given to pcr_prim.frag as a GS primitive's are (pcr_nd000.vert's
// sibling). The first pass is lit and textured from ST; the second (the sphere-mapped shine) has ST from the sphere
// matrices and the normal, Q 1, white with vertex 1's alpha.
#define PCR_ND
#include "pcr_common.h"

// the pass's triangles: x, y, z the arena's slots of its corners (two a vertex), w its kick's
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
// the meshes (the arena): eight qwords a vertex (render.cpp, take_batch_280)
layout(set = 2, binding = 0, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "vu1_280_draw.h"
// the draws' records (port/render/vu1_record.hpp, Vu1Record280)
layout(set = 2, binding = 1, std430) readonly buffer Draws {
    Draw draws[];
};
#include "vu1_280_maths.h"

void main() {
    uvec4 tri = tris[gl_VertexIndex / 3];
    uint which = uint(gl_VertexIndex) % 3u;
    uint slot = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w;
    // the instance: the record (bits 0-27), the context whose XYOFFSET (28), the pass (29), IIP (30)
    uint instance = uint(gl_InstanceIndex);
    d = draws[instance & 0x0FFFFFFFu];
    bool sphere = ((instance >> 29) & 1u) != 0u, gouraud = ((instance >> 30) & 1u) != 0u;
    ivec2 offset = ((instance >> 28) & 1u) != 0u ? d.offset.zw : d.offset.xy;
    ivec4 X = mesh.q[slot * 4u + 4u];
    int s = vertex_set(slot);
    Place pl = vu_place(slot);
    float q = pl.q;
    uint z = uint(pl.z >> 4) & 0xFFFFFFu;
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, q)), 255.0), 0.0);
    uint w16 = (uint(ftoi(fogf * 16.0)) | uint(X.z)) & 0xFFFFu;
    vec2 xy = vec2(float(int(uint(pl.x) & 0xFFFFu) - offset.x), float(int(uint(pl.y) & 0xFFFFu) - offset.y)) / 16.0;
    xy += vec2(pc.place);
    vec3 stq;
    uvec4 rgba;
    if (!sphere) {
        vec4 st = qf(slot, 2u);
        stq = vec3(vu_mul(st.x, q), vu_mul(st.y, q), vu_mul(st.z, q));
        rgba = lit_colour(gouraud ? slot : kick);
    } else {
        // ((Ms * normal).xy + 1 * 1) * 0.5; Q 1; white, with vertex 1's alpha (its u's: lit.w is 1)
        vec4 acc = vu_transform3(d.sphere[s], qf(slot, 1u).xyz);
        stq = vec3(vu_mul(vu_add(acc.x, 1.0), 0.5), vu_mul(vu_add(acc.y, 1.0), 0.5), 1.0);
        uint one = slot - 2u * uint(X.x) + 2u;
        float a1 = min(vu_add(vu_mul(qf(one, 3u).w, d.scale.w), d.bias.w), 255.0);
        rgba = uvec4(255u, 255u, 255u, uint(ftoi(a1)) & 0xFFu);
    }
    // as pcr_prim.vert
    vec2 ndc = (xy + 0.5) * pc.inv_size * 2.0 - 1.0;
    gl_Position = vec4(ndc, float(min(z, pc.zmax)) * pc.zscale, 1.0);
    v_color = vec4(rgba);
    v_stq = stq;
    v_fog = float((w16 >> 4) & 0xFFu);
}
