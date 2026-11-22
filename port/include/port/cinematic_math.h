/* The cinematic camera's arithmetic (game/cinematic.c), apart so it can be tested (tests/cinematic_test.c).
   Angles in radians as player_state.rotation keeps them: yaw wrapped to [-pi, pi], pitch held within the game's
   limit. */
#ifndef PORT_CINEMATIC_MATH_H
#define PORT_CINEMATIC_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

#define CINEMATIC_PITCH_LIMIT_RAD 1.012291 /* the game's (Player_Move, Interact_UpdateViewRay) */
#define CINEMATIC_MAX_STEP_S 0.1           /* a longer gap (a load, a pause) counts as this */

/* The share of the way to its target a first-order lag moves in step_s seconds, with time constant tau_s
   (1 for a tau of about 0: no lag) */
double cinematic_lag(double tau_s, double step_s);

/* b - a, the short way round */
double cinematic_turn_between(double a_rad, double b_rad);

/* An angle wrapped into [-pi, pi] */
double cinematic_wrap(double angle_rad);

/* The look filter: a target that takes the turns the game applied, and the smoothed view that trails it */
typedef struct {
    int primed;
    double yaw_to_go_rad;        /* the turn the view has still to make, unwrapped: a long spin is never cut short */
    double target_pitch_rad;
    double view_yaw_rad, view_pitch_rad; /* what was written back last: the next turn is measured from it */
} CinematicLook;

/* Takes the game's yaw and pitch after its turn this pass and gives the view's in them. The first call after
   cinematic_look_reset only takes the angles as they are. */
void cinematic_look_step(CinematicLook *look, double *yaw_rad, double *pitch_rad, double tau_s, double step_s);
void cinematic_look_reset(CinematicLook *look);

#ifdef __cplusplus
}
#endif

#endif
