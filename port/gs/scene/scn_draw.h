// The scene renderer's drawing side (scn_scene.h): a closed scene's steps recorded into a command buffer, in order.
// Its programs are the PC renderer's shaders (pcr_prim.vert and .frag, pcr_nd000.vert, pcr_nd280.vert, pcr_nd700.vert,
// pcr_ndripple.vert) built with exact Z (PCR_EXACT_Z, where the GPU has depth clamp and 64-bit integers: exact_z): each
// pixel's whole Z as the GS works it out, from the triangle's plane in integers where its corners' Z span far (near the
// eye, scn_corner's SCN_PLANE_SPAN) and else from the interpolated Z, with a fine part below it, written by the fragment
// stage into D32F with a stencil; decals tested with the top fine part and settled (settle_run, own_depth_first).
// Without exact Z (KF4_SCN_DEPTH=gpu) the depth the GPU's: each draw's Z / zmax, clamped at each pixel by the depth
// clamp to the viewport's 0 to 1, into D16_UNORM for the 16-bit Z buffers (docs/plans/scene-renderer-design.md,
// "Depth's exactness and DATE": the moss on map 063's low wall cut in slashes, the rope ladder's planks in bands at
// map 000, 2026-11-21). And DATE's shadows by the first-draw image: scn_first_draw.frag, then pcr_prim.frag with
// SCN_FIRST_DRAW.
//
// A draw's fields are a record of the scene's (slice 3, HANDOFF_scene_slice3.md, part 3a): a records buffer made for each
// scene recorded, a record a draw's pass, read at the push constants' place (+ gl_DrawID) by the PC renderer's shaders
// built with PCR_RECORDS and PCR_DRAWS_APART (pcr_common.h: the fields loaded once a draw, in both stages); the textures the scene's draws read in one bindless table (set 2, u_table) with their
// samplers (set 0, u_samplers), the CLUTs in one buffer (u_walk_cluts, the record's clut_at). Draws that read a target or
// its copy (u_surface, u_dest: the HUD's DATE sprite, the shader's blends, the textures drawn by the frame) or a sampler
// past the sixteen keep their own set 1 and push constants, as do the picture's layers (scn_picture_layer.frag).
// KF4_SCN_RECORDS=0: every draw with its own push constants and set 1, as slice 2 (to compare).
// Runs (part 3b): consecutive calls from the records with the same pipeline and dynamic state and consecutive records
// are one vkCmdDrawIndirect (Pending), a command a call in the scene's commands buffer, the push constants' record the
// run's first (gl_DrawID the rest; the fragment stage has the record's place from the vertex stage, pcr_common.h); a run
// of one call a vkCmdDraw. The GIF's primitives drawn from the records read the scene's vertices from one buffer.
// KF4_SCN_INDIRECT=0: every call its own vkCmdDraw, the same shaders (to compare).
// One thread: the builder's.
#pragma once

#include <stdlib.h>

#include <stdio.h>

#include <array>
#include <span>
#include <unordered_map>
#include <cstdint>
#include <vector>

#include "device.hpp"
#include "gs_drawing.h"
#include "pcr_gpu_time.h"
#include "scn_scene.h"

namespace scn {

class Drawer {
public:
    Drawer(Vulkan::Device &device, pcr::GpuTimes &times);
    Drawer(const Drawer &) = delete;
    Drawer &operator=(const Drawer &) = delete;

    // (slice 4) A picture between drawn from a kept scene: its buffers bound where the shaders read a picture's (set 0's
    // bindings 0 to 2: pcr_between.h's), each native draw by the record its pictures draw it with
    // (Draw::picture_instance), and every image in stand_ins drawn into or read in its stand-in's place (the images the
    // scene writes: the picture never draws into the GS's memory)
    // (the volumetric fog, Scene::fog: by the picture's camera, at its time, t of the way from the frame before's to the
    // frame's)
    struct PictureDraw {
        pcr::Drawing::PictureBuffers buffers;
        const std::unordered_map<const Vulkan::Image *, Vulkan::ImageHandle> *stand_ins = nullptr;
        FogCamera camera;
        float t = 1.0f;
    };

