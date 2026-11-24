// The profiler overlay's copy of the frame profiler's record (frame_snapshot.hpp).
#include "frame_snapshot.hpp"

#include <algorithm>
#include <unordered_map>

namespace ui {

namespace {

const double MARGIN_SHARE = 0.25;        // of a picture's length, either side of it on the timeline
const double SLOWEST_FRESH_SECONDS = 1.0; // a slowest picture is taken only this young: its spans are still kept

double ms_of(uint64_t ns) {
    return double(ns) / 1e6;
}

uint64_t ns_of(double seconds) {
    return uint64_t(seconds * 1e9);
}

double percentile(std::vector<double> values, double share) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(double(values.size()) * share))];
}

double gpu_ms_of(const zones::Frame &f, int lanes) {
    double ms = 0;
    for (int lane = 0; lane < lanes; lane++) {
        ms += f.lane_ms(lane, ZONE_GPU);
    }
    return ms;
}

FrameBar bar_of(const zones::Frame &f, int lanes) {
    return {f.index, float(f.ms()), float(gpu_ms_of(f, lanes))};
}

uint64_t margin_of(const zones::Frame &f) {
    return uint64_t(double(f.end_ns - f.start_ns) * MARGIN_SHARE);
}

void read_bars(const zones::Record &r, FrameSnapshot &s) {
    const auto &frames = r.frames();
    size_t first = frames.size() > FrameReader::BARS_MOST ? frames.size() - FrameReader::BARS_MOST : 0;
    for (size_t i = first; i < frames.size(); i++) {
        s.bars.push_back(bar_of(frames[i], r.lanes()));
    }
}

// The window's frames, newest first
std::vector<const zones::Frame *> window_frames(const zones::Record &r) {
    std::vector<const zones::Frame *> out;
    const auto &frames = r.frames();
    if (frames.empty()) {
        return out;
    }
    uint64_t newest_end = frames.back().end_ns, window_ns = ns_of(FrameReader::WINDOW_SECONDS);
    for (auto it = frames.rbegin(); it != frames.rend() && it->end_ns + window_ns >= newest_end; ++it) {
        out.push_back(&*it);
    }
    return out;
}

// The averages a picture over the window: each lane's, each zone's; the lengths' figures and the spikes
void read_window(const zones::Record &r, const std::vector<const zones::Frame *> &window, FrameSnapshot &s) {
    s.lanes.assign(size_t(r.lanes()), LaneTimes{});
    for (int lane = 0; lane < r.lanes(); lane++) {
        s.lanes[size_t(lane)].name = r.lane_name(lane);
        s.lanes[size_t(lane)].gpu = r.lane_gpu(lane);
    }
    if (window.empty()) {
        return;
    }
    std::vector<double> lengths;
    std::unordered_map<uint32_t, ZoneTimes> zones_by_key;
    for (const zones::Frame *f : window) {
        lengths.push_back(f->ms());
        for (int lane = 0; lane < r.lanes(); lane++) {
            for (int k = 0; k < ZONE_KINDS; k++) {
                s.lanes[size_t(lane)].ms[k] += f->lane_ms(lane, ZoneKind(k));
            }
        }
        for (const zones::ZoneTime &z : f->zones) {
            ZoneTimes &row = zones_by_key[uint32_t(z.lane) << 16 | z.id];
            row.lane = z.lane;
            row.id = z.id;
            row.self_ms += ms_of(z.self_ns);
            row.incl_ms += ms_of(z.incl_ns);
            row.calls += z.calls;
            row.most_self_ms = std::max(row.most_self_ms, ms_of(z.self_ns));
        }
    }
    double n = double(window.size());
    s.window_frames = window.size();
    for (LaneTimes &l : s.lanes) {
        for (double &ms : l.ms) {
            ms /= n;
        }
    }
    for (auto &kv : zones_by_key) {
        ZoneTimes row = kv.second;
        row.self_ms /= n;
        row.incl_ms /= n;
        row.calls /= n;
        s.zones.push_back(row);
    }
    std::sort(s.zones.begin(), s.zones.end(), [](const ZoneTimes &a, const ZoneTimes &b) {
        return a.self_ms != b.self_ms ? a.self_ms > b.self_ms : (a.lane != b.lane ? a.lane < b.lane : a.id < b.id);
    });
    double sum = 0;
    for (double ms : lengths) {
        sum += ms;
    }
    s.avg_ms = sum / n;
    s.p99_ms = percentile(lengths, 0.99);
    s.most_ms = percentile(lengths, 1.0);
    uint64_t span_ns = window.front()->end_ns - window.back()->start_ns;
    s.per_second = span_ns > 0 ? n / (double(span_ns) / 1e9) : 0;
    s.spike_ms = 2.0 * percentile(lengths, 0.5) + 2.0;
    for (auto it = r.frames().rbegin(); it != r.frames().rend() && s.spikes.size() < FrameReader::SPIKES_MOST; ++it) {
        if (it->ms() > s.spike_ms) {
            s.spikes.push_back(bar_of(*it, r.lanes()));
        }
    }
}

