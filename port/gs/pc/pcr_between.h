// The PC renderer's pictures between on the GPU (docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native
// GS", step 5). frame_interp.c ("On the GPU") hands over the frame once (its packets, as a picture between draws them,
// and for each vertex kick in them how a picture between moves it: gsn.h's GsnInterpMove), then for each picture its
// time, its cameras and the model parts that move with their model then. The frame's packets are walked again for each
// picture with the vertices where the game put them, and the shaders move them: a GIF vertex by its kicks' moves
// (shaders/pcr_prim.vert, port/render/shaders/gif_moves.h: gs_replay.comp's maths), a native draw's by its record's
// (port/render/part_moves.hpp, vu1_*_maths.h's moved()).
//
// Set 1 of the canvas's draws: binding 0 the picture (gs_replay.h's Picture: the cameras, the projections, t, the
// kicks and runs and the runs' bits; zeros outside a picture between), 1 the walk's kicks as the GS queued them
// (gsn.cpp's InterpKick), 2 the frame's moves.
//
// One thread: the PC renderer's ("kf4-pc").
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>

#include "device.hpp"
#include "gsn.h"
#include "pcr_gif.h"

namespace pcr {

// A picture between's frame (frame_interp.c's, malloc'd): the receiver frees it, its moves and its stream
struct FreeInterpFrame {
    void operator()(GsnInterpFrame *frame) const {
        free(frame->moves);
        free(frame->stream);
        free(frame);
    }
};
using InterpFrame = std::unique_ptr<GsnInterpFrame, FreeInterpFrame>;

struct BetweenCounts {
    uint64_t frames = 0, pictures = 0, missed = 0, kicks_not_the_frames = 0;
};

class Between {
public:
    explicit Between(Vulkan::Device &device);
    ~Between();
    Between(const Between &) = delete;
    Between &operator=(const Between &) = delete;

    // The frame (the one before let go: the GPU keeps what it still reads until it is done with it)
    void take_frame(InterpFrame frame);
    // A picture asked for: its Picture made for the shaders. The frame's packets to walk for it (a uint32_t path, a
    // uint32_t size, then the bytes, as GsnInterpFrame's stream), or none when it is not of the frame taken (counted:
    // nothing is drawn again).
    std::span<const uint8_t> begin_picture(const GsnInterpPicture &picture);
    // During the walk: a numbered kick (GifVertex::kick), with its context's XYOFFSET
    void kicked(const GifVertex &v, uint64_t xyoffset);
    // The walk's end, after its draws: false (counted) when its kicks were not the frame's count (moves taken by kicks
    // past the frame's are none)
    bool end_picture(uint32_t kicks);
    // Between begin_picture and end_picture
    bool active() const { return active_; }

    // Set 1's buffers (the picture's while active, else zeros and empty lists)
    const Vulkan::Buffer &picture() const;
    const Vulkan::Buffer &kicks() const;
    const Vulkan::Buffer &moves() const;

    BetweenCounts counts() const { return counts_; }
    // --stats: the "[stats] pc pictures between" line
    void print_counts(FILE *out) const;

private:
    Vulkan::BufferHandle make(const void *data, size_t bytes, bool mapped);

    Vulkan::Device &device_;
    InterpFrame frame_;
    Vulkan::BufferHandle moves_, kicks_, picture_;
    Vulkan::BufferHandle zero_picture_, empty_list_;
    uint8_t *kick_map_ = nullptr;
    bool active_ = false;
    BetweenCounts counts_;
};

} // namespace pcr
