/* PC controls (port/sdk/controls.c): the keyboard, the mouse and a game controller as the pad, by
   tables that can be rebound (the settings overlay, F1). Two layouts: PS2 (the default), the keys of
   port/sdk/libpad.c, and Verdite (--controls verdite), the keyboard layout and mouse look of the
   KF2/KF3 ports. */
#ifndef PORT_CONTROLS_H
#define PORT_CONTROLS_H

#include <stddef.h>

/* Called by the game where the player's frame applies the turn and look (Player_Move), whether the
   player is in control (`gameplay`). Gives the mouse's turn and look since the last call, in
   radians, and returns 1; returns 0 and changes nothing when mouse look is off, the mouse is not
   captured, the player is not in control, or a pad script is playing (so runs stay deterministic).
   A pad script recorded with mouse look (--record-input) gives back the recorded turns instead. */
int port_mouse_look(int gameplay, float *yaw, float *pitch);

/* Called by the game (Player_Update) with the pad's held buttons and the action button's bit (the
   game's button for running and for using what is in front). With the separate run mod on
   (port_options.separate_run) and Run bound in the layout, running is Run's: the action bit of
   `held` becomes whether Run is held, so the action button only uses things (pressed is the game's
   own). Otherwise `held` is returned as it is: the action button runs, as on the PS2. A pad script
   gives what its recording held (run=), or nothing (the action button runs). */
unsigned int port_controls_run_held(unsigned int held, unsigned int action_bit);
int port_controls_run_state(void); /* Run held (1) or not (0) with the mod on and bound; -1: the mod does not apply */

/* ---- Recording and replaying the PC's controls (port/sdk/libpad.c, --record-input and --input) ---- */
int port_pad_script_run(void);                      /* the script's run= at this vblank; -1 none */
int port_pad_replay_look(float *yaw, float *pitch); /* the recorded turn of this call (1) or none (0); -1 no turns */
void port_pad_record_look(float yaw, float pitch);  /* (recording) a turn port_mouse_look gave */
void port_pad_record_event(const char *name, int slot); /* (recording) a PC key the game sees */
void port_pad_record_note(const char *note);        /* (recording) a comment at this vblank */

/* ---- Layouts --------------------------------------------------------------------------------- */
#define CONTROLS_PS2 0
#define CONTROLS_VERDITE 1
#define CONTROLS_LAYOUTS 2

void port_controls_init(const char *layout); /* "ps2" (or NULL) or "verdite" */
int port_controls_verdite(void);             /* the Verdite layout is on (and no pad script plays) */
int port_controls_layout(void);
void port_controls_set_layout(int layout);
const char *port_controls_layout_name(int layout); /* "ps2", "verdite" */
int port_controls_layout_from_name(const char *name); /* -1: not a layout */

/* ---- What can be bound ------------------------------------------------------------------------ */
typedef enum {
    ACTION_UP,
    ACTION_DOWN,
    ACTION_LEFT,
    ACTION_RIGHT,
    ACTION_CROSS,
    ACTION_CIRCLE,
    ACTION_SQUARE,
    ACTION_TRIANGLE,
    ACTION_L1,
    ACTION_R1,
    ACTION_L2,
    ACTION_R2,
    ACTION_L3,
    ACTION_R3,
    ACTION_START,
    ACTION_SELECT,
    ACTION_LEFT_STICK_UP,
    ACTION_LEFT_STICK_DOWN,
    ACTION_LEFT_STICK_LEFT,
    ACTION_LEFT_STICK_RIGHT,
    ACTION_RIGHT_STICK_UP,
    ACTION_RIGHT_STICK_DOWN,
    ACTION_RIGHT_STICK_LEFT,
    ACTION_RIGHT_STICK_RIGHT,
    ACTION_RUN,           /* the separate run mod: running apart from the action button (port_controls_run_held) */
    ACTION_CAPTURE_MOUSE, /* Verdite: capture or release the mouse */
    ACTION_QUICK_SAVE,    /* save states (port/include/port/savestate.h), the quick slot: F5, F7 */
    ACTION_QUICK_LOAD,
    ACTION_GAME_SAVE,     /* quick saves (port/include/port/quicksave.h), any build: F2, F4 */
    ACTION_GAME_LOAD,
    ACTION_GS_CAPTURE,    /* the debugging tools (port/include/port/debug.h): F9, F10, F12 */
    ACTION_STATE_DUMP,
    ACTION_AUDIO_TAIL,
    ACTION_FLASH_RECORD,  /* the flash recorder (port/include/port/debug.h): F8 */
    ACTION_SHOW_FPS,      /* the frame-rate counter shown or hidden (port_options.show_fps): F3 */
    ACTION_STATS_PANEL,   /* the extended stats mod's panel shown or hidden (port/include/port/stats.h): F6 */
    ACTION_MAP,           /* the automap mod: the full map opened or closed (port/include/port/automap.h): M */
    ACTION_WEAPON_SWAP,   /* the weapon sets mod: set 2 swapped in (port/include/port/weapon_sets.h): R, R3 */
    ACTION_COUNT
} PortAction;

