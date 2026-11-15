#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's native draws of program 0x700, the effects (port/gs/pc/pcr_native.cpp): a strip's triangles VU1
// kicks (the CPU's plan: port/render/vu1_record.cpp), each corner what VU1's program would have kicked
// (port/render/shaders/vu1_700_maths.h: one colour for the model, no point lights), given to pcr_prim.frag as a GS
// primitive's are (pcr_nd000.vert's sibling).
#define PCR_ND
#include "pcr_common.h"
#include "pcr_exact_z.h"

// the strip's triangles: x, y, z the arena's numbers of its corners, w its kick's
layout(set = 0, binding = 11, std430) readonly buffer Tris {
    uvec4 tris[];
};

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;
// (whole_triangle.h's whole triangles) the divisor of v_color: Q (1 / w) for a colour given times Q, so that
// pcr_prim.frag's colour is the corners' in 3D, as the game's near cut gives its pieces; 0 for a colour as is
layout(location = 12) noperspective out float v_color_q;
layout(location = 3) flat out uvec2 v_rect; // (the 2D mode's: none for a native draw)
out float gl_ClipDistance[1]; // the mirror image's cut at the water (mirror_cut)

// a picture between's (port/gs/pc/pcr_between.h; zeros outside one)
layout(set = 0, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;
} pic;
// the meshes (the arena): four qwords a vertex, as VU1 memory had them
layout(set = 0, binding = 9, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "vu1_700_draw.h"
// the draws' records (port/render/vu1_record.hpp, Vu1Record)
layout(set = 0, binding = 10, std430) readonly buffer Draws {
    Draw draws[];
};
#include "vu1_700_maths.h"

// The mirror image's cut at the water (pcr_nd000.vert's): position v's distance from the water, times its own w
float mirror_cut(vec4 v) {
    return d.clip == vec4(0.0) ? 0.0 : dot(d.clip, d.vertex * v);
}

void main() {
    v_color_q = 0.0;
    pcr_plane_none();
    uint triangle, instance;
    pcr_triangle(triangle, instance);
#ifdef PCR_RECORDS
    pcr_pass_record(); // (the fragment stage's fields: pcr_common.h)
#endif
    uvec4 tri = tris[triangle];
    uint which = uint(gl_VertexIndex) % 3u;
    uint at = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w & 0x7FFFFFFFu;
    bool left_out = (tri.w & 0x80000000u) != 0u; // (VU1 left it out of the frame: drawn in pictures between only)
    d = draws[instance & 0x3FFFFFFFu];
    bool gouraud = ((instance >> 30) & 1u) != 0u;
    ivec4 N = mesh.q[at * 4u + 1u], T = mesh.q[at * 4u + 2u];
    Place p = vu_place(at);
    // (drawn whole: by the position's own w, as the corner's w is the depth without it; on the screen: by q, which
    // has it, pcr_nd000.vert)
    float cut = mirror_cut(vec4(mesh.q[at * 4u]));
    float pw = float(mesh.q[at * 4u].w);
    gl_ClipDistance[0] = pw != 0.0 ? cut / pw : 0.0;
    float f = 1.0;
    // the fog: ACC.w = 1 * fog.z, then + fog.w * q; clamped to 0..255
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, p.q)), 255.0), 0.0);
    vec3 st = vec3(T.xyz) / 4096.0; // (ITOF12: exact)
    vec3 stq = vec3(vu_mul(st.x, p.q), vu_mul(st.y, p.q), vu_mul(st.z, p.q));
    uint z = gs_z(p);
    float zfrac = 0.0;
    // (exact Z, pcr_common.h: a whole Z the triangle's corners share, its first corner's in the frame)
    uint zref = which == 0u ? z : gs_z(vu_place(tri.x));
    int xi = p.x, yi = p.y;
    bool gone = left_out;
    if ((d.info.w & ND_C_MOVING) != 0) {
        // left out of the frame, or with a corner behind the picture's eye: drawn whole (whole_triangle.h)
        Whole w0 = whole_place(tri.x), w1 = whole_place(tri.y), w2 = whole_place(tri.z);
        if (left_out || whole_behind(w0, w1, w2)) {
            Whole c = which == 0u ? w0 : which == 1u ? w1 : w2;
            v_rect = uvec2(PCR_NO_RECT);
            if (whole_outside(w0, w1, w2)) {
                whole_not_drawn();
                v_color = vec4(0.0);
                v_stq = vec3(0.0);
                v_fog = 0.0;
                return;
            }
            whole_position(c, w0, w1, w2, ivec2(d.info.y, d.info.z), false);
            float qn = 1.0 / (c.w * float(mesh.q[at * 4u].w)); // (VU1's q: 1 / c.w, c.w the depth times the position's w)
            // (the colour times Q, as in 3D: pcr_nd000.vert's draw_whole)
            v_color = vec4(lit_colour(gouraud ? at : kick)) * qn;
            v_color_q = qn;
            v_stq = st * qn;
            v_fog = d.fog.z + d.fog.w * qn; // (the fragment stage clamps it to 0..255)
            return;
        }
        // a picture between's: moved, S, T and Q by the change of depth; dropped with its kick or one of the two kicks
        // before it, the same for its three corners
        Moved m = moved(at, p);
        xi = m.x;
        yi = m.y;
        z = m.z;
        zfrac = m.zfrac;
        stq *= m.f;
        f = m.f;
        for (uint i = 0u; i < 3u && !gone; i++) {
            uint k = kick - i;
            gone = k == at ? m.drop : moved(k, vu_place(k)).drop;
        }
    }
    uint w16 = (uint(ftoi(fogf * 16.0)) | uint(N.w)) & 0xFFFFu;
    // (in a picture between not wrapped to the GS's 16 bits: a place past the guard band is kept, past_band.h)
    ivec2 g = (d.info.w & ND_C_MOVING) != 0 ? ivec2(xi, yi) : ivec2(int(uint(xi) & 0xFFFFu), int(uint(yi) & 0xFFFFu));
    vec2 xy = vec2(float(g.x - d.info.y), float(g.y - d.info.z)) / 16.0;
    xy += vec2(pc.place);
    if (gone) {
        xy = vec2(1.0e9); // (far past the target's right edge, as are its two other corners: not drawn)
    }
    if ((pc.flags & PCR_F_EXACT_Z) != 0u && !gone) {
        pcr_plane_of_draw(); // (exact Z: the plane pass's, pcr_exact_z.h)
    }
    uvec4 rgba = lit_colour(gouraud ? at : kick);
    // as pcr_prim.vert
    vec2 ndc = gs_to_clip(xy);
    gl_Position = vec4(ndc, pcr_depth(z, zfrac, zref), 1.0);
    gl_ClipDistance[0] = cut * p.q * f;
    v_color = vec4(rgba);
    v_stq = stq;
    v_fog = float((w16 >> 4) & 0xFFu);
    v_rect = uvec2(PCR_NO_RECT);
}
