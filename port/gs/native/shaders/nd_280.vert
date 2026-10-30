#version 450
#extension GL_GOOGLE_include_directive : require
// Native draw (port/gs/native/gsn_nd_280.inc): VU1's program 0x280, the skinned models' (enemies, the arm,
// the held weapon), as a vertex shader: nd_000.vert's sibling, with the same outputs, so every fragment
// shader of the native GS draws it. port/platform/vu1_hle_280.c is the specification. Per vertex of a
// batch (one VU1 run):
//   the vertex's set s (its ST's w: 0 the part's matrices, else its parent's: rigid skinning, no weights);
//   c = vertex matrix[s] * position; q = 1 / c.w; X, Y, Z = FTOI4(c.xyz * q);
//   fog = clamp(fog.z + fog.w * q, 0, 255); dots = max(L[s] * normal, 0);
//   u = colour * scale + bias for vertex 0, then for vertex k its own alpha but the RGB of the vertex
//   before it carried on: u.rgb = u_prev.rgb * scale + bias (the program's pipeline loads only the alpha
//   after the first vertex); colour = min((C * dots + ambient) * u, 255) (alpha: u's), FTOI0;
//   each point light (in the vertex's set; vertex 0 takes the batch tag's: the parser's qword 4) adds
//   colour * max(1 - |light - position|^2 * light A's w, 0), clamped at 255;
//   S, T, Q = ST * q.
// The second pass (the "phong" shine: gsn_nd_280.inc draws it when the batch has it): the same XYZF2,
// ST = ((sphere matrix[s] * normal).xy + 1) / 2 and Q 1, RGBA (255, 255, 255, vertex 1's alpha).
// VU1's arithmetic as in nd_000.vert (the vu_* helpers below are its: each operation rounded toward zero,
// in the program's order; the division exact). The constants come as VU1's set-up leaves them
// (gsn_nd_280.inc, nd280_const). Which triangles are drawn, and whether a batch draws at all (VU1 ends
// the run when every vertex is clipped), are the CPU's (nd280_plan).
// The arena (set 2, binding 0) holds a vertex in eight qwords (render.cpp, take_batch_280): position,
// normal, ST, colour as VU1 memory had them (floats), then (k, the point lights' set, what XYZF2's w is
// ORed with, n). A corner's fog input is -1 less 2^22 less its vertex's first qword / 4 in the arena (below
// 0x000's corners', so that a batch holds both kinds: nd280_corner_vertex, nd_draw_range); its colour's RGB the
// kick's number less its own, in vertices (bits 0-5), IIP (bit 6), the draw's record (bits 7-21), the
// context whose XYOFFSET (bit 22) and the pass (bit 23).
// Pictures between (P2-5; ND_C_MOVING): the place moved as nd_000.vert's moved does it (from VU1's X, Y and
// Z by the picture's camera, as gs_replay.comp moves the PS2 path's vertices), the part's own move added
// (ND_C_MODEL: by the vertex's set's matrices t of the way from the frame before's, nd_between.h; with
// ND_C_EYE, the arm's, in the eye's space and without the camera's move); S, T and Q of the first pass by
// the change of depth (the second's are a direction: as they are); a triangle not drawn when its kick or
// one of the two before it is dropped. With nothing moving the Picture (set 1) is not read.
// Built a second time with ND_STANDALONE as sa_280.vert (P8, gsn_standalone.inc): the same, with the scene's own
// depth (Z before any rounding, nd_000.vert's sa_000.vert) in place of the GS's integer Z.
// Built a third time with ND_STANDALONE and PC_DRAW as pc_280.vert (--renderer fast, gsn_pc_draw.inc): a pass of a batch
// drawn by the PC renderer with no corners: its triangles VU1 kicks (the CPU's plan) a list on the GPU (Tris: the arena's
// slots of its three corners and of its kick), and the draw's record, IIP, the pass and the context its instance.
#include "gs_common.h"

#ifdef PC_DRAW
layout(set = 2, binding = 2, std430) readonly buffer Tris {
    uvec4 tris[];
};
#else
layout(location = 2) in uvec4 a_corner;
layout(location = 4) in float a_vertex;
#endif

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) noperspective out float v_z;

