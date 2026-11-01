// VU1's program 0x280 on the GPU, shared (vu1_280_draw.h): its arithmetic and maths. Before it, the including shader
// declares `pic` (a picture between's Picture), `mesh` (the arena: eight qwords a vertex) and the record's struct.
// nd_280.vert's top says what each step is (port/platform/vu1_hle_280.c is the specification).
Draw d;

// ---- VU1's arithmetic (nd_000.vert's helpers) ----
// FTOI: toward zero; from 2^31 on, the largest integer with the float's sign (VU1's saturation: a vertex
// far past the guard band, never drawn, then has VU1's X, Y and Z too)
int ftoi(float v) {
    return v >= 2147483648.0 ? 0x7FFFFFFF : v <= -2147483648.0 ? int(0x80000000u) : int(v);
}

float toward_zero(float r) {
    return r == 0.0 ? r : uintBitsToFloat(floatBitsToUint(r) - 1u);
}

float vu_mul(float a, float b) {
    precise float r = a * b;
    precise float e = fma(a, b, -r);
    return e != 0.0 && (e < 0.0) != (r < 0.0) ? toward_zero(r) : r;
}

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

vec4 vu_transform(mat4 m, vec4 v) {
    vec4 acc = vu_mul4(m[0], vec4(v.x));
    acc = vu_add4(acc, vu_mul4(m[1], vec4(v.y)));
    acc = vu_add4(acc, vu_mul4(m[2], vec4(v.z)));
    return vu_add4(acc, vu_mul4(m[3], vec4(v.w)));
}

// m's first three columns by v.xyz (MULAx; MADDAy; MADDz)
vec4 vu_transform3(mat4 m, vec3 v) {
    vec4 acc = vu_mul4(m[0], vec4(v.x));
    acc = vu_add4(acc, vu_mul4(m[1], vec4(v.y)));
    return vu_add4(acc, vu_mul4(m[2], vec4(v.z)));
}

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

// ---- Program 0x280 ----
vec4 qf(uint slot, uint i) {
    return intBitsToFloat(mesh.q[slot * 4u + i]);
}

// ---- Pictures between (nd_000.vert's moved, for a vertex of set s) ----
// n / (dh + dl), the float nearest it
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

// A place as VU1 leaves it: FTOI4 of X, Y and Z, and q
struct Place {
    int x, y, z;
    float q;
#ifdef ND_STANDALONE
    float zf; // (sa_280.vert) Z before FTOI4: what the scene's depth is made of, unrounded
#endif
};

struct Moved {
    int x, y;
    uint z;
    float f;
    bool drop;
#ifdef ND_STANDALONE
    float zf; // (sa_280.vert) Z unrounded
#endif
};

Moved moved(uint slot, int s, Place p) {
    Moved r;
    r.x = p.x & 0xFFFF;
    r.y = p.y & 0xFFFF;
    r.z = uint(p.z >> 4) & 0xFFFFFFu;
    r.f = 1.0;
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
    vec3 dm = vec3(0.0);
    if ((d.info.y & ND_C_EYE) == 0) {
        dm = vec3(dot(pic.move[0].xyz, v) + pic.move[0].w, dot(pic.move[1].xyz, v) + pic.move[1].w,
                  dot(pic.move[2].xyz, v) + pic.move[2].w);
    }
    if ((d.info.y & ND_C_MODEL) != 0) {
        vec3 e = between_move(d.bt[s], qf(slot, 0u), pic.zt.w);
        dm = (d.info.y & ND_C_EYE) != 0 ? e : dm + vec3(dot(pic.mid[0].xyz, e), dot(pic.mid[1].xyz, e), dot(pic.mid[2].xyz, e));
    }
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

// The vertex's set: its ST's w (the low 16 bits, as ILW loads them)
int vertex_set(uint slot) {
    return (uint(mesh.q[slot * 4u + 2u].w) & 0xFFFFu) != 0u ? 1 : 0;
}

// The vertex's place as VU1 leaves it (its set's vertex matrix)
Place vu_place(uint slot) {
    vec4 c = vu_transform(d.vertex[vertex_set(slot)], qf(slot, 0u));
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

// u of the vertex in slot, k its number in the batch: vertex 0's colour by the scale and bias, then the
// RGB carried on k times, with the vertex's own alpha
vec4 colour_u(uint slot, int k) {
    vec4 u = vu_add4(vu_mul4(qf(slot - 2u * uint(k), 3u), d.scale), d.bias);
    for (int j = 1; j <= k; j++) {
        u.xyz = vec3(vu_add(vu_mul(u.x, d.scale.x), d.bias.x), vu_add(vu_mul(u.y, d.scale.y), d.bias.y),
                     vu_add(vu_mul(u.z, d.scale.z), d.bias.z));
    }
    u.w = vu_add(vu_mul(qf(slot, 3u).w, d.scale.w), d.bias.w);
    return u;
}

// The vertex's colour as VU1 leaves it in the strip (the lights; the point lights' passes)
uvec4 lit_colour(uint slot) {
    ivec4 X = mesh.q[slot * 4u + 4u];
    int s = vertex_set(slot);
    vec4 pos = qf(slot, 0u), n = qf(slot, 1u);
    vec4 dots = max(vu_transform3(d.light_dir[s], n.xyz), vec4(0.0));
    vec4 acc = vu_mul4(d.light_col[0], vec4(dots.x));
    acc = vu_add4(acc, vu_mul4(d.light_col[1], vec4(dots.y)));
    vec4 lit = vu_add4(acc, vu_mul4(d.light_col[2], vec4(dots.z)));
    lit = vec4(vu_add(lit.x, d.light_col[3].x), vu_add(lit.y, d.light_col[3].y), vu_add(lit.z, d.light_col[3].z), 1.0);
    vec4 last = min(vu_mul4(lit, colour_u(slot, X.x)), vec4(255.0));
    ivec4 c = ivec4(ftoi(last.x), ftoi(last.y), ftoi(last.z), ftoi(last.w));
    int ps = X.y != 0 ? 4 : 0;
    for (int l = 0; l < d.info.x && l < 4; l++) {
        vec4 lp = d.point_pos[ps + l];
        vec3 dd = vec3(vu_add(lp.x, -pos.x), vu_add(lp.y, -pos.y), vu_add(lp.z, -pos.z));
        float p = vu_add(vu_add(vu_mul(dd.x, dd.x), vu_mul(dd.y, dd.y)), vu_mul(dd.z, dd.z));
        float att = vu_add(1.0, -vu_mul(p, d.point_pos[l].w)); // (the falloff: set A's light's)
        vec3 a = max(vec3(vu_mul(d.point_col[l].x, att), vu_mul(d.point_col[l].y, att), vu_mul(d.point_col[l].z, att)),
                     vec3(0.0));
        vec3 sum = vec3(vu_add(float(c.x), a.x), vu_add(float(c.y), a.y), vu_add(float(c.z), a.z));
        sum = min(sum, vec3(255.0));
        c.xyz = ivec3(ftoi(sum.x), ftoi(sum.y), ftoi(sum.z));
    }
    return uvec4(c) & 0xFFu;
}
