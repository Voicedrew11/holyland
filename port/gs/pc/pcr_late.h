// The PC renderer's pictures between timed to when the window shows them (GsnInterpPicture's shown_s).
//
// --frame-rate unlocked asks for a picture between every 1 / the cap of a second (port/platform/frame_interp.c). The
// pump (port/gs/pc/pcr.cpp) draws them in order, but begins one no sooner than it needs to have it ready when it is
// shown (begin_at_s), and leaves out one it would have ready after that while a later one waits (leave_out): it always
// draws the next picture it can have ready in time, and every picture shown is the world at the time it is shown.
// Other games' unlocked rate: as many pictures as the renderer has time for, picture by picture.
//
// Instant mouse look's last step (port/include/port/mouse_lead.h) turns each picture to the mouse's hand as it is
// drawn. The hand is taken a steady time before the picture is shown (TurnLead), not whenever it was last read:
// a turn's lag that changed from picture to picture would show as the view shaking while the mouse turns evenly.
//
// Header only, no locks (the caller's), no globals.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace pcr {

// How long before its time the pump must begin a picture between: from what the last ones took (the pump's thread,
// the picture between drawn and the picture that shows it), and room for the GPU after that
class PictureCost {
public:
    static constexpr double START_S = 0.004;    // before any is measured
    static constexpr double GPU_LEAD_S = 0.002; // the GPU's work after the pump hands a picture out
    static constexpr double MARGIN = 1.25;      // the time taken, this many times over (a picture costs more at times)

    // A picture made, in seconds: up at once by a quarter of the difference, down by a sixteenth
    void made(double seconds) {
        if (seconds <= 0) {
            return;
        }
        mean_s_ += (seconds - mean_s_) / (seconds > mean_s_ ? 4.0 : 16.0);
    }

    double ready_s() const {
        return mean_s_ * MARGIN + GPU_LEAD_S;
    }

    double mean_s() const {
        return mean_s_;
    }

private:
    double mean_s_ = START_S;
};

// How many pictures between a second the PC renderer's thread (kf4-pc) has time for: from its time over the last
// WINDOW_S, split into what its pictures between took (each with the picture that shows it) and everything else (a
// game frame's packets, 5-6 ms in one go at 063, and the vblanks' own pictures), it has BUSY_SHARE of the window for
// pictures between, less the rest. --frame-rate unlocked asks for no more than that (port/platform/frame_interp.c).
// Asked for more (a quarter over what the window showed, as before), kf4-pc was never idle: the pictures queued behind a
// game frame's packets were begun late, and it never caught up before the next frame's (2026-11-20, 063: 100-470 ms with
// no picture, 1,930 made too late in 60 s; asked for less than it had time for, none).
class PictureRoom {
public:
    static constexpr double WINDOW_S = 0.5;
    // (the rest: a dearer picture now and then, the packets' bursts. 063, 60 s: at 0.85, 7% fewer pictures shown than
    // asking a quarter over and leaving out the late ones; at 0.92 as many, 5 made too late, the longest gap 9 ms)
    static constexpr double BUSY_SHARE = 0.92;

    // kf4-pc spent seconds on a picture between drawn here (with the picture that shows it), or on anything else
    void picture(double seconds) {
        picture_s_ += seconds;
        pictures_++;
    }
    void other(double seconds) {
        other_s_ += seconds;
    }

    // At now_s: a window WINDOW_S or more since the last one closed, and its room worked out (it has drawn a
    // picture between); true when it closed
    bool close_window(double now_s) {
        if (start_s_ < 0) {
            start_s_ = now_s;
            return false;
        }
        const double window_s = now_s - start_s_;
        if (window_s < WINDOW_S) {
            return false;
        }
        room_hz_ = 0;
        if (pictures_ > 0 && picture_s_ > 0) {
            const double each_s = picture_s_ / double(pictures_);
            // (at least one: no room is still known, and asks for unlocked's least)
            room_hz_ = std::max(1.0, std::max(0.0, BUSY_SHARE * window_s - other_s_) / each_s / window_s);
        }
        start_s_ = now_s;
        picture_s_ = other_s_ = 0;
        pictures_ = 0;
        return true;
    }

    // Pictures between a second there is time for, as of the last window closed (0: not known: none drawn here; at
    // least 1 once known)
    double room_hz() const {
        return room_hz_;
    }

private:
    double start_s_ = -1, picture_s_ = 0, other_s_ = 0, room_hz_ = 0;
    uint64_t pictures_ = 0;
};

// A picture shown at shown_s (0: not known) left out, begun at now_s: it would be late, and a later one waits
inline bool leave_out(double shown_s, double now_s, const PictureCost &cost, bool later_waiting) {
    return shown_s > 0 && later_waiting && now_s + cost.ready_s() > shown_s;
}

// A picture made too late to show: handed out SHOW_LATE_S past its time, it would show the world and the hand of a time
// already gone (a step back as the view turns); left out, the picture before stays a little longer
constexpr double SHOW_LATE_S = 0.001;
inline bool too_late_to_show(double shown_s, double now_s) {
    return shown_s > 0 && now_s > shown_s + SHOW_LATE_S;
}

// The soonest a picture shown at shown_s is begun (0: at once): BEGIN_SLACK_S before leave_out would leave it out, for
// the wait's waking late
constexpr double BEGIN_SLACK_S = 0.0005;
inline double begin_at_s(double shown_s, const PictureCost &cost) {
    return shown_s > 0 ? shown_s - cost.ready_s() - BEGIN_SLACK_S : 0;
}

// How long before a picture is shown its mouse turn is taken: toward TAKEN_PERCENTILE of how long before they were shown
// the newest readings were as the last TAKEN_KEPT pictures were turned, so that nearly every picture has the hand at
// the same time before it is shown (the rest, whose newest reading was older than that, the newest hand: a little
// later). It moves by at most STEP_S a picture,
// up and down alike: a turn's lag that jumped would show as the view jumping while the mouse turns evenly.
class TurnLead {
public:
    static constexpr size_t TAKEN_KEPT = 256;
    static constexpr size_t LOOK_EVERY = 32;
    static constexpr double TAKEN_PERCENTILE = 0.99;
    static constexpr double START_S = 0.01;
    static constexpr double MOST_S = 0.05;    // (a hitch does not hold the hand back for long)
    static constexpr double STEP_S = 0.00002; // a picture (8 ms a second at 400 a second; a fifth of a pixel's turn)

    // A picture turned when the newest reading of the hand was before_s before it is shown
    void turned(double before_s) {
        taken_[next_] = std::clamp(before_s, 0.0, MOST_S);
        next_ = (next_ + 1) % TAKEN_KEPT;
        count_ += count_ < TAKEN_KEPT ? 1 : 0;
        if (count_ >= LOOK_EVERY && next_ % LOOK_EVERY == 0) {
            std::array<double, TAKEN_KEPT> sorted = taken_;
            auto end = sorted.begin() + static_cast<std::ptrdiff_t>(count_);
            auto at = sorted.begin() + static_cast<std::ptrdiff_t>(TAKEN_PERCENTILE * double(count_ - 1));
            std::nth_element(sorted.begin(), at, end);
            target_s_ = *at;
        }
        lead_s_ += std::clamp(target_s_ - lead_s_, -STEP_S, STEP_S);
    }

    // The hand is taken this long before the picture is shown
    double lead_s() const {
        return lead_s_;
    }

private:
    std::array<double, TAKEN_KEPT> taken_{};
    size_t next_ = 0, count_ = 0;
    double lead_s_ = START_S, target_s_ = START_S;
};

} // namespace pcr
