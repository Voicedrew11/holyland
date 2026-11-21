// The picture's anti-aliasing (docs/port/PC_RENDERER.md, "Anti-aliasing"): run on the picture at the renderer's
// resolution, after the canvas's surface is copied into it (PictureOut::draw_picture, pcr.cpp) and before anything
// reads it (the scaling to the window, the scanout ring, a read-back). It replaces the MSAA the port had until
// 2026-11-16, which drew every surface a second time at its samples.
//
// A method is one entry of port/antialias.h's table and one implementation of AntiAliasMethod here, made by
// AntiAliasing::method_for. FXAA and SMAA 1x need the picture alone. A temporal method (SMAA T2x; DLAA, FSR 4 later)
// also needs what AntiAliasFrame gives: the 3D drawn moved by a sub-pixel jitter each drawing (it names its sequence,
// pcr_jitter.h), the picture's depth, each pixel's motion to the previous picture, and that picture's output.
//
// Two threads make pictures, kf4-pc and the pump (pcr.cpp), each with its own canvas and AntiAliasing; the history
// is one, AntiAliasHistory, shared by both, in the order pictures are made. kf4-pc makes its own pictures after the
// pump's before them while the history is wanted (Renderer::make_in_order), so that order is the order they are
// handed out in ("Temporal inputs" in PC_RENDERER.md says why, and what it costs).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "device.hpp"
#include "pcr_jitter.h"
#include "port/antialias.h"

namespace pcr {

struct FullscreenPipeline;

// The camera of a picture's 3D: the world to its view and the world's projection (frame_interp.c's, gsn.h's
// GsnInterpPicture mid and screen), so that a pixel and its depth give a place in the world
struct PictureCamera {
    bool known = false;
    std::array<float, 12> world_to_view{}; // rows of the rotation, each with its translation: view = R world + t
    std::array<float, 6> screen{};         // sx, sy, cx, cy, cz, az: X = sx x / z + cx, Y = sy y / z + cy,
                                           // Z = cz + az / z
};

// What is known of a picture beside its pixels (its maker's: PictureOut::make)
struct PictureFacts {
    PictureCamera camera; // (of the drawing it shows: a picture between's; not known for a vblank's own picture)
    Jitter jitter;        // what that drawing's 3D was moved by (Canvas::begin_drawing), in the picture's pixels
    uint32_t jitter_index = 0; // (its place in the sequence)
    uint64_t frame = 0;   // the game frame the drawing is of (GsnInterpPicture's serial; 0 not known)
    bool cut = false;     // its sender knows the history is of no use (a teleport, a load: pcr_antialias_cut)
};

// Where the picture is on the GS's screen: picture pixel p is at the GS's X = origin.x + p.x / scale.x (and so for Y)
struct PicturePlace {
    bool known = false; // (false: none of it; no motion to or from this picture)
    std::array<float, 2> origin{}, scale{1.0f, 1.0f};
    float zscale = 0.0f; // the depth's Z to the GS's: Z = depth / zscale (Canvas::scanout_depth); 0: no depth
};

// What a method is given for one picture. The images are the picture's size and stay valid for the commands recorded
// in apply.
struct AntiAliasFrame {
    const Vulkan::Image &picture;             // RGBA8, the colours as shown (not linear): anti-aliased in place
    const Vulkan::Image *depth = nullptr;     // R32F, PicturePlace's zscale times the GS's Z, larger nearer; nullptr none
    const Vulkan::Image *motion = nullptr;    // RG16F, picture pixels from each pixel to where it was in the previous
                                              // picture (the jitter not in it); nullptr none
    const Vulkan::Image *previous = nullptr;  // RGBA8, the previous picture's history (what the method kept of it,
                                              // keep_into; of the last picture whose camera was known); nullptr
                                              // none: reset
    const Vulkan::Image *keep_into = nullptr; // RGBA8: what the method keeps of this picture for the next, written by
                                              // apply when it keeps its own (keeps_own_history), else the picture
                                              // after apply is copied into it; nullptr: nothing is kept (no history
                                              // wanted)
    Jitter jitter;                            // this picture's 3D's
    uint32_t jitter_index = 0;                // its place in the method's sequence
    const Vulkan::Image *into = nullptr;      // RGBA8, the picture's size and kind: where a method that reads the
                                              // picture whole may write the anti-aliased picture instead of in place
                                              // (FXAA: no copy of the picture to read from), saying so in wrote_into;
                                              // nullptr: in place
    mutable bool wrote_into = false;
};

// One anti-aliasing method. (A picture's thread: kf4-pc's or the pump's, each with its own.)
class AntiAliasMethod {
public:
    virtual ~AntiAliasMethod() = default;
    // frame.picture anti-aliased in place, with commands recorded into cmd after whatever wrote it; read by anything
    // after. False where it could not be (no image for its work): the picture left as drawn.
    virtual bool apply(Vulkan::CommandBuffer &cmd, const AntiAliasFrame &frame) = 0;
    // The jitter its 3D is to be drawn with (pcr_jitter.h): none for a method of one picture
    virtual JitterSequence jitter() const {
        return JitterSequence::none;
    }
    // Whether apply writes frame.keep_into itself
    virtual bool keeps_own_history() const {
        return false;
    }
};

// The temporal methods' one history, shared by the threads that make pictures (any thread; under its own lock).
// Pictures come to it in the order they are made, which the renderer keeps the order they are shown in.
class AntiAliasHistory {
public:
    // (any thread, as a drawing for a picture begins) The jitter that drawing's 3D is moved by, the next of sequence's
    // (and its place in it)
    struct Drawn {
        Jitter jitter;
        uint32_t index = 0;
        uint64_t number = 0; // (the drawing's, unique on both threads: from 1)
    };
    Drawn next_drawing(JitterSequence sequence);
    // (any thread) A drawing that takes the jitter the next one will have without stepping the sequence: a game
    // frame's, whose pictures come between the pictures between (which must alternate among themselves, SMAA T2x's two)
    Drawn drawing_in_step(JitterSequence sequence);
    // (any thread) The last next_drawing's drawing not made after all (kf4-pc's picture between given to the pump,
    // which takes its own): its place in the sequence the next one's
    void drawing_not_made();
    // (any thread) The next picture's history made empty: a teleport, a load (pcr_antialias_cut)
    void cut();
    // (any thread) Whether a temporal method or a debug view (KF4_AA_DEBUG) is on: the renderer keeps the pictures'
    // order then, and draws with the jitter
    bool wanted() const;
    void set_wanted(bool wanted);

private:
    friend class AntiAliasing;
    mutable std::mutex lock_;
    uint32_t drawings_ = 0;
    uint64_t numbered_ = 0;
    bool cut_ = true, wanted_ = false;
    // (under the lock, written by the picture that made them) the previous picture's: its kept image, facts and place
    Vulkan::ImageHandle previous_, next_;
    PictureFacts previous_facts_;
    PicturePlace previous_place_;
};

// The methods of one thread that makes pictures, each made the first time it is asked for; and the temporal inputs
// it makes for them (the depth, the motion) and the debug views of them (KF4_AA_DEBUG=depth|motion|reproject|jitter)
class AntiAliasing {
public:
    AntiAliasing(Vulkan::Device &device, AntiAliasHistory &history);
    ~AntiAliasing();

