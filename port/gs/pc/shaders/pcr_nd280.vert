#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's native draws of program 0x280, the skinned models (port/gs/pc/pcr_native.cpp): a pass of a batch,
// its triangles those VU1 kicks (the CPU's plan: port/render/vu1_record.cpp), each corner what VU1's program would have
// kicked (port/render/shaders/vu1_280_maths.h), given to pcr_prim.frag as a GS primitive's are (pcr_nd000.vert's
// sibling). The first pass is lit and textured from ST; the second (the sphere-mapped shine) has ST from the sphere
// matrices and the normal, Q 1, white with vertex 1's alpha.
#define PCR_ND
#include "pcr_common.h"
#include "pcr_exact_z.h"

// the pass's triangles: x, y, z the arena's slots of its corners (two a vertex), w its kick's
layout(set = 0, binding = 8, std430) readonly buffer Tris {
    uvec4 tris[];
};

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;
// (whole_triangle.h's whole triangles) the divisor of v_color: Q (1 / w) for a colour given times Q, so that
// pcr_prim.frag's colour is the corners' in 3D, as the game's near cut gives its pieces; 0 for a colour as is
layout(location = 12) noperspective out float v_color_q;
layout(location = 3) flat out uvec2 v_rect; // (the 2D mode's: none for a native draw)
out float gl_ClipDistance[1]; // the mirror image's cut at the water (pcr_nd000.vert's mirror_cut)
#ifdef SCN_FUSE_280
// (the scene renderer's first pass with the sphere pass in it: pcr_prim.h's SCN_FUSE_280) the sphere pass's S, T and Q
// and its alpha, as its own pass would hand them down in v_stq and v_color.a
layout(location = 5) noperspective out vec4 v_sphere;
#endif

