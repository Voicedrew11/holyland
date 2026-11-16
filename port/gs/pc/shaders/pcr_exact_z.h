// Exact Z by the triangle's plane (pcr_common.h's exact Z; included there, after pcr_depth).
//
// A pixel's Z is the triangle's plane through its three corners (X and Y in the GS's 12.4, Z with a moved corner's
// fraction) at the pixel's point, rounded to a whole Z (half to even), clamped to the Z buffer's largest. The plane is
// worked out once a triangle in integers (plane_z.h's plane_fixed, pcr_plane_words): its Z at the target's pixel
// (0, 0) and what a pixel across and down adds, each the exact value rounded to 2^-26 of a Z. Those depend on the plane
// alone, not on which corner is first, how the GPU interpolates, or which path (the GS's primitives, a native draw)
// drew the triangle: two meshes over one surface (a decal, or two map records that both hold a wall where they meet)
// have the same three numbers and so the same Z at every pixel, and round apart only where their planes differ. The
// fragment stage adds them up at its pixel (pz_pixels_z: 32-bit operations), within 2^-12 of a Z of the exact.
//
// Who works the plane out: for a native draw, the picture's plane pass (pcr_planes.h, a compute shader a native
// program, run before the command buffer's draws: pcr_canvas.cpp's Planes), one invocation a triangle, into a buffer
// the draw's provoking vertex reads its triangle's from (pcr_plane_of_draw); for a GS primitive, the provoking vertex
// itself (pcr_plane, pcr_prim.vert). (Worked out by each native draw's provoking vertex, the plane's 128-bit arithmetic
// in 1 of 3 lanes of a vertex shader that already spilled took the GPU about 10% longer at map 063: 2.44-2.52 ms a
// picture against 2.21; 2026-11-15.)
//
// (Before, each corner handed down its Z less a shared whole Z, interpolated by the GPU in floats and rounded. Close to
// the eye a triangle's corners span millions of Z, the float's error passed half a Z, and which of two coinciding
// surfaces won turned with the camera over whole triangles: a wall at map 063 lit twice as bright in patches as two
// records' copies of it were both drawn, 2026-11-15. The plane worked out exactly at each pixel, in 128 bits, took the
// GPU 2.6 times as long at the owner's settings: 5.7 ms a picture against 2.2.)
//
// The plane's arithmetic needs the GPU's 64-bit integers (shaderInt64): the plane pass, and the exact Z variants
// (tools/build_shaders.py's EXACT_Z) of pcr_prim.vert and the fragment shaders. A triangle whose corners have no place
// on the GS's screen (whole_triangle.h's, drawn from the eye's space in a picture between), or turned far past it
// (PCR_PLANE_MOST), or whose plane plane_fixed cannot hold (seen edge on), hands down none (pcr_plane_none): its Z is
// the interpolated one (v_zrel).
#ifndef PCR_EXACT_Z_H
#define PCR_EXACT_Z_H
// A corner's Z as the plane takes it: whole (x) and 256ths of a Z (y, 0..255) with a picture between's fraction
#define PCR_Z_FRACTION_BITS 8 // (plane_z.h's PLANE_Z_FRACTION)
// v_plane0.z's bit: the triangle has a plane (else its Z is the interpolated one)
#define PCR_PLANE_ON 0x8000

#ifdef PCR_EXACT_Z
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

// A corner as the GS gets it: X and Y in 16ths of a GS pixel in the target (gs_to_clip's place times 16: XYOFFSET
// off, the frame's place added), its Z and a picture between's fraction of it
struct GsCorner {
    ivec2 xy;
    uint z;
    float zfrac;
};

// The farthest a corner may be from the target's origin, in 16ths of a GS pixel: 65536 GS pixels, sixteen times the
// GS's whole range. Times the scale (at most 16, twice it with MSAA) that is plane_z.h's PLANE_Z_MOST, the most its
// arithmetic holds. Past it only a picture between's corners turned far past the frame's screen (past_band.h): their
// Z is the interpolated one.
#define PCR_PLANE_MOST (1 << 20)

#ifdef PCR_EXACT_Z
#include "plane_z.h"

// A corner's Z (z, and its fraction: a native draw's own, 0 to 1, plus a picture between's move, about -1/2 to 1/2)
// in 256ths, within 0 and the largest 32-bit Z (as a moved Z is: a corner past the far plane is dropped, one past the
// nearest clamped)
int64_t pcr_plane_z_of(GsCorner c) {
    int64_t z256 = (int64_t(c.z) << PCR_Z_FRACTION_BITS) +
                    int64_t(roundEven(clamp(c.zfrac, -2.0, 2.0) * float(1 << PCR_Z_FRACTION_BITS)));
    return clamp(z256, int64_t(0), (int64_t(0xFFFFFFFFu) << PCR_Z_FRACTION_BITS) | int64_t(255));
}

