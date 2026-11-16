// The frame profiler's frames (port/include/port/zone_record.hpp)
#include "port/zone_record.hpp"

#include <algorithm>
#include <unordered_map>

extern "C" {
#include "port/profile.h"
}

namespace zones {

namespace {

uint64_t seconds_ns(double seconds) {
    return uint64_t(seconds * 1e9);
}

uint32_t zone_key(int lane, ZoneId id) {
    return uint32_t(lane) << 16 | id;
}

// Events read from a ring in one go (each lane's, until it has no more)
const size_t READ_MOST = 1 << 16;

} // namespace

double Frame::lane_ms(int lane, ZoneKind kind) const {
    if (lane < 0 || size_t(lane) >= lanes.size() || unsigned(kind) >= ZONE_KINDS) {
        return 0.0;
    }
    return double(lanes[size_t(lane)].ns[kind]) / 1e6;
}

Record::Record(Limits limits) : limits_(limits) {}

Record::Lane &Record::lane(int index) {
    if (size_t(index) >= lanes_.size()) {
        lanes_.resize(size_t(index) + 1);
    }
    return lanes_[size_t(index)];
}

void Record::add_lane(int index, const std::string &name, bool gpu) {
    Lane &l = lane(index);
    l.name = name;
    l.gpu = gpu;
    l.made = true;
}

uint64_t Record::lane_lost(int index) const {
    return size_t(index) < lanes_.size() ? lanes_[size_t(index)].lost : 0;
}

const std::string &Record::lane_name(int index) const {
    static const std::string none;
    return size_t(index) < lanes_.size() ? lanes_[size_t(index)].name : none;
}

bool Record::lane_gpu(int index) const {
    return size_t(index) < lanes_.size() && lanes_[size_t(index)].gpu;
}

void Record::add_picture(uint64_t ns) {
    if (!pending_.empty()) {
        Pending &last = pending_.back();
        if (ns <= last.start_ns) {
            return; // (out of order or the same moment: not a frame)
        }
        last.end_ns = ns;
    }
    Pending next;
    next.index = next_index_++;
    next.start_ns = ns;
    next.end_ns = 0;
    pending_.push_back(std::move(next));
}

Record::Pending *Record::pending_at(uint64_t ns) {
    for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
        if (ns >= it->start_ns) {
            return it->end_ns == 0 || ns < it->end_ns ? &*it : nullptr;
        }
    }
    return nullptr;
}

ZoneTime &Record::zone_time(Pending &frame, int lane, ZoneId id) {
    // (a look along the frame's zones would be hundreds of steps a credit, thousands of credits a frame)
    auto [at, made] = frame.at.emplace(zone_key(lane, id), uint32_t(frame.zones.size()));
    if (made) {
        frame.zones.push_back({uint16_t(lane), id, 0, 0, 0});
    }
    return frame.zones[at->second];
}

// lane's stack from from_ns to to_ns into the frames it spans
void Record::credit(int index, uint64_t from_ns, uint64_t to_ns) {
    Lane &l = lanes_[size_t(index)];
    if (l.open.empty() || to_ns <= from_ns) {
        return;
    }
    const Open &top = l.open.back();
    ZoneKind kind = port_zone_kind(top.id);
    // (the first frame that ends after from_ns, by a search: dozens wait for the GPU's times, and a credit spans one
    // or two of them)
    auto first = std::upper_bound(pending_.begin(), pending_.end(), from_ns, [](uint64_t ns, const Pending &f) {
        return f.end_ns == 0 || ns < f.end_ns;
    });
    for (auto it = first; it != pending_.end() && it->start_ns < to_ns; ++it) {
        Pending &frame = *it;
        uint64_t end = frame.end_ns == 0 ? UINT64_MAX : frame.end_ns;
        uint64_t a = std::max(from_ns, frame.start_ns), b = std::min(to_ns, end);
        if (a >= b) {
            continue;
        }
        uint64_t ns = b - a;
        if (frame.lanes.size() <= size_t(index)) {
            frame.lanes.resize(size_t(index) + 1, LaneTime{});
        }
        frame.lanes[size_t(index)].ns[kind] += ns;
        zone_time(frame, index, top.id).self_ns += ns;
        for (size_t i = 0; i < l.open.size(); i++) {
            ZoneId id = l.open[i].id;
            bool again = false; // (open twice: counted once)
            for (size_t j = 0; j < i && !again; j++) {
                again = l.open[j].id == id;
            }
            if (!again) {
                zone_time(frame, index, id).incl_ns += ns;
            }
        }
    }
}

