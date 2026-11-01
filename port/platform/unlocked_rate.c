/* --frame-rate unlocked's rate (port/include/port/unlocked_rate.h) */
#include <string.h>

#include "port/unlocked_rate.h"

static double clamp(double v, double lo, double hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void unlocked_rate_init(UnlockedRate *rate, double start_hz, double floor_hz, double ceiling_hz, uint64_t vblank) {
    memset(rate, 0, sizeof(*rate));
    rate->floor_hz = floor_hz;
    rate->ceiling_hz = ceiling_hz > floor_hz ? ceiling_hz : floor_hz;
    rate->rate_hz = clamp(start_hz, rate->floor_hz, rate->ceiling_hz);
    rate->probe_rest = UNLOCKED_RATE_REST;
    rate->next_rise = vblank + UNLOCKED_RATE_REST;
    rate->changed_at = vblank;
}

void unlocked_rate_bound(UnlockedRate *rate, double floor_hz, double ceiling_hz) {
    rate->floor_hz = floor_hz;
    rate->ceiling_hz = ceiling_hz > floor_hz ? ceiling_hz : floor_hz;
    rate->rate_hz = clamp(rate->rate_hz, rate->floor_hz, rate->ceiling_hz);
}

/* Down to to_hz */
static void fall(UnlockedRate *rate, uint64_t vblank, double to_hz) {
    if (rate->probed_at && vblank < rate->probed_at + 2 * UNLOCKED_RATE_SETTLE) {
        rate->probe_rest = rate->probe_rest * 2 < UNLOCKED_RATE_REST_MOST ? rate->probe_rest * 2 : UNLOCKED_RATE_REST_MOST;
    }
    rate->fell_from_hz = rate->rate_hz;
    rate->rate_hz = clamp(to_hz, rate->floor_hz, rate->ceiling_hz);
    rate->changed_at = vblank;
    rate->settle_until = vblank + UNLOCKED_RATE_SETTLE;
    rate->next_rise = vblank + UNLOCKED_RATE_REST;
    rate->probed_at = 0;
    rate->fell_at = vblank;
    rate->behind_run = 0;
    rate->falls++;
}

static void rise(UnlockedRate *rate, uint64_t vblank) {
    double up = rate->rate_hz * UNLOCKED_RATE_UP;
    double safe = rate->fell_from_hz * UNLOCKED_RATE_SAFE;

    if (up < rate->rate_hz + UNLOCKED_RATE_STEP_HZ) {
        up = rate->rate_hz + UNLOCKED_RATE_STEP_HZ;
    }
    if (rate->probed_at) {
        rate->probe_rest = UNLOCKED_RATE_REST; /* (the probe before held: the limit is higher than was found) */
        rate->fell_from_hz = 0;
    }
    if (rate->fell_from_hz > 0 && rate->rate_hz < safe) {
        up = up < safe ? up : safe; /* (under the limit last found: quickly, then a probe's rest before past it) */
        rate->next_rise = vblank + (up < safe ? UNLOCKED_RATE_REST : rate->probe_rest);
        rate->probed_at = 0;
    } else {
        rate->next_rise = vblank + rate->probe_rest; /* (past it: a probe, then a rest) */
        rate->probed_at = vblank;
    }
    rate->rate_hz = clamp(up, rate->floor_hz, rate->ceiling_hz);
    rate->changed_at = vblank;
    rate->rises++;
}

void unlocked_rate_vblank(UnlockedRate *rate, uint64_t vblank, int behind) {
    rate->behind_run = behind ? rate->behind_run + 1 : 0;
    if (vblank < rate->settle_until) {
        return;
    }
    if (rate->behind_run >= 2 && rate->rate_hz > rate->floor_hz) {
        int again = rate->fell_at && vblank <= rate->fell_at + UNLOCKED_RATE_SETTLE + 2;

        fall(rate, vblank, rate->rate_hz * (again ? UNLOCKED_RATE_DOWN * UNLOCKED_RATE_DOWN : UNLOCKED_RATE_DOWN));
    } else if (rate->behind_run == 0 && vblank >= rate->next_rise && rate->rate_hz < rate->ceiling_hz) {
        rise(rate, vblank);
    }
}

void unlocked_rate_presented(UnlockedRate *rate, uint64_t vblank, double presented_hz) {
    if (presented_hz <= 0 || vblank < rate->settle_until || vblank < rate->changed_at + UNLOCKED_RATE_MEASURE ||
        rate->rate_hz <= rate->floor_hz || presented_hz >= UNLOCKED_RATE_SHORT * rate->rate_hz) {
        return;
    }
    fall(rate, vblank, presented_hz * UNLOCKED_RATE_UNDER);
}

int unlocked_rate_at_floor(const UnlockedRate *rate) {
    return rate->rate_hz <= rate->floor_hz;
}
