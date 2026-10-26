#version 450
#extension GL_GOOGLE_include_directive : require
// Native draw (port/gs/native/gsn_nd_water.inc): the reflection's rippled water, Wobble_DrawRippledModel
// (gfx/wobble.c) as a vertex shader. No VU1 program: the EE's maths, VU0's arithmetic for the places and the
// host's floats for the rest, as gsn_nd_water.inc's top says (and its plan does on the CPU):
//   place: FTOI4 of (vertex matrix * (x, y, z, 1)) / w, Z >> 4 (XYZ2: all 32 bits);
//   S, T: the place of (x, y + sine * 5, z, 1) (the sine wobble_sines[|(x + z)^3 % 32|]), (X - 0x6C00) /
//   10240 and (Y - 0x7200) / 7168, the quotients correctly rounded; Q 1;
//   the colour RGBAQ's, the fog FOG's (XYZ2).
// The GS's numbers, as nd_000.vert gives them to gs.vert's fragment shaders. In a picture between
// (ND_C_MOVING) both places are moved by the picture's camera as nd_000.vert moves a vertex (gs_replay.comp's
// moved(), the same code), and S and T are made from the moved one's place: the reflection's lookup follows
// the camera between. A triangle is not drawn when its kick or one of the two before it is dropped (as
// nd_000.vert's). Which triangles are drawn in the frame (the strip's ADC: the data's and the EE's bounds
// test) is the CPU's (the plan): a corner's fog is -1 less its vertex's number in the arena, its colour's RGB
// the kick's number less it (bits 0-5) and its draw's record (bits 7-23).
// Built a second time with ND_STANDALONE as sa_water.vert (P8 T3, gsn_scene_targets.inc): the same, with the
// scene's own depth (Z before any rounding, as sa_000.vert's) in place of the GS's integer Z.
#include "gs_common.h"

layout(location = 2) in uvec4 a_corner;
layout(location = 4) in float a_vertex;

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) noperspective out float v_z;

// a picture between's (gs_replay.h's Picture; with nothing moving, a buffer of zeros)
layout(set = 1, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];   // the frame's view to the picture's: rows of the rotation less the identity, each with its translation
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;        // cz (the world's, the reflection's), t, 1 - t
#ifdef ND_JITTER
    vec4 jitter;    // (upscaling, sa_*_jitter.vert) gs_replay.h's counts: zw the picture's sub-pixel jitter in NDC
