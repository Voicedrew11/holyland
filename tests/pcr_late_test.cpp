// --frame-rate unlocked's pictures left out by the pump (port/gs/pc/pcr_late.h): the cost's estimate, the rule, and a
// pump slower than the pictures asked for, which must draw only pictures it has ready by their time, evenly.
#include "pcr_late.h"

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

void cost_follows() {
    PictureCost cost;
    CHECK(std::fabs(cost.mean_s() - PictureCost::START_S) < 1e-12);
    for (int i = 0; i < 200; i++) {
        cost.made(0.010);
    }
    CHECK(std::fabs(cost.mean_s() - 0.010) < 1e-6);
    cost.made(0.020); // up quickly
    CHECK(cost.mean_s() > 0.012);
    for (int i = 0; i < 400; i++) {
        cost.made(0.002);
    }
    CHECK(std::fabs(cost.mean_s() - 0.002) < 1e-6);
    cost.made(0);  // (not a measure)
    cost.made(-1);
    CHECK(std::fabs(cost.mean_s() - 0.002) < 1e-6);
    CHECK(std::fabs(cost.ready_s() - (0.002 * PictureCost::MARGIN + PictureCost::GPU_LEAD_S)) < 1e-12);
}

void rule() {
    PictureCost cost; // ready_s 7 ms
    CHECK(!leave_out(0, 1.0, cost, true));              // no time: drawn
    CHECK(!leave_out(1.0, 2.0, cost, false));           // late, but the last waiting: drawn
    CHECK(leave_out(1.006, 1.0, cost, true));           // would be late, a later one waits
    CHECK(!leave_out(1.0 + cost.ready_s() + 1e-4, 1.0, cost, true)); // in time
}

// Pictures asked every 2 ms, each a vblank (16.7 ms) ahead of its time and shown 4 ms after it; the pump takes cost_s
// for each. Every picture drawn must be ready by its time (made + the GPU's lead), the steps between them even.
void slow_pump(double cost_s) {
    const double asked_every = 0.002, ahead = 1001.0 / 60000.0;
    std::vector<double> due;
    for (int i = 0; i < 2000; i++) {
        due.push_back(ahead + i * asked_every);
    }
    PictureCost cost;
    double now = 0;
    std::vector<double> drawn;
    size_t next = 0;
    while (next < due.size()) {
        // what is waiting: those asked for by now (each asked a vblank before its time)
        size_t waiting_end = next;
        while (waiting_end < due.size() && due[waiting_end] - ahead <= now) {
            waiting_end++;
        }
        if (waiting_end == next) {
            now = due[next] - ahead; // idle until the next is asked for
            continue;
        }
        if (leave_out(due[next], now, cost, waiting_end > next + 1)) {
            next++;
            continue;
        }
        now += cost_s;
        cost.made(cost_s);
        if (next > 100 && next + 20 < due.size()) { // (after the estimate has settled, before the pictures run out)
            drawn.push_back(due[next]);
            CHECK(now + PictureCost::GPU_LEAD_S <= due[next] + 1e-9);
        }
        next++;
    }
    CHECK(drawn.size() > 10);
    double most = 0, least = 1;
    for (size_t i = 1; i < drawn.size(); i++) {
        double step = drawn[i] - drawn[i - 1];
        most = step > most ? step : most;
        least = step < least ? step : least;
    }
    // each step the pump's own time, to the nearest picture asked for (faster than asked: each one asked)
    double step_least = cost_s > asked_every ? cost_s - asked_every : asked_every;
    CHECK(least >= step_least - 1e-9);
    CHECK(most <= (cost_s > asked_every ? cost_s : 0) + asked_every + 1e-9);
}

void late_to_show() {
    CHECK(!too_late_to_show(0, 5.0));
    CHECK(!too_late_to_show(1.0, 1.0 + SHOW_LATE_S));
    CHECK(too_late_to_show(1.0, 1.0 + SHOW_LATE_S + 1e-6));
}

void begin_at() {
    PictureCost cost;
    CHECK(begin_at_s(0, cost) == 0);
    CHECK(std::fabs(begin_at_s(2.0, cost) - (2.0 - cost.ready_s() - BEGIN_SLACK_S)) < 1e-12);
    CHECK(!leave_out(2.0, begin_at_s(2.0, cost) + BEGIN_SLACK_S * 0.9, cost, true)); // (woken a little late: kept)
}

// The lead moves toward the percentile of how long before they were shown the pictures were turned, by at most STEP_S a
// picture
void turn_lead() {
    TurnLead lead;
    CHECK(std::fabs(lead.lead_s() - TurnLead::START_S) < 1e-12);
    double before = lead.lead_s();
    bool small_steps = true;
    for (size_t i = 0; i < 4000; i++) {
        lead.turned(0.004 + 0.0001 * double(i % 10)); // 4.0 to 4.9 ms
        small_steps &= std::fabs(lead.lead_s() - before) <= TurnLead::STEP_S + 1e-15;
        before = lead.lead_s();
    }
    CHECK(small_steps);
    CHECK(std::fabs(lead.lead_s() - 0.0049) < 1e-9); // the 99th percentile of 4.0..4.9
    for (size_t i = 0; i < TurnLead::LOOK_EVERY; i++) {
        lead.turned(0.020);
    }
    // (32 of 256 at 20 ms: the target is past it at once, the lead follows a step a picture)
    CHECK(lead.lead_s() > 0.0049 && lead.lead_s() <= 0.0049 + TurnLead::STEP_S + 1e-12);
    for (size_t i = 0; i < 10000; i++) {
        lead.turned(10); // (held to MOST_S)
    }
    CHECK(std::fabs(lead.lead_s() - TurnLead::MOST_S) < 1e-9);
}

} // namespace

int main() {
    cost_follows();
    rule();
    begin_at();
    late_to_show();
    turn_lead();
    slow_pump(0.0015); // faster than asked: every one drawn
    slow_pump(0.003);
    slow_pump(0.0055);
    slow_pump(0.009);
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("pcr_late: all passed\n");
    return 0;
}
