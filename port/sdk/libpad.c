/* libpad for the PC port (port/include/libpad.h): a DualShock 2 in port 0, slot 0, and nothing in
   port 1. Its buttons come from a pad script (--input), for tests, or else from the first SDL game
   controller, the keyboard and the mouse, by the bindings of port/sdk/controls.c (by default W A S D
   the left stick, the arrows the d-pad, the mouse turning and looking, Space square, E cross, Q
   triangle, Escape circle, Enter start).

   The pad behaves as a DualShock 2 does for libpad: digital (id 4, report type 0x41) or analog (id 7,
   0x73), pressure-sensitive (0x79) when the game asks, two motors (the SDL controller's rumble), and
   each request finished at once (scePadGetReqState is always "done"). It starts analog, as a pad with its ANALOG light on: the title
   screen reads the left stick before the game asks for analog mode (Input_Update), and a digital pad
   sends no stick bytes, which the game reads as 0, the stick held up.

   A pad script holds lines of
       <vblank> <buttons> [<vblanks>]
   which hold the buttons, joined with '+' (START, CROSS+UP, ...; '-' for none), from that vblank for
   that many vblanks (6 when left out), and lines of
       <vblank> lx=<0..255> ly=.. rx=.. ry=.. [<vblanks>]
   which hold the sticks the same way (0x80 is the middle). Text from '#' on is a comment. The lines
   that cover a vblank add up: their buttons are all down.

   --record-input FILE writes what the pad and keyboard send as such a script, a line per run of vblanks
   with the same buttons and sticks, so a played session can be replayed with --input. The PC's own
   controls stay on while recording (mouse look, the separate run key, twin-stick), and what they hand the game is written too, so that the replay gives the game the same:
       <vblank> <buttons> rx=.. ry=.. lx=.. ly=.. ts=<0|1> run=<0|1> <vblanks>
   ts= is whether twin-stick shaped the sticks (port_twin_stick_on), run= whether the separate run key
   was held (left out when the separate run mod did not apply: the action button runs), and
       <vblank> look <yaw> <pitch>       the mouse's turn one port_mouse_look call gave (C99 hex floats)
       <vblank> event <name> [<slot>]    a PC key the game sees: weapon_swap, spell_swap, hotkey_next,
                                         hotkey_previous (the hands mod), game_save, game_load
   A recording is the game's input per vblank, not per picture: it replays the same whatever the frame
   rate, headless or in a window, from the same start (the same --clock, --quick-load or save, and the
   data folder's settings, quick saves, automap and hands as they were). */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef KF4_HAVE_SDL
#include <SDL3/SDL.h>
#endif

#include "libpad.h"
#include "port/menu_pointer.h"
#include "port/twin_stick.h"
#include "port/controls.h"
#include "port/host.h"
#include "port/overlay.h"
#include "port/platform.h"
#include "port/quicksave.h"
#include "port/savestate.h"
#include "port/hands.h"

#define BUTTON_SELECT 0x0001
#define BUTTON_L3 0x0002
#define BUTTON_R3 0x0004
#define BUTTON_START 0x0008
#define BUTTON_UP 0x0010
#define BUTTON_RIGHT 0x0020
#define BUTTON_DOWN 0x0040
#define BUTTON_LEFT 0x0080
#define BUTTON_L2 0x0100
#define BUTTON_R2 0x0200
#define BUTTON_L1 0x0400
#define BUTTON_R1 0x0800
#define BUTTON_TRIANGLE 0x1000
#define BUTTON_CIRCLE 0x2000
#define BUTTON_CROSS 0x4000
#define BUTTON_SQUARE 0x8000

typedef struct {
    const char *name;
    unsigned int bit;
} ButtonName;

static const ButtonName button_names[] = {
    {"SELECT", BUTTON_SELECT}, {"L3", BUTTON_L3},       {"R3", BUTTON_R3},
    {"START", BUTTON_START},   {"UP", BUTTON_UP},       {"RIGHT", BUTTON_RIGHT},
    {"DOWN", BUTTON_DOWN},     {"LEFT", BUTTON_LEFT},   {"L2", BUTTON_L2},
    {"R2", BUTTON_R2},         {"L1", BUTTON_L1},       {"R1", BUTTON_R1},
    {"TRIANGLE", BUTTON_TRIANGLE}, {"CIRCLE", BUTTON_CIRCLE}, {"CROSS", BUTTON_CROSS},
    {"SQUARE", BUTTON_SQUARE},
};

