#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's native draws of program 0x000 (port/gs/pc/pcr_native.cpp): a batch's triangles from its mesh,
// each corner what VU1's program would have kicked (port/render/shaders/vu1_000_maths.h: its maths and arithmetic),
// given to pcr_prim.frag as a GS primitive's are: X and Y in the target's GS pixels (XYOFFSET
// off, the surface's place added), the GS's integer Z, RGBA, S T Q and fog. A triangle VU1's clipping would leave out
// (the ADC of its kick or of the two kicks before it) is not drawn. In a picture between (ND_C_MOVING) each corner is
// moved to the picture's time (moved()), S, T and Q by its change of depth, and the triangle dropped with its kick or one
// of the two before it in the batch; one VU1 left out, or with a corner behind the picture's eye, is drawn whole from
// its mesh and cut to the screen by the GPU (a fast turn: draw_whole).
#define PCR_ND
#include "pcr_common.h"
#include "pcr_exact_z.h"

// the batch's triangles: x, y, z the arena's numbers of its corners; w the kick's (bits 0-27) and how many of the two
// kicks before it are the batch's (bits 28-29)
layout(set = 0, binding = 5, std430) readonly buffer Tris {
    uvec4 tris[];
};

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;
// (whole_triangle.h's whole triangles) the divisor of v_color: Q (1 / w) for a colour given times Q, so that
// pcr_prim.frag's colour is the corners' in 3D, as the game's near cut gives its pieces; 0 for a colour as is
layout(location = 12) noperspective out float v_color_q;
layout(location = 3) flat out uvec2 v_rect; // (the 2D mode's: none for a native draw)
out float gl_ClipDistance[1]; // the mirror image's cut at the water (mirror_cut)

