// The frame profiler (port/include/port/zones.h, zone_record.hpp): the rings read while written, events written
// over counted as lost, and the frames' self, inclusive and call counts worked out from a lane's events, cut at the
// pictures, with GPU lanes that come in late.
//
//   zones_test
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "port/zone_record.hpp"

extern "C" {
#include "port/profile.h"
#include "port/thread_name.h"
}

namespace {

int failed, checked;

void check(const char *what, bool ok) {
    checked++;
    if (!ok) {
        failed++;
        std::printf("FAIL %s\n", what);
    } else {
        std::printf("ok   %s\n", what);
    }
}

const uint64_t MS = 1000000;

ZoneEvent begin(uint64_t ns, ZoneId id) {
    return {ns, id, 0};
}

ZoneEvent end(uint64_t ns, ZoneId id) {
    return {ns, id, 1};
}

const zones::ZoneTime *find(const zones::Frame &f, int lane, ZoneId id) {
    for (const zones::ZoneTime &z : f.zones) {
        if (z.lane == lane && z.id == id) {
            return &z;
        }
    }
    return nullptr;
}

// One lane, two pictures: nested zones, self and inclusive, a zone across the picture, idle time not counted
void frames_from_events() {
    ZoneId outer = port_zone_id("test outer", ZONE_WORK);
    ZoneId inner = port_zone_id("test inner", ZONE_WORK);
    ZoneId wait = port_zone_id("test wait", ZONE_WAIT);
    check("an id for each name", outer != ZONE_NONE && inner != ZONE_NONE && outer != inner);
    check("the same name, the same id", port_zone_id("test outer", ZONE_WAIT) == outer);
    check("the kind of the first registration", port_zone_kind(outer) == ZONE_WORK && port_zone_kind(wait) == ZONE_WAIT);

    zones::Record r;
    r.add_lane(0, "cpu", false);
    const uint64_t t = 1000 * MS;
    r.add_picture(t);
    r.add_picture(t + 10 * MS);
    r.add_picture(t + 20 * MS);
    // frame 0 [t, t+10): outer 1-9 with inner 2-5 and the wait 6-7
    // frame 1 [t+10, t+20): outer from 12 to 25, crossing into frame 2 (still open: its picture not there)
    ZoneEvent e[] = {begin(t + 1 * MS, outer), begin(t + 2 * MS, inner), end(t + 5 * MS, inner),
                     begin(t + 6 * MS, wait),  end(t + 7 * MS, wait),   end(t + 9 * MS, outer),
                     begin(t + 12 * MS, outer), end(t + 25 * MS, outer)};
    r.add_events(0, e, sizeof(e) / sizeof(e[0]));
    r.advance(t + 30 * MS);
    check("two frames done (the third open)", r.frames().size() == 2);
    if (r.frames().size() != 2) {
        return;
    }
    const zones::Frame &f0 = r.frames()[0], &f1 = r.frames()[1];
    const zones::ZoneTime *o0 = find(f0, 0, outer), *i0 = find(f0, 0, inner), *w0 = find(f0, 0, wait);
    check("outer's self: 8 ms less inner's 3 and the wait's 1", o0 && o0->self_ns == 4 * MS);
    check("outer's inclusive: 8 ms", o0 && o0->incl_ns == 8 * MS);
    check("inner's self and inclusive: 3 ms", i0 && i0->self_ns == 3 * MS && i0->incl_ns == 3 * MS);
    check("one call each", o0 && i0 && w0 && o0->calls == 1 && i0->calls == 1 && w0->calls == 1);
    check("the lane: 7 ms working, 1 ms waiting, idle not counted",
          f0.lanes[0].ns[ZONE_WORK] == 7 * MS && f0.lanes[0].ns[ZONE_WAIT] == 1 * MS);
    const zones::ZoneTime *o1 = find(f1, 0, outer);
    check("a zone across a picture: its time before it in that frame", o1 && o1->self_ns == 8 * MS && o1->calls == 1);
    check("frame lengths", f0.ms() == 10.0 && f1.index == f0.index + 1);

    std::vector<zones::Span> s = r.spans(0, t, t + 10 * MS);
    check("frame 0's spans: outer, inner, the wait", s.size() == 3 && s[0].id == outer && s[0].depth == 0 &&
                                                          s[1].id == inner && s[1].depth == 1);
}

// The same zone open within itself counts its inclusive time once; an end with no begin is counted, not credited
void nesting_and_unmatched() {
    ZoneId a = port_zone_id("test again", ZONE_WORK);
    ZoneId b = port_zone_id("test b", ZONE_WORK);
    zones::Record r;
    r.add_lane(0, "cpu", false);
    const uint64_t t = 2000 * MS;
    r.add_picture(t);
    r.add_picture(t + 10 * MS);
    ZoneEvent e[] = {begin(t + 1 * MS, a), begin(t + 2 * MS, a), end(t + 4 * MS, a), end(t + 5 * MS, a),
                     end(t + 6 * MS, b),
                     // a closed by its outer's end: the inner closed with it
                     begin(t + 7 * MS, a), begin(t + 7 * MS, b), end(t + 9 * MS, a)};
    r.add_events(0, e, sizeof(e) / sizeof(e[0]));
    r.advance(t + 20 * MS);
    check("one frame", r.frames().size() == 1);
    if (r.frames().empty()) {
        return;
    }
    const zones::ZoneTime *za = find(r.frames()[0], 0, a), *zb = find(r.frames()[0], 0, b);
    check("open within itself: inclusive once (4 + 2 ms)", za && za->incl_ns == 6 * MS);
    check("calls: 3 begins", za && za->calls == 3);
    check("an end with no begin: counted", r.unmatched() == 1);
    check("left open inside its outer: closed with it", zb && zb->self_ns == 2 * MS);
}

// A GPU lane's times come late: the frame waits for them until settled
void gpu_lane_waits() {
    ZoneId g = port_zone_id("test gpu", ZONE_GPU);
    zones::Record r(zones::Limits{10.0, 2.0, 0.5});
    r.add_lane(0, "cpu", false);
    r.add_lane(1, "gpu", true);
    const uint64_t t = 3000 * MS;
    r.add_picture(t);
    r.add_picture(t + 10 * MS);
    r.add_picture(t + 20 * MS);
    r.advance(t + 30 * MS);
    check("not done while the GPU lane is behind", r.frames().empty());
    ZoneEvent e[] = {begin(t + 2 * MS, g), end(t + 6 * MS, g), begin(t + 11 * MS, g), end(t + 12 * MS, g)};
    r.add_events(1, e, 4);
    r.advance(t + 31 * MS);
    check("done once the GPU is past it: frame 0 only", r.frames().size() == 1);
    check("its GPU time", !r.frames().empty() && r.frames()[0].lane_ms(1, ZONE_GPU) == 4.0);
    r.advance(t + 600 * MS);
    check("settled: the rest done all the same", r.frames().size() == 2);
}

// One writer thread, the record read at the same time: every event read once, in order, or counted as lost
void ring_read_while_written() {
    ZoneId z = port_zone_id("test ring", ZONE_WORK);
    port_zones_want(1);
    std::atomic<bool> done{false};
    const int pairs = 3000000; // (6M events: more than the ring holds, so a slow reader loses some)
    int lane = -1;
    std::thread writer([&] {
        for (int i = 0; i < pairs; i++) {
            if (port_zone_begin(z)) {
                port_zone_end(z);
            }
        }
        done.store(true);
    });
    std::vector<ZoneEvent> got(1 << 16);
    uint64_t cursor = 0, read = 0, lost_all = 0, last_ns = 0;
    bool ordered = true, alternating = true;
    int expect_end = -1;
    for (;;) {
        bool finished = done.load();
        if (lane < 0) {
            for (int i = 0; i < port_zone_lanes(); i++) {
                if (!port_zone_lane_is_gpu(i)) {
                    lane = i;
                }
            }
        }
        if (lane >= 0) {
            uint64_t lost = 0;
            size_t n = port_zone_lane_read(lane, &cursor, got.data(), got.size(), &lost);
            lost_all += lost;
            if (lost) {
                expect_end = -1;
            }
            for (size_t i = 0; i < n; i++) {
                ordered = ordered && got[i].ns >= last_ns && got[i].id == z;
                last_ns = got[i].ns;
                if (expect_end >= 0 && got[i].end != uint8_t(expect_end)) {
                    alternating = false;
                }
                expect_end = got[i].end ? 0 : 1;
            }
            read += n;
            if (finished && n == 0) {
                break;
            }
        }
    }
    writer.join();
    port_zones_want(-1);
    check("read while written: in order, whole", ordered);
    check("begins and ends alternate between losses", alternating);
    check("every event read or counted lost", read + lost_all == uint64_t(pairs) * 2);
    std::printf("     (%llu read, %llu lost)\n", (unsigned long long)read, (unsigned long long)lost_all);
}

// Off: nothing written
void off_writes_nothing() {
    ZoneId z = port_zone_id("test off", ZONE_WORK);
    int before = port_zone_lanes();
    std::thread t([&] { check("off: a begin not written", port_zone_begin(z) == 0); });
    t.join();
    check("off: no lane made", port_zone_lanes() == before);
}

// A thread's lane is named by the name it gave itself, on every system (Windows had "thread 22004", 2026-11-23)
void lane_named_by_its_thread() {
    ZoneId z = port_zone_id("test named", ZONE_WORK);
    port_zones_want(1);
    std::string given;
    std::thread t([&] {
        port_name_thread("kf4-test-lane");
        given = port_thread_name();
        if (port_zone_begin(z)) {
            port_zone_end(z);
        }
    });
    t.join();
    port_zones_want(-1);
    check("the name given is the thread's", given == "kf4-test-lane");
    bool lane_named = false;
    for (int i = 0; i < port_zone_lanes(); i++) {
        lane_named = lane_named || std::string(port_zone_lane_name(i)) == "kf4-test-lane";
    }
    check("its lane has its name", lane_named);
    std::string cut;
    std::thread longer([&] {
        port_name_thread("kf4-a-name-longer-than-fifteen");
        cut = port_thread_name();
    });
    longer.join();
    check("a long name kept from its start", cut.rfind("kf4-a-name-long", 0) == 0);
}

} // namespace

int main() {
    off_writes_nothing();
    frames_from_events();
    nesting_and_unmatched();
    gpu_lane_waits();
    ring_read_while_written();
    lane_named_by_its_thread();
    std::printf("%d of %d failed\n", failed, checked);
    return failed ? 1 : 0;
}
