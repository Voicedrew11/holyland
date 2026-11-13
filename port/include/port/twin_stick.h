/* Modern twin-stick controls (port/sdk/twin_stick.c), ported from Verdite1 (the KF1 port's
   patches/Analog.cs): the left stick walks and steps sideways, the right stick turns and looks. On the
   PS2 the left stick is the d-pad (it walks and turns) and the right stick only looks up and down, each
   all or nothing past half its throw (Input_ReadPort).

   The speeds stay the game's: a stick at full throw walks, turns and looks at the top speeds the
   d-pad and L2 / R2 reach (Player_Move's top_speed, top_turn, top_look: weight, ailments, water and
   running all still apply), and less in proportion. Turning and looking are direct, with no easing: the
   stick asks for a speed and gets it, and the view stops when the stick is let go. Pushing the right
   stick up looks up (the PS2's right stick is the other way round). Walking is direct too; released,
   the game slows the player as it always has. With twin-stick off (the PS2's controls, Verdite1's
   "3D") the game's own easing is untouched.

   The game sees the sticks as its own buttons, so all that reads them still works: running (the action
   button with a direction), the ladders, the masks that stop the player moving. Only the size of each
   speed is the stick's.

   On by default (settings.json "controls": "twin_stick"; the overlay's Controls page). Off while a pad
   script plays or one is recorded, so runs stay the same (KF4_TWIN_STICK_SCRIPTED=1 keeps it on, to test
   it: tests/twin_stick.pad). */
#ifndef PORT_TWIN_STICK_H
#define PORT_TWIN_STICK_H

/* The four axis bytes of the pad's report, 0x80 at rest: right x, right y, left x, left y (up and left
   are the low values) */
typedef struct {
    unsigned char right_x, right_y, left_x, left_y;
} PadSticks;

/* The game's buttons for moving (forward_button to look_up_button: Input_SetDefaultControls, and the
   game's own controls option), as pad bits */
typedef struct {
    unsigned short forward, back, turn_left, turn_right, step_left, step_right, look_up, look_down;
} MoveButtons;

/* The player's four speeds a frame (PlayerState): walk (+ forward), strafe (+ right), turn (radians,
   + right), look (radians, + the way look_down_button moves it: up on the screen) */
typedef struct {
    float walk, strafe, turn, look;
} PlayerSpeeds;

/* This frame's limits (Player_Move's locals) */
typedef struct {
    float top_speed, top_turn, top_look;
} SpeedLimits;

/* 1 while twin-stick controls apply: the setting is on and no pad script plays or is recorded */
int port_twin_stick_on(void);
int port_twin_stick_setting(void);
void port_twin_stick_set(int on);

/* (Player_Update) The held buttons with the sticks' own (the d-pad and L2 / R2 bits Input_ReadPort
   added from them: what `held` has that `pad_held`, the pad's report, has not) put back as twin-stick
   ones: the left stick forward, back and the two side steps, the right stick the two turns and the
   two looks (up: look_down_button), each from out of its deadzone. */
unsigned short twin_stick_held(unsigned short held, unsigned short pad_held, PadSticks sticks, MoveButtons buttons);

/* (Player_Move, after the buttons have moved the speeds to `after`) The speeds with each axis the
   sticks drive set by them: an axis is driven while its stick is out of the deadzone and `buttons`,
   the held buttons as Player_Move has them, still has its bits (the game's masks took nothing away).
   A turn or look the stick drove last call and nothing asks for now is stopped (0). The rest are
   `after`. Remembers which of the two the stick drove (port_twin_stick_state keeps it). */
PlayerSpeeds twin_stick_speeds(PlayerSpeeds after, PadSticks sticks, SpeedLimits limits, MoveButtons move,
                               unsigned short buttons);

/* Save states: which of the turn and look the stick drove */
typedef struct PortStateIo PortStateIo;
void port_twin_stick_state(PortStateIo *io);

#endif
