// The PC renderer (pcr.h; docs/port/PC_RENDERER.md), step 1:
// the skeleton. Its own Vulkan device (through Granite, the layer paraLLEl-GS runs on) and its
// own thread, "kf4-pc", which takes what gs.c sends in the order sent: GIF packets, vblanks and the pictures
// between. At a vblank the display registers name the buffer shown; its picture is, for now, an image cleared to
// black (KF4_PC_CLEAR=RRGGBB: that colour), handed out in three ways:
// - with GSN_WANT_IMAGE, copied into a slot of the scanout ring, which the presenter (port/platform/present_vk.cpp)
//   takes on the game's thread (pcr_scanout_take_tag) and gives back once its command buffer is submitted
//   (pcr_scanout_release). A slot is published (given a serial and the picture's tag) only after the commands that
//   write it are on the queue, so the presenter, submitting to the same queue after it, sees the whole picture;
// - with GSN_WANT_PIXELS (and for the presenter's pixels), read back into a PortGsFrame;
// - with GSN_PRESENT, handed to the presenter's callbacks from this thread once it is there.
// Nothing of the old native GS's code is here or linked from here: gsn.h (port/gs/shared) is included for its plain
// types only.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#endif

#include "context.hpp"
#include "pcr_between.h"
#include "pcr_canvas.h"
#include "pcr_gpu_time.h"
#include "pcr_native.h"
#include "pcr_census.h"
#include "pcr_gif.h"
#include "pcr_late.h"
#include "pcr_shaders.h"
#include "port/gs_capture_format.h"
#include "device.hpp"
#include "thread_id.hpp"

extern "C" {
#include "port/host.h"
#include "port/mouse_lead.h"
#include "port/platform.h"
#include "port/profile.h"
#include "port/render.h"
#include "pcr.h"
}

using namespace Vulkan;

namespace {

// ---- What the PC renderer's thread is sent -----------------------------------------------------------
// The display registers a vblank is given: PMODE..BGCOLOR, a register every 16 bytes (gs.c's VsyncRequest)
constexpr size_t PRIVILEGED_WORDS = 0xF0 / 8;

// The answer to a picture its sender waits for (pcr_picture without GSN_PRESENT): on the sender's stack, written
// by the PC renderer's thread under the inbox's lock
struct Reply {
    int result = 0;
    bool done = false;
};

struct GifPacket {
    int path;
    std::vector<uint8_t> bytes;
};

// The want_pixels bits gs.c gives, and the PC's switches as they were when the picture was asked for
struct VblankPicture {
    std::array<uint64_t, PRIVILEGED_WORDS> privileged;
    int field;
    int want;
    bool progressive, deinterlace;
    uint64_t tag;
    PortGsFrame *frame; // (the sender's, while it waits: reply set)
    Reply *reply;       // (nullptr: nobody waits)
    // (Renderer::picture) The pump has it already, sent there directly (kf4-pc only keeps its count of things), and
    // whether kf4-pc tells the sender it is done with it (Renderer::routed_done_)
    bool direct = false, counted = false;
};

// A picture between's frame (frame_interp.c's: pcr_between.h), and a picture asked for, its runs' bits kept
using pcr::InterpFrame;
struct InterpPicture {
    GsnInterpPicture picture;
    std::vector<uint32_t> run_bits;
    bool direct = false, counted = false; // (as VblankPicture's)
};

// When the pictures' mouse turns are taken (pcr_late.h's TurnLead): kf4-pc's and the pump's, one for both
struct SharedTurnLead {
    std::mutex lock;
    pcr::TurnLead lead;
};

// (kf4-pc or the pump, as it draws the picture) Instant mouse look's last step (port/include/port/mouse_lead.h): the
// picture's cameras turned on by what the mouse's hand has moved since the hand they were turned to, to the hand
// lead.lead_s() before the picture is shown (the newest, when that is not read yet, or the picture's time is not
// known). How long before it is shown the newest reading was (0: not known), for the caller that knows the turn is the
// last to give to lead.turned: the game's thread reads the mouse as it waits and between its calls, so a reading may
// be a few milliseconds old.
double turn_to_hand(GsnInterpPicture &picture, SharedTurnLead &lead) {
    constexpr double VBLANK_S = 1001.0 / 60000.0;
    double at = HUGE_VAL, before_s = 0;
    if (picture.shown_s > 0) {
        double lead_s;
        {
            std::lock_guard<std::mutex> hold(lead.lock);
            lead_s = lead.lead.lead_s();
        }
        double shown_v = port_video_vblanks_at(picture.shown_s), newest_v = port_mouse_lead_newest_vblanks();
        if (shown_v >= 0) {
            at = shown_v - lead_s / VBLANK_S;
            before_s = newest_v >= 0 ? (shown_v - newest_v) * VBLANK_S : 0;
        }
    }
    MouseTurn hand = {picture.hand_yaw_rad, picture.hand_pitch_rad};
    MouseTurn since = port_mouse_lead_since(hand, at);
    if (since.yaw_rad != 0 || since.pitch_rad != 0) {
        mouse_lead_turn_picture(picture.move, picture.mid, since);
        picture.hand_yaw_rad += since.yaw_rad;
        picture.hand_pitch_rad += since.pitch_rad;
    }
    return before_s;
}

void turned_before(SharedTurnLead &lead, double before_s) {
    if (before_s > 0) {
        std::lock_guard<std::mutex> hold(lead.lock);
        lead.lead.turned(before_s);
    }
}

// A frame's pictures between as the pump draws them again (the pump, below): the frame's kept walk, the frame as walked,
// and kf4-pc's surfaces copied for the pump's canvas to draw on
struct PumpFrame {
    std::shared_ptr<const pcr::KeptWalk> walk;
    pcr::Between::Walked walked;
    std::shared_ptr<pcr::SurfaceCopies> surfaces;
};

// Work that must be done on the PC renderer's thread (its device's), its sender waiting: save states
struct Job {
    std::function<void()> work;
    Reply *reply;
};

using Message = std::variant<GifPacket, VblankPicture, InterpFrame, InterpPicture, Job>;

// The queue from the senders (the DMA controller's thread, mostly) to the PC renderer's thread, and the waits on
// it: for the thread to be idle, for a picture's reply, for room (the packets' bytes held), for a window's
// pictures to be at most two vblanks behind
class Inbox {
public:
    // Any thread. False once the PC renderer's thread has stopped (the message is dropped).
    bool put(Message message, size_t bytes, bool vblank) {
        std::unique_lock<std::mutex> hold(lock_);
        room_.wait(hold, [&] { return stopping_ || held_bytes_ == 0 || held_bytes_ + bytes <= MAX_HELD_BYTES; });
        if (stopping_) {
            return false;
        }
        held_bytes_ += bytes;
        vblanks_sent_ += vblank ? 1 : 0;
        messages_.push_back(std::move(message));
        wake_.notify_one();
        return true;
    }

    // (the PC renderer's thread) The next message, or none when it is to stop
    std::optional<Message> take() {
        std::unique_lock<std::mutex> hold(lock_);
        wake_.wait(hold, [&] { return stopping_ || !messages_.empty(); });
        if (stopping_) {
            return std::nullopt;
        }
        Message message = std::move(messages_.front());
        messages_.pop_front();
        busy_ = true;
        return message;
    }

    // (the PC renderer's thread) The message taken is done
    void done(size_t bytes) {
        std::lock_guard<std::mutex> hold(lock_);
        held_bytes_ -= bytes;
        busy_ = false;
        room_.notify_all();
        idle_.notify_all();
    }

    // (kf4-pc or the pump) A message's sender answered (the reply is the sender's: not touched after)
    void answer(Reply *reply, int result) {
        std::lock_guard<std::mutex> hold(lock_);
        reply->result = result;
        reply->done = true;
        idle_.notify_all();
    }

    // (kf4-pc or the pump) A vblank's picture handed out (or none made): the vblank done, for backlog and the window's
    // wait
    void vblank_handed_out() {
        std::lock_guard<std::mutex> hold(lock_);
        vblanks_done_++;
        idle_.notify_all();
    }

    // Any thread: everything sent before is done (or the thread is gone)
    void wait_idle() {
        std::unique_lock<std::mutex> hold(lock_);
        idle_.wait(hold, [&] { return gone_ || (messages_.empty() && !busy_); });
    }

    bool wait_idle_for(std::chrono::milliseconds most) {
        std::unique_lock<std::mutex> hold(lock_);
        return idle_.wait_for(hold, most, [&] { return gone_ || (messages_.empty() && !busy_); });
    }

    // The sender: its picture's answer (0 if the thread went first)
    int wait_reply(const Reply &reply) {
        std::unique_lock<std::mutex> hold(lock_);
        idle_.wait(hold, [&] { return gone_ || reply.done; });
        return reply.done ? reply.result : 0;
    }

    // The sender of a window's picture: at most two vblanks ahead of their pictures handed out
    void wait_behind_at_most(uint64_t vblanks) {
        std::unique_lock<std::mutex> hold(lock_);
        idle_.wait(hold, [&] { return gone_ || vblanks_sent_ - vblanks_done_ <= vblanks; });
    }

    int backlog() {
        std::lock_guard<std::mutex> hold(lock_);
        return int(vblanks_sent_ - vblanks_done_);
    }

    // At exit: nothing more is taken; the waits end once the thread has said it is gone
    void stop() {
        std::lock_guard<std::mutex> hold(lock_);
        stopping_ = true;
        wake_.notify_all();
        room_.notify_all();
    }

    // (the PC renderer's thread, last)
    void gone() {
        std::lock_guard<std::mutex> hold(lock_);
        gone_ = true;
        idle_.notify_all();
    }

    bool wait_gone_for(std::chrono::milliseconds most) {
        std::unique_lock<std::mutex> hold(lock_);
        return idle_.wait_for(hold, most, [&] { return gone_; });
    }

private:
    static constexpr size_t MAX_HELD_BYTES = size_t(32) << 20; // (the packets not yet worked on)
    std::mutex lock_;
    std::condition_variable wake_, idle_, room_;
    std::deque<Message> messages_;
    size_t held_bytes_ = 0;
    uint64_t vblanks_sent_ = 0, vblanks_done_ = 0;
    bool busy_ = false, stopping_ = false, gone_ = false;
};

// ---- The scanout ring --------------------------------------------------------------------------------
// The pictures handed to the presenter: a ring of images (with --frame-rate above 60
// a vblank has several pictures, shown at their own times, so the newest must not write over those waiting). The
// slot the presenter holds is passed over. Written by the PC renderer's thread, taken by the game's.
constexpr int SCANOUT_SLOTS = 16;

class ScanoutRing {
public:
    // (the PC renderer's thread) The slot the next picture is written into (-1: none), with an image of that size: made
    // here when the slot has none of it (outside the lock: Granite's device lock, which the presenter holds while
    // it releases a slot, comes before it)
    int claim(Device &device, uint32_t width, uint32_t height) {
        int slot;
        bool fresh;
        {
            std::lock_guard<std::mutex> hold(lock_);
            slot = (written_ + 1) % SCANOUT_SLOTS;
            if (slot == taken_) {
                slot = (slot + 1) % SCANOUT_SLOTS;
            }
            Slot &s = slots_[slot];
            s.serial = 0; // (not to be taken while it is written)
            s.scaled_valid = false;
            fresh = !s.image || s.width != width || s.height != height;
            written_ = slot;
        }
        if (!fresh) {
            return slot;
        }
        ImageHandle made = make_picture_image(device, width, height);
        if (!made) {
            fprintf(stderr, "[pcr] no image for a %ux%u scanout\n", width, height);
            return -1; // (the slot keeps its serial 0: never taken)
        }
        ImageHandle old;
        {
            std::lock_guard<std::mutex> hold(lock_);
            Slot &s = slots_[slot];
            old = std::move(s.image); // (never the slot taken: passed over above)
            s.image = std::move(made);
            s.width = width;
            s.height = height;
        }
        return slot; // (old let go here, on this thread: Granite destroys it once the GPU is past it)
    }

    const Image &image(int slot) {
        std::lock_guard<std::mutex> hold(lock_);
        return *slots_[slot].image;
    }