/* What the pad sends: buttons (a set bit is down) and the sticks. */
typedef struct {
    unsigned int buttons;
    int axis[4]; /* rx, ry, lx, ly: -1 where the line does not set it */
    int twin;    /* ts=: twin-stick on (1) or off (0); -1 not set (a pad script's own rule) */
    int run;     /* run=: the separate run key held (1) or not (0); -1 not set (the action button runs) */
} PadInput;

typedef struct {
    uint64_t first;
    uint64_t count;
    PadInput input;
} ScriptLine;

static ScriptLine *script;
static int script_lines;

/* A recording's mouse turns and PC keys (look and event lines), in their order */
#define REPLAY_LOOK 0
#define REPLAY_HANDS 1 /* a hand key: the item's slot is the HandsKey */
#define REPLAY_GAME_SAVE 2
#define REPLAY_GAME_LOAD 3

typedef struct {
    uint64_t vblank;
    int kind; /* REPLAY_ */
    int slot;
    float yaw, pitch;
} ReplayItem;

static ReplayItem *looks, *events;
static int look_count, event_count, look_next, event_next;
static int script_has_looks; /* the script is a recording with mouse look's turns: port_mouse_look plays them */

/* ---- Recording (--record-input) ---------------------------------------------------------------- */
static FILE *record_file;
static PadInput record_run;
static uint64_t record_first;
static uint64_t record_count;

static void record_flush(void) {
    size_t i;
    int any = 0;

    if (record_file == NULL || record_count == 0) {
        return;
    }
    fprintf(record_file, "%llu ", (unsigned long long)record_first);
    for (i = 0; i < sizeof(button_names) / sizeof(button_names[0]); i++) {
        if (record_run.buttons & button_names[i].bit) {
            fprintf(record_file, "%s%s", any ? "+" : "", button_names[i].name);
            any = 1;
        }
    }
    if (!any) {
        fputc('-', record_file);
    }
    fprintf(record_file, " rx=%d ry=%d lx=%d ly=%d", record_run.axis[0], record_run.axis[1], record_run.axis[2],
            record_run.axis[3]);
    if (record_run.twin >= 0) {
        fprintf(record_file, " ts=%d", record_run.twin);
    }
    if (record_run.run >= 0) {
        fprintf(record_file, " run=%d", record_run.run);
    }
    fprintf(record_file, " %llu\n", (unsigned long long)record_count);
    fflush(record_file); /* (a crash keeps what was played) */
    record_count = 0;
}

static void record_close(void) {
    record_flush();
    if (record_file != NULL) {
        fclose(record_file);
        record_file = NULL;
    }
}

/* Once a vblank: what the game reads until the next one. The pad is read again within a vblank while the game
   waits (the overlay or full map holding it, pipelines being made): the last reading is what the game gets. */
static void record_vblank(const PadInput *input, uint64_t vblank) {
    if (record_count != 0 && record_first + record_count - 1 == vblank) {
        if (memcmp(input, &record_run, sizeof(record_run)) == 0) {
            return;
        }
        if (record_count == 1) {
            record_run = *input;
            return;
        }
        record_count--;
        record_flush();
        record_run = *input;
        record_first = vblank;
        record_count = 1;
        return;
    }
    if (record_count != 0 && memcmp(input, &record_run, sizeof(record_run)) == 0 &&
        record_first + record_count == vblank) {
        record_count++;
        return;
    }
    if (record_count != 0 && vblank < record_first + record_count && record_file != NULL) {
        fprintf(record_file, "# the vblank went back to %llu (a save state loaded): a replay goes wrong from here\n",
                (unsigned long long)vblank);
    }
    record_flush();
    record_run = *input;
    record_first = vblank;
    record_count = 1;
}

