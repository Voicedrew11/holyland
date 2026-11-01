/* --frame-rate unlocked: how many pictures a second, from whether the renderer keeps up with them.

   Unlocked draws as many pictures between the game's frames as the renderer can, each shown at its time (the
   swapchain mailbox, or immediate where tearing is allowed: port/platform/present_vk.cpp). How many it can is not
   known ahead (the GPU, the settings, the place), so the rate is found as it plays: at each game vblank the caller
   says whether the native GS was behind (its thread a vblank late, as the cut of the pictures between judges it:
   port/platform/gs.c). Behind at two vblanks in a row, the rate comes down by UNLOCKED_RATE_DOWN (twice that when it
   fell just before too: a place much heavier than the last), and is not judged again for UNLOCKED_RATE_SETTLE
   vblanks (the pictures already asked for drain). Kept up, it goes up by UNLOCKED_RATE_UP every UNLOCKED_RATE_REST
   vblanks to just under the rate it last fell from (UNLOCKED_RATE_SAFE of it), the renderer's limit as last found;
   past that only every `probe_rest` vblanks, which doubles each time such a probe falls behind (up to
   UNLOCKED_RATE_REST_MOST) and is UNLOCKED_RATE_REST again when one holds. So at the renderer's limit the rate
   probes past it seldom, and follows a lighter place up within a few probes.

   Behind is not the only sign: past its limit a renderer may leave pictures out, or show fewer of the game's
   vblanks, before its thread is a vblank late. So the caller also says how many pictures a second were presented
   (unlocked_rate_presented): under UNLOCKED_RATE_SHORT of the rate asked for, once UNLOCKED_RATE_MEASURE vblanks have
   passed since the rate last changed (the measure is of this rate), it falls as from behind, straight to
   UNLOCKED_RATE_UNDER of what was presented.

   The rate stays between floor_hz and ceiling_hz (the cap, and the most pictures a vblank the schedule takes).

   port/platform/unlocked_rate.c; no locks (the caller's: the controller's thread), no globals. */
#ifndef PORT_UNLOCKED_RATE_H
#define PORT_UNLOCKED_RATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNLOCKED_RATE_DOWN 0.85      /* the rate times this when behind */
#define UNLOCKED_RATE_UP 1.05        /* the rate times this after a rest kept up */
#define UNLOCKED_RATE_STEP_HZ 5.0    /* and up by at least this */
#define UNLOCKED_RATE_SETTLE 30      /* vblanks after a fall not judged */
#define UNLOCKED_RATE_REST 60        /* vblanks kept up before a rise */
#define UNLOCKED_RATE_REST_MOST 480  /* the longest rest before a probe: 8 s */
#define UNLOCKED_RATE_SAFE 0.95      /* rises without a probe's rest up to this much of the rate last fallen from */
#define UNLOCKED_RATE_SHORT 0.9      /* presented under this much of the rate asked: the renderer cannot keep it */
#define UNLOCKED_RATE_UNDER 0.95     /* and the rate then this much of what was presented */
#define UNLOCKED_RATE_MEASURE 45     /* vblanks after a change before the pictures presented are of the new rate */

typedef struct {
    double floor_hz, ceiling_hz; /* the rate's bounds */
    double rate_hz;              /* pictures a second now */
    double fell_from_hz;         /* the rate it last fell from (0: none yet) */
    int behind_run;              /* vblanks in a row behind */
    int probe_rest;              /* vblanks kept up before a rise past UNLOCKED_RATE_SAFE of fell_from_hz */
    uint64_t settle_until;       /* not judged before this vblank */
    uint64_t next_rise;          /* the next rise at or after this vblank */
    uint64_t probed_at;          /* the last probe's vblank (0: none since the last fall) */
    uint64_t fell_at;            /* the last fall's vblank (0: none yet) */
    uint64_t changed_at;         /* the last fall's or rise's vblank */
    uint64_t falls, rises;       /* counted, for --stats */
} UnlockedRate;

/* Starting at start_hz, kept between floor_hz and ceiling_hz; the first rise after a rest from vblank */
void unlocked_rate_init(UnlockedRate *rate, double start_hz, double floor_hz, double ceiling_hz, uint64_t vblank);
/* New bounds (the cap changed), the rate brought within them */
void unlocked_rate_bound(UnlockedRate *rate, double floor_hz, double ceiling_hz);
/* A game vblank judged: behind, 1 when the renderer was behind as it began */
void unlocked_rate_vblank(UnlockedRate *rate, uint64_t vblank, int behind);
/* A game vblank's measure of the pictures presented a second lately (0: not known) */
void unlocked_rate_presented(UnlockedRate *rate, uint64_t vblank, double presented_hz);
/* 1 at the floor: lower would be fewer pictures than the floor allows (the caller's own cut then judges) */
int unlocked_rate_at_floor(const UnlockedRate *rate);

#ifdef __cplusplus
}
#endif

#endif