    // (the PC renderer's thread, between claim and publish) The slot's picture at the window's size, and with work the
    // image the scaling writes before the sharpening: made here when the slot has none of that size (outside the lock,
    // as claim's). Taken with the slot's picture once published. Both null when they cannot be made.
    struct Scaled {
        const Image *scaled = nullptr, *work = nullptr;
    };
    Scaled scaled(Device &device, int slot, uint32_t width, uint32_t height, bool work) {
        bool fresh, fresh_work;
        {
            std::lock_guard<std::mutex> hold(lock_);
            const Slot &s = slots_[slot];
            fresh = !s.scaled || s.scaled_width != width || s.scaled_height != height;
            fresh_work = work && (fresh || !s.scaled_work);
        }
        ImageHandle made, made_work, old, old_work;
        if (fresh) {
            made = make_picture_image(device, width, height);
        }
        if (fresh_work) {
            made_work = make_picture_image(device, width, height);
        }
        if ((fresh && !made) || (fresh_work && !made_work)) {
            fprintf(stderr, "[pcr] no image for a %ux%u scaled picture\n", width, height);
            return {};
        }
        std::lock_guard<std::mutex> hold(lock_);
        Slot &s = slots_[slot];
        if (fresh) {
            old = std::move(s.scaled);
            old_work = std::move(s.scaled_work);
            s.scaled = std::move(made);
            s.scaled_width = width;
            s.scaled_height = height;
        }
        if (fresh_work) {
            s.scaled_work = std::move(made_work);
        }
        s.scaled_valid = true;
        return {s.scaled.get(), work ? s.scaled_work.get() : nullptr};
    }

    // (the PC renderer's thread) Once the commands writing the slot are on the queue: the presenter may take it
    void publish(int slot, uint64_t tag) {
        std::lock_guard<std::mutex> hold(lock_);
        slots_[slot].serial = ++serial_;
        slots_[slot].tag = tag;
        newest_ = slot;
    }

    // (the game's thread) The newest picture with that tag, or the newest of all when it is no longer there
    bool take_tag(GsnScanout &out, uint64_t tag) {
        std::lock_guard<std::mutex> hold(lock_);
        int pick = newest_;
        for (int i = 0; i < SCANOUT_SLOTS; i++) {
            const Slot &s = slots_[i];
            if (s.serial != 0 && s.tag == tag &&
                (pick < 0 || slots_[pick].tag != tag || s.serial > slots_[pick].serial)) {
                pick = i;
            }
        }
        if (pick < 0 || !slots_[pick].image || slots_[pick].serial == 0) {
            return false;
        }
        const Slot &s = slots_[pick];
        out = {};
        out.image = uint64_t(s.image->get_image());
        out.width = s.width;
        out.height = s.height;
        out.serial = s.serial;
        out.tag = s.tag;
        if (s.scaled_valid) {
            out.scaled = uint64_t(s.scaled->get_image());
            out.scaled_width = s.scaled_width;
            out.scaled_height = s.scaled_height;
        }
        taken_ = pick;
        return true;
    }

    std::optional<uint64_t> newest_tag() {
        std::lock_guard<std::mutex> hold(lock_);
        if (newest_ < 0) {
            return std::nullopt;
        }
        return slots_[newest_].tag;
    }

    void release() {
        std::lock_guard<std::mutex> hold(lock_);
        taken_ = -1;
    }

    // An RGBA8 image in the GENERAL layout, for the scanout and the picture: copied into and out of, and read by
    // the presenter (a copy, a blit or a shader)
    static ImageHandle make_picture_image(Device &device, uint32_t width, uint32_t height) {
        ImageCreateInfo info = ImageCreateInfo::render_target(width, height, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        return device.create_image(info, nullptr);
    }

private:
    struct Slot {
        ImageHandle image;
        uint32_t width = 0, height = 0;
        ImageHandle scaled, scaled_work; // (scaled: the picture at the window's size; work: before the sharpening)
        uint32_t scaled_width = 0, scaled_height = 0;
        bool scaled_valid = false; // (scaled is this picture's)
        uint64_t serial = 0; // 0: none published in it (or it is being written)
        uint64_t tag = 0;
    };
    std::mutex lock_;
    std::array<Slot, SCANOUT_SLOTS> slots_;
    int newest_ = -1;  // the slot published last
    int written_ = -1; // the slot written last
    int taken_ = -1;   // the slot the presenter holds
    uint64_t serial_ = 0;
};

// ---- The display registers ---------------------------------------------------------------------------
uint32_t bits(uint64_t value, int first, int count) {
    return uint32_t((value >> first) & ((uint64_t(1) << count) - 1));
}

// GS_CSR's FIELD at a vblank when the field begun is the one an interlaced TV shows a line lower (the odd lines)
constexpr int LOWER_FIELD = 1;

// The buffer the display shows (circuit 2, else 1; a field's lines in field mode)
bool display_rect(const uint64_t *privileged, GsnRect &rect) {
    uint64_t pmode = privileged[0], smode2 = privileged[2 * 2];
    int circuit = (pmode & 2) ? 2 : (pmode & 1) ? 1 : 0;
    if (!circuit) {
        return false;
    }
    uint64_t dispfb = privileged[(circuit == 2 ? 9 : 7) * 2], display = privileged[(circuit == 2 ? 10 : 8) * 2];
    rect.bp = bits(dispfb, 0, 9) * 32;
    rect.bw = bits(dispfb, 9, 6);
    rect.psm = bits(dispfb, 15, 5);
    rect.x = bits(dispfb, 32, 11);
    rect.y = bits(dispfb, 43, 11);
    rect.w = (bits(display, 32, 12) + 1) / (bits(display, 23, 4) + 1);
    rect.h = bits(display, 44, 11) + 1;
    if ((smode2 & 3) == 3) {
        rect.h /= 2; // field mode: a field's picture
    }
    return rect.w > 0 && rect.h > 0 && rect.w <= 1024 && rect.h <= 1024;
}

// (the DMA controller's thread, which tells of every vblank and its pictures between) The screens the game draws as
// fields (the menus, the title), with whole frames (port_whole_frame(): port/platform/field_frames.c): each vblank's
// lower field shown from the upper field's buffer, which field_frames drew again for the same moment and told of just
// before. The game draws the two half a line apart, rounded to a whole line of the field where its pictures are on
// whole pixels: shown in turn, a line apart or the field's lines moved as on a TV, every other picture was a line
// higher or lower (the menus shook). Without whole frames, the other buffer holds the moment before: the lower field
// is moved a line down instead (Canvas::scanout, ShownRect's lower_field).
class UpperField {
public:
    void show_in(VblankPicture &v, bool extra) {
        uint64_t smode2 = v.privileged[2 * 2];
        if ((smode2 & 3) != 3 || !v.deinterlace || !port_whole_frame()) {
            dispfb_.reset();
            return;
        }
        if (v.field != LOWER_FIELD) {
            if (!extra) {
                dispfb_ = std::array<uint64_t, 2>{v.privileged[DISPFB1], v.privileged[DISPFB2]};
            }
            return;
        }
        if (dispfb_) {
            v.privileged[DISPFB1] = (*dispfb_)[0];
            v.privileged[DISPFB2] = (*dispfb_)[1];
            v.field = LOWER_FIELD ^ 1;
        }
    }

private:
    static constexpr size_t DISPFB1 = 7 * 2, DISPFB2 = 9 * 2; // (privileged's words)
    std::optional<std::array<uint64_t, 2>> dispfb_; // the upper field's buffers, as the last vblank of it told
};

struct PictureSize {
    uint32_t width, height;
};

// The picture's size at a vblank, from the buffer shown at scale sx x sy: a
// field-mode picture woven with the one before (--deinterlace) or alone; a frame-mode picture whole (progressive),
// or a field's lines (deinterlaced to the whole height, or alone). A picture between is whole or none.
std::optional<PictureSize> picture_size(const VblankPicture &v, bool extra, uint32_t sx, uint32_t sy) {
    GsnRect rect;
    if (!display_rect(v.privileged.data(), rect)) {
        return std::nullopt;
    }
    uint64_t smode2 = v.privileged[2 * 2];
    bool interlaced = smode2 & 1, field_mode = (smode2 >> 1) & 1;
    uint32_t lines = rect.h;
    bool whole = true;
    if (interlaced && field_mode) {
        whole = !v.deinterlace;
        lines = v.deinterlace ? rect.h * 2 : rect.h;
    } else if (interlaced && !v.progressive) {
        whole = false;
        lines = v.deinterlace ? rect.h : rect.h / 2;
    }
    if (extra && !whole) {
        return std::nullopt;
    }
    if (lines == 0 || size_t(rect.w) * sx * lines * sy > PORT_GS_FRAME_PIXELS) {
        return std::nullopt;
    }
    return PictureSize{rect.w * sx, lines * sy};
}

// ---- The settings: kept for the steps that draw -------------------------------------------------------
struct Settings {
    std::atomic<uint32_t> scale{1 | 1 << 8}; // across | down << 8, from the next vblank
    std::atomic<int> lod{GSN_LOD_SCALE}, aniso{1}, msaa{1}, cutout_aa{GSN_CUTOUT_AA_OFF}, mode_2d{GSN_2D_PS2},
        colour{GSN_COLOUR_EXACT}, layer_z{GSN_LAYER_Z_PS2}, native_draw{GSN_NATIVE_DRAW_NATIVE};
    std::atomic<uint32_t> present_size{0}, present_sharpen{0};
};

// ---- --stats ---------------------------------------------------------------------------------------
struct Stats {
    std::atomic<uint64_t> packets{0}, bytes{0}, vblanks{0}, pictures{0}, extras{0}, interp_frames{0},
        interp_pictures{0}, interp_late{0};
    // (Renderer::interp_picture) pictures between sent to the pump directly, and of them those kf4-pc then found
    // the frame's kept walk no longer fitting for (the store changed by the next frame's packets before them)
    std::atomic<uint64_t> direct{0}, direct_stale{0};
};

// (kf4-pc) Each frame's intake, for --stats: the time kf4-pc spent taking its packets in (busy), and from its first
// packet to its pictures between handed their frame (ready: what a picture between of it waits for at the soonest)
class IntakeTimes {
public:
    using Clock = std::chrono::steady_clock;

    void packet_taken(Clock::time_point start, Clock::time_point end) {
        if (!in_frame_) {
            in_frame_ = true;
            first_ = start;
        }
        busy_ += end - start;
    }

    void frame_taken(Clock::time_point end) {
        if (in_frame_ && frames_.size() < MOST) {
            frames_.push_back({ms(busy_), ms(end - first_)});
        }
        in_frame_ = false;
        busy_ = {};
    }

    // "mean, median, 95th and 99th percentile, most", busy then ready
    void print(FILE *out) const {
        if (frames_.empty()) {
            return;
        }
        std::vector<float> busy, ready;
        for (const Frame &f : frames_) {
            busy.push_back(f.busy);
            ready.push_back(f.ready);
        }
        auto line = [&](std::vector<float> &v) {
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (float x : v) {
                sum += x;
            }
            auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
            fprintf(out, "mean %.3f, median %.3f, p95 %.3f, p99 %.3f, most %.3f", sum / double(v.size()), at(0.5),
                    at(0.95), at(0.99), double(v.back()));
        };
        fprintf(out, "[stats] pc intake a frame (%zu frames, ms): busy ", frames_.size());
        line(busy);
        fprintf(out, "; ready ");
        line(ready);
        fprintf(out, "\n");
    }

private:
    static constexpr size_t MOST = 1 << 20;
    struct Frame {
        float busy, ready;
    };
    static float ms(Clock::duration d) {
        return std::chrono::duration<float, std::milli>(d).count();
    }
    bool in_frame_ = false;
    Clock::time_point first_;
    Clock::duration busy_{};
    std::vector<Frame> frames_;
};

// KF4_PC_PUMP=0: no pump (every picture between drawn on kf4-pc, as before)
bool pump_wanted() {
    const char *v = getenv("KF4_PC_PUMP");
    return !(v && strcmp(v, "0") == 0);
}

// ---- What the GIF packets make ------------------------------------------------------------------------
// (the PC renderer's thread) The front end's events, drawn by the canvas; with KF4_PC_CENSUS=1 counted for the census
class Intake final : public pcr::GifEvents {
public:
    // (on the thread, once the device is there)
    void attach(Device &device) {
        times_ = std::make_unique<pcr::GpuTimes>(device);
        canvas_ = std::make_unique<pcr::Canvas>(device, *times_);
        natives_ = std::make_unique<pcr::NativeDraws>(device);
        between_ = std::make_unique<pcr::Between>(device);
    }