/* The pad's mode: 0 digital, 1 analog; pressure on top of analog. */
static int analog = 1;
static int pressure;
static PadInput live; /* from SDL */

/* ---- The pad script --------------------------------------------------------------------------- */
static unsigned int parse_buttons(const char *text, const char *path, int line_number) {
    unsigned int bits = 0;
    char word[32];

    if (strcmp(text, "-") == 0) {
        return 0;
    }
    while (*text) {
        size_t length = strcspn(text, "+");
        size_t i;
        int found = 0;

        if (length >= sizeof(word)) {
            length = sizeof(word) - 1;
        }
        for (i = 0; i < length; i++) {
            word[i] = (char)toupper((unsigned char)text[i]);
        }
        word[length] = 0;
        for (i = 0; i < sizeof(button_names) / sizeof(button_names[0]); i++) {
            if (strcmp(word, button_names[i].name) == 0) {
                bits |= button_names[i].bit;
                found = 1;
            }
        }
        if (!found) {
            PORT_FATAL("%s:%d: no button called %s", path, line_number, word);
        }
        text += length;
        if (*text == '+') {
            text++;
        }
    }
    return bits;
}

/* A look or event line (the rest of strtok's words) */
static void load_item(const ScriptLine *line, const char *word, const char *path, int line_number) {
    ReplayItem item;
    ReplayItem **list;
    int *count;
    char *a = strtok(NULL, " \t\r\n");
    char *b = strtok(NULL, " \t\r\n");

    memset(&item, 0, sizeof(item));
    item.vblank = line->first;
    if (strcmp(word, "look") == 0) {
        if (a == NULL || b == NULL) {
            PORT_FATAL("%s:%d: look wants a yaw and a pitch", path, line_number);
        }
        item.kind = REPLAY_LOOK;
        item.yaw = strtof(a, NULL);
        item.pitch = strtof(b, NULL);
        list = &looks;
        count = &look_count;
        script_has_looks = 1;
    } else {
        if (a == NULL) {
            PORT_FATAL("%s:%d: an event wants a name", path, line_number);
        }
        item.kind = strcmp(a, "game_save") == 0 ? REPLAY_GAME_SAVE : strcmp(a, "game_load") == 0 ? REPLAY_GAME_LOAD : -1;
        item.slot = b != NULL ? (int)strtol(b, NULL, 0) : 0;
        if (item.kind < 0) {
            item.slot = port_hands_event_key(a);
            item.kind = item.slot >= 0 ? REPLAY_HANDS : -1;
        }
        if (item.kind < 0) {
            PORT_FATAL("%s:%d: no event called %s", path, line_number, a);
        }
        list = &events;
        count = &event_count;
    }
    /* grown at each power of two */
    if ((*count & (*count - 1)) == 0 && *count >= 64) {
        *list = realloc(*list, (size_t)*count * 2 * sizeof(**list));
    } else if (*count == 0) {
        *list = malloc(64 * sizeof(**list));
    }
    if (*list == NULL) {
        PORT_FATAL("out of memory for the pad script");
    }
    (*list)[(*count)++] = item;
}