    // The scene's steps into cmd, in order: each op between full barriers, each pass a render pass. What follows in cmd
    // sees all of it. picture: as a picture between's (above), else the scene's own. left_out (by step, non-zero: not
    // recorded, nor its draws' vertices placed): the steps another record draws, or none does (slice 4b).
    // substitutes: images drawn into or read in another's place (as a picture's stand-ins, the scene's own records
    // otherwise): a held frame's steps drawn late (slice 4b, scn_build.cpp's HeldFrame)
    using Substitutes = std::unordered_map<const Vulkan::Image *, Vulkan::ImageHandle>;
    void record(const Scene &scene, Vulkan::CommandBuffer &cmd, const PictureDraw *picture = nullptr,
                const std::vector<uint8_t> *left_out = nullptr, const Substitutes *substitutes = nullptr);

    // Every pipeline record() can draw with (for pcr::warm_drawing), worked out from the drawer's programs and the
    // states its calls can have (with the device's dynamic state, the program and the pass's formats alone), into
    // passes of these formats: colour_formats the targets' (Drawer::first_format: the first-draw passes', drawn by
    // the first-draw programs alone), depth_formats the depth surfaces'. Each as the draw looks it up (set by the same
    // setters on a command buffer of the caller's thread, submitted empty). Plus the compute programs it dispatches.
    std::vector<Vulkan::DeferredPipelineCompile> pipeline_set(std::span<const VkFormat> colour_formats,
                                                              std::span<const VkFormat> depth_formats);
    std::vector<Vulkan::Program *> compute_programs() const;