    // (pcr_between.h) The frame for the pictures between. The front end's state now is where each of its walks
    // starts, so that every walk of the frame makes the same calls.
    //
    // A frame walked once (pcr_frame_walk.h): the frame's own walk, kept (the last stretch of packets between two
    // vblanks), is made its pictures' walk when the frame is its packets, and the rules make the stream's of it.
    void take_frame(InterpFrame frame) {
        if (!between_) {
            return;
        }
        drop_stretch();
        frame_taken_at_ = vblanks_;
        const pcr::FrameTargets targets = {frame->draw_fbp, frame->shown_fbp, frame->uploads_asked_again != 0,
                                           frame->shown_kept != 0};
        const uint64_t packet_bytes = frame->packet_bytes;
        between_->take_frame(std::move(frame));
        walk_start_ = front_.state();
        kept_walk_.reset();
        if (own_walks_) {
            pcr::FrameWalkRefused refused = own_walk(packet_bytes, targets);
            between_->frame_from_own_walk(refused);
        }
        last_stretch_.reset();
    }

    // What a picture between leaves to the pump: the picture, with the frame first when the pump has not this walk
    struct ForPump {
        std::optional<PumpFrame> frame;
    };

    // A picture between: drawn again from the frame's kept walk when what that read of the store is as it was (step
    // 5b): by the pump when there is one (returned: the picture is the pump's, nothing drawn here); else the frame's
    // packets walked again from the frame's state, its kicks numbered, the canvas's and the native draws' corners
    // moved by the shaders to the picture's time, and the walk kept. The live state is put back after.
    std::optional<ForPump> walk_picture(const GsnInterpPicture &picture) {
        if (!between_ || !canvas_) {
            return std::nullopt;
        }
        drop_stretch();
        pcr::WalkReason why = pcr::WalkReason::none_kept;
        if (kept_walk_ && keep_walks_) {
            if (canvas_->fits(*kept_walk_)) {
                if (pump_on_) {
                    return to_pump(picture);
                }
                if (between_->begin_picture_again(picture)) {
                    canvas_->draw_again(*kept_walk_, *between_);
                    between_->end_picture_again();
                }
                return std::nullopt;
            }
            why = pcr::WalkReason::store_changed;
        }
        kept_walk_.reset();
        std::span<const uint8_t> stream = between_->begin_picture(picture);
        if (stream.empty()) {
            return std::nullopt;
        }
        between_->walked(why);
        const pcr::GifFrontEnd::State live = front_.state();
        front_.set_state(walk_start_);
        if (keep_walks_) {
            canvas_->keep_walk();
        }
        canvas_->set_between(between_.get());
        natives_->set_picture(&picture);
        front_.number_kicks();
        for (size_t at = 0; at + 8 <= stream.size();) {
            uint32_t head[2];
            memcpy(head, stream.data() + at, 8);
            if (head[1] > stream.size() - at - 8) {
                break; // (never: frame_interp.c wrote it whole)
            }
            front_.feed(int(head[0]), stream.subspan(at + 8, head[1]));
            at += 8 + size_t(head[1]);
        }
        front_.stop_numbering();
        natives_->set_picture(nullptr);
        canvas_->set_between(nullptr); // (the walk's last batch drawn while the picture's buffers are bound)
        bool kicks_right = between_->end_picture(front_.kicks_numbered());
        front_.set_state(live);
        if (keep_walks_) {
            kept_walk_ = canvas_->take_kept_walk();
            if (!kicks_right || natives_->moved_on_cpu()) {
                kept_walk_.reset();
            }
            if (!kept_walk_) {
                between_->walked(pcr::WalkReason::not_kept);
            }
        }
        return std::nullopt;
    }

    // A picture between the pump was sent directly (Renderer::interp_picture: the frame's walk was the pump's when it
    // was sent), taken in turn: counted as walk_picture counts one it gives the pump. False when walk_picture would not
    // have given it to the pump now (the frame's kept walk no longer fits the store: the next frame's packets before it
    // changed what it read); the pump drew it from the walk all the same.
    bool direct_picture(const GsnInterpPicture &picture) {
        if (!between_ || !canvas_) {
            return false;
        }
        drop_stretch();
        if (!kept_walk_ || !keep_walks_ || !pump_on_ || kept_walk_ != pump_walk_ || !canvas_->fits(*kept_walk_)) {
            return false;
        }
        return between_->hand_over(picture).has_value();
    }

    // The picture to the pump (nullopt: not of the frame, counted, nothing drawn), with the frame when the pump has
    // not this walk: kf4-pc's surfaces copied now, where the picture would have been drawn on them
    std::optional<ForPump> to_pump(const GsnInterpPicture &picture) {
        std::optional<pcr::Between::Walked> walked = between_->hand_over(picture);
        if (!walked) {
            return std::nullopt;
        }
        ForPump out;
        if (kept_walk_ != pump_walk_) {
            out.frame = PumpFrame{kept_walk_, std::move(*walked), canvas_->copy_surfaces()};
            pump_walk_ = kept_walk_;
        }
        return out;
    }

    pcr::Canvas *canvas() {
        return canvas_.get();
    }

    pcr::GpuTimes *gpu_times() {
        return times_.get();
    }

    pcr::GifFrontEnd &front() {
        return front_;
    }

    Intake() : front_(*this) {
        const char *keep = getenv("KF4_PC_KEPT_WALK");
        keep_walks_ = !(keep && strcmp(keep, "0") == 0);
        pump_on_ = pump_wanted();
        const char *own = getenv("KF4_PC_LIVE_WALK");
        own_walks_ = keep_walks_ && !(own && strcmp(own, "0") == 0);
        const char *census = getenv("KF4_PC_CENSUS");
        if (census && strcmp(census, "1") == 0) {
            census_ = std::make_unique<pcr::Census>();
        }
    }

    void feed(int path, const std::vector<uint8_t> &bytes) {
        if (own_walks_ && !stretch_open_ && !in_replay_ && vblanks_ <= frame_taken_at_ + STRETCHES_KEPT_VBLANKS) {
            open_stretch();
        }
        stretch_bytes_ += bytes.size();
        front_.feed(path, bytes);
    }

    // A state loaded: no stretch kept from before it
    void forget() {
        drop_stretch();
        last_stretch_.reset();
        in_replay_ = false;
    }

    // scale: across | down << 8; lod and aniso: texture detail's and anisotropy's (Canvas::set_sampling); samples and
    // cutout_aa: MSAA's (Canvas::set_msaa); mode_2d and colour: Canvas::set_2d's and set_colour's
    void vblank(const uint64_t *privileged, uint32_t scale, int lod, int aniso, uint32_t samples, bool cutout_aa,
                int mode_2d, int colour) {
        vblanks_++;
        if (stretch_open_) {
            last_stretch_ = close_stretch();
        }
        if (canvas_) {
            canvas_->set_scale(scale & 0xFF, scale >> 8);
            canvas_->set_sampling(lod, uint32_t(aniso));
            canvas_->set_msaa(samples, cutout_aa);
            canvas_->set_2d(mode_2d);
            canvas_->set_colour(colour);
            canvas_->vblank();
            times_->collect();
        }
        if (census_) {
            census_->display(privileged[0], privileged[14], privileged[18]);
        }
    }

    void print_census() const {
        if (canvas_) {
            canvas_->print_counts();
        }
        if (natives_) {
            natives_->print_counts(stderr);
        }
        if (between_) {
            between_->print_counts(stderr);
        }
        if (census_) {
            census_->print();
        }
    }

private:
    void register_written(uint32_t, uint64_t, uint64_t) override {}
    void texture_written(uint32_t, uint64_t tex0, uint64_t texclut) override {
        if (canvas_) {
            canvas_->texture_written(tex0, texclut);
        }
    }
    void primitive(const pcr::GifPrimitive &primitive) override {
        if (census_) {
            census_->primitive(front_.registers(), primitive);
        }
        if (canvas_) {
            canvas_->primitive(front_.registers(), primitive);
        }
    }
    void image_upload(const pcr::TransferRegisters &transfer, std::span<const uint8_t> bytes, pcr::UploadEnd) override {
        if (census_) {
            census_->upload(transfer, bytes.size());
        }
        if (canvas_) {
            canvas_->upload(transfer, bytes);
        }
    }
    void local_copy(const pcr::TransferRegisters &transfer) override {
        if (census_) {
            census_->copy(transfer, "local");
        }
        if (canvas_) {
            canvas_->local_copy(transfer);
        }
    }
    void local_to_host(const pcr::TransferRegisters &transfer) override {
        if (census_) {
            census_->copy(transfer, "read-back");
        }
        if (canvas_) {
            canvas_->local_to_host(transfer);
        }
    }
    void native_draw(uint64_t value) override {
        if (census_) {
            census_->other("native draw (0x7B)");
        }
        if (natives_ && canvas_) {
            natives_->met(value, front_, *canvas_);
        } else if (RD_ND_COMMAND(value) == RD_ND_FRAME) {
            rd_consumer_frame(value); // (the core waits for the frames' marks to be met)
        }
    }
    void upload_again(uint64_t value) override {
        if (census_) {
            census_->other("upload again (0x7C)");
        }
        if (canvas_) {
            canvas_->upload_again(front_.registers(), value);
        }
    }
    void frame_mark(uint64_t value) override {
        if (census_) {
            census_->other("frame mark (0x7D)");
        }
        // (frame_interp.c's replays sent as packets, between 0xF0 and 0xF1 (the exact restore): not the game's, no
        // stretch of them kept)
        uint32_t index = uint32_t(value >> 32) & 0xFF;
        if (index == 0xF0 && !between_active()) {
            drop_stretch();
            in_replay_ = true;
        } else if (index == 0xF1 && !between_active()) {
            in_replay_ = false;
        }
        if (canvas_) {
            canvas_->frame_mark(value);
        }
        if (natives_) {
            natives_->frame_mark(value);
        }
    }
    void kick_numbered(const pcr::GifVertex &vertex, uint32_t prim) override {
        if (between_) {
            between_->kicked(vertex, front_.registers().reg[pcr::gs_reg::xyoffset_1 + ((prim >> 9) & 1)]);
        }
    }
    void widescreen(uint64_t) override {}
    void gs_sync(uint32_t, uint64_t) override {}

    // (a frame walked once) The packets from here kept as they are walked: the canvas's calls, the kicks numbered, the
    // native draws' records for the pictures made beside the frame's
    struct Stretch {
        std::shared_ptr<const pcr::KeptWalk> walk; // (nullptr: not kept whole)
        pcr::Between::KickList kicks;
        uint64_t bytes = 0;
        bool trail_paired = false;
    };
    void open_stretch() {
        if (!canvas_ || !natives_ || !between_) {
            return;
        }
        canvas_->keep_walk();
        front_.number_kicks();
        between_->keep_live_kicks();
        natives_->begin_frame_kept();
        stretch_open_ = true;
        stretch_bytes_ = 0;
    }
    Stretch close_stretch() {
        Stretch s;
        canvas_->stop_keeping();
        s.walk = canvas_->take_kept_walk();
        front_.stop_numbering();
        s.kicks = between_->end_live_kicks();
        natives_->end_frame_kept();
        s.trail_paired = natives_->trail_paired();
        s.bytes = stretch_bytes_;
        stretch_open_ = false;
        return s;
    }
    bool between_active() const {
        return between_ && between_->active();
    }
    // A picture between, a frame taken or a state loaded in a stretch: what it kept is not a frame's own walk
    void drop_stretch() {
        if (stretch_open_) {
            close_stretch();
        }
    }
    // The last stretch as the frame's pictures' walk (kept_walk_, its kicks the frame's walked kicks), or why not
    pcr::FrameWalkRefused own_walk(uint64_t packet_bytes, const pcr::FrameTargets &targets) {
        if (!last_stretch_ || !last_stretch_->walk) {
            return pcr::FrameWalkRefused::not_kept;
        }
        Stretch &s = *last_stretch_;
        if (s.bytes != packet_bytes) {
            return pcr::FrameWalkRefused::not_the_frame;
        }
        if (s.trail_paired) {
            return pcr::FrameWalkRefused::trail_paired;
        }
        pcr::Canvas::PictureWalk picture = canvas_->as_picture_walk(*s.walk, targets);
        if (!picture.walk) {
            return picture.refused;
        }
        if (!between_->adopt_live_kicks(std::move(s.kicks))) {
            return pcr::FrameWalkRefused::not_the_frame; // (its kicks not the frame's count)
        }
        kept_walk_ = std::move(picture.walk);
        return pcr::FrameWalkRefused::none;
    }