void Record::run_to(int index, uint64_t ns) {
    Lane &l = lanes_[size_t(index)];
    if (ns <= l.at_ns) {
        return;
    }
    credit(index, l.at_ns, ns);
    l.at_ns = ns;
}

void Record::add_events(int index, const ZoneEvent *events, size_t count) {
    lane(index);
    for (size_t i = 0; i < count; i++) {
        const ZoneEvent &e = events[i];
        Lane &l = lanes_[size_t(index)];
        uint64_t ns = std::max(e.ns, l.at_ns); // (a GPU lane's times a little out of order: kept in order)
        if (l.at_ns == 0) {
            l.at_ns = ns; // (the lane's first: nothing before it)
        }
        run_to(index, ns);
        if (!e.end) {
            l.open.push_back({e.id, ns});
            if (Pending *frame = pending_at(ns)) {
                zone_time(*frame, index, e.id).calls++;
            }
            continue;
        }
        // The zone it ends, and any begun inside it and left open (closed with it)
        size_t at = l.open.size();
        while (at > 0 && l.open[at - 1].id != e.id) {
            at--;
        }
        if (at == 0) {
            unmatched_++;
            continue;
        }
        while (l.open.size() >= at) {
            const Open &o = l.open.back();
            l.spans.push_back({o.start_ns, ns, o.id, uint16_t(l.open.size() - 1)});
            l.open.pop_back();
        }
    }
}

void Record::advance(uint64_t horizon_ns) {
    for (size_t i = 0; i < lanes_.size(); i++) {
        if (!lanes_[i].gpu && lanes_[i].at_ns != 0) {
            run_to(int(i), horizon_ns);
        }
    }
    finish_frames(horizon_ns);
    let_go(horizon_ns);
}

void Record::finish_frames(uint64_t horizon_ns) {
    uint64_t settle = seconds_ns(limits_.settle_seconds);
    while (!pending_.empty() && pending_.front().end_ns != 0) {
        Pending &p = pending_.front();
        if (p.end_ns > horizon_ns) {
            return;
        }
        bool settled = horizon_ns - p.end_ns >= settle;
        for (const Lane &l : lanes_) {
            if (l.gpu && l.at_ns < p.end_ns && !settled) {
                return;
            }
        }
        Frame f;
        f.index = p.index;
        f.start_ns = p.start_ns;
        f.end_ns = p.end_ns;
        f.lanes = std::move(p.lanes);
        f.lanes.resize(lanes_.size(), LaneTime{});
        f.zones = std::move(p.zones);
        std::sort(f.zones.begin(), f.zones.end(), [](const ZoneTime &a, const ZoneTime &b) {
            return zone_key(a.lane, a.id) < zone_key(b.lane, b.id);
        });
        pending_.pop_front();
        frames_.push_back(std::move(f));
    }
}

bool Record::kept(uint64_t index) const {
    return std::find(kept_.begin(), kept_.end(), index) != kept_.end();
}

void Record::let_go(uint64_t horizon_ns) {
    uint64_t keep = seconds_ns(limits_.keep_seconds), timeline = seconds_ns(limits_.timeline_seconds);
    while (!frames_.empty() && frames_.front().end_ns + keep < horizon_ns) {
        frames_.pop_front();
    }
    for (Lane &l : lanes_) {
        while (!l.spans.empty() && l.spans.front().end_ns + timeline < horizon_ns) {
            l.spans.pop_front();
        }
    }
    // (a frame kept whose record is gone: let go too)
    for (auto it = kept_spans_.begin(); it != kept_spans_.end();) {
        if (frame(it->first) == nullptr) {
            kept_.erase(std::remove(kept_.begin(), kept_.end(), it->first), kept_.end());
            it = kept_spans_.erase(it);
        } else {
            ++it;
        }
    }
}

const Frame *Record::frame(uint64_t index) const {
    if (frames_.empty() || index < frames_.front().index || index > frames_.back().index) {
        return nullptr;
    }
    // (indices run on without gaps)
    const Frame &f = frames_[size_t(index - frames_.front().index)];
    return f.index == index ? &f : nullptr;
}

