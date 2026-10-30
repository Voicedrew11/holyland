// The PC renderer (pcr.h; docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native GS"), step 1:
// the skeleton. Its own Vulkan device (through Granite, the layer paraLLEl-GS and the native GS run on) and its
// own thread, "kf4-pc", which takes what gs.c sends in the order sent: GIF packets, vblanks and the pictures
// between. At a vblank the display registers name the buffer shown; its picture is, for now, an image cleared to
// black (KF4_PC_CLEAR=RRGGBB: that colour), handed out as the native GS hands out its pictures:
// - with GSN_WANT_IMAGE, copied into a slot of the scanout ring, which the presenter (port/platform/present_vk.cpp)
//   takes on the game's thread (pcr_scanout_take_tag) and gives back once its command buffer is submitted
//   (pcr_scanout_release). A slot is published (given a serial and the picture's tag) only after the commands that
//   write it are on the queue, so the presenter, submitting to the same queue after it, sees the whole picture;
// - with GSN_WANT_PIXELS (and for the presenter's pixels), read back into a PortGsFrame;
// - with GSN_PRESENT, handed to the presenter's callbacks from this thread once it is there.
// Nothing of the native GS's code is here or linked from here: gsn.h is included for its plain types only.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
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
#include "port/gs_capture_format.h"
#include "device.hpp"
#include "thread_id.hpp"

extern "C" {
#include "port/platform.h"
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
};

// A picture between's frame (frame_interp.c's: pcr_between.h), and a picture asked for, its runs' bits kept
using pcr::InterpFrame;
struct InterpPicture {
    GsnInterpPicture picture;
    std::vector<uint32_t> run_bits;
};

// Work that must be done on the PC renderer's thread (its device's), its sender waiting: save states
struct Job {
    std::function<void()> work;
    Reply *reply;
};

using Message = std::variant<GifPacket, VblankPicture, InterpFrame, InterpPicture, Job>;

