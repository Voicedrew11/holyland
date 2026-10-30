#version 450
#extension GL_GOOGLE_include_directive : require
// Native draw (port/gs/native/gsn_nd_700.inc): VU1's program 0x700, the effects' (the sfx.dat models: fire,
// glows, magic), as a vertex shader: nd_000.vert's sibling, with the same inputs (its records, NdConst, and
// the arena's four qwords a vertex) and the same outputs, so every fragment shader of the native GS draws
// it, and a batch can hold its corners, 0x000's and the PS2 path's together (nd_draw_range draws each
// stretch with its own program). port/platform/vu1_hle_700.c is the specification. Per vertex:
//   c = vertex matrix * position; q = 1 / c.w; X, Y, Z = FTOI4(c.xyz * q);
//   fog = clamp(fog.z + fog.w * q, 0, 255); dots = max(L' * normal, 0);
//   colour = min((C * dots + ambient) * the model's colour (VU1 0x64, vf28), 255), FTOI0 (alpha: 1 * its w);
//   S, T, Q = ST * q.
// What differs from program 0x000: no vertex colour (the fourth qword is not read) but the model's, no
// point lights, and the texture animation (TEX0 replaced by VU1 0x66 when its TBP0 is 0x65.w), which is the
// CPU's (gsn_nd_700.inc: the A+D writes). The model's colour comes in the record's point_col[0] (NdConst:
// 0x700 has no point lights). The positions' own w is 16 in the game's effects, so c.w is 16 times the
// depth and Q is 1 / (16 w) (STEP3.md, "Higher frame rates": the fire fix); VU1's arithmetic does not care,
// the pictures between do: their place in the view is taken from the depth 1 / (q * the position's w), and
// the model's own move (nd_between.h, of the position as VU1 has it) divided by that w.
// VU1's arithmetic as in nd_000.vert (its vu_* helpers, copied: each operation rounded toward zero, in the
// program's order; the division exact). Which triangles are drawn (the data's ADC, VU1's clipping, and
// whether the strip is kicked at all: vertex n) is the CPU's (gsn_nd_700.inc, nd700_plan). A corner is
// nd_000.vert's: its fog -1 less its vertex's number in the arena, its colour's RGB the kick's number less
// its own (bits 0-5), IIP (bit 6) and its draw's record (bits 7-23).
// Built a second time with ND_STANDALONE as sa_700.vert (P8, gsn_standalone.inc): the same, with the scene's own
// depth (Z before any rounding, nd_000.vert's sa_000.vert) in place of the GS's integer Z.
// Built a third time with ND_STANDALONE and PC_DRAW as pc_700.vert (--renderer fast, gsn_pc_draw.inc): a strip drawn by the
// PC renderer with no corners: its triangles VU1 kicks (the CPU's plan) a list on the GPU (Tris: the arena's numbers of its
// three corners; w its kick's, bits 0-23, and the triangle's Z slope, 24-31, as sa_slope has it), the draw's record and
// IIP its instance.
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

// a picture between's (gs_replay.h's Picture; with nothing moving, a buffer of zeros)
layout(set = 1, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];   // the frame's view to the picture's: rows of the rotation less the identity, each with its translation
    vec4 mid[3];    // the world to the picture's view: rows of the rotation (and the translation)
    vec4 screen[2]; // (the frame's projections: the draw has its own)
    vec4 zt;        // cz (the world's, the reflection's), t, 1 - t
#ifdef ND_JITTER
    vec4 jitter;    // (upscaling, sa_*_jitter.vert) gs_replay.h's counts: zw the picture's sub-pixel jitter in NDC
#endif
} pic;
// the meshes (the arena): four qwords a vertex (position, normal, ST, colour), as VU1 memory had them
layout(set = 2, binding = 0, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "vu1_700_draw.h"
layout(set = 2, binding = 1, std430) readonly buffer Draws {
    Draw draws[];
};

#ifdef ND_ORACLE
// The vertex oracle (nd_700_oracle.vert, built from this with ND_ORACLE: gsn_nd_700.inc): what each
// corner gives the GS, two qwords a corner: X, Y and Z as worked out below, XYZF2's w; RGBA, S, T, Q
layout(set = 2, binding = 2, std430) writeonly buffer Oracle {
    uvec4 o[];
} oracle;
#endif

#include "vu1_700_maths.h"

void main() {
#ifdef PC_DRAW
    uvec4 tri = tris[gl_VertexIndex / 3];
    uint which = uint(gl_VertexIndex) % 3u;
    uint at = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w & 0xFFFFFFu;
    uint instance = uint(gl_InstanceIndex);
    uint corner = (instance & 0x3FFFFFFFu) << 7 | ((instance >> 30) & 1u) << 6;
    uvec4 a_corner = uvec4(0u, 0u, 0u, tri.w >> 24);
#else
    uint at = uint(-a_vertex) - 1u; // (-1 less it: nd_corner_vertex)
    uint corner = a_corner.r | a_corner.g << 8 | a_corner.b << 16;
    uint kick = at + (corner & 0x3Fu);
#endif
    d = draws[corner >> 7];
    ivec4 N = mesh.q[at * 4u + 1u], T = mesh.q[at * 4u + 2u];
    Place p = vu_place(at);
    // the fog: ACC.w = 1 * fog.z, then + fog.w * q; clamped to 0..255
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, p.q)), 255.0), 0.0);
    vec3 st = vec3(T.xyz) / 4096.0; // (ITOF12: exact)
    vec3 stq = vec3(vu_mul(st.x, p.q), vu_mul(st.y, p.q), vu_mul(st.z, p.q));

    // XYZF2 as the GS takes it
    int xi = p.x, yi = p.y;
    uint z = gs_z(p);
#ifdef ND_STANDALONE
    float zf = p.zf;
#endif
    bool gone = false;
    if ((d.info.w & ND_C_MOVING) != 0) {
        Moved m = moved(at, p);
        xi = m.x;
        yi = m.y;
        z = m.z;
#ifdef ND_STANDALONE
        zf = m.zf;
#endif
        stq *= m.f;
        // dropped when its kick or one of the two kicks before it is (gs_replay_corners.comp: the ADC
        // frame_interp.c gives a vertex it drops and the two after it), whichever vertices the triangle
        // has: the same for its three corners. (Not the corner's own: a fan's first vertex is a corner of
        // every triangle but in the window of the first only.)
        for (uint i = 0u; i < 3u && !gone; i++) {
            uint k = kick - i;
            gone = k == at ? m.drop : moved(k, vu_place(k)).drop;
        }
    }
    uint w16 = (uint(ftoi(fogf * 16.0)) | uint(N.w)) & 0xFFFFu;
    float fog = float((w16 >> 4) & 0xFFu);
    vec2 xy = vec2(float(int(uint(xi) & 0xFFFFu) - d.info.y), float(int(uint(yi) & 0xFFFFu) - d.info.z)) / 16.0;
    if (gone) {
        xy = vec2(1.0e9); // (far past the target's right edge, as are its two other corners: not drawn)
    }

    uvec4 rgba = lit_colour((corner & 0x40u) != 0u ? at : kick);

    // as gs.vert
    vec2 ndc = (xy + 0.5) * pc.inv_size * 2.0 - 1.0;
#ifdef ND_STANDALONE
    // the scene's own depth, its tie slack and a layer's slope term (pc.zbias, pc.zslope): as sa_000.vert's
    float depth = clamp((zf + sa_slack(zf, p.q * max(float(mesh.q[at * 4u].w), 1.0), d.screen[2].z, d.screen[0].x, a_corner.a)) * pc.zscale, 0.0, 1.0);
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
