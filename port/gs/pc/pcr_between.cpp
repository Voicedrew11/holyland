// The PC renderer's pictures between (pcr_between.h)
#include "pcr_between.h"

#include <string.h>

#include <algorithm>
#include <vector>

using namespace Vulkan;

namespace pcr {

namespace {

// gs_replay.h's Picture, before the runs' bits (gsn.cpp's InterpPictureHeader)
struct PictureHeader {
    float move[12], mid[12];
    float screen[2][4]; // sx sy cx cy: the world's, the reflection's
    float zt[4];        // cz (the world's, the reflection's), t, 1 - t
    uint32_t kicks, runs;
    float jitter[2];    // (the native GS's upscaling: 0 here)
};
static_assert(sizeof(PictureHeader) == 160, "PictureHeader is gs_replay.h's Picture");

// A kick as the GS queued it (gif_moves.h's K(): gsn.cpp's InterpKick)
struct Kick {
    uint32_t xy;     // X | Y << 16 (12.4, before XYOFFSET)
    uint32_t z;
    uint32_t rgba;
    float s, t, q;
    uint32_t uv;     // U | V << 16
    uint32_t fog;
    uint32_t offset; // XYOFFSET of its context, X | Y << 16
    uint32_t st_owner, rgba_owner, q_owner, uv_owner;
    uint32_t pad[3];
};
static_assert(sizeof(Kick) == 64, "Kick is gs_replay.h's");
static_assert(sizeof(GsnInterpMove) == 64, "GsnInterpMove is gif_moves.h's M()");

// A numbered vertex as its kick (gsn.cpp's InterpKick), with its context's XYOFFSET
Kick kick_of(const GifVertex &v, uint64_t xyoffset) {
    Kick k = {};
    k.xy = uint32_t(v.x) | uint32_t(v.y) << 16;
    k.z = v.z;
    k.rgba = v.rgba;
    k.s = v.s;
    k.t = v.t;
    k.q = v.q;
    k.uv = uint32_t(v.u) | uint32_t(v.v) << 16;
    k.fog = v.fog;
    k.offset = uint32_t(xyoffset & 0xFFFF) | uint32_t((xyoffset >> 32) & 0xFFFF) << 16;
    k.st_owner = v.st_owner;
    k.rgba_owner = v.rgba_owner;
    k.q_owner = v.q_owner;
    k.uv_owner = v.uv_owner;
    return k;
}

} // namespace

Between::Between(Device &device) : device_(device) {
    std::vector<uint8_t> zeros(sizeof(PictureHeader) + 16, 0);
    zero_picture_ = make(zeros.data(), zeros.size(), false);
    empty_list_ = make(zeros.data(), sizeof(Kick), false);
}

Between::~Between() {
    if (kicks_ && kick_map_) {
        device_.unmap_host_buffer(*kicks_, MEMORY_ACCESS_WRITE_BIT);
    }
}

BufferHandle Between::make(const void *data, size_t bytes, bool mapped) {
    BufferCreateInfo info = {};
    info.domain = mapped ? BufferDomain::LinkedDeviceHostPreferDevice : BufferDomain::Device;
    info.size = std::max<size_t>(bytes, 16);
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    BufferHandle buffer = device_.create_buffer(info, data);
    if (!buffer) {
        fprintf(stderr, "[pcr] pictures between: no buffer of %zu bytes\n", bytes);
        abort();
    }
    return buffer;
}

void Between::take_frame(InterpFrame frame) {
    counts_.frames++;
    frame_ = std::move(frame);
    moves_.reset();
    if (frame_->kicks != 0 && frame_->moves) {
        moves_ = make(frame_->moves, size_t(frame_->kicks) * sizeof(GsnInterpMove), false);
    }
}

// The picture's buffer: the header (the frame's projections and kicks added), then the runs' bits
void Between::make_picture(const GsnInterpPicture &p) {
    size_t words = (size_t(p.runs) + 31) / 32;
    std::vector<uint8_t> data(sizeof(PictureHeader) + std::max<size_t>(words, 1) * 4, 0);
    PictureHeader h = {};
    memcpy(h.move, p.move, sizeof(h.move));
    memcpy(h.mid, p.mid, sizeof(h.mid));
    for (int i = 0; i < 2; i++) {
        memcpy(h.screen[i], frame_->screen[i], 4 * sizeof(float));
        h.zt[i] = frame_->screen[i][4];
    }
    h.zt[2] = p.t;
    h.zt[3] = 1.0f - p.t;
    h.kicks = frame_->kicks;
    h.runs = p.runs;
    memcpy(data.data(), &h, sizeof(h));
    if (words && p.run_bits) {
        memcpy(data.data() + sizeof(h), p.run_bits, words * 4);
    }
    picture_ = make(data.data(), data.size(), false);
}

std::span<const uint8_t> Between::begin_picture(const GsnInterpPicture &p) {
    counts_.pictures++;
    if (!frame_ || frame_->serial != p.serial) {
        counts_.missed++;
        return {};
    }
    make_picture(p);
    // the walk's kicks: a buffer of the picture's own (the last picture's draws may still read theirs)
    if (kicks_ && kick_map_) {
        device_.unmap_host_buffer(*kicks_, MEMORY_ACCESS_WRITE_BIT);
    }
    kicks_ = make(nullptr, std::max<size_t>(frame_->kicks, 1) * sizeof(Kick), true);
    kick_map_ = static_cast<uint8_t *>(device_.map_host_buffer(*kicks_, MEMORY_ACCESS_WRITE_BIT));
    if (!kick_map_) {
        fprintf(stderr, "[pcr] pictures between: the kicks' buffer cannot be mapped\n");
        abort();
    }
    walked_serial_ = frame_->serial;
    walked_ = true;
    active_ = true;
    return {frame_->stream, frame_->stream_bytes};
}

bool Between::begin_picture_again(const GsnInterpPicture &p) {
    counts_.pictures++;
    if (!frame_ || frame_->serial != p.serial || !walked_ || walked_serial_ != p.serial || !kicks_) {
        counts_.missed++;
        return false;
    }
    make_picture(p);
    counts_.drawn_again++;
    active_ = true;
    return true;
}

void Between::end_picture_again() {
    active_ = false;
}

void Between::walked(WalkReason why) {
    switch (why) {
    case WalkReason::none_kept:
        counts_.walked_none_kept++;
        break;
    case WalkReason::store_changed:
        counts_.walked_store_changed++;
        break;
    case WalkReason::not_kept:
        counts_.walked_not_kept++;
        break;
    }
}

void Between::kicked(const GifVertex &v, uint64_t xyoffset) {
    if (active_) {
        if (v.kick >= frame_->kicks) {
            return; // (end_picture counts it)
        }
        const Kick k = kick_of(v, xyoffset);
        memcpy(kick_map_ + size_t(v.kick) * sizeof(Kick), &k, sizeof(k)); // (one write to the mapped buffer)
        return;
    }
    if (!live_on_ || v.kick == no_kick) {
        return;
    }
    const size_t bytes = (size_t(v.kick) + 1) * sizeof(Kick);
    if (live_kicks_.size() < bytes) {
        live_kicks_.resize(bytes, 0);
    }
    const Kick k = kick_of(v, xyoffset);
    memcpy(live_kicks_.data() + size_t(v.kick) * sizeof(Kick), &k, sizeof(k));
}

void Between::keep_live_kicks() {
    live_kicks_.clear();
    live_on_ = true;
}

Between::KickList Between::end_live_kicks() {
    live_on_ = false;
    return std::move(live_kicks_);
}

bool Between::adopt_live_kicks(KickList kicks) {
    if (!frame_ || kicks.size() != size_t(frame_->kicks) * sizeof(Kick)) {
        return false;
    }
    if (kicks_ && kick_map_) {
        device_.unmap_host_buffer(*kicks_, MEMORY_ACCESS_WRITE_BIT);
        kick_map_ = nullptr;
    }
    kicks.resize(std::max(kicks.size(), sizeof(Kick)), 0); // (no kicks: one zeroed)
    kicks_ = make(kicks.data(), kicks.size(), false);
    walked_serial_ = uint32_t(frame_->serial);
    walked_ = true;
    return true;
}

void Between::frame_from_own_walk(FrameWalkRefused refused) {
    if (refused == FrameWalkRefused::none) {
        counts_.frames_from_own_walk++;
        return;
    }
    counts_.own_walk_refused[size_t(refused)]++;
}

bool Between::end_picture(uint32_t kicks) {
    active_ = false;
    if (kicks_ && kick_map_) {
        device_.unmap_host_buffer(*kicks_, MEMORY_ACCESS_WRITE_BIT);
        kick_map_ = nullptr;
    }
    if (!frame_ || kicks != frame_->kicks) {
        static int told;
        if (!told++) {
            fprintf(stderr, "[pcr] a picture between: %u vertex kicks where the frame has %u\n", kicks,
                    frame_ ? frame_->kicks : 0u);
        }
        counts_.kicks_not_the_frames++;
        return false;
    }
    return true;
}

const Buffer &Between::picture() const {
    return active_ && picture_ ? *picture_ : *zero_picture_;
}

const Buffer &Between::kicks() const {
    return active_ && kicks_ ? *kicks_ : *empty_list_;
}

const Buffer &Between::moves() const {
    return active_ && moves_ ? *moves_ : *empty_list_;
}

void Between::print_counts(FILE *out) const {
    const auto own_walk_refused = [this](FrameWalkRefused why) { return counts_.own_walk_refused[size_t(why)]; };
    fprintf(out,
            "[stats] pc pictures between: %llu frames, %llu pictures (%llu not of the frame taken, %llu whose kicks were "
            "not the frame's); %llu drawn again from a kept walk, walked: %llu with none kept, %llu the store "
            "changed; %llu walks not kept; %llu frames' pictures made from the frame's own walk, refused: "
            "%llu not kept, %llu not the frame, %llu trail paired, %llu reads the buffer drawn, %llu copies it, "
            "%llu uploads into it, %llu native unpaired, %llu marks\n",
            (unsigned long long)counts_.frames, (unsigned long long)counts_.pictures, (unsigned long long)counts_.missed,
            (unsigned long long)counts_.kicks_not_the_frames, (unsigned long long)counts_.drawn_again,
            (unsigned long long)counts_.walked_none_kept, (unsigned long long)counts_.walked_store_changed,
            (unsigned long long)counts_.walked_not_kept, (unsigned long long)counts_.frames_from_own_walk,
            (unsigned long long)own_walk_refused(FrameWalkRefused::not_kept),
            (unsigned long long)own_walk_refused(FrameWalkRefused::not_the_frame),
            (unsigned long long)own_walk_refused(FrameWalkRefused::trail_paired),
            (unsigned long long)own_walk_refused(FrameWalkRefused::reads_drawn),
            (unsigned long long)own_walk_refused(FrameWalkRefused::copies_drawn),
            (unsigned long long)own_walk_refused(FrameWalkRefused::uploads_drawn),
            (unsigned long long)own_walk_refused(FrameWalkRefused::native_unpaired),
            (unsigned long long)own_walk_refused(FrameWalkRefused::marks));
}

} // namespace pcr
