#version 450
#extension GL_GOOGLE_include_directive : require
// Native draw (port/gs/native/gsn_native_draw.inc): VU1's program 0x000 as a vertex shader. Each corner of
// a batch's triangles is made from the mesh's vertex as VU1 memory had it, and the draw's constants (what
// VU1's fixed block held, through VU1's set-up), into what gs.vert takes from a GS vertex: the same
// outputs, so every fragment shader of the native GS draws it. port/platform/vu1_hle.c is the
// specification:
//   c = vertex matrix * position; q = 1 / c.w; X, Y, Z = FTOI4(c.xyz * q) (12.4 and Z);
//   fog = clamp(fog.z + fog.w * q, 0, 255); dots = max(L' * normal, 0) (L' = the light directions by the
//   model's rotation); colour = min((C * dots + ambient) * vertex colour, 255) (alpha: the vertex's);
//   then each point light adds colour * max(1 - |light - position|^2 * light.w, 0), clamped at 255;
//   S, T, Q = ST * q (ST's z is 1: Q is q).
// The GS's numbers: X and Y as XYZF2 has them (the integer's low 16 bits, less XYOFFSET), Z an integer
// (Z / 16 of FTOI4's, as the GIF's PACKED XYZF2 takes it), RGBA bytes, fog F as an integer.
// VU1's arithmetic: every operation rounded toward zero, in the program's order (vu_* below: the GPU's
// result rounded to nearest, moved a unit toward zero where its exact error says it went past; the
// division made exact). The draw's constants come as VU1's set-up leaves them (gsn_native_draw.inc,
// nd_draw: the vertex matrix, the lights). Denormals are taken as the GPU has them (VU1 reads them as 0;
// the game's numbers do not come near them).
// In a picture between (ND_C_MOVING) the place is moved as gs_replay.comp moves the PS2 path's vertices,
// so that the two meet: from VU1's place (X, Y, Z exact, as in the frame) by the change of camera and
// the part's own move, seen from where the vertex is in the frame's view (its X, Y and 1 / q, as
// frame_interp.c takes the PS2 path's; plain floats: the move's rounding, not the place's; no move, no
// change); S, T and Q by the change of depth; the fog and the
// colour as in the frame. A triangle is not drawn when its kick or one of the two before it goes behind
// the camera or past the guard band (the ADC frame_interp.c gives such a vertex). The models' parts
// (ND_C_MODEL: map objects, the picked item) move by their own matrices t of the way from the frame before's
// (nd_between.h), the camera's move added (ND_C_EYE: not).
// Which triangles are drawn (the data's ADC bits, and VU1's clipping) is the CPU's (gsn_native_draw.inc,
// nd_plan): a corner's fog input is -1 less its vertex's number in the arena, and its colour's RGB the kick's
// number less it (bits 0-5: a flat triangle has the kick's colour), IIP (bit 6) and its draw's record
// (bits 7-23).
// Built a second time with ND_STANDALONE as sa_000.vert (P8, gsn_standalone.inc): the same, with the scene's own
// depth (Z before any rounding) in place of the GS's integer Z.
// Built a third time with ND_STANDALONE and PC_DRAW as pc_000.vert (--renderer fast, gsn_pc_draw.inc): a batch drawn from
// its mesh by the PC renderer, with no corners planned on the CPU. Its triangles are a list on the GPU (Tris: each the
// arena's numbers of its three corners and of its kick), a draw's record is its instance, and VU1's clipping, which
// gives the ADC that leaves a triangle out, is worked out here for the kick and the two before it, as nd_plan does.
// A layer's Z slope (the corner's alpha in the scene's draws, sa_slope) is worked out here too, from the triangle's
// three corners as the frame has them.
#include "gs_common.h"

#ifdef PC_DRAW
// the batch's triangles: x, y, z the arena's numbers of its corners; w the kick's (bits 0-27) and how many of the
// two kicks before it are the batch's (bits 28-29: VU1's clipping looks at those)
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
#include "vu1_000_draw.h"
layout(set = 2, binding = 1, std430) readonly buffer Draws {
    Draw draws[];
};

#ifdef ND_ORACLE
// The vertex oracle (nd_000_oracle.vert, built from this with ND_ORACLE: gsn_nd_oracle.inc): what each
// corner gives the GS, two qwords a corner: X, Y and Z as worked out below, XYZF2's w; RGBA, S, T, Q
layout(set = 2, binding = 2, std430) writeonly buffer Oracle {
    uvec4 o[];
} oracle;
#endif

#include "vu1_000_maths.h"

