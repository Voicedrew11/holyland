#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's GS primitives (port/gs/pc/pcr_canvas.cpp): already in the target's GS pixels (the GS's X and Y
// less XYOFFSET), one vertex a triangle corner. A GS pixel is drawn when its integer point is inside a primitive;
// Vulkan tests a pixel's middle, so every vertex moves half a GS pixel (at any scale: the middle of its pixels).
// The GS interpolates everything linearly across the screen: noperspective throughout.
//
// In a picture between (port/gs/pc/pcr_between.h) a corner names the kicks it was made of (a_kicks), and is made again
// of them at the picture's time (port/render/shaders/gif_moves.h, gs_replay.comp's maths) as the canvas made it of
// them in the frame: a triangle's corner of its own kick (its colour the third's when flat), a sprite's of its two;
// a triangle whose third kick, or one of the two before it with a place, is dropped there is not drawn.
#include "pcr_common.h"

layout(location = 0) in vec2 a_pos;
layout(location = 1) in uint a_z;
layout(location = 2) in uvec4 a_color;
layout(location = 3) in vec3 a_stq;
layout(location = 4) in float a_fog;
// (pictures between) x: a triangle's corner's own kick, a sprite's first (NONE: as it is); y: the colour's kick, a
// sprite's second; z: a triangle's third kick; w: PCR_K_* below
layout(location = 5) in uvec4 a_kicks;

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;

// the picture's (gs_replay.h's Picture; zeros outside a picture between)
layout(set = 1, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;
    uvec4 counts;
    uint run_bits[];
} pic;
// the walk's kicks as the GS queued them (gsn.cpp's InterpKick), 16 words each
layout(set = 1, binding = 1, std430) readonly buffer Kicks {
    uint kicks[];
};
// how each kick moves (gsn.h's GsnInterpMove), 16 words each
layout(set = 1, binding = 2, std430) readonly buffer Moves {
    uint moves[];
};
#define GIF_KICK_NOW
#include "gif_moves.h"

#define NONE 0xFFFFFFFFu
// a_kicks.w (pcr_canvas.cpp's K_*): a sprite's corner (bits 0-1: 0 its first kick's X and Y, 1 the second's X and the
// first's Y, 2 the first's X and the second's Y, 3 the second's), a sprite's, UV for S and T
#define PCR_K_CORNER 3u
#define PCR_K_SPRITE 4u
#define PCR_K_FST 8u

// a moved kick's place less its XYOFFSET, in the surface (the frame's place in it added)
vec2 place_of(KickNow r, uint k) {
    uint offset = K(k, 8u);
    return vec2(float(r.x - int(offset & 0xFFFFu)), float(r.y - int(offset >> 16))) / 16.0 + vec2(pc.place);
}

vec3 stq_of(KickNow r, bool fst) {
    return fst ? vec3(float(r.uv & 0x3FFFu) / 16.0, float((r.uv >> 16) & 0x3FFFu) / 16.0, 1.0) : vec3(r.s, r.t, r.q);
}

uvec4 colour_of(uint rgba) {
    return uvec4(rgba & 0xFFu, (rgba >> 8) & 0xFFu, (rgba >> 16) & 0xFFu, rgba >> 24);
}

// dropped at the picture's time (moved as a vertex with a place, and dropped)
bool dropped(uint k) {
    if (k >= pic.counts.x) {
        return false;
    }
    Moved m = moved(k);
    return m.proc && m.drop;
}

struct Corner {
    vec2 pos;
    uint z;
    uvec4 rgba;
    vec3 stq;
    float fog;
};

// a sprite's corner (the canvas's add_primitive: Z, colour, fog and Q the second's; each corner's S / Q and T / Q
// over the second's Q)
Corner sprite_corner(uint ka, uint kb, uint corner, bool fst) {
    KickNow a = kick_now(ka), b = kick_now(kb);
    bool ax = corner == 0u || corner == 2u, ay = corner < 2u;
    vec2 pa = place_of(a, ka), pb = place_of(b, kb);
    Corner c;
    c.pos = vec2(ax ? pa.x : pb.x, ay ? pa.y : pb.y);
    c.z = b.z;
    c.rgba = colour_of(b.rgba);
    c.fog = float(K(kb, 7u));
    vec3 sa = stq_of(a, fst), sb = stq_of(b, fst);
    if (!fst) {
        float q = sb.z != 0.0 ? sb.z : 1.0;
        float ua = sa.z != 0.0 ? sa.x / sa.z : 0.0, ta = sa.z != 0.0 ? sa.y / sa.z : 0.0;
        float ub = sb.x / q, tb = sb.y / q;
        sa = vec3(ua * q, ta * q, q);
        sb = vec3(ub * q, tb * q, q);
    }
    c.stq = vec3(ax ? sa.x : sb.x, ay ? sa.y : sb.y, sb.z);
    return c;
}

// a triangle's corner: its own kick's, the colour the colour kick's; gone when the triangle is dropped there
Corner triangle_corner(uint k, uint colour, uint last, bool fst, out bool gone) {
    KickNow r = kick_now(k);
    Corner c;
    c.pos = place_of(r, k);
    c.z = r.z;
    c.rgba = colour_of(colour == k ? r.rgba : kick_now(colour).rgba);
    c.stq = stq_of(r, fst);
    c.fog = float(K(k, 7u));
    Moved m = moved(last);
    gone = m.proc && (m.drop || dropped(M(last, 2u)) || dropped(M(last, 3u)));
    return c;
}

void main() {
    vec2 pos = a_pos;
    uint z = a_z;
    vec4 colour = vec4(a_color);
    vec3 stq = a_stq;
    float fog = a_fog;
    if (a_kicks.x != NONE && a_kicks.x < pic.counts.x && a_kicks.y < pic.counts.x) {
        bool fst = (a_kicks.w & PCR_K_FST) != 0u;
        Corner c;
        bool gone = false;
        if ((a_kicks.w & PCR_K_SPRITE) != 0u) {
            c = sprite_corner(a_kicks.x, a_kicks.y, a_kicks.w & PCR_K_CORNER, fst);
        } else if (a_kicks.z < pic.counts.x) {
            c = triangle_corner(a_kicks.x, a_kicks.y, a_kicks.z, fst, gone);
        } else {
            c = Corner(a_pos, a_z, a_color, a_stq, a_fog);
        }
        pos = gone ? vec2(1.0e9) : c.pos; // (far past the target's right edge, as are its two other corners: not drawn)
        z = c.z;
        colour = vec4(c.rgba);
        stq = c.stq;
        fog = c.fog;
    }
    vec2 ndc = (pos + 0.5) * pc.inv_size * 2.0 - 1.0;
    // (the GS keeps Z within what the Z buffer's format holds; taken in at the corners)
    gl_Position = vec4(ndc, float(min(z, pc.zmax)) * pc.zscale, 1.0);
    v_color = colour;
    v_stq = stq;
    v_fog = fog;
}