// The slowest of the window's fresh pictures (their spans still in the record), or nullptr
const zones::Frame *slowest_fresh(const std::vector<const zones::Frame *> &window) {
    const zones::Frame *slowest = nullptr;
    if (window.empty()) {
        return nullptr;
    }
    uint64_t fresh_from = window.front()->end_ns - std::min(window.front()->end_ns, ns_of(SLOWEST_FRESH_SECONDS));
    for (const zones::Frame *f : window) {
        if (f->end_ns < fresh_from) {
            break;
        }
        if (slowest == nullptr || f->ms() > slowest->ms()) {
            slowest = f;
        }
    }
    return slowest;
}

void read_timeline(const zones::Record &r, const zones::Frame &f, FrameSnapshot &s) {
    TimelinePicture &t = s.timeline;
    t.have = true;
    t.frame = f;
    uint64_t margin = margin_of(f);
    t.from_ns = f.start_ns > margin ? f.start_ns - margin : 0;
    t.to_ns = f.end_ns + margin;
    for (int lane = 0; lane < r.lanes(); lane++) {
        t.spans.push_back(r.spans(lane, t.from_ns, t.to_ns));
    }
    // the zones' time in this picture, beside their averages
    for (ZoneTimes &z : s.zones) {
        for (const zones::ZoneTime &own : f.zones) {
            if (own.lane == z.lane && own.id == z.id) {
                z.picture_self_ms = ms_of(own.self_ns);
            }
        }
    }
}

} // namespace

FrameSnapshot FrameReader::read(zones::Record &record, TimelineChoice choice) {
    FrameSnapshot s;
    s.lost = record.lost();
    s.unmatched = record.unmatched();
    read_bars(record, s);
    std::vector<const zones::Frame *> window = window_frames(record);
    read_window(record, window, s);
    if (window.empty()) {
        return s;
    }
    if (choice == TimelineChoice::newest) {
        forget(record);
        read_timeline(record, *window.front(), s);
        return s;
    }
    // slowest: the held picture while it is in the window, unless a fresh one is slower
    const zones::Frame *held = held_ >= 0 ? record.frame(uint64_t(held_)) : nullptr;
    if (held != nullptr && held->end_ns + ns_of(WINDOW_SECONDS) < window.front()->end_ns) {
        held = nullptr;
    }
    const zones::Frame *fresh = slowest_fresh(window);
    if (fresh != nullptr && (held == nullptr || fresh->ms() > held->ms())) {
        forget(record);
        record.keep_frame(fresh->index, margin_of(*fresh));
        held_ = int64_t(fresh->index);
        held = fresh;
    }
    read_timeline(record, held != nullptr ? *held : *window.front(), s);
    return s;
}

void FrameReader::forget(zones::Record &record) {
    if (held_ >= 0) {
        record.let_go_frame(uint64_t(held_));
        held_ = -1;
    }
}

} // namespace ui
