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
#include <span>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "context.hpp"
#include "pcr_antialias.h"
#include "pcr_between.h"
#include "pcr_budget.h"
#include "pcr_canvas.h"
#include "pcr_gpu_time.h"
#include "pcr_hang_watch.h"
#include "pcr_native.h"
#include "pcr_census.h"
#include "pcr_frame_census.h"
#include "pcr_gif.h"
#include "pcr_late.h"
#include "pcr_pipelines.h"
#include "pcr_shell.h"
#include "pcr_shaders.h"
#include "pcr_sync2.h"
#include "port/gs_capture_format.h"
#include "port/play_priority.h"
#include "port/thread_name.h"
#include "port/zone_scope.hpp"
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
#include "button_pictures.h" /* (pcr_key_width_u, pcr_hint_parts) */

using namespace Vulkan;

// The frame profiler's zones (port/include/port/zones.h)
const ZoneId pc_inbox_waits_for_work_zone = port_zone_id("pc inbox: waits for work", ZONE_WAIT);
const ZoneId pump_waits_for_work_zone = port_zone_id("pump: waits for work", ZONE_WAIT);
const ZoneId pump_waits_until_a_picture_s_time_zone = port_zone_id("pump: waits until a picture's time", ZONE_WAIT);
const ZoneId waits_for_the_pump_zone = port_zone_id("waits for the pump (idle)", ZONE_WAIT);
const ZoneId gpu_times_collected_zone = port_zone_id("GPU times collected", ZONE_WORK);
const ZoneId pc_frame_taken_pictures_between_zone = port_zone_id("pc frame taken (pictures between)", ZONE_WORK);
const ZoneId pc_walk_picture_a_picture_between_zone = port_zone_id("pc walk_picture (a picture between)", ZONE_WORK);
const ZoneId pc_direct_picture_zone = port_zone_id("pc direct_picture", ZONE_WORK);
const ZoneId pc_gif_packet_fed_zone = port_zone_id("pc GIF packet fed", ZONE_WORK);
const ZoneId picture_made_zone = port_zone_id("picture made", ZONE_WORK);
const ZoneId picture_handed_out_zone = port_zone_id("picture handed out", ZONE_WORK);
const ZoneId pc_take_picture_zone = port_zone_id("pc take_picture", ZONE_WORK);
const ZoneId pc_next_frame_context_waits_for_the_gpu_zone = port_zone_id("pc next_frame_context (waits for the GPU)", ZONE_WAIT);
const ZoneId pc_waits_for_the_gpu_unlocked_zone = port_zone_id("pc waits for the GPU (outside Granite's lock)", ZONE_WAIT);
const ZoneId pump_begin_picture_again_zone = port_zone_id("pump begin_picture_again", ZONE_WORK);
const ZoneId pump_waits_for_the_gpu_zone = port_zone_id("pump: waits for the GPU (its pictures in flight)", ZONE_WAIT);
const ZoneId pc_waits_for_the_pump_room_for_a_frame_zone = port_zone_id("pc waits for the pump (room for a frame)", ZONE_WAIT);
const ZoneId pump_frame_taken_zone = port_zone_id("pump frame taken", ZONE_WORK);
const ZoneId pump_picture_between_zone = port_zone_id("pump picture between", ZONE_WORK);
const ZoneId pump_picture_zone = port_zone_id("pump picture", ZONE_WORK);
const ZoneId pc_take_picture_between_zone = port_zone_id("pc take_picture_between", ZONE_WORK);
const ZoneId pc_inbox_waits_for_room_zone = port_zone_id("pc inbox: the sender waits for room", ZONE_WAIT);
const ZoneId pc_inbox_waits_for_a_reply_zone = port_zone_id("pc inbox: the sender waits for its picture", ZONE_WAIT);
const ZoneId pc_inbox_waits_behind_zone = port_zone_id("pc inbox: the sender waits for the renderer (2 vblanks behind)", ZONE_WAIT);
const ZoneId pc_waits_for_a_picture_s_time_zone = port_zone_id("pc waits until a picture's time (the budget)", ZONE_WAIT);
const ZoneId watch_waits_for_a_picture_zone = port_zone_id("watch: waits for a picture's GPU work", ZONE_WAIT);

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
    // (gs.c's own at a vblank, among the vblank's pictures: frame_interp.c's replays, the fields' screens) never taken
    // ahead of a picture (Inbox::take_packet_ahead), nor the packets behind it
    bool vblanks = false;
    // (KF4_SCN_AHEAD_FORCE's) sent past pictures between held back: taken ahead of them (Renderer::take_forced), and
    // how many
    bool ahead = false;
    uint32_t passed = 0;
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
    double sent_s = 0; // (slice 5's question: when the sender put it in)
};

