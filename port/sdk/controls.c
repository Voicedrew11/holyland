/* PC controls (port/include/port/controls.h): the keyboard, the mouse and a game controller as the pad, all at
   once, through one binding table the settings overlay rebinds and settings.json keeps. There is no layout to
   choose: what each does follows what the player is doing.

   The keyboard (the game's own button for each in brackets):
       W A S D       the left stick: walk and step sideways (twin-stick), move the menus' cursor
       arrows        the d-pad: walk and turn, move the menus' cursor
       I J K L       the right stick: turn and look
       Space         square (attack)                 E          cross (use, talk, confirm)
       Q             triangle (magic)                Escape, Tab  circle (the menu, back)
       Z / C         L1 / R1 (side steps, a menu's pages)
       Page Down / Up  L2 / R2 (look down / up)      Enter      start (pause)
       Backspace     select (the shortcut)           Left Shift  run (the separate run mod)
   The mouse, held while the player is in control (the window focused, no menu over the game): moving turns and
   looks; left square, right triangle, middle cross, the side buttons L1 / R1. Out of play (menus, pause,
   dialogue) left is cross, right circle, middle cross (the menu mouse), and the wheel up / down. On a menu
   screen that takes pointing (port/include/port/menu_pointer.h) the mouse is let go: the pointer moves the
   cursor to the line under it and the left button chooses the line clicked; a click on nothing does nothing.
   Elsewhere out of play (a message, a cutscene) the mouse stays held and the cursor hidden. A menu over the
   game (F1, the overlay) or another window lets it go; a click in play takes it back.

   While the mouse is let go its cursor shows only while the mouse is what is being used: a key or a controller
   hides it, moving or clicking the mouse shows it again.

   A controller's buttons are the PS2 pad's (A cross, B circle, X square, Y triangle, the shoulders and triggers
   L1 R1 L2 R2). F2 / F4 take and load a quick save (port/platform/quicksave.c), F5 / F7 save and load the quick
   slot's save state, and F3 shows or hides the frame-rate counter.

   With the separate run mod on (--separate-run, the Mods page), Left Shift and the controller's left stick
   press run while held, and the action button (cross) only uses things. Off, Run is not used and the action
   button runs, as on the PS2. Twin-stick controls (port/include/port/twin_stick.h, on by default) make the left
   stick walk and step sideways and the right stick turn and look; off, the left stick is the d-pad (walk and
   turn), as on the PS2.

   Mouse look turns the player in Player_Move, where the game adds the pad's turning (port_mouse_look): each
   movement is applied once, so it neither coasts nor builds speed. A mouse button held as play starts or stops
   does nothing until it is let go (a menu's click does not become an attack). A pad script (--input) turns all of this off, so scripted runs stay the same. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/automap.h"
#include "port/controls.h"
#include "port/debug.h"
#include "port/host.h"
#include "port/menu_pointer.h"
#include "port/mouse_lead.h"
#include "port/cinematic.h"
#include "port/overlay.h"
#include "port/platform.h"
#include "port/present_vk.h"
#include "port/savestate.h"
#include "port/quicksave.h"
#include "port/settings.h"
#include "port/stats.h"
#include "port/hands.h"

#ifdef KF4_HAVE_SDL
#include <SDL3/SDL.h>
#endif

#define PI 3.14159265358979323846
#define RADIANS_PER_PIXEL (0.15 * PI / 180.0)
#define MAX_TURN (PI / 2.0)

static struct {
    int captured;                /* the mouse is held (SDL's relative mouse mode) */
    int wants_capture;           /* the player has not let it go (ACTION_CAPTURE_MOUSE) */
    int mouse_last;              /* the mouse was used last, not a key or a controller: its cursor shows */
    int cursor_shown;            /* SDL's cursor is shown (while the mouse is let go) */
    uint32_t motion_after;       /* SDL_GetTicks before which mouse motion is not the player's (SDL's warp) */
    uint32_t gameplay_ms;        /* SDL_GetTicks at the last read in play */
    int focused;
    int blocked;                 /* the overlay is open */
    int unblocked;               /* it has just closed: what is held is ignored until let go */
    double yaw;                  /* mouse movement not yet applied, in radians */
    double pitch;
    uint64_t look_vblank;        /* the vblank of the game's last call, and what it said */
    int look_gameplay;
    int seen_look;
    int previous_gameplay;
    unsigned int previous_buttons;
    unsigned int swallowed;      /* mouse buttons held across a change of mode, ignored until released */
    uint64_t previous_actions;
    uint64_t swallowed_actions;  /* actions held when the overlay closed */
    int wheel;                   /* wheel movement since the last read: +1 up, -1 down */
    uint32_t tap_until[16];      /* a short press is held this long, so the game's frame sees it */
    uint32_t wheel_until[2];
    int run;                     /* Run is held */
    int listen_action;           /* rebinding: the slot waiting for a press (-1: none) */
    int listen_slot;
    uint32_t listen_until;
} pc;

