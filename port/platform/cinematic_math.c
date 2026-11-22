/* The cinematic camera's arithmetic (port/include/port/cinematic_math.h), ported from Verdite3's
   mods/kf3debug/Noclip.cs (Lag, SmoothLook) to radians. */
#include <math.h>

#include "port/cinematic_math.h"

#define PI 3.14159265358979323846

double cinematic_lag(double tau_s, double step_s) {
    if (tau_s <= 1e-4) {
        return 1.0;
    }
    if (step_s <= 0.0) {
        return 0.0;
    }
    return 1.0 - exp(-step_s / tau_s);
}

double cinematic_wrap(double angle_rad) {
    double wrapped = fmod(angle_rad + PI, 2.0 * PI);

    if (wrapped < 0.0) {
        wrapped += 2.0 * PI;
    }
    return wrapped - PI;
}

double cinematic_turn_between(double a_rad, double b_rad) {
    return cinematic_wrap(b_rad - a_rad);
}

static double clamp_pitch(double pitch_rad) {
    if (pitch_rad > CINEMATIC_PITCH_LIMIT_RAD) {
        return CINEMATIC_PITCH_LIMIT_RAD;
    }
    if (pitch_rad < -CINEMATIC_PITCH_LIMIT_RAD) {
        return -CINEMATIC_PITCH_LIMIT_RAD;
    }
    return pitch_rad;
}

void cinematic_look_reset(CinematicLook *look) {
    look->primed = 0;
}

/* The game turned from what was written back last (view_*) to *yaw_rad, *pitch_rad: that turn is added to what is
   still to go, and the view makes its share of it. So the view trails the turn but never loses any of it. The target
   pitch is held in the game's limit, or holding look up at the limit would run it away and the view would overshoot
   when let go. */
void cinematic_look_step(CinematicLook *look, double *yaw_rad, double *pitch_rad, double tau_s, double step_s) {
    double k;
    double yaw_step;

    if (!look->primed) {
        look->yaw_to_go_rad = 0.0;
        look->view_yaw_rad = *yaw_rad;
        look->target_pitch_rad = look->view_pitch_rad = *pitch_rad;
        look->primed = 1;
        return;
    }
    look->yaw_to_go_rad += cinematic_turn_between(look->view_yaw_rad, *yaw_rad);
    look->target_pitch_rad = clamp_pitch(look->target_pitch_rad + (*pitch_rad - look->view_pitch_rad));

    k = cinematic_lag(tau_s, step_s);
    yaw_step = look->yaw_to_go_rad * k;
    look->yaw_to_go_rad -= yaw_step;
    look->view_yaw_rad = cinematic_wrap(look->view_yaw_rad + yaw_step);
    look->view_pitch_rad = clamp_pitch(look->view_pitch_rad + (look->target_pitch_rad - look->view_pitch_rad) * k);

    *yaw_rad = look->view_yaw_rad;
    *pitch_rad = look->view_pitch_rad;
}
