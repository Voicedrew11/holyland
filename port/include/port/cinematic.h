/* The trailer's cheats (the player menu's Cheats tab): the cinematic camera and enemies that ignore you.

   The cinematic camera is noclip flight for filming, from Verdite3's KF3 debug mod (mods/kf3debug/Noclip.cs): with
   the cheat on, its key (V, or a controller's spare button: the DualSense's mute key) starts and stops the flight.
   Flying, the player goes through walls and floors where the camera points (looking down and pushing forward
   descends), Fly up / Fly down rise and sink, and Run flies faster. The speed eases toward what is asked and coasts
   down when let go, and the view trails the turn (first-order lags, seconds to about two thirds of the way, spent
   against the wall clock so the feel is the same at any rate). No notice is shown, so nothing lands in the shot.
   While flying the player can't be hurt or die (port_invincible), the screen shows no conditions or damage, and
   instant mouse look is held off (its lead and
   the look's smoothing together judder; Verdite3 aeb5030).

   Enemies ignore you: enemies never notice the player, and one that is hit goes back to idle
   (EntityAi_SensePlayer, EntityAi_StepState). Enemies with their own AI may still act.

   Neither is saved: both are off again at the next start, like Invincible. Everything here runs on the game's
   thread (the host's keys are read at the vblank, which is on it too: port/platform/fiber.c).

   port/platform/cinematic.c keeps the switches; game/cinematic.c flies. */
#ifndef PORT_CINEMATIC_H
#define PORT_CINEMATIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The switches (the player menu, the overlay) ----------------------------------------------- */
int port_cinematic_enabled(void);
void port_cinematic_set_enabled(int on); /* off also lands a flight */
int port_enemies_ignore(void);
void port_enemies_ignore_set(int on);

/* The flight's feel (the menu's rows): units a second at full input, and the two lags' time constants */
#define PORT_CINEMATIC_SPEED_MIN 100
#define PORT_CINEMATIC_SPEED_MAX 3000
#define PORT_CINEMATIC_SMOOTHING_MIN_S 0.05f
#define PORT_CINEMATIC_SMOOTHING_MAX_S 2.0f
#define PORT_CINEMATIC_FAST_MULTIPLIER 4.0f /* with Run held */
int port_cinematic_speed(void);
void port_cinematic_set_speed(int units_per_s);
float port_cinematic_move_smoothing_s(void);
void port_cinematic_set_move_smoothing_s(float seconds);
float port_cinematic_look_smoothing_s(void);
void port_cinematic_set_look_smoothing_s(float seconds);

/* ---- The keys (port/sdk/controls.c, at each vblank) -------------------------------------------- */
void port_cinematic_key_toggle(void);                  /* the camera's key went down: taken only with the cheat on */
void port_cinematic_set_held(int vertical, int fast); /* Fly up minus Fly down (-1, 0, 1); Run held */

/* ---- The game's side (game/cinematic.c) -------------------------------------------------------- */
/* 1 once per press of the key since the last call (with the cheat on) */
int port_cinematic_take_toggle(void);
int port_cinematic_vertical(void);
int port_cinematic_fast(void);
/* Flying now: game/cinematic.c sets it; port_invincible() and instant mouse look read it */
int port_cinematic_flying(void);
void port_cinematic_set_flying(int on);

/* Game side, called by the patched game (patches/game/player.patch, core.patch) */
void Cinematic_AfterMove(void);   /* Player_Update, right after Player_Move: the flight */
void Cinematic_PlaceCamera(void); /* GameLoop_Run, after GameLoop_UpdateCamera: the view's angles while flying */
/* GameLoop_DrawFrame's screen passes, between the two: while flying, the picture shows none of the player's conditions
   or damage (the poison's and the other ailments' tints, the darkness, the red flash and knock-back blur of a hit, the MP
   drain's wobble).
   Only the picture: the game's own state is put back, so they show again as soon as the flight lands. */
void Cinematic_HideScreenEffects(void);
void Cinematic_ShowScreenEffects(void);

#ifdef __cplusplus
}
#endif

#endif
