/* Modern twin-stick controls (port/include/port/twin_stick.h), as Verdite1 has them
   (patches/Analog.cs there): a radial deadzone of 0.15 on each stick, curves of 1.0 (walking) and
   1.35 (turning and looking), full throw the game's top speeds, the right stick with no easing (as
   Verdite1's with its ease off). Verdite1's 3D mode (the left stick walks and turns) is the PS2's own
   here: twin-stick off, the game's easing and all. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "port/platform.h"
#include "port/savestate.h"
#include "port/twin_stick.h"

#define DEADZONE 0.15f
#define WALK_CURVE 1.0f
#define LOOK_CURVE 1.35f

#define DROVE_TURN 1
#define DROVE_LOOK 2

static int setting = 1;
static unsigned char drove; /* DROVE_: what the right stick drove last call */

int port_twin_stick_setting(void) {
    return setting;
}

void port_twin_stick_set(int on) {
    setting = on != 0;
}

/* KF4_TWIN_STICK_SCRIPTED=1: on with a pad script too, to test it (tests/twin_stick.pad) */
int port_twin_stick_on(void) {
    const char *scripted;

    if (!setting) {
        return 0;
    }
    if (port_options.input == NULL && port_options.record_input == NULL) {
        return 1;
    }
    scripted = getenv("KF4_TWIN_STICK_SCRIPTED");
    return scripted != NULL && strcmp(scripted, "1") == 0;
}

typedef struct {
    float x, y; /* -1 to 1: + right and down; 0 in the deadzone */
} Stick;

/* One stick, radially deadzoned and curved */
static Stick shape(unsigned char byte_x, unsigned char byte_y, float curve) {
    Stick out = {0.0f, 0.0f};
    float x = ((float)byte_x - 128.0f) / 127.0f;
    float y = ((float)byte_y - 128.0f) / 127.0f;
    float magnitude = sqrtf(x * x + y * y);
    float unit;
    float scale;

    if (magnitude <= DEADZONE) {
        return out;
    }
    unit = (magnitude - DEADZONE) / (1.0f - DEADZONE);
    if (unit > 1.0f) {
        unit = 1.0f;
    }
    scale = powf(unit, curve) / magnitude;
    out.x = x * scale;
    out.y = y * scale;
    return out;
}

static Stick left_stick(PadSticks sticks) {
    return shape(sticks.left_x, sticks.left_y, WALK_CURVE);
}

static Stick right_stick(PadSticks sticks) {
    return shape(sticks.right_x, sticks.right_y, LOOK_CURVE);
}

/* `negative` below zero, `positive` above, nothing at zero */
static unsigned short axis_buttons(float value, unsigned short negative, unsigned short positive) {
    return value < 0.0f ? negative : value > 0.0f ? positive : 0;
}

unsigned short twin_stick_held(unsigned short held, unsigned short pad_held, PadSticks sticks, MoveButtons buttons) {
    /* Input_ReadPort's: the d-pad from the left stick, L2 / R2 from the right */
    const unsigned short from_sticks = 0x00F0 | 0x0100 | 0x0200;
    Stick left = left_stick(sticks);
    Stick right = right_stick(sticks);
    unsigned short made = held & ~pad_held & from_sticks;

    held &= (unsigned short)~made;
    held |= axis_buttons(left.y, buttons.forward, buttons.back);
    held |= axis_buttons(left.x, buttons.step_left, buttons.step_right);
    held |= axis_buttons(right.x, buttons.turn_left, buttons.turn_right);
    held |= axis_buttons(right.y, buttons.look_down, buttons.look_up); /* up raises the view */
    return held;
}

/* The walk and strafe that Player_Move's own evening out of a diagonal (each scaled by the cosine
   or sine of atan2(|walk|, |strafe|)) turns into `magnitude` in the stick's direction: it would
   otherwise slow a part-diagonal stick and pull the walk toward the nearer axis. */
static PlayerSpeeds walk_for(Stick left, float top_speed) {
    PlayerSpeeds out = {0.0f, 0.0f, 0.0f, 0.0f};
    float magnitude = sqrtf(left.x * left.x + left.y * left.y);
    float across;
    float along;

    if (magnitude <= 0.0f) {
        return out;
    }
    across = fabsf(left.x) / magnitude;
    along = fabsf(left.y) / magnitude;
    out.strafe = copysignf(magnitude * top_speed * sqrtf(across * (across + along)), left.x);
    out.walk = copysignf(magnitude * top_speed * sqrtf(along * (across + along)), -left.y);
    return out;
}

/* One axis of the right stick: the stick's speed while it drives, 0 the call after it stops driving
   unless a button turns or looks now, otherwise the game's */
static float view_speed(float stick, float top, float game, unsigned short axis_buttons, unsigned short buttons,
                        unsigned char flag) {
    if (stick != 0.0f && (buttons & axis_buttons)) {
        drove |= flag;
        return stick * top;
    }
    if ((drove & flag) && !(buttons & axis_buttons)) {
        game = 0.0f;
    }
    drove &= (unsigned char)~flag;
    return game;
}

PlayerSpeeds twin_stick_speeds(PlayerSpeeds after, PadSticks sticks, SpeedLimits limits, MoveButtons move,
                               unsigned short buttons) {
    Stick left = left_stick(sticks);
    Stick right = right_stick(sticks);
    PlayerSpeeds walk = walk_for(left, limits.top_speed);
    PlayerSpeeds out = after;

    if (left.y != 0.0f && (buttons & (move.forward | move.back))) {
        out.walk = walk.walk;
    }
    if (left.x != 0.0f && (buttons & (move.step_left | move.step_right))) {
        out.strafe = walk.strafe;
    }
    out.turn = view_speed(right.x, limits.top_turn, after.turn, move.turn_left | move.turn_right, buttons, DROVE_TURN);
    out.look = view_speed(-right.y, limits.top_look, after.look, move.look_up | move.look_down, buttons, DROVE_LOOK);
    return out;
}

void port_twin_stick_state(PortStateIo *io) {
    PORT_STATE(io, drove);
}
