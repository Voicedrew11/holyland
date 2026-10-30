// VU1's program 0x280 (the skinned models) on the GPU, shared by the renderers that draw native draws: the native GS's
// nd_280.vert (and its builds) and the PC renderer's pcr_nd280.vert. This part: a draw's record as the shaders read it
// (the native GS's Nd280Const, port/render/vu1_record.hpp's Vu1Record280) and its flags. Include it, declare the buffer
// of records, then vu1_280_maths.h.
#include "nd_between.h"
// the draws (gsn_nd_280.inc, Nd280Const): matrices a VU qword a column; [0] set A, [1] set B
struct Draw {
    mat4 vertex[2];     // VU1 0x14-0x17, 0x1C-0x1F: view-screen by the part's matrix, by its parent's
    mat4 light_dir[2];  // VU1 0x18-0x1A, 0x20-0x22: the light directions by them (a column each)
    mat4 light_col;     // vf08-vf11: the light colours, then the ambient
    mat4 sphere[2];     // VU1 0x2C-0x2E, 0x30-0x32: the turns for the sphere pass
    vec4 fog;           // VU1 0x3C
    vec4 scale;         // VU1 0x72 (0x64): the colour scale
    vec4 bias;          // VU1 0x73 (0x65): and bias
    vec4 point_pos[8];  // VU1 0x4A-0x4D (set A's space; w the falloff), 0x53-0x56 (set B's)
    vec4 point_col[4];  // VU1 0x4E-0x51
    ivec4 info;         // the point lights, ND_C_* flags
    ivec4 offset;       // XYOFFSET_1's X and Y, XYOFFSET_2's (12.4)
    mat4 screen;        // view to screen (pictures between: sx, sy, cx, cy, cz)
    Between bt[2];      // (ND_C_MODEL) set A's and set B's moves from the frame before
};

#define ND_C_MOVING 1
#define ND_C_MODEL 4
#define ND_C_EYE 8
