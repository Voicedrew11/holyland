/* port/platform/refresh_clock.c: the screen's refresh period and phase from the times pictures were seen,
   late wakes, refreshes without a picture, times on no refresh, and a screen that changes its rate.

     refresh_clock_test */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "port/refresh_clock.h"

static int failed, checked;

static void check(const char *what, int ok) {
    checked++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

/* A repeatable 0..1 */
static double random_unit(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / (double)(1u << 24);
}

/* The screen: refresh n at start_s + n / hz. Each refresh but every skip-th one (0: none) shows a picture,
   seen up to late_s after it, and every wild-th (0: none) is seen at a time on no refresh at all. */
static void play(RefreshClock *clock, double hz, double start_s, int refreshes, int skip, double late_s, int wild,
                 uint32_t *state) {
    int n;

    for (n = 0; n < refreshes; n++) {
        double at = start_s + n / hz;

        if (skip > 0 && n % skip == skip - 1) {
            continue;
        }
        if (wild > 0 && n % wild == wild - 1) {
            at += (0.35 + 0.3 * random_unit(state)) / hz;
        }
        refresh_clock_add(clock, at + late_s * random_unit(state));
    }
}

/* How far the clock's next refresh after at_s is from the screen's, in seconds */
static double next_error(const RefreshClock *clock, double hz, double start_s, double at_s) {
    double truth = start_s + ceil((at_s - start_s) * hz) / hz;
    double got = refresh_clock_next_s(clock, at_s);
    double d = fabs(got - truth);

    return fmin(d, fabs(d - 1.0 / hz));
}

int main(void) {
    RefreshClock clock;
    uint32_t state = 1;

    /* 169.83 Hz, named 170: the period the screen keeps, not the one named */
    refresh_clock_init(&clock, 170);
    check("knows nothing at first", !refresh_clock_locked(&clock));
    check("the named period until locked", fabs(refresh_clock_period_s(&clock) - 1.0 / 170) < 1e-12);
    play(&clock, 169.83, 1000.0, 400, 0, 0.0002, 0, &state);
    check("locks on a steady screen", refresh_clock_locked(&clock));
    check("the screen's period, to 0.2 us (170 Hz's is 5.9 us off)", fabs(refresh_clock_period_s(&clock) - 1.0 / 169.83) < 2e-7);
    check("its refreshes, to 0.2 ms", next_error(&clock, 169.83, 1000.0, 1002.5 + 0.001234) < 0.0002);

    /* the same with every third refresh missed and a wait that wakes up to a millisecond late */
    refresh_clock_init(&clock, 170);
    play(&clock, 169.83, 50.0, 600, 3, 0.001, 0, &state);
    check("locks with refreshes missed and late wakes", refresh_clock_locked(&clock));
    check("the period still, to 0.5 us", fabs(refresh_clock_period_s(&clock) - 1.0 / 169.83) < 5e-7);
    check("its refreshes, to 0.6 ms (the wakes' mean lateness)", next_error(&clock, 169.83, 50.0, 60.0) < 0.0006);

    /* times on no refresh now and then are left out */
    refresh_clock_init(&clock, 144);
    play(&clock, 143.86, 7.0, 800, 0, 0.0003, 9, &state);
    check("stray times are left out", refresh_clock_locked(&clock));
    check("and do not move the period", fabs(refresh_clock_period_s(&clock) - 1.0 / 143.86) < 5e-7);

    /* pictures shown the moment they are presented (tearing): no refresh to find */
    refresh_clock_init(&clock, 170);
    {
        double at = 3.0;
        int i, ever = 0;

        for (i = 0; i < 2000; i++) {
            at += (0.5 + random_unit(&state)) / 170;
            refresh_clock_add(&clock, at);
            ever |= refresh_clock_locked(&clock);
        }
        check("times at random never lock", !ever);
    }

    /* the screen goes from 60 to 144 Hz: the clock starts again and finds the new rate */
    refresh_clock_init(&clock, 60);
    play(&clock, 59.95, 10.0, 300, 0, 0.0002, 0, &state);
    check("locks at 60", refresh_clock_locked(&clock) && fabs(refresh_clock_period_s(&clock) - 1.0 / 59.95) < 5e-7);
    play(&clock, 144.0, 20.0, 600, 0, 0.0002, 0, &state);
    check("then at 144", refresh_clock_locked(&clock) && fabs(refresh_clock_period_s(&clock) - 1.0 / 144.0) < 5e-7);

    printf("%d of %d checks failed\n", failed, checked);
    return failed ? 1 : 0;
}