#ifdef PC_DRAW
// A triangle's Z slope as sa_slope has it (the corner's alpha of the scene's layers): from its corners' places in the
// frame, X and Y in GS pixels, Z the GS's; world (pc.zslope below 0): 32 log2(1 + g), g the Z's change across a GS
// pixel; else the larger of its changes in X and Y, halved, 255 a Z
uint tri_slope(uvec4 t) {
    Place p0 = vu_place(t.x), p1 = vu_place(t.y), p2 = vu_place(t.z);
    float ax = float((p1.x & 0xFFFF) - (p0.x & 0xFFFF)) / 16.0, ay = float((p1.y & 0xFFFF) - (p0.y & 0xFFFF)) / 16.0;
    float bx = float((p2.x & 0xFFFF) - (p0.x & 0xFFFF)) / 16.0, by = float((p2.y & 0xFFFF) - (p0.y & 0xFFFF)) / 16.0;
    float az = float(gs_z(p1)) - float(gs_z(p0)), bz = float(gs_z(p2)) - float(gs_z(p0));
    float det = ax * by - ay * bx;
    if (det == 0.0) {
        return 0u;
    }
    float dzdx = (az * by - bz * ay) / det, dzdy = (bz * ax - az * bx) / det;
    if (pc.zslope < 0.0) {
        float g = sqrt(dzdx * dzdx + dzdy * dzdy);
        return uint(min(32.0 * log2(1.0 + g) + 0.5, 255.0));
    }
    float e = min(0.5 * max(abs(dzdx), abs(dzdy)), 1.0);
    return uint(e * 255.0 + 0.5);
}
#endif

void main() {
#ifdef PC_DRAW
    uvec4 tri = tris[gl_VertexIndex / 3];
    uint which = uint(gl_VertexIndex) % 3u;
    uint at = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w & 0x0FFFFFFFu;
    uint befores = (tri.w >> 28) & 3u; // (the kicks before it in the batch, at most 2)
    uint instance = uint(gl_InstanceIndex);
    d = draws[instance & 0x3FFFFFFFu];
    uint corner = (instance >> 30) != 0u ? 0x40u : 0u; // (IIP: Gouraud)
    // VU1's ADC: the kick's clip and the two kicks' before it (nd_plan_batch), in the frame
    bool frame_gone = false;
    for (uint i = 0u; i <= befores && !frame_gone; i++) {
        frame_gone = vu_clipped(kick - i);
    }
    uvec4 a_corner = uvec4(0u, 0u, 0u, (pc.zslope != 0.0 && !frame_gone) ? tri_slope(tri) : 0u);
#else
    uint at = uint(-a_vertex) - 1u; // (-1 less it: nd_corner_vertex)
    uint corner = a_corner.r | a_corner.g << 8 | a_corner.b << 16;
    uint kick = at + (corner & 0x3Fu);
    d = draws[corner >> 7];
#endif
    ivec4 N = mesh.q[at * 4u + 1u], T = mesh.q[at * 4u + 2u];
    Place p = vu_place(at);
    // the fog: ACC.w = 1 * fog.z, then + fog.w * q; clamped to 0..255
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, p.q)), 255.0), 0.0);
    if ((d.info.w & ND_C_WAVE) != 0) {
        // the wave UVs on the GPU (P6-2): the vertex's own S and T moved by its wave's (ST's w) offsets in 16
        // bits, as Wobble_ApplyWaveUvs writes them, read back signed as VIF1's UNPACK does
        uint w = uint(T.w) & 15u;
        uint o = d.wave[w >> 2][w & 3u];
        T.x = (int(uint(T.x) + (o & 0xFFFFu)) << 16) >> 16;
        T.y = (int(uint(T.y) + (o >> 16)) << 16) >> 16;
    }
    vec3 st = vec3(T.xyz) / 4096.0; // (ITOF12: exact)
    vec3 stq = vec3(vu_mul(st.x, p.q), vu_mul(st.y, p.q), vu_mul(st.z, p.q));

    // XYZF2 as the GS takes it
    int xi = p.x, yi = p.y;
    uint z = gs_z(p);
#ifdef ND_STANDALONE
    float zf = p.zf;
#endif
    bool gone = false;
#ifdef PC_DRAW
    gone = frame_gone;
#endif
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
    if ((d.info.w & ND_C_FAR) != 0) {
        z = 0u; // at the far plane (the modern sky, P3-2: the Z buffer's clear value, GEQUAL)
#ifdef ND_STANDALONE
        zf = 0.0; // (the scene's far plane too: its depth's clear value, P8 T5)
#endif
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
    // The scene's own depth (P8, gsn_standalone.inc): D32F, larger nearer (the GS's Z is a + b / w: reversed
    // Z already), Z before any rounding, in the GS's units (Z / 2^24, so the GS's Z buffer and the scene's
    // hold the same numbers). pc.zbias: Z added for a tie's colour pass (gsn_standalone.inc, "Ties": a draw
    // passes within that many Z of the surface it lies on, as on the GS).
    // (not kept within the Z buffer's format at the vertex: a vertex past it, under the eye, would tilt its
    // triangle's plane, the lava's far edge behind the hills in teeth; the GS's Z is kept so per pixel, and
    // the scene's when it is given back, gsn_standalone.inc. Within the depth range only.)
    // (pc.zslope: a layer's colour pass, its triangle's slope term in the corner's alpha, 255 a Z: gsn_standalone.inc)
    float depth = clamp((zf + sa_slack(zf, p.q, d.screen[2].z, d.screen[0].x, a_corner.a)) * pc.zscale, 0.0, 1.0);
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
