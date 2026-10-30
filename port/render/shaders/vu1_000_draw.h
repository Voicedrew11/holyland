// VU1's program 0x000 on the GPU, shared by the renderers that draw native draws (port/include/port/render.h): the
// native GS's nd_000.vert (and its builds: sa_000, pc_000) and the PC renderer's pcr_nd000.vert. This part: a draw's
// record as the shaders read it (the native GS's NdConst, the PC renderer's pcr_nd.h: a qword a column), and its
// flags. Include it, declare the buffer of records, then vu1_000_maths.h.
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
    vec4 point_pos[4]; // in the model's space, w the falloff
    vec4 point_col[4];
    vec4 obj;          // (ND_C_OBJ) the part's move from the frame before to this one, in the world
    ivec4 info;        // the point lights, XYOFFSET X and Y (12.4), ND_C_*
    Between bt;        // (ND_C_MODEL) the part's own move from the frame before (nd_between.h)
    uvec4 wave[4];     // (ND_C_WAVE) the 16 waves' offsets to S (low 16 bits) and T (high)
};
#define ND_C_MOVING 1
#define ND_C_OBJ 2
#define ND_C_MODEL 4
#define ND_C_EYE 8
#define ND_C_WAVE 16
#define ND_C_FAR 32