// The queue from the senders (the DMA controller's thread, mostly) to the PC renderer's thread, and the waits on
// it: for the thread to be idle, for a picture's reply, for room (the packets' bytes held), for a window's
// pictures to be at most two vblanks behind (as the native GS holds its sender)
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

    // (the PC renderer's thread) The message taken is done; with reply, its answer
    void done(size_t bytes, bool vblank, Reply *reply, int result) {
        std::lock_guard<std::mutex> hold(lock_);
        held_bytes_ -= bytes;
        vblanks_done_ += vblank ? 1 : 0;
        busy_ = false;
        if (reply) {
            reply->result = result;
            reply->done = true;
        }
        room_.notify_all();
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

    // The sender of a window's picture: at most two vblanks ahead of the thread
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
// The pictures handed to the presenter: a ring of images, as the native GS keeps them (with --frame-rate above 60
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
        out.scaled = 0; // (no scaled picture yet: the presenter blits)
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

// The buffer the display shows (circuit 2, else 1; a field's lines in field mode): as gsn.h's gsn_display_rect
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

struct PictureSize {
    uint32_t width, height;
};

// The picture's size at a vblank, as the native GS's scanout makes it from the buffer shown at scale sx x sy: a
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

// ---- The settings (gsn.h's): kept for the steps that draw ---------------------------------------------
struct Settings {
    std::atomic<uint32_t> scale{1 | 1 << 8}; // across | down << 8, from the next vblank
    std::atomic<int> lod{GSN_LOD_SCALE}, aniso{1}, msaa{1}, cutout_aa{GSN_CUTOUT_AA_OFF}, mode_2d{GSN_2D_PS2},
        colour{GSN_COLOUR_EXACT}, layer_z{GSN_LAYER_Z_PS2}, shine_exact{0}, native_draw{GSN_NATIVE_DRAW_NATIVE},
        standalone{0}, hardware_sampler{0}, pc_world{0}, upscaler{0}, scene_map{-1};
    std::atomic<uint32_t> present_size{0}, present_sharpen{0};
};

// ---- --stats ---------------------------------------------------------------------------------------
struct Stats {
    std::atomic<uint64_t> packets{0}, bytes{0}, vblanks{0}, pictures{0}, extras{0}, interp_frames{0},
        interp_pictures{0};
};

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
    // starts (as the native GS's interp_capture), so that every walk of the frame makes the same calls.
    void take_frame(InterpFrame frame) {
        if (between_) {
            between_->take_frame(std::move(frame));
            walk_start_ = front_.state();
            kept_walk_.reset();
        }
    }

    // A picture between: drawn again from the frame's kept walk when what that read of the store is as it was (step
    // 5b); else the frame's packets walked again from the frame's state, its kicks numbered, the canvas's and the
    // native draws' corners moved by the shaders to the picture's time, and the walk kept. The live state is put
    // back after.
    void walk_picture(const GsnInterpPicture &picture) {
        if (!between_ || !canvas_) {
            return;
        }
        pcr::WalkReason why = pcr::WalkReason::none_kept;
        if (kept_walk_ && keep_walks_) {
            if (canvas_->fits(*kept_walk_)) {
                if (between_->begin_picture_again(picture)) {
                    canvas_->draw_again(*kept_walk_, *between_);
                    between_->end_picture_again();
                }
                return;
            }
            why = pcr::WalkReason::store_changed;
        }
        kept_walk_.reset();
        std::span<const uint8_t> stream = between_->begin_picture(picture);
        if (stream.empty()) {
            return;
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
        const char *census = getenv("KF4_PC_CENSUS");
        if (census && strcmp(census, "1") == 0) {
            census_ = std::make_unique<pcr::Census>();
        }
    }

    void feed(int path, const std::vector<uint8_t> &bytes) {
        front_.feed(path, bytes);
    }

    void vblank(const uint64_t *privileged, uint32_t scale) {
        if (canvas_) {
            canvas_->set_scale(scale & 0xFF, scale >> 8);
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
        clear_ = clear_colour();
        thread_ = std::thread([this] { run(); });
        fprintf(stderr, "[pcr] the PC renderer on %s\n", props.deviceName);
        return true;
    }

    // At exit: what was sent is done (two seconds at most), then the thread stops; if it does not within two
    // seconds more it is left (the process ends with it)
    void stop() {
        inbox_.wait_idle_for(std::chrono::seconds(2));
        inbox_.stop();
        if (inbox_.wait_gone_for(std::chrono::seconds(2))) {
            thread_.join();
        } else {
            thread_.detach();
        }
    }

    void gif(int path, const void *data, size_t bytes) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        inbox_.put(GifPacket{path, std::vector<uint8_t>(p, p + bytes)}, bytes, false);
    }

    void wait() {
        inbox_.wait_idle();
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
        // As the native GS: a picture nobody waits for (none, or one this thread hands to the presenter) is
        // queued and the sender goes on; one wanted in frame, or an image the sender shows itself, is waited for
        bool queued = !(want & GSN_WANT_PIXELS) && (!(want & GSN_WANT_IMAGE) || (want & GSN_PRESENT));
        if (queued) {
            if (!inbox_.put(std::move(v), 0, !extra)) {
                return 0;
            }
            if ((want & GSN_PRESENT) && !extra) {
                inbox_.wait_behind_at_most(2); // (a window's picture at most two vblanks behind the game)
            }
            return want & (GSN_PRESENT | GSN_WANT_IMAGE);
        }
        Reply reply;
        v.frame = frame;
        v.reply = &reply;
        if (!inbox_.put(std::move(v), 0, !extra)) {
            return 0;
        }
        return inbox_.wait_reply(reply);
    }

    void interp_frame(GsnInterpFrame *frame) {
        inbox_.put(InterpFrame(frame), 0, false);
    }

    void interp_picture(const GsnInterpPicture &picture) {
        InterpPicture p = {picture, {}};
        if (picture.runs && picture.run_bits) {
            p.run_bits.assign(picture.run_bits, picture.run_bits + (size_t(picture.runs) + 31) / 32);
        }
        p.picture.run_bits = nullptr; // (set to the kept bits where it is taken)
        inbox_.put(std::move(p), 0, false);
    }

    int backlog() {
        return inbox_.backlog();
    }

    void set_presenter(void (*pixels)(const PortGsFrame *, uint64_t), void (*image)(uint64_t)) {
        presenter_pixels_.store(pixels);
        presenter_image_.store(image);
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

    // The canvas, once everything sent before is done (the thread idle until more is sent: the read-back FIFO's
    // caller, the DMA controller's thread, sends nothing meanwhile)
    pcr::Canvas *canvas_after_wait() {
        inbox_.wait_idle();
        return intake_.canvas();
    }

    // (after wait: the thread is idle)
    void print_census() const {
        intake_.print_census();
    }

private:
    struct ClearColour {
        uint32_t rgba; // R in the low byte, alpha 255
        VkClearValue value;
    };

    // KF4_PC_CLEAR=RRGGBB: the pictures' colour (black without it)
    static ClearColour clear_colour() {
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

    // The Vulkan instance and device: with the presenter's surface and swapchain extensions when it wants
    // them, else (or when that fails) without
    bool make_context() {
        std::vector<const char *> instance_ext, device_ext;
        if (present_wanted_) {
            present_extensions(instance_ext, device_ext);
        }
        if (!instance_ext.empty()) {
            context_ = std::make_unique<Context>();
            context_->set_num_thread_indices(1); // (the PC renderer's thread)
            if (context_->init_instance_and_device(instance_ext.data(), uint32_t(instance_ext.size()),
                                                   device_ext.data(), uint32_t(device_ext.size()), 0)) {
                return true;
            }
            fprintf(stderr, "[pcr] no Vulkan device with a swapchain: the window is shown without one\n");
        }
        present_wanted_ = false;
        context_ = std::make_unique<Context>();
        context_->set_num_thread_indices(1);
        if (!context_->init_instance_and_device(nullptr, 0, nullptr, 0, 0)) {
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
                intake_.feed(packet->path, packet->bytes);
                inbox_.done(packet->bytes.size(), false, nullptr, 0);
            } else if (VblankPicture *v = std::get_if<VblankPicture>(&*message)) {
                bool extra = (v->want & GSN_EXTRA) != 0;
                if (!extra) {
                    intake_.vblank(v->privileged.data(), settings.scale.load());
                }
                int result = make_picture(*v);
                inbox_.done(0, !extra, v->reply, result);
            } else if (Job *job = std::get_if<Job>(&*message)) {
                job->work();
                inbox_.done(0, false, job->reply, 1);
            } else if (InterpFrame *frame = std::get_if<InterpFrame>(&*message)) {
                stats.interp_frames++;
                intake_.take_frame(std::move(*frame));
                inbox_.done(0, false, nullptr, 0);
            } else if (InterpPicture *p = std::get_if<InterpPicture>(&*message)) {
                stats.interp_pictures++;
                p->picture.run_bits = p->run_bits.empty() ? nullptr : p->run_bits.data();
                intake_.walk_picture(p->picture);
                inbox_.done(0, false, nullptr, 0);
            }
        }
        inbox_.gone();
    }

    // (the PC renderer's thread) A vblank's picture, or with GSN_EXTRA one more between two (the last picture
    // again), handed out as gsn_picture's: GSN_WANT_IMAGE when the scanout ring has it, GSN_WANT_PIXELS when
    // frame has it, and with GSN_PRESENT to the presenter once it is there
    int make_picture(const VblankPicture &v) {
        bool extra = (v.want & GSN_EXTRA) != 0;
        int want = v.want;
        PortGsFrame *frame = v.frame;
        // SDL_Renderer's picture at the window's size (GSN_WANT_SCALED with GSN_PRESENT and GSN_WANT_IMAGE): no
        // scaled picture yet, so the picture's own pixels for the presenter
        bool scaled_pixels = (want & GSN_PRESENT) && (want & GSN_WANT_SCALED) && (want & GSN_WANT_IMAGE) &&
                             !(want & GSN_WANT_PIXELS);
        bool present_pixels = ((want & GSN_PRESENT) && !(want & (GSN_WANT_IMAGE | GSN_WANT_PIXELS))) || scaled_pixels;
        if (present_pixels) {
            frame = &presented_;
        }
        stats.pictures++;
        if (extra) {
            stats.extras++;
        } else {
            stats.vblanks++;
        }
        uint32_t scale = picture_scale_;
        std::optional<PictureSize> size = picture_size(v, extra, scale & 0xFF, scale >> 8);
        if (!extra) {
            picture_scale_ = settings.scale.load(); // (a new scale from the next vblank)
        }
        if (!size || !(want & (GSN_WANT_IMAGE | GSN_WANT_PIXELS | GSN_PRESENT))) {
            return 0; // (no picture shown, or none wanted)
        }
        CommandBufferHandle cmd = device_->request_command_buffer();
        pcr::GpuTimes &times = *intake_.gpu_times();
        times.begin(*cmd, pcr::GpuWork::picture);
        // (a picture between too: the frame's packets were sent again, moved to its time, before it)
        if (!draw_picture(*cmd, *size, v)) {
            times.end(*cmd);
            device_->submit(cmd);
            return 0;
        }
        times.picture();
        int ok = 0, slot = -1;
        if (want & GSN_WANT_IMAGE) {
            slot = scanout_.claim(*device_, picture_->get_width(), picture_->get_height());
            if (slot >= 0) {
                copy_to_scanout(*cmd, scanout_.image(slot));
                ok |= GSN_WANT_IMAGE;
            }
        }
        if (frame && ((want & GSN_WANT_PIXELS) || present_pixels)) {
            if (read_back(cmd, *frame)) {
                ok |= GSN_WANT_PIXELS;
            }
        } else {
            times.end(*cmd);
            device_->submit(cmd);
        }
        if (slot >= 0) {
            scanout_.publish(slot, v.tag);
        }
        device_->next_frame_context();
        if (ok && (want & GSN_PRESENT)) {
            hand_to_presenter(want, ok, scaled_pixels, frame, v.tag);
        }
        return ok;
    }

    // The picture: the canvas's surface the display shows, scaled to its size (cleared to the picture's colour where
    // there is none). False if the image cannot be made.
    bool draw_picture(CommandBuffer &cmd, PictureSize size, const VblankPicture &v) {
        if (!picture_ || picture_->get_width() != size.width || picture_->get_height() != size.height) {
            picture_ = ScanoutRing::make_picture_image(*device_, size.width, size.height);
            if (!picture_) {
                fprintf(stderr, "[pcr] no image for a %ux%u picture\n", size.width, size.height);
                return false;
            }
        }
        // (after whatever read or wrote it before: the copies of the last picture)
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                    VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        GsnRect rect;
        pcr::Canvas *canvas = intake_.canvas();
        bool shown = false;
        if (canvas && display_rect(v.privileged.data(), rect)) {
            pcr::ShownRect shown_rect = {rect.bp, rect.bw, rect.psm, rect.x, rect.y, rect.w, rect.h};
            shown = canvas->scanout(cmd, shown_rect, *picture_);
        }
        if (!shown) {
            cmd.clear_image(*picture_, clear_.value);
        }
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        return true;
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
    bool read_back(CommandBufferHandle &cmd, PortGsFrame &frame) {
        uint32_t width = picture_->get_width(), height = picture_->get_height();
        VkDeviceSize size = VkDeviceSize(width) * height * 4;
        if (!readback_ || readback_->get_create_info().size < size) {
            BufferCreateInfo info = {};
            info.domain = BufferDomain::CachedHost;
            info.size = size;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            readback_ = device_->create_buffer(info, nullptr);
            if (!readback_) {
                fprintf(stderr, "[pcr] no buffer to read a picture back into\n");
                intake_.gpu_times()->end(*cmd);
                device_->submit(cmd);
                return false;
            }
        }
        cmd->copy_image_to_buffer(*readback_, *picture_, 0, {0, 0, 0}, {width, height, 1}, 0, 0,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                     VK_ACCESS_2_HOST_READ_BIT);
        Fence fence;
        intake_.gpu_times()->end(*cmd);
        device_->submit(cmd, &fence);
        fence->wait();
        if (&frame == &presented_ && presented_pixels_.size() < size_t(width) * height) {
            presented_pixels_.resize(size_t(width) * height);
            presented_.rgba = presented_pixels_.data();
        }
        const uint8_t *mapped = static_cast<const uint8_t *>(device_->map_host_buffer(*readback_, MEMORY_ACCESS_READ_BIT));
        if (!mapped) {
            fprintf(stderr, "[pcr] the read-back buffer could not be mapped\n");
            return false;
        }
        for (size_t i = 0; i < size_t(width) * height; i++) {
            uint32_t pixel;
            memcpy(&pixel, mapped + i * 4, 4);
            frame.rgba[i] = pixel | 0xFF000000u;
        }
        device_->unmap_host_buffer(*readback_, MEMORY_ACCESS_READ_BIT);
        frame.width = width;
        frame.height = height;
        return true;
    }

    // The window's picture, now that it is there: its pixels, or word that the image is published
    void hand_to_presenter(int want, int ok, bool scaled_pixels, const PortGsFrame *frame, uint64_t tag) {
        void (*pixels)(const PortGsFrame *, uint64_t) = presenter_pixels_.load();
        void (*image)(uint64_t) = presenter_image_.load();
        if (scaled_pixels && (ok & GSN_WANT_PIXELS) && pixels) {
            pixels(frame, tag);
        } else if ((ok & GSN_WANT_IMAGE) && image && !scaled_pixels) {
            image(tag);
        } else if (!(want & GSN_WANT_IMAGE) && (ok & GSN_WANT_PIXELS) && pixels) {
            pixels(frame, tag);
        }
    }

public:
    Settings settings;
    Stats stats;

private:
    bool present_wanted_ = false;
    std::unique_ptr<Context> context_; // (before the device: destroyed after it)
    std::unique_ptr<Device> device_;
    int gpu_kind_ = GSN_GPU_OTHER;
    uint32_t msaa_samples_have_ = 1;
    ClearColour clear_ = {};
    Inbox inbox_;
    ScanoutRing scanout_;
    Intake intake_; // (the PC renderer's thread's)
    std::atomic<void (*)(const PortGsFrame *, uint64_t)> presenter_pixels_{nullptr};
    std::atomic<void (*)(uint64_t)> presenter_image_{nullptr};
    // (the PC renderer's thread's)
    uint32_t picture_scale_ = 1 | 1 << 8;
    ImageHandle picture_;
    BufferHandle readback_;
    std::vector<uint32_t> presented_pixels_;
    PortGsFrame presented_ = {0, 0, nullptr};
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
    made->settings.upscaler.store(settings_before_open.upscaler.load());
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
    // (kept: no scaled picture is made yet, so GsnScanout's scaled is 0 and the presenter blits)
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
    // a power of two, at most 16 (gsn.h)
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

extern "C" void pcr_set_shine_exact(int exact) {
    settings().shine_exact.store(exact ? 1 : 0);
}

extern "C" int pcr_native_kinds(void) {
    return pcr::NativeDraws::kinds();
}

extern "C" void pcr_set_native_draw(int mode) {
    settings().native_draw.store(mode == GSN_NATIVE_DRAW_PS2 ? GSN_NATIVE_DRAW_PS2 : GSN_NATIVE_DRAW_NATIVE);
}

extern "C" void pcr_set_standalone(int on) {
    settings().standalone.store(on != 0);
}

extern "C" void pcr_set_hardware_sampler(int on) {
    settings().hardware_sampler.store(on != 0);
}

extern "C" void pcr_set_pc_world(int on) {
    settings().pc_world.store(on != 0);
}

extern "C" void pcr_set_scene(int x_scale, int y_scale, int samples) {
    (void)x_scale; // (the scene has no scale of its own in the PC renderer: the picture is drawn at one)
    (void)y_scale;
    (void)samples;
}

extern "C" void pcr_set_upscaler(int mode) {
    settings().upscaler.store(mode);
}

extern "C" void pcr_set_scene_map(int map) {
    settings().scene_map.store(map);
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
// The PC renderer's state: the GS's 4 MB first (so that the native GS and paraLLEl-GS take it by its VRAM, as the PC
// renderer takes theirs: gs.c's take_other_vram), then the GS's registers, the GIF's Q and the CLUT buffer's mirror
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
    port_state_bytes(io, nullptr, size); // (a table saved by the native GS's modern: passed over)
}

extern "C" void pcr_print_stats(void) {
    if (!renderer()) {
        return;
    }
    renderer()->wait();
    const Stats &s = renderer()->stats;
    fprintf(stderr,
            "[stats] pc renderer: %llu packets (%.1f MB), %llu vblanks, %llu pictures (%llu between); pictures "
            "between: %llu frames taken, %llu pictures asked for\n",
            (unsigned long long)s.packets.load(), double(s.bytes.load()) / 1048576.0,
            (unsigned long long)s.vblanks.load(), (unsigned long long)s.pictures.load(),
            (unsigned long long)s.extras.load(), (unsigned long long)s.interp_frames.load(),
            (unsigned long long)s.interp_pictures.load());
    renderer()->print_census();
    // (here, not by a job: at exit the thread may be stopped already; idle or gone, it records nothing meanwhile)
    if (pcr::GpuTimes *times = renderer()->intake().gpu_times()) {
        times->print();
    }
}