// a picture between's (as nd_000.vert's; with nothing moving, a buffer of zeros)
layout(set = 1, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;
#ifdef ND_JITTER
    vec4 jitter;    // (upscaling, sa_*_jitter.vert) gs_replay.h's counts: zw the picture's sub-pixel jitter in NDC
#endif
} pic;
layout(set = 2, binding = 0, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "vu1_280_draw.h"
layout(set = 2, binding = 1, std430) readonly buffer Draws {
    Draw draws[];
};

#ifdef ND_ORACLE
// the vertex oracle (nd_280_oracle.vert: gsn_nd_280.inc's oracle): what each corner gives the GS, two
// qwords a corner: X, Y and Z as worked out below, XYZF2's w; RGBA, S, T, Q
layout(set = 2, binding = 2, std430) writeonly buffer Oracle {
    uvec4 o[];
} oracle;
#endif

#include "vu1_280_maths.h"

void main() {
#ifdef PC_DRAW
    uvec4 tri = tris[gl_VertexIndex / 3];
    uint which = uint(gl_VertexIndex) % 3u;
    uint slot = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w;
    // the instance: the record (bits 0-14), the context (28), the pass (29), IIP (30), as a corner has them
    uint instance = uint(gl_InstanceIndex);
    uint corner = (instance & 0x7FFFu) << 7 | ((instance >> 28) & 1u) << 22 | ((instance >> 29) & 1u) << 23 |
                  ((instance >> 30) & 1u) << 6;
    uvec4 a_corner = uvec4(0u); // (a skinned model's ties are exact: no slope)
#else
    uint slot = uint(-a_vertex) - 0x400001u; // (-1 less 2^22 less it: nd280_corner_vertex)
    uint corner = a_corner.r | a_corner.g << 8 | a_corner.b << 16;
    uint kick = slot + 2u * (corner & 0x3Fu);
#endif
    bool sphere = (corner & 0x800000u) != 0u;
    d = draws[(corner >> 7) & 0x7FFFu];
    ivec2 offset = (corner & 0x400000u) != 0u ? d.offset.zw : d.offset.xy;
    ivec4 X = mesh.q[slot * 4u + 4u];
    int s = vertex_set(slot);

    // the place: XYZF2 (the same in both passes)
    Place pl = vu_place(slot);
    float q = pl.q;
    int xi = pl.x, yi = pl.y;
    uint z = uint(pl.z >> 4) & 0xFFFFFFu;
#ifdef ND_STANDALONE
    float zf = pl.zf;
#endif
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, q)), 255.0), 0.0);
    uint w16 = (uint(ftoi(fogf * 16.0)) | uint(X.z)) & 0xFFFFu;
    float fog = float((w16 >> 4) & 0xFFu);
    // a picture between's (ND_C_MOVING): moved, S, T and Q by the change of depth; dropped with its kick or
    // one of the two before it in the batch (gs_replay_corners.comp's window), the same for all three corners
    float f = 1.0;
    bool gone = false;
    if ((d.info.y & ND_C_MOVING) != 0) {
        Moved m = moved(slot, s, pl);
        xi = m.x;
        yi = m.y;
        z = m.z;
#ifdef ND_STANDALONE
        zf = m.zf;
#endif
        f = m.f;
        int kk = mesh.q[kick * 4u + 4u].x; // (the kick's number in the batch)
        for (int i = 0; i < 3 && i <= kk && !gone; i++) {
            uint k = kick - 2u * uint(i);
            gone = k == slot ? m.drop : moved(k, vertex_set(k), vu_place(k)).drop;
        }
    }
    vec2 xy = vec2(float(int(uint(xi) & 0xFFFFu) - offset.x), float(int(uint(yi) & 0xFFFFu) - offset.y)) / 16.0;
    if (gone) {
        xy = vec2(1.0e9); // (far past the target's right edge, as are its two other corners: not drawn)
    }

    vec3 stq;
    uvec4 rgba;
    if (!sphere) {
        vec4 st = qf(slot, 2u);
        stq = vec3(vu_mul(st.x, q), vu_mul(st.y, q), vu_mul(st.z, q)) * f;
        rgba = lit_colour((corner & 0x40u) != 0u ? slot : kick);
    } else {
        // ((Ms * normal).xy + 1 * 1) * 0.5; Q 1; white, with vertex 1's alpha (its u's: lit.w is 1)
        vec4 acc = vu_transform3(d.sphere[s], qf(slot, 1u).xyz);
        stq = vec3(vu_mul(vu_add(acc.x, 1.0), 0.5), vu_mul(vu_add(acc.y, 1.0), 0.5), 1.0);
        uint one = slot - 2u * uint(X.x) + 2u;
        float a1 = min(vu_add(vu_mul(qf(one, 3u).w, d.scale.w), d.bias.w), 255.0);
        rgba = uvec4(255u, 255u, 255u, uint(ftoi(a1)) & 0xFFu);
    }

    // as gs.vert
    vec2 ndc = (xy + 0.5) * pc.inv_size * 2.0 - 1.0;
#ifdef ND_STANDALONE
    // the scene's own depth, its tie slack and a layer's slope term (pc.zbias, pc.zslope): as sa_000.vert's
    float depth = clamp((zf + sa_slack(zf, q, d.screen[2].z, d.screen[0].x, a_corner.a)) * pc.zscale, 0.0, 1.0);
#else
    float depth = (pc.misc & GSN_M_ZCLAMP) != 0u ? 0.0 : float(min(z, pc.zmax)) * pc.zscale;
#endif
#ifdef ND_JITTER
    if ((pc.misc & GSN_M_JITTER) != 0u) {
        ndc += pic.jitter.zw; // (an upscaler's sub-pixel jitter: the scene's frame only, gsn_standalone.inc's sa_jitter)
    }
#endif
    gl_Position = vec4(ndc, depth, 1.0);
    v_z = float(z);
    v_color = vec4(rgba);
    v_stq = stq;
    v_fog = fog;
#ifdef ND_ORACLE
    oracle.o[gl_VertexIndex * 2] = uvec4(uint(xi), uint(yi), z, w16);
    oracle.o[gl_VertexIndex * 2 + 1] = uvec4(rgba.r | rgba.g << 8 | rgba.b << 16 | rgba.a << 24, floatBitsToUint(v_stq));
#endif
}
