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
#include "nd_between.h"
// the draws (gsn_native_draw.inc, NdConst), a record each: matrices a VU qword a column
struct Draw {
    mat4 vertex;       // VU1 0x14: the set-up's vertex matrix (0x00 by 0x04)
    mat4 model;        // VU1 0x04: the part to the world (not read yet)
    mat4 view;         // world to view (not read yet)
    mat4 screen;       // view to screen (pictures between: sx, sy, cx, cy, cz)
    mat4 light_dir;    // vf05-vf07: the light directions by the part's rotation, a column each
    mat4 light_col;    // VU1 0x10: the light colours, then the ambient
    vec4 fog;          // VU1 0x3C
    vec4 point_pos[4]; // in the model's space, w the falloff
    vec4 point_col[4];
    vec4 obj;          // (ND_C_OBJ) the part's move from the frame before to this one, in the world
    ivec4 info;        // the point lights, XYOFFSET X and Y (12.4), ND_C_*
    Between bt;        // (ND_C_MODEL) the part's own move from the frame before (nd_between.h)
    uvec4 wave[4];     // (ND_C_WAVE) the 16 waves' offsets to S (low 16 bits) and T (high)
};
layout(set = 2, binding = 1, std430) readonly buffer Draws {
    Draw draws[];
};

#define ND_C_MOVING 1
#define ND_C_OBJ 2
#define ND_C_MODEL 4
#define ND_C_EYE 8
#define ND_C_WAVE 16
#define ND_C_FAR 32

#ifdef ND_ORACLE
// The vertex oracle (nd_000_oracle.vert, built from this with ND_ORACLE: gsn_nd_oracle.inc): what each
// corner gives the GS, two qwords a corner: X, Y and Z as worked out below, XYZF2's w; RGBA, S, T, Q
layout(set = 2, binding = 2, std430) writeonly buffer Oracle {
    uvec4 o[];
} oracle;
#endif

Draw d;

// FTOI: toward zero, kept within what an int holds
int ftoi(float v) {
    return int(clamp(v, -2147483520.0, 2147483520.0));
}

// One unit of the last place toward zero
float toward_zero(float r) {
    return r == 0.0 ? r : uintBitsToFloat(floatBitsToUint(r) - 1u);
}

// a * b, rounded toward zero: the GPU's product, less a unit where it is past the exact one
float vu_mul(float a, float b) {
    precise float r = a * b;
    precise float e = fma(a, b, -r);
    return e != 0.0 && (e < 0.0) != (r < 0.0) ? toward_zero(r) : r;
}

// a + b, rounded toward zero (the sum's error by Knuth's two-sum)
float vu_add(float a, float b) {
    precise float s = a + b;
    precise float bb = s - a;
    precise float e = (a - (s - bb)) + (b - bb);
    return e != 0.0 && (e < 0.0) != (s < 0.0) ? toward_zero(s) : s;
}

vec4 vu_add4(vec4 a, vec4 b) {
    return vec4(vu_add(a.x, b.x), vu_add(a.y, b.y), vu_add(a.z, b.z), vu_add(a.w, b.w));
}

vec4 vu_mul4(vec4 a, vec4 b) {
    return vec4(vu_mul(a.x, b.x), vu_mul(a.y, b.y), vu_mul(a.z, b.z), vu_mul(a.w, b.w));
}

// m v, as MULAx ACC; MADDAy; MADDAz; MADDw (each product rounded, then the sum)
vec4 vu_transform(mat4 m, vec4 v) {
    vec4 acc = vu_mul4(m[0], vec4(v.x));
    acc = vu_add4(acc, vu_mul4(m[1], vec4(v.y)));
    acc = vu_add4(acc, vu_mul4(m[2], vec4(v.z)));
    return vu_add4(acc, vu_mul4(m[3], vec4(v.w)));
}

// 1 / w rounded toward zero (DIV): the GPU's reciprocal made the nearest by a Newton step, then the
// largest whose product with |w| is not over 1; by 0 the largest float with w's sign
float vu_recip(float w) {
    if ((floatBitsToUint(w) & 0x7F800000u) == 0u) {
        return uintBitsToFloat((floatBitsToUint(w) & 0x80000000u) | 0x7FFFFFFFu);
    }
    float aw = abs(w);
    precise float r = 1.0 / aw;
    precise float e = fma(-aw, r, 1.0);
    r = fma(r, e, r);
    for (int i = 0; i < 2; i++) {
        float up = uintBitsToFloat(floatBitsToUint(r) + 1u);
        if (fma(-aw, r, 1.0) < 0.0) {
            r = uintBitsToFloat(floatBitsToUint(r) - 1u);
        } else if (fma(-aw, up, 1.0) >= 0.0) {
            r = up;
        }
    }
    return w < 0.0 ? -r : r;
}

