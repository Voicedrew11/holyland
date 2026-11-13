/* PC controls (port/include/port/controls.h): the keyboard, the mouse and a game controller as the
   pad, through binding tables the settings overlay rebinds (F1, Controls) and settings.json keeps.
   Two layouts, each with its own table:

   PS2 (the default), as port/sdk/libpad.c always had it:
       arrows      the d-pad              W A S D    the left stick   I J K L   the right stick
       Z or space  cross    X  circle     C  square  V  triangle
       Q  L1    E  R1    1  L2    3  R2   Enter  start  Backspace  select

   Verdite (--controls verdite), the keyboard layout of Verdite, the KF2/KF3 ports:
       W / Up        d-pad up (forward)       S / Down     d-pad down (back)
       Left / Right  d-pad left / right       A / D        L1 / R1 (side steps)
       Space         square (attack)          F            cross
       Q             triangle                 Tab          circle
       Enter         start                    Right Shift  select
       Escape        capture / release the mouse
   Mouse, captured, while the player is in control: moving turns and looks; left square, right
   triangle, middle cross (the mouse bindings of the table). Otherwise (menus, pause, dialogue): left
   cross, right circle, middle cross (the menu mouse), the wheel up / down.
   Outside play, too, A / D (the keys bound to L1 / R1) press d-pad left / right and Left / Right (the
   keys bound to d-pad left / right) press L1 / R1 (port_controls_menu_side_keys, on by default): A / D
   then move left and right in the menus as W / S move up and down (the equipment screen's items, a
   list's pages, the options' values, the King's Map), and the arrows do what L1 / R1 do there (turn
   the weapon on the equipment screen, zoom the King's Map). A key held as play starts or stops does
   nothing until it is let go. Not while a pad script plays or one is recorded.

   In both, with the separate run mod on (--separate-run, the Mods page), Left Shift and the
   controller's left stick press run while held, and the action button (cross; F in Verdite) only
   uses things. Off, Run is not used and the action button runs, as on the PS2.

   In both, twin-stick controls (port/include/port/twin_stick.h, on by default) make the left stick
   (W A S D in the PS2 layout) walk and step sideways and the right stick (I J K L) turn and look.

   In both, a controller's buttons are as the PS2 pad's (A cross, B circle, X square, Y triangle, the
   shoulders and triggers L1 R1 L2 R2), F2 / F4 take and load a quick save (port/platform/quicksave.c),
   F5 / F7 save and load the quick slot's save state, and F3 shows or hides the frame-rate counter.

   Mouse look turns the player in Player_Move, where the game adds the pad's turning (port_mouse_look):
   each movement is applied once, so it neither coasts nor builds speed. Capture is manual (Escape),
   as in Verdite; losing the window's focus releases it. A small mouse in the corner shows the change.
   A pad script (--input) turns all of this off, so scripted runs stay the same. */
#include <stdio.h>
#include <string.h>

#include "port/automap.h"
#include "port/controls.h"
#include "port/debug.h"
#include "port/host.h"
#include "port/mouse_lead.h"
#include "port/overlay.h"
#include "port/platform.h"
#include "port/savestate.h"
#include "port/quicksave.h"
#include "port/settings.h"
#include "port/stats.h"
#include "port/weapon_sets.h"

#ifdef KF4_HAVE_SDL
#include <SDL.h>
#endif

#define PI 3.14159265358979323846
#define RADIANS_PER_PIXEL (0.15 * PI / 180.0)
#define MAX_TURN (PI / 2.0)

static struct {
    int layout;
    int captured;
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
    int previous_side;           /* the side keys were swapped at the last read (port_controls_menu_side_keys) */
    uint64_t swallowed_side;     /* side keys held across that changing, ignored until released */
    uint64_t previous_actions;
    uint64_t swallowed_actions;  /* actions held when the overlay closed */
    int wheel;                   /* wheel movement since the last read: +1 up, -1 down */
    uint32_t tap_until[16];      /* a short press is held this long, so the game's frame sees it */
    uint32_t wheel_until[2];
    uint32_t indicator_time;     /* when the capture last changed (0: never) */
    int indicator_captured;
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
    [ACTION_CAPTURE_MOUSE] = {"capture_mouse", "Capture / release the mouse", 0},
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
    [ACTION_WEAPON_SWAP] = {"weapon_swap", "Swap weapon sets (mod)", 0},
};