// A picture between's frame (frame_interp.c's: pcr_between.h), and a picture asked for, its runs' bits kept
using pcr::InterpFrame;
struct InterpPicture {
    GsnInterpPicture picture;
    std::vector<uint32_t> run_bits;
    bool direct = false, counted = false; // (as VblankPicture's)
    double sent_s = 0;
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
    const double VBLANK_S = port_vblank_wall_seconds(); // (the host's: --game-speed shortens it, as the vblanks' times)
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
        auto room = [&] { return stopping_ || held_bytes_ == 0 || held_bytes_ + bytes <= MAX_HELD_BYTES; };
        if (!room()) {
            ZoneScope zone(pc_inbox_waits_for_room_zone);
            room_.wait(hold, room);
        }
        if (stopping_) {
            return false;
        }
        held_bytes_ += bytes;
        vblanks_sent_ += vblank ? 1 : 0;
        puts_++;
        messages_.push_back(std::move(message));
        wake_.notify_one();
        return true;
    }

    // (the PC renderer's thread) The next message, or none when it is to stop
    std::optional<Message> take() {
        std::unique_lock<std::mutex> hold(lock_);
        if (messages_.empty() && !stopping_) {
            ZoneScope zone(pc_inbox_waits_for_work_zone);
            wake_.wait(hold, [&] { return stopping_ || !messages_.empty(); });
        }
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

    // (kf4-pc, slice 5: the packets taken ahead, docs/plans/scene-slice5-results.md) The first of the game's packets
    // queued behind nothing but pictures between and the extra pictures that show them, taken out of its turn: the
    // scene it adds to is not drawn until its vblank closes it. None behind a vblank's own picture (its scene closed
    // there), a frame handed over, a job, or a vblank's own packets (GifPacket::vblanks). Its bytes are held until
    // packet_ahead_done. seen: the puts so far, for wait_put_until; passed: the pictures queued before it.
    std::optional<GifPacket> take_packet_ahead(uint64_t &seen, uint32_t &passed) {
        std::lock_guard<std::mutex> hold(lock_);
        seen = puts_;
        passed = 0;
        for (auto it = messages_.begin(); it != messages_.end(); ++it, passed++) {
            if (GifPacket *packet = std::get_if<GifPacket>(&*it)) {
                if (packet->vblanks) {
                    return std::nullopt;
                }
                GifPacket out = std::move(*packet);
                messages_.erase(it);
                return out;
            }
            if (std::holds_alternative<InterpPicture>(*it)) {
                continue;
            }
            const VblankPicture *v = std::get_if<VblankPicture>(&*it);
            if (!v || !(v->want & GSN_EXTRA)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    // (kf4-pc) A packet taken ahead done (the message kf4-pc is on is not: busy as it was)
    void packet_ahead_done(size_t bytes) {
        std::lock_guard<std::mutex> hold(lock_);
        held_bytes_ -= bytes;
        room_.notify_all();
    }

    // (kf4-pc) Until something more is put in than seen, or until
    void wait_put_until(uint64_t seen, std::chrono::steady_clock::time_point until) {
        std::unique_lock<std::mutex> hold(lock_);
        wake_.wait_until(hold, until, [&] { return stopping_ || puts_ != seen; });
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
        ZoneScope zone(pc_inbox_waits_for_a_reply_zone);
        idle_.wait(hold, [&] { return gone_ || reply.done; });
        return reply.done ? reply.result : 0;
    }

    // The sender of a window's picture: at most two vblanks ahead of their pictures handed out
    void wait_behind_at_most(uint64_t vblanks) {
        std::unique_lock<std::mutex> hold(lock_);
        auto caught_up = [&] { return gone_ || vblanks_sent_ - vblanks_done_ <= vblanks; };
        if (!caught_up()) {
            // (the wait that holds the DMA controller's thread, and so the game, when the GPU is too slow: without
            // its zone the profiler counted it as the "dma GS vsync" zone's own work)
            ZoneScope zone(pc_inbox_waits_behind_zone);
            idle_.wait(hold, caught_up);
        }
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
    uint64_t vblanks_sent_ = 0, vblanks_done_ = 0, puts_ = 0;
    bool busy_ = false, stopping_ = false, gone_ = false;
};

// ---- The scanout ring --------------------------------------------------------------------------------
// The pictures handed to the presenter: a ring of images (with --frame-rate above 60
// a vblank has several pictures, shown at their own times, so the newest must not write over those waiting). The
// slot the presenter holds is passed over. Written by the PC renderer's thread, taken by the game's.
constexpr int SCANOUT_SLOTS = 16;

class ScanoutRing {
public:
    // (the PC renderer's thread) The slot the next picture is written into. It has no picture until one of
    // whole_image and scaled gives it one; with neither it is never taken.
    int claim() {
        std::lock_guard<std::mutex> hold(lock_);
        int slot = (written_ + 1) % SCANOUT_SLOTS;
        // (nor the newest published: after pictures that could not be made, a claim came round to it and emptied it,
        // and the presenter, finding no picture there, showed black until one could be made again)
        while (slot == taken_ || slot == newest_) {
            slot = (slot + 1) % SCANOUT_SLOTS;
        }
        Slot &s = slots_[slot];
        s.serial = 0; // (not to be taken while it is written)
        s.scaled_valid = false;
        s.whole_valid = false;
        s.sharpen = 0;
        s.check_valid = false;
        written_ = slot;
        return slot;
    }

    // (the PC renderer's thread, between claim and publish) The slot's image for the picture whole, at the picture's
    // own size: made here when the slot has none of that size (outside the lock: Granite's device lock, which the
    // presenter holds while it releases a slot, comes before it). Null when it cannot be made.
    // Only for a picture the window does not get at its own size (scaled): a slot holds one or the other, since at 4x
    // the whole picture is 28 MB a slot that the presenter never read (PC_OPTIMIZATION.md, 2026-11-14).
    const Image *whole_image(Device &device, int slot, uint32_t width, uint32_t height) {
        bool fresh;
        {
            std::lock_guard<std::mutex> hold(lock_);
            const Slot &s = slots_[slot];
            fresh = !s.image || s.width != width || s.height != height;
        }
        ImageHandle made, old;
        if (fresh) {
            made = make_picture_image(device, width, height);
            if (!made) {
                fprintf(stderr, "[pcr] no image for a %ux%u scanout\n", width, height);
                return nullptr;
            }
        }
        std::lock_guard<std::mutex> hold(lock_);
        Slot &s = slots_[slot];
        if (fresh) {
            old = std::move(s.image); // (never the slot taken: claim passed over it)
            s.image = std::move(made);
            s.width = width;
            s.height = height;
        }
        s.whole_valid = true;
        return s.image.get(); // (old let go here, on this thread: Granite destroys it once the GPU is past it)
    }

    // (the PC renderer's thread, between claim and publish) The slot's picture at the window's size: made here when the
    // slot has none of that size (outside the lock, as whole_image's). The slot's whole image is let go: the presenter
    // is given this one. Null when it cannot be made.
    const Image *scaled(Device &device, int slot, uint32_t width, uint32_t height) {
        bool fresh;
        {
            std::lock_guard<std::mutex> hold(lock_);
            const Slot &s = slots_[slot];
            fresh = !s.scaled || s.scaled_width != width || s.scaled_height != height;
        }
        ImageHandle made, old, old_whole;
        if (fresh) {
            made = make_picture_image(device, width, height, false, true);
            if (!made) {
                fprintf(stderr, "[pcr] no image for a %ux%u scaled picture\n", width, height);
                return nullptr;
            }
        }
        std::lock_guard<std::mutex> hold(lock_);
        Slot &s = slots_[slot];
        if (fresh) {
            old = std::move(s.scaled);
            s.scaled = std::move(made);
            s.scaled_width = width;
            s.scaled_height = height;
        }
        if (!s.whole_valid) {
            old_whole = std::move(s.image); // (never the slot taken: claim passed over it)
        }
        s.scaled_valid = true;
        return s.scaled.get();
    }

    // (the PC renderer's thread, between claim and publish; KF4_PRESENT_FOLD_CHECK) The slot's scaled picture sharpened
    // here too, for the presenter to compare its own with (GsnScanout's check): an image of the scaled one's size
    const Image *check_image(Device &device, int slot) {
        ImageHandle made;
        uint32_t w, h;
        {
            std::lock_guard<std::mutex> hold(lock_);
            const Slot &s = slots_[slot];
            w = s.scaled_width;
            h = s.scaled_height;
            if (s.check && s.check->get_width() == w && s.check->get_height() == h) {
                slots_[slot].check_valid = true;
                return s.check.get();
            }
        }
        made = make_picture_image(device, w, h);
        std::lock_guard<std::mutex> hold(lock_);
        Slot &s = slots_[slot];
        s.check = std::move(made);
        s.check_valid = bool(s.check);
        return s.check.get();
    }

    // (the PC renderer's thread, between claim and publish) The slot's scaled picture left for the presenter to sharpen
    // by strength (GsnScanout's sharpen)
    void left_to_sharpen(int slot, float strength) {
        std::lock_guard<std::mutex> hold(lock_);
        slots_[slot].sharpen = strength;
    }

    // (the PC renderer's thread) Once the commands writing the slot are on the queue: the presenter may take it
    // A slot with no picture in it (a repeat of one claimed since, whose picture could not be made) is not published:
    // the presenter keeps the newest that has one.
    void publish(int slot, uint64_t tag, const GsnPictureTurn &turn) {
        std::lock_guard<std::mutex> hold(lock_);
        if (!slots_[slot].whole_valid && !slots_[slot].scaled_valid) {
            return;
        }
        slots_[slot].serial = ++serial_;
        slots_[slot].tag = tag;
        slots_[slot].turn = turn;
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
        if (pick < 0 || slots_[pick].serial == 0 || !(slots_[pick].whole_valid || slots_[pick].scaled_valid)) {
            return false;
        }
        const Slot &s = slots_[pick];
        out = {};
        out.serial = s.serial;
        out.tag = s.tag;
        out.turn = s.turn;
        if (s.scaled_valid) {
            out.scaled = uint64_t(s.scaled->get_image());
            out.scaled_width = s.scaled_width;
            out.scaled_height = s.scaled_height;
            out.sharpen = s.sharpen;
            out.scaled_view = uint64_t(s.scaled->get_view().get_view().view);
            out.check = s.check_valid ? uint64_t(s.check->get_image()) : 0;
        }
        // (a slot with the scaled picture alone: that is its picture too, for a window whose size has changed since)
        out.image = s.whole_valid ? uint64_t(s.image->get_image()) : out.scaled;
        out.width = s.whole_valid ? s.width : s.scaled_width;
        out.height = s.whole_valid ? s.height : s.scaled_height;
        taken_ = pick;
        return true;
    }

    // (the PC renderer's thread: KF4_SCN_REPEAT_CHECK) The picture a published slot holds (its scaled one, else whole)
    const Image *picture_of(int slot) {
        std::lock_guard<std::mutex> hold(lock_);
        const Slot &s = slots_[slot];
        return s.scaled_valid ? s.scaled.get() : s.whole_valid ? s.image.get() : nullptr;
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
    // (srgb_view: read as sRGB too, the sampler decoding it to linear light: PresentScaler's source)
    // (storage: written by a compute shader too, PresentScaler::fused's)
    static ImageHandle make_picture_image(Device &device, uint32_t width, uint32_t height, bool srgb_view = false,
                                          bool storage = false) {
        ImageCreateInfo info = ImageCreateInfo::render_target(width, height, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
        if (srgb_view) {
            info.misc |= IMAGE_MISC_MUTABLE_SRGB_BIT;
        }
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        return device.create_image(info, nullptr);
    }

private:
    struct Slot {
        ImageHandle image;
        uint32_t width = 0, height = 0;
        ImageHandle scaled; // (the picture at the window's size)
        uint32_t scaled_width = 0, scaled_height = 0;
        bool scaled_valid = false; // (scaled is this picture's)
        bool whole_valid = false;  // (image is this picture's)
        float sharpen = 0;         // (scaled left to the presenter to sharpen: left_to_sharpen)
        ImageHandle check;         // (KF4_PRESENT_FOLD_CHECK: scaled sharpened here, to compare: check_image)
        bool check_valid = false;
        uint64_t serial = 0; // 0: none published in it (or it is being written)
        uint64_t tag = 0;
        GsnPictureTurn turn{};
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
// fields (the menus, the title), with whole frames (port_whole_fields(): port/platform/field_frames.c): each vblank's
// lower field shown from the upper field's buffer, which field_frames drew again for the same moment and told of just
// before. The game draws the two half a line apart, rounded to a whole line of the field where its pictures are on
// whole pixels: shown in turn, a line apart or the field's lines moved as on a TV, every other picture was a line
// higher or lower (the menus shook). Without whole frames, the other buffer holds the moment before: the lower field
// is moved a line down instead (Canvas::scanout, ShownRect's lower_field).
class UpperField {
public:
    void show_in(VblankPicture &v, bool extra) {
        uint64_t smode2 = v.privileged[2 * 2];
        if ((smode2 & 3) != 3 || !v.deinterlace || !port_whole_fields()) {
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

// The scale the frames after a vblank are drawn at, across | down << 8: the settings', with twice the rows down while
// the display shows a screen the game draws as fields, whole (UpperField's): the upper field's 224 lines drawn as the
// picture's 448, every line of the picture its own row, where stretched they were each two (the logos' and menus'
// edges in steps of two lines). The lower field, drawn half a field's line higher, is then a line of the picture
// higher, and is not shown. SCALE_FIELD_LINES marks it: the picture's size is the settings' (picture_down).
constexpr uint32_t SCALE_FIELD_LINES = 1u << 16;
uint32_t drawing_scale(uint32_t scale, const uint64_t *privileged, bool deinterlace) {
    uint64_t smode2 = privileged[2 * 2];
    if ((smode2 & 3) != 3 || !deinterlace || !port_whole_fields()) {
        return scale;
    }
    uint32_t down = std::min(((scale >> 8) & 0xFF) * 2, 8u);
    return (scale & 0xFF) | down << 8 | SCALE_FIELD_LINES;
}
uint32_t scale_down(uint32_t scale) {
    return (scale >> 8) & 0xFF;
}
// (the picture's: the settings' scale down, the frames' drawn at twice it or not)
uint32_t picture_down(uint32_t scale) {
    return (scale & SCALE_FIELD_LINES) ? scale_down(scale) / 2 : scale_down(scale);
}

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
    if (lines == 0) {
        return std::nullopt;
    }
    if (size_t(rect.w) * sx * lines * sy > PORT_GS_FRAME_PIXELS) {
        // (not met at the game's own sizes and the scales allowed: in a player's log it says why the window is black)
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            fprintf(stderr, "[pcr] a %ux%u picture at scale %ux%u is over the most pixels a picture has: not shown\n",
                    rect.w, lines, sx, sy);
        }
        return std::nullopt;
    }
    return PictureSize{rect.w * sx, lines * sy};
}

// ---- The settings: kept for the steps that draw -------------------------------------------------------
struct Settings {
    std::atomic<uint32_t> scale{1 | 1 << 8}; // across | down << 8, from the next vblank
    std::atomic<int> lod{GSN_LOD_SCALE}, aniso{1}, mode_2d{GSN_2D_PS2}, antialias{PORT_AA_OFF},
        layer_z{GSN_LAYER_Z_PS2}, native_draw{GSN_NATIVE_DRAW_NATIVE}, font{GSN_FONT_SHARP};
    std::atomic<uint32_t> present_size{0}, present_sharpen{0};
    // (the controller's buttons: set on the main thread, read at each vblank on the PC renderer's)
    mutable std::mutex glyphs_lock;
    GsnGlyphs glyphs{};

    GsnGlyphs glyphs_now() const {
        std::lock_guard<std::mutex> hold(glyphs_lock);
        return glyphs;
    }
};

// ---- --stats ---------------------------------------------------------------------------------------
struct Stats {
    std::atomic<uint64_t> packets{0}, bytes{0}, vblanks{0}, pictures{0}, extras{0}, interp_frames{0},
        interp_pictures{0}, interp_late{0};
    // (Renderer::interp_picture) pictures between sent to the pump directly, and of them those kf4-pc then found
    // the frame's kept walk no longer fitting for (the store changed by the next frame's packets before them)
    std::atomic<uint64_t> direct{0}, direct_stale{0};
    // (slice 5: the scene renderer's) vblanks shown again from the last picture's slot (PictureOut::repeat)
    std::atomic<uint64_t> repeats{0};
    std::atomic<uint64_t> repeat_checks{0}, repeat_differ{0}; // (KF4_SCN_REPEAT_CHECK=1: made, and found different)
};

// (kf4-pc) Each frame's intake, for --stats: the time kf4-pc spent taking its packets in (busy), and from its first
// packet to its pictures between handed their frame (ready: what a picture between of it waits for at the soonest)
class IntakeTimes {
public:
    // (port_wall_seconds' times: the clock read once a packet, not again for this; it can be slow, port_clock_cost)
    void packet_taken(double start_s, double end_s) {
        if (!in_frame_) {
            in_frame_ = true;
            first_s_ = start_s;
        }
        busy_s_ += end_s - start_s;
    }

    void frame_taken(double end_s) {
        if (in_frame_ && frames_.size() < MOST) {
            frames_.push_back({ms(busy_s_), ms(end_s - first_s_)});
        }
        in_frame_ = false;
        busy_s_ = 0;
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
    static float ms(double s) {
        return float(s * 1000.0);
    }
    bool in_frame_ = false;
    double first_s_ = 0, busy_s_ = 0;
    std::vector<Frame> frames_;
};

// (pcr_shell.h) The renderer under the shell other than the canvas, set before pcr_open; nullptr: the canvas
pcr::DrawingMaker &drawing_maker() {
    static pcr::DrawingMaker maker = nullptr;
    return maker;
}
const char *&drawing_name() {
    static const char *name = "the PC renderer";
    return name;
}

// KF4_PC_PUMP=0: no pump (every picture between drawn on kf4-pc, as before). None under another renderer than the
// canvas (pcr_shell.h).
bool pump_wanted() {
    const char *v = getenv("KF4_PC_PUMP");
    return !(v && strcmp(v, "0") == 0) && drawing_maker() == nullptr;
}

// ---- What the GIF packets make ------------------------------------------------------------------------
// (the PC renderer's thread) The front end's events, drawn by the canvas; with KF4_PC_CENSUS=1 counted for the census
class Intake final : public pcr::GifEvents {
public:
    // (on the thread, once the device is there)
    void attach(Device &device, pcr::HangNotes &hang_notes) {
        times_ = std::make_unique<pcr::GpuTimes>(device);
        if (pcr::DrawingMaker maker = drawing_maker()) {
            // (another renderer under the shell, pcr_shell.h: no canvas, no pictures between of its own, no census)
            drawing_ = maker(device, *times_, hang_notes);
            natives_ = std::make_unique<pcr::NativeDraws>(device);
            between_ = std::make_unique<pcr::Between>(device); // (the pictures' buffers: the drawing draws them)
            return;
        }
        auto canvas = std::make_unique<pcr::Canvas>(device, *times_);
        canvas_ = canvas.get();
        drawing_ = std::move(canvas);
        canvas_->watch_hangs(hang_notes, "kf4-pc");
        if (frame_census_) {
            canvas_->observe_native_batches([this](const pcr::GsRegisterFile &file, uint32_t prim, const pcr::NativeBatch &nb) {
                if (census_now()) {
                    frame_census_->native_batch(file, prim, nb);
                }
            });
        }
        natives_ = std::make_unique<pcr::NativeDraws>(device);
        between_ = std::make_unique<pcr::Between>(device);
    }

    // (pcr_between.h) The frame for the pictures between. The front end's state now is where each of its walks
    // starts, so that every walk of the frame makes the same calls.
    //
    // A frame walked once (pcr_frame_walk.h): the frame's own walk, kept (the last stretch of packets between two
    // vblanks), is made its pictures' walk when the frame is its packets, and the rules make the stream's of it.
    void take_frame(InterpFrame frame) {
        ZoneScope zone(pc_frame_taken_pictures_between_zone);
        if (!canvas_) {
            take_frame_drawn(std::move(frame));
            return;
        }
        if (!between_) {
            return;
        }
        // A frame is taken at the vblank after its packets, mostly after that vblank's own picture, which closed the
        // stretch and made the frame's uploads the ones asked for again (vblank). With the mouse turned, at a fixed
        // rate, the vblank's first picture between (phase 0) comes before the vblank's own, and the frame with it: the
        // packets since the last vblank are then the frame's (their bytes its own), the stretch still open is its own
        // walk, closed here as the vblank would have, and the uploads since that vblank are the frame's. Dropped
        // instead, every frame taken so walked its packets for its pictures, the first of them with the frame before's
        // uploads (the HUD and the torches as rectangles of static for a picture). KF4_PC_FRAME_STRETCH=0: dropped, as
        // before.
        if (frame_stretch_ && since_vblank_bytes_ != 0 && since_vblank_bytes_ == frame->packet_bytes) {
            if (stretch_open_) {
                last_stretch_ = close_stretch();
            }
            canvas_->frame_taken();
        } else {
            drop_stretch();
        }
        frame_taken_at_ = vblanks_;
        const pcr::FrameTargets targets = {frame->draw_fbp,
                                           frame->shown_fbp,
                                           frame->uploads_asked_again != 0,
                                           frame->shown_kept != 0,
                                           frame->kicks,
                                           frame->moves, // (the moves live in between_ until the next frame's)
                                           frame_copies_ ? frame->frame_copies : 0,
                                           frame->copies_between};
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

    // (another renderer under the shell, pcr_shell.h: its own pictures between) The frame's stretch of packets, kept as
    // for the canvas (its kicks numbered, the native draws' records paired with their pictures'), and its kicks made the
    // frame's walked kicks: the stretch still open where the frame's packets are those since the last vblank (the mouse
    // turned: the frame taken at its own vblank's first picture), else the one closed at the last vblank (mostly: the
    // frame taken at its first picture between, a vblank after its packets). The drawing is told which, or none.
    void take_frame_drawn(InterpFrame frame) {
        using FS = pcr::Drawing::FrameStretch;
        FS which = FS::none;
        if (frame_stretch_ && stretch_open_ && since_vblank_bytes_ == frame->packet_bytes) {
            last_stretch_ = close_stretch();
            which = FS::open;
        } else {
            drop_stretch();
            which = since_vblank_bytes_ == 0 ? FS::last : FS::none;
        }
        frame_taken_at_ = vblanks_;
        pcr::Drawing::FrameShape shape;
        shape.draw_fbp = frame->draw_fbp;
        shape.shown_fbp = frame->shown_fbp;
        shape.kicks = frame->kicks;
        const uint64_t packet_bytes = frame->packet_bytes;
        between_->take_frame(std::move(frame));
        pcr::FrameWalkRefused refused = pcr::FrameWalkRefused::none;
        if (which == FS::none || !last_stretch_) {
            refused = pcr::FrameWalkRefused::not_kept;
        } else if (last_stretch_->bytes != packet_bytes) {
            refused = pcr::FrameWalkRefused::not_the_frame;
        } else if (last_stretch_->trail_paired) {
            refused = pcr::FrameWalkRefused::trail_paired;
        } else if (!between_->adopt_live_kicks(std::move(last_stretch_->kicks), std::move(last_stretch_->trail_moves))) {
            refused = pcr::FrameWalkRefused::not_the_frame;
        }
        between_->frame_from_own_walk(refused);
        drawing_->frame_taken(refused == pcr::FrameWalkRefused::none ? which : FS::none, shape);
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
        ZoneScope zone(pc_walk_picture_a_picture_between_zone);
        if (!canvas_) {
            // (another renderer under the shell: the picture's buffers made, the drawing draws it. Packets taken ahead
            // of it, the stretch open is theirs, after it: kept.)
            if (!drawing_ || !drawing_->packets_ahead_open()) {
                drop_stretch();
            }
            if (drawing_ && between_ && between_->begin_picture_again(picture)) {
                drawing_->picture_between(picture, {&between_->picture(), &between_->kicks(), &between_->moves()});
                between_->end_picture_again();
            }
            return std::nullopt;
        }
        if (!between_) {
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
        ZoneScope zone(pc_direct_picture_zone);
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
        return canvas_;
    }

    // What draws the frame: the canvas, or the renderer pcr_shell.h's use_drawing named
    pcr::Drawing *drawing() {
        return drawing_.get();
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
        const char *copies = getenv("KF4_PC_FRAME_COPIES");
        frame_copies_ = !(copies && strcmp(copies, "0") == 0);
        const char *stretch = getenv("KF4_PC_FRAME_STRETCH");
        frame_stretch_ = !(stretch && strcmp(stretch, "0") == 0);
        const char *census = getenv("KF4_PC_CENSUS");
        if (census && strcmp(census, "1") == 0) {
            census_ = std::make_unique<pcr::Census>();
        }
        frame_census_ = pcr::FrameCensus::from_env();
    }

    void feed(int path, const std::vector<uint8_t> &bytes) {
        ZoneScope zone(pc_gif_packet_fed_zone);
        if (own_walks_ && !stretch_open_ && !in_replay_ && vblanks_ <= frame_taken_at_ + STRETCHES_KEPT_VBLANKS) {
            open_stretch();
        }
        stretch_bytes_ += bytes.size();
        since_vblank_bytes_ += bytes.size();
        if (frame_census_ && census_now()) {
            frame_census_->bytes(path, bytes.size());
        }
        feeding_path_ = path;
        front_.feed(path, bytes);
        feeding_path_ = 0;
    }

    // A state loaded: no stretch kept from before it, nor the temporal anti-aliasing's history
    void forget() {
        pcr_antialias_cut();
        drop_stretch();
        last_stretch_.reset();
        in_replay_ = false;
        since_vblank_bytes_ = 0;
    }

    // scale: across | down << 8; lod and aniso: texture detail's and anisotropy's (Canvas::set_sampling); mode_2d,
    // font and glyphs: Drawing::set_2d's, set_font's and set_glyphs'
    // game_vblank: the game's vblank number (the picture's tag's; the developers' counters name the frames by it)
    void vblank(const uint64_t *privileged, uint32_t scale, int lod, int aniso, int mode_2d, int font,
                const GsnGlyphs &glyphs, uint64_t game_vblank) {
        vblanks_++;
        since_vblank_bytes_ = 0;
        if (stretch_open_) {
            last_stretch_ = close_stretch();
        }
        if (drawing_) {
            drawing_->set_scale(scale & 0xFF, scale_down(scale));
            drawing_->set_sampling(lod, uint32_t(aniso));
            drawing_->set_2d(mode_2d);
            drawing_->set_font(font);
            drawing_->set_glyphs(glyphs);
            drawing_->vblank(game_vblank);
            ZoneScope collect_zone(gpu_times_collected_zone);
            times_->collect();
        }
        if (census_) {
            census_->display(privileged[0], privileged[14], privileged[18]);
        }
        if (frame_census_ && canvas_) {
            frame_census_->vblank(game_vblank, canvas_->counts());
        }
    }

    void print_census() const {
        if (drawing_) {
            drawing_->print_counts();
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
        if (drawing_) {
            drawing_->texture_written(tex0, texclut);
        }
    }
    void primitive(const pcr::GifPrimitive &primitive) override {
        if (census_) {
            census_->primitive(front_.registers(), primitive);
        }
        if (frame_census_ && census_now()) {
            frame_census_->primitive(feeding_path_, in_native_, front_.registers(), primitive);
        }
        if (drawing_) {
            drawing_->primitive(front_.registers(), primitive);
        }
    }
    void image_upload(const pcr::TransferRegisters &transfer, std::span<const uint8_t> bytes, pcr::UploadEnd) override {
        if (census_) {
            census_->upload(transfer, bytes.size());
        }
        if (frame_census_ && census_now()) {
            frame_census_->upload(transfer, bytes.size());
        }
        if (drawing_) {
            drawing_->upload(transfer, bytes);
        }
    }
    void local_copy(const pcr::TransferRegisters &transfer) override {
        if (census_) {
            census_->copy(transfer, "local");
        }
        if (frame_census_ && census_now()) {
            frame_census_->local_copy(transfer);
        }
        if (drawing_) {
            drawing_->local_copy(transfer);
        }
    }
    void local_to_host(const pcr::TransferRegisters &transfer) override {
        if (census_) {
            census_->copy(transfer, "read-back");
        }
        if (frame_census_ && census_now()) {
            frame_census_->read_back(transfer);
        }
        if (drawing_) {
            drawing_->local_to_host(transfer);
        }
    }
    void native_draw(uint64_t value) override {
        if (census_) {
            census_->other("native draw (0x7B)");
        }
        if (frame_census_ && census_now()) {
            frame_census_->native_draw(value);
        }
        if (natives_ && drawing_) {
            in_native_ = true;
            natives_->met(value, front_, *drawing_);
            in_native_ = false;
        } else if (RD_ND_COMMAND(value) == RD_ND_FRAME) {
            rd_consumer_frame(value); // (the core waits for the frames' marks to be met)
        }
    }
    void upload_again(uint64_t value) override {
        if (census_) {
            census_->other("upload again (0x7C)");
        }
        if (frame_census_ && census_now()) {
            frame_census_->upload_again();
        }
        if (drawing_) {
            drawing_->upload_again(front_.registers(), value);
        }
    }
    void frame_mark(uint64_t value) override {
        if (census_) {
            census_->other("frame mark (0x7D)");
        }
        if (frame_census_ && census_now()) {
            frame_census_->frame_mark(value);
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
        if (drawing_) {
            drawing_->frame_mark(value);
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
        pcr::Between::LiveKicks kicks;
        std::vector<GsnInterpMove> trail_moves; // (its native kicks', NativeDraws::take_trail_moves)
        uint64_t bytes = 0;
        bool trail_paired = false;
    };
    void open_stretch() {
        if (!natives_ || !between_) {
            return;
        }
        if (canvas_) {
            canvas_->keep_walk();
        }
        front_.number_kicks();
        between_->keep_live_kicks();
        natives_->begin_frame_kept();
        stretch_open_ = true;
        stretch_bytes_ = 0;
    }
    Stretch close_stretch() {
        Stretch s;
        if (canvas_) {
            canvas_->stop_keeping();
            s.walk = canvas_->take_kept_walk();
        }
        front_.stop_numbering();
        s.kicks = between_->end_live_kicks();
        natives_->end_frame_kept();
        s.trail_paired = natives_->trail_paired();
        s.trail_moves = natives_->take_trail_moves();
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
        if (!between_->adopt_live_kicks(std::move(s.kicks), std::move(s.trail_moves))) {
            return pcr::FrameWalkRefused::not_the_frame; // (its kicks not the frame's count)
        }
        kept_walk_ = std::move(picture.walk);
        return pcr::FrameWalkRefused::none;
    }

    pcr::GifFrontEnd front_;
    std::unique_ptr<pcr::Census> census_;
    // (KF4_PC_FRAME_CENSUS: the game's own packets, not a picture between's walk or frame_interp.c's replays)
    std::unique_ptr<pcr::FrameCensus> frame_census_;
    int feeding_path_ = 0;
    bool in_native_ = false;
    bool census_now() const {
        return !in_replay_ && !between_active();
    }
    std::unique_ptr<pcr::GpuTimes> times_; // (before the canvas: it outlives it)
    std::unique_ptr<pcr::Drawing> drawing_;
    pcr::Canvas *canvas_ = nullptr; // (drawing_ when it is the canvas, else nullptr)
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
    // KF4_PC_FRAME_COPIES (0: a frame that copies the buffer drawn walks its pictures, as before; else its own walk
    // makes the copies the stream makes, from the buffer shown: pcr_frame_walk.h's frame_copy_in_picture)
    bool frame_copies_ = true;
    // KF4_PC_FRAME_STRETCH (0: a frame taken in a stretch drops it, as before; else the stretch is the frame's own walk
    // and its uploads the frame's: take_frame), and the bytes of the packets taken in since the last vblank
    bool frame_stretch_ = true;
    uint64_t since_vblank_bytes_ = 0;
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

    // (compute: the final pass in one's program, where it may be used)
    void warm_pipelines(std::vector<pcr::FullscreenPipeline> &passes, std::vector<Program *> &compute, bool sharpen) {
        make_programs();
        if (fused_on_ && two_passes_ && srgb_decode_) {
            make_fused();
            compute.push_back(fused_);
        }
        passes.push_back({scale_, VK_FORMAT_R8G8B8A8_UNORM});
        if (two_passes_) {
            passes.push_back({scale_1d_, VK_FORMAT_R16G16B16A16_SFLOAT});
            passes.push_back({scale_1d_, VK_FORMAT_R8G8B8A8_UNORM});
        }
        if (sharpen) {
            passes.push_back({sharpen_, VK_FORMAT_R8G8B8A8_UNORM});
        }
    }

    // picture into scaled, or with sharpen above 0 into work and sharpened from it into scaled. A picture made with an
    // sRGB view (ScanoutRing::make_picture_image's srgb_view) is decoded to linear light by the sampler as it is read.
    void scale(CommandBuffer &cmd, const Image &picture, const Image &scaled, const Image *work, float sharpen,
               pcr::GpuTimes &times) {
        if (!scale_) {
            make_programs();
        }
        uint32_t pw = picture.get_width(), ph = picture.get_height();
        uint32_t w = scaled.get_width(), h = scaled.get_height();
        bool sharpened = sharpen > 0 && work;
        const Image &to = sharpened ? *work : scaled;
        // (the sampler's sRGB decode in place of the shader's: pow() three times a pixel read, five or more reads a
        // pixel made, where the scaling is the shader's arithmetic more than its reads. 0.6 ms a picture of 1.8 on a
        // Vega 7 at 1080p, 2026-11-18. The sampler's decode is exact; the shader's pow() is within a rounding of it)
        const bool decoded = srgb_decode_ && (picture.get_create_info().misc & IMAGE_MISC_MUTABLE_SRGB_BIT) != 0;
        struct OneDirection {
            int32_t src[2], along[2];
            float ratio;
            int32_t decode, encode;
        };
        // (two passes, across then down, pcr_present_scale_1d.frag: far fewer reads than the one pass. The one pass
        // where the image between them cannot be made, or with KF4_PC_SCALE_PASSES=1, to compare)
        if (two_passes_ && (pw == w || ph == h)) {
            // One direction the picture's own size already (native widescreen's 1920 across at 2x in a 1080p
            // window): the filter there is the identity (the cubic's weights at whole pixels are 1 and 0, and the
            // clamp keeps the pixel), so that pass is left out and the other decodes and encodes alone. The same
            // picture but for the image between's half-float rounding. 1.0 ms a picture less on a Vega 7 at 1080p
            // (2026-11-18): a whole pass writing 8 bytes a pixel.
            const bool down = pw == w;
            const OneDirection one = {{int32_t(pw), int32_t(ph)},
                                      {down ? 0 : 1, down ? 1 : 0},
                                      down ? float(ph) / float(h) : float(pw) / float(w),
                                      decoded ? 0 : 1,
                                      1};
            across_.reset(); // (not needed while the sizes stay)
            times.begin(cmd, down ? pcr::GpuWork::scale_down : pcr::GpuWork::scale_across);
            pass(cmd, to, scale_1d_, picture.get_view(), decoded, &one, sizeof(one));
        } else if (two_passes_ && across_image(w, ph)) {
            const OneDirection across = {{int32_t(pw), int32_t(ph)}, {1, 0}, float(pw) / float(w), decoded ? 0 : 1, 0},
                               down = {{int32_t(w), int32_t(ph)}, {0, 1}, float(ph) / float(h), 0, 1};
            times.begin(cmd, pcr::GpuWork::scale_across);
            pass(cmd, *across_, scale_1d_, picture.get_view(), decoded, &across, sizeof(across));
            times.begin(cmd, pcr::GpuWork::scale_down);
            pass(cmd, to, scale_1d_, across_->get_view(), false, &down, sizeof(down));
        } else {
            struct {
                int32_t src[2];
                float ratio[2];
                int32_t linear;
            } scaling = {{int32_t(pw), int32_t(ph)}, {float(pw) / float(w), float(ph) / float(h)}, 1};
            times.begin(cmd, pcr::GpuWork::scale_across);
            pass(cmd, to, scale_, picture.get_view(), false, &scaling, sizeof(scaling));
        }
        if (sharpened) {
            struct {
                int32_t size[2], origin[2];
                float strength;
            } sharpening = {{int32_t(w), int32_t(h)}, {0, 0}, sharpen};
            times.begin(cmd, pcr::GpuWork::sharpen);
            pass(cmd, scaled, sharpen_, work->get_view(), false, &sharpening, sizeof(sharpening));
        }
    }

    // Whether fused can make a picture of pw x ph into w x h: a tile's source pixels within the shader's shared memory
    // (pcr_present_fused.comp's SRC_MOST), and the two passes' way (not KF4_PC_SCALE_PASSES=1 or KF4_PC_SCALE_SRGB=0,
    // which compare other ways). Developers' only, KF4_PC_FUSED=1: slower than the passes on a Vega 7 (below).
    bool fits(uint32_t pw, uint32_t ph, uint32_t w, uint32_t h) const {
        if (!fused_on_ || !two_passes_ || !srgb_decode_ || pw == 0 || ph == 0 || w == 0 || h == 0) {
            return false;
        }
        // (the most source pixels 18 output pixels' kernels span along a direction, with a pixel to spare each side)
        auto most = [](uint32_t from, uint32_t to) {
            float ratio = float(from) / float(to);
            return 17.0f * ratio + 4.0f * std::max(ratio, 1.0f) + 3.0f;
        };
        const float SRC_MOST = 24.0f;
        const bool down = pw == w || ph != h, across = pw != w; // (as fused has them)
        return (!across || most(pw, w) <= SRC_MOST) && (!down || most(ph, h) <= SRC_MOST);
    }

    // The final pass in one (pcr_present_fused.comp): picture FXAA'd (fxaa) or as it is, scaled and, with sharpen above
    // 0, sharpened, into scaled; the same picture as the passes one by one (scale, after the FXAA pass) but for a level
    // of 255 now and then. Only where fits.
    void fused(CommandBuffer &cmd, const Image &picture, const Image &scaled, bool fxaa, float sharpen,
               pcr::GpuTimes &times) {
        if (!fused_) {
            make_fused();
        }
        uint32_t pw = picture.get_width(), ph = picture.get_height();
        uint32_t w = scaled.get_width(), h = scaled.get_height();
        // (as scale: one direction where the other is the picture's own, down when both are)
        const bool down = pw == w || ph != h, across = pw != w;
        struct {
            int32_t src_size[2], dst_size[2];
            float ratio[2], texel[2];
            float strength;
            int32_t flags;
        } push = {{int32_t(pw), int32_t(ph)},
                  {int32_t(w), int32_t(h)},
                  {float(pw) / float(w), float(ph) / float(h)},
                  {1.0f / float(pw), 1.0f / float(ph)},
                  sharpen,
                  (fxaa ? 1 : 0) | (across ? 2 : 0) | (down ? 4 : 0) | (sharpen > 0 ? 8 : 0) |
                      (across && down ? 16 : 0)};
        push.flags &= ~drop_flags_; // (KF4_PC_FUSED_DROP: a measurement)
        times.begin(cmd, pcr::GpuWork::final_in_one);
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        cmd.set_program(fused_);
        cmd.set_texture(0, 0, picture.get_view(), StockSampler::LinearClamp);
        cmd.set_storage_texture(0, 1, scaled.get_view());
        cmd.set_srgb_texture(0, 2, values_->get_view());
        cmd.set_sampler(0, 2, StockSampler::NearestClamp);
        cmd.set_texture(0, 3, values_->get_view(), StockSampler::NearestClamp);
        cmd.set_uniform_buffer(0, 4, *least_);
        cmd.push_constants(&push, 0, sizeof(push));
        cmd.dispatch((w + 15) / 16, (h + 15) / 16, 1);
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

private:
    // A half float's value (positive, normal or subnormal: the 8-bit range's)
    static float half_to_float(uint16_t h) {
        const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
        return exponent == 0 ? std::ldexp(float(mantissa), -24) : std::ldexp(float(mantissa | 0x400), exponent - 25);
    }

    void make_fused() {
        ResourceLayout layout = {};
        layout.sets[0].sampled_image_mask = 1u | 4u | 8u;
        layout.sets[0].storage_image_mask = 2u;
        layout.sets[0].uniform_buffer_mask = 16u;
        layout.sets[0].fp_mask = 15u;
        for (int b : {0, 1, 2, 3, 4}) {
            layout.sets[0].meta[b].array_size = 1;
        }
        layout.push_constant_size = 10 * sizeof(int32_t);
        fused_ = device_.request_program(
            device_.request_shader(pcr_spv_pcr_present_fused_comp, sizeof(pcr_spv_pcr_present_fused_comp), &layout));
        // (each 8-bit value v at (v, 0), to be read through the image's sRGB view, decoded by the sampler as the passes'
        // reads are, and through its UNORM view)
        uint32_t values[256];
        for (uint32_t v = 0; v < 256; v++) {
            values[v] = v | v << 8 | v << 16 | 0xFF000000u;
        }
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(256, 1, VK_FORMAT_R8G8B8A8_UNORM);
        info.misc |= IMAGE_MISC_MUTABLE_SRGB_BIT;
        ImageInitialData initial = {values, 0, 0};
        values_ = device_.create_image(info, &initial);
        // (each 8-bit value k's least float an 8-bit attachment holds as k, not k - 1: the least half float at or over
        // (k - 0.5) / 255, the export's half floats being rounded toward zero; 0's none)
        float least[256];
        least[0] = -1.0f;
        uint16_t half = 0;
        for (int k = 1; k < 256; k++) {
            const double boundary = (double(k) - 0.5) / 255.0;
            while (half_to_float(half) < boundary) {
                half++;
            }
            least[k] = half_to_float(half);
        }
        BufferCreateInfo buffer = {};
        buffer.domain = BufferDomain::Device;
        buffer.size = sizeof(least);
        buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        least_ = device_.create_buffer(buffer, least);
    }

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
        frag.push_constant_size = 7 * sizeof(int32_t);
        scale_1d_ = device_.request_program(vs, device_.request_shader(pcr_spv_pcr_present_scale_1d_frag,
                                                                       sizeof(pcr_spv_pcr_present_scale_1d_frag), &frag));
        frag.push_constant_size = 5 * sizeof(int32_t);
        sharpen_ = device_.request_program(
            vs, device_.request_shader(pcr_spv_pcr_present_sharpen_frag, sizeof(pcr_spv_pcr_present_sharpen_frag), &frag));
    }

    // A pass over the whole of dst, reading src (after whatever wrote it; read after by anything); srgb: through its
    // sRGB view, the sampler decoding it
    static void pass(CommandBuffer &cmd, const Image &dst, Program *program, const ImageView &src, bool srgb,
                     const void *push, size_t push_size) {
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
        if (srgb) {
            cmd.set_srgb_texture(0, 0, src);
            cmd.set_sampler(0, 0, StockSampler::NearestClamp);
        } else {
            cmd.set_texture(0, 0, src, StockSampler::NearestClamp);
        }
        cmd.push_constants(push, 0, push_size);
        cmd.draw(3);
        cmd.end_render_pass();
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    // The image between the two passes: the output's width, the picture's height, linear light in floats (the
    // cubic's sums before the second pass clamps them). Kept while the sizes stay; false when it cannot be made.
    bool across_image(uint32_t width, uint32_t height) {
        if (across_ && across_->get_width() == width && across_->get_height() == height) {
            return true;
        }
        ImageCreateInfo info = ImageCreateInfo::render_target(width, height, VK_FORMAT_R16G16B16A16_SFLOAT);
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        across_ = device_.create_image(info, nullptr);
        return bool(across_);
    }

    Device &device_;
    Program *scale_ = nullptr, *scale_1d_ = nullptr, *sharpen_ = nullptr, *fused_ = nullptr;
    ImageHandle across_;
    ImageHandle values_;  // (fused's: each 8-bit value)
    BufferHandle least_;  // (fused's: each 8-bit value's least float as an attachment holds it)
    // KF4_PC_FUSED=1: the final pass in one (fused), developers' only. The same pictures byte for byte, but on a Vega 7
    // 3.5 ms a picture against the passes' 2.3 at 1920 x 896 into 1920 x 1080 (2026-11-20,
    // docs/plans/scene-final-pass-results.md): a compute pass of tiles in steps waits on each step's memory with too few
    // waves to hide it, where the fragment passes run many.
    const bool fused_on_ = getenv("KF4_PC_FUSED") && strcmp(getenv("KF4_PC_FUSED"), "1") == 0;
    // KF4_PC_FUSED_DROP=N: the final pass in one without the parts whose flags N has (pcr_present_fused.comp's FUSED_*:
    // 1 FXAA, 8 the sharpening): what each costs, a measurement (wrong pictures)
    const int32_t drop_flags_ = getenv("KF4_PC_FUSED_DROP") ? atoi(getenv("KF4_PC_FUSED_DROP")) : 0;
    const bool two_passes_ = !(getenv("KF4_PC_SCALE_PASSES") && strcmp(getenv("KF4_PC_SCALE_PASSES"), "1") == 0);
    // KF4_PC_SCALE_SRGB=0: the shader decodes the picture's sRGB itself (to compare)
    const bool srgb_decode_ = !(getenv("KF4_PC_SCALE_SRGB") && strcmp(getenv("KF4_PC_SCALE_SRGB"), "0") == 0);
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
    GsnPictureTurn turn{}; // (a picture that shows a picture between: its mouse turn, for the presenter)
};

// The mouse turn of a picture between (its hand, turned at turned_s), as the picture that shows it carries it
GsnPictureTurn picture_turn(const GsnInterpPicture &between, double turned_s) {
    GsnPictureTurn t{};
    if (between.serial == 0 || turned_s <= 0) {
        return t;
    }
    t.known = 1;
    t.sx = between.screen[0];
    t.sy = between.screen[1];
    t.hand_yaw_rad = between.hand_yaw_rad;
    t.hand_pitch_rad = between.hand_pitch_rad;
    t.turned_s = turned_s;
    t.shown_s = between.shown_s;
    return t;
}

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
// (the Vulkan presenter's, pcr_present_sharpens) It draws the sharpening itself, straight into the window
std::atomic<bool> presenter_sharpens_{false};

class PictureOut {
public:
    // hang_notes: its submits' notes and waits (pcr_hang_notes.h); who: its thread, a name that outlives this
    PictureOut(Device &device, ScanoutRing &scanout, PresenterHooks &presenter, Stats &stats, const ClearColour &clear,
               pcr::HangNotes &hang_notes, pcr::AntiAliasHistory &history, const char *who)
        : device_(device), scanout_(scanout), presenter_(presenter), stats_(stats), clear_(clear), scaler_(device),
          antialiasing_(device, history), history_(history), hang_notes_(hang_notes), who_(who) {}

    // (this one's thread, as canvas begins a drawing a picture will show: a picture between's, between; the game's
    // frame, nullptr) Its 3D jittered as the temporal anti-aliasing wants (none when it is not wanted), and what the
    // pictures that show it are kept for them (the canvas stamps the buffers it draws into with its number)
    void begin_drawing(pcr::Drawing *canvas, const GsnInterpPicture *between) {
        pcr::JitterSequence sequence = history_.wanted()
                                           ? antialiasing_.jitter_of(settings().antialias.load(std::memory_order_relaxed))
                                           : pcr::JitterSequence::none;
        pcr::AntiAliasHistory::Drawn d =
            between ? history_.next_drawing(sequence) : history_.drawing_in_step(sequence);
        Drawing &drawing = drawings_[d.number % drawings_.size()];
        drawing = {d.number, {}};
        drawing.facts.jitter = d.jitter;
        drawing.facts.jitter_index = d.index;
        if (between) {
            drawing.facts.camera.known = true;
            std::copy(std::begin(between->mid), std::end(between->mid), drawing.facts.camera.world_to_view.begin());
            std::copy(std::begin(between->screen), std::end(between->screen), drawing.facts.camera.screen.begin());
            drawing.facts.frame = between->serial;
        } else {
            frame_drawing_ = drawing; // (the game's frame: resume_frame_drawing goes back to it)
        }
        if (canvas) {
            canvas->begin_drawing(drawing.number, d.jitter);
        }
    }

    // (kf4-pc, as frame_interp.c hands over a game frame: after its packets were drawn) The frame's drawing given the
    // frame's own camera (its pictures, the vblanks' own, have it then), and the next frame's drawing begun on canvas.
    // (Frames are drawn at a frame's boundary, not a vblank's, so that one frame's 3D has one jitter.)
    void frame_taken(pcr::Drawing *canvas, const GsnInterpFrame &frame) {
        Drawing &d = drawings_[frame_drawing_.number % drawings_.size()];
        if (frame_drawing_.number != 0 && d.number == frame_drawing_.number) {
            pcr::PictureFacts &f = d.facts;
            f.camera.known = true;
            std::copy(std::begin(frame.mid1), std::end(frame.mid1), f.camera.world_to_view.begin());
            std::copy(std::begin(frame.screen[0]), std::end(frame.screen[0]), f.camera.screen.begin());
            f.camera.screen[5] = frame.world_az;
            f.frame = frame.serial;
        }
        begin_drawing(canvas, nullptr);
    }

    // (kf4-pc, after a picture between it walked or gave to the pump) canvas back on the game's frame's drawing
    void resume_frame_drawing(pcr::Drawing *canvas) {
        if (canvas) {
            canvas->begin_drawing(frame_drawing_.number, frame_drawing_.facts.jitter);
        }
    }

    // A vblank's picture, or with GSN_EXTRA one more between two (the buffer shown again: a picture between drawn into
    // it before), from canvas at scale (across | down << 8, the vblank's)
    MadePicture make(const VblankPicture &v, uint32_t scale, pcr::Drawing *canvas, pcr::GpuTimes &times) {
        ZoneScope zone(picture_made_zone);
        MadePicture m;
        bool extra = (v.want & GSN_EXTRA) != 0;
        last_made_.valid = false;
        shown_ok_ = false;
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
        std::optional<PictureSize> size = picture_size(v, extra, scale & 0xFF, picture_down(scale));
        if (!size || !(want & (GSN_WANT_IMAGE | GSN_WANT_PIXELS | GSN_PRESENT))) {
            device_.flush_frame(); // (a picture between submitted for this picture to flush: on the GPU's queue now)
            return m; // (no picture shown, or none wanted)
        }
        // (the final pass in one, PresentScaler::fused: the picture's FXAA left to it, when the picture wanted is the one
        // scaled to the window)
        const uint32_t present_size = settings().present_size.load(std::memory_order_relaxed);
        const int aa = settings().antialias.load(std::memory_order_relaxed);
        const bool fuse = (want & GSN_WANT_IMAGE) && (!(want & GSN_WANT_PIXELS) || (want & GSN_WANT_SCALED)) &&
                          window_size(present_size) && antialiasing_.fusable(aa) &&
                          scaler_.fits(size->width, size->height, present_size >> 16, present_size & 0xFFFF);
        CommandBufferHandle cmd = device_.request_command_buffer();
        times.begin(*cmd, pcr::GpuWork::picture);
        m.drawn = true;
        if (!draw_picture(*cmd, *size, v, canvas, times, fuse)) {
            times.end(*cmd);
            submit_now(cmd);
            return m;
        }
        times.picture();
        port_profile_picture(); // (a picture drawn: the profiler's GPU time a picture divides by these)
        const Image *scaled = nullptr;
        if (want & GSN_WANT_IMAGE) {
            // (the slot holds the picture at the window's size when the presenter asks for that, else the picture whole)
            int slot = scanout_.claim();
            // (the sharpening left to the Vulkan presenter, drawn by it straight into the window in place of its copy: a
            // picture it shows and nothing reads back. KF4_PRESENT_FOLD=0: drawn here, as before)
            const bool presenter_sharpens = presenter_sharpens_.load(std::memory_order_relaxed) &&
                                            (want & GSN_PRESENT) && !(want & GSN_WANT_PIXELS) && !present_pixels;
            scaled = scale_for_window(*cmd, slot, times, present_size, fuse ? aa : -1, presenter_sharpens);
            if (fuse && !scaled) {
                // (no scaled image after all: the picture's FXAA as it would have been)
                times.begin(*cmd, pcr::GpuWork::antialias);
                antialiasing_.apply(*cmd, *picture_, aa, {}, {});
                times.begin(*cmd, pcr::GpuWork::picture);
            }
            const Image *whole = scaled ? nullptr
                                        : scanout_.whole_image(device_, slot, picture_->get_width(), picture_->get_height());
            if (whole) {
                times.begin(*cmd, pcr::GpuWork::slot_copy);
                copy_to_scanout(*cmd, *whole);
                times.begin(*cmd, pcr::GpuWork::picture);
            }
            if (scaled || whole) {
                m.slot = slot;
                m.ok |= GSN_WANT_IMAGE;
                last_slot_ = slot;
                if (!extra && shown_ok_ && !m.scaled_pixels && !(want & GSN_WANT_PIXELS)) {
                    last_made_ = {true, want, shown_rect_, *size, scale, present_size,
                                  settings().present_sharpen.load(std::memory_order_relaxed), aa, slot};
                }
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

    // (slice 5, docs/plans/scene-slice5-results.md) A vblank's own picture asked as the last one made was (the buffer
    // shown, its size, the scale and the final pass's settings), its drawing's scanout of that buffer unchanged since
    // (Drawing::scanout_unchanged): the same picture, which repeat publishes again from its slot with no GPU work. Not
    // with the temporal anti-aliasing (its history moves on each picture), nor for pixels.
    bool can_repeat(const VblankPicture &v, uint32_t scale, pcr::Drawing *canvas) {
        const LastMade &l = last_made_;
        GsnRect rect;
        if (!l.valid || !canvas || (v.want & (GSN_EXTRA | GSN_WANT_PIXELS)) || v.want != l.want ||
            history_.wanted() || scale != l.scale ||
            settings().present_size.load(std::memory_order_relaxed) != l.present_size ||
            settings().present_sharpen.load(std::memory_order_relaxed) != l.sharpen ||
            settings().antialias.load(std::memory_order_relaxed) != l.aa || !display_rect(v.privileged.data(), rect)) {
            return false;
        }
        std::optional<PictureSize> size = picture_size(v, false, scale & 0xFF, picture_down(scale));
        uint64_t smode2 = v.privileged[2 * 2];
        bool field_doubled = (smode2 & 3) == 3 && v.deinterlace;
        const pcr::ShownRect r = {rect.bp, rect.bw, rect.psm, rect.x, rect.y, rect.w, rect.h,
                                  uint32_t(field_doubled && v.field == LOWER_FIELD)};
        const pcr::ShownRect &o = l.shown;
        if (!size || size->width != l.size.width || size->height != l.size.height || r.bp != o.bp || r.bw != o.bw ||
            r.psm != o.psm || r.x != o.x || r.y != o.y || r.w != o.w || r.h != o.h || r.lower_field != o.lower_field) {
            return false;
        }
        return canvas->scanout_unchanged(r);
    }

    // (slice 5) A vblank whose picture is left out: the last picture made (any), its slot published again under v's tag
    MadePicture again(const VblankPicture &v) {
        MadePicture m;
        m.want = v.want;
        m.tag = v.tag;
        m.reply = v.reply;
        m.vblank = (v.want & GSN_EXTRA) == 0;
        // (above 60 the window's own thread shows the pictures by their times and waits for none: nothing handed out)
        const bool window_waits = port_options.frame_rate <= 60;
        if (window_waits && last_slot_ >= 0 && (v.want & GSN_WANT_IMAGE) && !(v.want & GSN_WANT_PIXELS)) {
            m.slot = last_slot_;
            m.ok = GSN_WANT_IMAGE;
        } else {
            m.want &= ~GSN_PRESENT;
        }
        stats_.pictures++;
        stats_.vblanks += m.vblank ? 1 : 0;
        return m;
    }

    // The vblank's picture as the last one made (can_repeat): its slot published again under v's tag
    MadePicture repeat(const VblankPicture &v) {
        MadePicture m;
        m.want = v.want;
        m.tag = v.tag;
        m.reply = v.reply;
        m.vblank = true;
        m.slot = last_made_.slot;
        m.ok = GSN_WANT_IMAGE;
        stats_.pictures++;
        stats_.vblanks++;
        stats_.repeats++;
        return m;
    }

    // (developers', KF4_SCN_REPEAT_CHECK=1) Whether slots a and b hold the same picture, byte for byte: both read back
    bool same_pictures(int a, int b) {
        const Image *ia = scanout_.picture_of(a), *ib = scanout_.picture_of(b);
        if (!ia || !ib || ia->get_width() != ib->get_width() || ia->get_height() != ib->get_height()) {
            return false;
        }
        const VkDeviceSize size = VkDeviceSize(ia->get_width()) * ia->get_height() * 4;
        BufferCreateInfo info = {};
        info.domain = BufferDomain::CachedHost;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        BufferHandle ba = device_.create_buffer(info, nullptr), bb = device_.create_buffer(info, nullptr);
        if (!ba || !bb) {
            return false;
        }
        CommandBufferHandle cmd = device_.request_command_buffer();
        cmd->barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cmd->copy_image_to_buffer(*ba, *ia, 0, {0, 0, 0}, {ia->get_width(), ia->get_height(), 1}, 0, 0, colour);
        cmd->copy_image_to_buffer(*bb, *ib, 0, {0, 0, 0}, {ib->get_width(), ib->get_height(), 1}, 0, 0, colour);
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
        Fence fence;
        device_.submit(cmd, &fence);
        fence->wait();
        const void *ma = device_.map_host_buffer(*ba, MEMORY_ACCESS_READ_BIT);
        const void *mb = device_.map_host_buffer(*bb, MEMORY_ACCESS_READ_BIT);
        bool same = ma && mb && memcmp(ma, mb, size_t(size)) == 0;
        device_.unmap_host_buffer(*ba, MEMORY_ACCESS_READ_BIT);
        device_.unmap_host_buffer(*bb, MEMORY_ACCESS_READ_BIT);
        return same;
    }

    int last_made_slot() const {
        return last_made_.valid ? last_made_.slot : -1;
    }

    // (slice 5) The fence of the picture last submitted (submit_now's): done once the GPU is past it
    Fence last_fence() const {
        return last_fence_;
    }

    // m published, given to the presenter, its sender answered, its vblank done
    void hand_out(const MadePicture &m, Inbox &inbox) {
        ZoneScope zone(picture_handed_out_zone);
        if (m.slot >= 0) {
            scanout_.publish(m.slot, m.tag, m.turn);
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

    // Waits, without Granite's device lock, until no more than in_flight of the pictures this one submitted are
    // still on the GPU (at most PICTURES_FENCED - 1)
    void wait_for_the_gpu(unsigned in_flight) {
        if (in_flight >= PICTURES_FENCED || submitted_ <= in_flight) {
            return;
        }
        Fence &fence = fences_[(submitted_ - 1 - in_flight) % PICTURES_FENCED];
        if (!fence) {
            return;
        }
        pcr::GpuWait wait(&hang_notes_, "a picture's fence", pcr::hang_clock_s());
        fence->wait();
        fence.reset();
    }

private:
    static constexpr unsigned PICTURES_FENCED = 4;

    // cmd submitted and on the GPU's queue, before the picture is handed out: Granite holds a submit without a fence
    // until the next frame context, which only kf4-pc moves on (the pump's pictures would be shown before they are
    // drawn, and drawn up to a vblank late). Its fence kept for wait_for_the_gpu.
    void submit_now(CommandBufferHandle &cmd) {
        hang_notes_.submitted(who_, "a picture", pcr::hang_clock_s());
        Fence &fence = fences_[submitted_ % PICTURES_FENCED];
        fence.reset();
        device_.submit(cmd, &fence);
        device_.flush_frame();
        submitted_++;
        last_fence_ = fence;
    }

    // The picture: the canvas's surface the display shows, scaled to its size (cleared to the picture's colour where
    // there is none). False if the image cannot be made.
    // (leave_aa: the anti-aliasing left to the final pass in one)
    bool draw_picture(CommandBuffer &cmd, PictureSize size, const VblankPicture &v, pcr::Drawing *canvas,
                      pcr::GpuTimes &times, bool leave_aa) {
        if (!picture_ || picture_->get_width() != size.width || picture_->get_height() != size.height) {
            picture_ = ScanoutRing::make_picture_image(device_, size.width, size.height, true);
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
        pcr::ShownRect shown_rect;
        if (canvas && display_rect(v.privileged.data(), rect)) {
            uint64_t smode2 = v.privileged[2 * 2];
            bool field_doubled = (smode2 & 3) == 3 && v.deinterlace;
            shown_rect = {rect.bp, rect.bw, rect.psm, rect.x, rect.y, rect.w, rect.h,
                          uint32_t(field_doubled && v.field == LOWER_FIELD)};
            shown = canvas->scanout(cmd, shown_rect, *picture_);
            shown_rect_ = shown_rect;
            shown_ok_ = shown;
        }
        if (!shown) {
            cmd.clear_image(*picture_, clear_.value);
        } else if (!leave_aa) {
            // (the picture's anti-aliasing, at the canvas's scale: pcr_antialias.h; with its depth and place when the
            // temporal inputs are wanted)
            pcr::PicturePlace place;
            pcr::PictureFacts facts;
            if (history_.wanted()) {
                ShownPicture shown_picture = picture_place(cmd, *canvas, shown_rect);
                place = shown_picture.place;
                facts = facts_of(shown_picture.drawing);
            }
            // (a method of one picture, FXAA, draws into the spare picture, which is the picture from then on: no copy
            // of the picture to read from)
            if (!spare_picture_ || spare_picture_->get_width() != size.width ||
                spare_picture_->get_height() != size.height) {
                spare_picture_ = ScanoutRing::make_picture_image(device_, size.width, size.height, true);
            }
            times.begin(cmd, pcr::GpuWork::antialias);
            bool wrote_spare = false;
            antialiasing_.apply(cmd, *picture_, settings().antialias.load(std::memory_order_relaxed), facts, place,
                                spare_picture_.get(), &wrote_spare);
            if (wrote_spare) {
                std::swap(picture_, spare_picture_);
            }
            times.begin(cmd, pcr::GpuWork::picture);
        }
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        return true;
    }

    // (the temporal anti-aliasing's inputs) Where the picture shown from rect is on the GS's screen, its depth copied
    // for it (place.zscale 0: none), and the drawing whose 3D it shows (0: none)
    struct ShownPicture {
        pcr::PicturePlace place;
        uint64_t drawing = 0;
    };
    ShownPicture picture_place(CommandBuffer &cmd, pcr::Drawing &canvas, const pcr::ShownRect &rect) {
        ShownPicture shown;
        pcr::PicturePlace &place = shown.place;
        uint32_t w = picture_->get_width(), h = picture_->get_height();
        if (rect.w == 0 || rect.h == 0) {
            return shown;
        }
        place.scale = {float(w) / float(rect.w), float(h) / float(rect.h)};
        const Image *depth = antialiasing_.depth_image(w, h);
        std::optional<pcr::ShownDepth> d;
        if (depth) {
            d = canvas.scanout_depth(cmd, rect, *depth);
        }
        if (d) {
            place.zscale = d->zscale;
            place.origin = {d->xyoffset[0] + float(rect.x), d->xyoffset[1] + float(rect.y)};
            place.known = true;
            shown.drawing = d->drawing;
        }
        return shown;
    }

    // Whether size (pcr_set_present_scale's, width << 16 | height) is a window's to scale to
    static bool window_size(uint32_t size) {
        uint32_t w = size >> 16, h = size & 0xFFFF;
        return w != 0 && h != 0 && w <= 8192 && h <= 8192;
    }

    // The picture scaled to the window's size into the slot's scaled image (PresentScaler), when the presenter asks for
    // it (size, pcr_set_present_scale's); that image, or null. fused_aa: the final pass in one (PresentScaler::fused)
    // with the anti-aliasing method it does (PORT_AA_OFF or PORT_AA_FXAA); -1, the passes one by one.
    // presenter_sharpens: the sharpening left to the presenter (the slot's left_to_sharpen)
    const Image *scale_for_window(CommandBuffer &cmd, int slot, pcr::GpuTimes &times, uint32_t size, int fused_aa,
                                  bool presenter_sharpens) {
        if (!window_size(size)) {
            return nullptr;
        }
        uint32_t w = size >> 16, h = size & 0xFFFF;
        float sharpen = float(settings().present_sharpen.load(std::memory_order_relaxed)) / 1000.0f;
        const Image *check = nullptr;
        if (presenter_sharpens && fused_aa < 0 && sharpen > 0) {
            scanout_.left_to_sharpen(slot, sharpen);
            static const bool checking = getenv("KF4_PRESENT_FOLD_CHECK") && strcmp(getenv("KF4_PRESENT_FOLD_CHECK"), "0") != 0;
            if (!checking) {
                sharpen = 0;
            }
        }
        // (the image the scaling writes before the sharpening: one for every slot, written and read within a picture's
        // commands)
        if (fused_aa < 0 && sharpen > 0 && (!scaled_work_ || scaled_work_->get_width() != w || scaled_work_->get_height() != h)) {
            scaled_work_ = ScanoutRing::make_picture_image(device_, w, h);
            if (!scaled_work_) {
                fprintf(stderr, "[pcr] no image for a %ux%u scaled picture\n", w, h);
                return nullptr;
            }
        }
        const Image *scaled = scanout_.scaled(device_, slot, w, h);
        if (!scaled) {
            return nullptr;
        }
        const uint64_t sizes = uint64_t(picture_->get_width()) << 48 | uint64_t(picture_->get_height()) << 32 | size;
        if (sizes != scaled_sizes_ || (fused_aa >= 0) != scaled_fused_) {
            scaled_sizes_ = sizes;
            scaled_fused_ = fused_aa >= 0;
            fprintf(stderr, "[pcr] pictures of %ux%u scaled to %ux%u for the window (%s)\n", picture_->get_width(),
                    picture_->get_height(), w, h, scaled_fused_ ? "the final pass in one" : "pass by pass");
        }
        if (fused_aa >= 0) {
            scaler_.fused(cmd, *picture_, *scaled, fused_aa == PORT_AA_FXAA, sharpen, times);
        } else if (presenter_sharpens && sharpen > 0 && (check = scanout_.check_image(device_, slot))) {
            // (KF4_PRESENT_FOLD_CHECK: scaled into the slot's image as the presenter gets it, and sharpened from it here
            // into the check image)
            scaler_.scale(cmd, *picture_, *check, scaled, sharpen, times);
        } else {
            scaler_.scale(cmd, *picture_, *scaled, sharpen > 0 ? scaled_work_.get() : nullptr, sharpen, times);
        }
        times.begin(cmd, pcr::GpuWork::picture);
        return scaled;
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
        hang_notes_.submitted(who_, "a picture read back", pcr::hang_clock_s());
        device_.submit(cmd, &fence);
        {
            pcr::GpuWait wait(&hang_notes_, "a picture's read-back fence", pcr::hang_clock_s());
            fence->wait();
        }
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
    uint64_t scaled_sizes_ = 0; // (the sizes scale_for_window last logged, and the way)
    bool scaled_fused_ = false;
    pcr::AntiAliasing antialiasing_;
    pcr::AntiAliasHistory &history_; // (both threads')
    // (the temporal anti-aliasing) The last drawings begun (begin_drawing), by their number, and the game frame's
    struct Drawing {
        uint64_t number = 0;
        pcr::PictureFacts facts;
    };
    std::array<Drawing, 8> drawings_{};
    Drawing frame_drawing_;

    // The facts of drawing number (none known when it is not one of the last drawings_)
    pcr::PictureFacts facts_of(uint64_t number) const {
        const Drawing &d = drawings_[number % drawings_.size()];
        return number != 0 && d.number == number ? d.facts : pcr::PictureFacts{};
    }
    pcr::HangNotes &hang_notes_;
    const char *who_;
    std::array<Fence, PICTURES_FENCED> fences_; // (submit_now's, the last PICTURES_FENCED pictures')
    uint64_t submitted_ = 0;
    Fence last_fence_;
    // (slice 5: can_repeat) the last picture made into a slot as a vblank's own, as it was asked, and the scanout of it
    struct LastMade {
        bool valid = false;
        int want = 0;
        pcr::ShownRect shown;
        PictureSize size{};
        uint32_t scale = 0, present_size = 0, sharpen = 0;
        int aa = 0;
        int slot = -1;
    };
    LastMade last_made_;
    int last_slot_ = -1; // (the slot of the last picture made into one: again's)
    pcr::ShownRect shown_rect_; // (draw_picture's last scanout, and whether it showed anything)
    bool shown_ok_ = false;
    ImageHandle picture_;
    ImageHandle spare_picture_; // (the picture's kind and size: what FXAA draws into, swapped with picture_)
    ImageHandle scaled_work_; // (scale_for_window: the picture scaled, before the sharpening)
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
         Inbox &inbox, SharedTurnLead &turn_lead, pcr::HangNotes &hang_notes, pcr::AntiAliasHistory &history)
        : device_(device), inbox_(inbox), turn_lead_(turn_lead), hang_notes_(hang_notes),
          out_(device, scanout, presenter, stats, clear, hang_notes, history, "kf4-pc-pump") {
        const char *in_flight = getenv("KF4_PC_PUMP_IN_FLIGHT");
        if (in_flight && *in_flight) {
            in_flight_ = unsigned(strtoul(in_flight, nullptr, 10));
        }
    }

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
        ZoneScope zone(pc_waits_for_the_pump_room_for_a_frame_zone);
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
        ZoneScope zone(waits_for_the_pump_zone);
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
                "[stats] pc pump: %llu frames (%llu surfaces copied for them, %llu images of theirs spares), %llu pictures between drawn again "
                "(%llu not of its frame), %llu pictures showing them, %llu left out as too late and %llu not shown as made "
                "too late (%.2f ms a picture at the end); its canvas: %llu batches, %llu native draw calls, %llu "
                "passes, %llu surfaces made (%llu of them with images used again), %llu uploads into surfaces, %llu "
                "textures' copies of surfaces in images used again; kept walks' textured batches %llu, %llu from "
                "their table, %llu with their CLUT; %llu runs of them drawn indirect, %llu draws; %llu merged runs, %llu "
                "triangles; %llu batches cleared, %llu blends left out, %llu dead, %llu decals settled\n",
                (unsigned long long)b.frames, (unsigned long long)intake.surfaces_copied,
                (unsigned long long)intake.surface_copies_into_spares,
                (unsigned long long)b.drawn_again, (unsigned long long)b.missed, (unsigned long long)pictures_,
                (unsigned long long)left_out_, (unsigned long long)late_, cost_.mean_s() * 1e3,
                (unsigned long long)c.batches, (unsigned long long)c.native_draw_calls, (unsigned long long)c.passes,
                (unsigned long long)c.surfaces_made, (unsigned long long)c.surface_images_reused,
                (unsigned long long)c.uploads_into_surfaces, (unsigned long long)c.texture_copies_reused,
                (unsigned long long)c.walk_texture_draws, (unsigned long long)c.table_draws,
                (unsigned long long)c.walk_clut_draws, (unsigned long long)c.runs, (unsigned long long)c.run_draws,
                (unsigned long long)c.merged_runs, (unsigned long long)c.merged_triangles,
                (unsigned long long)c.clears, (unsigned long long)c.blends_left_out,
                (unsigned long long)c.dead_batches, (unsigned long long)c.decals_settled);
    }

private:
    // ("kf4-pc-pump") The messages, in the order sent, until stop
    void run() {
        Util::register_thread_index(1); // (Granite's command pools: this thread's)
        port_name_thread("kf4-pc-pump");
        port_play_thread();
        times_ = std::make_unique<pcr::GpuTimes>(device_);
        canvas_ = std::make_unique<pcr::Canvas>(device_, *times_);
        canvas_->watch_hangs(hang_notes_, "kf4-pc-pump");
        between_ = std::make_unique<pcr::Between>(device_);
        for (;;) {
            wait_for_the_gpu();
            std::optional<PumpMessage> message = take();
            if (!message) {
                break;
            }
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
            if (messages_.empty() && !stopping_) {
                ZoneScope zone(pump_waits_for_work_zone);
                wake_.wait(hold, [&] { return stopping_ || !messages_.empty(); });
            }
            if (stopping_) {
                return std::nullopt;
            }
            leave_out_late();
            double early_s = early_by_s();
            if (early_s <= 0) {
                break;
            }
            ZoneScope zone(pump_waits_until_a_picture_s_time_zone);
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
            ZoneScope zone(pump_frame_taken_zone);
        canvas_->start_from(*frame.surfaces);
        between_->take_walked(std::move(frame.walked));
        walk_ = std::move(frame.walk);
    }

    void work(InterpPicture &p) {
        ZoneScope zone(pump_picture_between_zone);
        double began_s = port_wall_seconds();
        turned_before(turn_lead_, turn_to_hand(p.picture, turn_lead_));
        last_turned_s_ = port_wall_seconds();
        last_between_ = p.picture;
        p.picture.run_bits = p.run_bits.empty() ? nullptr : p.run_bits.data();
        out_.begin_drawing(canvas_.get(), &p.picture);
        if (walk_ && begin_picture_again(p.picture)) {
            canvas_->draw_again_alone(*walk_, *between_, picture_next());
            between_->end_picture_again();
        }
        between_took_s_ = port_wall_seconds() - began_s;
    }

    // Before each message is taken (so before a picture between waits for its time): the GPU's queue kept short. The
    // pump used to be held back by Granite's device lock while kf4-pc's next_frame_context waited for the GPU (2.4 ms at
    // a time at 009); without that, it queued pictures faster than the GPU drew them (kf4-pc then waited up to 195 ms
    // for a frame context) and they came late (777 made, not shown, against 50). Neither the mouse turn nor the picture's cost (from its time) counts the wait.
    // KF4_PC_PUMP_IN_FLIGHT: the pump's pictures that may still be on the GPU (1; 4 or more: not waited for).
    void wait_for_the_gpu() {
        ZoneScope zone(pump_waits_for_the_gpu_zone);
        out_.wait_for_the_gpu(in_flight_);
    }

    bool begin_picture_again(const GsnInterpPicture &picture) {
        ZoneScope zone(pump_begin_picture_again_zone);
        return between_->begin_picture_again(picture);
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
        ZoneScope zone(pump_picture_zone);
        pictures_++;
        double began_s = port_wall_seconds();
        MadePicture made = out_.make(e.picture, e.scale, canvas_.get(), *times_);
        late_ += uint64_t(not_shown_when_late(made, last_between_));
        made.turn = picture_turn(last_between_, last_turned_s_);
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
        ZoneScope collect_zone(gpu_times_collected_zone);
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
    pcr::HangNotes &hang_notes_;
    PictureOut out_;
    unsigned in_flight_ = 1; // (KF4_PC_PUMP_IN_FLIGHT; set at construction)
    std::unique_ptr<pcr::GpuTimes> times_; // (before the canvas: it outlives it)
    std::unique_ptr<pcr::Canvas> canvas_;
    std::unique_ptr<pcr::Between> between_;
    std::shared_ptr<const pcr::KeptWalk> walk_;
    uint64_t pictures_ = 0;
    uint64_t left_out_ = 0;         // (under the lock) pictures between left out as too late (unlocked)
    uint64_t late_ = 0;             // (the pump's thread) and those made but too late to show
    GsnInterpPicture last_between_{}; // (the pump's thread) the picture between last drawn (its run bits not kept)
    double last_turned_s_ = 0;        // (the pump's thread) when its mouse turn was taken
    pcr::PictureCost cost_;         // (under the lock: written by the pump's thread, read in take)
    double between_took_s_ = 0;     // (the pump's thread) the last picture between's work, until its picture's
    std::mutex lock_;
    std::condition_variable wake_, idle_;
    std::deque<PumpMessage> messages_;
    int frames_waiting_ = 0; // (PumpFrames in messages_)
    bool busy_ = false, stopping_ = false, gone_ = false;
    std::thread thread_;
};

// ---- Slice 5: the pictures for the window by budget (pcr_budget.h; docs/plans/scene-slice5-results.md) --------------
// The scene renderer's (KF4_SCN_BUDGET=0: as before; never in a run that dumps frames or has no window). kf4-pc times
// each picture it makes for the window from begun to submitted; this thread ("kf4-pc-watch") waits for each one's fence
// in turn and times its GPU work from then (or from the picture before done, when that is later): the budget's
// measures, when the GPU is done with the pictures in flight, and how long before its time each was done.
class PictureWatch {
public:
    struct Sent {
        Fence fence;
        pcr::PictureKind kind = pcr::PictureKind::frame;
        uint64_t serial = 0;
        double begun_s = 0, submitted_s = 0, shown_s = 0;
    };

    void start() {
        thread_ = std::thread([this] { run(); });
    }

    // At exit, after kf4-pc: the pictures in flight waited for (two seconds at most, else the thread is left)
    void stop() {
        if (!thread_.joinable()) {
            return;
        }
        std::unique_lock<std::mutex> hold(lock_);
        stopping_ = true;
        wake_.notify_all();
        bool gone = idle_.wait_for(hold, std::chrono::seconds(2), [&] { return gone_; });
        hold.unlock();
        if (gone) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }

    // (kf4-pc) A picture submitted
    void sent(Sent sent) {
        std::lock_guard<std::mutex> hold(lock_);
        queue_.submitted(sent.serial, sent.submitted_s, budget_.gpu_s(sent.kind));
        pending_.push_back(std::move(sent));
        wake_.notify_one();
    }

    // (kf4-pc) f(budget, the GPU's queue) under the lock
    template <class F> auto with(F f) {
        std::lock_guard<std::mutex> hold(lock_);
        return f(static_cast<const pcr::PictureBudget &>(budget_), static_cast<const pcr::GpuQueueTime &>(queue_));
    }

    // (kf4-pc) Whether a picture between is made (PictureBudget::admit, under the lock)
    bool admit(pcr::PictureKind kind, double now_s, double shown_s, double phase, double vblank_s, bool next_frame) {
        std::lock_guard<std::mutex> hold(lock_);
        return budget_.admit(kind, now_s, queue_.free_s(), shown_s, phase, vblank_s, next_frame);
    }

    // --stats (the threads idle)
    void print(FILE *out) {
        std::lock_guard<std::mutex> hold(lock_);
        if (ahead_ms_.empty()) {
            return;
        }
        std::vector<float> a = ahead_ms_;
        std::sort(a.begin(), a.end());
        auto at = [&](double q) { return a[std::min(a.size() - 1, size_t(q * double(a.size())))]; };
        size_t late = size_t(std::lower_bound(a.begin(), a.end(), 0.0f) - a.begin());
        fprintf(out,
                "[stats] scene renderer pictures timed: %zu, done before their time (ms): p1 %.2f, p10 %.2f, median %.2f, "
                "p90 %.2f; %zu done after it; the budget's last (CPU + GPU ms, the high percentile and margin): a game "
                "frame's picture %.2f + %.2f (%zu measured), a picture between's %.2f + %.2f (%zu), shown by the "
                "vblank's own %.2f + %.2f (%zu), the vblank's scene alone %.2f (%zu)\n",
                a.size(), double(at(0.01)), double(at(0.1)), double(at(0.5)), double(at(0.9)), late,
                budget_.cpu_s(pcr::PictureKind::frame) * 1e3, budget_.gpu_s(pcr::PictureKind::frame) * 1e3,
                budget_.measured(pcr::PictureKind::frame), budget_.cpu_s(pcr::PictureKind::between) * 1e3,
                budget_.gpu_s(pcr::PictureKind::between) * 1e3, budget_.measured(pcr::PictureKind::between),
                budget_.cpu_s(pcr::PictureKind::between_own) * 1e3, budget_.gpu_s(pcr::PictureKind::between_own) * 1e3,
                budget_.measured(pcr::PictureKind::between_own), budget_.gpu_s(pcr::PictureKind::scene) * 1e3,
                budget_.measured(pcr::PictureKind::scene));
    }

private:
    void run() {
        port_name_thread("kf4-pc-watch");
        std::unique_lock<std::mutex> hold(lock_);
        for (;;) {
            wake_.wait(hold, [&] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) {
                break; // (stopping, nothing in flight)
            }
            Sent sent = pending_.front();
            hold.unlock();
            if (sent.fence) {
                ZoneScope zone(watch_waits_for_a_picture_zone);
                sent.fence->wait();
            }
            const double done = port_wall_seconds();
            hold.lock();
            pending_.pop_front();
            if (log_) {
                fprintf(log_, "%d %.6f %.6f %.6f %.6f %.6f %.3f %.3f\n", int(sent.kind), sent.begun_s, sent.submitted_s,
                        done, sent.shown_s, last_done_s_, budget_.cpu_s(sent.kind) * 1e3,
                        budget_.gpu_s(sent.kind) * 1e3);
            }
            budget_.made(sent.kind, sent.submitted_s - sent.begun_s, done - std::max(sent.submitted_s, last_done_s_),
                         sent.begun_s);
            queue_.done(sent.serial, done);
            last_done_s_ = done;
            if (sent.shown_s > 0 && ahead_ms_.size() < MOST) {
                ahead_ms_.push_back(float((sent.shown_s - done) * 1e3));
            }
        }
        gone_ = true;
        idle_.notify_all();
    }

    static constexpr size_t MOST = 1 << 20;
    std::mutex lock_;
    std::condition_variable wake_, idle_;
    std::deque<Sent> pending_;
    pcr::PictureBudget budget_;
    pcr::GpuQueueTime queue_;
    double last_done_s_ = 0;
    std::vector<float> ahead_ms_; // (each picture: its time less when it was done)
    // (developers', KF4_SCN_BUDGET_LOG=FILE) a line a picture: kind begun submitted done shown done_before cpu_ms gpu_ms
    // (times in port_wall_seconds; done_before: the picture before it done; the budget's estimates as it was begun)
    FILE *log_ = [] {
        const char *path = getenv("KF4_SCN_BUDGET_LOG");
        return path && *path ? fopen(path, "w") : nullptr;
    }();
    bool stopping_ = false, gone_ = false;
    std::thread thread_;
};

// ---- The renderer ------------------------------------------------------------------------------------
std::string &pipeline_cache_path_before_open(); // (below)

class Renderer {
public:
    // The device (with the presenter's extensions when present_wanted, on a GPU and queue that can present to
    // make_surface's window when given), and the thread. False without one.
    // hang_folder: where a GPU hang's report goes (pcr_hang_reports_in)
    bool open(bool present_wanted, PcrMakeSurface make_surface, const std::string &hang_folder) {
        if (!Context::init_loader(nullptr)) {
            fprintf(stderr, "[pcr] no Vulkan loader\n");
            return false;
        }
        present_wanted_ = present_wanted;
        if (!make_context(make_surface)) {
            fprintf(stderr, "[pcr] no Vulkan device\n");
            return false;
        }
        device_ = std::make_unique<Device>();
        device_->set_context(*context_);
        // GRANITE_VULKAN_POST_MORTEM=1: say whether a hang will name its draw (markers) or not
        const auto &features = device_->get_device_features();
        if (features.supports_post_mortem) {
            fprintf(stderr, "[pcr] post-mortem on (AMD buffer markers %s)\n",
                    features.supports_amd_buffer_marker ? "yes" : "no");
        }
        // a frame context a picture: with four, the thread waits at a picture for the GPU's work of four before
        device_->init_frame_contexts(4);
        // the pipelines kept between runs (pcr_pipelines.h): before anything draws, so that every pipeline is made
        // through the cache
        if (!pipeline_cache_path_before_open().empty()) {
            pcr::store_open(*device_, pipeline_cache_path_before_open().c_str());
        }
        const VkPhysicalDeviceProperties &props = device_->get_gpu_properties();
        gpu_kind_ = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? GSN_GPU_DISCRETE
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? GSN_GPU_INTEGRATED
                                                                                 : GSN_GPU_OTHER;
        out_.emplace(*device_, scanout_, presenter_, stats, clear_, hang_notes_, aa_history_, "kf4-pc");
        const char *watch = getenv("KF4_PC_HANG_WATCH");
        if (!(watch && strcmp(watch, "0") == 0)) {
            hang_watch_ = std::make_unique<pcr::HangWatch>(hang_notes_, hang_folder, pcr::HANG_SECONDS);
        }
        if (pump_wanted()) {
            pump_ = std::make_unique<Pump>(*device_, scanout_, presenter_, stats, clear_, inbox_, turn_lead_,
                                           hang_notes_, aa_history_);
            pump_->start();
            const char *direct = getenv("KF4_PC_DIRECT");
            direct_on_ = !(direct && strcmp(direct, "0") == 0);
            const char *wait = getenv("KF4_PC_CONTEXT_WAIT");
            try_contexts_ = !(wait && strcmp(wait, "1") == 0);
            const char *locked = getenv("KF4_PC_GPU_WAIT_LOCKED");
            gpu_wait_unlocked_ = !(locked && strcmp(locked, "1") == 0);
        }
        budget_on_ = budget_wanted();
        ahead_on_ = budget_on_ && ahead_wanted();
        const char *force = getenv("KF4_SCN_AHEAD_FORCE");
        ahead_force_ = drawing_maker() != nullptr && !pump_ && port_options.headless && force && strcmp(force, "0") != 0;
        if (budget_on_) {
            watch_.start();
        }
        thread_ = std::thread([this] { run(); });
        // (the driver too: a crash or a wrong picture on one driver is told apart by it in a player's log)
        const VkPhysicalDeviceVulkan12Properties &v12 = device_->get_device_features().vk12_props;
        fprintf(stderr, "[pcr] %s on %s (vendor %04x device %04x, driver %s %s, version %08x, Vulkan %u.%u)\n",
                drawing_name(), props.deviceName, props.vendorID, props.deviceID, v12.driverName[0] ? v12.driverName : "?",
                v12.driverInfo, props.driverVersion, VK_API_VERSION_MAJOR(props.apiVersion),
                VK_API_VERSION_MINOR(props.apiVersion));
        return true;
    }

    void precompile(bool scaling, bool sharpen) {
        // (started once the drawing's pipelines are known too: the canvas's programs (warm_watch), or another
        // renderer's list of its own (warm_drawing: the scene renderer's Builder))
        std::vector<pcr::FullscreenPipeline> passes;
        std::vector<Program *> compute;
        pcr::AntiAliasing::warm_pipelines(*device_, settings.antialias.load(), passes);
        if (scaling) {
            PresentScaler scaler(*device_);
            scaler.warm_pipelines(passes, compute, sharpen);
        }
        pcr::precompile(std::move(passes), std::move(compute));
    }

    // What was sent done, two seconds at most (the pump's likewise, after kf4-pc's: kf4-pc may wait for it)
    void settle() {
        put_later();
        inbox_.wait_idle_for(std::chrono::seconds(2));
        if (pump_) {
            pump_->wait_idle_for(std::chrono::seconds(2));
        }
    }

    // At exit: what was sent is done (two seconds at most), then the thread stops; if it does not within two
    // seconds more it is left (the process ends with it)
    // (the pump's thread likewise, after kf4-pc's: kf4-pc may wait for it)
    void stop() {
        settle();
        inbox_.stop();
        if (inbox_.wait_gone_for(std::chrono::seconds(2))) {
            thread_.join();
        } else {
            thread_.detach();
        }
        if (pump_) {
            pump_->stop();
        }
        watch_.stop();
    }

    // vblanks: gs.c's own at a vblank (GifPacket::vblanks)
    void gif(int path, const void *data, size_t bytes, bool vblanks = false) {
        bool ahead = false;
        uint32_t passed = 0;
        if (ahead_force_) {
            if (vblanks) {
                put_later();
            } else {
                std::lock_guard<std::mutex> hold(later_lock_);
                ahead = !later_.empty();
                passed = uint32_t(later_.size());
                force_passed_ += ahead ? 1 : 0;
            }
        }
        const uint8_t *p = static_cast<const uint8_t *>(data);
        inbox_.put(GifPacket{path, std::vector<uint8_t>(p, p + bytes), vblanks, ahead, passed}, bytes, false);
    }

    // Everything sent before done: kf4-pc's, then the pump's (which only kf4-pc sends to)
    void wait() {
        put_later();
        inbox_.wait_idle();
        if (pump_) {
            pump_->wait_idle();
        }
    }

    // later_name (KF4_SCN_AHEAD_FORCE's, gs.c's dump of an extra picture): the picture dumped as that file once made
    int picture(const uint64_t *privileged, int field, int want, PortGsFrame *frame, uint64_t tag,
                const char *later_name = nullptr) {
        VblankPicture v = {};
        memcpy(v.privileged.data(), privileged, sizeof(v.privileged));
        v.field = field;
        v.want = want;
        v.progressive = port_options.progressive != 0;
        v.deinterlace = port_options.deinterlace != 0;
        v.tag = tag;
        v.sent_s = port_wall_seconds();
        bool extra = (want & GSN_EXTRA) != 0;
        upper_field_.show_in(v, extra);
        // (the vblank's scale, as kf4-pc takes it: a picture's is the one of the last vblank's before it)
        uint32_t scale = send_scale_;
        if (!extra) {
            send_scale_ = drawing_scale(settings.scale.load(), v.privileged.data(), v.deinterlace);
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
        if (ahead_force_) {
            if (extra && (queued || later_name)) {
                Later later;
                if (!queued) {
                    later.pixels.reset(new uint32_t[PORT_GS_FRAME_PIXELS]);
                    later.frame = std::make_unique<PortGsFrame>();
                    later.frame->rgba = later.pixels.get();
                    later.reply = std::make_unique<Reply>();
                    later.name = later_name;
                    v.frame = later.frame.get();
                    v.reply = later.reply.get();
                }
                later.message = std::move(v);
                std::lock_guard<std::mutex> hold(later_lock_);
                later_.push_back(std::move(later));
                force_later_++;
                return want & (GSN_PRESENT | GSN_WANT_IMAGE | GSN_WANT_PIXELS);
            }
            put_later();
        }
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
        put_later();
        inbox_.put(InterpFrame(frame), 0, false);
    }

    // (developers', KF4_SCN_AHEAD_FORCE=1: a headless run of the scene renderer) The packets taken ahead of the pictures
    // between (take_packets_ahead) as far as they go, to compare byte for byte with a run without: each picture between
    // and the extra picture that shows it held back here, and sent after the game's packets that follow them, before
    // the next of anything else (a vblank's own picture, a frame, a job, a vblank's own packets, a wait). An extra
    // picture gs.c dumps is dumped once made (picture's later_name).
    struct Later {
        Message message;
        std::unique_ptr<uint32_t[]> pixels;
        std::unique_ptr<PortGsFrame> frame;
        std::unique_ptr<Reply> reply;
        std::string name;
    };
    void put_later() {
        if (!ahead_force_) {
            return;
        }
        std::vector<Later> list;
        {
            std::lock_guard<std::mutex> hold(later_lock_);
            list.swap(later_);
        }
        for (Later &later : list) {
            inbox_.put(std::move(later.message), 0, false);
        }
        for (Later &later : list) {
            if (later.reply && (inbox_.wait_reply(*later.reply) & GSN_WANT_PIXELS)) {
                port_video_dump_named(later.frame.get(), later.name.c_str());
            }
        }
    }

    // (gs.c) KF4_SCN_AHEAD_FORCE's: an extra picture to dump sent through picture's later_name
    bool pictures_later() const {
        return ahead_force_;
    }

    // A picture between: to kf4-pc, or straight to the pump when the pump has this frame's walk from kf4-pc already
    // and kf4-pc has taken every picture before it that it might give the pump or make itself (so the pump's queue
    // keeps the pictures' order). kf4-pc's inbox then holds the next frame's packets ahead of it (sent between this
    // vblank and the one before), which the picture need not wait for: drawn from the kept walk, it reads nothing of
    // kf4-pc's store or surfaces. kf4-pc is sent it too, to count it and to stop this when the walk no longer fits
    // (take_picture_between). KF4_PC_DIRECT=0: every picture between through kf4-pc (docs/port/PERFORMANCE.md).
    void interp_picture(const GsnInterpPicture &picture) {
        InterpPicture p = {picture, {}};
        p.sent_s = port_wall_seconds();
        if (picture.runs && picture.run_bits) {
            p.run_bits.assign(picture.run_bits, picture.run_bits + (size_t(picture.runs) + 31) / 32);
        }
        p.picture.run_bits = nullptr; // (set to the kept bits where it is taken)
        bool direct = direct_on_ && port_options.frame_rate != PORT_FRAME_RATE_UNLOCKED && picture.serial != 0 &&
                      direct_serial_.load(std::memory_order_acquire) == picture.serial &&
                      routed_done_.load(std::memory_order_acquire) == routed_ && port_interp_behind_lately();
        direct_show_ = direct;
        normal_show_ = !direct;
        if (!direct) {
            p.counted = true;
            routed_++;
            if (ahead_force_) {
                Later later;
                later.message = std::move(p);
                std::lock_guard<std::mutex> hold(later_lock_);
                later_.push_back(std::move(later));
                return;
            }
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

    uint32_t clear_rgba() const {
        return clear_.rgba;
    }

    // fn on the PC renderer's thread, after everything sent before; returns once it has run (false if the thread
    // is gone)
    bool run_job(std::function<void()> fn) {
        put_later();
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
    pcr::Drawing *drawing_after_wait() {
        put_later();
        inbox_.wait_idle();
        return intake_.drawing();
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
    // them, else (or when that fails) without. With make_surface, the GPU and its graphics queue are ones that can
    // present to the window: chosen without it, the GPU or the queue may not (NVIDIA on Windows, 2026-11-18:
    // "the PC renderer's queue cannot present to the window", and the window fell back to SDL_Renderer)
    bool make_context(PcrMakeSurface make_surface) {
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
            if (make_instance(instance_ext.data(), uint32_t(instance_ext.size()), context_flags)) {
                VkSurfaceKHR window = window_surface(make_surface);
                bool made = context_->init_device(VK_NULL_HANDLE, window, device_ext.data(), uint32_t(device_ext.size()),
                                                  context_flags);
                if (window != VK_NULL_HANDLE) {
                    vkDestroySurfaceKHR(context_->get_instance(), window, nullptr);
                }
                if (made) {
                    return true;
                }
                if (window != VK_NULL_HANDLE) {
                    fprintf(stderr, "[pcr] no Vulkan GPU can present to the window (is the screen plugged into the graphics "
                                    "card?)\n");
                }
            }
            fprintf(stderr, "[pcr] no Vulkan device with a swapchain: the window is shown without one\n");
        }
        present_wanted_ = false;
        if (!make_instance(nullptr, 0, context_flags) ||
            !context_->init_device(VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, 0, context_flags)) {
            context_.reset();
            return false;
        }
        return true;
    }

    // Granite's instance in context_. When a GPU has no VK_KHR_synchronization2 of its own (a driver before Vulkan 1.3),
    // made again with the layer that adds it, which Granite cannot make a device without (pcr_sync2.h).
    bool make_instance(const char *const *instance_ext, uint32_t instance_ext_count,
                       Vulkan::ContextCreationFlags context_flags) {
        context_ = std::make_unique<Context>();
        context_->set_num_thread_indices(2); // (kf4-pc's and the pump's)
        if (!context_->init_instance(instance_ext, instance_ext_count, context_flags)) {
            return false;
        }
        uint32_t version = context_->get_enabled_device_features().instance_api_core_version;
        if (!pcr::sync2::instance_needs_layer(context_->get_instance(), version)) {
            return true;
        }
        if (!pcr::sync2::loader_has_layer()) {
            fprintf(stderr, "[pcr] and %s is not beside the game (%s/): a newer driver is needed\n",
                    pcr::sync2::LAYER_NAME, pcr::sync2::LAYERS_FOLDER);
            return true; // (Granite then says what it lacks)
        }
        fprintf(stderr, "[pcr] VK_KHR_synchronization2 from %s\n", pcr::sync2::LAYER_NAME);
        context_ = std::make_unique<Context>();
        context_->set_num_thread_indices(2);
        context_->set_instance_factory(&pcr::sync2::layer_instance_factory());
        return context_->init_instance(instance_ext, instance_ext_count, context_flags);
    }

    // The window's surface on the instance made (VK_NULL_HANDLE without make_surface or when it fails: the GPU is then
    // chosen without the window)
    VkSurfaceKHR window_surface(PcrMakeSurface make_surface) const {
        uint64_t surface = 0;
        if (make_surface == nullptr) {
            return VK_NULL_HANDLE;
        }
        if (!make_surface(context_->get_instance(), &surface)) {
            fprintf(stderr, "[pcr] no Vulkan surface for the window: the GPU is chosen without it\n");
            return VK_NULL_HANDLE;
        }
        return reinterpret_cast<VkSurfaceKHR>(surface);
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
        intake_.attach(*device_, hang_notes_);
        port_name_thread("kf4-pc");
        port_play_thread();
        double idle_from = port_wall_seconds();
        while (std::optional<Message> message = inbox_.take()) {
            const double taken = port_wall_seconds();
            note(DOING_IDLE, idle_from, taken);
            struct Done {
                Renderer &r;
                double &idle_from;
                double ended_s = 0; // (a packet's end, already read: the clock is read once a packet)
                ~Done() {
                    idle_from = ended_s > 0 ? ended_s : port_wall_seconds();
                }
            } done_at{*this, idle_from};
            if (ahead_force_) {
                GifPacket *packet = std::get_if<GifPacket>(&*message);
                const size_t bytes = packet ? packet->bytes.size() : 0;
                const bool ahead = packet && packet->ahead;
                take_forced(*message); // (taken ahead or held back; or before anything else, those held back)
                if (ahead) {
                    inbox_.done(bytes);
                    continue;
                }
            }
            if (GifPacket *packet = std::get_if<GifPacket>(&*message)) {
                done_at.ended_s = take_packet(*packet, taken);
                inbox_.done(packet->bytes.size());
            } else if (VblankPicture *v = std::get_if<VblankPicture>(&*message)) {
                if (v->want & GSN_EXTRA) {
                    picture_taken_for_ahead();
                }
                bool counted = v->counted;
                take_picture(std::move(*v));
                if (counted) {
                    routed_done_.fetch_add(1, std::memory_order_release); // (what it gave the pump sent)
                }
                inbox_.done(0);
            } else if (Job *job = std::get_if<Job>(&*message)) {
                direct_serial_.store(0, std::memory_order_release); // (a state saved or loaded: through kf4-pc again)
                job->work();
                note(DOING_OTHER, taken, port_wall_seconds());
                inbox_.answer(job->reply, 1);
                inbox_.done(0);
            } else if (InterpFrame *frame = std::get_if<InterpFrame>(&*message)) {
                stats.interp_frames++;
                direct_serial_.store(0, std::memory_order_release); // (a new frame: its walk not the pump's yet)
                out_->frame_taken(intake_.drawing(), **frame);
                intake_.take_frame(std::move(*frame));
                const double frame_end_s = port_wall_seconds();
                intake_times.frame_taken(frame_end_s);
                note(DOING_FRAME, taken, frame_end_s);
                done_at.ended_s = frame_end_s;
                inbox_.done(0);
            } else if (InterpPicture *p = std::get_if<InterpPicture>(&*message)) {
                picture_taken_for_ahead();
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

    // (kf4-pc) A packet of the GS's input taken in (begun at from_s): in its turn, or ahead of the pictures before it
    // (take_packets_ahead). Its end, the one read of the clock it makes: ~28 000 packets a second, and a clock can cost
    // 1 to 2 us a read on Windows (port_clock_cost)
    double take_packet(const GifPacket &packet, double from_s) {
        stats.packets++;
        stats.bytes += packet.bytes.size();
        intake_.feed(packet.path, packet.bytes);
        const double to = port_wall_seconds();
        intake_times.packet_taken(from_s, to);
        note(DOING_PACKETS, from_s, to);
        if (ahead_on_) {
            packet_s_.add(to - from_s);
        }
        return to;
    }

    // (KF4_SCN_AHEAD_FORCE's, kf4-pc) A packet sent past the pictures between held back taken ahead of them as
    // take_packets_ahead would: where the drawing has them come before those pictures (packets_ahead), else held here
    // and taken in their turn, after the pictures (before anything else)
    void take_forced(Message &message) {
        GifPacket *packet = std::get_if<GifPacket>(&message);
        if (packet && packet->ahead) {
            pcr::Drawing *drawing = intake_.drawing();
            if (forced_held_.empty() && !ahead_stopped_ && drawing && drawing->packets_ahead()) {
                ahead_left_ = std::max(ahead_left_, packet->passed);
                take_packet(*packet, port_wall_seconds());
                ahead_packets_++;
                ahead_bytes_ += packet->bytes.size();
                if (!drawing->packets_ahead_open()) {
                    ahead_cut_++;
                    ahead_stopped_ = true;
                }
            } else {
                if (forced_held_.empty()) {
                    forced_held_left_ = packet->passed;
                }
                forced_held_.push_back(std::move(*packet));
            }
            return;
        }
        const bool picture = std::holds_alternative<InterpPicture>(message) ||
                             (std::holds_alternative<VblankPicture>(message) &&
                              (std::get<VblankPicture>(message).want & GSN_EXTRA));
        if (picture && forced_held_left_ > 0) {
            forced_held_left_--; // (one the packets held were sent after)
            return;
        }
        for (GifPacket &held : forced_held_) {
            take_packet(held, port_wall_seconds());
        }
        forced_held_.clear();
    }

    // (kf4-pc) A picture between or an extra picture taken: one of those the packets taken ahead were sent after
    // (ahead_left_), or one sent after them, which sees them (the drawing draws what was added so far first)
    void picture_taken_for_ahead() {
        if (ahead_left_ > 0) {
            ahead_left_--;
            return;
        }
        pcr::Drawing *drawing = intake_.drawing();
        if (drawing && drawing->packets_ahead_open()) {
            drawing->packets_ahead_end();
        }
    }

    // (kf4-pc) A vblank's picture, or an extra one: the pump's when it shows the picture between the pump drew (the
    // last picture between taken was the pump's, and no picture has shown it yet: gs.c draws a picture between, then
    // the picture that shows it), else made here and handed out in order
    void take_picture(VblankPicture v) {
        ZoneScope zone(pc_take_picture_zone);
        bool extra = (v.want & GSN_EXTRA) != 0;
        if (!extra) {
            ahead_stopped_ = false;
            ahead_left_ = 0;
            const double from = port_wall_seconds();
            intake_.vblank(v.privileged.data(), drawing_scale(settings.scale.load(), v.privileged.data(), v.deinterlace),
                           settings.lod.load(), settings.aniso.load(),
                           settings.mode_2d.load(), settings.font.load(), settings.glyphs_now(), v.tag >> 16);
            note(DOING_VBLANK, from, port_wall_seconds());
            if (budget_on_ && !timing_.on) {
                // (slice 5) the vblank's scene, closed alone (not in a picture's midst): its GPU work timed by a fence
                // after it, ahead of the pictures after it
                CommandBufferHandle cmd = device_->request_command_buffer();
                Fence fence;
                const double now = port_wall_seconds();
                device_->submit(cmd, &fence);
                watch_.sent({fence, pcr::PictureKind::scene, ++timed_serial_, now, now, 0});
            }
        }
        uint32_t scale = picture_scale_;
        if (!extra) {
            // (a new scale from the next vblank)
            picture_scale_ = drawing_scale(settings.scale.load(), v.privileged.data(), v.deinterlace);
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
            if (pcr::Drawing *drawing = intake_.drawing()) {
                drawing->submit();
            }
            frame_context_owed();
            return;
        }
        if (!extra) {
            pay_frame_contexts(); // (the canvas submitted at the vblank)
        }
        if (extra && between_left_out_) {
            // (slice 5: the picture between it shows left out by the budget) nothing made, nothing shown
            between_left_out_ = false;
            MadePicture none;
            none.want = v.want & ~GSN_PRESENT;
            none.tag = v.tag;
            none.reply = v.reply;
            hand_out_in_order(none);
            return;
        }
        if (!extra && between_left_out_ && left_out_frame_own_) {
            // (the game frame's first vblank, the mouse turned: the picture between left out showed the frame at t 0)
            // the frame's own picture instead, as without the mouse: its steps held back (slice 4b) drawn late by it. Not
            // the last picture again: where no picture between fits (a GPU slower than the scale asks), nothing new was
            // shown while the mouse turned, the view stood still until it stopped (walking, the frames' own pictures
            // showed it at 30 a second). Only where no picture between was made lately (take_picture_between): where the
            // others are, the frame's own picture is the view of two game frames before them, and the last picture
            // again is right (the owner's report of 2026-11-22: a ghost while turning, unlocked at 1440p).
            between_left_out_ = false;
            left_out_frame_own_ = false;
            budget_frame_instead_++;
        }
        if (!extra && between_left_out_) {
            // (slice 5: the vblank's own picture, a picture between left out by the budget) the last picture again, so
            // that the window has this vblank's (at 60 and below it waits for one a while)
            between_left_out_ = false;
            MadePicture again = out_->again(v);
            last_between_ = {};
            hand_out_in_order(again);
            return;
        }
        if (!extra && !timing_.on && (v.want & GSN_PRESENT) && budget_on_) {
            frames_shown_lately_ = true;
        }
        if (!extra && !timing_.on && (v.want & GSN_PRESENT)) {
            // (slice 5) the vblank's own picture of the game's frame: the last one again where nothing it shows changed,
            // else begun no sooner than its cost before its time
            if (repeat_on_ && repeat_check_ && out_->can_repeat(v, scale, intake_.drawing())) {
                // (developers': made all the same, and compared with the picture it would have shown again)
                const int was = out_->last_made_slot();
                MadePicture made = out_->make(v, scale, intake_.drawing(), *intake_.gpu_times());
                stats.repeat_checks++;
                if (made.slot < 0 || !out_->same_pictures(was, made.slot)) {
                    stats.repeat_differ++;
                }
                if (pcr::Drawing *drawing = intake_.drawing()) {
                    drawing->picture_made();
                }
                last_between_ = {};
                if (made.drawn) {
                    next_frame_context();
                }
                hand_out_in_order(made);
                return;
            }
            if (repeat_on_ && out_->can_repeat(v, scale, intake_.drawing())) {
                MadePicture made = out_->repeat(v);
                if (pcr::Drawing *drawing = intake_.drawing()) {
                    drawing->picture_made();
                }
                last_between_ = {};
                hand_out_in_order(made);
                return;
            }
            const double shown = budget_on_ ? port_video_picture_due_s(v.tag) : 0;
            sent_s_ = v.sent_s;
            if (shown > 0) {
                wait_until(watch_.with([&](const pcr::PictureBudget &b, const pcr::GpuQueueTime &) {
                    return b.begin_at_s(pcr::PictureKind::frame, shown);
                }));
                timing_ = {true, pcr::PictureKind::frame, port_wall_seconds(), shown};
            }
        }
        if (aa_history_.wanted() && pump_ && !pump_->idle()) {
            // (the temporal anti-aliasing's history is in the order pictures are made: made after the pump's before it,
            // as it is handed out after them)
            pump_->wait_idle();
        }
        const double make_from = port_wall_seconds();
        MadePicture made = out_->make(v, scale, intake_.drawing(), *intake_.gpu_times());
        note(DOING_PICTURE, make_from, port_wall_seconds());
        if (timing_.on) {
            // (slice 5) its GPU work timed by the watch from its fence
            if (made.slot >= 0 && made.drawn && (made.want & GSN_PRESENT)) {
                watch_.sent({out_->last_fence(), timing_.kind, ++timed_serial_, timing_.begun_s, port_wall_seconds(),
                             timing_.shown_s});
            }
            timing_.on = false;
        }
        if (pcr::Drawing *drawing = intake_.drawing()) {
            drawing->picture_made();
        }
        if (extra) {
            stats.interp_late += uint64_t(not_shown_when_late(made, last_between_));
        }
        made.turn = picture_turn(last_between_, last_turned_s_); // (a vblank's own picture shows one too, at 60)
        last_between_ = {}; // (shown by this picture, or not one's: not again)
        if (made.drawn) {
            next_frame_context();
        }
        hand_out_in_order(made);
    }

    // (kf4-pc) Granite's frame context moved on: waits for the GPU's work of the context it comes round to
    void next_frame_context() {
        const double from = port_wall_seconds();
        wait_for_the_gpu_unlocked();
        ZoneScope zone(pc_next_frame_context_waits_for_the_gpu_zone);
        pcr::GpuWait wait(&hang_notes_, "kf4-pc's next_frame_context", pcr::hang_clock_s());
        device_->next_frame_context();
        note(DOING_GPU_WAIT, from, port_wall_seconds());
    }

    // (kf4-pc) Before the frame context moves on: the GPU's work of the context it comes round to waited for without
    // Granite's device lock (cmake/granite_frame_context_wait.cmake), so next_frame_context, which waits holding it,
    // finds it done and the pump, which takes the lock for every buffer and command buffer, is not held up by the GPU
    // (it was, 2.4 ms at a time at 009). A timeout (a hang: HangWatch's to note) leaves the wait to
    // next_frame_context, as before. KF4_PC_GPU_WAIT_LOCKED=1: not waited for here, as before.
    void wait_for_the_gpu_unlocked() {
        if (!gpu_wait_unlocked_) {
            return;
        }
        constexpr uint64_t TIMEOUT_NS = 1000ull * 1000 * 1000;
        ZoneScope zone(pc_waits_for_the_gpu_unlocked_zone);
        pcr::GpuWait wait(&hang_notes_, "kf4-pc's wait for the next frame context", pcr::hang_clock_s());
        device_->wait_next_frame_context_idle(TIMEOUT_NS);
    }

    // (kf4-pc) A picture the pump drew: a frame context moved on for it (above), now when that waits for nobody's open
    // command buffer (the pump's, mostly: kf4-pc waited 0.87 ms a picture for it at 500, 32% of its time), else owed,
    // and waited for only when FRAME_CONTEXTS_OWED_MOST are owed. Its canvas's submit goes to the GPU's queue at once
    // either way (next_frame_context flushed it). KF4_PC_CONTEXT_WAIT=1: moved on at each picture, waiting, as before.
    void frame_context_owed() {
        if (!try_contexts_ || !port_interp_behind_lately()) {
            // (and those owed paid when that waits for nobody: kept up, as before the owing)
            pay_frame_contexts();
            next_frame_context();
            return;
        }
        contexts_owed_++;
        pay_frame_contexts();
        if (contexts_owed_ > FRAME_CONTEXTS_OWED_MOST) {
            next_frame_context();
            contexts_owed_--;
        } else if (contexts_owed_ > 0) {
            device_->flush_frame();
        }
    }

    // (kf4-pc, its canvas submitted) The frame contexts owed moved on while that waits for nobody's open command
    // buffer. Each still waits for the GPU, as next_frame_context does (outside Granite's lock): moved on only when the
    // GPU was already done (a test, not a wait), up to 8 contexts' work queued up and the pump's pictures came late
    // (932 not shown as made too late at 009, against 50).
    void pay_frame_contexts() {
        while (contexts_owed_ > 0) {
            wait_for_the_gpu_unlocked();
            if (!device_->try_next_frame_context()) {
                return;
            }
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
        ZoneScope zone(pc_take_picture_between_zone);
        if (p.direct) {
            between_made_s_ = port_wall_seconds();
            last_between_ = p.picture;
            shows_pumped_ = true;
            if (!intake_.direct_picture(p.picture)) {
                direct_serial_.store(0, std::memory_order_release);
                stats.direct_stale++;
            }
            return;
        }
        timing_.on = false;
        sent_s_ = p.sent_s;
        if (budget_on_ && p.picture.shown_s > 0) {
            // (slice 5) begun no sooner than its cost before its time (and where the next vblank's own picture is a game
            // frame's, before that one's GPU work); left out, with the picture that would show it, when it would not be
            // ready then. The game frame's first vblank's own picture is the frame's, unless the mouse turned it.
            const double shown = p.picture.shown_s, phase = p.picture.phase;
            if (phase == 0 && p.picture.t == 0) {
                frames_shown_lately_ = false; // (the frame's own vblank shown as a picture between: the mouse)
            }
            const bool next_frame = frames_shown_lately_ && p.picture.t + (1.0 - phase) * 0.5 >= 0.999;
            const pcr::PictureKind kind = phase == 0 ? pcr::PictureKind::between_own : pcr::PictureKind::between;
            wait_until(watch_.with([&](const pcr::PictureBudget &b, const pcr::GpuQueueTime &) {
                           return b.between_begin_at_s(kind, shown, phase, port_vblank_wall_seconds(), next_frame);
                       }),
                       true);
            const double now = port_wall_seconds();
            if (!watch_.admit(kind, now, shown, phase, port_vblank_wall_seconds(), next_frame)) {
                between_left_out_ = true;
                // (the frame's own picture instead only where no picture between reaches the screen: where the others
                // do, the frame's own shows its camera before the mouse's turn, two game frames behind the pictures
                // around it, a step back and forth 30 times a second while the mouse turns)
                left_out_frame_own_ = phase == 0 && p.picture.t == 0 && !(now - between_made_s_ < BETWEEN_LATELY_S);
                budget_left_out_++;
                budget_left_out_own_ += phase == 0 ? 1 : 0;
                direct_serial_.store(0, std::memory_order_release);
                return;
            }
            timing_ = {true, kind, now, shown};
        }
        between_made_s_ = port_wall_seconds();
        double before_s = turn_to_hand(p.picture, turn_lead_);
        last_turned_s_ = port_wall_seconds();
        last_between_ = p.picture;
        p.picture.run_bits = p.run_bits.empty() ? nullptr : p.run_bits.data();
        out_->begin_drawing(intake_.drawing(), &p.picture); // (drawn here unless it goes to the pump, which begins its own)
        const double walk_from = port_wall_seconds();
        std::optional<Intake::ForPump> for_pump = intake_.walk_picture(p.picture);
        note(DOING_PICTURE, walk_from, port_wall_seconds());
        shows_pumped_ = for_pump.has_value();
        if (for_pump) {
            aa_history_.drawing_not_made();
        }
        out_->resume_frame_drawing(intake_.drawing());
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


    // (developers', slice 5's question: what kf4-pc was doing when a picture's time came and it was not there) the last
    // stretches of kf4-pc's time by what it did, and for each picture begun late, how much of each lay between its
    // begin time and its begin
    enum Doing : uint8_t { DOING_PACKETS, DOING_FRAME, DOING_VBLANK, DOING_GPU_WAIT, DOING_PICTURE, DOING_IDLE,
                           DOING_OTHER, DOING_KINDS };
    struct Stretch {
        Doing doing;
        double from_s, to_s;
    };
    std::array<Stretch, 16384> stretches_{};
    size_t stretch_next_ = 0;
    std::array<double, DOING_KINDS> late_doing_s_{};
    double sent_s_ = 0, first_timed_s_ = 0, late_sent_s_ = 0, late_counted_s_ = 0;
    uint64_t late_counted_ = 0;
    void note(Doing doing, double from_s, double to_s) {
        if (budget_on_) {
            stretches_[stretch_next_++ % stretches_.size()] = {doing, from_s, to_s};
        }
    }
    void note_late(double at_s, double now_s) {
        if (first_timed_s_ == 0 || now_s < first_timed_s_ + 2.0) {
            return; // (the first two seconds: the load)
        }
        late_counted_++;
        late_counted_s_ += now_s - at_s;
        if (sent_s_ > at_s) {
            late_sent_s_ += std::min(sent_s_, now_s) - at_s; // (not even sent by then)
            at_s = sent_s_;
        }
        for (const Stretch &st : stretches_) {
            double a = std::max(st.from_s, at_s), b = std::min(st.to_s, now_s);
            if (b > a) {
                late_doing_s_[st.doing] += b - a;
            }
        }
    }

    // (kf4-pc, slice 5) Until at_s (50 ms at most): the budget's wait for a picture's time. ahead (a picture between's):
    // the game's packets queued behind the picture taken in meanwhile (take_packets_ahead).
    void wait_until(double at_s, bool ahead = false) {
        const double now = port_wall_seconds();
        if (first_timed_s_ == 0) {
            first_timed_s_ = now;
        }
        if (at_s - now > 0.0002) {
            const double rest = std::min(at_s - now, 0.05), until = now + rest;
            budget_waits_++;
            budget_waited_s_ += rest;
            if (ahead && ahead_on_) {
                take_packets_ahead(until);
            }
            ZoneScope zone(pc_waits_for_a_picture_s_time_zone);
            // (slept to a little before, then the last of it in turns given up: a sleep may wake a millisecond late,
            // more on Windows)
            const double left = until - port_wall_seconds();
            if (left > SLEEP_EARLY_S) {
                std::this_thread::sleep_for(std::chrono::duration<double>(left - SLEEP_EARLY_S));
            }
            while (port_wall_seconds() < until) {
                std::this_thread::yield();
            }
        } else if (now - at_s > 0.001) {
            budget_begun_late_++;
            budget_late_s_ += now - at_s;
            note_late(at_s, now);
        }
    }

    // (kf4-pc, slice 5) While a picture between waits for its time, until until_s: the game's packets queued behind it
    // (and behind the pictures after it, to the next vblank's own) taken in, each when its cost (the packets' high
    // percentile) fits before until_s, and more waited for meanwhile. kf4-pc otherwise sat idle in the wait while the
    // packets queued, and took them when the next picture's time had come (at 240, 268 pictures begun late in 16 s, 397
    // ms of it taking packets). The scene they add to is not drawn until its vblank closes it; the picture between
    // closes it so far (picture_between) and draws the frame kept, as it does with the next frame's packets taken before
    // it in their turn (those sent before its vblank's pictures). KF4_SCN_AHEAD=0: not taken ahead.
    void take_packets_ahead(double until_s) {
        for (;;) {
            const double now = port_wall_seconds();
            const double cost = std::max(packet_s_.high_s() * pcr::PictureBudget::MARGIN, AHEAD_LEAST_S);
            if (until_s - now <= cost) {
                return;
            }
            uint64_t seen = 0;
            pcr::Drawing *drawing = intake_.drawing();
            if (ahead_stopped_ || !drawing) {
                return;
            }
            if (!drawing->packets_ahead()) {
                ahead_stopped_ = true; // (until the vblank)
                return;
            }
            uint32_t passed = 0;
            if (std::optional<GifPacket> packet = inbox_.take_packet_ahead(seen, passed)) {
                ahead_left_ = std::max(ahead_left_, passed);
                take_packet(*packet, now);
                inbox_.packet_ahead_done(packet->bytes.size());
                const double done = port_wall_seconds();
                ahead_packets_++;
                ahead_bytes_ += packet->bytes.size();
                ahead_s_ += done - now;
                if (done > until_s) {
                    ahead_over_++;
                    ahead_over_s_ += done - until_s;
                }
                if (!drawing->packets_ahead_open()) {
                    // (the packet drew the scene so far: the pictures to come see what was taken ahead)
                    ahead_cut_++;
                    ahead_stopped_ = true;
                    return;
                }
                continue;
            }
            ZoneScope zone(pc_waits_for_a_picture_s_time_zone);
            const double wake_s = until_s - cost;
            inbox_.wait_put_until(seen, std::chrono::steady_clock::now() +
                                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                std::chrono::duration<double>(wake_s - now)));
            if (port_wall_seconds() >= wake_s) {
                return;
            }
        }
    }

    // KF4_SCN_AHEAD=0: no packets taken ahead of a picture between (slice 5 as first built)
    static bool ahead_wanted() {
        const char *e = getenv("KF4_SCN_AHEAD");
        return !(e && strcmp(e, "0") == 0);
    }

    // KF4_SCN_BUDGET=0: the scene renderer's pictures as before slice 5 (each made as it is taken, gs.c's cut). Only for
    // a window's pictures: never in a run without one or that dumps frames (whose pictures must not depend on time).
    static bool budget_wanted() {
        const char *e = getenv("KF4_SCN_BUDGET");
        return drawing_maker() != nullptr && !(e && strcmp(e, "0") == 0) && !port_options.headless &&
               !port_options.fast && !port_options.dump_frames;
    }
    // KF4_SCN_REPEAT=0: every vblank's picture made, as before slice 5
    static bool repeat_wanted() {
        const char *e = getenv("KF4_SCN_REPEAT");
        return drawing_maker() != nullptr && !(e && strcmp(e, "0") == 0);
    }

    static constexpr double VBLANK_S = 1001.0 / 60000.0;
    static constexpr double AHEAD_LEAST_S = 0.0005; // (a packet's cost taken ahead: at least)
    pcr::RecentTimes packet_s_{AHEAD_LEAST_S};
    uint64_t ahead_packets_ = 0, ahead_bytes_ = 0, ahead_over_ = 0, ahead_cut_ = 0;
    bool ahead_stopped_ = false;           // (no more taken ahead until the vblank)
    uint32_t ahead_left_ = 0;              // (the pictures not yet taken that the packets taken ahead were sent after)
    std::vector<GifPacket> forced_held_;   // (KF4_SCN_AHEAD_FORCE's: take_forced, and the pictures they passed left)
    uint32_t forced_held_left_ = 0;
    double ahead_s_ = 0, ahead_over_s_ = 0;
#ifdef _WIN32
    static constexpr double SLEEP_EARLY_S = 0.0015;
#else
    static constexpr double SLEEP_EARLY_S = 0.0003;
#endif

public:
    // (any thread) The pictures by budget (slice 5): gs.c then asks for every picture between, cut by nothing
    bool budgets_pictures() const {
        return budget_on_;
    }

    // --stats (the threads idle)
    void print_budget(FILE *out) {
        if (budget_on_) {
            fprintf(out,
                    "[stats] scene renderer pictures by budget: %llu waits for a picture's time (%.1f ms on average), "
                    "%llu begun more than 1 ms after it; %llu pictures between left out (not ready by their time), %llu of them "
                    "a vblank's own (%llu the game frame's first, its own picture shown instead; the others the last picture "
                    "shown again)\n",
                    (unsigned long long)budget_waits_, budget_waits_ ? budget_waited_s_ * 1e3 / double(budget_waits_) : 0.0,
                    (unsigned long long)budget_begun_late_, (unsigned long long)budget_left_out_,
                    (unsigned long long)budget_left_out_own_, (unsigned long long)budget_frame_instead_);
            watch_.print(out);
            if (ahead_on_) {
                fprintf(out,
                        "[stats] scene renderer packets taken ahead of a picture between as it waited: %llu (%.1f MB, %.1f "
                        "ms), %llu of them done after the wait's end (%.1f ms after in all), %llu that drew the scene so "
                        "far (then none more until the vblank); a packet's cost planned on %.2f ms\n",
                        (unsigned long long)ahead_packets_, double(ahead_bytes_) / 1048576.0, ahead_s_ * 1e3,
                        (unsigned long long)ahead_over_, ahead_over_s_ * 1e3, (unsigned long long)ahead_cut_,
                        std::max(packet_s_.high_s() * pcr::PictureBudget::MARGIN, AHEAD_LEAST_S) * 1e3);
            }
            const double *d = late_doing_s_.data();
            fprintf(out,
                    "[stats] scene renderer pictures begun late (after the first 2 s): %llu, %.1f ms late in all; not sent "
                    "yet %.1f; kf4-pc meanwhile (ms): packets %.1f, a frame taken %.1f, a vblank's scene closed %.1f, "
                    "waits for the GPU %.1f, other pictures %.1f, idle %.1f, other %.1f\n",
                    (unsigned long long)late_counted_, late_counted_s_ * 1e3, late_sent_s_ * 1e3,
                    d[DOING_PACKETS] * 1e3, d[DOING_FRAME] * 1e3, d[DOING_VBLANK] * 1e3,
                    d[DOING_GPU_WAIT] * 1e3, d[DOING_PICTURE] * 1e3, d[DOING_IDLE] * 1e3, d[DOING_OTHER] * 1e3);
        }
        if (ahead_force_) {
            fprintf(out, "[stats] scene renderer packets ahead forced (KF4_SCN_AHEAD_FORCE): %llu pictures sent after the "
                    "game's next packets, %llu packets sent past them, %llu taken ahead (%llu drew the scene so far)\n",
                    (unsigned long long)force_later_, (unsigned long long)force_passed_,
                    (unsigned long long)ahead_packets_, (unsigned long long)ahead_cut_);
        }
        if (repeat_on_) {
            fprintf(out, "[stats] scene renderer vblanks shown again from the last picture (nothing new): %llu\n",
                    (unsigned long long)stats.repeats.load());
            if (repeat_check_) {
                fprintf(out, "[stats] scene renderer repeats checked (KF4_SCN_REPEAT_CHECK): %llu made all the same, %llu "
                        "of them different from the picture shown again\n",
                        (unsigned long long)stats.repeat_checks.load(), (unsigned long long)stats.repeat_differ.load());
            }
        }
    }

    Settings settings;
    Stats stats;
    IntakeTimes intake_times; // (kf4-pc's; read by --stats once it is idle)

private:
    bool present_wanted_ = false;
    pcr::HangNotes hang_notes_; // (before everything that leaves notes, and the watch)
    std::unique_ptr<pcr::HangWatch> hang_watch_; // (nullptr: KF4_PC_HANG_WATCH=0)
    std::unique_ptr<Context> context_; // (before the device: destroyed after it)
    std::unique_ptr<Device> device_;
    int gpu_kind_ = GSN_GPU_OTHER;
    ClearColour clear_ = clear_colour();
    Inbox inbox_;
    pcr::AntiAliasHistory aa_history_; // (before the pictures' makers, which hold it)
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
    bool gpu_wait_unlocked_ = true; // (KF4_PC_GPU_WAIT_LOCKED=1: false; set at open)
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
    double last_turned_s_ = 0;        // (kf4-pc's) when its mouse turn was taken (turn_to_hand)
    SharedTurnLead turn_lead_;   // (before the pump, which holds it)
    // (slice 5, kf4-pc's) the pictures by budget: on (budget_wanted, set at open), the watch, the picture being timed,
    // whether the picture between taken last was left out (with the picture that would show it), and the counts
    bool budget_on_ = false;
    bool ahead_on_ = false; // (the packets taken ahead of a picture between: take_packets_ahead)
    // (KF4_SCN_AHEAD_FORCE's, the senders') the pictures held back, those held, the game's packets sent past them
    bool ahead_force_ = false;
    std::mutex later_lock_;
    std::vector<Later> later_;
    uint64_t force_later_ = 0, force_passed_ = 0;
    const bool repeat_on_ = repeat_wanted();
    const bool repeat_check_ = getenv("KF4_SCN_REPEAT_CHECK") && strcmp(getenv("KF4_SCN_REPEAT_CHECK"), "0") != 0;
    PictureWatch watch_;
    struct Timing {
        bool on = false;
        pcr::PictureKind kind = pcr::PictureKind::frame;
        double begun_s = 0, shown_s = 0;
    };
    Timing timing_;
    uint64_t timed_serial_ = 0;
    bool between_left_out_ = false;
    bool left_out_frame_own_ = false; // (the one left out was the game frame's first vblank's own, at t 0)
    double between_made_s_ = -1e9;    // (when the last picture between was made: not left out)
    static constexpr double BETWEEN_LATELY_S = 2.5 * VBLANK_S; // (made this lately: since the game frame before, at 30)
    bool frames_shown_lately_ = true; // (the game frames' own pictures are shown: not with the mouse turning them)
    uint64_t budget_waits_ = 0, budget_begun_late_ = 0, budget_left_out_ = 0, budget_left_out_own_ = 0,
             budget_frame_instead_ = 0;
    double budget_waited_s_ = 0, budget_late_s_ = 0;
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
PcrMakeSurface make_surface_asked; // likewise (NULL: the GPU is chosen without the window)
std::string hang_folder_asked; // pcr_hang_reports_in, likewise

// pcr_pipeline_cache's path, before pcr_open
std::string &pipeline_cache_path_before_open() {
    static std::string path;
    return path;
}
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
    if (!made->open(present_asked, make_surface_asked, hang_folder_asked)) {
        return 0;
    }
    renderer() = std::move(made);
    atexit([] {
        renderer()->stop();
        pcr::store_close();
    });
    return 1;
}

void pcr::use_drawing(DrawingMaker maker, const char *name) {
    drawing_maker() = maker;
    drawing_name() = maker && name ? name : "the PC renderer";
}

extern "C" void pcr_enable_present(PcrMakeSurface make_surface) {
    present_asked = true;
    make_surface_asked = make_surface;
}

extern "C" void pcr_hang_reports_in(const char *folder) {
    hang_folder_asked = folder ? folder : "";
}

extern "C" int pcr_gpu_kind(void) {
    return renderer() ? renderer()->gpu_kind() : GSN_GPU_OTHER;
}

extern "C" void pcr_gif(int path, const void *data, size_t bytes) {
    renderer()->gif(path, data, bytes);
}

extern "C" const uint32_t *pcr_present_sharpen_spirv(size_t *vert_bytes, const uint32_t **frag, size_t *frag_bytes) {
    *vert_bytes = sizeof(pcr_spv_pcr_present_vert);
    *frag = pcr_spv_pcr_present_sharpen_frag;
    *frag_bytes = sizeof(pcr_spv_pcr_present_sharpen_frag);
    return pcr_spv_pcr_present_vert;
}

extern "C" void pcr_present_sharpens(int on) {
    const char *fold = getenv("KF4_PRESENT_FOLD");
    presenter_sharpens_.store(on != 0 && !(fold && strcmp(fold, "0") == 0));
}

extern "C" void pcr_gif_vblank(int path, const void *data, size_t bytes) {
    renderer()->gif(path, data, bytes, true);
}

extern "C" int pcr_pictures_later(void) {
    return renderer() && renderer()->pictures_later() ? 1 : 0;
}

extern "C" int pcr_picture_later(const uint64_t *privileged, int field, int want_pixels, uint64_t tag, const char *name) {
    return renderer()->picture(privileged, field, want_pixels, nullptr, tag, name);
}

extern "C" void pcr_wait(void) {
    renderer()->wait();
}

extern "C" void pcr_settle(void) {
    if (renderer()) {
        renderer()->settle();
    }
}

extern "C" void pcr_read_fifo(void *dst, uint32_t qwords) {
    // (the transfer's packet was sent before: once the thread has done it, the canvas has the pixels ready)
    pcr::Drawing *canvas = renderer() ? renderer()->drawing_after_wait() : nullptr;
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

extern "C" int pcr_budgets_pictures(void) {
    return renderer() && renderer()->budgets_pictures() ? 1 : 0;
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

extern "C" void pcr_set_2d(int mode) {
    settings().mode_2d.store(mode);
}

extern "C" void pcr_set_font(int font) {
    settings().font.store(font);
}

extern "C" void pcr_set_glyphs(const GsnGlyphs *glyphs) {
    std::lock_guard<std::mutex> hold(settings().glyphs_lock);
    settings().glyphs = *glyphs;
}

extern "C" float pcr_key_width_u(const char *key_name) {
    return pcr::key_width_u(key_name ? key_name : "");
}

extern "C" int pcr_hint_parts(const GsnHintSprite *sprite, const float widths_u[4], GsnHintPart parts[GSN_HINT_PARTS]) {
    static_assert(GSN_HINT_PARTS == pcr::hint_part_count);
    const pcr::HintSprite asked = {sprite->x0, sprite->x1, sprite->y0, sprite->y1, sprite->u0, sprite->u1, sprite->v0, sprite->v1};
    std::array<pcr::HintPart, pcr::hint_part_count> out{};
    size_t count = pcr::hint_parts(asked, std::span<const float, pcr::face_button_count>(widths_u, 4), out);
    for (size_t i = 0; i < count; i++) {
        parts[i] = {out[i].x0, out[i].x1, out[i].u0, out[i].u1, out[i].v0, out[i].v1};
    }
    return int(count);
}

extern "C" void pcr_set_antialias(int method) {
    settings().antialias.store(port_antialias(method) ? method : PORT_AA_OFF);
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

// (before pcr_open) Where the pipelines are kept between runs (pcr_pipelines.h); after it, too late: nothing
extern "C" void pcr_pipeline_cache(const char *path) {
    if (!renderer() && path) {
        pipeline_cache_path_before_open() = path;
    }
}

// (after the settings, before the game draws) The pipelines made before play (pcr_pipelines.h, precompile)
extern "C" void pcr_precompile(int scaling, int sharpen) {
    renderer()->precompile(scaling != 0, sharpen != 0);
}

// The game waits while they are made (port/platform/kernel.c)
extern "C" int pcr_pipelines_running(void) {
    return pcr::precompile_running() ? 1 : 0;
}

// "Building shaders... done/total" (the overlay)
extern "C" int pcr_pipelines_building(int *done, int *total) {
    return pcr::precompile_progress(done, total) ? 1 : 0;
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

// A GS capture's state (F9, port/debug/gs_capture.c): the GS's 4 MB as the save states' and the registers
extern "C" void pcr_capture(void (*emit)(uint32_t type, const void *data, size_t size)) {
    if (!renderer()) {
        return;
    }
    Renderer &r = *renderer();
    std::vector<uint8_t> vram;
    pcr::GsRegisterFile file;
    r.run_job([&] {
        pcr::Drawing *canvas = r.intake().drawing();
        vram = canvas ? canvas->vram_image() : std::vector<uint8_t>(STATE_VRAM_BYTES);
        file = r.intake().front().registers();
    });
    vram.resize(STATE_VRAM_BYTES);
    emit(KF4GS_VRAM, vram.data(), vram.size());
    std::vector<uint8_t> regs = pcr::capture_registers(file);
    emit(KF4GS_GS_REGS, regs.data(), regs.size());
}

// A capture's state record (kf4-gsreplay --renderer pc), or a state's VRAM from paraLLEl-GS (gs.c's take_other_vram):
// the GS's memory, or its registers
extern "C" int pcr_restore(uint32_t type, const void *data, size_t size) {
    if (!renderer()) {
        return 0;
    }
    Renderer &r = *renderer();
    if (type == KF4GS_GS_REGS) {
        bool fits = true;
        bool done = r.run_job([&] {
            pcr::GsRegisterFile file = r.intake().front().registers();
            fits = pcr::take_capture_registers({static_cast<const uint8_t *>(data), size}, file);
            if (fits) {
                r.intake().front().restore(file);
            }
        });
        return done && fits ? 1 : 0;
    }
    if (type != KF4GS_VRAM || size != STATE_VRAM_BYTES) {
        return 0;
    }
    std::vector<uint8_t> vram(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    return r.run_job([&] {
        pcr::TextureStore::ClutState clut;
        clut.unloaded.fill(1);
        clut.cbp = {UINT32_MAX, UINT32_MAX};
        r.intake().forget();
        if (pcr::Drawing *canvas = r.intake().drawing()) {
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
            pcr::Drawing *canvas = r.intake().drawing();
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
            if (pcr::Drawing *canvas = r.intake().drawing()) {
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
    renderer()->print_budget(stderr);
    renderer()->print_census();
    // (here, not by a job: at exit the thread may be stopped already; idle or gone, it records nothing meanwhile)
    if (pcr::GpuTimes *times = renderer()->intake().gpu_times()) {
        if (pcr::GpuTimes *pump = renderer()->pump_gpu_times()) {
            times->take_in(*pump);
        }
        times->print();
    }
}

// ---- Developers' views (pcr.h) --------------------------------------------------------------------------
namespace {
// The canvas's call, on the PC renderer's thread after everything sent before; false without a canvas
bool on_canvas(const std::function<void(pcr::Canvas &)> &call) {
    if (!renderer()) {
        return false;
    }
    Renderer &r = *renderer();
    bool done = false;
    r.run_job([&] {
        if (pcr::Canvas *canvas = r.intake().canvas()) {
            call(*canvas);
            done = true;
        }
    });
    return done;
}
} // namespace

extern "C" int pcr_debug_trace(const PcrTraceSpot *spot) {
    bool started = false;
    on_canvas([&](pcr::Canvas &canvas) { started = canvas.trace_pixel(*spot); });
    return started ? 1 : 0;
}

extern "C" void pcr_debug_trace_draw(uint32_t draw) {
    on_canvas([&](pcr::Canvas &canvas) { canvas.trace_draw(draw); });
}

extern "C" void pcr_debug_gpu_draw(uint32_t draw) {
    on_canvas([&](pcr::Canvas &canvas) { canvas.gpu_draw(draw); });
}

extern "C" void pcr_debug_trace_probe(void) {
    on_canvas([](pcr::Canvas &canvas) { canvas.trace_probe(); });
}

extern "C" size_t pcr_debug_trace_take(PcrTraceFragment *fragments, size_t room, PcrTraceProbe *probes,
                                       size_t probe_room, size_t *probes_taken) {
    std::vector<PcrTraceFragment> f;
    std::vector<PcrTraceProbe> p;
    size_t counted = 0;
    on_canvas([&](pcr::Canvas &canvas) { counted = canvas.take_trace(f, p); });
    size_t nf = std::min(room, f.size()), np = std::min(probe_room, p.size());
    std::copy_n(f.begin(), nf, fragments);
    std::copy_n(p.begin(), np, probes);
    *probes_taken = np;
    return counted;
}

extern "C" void pcr_debug_decal_losses(int on) {
    on_canvas([&](pcr::Canvas &canvas) { canvas.show_decal_losses(on != 0); });
}

extern "C" int pcr_debug_settled(void) {
    bool settled = false;
    if (renderer()) {
        Renderer &r = *renderer();
        r.run_job([&] {
            if (pcr::Drawing *drawing = r.intake().drawing()) {
                settled = drawing->settled();
            }
        });
    }
    return settled ? 1 : 0;
}
