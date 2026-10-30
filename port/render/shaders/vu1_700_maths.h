// VU1's program 0x700 on the GPU, shared (vu1_700_draw.h): its arithmetic and maths. Before it, the including shader
// declares `pic` (a picture between's Picture), `mesh` (the arena: four qwords a vertex) and the record's struct.
// nd_700.vert's top says what each step is (port/platform/vu1_hle_700.c is the specification).
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

// The vertex's colour as VU1 leaves it in RGBAQ: the lights, by the model's colour (vu1_hle_700.c: lit by
// vf28, then MINI 255 and FTOI0; no point lights)
uvec4 lit_colour(uint at) {
    ivec4 N = mesh.q[at * 4u + 1u];
    vec3 n = vec3(N.xyz) / 4096.0; // (ITOF12: exact)
    // the light dots: MULAx ACC, l0, n.x; MADDAy; MADDz; floored at 0
    vec4 acc = vu_mul4(d.light_dir[0], vec4(n.x));
    acc = vu_add4(acc, vu_mul4(d.light_dir[1], vec4(n.y)));
    vec4 dots = max(vu_add4(acc, vu_mul4(d.light_dir[2], vec4(n.z))), vec4(0.0));
    // the colours by the dots, the ambient added; w 0 + 1
    acc = vu_mul4(d.light_col[0], vec4(dots.x));
    acc = vu_add4(acc, vu_mul4(d.light_col[1], vec4(dots.y)));
    vec4 lit = vu_add4(acc, vu_mul4(d.light_col[2], vec4(dots.z)));
    lit = vec4(vu_add(lit.x, d.light_col[3].x), vu_add(lit.y, d.light_col[3].y), vu_add(lit.z, d.light_col[3].z), 1.0);
    ivec4 c = ivec4(min(vu_mul4(lit, d.point_col[0]), vec4(255.0)));
    return uvec4(c) & 0xFFu;
}

// A vertex's place as VU1 leaves it: FTOI4 of X, Y and Z (X and Y the GS's 12.4), and q = 1 / w
struct Place {
    int x, y, z;
    float q;
#ifdef ND_STANDALONE
    float zf; // (sa_700.vert) Z before FTOI4: what the scene's depth is made of, unrounded
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
    float zf; // (sa_700.vert) Z unrounded
#endif
};

Moved moved(uint at, Place p) {
    Moved r;
    // (the position's own w: 16 in the game's effects; c.w is that times the depth)
    float pw = float(mesh.q[at * 4u].w);
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
    float qt = p.q * pw; // (1 / the depth: exact for a w that is a power of two)
    float depth = div_nearest(1.0, qt, 0.0);
    if (!(depth > 0.0) || isinf(depth) || !(pw > 0.0)) {
        return r; // (as it is)
    }
    vec3 v = vec3(view_place(float(r.x) / 16.0 - d.screen[2].x, qt, d.screen[0].x),
                  view_place(float(r.y) / 16.0 - d.screen[2].y, qt, d.screen[1].y), depth);
    vec3 dm = vec3(dot(pic.move[0].xyz, v) + pic.move[0].w, dot(pic.move[1].xyz, v) + pic.move[1].w,
                   dot(pic.move[2].xyz, v) + pic.move[2].w);
    if ((d.info.w & ND_C_OBJ) != 0) {
        vec3 e = -d.obj.xyz + d.obj.xyz * pic.zt.z;
        dm += vec3(dot(pic.mid[0].xyz, e), dot(pic.mid[1].xyz, e), dot(pic.mid[2].xyz, e));
    }
    if ((d.info.w & ND_C_MODEL) != 0) {
        // (the models': by the part's own matrices, nd_between.h; in the eye's space, the arm's, alone)
        vec3 e = between_move(d.bt, vec4(mesh.q[at * 4u]), pic.zt.w) / pw; // (the position's w: the move of p / w)
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
