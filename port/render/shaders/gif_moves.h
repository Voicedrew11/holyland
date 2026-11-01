// --frame-rate's pictures between on the GPU, shared by both renderers: a vertex kick of the frame's packets at the
// picture's time, as port/platform/frame_interp.c moves it on the CPU (move_vertex: with the camera or its model, or
// dropped; move_flat_vertex: a 2D item t of the way from the frame before's). The native GS's
// shaders/gs_replay.comp, and the PC renderer's shaders/pcr_prim.vert (port/gs/pc/pcr_between.h).
// Before it, the including shader declares `pic` (gs_replay.h's Picture), `kicks` (the frame's kicks as the GS queued
// them, gsn.cpp's InterpKick: 16 words each) and `moves` (how each kick moves, gsn.h's GsnInterpMove: 16 words each).
// gsn.h's GSN_IM_*
#define IM_3D 1u
#define IM_FLAT 2u
#define IM_VIEW 0x4u
#define IM_OBJ 0x8u
#define IM_WORLD 0x10u
#define IM_EYE 0x20u
#define IM_REFLECT 0x40u
#define IM_SCALE_ST 0x80u
#define IM_XYZF 0x100u
#define IM_SCREEN_ST 0x200u
#define IM_XY 0x4u
#define IM_RGBA 0x8u
#define IM_ST 0x10u
#define IM_UV 0x20u
#define IM_PREV 0x40u
#define IM_PAIRED 0x80u

uint K(uint k, uint i) {
    return kicks[k * 16u + i];
}

uint M(uint k, uint i) {
    return moves[k * 16u + i];
}

float MF(uint k, uint i) {
    return uintBitsToFloat(moves[k * 16u + i]);
}

bool run_bit(uint r) {
    return r < pic.counts.y && (pic.run_bits[r >> 5] & (1u << (r & 31u))) != 0u;
}

struct Moved {
    int x, y;  // 12.4
    uint z;
    bool proc; // moved as a vertex of the world or a model (it may be dropped, and its ADC then covers the next two)
    bool drop; // behind the camera, past the guard band or the far plane: its triangles are not drawn
    float f;   // its depth / the new one: its own S and T scaled by it
    float qn;  // its new Q, 1 / w
};