/* ---- The actions ------------------------------------------------------------------------------ */
static const struct {
    const char *id;
    const char *label;
    unsigned int bit; /* libpad's button (0: a stick or the PC's own) */
} actions[ACTION_COUNT] = {
    [ACTION_UP] = {"up", "D-pad up", 0x0010},
    [ACTION_DOWN] = {"down", "D-pad down", 0x0040},
    [ACTION_LEFT] = {"left", "D-pad left", 0x0080},
    [ACTION_RIGHT] = {"right", "D-pad right", 0x0020},
    [ACTION_CROSS] = {"cross", "Cross", 0x4000},
    [ACTION_CIRCLE] = {"circle", "Circle", 0x2000},
    [ACTION_SQUARE] = {"square", "Square", 0x8000},
    [ACTION_TRIANGLE] = {"triangle", "Triangle", 0x1000},
    [ACTION_L1] = {"l1", "L1", 0x0400},
    [ACTION_R1] = {"r1", "R1", 0x0800},
    [ACTION_L2] = {"l2", "L2", 0x0100},
    [ACTION_R2] = {"r2", "R2", 0x0200},
    [ACTION_L3] = {"l3", "L3 (left stick press)", 0x0002},
    [ACTION_R3] = {"r3", "R3 (right stick press)", 0x0004},
    [ACTION_START] = {"start", "Start", 0x0008},
    [ACTION_SELECT] = {"select", "Select", 0x0001},
    [ACTION_LEFT_STICK_UP] = {"left_stick_up", "Left stick up", 0},
    [ACTION_LEFT_STICK_DOWN] = {"left_stick_down", "Left stick down", 0},
    [ACTION_LEFT_STICK_LEFT] = {"left_stick_left", "Left stick left", 0},
    [ACTION_LEFT_STICK_RIGHT] = {"left_stick_right", "Left stick right", 0},
    [ACTION_RIGHT_STICK_UP] = {"right_stick_up", "Right stick up", 0},
    [ACTION_RIGHT_STICK_DOWN] = {"right_stick_down", "Right stick down", 0},
    [ACTION_RIGHT_STICK_LEFT] = {"right_stick_left", "Right stick left", 0},
    [ACTION_RIGHT_STICK_RIGHT] = {"right_stick_right", "Right stick right", 0},
    [ACTION_RUN] = {"run", "Run (hold)", 0},
    [ACTION_CAPTURE_MOUSE] = {"capture_mouse", "Release / take back the mouse", 0},
    [ACTION_QUICK_SAVE] = {"quick_save", "Save state (debugging, same build)", 0},
    [ACTION_QUICK_LOAD] = {"quick_load", "Load state (debugging, same build)", 0},
    [ACTION_GAME_SAVE] = {"game_quick_save", "Quick save", 0},
    [ACTION_GAME_LOAD] = {"game_quick_load", "Quick load", 0},
    [ACTION_GS_CAPTURE] = {"gs_capture", "GS frame capture (debug)", 0},
    [ACTION_STATE_DUMP] = {"state_dump", "Game state dump (debug)", 0},
    [ACTION_AUDIO_TAIL] = {"audio_tail", "Last 20 s of sound (debug)", 0},
    [ACTION_FLASH_RECORD] = {"flash_record", "Flash recorder: the last pictures (debug)", 0},
    [ACTION_SHOW_FPS] = {"show_fps", "Show / hide the frame rate", 0},
    [ACTION_STATS_PANEL] = {"stats_panel", "Show / hide the stats panel (mod)", 0},
    [ACTION_MAP] = {"map", "Map (the automap mod)", 0},
    [ACTION_WEAPON_SWAP] = {"weapon_swap", "Swap weapon (hands mod)", 0},
    [ACTION_SPELL_SWAP] = {"spell_swap", "Swap spell (hands mod)", 0},
    [ACTION_HOTKEY_NEXT] = {"hotkey_next", "Next hotkey (hands mod)", 0},
    [ACTION_HOTKEY_PREVIOUS] = {"hotkey_previous", "Previous hotkey (hands mod)", 0},
    [ACTION_CINEMATIC_CAMERA] = {"cinematic_camera", "Cinematic camera: fly / land (cheat)", 0},
    [ACTION_FLY_UP] = {"fly_up", "Cinematic camera: fly up", 0},
    [ACTION_FLY_DOWN] = {"fly_down", "Cinematic camera: fly down", 0},
};

/* The hands mod's key of the action, -1 none */
static int hands_key_of(int action) {
    switch (action) {
    case ACTION_WEAPON_SWAP:
        return HANDS_KEY_WEAPON;
    case ACTION_SPELL_SWAP:
        return HANDS_KEY_SPELL;
    case ACTION_HOTKEY_NEXT:
        return HANDS_KEY_HOTKEY_NEXT;
    case ACTION_HOTKEY_PREVIOUS:
        return HANDS_KEY_HOTKEY_PREVIOUS;
    default:
        return -1;
    }
}

const char *port_controls_action_label(int action) {
    return action >= 0 && action < ACTION_COUNT ? actions[action].label : "?";
}

const char *port_controls_action_id(int action) {
    return action >= 0 && action < ACTION_COUNT ? actions[action].id : "?";
}

int port_controls_action_used(int action) {
    if (action == ACTION_RUN) {
        return port_options.separate_run;
    }
    if (action == ACTION_STATS_PANEL) {
        return port_stats_options.on;
    }
    if (action == ACTION_MAP) {
        return port_automap.enabled;
    }
    if (hands_key_of(action) >= 0) {
        return port_hands.enabled;
    }
    if (action == ACTION_CINEMATIC_CAMERA || action == ACTION_FLY_UP || action == ACTION_FLY_DOWN) {
        return port_cinematic_enabled();
    }
    return action >= 0 && action < ACTION_COUNT;
}

/* ---- The table ------------------------------------------------------------------------------- */
#ifdef KF4_HAVE_SDL
#define KEY(name) (BINDING_KEY | SDL_SCANCODE_##name)
#define MOUSE(n) (BINDING_MOUSE | (n))
#define PAD(name) (BINDING_PAD | SDL_GAMEPAD_BUTTON_##name)
#define TRIGGER(name) (BINDING_TRIGGER | SDL_GAMEPAD_AXIS_##name##_TRIGGER)
#else
#define KEY(name) 0
#define MOUSE(n) 0
#define PAD(name) 0
#define TRIGGER(name) 0
#endif

/* Two keys or mouse buttons and a controller button an action. The game's buttons: square attacks, triangle
   casts, cross uses (and runs, without the separate run mod), circle opens the menu, select the shortcut, start
   pauses (Input_SetDefaultControls). In the menus cross chooses, circle goes back, and square and triangle do
   a screen's own things (triangle takes off what is worn). */