#endif
} pic;
// the meshes (the arena): four qwords a vertex (position, normal, ST, colour), as VU1 memory would have them
layout(set = 2, binding = 0, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
// the draws (gsn_nd_water.inc, NdWaterConst)
struct Draw {
    mat4 vertex;   // the world to screen matrix by the part's
    mat4 screen;   // view to screen (pictures between: sx, sy, cx, cy, cz)
    vec4 sines[8]; // wobble_sines
    ivec4 info;    // XYOFFSET X and Y (12.4), ND_C_*, RGBAQ's RGBA
    vec4 fog;      // FOG's fog in x
};
layout(set = 2, binding = 1, std430) readonly buffer Draws {
    Draw draws[];
};

#define ND_C_MOVING 1
#define NDW_C_NEAREST 0x100 // the game's floats round to nearest (--fpu-round nearest), not toward zero

Draw d;

// FTOI: toward zero, kept within what an int holds
int ftoi(float v) {
    return int(clamp(v, -2147483520.0, 2147483520.0));
}

// One unit of the last place toward zero
float toward_zero(float r) {
    return r == 0.0 ? r : uintBitsToFloat(floatBitsToUint(r) - 1u);
}

// a * b, rounded toward zero (VU arithmetic: nd_000.vert's)
float vu_mul(float a, float b) {
    precise float r = a * b;
    precise float e = fma(a, b, -r);
    return e != 0.0 && (e < 0.0) != (r < 0.0) ? toward_zero(r) : r;
}

// a + b, rounded toward zero
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

// m v, as MULAx ACC; MADDAy; MADDAz; MADDw
vec4 vu_transform(mat4 m, vec4 v) {
    vec4 acc = vu_mul4(m[0], vec4(v.x));
    acc = vu_add4(acc, vu_mul4(m[1], vec4(v.y)));
    acc = vu_add4(acc, vu_mul4(m[2], vec4(v.z)));
    return vu_add4(acc, vu_mul4(m[3], vec4(v.w)));
}

// 1 / w rounded toward zero (DIV; nd_000.vert's)
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

// A place as Vu0_ProjectToScreen leaves it: FTOI4 of X, Y and Z, and q = 1 / w
struct Place {
    int x, y, z;
    float q;
#ifdef ND_STANDALONE
    float zf; // (sa_water.vert) Z before FTOI4, unrounded
#endif
};

Place vu_place(vec4 v) {
    vec4 c = vu_transform(d.vertex, v);
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

// The game's floats (its threads round toward zero, as the EE's FPU; to nearest with --fpu-round nearest)
bool nearest() {
    return (d.info.z & NDW_C_NEAREST) != 0;
}

// n as a float
float game_float(int n) {
    uint m = n < 0 ? 0u - uint(n) : uint(n);
    float f = float(m);
    if (!nearest() && f > 16777216.0 && uint(f) > m) {
        f = toward_zero(f); // (past 2^24 the GPU's nearest may be above it)
    }
    return n < 0 ? -f : f;
}

// n / b, y the float nearest 1 / b: the nearest quotient (Markstein: the residual by a fused multiply-add,
// then one step; checked against the exact quotients of every n the screen gives and more), then toward
// zero where it went past (its residual's sign)
float game_div(float n, float b, float y) {
    precise float q = n * y;
    precise float r = fma(-q, b, n);
    precise float s = fma(r, y, q);
    precise float e = fma(-s, b, n);
    return !nearest() && e != 0.0 && (e < 0.0) != (s < 0.0) ? toward_zero(s) : s;
}

// S or T from a place's X or Y (12.4): (X - 0x6C00) / 10240, (Y - 0x7200) / 7168
vec2 screen_st(int x, int y) {
    return vec2(game_div(game_float(x - 0x6C00), 10240.0, uintBitsToFloat(0x38CCCCCDu)),
                game_div(game_float(y - 0x7200), 7168.0, uintBitsToFloat(0x39124925u)));
}

// The sine a vertex at x, z takes: wobble_sines[|(x + z)^3 % 32|] in 32-bit ints
float sine(int x, int z) {
    int i = x + z;
    int c = i * i * i;
    uint magnitude = c < 0 ? 0u - uint(c) : uint(c);
    uint at = magnitude & 31u;
    return d.sines[at >> 2][at & 3u];
}

// n / (dh + dl), the float nearest it (nd_000.vert's)
float div_nearest(float n, float dh, float dl) {
    precise float r = n / dh;
    for (int i = 0; i < 2; i++) {
        precise float e = fma(-r, dh, n) - r * dl;
        r = r + e / dh;
    }
    return r;
}

float view_place(float n, float q, float s) {
    precise float dh = q * s;
    precise float dl = fma(q, s, -dh);
    return div_nearest(n, dh, dl);
}

// A picture between's place (nd_000.vert's moved, by the camera only): X and Y 12.4 (the 16 bits), Z
struct Moved {
    int x, y;
    uint z;
    bool drop;
#ifdef ND_STANDALONE
    float zf; // (sa_water.vert) Z unrounded
#endif
};

Moved moved(Place p, uint z) {
    Moved r;
    r.x = p.x & 0xFFFF;
    r.y = p.y & 0xFFFF;
    r.z = z;
    r.drop = false;
#ifdef ND_STANDALONE
    r.zf = p.zf;
#endif
    float depth = div_nearest(1.0, p.q, 0.0);
    if (!(depth > 0.0) || isinf(depth)) {
        return r; // (as it is)
    }
    vec3 v = vec3(view_place(float(r.x) / 16.0 - d.screen[2].x, p.q, d.screen[0].x),
                  view_place(float(r.y) / 16.0 - d.screen[2].y, p.q, d.screen[1].y), depth);
    vec3 dm = vec3(dot(pic.move[0].xyz, v) + pic.move[0].w, dot(pic.move[1].xyz, v) + pic.move[1].w,
                   dot(pic.move[2].xyz, v) + pic.move[2].w);
    float mz = v.z + dm.z;
    if (!(mz > 0.0)) {
        r.drop = true;
        return r;
    }
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
    r.z = uint(min(floor(nz + 0.5), 4294967040.0));
#ifdef ND_STANDALONE
    // (the scene's depth moved from its own unrounded Z, as the frame has it: from the GS's Z, FTOI4 and the
    // shift down to whole Z, a picture between's corners lay up to a Z farther than the frame's; at a distance a Z
    // is many world units, and the layers tied to their surface (the light on a wall) failed there in patches)
    r.zf = r.zf - (r.zf - d.screen[2].z) * dm.z / mz;
#endif
    return r;
}

// A vertex's place in the frame (the unmoved), its Z as XYZ2 takes it
Place vertex_place(uint at, out uint z) {
    ivec4 P = mesh.q[at * 4u];
    Place p = vu_place(vec4(float(P.x), float(P.y), float(P.z), 1.0));
    z = uint(p.z >> 4);
    return p;
}

void main() {
    uint at = uint(-a_vertex) - 1u; // (-1 less it: nd_corner_vertex)
    uint corner = a_corner.r | a_corner.g << 8 | a_corner.b << 16;
    uint kick = at + (corner & 0x3Fu);
    d = draws[corner >> 7];
    ivec4 P = mesh.q[at * 4u];
    uint z;
    Place p = vertex_place(at, z);
    // the moved one: y + sine * 5 (the game's floats: the product rounded, then the sum)
    precise float lift_n = sine(P.x, P.z) * 5.0;
    float lift = nearest() ? lift_n : vu_mul(sine(P.x, P.z), 5.0);
    precise float y_n = float(P.y) + lift;
    float y = nearest() ? y_n : vu_add(float(P.y), lift);
    Place w = vu_place(vec4(float(P.x), y, float(P.z), 1.0));
    int xi = p.x, yi = p.y, wx = w.x, wy = w.y;
#ifdef ND_STANDALONE
    float zf = p.zf;
#endif
    bool gone = false;
    if ((d.info.z & ND_C_MOVING) != 0) {
        Moved m = moved(p, z);
        xi = m.x;
        yi = m.y;
        z = m.z;
#ifdef ND_STANDALONE
        zf = m.zf;
#endif
        Moved mw = moved(w, 0u);
        // (a moved place off the 16 bits keeps the frame's S and T: moved() starts from the GS's 16 bits)
        if (!mw.drop && ((w.x | w.y) & int(0xFFFF0000u)) == 0) {
            wx = mw.x;
            wy = mw.y;
        }
        for (uint i = 0u; i < 3u && !gone; i++) {
            uint k = kick - i;
            if (k == at) {
                gone = m.drop;
            } else {
                uint kz;
                gone = moved(vertex_place(k, kz), kz).drop;
            }
        }
    }
    vec2 xy = vec2(float(int(uint(xi) & 0xFFFFu) - d.info.x), float(int(uint(yi) & 0xFFFFu) - d.info.y)) / 16.0;
    if (gone) {
        xy = vec2(1.0e9); // (far past the target's right edge, as are its two other corners: not drawn)
    }
    uint rgba = uint(d.info.w);

    // as gs.vert
    vec2 ndc = (xy + 0.5) * pc.inv_size * 2.0 - 1.0;
#ifdef ND_STANDALONE
    // the scene's own depth, as sa_000.vert's (nd_000.vert, ND_STANDALONE): unrounded, with the tie's slack
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
    v_color = vec4(float(rgba & 0xFFu), float((rgba >> 8) & 0xFFu), float((rgba >> 16) & 0xFFu), float(rgba >> 24));
    v_stq = vec3(screen_st(wx, wy), 1.0);
    v_fog = d.fog.x;
}
