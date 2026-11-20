// A game frame as a scene (docs/plans/scene-renderer-design.md, "The scene"; slice 1, HANDOFF_scene_slice1.md):
// built by the builder (scn_build.h) as the renderer's thread meets the chain, closed at the vblank (or where the GPU's
// result is wanted sooner: a read-back, a picture, a save state), then drawn by the drawer (scn_draw.h). Read-only once
// closed; every image and buffer it names is held by it.
//
// Its steps are in the stream's order: passes (a run of draws into one colour target and its depth) and the ops between
// them (targets made, grown, cleared, copied; uploads written into them; a target's rectangle copied for a draw to read).
// A draw is a native batch (its triangles of the arena's meshes, with its record: port/render/vu1_record.hpp) or the
// GIF's primitives (2D items: their corners in vertices), with its material: the GS's state at its place in the chain,
// resolved (the texture the store answered, the blend, the tests, the shader's constants).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "device.hpp"
#include "gs_drawing.h"
#include "pcr_blend.h"
#include "pcr_prim_layout.h"

namespace scn {

constexpr uint32_t no_clut = 0xFFFFFFFFu;
constexpr uint32_t no_material = 0xFFFFFFFFu;
constexpr uint32_t no_shadow = 0xFFFFFFFFu;

// The draw's vertex shader (pcr_canvas.cpp's v): pcr_prim.vert for the GIF's, pcr_nd000.vert, pcr_nd280.vert,
// pcr_nd700.vert and pcr_ndripple.vert for the native draws'
enum class Program : uint8_t { gif, nd000, nd280, nd700, ripple, count };

// A draw's GS state, resolved where the chain has it
struct Material {
    pcr::PrimPush push{}; // (pcr_common.h's: the flags without the alpha test's pass)
    Program program = Program::gif;
    // The alpha test's passes (pcr_canvas.cpp's draw_batch): pcr_common.h's pass mode, the colour write mask, Z written
    struct AlphaPass {
        uint32_t mode = 0, mask = 0;
        bool zwrite = false;
    };
    std::array<AlphaPass, 2> passes{};
    uint32_t pass_count = 0;
    bool ztest = false;
    VkCompareOp zcompare = VK_COMPARE_OP_ALWAYS;
    pcr::BlendChoice blend;
    float constants[4] = {};
    // Set 1 (pcr_prim.h): the texels (raw RGBA8 or indices: u_texels), a target's copy (u_surface), the colours the
    // GPU's sampler reads (u_sampled, with sampler), the destination's copy (u_dest: DATE by a copy, the shader's blend);
    // null where the draw reads none. clut: the colours an index texture looks up, Scene::cluts' (no_clut: none).
    Vulkan::ImageHandle texels, surface, sampled, dest;
    const Vulkan::Sampler *sampler = nullptr;
    uint32_t clut = no_clut;
    // (a picture's) drawn by scn_picture_layer.frag: a kept layer that is the frame read at its own place, nothing more
    bool picture_layer = false;
    // (slice 3d, program 0x280's first pass with its sphere pass in it: scn_build.cpp's fuse_280) the sphere pass's
    // material, its record the one the fused shaders read after the first's (pcr_prim.h's SCN_FUSE_280); no_material: none
    uint32_t fuse = no_material;
    // (the GPU's time by shader state, KF4_GPU_DETAIL: pcr_gpu_time.h's shader_state_key, without the first-draw field
    // the drawer adds by its pass)
    pcr::ShaderState state{};
};

struct Draw {
    uint32_t material = 0;
    VkViewport viewport{};
    VkRect2D scissor{};
    // A native batch: its program's buffers (pcr_native.h), its triangles and record
    bool native = false;
    pcr::NativeProgram native_program = pcr::NativeProgram::vu1_000;
    Vulkan::BufferHandle arena, records, triangles;
    uint32_t first_triangle = 0, triangle_count = 0, instance = 0;
    // (slice 4) the record its pictures between draw it with (its move: pcr_native.h's NativeBatch::picture_instance);
    // pcr::no_instance: none (drawn as in the frame)
    uint32_t picture_instance = 0xFFFFFFFFu;
    // The GIF's primitives: their corners in Scene::vertices, three a triangle
    uint32_t first_vertex = 0, vertex_count = 0;
    // (the GIF's triangles Z tested: 3D the native draw does not take, program 0x000's triangles cut up where they cross
    // the GS's range, scene-renderer-design.md "What still comes as GIF"; counted as what ends a run)
    bool gif_3d = false;
    // A clear (vkCmdClearAttachments over clear_rect: the colour, the depth or both) rather than a draw
    bool clear = false;
    VkClearRect clear_rect{};
    VkClearAttachment clear_att[2]{};
    uint32_t clear_count = 0;
    // DATE's shadows (scn_first_draw.frag): the number of its first triangle in the frame's shadows; no_shadow: none
    uint32_t shadow = no_shadow;
    // (pictures between, later slices) the draw it is of: RdDraw's kind and identity
    uint32_t kind = 0, id[3] = {};
};

// What a pass's draws are drawn as
enum class PassKind : uint8_t {
    plain,
    first_draw, // DATE's shadows' numbers into the frame's first-draw image (colour), MIN blended, Z tested
    darken,     // ... then the shadows drawn where their triangle came first (pcr_prim.frag's SCN_FIRST_DRAW)
};

struct Pass {
    PassKind kind = PassKind::plain;
    Vulkan::ImageHandle colour, depth; // (either may be none: a clear of the other alone)
    // Load ops: the target cleared as the pass begins (over area), or kept
    bool clear_colour = false, clear_depth = false;
    // The render area: the whole target, or the rectangle its load ops clear (a frame's rows where both frame buffers
    // are one target); every draw of the pass is inside it
    VkRect2D area = {{0, 0}, {UINT32_MAX, UINT32_MAX}};
    VkClearColorValue colour_value{};
    float depth_value = 0.0f;
    uint32_t first_draw = 0, draw_count = 0;
    Vulkan::ImageHandle first; // (darken: the first-draw image its draws read)
    bool picture = false;      // (the picture's: the frame's layers drawn over it, timed as the picture)
};

// What is done outside the passes, between them
struct Op {
    enum class Kind : uint8_t {
        clear,  // dst cleared whole to value (colour or depth)
        copy,   // src's rectangle (src_at, extent) to dst at dst_at, the same size (colour or depth)
        blit,   // src's rectangle (src_at, src_extent) to dst's (dst_at, extent), nearest
    };
    Kind kind = Kind::clear;
    Vulkan::ImageHandle dst, src;
    VkOffset3D dst_at{}, src_at{};
    VkExtent3D extent{}, src_extent{};
    VkClearValue value{};
    bool depth = false; // (the images' aspect: depth, else colour)
    pcr::GpuWork work = pcr::GpuWork::surface; // (for the GPU's time by kind)
};

struct Step {
    enum class Kind : uint8_t { pass, op };
    Kind kind;
    uint32_t index; // (Scene::passes' or ops')
};

struct Scene {
    uint64_t vblank = 0; // (the game's, at which it was closed)
    std::vector<Step> steps;
    std::vector<Pass> passes;
    std::vector<Op> ops;
    std::vector<Material> materials;
    std::vector<Draw> draws;
    std::vector<pcr::Vertex> vertices;
    std::vector<std::array<uint32_t, 256>> cluts;

    bool empty() const {
        return steps.empty();
    }
    void clear() {
        steps.clear();
        passes.clear();
        ops.clear();
        materials.clear();
        draws.clear();
        vertices.clear();
        cluts.clear();
    }
};

} // namespace scn
