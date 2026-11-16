// The frame profiler's frames (docs/port/PROFILING.md, "Frames"): every lane's zones (port/include/port/zones.h)
// read from the rings and worked out picture by picture.
//
// A frame is the span between two pictures shown. In it, each lane's time goes to the zone on top of its stack (its
// self time), and to every zone on the stack (their inclusive time, a zone open twice counted once); a lane with no
// zone open is idle. A zone's calls are the begins in the frame. Zones still open at a frame's edge are cut there:
// their time before it is that frame's, the rest the next's.
//
// The CPU's lanes are known up to the moment they are read (the horizon). A GPU lane's times come in later, as its
// timestamps are read back, so a frame is done once every GPU lane is past its end too, or settle_seconds after it
// (a GPU lane with nothing to do). Done frames are kept keep_seconds; each lane's zones as spans (for the timeline)
// the last timeline_seconds, and those of the frames kept (keep_frame: spikes, the frame picked) until let go.
//
// One thread works it (take: "kf4-profiler"); readers hold the owner's lock (port/overlay/frames_view.cpp).
// port/platform/zone_record.cpp, kf4_zone_record.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "port/zones.h"
}

namespace zones {

// A zone's time on one lane in one frame
struct ZoneTime {
    uint16_t lane;
    ZoneId id;
    uint64_t self_ns;
    uint64_t incl_ns;
    uint32_t calls;
};

// A lane's time in one frame: its self time by the kind of the zone on top (idle not counted)
struct LaneTime {
    uint64_t ns[ZONE_KINDS];
};

struct Frame {
    uint64_t index;
    uint64_t start_ns, end_ns;
    std::vector<LaneTime> lanes; // (by lane; lanes made after the frame are missing)
    std::vector<ZoneTime> zones; // (by lane, then id)

    double ms() const {
        return double(end_ns - start_ns) / 1e6;
    }
    // The lane's time of kind in ms (0 for a lane it has not)
    double lane_ms(int lane, ZoneKind kind) const;
};

// A zone as drawn on the timeline: begun and ended, depth 0 the outermost
struct Span {
    uint64_t start_ns, end_ns;
    ZoneId id;
    uint16_t depth;
};

struct Limits {
    double keep_seconds = 10.0;
    double timeline_seconds = 2.0;
    double settle_seconds = 0.5;
};

class Record {
public:
    explicit Record(Limits limits = {});

    // ---- Fed: by take, or a test directly. Times in ns on one clock, each lane's in order.
    void add_lane(int lane, const std::string &name, bool gpu);
    void add_picture(uint64_t ns);
    void add_events(int lane, const ZoneEvent *events, size_t count);
    // The CPU's lanes are known up to horizon_ns: their open zones run to it, frames done (above), old ones let go
    void advance(uint64_t horizon_ns);
    // From the port's rings (zones.h): new lanes, the pictures, every lane's events, then advance to the time read
    void take();

    // ---- Read (by the reader under the owner's lock)
    const std::deque<Frame> &frames() const {
        return frames_;
    }
    // A done frame by its index; nullptr when not kept
    const Frame *frame(uint64_t index) const;
    int lanes() const {
        return int(lanes_.size());
    }
    const std::string &lane_name(int lane) const;
    bool lane_gpu(int lane) const;
    // The lane's spans that overlap [from_ns, to_ns), each lane's outermost first, by start
    std::vector<Span> spans(int lane, uint64_t from_ns, uint64_t to_ns) const;
    // The frame's spans, and margin_ns either side, kept past timeline_seconds (a spike, the frame picked) while the
    // frame is; let go sooner when no longer wanted
    void keep_frame(uint64_t index, uint64_t margin_ns);
    void let_go_frame(uint64_t index);
    // Events written over in the rings before they were read, and ends that matched no begin (both should be 0)
    uint64_t lost() const {
        return lost_;
    }
    uint64_t unmatched() const {
        return unmatched_;
    }
    // A lane's events written over before they were read
    uint64_t lane_lost(int lane) const;

private:
    struct Open {
        ZoneId id;
        uint64_t start_ns;
    };
    // A frame not done yet, its times as they come in
    struct Pending {
        uint64_t index, start_ns, end_ns; // (end_ns 0: still open)
        std::vector<LaneTime> lanes;
        std::vector<ZoneTime> zones; // (unsorted until done)
        std::unordered_map<uint32_t, uint32_t> at; // (a zone's place in zones, by lane and id)
    };
    struct Lane {
        std::string name;
        bool gpu = false;
        bool made = false;
        uint64_t cursor = 0;    // (the ring's: take's)
        uint64_t lost = 0;
        uint64_t at_ns = 0;     // known up to here: its time before is in the frames
        std::vector<Open> open; // its stack
        std::deque<Span> spans; // (by end)
        std::vector<ZoneEvent> backlog; // (read past the horizon: for the next take)
    };
    struct KeptSpans {
        uint64_t from_ns, to_ns;
        std::vector<std::vector<Span>> lanes;
    };

    Lane &lane(int index);
    void run_to(int lane, uint64_t ns);
    void credit(int lane, uint64_t from_ns, uint64_t to_ns);
    Pending *pending_at(uint64_t ns);
    ZoneTime &zone_time(Pending &frame, int lane, ZoneId id);
    void finish_frames(uint64_t horizon_ns);
    void let_go(uint64_t horizon_ns);
    bool kept(uint64_t index) const;
    std::vector<Span> live_spans(int lane, uint64_t from_ns, uint64_t to_ns) const;

    Limits limits_;
    std::vector<Lane> lanes_;
    std::vector<uint64_t> pictures_; // (marks not yet a frame's end)
    std::deque<Pending> pending_;    // (by time; the last open while pictures come)
    std::deque<Frame> frames_;       // done, oldest first
    std::vector<uint64_t> kept_;     // frames whose spans are kept
    std::map<uint64_t, KeptSpans> kept_spans_;
    uint64_t next_index_ = 0;
    uint64_t lost_ = 0, unmatched_ = 0;
    uint64_t picture_cursor_ = 0;
    std::vector<ZoneEvent> scratch_;
};

} // namespace zones
