// The reflection's rippled water on the GPU (ripple_draw.h): Wobble_DrawRippledModel's maths (port/render/ripple.hpp
// says what it is). Before it, the including shader declares `pic` (a picture between's Picture), `mesh` (the arena:
// four qwords a vertex) and the record's struct (ripple_draw.h). The places in VU0's arithmetic (as ripple.cpp's), S and
// T in the game's floats (its threads round toward zero, as the EE's FPU; to nearest with --fpu-round nearest).
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

// m v, as MULAx ACC; MADDAy; MADDAz; MADDw
vec4 vu_transform(mat4 m, vec4 v) {
    vec4 acc = vu_mul4(m[0], vec4(v.x));
    acc = vu_add4(acc, vu_mul4(m[1], vec4(v.y)));
    acc = vu_add4(acc, vu_mul4(m[2], vec4(v.z)));
    return vu_add4(acc, vu_mul4(m[3], vec4(v.w)));
}

// 1 / w rounded toward zero (DIV): the GPU's reciprocal made the nearest by a Newton step, then the largest whose
// product with |w| is not over 1; by 0 the largest float with w's sign
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
};

Place vu_place(vec4 v) {
    vec4 c = vu_transform(d.vertex, v);
    Place p;
    p.q = vu_recip(c.w);
    p.x = ftoi(vu_mul(c.x, p.q) * 16.0);
    p.y = ftoi(vu_mul(c.y, p.q) * 16.0);
    p.z = ftoi(vu_mul(c.z, p.q) * 16.0);
    return p;
}

bool nearest() {
    return (d.info.z & RIPPLE_C_NEAREST) != 0;
}

// n as a game's float
float game_float(int n) {
    uint m = n < 0 ? 0u - uint(n) : uint(n);
    float f = float(m);
    if (!nearest() && f > 16777216.0 && uint(f) > m) {
        f = toward_zero(f); // (past 2^24 the GPU's nearest may be above it)
    }
    return n < 0 ? -f : f;
}

// n / b, y the float nearest 1 / b: the nearest quotient (Markstein: the residual by a fused multiply-add, then one
// step), then toward zero where it went past (its residual's sign)
float game_div(float n, float b, float y) {
    precise float q = n * y;
    precise float r = fma(-q, b, n);
    precise float s = fma(r, y, q);
    precise float e = fma(-s, b, n);
    return !nearest() && e != 0.0 && (e < 0.0) != (s < 0.0) ? toward_zero(s) : s;
}

// S and T from a place's X and Y (12.4): (X - 0x6C00) / 10240, (Y - 0x7200) / 7168
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

// The place the vertex's S and T are made from: (x, y + sine * 5, z) (the game's floats: the product rounded, then the
// sum)
Place lifted_place(ivec4 P) {
    precise float lift_n = sine(P.x, P.z) * 5.0;
    float lift = nearest() ? lift_n : vu_mul(sine(P.x, P.z), 5.0);
    precise float y_n = float(P.y) + lift;
    float y = nearest() ? y_n : vu_add(float(P.y), lift);
    return vu_place(vec4(float(P.x), y, float(P.z), 1.0));
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

// (X - cx) / (q sx): a place in the view across, from the screen's
float view_place(float n, float q, float s) {
    precise float dh = q * s;
    precise float dl = fma(q, s, -dh);
    return div_nearest(n, dh, dl);
}

// A picture between's place (vu1_000_maths.h's moved(), by the camera only): X and Y 12.4 (the 16 bits), Z
struct Moved {
    int x, y;
    uint z;
    bool drop;
    float zfrac; // the moved Z less z: about -0.5 to 0.5, its fraction (pcr_common.h's exact Z rounds a pixel's Z once)
};

Moved moved(Place p, uint z) {
    Moved r;
    r.x = p.x & 0xFFFF;
    r.y = p.y & 0xFFFF;
    r.z = z;
    r.drop = false;
    r.zfrac = 0.0;
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
    float dz = (float(r.z) - d.screen[2].z) * dm.z / mz;
    float nz = float(r.z) - dz;
    if (!(abs(nx - 2048.0) <= 2047.0 && abs(ny - 2048.0) <= 2047.0 && nz >= 0.0)) {
        r.drop = true;
        return r;
    }
    r.x = int(floor(nx * 16.0 + 0.5));
    r.y = int(floor(ny * 16.0 + 0.5));
    uint z_was = r.z;
    r.z = uint(min(floor(nz + 0.5), 4294967040.0));
    r.zfrac = float(int(z_was - r.z)) - dz; // (from the move alone: small, exact to its float)
    return r;
}

// A vertex's place in the frame (the unmoved), its Z as XYZ2 takes it
Place vertex_place(uint at, out uint z) {
    ivec4 P = mesh.q[at * 4u];
    Place p = vu_place(vec4(float(P.x), float(P.y), float(P.z), 1.0));
    z = uint(p.z >> 4);
    return p;
}