const char *port_controls_action_label(int action) {
    return action >= 0 && action < ACTION_COUNT ? actions[action].label : "?";
}

const char *port_controls_action_id(int action) {
    return action >= 0 && action < ACTION_COUNT ? actions[action].id : "?";
}

int port_controls_action_used(int layout, int action) {
    if (action == ACTION_CAPTURE_MOUSE) {
        return layout == CONTROLS_VERDITE;
    }
    if (action == ACTION_RUN) {
        return port_options.separate_run;
    }
    if (action == ACTION_STATS_PANEL) {
        return port_stats_options.on;
    }
    if (action == ACTION_MAP) {
        return port_automap.enabled;
    }
    if (action == ACTION_WEAPON_SWAP) {
        return port_weapon_sets.enabled;
    }
    return action >= 0 && action < ACTION_COUNT;
}

/* ---- The layouts' tables ---------------------------------------------------------------------- */
#ifdef KF4_HAVE_SDL
#define KEY(name) (BINDING_KEY | SDL_SCANCODE_##name)
#define MOUSE(n) (BINDING_MOUSE | (n))
#define PAD(name) (BINDING_PAD | SDL_CONTROLLER_BUTTON_##name)
#define TRIGGER(name) (BINDING_TRIGGER | SDL_CONTROLLER_AXIS_TRIGGER##name)
#else
#define KEY(name) 0
#define MOUSE(n) 0
#define PAD(name) 0
#define TRIGGER(name) 0
#endif

static const int default_bindings[CONTROLS_LAYOUTS][ACTION_COUNT][BINDING_SLOTS] = {
    [CONTROLS_PS2] = {
        [ACTION_UP] = {KEY(UP), 0, PAD(DPAD_UP)},
        [ACTION_DOWN] = {KEY(DOWN), 0, PAD(DPAD_DOWN)},
        [ACTION_LEFT] = {KEY(LEFT), 0, PAD(DPAD_LEFT)},
        [ACTION_RIGHT] = {KEY(RIGHT), 0, PAD(DPAD_RIGHT)},
        [ACTION_CROSS] = {KEY(Z), KEY(SPACE), PAD(A)},
        [ACTION_CIRCLE] = {KEY(X), 0, PAD(B)},
        [ACTION_SQUARE] = {KEY(C), 0, PAD(X)},
        [ACTION_TRIANGLE] = {KEY(V), 0, PAD(Y)},
        [ACTION_L1] = {KEY(Q), 0, PAD(LEFTSHOULDER)},
        [ACTION_R1] = {KEY(E), 0, PAD(RIGHTSHOULDER)},
        [ACTION_L2] = {KEY(1), 0, TRIGGER(LEFT)},
        [ACTION_R2] = {KEY(3), 0, TRIGGER(RIGHT)},
        [ACTION_L3] = {0, 0, PAD(LEFTSTICK)},
        [ACTION_R3] = {0, 0, PAD(RIGHTSTICK)},
        [ACTION_START] = {KEY(RETURN), 0, PAD(START)},
        [ACTION_SELECT] = {KEY(BACKSPACE), 0, PAD(BACK)},
        [ACTION_RUN] = {KEY(LSHIFT), 0, PAD(LEFTSTICK)},
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
        [ACTION_WEAPON_SWAP] = {KEY(R), 0, PAD(RIGHTSTICK)},
    },
    [CONTROLS_VERDITE] = {
        [ACTION_UP] = {KEY(W), KEY(UP), PAD(DPAD_UP)},
        [ACTION_DOWN] = {KEY(S), KEY(DOWN), PAD(DPAD_DOWN)},
        [ACTION_LEFT] = {KEY(LEFT), 0, PAD(DPAD_LEFT)},
        [ACTION_RIGHT] = {KEY(RIGHT), 0, PAD(DPAD_RIGHT)},
        [ACTION_CROSS] = {KEY(F), MOUSE(2), PAD(A)},
        [ACTION_CIRCLE] = {KEY(TAB), 0, PAD(B)},
        [ACTION_SQUARE] = {KEY(SPACE), MOUSE(1), PAD(X)},
        [ACTION_TRIANGLE] = {KEY(Q), MOUSE(3), PAD(Y)},
        [ACTION_L1] = {KEY(A), 0, PAD(LEFTSHOULDER)},
        [ACTION_R1] = {KEY(D), 0, PAD(RIGHTSHOULDER)},
        [ACTION_L2] = {0, 0, TRIGGER(LEFT)},
        [ACTION_R2] = {0, 0, TRIGGER(RIGHT)},
        [ACTION_L3] = {0, 0, PAD(LEFTSTICK)},
        [ACTION_R3] = {0, 0, PAD(RIGHTSTICK)},
        [ACTION_START] = {KEY(RETURN), 0, PAD(START)},
        [ACTION_SELECT] = {KEY(RSHIFT), 0, PAD(BACK)},
        [ACTION_RUN] = {KEY(LSHIFT), 0, PAD(LEFTSTICK)},
        [ACTION_CAPTURE_MOUSE] = {KEY(ESCAPE)},
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
        [ACTION_WEAPON_SWAP] = {KEY(R), 0, PAD(RIGHTSTICK)},
    },
};
/* Verdite's mouse outside play: left cross, right circle, middle cross */
static const int default_menu_mouse[3] = {ACTION_CROSS, ACTION_CIRCLE, ACTION_CROSS};

