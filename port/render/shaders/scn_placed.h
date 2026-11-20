// The scene renderer's placed vertices (port/gs/scene/shaders/scn_place_*.comp, port/gs/scene/scn_draw.cpp's
// place_vertices): a native draw's mesh vertices each worked out once by VU1's maths (vu1_000_maths.h,
// vu1_280_maths.h) before its triangles are drawn, and read by pcr_nd000.vert and pcr_nd280.vert made with SCN_PLACED,
// whose corners were each a mesh vertex's maths whole: about three times a vertex in a frame, up to nine in a picture
// between, the draws' small batches one after another (docs/plans/scene-slice4-results.md, "Vertices once").
//
// A job is a draw's run of vertices and the instance it is drawn with (pcr_native.h's NativeBatch): x the instance, y
// the arena's slot of its first vertex, z its first vertex here, w how many. The vertex shaders' firstInstance is
// their job's number.
#ifndef SCN_PLACED_H
#define SCN_PLACED_H

struct Placed {
    ivec2 xy;     // the GS's X and Y (12.4) as the draw takes them: VU1's place, in a picture between moved
    uint z;       // the GS's Z (moved in a picture between)
    uint zframe;  // the frame's Z (exact Z's whole Z a triangle shares: its first corner's in the frame)
    float zfrac;  // (moved) the moved Z less z
    float q;      // VU1's Q
    float f;      // (moved) the change of depth S, T and Q are multiplied by
    uint flags;   // SCN_PLACED_*
    vec4 st;      // xyz: the pass's S, T and Q before Q (the sphere pass's with Q 1); w: the fog (0..255)
    vec4 whole;   // (a picture between) whole_triangle.h's Whole: h, w
    uint rgba;    // the vertex's colour (the lights'; the sphere pass's white with vertex 1's alpha)
    float cut;    // the mirror image's cut at the water (0: none)
    uint pad0, pad1;
};

#define SCN_PLACED_DROP 1u    // (moved) moved()'s drop
#define SCN_PLACED_CLIPPED 2u // (0x000) VU1's ADC for it (vu_clipped)

uint scn_pack_rgba(uvec4 c) {
    return (c.x & 0xFFu) | (c.y & 0xFFu) << 8 | (c.z & 0xFFu) << 16 | (c.w & 0xFFu) << 24;
}

uvec4 scn_unpack_rgba(uint c) {
    return uvec4(c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24);
}

#endif