static const int default_bindings[ACTION_COUNT][BINDING_SLOTS] = {
    [ACTION_UP] = {KEY(UP), 0, PAD(DPAD_UP)},
    [ACTION_DOWN] = {KEY(DOWN), 0, PAD(DPAD_DOWN)},
    [ACTION_LEFT] = {KEY(LEFT), 0, PAD(DPAD_LEFT)},
    [ACTION_RIGHT] = {KEY(RIGHT), 0, PAD(DPAD_RIGHT)},
    [ACTION_CROSS] = {KEY(E), MOUSE(2), PAD(SOUTH)},
    [ACTION_CIRCLE] = {KEY(ESCAPE), KEY(TAB), PAD(EAST)},
    [ACTION_SQUARE] = {KEY(SPACE), MOUSE(1), PAD(WEST)},
    [ACTION_TRIANGLE] = {KEY(Q), MOUSE(3), PAD(NORTH)},
    [ACTION_L1] = {KEY(Z), MOUSE(4), PAD(LEFT_SHOULDER)},
    [ACTION_R1] = {KEY(C), MOUSE(5), PAD(RIGHT_SHOULDER)},
    [ACTION_L2] = {KEY(PAGEDOWN), 0, TRIGGER(LEFT)},
    [ACTION_R2] = {KEY(PAGEUP), 0, TRIGGER(RIGHT)},
    [ACTION_L3] = {0, 0, PAD(LEFT_STICK)},
    [ACTION_R3] = {0, 0, PAD(RIGHT_STICK)},
    [ACTION_START] = {KEY(RETURN), 0, PAD(START)},
    [ACTION_SELECT] = {KEY(BACKSPACE), 0, PAD(BACK)},
    [ACTION_RUN] = {KEY(LSHIFT), 0, PAD(LEFT_STICK)},
    [ACTION_LEFT_STICK_UP] = {KEY(W)},
    [ACTION_LEFT_STICK_DOWN] = {KEY(S)},
    [ACTION_LEFT_STICK_LEFT] = {KEY(A)},
    [ACTION_LEFT_STICK_RIGHT] = {KEY(D)},
    [ACTION_RIGHT_STICK_UP] = {KEY(I)},
    [ACTION_RIGHT_STICK_DOWN] = {KEY(K)},
    [ACTION_RIGHT_STICK_LEFT] = {KEY(J)},
    [ACTION_RIGHT_STICK_RIGHT] = {KEY(L)},
    [ACTION_QUICK_SAVE] = {KEY(F5)},
    [ACTION_QUICK_LOAD] = {KEY(F7)},
    [ACTION_GAME_SAVE] = {KEY(F2)},
    [ACTION_GAME_LOAD] = {KEY(F4)},
    [ACTION_GS_CAPTURE] = {KEY(F9)},
    [ACTION_STATE_DUMP] = {KEY(F10)},
    [ACTION_AUDIO_TAIL] = {KEY(F12)},
    [ACTION_FLASH_RECORD] = {KEY(F8)},
    [ACTION_SHOW_FPS] = {KEY(F3)},
    [ACTION_STATS_PANEL] = {KEY(F6)},
    [ACTION_MAP] = {KEY(M)},
    [ACTION_WEAPON_SWAP] = {KEY(1), KEY(R), PAD(DPAD_RIGHT)},
    [ACTION_SPELL_SWAP] = {KEY(2), KEY(T), PAD(DPAD_LEFT)},
    [ACTION_HOTKEY_NEXT] = {KEY(3), KEY(G), PAD(DPAD_DOWN)},
    [ACTION_HOTKEY_PREVIOUS] = {KEY(4), 0, PAD(DPAD_UP)},
    [ACTION_CINEMATIC_CAMERA] = {KEY(V), 0, PAD(MISC1)},
    [ACTION_FLY_UP] = {KEY(X)},
    [ACTION_FLY_DOWN] = {KEY(LCTRL)},
};
/* The mouse outside play: left cross, right circle, middle cross */
static const int default_menu_mouse[3] = {ACTION_CROSS, ACTION_CIRCLE, ACTION_CROSS};

/* The table in use: the defaults until settings.json (port/overlay/settings.c) or the overlay changes it, which
   may be before port_controls_init */
static int tables_ready;
static int bindings[ACTION_COUNT][BINDING_SLOTS];
static int menu_mouse[3];
static double sensitivity;
static int invert_y;
static int mouse_lead = 1; /* instant mouse look (port/include/port/mouse_lead.h) */

static void reset_tables(void) {
    memcpy(bindings, default_bindings, sizeof(bindings));
    memcpy(menu_mouse, default_menu_mouse, sizeof(menu_mouse));
    sensitivity = 1.0;
    invert_y = 0;
}

static void tables_init(void) {
    if (tables_ready) {
        return;
    }
    reset_tables();
    tables_ready = 1;
}

void port_controls_reset(void) {
    tables_init();
    reset_tables();
}

int port_controls_binding(int action, int slot) {
    if (action < 0 || action >= ACTION_COUNT || slot < 0 || slot >= BINDING_SLOTS) {
        return BINDING_NONE;
    }
    tables_init();
    return bindings[action][slot];
}

void port_controls_bind(int action, int slot, int binding) {
    if (action < 0 || action >= ACTION_COUNT || slot < 0 || slot >= BINDING_SLOTS) {
        return;
    }
    tables_init();
    bindings[action][slot] = binding;
}

int port_controls_is_default(int action) {
    tables_init();
    return memcmp(bindings[action], default_bindings[action], sizeof(bindings[action])) == 0;
}

int port_controls_menu_mouse(int button) {
    tables_init();
    return button >= 0 && button < 3 ? menu_mouse[button] : -1;
}

void port_controls_set_menu_mouse(int button, int action) {
    tables_init();
    if (button >= 0 && button < 3) {
        menu_mouse[button] = action;
    }
}

double port_controls_mouse_sensitivity(void) {
    tables_init();
    return sensitivity;
}

int port_controls_mouse_invert(void) {
    tables_init();
    return invert_y;
}

void port_controls_set_mouse(double new_sensitivity, int new_invert_y) {
    tables_init();
    sensitivity = new_sensitivity < 0.05 ? 0.05 : new_sensitivity > 10.0 ? 10.0 : new_sensitivity;
    invert_y = new_invert_y != 0;
}

int port_controls_mouse_lead(void) {
    return mouse_lead;
}

void port_controls_set_mouse_lead(int on) {
    mouse_lead = on != 0;
}

/* Instant mouse look now: held off while the cinematic camera flies, whose look smoothing and the lead together
   judder (Verdite3 aeb5030); the setting is left as it is */
static int lead_on(void) {
    return mouse_lead && !port_cinematic_flying();
}

/* The mouse motion not yet spent, as the next port_mouse_look will give it, to instant mouse look (nothing
   while it is off: then nothing is spent there either, and no picture is turned) */
static double held_turn(double turn) {
    return turn < -MAX_TURN ? -MAX_TURN : turn > MAX_TURN ? MAX_TURN : turn;
}