/* The tables in use: the defaults until settings.json (port/overlay/settings.c) or the overlay changes
   them, which may be before port_controls_init */
static int tables_ready;
static int bindings[CONTROLS_LAYOUTS][ACTION_COUNT][BINDING_SLOTS];
static int menu_mouse[3];
static int menu_side_keys;
static double sensitivity;
static int invert_y;
static int mouse_lead = 1; /* instant mouse look (port/include/port/mouse_lead.h) */

static void reset_layout(int layout) {
    memcpy(bindings[layout], default_bindings[layout], sizeof(bindings[layout]));
    if (layout == CONTROLS_VERDITE) {
        memcpy(menu_mouse, default_menu_mouse, sizeof(menu_mouse));
        menu_side_keys = 1;
        sensitivity = 1.0;
        invert_y = 0;
    }
}

static void tables_init(void) {
    int layout;

    if (tables_ready) {
        return;
    }
    for (layout = 0; layout < CONTROLS_LAYOUTS; layout++) {
        reset_layout(layout);
    }
    tables_ready = 1;
}

void port_controls_reset(int layout) {
    tables_init();
    if (layout >= 0 && layout < CONTROLS_LAYOUTS) {
        reset_layout(layout);
    }
}

int port_controls_binding(int layout, int action, int slot) {
    if (layout < 0 || layout >= CONTROLS_LAYOUTS || action < 0 || action >= ACTION_COUNT || slot < 0 ||
        slot >= BINDING_SLOTS) {
        return BINDING_NONE;
    }
    tables_init();
    return bindings[layout][action][slot];
}

void port_controls_bind(int layout, int action, int slot, int binding) {
    if (layout < 0 || layout >= CONTROLS_LAYOUTS || action < 0 || action >= ACTION_COUNT || slot < 0 ||
        slot >= BINDING_SLOTS) {
        return;
    }
    tables_init();
    bindings[layout][action][slot] = binding;
}