// a picture between's (port/gs/pc/pcr_between.h; zeros outside one)
layout(set = 0, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;
} pic;
// the meshes (the arena): eight qwords a vertex (render.cpp, take_batch_280)
layout(set = 0, binding = 6, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "vu1_280_draw.h"
// the draws' records (port/render/vu1_record.hpp, Vu1Record280)
layout(set = 0, binding = 7, std430) readonly buffer Draws {
    Draw draws[];
};
#include "vu1_280_maths.h"

#ifdef SCN_PLACED
// (the scene renderer's: each vertex worked out once a draw, scn_place_280.comp; the instance its job's number)
#include "scn_placed.h"
layout(set = 1, binding = 11, std430) readonly buffer Jobs {
    uvec4 jobs[];
};
layout(set = 1, binding = 12, std430) readonly buffer PlacedIn {
    Placed placed[];
};
uvec4 job;
Placed at_slot(uint slot) {
    return placed[job.z + (slot - job.y) / 2u];
}
Whole whole_of(Placed p) {
    Whole w;
    w.h = p.whole.xyz;
    w.w = p.whole.w;
    return w;
}

#ifdef PCR_EXACT_Z
// (exact Z) Whether a triangle's plane is worked out (pcr_exact_z.h): where its corners' Z span this much or more.
// Below it the interpolated Z (v_zrel, the corners' Z less a shared whole one) is within about 2^-11 of a Z of the
// plane's (its float's error, the span 2^-23 a few times), the same whole Z and nearly always the same fine part; above
// it, near the eye, the float's error passes half a Z (map 063's low wall, cut by its moss: 2026-11-21)
#define SCN_PLANE_SPAN 4096u
bool scn_plane_wanted(GsCorner a, GsCorner b, GsCorner c) {
    return max(max(a.z, b.z), c.z) - min(min(a.z, b.z), c.z) >= SCN_PLANE_SPAN;
}

// (exact Z) A placed corner as the GS gets it (pcr_planes_*.comp's corner): the frame's whole Z with its own fraction,
// in a picture between moved; at the far plane (the modern sky) Z 0; origin the draw's offset
GsCorner scn_corner(Placed p, bool moving, bool far, ivec2 origin) {
    ivec2 xy = moving ? p.xy : ivec2(int(uint(p.xy.x) & 0xFFFFu), int(uint(p.xy.y) & 0xFFFFu));
    GsCorner c = GsCorner(xy + 16 * ivec2(pc.place) - origin, moving ? p.z : p.zframe, (moving ? p.zfrac : 0.0) + p.zown);
    if (far) {
        c.z = 0u;
        c.zfrac = 0.0;
    }
    return c;
}
#endif

void main() {
    v_color_q = 0.0;
    pcr_plane_none();
    uint triangle, instance;
    pcr_triangle(triangle, instance);
    job = jobs[instance];
    instance = job.x;
#ifdef PCR_RECORDS
    pcr_pass_record(); // (the fragment stage's fields: pcr_common.h)
#endif
    uvec4 tri = tris[triangle];
    uint which = uint(gl_VertexIndex) % 3u;
    uint slot = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w & 0x7FFFFFFFu;
    bool left_out = (tri.w & 0x80000000u) != 0u;
    uint record = instance & 0x0FFFFFFFu;
    bool sphere = ((instance >> 29) & 1u) != 0u, gouraud = ((instance >> 30) & 1u) != 0u;
    ivec4 off = draws[record].offset;
    ivec2 offset = ((instance >> 28) & 1u) != 0u ? off.zw : off.xy;
    bool moving = (draws[record].info.y & ND_C_MOVING) != 0;
    Placed c = at_slot(slot);
    // (the colour: the kick's for a flat triangle's first pass, as pcr_nd280.vert's lit_colour(gouraud ? slot : kick))
    uvec4 rgba = scn_unpack_rgba(!sphere && !gouraud ? at_slot(kick).rgba : c.rgba);
    gl_ClipDistance[0] = c.cut;
    uint z = c.z;
    float zfrac = c.zfrac;
#ifdef PCR_EXACT_Z
    zfrac += c.zown; // (the corner's own fraction, as scn_corner gives the plane)
#endif
    uint zref = which == 0u ? c.zframe : at_slot(tri.x).zframe;
    float f = c.f;
    bool gone = left_out;
    bool whole = false;
    Whole w0, w1, w2;
    if (moving) {
        w0 = whole_of(at_slot(tri.x));
        w1 = whole_of(at_slot(tri.y));
        w2 = whole_of(at_slot(tri.z));
        whole = left_out || whole_behind(w0, w1, w2);
    }
    if (whole) {
        Whole cw = which == 0u ? w0 : which == 1u ? w1 : w2;
        v_rect = uvec2(PCR_NO_RECT);
        if (whole_outside(w0, w1, w2)) {
            whole_not_drawn();
            v_color = vec4(0.0);
            v_stq = vec3(0.0);
            v_fog = 0.0;
            return;
        }
        whole_position(cw, w0, w1, w2, offset, false);
        float qn = 1.0 / cw.w;
        if (!sphere) {
            v_stq = c.st.xyz * qn;
            v_color = vec4(rgba) * qn;
            v_color_q = qn;
        } else {
            v_stq = c.st.xyz;
            v_color = vec4(rgba);
        }
        vec4 fog = draws[record].fog;
        v_fog = fog.z + fog.w * qn;
        return;
    }
    ivec2 xyg = c.xy;
    if (moving) {
        int kk = mesh.q[kick * 4u + 4u].x; // (the kick's number in the batch)
        for (int i = 0; i < 3 && i <= kk && !gone; i++) {
            gone = (at_slot(kick - 2u * uint(i)).flags & SCN_PLACED_DROP) != 0u;
        }
    }
    ivec2 g = moving ? xyg : ivec2(int(uint(xyg.x) & 0xFFFFu), int(uint(xyg.y) & 0xFFFFu));
    vec2 xy = vec2(float(g.x - offset.x), float(g.y - offset.y)) / 16.0;
    xy += vec2(pc.place);
    if (gone) {
        xy = vec2(1.0e9);
    }
#ifdef PCR_EXACT_Z
    // (the triangle's plane worked out by its provoking vertex, from the placed corners: pcr_exact_z.h)
    if ((pc.flags & PCR_F_EXACT_Z) != 0u && !gone && uint(gl_VertexIndex) % 3u == 0u) {
        GsCorner c0 = scn_corner(at_slot(tri.x), moving, false, offset), c1 = scn_corner(at_slot(tri.y), moving, false, offset),
                 c2 = scn_corner(at_slot(tri.z), moving, false, offset);
        if (scn_plane_wanted(c0, c1, c2)) {
            pcr_plane_words(c0, c1, c2, pc.src_scale, v_plane0, v_plane1);
        }
    }
#endif
    vec3 stq = sphere ? c.st.xyz : vec3(vu_mul(c.st.x, c.q), vu_mul(c.st.y, c.q), vu_mul(c.st.z, c.q)) * f;
    vec2 ndc = gs_to_clip(xy);
    gl_Position = vec4(ndc, pcr_depth(z, zfrac, zref), 1.0);
    gl_ClipDistance[0] = c.cut * c.q * f;
    v_color = vec4(rgba);
    v_stq = stq;
    v_fog = c.st.w;
    v_rect = uvec2(PCR_NO_RECT);
}
#else
void main() {
    v_color_q = 0.0;
    pcr_plane_none();
    uint triangle, instance;
    pcr_triangle(triangle, instance);
#ifdef PCR_RECORDS
    pcr_pass_record(); // (the fragment stage's fields: pcr_common.h)
#endif
    uvec4 tri = tris[triangle];
#ifdef SCN_ND_EMPTY
    // (the scene renderer's developers' bound, KF4_SCN_ND_EMPTY: the draws with no maths, every triangle off the target)
    gl_Position = vec4(2.0, 2.0, 0.5, 1.0 + float(tri.x & 1u) * 1.0e-7);
    return;
#endif
    uint which = uint(gl_VertexIndex) % 3u;
    uint slot = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w & 0x7FFFFFFFu;
    bool left_out = (tri.w & 0x80000000u) != 0u; // (VU1 left it out of the frame: drawn in pictures between only)
    // the instance: the record (bits 0-27), the context whose XYOFFSET (28), the pass (29), IIP (30)
    d = draws[instance & 0x0FFFFFFFu];
    bool sphere = ((instance >> 29) & 1u) != 0u, gouraud = ((instance >> 30) & 1u) != 0u;
    ivec2 offset = ((instance >> 28) & 1u) != 0u ? d.offset.zw : d.offset.xy;
    ivec4 X = mesh.q[slot * 4u + 4u];
    int s = vertex_set(slot);
    Place pl = vu_place(slot);
    float q = pl.q;
    // the mirror image's cut at the water (pcr_nd000.vert's): drawn whole, as it is; on the screen, by q
    float cut = d.clip == vec4(0.0) ? 0.0 : dot(d.clip, d.vertex[s] * qf(slot, 0u));
    gl_ClipDistance[0] = cut;
    uint z = uint(pl.z >> 4) & 0xFFFFFFu;
    float zfrac = 0.0;
    // (exact Z, pcr_common.h: a whole Z the triangle's corners share, its first corner's in the frame)
    uint zref = which == 0u ? z : uint(vu_place(tri.x).z >> 4) & 0xFFFFFFu;
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, q)), 255.0), 0.0);
    uint w16 = (uint(ftoi(fogf * 16.0)) | uint(X.z)) & 0xFFFFu;
    // a picture between's (ND_C_MOVING): moved, S, T and Q by the change of depth; dropped with its kick or one of the
    // two before it in the batch, the same for all three corners
    int xi = pl.x, yi = pl.y;
    float f = 1.0;
    bool gone = left_out;
    bool whole = false;
    Whole w0, w1, w2;
    if ((d.info.y & ND_C_MOVING) != 0) {
        // left out of the frame, or with a corner behind the picture's eye: drawn whole (whole_triangle.h)
        w0 = whole_place(tri.x);
        w1 = whole_place(tri.y);
        w2 = whole_place(tri.z);
        whole = left_out || whole_behind(w0, w1, w2);
    }
    if (whole) {
        Whole c = which == 0u ? w0 : which == 1u ? w1 : w2;
        v_rect = uvec2(PCR_NO_RECT);
        if (whole_outside(w0, w1, w2)) {
            whole_not_drawn();
            v_color = vec4(0.0);
            v_stq = vec3(0.0);
            v_fog = 0.0;
#ifdef SCN_FUSE_280
            v_sphere = vec4(0.0);
#endif
            return;
        }
        whole_position(c, w0, w1, w2, offset, false);
        float qn = 1.0 / c.w;
#ifdef SCN_FUSE_280
        {
            vec4 acc = vu_transform3(d.sphere[s], qf(slot, 1u).xyz);
            uint one = slot - 2u * uint(X.x) + 2u;
            float a1 = min(vu_add(vu_mul(qf(one, 3u).w, d.scale.w), d.bias.w), 255.0);
            v_sphere = vec4(vu_mul(vu_add(acc.x, 1.0), 0.5), vu_mul(vu_add(acc.y, 1.0), 0.5), 1.0,
                            float(uint(ftoi(a1)) & 0xFFu));
        }
#endif
        if (!sphere) {
            vec4 st = qf(slot, 2u);
            v_stq = st.xyz * qn;
            // (the colour times Q, as in 3D: pcr_nd000.vert's draw_whole)
            v_color = vec4(lit_colour(gouraud ? slot : kick)) * qn;
            v_color_q = qn;
        } else {
            // (as below: the sphere's ST from the normal, Q 1)
            vec4 acc = vu_transform3(d.sphere[s], qf(slot, 1u).xyz);
            v_stq = vec3(vu_mul(vu_add(acc.x, 1.0), 0.5), vu_mul(vu_add(acc.y, 1.0), 0.5), 1.0);
            uint one = slot - 2u * uint(X.x) + 2u;
            float a1 = min(vu_add(vu_mul(qf(one, 3u).w, d.scale.w), d.bias.w), 255.0);
            v_color = vec4(255.0, 255.0, 255.0, float(uint(ftoi(a1)) & 0xFFu));
        }
        v_fog = d.fog.z + d.fog.w * qn; // (the fragment stage clamps it to 0..255)
        return;
    }
    if ((d.info.y & ND_C_MOVING) != 0) {
        Moved m = moved(slot, s, pl);
        xi = m.x;
        yi = m.y;
        z = m.z;
        zfrac = m.zfrac;
        f = m.f;
        int kk = mesh.q[kick * 4u + 4u].x; // (the kick's number in the batch)
        for (int i = 0; i < 3 && i <= kk && !gone; i++) {
            uint k = kick - 2u * uint(i);
            gone = k == slot ? m.drop : moved(k, vertex_set(k), vu_place(k)).drop;
        }
    }
    // (in a picture between not wrapped to the GS's 16 bits: a place past the guard band is kept, past_band.h)
    ivec2 g = (d.info.y & ND_C_MOVING) != 0 ? ivec2(xi, yi) : ivec2(int(uint(xi) & 0xFFFFu), int(uint(yi) & 0xFFFFu));
    vec2 xy = vec2(float(g.x - offset.x), float(g.y - offset.y)) / 16.0;
    xy += vec2(pc.place);
    if (gone) {
        xy = vec2(1.0e9); // (far past the target's right edge, as are its two other corners: not drawn)
    }
    if ((pc.flags & PCR_F_EXACT_Z) != 0u && !gone) {
        pcr_plane_of_draw(); // (exact Z: the plane pass's, pcr_exact_z.h)
    }
    vec3 stq;
    uvec4 rgba;