    static void warm_pipelines(Vulkan::Device &device, int method, std::vector<FullscreenPipeline> &passes);

    // The image Canvas::scanout_depth is to copy this picture's depth into (R32F, width x height); null when it
    // cannot be made
    const Vulkan::Image *depth_image(uint32_t width, uint32_t height);
    // picture anti-aliased by method (PORT_AA_*: nothing for PORT_AA_OFF or a number past the last), with facts and
    // place (place.zscale 0: depth_image holds nothing of this picture); false where it is left as drawn. into (as
    // AntiAliasFrame's, a method of one picture's): written in place of the picture, *wrote_into true, when the
    // method does that.
    bool apply(Vulkan::CommandBuffer &cmd, const Vulkan::Image &picture, int method, const PictureFacts &facts,
               const PicturePlace &place, const Vulkan::Image *into = nullptr, bool *wrote_into = nullptr);
    // Whether method is FXAA or none, a picture alone (no debug view, no jitter asked, no history wanted): the final
    // pass in one (pcr.cpp's PresentScaler::fused) may do it in place of apply, which then has nothing else to do
    bool fusable(int method) const;
    // The jitter method draws with (none for none)
    JitterSequence jitter_of(int method);

private:
    AntiAliasMethod *method(int method);
    std::unique_ptr<AntiAliasMethod> method_for(int method);

    class Inputs; // (the motion pass and the debug views: pcr_antialias.cpp)
    Vulkan::Device &device_;
    AntiAliasHistory &history_;
    std::array<std::unique_ptr<AntiAliasMethod>, PORT_AA_COUNT> made_; // (by PORT_AA_*; [PORT_AA_OFF] stays empty)
    std::unique_ptr<Inputs> inputs_;
};

} // namespace pcr
