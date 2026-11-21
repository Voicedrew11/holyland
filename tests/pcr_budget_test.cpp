// The scene renderer's pictures by budget (port/gs/pc/pcr_budget.h): the times' high percentile, when a picture is
// begun, and a GPU slower than the pictures asked for, on which only pictures that are ready by their time are made and
// the vblank's own pictures are never late.
#include "pcr_budget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace pcr;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

bool near(double a, double b) {
    return std::fabs(a - b) < 1e-9;
}

void recent_high() {
    RecentTimes t(0.005);
    CHECK(near(t.high_s(), 0.005));
    for (int i = 0; i < 100; i++) {
        t.add(0.001 * (i % 10 + 1)); // 1 to 10 ms
    }
    CHECK(t.count() == RecentTimes::KEPT);
    CHECK(t.high_s() >= 0.009 - 1e-12 && t.high_s() <= 0.010 + 1e-12);
    t.add(-1); // (not a time)
    CHECK(t.count() == RecentTimes::KEPT);
    for (size_t i = 0; i < RecentTimes::KEPT; i++) {
        t.add(0.002);
    }
    CHECK(near(t.high_s(), 0.002));
}

void begin_and_done() {
    PictureBudget b;
    for (int i = 0; i < 40; i++) {
        b.made(PictureKind::between, 0.001, 0.006);
        b.made(PictureKind::frame, 0.0005, 0.003);
    }
    double ready = 0.001 * PictureBudget::MARGIN + 0.006 * PictureBudget::MARGIN + PictureBudget::SLACK_S;
    CHECK(near(b.ready_s(PictureKind::between), ready));
    CHECK(near(b.begin_at_s(PictureKind::between, 10.0), 10.0 - ready));
    // the GPU idle: done after the whole cost; busy past the CPU's part: after it
    CHECK(near(b.done_s(PictureKind::between, 5.0, 0), 5.0 + ready - PictureBudget::SLACK_S));
    CHECK(near(b.done_s(PictureKind::between, 5.0, 5.1), 5.1 + 0.006 * PictureBudget::MARGIN));
    // a picture between: by its own time; where the next vblank's own picture is a game frame's, before that one's GPU
    // work too
    const double v = 1001.0 / 60000.0;
    CHECK(near(b.deadline_s(10.0, 0.95, v, false), 10.0));
    CHECK(near(b.deadline_s(10.0, 0.25, v, true), 10.0));
    for (int i = 0; i < 40; i++) {
        b.made(PictureKind::scene, 0, 0.001);
    }
    CHECK(near(b.deadline_s(10.0, 0.95, v, true),
               10.0 + 0.05 * v - 0.004 * PictureBudget::MARGIN - PictureBudget::SLACK_S));
    CHECK(near(b.between_begin_at_s(PictureKind::between, 10.0, 0.95, v, true), b.deadline_s(10.0, 0.95, v, true) - ready));
}

void queue_time() {
    GpuQueueTime q;
    CHECK(q.free_s() == 0);
    q.submitted(1, 1.000, 0.008);
    q.submitted(2, 1.002, 0.008);
    CHECK(near(q.free_s(), 1.016));
    CHECK(q.in_flight() == 2);
    q.done(1, 1.007);
    CHECK(near(q.free_s(), 1.016)); // (2 still in flight: as predicted)
    q.done(2, 1.013);
    CHECK(q.free_s() == 0);
    CHECK(q.in_flight() == 0);
}

// 240 a second (4 pictures a vblank), every picture 1 ms of the CPU's and gpu_ms of the GPU's (a game frame's own
// picture frame_ms), shown a quarter of a vblank after its time. With turning (the mouse), every picture is a picture
// between; without, each game frame's first vblank's own picture (every other vblank) is the game frame's. Each picture
// is begun at its begin time (or as soon as the one before is begun); a picture between is left out unless it fits.
struct Run {
    int made = 0, left_out = 0, frames_late = 0, late = 0;
    double most_gap_ms = 0; // (the longest between two pictures made, once measured)
    int own_after = -1;     // (the vblank of the first picture between shown by a vblank's own made after the spike)
};