static void load_script(const char *path) {
    FILE *file = fopen(path, "r");
    char text[256];
    int line_number = 0;
    int capacity = 0;

    if (file == NULL) {
        PORT_FATAL("cannot open the pad script %s", path);
    }
    while (fgets(text, sizeof(text), file)) {
        ScriptLine line;
        char *hash = strchr(text, '#');
        char *word;
        int words = 0;

        line_number++;
        if (hash) {
            *hash = 0;
        }
        memset(&line, 0, sizeof(line));
        line.count = 6;
        line.input.axis[0] = line.input.axis[1] = line.input.axis[2] = line.input.axis[3] = -1;
        line.input.twin = line.input.run = -1;
        for (word = strtok(text, " \t\r\n"); word != NULL; word = strtok(NULL, " \t\r\n")) {
            static const char *const axis_names[4] = {"rx=", "ry=", "lx=", "ly="};
            int axis;

            if (words == 0) {
                line.first = strtoull(word, NULL, 0);
                words++;
                continue;
            }
            if (words == 1 && (strcmp(word, "look") == 0 || strcmp(word, "event") == 0)) {
                load_item(&line, word, path, line_number);
                words = -1;
                break;
            }
            if (strncmp(word, "ts=", 3) == 0) {
                line.input.twin = (int)strtol(word + 3, NULL, 0) != 0;
                words++;
                continue;
            }
            if (strncmp(word, "run=", 4) == 0) {
                line.input.run = (int)strtol(word + 4, NULL, 0) != 0;
                words++;
                continue;
            }
            for (axis = 0; axis < 4; axis++) {
                if (strncmp(word, axis_names[axis], 3) == 0) {
                    line.input.axis[axis] = (int)strtol(word + 3, NULL, 0) & 0xFF;
                    break;
                }
            }
            if (axis < 4) {
                words++;
                continue;
            }
            if (isdigit((unsigned char)word[0]) && words >= 2) {
                line.count = strtoull(word, NULL, 0);
            } else {
                line.input.buttons |= parse_buttons(word, path, line_number);
            }
            words++;
        }
        if (words <= 0) {
            continue; /* empty, or a look or event line */
        }
        if (words == 1) {
            PORT_FATAL("%s:%d: a vblank with nothing to press", path, line_number);
        }
        if (script_lines == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            script = realloc(script, (size_t)capacity * sizeof(*script));
            if (script == NULL) {
                PORT_FATAL("out of memory for the pad script");
            }
        }
        script[script_lines++] = line;
    }
    fclose(file);
}

static PadInput script_input(uint64_t vblank) {
    PadInput input;
    int i;

    memset(&input, 0, sizeof(input));
    input.axis[0] = input.axis[1] = input.axis[2] = input.axis[3] = 0x80;
    input.twin = input.run = -1;
    for (i = 0; i < script_lines; i++) {
        const ScriptLine *line = &script[i];
        int axis;

        if (vblank < line->first || vblank - line->first >= line->count) {
            continue;
        }
        input.buttons |= line->input.buttons;
        for (axis = 0; axis < 4; axis++) {
            if (line->input.axis[axis] >= 0) {
                input.axis[axis] = line->input.axis[axis];
            }
        }
        if (line->input.twin >= 0) {
            input.twin = line->input.twin;
        }
        if (line->input.run >= 0) {
            input.run = line->input.run;
        }
    }
    return input;
}

/* ---- What a recording hands the PC's controls (port/include/port/controls.h) ------------------- */
int port_pad_script_run(void) {
    return port_options.input ? script_input(port_vblank_count()).run : -1;
}

int port_pad_replay_look(float *yaw, float *pitch) {
    uint64_t vblank = port_vblank_count();
    static int warned;

    *yaw = *pitch = 0.0f;
    if (port_options.input == NULL || !script_has_looks) {
        return -1;
    }
    while (look_next < look_count && looks[look_next].vblank < vblank) {
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[pad] the replay is out of step: the mouse turn of vblank %llu was not taken by vblank %llu\n",
                    (unsigned long long)looks[look_next].vblank, (unsigned long long)vblank);
        }
        look_next++;
    }
    if (look_next < look_count && looks[look_next].vblank == vblank) {
        *yaw = looks[look_next].yaw;
        *pitch = looks[look_next].pitch;
        look_next++;
        return 1;
    }
    return 0;
}

void port_pad_record_look(float yaw, float pitch) {
    if (record_file != NULL) {
        fprintf(record_file, "%llu look %a %a\n", (unsigned long long)port_vblank_count(), (double)yaw, (double)pitch);
    }
}

void port_pad_record_event(const char *name, int slot) {
    if (record_file != NULL) {
        fprintf(record_file, "%llu event %s %d\n", (unsigned long long)port_vblank_count(), name, slot);
    }
}

void port_pad_record_note(const char *note) {
    if (record_file != NULL) {
        fprintf(record_file, "# vblank %llu: %s\n", (unsigned long long)port_vblank_count(), note);
        fflush(record_file);
    }
}

