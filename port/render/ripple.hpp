// A native draw of the reflection's rippled water (RD_PROGRAM_RIPPLE: game/native_draw/nd_water.c) on the GPU, the CPU's
// part. Wobble_DrawRippledModel (gfx/wobble.c) runs no VU1 program: the EE makes each vertex of the water record's
// blocks itself, in VU0's arithmetic, and sends each block as a PATH2 strip:
//   its place: FTOI4 of (the world to screen matrix by the part's) times (x, y, z, 1), divided by w; Z the GS gets that
//     >> 4 (XYZ2: 32 bits);
//   its ADC: its place (Z >> 4 above 0xFFFF0, X or Y past 16 bits) or one of the two before it in the block's out of
//     bounds, or the data's (the normal's w, bit 15);
//   its S and T: the place of (x, y + sine * 5, z, 1) (the sine wobble_sines[|(x + z)^3 % 32|]), (X - 0x6C00) / 10240
//     and (Y - 0x7200) / 7168 in the game's floats; Q 1; its colour RGBAQ's (the map's tint).
// The renderer draws the strips from the record's own meshes (RD_VF_PACKED16: only the positions and the data's ADC are
// read): the record here (the matrix, the sines) and which triangles the strips kick (the ADC plan); the places, S and T
// in the vertex shader (port/render/shaders/ripple_maths.h), which in a picture between makes them again from the
// places moved by the picture's camera (the reflection stays under the water).
#pragma once
#include <stdint.h>

#include <vector>

#include "render_internal.hpp"

// shaders/ripple_draw.h's Draw (std430)
struct RippleRecord {
    float vertex[16]; // the world to screen matrix by the part's (VU arithmetic), a qword a column
    float screen[16]; // view to screen (the pictures between take sx, sy, cx, cy and cz from it)
    float sines[32];  // wobble_sines of the frame
    int32_t info[4];  // XYOFFSET's X and Y (12.4) of the strip's context, RIPPLE_* flags, RGBAQ's RGBA: the renderer's
    float fog[4];     // FOG's fog (XYZ2 takes it), in x: the renderer's
};
static_assert(sizeof(RippleRecord) == 18 * 16, "RippleRecord is ripple_draw.h's Draw");

// RippleRecord.info[2] (ripple_draw.h's RIPPLE_C_*)
enum : int32_t {
    RIPPLE_MOVE_CAMERA = 0x1, // a picture between: moved by the picture's camera
    RIPPLE_NEAREST = 0x100,   // the game's floats round to nearest (--fpu-round nearest), not toward zero
};

// The draw's record: the vertex matrix (the camera's world_screen by the part's matrix, as Wobble_DrawRippledModel's
// Matrix_Multiply makes it in VU0's arithmetic), the view to screen, the sines, the rounding. XYOFFSET, the colour, the
// fog and the move are left 0: the renderer's.
RippleRecord ripple_record(const RdDraw &draw, const RdCamera &camera);

// A vertex's place as the EE leaves it (Vu0_ProjectToScreen3 and FTOI4): X and Y 12.4, Z as XYZ2 takes it (>> 4)
struct RipplePlace {
    int32_t x, y, z;
};
// The place of (x, y, z, 1) by the record's vertex matrix
RipplePlace ripple_place(const RippleRecord &record, float x, float y, float z);

// A block's strip as the EE sends it: each vertex's ADC (vertices: the block's, four qwords each as
// RD_VF_PACKED16's, n of them). adc[k] set: kick k draws nothing.
void ripple_adc(const int32_t *vertices, uint32_t n, const RippleRecord &record, std::vector<uint8_t> &adc);
