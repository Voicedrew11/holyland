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

std::span<const uint8_t> Between::begin_picture(const GsnInterpPicture &p) {
    counts_.pictures++;
    if (!frame_ || frame_->serial != p.serial) {
        counts_.missed++;
        return {};
    }
    // the picture's buffer: the header (the frame's projections and kicks added), then the runs' bits
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
    active_ = true;
    return {frame_->stream, frame_->stream_bytes};
}

void Between::kicked(const GifVertex &v, uint64_t xyoffset) {
    if (!active_ || v.kick >= frame_->kicks) {
        return; // (end_picture counts it)
    }
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
    memcpy(kick_map_ + size_t(v.kick) * sizeof(Kick), &k, sizeof(k)); // (one write to the mapped buffer)
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
    fprintf(out,
            "[stats] pc pictures between: %llu frames, %llu pictures (%llu not of the frame taken, %llu whose kicks were "
            "not the frame's)\n",
            (unsigned long long)counts_.frames, (unsigned long long)counts_.pictures, (unsigned long long)counts_.missed,
            (unsigned long long)counts_.kicks_not_the_frames);
}

} // namespace pcr
