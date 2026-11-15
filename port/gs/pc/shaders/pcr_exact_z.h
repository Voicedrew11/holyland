// Exact Z by the triangle's plane (pcr_common.h's exact Z; included there, after pcr_depth).
//
// A pixel's Z is the triangle's plane through its three corners (X and Y in the GS's 12.4, Z with a moved corner's
// fraction) at the pixel's point, rounded to a whole Z (half to even), clamped to the Z buffer's largest. The plane is
// worked out once a triangle, by the vertex stage, in integers (plane_z.h's plane_fixed): its Z at the target's pixel
// (0, 0) and what a pixel across and down adds, each the exact value rounded to 2^-26 of a Z. Those depend on the plane
// alone, not on which corner is first, how the GPU interpolates, or which path (the GS's primitives, a native draw)
// drew the triangle: two meshes over one surface (a decal, or two map records that both hold a wall where they meet)
// have the same three numbers and so the same Z at every pixel, and round apart only where their planes differ. The
// fragment stage adds them up at its pixel (pz_pixels_z: 32-bit operations), within 2^-12 of a Z of the
// exact.
//
// (Before, each corner handed down its Z less a shared whole Z, interpolated by the GPU in floats and rounded. Close to
// the eye a triangle's corners span millions of Z, the float's error passed half a Z, and which of two coinciding
// surfaces won turned with the camera over whole triangles: a wall at map 063 lit twice as bright in patches as two
// records' copies of it were both drawn, 2026-11-15. The plane worked out exactly at each pixel, in 128 bits, took the
// GPU 2.6 times as long at the owner's settings: 5.7 ms a picture against 2.2.)
//
// Both stages need the GPU's 64-bit integers (shaderInt64): the exact Z variants only (tools/build_shaders.py's
// EXACT_Z, vertex shaders too). A native draw's plane is worked out by its triangle's first corner alone (the provoking
// vertex: the flat outputs are its). A triangle whose corners have no place on the GS's screen (whole_triangle.h's,
// drawn from the eye's space in a picture between), or turned far past it (PCR_PLANE_MOST), or whose plane plane_fixed
// cannot hold (seen edge on), hands down none (pcr_plane_none): its Z is the interpolated one (v_zrel).
#ifndef PCR_EXACT_Z_H
#define PCR_EXACT_Z_H
// A corner's Z as the plane takes it: whole (x) and 256ths of a Z (y, 0..255) with a picture between's fraction
#define PCR_Z_FRACTION_BITS 8 // (plane_z.h's PLANE_Z_FRACTION)
// v_plane1.z's bit: the triangle has a plane (else its Z is the interpolated one)
#define PCR_PLANE_ON 0x1

#ifdef PCR_EXACT_Z
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif

// The triangle's plane as plane_z.h's PzPixels (first_low and first_high in v_plane0.xy; first_x + 1 and first_y + 1
// in .z's 16-bit halves; step_x_low in .w, step_y_low and steps_high in v_plane1.xy), and PCR_PLANE_ON in v_plane1.z:
// the same at all three corners, flat
#ifdef PCR_STAGE_VERTEX
layout(location = 13) flat out ivec4 v_plane0;
layout(location = 14) flat out ivec4 v_plane1;

void pcr_plane_none() {
    v_plane0 = ivec4(0);
    v_plane1 = ivec4(0);
}

// The farthest a corner may be from the target's origin, in 16ths of a GS pixel: 65536 GS pixels, sixteen times the
// GS's whole range. Times the scale (at most 16, twice it with MSAA) that is plane_z.h's PLANE_Z_MOST, the most its
// arithmetic holds. Past it only a picture between's corners turned far past the frame's screen (past_band.h): their
// Z is the interpolated one.
#define PCR_PLANE_MOST (1 << 20)

// A corner as the GS gets it: X and Y in 16ths of a GS pixel in the target (gs_to_clip's place times 16: XYOFFSET
// off, the frame's place added), its Z and a picture between's fraction of it
struct GsCorner {
    ivec2 xy;
    uint z;
    float zfrac;
};

#ifdef PCR_EXACT_Z
#include "plane_z.h"

