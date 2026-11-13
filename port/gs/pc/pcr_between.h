// The PC renderer's pictures between on the GPU (docs/port/PC_RENDERER.md, step 5). frame_interp.c ("On the GPU") hands over the frame once (its packets, as a picture between draws them,
// and for each vertex kick in them how a picture between moves it: gsn.h's GsnInterpMove), then for each picture its
// time, its cameras and the model parts that move with their model then. The frame's packets are walked again for each
// picture with the vertices where the game put them, and the shaders move them: a GIF vertex by its kicks' moves
// (shaders/pcr_prim.vert, port/render/shaders/gif_moves.h), a native draw's by its record's
// (port/render/part_moves.hpp, vu1_*_maths.h's moved()).
//
// Step 5b: a walk is kept (pcr_canvas.h's KeptWalk) and the frame's other pictures draw it again, with only their
// Picture new (begin_picture_again): every walk of a frame starts from the front end's state when the frame was taken,
// so its kicks are the last walk's. pcr.cpp's Intake decides, by what the kept walk read of the texture store.
//
// Set 1 of the canvas's draws: binding 0 the picture (the picture's header: the cameras, the projections, t, the
// kicks and runs and the runs' bits; zeros outside a picture between), 1 the walk's kicks as the GS queued them
// (the GIF front end's kick), 2 the frame's moves.
//
// Two threads: the PC renderer's ("kf4-pc") has the Between that frames are given to and walked with; the pump's
// ("kf4-pc-pump", pcr.cpp) has one of its own, given the frame as walked (hand_over, take_walked), which draws the
// kept walks again. Each Between is used by one thread only.
#pragma once

#include <cstdint>
#include <cstdio>
#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "device.hpp"
#include "gsn.h"
#include "pcr_frame_walk.h"
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
    // (step 5b) pictures drawn again from a kept walk, and those that walked: no walk of the frame kept, or what the
    // kept walk read of the store changed since; and walks not kept (a trail moved on the CPU, a read-back, a kick
    // count off)
    uint64_t drawn_again = 0, walked_none_kept = 0, walked_store_changed = 0, walked_not_kept = 0;
    // (a frame walked once) frames whose pictures' walk was made from the frame's own, and why the others' was not
    uint64_t frames_from_own_walk = 0;
    std::array<uint64_t, size_t(FrameWalkRefused::count)> own_walk_refused{};
};

// Why a picture walked the frame's packets in place of drawing a kept walk again (BetweenCounts)
// (not_kept: after the walk, a walk not kept)
enum class WalkReason : uint8_t { none_kept, store_changed, not_kept };

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
    // During the walk: a numbered kick (GifVertex::kick), with its context's XYOFFSET. Outside one, while the frame's
    // own walk's kicks are kept (keep_live_kicks): kept for adopt_live_kicks.
    void kicked(const GifVertex &v, uint64_t xyoffset);

    // A frame walked once (pcr_frame_walk.h): the frame's own walk's kicks kept from keep_live_kicks until
    // end_live_kicks, which hands them out (Kick's bytes by kick number), outside a picture between
    // (and the kicks of native draws numbered from native_kick_base, the trail's: kept apart, by their number less it)
    using KickList = std::vector<uint8_t>;
    struct LiveKicks {
        KickList kicks, native;
    };
    void keep_live_kicks();
    LiveKicks end_live_kicks();
    // After take_frame: kicks made the frame's walked kicks, as a first walk's (begin_picture_again draws with them),
    // when they are the frame's count. The native draws' kicks after them, numbered on from the frame's count, with
    // native_moves (NativeDraws::take_trail_moves, one each) after the frame's moves. False otherwise (nothing changed).
    bool adopt_live_kicks(LiveKicks kicks, std::vector<GsnInterpMove> native_moves);
    // A frame's own walk made its pictures' (adopted), or why not (counted)
    void frame_from_own_walk(FrameWalkRefused refused);
    // The walk's end, after its draws: false (counted) when its kicks were not the frame's count (moves taken by kicks
    // past the frame's are none)
    bool end_picture(uint32_t kicks);
    // A picture drawn again from a kept walk: its Picture made, the kicks the last walk's. False (counted, nothing
    // made) when it is not of the frame taken or the frame has not been walked yet.
    bool begin_picture_again(const GsnInterpPicture &picture);
    void end_picture_again();
    // A picture that walked, and why (counted)
    void walked(WalkReason why);

    // Between begin_picture and end_picture (or the _again pair)
    bool active() const { return active_; }

    // A frame as walked, for the pump's Between: its serial, its projections and kick count, its walked kicks and its
    // moves (buffers nothing writes again once walked)
    struct Walked {
        uint64_t serial = 0;
        std::array<std::array<float, 5>, 2> screen{};
        uint32_t kicks = 0;
        Vulkan::BufferHandle kick_list, moves;
    };
    // (kf4-pc) A picture to be drawn again by the pump: the frame as walked when the picture is of the frame taken
    // and the frame was walked (counted as begin_picture_again counts a picture drawn again), else nullopt (counted
    // as not of the frame)
    std::optional<Walked> hand_over(const GsnInterpPicture &picture);
    // (the pump) The frame its pictures are drawn again with (begin_picture_again): the one before let go
    void take_walked(Walked frame);

    // Set 1's buffers (the picture's while active, else zeros and empty lists)
    const Vulkan::Buffer &picture() const;
    const Vulkan::Buffer &kicks() const;
    const Vulkan::Buffer &moves() const;

    BetweenCounts counts() const { return counts_; }
    // --stats: the "[stats] pc pictures between" line
    void print_counts(FILE *out) const;

private:
    Vulkan::BufferHandle make(const void *data, size_t bytes, bool mapped);
    void make_picture(const GsnInterpPicture &picture);

    Vulkan::Device &device_;
    InterpFrame frame_;
    // the frame's serial, projections and kicks (frame_'s, or the pump's frame as walked)
    struct Shape {
        uint64_t serial = 0;
        std::array<std::array<float, 5>, 2> screen{};
        uint32_t kicks = 0;
    };
    std::optional<Shape> shape_;
    Vulkan::BufferHandle moves_, kicks_, picture_;
    Vulkan::BufferHandle zero_picture_, empty_list_;
    uint8_t *kick_map_ = nullptr;
    bool active_ = false;
    uint64_t walked_serial_ = 0; // (the frame whose kicks kicks_ holds)
    bool walked_ = false;
    // the frame's own walk's kicks (keep_live_kicks), and whether they are kept
    KickList live_kicks_, live_native_;
    bool live_on_ = false;
    BetweenCounts counts_;
};

} // namespace pcr