    pcr::GifFrontEnd front_;
    std::unique_ptr<pcr::Census> census_;
    std::unique_ptr<pcr::GpuTimes> times_; // (before the canvas: it outlives it)
    std::unique_ptr<pcr::Canvas> canvas_;
    std::unique_ptr<pcr::NativeDraws> natives_;
    std::unique_ptr<pcr::Between> between_;
    // (step 5b) where the frame's walks start, the frame's kept walk, and KF4_PC_KEPT_WALK (0: every picture walks)
    pcr::GifFrontEnd::State walk_start_;
    std::shared_ptr<const pcr::KeptWalk> kept_walk_;
    bool keep_walks_ = true;
    // (the pump) KF4_PC_PUMP (0: none), and the walk it was handed last (held: never another's address)
    bool pump_on_ = true;
    std::shared_ptr<const pcr::KeptWalk> pump_walk_;
    // (a frame walked once) the stretch being kept and its bytes, the last kept at a vblank, and KF4_PC_LIVE_WALK (0:
    // the frame's stream walked for its first picture, as before)
    // (kept only while frames are taken: none at 30, in the menus)
    static constexpr uint64_t STRETCHES_KEPT_VBLANKS = 8;
    bool stretch_open_ = false, in_replay_ = false;
    uint64_t stretch_bytes_ = 0, vblanks_ = 0, frame_taken_at_ = 0;
    std::optional<Stretch> last_stretch_;
    bool own_walks_ = true;
};

// ---- A picture made, and handed out ------------------------------------------------------------------
// The pictures' colour where nothing is shown: KF4_PC_CLEAR=RRGGBB (black without it)
struct ClearColour {
    uint32_t rgba; // R in the low byte, alpha 255
    VkClearValue value;
};

ClearColour clear_colour() {
    uint32_t rgb = 0;
    const char *asked = getenv("KF4_PC_CLEAR");
    if (asked) {
        char *end = nullptr;
        unsigned long v = strtoul(asked, &end, 16);
        if (end == asked || *end != '\0' || v > 0xFFFFFF) {
            fprintf(stderr, "[pcr] KF4_PC_CLEAR wants RRGGBB in hex, not %s: black\n", asked);
        } else {
            rgb = uint32_t(v);
        }
    }
    uint32_t r = rgb >> 16, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    ClearColour c = {};
    c.rgba = 0xFF000000u | b << 16 | g << 8 | r;
    c.value.color.float32[0] = float(r) / 255.0f;
    c.value.color.float32[1] = float(g) / 255.0f;
    c.value.color.float32[2] = float(b) / 255.0f;
    c.value.color.float32[3] = 1.0f;
    return c;
}

// ---- The presenter's scaling --------------------------------------------------------------------------
// The picture scaled to the size the window shows it at (pcr_set_present_scale: the presenter's, SDL_Renderer's, or
// --dump-present's), on the GPU after its copy into the scanout ring, with shaders/pcr_present_scale.frag (a
// Catmull-Rom cubic, stretched by the ratio when the picture is made smaller, on linear light) and, with a strength,
// shaders/pcr_present_sharpen.frag after it; the presenter then copies it into the window a pixel a pixel. None when
// the presenter scales by itself (sharp or smooth, and no sharpening). One for each thread that makes pictures.
class PresentScaler {
public:
    explicit PresentScaler(Device &device) : device_(device) {}

    // picture into scaled, or with sharpen above 0 into work and sharpened from it into scaled
    void scale(CommandBuffer &cmd, const Image &picture, const Image &scaled, const Image *work, float sharpen) {
        if (!scale_) {
            make_programs();
        }
        uint32_t pw = picture.get_width(), ph = picture.get_height();
        uint32_t w = scaled.get_width(), h = scaled.get_height();
        struct {
            int32_t src[2];
            float ratio[2];
            int32_t linear;
        } scaling = {{int32_t(pw), int32_t(ph)}, {float(pw) / float(w), float(ph) / float(h)}, 1};
        bool sharpened = sharpen > 0 && work;
        pass(cmd, sharpened ? *work : scaled, scale_, picture.get_view(), &scaling, sizeof(scaling));
        if (sharpened) {
            struct {
                int32_t size[2];
                float strength;
            } sharpening = {{int32_t(w), int32_t(h)}, sharpen};
            pass(cmd, scaled, sharpen_, work->get_view(), &sharpening, sizeof(sharpening));
        }
    }

private:
    void make_programs() {
        ResourceLayout vert = {};
        Shader *vs = device_.request_shader(pcr_spv_pcr_present_vert, sizeof(pcr_spv_pcr_present_vert), &vert);
        ResourceLayout frag = {};
        frag.output_mask = 1;
        frag.sets[0].sampled_image_mask = 1;
        frag.sets[0].fp_mask = 1;
        frag.sets[0].meta[0].array_size = 1;
        frag.push_constant_size = 5 * sizeof(int32_t);
        scale_ = device_.request_program(
            vs, device_.request_shader(pcr_spv_pcr_present_scale_frag, sizeof(pcr_spv_pcr_present_scale_frag), &frag));
        frag.push_constant_size = 3 * sizeof(int32_t);
        sharpen_ = device_.request_program(
            vs, device_.request_shader(pcr_spv_pcr_present_sharpen_frag, sizeof(pcr_spv_pcr_present_sharpen_frag), &frag));
    }

    // A pass over the whole of dst, reading src (after whatever wrote it; read after by anything)
    static void pass(CommandBuffer &cmd, const Image &dst, Program *program, const ImageView &src, const void *push,
                     size_t push_size) {
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &dst.get_view();
        rp.store_attachments = 1;
        cmd.begin_render_pass(rp);
        cmd.set_opaque_state();
        cmd.set_program(program);
        cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        cmd.set_cull_mode(VK_CULL_MODE_NONE);
        cmd.set_depth_test(false, false);
        cmd.set_texture(0, 0, src, StockSampler::NearestClamp);
        cmd.push_constants(push, 0, push_size);
        cmd.draw(3);
        cmd.end_render_pass();
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    Device &device_;
    Program *scale_ = nullptr, *sharpen_ = nullptr;
};

Settings &settings(); // (below)

// The presenter's callbacks (pcr_set_presenter): any thread sets them, the threads that hand pictures out call them
struct PresenterHooks {
    std::atomic<void (*)(const PortGsFrame *, uint64_t)> pixels{nullptr};
    std::atomic<void (*)(uint64_t)> image{nullptr};
};

// A picture made (its commands submitted, read back when asked), to be handed out: published in the scanout ring, given
// to the presenter, its sender answered. By the thread that made it, or by the pump after the pictures before it.
struct MadePicture {
    int want = 0, ok = 0, slot = -1;
    bool scaled_pixels = false;
    bool drawn = false;  // (its commands submitted: kf4-pc moves Granite's frame context on)
    bool vblank = false; // (a vblank's own picture: the vblank done once handed out)
    const PortGsFrame *frame = nullptr;
    uint64_t tag = 0;
    Reply *reply = nullptr;
};

// (kf4-pc or the pump, as it hands out the extra picture that shows a picture between) --frame-rate unlocked: not to the
// presenter when it is too late to show (pcr_late.h). 1 if it was left out.
int not_shown_when_late(MadePicture &made, const GsnInterpPicture &between) {
    if (!between.leave_out_late || made.vblank || made.reply != nullptr ||
        !pcr::too_late_to_show(between.shown_s, port_wall_seconds())) {
        return 0;
    }
    made.want &= ~GSN_PRESENT;
    return 1;
}

// The pictures one thread makes (kf4-pc's, the pump's), as pcr_picture hands them out: GSN_WANT_IMAGE when the scanout
// ring has it, GSN_WANT_PIXELS when frame has it, and with GSN_PRESENT to the presenter once it is there
class PictureOut {
public:
    PictureOut(Device &device, ScanoutRing &scanout, PresenterHooks &presenter, Stats &stats, const ClearColour &clear)
        : device_(device), scanout_(scanout), presenter_(presenter), stats_(stats), clear_(clear), scaler_(device) {}

    // A vblank's picture, or with GSN_EXTRA one more between two (the buffer shown again: a picture between drawn into
    // it before), from canvas at scale (across | down << 8, the vblank's)
    MadePicture make(const VblankPicture &v, uint32_t scale, pcr::Canvas *canvas, pcr::GpuTimes &times) {
        MadePicture m;
        bool extra = (v.want & GSN_EXTRA) != 0;
        m.want = v.want;
        m.tag = v.tag;
        m.reply = v.reply;
        m.vblank = !extra;
        PortGsFrame *frame = v.frame;
        // SDL_Renderer's picture at the window's size (GSN_WANT_SCALED with GSN_PRESENT and GSN_WANT_IMAGE): no
        // scaled picture yet, so the picture's own pixels for the presenter
        int want = v.want;
        m.scaled_pixels = (want & GSN_PRESENT) && (want & GSN_WANT_SCALED) && (want & GSN_WANT_IMAGE) &&
                          !(want & GSN_WANT_PIXELS);
        bool present_pixels =
            ((want & GSN_PRESENT) && !(want & (GSN_WANT_IMAGE | GSN_WANT_PIXELS))) || m.scaled_pixels;
        if (present_pixels) {
            frame = &presented_;
        }
        m.frame = frame;
        stats_.pictures++;
        if (extra) {
            stats_.extras++;
        } else {
            stats_.vblanks++;
        }
        std::optional<PictureSize> size = picture_size(v, extra, scale & 0xFF, scale >> 8);
        if (!size || !(want & (GSN_WANT_IMAGE | GSN_WANT_PIXELS | GSN_PRESENT))) {
            device_.flush_frame(); // (a picture between submitted for this picture to flush: on the GPU's queue now)
            return m; // (no picture shown, or none wanted)
        }
        CommandBufferHandle cmd = device_.request_command_buffer();
        times.begin(*cmd, pcr::GpuWork::picture);
        m.drawn = true;
        if (!draw_picture(*cmd, *size, v, canvas)) {
            times.end(*cmd);
            submit_now(cmd);
            return m;
        }
        times.picture();
        port_profile_picture(); // (a picture drawn: the profiler's GPU time a picture divides by these)
        const Image *scaled = nullptr;
        if (want & GSN_WANT_IMAGE) {
            m.slot = scanout_.claim(device_, picture_->get_width(), picture_->get_height());
            if (m.slot >= 0) {
                copy_to_scanout(*cmd, scanout_.image(m.slot));
                m.ok |= GSN_WANT_IMAGE;
                scaled = scale_for_window(*cmd, m.slot, times);
            }
        }
        if (frame && ((want & GSN_WANT_PIXELS) || present_pixels)) {
            // (GSN_WANT_SCALED: the picture as the window shows it, when there is a scaled one)
            if (read_back(cmd, (want & GSN_WANT_SCALED) && scaled ? *scaled : *picture_, *frame, times)) {
                m.ok |= GSN_WANT_PIXELS;
            }
        } else {
            times.end(*cmd);
            submit_now(cmd);
        }
        return m;
    }

    // Whether handing m out reads this one's own pixels (which its next picture writes over)
    bool own_pixels(const MadePicture &m) const {
        return m.frame == &presented_;
    }

    // m published, given to the presenter, its sender answered, its vblank done
    void hand_out(const MadePicture &m, Inbox &inbox) {
        if (m.slot >= 0) {
            scanout_.publish(m.slot, m.tag);
        }
        if (m.ok && (m.want & GSN_PRESENT)) {
            hand_to_presenter(m);
        }
        if (m.reply) {
            inbox.answer(m.reply, m.ok);
        }
        if (m.vblank) {
            inbox.vblank_handed_out();
        }
    }

private:
    // cmd submitted and on the GPU's queue, before the picture is handed out: Granite holds a submit without a fence
    // until the next frame context, which only kf4-pc moves on (the pump's pictures would be shown before they are
    // drawn, and drawn up to a vblank late)
    void submit_now(CommandBufferHandle &cmd) {
        device_.submit(cmd);
        device_.flush_frame();
    }

    // The picture: the canvas's surface the display shows, scaled to its size (cleared to the picture's colour where
    // there is none). False if the image cannot be made.
    bool draw_picture(CommandBuffer &cmd, PictureSize size, const VblankPicture &v, pcr::Canvas *canvas) {
        if (!picture_ || picture_->get_width() != size.width || picture_->get_height() != size.height) {
            picture_ = ScanoutRing::make_picture_image(device_, size.width, size.height);
            if (!picture_) {
                fprintf(stderr, "[pcr] no image for a %ux%u picture\n", size.width, size.height);
                return false;
            }
        }
        // (after whatever read or wrote it before: the copies of the last picture)
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        GsnRect rect;
        bool shown = false;
        if (canvas && display_rect(v.privileged.data(), rect)) {
            uint64_t smode2 = v.privileged[2 * 2];
            bool field_doubled = (smode2 & 3) == 3 && v.deinterlace;
            pcr::ShownRect shown_rect = {rect.bp, rect.bw, rect.psm, rect.x, rect.y, rect.w, rect.h,
                                         uint32_t(field_doubled && v.field == LOWER_FIELD)};
            shown = canvas->scanout(cmd, shown_rect, *picture_);
        }
        if (!shown) {
            cmd.clear_image(*picture_, clear_.value);
        }
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        return true;
    }