/* Each action has three slots: two keys or mouse buttons, and a controller button */
#define BINDING_SLOTS 3
#define BINDING_PAD_SLOT 2

/* A binding is a kind and SDL's code for the input */
#define BINDING_NONE 0
#define BINDING_KEY 0x10000     /* | SDL_Scancode */
#define BINDING_MOUSE 0x20000   /* | SDL's mouse button: 1 left, 2 middle, 3 right, 4 and 5 the side ones */
#define BINDING_PAD 0x30000     /* | SDL_GameControllerButton */
#define BINDING_TRIGGER 0x40000 /* | SDL_GameControllerAxis: the two triggers */
#define BINDING_KIND(b) ((b) & 0x7FFF0000)
#define BINDING_CODE(b) ((b) & 0xFFFF)

const char *port_controls_action_label(int action); /* "Cross", "Left stick up", "Quick save" */
const char *port_controls_action_id(int action);    /* "cross", "left_stick_up": settings.json's names */
int port_controls_action_used(int layout, int action); /* the action means something in that layout */
int port_controls_binding(int layout, int action, int slot);
void port_controls_bind(int layout, int action, int slot, int binding);
int port_controls_is_default(int layout, int action);
void port_controls_reset(int layout); /* the layout's defaults, mouse included */
/* For people ("Space", "Mouse left", "Pad A") and for settings.json ("key:Space", "mouse:left",
   "pad:a", "trigger:lefttrigger"); 0 when the text is no binding. */
void port_controls_binding_label(int binding, char *out, size_t size);
void port_controls_binding_text(int binding, char *out, size_t size);
int port_controls_binding_parse(const char *text, int *binding);

/* Verdite: the pad button each mouse button (0 left, 1 right, 2 middle) presses outside play, an
   ACTION_ of the face buttons and d-pad (-1: none) */
int port_controls_menu_mouse(int button);
void port_controls_set_menu_mouse(int button, int action);
/* Verdite outside play (the menu mouse's "not in play"): the keys bound to L1 / R1 (A / D) press d-pad
   left / right and the keys bound to d-pad left / right (the arrows) press L1 / R1, so A / D move left
   and right in the menus as W / S move up and down. On by default; the controller is unchanged. */
int port_controls_menu_side_keys(void);
void port_controls_set_menu_side_keys(int on);
/* Verdite's mouse look: a multiple of 0.15 degrees a pixel, and whether up and down are swapped */
double port_controls_mouse_sensitivity(void);
int port_controls_mouse_invert(void);
void port_controls_set_mouse(double sensitivity, int invert_y);
/* Instant mouse look (port/include/port/mouse_lead.h): the pictures between show the mouse's turn by the vblank
   it is read. On by default. */
int port_controls_mouse_lead(void);
void port_controls_set_mouse_lead(int on);

/* Rebinding: the next key, mouse button or controller button pressed goes into the slot. F1 or five
   seconds cancel, Delete clears it. */
void port_controls_listen(int action, int slot);
int port_controls_listening(int *action, int *slot);
void port_controls_listen_cancel(void);

/* The port's side (port/sdk/libpad.c, port/platform/video.c, port/overlay) */
int port_controls_event(const void *sdl_event); /* 1: the event was taken (rebinding) */
/* Once a vblank: the keyboard, mouse and controller into libpad's buttons and sticks (rx, ry, lx, ly) */
void port_controls_read(unsigned int *buttons, int axis[4], void *sdl_controller);
/* (the game's thread) Between vblanks, as it waits and works: the mouse's motion read now for instant mouse look's
   readings (port/include/port/mouse_lead.h), at most every half millisecond; nothing outside play */
void port_controls_sample_mouse(void);
/* While the overlay is open the game gets nothing; what is held when it closes is ignored until let go */
void port_controls_block(int blocked);
/* The mouse indicator over the picture (x, y, width, height in the window's pixels), drawn as squares
   by fill (rgba: R in the low byte, alpha in the high) with the window's renderer (SDL_Renderer, or
   ImGui's draw list on the Vulkan presenter). port_controls_indicator_shown: 1 while it is drawn. */
typedef void (*PortFillRect)(void *context, int x, int y, int width, int height, unsigned int rgba);
void port_controls_draw(PortFillRect fill, void *context, int x, int y, int width, int height);
int port_controls_indicator_shown(void);

#endif
