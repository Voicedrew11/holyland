/* The cameras of the pictures between (port/platform/frame_interp.c): the camera t of the way from one game frame's to
   the next, and whether the two are a cut apart (not interpolated: the vblank shows the frame before, as on the PS2).

   A cut is a camera that jumped: a move of more than INTERP_MAX_MOVE, or a turn of more than INTERP_MAX_TURN besides
   what the mouse turned it. A fast spin of the mouse is not a cut: the game turns the player by up to a quarter turn
   a frame for it (port/sdk/controls.c's MAX_TURN), and taken for a cut, a spin of more than 46 degrees a frame
   (1,400 a second) had no pictures between, the game's 30 frames a second and no instant mouse look. The turn the
   mouse made is what the game had spent of it at each frame (port/include/port/mouse_lead.h), put onto the first
   camera as the game builds its view; what is left is the camera's own.

   port/platform/interp_camera.c. No state: any thread. */
#ifndef PORT_INTERP_CAMERA_H
#define PORT_INTERP_CAMERA_H

#include "port/mouse_lead.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INTERP_MAX_TURN 0.8   /* radians between two frames, besides the mouse's turn: more is a cut */
#define INTERP_MOST_TURN 2.6  /* radians between two frames in all, the mouse's turn too: more is a cut (the way
                                 round is not sure near a half turn) */
#define INTERP_MAX_MOVE 400.0 /* world units between two frames: more is a cut */

typedef struct {
    double r[3][3]; /* world to view: rotation (row, column) */
    double t[3];    /* and translation */
    double eye[3];  /* the camera's place in the world */
} InterpCamera;

/* Cameras a and b (a frame's and the next one's), with the mouse turn the game had spent at each, are a cut apart */
int interp_cameras_cut(const InterpCamera *a, MouseTurn mouse_a, const InterpCamera *b, MouseTurn mouse_b);

/* The camera a fraction f (0 to 1) of the way from a to b, two cameras not a cut apart: the rotation by slerp, the
   eye along the line */
InterpCamera interp_camera_between(const InterpCamera *a, const InterpCamera *b, double f);

/* How far rotation b is turned from rotation a, in radians (0 to pi) */
double interp_turn_between(const double a[3][3], const double b[3][3]);

#ifdef __cplusplus
}
#endif

#endif