/* A reading's time in vblanks: now, as the pictures' tags reckon it (the vblank's number before its times are known) */
static double reading_time(void) {
    double at = port_video_vblanks_at(port_wall_seconds());

    return at >= 0 ? at : (double)port_vblank_count();
}

static void lead_waiting(void) {
    MouseTurn waiting = {0, 0};

    if (lead_on()) {
        waiting.yaw_rad = held_turn(pc.yaw);
        waiting.pitch_rad = held_turn(pc.pitch);
    }
    port_mouse_lead_waiting(waiting, reading_time());
}

/* KF4_DIAG_SPIN=RAD_S (developers): a steady turn of RAD_S radians a second added in play as the mouse's motion
   would be, also with a pad script (--input), to test a fast spin of the mouse without one (the
   pictures between's cuts, the world at the screen's edge: port/include/port/interp_camera.h, past_band.h). 0: off */
static double diag_spin_rad_s(void) {
    static int read;
    static double rad_s;

    if (!read) {
        const char *e = getenv("KF4_DIAG_SPIN");

        rad_s = e != NULL ? atof(e) : 0.0;
        read = 1;
    }
    return rad_s;
}

/* (KF4_DIAG_SPIN) the spin's turn since the last call added to what is waiting, by the time the readings go by
   (reading_time: as a mouse read then would have moved); 1 when it is on */
static int diag_spin_add(void) {
    static double last_v = -1;
    double rad_s = diag_spin_rad_s(), now_v;

    if (rad_s == 0.0) {
        return 0;
    }
    now_v = reading_time();
    if (last_v >= 0 && now_v > last_v && now_v - last_v < 8) {
        pc.yaw += rad_s * (now_v - last_v) * (1001.0 / 60000.0);
    }
    last_v = now_v;
    return 1;
}

static void set_captured(int captured);

void port_controls_init(void) {
    memset(&pc, 0, sizeof(pc));
    pc.focused = 1;
    pc.wants_capture = 1;
    pc.mouse_last = 1;
    pc.cursor_shown = 1;
    pc.listen_action = -1;
    tables_init();
}

/* The player was in control at the game's last frame */
static int in_gameplay(void) {
    return pc.seen_look && pc.look_gameplay && port_vblank_count() - pc.look_vblank <= 2;
}

int port_mouse_look(int gameplay, float *yaw, float *pitch) {
    double y;
    double p;

    *yaw = *pitch = 0.0f;
    if (port_options.input != NULL) {
        /* a recording made with mouse look: its turns, call for call */
        int turned = port_pad_replay_look(yaw, pitch);

        if (turned >= 0) {
            MouseTurn none = {0, 0};

            /* (no instant mouse look: its readings go by the wall clock, and the pictures between would not
               repeat) */
            port_mouse_lead_waiting(none, reading_time());
            return turned;
        }
    }
    if (port_options.input != NULL && diag_spin_rad_s() == 0.0) {
        MouseTurn none = {0, 0};

        port_mouse_lead_waiting(none, reading_time()); /* (no mouse look: nothing ahead) */
        return 0;
    }
    pc.seen_look = 1;
    pc.look_vblank = port_vblank_count();
    pc.look_gameplay = gameplay;
    if (!gameplay || (!pc.captured && diag_spin_rad_s() == 0.0)) {
        pc.yaw = pc.pitch = 0.0;
        lead_waiting();
        return 0;
    }
    diag_spin_add();
    y = held_turn(pc.yaw);
    p = held_turn(pc.pitch);
    pc.yaw = pc.pitch = 0.0;
    if (y == 0.0 && p == 0.0) {
        return 0;
    }
    if (lead_on()) {
        MouseTurn spent = {y, p};

        port_mouse_lead_spent(spent); /* (out of what waited: the hand is where it was) */
    } else {
        lead_waiting();
    }
    *yaw = (float)y;
    *pitch = (float)p;
    port_pad_record_look(*yaw, *pitch); /* (--record-input) */
    return 1;
}

/* Whether Run is held, with the mod on and Run bound; -1 when the mod does not apply (the action button runs) */
int port_controls_run_state(void) {
    int slot;

    if (!port_options.separate_run || port_options.input != NULL) {
        return -1;
    }
    for (slot = 0; slot < BINDING_SLOTS; slot++) {
        if (port_controls_binding(ACTION_RUN, slot) != BINDING_NONE) {
            return pc.run;
        }
    }
    return -1;
}

/* Running is Run's with the mod on and Run bound; under a pad script, what its recording says (run=) */
unsigned int port_controls_run_held(unsigned int held, unsigned int action_bit) {
    int run = port_options.input != NULL ? port_pad_script_run() : port_controls_run_state();

    if (run < 0) {
        return held;
    }
    return run ? held | action_bit : held & ~action_bit;
}

#ifndef KF4_HAVE_SDL
static void set_captured(int captured) {
    (void)captured;
}

void port_controls_binding_label(int binding, char *out, size_t size) {
    (void)binding;
    snprintf(out, size, "-");
}

void port_controls_binding_text(int binding, char *out, size_t size) {
    (void)binding;
    snprintf(out, size, "none");
}

int port_controls_binding_parse(const char *text, int *binding) {
    (void)text;
    *binding = BINDING_NONE;
    return 0;
}

void port_controls_listen(int action, int slot) {
    (void)action;
    (void)slot;
}

int port_controls_listening(int *action, int *slot) {
    (void)action;
    (void)slot;
    return 0;
}

void port_controls_listen_cancel(void) {
}

void port_controls_sample_mouse(void) {
}
#else
#define B_UP 0x0010u
#define B_DOWN 0x0040u
#define B_SELECT 0x0001u
#define B_START 0x0008u
#define B_TRIANGLE 0x1000u
#define B_CIRCLE 0x2000u
#define B_CROSS 0x4000u
#define B_SQUARE 0x8000u
#define TAP_BUTTONS (B_SELECT | B_START | B_TRIANGLE | B_CIRCLE | B_CROSS | B_SQUARE)
#define TAP_MS 100u
#define LISTEN_MS 5000u
#define TRIGGER_DOWN 16384

/* ---- Names ------------------------------------------------------------------------------------ */
static const char *const mouse_ids[6] = {"", "left", "middle", "right", "x1", "x2"};
static const char *const mouse_labels[6] = {"", "Mouse left", "Mouse middle", "Mouse right", "Mouse 4", "Mouse 5"};