// (a native plane pass's) A corner's own fraction of a Z: VU1's Z before FTOI4 (zf) less the GS's whole Z of it (z),
// 0 to 1. The GS takes whole Z at the corners; with it rounded off, two surfaces less than a Z apart (a rope bound on
// a log, map 000) sorted by that rounding, which each game frame made again: the fine part (plane_z.h) flickered
// between them at the game's rate. Native draws alone: a GS primitive's corners come whole.
float pcr_frame_zfrac(float zf, uint z) {
    float f = zf - float(z);
    return f >= 0.0 && f < 1.0 ? f : 0.0;
}

// The triangle's plane as the fragment stage takes it (v_plane0 and v_plane1, below), from its three corners, in a
// target of scale (the target's pixels a GS pixel, across and down) multisampled or not. The target's pixel k stands
// for the GS's point gs_to_clip's inverse gives it: without MSAA the first of a GS pixel's pixels is at its integer
// point, so k / scale; with MSAA a pixel's middle, (k + 1/2) / scale. In 1/16 / m of a GS pixel (m the scale, twice
// it with MSAA) both are whole: 16 k, or 16 (2 k + 1). None (zeros) past PCR_PLANE_MOST or where plane_fixed cannot
// hold the plane.
void pcr_plane_words(GsCorner c0, GsCorner c1, GsCorner c2, ivec2 scale, bool msaa, out ivec4 w0, out ivec4 w1) {
    w0 = ivec4(0);
    w1 = ivec4(0);
    ivec2 most = max(max(abs(c0.xy), abs(c1.xy)), abs(c2.xy));
    if (max(most.x, most.y) > PCR_PLANE_MOST) {
        return;
    }
    i64vec2 m = i64vec2(scale) * (msaa ? 2l : 1l);
    i64vec2 p0 = i64vec2(c0.xy) * m, p1 = i64vec2(c1.xy) * m, p2 = i64vec2(c2.xy) * m;
    PzFixed f = plane_fixed(msaa ? 32l : 16l, msaa ? 16l : 0l, p0.x, p0.y, pcr_plane_z_of(c0), p1.x, p1.y,
                            pcr_plane_z_of(c1), p2.x, p2.y, pcr_plane_z_of(c2));
    if (!f.on) {
        return;
    }
    // (the least pixel the triangle covers less one, -1 at least: pz_pixels)
    int64_t step = msaa ? 32l : 16l, first = msaa ? 16l : 0l;
    i64vec2 least = min(min(p0, p1), p2) - first;
    least = i64vec2(least.x >= 0l ? least.x / step : -((-least.x + step - 1l) / step),
                    least.y >= 0l ? least.y / step : -((-least.y + step - 1l) / step)) - 1l;
    ivec2 at = ivec2(clamp(least, i64vec2(-1l), i64vec2(32766l)));
    PzPixels px = pz_pixels(f, at.x, at.y);
    w0 = ivec4(int(px.first_low), int(px.first_high), (at.x + 1) | ((at.y + 1) << 16) | PCR_PLANE_ON,
               int(px.step_x_low));
    w1 = ivec4(int(px.step_y_low), int(px.steps_high), floatBitsToInt(px.step_x_z), floatBitsToInt(px.step_y_z));
}
#endif

// The triangle's plane as plane_z.h's PzPixels (first_low and first_high in v_plane0.xy; first_x + 1 and first_y + 1
// in .z's bits 0-14 and 16-31, with PCR_PLANE_ON; step_x_low in .w; step_y_low, steps_high, step_x_z and step_y_z in
// v_plane1): the same at all three corners, flat
#ifdef PCR_STAGE_VERTEX
layout(location = 13) flat out ivec4 v_plane0;
layout(location = 14) flat out ivec4 v_plane1;

void pcr_plane_none() {
    v_plane0 = ivec4(0);
    v_plane1 = ivec4(0);
}

#ifdef PCR_ND
// (a native draw's) The planes the picture's plane pass worked out (pcr_planes.h: a triangle's two words at 2 n and
// 2 n + 1), where the draw has them (pcr_canvas.cpp's Planes)
layout(set = 1, binding = 9, std430) readonly buffer Planes {
    ivec4 w[];
} u_planes;

#define PCR_NO_PLANE 0xFFFFFFFFu

