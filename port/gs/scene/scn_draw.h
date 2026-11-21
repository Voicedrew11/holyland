// The scene renderer's drawing side (scn_scene.h): a closed scene's steps recorded into a command buffer, in order.
// Its programs are the PC renderer's shaders built without exact Z (pcr_prim.vert and .frag, pcr_nd000.vert,
// pcr_nd280.vert, pcr_nd700.vert, pcr_ndripple.vert), with the depth the GPU's: each draw's Z / zmax, clamped at each
// pixel by the depth clamp to the viewport's 0 to 1, into D16_UNORM for the game's 16-bit Z buffers (the GS's whole Z,
// rounded by the GPU) and D32F for the others (docs/plans/scene-renderer-design.md, "Depth's exactness and DATE"). And
// DATE's shadows by the first-draw image: scn_first_draw.frag, then pcr_prim.frag with SCN_FIRST_DRAW.
//
// A draw's fields are a record of the scene's (slice 3, HANDOFF_scene_slice3.md, part 3a): a records buffer made for each
// scene recorded, a record a draw's pass, read at the push constants' place (+ gl_DrawID) by the PC renderer's shaders
// built with PCR_RECORDS and PCR_DRAWS_APART (pcr_common.h: the fields loaded once a draw, in both stages); the textures the scene's draws read in one bindless table (set 2, u_table) with their
// samplers (set 0, u_samplers), the CLUTs in one buffer (u_walk_cluts, the record's clut_at). Draws that read a target or
// its copy (u_surface, u_dest: the HUD's DATE sprite, the shader's blends, the textures drawn by the frame) or a sampler
// past the sixteen keep their own set 1 and push constants, as do the picture's layers (scn_picture_layer.frag). Still a
// call a draw's pass; the runs that 3b draws as indirect draws are counted (Counts). KF4_SCN_RECORDS=0: every draw with
// its own push constants and set 1, as slice 2 (to compare).
// One thread: the builder's.
#pragma once

#include <stdlib.h>

#include <stdio.h>

#include <array>
#include <cstdint>
#include <vector>

#include "device.hpp"
#include "pcr_gpu_time.h"
#include "scn_scene.h"

namespace scn {

class Drawer {
public:
    Drawer(Vulkan::Device &device, pcr::GpuTimes &times);
    Drawer(const Drawer &) = delete;
    Drawer &operator=(const Drawer &) = delete;

    // The scene's steps into cmd, in order: each op between full barriers, each pass a render pass. What follows in cmd
    // sees all of it.
    void record(const Scene &scene, Vulkan::CommandBuffer &cmd);

    // DATE by the first-draw image is possible here: an R32F colour target that blends (MIN)
    bool first_draw_ok() const {
        return first_draw_ok_;
    }
    static constexpr VkFormat first_format = VK_FORMAT_R32_SFLOAT;

    // Vulkan's dynamic state for the draws, as the canvas has it (pcr_canvas.cpp's choose_dynamic): set on every
    // command buffer the scene is recorded into
    bool dynamic_ds() const {
        return dynamic_ds_;
    }
    bool dynamic_blend() const {
        return dynamic_blend_;
    }

