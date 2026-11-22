/* cinematic.c: PC-only. The cinematic camera's flight (port/include/port/cinematic.h), from Verdite3's
   mods/kf3debug/Noclip.cs.

   The game's own Player_Move still runs each pass: it reads the turn and look (pad, mouse, twin-stick, whatever is
   bound) and walks the player with its collision. Then Cinematic_AfterMove throws the walk away and writes the
   flight's own position, kept in doubles here: it is the answer whatever the collision and the floor decided, and
   flying from what is in player_state would add each step to a floor-snapped y and hover instead of rising. The
   fall is held at nothing, so no landing, fall damage or fall death ever comes due (and port_invincible() is on while
   flying, for the rest).

   Input: forward and sideways are the game's own walk and strafe speeds as a share of its top speed (so a key, the
   d-pad and a stick all reach it), up and down the Fly up / Fly down keys, Run is faster. Forward goes where the
   view points, pitch and all, as in every noclip: looking down and pushing forward descends. Sideways stays level.

   Smoothing: the velocity eases toward what is asked (cinematic_lag), and the view trails the game's turn
   (cinematic_look_step), both against the wall clock so the feel is the same at 30 and 60 game frames a second.

   The camera: the game aims its camera at what the view ray hits (GameLoop_UpdateCamera), which inside a wall is
   right at the eye, and the angles jump. So while flying Cinematic_PlaceCamera aims it along the view's direction
   instead, as far as it likes. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "common.h"
#include "core/game.h"
#include "player/player.h"
#include "port/cinematic.h"
#include "port/cinematic_math.h"
#include "port/platform.h"

extern PlayerState player_state;
extern GameStatus game_status;
extern CameraState camera_state;
extern PlayerScreenTint player_screen_tint;
extern f32 player_top_speed[6];

void Player_FindMapRecord(void);
const char *Quick_WhyNotInPlay(void);

static struct {
    double x, y, z;    /* the flight's own position */
    double vx, vy, vz; /* its velocity, units a second */
    double last_s;     /* the wall clock at the last pass */
    CinematicLook look;
} fly;

static void start(void) {
    fly.x = player_state.position.x;
    fly.y = player_state.position.y;
    fly.z = player_state.position.z;
    fly.vx = fly.vy = fly.vz = 0.0;
    fly.last_s = port_wall_seconds();
    cinematic_look_reset(&fly.look);
    port_cinematic_set_flying(1);
    fprintf(stderr, "[cinematic] flying from (%.0f, %.0f, %.0f)\n", fly.x, fly.y, fly.z);
}

static void land(void) {
    port_cinematic_set_flying(0);
    fprintf(stderr, "[cinematic] landed at (%.0f, %.0f, %.0f), map record %d\n", player_state.position.x,
            player_state.position.y, player_state.position.z, player_state.map_index);
    player_state.fall_speed = 0.0f;
    player_state.airborne_start_y = player_state.position.y; /* a fall from here is measured from here */
}

/* Seconds since the last pass, at most CINEMATIC_MAX_STEP_S (a load or a pause would fling the flight) */
static double step_seconds(void) {
    double now = port_wall_seconds();
    double step = now - fly.last_s;

    fly.last_s = now;
    if (!(step > 0.0)) {
        return 0.0;
    }
    return step > CINEMATIC_MAX_STEP_S ? CINEMATIC_MAX_STEP_S : step;
}

static double share_of_top(f32 speed) {
    double top = player_top_speed[0];
    double share = top > 0.0 ? speed / top : 0.0;

    return share > 1.0 ? 1.0 : share < -1.0 ? -1.0 : share;
}

/* The velocity asked for, units a second (y grows downward) */
static void asked_velocity(double *vx, double *vy, double *vz) {
    double forward = share_of_top(player_state.walk_speed);
    double sideways = share_of_top(player_state.strafe_speed);
    double up = port_cinematic_vertical();
    double rate = port_cinematic_speed() * (port_cinematic_fast() ? PORT_CINEMATIC_FAST_MULTIPLIER : 1.0);
    double yaw = player_state.rotation.y;
    double pitch = player_state.rotation.x;
    double level = cos(pitch);

    /* forward is the view's direction, RotY(yaw) RotX(pitch) of +z (Interact_UpdateViewRay, libvu0's rotations):
       (sin yaw cos pitch, -sin pitch, cos yaw cos pitch); sideways a quarter turn right of it, level */
    *vx = (sin(yaw) * forward * level + cos(yaw) * sideways) * rate;
    *vz = (cos(yaw) * forward * level - sin(yaw) * sideways) * rate;
    *vy = (-sin(pitch) * forward - up) * rate;
}

