/* PC controls (port/sdk/controls.c): the keyboard, the mouse and a game controller as the pad, all at once,
   by one table that can be rebound (the settings overlay). There is no layout to choose: the mouse is held and
   looks in play, is let go in the menus, and its cursor shows while it is the thing being used. */
#ifndef PORT_CONTROLS_H
#define PORT_CONTROLS_H

#include <stddef.h>

/* Called by the game where the player's frame applies the turn and look (Player_Move), whether the
   player is in control (`gameplay`). Gives the mouse's turn and look since the last call, in
   radians, and returns 1; returns 0 and changes nothing when mouse look is off, the mouse is not
   captured, the player is not in control, or a pad script is playing (so runs stay deterministic).
   A pad script recorded with mouse look (--record-input) gives back the recorded turns instead. */
int port_mouse_look(int gameplay, float *yaw, float *pitch);

/* 1 while the mouse is what turns the view: it has turned it (port_mouse_look gave a turn, a pad script's recorded
   turns too) since a game controller's last button or stick; 0 from the start until then. Keys change nothing, so
   a mouse player standing still keeps it. Read by port_attack_tracking_scale's Auto. */
int port_mouse_turning(void);

/* Called by the game (Player_Update) with the pad's held buttons and the action button's bit (the
   game's button for running and for using what is in front). The action bit of `held` becomes whether the
   player runs: Run held on the keyboard, or the controller's action button held (a controller has no Run: it
   runs as on the PS2). So the keyboard's action key only uses things (pressed is the game's own). A pad script
   gives what its recording held (run=), or nothing (`held` as it is: the action button runs). */
unsigned int port_controls_run_held(unsigned int held, unsigned int action_bit);
int port_controls_run_state(void); /* running (1) or not (0); -1 under a pad script */

/* ---- Recording and replaying the PC's controls (port/sdk/libpad.c, --record-input and --input) ---- */
int port_pad_script_run(void);                      /* the script's run= at this vblank; -1 none */
int port_pad_replay_look(float *yaw, float *pitch); /* the recorded turn of this call (1) or none (0); -1 no turns */
void port_pad_record_look(float yaw, float pitch);  /* (recording) a turn port_mouse_look gave */
void port_pad_record_event(const char *name, int slot); /* (recording) a PC key the game sees */
void port_pad_record_note(const char *note);        /* (recording) a comment at this vblank */

void port_controls_init(void);

/* What the player used last, for the buttons the menus show with --glyphs auto (main thread): the keyboard or mouse
   until a controller is used, or one is plugged in before anything is pressed */
typedef enum { PORT_DEVICE_KEYS, PORT_DEVICE_PLAYSTATION_PAD, PORT_DEVICE_OTHER_PAD } PortDevice;
PortDevice port_controls_device(void);

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
    ACTION_RUN,           /* running apart from the action key, keyboard and mouse only (port_controls_run_held) */
    ACTION_CAPTURE_MOUSE, /* let the mouse go in play, or take it back (unbound by default) */
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
    ACTION_WEAPON_SWAP,   /* the hands mod (port/include/port/hands.h): the other weapon; its d-pad is the hands' in play */
    ACTION_SPELL_SWAP,    /* the other spell */
    ACTION_HOTKEY_NEXT,   /* the next hotkey that holds something */
    ACTION_HOTKEY_PREVIOUS,
    ACTION_CINEMATIC_CAMERA, /* the cinematic camera cheat (port/include/port/cinematic.h): fly or land: V, the pad's
                                spare button (a DualSense's mute key) */
    ACTION_FLY_UP,           /* flying: rise and sink */
    ACTION_FLY_DOWN,
    ACTION_MOD_FIRST,        /* the mods' keys (mod.json's "keys"): given out at start (port_controls_mod_action) */
    ACTION_MOD_LAST = ACTION_MOD_FIRST + 15,
    ACTION_COUNT
} PortAction;

/* Each action has three slots: two keys or mouse buttons, and a controller button */
#define BINDING_SLOTS 3
#define BINDING_PAD_SLOT 2

/* A binding is a kind and SDL's code for the input */
#define BINDING_NONE 0
#define BINDING_KEY 0x10000     /* | SDL_Scancode */
#define BINDING_MOUSE 0x20000   /* | SDL's mouse button: 1 left, 2 middle, 3 right, 4 and 5 the side ones */
#define BINDING_PAD 0x30000     /* | SDL_GamepadButton */
#define BINDING_TRIGGER 0x40000 /* | SDL_GamepadAxis: the two triggers */
#define BINDING_KIND(b) ((b) & 0x7FFF0000)
#define BINDING_CODE(b) ((b) & 0xFFFF)

const char *port_controls_action_label(int action); /* "Cross", "Left stick up", "Quick save" */
const char *port_controls_action_id(int action);    /* "cross", "left_stick_up": settings.json's names */
int port_controls_action_used(int action); /* the action means something (its mod is on) */
/* An action's binding slots: all of them, or for Run (keys and mouse buttons only: a controller has no Run) those
   before the controller's */
static inline int port_controls_action_slots(int action) {
    return action == ACTION_RUN ? BINDING_PAD_SLOT : BINDING_SLOTS;
}
/* A mod's key: the action `action` (ACTION_MOD_FIRST ...) named id ("mod.kf4-journal.open"), labelled, bound by
   default to `binding` (port_controls_binding_parse's form, "key:B"; NULL none) */
void port_controls_mod_action(int action, const char *id, const char *label, const char *binding);
/* whether a mod's key is in use (its mod is applied): its row shown on Key Bindings */
void port_controls_mod_action_used(int action, int used);
/* a mod's key: presses since the last call, and whether held now */
int port_controls_mod_presses(int action);
int port_controls_mod_held(int action);
int port_controls_binding(int action, int slot);
void port_controls_bind(int action, int slot, int binding);
int port_controls_is_default(int action);
void port_controls_reset(void); /* the defaults, mouse included */
/* For people ("Space", "Mouse left", "Pad A") and for settings.json ("key:Space", "mouse:left",
   "pad:a", "trigger:lefttrigger"); 0 when the text is no binding. */
void port_controls_binding_label(int binding, char *out, size_t size);
void port_controls_binding_text(int binding, char *out, size_t size);
int port_controls_binding_parse(const char *text, int *binding);

/* The pad button each mouse button (0 left, 1 right, 2 middle) presses outside play, an ACTION_ of the face
   buttons and d-pad (-1: none) */
int port_controls_menu_mouse(int button);
void port_controls_set_menu_mouse(int button, int action);
/* Mouse look: a multiple of 0.15 degrees a pixel, and whether up and down are swapped */
double port_controls_mouse_sensitivity(void);
int port_controls_mouse_invert(void);
void port_controls_set_mouse(double sensitivity, int invert_y);
/* Instant mouse look (port/include/port/mouse_lead.h): the pictures between show the mouse's turn by the vblank
   it is read. Off by default. */
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
/* (While a layer over the game holds the input, port_overlay_holds_input, the game gets nothing, and what is held when
   the last one closes is ignored until let go: port_controls_read asks at each vblank, the one place that decides.) */

#endif