std::vector<Span> Record::live_spans(int index, uint64_t from_ns, uint64_t to_ns) const {
    std::vector<Span> out;
    if (size_t(index) >= lanes_.size()) {
        return out;
    }
    const Lane &l = lanes_[size_t(index)];
    // (by end: those ending before from_ns are skipped by a search, the rest looked at)
    auto first = std::lower_bound(l.spans.begin(), l.spans.end(), from_ns,
                                  [](const Span &s, uint64_t ns) { return s.end_ns <= ns; });
    for (auto it = first; it != l.spans.end(); ++it) {
        if (it->start_ns < to_ns) {
            out.push_back(*it);
        }
    }
    for (size_t d = 0; d < l.open.size(); d++) {
        const Open &o = l.open[d];
        if (o.start_ns < to_ns && l.at_ns > from_ns) {
            out.push_back({o.start_ns, l.at_ns, o.id, uint16_t(d)});
        }
    }
    std::sort(out.begin(), out.end(), [](const Span &a, const Span &b) {
        return a.start_ns != b.start_ns ? a.start_ns < b.start_ns : a.depth < b.depth;
    });
    return out;
}

std::vector<Span> Record::spans(int index, uint64_t from_ns, uint64_t to_ns) const {
    for (const auto &k : kept_spans_) {
        const Frame *f = frame(k.first);
        if (f != nullptr && from_ns >= k.second.from_ns && to_ns <= k.second.to_ns) {
            std::vector<Span> out;
            if (size_t(index) < k.second.lanes.size()) {
                for (const Span &s : k.second.lanes[size_t(index)]) {
                    if (s.start_ns < to_ns && s.end_ns > from_ns) {
                        out.push_back(s);
                    }
                }
            }
            return out;
        }
    }
    return live_spans(index, from_ns, to_ns);
}

void Record::keep_frame(uint64_t index, uint64_t margin_ns) {
    const Frame *f = frame(index);
    if (f == nullptr || kept(index)) {
        return;
    }
    KeptSpans k;
    k.from_ns = f->start_ns > margin_ns ? f->start_ns - margin_ns : 0;
    k.to_ns = f->end_ns + margin_ns;
    for (size_t i = 0; i < lanes_.size(); i++) {
        k.lanes.push_back(live_spans(int(i), k.from_ns, k.to_ns));
    }
    kept_.push_back(index);
    kept_spans_.emplace(index, std::move(k));
}

void Record::let_go_frame(uint64_t index) {
    kept_.erase(std::remove(kept_.begin(), kept_.end(), index), kept_.end());
    kept_spans_.erase(index);
}

void Record::take() {
    uint64_t horizon = port_profile_now_ns();
    int count = port_zone_lanes();
    for (int i = 0; i < count; i++) {
        Lane &l = lane(i);
        if (!l.made) {
            add_lane(i, port_zone_lane_name(i), port_zone_lane_is_gpu(i) != 0);
        }
    }
    uint64_t pictures[256];
    while (size_t n = port_zone_pictures_read(&picture_cursor_, pictures, 256)) {
        for (size_t k = 0; k < n; k++) {
            add_picture(pictures[k]);
        }
    }
    scratch_.resize(READ_MOST);
    for (int i = 0; i < count; i++) {
        Lane &l = lanes_[size_t(i)];
        // (what the ring has past the horizon waits for the next take: a picture after the horizon may not be read
        // yet, and its frame would get the time)
        size_t held = 0;
        for (const ZoneEvent &e : l.backlog) {
            if (e.ns > horizon) {
                break;
            }
            held++;
        }
        add_events(i, l.backlog.data(), held);
        l.backlog.erase(l.backlog.begin(), l.backlog.begin() + long(held));
        for (;;) {
            uint64_t lost = 0;
            size_t n = port_zone_lane_read(i, &lanes_[size_t(i)].cursor, scratch_.data(), READ_MOST, &lost);
            lost_ += lost;
            lanes_[size_t(i)].lost += lost;
            if (n == 0) {
                break;
            }
            Lane &now = lanes_[size_t(i)];
            size_t ready = 0;
            while (ready < n && now.backlog.empty() && scratch_[ready].ns <= horizon) {
                ready++;
            }
            add_events(i, scratch_.data(), ready);
            Lane &after = lanes_[size_t(i)];
            after.backlog.insert(after.backlog.end(), scratch_.begin() + long(ready), scratch_.begin() + long(n));
            if (n < READ_MOST) {
                break;
            }
        }
    }
    advance(horizon);
}

} // namespace zones
