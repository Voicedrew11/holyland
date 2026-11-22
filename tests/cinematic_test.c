/* The cinematic camera's arithmetic (port/platform/cinematic_math.c): the lag the same at any rate, turns the short
   way round, and a look filter that trails the turn without losing any of it or overshooting the pitch limit. */
#include <math.h>
#include <stdio.h>

#include "port/cinematic_math.h"

#define PI 3.14159265358979323846

static int failures;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        if (!(condition)) {                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                    \
            failures++;                                                                                        \
        }                                                                                                      \
    } while (0)

static int near(double a, double b, double tolerance) {
    return fabs(a - b) <= tolerance;
}

static void test_lag_same_at_any_rate(void) {
    double at_30 = 0.0, at_240 = 0.0;
    int i;

    for (i = 0; i < 30; i++) {
        at_30 += (1.0 - at_30) * cinematic_lag(0.35, 1.0 / 30.0);
    }
    for (i = 0; i < 240; i++) {
        at_240 += (1.0 - at_240) * cinematic_lag(0.35, 1.0 / 240.0);
    }
    CHECK(near(at_30, at_240, 1e-9));
    CHECK(near(at_30, 1.0 - exp(-1.0 / 0.35), 1e-9));
    CHECK(cinematic_lag(0.0, 0.01) == 1.0); /* no lag */
    CHECK(cinematic_lag(0.35, 0.0) == 0.0);
}

static void test_turn_short_way(void) {
    CHECK(near(cinematic_turn_between(PI - 0.1, -PI + 0.1), 0.2, 1e-12));
    CHECK(near(cinematic_turn_between(-PI + 0.1, PI - 0.1), -0.2, 1e-12));
    CHECK(near(cinematic_wrap(3.0 * PI + 0.5), -PI + 0.5, 1e-12));
    CHECK(near(cinematic_wrap(-0.25), -0.25, 1e-12));
}

/* The game turns 0.03 radians a pass for 200 passes (nearly a turn), across the wrap; the view gets there in full */
static void test_look_trails_and_arrives(void) {
    CinematicLook look;
    double game_yaw = 3.0, game_pitch = 0.0, yaw, pitch;
    double lagged = 0.0;
    int i;

    cinematic_look_reset(&look);
    yaw = game_yaw;
    pitch = game_pitch;
    cinematic_look_step(&look, &yaw, &pitch, 0.25, 1.0 / 60.0);
    CHECK(yaw == game_yaw && pitch == game_pitch); /* primed: as it was */
    for (i = 0; i < 200; i++) {
        game_yaw = cinematic_wrap(yaw + 0.03); /* the game turns on from what was written back */
        yaw = game_yaw;
        pitch = 0.0;
        cinematic_look_step(&look, &yaw, &pitch, 0.25, 1.0 / 60.0);
        lagged += cinematic_turn_between(yaw, game_yaw);
    }
    CHECK(lagged > 0.0); /* it trailed */
    for (i = 0; i < 600; i++) {
        cinematic_look_step(&look, &yaw, &pitch, 0.25, 1.0 / 60.0); /* the game turns no more */
    }
    CHECK(near(yaw, cinematic_wrap(3.0 + 200 * 0.03), 1e-6)); /* arrived: none of the turn lost */
    CHECK(yaw >= -PI && yaw <= PI);
}

/* Held at the limit, the target does not run away past it, so letting go does not swing the view back */
static void test_pitch_held_at_limit(void) {
    CinematicLook look;
    double yaw = 0.0, pitch = 0.0;
    int i;

    cinematic_look_reset(&look);
    cinematic_look_step(&look, &yaw, &pitch, 0.25, 1.0 / 60.0);
    for (i = 0; i < 300; i++) {
        pitch = pitch + 0.05;
        if (pitch > CINEMATIC_PITCH_LIMIT_RAD) {
            pitch = CINEMATIC_PITCH_LIMIT_RAD; /* as the game clamps */
        }
        cinematic_look_step(&look, &yaw, &pitch, 0.25, 1.0 / 60.0);
        CHECK(pitch <= CINEMATIC_PITCH_LIMIT_RAD);
    }
    CHECK(near(pitch, CINEMATIC_PITCH_LIMIT_RAD, 1e-3));
    for (i = 0; i < 60; i++) {
        cinematic_look_step(&look, &yaw, &pitch, 0.25, 1.0 / 60.0); /* let go */
    }
    CHECK(near(pitch, CINEMATIC_PITCH_LIMIT_RAD, 1e-3));
}

int main(void) {
    test_lag_same_at_any_rate();
    test_turn_short_way();
    test_look_trails_and_arrives();
    test_pitch_held_at_limit();
    if (failures != 0) {
        fprintf(stderr, "cinematic_test: %d failed\n", failures);
        return 1;
    }
    printf("cinematic_test: ok\n");
    return 0;
}
