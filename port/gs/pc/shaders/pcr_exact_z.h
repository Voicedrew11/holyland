// Exact Z by the triangle's plane (pcr_common.h's exact Z; included there, after pcr_depth).
//
// A pixel's Z is the triangle's plane through its three corners (X and Y in the GS's 12.4, Z with a moved corner's
// fraction) at the pixel's point, rounded to a whole Z (half to even), clamped to the Z buffer's largest. Worked out
// exactly, in integers: the value depends on the plane and the point alone, not on which corner is first, how the GPU
// interpolates, or which path (the GS's primitives, a native draw) drew the triangle. Two meshes over one surface
// (a decal, or two map records that both hold a wall where they meet) round alike wherever their planes agree, and
// apart only where the planes themselves differ.
//
// (Before, each corner handed down its Z less a shared whole Z, interpolated by the GPU in floats and rounded. Close to
// the eye a triangle's corners span millions of Z, the float's error passed half a Z, and which of two coinciding
// surfaces won turned with the camera over whole triangles: a wall at map 063 lit twice as bright in patches as two
// records' copies of it were both drawn, 2026-11-15.)
//
// The vertex stage hands each corner its triangle's three corners, the same at all three (pcr_plane), flat. The
// fragment stage (pcr_plane_z, the exact Z variants only) needs the GPU's 64-bit integers (shaderInt64). A triangle whose corners have no place
// on the GS's screen (whole_triangle.h's, drawn from the eye's space in a picture between) hands down none
// (pcr_plane_none): its Z is the interpolated one (v_zrel).
#ifndef PCR_EXACT_Z_H
#define PCR_EXACT_Z_H
// A corner's Z as the plane takes it: whole (x) and 256ths of a Z (y, 0..255) with a picture between's fraction
#define PCR_Z_FRACTION_BITS 8 // (plane_z.h's PLANE_Z_FRACTION)
// v_plane0.w's bit: the triangle has a plane (else its Z is the interpolated one)
#define PCR_PLANE_ON 0x100

// The triangle's three corners, each X and Y (16ths of a GS pixel, in the target: XYOFFSET off, the frame's place
// added), its whole Z and its 256ths (with PCR_PLANE_ON in corner 0's): the same at all three corners. 32-bit values:
// the vertex stage, which runs with exact Z on or off, needs nothing the GPU may lack; the fragment stage's exact Z
// works it out in 64 and 128 bits.
#ifdef PCR_STAGE_VERTEX
layout(location = 13) flat out ivec4 v_plane0;
layout(location = 14) flat out ivec4 v_plane1;
layout(location = 15) flat out ivec4 v_plane2;

// A corner's Z (z, and a picture between's fraction, about -1/2 to 1/2) to whole and 256ths, within 0 and the
// largest 32-bit Z (as a moved Z is: a corner past the far plane is dropped, one past the nearest clamped)
ivec2 pcr_plane_z_of(uint z, float zfrac) {
    int f = int(roundEven(clamp(zfrac, -1.0, 1.0) * float(1 << PCR_Z_FRACTION_BITS)));
    int whole = f >> PCR_Z_FRACTION_BITS; // (floor: -1, 0 or 1)
    if (whole < 0 && z == 0u) {
        return ivec2(0);
    }
    if (whole > 0 && z == 0xFFFFFFFFu) {
        return ivec2(-1, (1 << PCR_Z_FRACTION_BITS) - 1);
    }
    return ivec2(int(z + uint(whole)), f & ((1 << PCR_Z_FRACTION_BITS) - 1));
}

void pcr_plane_none() {
    v_plane0 = ivec4(0);
    v_plane1 = ivec4(0);
    v_plane2 = ivec4(0);
}

// The farthest a corner may be from the target's origin, in 16ths of a GS pixel: 65536 GS pixels, sixteen times the
// GS's whole range. Times the scale (at most 16, twice it with MSAA: pcr_plane_z) that is plane_z.h's PLANE_Z_MOST, the
// most its arithmetic holds. Past it only a picture between's corners turned far past the frame's screen (past_band.h):
// their Z is the interpolated one.
#define PCR_PLANE_MOST (1 << 20)

// A corner as the GS gets it: X and Y in 16ths of a GS pixel in the target (gs_to_clip's place times 16: XYOFFSET
// off, the frame's place added), its Z and a picture between's fraction of it
struct GsCorner {
    ivec2 xy;
    uint z;
    float zfrac;
};

// The triangle's corners handed down (its vertices' own, as each is drawn: the three corners call this alike)
void pcr_plane(GsCorner c0, GsCorner c1, GsCorner c2) {
    ivec2 most = max(max(abs(c0.xy), abs(c1.xy)), abs(c2.xy));
    if (max(most.x, most.y) > PCR_PLANE_MOST) {
        pcr_plane_none();
        return;
    }
    ivec2 a = pcr_plane_z_of(c0.z, c0.zfrac), b = pcr_plane_z_of(c1.z, c1.zfrac), c = pcr_plane_z_of(c2.z, c2.zfrac);
    v_plane0 = ivec4(c0.xy, a.x, a.y | PCR_PLANE_ON);
    v_plane1 = ivec4(c1.xy, b);
    v_plane2 = ivec4(c2.xy, c);
}

#else
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(location = 13) flat in ivec4 v_plane0;
layout(location = 14) flat in ivec4 v_plane1;
layout(location = 15) flat in ivec4 v_plane2;

#include "plane_z.h"

// A corner's Z in 256ths
int64_t pcr_plane_fixed(ivec4 corner) {
    return (int64_t(uint(corner.z)) << PCR_Z_FRACTION_BITS) | int64_t(corner.w & ((1 << PCR_Z_FRACTION_BITS) - 1));
}

// The Z of the triangle's plane at this pixel, whole and clamped to 0..zmax; false: the triangle has no plane
// (pcr_plane_none). The pixel's point: the GS's point a pixel of the target stands for (gs_to_clip's inverse): without
// MSAA the first of a GS pixel's pixels is at its integer point, so pixel k is at k / scale; with MSAA a pixel's
// middle is (k + 1/2) / scale. In 1/16 / m of a GS pixel (m the scale, twice it with MSAA) both are whole.
bool pcr_plane_z(out uint z) {
    z = 0u;
    if ((v_plane0.w & PCR_PLANE_ON) == 0) {
        return false;
    }
    bool msaa = (pc.flags & PCR_F_MSAA) != 0u;
    i64vec2 m = i64vec2(pc.src_scale) * (msaa ? 2l : 1l);
    i64vec2 k = i64vec2(floor(gl_FragCoord.xy));
    i64vec2 p = msaa ? 16l * (2l * k + 1l) : 16l * k;
    i64vec2 c0 = i64vec2(v_plane0.xy) * m, c1 = i64vec2(v_plane1.xy) * m, c2 = i64vec2(v_plane2.xy) * m;
    int64_t whole = plane_z(p.x, p.y, c0.x, c0.y, pcr_plane_fixed(v_plane0), c1.x, c1.y, pcr_plane_fixed(v_plane1),
                            c2.x, c2.y, pcr_plane_fixed(v_plane2));
    if (whole == PLANE_Z_NONE) {
        return false;
    }
    z = uint(clamp(whole, 0l, int64_t(pc.zmax)));
    return true;
}
#endif

#endif
