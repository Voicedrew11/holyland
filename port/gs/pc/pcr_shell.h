// The PC renderer's shell (pcr.cpp: its device, thread, inbox, scanout ring, presenter, save states and captures) with
// another renderer under it than the canvas: the scene renderer (port/gs/scene/scn.h, --renderer scene). That renderer
// draws the frame through gs_drawing.h's Drawing; the shell then has no pictures between of its own (a picture between
// shows the last picture again), no pump, no census and no pipelines made before play.
#pragma once

#include <memory>

#include "device.hpp"
#include "gs_drawing.h"

namespace pcr {

class GpuTimes;
class HangNotes;

// Made on the shell's thread ("kf4-pc") once the device is there. times outlives what it makes; notes too.
using DrawingMaker = std::unique_ptr<Drawing> (*)(Vulkan::Device &device, GpuTimes &times, HangNotes &notes);

// (before pcr_open) What draws the frame: maker's renderer, named name in the log (nullptr: the canvas, as without)
void use_drawing(DrawingMaker maker, const char *name);

} // namespace pcr
