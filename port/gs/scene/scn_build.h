// The scene renderer's builder (docs/plans/scene-renderer-design.md; slice 1, HANDOFF_scene_slice1.md): the GS's
// input under the PC renderer's shell (gs_drawing.h's Drawing), built into a scene (scn_scene.h) in the chain's order and
// drawn from it (scn_draw.h) when the frame's vblank closes it, or sooner where the GPU's result is wanted (a read-back,
// a save state's memory, a picture).
//
// What it keeps between scenes is the GS's memory as far as the renderer needs it:
// - the targets: a GPU image of its own for each buffer the game draws into, colour and depth, found by its base page,
//   width and format (as the canvas's surfaces: grown, taken in and let go as the game's buffers overlap), RGBA8 for
//   every colour format (a 16-bit target's texture read as the GS reads it, pcr_prim.h's PCR_F_SURFACE_CT16), D16_UNORM
//   for the 16-bit Z formats and D32F for the others;
// - the texture store (pcr_textures.h), which answers each draw's texture at its place: decoded from the uploads, once a
//   key, into an image kept by its key (the CLUT's colours in it where the GPU's sampler reads it), or a copy of the
//   target drawn there.
// A draw's material is resolved as the draw is met, so that a later upload over the same pages never changes it.
//
// DATE (the shadows', Entity_DrawPoolModels): drawn by the first-draw image (scn_draw.h) where GsDraw_SetScreenAlpha(0)
// cleared the target's alpha before them and nothing wrote it since but the shadows; elsewhere by a copy of the
// rectangle drawn in, as the HUD's one DATE sprite is.
//
// One thread: the shell's ("kf4-pc").
#pragma once

#include <memory>

#include "device.hpp"
#include "gs_drawing.h"

namespace pcr {
class GpuTimes;
class HangNotes;
} // namespace pcr

namespace scn {

std::unique_ptr<pcr::Drawing> make_builder(Vulkan::Device &device, pcr::GpuTimes &times, pcr::HangNotes &notes);

} // namespace scn
