// What the profiler overlay (profiler_overlay.hpp, Shift+P) shows of the frame profiler's record
// (port/include/port/zone_record.hpp), copied out of it so the record's lock is held only while copying: a bar a
// picture, the last WINDOW_SECONDS' averages (each lane's and each zone's), the spikes, and one picture with its
// spans for the timeline: the newest, or the slowest of the window.
//
// No RmlUi, no SDL: tests/ui_frame_snapshot_test.cpp feeds a Record and reads the snapshots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "port/zone_record.hpp"

namespace ui {

// A picture's length and the GPU's busy time in it
struct FrameBar {
    uint64_t index;
    float ms, gpu_ms;
};

// A lane's time a picture, averaged over the window, by the kind of the zone on top (ms[ZONE_GPU]: a GPU lane busy)
struct LaneTimes {
    std::string name;
    bool gpu;
    double ms[ZONE_KINDS];
};

// A zone's time on one lane: its average a picture over the window, its most in one picture of the window, and its
// time in the timeline's picture
struct ZoneTimes {
    int lane;
    ZoneId id;
    double self_ms, incl_ms, calls, most_self_ms;
    double picture_self_ms;
};

// One picture with a quarter of its length either side: every lane's spans in that stretch
struct TimelinePicture {
    bool have = false;
    zones::Frame frame;
    uint64_t from_ns = 0, to_ns = 0;
    std::vector<std::vector<zones::Span>> spans; // (by lane)
};

enum class TimelineChoice {
    newest,  // the newest picture done
    slowest, // the slowest of the window, held until a slower one comes or it leaves the window
};

struct FrameSnapshot {
    std::vector<FrameBar> bars;  // oldest first, at most BARS_MOST
    std::vector<LaneTimes> lanes; // (every lane, idle ones too: they keep their places)
    std::vector<ZoneTimes> zones; // by self time, most first
    size_t window_frames = 0;
    double avg_ms = 0, p99_ms = 0, most_ms = 0, per_second = 0;
    double spike_ms = 0;         // a picture longer is a spike: twice the median and 2 ms
    std::vector<FrameBar> spikes; // newest first, at most SPIKES_MOST
    TimelinePicture timeline;
    uint64_t lost = 0, unmatched = 0; // the record's (both should be 0)
};

// Reads snapshots out of the record. It keeps the slowest picture's spans in the record (keep_frame) while that
// picture is the timeline's, and lets them go when another takes its place, at newest, or at forget.
class FrameReader {
public:
    static constexpr double WINDOW_SECONDS = 3.0;
    static constexpr size_t BARS_MOST = 1500;
    static constexpr size_t SPIKES_MOST = 40;

    // (under the record's owner's lock: zones::with_record)
    FrameSnapshot read(zones::Record &record, TimelineChoice choice);
    // The kept picture let go (the overlay hidden)
    void forget(zones::Record &record);

private:
    // the timeline's picture at slowest: its index, or -1
    int64_t held_ = -1;
};

} // namespace ui