/* At each poll: the recording's PC keys up to this vblank */
static void replay_events(void) {
    uint64_t vblank = port_vblank_count();

    while (event_next < event_count && events[event_next].vblank <= vblank) {
        const ReplayItem *item = &events[event_next++];

        if (item->kind == REPLAY_HANDS) {
            port_hands_key((HandsKey)item->slot);
        } else if (item->kind == REPLAY_GAME_SAVE) {
            port_quick_request_save(item->slot);
        } else if (item->kind == REPLAY_GAME_LOAD) {
            port_quick_request_load(item->slot);
        }
    }
}

/* ---- SDL -------------------------------------------------------------------------------------- */
#ifdef KF4_HAVE_SDL
static SDL_Gamepad *controller;

static int stick_byte(Sint16 value) {
    return (value + 32768) >> 8;
}

static void sdl_read(void) {
    size_t i;

    memset(&live, 0, sizeof(live));
    live.axis[0] = live.axis[1] = live.axis[2] = live.axis[3] = 0x80;
    live.twin = live.run = -1;
    /* the buttons, and the sticks' keys; nothing while a layer over the game (a menu, the full map) has the input */
    port_controls_read(&live.buttons, live.axis, controller);
    if (controller == NULL || port_overlay_holds_input()) {
        return;
    }
    {
        static const SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
                                                       SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY};

        for (i = 0; i < 4; i++) {
            int value = stick_byte(SDL_GetGamepadAxis(controller, axes[i]));

            /* the stick, unless it rests in the middle and a key is held */
            if (live.axis[i] == 0x80 || value < 0x60 || value > 0xA0) {
                live.axis[i] = value;
            }
        }
    }
}

static void sdl_open_first(void) {
    int count = 0;
    int i;
    SDL_JoystickID *pads = SDL_GetGamepads(&count);

    if (pads == NULL) {
        return;
    }
    for (i = 0; i < count && controller == NULL; i++) {
        controller = SDL_OpenGamepad(pads[i]);
        if (controller) {
            const char *name = SDL_GetGamepadName(controller);

            port_event("pad: %s", name != NULL ? name : "(no name)");
        }
    }
    SDL_free(pads);
}
#endif

static int use_sdl;

/* (--record-input) at each poll: what the game reads until the next */
static void record_poll(void) {
    if (record_file != NULL) {
        PadInput recorded = live;

        recorded.twin = port_twin_stick_on();
        recorded.run = port_controls_run_state();
        record_vblank(&recorded, port_vblank_count());
    }
}

void port_pad_init(void) {
    live.axis[0] = live.axis[1] = live.axis[2] = live.axis[3] = 0x80;
    live.twin = live.run = -1;
    if (port_options.record_input != NULL) {
        if (port_options.input != NULL) {
            PORT_FATAL("--record-input records the live pad: it cannot be used with --input");
        }
        record_file = fopen(port_options.record_input, "w");
        if (record_file == NULL) {
            PORT_FATAL("cannot write the pad script %s", port_options.record_input);
        }
        fprintf(record_file, "# recorded with --record-input; play it back with --input (and the same --clock, card, --quick-load\n"
                             "# and data folder: settings, quick saves, automap, hands)\n");
        atexit(record_close);
    }
    port_controls_init();
    if (port_options.input) {
        load_script(port_options.input);
    }
#ifdef KF4_HAVE_SDL
    /* with a window, SDL's events are read even when a script plays the pad */
    if (!port_options.headless) {
        if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
            fprintf(stderr, "[port] SDL: %s; the pad stays idle\n", SDL_GetError());
            return;
        }
        use_sdl = 1;
        if (!port_options.input) {
            sdl_open_first();
        }
    }
#endif
}

/* (debugging, under a pad script) KF4_MENU_POINTER_SCRIPT: the mouse over the menus, to test their pointing in
   a scripted run (port/include/port/menu_pointer.h): "VBLANK:X,Y[,CLICKS];..." with X, Y in the 640 x 448
   picture's pixels; from each VBLANK on the mouse is there, and clicks there that many times at that vblank */