int port_controls_is_default(int layout, int action) {
    tables_init();
    return memcmp(bindings[layout][action], default_bindings[layout][action], sizeof(bindings[layout][action])) == 0;
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

int port_controls_menu_side_keys(void) {
    tables_init();
    return menu_side_keys;
}

void port_controls_set_menu_side_keys(int on) {
    tables_init();
    menu_side_keys = on != 0;
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

    if (mouse_lead) {
        waiting.yaw_rad = held_turn(pc.yaw);
        waiting.pitch_rad = held_turn(pc.pitch);
    }
    port_mouse_lead_waiting(waiting, reading_time());
}

/* ---- Layouts ---------------------------------------------------------------------------------- */
static const char *const layout_names[CONTROLS_LAYOUTS] = {"ps2", "verdite"};

const char *port_controls_layout_name(int layout) {
    return layout >= 0 && layout < CONTROLS_LAYOUTS ? layout_names[layout] : "?";
}

int port_controls_layout_from_name(const char *name) {
    int i;

    for (i = 0; name != NULL && i < CONTROLS_LAYOUTS; i++) {
        if (strcmp(name, layout_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void set_captured(int captured);

void port_controls_init(const char *layout) {
    memset(&pc, 0, sizeof(pc));
    pc.focused = 1;
    pc.listen_action = -1;
    tables_init();
    pc.layout = layout == NULL ? CONTROLS_PS2 : port_controls_layout_from_name(layout);
    if (pc.layout < 0) {
        PORT_FATAL("--controls: \"%s\" is not a layout (ps2, verdite)", layout);
    }
}

int port_controls_layout(void) {
    return pc.layout;
}

void port_controls_set_layout(int layout) {
    if (layout < 0 || layout >= CONTROLS_LAYOUTS || layout == pc.layout) {
        return;
    }
    set_captured(0);
    pc.layout = layout;
}

int port_controls_verdite(void) {
    return pc.layout == CONTROLS_VERDITE && port_options.input == NULL;
}

/* The player was in control at the game's last frame */
static int in_gameplay(void) {
    return pc.seen_look && pc.look_gameplay && port_vblank_count() - pc.look_vblank <= 2;
}

int port_mouse_look(int gameplay, float *yaw, float *pitch) {
    double y;
    double p;

    *yaw = *pitch = 0.0f;
    if (!port_controls_verdite() || port_options.record_input != NULL) {
        MouseTurn none = {0, 0};

        port_mouse_lead_waiting(none, reading_time()); /* (no mouse look: nothing ahead) */
        return 0;
    }
    pc.seen_look = 1;
    pc.look_vblank = port_vblank_count();
    pc.look_gameplay = gameplay;
    if (!gameplay || !pc.captured) {
        pc.yaw = pc.pitch = 0.0;
        lead_waiting();
        return 0;
    }
    y = held_turn(pc.yaw);
    p = held_turn(pc.pitch);
    pc.yaw = pc.pitch = 0.0;
    if (y == 0.0 && p == 0.0) {
        return 0;
    }
    if (mouse_lead) {
        MouseTurn spent = {y, p};

        port_mouse_lead_spent(spent); /* (out of what waited: the hand is where it was) */
    } else {
        lead_waiting();
    }
    *yaw = (float)y;
    *pitch = (float)p;
    return 1;
}

/* Running is Run's with the mod on and Run bound, and no pad script playing or recorded */
unsigned int port_controls_run_held(unsigned int held, unsigned int action_bit) {
    int slot;

    if (!port_options.separate_run || port_options.input != NULL || port_options.record_input != NULL) {
        return held;
    }
    for (slot = 0; slot < BINDING_SLOTS; slot++) {
        if (port_controls_binding(pc.layout, ACTION_RUN, slot) != BINDING_NONE) {
            return pc.run ? held | action_bit : held & ~action_bit;
        }
    }
    return held;
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

void port_controls_block(int blocked) {
    (void)blocked;
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
    case SDL_CONTROLLER_BUTTON_A: return "Pad A";
    case SDL_CONTROLLER_BUTTON_B: return "Pad B";
    case SDL_CONTROLLER_BUTTON_X: return "Pad X";
    case SDL_CONTROLLER_BUTTON_Y: return "Pad Y";
    case SDL_CONTROLLER_BUTTON_BACK: return "Pad Back";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "Pad Guide";
    case SDL_CONTROLLER_BUTTON_START: return "Pad Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Pad left stick";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Pad right stick";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "Pad LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Pad RB";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "Pad up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "Pad down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "Pad left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "Pad right";
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
            name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)code);
            snprintf(out, size, "Pad %s", name ? name : "?");
        } else {
            snprintf(out, size, "%s", name);
        }
        return;
    case BINDING_TRIGGER:
        snprintf(out, size, "%s", code == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? "Pad LT" : "Pad RT");
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
        name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)code);
        snprintf(out, size, "pad:%s", name ? name : "");
        return;
    case BINDING_TRIGGER:
        name = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)code);
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
        SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(text + 4);

        if (button == SDL_CONTROLLER_BUTTON_INVALID) {
            return 0;
        }
        *binding = BINDING_PAD | (int)button;
        return 1;
    }
    if (strncmp(text, "trigger:", 8) == 0) {
        SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(text + 8);

        if (axis != SDL_CONTROLLER_AXIS_TRIGGERLEFT && axis != SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
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
    pc.listen_until = SDL_GetTicks() + LISTEN_MS;
}

int port_controls_listening(int *action, int *slot) {
    if (pc.listen_action >= 0 && (int32_t)(pc.listen_until - SDL_GetTicks()) <= 0) {
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
    bindings[pc.layout][pc.listen_action][pc.listen_slot] = binding;
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
    case SDL_KEYDOWN:
        if (event->key.repeat) {
            return 1;
        }
        if (event->key.keysym.scancode == SDL_SCANCODE_F1) {
            pc.listen_action = -1;
        } else if (event->key.keysym.scancode == SDL_SCANCODE_DELETE) {
            listened(BINDING_NONE);
        } else if (!pad_slot) {
            listened(BINDING_KEY | event->key.keysym.scancode);
        }
        return 1;
    case SDL_MOUSEBUTTONDOWN:
        if (!pad_slot && event->button.button >= 1 && event->button.button <= 5) {
            listened(BINDING_MOUSE | event->button.button);
        }
        return 1;
    case SDL_CONTROLLERBUTTONDOWN:
        if (pad_slot) {
            listened(BINDING_PAD | event->cbutton.button);
        }
        return 1;
    case SDL_CONTROLLERAXISMOTION:
        if (pad_slot && event->caxis.value > TRIGGER_DOWN &&
            (event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
             event->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
            listened(BINDING_TRIGGER | event->caxis.axis);
        }
        return 1;
    case SDL_TEXTINPUT:
    case SDL_MOUSEWHEEL:
        return 1;
    default:
        return 0;
    }
}

/* ---- Reading ---------------------------------------------------------------------------------- */
static void set_captured(int captured) {
    if (captured == pc.captured) {
        return;
    }
    pc.captured = captured;
    pc.yaw = pc.pitch = 0.0;
    lead_waiting();
    SDL_SetRelativeMouseMode(captured ? SDL_TRUE : SDL_FALSE);
    SDL_GetRelativeMouseState(NULL, NULL); /* motion before the change is dropped */
    pc.indicator_time = SDL_GetTicks() | 1u;
    pc.indicator_captured = captured;
}

void port_controls_block(int blocked) {
    if (blocked && !pc.blocked) {
        set_captured(0);
    }
    if (!blocked && pc.blocked) {
        pc.unblocked = 1;
    }
    pc.blocked = blocked;
}

int port_controls_event(const void *sdl_event) {
    const SDL_Event *event = sdl_event;

    if (listen_event(event)) {
        return 1;
    }
    if (event->type == SDL_WINDOWEVENT) {
        if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            pc.focused = 0;
            set_captured(0);
        } else if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            pc.focused = 1;
        }
    }
    if (event->type == SDL_MOUSEWHEEL && port_controls_verdite() && !pc.blocked) {
        pc.wheel += event->wheel.y > 0 ? 1 : event->wheel.y < 0 ? -1 : 0;
    }
    return 0;
}

