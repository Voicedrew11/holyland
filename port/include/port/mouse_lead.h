/* Instant mouse look: with --frame-rate 60 or more, each picture between shows the mouse's turn by the vblank it
   was read, not when the game's frame that spends it is shown.

   Without it, a turn of the mouse reaches the screen late: it is read at a vblank, Player_Move spends it at the
   game's next pass (30 a second), that pass's packet chain is sent at the pass after, and the pictures between then
   move the camera to it over two more vblanks. With it, every picture between (port/platform/frame_interp.c)
   is drawn with its camera turned on by what the hand has moved that the camera between does not show yet:

       the hand - (spent at the frame shown, A, mixed t of the way to the spent at the frame drawn, B)

   "Spent" is the mouse turn the game had added when it built that frame's camera (port_mouse_lead_spent_total,
   carried in the frame's camera mark: PORT_FRAME_INTERP_MOUSE_YAW and _PITCH); "the hand" is that and the motion
   read and not yet spent, read once a vblank. Both are kept wrapped to [-pi, pi), so they keep their precision in
   a long session; their differences are wrapped too.

   Each picture is given the hand at its own time, MOUSE_LEAD_DELAY_VBLANKS before it, mixed between the two readings
   about it: the vblank's pictures between are asked for before their time (a vblank before, as gs.c gets the vblank),
   so the hand at their own time is not read yet. Mixed between readings, not taken from the newest, the turn moves on
   in every picture; and not carried on past it, it never overshoots where the hand stopped. The turn is put onto the
   view as the game builds it (GameLoop_SetCameraMatrices: RotX(-pitch) RotY(-yaw) from the eye), pitch held within
   the game's limit.

   Then, as the renderer draws the picture, it turns it on again by what the hand has moved since
   (port_mouse_lead_since, mouse_lead_turn_picture), to the hand a steady time before the picture is shown
   (port/gs/pc/pcr_late.h's TurnLead): the readings are taken every half millisecond as the game's thread waits and
   works (port_controls_sample_mouse), not once a vblank. So the mouse's turn is on the screen a few milliseconds
   after the hand's, as in a game drawn at the screen's rate, rather than the two vblanks and the window's lead later
   that the pictures are asked for.

   Verdite2's FrameSmoothing.MouseLead and Verdite3's ViewSmoothing.Lead ("Instant mouse look"), done here on the
   camera of the pictures between.

   port/platform/mouse_lead.c. The spent total is the game's thread's; the readings are shared with the DMA
   controller's thread behind a lock. */
#ifndef PORT_MOUSE_LEAD_H
#define PORT_MOUSE_LEAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOUSE_LEAD_PITCH_LIMIT_RAD 1.012291 /* the game's: Player_Move and Interact_UpdateViewRay hold the pitch in it */
#define MOUSE_LEAD_DELAY_VBLANKS 2.0        /* a picture shows the hand this long before its time */
#define MOUSE_LEAD_READINGS 512             /* readings kept: about 32 vblanks at one each half millisecond */

typedef struct {
    double yaw_rad;   /* added to the camera's yaw (player_state.rotation.y's way) */
    double pitch_rad; /* added to its pitch (rotation.x's way) */
} MouseTurn;

/* ---- The game's thread (port/sdk/controls.c, game/frame_interp.c) ---- */

/* port_mouse_look: the turn Player_Move adds this pass, out of what was waiting (the hand is where it was) */
void port_mouse_lead_spent(MouseTurn spent);
/* After each read of the mouse, at_vblanks the time of the read in vblanks (its vblank and how far into it): the
   motion read and not yet spent (zero outside play). A reading no later than the one before replaces it; one more than
   a vblank before it starts the readings again (a save state loaded). */
void port_mouse_lead_waiting(MouseTurn waiting, double at_vblanks);
/* All the turn spent so far, wrapped: the frame's camera mark carries it */
MouseTurn port_mouse_lead_spent_total(void);

/* ---- The DMA controller's thread (port/platform/frame_interp.c) ---- */

/* The turn to add to the picture t of the way (0 to 1) from the frame shown to the frame drawn, given what each
   had spent; the picture's time in vblanks (its vblank and its phase in it) */
MouseTurn port_mouse_lead_ahead(MouseTurn spent_shown, MouseTurn spent_drawn, double t, double picture_vblank);
/* The hand a picture at picture_vblank is given (MOUSE_LEAD_DELAY_VBLANKS before it): what the picture's turn is from */
MouseTurn port_mouse_lead_hand(double picture_vblank);

/* ---- Any thread (the renderer's, as it draws a picture) ---- */

/* What the hand has moved since `hand` to its place at at_vblanks (mixed between the readings about it; the newest
   reading's past it), wrapped; none before the first reading */
MouseTurn port_mouse_lead_since(MouseTurn hand, double at_vblanks);
/* When the newest reading was read, in vblanks (-1: none yet) */
double port_mouse_lead_newest_vblanks(void);
/* A picture between's cameras (gsn.h's GsnInterpPicture: move, the frame's view to the picture's, rows of the rotation
   less the identity, each with its translation; mid, the world to the picture's view, rows of the rotation and the
   translation) turned on by `turn` about the eye, as mouse_lead_turn_view turns a view */
void mouse_lead_turn_picture(float move[12], float mid[12], MouseTurn turn);

/* r (world to view, r[row][col], as frame_interp.c's Camera has it) turned on by `turn`: RotX(-dp) r RotY(-dy), the
   pitch held within MOUSE_LEAD_PITCH_LIMIT_RAD (or where it already is, past it). The caller keeps the eye. */
void mouse_lead_turn_view(double r[3][3], MouseTurn turn);

#ifdef __cplusplus
}
#endif

#endif