// A corner's Z (z, and a picture between's fraction, about -1/2 to 1/2) in 256ths, within 0 and the largest 32-bit Z
// (as a moved Z is: a corner past the far plane is dropped, one past the nearest clamped)
int64_t pcr_plane_z_of(GsCorner c) {
    int64_t z256 = (int64_t(c.z) << PCR_Z_FRACTION_BITS) +
                    int64_t(roundEven(clamp(c.zfrac, -1.0, 1.0) * float(1 << PCR_Z_FRACTION_BITS)));
    return clamp(z256, int64_t(0), (int64_t(0xFFFFFFFFu) << PCR_Z_FRACTION_BITS) | int64_t(255));
}

// The triangle's plane handed down, from its corners as each is drawn. The target's pixel k stands for the GS's point
// gs_to_clip's inverse gives it: without MSAA the first of a GS pixel's pixels is at its integer point, so k / scale;
// with MSAA a pixel's middle, (k + 1/2) / scale. In 1/16 / m of a GS pixel (m the scale, twice it with MSAA) both are
// whole: 16 k, or 16 (2 k + 1).
void pcr_plane(GsCorner c0, GsCorner c1, GsCorner c2) {
    ivec2 most = max(max(abs(c0.xy), abs(c1.xy)), abs(c2.xy));
    if (max(most.x, most.y) > PCR_PLANE_MOST) {
        pcr_plane_none();
        return;
    }
    bool msaa = (pc.flags & PCR_F_MSAA) != 0u;
    i64vec2 m = i64vec2(pc.src_scale) * (msaa ? 2l : 1l);
    i64vec2 p0 = i64vec2(c0.xy) * m, p1 = i64vec2(c1.xy) * m, p2 = i64vec2(c2.xy) * m;
    PzFixed f = plane_fixed(msaa ? 32l : 16l, msaa ? 16l : 0l, p0.x, p0.y, pcr_plane_z_of(c0), p1.x, p1.y,
                            pcr_plane_z_of(c1), p2.x, p2.y, pcr_plane_z_of(c2));
    if (!f.on) {
        pcr_plane_none();
        return;
    }
    // (the least pixel the triangle covers less one, -1 at least: pz_pixels)
    int64_t step = msaa ? 32l : 16l, first = msaa ? 16l : 0l;
    i64vec2 least = min(min(p0, p1), p2) - first;
    least = i64vec2(least.x >= 0l ? least.x / step : -((-least.x + step - 1l) / step),
                    least.y >= 0l ? least.y / step : -((-least.y + step - 1l) / step)) - 1l;
    ivec2 at = ivec2(clamp(least, i64vec2(-1l), i64vec2(32766l)));
    PzPixels px = pz_pixels(f, at.x, at.y);
    v_plane0 = ivec4(int(px.first_low), int(px.first_high), (at.x + 1) | ((at.y + 1) << 16), int(px.step_x_low));
    v_plane1 = ivec4(int(px.step_y_low), int(px.steps_high), PCR_PLANE_ON, 0);
}
#else
// (the variants without exact Z: the GPU may have no 64-bit integers)
void pcr_plane(GsCorner c0, GsCorner c1, GsCorner c2) {
    pcr_plane_none();
}
#endif

#else
layout(location = 13) flat in ivec4 v_plane0;
layout(location = 14) flat in ivec4 v_plane1;

#include "plane_z.h"

// The Z of the triangle's plane at this pixel, whole and clamped to 0..zmax; false: the triangle has no plane
// (pcr_plane_none). (The pixel's, even in the shaders run once a sample.)
bool pcr_plane_z(out uint z) {
    z = 0u;
    if ((v_plane1.z & PCR_PLANE_ON) == 0) {
        return false;
    }
    PzPixels p;
    p.first_low = uint(v_plane0.x);
    p.first_high = uint(v_plane0.y);
    p.first_x = (v_plane0.z & 0xFFFF) - 1;
    p.first_y = (v_plane0.z >> 16) - 1;
    p.step_x_low = uint(v_plane0.w);
    p.step_y_low = uint(v_plane1.x);
    p.steps_high = uint(v_plane1.y);
    ivec2 k = ivec2(gl_FragCoord.xy); // (floor: not negative)
    z = pz_pixels_z(p, k.x, k.y, pc.zmax);
    return true;
}
#endif

#endif