    // What ends a run (Counts::ends): a draw that cannot join the call before it in one indirect draw
    enum End : uint32_t {
        end_pass,  // the pass's first call
        end_state, // another pipeline or dynamic state (the program, depth, blend, write mask, constants, viewport,
                   // scissor, a native program's buffers): a material's, or 0x280's passes alternating
        end_own,   // it or the call before keeps its own descriptors (set 1, push constants)
        end_clear, // a clear in the pass between
        end_gif3d, // it or the call before is the GIF's 3D (Draw::gif_3d)
        end_count
    };
    struct Counts {
        uint64_t passes = 0, draws = 0, native_draws = 0, gif_draws = 0, clears = 0, ops = 0;
        uint64_t first_draws = 0, darkened = 0; // (DATE's shadows: drawn into the first-draw image, then darkened)
        // (draws that set their material's state: the others are the one before's)
        uint64_t state_set = 0;
        // (slice 3) the calls (a draw's pass each), of them drawn from the scene's records; the runs of calls one indirect
        // draw could draw (the same pipeline and dynamic state, textures bindless, consecutive in a pass), of them program
        // 0x280's; what ended each run; the runs the GIF's 3D cut in two (the same state either side of it)
        uint64_t calls = 0, record_calls = 0, runs = 0, runs_280 = 0, gif3d_splits = 0;
        std::array<uint64_t, end_count> ends{};
        // (the scenes' records, table entries and CLUTs, and the scenes recorded)
        uint64_t records = 0, table_entries = 0, cluts = 0, scenes = 0, table_max = 0;
        uint64_t own_sampler = 0; // (materials kept to their own set 1: no room for their sampler)
    };
    const Counts &counts() const {
        return counts_;
    }

private:
    static constexpr uint32_t no_record = 0xFFFFFFFFu;
    // A scene's records, table and CLUTs (prepare), held until the next scene is recorded (Granite lets go of them once
    // the GPU is done with the frame)
    struct Prepared {
        Vulkan::BufferHandle records, cluts;
        Vulkan::BindlessDescriptorPoolHandle table_pool;
        Vulkan::BindlessDescriptorSet table{};
        std::array<const Vulkan::Sampler *, 16> samplers{};
        std::vector<uint32_t> first_record; // (by draw: its first pass's record; no_record: its own push constants and set 1)
    };
    // A call as a run sees it: what must be the same for two calls to be one indirect draw
    struct RunKey {
        Vulkan::Program *program = nullptr;
        bool ztest = false, zwrite = false, blend = false;
        VkCompareOp zcompare = VK_COMPARE_OP_ALWAYS;
        VkBlendFactor src = VK_BLEND_FACTOR_ONE, src_alpha = VK_BLEND_FACTOR_ONE, dst = VK_BLEND_FACTOR_ZERO;
        VkBlendOp op = VK_BLEND_OP_ADD;
        uint32_t mask = 0;
        float constants[4] = {};
        VkViewport viewport{};
        VkRect2D scissor{};
        const void *native[3] = {};
        bool own = false, gif_3d = false, n280 = false;
    };
    struct RunState {
        bool open = false, after_clear = false;
        RunKey last;
        // (a stretch of the GIF's 3D: the key of the call before it, to see whether it cut a run in two)
        bool in_gif3d = false, before_gif3d_valid = false;
        RunKey before_gif3d;
    };
    void prepare(const Scene &scene);
    void count_call(const RunKey &key);
    void log_scene(const Scene &scene, const Counts &before);
    void record_op(const Op &op, Vulkan::CommandBuffer &c);
    void record_pass(const Scene &scene, const Pass &pass, Vulkan::CommandBuffer &c);
    // set: the material whose state the command buffer has from the draw before in the pass (no_material: none)
    void record_draw(const Scene &scene, const Pass &pass, uint32_t index, Vulkan::CommandBuffer &c, uint32_t set);
    void bind_records(const Pass &pass, const Draw &d, Vulkan::CommandBuffer &c);
    void bind_own(const Scene &scene, const Pass &pass, const Draw &d, const Material &m, Vulkan::CommandBuffer &c);
    void push_record(Vulkan::CommandBuffer &c, uint32_t record);
    void full_barrier(Vulkan::CommandBuffer &c);
    void pass_to_pass_barrier(Vulkan::CommandBuffer &c);
    void make_programs();

    Vulkan::Device &device_;
    pcr::GpuTimes &times_;
    bool dynamic_ds_ = false, dynamic_blend_ = false, first_draw_ok_ = false;
    const bool time_clears_ = getenv("KF4_SCN_TIME_CLEARS") && atoi(getenv("KF4_SCN_TIME_CLEARS")) != 0;
    // by Program: the plain fragment shader, the one tested before it (pcr_prim_early.frag), the first-draw image's
    // (scn_first_draw.frag) and the shadows' darkening (pcr_prim.frag with SCN_FIRST_DRAW)
    std::array<Vulkan::Program *, size_t(Program::count)> plain_{}, early_{}, first_id_{}, first_{};
    // (the same reading the draw's fields from the scene's records: PCR_RECORDS)
    std::array<Vulkan::Program *, size_t(Program::count)> plain_rec_{}, early_rec_{}, first_id_rec_{}, first_rec_{};
    const bool records_on_ = !getenv("KF4_SCN_RECORDS") || atoi(getenv("KF4_SCN_RECORDS")) != 0;
    Prepared prepared_;
    RunState run_;
    FILE *runs_log_ = nullptr; // (developers', KF4_SCN_RUNS_LOG=FILE: a line a scene recorded, its calls and runs)
    Vulkan::Program *picture_layer_ = nullptr; // (pcr_prim.vert with scn_picture_layer.frag)
    Vulkan::ImageHandle dummy_uint_, dummy_float_;
    Vulkan::BufferHandle zeros_, no_clut_, no_trace_;
    Vulkan::BindlessDescriptorPoolHandle table_pool_;
    Vulkan::BindlessDescriptorSet table_{};
    Counts counts_;
};

} // namespace scn