static int binding_down(int binding, const Uint8 *keys, unsigned int mouse, SDL_GameController *controller) {
    int code = BINDING_CODE(binding);

    switch (BINDING_KIND(binding)) {
    case BINDING_KEY:
        return pc.focused && code < SDL_NUM_SCANCODES && keys[code];
    case BINDING_MOUSE:
        return (mouse >> code & 1u) != 0;
    case BINDING_PAD:
        return controller != NULL && SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)code);
    case BINDING_TRIGGER:
        return controller != NULL && SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)code) > TRIGGER_DOWN;
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

#define SIDE_ACTIONS \
    ((uint64_t)1 << ACTION_LEFT | (uint64_t)1 << ACTION_RIGHT | (uint64_t)1 << ACTION_L1 | (uint64_t)1 << ACTION_R1)

/* Verdite outside play (port_controls_menu_side_keys): the keys' L1 / R1 become d-pad left / right and
   their d-pad left / right L1 / R1. A side key held when that changes does nothing until it is let go,
   so a side step held as the menu opens does not move its cursor. */
static void side_keys(uint64_t *held_keys, int outside_play) {
    int side = outside_play && menu_side_keys && port_options.input == NULL && port_options.record_input == NULL;
    uint64_t held = *held_keys;

    if (side != pc.previous_side) {
        pc.swallowed_side = held & SIDE_ACTIONS;
        pc.previous_side = side;
    }
    pc.swallowed_side &= held;
    held &= ~pc.swallowed_side;
    if (side) {
        uint64_t moved = held & ~SIDE_ACTIONS;

        moved |= (held >> ACTION_L1 & 1u) << ACTION_LEFT | (held >> ACTION_R1 & 1u) << ACTION_RIGHT;
        moved |= (held >> ACTION_LEFT & 1u) << ACTION_L1 | (held >> ACTION_RIGHT & 1u) << ACTION_R1;
        held = moved;
    }
    *held_keys = held;
}

