// The PC renderer's pipelines made ahead when MSAA's samples change (pcr_canvas.cpp, docs/port/PC_RENDERER.md).
//
// A Vulkan pipeline is made for a render pass's sample count, so a change of MSAA needs every pipeline the canvas
// draws with made again; made by the draw that first needs it, the canvas's thread waits (0.2 to 1.5 s each on this
// machine) and the picture stands still. So every graphics pipeline made for one of the canvas's programs is kept (what
// Granite made it from: cmake/granite_pipeline_warm.inc's hook), with the attachments of its render pass; when the
// samples change, each is made again at the new count on a thread of its own (with its program's variants that draw
// only with MSAA: the cut-outs shaded at every sample, DATE's set-up a sample at a time, the expand pass), and the
// canvas draws at the new count only once they are made (Canvas::Impl::apply_samples).
//
// Only what was drawn before is known: the first draws at a count never drawn at (a run started with MSAA) make their
// pipelines as before.
//
// Between runs: a pipeline is made from the GPU's own code for it, which the driver compiles (0.2 to 1 s each for the
// canvas's programs on this machine), at the first draw that needs it: as a run starts and as a map first draws with a
// state not drawn yet, the picture stands still that long. So (store_open) Granite's Vulkan pipeline cache is kept in the
// data folder (pc_pipelines.bin: read at the start, written again every ten seconds when new pipelines were made, and at
// the end), and with it the list of the canvas's pipelines drawn with (pc_pipeline_list.bin: each named by the canvas's
// program index, its state and its attachments' formats and samples, so that it holds across builds where the shaders
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
// the menus and the effects, at 1x and with MSAA, the native draws on and off): the canvas's pipelines as the list
// keeps them, renderer state only. At every start, once the settings are set (gs.c) and the canvas's programs made,
// the seed's pipelines for those settings (MSAA's samples, colour high's format, the device's depth format and
// dynamic state put in) and the list's are made on every core but one, through the cache, while the game waits at
// its first vblank ("Preparing shaders... N%" in the window; with the cache good, a fraction of a second of cache
// hits). A pipeline that fails is left: its draw makes it as before. KF4_PC_PRECOMPILE=0: not made before play (the
// list made ahead while the game runs, as before).
#pragma once

#include <cstdint>

#include "device.hpp"

namespace pcr {

// A program and the variants of it that draw only with MSAA (nullptr: none)
struct WarmProgram {
    Vulkan::Program *program = nullptr;
    Vulkan::Program *per_sample = nullptr;
};

// What of the canvas's pipelines depends on the device (the seed's pipelines are made with it: precompile)
struct WarmDevice {
    VkFormat depth_format = VK_FORMAT_UNDEFINED; // (the depth surfaces')
    bool dynamic_ds = false, dynamic_blend = false; // (its dynamic state: Canvas::Impl::choose_dynamic)
    bool high_colour = false;                       // (colour high can draw: 16-bit colour surfaces)
};

// (any thread) The canvas's programs: those whose pipelines are kept. extra: a program drawn only into a
// multisampled colour attachment alone (the expand pass), made with the state of a pass of one colour attachment.
void warm_watch(Vulkan::Device &device, const WarmProgram *programs, uint32_t count, Vulkan::Program *extra,
                const WarmDevice &traits);

// (any thread, after the settings are set, before the game draws) The pipelines made before play: the seed's (every
// pipeline the canvas draws with in a tour of the game, for these settings) and the list's, on every core but one,
// through the cache; precompile_running is true until they are made (the game waits, "Building shaders"). Not with
// KF4_PC_PRECOMPILE=0 (the list made ahead while the game runs, as before) or without the cache.
void precompile(uint32_t samples, bool colour_high);
bool precompile_running();
bool precompile_progress(int *done, int *total);

// (the canvas's thread) Whether every pipeline kept is made at samples: false starts making them (on a thread of its
// own, once; again when pipelines were kept since) and the caller asks again later. colour and depth: images of the
// canvas's colour and depth formats at samples (the passes' attachments), kept by the caller until warm_stop.
bool warm_ready(uint32_t samples, const Vulkan::ImageView &colour, const Vulkan::ImageView *depth);

// (at the end) the thread joined
void warm_stop();

// (before the device draws) The pipeline cache read from cache_path (and the list beside it), the saver started
void store_open(Vulkan::Device &device, const char *cache_path);

// (at the end, the device still there) The threads stopped, the files written
void store_close();

// --stats: the pipelines made ahead
void warm_print_counts();

} // namespace pcr