// The vertex's colour as VU1 leaves it in RGBAQ (the lights; the point lights' passes)
uvec4 lit_colour(uint at) {
    ivec4 P = mesh.q[at * 4u], N = mesh.q[at * 4u + 1u], C = mesh.q[at * 4u + 3u];
    vec3 n = vec3(N.xyz) / 4096.0; // (ITOF12: exact)
    // the light dots: MULAx ACC, l0, n.x; MADDAy; MADDz; floored at 0
    vec4 acc = vu_mul4(d.light_dir[0], vec4(n.x));
    acc = vu_add4(acc, vu_mul4(d.light_dir[1], vec4(n.y)));
    vec4 dots = max(vu_add4(acc, vu_mul4(d.light_dir[2], vec4(n.z))), vec4(0.0));
    // the colours by the dots, the ambient added; w 1
    acc = vu_mul4(d.light_col[0], vec4(dots.x));
    acc = vu_add4(acc, vu_mul4(d.light_col[1], vec4(dots.y)));
    vec4 lit = vu_add4(acc, vu_mul4(d.light_col[2], vec4(dots.z)));
    lit = vec4(vu_add(lit.x, d.light_col[3].x), vu_add(lit.y, d.light_col[3].y), vu_add(lit.z, d.light_col[3].z), 1.0);
    ivec4 c = ivec4(min(vu_mul4(lit, vec4(C)), vec4(255.0)));
    vec3 pf = vec3(P.xyz);
    for (int l = 0; l < d.info.x && l < 4; l++) {
        vec4 lp = d.point_pos[l];
        vec3 dd = vec3(vu_add(lp.x, -pf.x), vu_add(lp.y, -pf.y), vu_add(lp.z, -pf.z));
        float p = vu_add(vu_add(vu_mul(dd.x, dd.x), vu_mul(dd.y, dd.y)), vu_mul(dd.z, dd.z));
        float att = vu_add(1.0, -vu_mul(p, lp.w));
        vec3 a = max(vec3(vu_mul(d.point_col[l].x, att), vu_mul(d.point_col[l].y, att), vu_mul(d.point_col[l].z, att)),
                     vec3(0.0));
        vec3 sum = vec3(vu_add(float(c.x), a.x), vu_add(float(c.y), a.y), vu_add(float(c.z), a.z));
        c.xyz = ivec3(min(sum, vec3(255.0)));
    }
    return uvec4(c) & 0xFFu;
}

// A vertex's place as VU1 leaves it: FTOI4 of X, Y and Z (X and Y the GS's 12.4), and q = 1 / w
struct Place {
    int x, y, z;
    float q;
#ifdef ND_STANDALONE
    float zf; // (sa_000.vert) Z before FTOI4: what the scene's depth is made of, unrounded
#endif
};

Place vu_place(uint at) {
    vec4 c = vu_transform(d.vertex, vec4(mesh.q[at * 4u]));
    Place p;
    p.q = vu_recip(c.w);
    p.x = ftoi(vu_mul(c.x, p.q) * 16.0);
    p.y = ftoi(vu_mul(c.y, p.q) * 16.0);
    p.z = ftoi(vu_mul(c.z, p.q) * 16.0);
#ifdef ND_STANDALONE
    p.zf = vu_mul(c.z, p.q);
#endif
    return p;
}

// The GS's Z of a place
uint gs_z(Place p) {
    return uint(p.z >> 4) & 0xFFFFFFu;
}

// n / (dh + dl), the float nearest it: the GPU's quotient, less its remainder's share
float div_nearest(float n, float dh, float dl) {
    precise float r = n / dh;
    for (int i = 0; i < 2; i++) {
        precise float e = fma(-r, dh, n) - r * dl;
        r = r + e / dh;
    }
    return r;
}

// (X - cx) / (q sx): a place in the view across, from the screen's (frame_interp.c's (X - cx) depth / sx)
float view_place(float n, float q, float s) {
    precise float dh = q * s;
    precise float dl = fma(q, s, -dh);
    return div_nearest(n, dh, dl);
}

// A picture between's place (gs_replay.comp's moved): X and Y 12.4, Z, the depth's change (f), dropped
struct Moved {
    int x, y;
    uint z;
    float f;
    bool drop;
#ifdef ND_STANDALONE
    float zf; // (sa_000.vert) Z unrounded
#endif
};

