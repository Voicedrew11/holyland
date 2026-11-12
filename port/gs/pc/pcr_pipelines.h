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
#pragma once

#include <cstdint>

#include "device.hpp"

namespace pcr {

// A program and the variants of it that draw only with MSAA (nullptr: none)
struct WarmProgram {
    Vulkan::Program *program = nullptr;
    Vulkan::Program *per_sample = nullptr;
};

// (any thread) The canvas's programs: those whose pipelines are kept. extra: a program drawn only into a
// multisampled colour attachment alone (the expand pass), made with the state of a pass of one colour attachment.
void warm_watch(Vulkan::Device &device, const WarmProgram *programs, uint32_t count, Vulkan::Program *extra);

// (the canvas's thread) Whether every pipeline kept is made at samples: false starts making them (on a thread of its
// own, once; again when pipelines were kept since) and the caller asks again later. colour and depth: images of the
// canvas's colour and depth formats at samples (the passes' attachments), kept by the caller until warm_stop.
bool warm_ready(uint32_t samples, const Vulkan::ImageView &colour, const Vulkan::ImageView *depth);

// (at the end) the thread joined
void warm_stop();

// --stats: the pipelines made ahead
void warm_print_counts();

} // namespace pcr
