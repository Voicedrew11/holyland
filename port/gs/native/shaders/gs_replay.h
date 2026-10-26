// --frame-rate's pictures between drawn on the GPU (port/gs/native/gsn.cpp, "Pictures between on the
// GPU"): what gs_replay.comp (each vertex kick moved to the picture's time) and gs_replay_corners.comp (each
// corner of a triangle made of the kicks) share. Set 1.

// the picture's (gsn.cpp, InterpPictureHeader)
layout(set = 1, binding = 0, std430) readonly buffer Picture {
    vec4 move[3];   // the frame's view to the picture's: rows of the rotation less the identity, each with its translation
    vec4 mid[3];    // the world to the picture's view: rows of the rotation (and the translation)
    vec4 screen[2]; // sx sy cx cy: the world's projection, the reflection's
    vec4 zt;        // cz (the world's, the reflection's), t, 1 - t
    uvec4 counts;   // the kicks, the runs; zw (floats) the picture's sub-pixel jitter in NDC (upscaling: sa_*_jitter.vert)
    uint run_bits[];
} pic;
// the frame's kicks as the GS queued them (gsn.cpp, InterpKick), 16 words each
layout(set = 1, binding = 1, std430) readonly buffer Kicks {
    uint kicks[];
};
// each kick at the picture's time (gs_replay.comp's), 8 words each: X | Y << 16 (12.4), Z, RGBA, S, T, Q,
// U | V << 16, and bit 0 moved as a vertex with a place (frame_interp.c's move_vertex), bit 1 dropped
// there (its triangles not drawn)
#define RK_PROC 1u
#define RK_DROP 2u

#define NONE 0xFFFFFFFFu
// gsn.cpp's IC_*: how a corner is made of its kicks
#define IC_SPRITE 0x8u
#define IC_FLAT_SHADE 0x10u
#define IC_FLAT_2D 0x20u
#define IC_SNAP 0x40u
#define IC_FST 0x80u
