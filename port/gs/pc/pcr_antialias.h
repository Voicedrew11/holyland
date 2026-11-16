// The picture's anti-aliasing (docs/port/PC_RENDERER.md, "Anti-aliasing"): run on the picture at the renderer's
// resolution, after the canvas's surface is copied into it (PictureOut::draw_picture, pcr.cpp) and before anything
// reads it (the scaling to the window, the scanout ring, a read-back). It replaces the MSAA the port had until
// 2026-11-16, which drew every surface a second time at its samples.
//
// A method is one entry of port/antialias.h's table and one implementation of AntiAliasMethod here, made by
// AntiAliasing::method_for. FXAA needs the picture alone. A method that needs more of the frame (DLAA, FSR 4: the
// depth, motion vectors, a jittered camera, the frame before) would need the canvas to give it those first; the
// picture is the only input the interface has now.
#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "device.hpp"
#include "port/antialias.h"

namespace pcr {

// One anti-aliasing method. (A picture's thread: kf4-pc's or the pump's, each with its own.)
class AntiAliasMethod {
public:
    virtual ~AntiAliasMethod() = default;
    // picture (RGBA8, the colours as shown: not linear light) anti-aliased in place, with commands recorded into cmd
    // after whatever wrote it; read by anything after. False where it could not be (no image for its work): the picture
    // left as drawn.
    virtual bool apply(Vulkan::CommandBuffer &cmd, const Vulkan::Image &picture) = 0;
};

// The methods of one thread that makes pictures, each made the first time it is asked for
class AntiAliasing {
public:
    explicit AntiAliasing(Vulkan::Device &device);
    ~AntiAliasing();

    // picture anti-aliased by method (PORT_AA_*: nothing for PORT_AA_OFF or a number past the last); false where it
    // is left as drawn
    bool apply(Vulkan::CommandBuffer &cmd, const Vulkan::Image &picture, int method);

private:
    std::unique_ptr<AntiAliasMethod> method_for(int method);

    Vulkan::Device &device_;
    std::array<std::unique_ptr<AntiAliasMethod>, PORT_AA_COUNT> made_; // (by PORT_AA_*; [PORT_AA_OFF] stays empty)
};

} // namespace pcr