#ifdef SCN_FUSE_280
    {
        vec4 acc = vu_transform3(d.sphere[s], qf(slot, 1u).xyz);
        uint one = slot - 2u * uint(X.x) + 2u;
        float a1 = min(vu_add(vu_mul(qf(one, 3u).w, d.scale.w), d.bias.w), 255.0);
        v_sphere = vec4(vu_mul(vu_add(acc.x, 1.0), 0.5), vu_mul(vu_add(acc.y, 1.0), 0.5), 1.0,
                        float(uint(ftoi(a1)) & 0xFFu));
    }
#endif
    if (!sphere) {
        vec4 st = qf(slot, 2u);
        stq = vec3(vu_mul(st.x, q), vu_mul(st.y, q), vu_mul(st.z, q)) * f;
        rgba = lit_colour(gouraud ? slot : kick);
    } else {
        // ((Ms * normal).xy + 1 * 1) * 0.5; Q 1; white, with vertex 1's alpha (its u's: lit.w is 1)
        vec4 acc = vu_transform3(d.sphere[s], qf(slot, 1u).xyz);
        stq = vec3(vu_mul(vu_add(acc.x, 1.0), 0.5), vu_mul(vu_add(acc.y, 1.0), 0.5), 1.0);
        uint one = slot - 2u * uint(X.x) + 2u;
        float a1 = min(vu_add(vu_mul(qf(one, 3u).w, d.scale.w), d.bias.w), 255.0);
        rgba = uvec4(255u, 255u, 255u, uint(ftoi(a1)) & 0xFFu);
    }
    // as pcr_prim.vert
    vec2 ndc = gs_to_clip(xy);
    gl_Position = vec4(ndc, pcr_depth(z, zfrac, zref), 1.0);
    gl_ClipDistance[0] = cut * q * f;
    v_color = vec4(rgba);
    v_stq = stq;
    v_fog = float((w16 >> 4) & 0xFFu);
    v_rect = uvec2(PCR_NO_RECT);
}
#endif
