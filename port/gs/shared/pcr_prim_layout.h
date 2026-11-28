// What the GS pixel's shaders are given (port/gs/pc/shaders/pcr_common.h: the two must agree): the push constants, the
// flags, the texture sources and a GIF primitive's corners as pcr_prim.vert reads them. The PC renderer's canvas
// (port/gs/pc/pcr_canvas.cpp) and the scene renderer (port/gs/scene/) draw with them.
#pragma once

#include <cstddef>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "pcr_gif.h"

namespace pcr {

struct PrimPush {
    float inv_size[2];
    float zscale;
    uint32_t zmax;
    uint32_t flags;
    uint32_t tex_size;
    uint32_t clamp_lo, clamp_hi;
    uint32_t test;
    uint32_t fogcol;
    uint32_t texa;
    uint32_t tex1;
    float lod_k;
    uint32_t levels;
    int32_t src_offset[2];
    int32_t src_scale[2];
    int32_t place[2];
    int32_t dest_offset[2];
    uint32_t alpha;
    uint32_t clut_at;
    uint32_t table;
    uint32_t record; // (a run's programs, PCR_RECORDS: its first record in the picture's records, Impl::Runs)
    uint32_t factors; // (a run's draw, PCR_RECORDS: its blend's factors from the shader, dual-source: F_DUAL | ...)
    uint32_t trace_xy;   // (developers' pixel trace) the target's pixel traced: x | y << 16, or no_trace
    uint32_t trace_draw; // ... the draw its fragments are recorded as
    uint32_t merged;     // (a merged run's, PCR_RECORDS: not 0, its triangles from the picture's entries, Impl::Runs)
    uint32_t plane_at = 0xFFFFFFFFu; // (a native draw's, exact Z: its triangle n's plane the planes buffer's n + this,
                                     // n gl_VertexIndex / 3; no_plane none: pcr_exact_z.h's pcr_plane_of_draw)
    uint32_t jitter = 0; // (the frame's 3D with a temporal anti-aliasing method: pcr_jitter.h's jitter_packed)
};
static_assert(sizeof(PrimPush) <= 128, "push constants: 128 bytes at most");
static_assert(sizeof(PrimPush) % 8 == 0, "a record: std430's stride of pcr_common.h's PcrRecord");
constexpr uint32_t no_trace = 0xFFFFFFFFu; // (pcr_common.h's PCR_NO_TRACE)
constexpr uint32_t no_plane = 0xFFFFFFFFu; // (pcr_exact_z.h's PCR_NO_PLANE)
// (tex1) the sampled texture read at the GPU's own level, its levels made on the PC (pcr_common.h's PCR_TEX1_GPU_LEVELS)
constexpr uint32_t tex1_gpu_levels = 0x2000u;

constexpr uint32_t F_TME = 0x1;
constexpr uint32_t F_FST = 0x2;
constexpr uint32_t F_TFX_SHIFT = 2;
constexpr uint32_t F_TCC = 0x10;
constexpr uint32_t F_FGE = 0x20;
constexpr uint32_t F_SRC_SHIFT = 6;
constexpr uint32_t F_PASS_SHIFT = 9;
constexpr uint32_t F_FBA = 0x800;
constexpr uint32_t F_DATE = 0x8000;
constexpr uint32_t F_DATM = 0x10000;
constexpr uint32_t F_SHADER_BLEND = 0x20000;
constexpr uint32_t F_SURFACE_CT16 = 0x40000;
constexpr uint32_t F_SMOOTH_2D = 0x100000;
constexpr uint32_t F_DECAL = 0x400000;
constexpr uint32_t F_DECAL_LOSS = 0x800000; // (developers' decal losses: pcr_common.h)
constexpr uint32_t F_DECAL_LOSS_FAR = 0x1000000;
constexpr uint32_t F_EXACT_Z = 0x2000000; // (pcr_common.h's exact Z)
constexpr uint32_t F_FULL_DEPTH = 0x4000000; // (the depth rule: pcr_common.h)
constexpr uint32_t F_ZGREATER = 0x8000000;
constexpr uint32_t F_ZWRITE = 0x10000000;
constexpr uint32_t F_ZALWAYS = 0x20000000;
constexpr uint32_t F_DATE_MARK = 0x40000000;
constexpr uint32_t F_DATE_CLEARS = 0x200000;
constexpr uint32_t F_SCN_OWN_DEPTH = 0x200000; // (the scene renderer's settled decals' own depth: pcr_common.h; the rule's bit above)
constexpr uint32_t F_SCN_FOG_VOLUME = 0x10000000; // (the scene renderer's volumetric fog: pcr_common.h; F_ZWRITE's bit)
constexpr uint32_t F_LAYER = 0x80000; // (developers' KF4_PC_DEPTH=layers: pcr_common.h)
constexpr uint32_t F_PIXEL_CLAMP = 0x80000000u; // (... and its Z clamped at each pixel: pcr_common.h)
enum : uint32_t { SRC_CT32 = 0, SRC_CT24 = 1, SRC_CT16 = 2, SRC_INDEX = 3, SRC_SURFACE = 4, SRC_SAMPLED = 5 };

// Vertex::kicks[3] (pcr_prim.vert's PCR_K_*): a sprite's corner (0-3), a sprite's, UV for S and T
// (the scene renderer's) a corner of the frame only, or of its pictures between only (pcr_prim.vert's PCR_K_FRAME_ONLY)
constexpr uint32_t K_CORNER = 0x3;
constexpr uint32_t K_SPRITE = 0x4;
constexpr uint32_t K_FST = 0x8;
constexpr uint32_t K_FRAME_ONLY = 0x10;
constexpr uint32_t K_PICTURE_ONLY = 0x20;

// (Vertex::rect: no texels known, the 2D mode's smooth not for its primitive)
constexpr uint32_t no_rect = 0xFFFFFFFFu;

struct Vertex {
    float x, y;
    uint32_t z;
    uint8_t rgba[4];
    float s, t, q;
    float fog;
    // (a picture between's corner, pcr_prim.vert's a_kicks: the kicks it is made of and how, K_* below; no_kick: as
    // it is)
    uint32_t kicks[4] = {no_kick, no_kick, no_kick, 0};
    // (the 2D mode's smooth: the texels its primitive covers across, rect[0], and down, rect[1], each first | last << 16,
    // of a sprite's or a 2D triangle's corners (FST, or one Z and Q); no_rect for the rest: texel_rect)
    uint32_t rect[2] = {no_rect, no_rect};
    // (exact Z, pcr_common.h) a whole Z its triangle's three corners share: the first corner's (link_corners)
    uint32_t zref = 0;
    // (exact Z by the plane, port/gs/pc/shaders/pcr_exact_z.h) its triangle's two other corners (link_corners): their
    // X and Y and Z as this corner's are, and in a picture between what each is made of: a triangle's corner its own
    // kick (kicks[0]), a sprite's its corner (kicks[3]'s K_CORNER bits; the sprite's two kicks are this corner's)
    float other_xy[2][2] = {};
    uint32_t other_z[2] = {};
    uint32_t other_kick[2] = {no_kick, no_kick};
};
static_assert(sizeof(Vertex) == 92, "Vertex: the vertex shader's inputs, packed");

// pcr_prim.vert's inputs, a location each (0-10): Vertex's fields, binding 0
struct VertexInput {
    VkFormat format;
    uint32_t offset;
};
constexpr VertexInput vertex_inputs[] = {
    {VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)},           {VK_FORMAT_R32_UINT, offsetof(Vertex, z)},
    {VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, rgba)},        {VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, s)},
    {VK_FORMAT_R32_SFLOAT, offsetof(Vertex, fog)},            {VK_FORMAT_R32G32B32A32_UINT, offsetof(Vertex, kicks)},
    {VK_FORMAT_R32G32_UINT, offsetof(Vertex, rect)},          {VK_FORMAT_R32_UINT, offsetof(Vertex, zref)},
    {VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, other_xy)}, {VK_FORMAT_R32G32_UINT, offsetof(Vertex, other_z)},
    {VK_FORMAT_R32G32_UINT, offsetof(Vertex, other_kick)},
};
constexpr uint32_t vertex_input_count = sizeof(vertex_inputs) / sizeof(vertex_inputs[0]);

} // namespace pcr