/* the PlayStation's name next to the controller's own, as SDL lays the buttons out (Xbox's) */
static const char *pad_label(int button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "Pad A";
    case SDL_GAMEPAD_BUTTON_EAST: return "Pad B";
    case SDL_GAMEPAD_BUTTON_WEST: return "Pad X";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Pad Y";
    case SDL_GAMEPAD_BUTTON_BACK: return "Pad Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Pad Guide";
    case SDL_GAMEPAD_BUTTON_START: return "Pad Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Pad left stick";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Pad right stick";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "Pad LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "Pad RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "Pad up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "Pad down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "Pad left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "Pad right";
    case SDL_GAMEPAD_BUTTON_MISC1: return "Pad mute / share";
    default: return NULL;
    }
}

void port_controls_binding_label(int binding, char *out, size_t size) {
    int code = BINDING_CODE(binding);
    const char *name;

    switch (BINDING_KIND(binding)) {
    case BINDING_KEY:
        name = SDL_GetScancodeName((SDL_Scancode)code);
        snprintf(out, size, "%s", name && *name ? name : "?");
        return;
    case BINDING_MOUSE:
        snprintf(out, size, "%s", code >= 1 && code <= 5 ? mouse_labels[code] : "Mouse ?");
        return;
    case BINDING_PAD:
        name = pad_label(code);
        if (name == NULL) {
            name = SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
            snprintf(out, size, "Pad %s", name ? name : "?");
        } else {
            snprintf(out, size, "%s", name);
        }
        return;
    case BINDING_TRIGGER:
        snprintf(out, size, "%s", code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "Pad LT" : "Pad RT");
        return;
    default:
        snprintf(out, size, "-");
        return;
    }
}

void port_controls_binding_text(int binding, char *out, size_t size) {
    int code = BINDING_CODE(binding);
    const char *name = NULL;

    switch (BINDING_KIND(binding)) {
    case BINDING_KEY:
        name = SDL_GetScancodeName((SDL_Scancode)code);
        snprintf(out, size, "key:%s", name ? name : "");
        return;
    case BINDING_MOUSE:
        snprintf(out, size, "mouse:%s", code >= 1 && code <= 5 ? mouse_ids[code] : "");
        return;
    case BINDING_PAD:
        name = SDL_GetGamepadStringForButton((SDL_GamepadButton)code);
        snprintf(out, size, "pad:%s", name ? name : "");
        return;
    case BINDING_TRIGGER:
        name = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)code);
        snprintf(out, size, "trigger:%s", name ? name : "");
        return;
    default:
        snprintf(out, size, "none");
        return;
    }
}

int port_controls_binding_parse(const char *text, int *binding) {
    int i;

    *binding = BINDING_NONE;
    if (strcmp(text, "none") == 0) {
        return 1;
    }
    if (strncmp(text, "key:", 4) == 0) {
        SDL_Scancode key = SDL_GetScancodeFromName(text + 4);

        if (key == SDL_SCANCODE_UNKNOWN) {
            return 0;
        }
        *binding = BINDING_KEY | (int)key;
        return 1;
    }
    if (strncmp(text, "mouse:", 6) == 0) {
        for (i = 1; i <= 5; i++) {
            if (strcmp(text + 6, mouse_ids[i]) == 0) {
                *binding = BINDING_MOUSE | i;
                return 1;
            }
        }
        return 0;
    }
    if (strncmp(text, "pad:", 4) == 0) {
        SDL_GamepadButton button = SDL_GetGamepadButtonFromString(text + 4);

        if (button == SDL_GAMEPAD_BUTTON_INVALID) {
            return 0;
        }
        *binding = BINDING_PAD | (int)button;
        return 1;
    }
    if (strncmp(text, "trigger:", 8) == 0) {
        SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(text + 8);

        if (axis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            return 0;
        }
        *binding = BINDING_TRIGGER | (int)axis;
        return 1;
    }
    return 0;
}

/* ---- Rebinding -------------------------------------------------------------------------------- */
void port_controls_listen(int action, int slot) {
    if (action < 0 || action >= ACTION_COUNT || slot < 0 || slot >= BINDING_SLOTS) {
        return;
    }
    pc.listen_action = action;
    pc.listen_slot = slot;
    pc.listen_until = (uint32_t)SDL_GetTicks() + LISTEN_MS;
}

int port_controls_listening(int *action, int *slot) {
    if (pc.listen_action >= 0 && (int32_t)(pc.listen_until - (uint32_t)SDL_GetTicks()) <= 0) {
        pc.listen_action = -1;
    }
    if (pc.listen_action < 0) {
        return 0;
    }
    if (action) {
        *action = pc.listen_action;
    }
    if (slot) {
        *slot = pc.listen_slot;
    }
    return 1;
}

void port_controls_listen_cancel(void) {
    pc.listen_action = -1;
}

static void listened(int binding) {
    bindings[pc.listen_action][pc.listen_slot] = binding;
    pc.listen_action = -1;
}

/* While a slot waits: the keyboard, the mouse and the controller go to it, not to the overlay. The
   releases still reach the overlay, which saw the presses before. */
static int listen_event(const SDL_Event *event) {
    int pad_slot = pc.listen_slot == BINDING_PAD_SLOT;

    if (!port_controls_listening(NULL, NULL)) {
        return 0;
    }
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
        if (event->key.repeat) {
            return 1;
        }
        if (event->key.scancode == SDL_SCANCODE_F1) {
            pc.listen_action = -1;
        } else if (event->key.scancode == SDL_SCANCODE_DELETE) {
            listened(BINDING_NONE);
        } else if (!pad_slot) {
            listened(BINDING_KEY | event->key.scancode);
        }
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!pad_slot && event->button.button >= 1 && event->button.button <= 5) {
            listened(BINDING_MOUSE | event->button.button);
        }
        return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (pad_slot) {
            listened(BINDING_PAD | event->gbutton.button);
        }
        return 1;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (pad_slot && event->gaxis.value > TRIGGER_DOWN &&
            (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
             event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            listened(BINDING_TRIGGER | event->gaxis.axis);
        }
        return 1;
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_WHEEL:
        return 1;
    default:
        return 0;
    }
}

