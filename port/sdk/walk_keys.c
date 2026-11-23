/* The keyboard's walking (port/include/port/walk_keys.h): W A S D as the game's own walking buttons, or with
   instant WASD acceleration the left stick at full throw. */
#include <stddef.h>

#include "port/platform.h"
#include "port/walk_keys.h"

#define PAD_UP 0x0010u
#define PAD_RIGHT 0x0020u
#define PAD_DOWN 0x0040u
#define PAD_LEFT 0x0080u
#define PAD_L2 0x0100u
#define PAD_R2 0x0200u
#define PAD_L1 0x0400u
#define PAD_R1 0x0800u

/* Input_SetDefaultControls's */
static MoveButtons game_buttons = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_L1, PAD_R1, PAD_R2, PAD_L2};
static int instant_setting = 0;

void port_walk_keys_set_buttons(MoveButtons buttons) {
    game_buttons = buttons;
}

MoveButtons port_walk_keys_buttons(void) {
    return game_buttons;
}

int port_walk_keys_instant_setting(void) {
    return instant_setting;
}

void port_walk_keys_set_instant(int on) {
    instant_setting = on != 0;
}

int port_walk_keys_instant(void) {
    return instant_setting && port_options.input == NULL && port_options.record_input == NULL;
}

/* One stick axis from two keys, as the PC controls' other stick keys */
static int full_throw(unsigned int keys, unsigned int low, unsigned int high) {
    int l = (keys & low) != 0;
    int h = (keys & high) != 0;

    return l && !h ? 0x00 : h && !l ? 0xFF : 0x80;
}

unsigned short walk_keys_read(unsigned int keys, int gameplay, MoveButtons buttons, int instant, int axis[4]) {
    unsigned int bits = 0;

    if (instant) {
        axis[2] = full_throw(keys, WALK_KEY_LEFT, WALK_KEY_RIGHT);
        axis[3] = full_throw(keys, WALK_KEY_FORWARD, WALK_KEY_BACK);
        return 0;
    }
    if (keys & WALK_KEY_FORWARD) {
        bits |= gameplay ? buttons.forward : PAD_UP;
    }
    if (keys & WALK_KEY_BACK) {
        bits |= gameplay ? buttons.back : PAD_DOWN;
    }
    if (keys & WALK_KEY_LEFT) {
        bits |= gameplay ? buttons.step_left : PAD_LEFT;
    }
    if (keys & WALK_KEY_RIGHT) {
        bits |= gameplay ? buttons.step_right : PAD_RIGHT;
    }
    return (unsigned short)bits;
}
