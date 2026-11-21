// The scene renderer's pictures by budget (docs/plans/scene-renderer-design.md, "8. Scheduling and present"; slice 5,
// docs/plans/scene-slice5-results.md). The shell (port/gs/pc/pcr.cpp) times each picture it makes for the window: from
// beginning it (the hand of the mouse taken, its commands recorded) to its commands submitted (cpu), and from then, or
// from the GPU's work before it done if that is later, to its own done (gpu: its fence, seen by a watching thread). From
// the last few of each kind it then
// - begins a picture no sooner than its predicted cost before it is shown (begin_at_s): the picture is the world and the
//   hand of then, not of a vblank before (the late turn of slice 4b: docs/plans/scene-slice4-results.md, "4b");
// - leaves out a picture between that would not be ready by its time, or would make the game frame's own picture after
//   it late (fits): never waited for, never queued behind the GPU's work. A game frame's own picture is always made.
//
// Header only, no locks (the caller's), no globals.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace pcr {

// What a picture made for the window is, as it is timed
enum class PictureKind : uint8_t {
    frame,       // a vblank's own picture of the game's frame: the frame's buffer shown
    between,     // a picture between and the extra picture that shows it
    between_own, // a picture between and the vblank's own picture that shows it (at its phase 0): the vblank's scene
                 // closed between the two, its GPU work in the picture's
    scene,       // (not a picture) the vblank's scene closed alone: its GPU work, ahead of the pictures after it
};
constexpr size_t PICTURE_KINDS = 4;

// The last few times of one kind, and a high percentile of them: a picture costs more at times (the view, a
// spell), and one begun on the mean would be late every other time
class RecentTimes {
public:
    static constexpr size_t KEPT = 32;
    static constexpr double PERCENTILE = 0.9;

    explicit RecentTimes(double start_s) : high_s_(start_s) {}

    void add(double seconds) {
        if (!(seconds >= 0)) {
            return;
        }
        kept_[next_] = seconds;
        next_ = (next_ + 1) % KEPT;
        count_ += count_ < KEPT ? 1 : 0;
        std::array<double, KEPT> sorted = kept_;
        auto end = sorted.begin() + std::ptrdiff_t(count_);
        auto at = sorted.begin() + std::ptrdiff_t(PERCENTILE * double(count_ - 1) + 0.5);
        std::nth_element(sorted.begin(), at, end);
        high_s_ = *at;
    }

    double high_s() const {
        return high_s_;
    }

    size_t count() const {
        return count_;
    }

private:
    std::array<double, KEPT> kept_{};
    size_t next_ = 0, count_ = 0;
    double high_s_;
};

class PictureBudget {
public:
    static constexpr double START_CPU_S = 0.002;  // before any is measured
    static constexpr double START_GPU_S = 0.010;
    static constexpr double START_SCENE_S = 0.002;
    static constexpr double MARGIN = 1.1;         // the high percentile, this many times over
    static constexpr double SLACK_S = 0.0008;     // the wait's waking late, and the hand-out after the submit

    // A picture of kind made: cpu_s from begun to submitted, gpu_s its own GPU time
    void made(PictureKind kind, double cpu_s, double gpu_s) {
        cpu_[size_t(kind)].add(cpu_s);
        gpu_[size_t(kind)].add(gpu_s);
    }

    // (a picture between shown by the vblank's own not measured yet: one shown by an extra picture and the scene alone,
    // so that it is not left out for good on its first guess)
    double cpu_s(PictureKind kind) const {
        if (kind == PictureKind::between_own && !measured(kind) && measured(PictureKind::between)) {
            return cpu_s(PictureKind::between);
        }
        return cpu_[size_t(kind)].high_s() * MARGIN;
    }
    double gpu_s(PictureKind kind) const {
        if (kind == PictureKind::between_own && !measured(kind) && measured(PictureKind::between)) {
            return gpu_s(PictureKind::between) + gpu_s(PictureKind::scene);
        }
        return gpu_[size_t(kind)].high_s() * MARGIN;
    }
    size_t measured(PictureKind kind) const {
        return gpu_[size_t(kind)].count();
    }