struct Spike {
    int from = -1, to = -1; // (vblanks) where a picture between costs ms: a change of settings, a load
    double ms = 0;
};

// per: pictures a vblank (4: 240 a second; 1: 60, every picture a vblank's own); asked: a picture is begun no sooner
// than a vblank before its time, when it is asked for (as gs.c gets the vblank), not as soon as its begin time says;
// clocks: a GPU that lowers its clocks when it has been busy less than 30% of the time lately (over about 200 ms), its
// work then 1.8 times as long (an AMD APU's 400 MHz for 1800: busy half the time it kept 1800, a sixth 400)
Run simulate(double gpu_ms, double frame_ms, bool turning, int vblanks, Spike spike = {}, int per = 4,
             bool asked = false, bool clocks = false) {
    const double v = 1001.0 / 60000.0, cpu = 0.001;
    PictureBudget b;
    GpuQueueTime q;
    struct Flight {
        uint64_t serial;
        double done;
    };
    std::vector<Flight> flights;
    double now = 0, gpu_free = 0, last_shown = -1, busy = 1.0;
    uint64_t serial = 0;
    Run r;
    for (int n = 0; n < vblanks; n++) {
        for (int k = 0; k < per; k++) {
            const double phase = double(k) / per, shown = (n + 1) * v + phase * v + 0.25 * v;
            const bool frame = !turning && k == 0 && n % 2 == 0;
            const bool next_frame = !turning && n % 2 == 1;
            const PictureKind kind = frame ? PictureKind::frame : k == 0 ? PictureKind::between_own : PictureKind::between;
            // (the wait wakes up to 0.4 ms late)
            const double woke = 0.0001 * double((n * 7 + k * 3) % 5);
            now = std::max({now, asked ? shown - v : 0.0, (frame ? b.begin_at_s(kind, shown)
                                       : b.between_begin_at_s(kind, shown, phase, v, next_frame)) +
                                    woke});
            while (!flights.empty() && flights.front().done <= now) {
                q.done(flights.front().serial, flights.front().done);
                flights.erase(flights.begin());
            }
            if (!frame && !b.admit(kind, now, q.free_s(), shown, phase, v, next_frame)) {
                r.left_out += n >= 8;
                continue;
            }
            const bool spiked = !frame && n >= spike.from && n < spike.to;
            const double gpu = (frame ? frame_ms : spiked ? spike.ms : gpu_ms) / 1000.0;
            if (kind == PictureKind::between_own && spike.to >= 0 && n >= spike.to && r.own_after < 0) {
                r.own_after = n;
            }
            double submitted = now + cpu, start = std::max(submitted, gpu_free), slow = 1.0;
            if (clocks) {
                busy *= std::exp(-(start - gpu_free) / 0.2); // (idle since the last)
                slow = busy < 0.3 ? 1.8 : 1.0;
                busy = 1.0 - (1.0 - busy) * std::exp(-gpu * slow / 0.2);
            }
            double done = start + gpu * slow;
            gpu_free = done;
            q.submitted(++serial, submitted, b.gpu_s(kind));
            flights.push_back({serial, done});
            b.made(kind, cpu, done - start, now);
            now = submitted;
            if (n >= 8) { // (once measured)
                r.made++;
                if (done > shown + 1e-6) {
                    (frame ? r.frames_late : r.late)++;
                }
                if (last_shown > 0) {
                    r.most_gap_ms = std::max(r.most_gap_ms, (shown - last_shown) * 1e3);
                }
            }
            if (n >= 8) {
                last_shown = shown;
            }
        }
    }
    return r;
}

