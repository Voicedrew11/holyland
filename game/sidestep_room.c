/* sidestep_room.c: the Sidestep Room setting's sums (port/sidestep_room.h). */
#include "port/sidestep_room.h"

#include <math.h>

#define ROOM_SAMPLES 64    /* distances tried between the enemy and the hit's reach */
#define SPEEDUP_STEPS 20   /* halvings of the speed-up's range */
#define PI_F 3.1415927f
#define TAU_F 6.2831855f

static float wrap_angle(float angle) {
    angle -= (float)(int)(angle / TAU_F) * TAU_F;
    if (angle > PI_F) {
        return angle - TAU_F;
    }
    if (angle < -PI_F) {
        return angle + TAU_F;
    }
    return angle;
}

/* one frame of EntityAi_TurnToHeading towards target: the heading and turn speed after it */
static void turn_towards(float target, float turn_step, float turn_limit, float *heading, float *turn_speed) {
    float diff = wrap_angle(target - *heading);

    if (diff > 0.0f) {
        *turn_speed = fminf(*turn_speed + turn_step, turn_limit);
        if (diff <= *turn_speed) {
            *heading += diff;
            *turn_speed = 0.0f;
            return;
        }
        *heading += *turn_speed;
    } else if (diff < 0.0f) {
        *turn_speed = fmaxf(*turn_speed - turn_step, -turn_limit);
        if (*turn_speed <= diff) {
            *heading += diff;
            *turn_speed = 0.0f;
            return;
        }
        *heading += *turn_speed;
    } else {
        *turn_speed = 0.0f;
    }
}

/* 1 when a player distance in front of the enemy, sidestepping, is out of the hit's arc when it lands */
static int sidestep_gets_out(const SidestepAttack *attack, const SidestepPlayer *player, float speedup, float distance) {
    float turn_step = attack->turn_step * speedup * speedup;
    float turn_limit = attack->turn_limit * speedup;
    float across = 0.0f;
    float speed = 0.0f;
    float heading = 0.0f;
    float turn_speed = 0.0f;

    for (int frame = 0; frame < attack->strike_frames; frame++) {
        if (frame >= player->reaction_frames) {
            speed = fminf(speed + player->speed_gain, player->top_speed);
        }
        across += speed;
        if (frame < attack->turn_frames) {
            turn_towards(atan2f(across, distance), turn_step, turn_limit, &heading, &turn_speed);
        }
    }
    return fabsf(wrap_angle(atan2f(across, distance) - heading)) > attack->half_arc;
}

float sidestep_room(const SidestepAttack *attack, const SidestepPlayer *player, float speedup) {
    int out = 0;

    if (attack->reach <= 0.0f) {
        return 0.0f;
    }
    /* (counted over the distances, not searched for: nearer is not always easier once the enemy catches up) */
    for (int sample = 1; sample <= ROOM_SAMPLES; sample++) {
        out += sidestep_gets_out(attack, player, speedup, attack->reach * (float)sample / ROOM_SAMPLES);
    }
    return attack->reach * (float)out / ROOM_SAMPLES;
}

float sidestep_speedup(const SidestepAttack *attack, const SidestepPlayer *player, float room_share) {
    float target;
    float low = 1.0f;
    float high = SIDESTEP_MAX_SPEEDUP;
    float ps2_room;

    if (room_share >= 1.0f) {
        return 1.0f;
    }
    ps2_room = sidestep_room(attack, player, 1.0f);
    if (ps2_room <= 0.0f) {
        return 1.0f;
    }
    target = ps2_room * fmaxf(room_share, 0.0f);
    if (sidestep_room(attack, player, high) > target) {
        return high;
    }
    for (int step = 0; step < SPEEDUP_STEPS; step++) {
        float middle = 0.5f * (low + high);

        if (sidestep_room(attack, player, middle) > target) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return high;
}
