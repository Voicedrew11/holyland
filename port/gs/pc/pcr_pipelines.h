// The PC renderer's pipelines made ahead of the draws that need them (pcr_canvas.cpp, docs/port/PC_RENDERER.md).
//
// Between runs: a pipeline is made from the GPU's own code for it, which the driver compiles (0.2 to 1 s each for the
// canvas's programs on this machine), at the first draw that needs it: as a run starts and as a map first draws with a
// state not drawn yet, the picture stands still that long. So (store_open) Granite's Vulkan pipeline cache is kept in the
// data folder (pc_pipelines.bin: read at the start, written again every ten seconds when new pipelines were made, and at
// the end), and with it the list of the canvas's pipelines drawn with (pc_pipeline_list.bin: each named by the canvas's
// program index, its state and its attachments' formats, so that it holds across builds where the shaders
// changed and the cache does not). As the canvas's programs are made (warm_watch), the list's pipelines are made on threads
// of their own, through the cache, so that the draws find them made. Nothing waits for them: a draw that wants one not made
// yet makes it, as without them. Plain Vulkan 1.0 (a VkPipelineCache), no extension: on every GPU the same. The files are
// only read when they are this GPU's, this driver's and this build's layout (else started afresh, silently); the cache
// is checked by its UUID and hash besides (Granite), and is never read past 256 MB. KF4_PC_PIPELINE_CACHE=0: neither file
// read nor written (as before: no cache); KF4_PC_WARM_AHEAD=0: the list's pipelines not made ahead.
//
// Made before play (precompile): the list holds only what this data folder drew, so a first start (or one after a
// driver update, a new GPU or a build whose shaders changed: the cache refused) still compiled at the draws. So the
// repository carries a seed (port/gs/pc/pipeline_seed.bin, made by tools/pipeline_seed.py from tours of every map,
// the menus and the effects, the native draws on and off): the canvas's pipelines as the list keeps them, renderer
// state only. At every start, once the settings are set (gs.c) and the canvas's programs made, the seed's pipelines
// for this device (its depth format and dynamic state put in) and the list's are made on every core but one, through
// the cache, while the game waits at its first vblank ("Preparing shaders... N%" in the window; with the cache good,
// a fraction of a second of cache hits). A pipeline that fails is left: its draw makes it as before. KF4_PC_PRECOMPILE=0: not made before play (the
// list made ahead while the game runs, as before).
//
// (The port drew with MSAA until 2026-11-16. The seed and the lists made before then hold records of multisampled
// passes and of per-sample programs: they are skipped. The canvas's program indices (roles) are kept as they were,
// the multisampled ones' left empty, so that those records still name the programs they were made for.)
#pragma once

#include <cstdint>
#include <span>

#include "device.hpp"

namespace pcr {

// What of the canvas's pipelines depends on the device (the seed's pipelines are made with it: precompile)
struct WarmDevice {
    VkFormat depth_format = VK_FORMAT_UNDEFINED; // (the depth surfaces')
    bool dynamic_ds = false, dynamic_blend = false; // (its dynamic state: Canvas::Impl::choose_dynamic)
    // The canvas's vertex input (pcr_canvas.cpp's set_vertex_attribs, binding 0's stride): put into a pipeline kept
    // from an older build whose vertex shader reads an input the record has none for (the seed's and list's records
    // made before pcr_prim.vert read locations 7-10: their pipelines had no format there, invalid, and matched no draw)
    Vulkan::VertexAttribState attribs[Vulkan::VULKAN_NUM_VERTEX_ATTRIBS] = {};
    uint32_t vertex_stride = 0;
    // (exact Z) A decal's batch drawn again where it won (pcr_canvas.cpp's settle_decal_depth), in pipelines no tour of
    // an older build drew with: the seed's pipelines of the batch programs (roles below decal_roles) that test Z GEQUAL
    // are made with the decal's states too: its passes marking the stencil, the depth reset (role depth_reset_first +
    // role % decal_programs) and its passes again with no colour (role % decal_programs). decal_roles 0: none (no
    // stencil, or no exact Z)
    uint32_t decal_roles = 0, decal_programs = 1, depth_reset_first = 0;
    // (the depth rule, pcr_canvas.cpp's rule_on_) Its programs' first role (0: off): 20 (the batch programs' plain and
    // early, then the runs' plain and early, 5 each; the 20 after them were the multisampled ones'). The seed's
    // records of the batch programs (roles 0-9 and 11-20) that test Z GEQUAL or GREATER (or, rule_always, ALWAYS and
    // write it) are made with the rule's program in their place. zclear_role: the depth copy's clear in a pass
    // (clear_depth_copy), made for every pass with depth; date_mark_role: DATE's set-up marking the copy, made with the
    // state of DATE's own set-up (role 10).
    uint32_t rule_first = 0;
    bool rule_always = false;
    uint32_t zclear_role = 0, date_mark_role = 0;
};

// (any thread) The canvas's programs by role (its index of them: nullptr where a role is no longer drawn), whose
// pipelines are listed and made ahead. The plane pass's fixed compute programs are made before play too.
void warm_watch(Vulkan::Device &device, Vulkan::Program *const *programs, uint32_t count, const WarmDevice &traits,
                std::span<Vulkan::Program *const> compute_programs);

// (any thread, after the settings are set, before the game draws) The pipelines made before play: the seed's (every
// pipeline the canvas draws with in a tour of the game, for these settings), the list's and the plane pass's,
// on every core but one,
// through the cache; precompile_running is true until they are made (the game waits, "Building shaders"). Not with
// KF4_PC_PRECOMPILE=0 (the list made ahead while the game runs, as before) or without the cache.
void precompile();
bool precompile_running();
bool precompile_progress(int *done, int *total);

// (before the device draws) The pipeline cache read from cache_path (and the list beside it), the saver started
void store_open(Vulkan::Device &device, const char *cache_path);

// (at the end, the device still there) The threads stopped, the files written
void store_close();

// --stats: the pipelines kept between runs and made ahead
void warm_print_counts();

} // namespace pcr