#define SAMPLE_EVERY_S 0.0005 /* the mouse read between vblanks no more often than this */

void port_controls_sample_mouse(void) {
    static double next_s;
    double now;
    int dx = 0, dy = 0;

    if (pc.layout != CONTROLS_VERDITE || port_options.input != NULL || port_options.headless || !mouse_lead ||
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
    if (dx == 0 && dy == 0) {
        return;
    }
    pc.yaw += dx * RADIANS_PER_PIXEL * sensitivity;
    pc.pitch += (invert_y ? dy : -dy) * RADIANS_PER_PIXEL * sensitivity;
    lead_waiting();
}

/* Once a vblank: the keyboard, mouse and controller into libpad's buttons and the sticks' keys */
void port_controls_read(unsigned int *buttons, int axis[4], void *sdl_controller) {
    SDL_GameController *controller = sdl_controller;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int verdite = pc.layout == CONTROLS_VERDITE;
    uint32_t now = SDL_GetTicks();
    unsigned int pressed = 0;     /* from the keyboard and mouse */
    unsigned int pad = 0;         /* from the controller */
    unsigned int mouse = 0;       /* bit n: SDL's mouse button n */
    unsigned int menu = 0;        /* Verdite's mouse outside play */
    uint64_t held_keys = 0;       /* actions, from the keyboard and mouse */
    uint64_t held_pad = 0;        /* actions, from the controller */
    uint64_t held;
    uint64_t edges;
    Uint32 state;
    int dx = 0;
    int dy = 0;
    int gameplay = verdite && in_gameplay();
    int action;
    int slot;
    int bit;

    state = SDL_GetRelativeMouseState(&dx, &dy);
    if (pc.blocked) {
        pc.wheel = 0;
        pc.previous_buttons = 0;
        pc.run = 0;
        return;
    }
    if (pc.focused) {
        for (bit = 1; bit <= 5; bit++) {
            if (state & SDL_BUTTON(bit)) {
                mouse |= 1u << bit;
            }
        }
    }
    /* what was held when the overlay closed does nothing until it is let go */
    if (pc.unblocked) {
        pc.swallowed |= mouse;
    }
    if (verdite) {
        /* a button held when the mode changes does nothing until it is let go: a menu's confirmation
           does not become an attack */
        if (gameplay != pc.previous_gameplay) {
            pc.swallowed = mouse;
        }
    }
    pc.swallowed &= mouse;
    mouse &= ~pc.swallowed;
    if (verdite) {
        if (!gameplay) {
            static const int menu_buttons[3] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE};

            for (bit = 0; bit < 3; bit++) {
                if ((mouse >> menu_buttons[bit] & 1u) && menu_mouse[bit] >= 0) {
                    menu |= actions[menu_mouse[bit]].bit;
                }
            }
            /* the table's mouse buttons press in play, with the mouse captured */
            mouse = 0;
        } else if (!pc.captured) {
            mouse = 0;
        }
    }

    for (action = 0; action < ACTION_COUNT; action++) {
        for (slot = 0; slot < BINDING_SLOTS; slot++) {
            int binding = bindings[pc.layout][action][slot];

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
    side_keys(&held_keys, verdite && !gameplay);
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
        port_state_request_save(0);
        port_overlay_notify_state();
    }
    if (edges >> ACTION_QUICK_LOAD & 1u) {
        port_state_request_load(0);
        port_overlay_notify_state();
    }
    if (edges >> ACTION_GAME_SAVE & 1u) {
        port_quick_request_save(port_quick_slot());
        port_overlay_notify_quick();
    }
    if (edges >> ACTION_GAME_LOAD & 1u) {
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
    if (edges >> ACTION_WEAPON_SWAP & 1u) {
        port_weapon_sets_key(); /* the weapon sets mod: taken in play by game/weapon_sets.c */
    }
    if (verdite && (edges >> ACTION_CAPTURE_MOUSE & 1u)) {
        set_captured(!pc.captured);
    }

    axis[0] = action_axis(held, ACTION_RIGHT_STICK_LEFT, ACTION_RIGHT_STICK_RIGHT);
    axis[1] = action_axis(held, ACTION_RIGHT_STICK_UP, ACTION_RIGHT_STICK_DOWN);
    axis[2] = action_axis(held, ACTION_LEFT_STICK_LEFT, ACTION_LEFT_STICK_RIGHT);
    axis[3] = action_axis(held, ACTION_LEFT_STICK_UP, ACTION_LEFT_STICK_DOWN);

    if (!verdite) {
        *buttons |= pressed | pad;
        return;
    }

    if (gameplay && pc.captured) {
        pc.yaw += dx * RADIANS_PER_PIXEL * sensitivity;
        pc.pitch += (invert_y ? dy : -dy) * RADIANS_PER_PIXEL * sensitivity;
    } else {
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
    *buttons |= pressed | pad;
}

/* The mouse in the window's top right corner when the capture changes: fades in over 140 ms, stays
   1100 ms, fades out over 420 ms. A cut across it means released. The glyph and timing are Verdite2's
   (patches/MouseIndicator.cs, MIT, Voicedrew11). */
int port_controls_indicator_shown(void) {
    return port_controls_verdite() && pc.indicator_time != 0 && SDL_GetTicks() - pc.indicator_time < 1660;
}

void port_controls_draw(PortFillRect fill, void *context, int x, int y, int width, int height) {
    static const char *const glyph[13] = {
        "....#....", "....#....", "...###...", "..#####..", ".##.#.##.", ".##.#.##.", ".#######.",
        ".#######.", ".#######.", ".#######.", ".#######.", "..#####..", "...###...",
    };
    uint32_t age;
    double alpha;
    int cell;
    int margin;
    int origin_x;
    int origin_y;
    int pass;

    if (!port_controls_indicator_shown() || width < 64 || height < 64) {
        return;
    }
    age = SDL_GetTicks() - pc.indicator_time;
    alpha = age < 140 ? age / 140.0 : age < 1240 ? 1.0 : (1660 - age) / 420.0;
    cell = height / 180 > 2 ? height / 180 : 2;
    margin = height * 3 / 100 > cell * 2 ? height * 3 / 100 : cell * 2;
    origin_x = x + width - margin - cell * 9;
    /* below the frame-rate counter when it is shown (port/platform/video.c) */
    origin_y = y + margin + port_video_counter_height(height);
    if (origin_y != y + margin) {
        origin_y += cell * 2;
    }
    for (pass = 0; pass < 2; pass++) {
        int offset = pass == 0 ? 1 : 0;
        /* a shadow, then the glyph */
        unsigned int rgba = pass == 0 ? (unsigned int)(255.0 * 0.55 * alpha) << 24
                                      : ((unsigned int)(255.0 * alpha) << 24) | 0xFFFFFFu;
        int row;

        for (row = 0; row < 13; row++) {
            int column;

            for (column = 0; column < 9; column++) {
                if (glyph[row][column] != '#') {
                    continue;
                }
                if (!pc.indicator_captured && (row - column == 4 || row - column == 5)) {
                    continue;
                }
                fill(context, origin_x + (column + offset) * cell, origin_y + (row + offset) * cell, cell, cell, rgba);
            }
        }
    }
}
#endif