Moved moved(uint k) {
    Moved r;
    uint xy = K(k, 0u);
    r.x = int(xy & 0xFFFFu);
    r.y = int(xy >> 16);
    r.z = K(k, 1u);
    r.proc = false;
    r.drop = false;
    r.f = 1.0;
    r.qn = 0.0;
    uint w = M(k, 0u);
    if ((w & 3u) == IM_FLAT) {
        if ((w & IM_XY) != 0u) {
            // t of the way from the frame before's place
            uint a = M(k, 3u), b = M(k, 4u);
            int bx = int(b & 0xFFFFu), by = int(b >> 16);
            r.x = bx + int(floor(float(int(a & 0xFFFFu) - bx) * pic.zt.w + 0.5));
            r.y = by + int(floor(float(int(a >> 16) - by) * pic.zt.w + 0.5));
        }
        return r;
    }
    if ((w & 3u) != IM_3D || (w & IM_VIEW) == 0u) {
        return r;
    }
    bool obj = (w & IM_OBJ) != 0u && run_bit(M(k, 1u));
    if (!obj && (w & IM_WORLD) == 0u) {
        return r; // (before the eye or the arm, its model not moving: as it is)
    }
    r.proc = true;
    // Its place in the picture's view, m = v + dm, from v its place in the frame's (whose X, Y and Z the
    // kick has exactly): so that the floats' rounding is that of the move, and no move is no change
    vec3 v = vec3(MF(k, 4u), MF(k, 5u), MF(k, 6u));
    vec3 dm;
    vec3 e = vec3(MF(k, 8u), MF(k, 9u), MF(k, 10u)) + vec3(MF(k, 11u), MF(k, 12u), MF(k, 13u)) * pic.zt.z;
    if (obj && (w & IM_EYE) != 0u) {
        dm = e; // (before the eye: t of the way between its two places)
    } else {
        // with the camera: (R - I) v + T
        dm = vec3(dot(pic.move[0].xyz, v) + pic.move[0].w, dot(pic.move[1].xyz, v) + pic.move[1].w,
                  dot(pic.move[2].xyz, v) + pic.move[2].w);
        if (obj) {
            // with its model: t of the way between its two places, seen from the camera between
            dm += vec3(dot(pic.mid[0].xyz, e), dot(pic.mid[1].xyz, e), dot(pic.mid[2].xyz, e));
        }
    }
    float mz = v.z + dm.z;
    if (!(mz > 0.0)) {
        r.drop = true;
        return r;
    }
    bool reflect = (w & IM_REFLECT) != 0u;
    vec4 s = pic.screen[reflect ? 1 : 0];
    float depth = MF(k, 7u);
    // X = sx m.x / m.z + cx, as the kick's X and how far it moved: sx (m.x / m.z - v.x / v.z)
    float den = mz * v.z;
    float nx = float(xy & 0xFFFFu) / 16.0 + s.x * (dm.x * v.z - v.x * dm.z) / den;
    float ny = float(xy >> 16) / 16.0 + s.y * (dm.y * v.z - v.y * dm.z) / den;
    // Z = cz + (z - cz) depth / m.z (k / w, k the vertex's own), as the kick's Z less (z - cz) dm.z / m.z
    float nz = float(r.z) - MF(k, 14u) * dm.z / mz;
    if (!(abs(nx - 2048.0) <= 2047.0 && abs(ny - 2048.0) <= 2047.0 && nz >= 0.0)) {
        r.drop = true;
        return r;
    }
    r.x = int(floor(nx * 16.0 + 0.5));
    r.y = int(floor(ny * 16.0 + 0.5));
    nz = floor(nz + 0.5);
    r.z = (w & IM_XYZF) != 0u ? uint(min(nz, 16777215.0)) : nz >= 4294967040.0 ? 0xFFFFFFFFu : uint(nz);
    r.f = depth / mz;
    r.qn = 1.0 / mz;
    return r;
}


#ifdef GIF_KICK_NOW
// (the PC renderer's pcr_prim.vert, which makes each corner's kicks at its vertex: gs_replay.comp's main, which writes
// the same into its Resolved buffer, does it in place, so that the native GS's SPIR-V stays as it was)
// A kick at the picture's time: its place (12.4) and Z, the registers queued with it as the picture has them (RGBAQ,
// ST, UV written in a stretch frame_interp.c changes: their owners), and whether it was moved as a vertex with a place
// (it may be dropped: its triangles are not drawn then)
struct KickNow {
    int x, y;
    uint z, rgba;
    float s, t, q;
    uint uv;
    bool proc, drop;
};

