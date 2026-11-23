/* The keyboard's walking (port/sdk/walk_keys.c): W A S D (the walk actions, ACTION_LEFT_STICK_UP to _RIGHT in
   port/include/port/controls.h, whatever they are bound to) are the game's own walking buttons, not a stick. In
   play they hold the buttons the game walks forward and back and steps to each side with (forward_button,
   back_button, step_left_button, step_right_button: Input_SetDefaultControls's d-pad up and down and L1 / R1, or
   what the game's controls option made them), so the game eases the walk as it always has, the run (the action
   button or the Run key) and the ladders read them as the d-pad's, and W + A walks the diagonal. Out of
   play they are the d-pad (the menus' cursor). Twin-stick controls (port/include/port/twin_stick.h) change only
   a controller's sticks: the keys are the same with it on or off, and they set no stick, so a controller's stick
   at rest takes nothing from them.

   Instant WASD acceleration (the player menu's Controls page, settings.json "controls": "instant_wasd_accel", on
   by default; not in the first-start setup) makes the keys the left stick at full throw instead, which twin-stick
   controls walk at the top speed at once and turn round at once. With twin-stick off that stick is the PS2's
   (the d-pad: A and D turn). Never while a pad script plays or one is recorded: those keep the eased buttons, so
   the scripts recorded before stay the same. Off, live play walks eased as above (Sean's, 696bf0f0). */
#ifndef PORT_WALK_KEYS_H
#define PORT_WALK_KEYS_H

#include "port/twin_stick.h"

/* The walk actions held, as bits */
#define WALK_KEY_FORWARD 1u
#define WALK_KEY_BACK 2u
#define WALK_KEY_LEFT 4u
#define WALK_KEY_RIGHT 8u

/* (Player_Update, each frame) The game's buttons for moving, which the keys hold in play. Until the game first
   says, Input_SetDefaultControls's. */
void port_walk_keys_set_buttons(MoveButtons buttons);
MoveButtons port_walk_keys_buttons(void);

/* Instant WASD acceleration: the setting (on until set), and whether it applies now (on, no pad script, no
   recording) */
int port_walk_keys_instant_setting(void);
void port_walk_keys_set_instant(int on);
int port_walk_keys_instant(void);

/* The held walk keys (WALK_KEY_) into the pad: returns the buttons they hold (in play, `gameplay`, the game's
   `buttons`: forward, back, step_left, step_right; out of play the d-pad's up, down, left and right), or with
   `instant` none, the left stick (`axis` 2 and 3, the pad's left x and y) set to full throw instead: 0x00 up or
   left, 0xFF down or right, 0x80 for neither or both. Without `instant` `axis` is left as it is. */
unsigned short walk_keys_read(unsigned int keys, int gameplay, MoveButtons buttons, int instant, int axis[4]);

#endif
