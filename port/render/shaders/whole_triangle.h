// A native draw's triangle drawn whole in a picture between (the PC renderer's pcr_nd000.vert, pcr_nd280.vert,
// pcr_nd700.vert), after the including shader's VU1 maths (its `d`, the draw's record with its screen, and `pic`).
//
// A picture between is the frame turned on to the picture's camera, and in a fast turn of the mouse 60 to 100
// degrees past the frame's view: there lie the triangles VU1 left out of the frame (past the guard band, or a corner
// behind the frame's eye), and with them left out the world, the characters and the effects there were missing until
// the game's next frame. A native draw has each vertex's own place, so such a triangle is drawn whole: each corner
// in Vulkan's clip space from VU1's place times w (c, its transform), not from the frame's screen place, and the GPU
// cuts the triangle to the screen and at the eye. A corner behind the picture's eye has a w below 0; S T Q, Z and the
// fog, all affine on the screen in 1 / w, stay right on the part drawn.
#ifndef WHOLE_TRIANGLE_H
#define WHOLE_TRIANGLE_H

// A corner at the picture's time: h.xy the GS's X and Y (pixels) times w, h.z the GS's Z times w, w the picture's
// depth along its view
struct Whole {
    vec3 h;
    float w;
};

// The frame's view of a place from VU1's c (X w, Y w, Z w, w): (X - cx) depth / sx, (Y - cy) depth / sy, depth; pw the
// position's own w (program 0x700's effects: 16, c.w is the depth times it; else 1)
vec3 whole_frame_view(vec4 c, float pw) {
    float sx = d.screen[0].x, sy = d.screen[1].y, cx = d.screen[2].x, cy = d.screen[2].y;
    return vec3((c.x - cx * c.w) / (sx * pw), (c.y - cy * c.w) / (sy * pw), c.w / pw);
}

// The corner moved by dm (the shader's picture move of the frame's view place v): X and Y projected again, Z as
// cz + k / w with the vertex's own k ((Z - cz) depth, as the moves make it)
Whole whole_corner(vec4 c, float pw, vec3 v, vec3 dm) {
    float sx = d.screen[0].x, sy = d.screen[1].y, cx = d.screen[2].x, cy = d.screen[2].y, cz = d.screen[2].z;
    vec3 m = v + dm;
    Whole r;
    r.w = m.z == 0.0 ? -1.0e-30 : m.z;
    r.h = vec3(sx * m.x + cx * m.z, sy * m.y + cy * m.z, cz * m.z + (c.z - cz * c.w) / pw);
    return r;
}

// Not drawn: no corner in front of the eye, or one in front past the far plane (Z below 0, as VU1 clips)
bool whole_outside(Whole w0, Whole w1, Whole w2) {
    bool far = (w0.w > 0.0 && w0.h.z < 0.0) || (w1.w > 0.0 && w1.h.z < 0.0) || (w2.w > 0.0 && w2.h.z < 0.0);
    return !(w0.w > 0.0 || w1.w > 0.0 || w2.w > 0.0) || far;
}

// Some corner is behind the picture's eye
bool whole_behind(Whole w0, Whole w1, Whole w2) {
    return !(w0.w > 0.0 && w1.w > 0.0 && w2.w > 0.0);
}

#if !defined(PCR_PLANES) && !defined(SCN_PLACE) // (the plane pass and the placing, compute shaders, draw nothing)
// gl_Position of corner c (and the exact Z's fields, pcr_common.h): offset the draw's XYOFFSET (12.4); far: at the far
// plane (the modern sky). The triangle's whole Z is its first corner in front's, so the others' Z less it are small.
void whole_position(Whole c, Whole w0, Whole w1, Whole w2, ivec2 offset, bool far) {
    Whole r = w0.w > 0.0 ? w0 : w1.w > 0.0 ? w1 : w2;
    uint zref = far ? 0u : uint(clamp(floor(r.h.z / r.w + 0.5), 0.0, 16777215.0));
    vec2 xy_h = c.h.xy - (vec2(offset) / 16.0 - vec2(pc.place)) * c.w;
    gl_Position = vec4(gs_to_clip_h(xy_h, c.w), pcr_depth_h(far ? 0.0 : c.h.z, c.w, zref), c.w);
}

// A corner of a triangle not drawn (all three outside the clip space)
void whole_not_drawn() {
    gl_Position = vec4(0.0, 0.0, 0.0, -1.0);
    pcr_depth(0u, 0.0, 0u);
}
#endif

#endif
