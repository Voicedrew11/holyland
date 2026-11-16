// The picture's plane pass (pcr_exact_z.h; pcr_canvas.cpp's Planes): the planes of a native program's triangles, one
// invocation a triangle, worked out before the command buffer's draws that read them (pcr_exact_z.h's
// pcr_plane_of_draw). Included by pcr_planes_<program>.comp after its program's buffers and VU1 maths, the same
// bindings as its vertex shader's; the including shader defines plane_corners, its triangle's corners as its vertex
// shader draws them.
#ifndef PCR_PLANES_H
#define PCR_PLANES_H

layout(local_size_x = 64) in;

// A triangle's entry, written by the canvas as a native batch is drawn: x its triangle in the program's tris, y the
// draw's instance (what gl_InstanceIndex is to its vertex shader), z the frame's place in its surface (GS pixels: X
// bits 0-15, Y 16-31), w the target's scale (X bits 0-7, Y 8-15)
layout(set = 1, binding = 0, std430) readonly buffer Entries {
    uvec4 e[];
} u_entries;
// The planes: entry n's two words (pcr_plane_words') at 2 n and 2 n + 1
layout(set = 1, binding = 1, std430) writeonly buffer PlanesOut {
    ivec4 w[];
} u_planes_out;

// The run of entries this dispatch works out (one program's, with the same buffers)
layout(push_constant, std430) uniform PlanesPush {
    uint first;
    uint count;
} planes_push;

// The triangle's three corners as its vertex shader draws them (place: the frame's place in its surface, GS pixels)
void plane_corners(uvec4 tri, uint instance, ivec2 place, out GsCorner c0, out GsCorner c1, out GsCorner c2);

void main() {
    if (gl_GlobalInvocationID.x >= planes_push.count) {
        return;
    }
    uint n = planes_push.first + gl_GlobalInvocationID.x;
    uvec4 e = u_entries.e[n];
    ivec2 place = ivec2(int(e.z & 0xFFFFu), int(e.z >> 16));
    ivec2 scale = ivec2(int(e.w & 0xFFu), int((e.w >> 8) & 0xFFu));
    GsCorner c0, c1, c2;
    plane_corners(tris[e.x], e.y, place, c0, c1, c2);
    ivec4 w0, w1;
    pcr_plane_words(c0, c1, c2, scale, w0, w1);
    u_planes_out.w[2u * n] = w0;
    u_planes_out.w[2u * n + 1u] = w1;
}

#endif
