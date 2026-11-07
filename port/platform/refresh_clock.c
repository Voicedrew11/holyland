/* The screen's refresh clock (port/include/port/refresh_clock.h) */
#include <math.h>
#include <string.h>

#include "port/refresh_clock.h"

#define LOCK_SAMPLES 16  /* times kept before the clock may be locked */
#define LOCK_SPAN 60     /* and the refreshes they span */
#define LOCK_RESIDUAL 0.1 /* and their RMS distance from the line, in refreshes */
#define LOCK_LEFT_OUT 2   /* and at most this many of the last REFRESH_CLOCK_HISTORY times left out */

void refresh_clock_init(RefreshClock *clock, double named_hz) {
    memset(clock, 0, sizeof(*clock));
    clock->named_s = named_hz > 0 ? 1.0 / named_hz : 1.0 / 60.0;
    clock->period_s = clock->named_s;
}

/* The line through the times kept (least squares of time on refresh number), and their distance from it */
static void fit(RefreshClock *clock) {
    double mean_n = 0, mean_t = 0, snn = 0, snt = 0, sum2 = 0;
    int i;

    for (i = 0; i < clock->count; i++) {
        mean_n += (double)clock->index[i];
        mean_t += clock->time_s[i];
    }
    mean_n /= clock->count;
    mean_t /= clock->count;
    for (i = 0; i < clock->count; i++) {
        double dn = (double)clock->index[i] - mean_n;

        snn += dn * dn;
        snt += dn * (clock->time_s[i] - mean_t);
    }
    if (snn > 0) {
        clock->period_s = snt / snn;
    }
    clock->phase_s = mean_t - clock->period_s * mean_n;
    for (i = 0; i < clock->count; i++) {
        double r = clock->time_s[i] - (clock->phase_s + clock->period_s * (double)clock->index[i]);

        sum2 += r * r;
    }
    clock->residual_s = sqrt(sum2 / clock->count);
}

static void keep(RefreshClock *clock, int64_t index, double time_s) {
    clock->index[clock->next] = index;
    clock->time_s[clock->next] = time_s;
    clock->next = (clock->next + 1) % REFRESH_CLOCK_SAMPLES;
    if (clock->count < REFRESH_CLOCK_SAMPLES) {
        clock->count++;
    }
    fit(clock);
}

/* The times left out of the last REFRESH_CLOCK_HISTORY */
static int left_out_lately(uint32_t history) {
    int n = 0;

    for (history &= (1u << REFRESH_CLOCK_HISTORY) - 1; history != 0; history &= history - 1) {
        n++;
    }
    return n;
}

/* The shortest time between two of the last times added: a refresh, at least one of them apart (else the
   named refresh) */
static double shortest_gap(const RefreshClock *clock) {
    double shortest = clock->named_s;
    int i, n = clock->seen < REFRESH_CLOCK_HISTORY ? clock->seen : REFRESH_CLOCK_HISTORY;

    for (i = 1; i < n; i++) {
        double gap = clock->seen_s[(clock->seen - i) % REFRESH_CLOCK_HISTORY] -
                     clock->seen_s[(clock->seen - i - 1) % REFRESH_CLOCK_HISTORY];

        if (gap > 0.001 && gap < shortest) {
            shortest = gap;
        }
    }
    return shortest;
}

void refresh_clock_add(RefreshClock *clock, double seen_s) {
    double t, n;

    clock->seen_s[clock->seen % REFRESH_CLOCK_HISTORY] = seen_s;
    clock->seen++;

    if (clock->count == 0) {
        clock->origin_s = seen_s;
        clock->period_s = clock->named_s;
        clock->phase_s = 0;
        clock->newest = 0;
        keep(clock, 0, 0);
        return;
    }
    t = seen_s - clock->origin_s;
    n = floor((t - clock->phase_s) / clock->period_s + 0.5);
    clock->left_out <<= 1;
    if (fabs(t - (clock->phase_s + n * clock->period_s)) > 0.25 * clock->period_s || (int64_t)n <= clock->newest) {
        clock->left_out |= 1;
        if (left_out_lately(clock->left_out) >= REFRESH_CLOCK_RESTART) {
            refresh_clock_init(clock, 1.0 / shortest_gap(clock));
        }
        return;
    }
    clock->newest = (int64_t)n;
    keep(clock, clock->newest, t);
}

int refresh_clock_locked(const RefreshClock *clock) {
    int64_t low, high;
    int i;

    if (clock->count < LOCK_SAMPLES || clock->residual_s > LOCK_RESIDUAL * clock->period_s ||
        left_out_lately(clock->left_out) > LOCK_LEFT_OUT) {
        return 0;
    }
    low = high = clock->index[0];
    for (i = 1; i < clock->count; i++) {
        low = clock->index[i] < low ? clock->index[i] : low;
        high = clock->index[i] > high ? clock->index[i] : high;
    }
    return high - low >= LOCK_SPAN;
}

double refresh_clock_period_s(const RefreshClock *clock) {
    return refresh_clock_locked(clock) ? clock->period_s : clock->named_s;
}

double refresh_clock_next_s(const RefreshClock *clock, double at_s) {
    double t = at_s - clock->origin_s - clock->phase_s;

    return clock->origin_s + clock->phase_s + ceil(t / clock->period_s - 1e-9) * clock->period_s;
}
