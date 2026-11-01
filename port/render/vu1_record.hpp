// A native draw's record for VU1's programs 0x000, 0x280, 0x5A0 and 0x700 on the GPU (port/include/port/render.h's draws): what VU1's set-up
// leaves in its memory for the vertex program, worked out on the CPU in VU1's arithmetic, laid out as the shaders read
// it (port/render/shaders/vu1_000_draw.h's Draw). For the renderers that draw native draws without VU1: the PC
// renderer (port/gs/pc/pcr_native.cpp). The native GS has its own copy (gsn_native_draw.inc, nd_const), the same
// maths. port/platform/vu1_hle.c (setup) is the specification.
#pragma once
#include <stdint.h>

#include <vector>

#include "render_internal.hpp"

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

// The draw's record for program 0x5A0 (the shadows: port/platform/vu1_hle_5a0.c), drawn by 0x000's maths from the
// silhouette meshes (render.cpp, take_batch's silhouette): 0x000's set-up of the vertex matrix, with no lights, no
// point lights and no fog, so that the colour and fog come out 0 as 0x5A0 makes them
Vu1Record vu1_record_5a0(const RdDraw &draw, const RdCamera &camera);

// vu1_280_draw.h's Draw (program 0x280, the skinned models): two matrix sets, A the part's (VU1 0x04) and B its
// parent's (0x08), each vertex taking one by its ST's w
struct Vu1Record280 {
    float vertex[2][16];    // VU1 0x14-0x17, 0x1C-0x1F
    float light_dir[2][16]; // VU1 0x18-0x1A, 0x20-0x22
    float light_col[16];    // the light colours, then the ambient
    float sphere[2][16];    // the sphere pass's turns (RdDraw.turn)
    float fog[4];           // VU1 0x3C
    float scale[4], bias[4]; // the colour's scale and bias (RdDraw.colour[0], [1])
    float point_pos[8][4];  // set A's space (VU1 0x4A-0x4D), set B's (0x53-0x56); w the falloff
    float point_col[4][4];
    int32_t info[4];        // the point lights; VU1_MOVE_* flags (info[1])
    int32_t offset[4];      // XYOFFSET_1's X and Y, XYOFFSET_2's (12.4): the renderer's
    float screen[16];       // view to screen
    Vu1Between bt[2];       // (VU1_MOVE_MODEL) set A's and set B's moves from the frame before
};
static_assert(sizeof(Vu1Record280) == 71 * 16, "Vu1Record280 is the shaders' Draw");

// The draw's record for program 0x280: VU1's set-up for each matrix set (vu1_record_000's maths), the light colours,
// fog, sphere matrices, colour scale and bias as they are. XYOFFSET and the moves are left 0: the renderer's.
Vu1Record280 vu1_record_280(const RdDraw &draw, const RdCamera &camera);

// A batch of program 0x280 as VU1 runs it (vu1_hle_280.c's run): which of its kicks get ADC (VU1's clipping of the kick
// and the two vertices before it, and the tag's; not the data's, which the batch's triangles leave out already), and
// whether the run ends without a strip (its count of clipped vertices reached n: no strip, no second pass)
constexpr uint32_t VU1_BATCH_MOST = 40;
struct Vu1Kicks280 {
    uint32_t n = 0;
    bool adc[VU1_BATCH_MOST] = {};
    bool all_clipped = false;
};
// vertices: the batch's first vertex's eight qwords in its mesh. False when the batch has more vertices than a run.
bool vu1_kicks_280(const int32_t *vertices, const RdBatch &batch, const Vu1Record280 &record, Vu1Kicks280 &out);

// The draw's record for program 0x700 (the effects: port/platform/vu1_hle_700.c): 0x000's set-up of the vertex matrix
// and the light directions, no point lights, the model's colour (VU1 0x64) in point_col[0]
Vu1Record vu1_record_700(const RdDraw &draw, const RdCamera &camera);

// The A+D writes a run of program 0x700 kicks (register, value ...): the batch's, with its TEX0 (TOP+0xA2) replaced by
// VU1 0x66 (RdDraw.colour[2]) when its TBP0 is 0x65.w's (colour[1]): the flipbook's frame
std::vector<uint64_t> vu1_ad_700(const RdBatch &batch, const RdDraw &draw);

// A batch of program 0x700 as VU1 runs it: which kicks get ADC (VU1's clipping of the kick and the two before it), and
// whether the strip is kicked at all (fewer than n of its n vertices and the one past it, RdBatch::past, clipped)
struct Vu1Kicks700 {
    uint32_t n = 0;
    bool adc[VU1_BATCH_MOST] = {};
    bool kick = false;
};
// vertices: the batch's first vertex's four qwords in its mesh. False when the batch has more vertices than a run.
bool vu1_kicks_700(const int32_t *vertices, const RdBatch &batch, const Vu1Record &record, Vu1Kicks700 &out);