Moved moved(uint at, Place p) {
    Moved r;
    r.x = p.x & 0xFFFF;
    r.y = p.y & 0xFFFF;
    r.z = gs_z(p);
    r.f = 1.0;
    r.drop = false;
#ifdef ND_STANDALONE
    r.zf = p.zf;
#endif
    // its place in the frame's view, from its X and Y and its depth 1 / q (frame_interp.c's vertex_view:
    // the PS2 path's vertices are moved from the same place, and the mirror image's from its own), and
    // how far the picture moves it: (R - I) v + T, and with its model t of the way from its place in the
    // frame before (seen from the camera between)
    // (frame_interp.c works in doubles and hands over floats: each is the float nearest the exact value
    // here too, the GPU's quotient corrected by its remainder)
    float depth = div_nearest(1.0, p.q, 0.0);
    if (!(depth > 0.0) || isinf(depth)) {
        return r; // (as it is)
    }
    vec3 v = vec3(view_place(float(r.x) / 16.0 - d.screen[2].x, p.q, d.screen[0].x),
                  view_place(float(r.y) / 16.0 - d.screen[2].y, p.q, d.screen[1].y), depth);
    vec3 dm = vec3(dot(pic.move[0].xyz, v) + pic.move[0].w, dot(pic.move[1].xyz, v) + pic.move[1].w,
                   dot(pic.move[2].xyz, v) + pic.move[2].w);
    if ((d.info.w & ND_C_OBJ) != 0) {
        vec3 e = -d.obj.xyz + d.obj.xyz * pic.zt.z;
        dm += vec3(dot(pic.mid[0].xyz, e), dot(pic.mid[1].xyz, e), dot(pic.mid[2].xyz, e));
    }
    if ((d.info.w & ND_C_MODEL) != 0) {
        // (the models': by the part's own matrices, nd_between.h; in the eye's space, the arm's, alone)
        vec3 e = between_move(d.bt, vec4(mesh.q[at * 4u]), pic.zt.w);
        dm = (d.info.w & ND_C_EYE) != 0 ? e : dm + vec3(dot(pic.mid[0].xyz, e), dot(pic.mid[1].xyz, e), dot(pic.mid[2].xyz, e));
    }
    float mz = v.z + dm.z;
    if (!(mz > 0.0)) {
        r.drop = true;
        return r;
    }
    // X = sx m.x / m.z + cx, as VU1's X and how far it moved: sx (m.x / m.z - v.x / v.z); Z = cz + az / m.z,
    // as VU1's Z less (Z - cz) dm.z / m.z
    float den = mz * v.z;
    float nx = float(r.x) / 16.0 + d.screen[0].x * (dm.x * v.z - v.x * dm.z) / den;
    float ny = float(r.y) / 16.0 + d.screen[1].y * (dm.y * v.z - v.y * dm.z) / den;
    float nz = float(r.z) - (float(r.z) - d.screen[2].z) * dm.z / mz;
    if (!(abs(nx - 2048.0) <= 2047.0 && abs(ny - 2048.0) <= 2047.0 && nz >= 0.0)) {
        r.drop = true;
        return r;
    }
    r.x = int(floor(nx * 16.0 + 0.5));
    r.y = int(floor(ny * 16.0 + 0.5));
    r.z = uint(min(floor(nz + 0.5), 16777215.0));
#ifdef ND_STANDALONE
    // (the scene's depth moved from its own unrounded Z, as the frame has it: from the GS's Z, FTOI4 and the
    // shift down to whole Z, a picture between's corners lay up to a Z farther than the frame's; at a distance a Z
    // is many world units, and the layers tied to their surface (the light on a wall) failed there in patches)
    r.zf = r.zf - (r.zf - d.screen[2].z) * dm.z / mz;
#endif
    r.f = depth / mz;
    return r;
}

#ifdef PC_DRAW
// VU1's clip judgement of a place (nd_plan_vertex_vu): past the guard band (2047 about the 2048 middle) in X or Y, or Z
// below 0
bool vu_clipped(uint at) {
    vec4 c = vu_transform(d.vertex, vec4(mesh.q[at * 4u]));
    float q = vu_recip(c.w);
    float x = vu_mul(c.x, q), y = vu_mul(c.y, q), z = vu_mul(c.z, q);
    float rx = vu_add(x, -2048.0), ry = vu_add(y, -2048.0);
    return !(abs(rx) <= 2047.0 && abs(ry) <= 2047.0) || ((floatBitsToUint(z) & 0x80000000u) != 0u && (floatBitsToUint(z) & 0x7F800000u) != 0u);
}

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