static void menu_pointer_script(uint64_t vblank) {
    static const char *script;
    static int checked;
    static double x = -1.0, y = -1.0;
    const char *cursor;
    int moved = 0;
    int clicks = 0;

    if (!checked) {
        script = getenv("KF4_MENU_POINTER_SCRIPT");
        checked = 1;
    }
    if (script == NULL) {
        return;
    }
    for (cursor = script; *cursor != '\0';) {
        unsigned long long at = 0;
        int px = 0, py = 0, count = 0, read = 0;

        if (sscanf(cursor, "%llu:%d,%d%n", &at, &px, &py, &read) < 3) {
            break;
        }
        cursor += read;
        if (*cursor == ',' && sscanf(cursor + 1, "%d%n", &count, &read) >= 1) {
            cursor += 1 + read;
        }
        if (at == vblank) {
            x = px / 640.0;
            y = py / 448.0;
            moved = 1;
            clicks += count;
        }
        while (*cursor == ';' || *cursor == ' ') {
            cursor++;
        }
    }
    port_menu_pointer_mouse(x, y, moved, clicks);
}

void port_pad_poll(void) {
#ifdef KF4_HAVE_SDL
    SDL_Event event;
#endif

    if (port_options.input) {
        replay_events();
        port_twin_stick_replayed(script_input(port_vblank_count()).twin);
        menu_pointer_script(port_vblank_count());
    }
#ifdef KF4_HAVE_SDL
    if (!use_sdl) {
        record_poll();
        return;
    }
    while (SDL_PollEvent(&event)) {
        if (port_options.input) {
            if (event.type == SDL_EVENT_QUIT) {
                exit(0);
            }
            continue;
        }
        if (port_controls_event(&event) || port_overlay_event(&event)) {
            continue;
        }
        if (event.type == SDL_EVENT_GAMEPAD_ADDED && controller == NULL) {
            sdl_open_first();
        } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && controller != NULL &&
                   event.gdevice.which == SDL_GetGamepadID(controller)) {
            SDL_CloseGamepad(controller);
            controller = NULL;
            sdl_open_first();
        } else if (event.type == SDL_EVENT_QUIT) {
            port_event("quit");
            exit(0);
        }
    }
    if (!port_options.input) {
        sdl_read();
    }
#endif
    record_poll();
}

/* ---- libpad ----------------------------------------------------------------------------------- */
static int is_our_pad(int port, int slot) {
    return port == 0 && slot == 0;
}

int scePadInit(int mode) {
    (void)mode;
    return 1;
}

int scePadPortOpen(int port, int slot, void *dma_buffer) {
    (void)port;
    (void)slot;
    (void)dma_buffer;
    return 1;
}

int scePadRead(int port, int slot, void *data) {
    unsigned char *out = data;
    PadInput input;
    unsigned int held;
    int i;

    memset(out, 0, 32);
    if (!is_our_pad(port, slot)) {
        out[0] = 0xFF;
        return 0;
    }
    input = port_options.input ? script_input(port_vblank_count()) : live;
    held = ~input.buttons & 0xFFFF; /* the pad sends a pressed button as 0 */
    out[0] = 0;
    out[1] = !analog ? 0x41 : pressure ? 0x79 : 0x73;
    out[2] = (unsigned char)held;
    out[3] = (unsigned char)(held >> 8);
    if (analog) {
        for (i = 0; i < 4; i++) {
            out[4 + i] = (unsigned char)input.axis[i];
        }
    }
    if (pressure) {
        /* right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 */
        static const unsigned int order[12] = {
            BUTTON_RIGHT, BUTTON_LEFT, BUTTON_UP, BUTTON_DOWN, BUTTON_TRIANGLE, BUTTON_CIRCLE,
            BUTTON_CROSS, BUTTON_SQUARE, BUTTON_L1, BUTTON_R1, BUTTON_L2, BUTTON_R2,
        };

        for (i = 0; i < 12; i++) {
            out[8 + i] = (input.buttons & order[i]) ? 0xFF : 0x00;
        }
    }
    return 32;
}

int scePadGetState(int port, int slot) {
    return is_our_pad(port, slot) ? scePadStateStable : scePadStateDiscon;
}

