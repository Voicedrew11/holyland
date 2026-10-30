// VU1's program 0x700 (the effects) on the GPU, shared by the renderers that draw native draws: the native GS's
// nd_700.vert (and its builds) and the PC renderer's pcr_nd700.vert. This part: a draw's record as the shaders read it
// (0x000's layout: the native GS's NdConst, port/render/vu1_record.hpp's Vu1Record, the model's colour in point_col[0])
// and its flags. Include it, declare the buffer of records, then vu1_700_maths.h.
#include "nd_between.h"
// the draws (gsn_native_draw.inc, NdConst), a record each: matrices a VU qword a column
struct Draw {
    mat4 vertex;       // VU1 0x14: the set-up's vertex matrix (0x00 by 0x04)
    mat4 model;        // VU1 0x04: the part to the world (not read yet)
    mat4 view;         // world to view (not read yet)
    mat4 screen;       // view to screen (pictures between: sx, sy, cx, cy, cz)
    mat4 light_dir;    // vf05-vf07: the light directions by the part's rotation, a column each
    mat4 light_col;    // VU1 0x10: the light colours, then the ambient
    vec4 fog;          // VU1 0x3C
    vec4 point_pos[4]; // (0x700 has no point lights)
    vec4 point_col[4]; // [0]: VU1 0x64, the model's colour (vf28)
    vec4 obj;          // (ND_C_OBJ) the part's move from the frame before to this one, in the world
    ivec4 info;        // (0), XYOFFSET X and Y (12.4), ND_C_*
    Between bt;        // (ND_C_MODEL) the part's own move from the frame before (nd_between.h)
    uvec4 wave[4];     // (nd_000.vert's ND_C_WAVE: not read here; the records are NdConst, 0x000's)
};
#define ND_C_MOVING 1
#define ND_C_OBJ 2
#define ND_C_MODEL 4
#define ND_C_EYE 8
