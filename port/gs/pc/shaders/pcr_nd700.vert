#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's native draws of program 0x700, the effects (port/gs/pc/pcr_native.cpp): a strip's triangles VU1
// kicks (the CPU's plan: port/render/vu1_record.cpp), each corner what VU1's program would have kicked
// (port/render/shaders/vu1_700_maths.h: one colour for the model, no point lights), given to pcr_prim.frag as a GS
// primitive's are (pcr_nd000.vert's sibling).
#define PCR_ND
#include "pcr_common.h"

// the strip's triangles: x, y, z the arena's numbers of its corners, w its kick's
layout(set = 2, binding = 2, std430) readonly buffer Tris {
    uvec4 tris[];
};

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;

// a picture between's (port/gs/pc/pcr_between.h; zeros outside one)
layout(set = 1, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;
} pic;
// the meshes (the arena): four qwords a vertex, as VU1 memory had them
layout(set = 2, binding = 0, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "vu1_700_draw.h"
// the draws' records (port/render/vu1_record.hpp, Vu1Record)
layout(set = 2, binding = 1, std430) readonly buffer Draws {
    Draw draws[];
};
#include "vu1_700_maths.h"

void main() {
    uvec4 tri = tris[gl_VertexIndex / 3];
    uint which = uint(gl_VertexIndex) % 3u;
    uint at = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w;
    uint instance = uint(gl_InstanceIndex);
    d = draws[instance & 0x3FFFFFFFu];
    bool gouraud = ((instance >> 30) & 1u) != 0u;
    ivec4 N = mesh.q[at * 4u + 1u], T = mesh.q[at * 4u + 2u];
    Place p = vu_place(at);
    // the fog: ACC.w = 1 * fog.z, then + fog.w * q; clamped to 0..255
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, p.q)), 255.0), 0.0);
    vec3 st = vec3(T.xyz) / 4096.0; // (ITOF12: exact)
    vec3 stq = vec3(vu_mul(st.x, p.q), vu_mul(st.y, p.q), vu_mul(st.z, p.q));
    uint z = gs_z(p);
    int xi = p.x, yi = p.y;
    bool gone = false;
    if ((d.info.w & ND_C_MOVING) != 0) {
        // a picture between's: moved, S, T and Q by the change of depth; dropped with its kick or one of the two kicks
        // before it, the same for its three corners (the native GS's nd_700.vert)
        Moved m = moved(at, p);
        xi = m.x;
        yi = m.y;
        z = m.z;
        stq *= m.f;
        for (uint i = 0u; i < 3u && !gone; i++) {
            uint k = kick - i;
            gone = k == at ? m.drop : moved(k, vu_place(k)).drop;
        }
    }
    uint w16 = (uint(ftoi(fogf * 16.0)) | uint(N.w)) & 0xFFFFu;
    vec2 xy = vec2(float(int(uint(xi) & 0xFFFFu) - d.info.y), float(int(uint(yi) & 0xFFFFu) - d.info.z)) / 16.0;
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