int scePadGetReqState(int port, int slot) {
    (void)port;
    (void)slot;
    return 0; /* scePadReqStateComplete */
}

/* term 1: the current mode's id; 2: its extended id; 3: the current mode's index; 4: the modes (index -1
   for how many, else the id of that one) */
int scePadInfoMode(int port, int slot, int term, int index) {
    if (!is_our_pad(port, slot)) {
        return 0;
    }
    switch (term) {
    case 1:
        return analog ? 7 : 4;
    case 2:
        return analog ? 7 : 0;
    case 3:
        return analog ? 1 : 0;
    case 4:
        if (index == -1) {
            return 2;
        }
        return index == 0 ? 4 : index == 1 ? 7 : 0;
    default:
        return 0;
    }
}

int scePadSetMainMode(int port, int slot, int mode, int lock) {
    (void)lock;
    if (!is_our_pad(port, slot)) {
        return 0;
    }
    analog = mode != 0;
    if (!analog) {
        pressure = 0;
    }
    return 1;
}

/* actno -1: how many motors; else term 0..3 of that motor (small, then big) */
int scePadInfoAct(int port, int slot, int actno, int term) {
    if (!is_our_pad(port, slot)) {
        return 0;
    }
    if (actno == -1) {
        return 2;
    }
    if (actno < 0 || actno > 1) {
        return -1;
    }
    switch (term) {
    case 0:
        return 0;            /* sub-device */
    case 1:
        return actno ? 1 : 0; /* 0: on/off, 1: speed */
    case 2:
        return actno ? 1 : 0; /* body side */
    case 3:
        return actno ? 1 : 0;
    default:
        return -1;
    }
}

/* Which motor each of the six bytes of scePadSetActDirect drives (0 small, 1 big, 0xFF none); a pad
   starts with none, as libpad's does until the game aligns them. */
static unsigned char act_align[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

int scePadSetActAlign(int port, int slot, const unsigned char *data) {
    int i;

    if (!is_our_pad(port, slot)) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        act_align[i] = data[i];
    }
    return 1;
}

/* The motors, on the SDL controller: the small motor is on or off (the game sends 0 or 1) and the big
   one has a speed (0..255). Input_Update drives them (Pad_SetRumble) when the game's vibration option is
   on, and Pad_Update sends them each frame while the pad is stable (Pad_Update), so a change is applied at once; the motors are re-armed now and then,
   with a time-out, so they stop if the game stops sending. Nothing here reaches the game. */
static void set_motors(int small, int big) {
#ifdef KF4_HAVE_SDL
    static int last_small = -1, last_big = -1, calls;

    if (controller == NULL || port_options.input) {
        return;
    }
    if (small == last_small && big == last_big && ++calls < 30) {
        return;
    }
    calls = 0;
    last_small = small;
    last_big = big;
    SDL_RumbleGamepad(controller, (Uint16)(big * 257), small ? 0xFFFF : 0, 1000);
#else
    (void)small;
    (void)big;
#endif
}

int scePadSetActDirect(int port, int slot, const unsigned char *data) {
    int small = 0, big = 0, i;

    if (!is_our_pad(port, slot)) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        if (act_align[i] == 0) {
            small = data[i] != 0;
        } else if (act_align[i] == 1) {
            big = data[i];
        }
    }
    set_motors(small, big);
    return 1;
}

int scePadInfoPressMode(int port, int slot) {
    return is_our_pad(port, slot) ? 1 : 0;
}

int scePadEnterPressMode(int port, int slot) {
    if (!is_our_pad(port, slot) || !analog) {
        return 0;
    }
    pressure = 1;
    return 1;
}

int scePadExitPressMode(int port, int slot) {
    if (!is_our_pad(port, slot)) {
        return 0;
    }
    pressure = 0;
    return 1;
}

/* ---- Save states: the mode the game set (the buttons are the host's, or the script's at a vblank) - */
void port_pad_state(PortStateIo *io) {
    PORT_STATE(io, analog);
    PORT_STATE(io, pressure);
    port_twin_stick_state(io);
}