    // Exact Z (pcr_common.h's): the GS's whole Z at each pixel from the triangle's plane, with its fine part, written by
    // the fragment stage into D32F (the draws' flags PCR_F_EXACT_Z, decals PCR_F_DECAL); else the GPU's depth
    bool exact_z() const {
        return exact_z_;
    }
    // (exact Z) its decals' depth settled after them (Material::settle; settle_run, or own_depth_after where nearer),
    // else left with the top fine part they were tested with (port/scene_depth.h's PORT_SCENE_DEPTH_UNSETTLED)
    bool settle_decals() const {
        return settle_decals_;
    }
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
    // the draws' fields from the scene's records (KF4_SCN_RECORDS, and the device's features): slice 3d's fusion needs them
    bool records_on() const {
        return records_on_;
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
        // (3b) the draw calls made (vkCmdDraw and vkCmdDrawIndirect, clears not counted), of them indirect and the
        // commands in those; the draw calls by program (Pending::slot: the GIF's, 0x000, 0x280, 0x700, the ripple's)
        uint64_t draw_calls = 0, indirect_calls = 0, indirect_draws = 0;
        std::array<uint64_t, 5> draw_calls_by{};
        // (the scenes' records, table entries and CLUTs, and the scenes recorded)
        uint64_t records = 0, table_entries = 0, cluts = 0, scenes = 0, table_max = 0;
        uint64_t own_sampler = 0; // (materials kept to their own set 1: no room for their sampler)
        // (slice 3d) fused first passes drawn with their own set 1 for want of their sphere's place in the records: drawn
        // without the sphere pass (none met; counted)
        uint64_t fused_unrecorded = 0;
        uint64_t picture_scenes = 0; // (slice 4: the scenes recorded for pictures between)
        // (each vertex once) the jobs placed, the vertices in them, the draws that share a job with one before
        uint64_t place_jobs = 0, placed_vertices = 0, place_shared = 0, place_dispatches = 0;
        uint64_t settled_runs = 0; // (exact Z: the runs of decals settled, settle_run)
    };
    const Counts &counts() const {
        return counts_;
    }

private:
    static constexpr uint32_t no_record = 0xFFFFFFFFu;
    static constexpr uint8_t decal_won = 0x2; // (the stencil's bit of a settled decal: pcr_canvas.cpp's)
    // A scene's records, table and CLUTs (prepare), held until the next scene is recorded (Granite lets go of them once
    // the GPU is done with the frame)
    struct Prepared {
        Vulkan::BufferHandle records, cluts;
        Vulkan::BindlessDescriptorPoolHandle table_pool;
        Vulkan::BindlessDescriptorSet table{};
        std::array<const Vulkan::Sampler *, 16> samplers{};
        std::vector<uint32_t> first_record; // (by draw: its first pass's record; no_record: its own push constants and set 1)
        // (3b) the runs' commands, written as the calls are recorded (mapped until the scene is), and the GIF's
        // primitives' corners (Scene::vertices) for the draws from the records
        Vulkan::BufferHandle commands, vertices;
        VkDrawIndirectCommand *command_map = nullptr;
        uint32_t command_capacity = 0, commands_used = 0;
        // (each vertex once: place_vertices) by draw, its job (no_record: none, drawn as the PC renderer's shader
        // draws it); the jobs (scn_placed.h) and the placed vertices; the dispatches, a program's buffers each
        std::vector<uint32_t> job_of;
        Vulkan::BufferHandle jobs, placed;
        struct Placing {
            uint32_t slot = 0; // (native_slot: 0x000's 0, 0x280's 1)
            const Vulkan::Buffer *arena = nullptr, *records = nullptr;
            uint32_t first_job = 0, count = 0;
        };
        std::vector<Placing> placings;
        uint32_t own_offset = 0; // (exact Z's settled decals: record r's own depth at r + own_offset; 0: none)
    };
    // A call as a run sees it: what must be the same for two calls to be one indirect draw
    struct RunKey {
        Vulkan::Program *program = nullptr;
        bool ztest = false, zwrite = false, blend = false;
        VkCompareOp zcompare = VK_COMPARE_OP_ALWAYS;
        VkBlendFactor src = VK_BLEND_FACTOR_ONE, src_alpha = VK_BLEND_FACTOR_ONE, dst = VK_BLEND_FACTOR_ZERO,
                      dst_alpha = VK_BLEND_FACTOR_ZERO;
        VkBlendOp op = VK_BLEND_OP_ADD, op_alpha = VK_BLEND_OP_ADD;
        uint32_t mask = 0;
        float constants[4] = {};
        VkViewport viewport{};
        VkRect2D scissor{};
        const void *native[3] = {};
        bool own = false, gif_3d = false, n280 = false;
        // (an unblended call whose alpha out is 1.0 and not written: drawn the same in a run blended by it, merge)
        bool alpha_one = false;
        // (exact Z's decals settled, record_settled) the stencil: none, decal_won set where the call passes, or drawn
        // only where it is set and cleared there
        // (own_first: no stencil; program 0x280's decals' own depth drawn before the run, which writes none: settle_run)
        // (own_after, PORT_SCENE_DEPTH_NEARER: no stencil; the run writes no depth, then its own depth where nearer than
        // what is there: own_depth_after)
        enum class Stencil : uint8_t { off, mark_won, reset_won, own_first, own_after } stencil = Stencil::off;
    };
    struct RunState {
        bool open = false, after_clear = false;
        RunKey last, call; // (the run's state, merge's; the last call's own)
        // (a stretch of the GIF's 3D: the key of the call before it, to see whether it cut a run in two)
        bool in_gif3d = false, before_gif3d_valid = false;
        RunKey before_gif3d;
    };
    // The run being recorded (3b): its state is the command buffer's, its calls' commands from first_command in
    // Prepared::commands (the first also in first), not yet drawn
    struct Pending {
        bool open = false;
        RunKey key;
        uint32_t first_record = 0, first_command = 0, count = 0, slot = 0;
        VkDrawIndirectCommand first{};
    };
    static bool same_state(const RunKey &a, const RunKey &b);
    static void canonical(RunKey &k);
    static bool merge(RunKey &run, const RunKey &key);
    static bool alpha_one(const Material &m, uint32_t pass);
    void prepare(const Scene &scene);
    void count_call(const RunKey &key);
    // Whether a call of key from record joins the open run; a call into it (a run opened if none is: its state and
    // push constants already set); the open run drawn
    bool joins(const RunKey &key, uint32_t record) const;
    void add_to_run(const RunKey &key, uint32_t record, const VkDrawIndirectCommand &command, uint32_t slot,
                    Vulkan::CommandBuffer &c);
    void flush_run(Vulkan::CommandBuffer &c);
    // The state a draw's calls share (viewport, scissor, the GPU time's interval, bindings, depth compare, blend
    // constants, the GIF's corners) and a call's own (program, depth, blend, write mask), from its key
    void set_draw_state(const Scene &scene, const Pass &pass, const Draw &d, const Material &m, bool records,
                        Vulkan::CommandBuffer &c);
    void set_call_state(const RunKey &key, Vulkan::CommandBuffer &c);
    // The pipeline's part of the state a draw's calls share (set_draw_state's, pipeline_set's): fixed state, the depth
    // compare and the blend constants; and the GIF's vertex input (its corners in vertices)
    static void set_shared_pipeline_state(VkCompareOp zcompare, const float constants[4], Vulkan::CommandBuffer &c);
    static void set_vertex_input(const Vulkan::Buffer &vertices, Vulkan::CommandBuffer &c);
    // (pipeline_set) every call key a pass's calls can have, as record_draw makes them, by what the device's dynamic
    // state leaves in a pipeline: the first-draw passes' (first_draw) or the others'
    std::vector<RunKey> call_keys(bool first_draw) const;
    void log_scene(const Scene &scene, const Counts &before);
    void record_op(const Op &op, Vulkan::CommandBuffer &c);
    void record_pass(const Scene &scene, const Pass &pass, Vulkan::CommandBuffer &c);
    // set: the material whose state the command buffer has from the draw before in the pass (no_material: none)
    void record_draw(const Scene &scene, const Pass &pass, uint32_t index, Vulkan::CommandBuffer &c, uint32_t set);
    // (exact Z) Whether a draw's decal is settled (Material::settle, drawn from the records in a plain pass): its calls
    // that write Z mark the stencil's decal_won (RunKey::Stencil::mark_won), and the runs of them are settled
    // (settle_run, after the run is drawn)
    bool settles(const Pass &pass, const Material &m, uint32_t record) const;
    void settle_run(const Pending &r, Vulkan::CommandBuffer &c);
    void own_depth_first(const Pending &r, Vulkan::CommandBuffer &c);
    void own_depth_after(const Pending &r, Vulkan::CommandBuffer &c);
    // (own_depth_first's and own_depth_after's) the run's calls again with no colour and their own depth, by compare
    void own_depth(const Pending &r, VkCompareOp compare, Vulkan::CommandBuffer &c);
    void draw_run(const Pending &r, Vulkan::CommandBuffer &c); // (the run's calls: one draw, or one indirect draw)
    void bind_records(const Pass &pass, const Draw &d, Vulkan::CommandBuffer &c);
    void bind_own(const Scene &scene, const Pass &pass, const Draw &d, const Material &m, Vulkan::CommandBuffer &c);
    // (the volumetric fog, pcr_prim.h's SCN_FOG_VOLUME) the scene's fog for the picture recorded (record's), and its
    // uniform and noise bound at set 1's bindings 13 and 14, as every draw's (the defaults where the scene has none)
    void prepare_fog(const Scene &scene);
    void bind_fog(Vulkan::CommandBuffer &c);
    void make_fog_noise();
    void push_record(Vulkan::CommandBuffer &c, uint32_t record);
    void full_barrier(Vulkan::CommandBuffer &c);
    void pass_to_pass_barrier(Vulkan::CommandBuffer &c);
    void make_programs();
    // (a picture between's, record's picture) an image's stand-in, or itself; the picture's buffer at set 0's binding;
    // the record a native draw is drawn with
    const Vulkan::Image &image(const Vulkan::ImageHandle &h) const;
    const Vulkan::Buffer *picture_buffer(uint32_t binding) const;
    uint32_t instance_of(const Draw &d) const;
    // (each vertex once, scn_placed.h) the scene's jobs (prepare) and their placing, recorded before its passes; a
    // native draw's firstInstance (its job's number, or its instance)
    void prepare_placing(const Scene &scene);
    void place_vertices(Vulkan::CommandBuffer &c);
    uint32_t first_instance(uint32_t index, const Draw &d) const;
    // KF4_SCN_PLACED=0: 0x000's and 0x280's draws as the PC renderer's shaders draw them, each corner its maths whole
    const bool placed_on_ = !getenv("KF4_SCN_PLACED") || atoi(getenv("KF4_SCN_PLACED")) != 0;
    std::array<Vulkan::Program *, 2> place_{};
    const PictureDraw *picture_ = nullptr;
    const std::vector<uint8_t> *left_out_ = nullptr; // (record's)
    const Substitutes *substitutes_ = nullptr;       // (record's)
    // (developers', slice 3f's bounds: docs/plans/scene-slice4-results.md) KF4_SCN_FLAT=1: the plain passes' draws by a
    // fragment shader of one colour (scn_flat.frag); KF4_SCN_TINY=1: their scissor one pixel (the vertex stage and the
    // draws' own cost left). The pictures are wrong.
    const bool flat_on_ = getenv("KF4_SCN_FLAT") && atoi(getenv("KF4_SCN_FLAT")) != 0;
    const bool tiny_on_ = getenv("KF4_SCN_TINY") && atoi(getenv("KF4_SCN_TINY")) != 0;
    // KF4_SCN_THIRD=1: each native draw with a third of its triangles (its vertex stage's work as if each mesh vertex's
    // maths were made once, not once a triangle corner: the bound of making it once)
    const bool third_on_ = getenv("KF4_SCN_THIRD") && atoi(getenv("KF4_SCN_THIRD")) != 0;
    uint32_t native_corners(const Draw &d) const {
        return (third_on_ ? (d.triangle_count + 2) / 3 : d.triangle_count) * 3;
    }
    std::array<Vulkan::Program *, size_t(Program::count)> flat_{}, flat_rec_{};