KickNow kick_now(uint k) {
    Moved m = moved(k);
    if (m.drop) {
        // (as frame_interp.c leaves a vertex it drops: where it was, its triangles not drawn)
        uint xy = K(k, 0u);
        m.x = int(xy & 0xFFFFu);
        m.y = int(xy >> 16);
        m.z = K(k, 1u);
    }
    float u = pic.zt.w;

    // RGBAQ's colour: written in a paired 2D item's stretch, t of the way from the frame before's; the
    // previous frame blended in, A^t
    uint rgba = K(k, 2u);
    uint o = K(k, 10u);
    if (o < pic.counts.x && (M(o, 0u) & 3u) == IM_FLAT) {
        uint w = M(o, 0u);
        if ((w & IM_PREV) != 0u) {
            float va = float(M(o, 1u) >> 24), vb = float(M(o, 2u) >> 24);
            float base = (w & IM_PAIRED) != 0u ? vb + (va - vb) * u : vb;
            float a = base / 128.0;
            a = a <= 0.0 ? 0.0 : a >= 1.0 ? 1.0 : pow(a, pic.zt.z);
            rgba = (rgba & 0x00FFFFFFu) | ((uint(floor(a * 128.0 + 0.5)) & 0xFFu) << 24);
        } else if ((w & IM_RGBA) != 0u) {
            uint a = M(o, 1u), b = M(o, 2u);
            rgba = 0u;
            for (uint i = 0u; i < 32u; i += 8u) {
                int ac = int((a >> i) & 0xFFu), bc = int((b >> i) & 0xFFu);
                rgba |= (uint(bc + int(floor(float(ac - bc) * u + 0.5))) & 0xFFu) << i;
            }
        }
    }

    // ST: a vertex's own scaled for its new depth; a paired 2D item's t of the way from the frame before's
    float s = uintBitsToFloat(K(k, 3u)), t = uintBitsToFloat(K(k, 4u)), q = uintBitsToFloat(K(k, 5u));
    o = K(k, 9u);
    if (o < pic.counts.x) {
        uint w = M(o, 0u);
        if ((w & 3u) == IM_3D && (w & IM_SCALE_ST) != 0u) {
            Moved mo = o == k ? m : moved(o);
            if (mo.proc && !mo.drop) {
                s *= mo.f;
                t *= mo.f;
            }
        } else if ((w & 3u) == IM_3D && (w & IM_SCREEN_ST) != 0u) {
            // made from the screen: with the place (frame_interp.c, Run's screen_st)
            Moved mo = o == k ? m : moved(o);
            if (mo.proc && !mo.drop) {
                // S, T + H(x', y') - H(x, y), H projective (h0..h5 in 8..13, h6 in 15, h7 in 1)
                uint xy0 = K(o, 0u);
                vec2 p0 = vec2(float(xy0 & 0xFFFFu), float(xy0 >> 16)) / 16.0;
                vec2 p1 = vec2(float(mo.x), float(mo.y)) / 16.0;
                vec3 hs = vec3(MF(o, 8u), MF(o, 9u), MF(o, 10u)), ht = vec3(MF(o, 11u), MF(o, 12u), MF(o, 13u));
                vec3 hd = vec3(MF(o, 15u), MF(o, 1u), 1.0);
                float d0 = dot(hd, vec3(p0, 1.0)), d1 = dot(hd, vec3(p1, 1.0));
                s += dot(hs, vec3(p1, 1.0)) / d1 - dot(hs, vec3(p0, 1.0)) / d0;
                t += dot(ht, vec3(p1, 1.0)) / d1 - dot(ht, vec3(p0, 1.0)) / d0;
            }
        } else if ((w & 3u) == IM_FLAT && (w & IM_ST) != 0u) {
            s = MF(o, 5u) + MF(o, 7u) * u;
            t = MF(o, 6u) + MF(o, 8u) * u;
        }
    }
    // Q: 1 / w, when the RGBAQ queued with it took the Q of a vertex's own ST
    o = K(k, 11u);
    if (o < pic.counts.x && (M(o, 0u) & 3u) == IM_3D && (M(o, 0u) & IM_SCALE_ST) != 0u) {
        Moved mo = o == k ? m : moved(o);
        if (mo.proc && !mo.drop) {
            q = uintBitsToFloat(K(o, 5u)) * mo.f; // (1 / w: its Q by the depth's change, exact when it does not move)
        }
    }
    // UV: a paired 2D item's t of the way from the frame before's
    uint uv = K(k, 6u);
    o = K(k, 12u);
    if (o < pic.counts.x && (M(o, 0u) & 3u) == IM_FLAT && (M(o, 0u) & IM_UV) != 0u) {
        float us = MF(o, 5u) + MF(o, 7u) * u, vt = MF(o, 6u) + MF(o, 8u) * u;
        uv = (uint(int(floor(us * 16.0 + 0.5))) & 0x3FFFu) | (uint(int(floor(vt * 16.0 + 0.5))) & 0x3FFFu) << 16;
    }

    KickNow r;
    r.x = m.x;
    r.y = m.y;
    r.z = m.z;
    r.rgba = rgba;
    r.s = s;
    r.t = t;
    r.q = q;
    r.uv = uv;
    r.proc = m.proc;
    r.drop = m.drop;
    return r;
}
#endif
