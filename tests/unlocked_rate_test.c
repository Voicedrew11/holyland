/* port/platform/unlocked_rate.c: --frame-rate unlocked's rate against a renderer that can draw so many pictures a
   second, which changes; its bounds; and how seldom it probes past the renderer's limit.

     unlocked_rate_test */
#include <stdint.h>
#include <stdio.h>

#include "port/unlocked_rate.h"

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

/* A renderer that draws capacity_hz pictures a second: each vblank it is given rate / 60 pictures' work and does
   capacity / 60 of it; behind when more than a vblank's work waits (the native GS's thread a vblank late) */
typedef struct {
    double backlog; /* vblanks of work waiting */
} Renderer;

static int renderer_vblank(Renderer *r, double rate_hz, double capacity_hz) {
    r->backlog += rate_hz / capacity_hz - 1.0;
    r->backlog = r->backlog < 0 ? 0 : r->backlog;
    return r->backlog > 1.0;
}

/* Plays vblanks from *vblank on; returns the vblanks behind among them, the rate's lowest and highest in the last
   `tail` of them */
static int play(UnlockedRate *rate, Renderer *r, uint64_t *vblank, int vblanks, double capacity_hz, int tail,
                double *lo, double *hi) {
    int behind_count = 0, i;

    *lo = 1e9;
    *hi = 0;
    for (i = 0; i < vblanks; i++, (*vblank)++) {
        int behind = renderer_vblank(r, rate->rate_hz, capacity_hz);

        behind_count += behind;
        unlocked_rate_vblank(rate, *vblank, behind);
        if (i >= vblanks - tail) {
            *lo = rate->rate_hz < *lo ? rate->rate_hz : *lo;
            *hi = rate->rate_hz > *hi ? rate->rate_hz : *hi;
        }
    }
    return behind_count;
}

/* A renderer that leaves out what it cannot draw, never behind: presents min(rate, capacity), measured over the last
   half second as the window does (port_video_pictures_per_second) */
static void play_presenting(UnlockedRate *rate, uint64_t *vblank, int vblanks, double capacity_hz, double *lo, double *hi) {
    double window[30] = {0};
    int i;

    *lo = 1e9;
    *hi = 0;
    for (i = 0; i < vblanks; i++, (*vblank)++) {
        double sum = 0;
        int k;

        window[*vblank % 30] = rate->rate_hz < capacity_hz ? rate->rate_hz : capacity_hz;
        for (k = 0; k < 30; k++) {
            sum += window[k];
        }
        unlocked_rate_vblank(rate, *vblank, 0);
        unlocked_rate_presented(rate, *vblank, i >= 30 ? sum / 30 : 0);
        if (i >= vblanks - 60 * 10) {
            *lo = rate->rate_hz < *lo ? rate->rate_hz : *lo;
            *hi = rate->rate_hz > *hi ? rate->rate_hz : *hi;
        }
    }
}

int main(void) {
    UnlockedRate rate;
    Renderer r = {0};
    uint64_t vblank = 100, falls;
    double lo, hi;
    int behind;

    /* A renderer of 285 a second (the owner's GPU at 4x MSAA 2: docs/port/PC_RENDERER.md), from the screen's 170 */
    unlocked_rate_init(&rate, 170, 60, 959, vblank);
    play(&rate, &r, &vblank, 60 * 30, 285, 60 * 10, &lo, &hi);
    check("climbs to the renderer's limit within 30 s", lo >= 285 * UNLOCKED_RATE_DOWN * 0.95);
    check("and stays at most a rise above it", hi <= 285 * UNLOCKED_RATE_UP + UNLOCKED_RATE_STEP_HZ);
    falls = rate.falls;
    behind = play(&rate, &r, &vblank, 60 * 60, 285, 60 * 60, &lo, &hi);
    check("at the limit, a fall at most every 4 s after the rest has grown", rate.falls - falls <= 15);
    check("at the limit, behind at under 5% of vblanks", behind < 60 * 60 / 20);
    printf("     (a minute at 285: %llu falls, %d vblanks behind, the rate %.0f to %.0f)\n",
           (unsigned long long)(rate.falls - falls), behind, lo, hi);

    /* The place gets heavier: 140 a second */
    behind = play(&rate, &r, &vblank, 60 * 5, 140, 60 * 2, &lo, &hi);
    check("a heavier place: down to it within 3 s", hi <= 140 * UNLOCKED_RATE_UP + UNLOCKED_RATE_STEP_HZ && lo >= 60);
    check("a heavier place (half the pictures it was drawing): behind for under 3 s", behind < 180);
    printf("     (a heavier place: %d vblanks behind)\n", behind);

    /* And lighter again: 400 a second (the rest may be long: up to 8 s a rise) */
    play(&rate, &r, &vblank, 60 * 120, 400, 60 * 10, &lo, &hi);
    check("a lighter place: up to it within two minutes", lo >= 400 * UNLOCKED_RATE_DOWN * 0.95);

    /* The cap */
    unlocked_rate_bound(&rate, 60, 165);
    check("a cap brings the rate within it at once", rate.rate_hz <= 165);
    play(&rate, &r, &vblank, 60 * 20, 400, 60 * 10, &lo, &hi);
    check("a cap holds while the renderer could do more", hi <= 165 && lo >= 165 - 1e-9);

    /* The floor: a renderer slower than it */
    unlocked_rate_bound(&rate, 60, 959);
    r.backlog = 0;
    play(&rate, &r, &vblank, 60 * 10, 40, 60, &lo, &hi);
    check("never under the floor", lo >= 60);
    check("at the floor when the renderer is slower", unlocked_rate_at_floor(&rate));

    /* Bounds the wrong way round: the floor wins */
    unlocked_rate_init(&rate, 300, 60, 30, vblank);
    check("a ceiling under the floor is the floor", rate.rate_hz == 60 && rate.ceiling_hz == 60);

    /* A renderer that is never behind: up to the ceiling */
    unlocked_rate_init(&rate, 170, 60, 959, vblank);
    r.backlog = 0;
    play(&rate, &r, &vblank, 60 * 60, 100000, 60, &lo, &hi);
    check("never behind: up to the ceiling", lo >= 959 - 1e-9);
    check("and no fall", rate.falls == 0);

    /* A renderer that leaves pictures out instead of falling behind: found by the pictures presented */
    unlocked_rate_init(&rate, 170, 60, 959, vblank);
    play_presenting(&rate, &vblank, 60 * 60, 250, &lo, &hi);
    check("leaving pictures out: the rate at most a rise past what it cannot tell (UNLOCKED_RATE_SHORT)",
          hi <= 250 / UNLOCKED_RATE_SHORT * UNLOCKED_RATE_UP + 1);
    check("leaving pictures out: and near it", lo >= 250 * UNLOCKED_RATE_UNDER * UNLOCKED_RATE_SHORT);
    printf("     (presenting at most 250: the rate %.0f to %.0f in the last 10 s, %llu falls)\n", lo, hi,
           (unsigned long long)rate.falls);

    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
