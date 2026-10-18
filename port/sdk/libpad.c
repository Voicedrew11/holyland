/* libpad for the PC port (port/include/libpad.h): a DualShock 2 in port 0, slot 0, and nothing in
   port 1. Its buttons come from a pad script (--input), for tests, or else from the first SDL game
   controller.

   The pad behaves as a DualShock 2 does for libpad: digital (id 4, report type 0x41) or analog (id 7,
   0x73), pressure-sensitive (0x79) when the game asks, two motors, and each request finished at once
   (scePadGetReqState is always "done"). It starts analog, as a pad with its ANALOG light on: the title
   screen reads the left stick before the game asks for analog mode (Input_Update), and a digital pad
   sends no stick bytes, which the game reads as 0, the stick held up.

   A pad script holds lines of
       <vblank> <buttons> [<vblanks>]
   which hold the buttons, joined with '+' (START, CROSS+UP, ...; '-' for none), from that vblank for
   that many vblanks (6 when left out), and lines of
       <vblank> lx=<0..255> ly=.. rx=.. ry=.. [<vblanks>]
   which hold the sticks the same way (0x80 is the middle). Text from '#' on is a comment. The lines
   that cover a vblank add up: their buttons are all down. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef KF4_HAVE_SDL
#include <SDL.h>
#endif

#include "libpad.h"
#include "port/host.h"
#include "port/platform.h"

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
} PadInput;

typedef struct {
    uint64_t first;
    uint64_t count;
    PadInput input;
} ScriptLine;

static ScriptLine *script;
static int script_lines;

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
        for (word = strtok(text, " \t\r\n"); word != NULL; word = strtok(NULL, " \t\r\n")) {
            static const char *const axis_names[4] = {"rx=", "ry=", "lx=", "ly="};
            int axis;

            if (words == 0) {
                line.first = strtoull(word, NULL, 0);
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
        if (words == 0) {
            continue;
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
    }
    return input;
}

/* ---- SDL -------------------------------------------------------------------------------------- */
#ifdef KF4_HAVE_SDL
static SDL_GameController *controller;

static int stick_byte(Sint16 value) {
    return (value + 32768) >> 8;
}

static void sdl_read(void) {
    static const struct {
        SDL_GameControllerButton button;
        unsigned int bit;
    } map[] = {
        {SDL_CONTROLLER_BUTTON_BACK, BUTTON_SELECT},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, BUTTON_L3},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, BUTTON_R3},
        {SDL_CONTROLLER_BUTTON_START, BUTTON_START},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, BUTTON_UP},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, BUTTON_RIGHT},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, BUTTON_DOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, BUTTON_LEFT},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BUTTON_L1},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BUTTON_R1},
        {SDL_CONTROLLER_BUTTON_Y, BUTTON_TRIANGLE},
        {SDL_CONTROLLER_BUTTON_B, BUTTON_CIRCLE},
        {SDL_CONTROLLER_BUTTON_A, BUTTON_CROSS},
        {SDL_CONTROLLER_BUTTON_X, BUTTON_SQUARE},
    };
    size_t i;

    memset(&live, 0, sizeof(live));
    live.axis[0] = live.axis[1] = live.axis[2] = live.axis[3] = 0x80;
    if (controller == NULL) {
        return;
    }
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (SDL_GameControllerGetButton(controller, map[i].button)) {
            live.buttons |= map[i].bit;
        }
    }
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384) {
        live.buttons |= BUTTON_L2;
    }
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) {
        live.buttons |= BUTTON_R2;
    }
    live.axis[0] = stick_byte(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX));
    live.axis[1] = stick_byte(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
    live.axis[2] = stick_byte(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
    live.axis[3] = stick_byte(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
}

static void sdl_open_first(void) {
    int i;

    for (i = 0; i < SDL_NumJoysticks() && controller == NULL; i++) {
        if (SDL_IsGameController(i)) {
            controller = SDL_GameControllerOpen(i);
            if (controller) {
                port_event("pad: %s", SDL_GameControllerName(controller));
            }
        }
    }
}
#endif

static int use_sdl;

void port_pad_init(void) {
    live.axis[0] = live.axis[1] = live.axis[2] = live.axis[3] = 0x80;
    if (port_options.input) {
        load_script(port_options.input);
        return;
    }
#ifdef KF4_HAVE_SDL
    if (!port_options.headless) {
        if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "[port] SDL: %s; the pad stays idle\n", SDL_GetError());
            return;
        }
        use_sdl = 1;
        sdl_open_first();
    }
#endif
}

void port_pad_poll(void) {
#ifdef KF4_HAVE_SDL
    SDL_Event event;

    if (!use_sdl) {
        return;
    }
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_CONTROLLERDEVICEADDED && controller == NULL) {
            sdl_open_first();
        } else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller != NULL &&
                   event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))) {
            SDL_GameControllerClose(controller);
            controller = NULL;
            sdl_open_first();
        } else if (event.type == SDL_QUIT) {
            port_event("quit");
            exit(0);
        }
    }
    sdl_read();
#endif
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

int scePadSetActAlign(int port, int slot, const unsigned char *data) {
    (void)data;
    return is_our_pad(port, slot);
}

int scePadSetActDirect(int port, int slot, const unsigned char *data) {
    (void)data;
    return is_our_pad(port, slot);
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
