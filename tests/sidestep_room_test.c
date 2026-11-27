/* The Sidestep Room setting's sums (game/sidestep_room.c): the room a controller sidestep has against an attack, and
   the speed-up that leaves a share of it. The attacks are chr.prm's (King's Field IV's own numbers). */
#include <math.h>
#include <stdio.h>

#include "port/sidestep_room.h"

static int failures;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        if (!(condition)) {                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                    \
            failures++;                                                                                        \
        }                                                                                                      \
    } while (0)

#define DEGREES 0.017453292f

/* the player on foot: 12 a frame at the top, 1.2 gained a frame; stepping 6 frames in */
static const SidestepPlayer player = {12.0f, 1.2f, 6};

int main(void) {
    /* record 9's first melee attack: 2.3 degrees a frame at the top, 15 frames of turning, a 15-degree half arc */
    const SidestepAttack jab = {0.0401f, 0.0017f, 15, 15, 15.0f * DEGREES, 150.0f};
    /* record 72's first: a slow, long turn (1.8 degrees, 17 frames) */
    const SidestepAttack swing = {0.0314f, 0.0087f, 17, 22, 15.0f * DEGREES, 250.0f};
    /* an attack whose arc is so wide that no sidestep gets out of it */
    const SidestepAttack sweep = {0.0349f, 0.0035f, 10, 10, 90.0f * DEGREES, 200.0f};
    /* a type that never turns */
    const SidestepAttack statue = {0.0f, 0.0f, 15, 15, 15.0f * DEGREES, 250.0f};

    /* the PS2 leaves some room, never more than the reach */
    float jab_room = sidestep_room(&jab, &player, 1.0f);
    CHECK(jab_room > 0.0f && jab_room <= jab.reach);

    /* turning faster leaves less */
    CHECK(sidestep_room(&jab, &player, 2.0f) < jab_room);
    CHECK(sidestep_room(&swing, &player, 2.0f) < sidestep_room(&swing, &player, 1.0f));

    /* the speed-up leaves the share asked, or less, and a smaller share asks a bigger one */
    float half = sidestep_speedup(&jab, &player, 0.5f);
    float quarter = sidestep_speedup(&jab, &player, 0.25f);
    CHECK(half > 1.0f && half <= SIDESTEP_MAX_SPEEDUP);
    CHECK(sidestep_room(&jab, &player, half) <= 0.5f * jab_room);
    CHECK(sidestep_room(&jab, &player, half * 0.9f) > 0.5f * jab_room); /* (and not much more than it needs) */
    CHECK(quarter >= half);

    /* the whole room is the PS2's */
    CHECK(sidestep_speedup(&jab, &player, 1.0f) == 1.0f);

    /* no room on the PS2: nothing to take away */
    CHECK(sidestep_room(&sweep, &player, 1.0f) == 0.0f);
    CHECK(sidestep_speedup(&sweep, &player, 0.5f) == 1.0f);

    /* a player who steps after the hit has no room */
    const SidestepPlayer late = {12.0f, 1.2f, 30};
    CHECK(sidestep_room(&jab, &late, 1.0f) == 0.0f);

    /* turning faster cannot help a type that never turns: the most is given */
    CHECK(sidestep_speedup(&statue, &player, 0.5f) == SIDESTEP_MAX_SPEEDUP);

    if (failures == 0) {
        printf("sidestep_room: all passed (jab: room %.0f of %.0f, x%.2f for half, x%.2f for a quarter)\n",
               (double)jab_room, (double)jab.reach, (double)half, (double)quarter);
    }
    return failures != 0;
}