/* ---- Reading ---------------------------------------------------------------------------------- */
#define MOTION_SETTLE_MS 150u /* after the mouse is held or let go, motion is SDL's warp, not the player's */
#define STICK_USED 16384      /* a controller's stick pushed this far is the controller in use */

/* The cursor, while the mouse is let go: shown while the mouse is what is being used, or a menu is over the game,
   or the window is not focused; hidden after a key or a controller is used, until the mouse moves */
static void update_cursor(void) {
    int show = !pc.captured && (pc.mouse_last || pc.blocked || !pc.focused);

    if (show != pc.cursor_shown) {
        if (show) {
            SDL_ShowCursor();
        } else {
            SDL_HideCursor();
        }
        pc.cursor_shown = show;
    }
}

static void set_captured(int captured) {
    if (captured == pc.captured) {
        return;
    }
    pc.captured = captured;
    pc.yaw = pc.pitch = 0.0;
    lead_waiting();
    if (port_video_window() != NULL) {
        SDL_SetWindowRelativeMouseMode(port_video_window(), captured != 0);
    }
    SDL_GetRelativeMouseState(NULL, NULL); /* motion before the change is dropped */
    pc.motion_after = (uint32_t)SDL_GetTicks() + MOTION_SETTLE_MS;
    update_cursor();
}

/* The game blocked while a layer over it (the player menu, the developers' overlay, the full map, the profiler's
   text field) holds the input, and the mouse let go. Only port_controls_read sets it, from port_overlay_holds_input
   at each vblank: one place decides, so a layer opened over another (F1 over the map) and closed again, or one that
   closes while nothing is drawn, cannot leave it wrong. */
static void set_blocked(int blocked) {
    if (blocked && !pc.blocked) {
        set_captured(0);
    }
    if (!blocked && pc.blocked) {
        pc.unblocked = 1;
    }
    pc.blocked = blocked;
    update_cursor();
}

/* What the player is using: the mouse, or a key or controller (the cursor follows it: update_cursor) */
static void note_device(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!event->key.repeat) {
            pc.mouse_last = 0;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        pc.mouse_last = 0;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event->gaxis.value > STICK_USED || event->gaxis.value < -STICK_USED) {
            pc.mouse_last = 0;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if ((int32_t)((uint32_t)SDL_GetTicks() - pc.motion_after) >= 0) {
            pc.mouse_last = 1;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
        pc.mouse_last = 1;
        break;
    default:
        break;
    }
}

int port_controls_event(const void *sdl_event) {
    const SDL_Event *event = sdl_event;

    if (listen_event(event)) {
        return 1;
    }
    note_device(event);
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        pc.focused = 0;
        set_captured(0);
    } else if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        pc.focused = 1;
        pc.unblocked = 1; /* the click that brought the window back does nothing */
    }
    if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) {
        update_cursor();
    }
    if (event->type == SDL_EVENT_MOUSE_WHEEL && port_options.input == NULL && !pc.blocked && !port_overlay_holds_mouse()) {
        /* (whole notches: a smooth wheel's fractions add up to one first) */
        pc.wheel += event->wheel.integer_y > 0 ? 1 : event->wheel.integer_y < 0 ? -1 : 0;
    }
    return 0;
}

static int binding_down(int binding, const bool *keys, unsigned int mouse, SDL_Gamepad *controller) {
    int code = BINDING_CODE(binding);

    switch (BINDING_KIND(binding)) {
    case BINDING_KEY:
        return pc.focused && code < SDL_SCANCODE_COUNT && keys[code];
    case BINDING_MOUSE:
        return (mouse >> code & 1u) != 0;
    case BINDING_PAD:
        return controller != NULL && SDL_GetGamepadButton(controller, (SDL_GamepadButton)code);
    case BINDING_TRIGGER:
        return controller != NULL && SDL_GetGamepadAxis(controller, (SDL_GamepadAxis)code) > TRIGGER_DOWN;
    default:
        return 0;
    }
}

/* a stick from two actions: 0x00 or 0xFF along each axis held, 0x80 otherwise */
static int action_axis(uint64_t held, int low, int high) {
    int l = (held >> low & 1u) != 0;
    int h = (held >> high & 1u) != 0;

    return l && !h ? 0x00 : h && !l ? 0xFF : 0x80;
}

#define RELEASE_AFTER_MS 250u /* out of play this long before the mouse is let go */
#define SAMPLE_EVERY_S 0.0005 /* the mouse read between vblanks no more often than this */

void port_controls_sample_mouse(void) {
    static double next_s;
    double now;
    float dx = 0.0f, dy = 0.0f; /* (pixels, with a high-resolution mouse's fractions) */

    if (diag_spin_rad_s() != 0.0) {
        if (lead_on() && in_gameplay() && diag_spin_add()) {
            lead_waiting();
        }
        return;
    }
    if (port_options.input != NULL || port_options.headless || !lead_on() ||
        pc.blocked || !pc.captured || !in_gameplay()) {
        return;
    }
    now = port_wall_seconds();
    if (now < next_s) {
        return;
    }
    next_s = now + SAMPLE_EVERY_S;
    SDL_PumpEvents(); /* (the other events wait in SDL's queue for libpad's poll at the vblank) */
    SDL_GetRelativeMouseState(&dx, &dy);
    if (dx == 0.0f && dy == 0.0f) {
        return;
    }
    pc.yaw += dx * RADIANS_PER_PIXEL * sensitivity;
    pc.pitch += (invert_y ? dy : -dy) * RADIANS_PER_PIXEL * sensitivity;
    lead_waiting();
}

/* Where the mouse is over the menus' 4:3 2D, as a fraction (port_menu_pointer_mouse): 0 when it is not over
   the window. With --widescreen the 2D is squeezed into the middle three quarters (port/platform/gs.c). */
static int mouse_over_2d(double *x, double *y) {
    SDL_Window *window = SDL_GetMouseFocus();
    float mouse_x = 0.0f, mouse_y = 0.0f;
    int out_w = 0, out_h = 0, place_x, place_y, place_w, place_h;

    if (window == NULL) {
        return 0;
    }
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(window, &out_w, &out_h);
    port_video_fit(out_w, out_h, &place_x, &place_y, &place_w, &place_h);
    if (place_w <= 0 || place_h <= 0) {
        return 0;
    }
    *x = (mouse_x - place_x + 0.5) / place_w;
    *y = (mouse_y - place_y + 0.5) / place_h;
    if (port_options.widescreen) {
        *x = (*x - 0.125) / 0.75;
    }
    return 1;
}

