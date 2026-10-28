/* The screen's refresh clock: when the screen refreshes, learnt from the times it was seen to.

   A window's pictures are on the screen from one of its refreshes on, at the screen's own rate (169.83 Hz,
   not the 170 the system names), whatever the times they were handed over at. Pictures timed by any
   other clock beat against it: every so often a refresh shows two steps of the world or none. With the
   refresh times known, each picture is timed for the refresh it is shown at, one a refresh.

   The window adds the times of the screen's vblanks as the system knows them (port/include/port/screen_vblank.h),
   or any times that are refreshes give or take a little: the clock numbers the refreshes they fall on and
   fits the line through the last REFRESH_CLOCK_SAMPLES (least squares), so a time a little off moves it by
   1/REFRESH_CLOCK_SAMPLES of that. A time off the line by more than a quarter of a refresh, or on a
   refresh already seen, is not a refresh (a time read very late, a screen that refreshes faster than it
   did): left out, and when REFRESH_CLOCK_RESTART of the
   last REFRESH_CLOCK_HISTORY times were, the clock starts again from the next (the window moved to
   another screen, the screen's mode changed), guessing the refresh from the shortest time between the
   last REFRESH_CLOCK_HISTORY.

   port/platform/refresh_clock.c; no locks (the caller's), no globals. */
#ifndef PORT_REFRESH_CLOCK_H
#define PORT_REFRESH_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REFRESH_CLOCK_SAMPLES 128
#define REFRESH_CLOCK_HISTORY 16
#define REFRESH_CLOCK_RESTART 8

typedef struct {
    double named_s;                       /* the refresh time the system names, the first guess (0: 1/60) */
    double origin_s;                      /* the first time added: the others are kept after it */
    int64_t index[REFRESH_CLOCK_SAMPLES]; /* each time's refresh, counted from origin_s's */
    double time_s[REFRESH_CLOCK_SAMPLES]; /* and the time, less origin_s */
    int count, next;                      /* the times kept (up to REFRESH_CLOCK_SAMPLES), the next's place */
    uint32_t left_out;                    /* the last REFRESH_CLOCK_HISTORY times, a bit each: 1, left out */
    int64_t newest;                       /* the newest time's refresh */
    double seen_s[REFRESH_CLOCK_HISTORY]; /* the last REFRESH_CLOCK_HISTORY times as added, kept or not */
    int seen;                             /* how many times were added */
    double period_s, phase_s;             /* the line: refresh n at origin_s + phase_s + n * period_s */
    double residual_s;                    /* the times' RMS distance from it */
} RefreshClock;

/* A clock that knows nothing yet; named_hz, the screen's rate as the system names it (0: unknown) */
void refresh_clock_init(RefreshClock *clock, double named_hz);
/* The screen refreshed at seen_s (any one clock's seconds, port_wall_seconds) */
void refresh_clock_add(RefreshClock *clock, double seen_s);
/* 1 when the clock is sure enough to time pictures by: enough times over enough refreshes, on the line
   to within a tenth of a refresh, and few of the last left out */
int refresh_clock_locked(const RefreshClock *clock);
/* A refresh's length, in seconds (named_s until locked) */
double refresh_clock_period_s(const RefreshClock *clock);
/* The first refresh at or after at_s (when locked) */
double refresh_clock_next_s(const RefreshClock *clock, double at_s);

#ifdef __cplusplus
}
#endif

#endif