    // From begun to ready, the GPU idle
    double ready_s(PictureKind kind) const {
        return cpu_s(kind) + gpu_s(kind) + SLACK_S;
    }

    // The soonest a picture of kind shown at shown_s is begun
    double begin_at_s(PictureKind kind, double shown_s) const {
        return shown_s - ready_s(kind);
    }

    // When a picture of kind begun at now_s is done, the GPU busy with the pictures before it until gpu_free_s (0, or a
    // time before now_s: idle). (Without the slack, which the begin time has: a picture begun as late as the wait woke
    // still fits.)
    double done_s(PictureKind kind, double now_s, double gpu_free_s) const {
        return std::max(now_s + cpu_s(kind), gpu_free_s) + gpu_s(kind);
    }

    // The latest a picture between shown at shown_s (phase, of vblank_s, into its vblank) may be done: its time, and
    // where the next vblank's own picture is a game frame's (next_frame: the frame's first vblank, the mouse not
    // turning), early enough for that one to be ready by its time after it (its GPU work after this one's; its CPU's
    // may be done meanwhile)
    double deadline_s(double shown_s, double phase, double vblank_s, bool next_frame) const {
        if (!next_frame) {
            return shown_s;
        }
        double next_own = shown_s + (1.0 - phase) * vblank_s;
        return std::min(shown_s, next_own - gpu_s(PictureKind::scene) - gpu_s(PictureKind::frame) - SLACK_S);
    }

    // The soonest a picture between of kind (between or between_own) is begun: ready by its deadline
    double between_begin_at_s(PictureKind kind, double shown_s, double phase, double vblank_s, bool next_frame) const {
        return deadline_s(shown_s, phase, vblank_s, next_frame) - ready_s(kind);
    }

    // Whether a picture between of kind begun at now_s, the GPU busy until gpu_free_s, is done by its deadline
    bool fits(PictureKind kind, double now_s, double gpu_free_s, double shown_s, double phase, double vblank_s,
              bool next_frame) const {
        return done_s(kind, now_s, gpu_free_s) <= deadline_s(shown_s, phase, vblank_s, next_frame);
    }

private:
    std::array<RecentTimes, PICTURE_KINDS> cpu_{RecentTimes(START_CPU_S), RecentTimes(START_CPU_S),
                                                RecentTimes(START_CPU_S), RecentTimes(0)};
    std::array<RecentTimes, PICTURE_KINDS> gpu_{RecentTimes(START_GPU_S), RecentTimes(START_GPU_S),
                                                RecentTimes(START_GPU_S), RecentTimes(START_SCENE_S)};
};

// The GPU's work for the pictures submitted and not seen done, as predicted: when it will be done with all of them.
// kf4-pc adds each picture as it submits it; the watching thread says when each is done (in order).
class GpuQueueTime {
public:
    // A picture submitted at submit_s, gpu_s of the GPU's own work predicted
    void submitted(uint64_t serial, double submit_s, double gpu_s) {
        until_s_ = std::max(until_s_, submit_s) + gpu_s;
        last_submitted_ = serial;
    }

    // The picture serial seen done at done_s: with nothing after it in flight, the GPU idle from then
    void done(uint64_t serial, double done_s) {
        last_done_ = serial;
        if (serial == last_submitted_) {
            until_s_ = done_s;
        }
    }

    // When the GPU is done with the pictures submitted (0: all seen done)
    double free_s() const {
        return last_done_ == last_submitted_ ? 0 : until_s_;
    }

    uint64_t in_flight() const {
        return last_submitted_ - last_done_;
    }

private:
    double until_s_ = 0;
    uint64_t last_submitted_ = 0, last_done_ = 0;
};

} // namespace pcr