    Vulkan::Device &device_;
    pcr::GpuTimes &times_;
    bool dynamic_ds_ = false, dynamic_blend_ = false, first_draw_ok_ = false, exact_z_ = false;
    // (PORT_SCENE_DEPTH_NEARER: the decals other than 0x280's settled by own_depth_after, not settle_run)
    bool settle_decals_ = false, settle_nearer_ = false;
    const bool time_clears_ = getenv("KF4_SCN_TIME_CLEARS") && atoi(getenv("KF4_SCN_TIME_CLEARS")) != 0;
    // by Program: the plain fragment shader, the one tested before it (pcr_prim_early.frag), the first-draw image's
    // (scn_first_draw.frag) and the shadows' darkening (pcr_prim.frag with SCN_FIRST_DRAW)
    std::array<Vulkan::Program *, size_t(Program::count)> plain_{}, early_{}, first_id_{}, first_{};
    // (the same reading the draw's fields from the scene's records: PCR_RECORDS)
    std::array<Vulkan::Program *, size_t(Program::count)> plain_rec_{}, early_rec_{}, first_id_rec_{}, first_rec_{};
    bool records_on_ = !getenv("KF4_SCN_RECORDS") || atoi(getenv("KF4_SCN_RECORDS")) != 0;
    // (3b: indirect draws possible here and not switched off, KF4_SCN_INDIRECT=0; the most commands one may have)
    bool indirect_on_ = !getenv("KF4_SCN_INDIRECT") || atoi(getenv("KF4_SCN_INDIRECT")) != 0;
    uint32_t max_run_ = 1;
    // (developers', KF4_SCN_BLEND_RUNS=0: an unblended call never drawn in a run blended by the source's alpha, merge)
    const bool blend_runs_ = !getenv("KF4_SCN_BLEND_RUNS") || atoi(getenv("KF4_SCN_BLEND_RUNS")) != 0;
    Prepared prepared_;
    RunState run_;
    Pending pending_;
    FILE *runs_log_ = nullptr; // (developers', KF4_SCN_RUNS_LOG=FILE: a line a scene recorded, its calls and runs)
    Vulkan::Program *picture_layer_ = nullptr; // (pcr_prim.vert with scn_picture_layer.frag)
    Vulkan::ImageHandle fog_noise_;            // (pcr_prim.h's u_scn_noise: make_fog_noise)
    Vulkan::BufferHandle fog_uniform_;         // (its ScnFog, prepare_fog's: the scene's recorded last)
    Vulkan::Program *fused_280_ = nullptr;
    // (exact Z's settle: each records vertex shader with pcr_depth_reset.frag)
    std::array<Vulkan::Program *, size_t(Program::count)> depth_reset_rec_{};
    // (settle_run's viewport: the depth of the largest Z of the 16-bit Z buffers, the settled decals', Z * 2^-24)
    static constexpr float own_depth_most_ = 65535.0f / 16777216.0f; // (slice 3d: pcr_nd280.vert and pcr_prim_early.frag with SCN_FUSE_280)
    Vulkan::ImageHandle dummy_uint_, dummy_float_;
    Vulkan::BufferHandle zeros_, no_clut_, no_trace_;
    Vulkan::BindlessDescriptorPoolHandle table_pool_;
    Vulkan::BindlessDescriptorSet table_{};
    Counts counts_;
};

} // namespace scn