    // The picture scaled to the window's size into the slot's scaled image (PresentScaler), when the presenter asks for
    // it (pcr_set_present_scale); that image, or null
    const Image *scale_for_window(CommandBuffer &cmd, int slot, pcr::GpuTimes &times) {
        uint32_t size = settings().present_size.load(std::memory_order_relaxed);
        uint32_t w = size >> 16, h = size & 0xFFFF;
        if (w == 0 || h == 0 || w > 8192 || h > 8192) {
            return nullptr;
        }
        float sharpen = float(settings().present_sharpen.load(std::memory_order_relaxed)) / 1000.0f;
        ScanoutRing::Scaled images = scanout_.scaled(device_, slot, w, h, sharpen > 0);
        if (!images.scaled) {
            return nullptr;
        }
        times.begin(cmd, pcr::GpuWork::present);
        scaler_.scale(cmd, *picture_, *images.scaled, images.work, sharpen);
        times.begin(cmd, pcr::GpuWork::picture);
        return images.scaled;
    }

    // The picture into a slot of the scanout ring, after whatever the presenter submitted before (it reads the
    // slot after a barrier of its own)
    void copy_to_scanout(CommandBuffer &cmd, const Image &slot) {
        const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        cmd.copy_image(slot, *picture_, {0, 0, 0}, {0, 0, 0}, {picture_->get_width(), picture_->get_height(), 1},
                       colour, colour);
        cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    // The picture read back into frame (alpha 255): submitted with the commands so far, and waited for. False
    // (submitted all the same) if there is no buffer to read it into.
    bool read_back(CommandBufferHandle &cmd, const Image &image, PortGsFrame &frame, pcr::GpuTimes &times) {
        uint32_t width = image.get_width(), height = image.get_height();
        VkDeviceSize size = VkDeviceSize(width) * height * 4;
        if (!readback_ || readback_->get_create_info().size < size) {
            BufferCreateInfo info = {};
            info.domain = BufferDomain::CachedHost;
            info.size = size;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            readback_ = device_.create_buffer(info, nullptr);
            if (!readback_) {
                fprintf(stderr, "[pcr] no buffer to read a picture back into\n");
                times.end(*cmd);
                submit_now(cmd);
                return false;
            }
        }
        cmd->copy_image_to_buffer(*readback_, image, 0, {0, 0, 0}, {width, height, 1}, 0, 0,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
        Fence fence;
        times.end(*cmd);
        device_.submit(cmd, &fence);
        fence->wait();
        if (&frame == &presented_ && presented_pixels_.size() < size_t(width) * height) {
            presented_pixels_.resize(size_t(width) * height);
            presented_.rgba = presented_pixels_.data();
        }
        const uint8_t *mapped = static_cast<const uint8_t *>(device_.map_host_buffer(*readback_, MEMORY_ACCESS_READ_BIT));
        if (!mapped) {
            fprintf(stderr, "[pcr] the read-back buffer could not be mapped\n");
            return false;
        }
        for (size_t i = 0; i < size_t(width) * height; i++) {
            uint32_t pixel;
            memcpy(&pixel, mapped + i * 4, 4);
            frame.rgba[i] = pixel | 0xFF000000u;
        }
        device_.unmap_host_buffer(*readback_, MEMORY_ACCESS_READ_BIT);
        frame.width = width;
        frame.height = height;
        return true;
    }

    // The window's picture, now that it is there: its pixels, or word that the image is published
    void hand_to_presenter(const MadePicture &m) {
        void (*pixels)(const PortGsFrame *, uint64_t) = presenter_.pixels.load();
        void (*image)(uint64_t) = presenter_.image.load();
        if (m.scaled_pixels && (m.ok & GSN_WANT_PIXELS) && pixels) {
            pixels(m.frame, m.tag);
        } else if ((m.ok & GSN_WANT_IMAGE) && image && !m.scaled_pixels) {
            image(m.tag);
        } else if (!(m.want & GSN_WANT_IMAGE) && (m.ok & GSN_WANT_PIXELS) && pixels) {
            pixels(m.frame, m.tag);
        }
    }

    Device &device_;
    ScanoutRing &scanout_;
    PresenterHooks &presenter_;
    Stats &stats_;
    const ClearColour &clear_;
    PresentScaler scaler_;
    ImageHandle picture_;
    BufferHandle readback_;
    std::vector<uint32_t> presented_pixels_;
    PortGsFrame presented_ = {0, 0, nullptr};
};

// ---- The pump: the pictures between drawn again on a thread of their own ------------------------------
// docs/port/PERFORMANCE.md, item 1. While kf4-pc takes the next frame's packets in, the pump ("kf4-pc-pump") draws the
// pictures between again from the frame's kept walk, on a canvas of its own. kf4-pc hands it, in the order it takes
// them:
// - a frame (PumpFrame): its kept walk, the frame as walked (Between::hand_over), and kf4-pc's surfaces copied as they
//   were at the frame's first picture handed over (Canvas::copy_surfaces), which the pump's canvas draws on from then;
// - each picture between whose kept walk kf4-pc would have drawn again, and the picture that shows it (an extra one, or
//   the vblank's own when the picture between is at the vblank: frame_interp.c's phase 0);
// - the pictures kf4-pc made itself while the pump had work, which the pump hands out after its own before them, so
//   that the presenter, the scanout ring and the vblanks' count (pcr_backlog) have every picture in its order.
// The pump never moves Granite's frame contexts on (kf4-pc does, at each of its pictures: that waits for every thread's
// open command buffer, so the pump's canvas submits every few dozen batches).
// A picture that shows the pump's picture between (a vblank's own or an extra one), at the vblank's scale
struct PumpPicture {
    VblankPicture picture;
    uint32_t scale;
};
using PumpMessage = std::variant<PumpFrame, InterpPicture, PumpPicture, MadePicture>;

class Pump {
public:
    Pump(Device &device, ScanoutRing &scanout, PresenterHooks &presenter, Stats &stats, const ClearColour &clear,
         Inbox &inbox, SharedTurnLead &turn_lead)
        : device_(device), inbox_(inbox), turn_lead_(turn_lead), out_(device, scanout, presenter, stats, clear) {}

    ~Pump() {
        stop();
    }

    // (kf4-pc) The thread
    void start() {
        thread_ = std::thread([this] { run(); });
    }

    // (kf4-pc) In order
    void send(PumpMessage message) {
        std::lock_guard<std::mutex> hold(lock_);
        frames_waiting_ += std::holds_alternative<PumpFrame>(message) ? 1 : 0;
        messages_.push_back(std::move(message));
        wake_.notify_one();
    }

    // (kf4-pc) Before a frame is sent: until no frame sent is still waiting to be started. Each frame holds a copy of
    // every surface (hundreds of MB at 4x), so a pump behind (a rate above what it draws) must hold kf4-pc back, not
    // queue them: unbounded, they filled the GPU's memory and the system's in seconds at 500 a second (2026-11-07).
    void wait_for_room_for_frame() {
        std::unique_lock<std::mutex> hold(lock_);
        idle_.wait(hold, [&] { return gone_ || frames_waiting_ == 0; });
    }

    // (kf4-pc) Nothing sent left to do. Only kf4-pc sends, so an idle pump stays idle until it sends again.
    bool idle() {
        std::lock_guard<std::mutex> hold(lock_);
        return messages_.empty() && !busy_;
    }

    // Any thread: everything sent before done, or the thread gone
    void wait_idle() {
        std::unique_lock<std::mutex> hold(lock_);
        idle_.wait(hold, [&] { return gone_ || (messages_.empty() && !busy_); });
    }

    bool wait_idle_for(std::chrono::milliseconds most) {
        std::unique_lock<std::mutex> hold(lock_);
        return idle_.wait_for(hold, most, [&] { return gone_ || (messages_.empty() && !busy_); });
    }

    // At exit: nothing more is taken (what is left is dropped), and the thread is joined
    void stop() {
        {
            std::lock_guard<std::mutex> hold(lock_);
            stopping_ = true;
            wake_.notify_all();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // (the pump idle) --stats
    pcr::GpuTimes *gpu_times() {
        return times_.get();
    }

    void print_counts(const pcr::CanvasCounts &intake) const {
        if (!canvas_ || !between_) {
            return;
        }
        pcr::BetweenCounts b = between_->counts();
        pcr::CanvasCounts c = canvas_->counts();
        fprintf(stderr,
                "[stats] pc pump: %llu frames (%llu surfaces copied for them), %llu pictures between drawn again "
                "(%llu not of its frame), %llu pictures showing them, %llu left out as too late and %llu not shown as made "
                "too late (%.2f ms a picture at the end); its canvas: %llu batches, %llu native draw calls, %llu "
                "passes, %llu surfaces made, %llu uploads into surfaces\n",
                (unsigned long long)b.frames, (unsigned long long)intake.surfaces_copied,
                (unsigned long long)b.drawn_again, (unsigned long long)b.missed, (unsigned long long)pictures_,
                (unsigned long long)left_out_, (unsigned long long)late_, cost_.mean_s() * 1e3,
                (unsigned long long)c.batches, (unsigned long long)c.native_draw_calls, (unsigned long long)c.passes,
                (unsigned long long)c.surfaces_made, (unsigned long long)c.uploads_into_surfaces);
    }

private:
    // ("kf4-pc-pump") The messages, in the order sent, until stop
    void run() {
        Util::register_thread_index(1); // (Granite's command pools: this thread's)
#ifdef __linux__
        pthread_setname_np(pthread_self(), "kf4-pc-pump");
#endif
        times_ = std::make_unique<pcr::GpuTimes>(device_);
        canvas_ = std::make_unique<pcr::Canvas>(device_, *times_);
        between_ = std::make_unique<pcr::Between>(device_);
        while (std::optional<PumpMessage> message = take()) {
            std::visit([this](auto &m) { work(m); }, *message);
            std::lock_guard<std::mutex> hold(lock_);
            busy_ = false;
            idle_.notify_all();
        }
        std::lock_guard<std::mutex> hold(lock_);
        gone_ = true;
        idle_.notify_all();
    }

    std::optional<PumpMessage> take() {
        std::unique_lock<std::mutex> hold(lock_);
        for (;;) {
            wake_.wait(hold, [&] { return stopping_ || !messages_.empty(); });
            if (stopping_) {
                return std::nullopt;
            }
            leave_out_late();
            double early_s = early_by_s();
            if (early_s <= 0) {
                break;
            }
            wake_.wait_for(hold, std::chrono::duration<double>(early_s));
        }
        PumpMessage message = std::move(messages_.front());
        messages_.pop_front();
        busy_ = true;
        if (std::holds_alternative<PumpFrame>(message)) {
            frames_waiting_--;
            idle_.notify_all();
        }
        return message;
    }

    // (under the lock) --frame-rate unlocked: the pictures between at the front that the pump would have ready after their
    // time, each with the extra picture that shows it, left out while a later picture between waits (pcr_late.h)
    void leave_out_late() {
        double now_s = port_wall_seconds();
        while (messages_.size() >= 2) {
            const InterpPicture *p = std::get_if<InterpPicture>(&messages_[0]);
            const PumpPicture *shows = std::get_if<PumpPicture>(&messages_[1]);
            if (p == nullptr || shows == nullptr || (shows->picture.want & GSN_EXTRA) == 0 ||
                shows->picture.reply != nullptr) {
                return;
            }
            bool later_waiting = false;
            for (size_t i = 2; i < messages_.size() && !later_waiting; i++) {
                later_waiting = std::holds_alternative<InterpPicture>(messages_[i]);
            }
            if (!p->picture.leave_out_late || !pcr::leave_out(p->picture.shown_s, now_s, cost_, later_waiting)) {
                return;
            }
            messages_.pop_front();
            messages_.pop_front();
            left_out_++;
        }
    }

    // (under the lock) --frame-rate unlocked: how long until the picture between at the front need be begun (pcr_late.h:
    // drawn sooner, it would show the hand and the world sooner than it is shown); 0 or less: now
    double early_by_s() const {
        const InterpPicture *p = std::get_if<InterpPicture>(&messages_.front());
        if (p == nullptr || !p->picture.leave_out_late) {
            return 0;
        }
        return pcr::begin_at_s(p->picture.shown_s, cost_) - port_wall_seconds();
    }

        void work(PumpFrame &frame) {
        canvas_->start_from(*frame.surfaces);
        between_->take_walked(std::move(frame.walked));
        walk_ = std::move(frame.walk);
    }

    void work(InterpPicture &p) {
        double began_s = port_wall_seconds();
        turned_before(turn_lead_, turn_to_hand(p.picture, turn_lead_));
        last_between_ = p.picture;
        p.picture.run_bits = p.run_bits.empty() ? nullptr : p.run_bits.data();
        if (walk_ && between_->begin_picture_again(p.picture)) {
            canvas_->draw_again_alone(*walk_, *between_, picture_next());
            between_->end_picture_again();
        }
        between_took_s_ = port_wall_seconds() - began_s;
    }

    // Whether the next message is the picture that shows this picture between, already sent: taken at once, its
    // submit then puts the picture between's commands on the GPU's queue too (one vkQueueSubmit, not two). Not when
    // it is still to come (at a capped rate, until the picture's time): the picture between is flushed at once then,
    // so the GPU draws it in the meantime.
    bool picture_next() {
        std::lock_guard<std::mutex> hold(lock_);
        return pcr::submit_together() && !messages_.empty() && std::holds_alternative<PumpPicture>(messages_.front()) &&
               !stopping_;
    }

    void work(PumpPicture &e) {
        pictures_++;
        double began_s = port_wall_seconds();
        MadePicture made = out_.make(e.picture, e.scale, canvas_.get(), *times_);
        late_ += uint64_t(not_shown_when_late(made, last_between_));
        last_between_ = {}; // (shown by this picture: not again)
        out_.hand_out(made, inbox_);
        if (between_took_s_ > 0) {
            // (the picture between and this, which shows it: their work, not the wait between them)
            double took_s = between_took_s_ + port_wall_seconds() - began_s;
            std::lock_guard<std::mutex> hold(lock_);
            cost_.made(took_s);
            between_took_s_ = 0;
        }
        // (under Granite's device lock: kf4-pc's next_frame_context writes the timestamps' results as they come in)
        device_.external_queue_lock();
        times_->collect();
        device_.external_queue_unlock();
    }

    // (a picture kf4-pc made: never one whose pixels are kf4-pc's own, which it hands out itself)
    void work(MadePicture &made) {
        out_.hand_out(made, inbox_);
    }

    Device &device_;
    Inbox &inbox_;
    SharedTurnLead &turn_lead_;
    PictureOut out_;
    std::unique_ptr<pcr::GpuTimes> times_; // (before the canvas: it outlives it)
    std::unique_ptr<pcr::Canvas> canvas_;
    std::unique_ptr<pcr::Between> between_;
    std::shared_ptr<const pcr::KeptWalk> walk_;
    uint64_t pictures_ = 0;
    uint64_t left_out_ = 0;         // (under the lock) pictures between left out as too late (unlocked)
    uint64_t late_ = 0;             // (the pump's thread) and those made but too late to show
    GsnInterpPicture last_between_{}; // (the pump's thread) the picture between last drawn (its run bits not kept)
    pcr::PictureCost cost_;         // (under the lock: written by the pump's thread, read in take)
    double between_took_s_ = 0;     // (the pump's thread) the last picture between's work, until its picture's
    std::mutex lock_;
    std::condition_variable wake_, idle_;
    std::deque<PumpMessage> messages_;
    int frames_waiting_ = 0; // (PumpFrames in messages_)
    bool busy_ = false, stopping_ = false, gone_ = false;
    std::thread thread_;
};

// ---- The renderer ------------------------------------------------------------------------------------
class Renderer {
public:
    // The device (with the presenter's extensions when present_wanted), and the thread. False without one.
    bool open(bool present_wanted) {
        if (!Context::init_loader(nullptr)) {
            fprintf(stderr, "[pcr] no Vulkan loader\n");
            return false;
        }
        present_wanted_ = present_wanted;
        if (!make_context()) {
            fprintf(stderr, "[pcr] no Vulkan device\n");
            return false;
        }
        device_ = std::make_unique<Device>();
        device_->set_context(*context_);
        // a frame context a picture: with four, the thread waits at a picture for the GPU's work of four before
        device_->init_frame_contexts(4);
        const VkPhysicalDeviceProperties &props = device_->get_gpu_properties();
        gpu_kind_ = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? GSN_GPU_DISCRETE
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? GSN_GPU_INTEGRATED
                                                                                 : GSN_GPU_OTHER;
        msaa_samples_have_ = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts &
                             props.limits.sampledImageDepthSampleCounts;
        out_.emplace(*device_, scanout_, presenter_, stats, clear_);
        if (pump_wanted()) {
            pump_ = std::make_unique<Pump>(*device_, scanout_, presenter_, stats, clear_, inbox_, turn_lead_);
            pump_->start();
            const char *direct = getenv("KF4_PC_DIRECT");
            direct_on_ = !(direct && strcmp(direct, "0") == 0);
            const char *wait = getenv("KF4_PC_CONTEXT_WAIT");
            try_contexts_ = !(wait && strcmp(wait, "1") == 0);
        }
        thread_ = std::thread([this] { run(); });
        fprintf(stderr, "[pcr] the PC renderer on %s\n", props.deviceName);
        return true;
    }

    // At exit: what was sent is done (two seconds at most), then the thread stops; if it does not within two
    // seconds more it is left (the process ends with it)
    // (the pump's thread likewise, after kf4-pc's: kf4-pc may wait for it)
    void stop() {
        inbox_.wait_idle_for(std::chrono::seconds(2));
        if (pump_) {
            pump_->wait_idle_for(std::chrono::seconds(2));
        }
        inbox_.stop();
        if (inbox_.wait_gone_for(std::chrono::seconds(2))) {
            thread_.join();
        } else {
            thread_.detach();
        }
        if (pump_) {
            pump_->stop();
        }
    }

    void gif(int path, const void *data, size_t bytes) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        inbox_.put(GifPacket{path, std::vector<uint8_t>(p, p + bytes)}, bytes, false);
    }

    // Everything sent before done: kf4-pc's, then the pump's (which only kf4-pc sends to)
    void wait() {
        inbox_.wait_idle();
        if (pump_) {
            pump_->wait_idle();
        }
    }

    int picture(const uint64_t *privileged, int field, int want, PortGsFrame *frame, uint64_t tag) {
        VblankPicture v = {};
        memcpy(v.privileged.data(), privileged, sizeof(v.privileged));
        v.field = field;
        v.want = want;
        v.progressive = port_options.progressive != 0;
        v.deinterlace = port_options.deinterlace != 0;
        v.tag = tag;
        bool extra = (want & GSN_EXTRA) != 0;
        upper_field_.show_in(v, extra);
        // (the vblank's scale, as kf4-pc takes it: a picture's is the one of the last vblank's before it)
        uint32_t scale = send_scale_;
        if (!extra) {
            send_scale_ = settings.scale.load();
        }
        // The picture that shows a picture between sent to the pump directly (the next picture after it): the pump's
        // too, and kf4-pc only counts it (take_picture). One that shows a picture between kf4-pc was given, or that
        // kf4-pc may make itself, holds the next pictures between back from the pump until kf4-pc has taken it.
        bool direct = direct_show_;
        if (!direct && (normal_show_ || (want & (GSN_PRESENT | GSN_WANT_IMAGE | GSN_WANT_PIXELS)))) {
            v.counted = true;
            routed_++;
        }
        direct_show_ = normal_show_ = false;
        // A picture nobody waits for (none, or one this thread hands to the presenter) is
        // queued and the sender goes on; one wanted in frame, or an image the sender shows itself, is waited for
        bool queued = !(want & GSN_WANT_PIXELS) && (!(want & GSN_WANT_IMAGE) || (want & GSN_PRESENT));
        Reply reply;
        if (!queued) {
            v.frame = frame;
            v.reply = &reply;
        }
        if (direct) {
            // (kf4-pc's first: the vblank sent before the pump can hand it out)
            VblankPicture note = v;
            note.direct = true;
            note.frame = nullptr;
            note.reply = nullptr;
            if (!inbox_.put(std::move(note), 0, !extra)) {
                return 0;
            }
            pump_->send(PumpPicture{std::move(v), scale});
        }
        if (queued) {
            if (!direct && !inbox_.put(std::move(v), 0, !extra)) {
                return 0;
            }
            if ((want & GSN_PRESENT) && !extra) {
                // (a window's picture at most two vblanks behind the game, and the vblank the pump holds its pictures)
                inbox_.wait_behind_at_most(2 + uint64_t(held_vblanks()));
            }
            return want & (GSN_PRESENT | GSN_WANT_IMAGE);
        }
        if (!direct && !inbox_.put(std::move(v), 0, !extra)) {
            return 0;
        }
        return inbox_.wait_reply(reply);
    }

    void interp_frame(GsnInterpFrame *frame) {
        inbox_.put(InterpFrame(frame), 0, false);
    }

    // A picture between: to kf4-pc, or straight to the pump when the pump has this frame's walk from kf4-pc already
    // and kf4-pc has taken every picture before it that it might give the pump or make itself (so the pump's queue
    // keeps the pictures' order). kf4-pc's inbox then holds the next frame's packets ahead of it (sent between this
    // vblank and the one before), which the picture need not wait for: drawn from the kept walk, it reads nothing of
    // kf4-pc's store or surfaces. kf4-pc is sent it too, to count it and to stop this when the walk no longer fits
    // (take_picture_between). KF4_PC_DIRECT=0: every picture between through kf4-pc (docs/port/PERFORMANCE.md).
    void interp_picture(const GsnInterpPicture &picture) {
        InterpPicture p = {picture, {}};
        if (picture.runs && picture.run_bits) {
            p.run_bits.assign(picture.run_bits, picture.run_bits + (size_t(picture.runs) + 31) / 32);
        }
        p.picture.run_bits = nullptr; // (set to the kept bits where it is taken)
        bool direct = direct_on_ && port_options.frame_rate != PORT_FRAME_RATE_UNLOCKED && picture.serial != 0 &&
                      direct_serial_.load(std::memory_order_acquire) == picture.serial &&
                      routed_done_.load(std::memory_order_acquire) == routed_;
        direct_show_ = direct;
        normal_show_ = !direct;
        if (!direct) {
            p.counted = true;
            routed_++;
            inbox_.put(std::move(p), 0, false);
            return;
        }
        stats.direct++;
        InterpPicture note = {p.picture, {}, true, false};
        inbox_.put(std::move(note), 0, false);
        pump_->send(std::move(p));
    }

    // The vblanks behind the game, less the one whose pictures the pump holds until their time (unlocked: pcr_late.h):
    // waiting for its time, a picture is not behind
    int backlog() {
        int behind = inbox_.backlog() - held_vblanks();
        return behind > 0 ? behind : 0;
    }

    // (any thread) The vblanks of pictures the pump holds back until their time: 1 with --frame-rate unlocked
    int held_vblanks() const {
        return pump_ && port_options.frame_rate == PORT_FRAME_RATE_UNLOCKED ? 1 : 0;
    }

    void set_presenter(void (*pixels)(const PortGsFrame *, uint64_t), void (*image)(uint64_t)) {
        presenter_.pixels.store(pixels);
        presenter_.image.store(image);
    }

    bool present_device(GsnPresentDevice &out) const {
        if (!context_ || !present_wanted_) {
            return false;
        }
        const QueueInfo &queues = context_->get_queue_info();
        out.instance = context_->get_instance();
        out.gpu = context_->get_gpu();
        out.device = context_->get_device();
        out.queue = queues.queues[QUEUE_INDEX_GRAPHICS];
        out.queue_family = queues.family_indices[QUEUE_INDEX_GRAPHICS];
        out.api_version = context_->get_enabled_device_features().instance_api_core_version;
        out.get_instance_proc_addr = reinterpret_cast<void *>(Context::get_instance_proc_addr());
        return out.queue != VK_NULL_HANDLE;
    }

    void queue_lock() {
        device_->external_queue_lock();
    }

    void queue_unlock() {
        device_->external_queue_unlock();
    }

    ScanoutRing &scanout() {
        return scanout_;
    }

    int gpu_kind() const {
        return gpu_kind_;
    }

    uint32_t msaa_samples_have() const {
        return msaa_samples_have_;
    }

    uint32_t clear_rgba() const {
        return clear_.rgba;
    }

    // fn on the PC renderer's thread, after everything sent before; returns once it has run (false if the thread
    // is gone)
    bool run_job(std::function<void()> fn) {
        Reply reply;
        if (!inbox_.put(Job{std::move(fn), &reply}, 0, false)) {
            return false;
        }
        return inbox_.wait_reply(reply) != 0;
    }

    Intake &intake() {
        return intake_;
    }

    // (after wait) the pump's GPU time, or nullptr without a pump
    pcr::GpuTimes *pump_gpu_times() {
        return pump_ ? pump_->gpu_times() : nullptr;
    }

    // The canvas, once everything sent before is done (the thread idle until more is sent: the read-back FIFO's
    // caller, the DMA controller's thread, sends nothing meanwhile)
    pcr::Canvas *canvas_after_wait() {
        inbox_.wait_idle();
        return intake_.canvas();
    }

    // (after wait: the threads are idle)
    void print_census() {
        intake_.print_census();
        if (pump_ && intake_.canvas()) {
            pump_->print_counts(intake_.canvas()->counts());
        }
    }

private:
    // The Vulkan instance and device: with the presenter's surface and swapchain extensions when it wants
    // them, else (or when that fails) without
    bool make_context() {
        // (KF4_PC_PUSH_DESCRIPTORS=1: push descriptors. Granite then pushes the last set of each program's layout,
        // vkCmdPushDescriptorSet, rather than allocating and writing a descriptor set for it at every change: the
        // batches' textures, pcr_canvas.cpp's set 1. Off by default: on NVIDIA (RTX 5080, 2026-11-12) it took the
        // pump's CPU at 009 uncapped from about 490 to 440 ms a second, but Andrew's settings' uncapped rate from about
        // 321 to 296 pictures a second, the game from 58.4 to 56.5 vblanks a second (the owner's: 491 against 494);
        // docs/port/PERFORMANCE.md, "Built 2026-11-12".)
        const char *push = getenv("KF4_PC_PUSH_DESCRIPTORS");
        const Vulkan::ContextCreationFlags context_flags =
            push && strcmp(push, "1") == 0 ? Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT : 0;
        std::vector<const char *> instance_ext, device_ext;
        if (present_wanted_) {
            present_extensions(instance_ext, device_ext);
        }
        if (!instance_ext.empty()) {
            context_ = std::make_unique<Context>();
            context_->set_num_thread_indices(2); // (kf4-pc's and the pump's)
            if (context_->init_instance_and_device(instance_ext.data(), uint32_t(instance_ext.size()),
                                                   device_ext.data(), uint32_t(device_ext.size()), context_flags)) {
                return true;
            }
            fprintf(stderr, "[pcr] no Vulkan device with a swapchain: the window is shown without one\n");
        }
        present_wanted_ = false;
        context_ = std::make_unique<Context>();
        context_->set_num_thread_indices(2);
        if (!context_->init_instance_and_device(nullptr, 0, nullptr, 0, context_flags)) {
            context_.reset();
            return false;
        }
        return true;
    }

    // The surface extensions the Vulkan loader has (VK_KHR_surface and each platform's: SDL picks one;
    // VK_EXT_headless_surface for SDL's offscreen driver), and the swapchain's
    static void present_extensions(std::vector<const char *> &instance_ext, std::vector<const char *> &device_ext) {
        static const char *const surfaces[] = {"VK_KHR_win32_surface",   "VK_KHR_xlib_surface",
                                               "VK_KHR_xcb_surface",     "VK_KHR_wayland_surface",
                                               "VK_EXT_metal_surface",   "VK_EXT_headless_surface"};
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
            return;
        }
        std::vector<VkExtensionProperties> have(count);
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, have.data()) != VK_SUCCESS) {
            return;
        }
        auto has = [&](const char *name) {
            for (const VkExtensionProperties &e : have) {
                if (strcmp(e.extensionName, name) == 0) {
                    return true;
                }
            }
            return false;
        };
        if (!has(VK_KHR_SURFACE_EXTENSION_NAME)) {
            return;
        }
        for (const char *name : surfaces) {
            if (has(name)) {
                instance_ext.push_back(name);
            }
        }
        if (instance_ext.empty()) {
            return;
        }
        instance_ext.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        device_ext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    // (the PC renderer's thread, "kf4-pc") The messages, in the order sent, until the program ends
    void run() {
        Util::register_thread_index(0); // (Granite's command pools: this thread's)
        intake_.attach(*device_);
#ifdef __linux__
        pthread_setname_np(pthread_self(), "kf4-pc");
#endif
        while (std::optional<Message> message = inbox_.take()) {
            if (GifPacket *packet = std::get_if<GifPacket>(&*message)) {
                stats.packets++;
                stats.bytes += packet->bytes.size();
                const auto start = IntakeTimes::Clock::now();
                intake_.feed(packet->path, packet->bytes);
                intake_times.packet_taken(start, IntakeTimes::Clock::now());
                inbox_.done(packet->bytes.size());
            } else if (VblankPicture *v = std::get_if<VblankPicture>(&*message)) {
                bool counted = v->counted;
                take_picture(std::move(*v));
                if (counted) {
                    routed_done_.fetch_add(1, std::memory_order_release); // (what it gave the pump sent)
                }
                inbox_.done(0);
            } else if (Job *job = std::get_if<Job>(&*message)) {
                direct_serial_.store(0, std::memory_order_release); // (a state saved or loaded: through kf4-pc again)
                job->work();
                inbox_.answer(job->reply, 1);
                inbox_.done(0);
            } else if (InterpFrame *frame = std::get_if<InterpFrame>(&*message)) {
                stats.interp_frames++;
                direct_serial_.store(0, std::memory_order_release); // (a new frame: its walk not the pump's yet)
                intake_.take_frame(std::move(*frame));
                intake_times.frame_taken(IntakeTimes::Clock::now());
                inbox_.done(0);
            } else if (InterpPicture *p = std::get_if<InterpPicture>(&*message)) {
                stats.interp_pictures++;
                bool counted = p->counted;
                take_picture_between(std::move(*p));
                if (counted) {
                    routed_done_.fetch_add(1, std::memory_order_release);
                }
                inbox_.done(0);
            }
        }
        inbox_.gone();
    }

    // (kf4-pc) A vblank's picture, or an extra one: the pump's when it shows the picture between the pump drew (the
    // last picture between taken was the pump's, and no picture has shown it yet: gs.c draws a picture between, then
    // the picture that shows it), else made here and handed out in order
    void take_picture(VblankPicture v) {
        bool extra = (v.want & GSN_EXTRA) != 0;
        if (!extra) {
            intake_.vblank(v.privileged.data(), settings.scale.load(), settings.lod.load(), settings.aniso.load(),
                           uint32_t(settings.msaa.load()), settings.cutout_aa.load() != GSN_CUTOUT_AA_OFF,
                           settings.mode_2d.load(), settings.colour.load());
        }
        uint32_t scale = picture_scale_;
        if (!extra) {
            picture_scale_ = settings.scale.load(); // (a new scale from the next vblank)
        }
        bool pumped = shows_pumped_ || v.direct;
        shows_pumped_ = false;
        if (pumped) {
            last_between_ = {}; // (the pump's to show)
            if (!v.direct) {
                pump_->send(PumpPicture{std::move(v), scale});
            }
            // (Granite frees what a thread lets go of, the pump's each picture's blocks and command buffers too, only
            // as its frame contexts come round, and only kf4-pc moves them on: at a rate where the pump made most
            // pictures they were never freed, 6 MB a picture, and filled the GPU's memory and the system's at 500 a
            // second, 2026-11-07. Moving it on waits for the pump's open command buffer, which it submits every 128
            // batches, and for kf4-pc's own: submitted first, or kf4-pc waits for itself for good.)
            if (pcr::Canvas *canvas = intake_.canvas()) {
                canvas->submit();
            }
            frame_context_owed();
            return;
        }
        if (!extra) {
            pay_frame_contexts(); // (the canvas submitted at the vblank)
        }
        MadePicture made = out_->make(v, scale, intake_.canvas(), *intake_.gpu_times());
        if (extra) {
            stats.interp_late += uint64_t(not_shown_when_late(made, last_between_));
        }
        last_between_ = {}; // (shown by this picture, or not one's: not again)
        if (made.drawn) {
            device_->next_frame_context();
        }
        hand_out_in_order(made);
    }

    // (kf4-pc) A picture the pump drew: a frame context moved on for it (above), now when that waits for nobody's open
    // command buffer (the pump's, mostly: kf4-pc waited 0.87 ms a picture for it at 500, 32% of its time), else owed,
    // and waited for only when FRAME_CONTEXTS_OWED_MOST are owed. Its canvas's submit goes to the GPU's queue at once
    // either way (next_frame_context flushed it). KF4_PC_CONTEXT_WAIT=1: moved on at each picture, waiting, as before.
    void frame_context_owed() {
        if (!try_contexts_) {
            device_->next_frame_context();
            return;
        }
        contexts_owed_++;
        pay_frame_contexts();
        if (contexts_owed_ > FRAME_CONTEXTS_OWED_MOST) {
            device_->next_frame_context();
            contexts_owed_--;
        } else if (contexts_owed_ > 0) {
            device_->flush_frame();
        }
    }

    // (kf4-pc, its canvas submitted) The frame contexts owed moved on while that waits for nobody
    void pay_frame_contexts() {
        while (contexts_owed_ > 0 && device_->try_next_frame_context()) {
            contexts_owed_--;
        }
    }

    // (kf4-pc) After the pump's pictures before it. A picture whose sender waits, or whose pixels are kf4-pc's own
    // (written over by its next picture), waits for the pump here.
    void hand_out_in_order(const MadePicture &made) {
        if (!pump_ || pump_->idle()) {
            out_->hand_out(made, inbox_);
            return;
        }
        if (made.reply || out_->own_pixels(made)) {
            pump_->wait_idle();
            out_->hand_out(made, inbox_);
            return;
        }
        pump_->send(made);
    }

    // (kf4-pc) A picture between: walked here, or handed to the pump (with its frame first when the pump has not that
    // walk yet), and then its extra picture too
    // A picture between the pump was sent directly (Renderer::interp_picture) is only counted here; when the walk no
    // longer fits, the next ones come here again (and are walked again, as walk_picture does).
    void take_picture_between(InterpPicture p) {
        if (p.direct) {
            last_between_ = p.picture;
            shows_pumped_ = true;
            if (!intake_.direct_picture(p.picture)) {
                direct_serial_.store(0, std::memory_order_release);
                stats.direct_stale++;
            }
            return;
        }
        double before_s = turn_to_hand(p.picture, turn_lead_);
        last_between_ = p.picture;
        p.picture.run_bits = p.run_bits.empty() ? nullptr : p.run_bits.data();
        std::optional<Intake::ForPump> for_pump = intake_.walk_picture(p.picture);
        shows_pumped_ = for_pump.has_value();
        if (!for_pump) {
            direct_serial_.store(0, std::memory_order_release);
            turned_before(turn_lead_, before_s); // (drawn here: this turn was its last)
            return;
        }
        if (for_pump->frame) {
            pump_->wait_for_room_for_frame();
            pump_->send(std::move(*for_pump->frame));
        }
        uint64_t serial = p.picture.serial;
        pump_->send(std::move(p));
        // (the pump has this frame's walk: its next pictures between may go to it directly)
        direct_serial_.store(serial, std::memory_order_release);
    }


public:
    Settings settings;
    Stats stats;
    IntakeTimes intake_times; // (kf4-pc's; read by --stats once it is idle)

private:
    bool present_wanted_ = false;
    std::unique_ptr<Context> context_; // (before the device: destroyed after it)
    std::unique_ptr<Device> device_;
    int gpu_kind_ = GSN_GPU_OTHER;
    uint32_t msaa_samples_have_ = 1;
    ClearColour clear_ = clear_colour();
    Inbox inbox_;
    ScanoutRing scanout_;
    PresenterHooks presenter_;
    Intake intake_; // (the PC renderer's thread's)
    // (kf4-pc's: the pictures it makes, the vblank's scale, whether the next picture shows the pump's picture between)
    std::optional<PictureOut> out_;
    uint32_t picture_scale_ = 1 | 1 << 8;
    bool shows_pumped_ = false;
    // (kf4-pc's) frame contexts owed for the pump's pictures (frame_context_owed): at most this many, about 6 MB each
    static constexpr int FRAME_CONTEXTS_OWED_MOST = 8;
    int contexts_owed_ = 0;
    bool try_contexts_ = true; // (KF4_PC_CONTEXT_WAIT=1: false; set at open)
    // Pictures straight to the pump (interp_picture). The sender's (the DMA controller's thread): the scale its next
    // picture has, whether its next picture shows a picture between sent to the pump directly or to kf4-pc, and the
    // pictures it sent kf4-pc that kf4-pc says it has taken (routed_done_) once taken
    bool direct_on_ = false; // (KF4_PC_DIRECT=0: off; set at open)
    uint32_t send_scale_ = 1 | 1 << 8;
    bool direct_show_ = false, normal_show_ = false;
    uint64_t routed_ = 0;
    // (kf4-pc's, read by the sender) the frame whose pictures between kf4-pc last gave the pump (0: none, or not now),
    // and the pictures counted (counted set) it has taken: everything it gives the pump of them sent
    std::atomic<uint64_t> direct_serial_{0}, routed_done_{0};
    GsnInterpPicture last_between_{}; // (kf4-pc's) the picture between last taken (its run bits not kept)
    SharedTurnLead turn_lead_;   // (before the pump, which holds it)
    UpperField upper_field_;     // (the DMA controller's thread: picture)
    std::unique_ptr<Pump> pump_; // (nullptr: KF4_PC_PUMP=0)
    std::thread thread_;
};

// The PC renderer, once opened. Its holder is never destroyed: other threads (the DMA controller's, the game's
// presenter) may still call in while the program exits, and the presenter's swapchain is on its device until the
// process ends. At exit its thread is stopped (pcr_open's atexit), and the device goes with the process.
std::unique_ptr<Renderer> &renderer() {
    static std::unique_ptr<Renderer> &holder = *new std::unique_ptr<Renderer>();
    return holder;
}

bool present_asked; // pcr_enable_present, before pcr_open (the opener's thread)
Settings settings_before_open; // (the setters called before pcr_open: taken by it)

Settings &settings() {
    return renderer() ? renderer()->settings : settings_before_open;
}

} // namespace

// ---- The C interface ---------------------------------------------------------------------------------
extern "C" int pcr_open(void *shared_context) {
    if (shared_context) {
        fprintf(stderr, "[pcr] the PC renderer runs on a Vulkan device of its own, not a shared one\n");
        return 0;
    }
    if (renderer()) {
        return 1;
    }
    auto made = std::make_unique<Renderer>();
    if (!made->open(present_asked)) {
        return 0;
    }
    renderer() = std::move(made);
    atexit([] { renderer()->stop(); });
    return 1;
}

extern "C" void pcr_enable_present(void) {
    present_asked = true;
}

extern "C" int pcr_gpu_kind(void) {
    return renderer() ? renderer()->gpu_kind() : GSN_GPU_OTHER;
}

extern "C" void pcr_gif(int path, const void *data, size_t bytes) {
    renderer()->gif(path, data, bytes);
}

extern "C" void pcr_wait(void) {
    renderer()->wait();
}

extern "C" void pcr_read_fifo(void *dst, uint32_t qwords) {
    // (the transfer's packet was sent before: once the thread has done it, the canvas has the pixels ready)
    pcr::Canvas *canvas = renderer() ? renderer()->canvas_after_wait() : nullptr;
    if (!canvas) {
        memset(dst, 0, size_t(qwords) * 16);
        return;
    }
    canvas->read_fifo(dst, qwords);
}

extern "C" int pcr_vsync(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame) {
    return pcr_picture(privileged, field, want_pixels, frame, 0);
}

extern "C" int pcr_picture(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame, uint64_t tag) {
    return renderer()->picture(privileged, field, want_pixels, frame, tag);
}

extern "C" int pcr_backlog(void) {
    return renderer() ? renderer()->backlog() : 0;
}

extern "C" void pcr_interp_frame(GsnInterpFrame *frame) {
    renderer()->interp_frame(frame);
}

extern "C" void pcr_interp_picture(const GsnInterpPicture *picture) {
    renderer()->interp_picture(*picture);
}

extern "C" void pcr_set_presenter(void (*pixels)(const PortGsFrame *frame, uint64_t tag), void (*image)(uint64_t tag)) {
    renderer()->set_presenter(pixels, image);
}

extern "C" int pcr_present_device(GsnPresentDevice *out) {
    return renderer() && renderer()->present_device(*out) ? 1 : 0;
}

extern "C" void pcr_queue_lock(void) {
    renderer()->queue_lock();
}

extern "C" void pcr_queue_unlock(void) {
    renderer()->queue_unlock();
}

extern "C" int pcr_scanout_take_tag(GsnScanout *out, uint64_t tag) {
    return renderer() && renderer()->scanout().take_tag(*out, tag) ? 1 : 0;
}

extern "C" int pcr_scanout_take(GsnScanout *out) {
    if (!renderer()) {
        return 0;
    }
    std::optional<uint64_t> tag = renderer()->scanout().newest_tag();
    return tag && renderer()->scanout().take_tag(*out, *tag) ? 1 : 0;
}

extern "C" void pcr_scanout_release(void) {
    renderer()->scanout().release();
}

extern "C" void pcr_set_present_scale(int width, int height, float sharpen) {
    settings().present_size.store(width > 0 && height > 0 ? uint32_t(width) << 16 | uint32_t(height) : 0);
    settings().present_sharpen.store(uint32_t(sharpen > 0 ? sharpen * 1000.0f + 0.5f : 0.0f));
}

extern "C" int pcr_display_rect(const uint64_t *privileged, GsnRect *rect) {
    return display_rect(privileged, *rect) ? 1 : 0;
}

extern "C" void pcr_read_rect(const GsnRect *rect, uint32_t *rgba) {
    // (no buffers yet: every one is the pictures' colour)
    uint32_t colour = renderer() ? renderer()->clear_rgba() : 0xFF000000u;
    for (size_t i = 0; i < size_t(rect->w) * rect->h; i++) {
        rgba[i] = colour;
    }
}

extern "C" void pcr_set_scale(int x_scale, int y_scale) {
    uint32_t x = uint32_t(x_scale < 1 ? 1 : x_scale > GSN_MAX_SCALE_X ? GSN_MAX_SCALE_X : x_scale);
    uint32_t y = uint32_t(y_scale < 1 ? 1 : y_scale > 4 ? 4 : y_scale);
    settings().scale.store(x | y << 8);
}

extern "C" void pcr_set_lod(int mode) {
    settings().lod.store(mode);
}

extern "C" int pcr_set_aniso(int taps) {
    // a power of two, at most 16
    int l = 0;
    while (l < 4 && (2 << l) <= taps) {
        l++;
    }
    settings().aniso.store(1 << l);
    return 1 << l;
}

extern "C" int pcr_set_msaa(int samples) {
    // the most this GPU has up to `samples` (colour and depth attachments both, and depth read in a shader)
    uint32_t have = renderer() ? renderer()->msaa_samples_have() : 1;
    uint32_t n = 1;
    while (n * 2 <= uint32_t(samples) && n < 8 && (have & (n * 2))) {
        n *= 2;
    }
    settings().msaa.store(int(n));
    return int(n);
}

extern "C" void pcr_set_cutout_aa(int mode) {
    settings().cutout_aa.store(mode);
}

extern "C" void pcr_set_2d(int mode) {
    settings().mode_2d.store(mode);
}

extern "C" void pcr_set_colour(int mode) {
    settings().colour.store(mode == GSN_COLOUR_HIGH ? GSN_COLOUR_HIGH : GSN_COLOUR_EXACT);
}

extern "C" void pcr_set_layer_z(int mode) {
    settings().layer_z.store(mode == GSN_LAYER_Z_TOLERANT ? GSN_LAYER_Z_TOLERANT : GSN_LAYER_Z_PS2);
}

extern "C" int pcr_native_kinds(void) {
    return pcr::NativeDraws::kinds();
}

extern "C" void pcr_set_native_draw(int mode) {
    settings().native_draw.store(mode == GSN_NATIVE_DRAW_PS2 ? GSN_NATIVE_DRAW_PS2 : GSN_NATIVE_DRAW_NATIVE);
}

extern "C" void pcr_pipeline_cache(const char *path) {
    (void)path; // (no pipelines yet)
}

extern "C" int pcr_pipelines_running(void) {
    return 0;
}

extern "C" int pcr_pipelines_building(int *done, int *total) {
    *done = *total = 0;
    return 0;
}

extern "C" void pcr_capture(void (*emit)(uint32_t type, const void *data, size_t size)) {
    (void)emit; // (no state of its own yet)
}

// ---- Save states --------------------------------------------------------------------------------------
// The PC renderer's state: the GS's 4 MB first (so that paraLLEl-GS takes it by its VRAM, as the PC renderer takes
// theirs: gs.c's take_other_vram), then the GS's registers, the GIF's Q and the CLUT buffer's mirror
namespace {
constexpr size_t STATE_VRAM_BYTES = size_t(4) << 20;

size_t state_bytes() {
    return STATE_VRAM_BYTES + sizeof(pcr::GsRegisterFile::reg) + sizeof(uint32_t) +
           sizeof(pcr::TextureStore::ClutState::clut) + sizeof(pcr::TextureStore::ClutState::unloaded) +
           sizeof(pcr::TextureStore::ClutState::cbp);
}

} // namespace

extern "C" int pcr_restore(uint32_t type, const void *data, size_t size) {
    if (!renderer() || type != KF4GS_VRAM || size != STATE_VRAM_BYTES) {
        return 0;
    }
    std::vector<uint8_t> vram(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    Renderer &r = *renderer();
    return r.run_job([&] {
        pcr::TextureStore::ClutState clut;
        clut.unloaded.fill(1);
        clut.cbp = {UINT32_MAX, UINT32_MAX};
        r.intake().forget();
        if (pcr::Canvas *canvas = r.intake().canvas()) {
            canvas->restore(vram, clut);
        }
    });
}

extern "C" size_t pcr_state_size(void) {
    return renderer() ? state_bytes() : 0;
}

extern "C" void pcr_state(PortStateIo *io) {
    if (!renderer()) {
        return;
    }
    Renderer &r = *renderer();
    std::vector<uint8_t> vram;
    pcr::GsRegisterFile file;
    pcr::TextureStore::ClutState clut;
    if (!io->loading) {
        r.run_job([&] {
            pcr::Canvas *canvas = r.intake().canvas();
            vram = canvas ? canvas->vram_image() : std::vector<uint8_t>(STATE_VRAM_BYTES);
            if (canvas) {
                clut = canvas->clut_state();
            }
            file = r.intake().front().registers();
        });
    }
    vram.resize(STATE_VRAM_BYTES);
    port_state_bytes(io, vram.data(), vram.size());
    port_state_bytes(io, file.reg.data(), sizeof(file.reg));
    port_state_bytes(io, &file.internal_q, sizeof(file.internal_q));
    port_state_bytes(io, clut.clut.data(), sizeof(clut.clut));
    port_state_bytes(io, clut.unloaded.data(), sizeof(clut.unloaded));
    port_state_bytes(io, clut.cbp.data(), sizeof(clut.cbp));
    if (io->loading) {
        r.run_job([&] {
            r.intake().forget();
            if (pcr::Canvas *canvas = r.intake().canvas()) {
                canvas->restore(vram, clut);
            }
            r.intake().front().restore(file);
        });
    }
}

extern "C" size_t pcr_table_state_size(void) {
    return 0;
}

extern "C" void pcr_table_state(PortStateIo *io, size_t size) {
    port_state_bytes(io, nullptr, size); // (a table saved by an older build's native GS (since removed): passed over)
}

extern "C" void pcr_print_stats(void) {
    if (!renderer()) {
        return;
    }
    renderer()->wait();
    const Stats &s = renderer()->stats;
    fprintf(stderr,
            "[stats] pc renderer: %llu packets (%.1f MB), %llu vblanks, %llu pictures (%llu between); pictures "
            "between: %llu frames taken, %llu pictures asked for (%llu drawn here not shown as made too late)\n",
            (unsigned long long)s.packets.load(), double(s.bytes.load()) / 1048576.0,
            (unsigned long long)s.vblanks.load(), (unsigned long long)s.pictures.load(),
            (unsigned long long)s.extras.load(), (unsigned long long)s.interp_frames.load(),
            (unsigned long long)s.interp_pictures.load(), (unsigned long long)s.interp_late.load());
    fprintf(stderr, "[stats] pc pictures between straight to the pump: %llu (%llu then found not fitting)\n",
            (unsigned long long)s.direct.load(), (unsigned long long)s.direct_stale.load());
    renderer()->intake_times.print(stderr);
    renderer()->print_census();
    // (here, not by a job: at exit the thread may be stopped already; idle or gone, it records nothing meanwhile)
    if (pcr::GpuTimes *times = renderer()->intake().gpu_times()) {
        if (pcr::GpuTimes *pump = renderer()->pump_gpu_times()) {
            times->take_in(*pump);
        }
        times->print();
    }
}