// a picture between's (port/gs/pc/pcr_between.h; zeros outside one)
layout(set = 0, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;
} pic;
// the meshes (the arena): four qwords a vertex (position, normal, ST, colour), as VU1 memory had them
layout(set = 0, binding = 3, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "vu1_000_draw.h"
// the draws' records (port/render/vu1_record.hpp)
layout(set = 0, binding = 4, std430) readonly buffer Draws {
    Draw draws[];
};
#include "vu1_000_maths.h"

// The mirror image's cut at the water (port/render/vu1_record.hpp's vu1_mirror_plane): the distance of position v's
// own place from the water, the eye's side more than 0; 0 for a draw without a plane (none cut)
float mirror_cut(vec4 v) {
    return d.clip == vec4(0.0) ? 0.0 : dot(d.clip, d.vertex * v);
}

// (a picture between) A corner c of a triangle drawn whole (whole_triangle.h; w0..w2 its three corners). S T Q and the
// fog by its new Q; the colour times Q too (v_color_q), so it runs across the triangle as in 3D: the frame drew such a
// triangle as the near cut's pieces, whose corners' colours are the triangle's in 3D. (Not the kick's alone for a
// triangle with a corner behind the eye: a wall beside the player turned a flat colour for the pictures it was cut in.)
void draw_whole(Whole c, Whole w0, Whole w1, Whole w2, vec3 st, bool gouraud, uint at, uint kick) {
    v_rect = uvec2(PCR_NO_RECT);
    if (whole_outside(w0, w1, w2)) {
        whole_not_drawn();
        v_color = vec4(0.0);
        v_stq = vec3(0.0);
        v_fog = 0.0;
        return;
    }
    whole_position(c, w0, w1, w2, ivec2(d.info.y, d.info.z), (d.info.w & ND_C_FAR) != 0);
    float q = 1.0 / c.w;
    v_color = vec4(lit_colour(gouraud ? at : kick)) * q;
    v_color_q = q;
    v_stq = st * q;
    v_fog = d.fog.z + d.fog.w * q; // (the fragment stage clamps it to 0..255)
}

void main() {
    v_color_q = 0.0;
    pcr_plane_none();
    uint triangle, instance;
    pcr_triangle(triangle, instance);
#ifdef PCR_RECORDS
    pcr_pass_record(); // (the fragment stage's fields: pcr_common.h)
#endif
    uvec4 tri = tris[triangle];
    uint which = uint(gl_VertexIndex) % 3u;
    uint at = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    uint kick = tri.w & 0x0FFFFFFFu;
    uint befores = (tri.w >> 28) & 3u;
    d = draws[instance & 0x3FFFFFFFu];
    bool gouraud = (instance >> 30) != 0u;
    // VU1's ADC: the kick's clip and the two kicks' before it
    bool gone = false;
    for (uint i = 0u; i <= befores && !gone; i++) {
        gone = vu_clipped(kick - i);
    }
    ivec4 N = mesh.q[at * 4u + 1u], T = mesh.q[at * 4u + 2u];
    Place p = vu_place(at);
    // (a corner drawn whole, in clip space by its w, takes the distance as it is; one on the screen (w 1) it by its
    // q, which is affine on the screen as the cut is)
    float cut = mirror_cut(vec4(mesh.q[at * 4u]));
    gl_ClipDistance[0] = cut;
    float f = 1.0;
    // the fog: ACC.w = 1 * fog.z, then + fog.w * q; clamped to 0..255
    float fogf = max(min(vu_add(d.fog.z, vu_mul(d.fog.w, p.q)), 255.0), 0.0);
    if ((d.info.w & ND_C_WAVE) != 0) {
        // the wave UVs on the GPU: the vertex's own S and T moved by its wave's (ST's w) offsets in 16 bits, as
        // Wobble_ApplyWaveUvs writes them, read back signed as VIF1's UNPACK does
        uint w = uint(T.w) & 15u;
        uint o = d.wave[w >> 2][w & 3u];
        T.x = (int(uint(T.x) + (o & 0xFFFFu)) << 16) >> 16;
        T.y = (int(uint(T.y) + (o >> 16)) << 16) >> 16;
    }
    vec3 st = vec3(T.xyz) / 4096.0; // (ITOF12: exact)
    vec3 stq = vec3(vu_mul(st.x, p.q), vu_mul(st.y, p.q), vu_mul(st.z, p.q));
    int xi = p.x, yi = p.y;
    uint z = gs_z(p);
    float zfrac = 0.0;
    // (exact Z, pcr_common.h: a whole Z the triangle's corners share, its first corner's in the frame)
    uint zref = which == 0u ? z : gs_z(vu_place(tri.x));
    if ((d.info.w & ND_C_MOVING) != 0) {
        // A triangle VU1 left out of the frame (past the guard band, or a corner behind the frame's eye), or one with
        // a corner behind the picture's eye: drawn whole from its mesh, the GPU cutting it to the screen (whole_place).
        // In a fast turn of the mouse the picture is turned far past the frame, and these are what it turns toward:
        // left out, the world there was missing until the game's next frame. The near cut's pieces of such a triangle
        // are not drawn in the pictures (FrameInterp_MarkFrameOnly), so each is drawn once.
        Whole w0 = whole_place(tri.x), w1 = whole_place(tri.y), w2 = whole_place(tri.z);
        if (gone || whole_behind(w0, w1, w2)) {
            draw_whole(which == 0u ? w0 : which == 1u ? w1 : w2, w0, w1, w2, st, gouraud, at, kick);
            return;
        }
        Moved m = moved(at, p);
        xi = m.x;
        yi = m.y;
        z = m.z;
        zfrac = m.zfrac;
        stq *= m.f;
        f = m.f;
        for (uint i = 0u; i <= befores && !gone; i++) {
            uint k = kick - i;
            gone = k == at ? m.drop : moved(k, vu_place(k)).drop;
        }
    }
    z = (d.info.w & ND_C_FAR) != 0 ? 0u : z; // (at the far plane: the modern sky)
    zfrac = (d.info.w & ND_C_FAR) != 0 ? 0.0 : zfrac;
    zref = (d.info.w & ND_C_FAR) != 0 ? 0u : zref;
    uint w16 = (uint(ftoi(fogf * 16.0)) | uint(N.w)) & 0xFFFFu;
    // (in a picture between not wrapped to the GS's 16 bits: a place past the guard band is kept, past_band.h)
    ivec2 g = (d.info.w & ND_C_MOVING) != 0 ? ivec2(xi, yi) : ivec2(int(uint(xi) & 0xFFFFu), int(uint(yi) & 0xFFFFu));
    vec2 xy = vec2(float(g.x - d.info.y), float(g.y - d.info.z)) / 16.0;
    xy += vec2(pc.place);
    if (gone) {
        xy = vec2(1.0e9); // (far past the target's right edge, as are its two other corners: not drawn)
    }
    uvec4 rgba = lit_colour(gouraud ? at : kick);
    // as pcr_prim.vert
    vec2 ndc = gs_to_clip(xy);
    gl_Position = vec4(ndc, pcr_depth(z, zfrac, zref), 1.0);
    gl_ClipDistance[0] = cut * p.q * f;
    v_color = vec4(rgba);
    v_stq = stq;
    v_fog = float((w16 >> 4) & 0xFFu);
    v_rect = uvec2(PCR_NO_RECT);
    if ((pc.flags & PCR_F_EXACT_Z) != 0u && !gone) {
        pcr_plane_of_draw(); // (exact Z: the plane pass's, pcr_exact_z.h)
    }
}