// This corner's triangle's plane handed down from the plane pass's (the provoking vertex's alone: its flat outputs
// are drawn): the draw's triangle (gl_VertexIndex / 3) from the push constants' or record's plane_at on; none where
// the draw has no planes
void pcr_plane_of_draw() {
    if (pc.plane_at == PCR_NO_PLANE || uint(gl_VertexIndex) % 3u != 0u) {
        return;
    }
    uint at = uint(gl_VertexIndex) / 3u + pc.plane_at;
    v_plane0 = u_planes.w[2u * at];
    v_plane1 = u_planes.w[2u * at + 1u];
}
#else
#ifdef PCR_EXACT_Z
// (a GS primitive's) The triangle's plane handed down, from its corners as each is drawn
void pcr_plane(GsCorner c0, GsCorner c1, GsCorner c2) {
    pcr_plane_words(c0, c1, c2, pc.src_scale, (pc.flags & PCR_F_MSAA) != 0u, v_plane0, v_plane1);
}
#else
// (the variants without exact Z: the GPU may have no 64-bit integers)
void pcr_plane(GsCorner c0, GsCorner c1, GsCorner c2) {
    pcr_plane_none();
}
#endif
#endif

#elif !defined(PCR_PLANES)
layout(location = 13) flat in ivec4 v_plane0;
layout(location = 14) flat in ivec4 v_plane1;

#include "plane_z.h"

// The Z of the triangle's plane at this pixel, whole and clamped to 0..zmax, and its fine part (plane_z.h's
// pz_fine_bits); false: the triangle has no plane (pcr_plane_none). (The pixel's, even in the shaders run once a
// sample.)
bool pcr_plane_z(out uint z, out uint fine) {
    z = 0u;
    fine = 0u;
    if ((v_plane0.z & PCR_PLANE_ON) == 0) {
        return false;
    }
    PzPixels p;
    p.first_low = uint(v_plane0.x);
    p.first_high = uint(v_plane0.y);
    p.first_x = (v_plane0.z & 0x7FFF) - 1;
    p.first_y = (v_plane0.z >> 16) - 1;
    p.step_x_low = uint(v_plane0.w);
    p.step_y_low = uint(v_plane1.x);
    p.steps_high = uint(v_plane1.y);
    p.step_x_z = intBitsToFloat(v_plane1.z);
    p.step_y_z = intBitsToFloat(v_plane1.w);
    vec2 k = floor(gl_FragCoord.xy); // (not negative)
    // (the quick way, a few floats; where it cannot tell, the exact sum: the same Z, plane_z.h)
    if (!pz_pixels_quick(p, k.x, k.y, pc.zmax, z, fine)) {
        z = pz_pixels_z(p, int(k.x), int(k.y), pc.zmax, fine);
    }
    return true;
}

// (the depth rule, pcr_prim.h's PCR_DEPTH_RULE) The triangle's plane at this pixel, its Z with the fraction (in Z,
// half a Z added, clamped to 0..zmax: plane_z.h's pz_pixels_depth), and how far the GS's own rounding could move it
// (tolerance, in Z): its corners are on the GS's grid of 1/16 of a pixel, so each is up to 1/32 of a GS pixel from
// where the game put it (scale/32 of the target's pixels: the plane's step a target pixel times that, across and down)
// and its Z up to half a step of the corners' 256ths. Two meshes of one surface cut into other triangles are within it
// of each other; two surfaces apart by more than it are not. False: the triangle has no plane.
bool pcr_plane_depth(out float z, out float tolerance) {
    z = 0.0;
    tolerance = 0.0;
    if ((v_plane0.z & PCR_PLANE_ON) == 0) {
        return false;
    }
    PzPixels p;
    p.first_low = uint(v_plane0.x);
    p.first_high = uint(v_plane0.y);
    p.first_x = (v_plane0.z & 0x7FFF) - 1;
    p.first_y = (v_plane0.z >> 16) - 1;
    p.step_x_low = uint(v_plane0.w);
    p.step_y_low = uint(v_plane1.x);
    p.steps_high = uint(v_plane1.y);
    p.step_x_z = intBitsToFloat(v_plane1.z);
    p.step_y_z = intBitsToFloat(v_plane1.w);
    vec2 k = floor(gl_FragCoord.xy);
    if (!pz_pixels_depth_quick(p, k.x, k.y, pc.zmax, z)) {
        z = pz_pixels_depth(p, int(k.x), int(k.y), pc.zmax);
    }
    vec2 corner_step = vec2(pc.src_scale) * (1.0 / 32.0);
    tolerance = abs(p.step_x_z) * corner_step.x + abs(p.step_y_z) * corner_step.y + 1.0 / 512.0;
    return true;
}
#endif

#endif
