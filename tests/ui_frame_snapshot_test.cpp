// The profiler overlay's copy of the frame profiler's record (port/ui/frame_snapshot.hpp): the bars, the window's
// averages and figures, the spikes, the zones by self time, and the timeline's picture, the newest or the slowest
// held while it stays in the window.
//
//   ui_frame_snapshot_test
#include <cmath>
#include <cstdio>
#include <vector>

#include "frame_snapshot.hpp"

namespace {

int failed, checked;

void check(const char *what, bool ok) {
    checked++;
    if (!ok) {
        failed++;
    }
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

bool near(double a, double b) {
    return std::fabs(a - b) < 1e-6;
}

const uint64_t MS = 1000000;

ZoneId busy = port_zone_id("snapshot busy", ZONE_WORK);
ZoneId waiting = port_zone_id("snapshot waiting", ZONE_WAIT);

// Pictures 10 ms apart from `from` (one 30 ms long, the slow one, when slow_at is in range), each with busy for its
// first 4 ms and waiting 1 ms after; the lanes known up to the last picture. The pictures' times, oldest first.
std::vector<uint64_t> feed(zones::Record &r, uint64_t from, int pictures, int slow_at) {
    std::vector<uint64_t> at;
    uint64_t t = from;
    for (int i = 0; i <= pictures; i++) {
        at.push_back(t);
        r.add_picture(t);
        if (i < pictures) {
            ZoneEvent e[] = {{t + 1 * MS, busy, 0}, {t + 5 * MS, busy, 1}, {t + 6 * MS, waiting, 0},
                             {t + 7 * MS, waiting, 1}};
            r.add_events(0, e, 4);
        }
        t += i == slow_at ? 30 * MS : 10 * MS;
    }
    r.advance(at.back() + 1 * MS);
    return at;
}

void newest_and_window() {
    zones::Record r;
    r.add_lane(0, "game", false);
    // 5 s of pictures: the first 2 s fall outside the 3 s window
    feed(r, 1000 * MS, 500, 450);
    ui::FrameReader reader;
    ui::FrameSnapshot s = reader.read(r, ui::TimelineChoice::newest);
    check("a bar for each picture done", s.bars.size() == 500);
    check("the window: about 3 s of pictures", s.window_frames >= 280 && s.window_frames <= 302);
    check("the slow picture is the longest", near(s.most_ms, 30.0));
    check("spikes over twice the median and 2 ms: 22 ms", near(s.spike_ms, 22.0));
    check("one spike, the slow picture", s.spikes.size() == 1 && near(s.spikes[0].ms, 30.0));
    check("the lane's work a picture: 4 ms", s.lanes.size() == 1 && near(s.lanes[0].ms[ZONE_WORK], 4.0));
    check("the lane's waiting a picture: 1 ms", near(s.lanes[0].ms[ZONE_WAIT], 1.0));
    check("zones by self, most first", s.zones.size() == 2 && s.zones[0].id == busy && s.zones[1].id == waiting);
    check("a zone's calls a picture", near(s.zones[0].calls, 1.0));
    check("the timeline: the newest picture", s.timeline.have && s.timeline.frame.index == s.bars.back().index);
    check("the timeline's spans: the picture's zones", s.timeline.spans.size() == 1 && s.timeline.spans[0].size() >= 2);
    check("a zone's time in the timeline's picture", near(s.zones[0].picture_self_ms, 4.0));
    double margin = s.timeline.frame.ms() * 0.25;
    check("the timeline a quarter of the picture either side",
          near(double(s.timeline.frame.start_ns - s.timeline.from_ns) / 1e6, margin) &&
              near(double(s.timeline.to_ns - s.timeline.frame.end_ns) / 1e6, margin));
}

void slowest_held() {
    zones::Record r;
    r.add_lane(0, "game", false);
    // the slow picture half a second before the newest
    std::vector<uint64_t> at = feed(r, 1000 * MS, 200, 150);
    ui::FrameReader reader;
    ui::FrameSnapshot s = reader.read(r, ui::TimelineChoice::slowest);
    check("slowest: the slow picture", s.timeline.have && near(s.timeline.frame.ms(), 30.0));
    uint64_t slow = s.timeline.frame.index;
    // 2.2 s more: past the record's 2 s of spans, still inside the window
    at = feed(r, at.back() + 10 * MS, 220, -1);
    s = reader.read(r, ui::TimelineChoice::slowest);
    check("held while in the window", s.timeline.have && s.timeline.frame.index == slow);
    check("its spans kept past the record's timeline", !s.timeline.spans.empty() && !s.timeline.spans[0].empty());
    // a second more: out of the window, the slowest of the fresh pictures (all 10 ms) instead
    feed(r, at.back() + 10 * MS, 100, -1);
    s = reader.read(r, ui::TimelineChoice::slowest);
    check("let go once out of the window", s.timeline.have && s.timeline.frame.index != slow &&
                                               near(s.timeline.frame.ms(), 10.0));
    s = reader.read(r, ui::TimelineChoice::newest);
    check("newest again", s.timeline.frame.index == r.frames().back().index);
}

void empty_record() {
    zones::Record r;
    ui::FrameReader reader;
    ui::FrameSnapshot s = reader.read(r, ui::TimelineChoice::slowest);
    check("nothing recorded: nothing shown", s.bars.empty() && !s.timeline.have && s.zones.empty() && s.window_frames == 0);
}

} // namespace

int main() {
    newest_and_window();
    slowest_held();
    empty_record();
    std::printf("%d of %d checks failed\n", failed, checked);
    return failed ? 1 : 0;
}