/* The menus' pointer (port/include/port/menu_pointer.h): the mouse and the left button's new presses, while the
   mouse is let go outside play; not under a pad script or a recording, whose pad alone must say what happens */
static void feed_menu_pointer(int pointing, unsigned int mouse) {
    static unsigned int previous_mouse;
    static double previous_x = -1.0, previous_y = -1.0;
    unsigned int left = 1u << SDL_BUTTON_LEFT;
    double x = -1.0, y = -1.0;
    int clicks;

    if (port_options.input != NULL || port_options.record_input != NULL) {
        return;
    }
    if (!pointing || !pc.focused || !mouse_over_2d(&x, &y)) {
        previous_mouse = mouse;
        port_menu_pointer_mouse(-1.0, -1.0, 0, 0);
        return;
    }
    clicks = (mouse & left) && !(previous_mouse & left);
    previous_mouse = mouse;
    port_menu_pointer_mouse(x, y, x != previous_x || y != previous_y, clicks);
    previous_x = x;
    previous_y = y;
}

/* The controller buttons bound to the hands mod's keys */
typedef struct {
    int count;
    int binding[HANDS_KEY_COUNT];
} HandsPadBindings;

static HandsPadBindings hands_pad_bindings(void) {
    HandsPadBindings list = {0, {0}};
    int action;
    int binding;

    for (action = 0; action < ACTION_COUNT; action++) {
        binding = bindings[action][BINDING_PAD_SLOT];
        if (hands_key_of(action) >= 0 && BINDING_KIND(binding) >= BINDING_PAD && list.count < HANDS_KEY_COUNT) {
            list.binding[list.count++] = binding;
        }
    }
    return list;
}

static int binding_listed(HandsPadBindings list, int binding) {
    int i;

    for (i = 0; i < list.count; i++) {
        if (list.binding[i] == binding) {
            return 1;
        }
    }
    return 0;
}

