// A native draw's record for VU1's program 0x000 on the GPU (port/include/port/render.h's draws): what VU1's set-up
// leaves in its memory for the vertex program, worked out on the CPU in VU1's arithmetic, laid out as the shaders read
// it (port/render/shaders/vu1_000_draw.h's Draw). For the renderers that draw native draws without VU1: the PC
// renderer (port/gs/pc/pcr_native.cpp). The native GS has its own copy (gsn_native_draw.inc, nd_const), the same
// maths. port/platform/vu1_hle.c (setup) is the specification.
#pragma once
#include <stdint.h>

extern "C" {
#include "port/render.h"
}

// A model part's move in the pictures between (vu1_000_draw.h's Between; gsn_native_draw.inc's NdBetween says what
// each field is)
struct Vu1Between {
    float rot[12];
    float s[12];
    float ds[12];
    float axis[4];
    float dt[4];
};
static_assert(sizeof(Vu1Between) == 11 * 16, "Vu1Between is the shaders' Between");

// vu1_000_draw.h's Draw (std430): matrices VU layout, a qword a column
struct Vu1Record {
    float vertex[16];    // VU1 0x14-0x17: world to screen by the part's matrix (VU1 0x00 by 0x04)
    float model[16];     // the part to the world
    float view[16];      // world to view
    float screen[16];    // view to screen (the pictures between take sx, sy, cx, cy and cz from it)
    float light_dir[16]; // the light directions by the part's rotation, a column each
    float light_col[16]; // the light colours, then the ambient
    float fog[4];        // VU1 0x3C
    float point_pos[16]; // in the part's space, w the falloff
    float point_col[16];
    float obj[4];        // (VU1_MOVE_OBJ) the part's move from the frame before to this one, in the world
    int32_t info[4];     // the point lights; XYOFFSET's X and Y (12.4); VU1_MOVE_* and VU1_* flags below
    Vu1Between bt;       // (VU1_MOVE_MODEL) the part's own move from the frame before
    uint32_t wave[16];   // (VU1_WAVE) the 16 waves' offsets to S (low 16 bits) and T (high): RdDraw.parent's
};
static_assert(sizeof(Vu1Record) == 50 * 16, "Vu1Record is the shaders' Draw");

// Vu1Record.info[3] (vu1_000_draw.h's ND_C_*)
enum : int32_t {
    VU1_MOVE_CAMERA = 0x1, // a picture between: moved by the picture's camera
    VU1_MOVE_OBJ = 0x2,    // ... and with its model, t of the way from its place in the frame before (obj)
    VU1_MOVE_MODEL = 0x4,  // ... and with its model, by its matrices between the frame before's and this one's (bt)
    VU1_MOVE_EYE = 0x8,    // (with VU1_MOVE_MODEL) the matrices are the eye's (the arm): not moved by the camera
    VU1_WAVE = 0x10,       // the wave UVs on the GPU (RD_DRAW_WAVE_UVS): S and T moved by their wave's offsets
    VU1_FAR = 0x20,        // at the far plane (RD_DRAW_FAR_Z: the modern sky): Z 0
};

// The draw's record for program 0x000: VU1's set-up of the vertex matrix, the light directions and the point lights;
// the draw's own fog, colours, wave offsets and far plane. XYOFFSET (info[1], info[2]) and the pictures between's
// moves (obj, bt, the VU1_MOVE_* flags) are left 0: the renderer's.
Vu1Record vu1_record_000(const RdDraw &draw, const RdCamera &camera);
