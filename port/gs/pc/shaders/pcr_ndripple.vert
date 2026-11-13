#version 450
#extension GL_GOOGLE_include_directive : require
// The PC renderer's native draws of the reflection's rippled water (RD_PROGRAM_RIPPLE: port/gs/pc/pcr_native.cpp,
// port/render/ripple.hpp): a strip's triangles from the water record's mesh, each corner what Wobble_DrawRippledModel
// would have sent (port/render/shaders/ripple_maths.h), given to pcr_prim.frag as a GS primitive's are: X and Y in the
// target's GS pixels (XYOFFSET off, the surface's place added), XYZ2's 32-bit Z, RGBAQ's colour, S T 1 and FOG's fog.
// Which triangles the strip kicks (the EE's ADC) is the CPU's (ripple_adc). In a picture between (RIPPLE_C_MOVING) both
// places are moved by the picture's camera, S and T made from the moved lifted one (the reflection's lookup follows the
// camera between), and the triangle dropped with any of its corners.
#define PCR_ND
#include "pcr_common.h"

// the triangles: x, y, z the arena's numbers of its corners (the kick's two before it, then the kick's)
layout(set = 0, binding = 14, std430) readonly buffer Tris {
    uvec4 tris[];
};

layout(location = 0) noperspective out vec4 v_color;
layout(location = 1) noperspective out vec3 v_stq;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) flat out uvec2 v_rect; // (the 2D mode's: none for a native draw; pcr_prim.frag reads it)

// a picture between's (port/gs/pc/pcr_between.h; zeros outside one)
layout(set = 0, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];
    vec4 mid[3];
    vec4 screen[2];
    vec4 zt;
} pic;
// the meshes (the arena): four qwords a vertex (position, normal, ST, colour)
layout(set = 0, binding = 12, std430) readonly buffer Mesh {
    ivec4 q[];
} mesh;
#include "ripple_draw.h"
// the draws' records (port/render/ripple.hpp)
layout(set = 0, binding = 13, std430) readonly buffer Draws {
    Draw draws[];
};
#include "ripple_maths.h"

void main() {
#ifdef PCR_RECORDS
    pcr_pass_record(); // (the fragment stage's fields: pcr_common.h)
#endif
    uvec4 tri = tris[gl_VertexIndex / 3];
    uint which = uint(gl_VertexIndex) % 3u;
    uint at = which == 0u ? tri.x : which == 1u ? tri.y : tri.z;
    d = draws[uint(gl_InstanceIndex) & 0x3FFFFFFFu];
    ivec4 P = mesh.q[at * 4u];
    uint z;
    float zfrac = 0.0;
    Place p = vertex_place(at, z);
    // (exact Z, pcr_common.h: a whole Z the triangle's corners share, its first corner's in the frame)
    uint zref = z;
    if (which != 0u) {
        vertex_place(tri.x, zref);
    }
    Place w = lifted_place(P);
    int xi = p.x, yi = p.y, wx = w.x, wy = w.y;
    bool gone = false;
    if ((d.info.z & RIPPLE_C_MOVING) != 0) {
        Moved m = moved(p, z);
        xi = m.x;
        yi = m.y;
        z = m.z;
        zfrac = m.zfrac;
        Moved mw = moved(w, 0u);
        // (a moved place off the 16 bits keeps the frame's S and T: moved() starts from the GS's 16 bits)
        if (!mw.drop && ((w.x | w.y) & int(0xFFFF0000u)) == 0) {
            wx = mw.x;
            wy = mw.y;
        }
        for (uint i = 0u; i < 3u && !gone; i++) {
            uint k = i == 0u ? tri.x : i == 1u ? tri.y : tri.z;
            if (k == at) {
                gone = m.drop;
            } else {
                uint kz;
                Place kp = vertex_place(k, kz);
                gone = moved(kp, kz).drop;
            }
        }
    }
    vec2 xy = vec2(float(int(uint(xi) & 0xFFFFu) - d.info.x), float(int(uint(yi) & 0xFFFFu) - d.info.y)) / 16.0;
    xy += vec2(pc.place);
    if (gone) {
        xy = vec2(1.0e9); // (far past the target's right edge, as are its two other corners: not drawn)
    }
    uint rgba = uint(d.info.w);
    // as pcr_prim.vert
    vec2 ndc = gs_to_clip(xy);
    gl_Position = vec4(ndc, pcr_depth(z, zfrac, zref), 1.0);
    v_color = vec4(float(rgba & 0xFFu), float((rgba >> 8) & 0xFFu), float((rgba >> 16) & 0xFFu), float(rgba >> 24));
    v_stq = vec3(screen_st(wx, wy), 1.0);
    v_fog = d.fog.x;
    v_rect = uvec2(PCR_NO_RECT);
}