/* Once a vblank: the keyboard, mouse and controller into libpad's buttons and the sticks' keys */
void port_controls_read(unsigned int *buttons, int axis[4], void *sdl_controller) {
    SDL_Gamepad *controller = sdl_controller;
    const bool *keys = SDL_GetKeyboardState(NULL);
    uint32_t now = (uint32_t)SDL_GetTicks();
    unsigned int pressed = 0;     /* from the keyboard and mouse */
    unsigned int pad = 0;         /* from the controller */
    unsigned int mouse = 0;       /* bit n: SDL's mouse button n */
    unsigned int menu = 0;        /* the mouse outside play */
    uint64_t held_keys = 0;       /* actions, from the keyboard and mouse */
    uint64_t held_pad = 0;        /* actions, from the controller */
    uint64_t held;
    uint64_t edges;
    SDL_MouseButtonFlags state;
    float dx = 0.0f; /* (pixels, with a high-resolution mouse's fractions) */
    float dy = 0.0f;
    int gameplay = in_gameplay();
    int lent = port_overlay_holds_mouse(); /* the profiler has the mouse: it is let go, and nothing of it is read */
    HandsPadBindings hands_pad;
    int action;
    int slot;
    int bit;

    set_blocked(port_overlay_holds_input()); /* (after this poll's events: a layer they opened or closed counts) */
    state = SDL_GetRelativeMouseState(&dx, &dy);
    if (pc.blocked) {
        pc.wheel = 0;
        pc.previous_buttons = 0;
        pc.run = 0;
        return;
    }
    if (lent) {
        dx = dy = 0;
        pc.wheel = 0;
    }
    if (pc.focused && !lent) {
        for (bit = 1; bit <= 5; bit++) {
            if (state & SDL_BUTTON_MASK(bit)) {
                mouse |= 1u << bit;
            }
        }
    }
    /* what was held when the overlay closed does nothing until it is let go */
    if (pc.unblocked) {
        pc.swallowed |= mouse;
    }
    /* a button held when the mode changes does nothing until it is let go: a menu's confirmation does not become
       an attack */
    if (gameplay != pc.previous_gameplay) {
        pc.swallowed = mouse;
    }
    pc.swallowed &= mouse;
    mouse &= ~pc.swallowed;
    /* a click in play with the mouse let go takes it back, and does nothing else */
    if (gameplay && !pc.wants_capture && mouse != 0) {
        pc.wants_capture = 1;
        pc.swallowed |= mouse;
        mouse = 0;
    }
    if (gameplay) {
        pc.gameplay_ms = now;
    }
    /* out of play the mouse is let go (the cursor shown) only on a screen that takes pointing: a message
       (an item picked up, a sign read) keeps it held and the cursor hidden, its buttons still press. Let go
       only after a moment out of play, so a slow frame does not flash the cursor. */
    set_captured(!lent && pc.wants_capture && pc.focused &&
                 (gameplay || !port_menu_pointer_active() ||
                  (pc.captured && now - pc.gameplay_ms < RELEASE_AFTER_MS)));
    feed_menu_pointer(!gameplay && !pc.captured && !lent, mouse);
    if (!gameplay) {
        static const int menu_buttons[3] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE};

        for (bit = 0; bit < 3; bit++) {
            /* (where a screen takes pointing the left button is the pointer's: port/include/port/menu_pointer.h) */
            if (menu_buttons[bit] == SDL_BUTTON_LEFT && port_menu_pointer_active()) {
                continue;
            }
            if ((mouse >> menu_buttons[bit] & 1u) && menu_mouse[bit] >= 0) {
                menu |= actions[menu_mouse[bit]].bit;
            }
        }
        /* the table's mouse buttons press in play, with the mouse captured; out of play only the side
           buttons (L1 / R1: a menu's pages) */
        mouse &= 1u << SDL_BUTTON_X1 | 1u << SDL_BUTTON_X2;
    } else if (!pc.captured) {
        mouse = 0;
    }

    hands_pad = hands_pad_bindings();
    for (action = 0; action < ACTION_COUNT; action++) {
        for (slot = 0; slot < BINDING_SLOTS; slot++) {
            int binding = bindings[action][slot];

            /* the hands mod: a controller button of a hand's (the d-pad) is the hand's in play and the game's
               elsewhere (the menus' cursor) */
            if (port_hands.enabled && BINDING_KIND(binding) >= BINDING_PAD && binding_listed(hands_pad, binding) &&
                (hands_key_of(action) >= 0) != (in_gameplay() != 0)) {
                continue;
            }
            if (binding_down(binding, keys, mouse, controller)) {
                if (BINDING_KIND(binding) >= BINDING_PAD) {
                    held_pad |= (uint64_t)1 << action;
                } else {
                    held_keys |= (uint64_t)1 << action;
                }
            }
        }
    }
    held = held_keys | held_pad;
    if (pc.unblocked) {
        pc.swallowed_actions = held;
        pc.unblocked = 0;
    }
    pc.swallowed_actions &= held;
    held_keys &= ~pc.swallowed_actions;
    held_pad &= ~pc.swallowed_actions;
    held = held_keys | held_pad;
    for (action = 0; action < ACTION_COUNT; action++) {
        if (held_keys >> action & 1u) {
            pressed |= actions[action].bit;
        }
        if (held_pad >> action & 1u) {
            pad |= actions[action].bit;
        }
    }
    edges = held & ~pc.previous_actions;
    pc.previous_actions = held;
    pc.run = (held >> ACTION_RUN & 1u) != 0;

    /* the PC's own keys */
    if (edges >> ACTION_QUICK_SAVE & 1u) {
        port_pad_record_note("save state (a replay does not take it)");
        port_state_request_save(0);
        port_overlay_notify_state();
    }
    if (edges >> ACTION_QUICK_LOAD & 1u) {
        port_pad_record_note("load state: a replay goes wrong from here");
        port_state_request_load(0);
        port_overlay_notify_state();
    }
    if (edges >> ACTION_GAME_SAVE & 1u) {
        port_pad_record_event("game_save", port_quick_slot());
        port_quick_request_save(port_quick_slot());
        port_overlay_notify_quick();
    }
    if (edges >> ACTION_GAME_LOAD & 1u) {
        port_pad_record_event("game_load", port_quick_slot());
        port_quick_request_load(port_quick_slot());
        port_overlay_notify_quick();
    }
    if (edges >> ACTION_GS_CAPTURE & 1u) {
        port_debug_request_gs_capture();
    }
    if (edges >> ACTION_STATE_DUMP & 1u) {
        port_debug_write_state_dump();
    }
    if (edges >> ACTION_AUDIO_TAIL & 1u) {
        port_debug_write_audio_tail();
    }
    if (edges >> ACTION_FLASH_RECORD & 1u) {
        port_pad_record_note("F8 (flash record)");
        port_debug_flash_record();
    }
    if (edges >> ACTION_MAP & 1u) {
        port_automap_set_full_open(!port_automap_full_open()); /* the automap mod; closed by the overlay */
    }
    if (edges >> ACTION_SHOW_FPS & 1u) {
        port_options.show_fps = !port_options.show_fps;
        port_settings.show_fps = port_options.show_fps;
        port_settings_changed();
    }
    if (edges >> ACTION_STATS_PANEL & 1u) {
        port_stats_toggle_panel();
    }
    if (edges >> ACTION_CINEMATIC_CAMERA & 1u) {
        port_pad_record_event("cinematic_camera", 0);
        port_cinematic_key_toggle(); /* taken by game/cinematic.c; no notice, so nothing lands in the shot */
    }
    port_cinematic_set_held((int)(held >> ACTION_FLY_UP & 1u) - (int)(held >> ACTION_FLY_DOWN & 1u), pc.run);
    for (action = 0; action < ACTION_COUNT; action++) {
        int key = hands_key_of(action);

        if (key >= 0 && (edges >> action & 1u)) {
            port_pad_record_event(port_hands_event_name(key), 0);
            port_hands_key(key); /* the hands mod: taken by game/hands.c */
        }
    }
    if (edges >> ACTION_CAPTURE_MOUSE & 1u) {
        if (gameplay && !lent) {
            pc.wants_capture = !pc.captured;
            set_captured(pc.wants_capture && pc.focused);
        }
    }

    axis[0] = action_axis(held, ACTION_RIGHT_STICK_LEFT, ACTION_RIGHT_STICK_RIGHT);
    axis[1] = action_axis(held, ACTION_RIGHT_STICK_UP, ACTION_RIGHT_STICK_DOWN);
    axis[2] = action_axis(held, ACTION_LEFT_STICK_LEFT, ACTION_LEFT_STICK_RIGHT);
    axis[3] = action_axis(held, ACTION_LEFT_STICK_UP, ACTION_LEFT_STICK_DOWN);

    if (gameplay && pc.captured) {
        pc.yaw += dx * RADIANS_PER_PIXEL * sensitivity;
        pc.pitch += (invert_y ? dy : -dy) * RADIANS_PER_PIXEL * sensitivity;
    } else if (!gameplay || diag_spin_rad_s() == 0.0) {
        pc.yaw = pc.pitch = 0.0;
    }
    lead_waiting();
    if (!gameplay) {
        pressed |= menu;
        if (pc.wheel > 0) {
            pc.wheel_until[0] = now + TAP_MS;
        } else if (pc.wheel < 0) {
            pc.wheel_until[1] = now + TAP_MS;
        }
    }
    pc.wheel = 0;
    if ((int32_t)(pc.wheel_until[0] - now) > 0) {
        pressed |= B_UP;
    }
    if ((int32_t)(pc.wheel_until[1] - now) > 0) {
        pressed |= B_DOWN;
    }

    /* a tap is held 100 ms, so a frame of the game's (two vblanks or more) sees it; the movement
       keys are not, so letting go of them stops at once */
    for (bit = 0; bit < 16; bit++) {
        unsigned int mask = 1u << bit;

        if ((TAP_BUTTONS & mask) && (pressed & mask) && !(pc.previous_buttons & mask)) {
            pc.tap_until[bit] = now + TAP_MS;
        }
        if (pc.focused && (int32_t)(pc.tap_until[bit] - now) > 0) {
            *buttons |= mask;
        }
    }
    pc.previous_buttons = pressed;
    pc.previous_gameplay = gameplay;
    update_cursor();
    *buttons |= pressed | pad;
}
#endif