void slow_gpu() {
    const int counted = 192; // (vblanks once measured)
    // a GPU that makes every picture: none left out
    Run fast = simulate(2.0, 1.0, true, 200);
    CHECK(fast.left_out == 0 && fast.late == 0);
    // turning, about two pictures' time a vblank: about one in three phases, evenly; late only in the retries of a kind
    // left out a second (admit: at 1 s and 3 s here, a run of 0.25 s each)
    const double v = 1001.0 / 60000.0;
    Run slow = simulate(7.0, 3.0, true, 200);
    CHECK(slow.late <= 2 * int(PictureBudget::RETRY_RUN_S / v * 4 + 1));
    CHECK(slow.made >= counted * 4 / 3 - 4);
    CHECK(slow.most_gap_ms < 13.0);
    // slower than a vblank: fewer than one a vblank, late only in retries
    Run slower = simulate(20.0, 3.0, true, 200);
    CHECK(slower.late <= 2 * int(PictureBudget::RETRY_RUN_S / v * 4 + 1));
    CHECK(slower.made >= counted * 2 / 3 - 4 && slower.made <= counted);
    // not turning: every game frame's own picture made and on time, the pictures between around them
    Run walk = simulate(7.0, 3.0, false, 200);
    CHECK(walk.frames_late == 0 && walk.late == 0);
    CHECK(walk.made >= counted / 2 + counted / 2);
    std::printf("made a vblank: %.2f (2 ms), %.2f (7 ms, most %.1f ms apart, %d late), %.2f (20 ms, %d late), %.2f (7 "
                "ms, not turning)\n",
                fast.made / double(counted), slow.made / double(counted), slow.most_gap_ms, slow.late,
                slower.made / double(counted), slower.late, walk.made / double(counted));
}

// A cost met for a moment (half a second of pictures between at 25 ms: the scene renderer's first pictures after a
// change of its scale) does not leave the pictures between out for good once they cost what they did again. At 60 a
// second with the mouse turning every picture is a vblank's own picture between: left out for good, the view stood
// still while the mouse moved. Made again within the first retry (a second); then as before the spike.
void spike_recovers() {
    const double v = 1001.0 / 60000.0;
    const int retry_vblanks = int(std::min(PictureBudget::RETRY_FIRST_S, 100.0) / v) + 2, vblanks = 600;
    const Spike spike{100, 130, 25.0};
    Run before = simulate(12.0, 3.0, true, vblanks, {}, 1, true);
    Run r = simulate(12.0, 3.0, true, vblanks, spike, 1, true);
    CHECK(before.made >= vblanks - 8 - 4);
    CHECK(r.own_after >= spike.to && r.own_after <= spike.to + retry_vblanks);
    CHECK(r.made >= vblanks - 8 - (spike.to - spike.from) - retry_vblanks - 4);
    std::printf("after a spike (60 a second, turning): pictures between again %d vblanks after it; made a vblank %.2f "
                "(%.2f without the spike), %d late\n",
                r.own_after - spike.to, r.made / double(vblanks - 8), before.made / double(vblanks - 8), r.late);
    // a GPU that lowers its clocks when it has less to do: one picture alone costs too much to fit (22 ms), a run of
    // them what they did (12): the retry's run
    Run clocked = simulate(12.0, 3.0, true, vblanks, spike, 1, true, true);
    CHECK(clocked.own_after >= spike.to && clocked.own_after <= spike.to + retry_vblanks);
    CHECK(clocked.made >= vblanks - 8 - (spike.to - spike.from) - retry_vblanks - 16);
    std::printf("after a spike, the GPU's clocks lowered as it idles: made a vblank %.2f, %d late\n",
                clocked.made / double(vblanks - 8), clocked.late);
    // 240 a second, turning or not: the vblank's own pictures between again too
    for (bool turning : {true, false}) {
        Run fast = simulate(7.0, 3.0, turning, vblanks, {100, 130, 40.0}, 4, true);
        CHECK(fast.own_after >= spike.to && fast.own_after <= spike.to + retry_vblanks);
    }
}

} // namespace

int main() {
    recent_high();
    begin_and_done();
    queue_time();
    slow_gpu();
    spike_recovers();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("pcr_budget: ok\n");
    return 0;
}