/* The game's fall, landing and walk held down: nothing comes due when the flight ends */
static void hold_still(void) {
    player_state.fall_speed = 0.0f;
    player_state.airborne_start_y = player_state.position.y;
    player_state.landing_dip_offset = 0.0f;
    player_state.landing_dip_frames = 0;
    player_state.landing_dip_frame = 0;
    player_state.floor_push.x = player_state.floor_push.y = player_state.floor_push.z = 0.0f;
    player_state.aim_speed = 0.0f;
    player_state.view_tilt_frames = 0;
    player_state.view_tilt_pitch = 0.0f;
    player_state.view_tilt_roll = 0.0f;
    player_state.stride_phase = 0; /* no footsteps (and no head bob) */
}

static void fly_pass(void) {
    double step = step_seconds();
    double yaw = player_state.rotation.y;
    double pitch = player_state.rotation.x;
    double tx, ty, tz;
    double k;

    cinematic_look_step(&fly.look, &yaw, &pitch, port_cinematic_look_smoothing_s(), step);
    player_state.rotation.y = (f32)yaw;
    player_state.rotation.x = (f32)pitch;

    asked_velocity(&tx, &ty, &tz);
    k = cinematic_lag(port_cinematic_move_smoothing_s(), step);
    fly.vx += (tx - fly.vx) * k;
    fly.vy += (ty - fly.vy) * k;
    fly.vz += (tz - fly.vz) * k;
    fly.x += fly.vx * step;
    fly.y += fly.vy * step;
    fly.z += fly.vz * step;

    player_state.position.x = (f32)fly.x;
    player_state.position.y = (f32)fly.y;
    player_state.position.z = (f32)fly.z;
    hold_still();
    Player_FindMapRecord(); /* the map record under the flight: the next maps stream from it */
}

void Cinematic_AfterMove(void) {
    int flying = port_cinematic_flying();

    if (port_cinematic_take_toggle()) {
        if (flying) {
            land();
        } else if (Quick_WhyNotInPlay() == NULL) {
            start();
        } else {
            fprintf(stderr, "[cinematic] not flying: %s\n", Quick_WhyNotInPlay());
        }
        flying = port_cinematic_flying();
    }
    if (flying && (!port_cinematic_enabled() || game_status.bit9)) {
        land(); /* the cheat switched off, or play is ending */
        return;
    }
    if (flying) {
        fly_pass();
    }
}

void Cinematic_PlaceCamera(void) {
    if (!port_cinematic_flying() || game_status.camera_mode != 0) {
        return;
    }
    /* mode 0's eye (without the bob and dip, which the flight holds at nothing), aimed along the view's direction:
       GameLoop_UpdateCamera's angles of a point on it, atan2(-dy, level) and atan2(dx, dz), are the view's own */
    camera_state.position[0] = player_state.position.x;
    camera_state.position[1] = player_state.position.y - 220.0f + camera_state.height_offset;
    camera_state.position[2] = player_state.position.z;
    camera_state.pitch = player_state.rotation.x;
    camera_state.yaw = player_state.rotation.y;
    camera_state.roll = 0.0f;
    player_state.head_bob_offset = 0.0f;
}

/* What Cinematic_HideScreenEffects took off the picture, for Cinematic_ShowScreenEffects to put back */
static struct {
    int hidden;
    PlayerScreenTint tint;
    s32 mp_drain_tint;
    f32 aim_speed;
} kept_effects;

void Cinematic_HideScreenEffects(void) {
    if (!port_cinematic_flying()) {
        return;
    }
    kept_effects.hidden = 1;
    kept_effects.tint = player_screen_tint;
    kept_effects.mp_drain_tint = player_state.mp_drain_tint;
    kept_effects.aim_speed = player_state.aim_speed;
    /* Player_DrawScreenTints' fills, but the light spell's glow (a light, not something done to the player) */
    player_screen_tint.damage_flash_alpha = 0;
    player_screen_tint.green_tint_alpha = 0;
    player_screen_tint.dark_yellow_tint_alpha = 0;
    player_screen_tint.darkness_tint_alpha = 0;
    player_screen_tint.purple_tint_alpha = 0;
    player_screen_tint.white_tint_alpha = 0;
    player_screen_tint.cycling_tint_alpha = 0;
    player_screen_tint.death_fade_tint_alpha = 0;
    player_state.mp_drain_tint = 0; /* the drain's wobbling screen */
    player_state.aim_speed = 0.0f;  /* a hit's knock-back: the frame before blended in */
}

void Cinematic_ShowScreenEffects(void) {
    if (!kept_effects.hidden) {
        return;
    }
    kept_effects.hidden = 0;
    player_screen_tint = kept_effects.tint;
    player_state.mp_drain_tint = kept_effects.mp_drain_tint;
    player_state.aim_speed = kept_effects.aim_speed;
}
