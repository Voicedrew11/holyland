/* port/sdk/twin_stick.c: the sticks as the game's buttons, and the speeds they set, against Player_Move's own
   way of walking a diagonal.

     twin_stick_test */
#include <math.h>
#include <stdio.h>

#include "port/platform.h"
#include "port/twin_stick.h"

PortOptions port_options; /* no pad script */

#define PI 3.14159265358979323846

#define PAD_UP 0x0010
#define PAD_RIGHT 0x0020
#define PAD_DOWN 0x0040
#define PAD_LEFT 0x0080
#define PAD_L2 0x0100
#define PAD_R2 0x0200
#define PAD_L1 0x0400
#define PAD_R1 0x0800
#define PAD_CROSS 0x4000

/* Input_SetDefaultControls */
static const MoveButtons defaults = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_L1, PAD_R1, PAD_R2, PAD_L2};
static const SpeedLimits limits = {40.0f, 0.06f, 0.04f};
static const PadSticks centred = {0x80, 0x80, 0x80, 0x80};
static const PlayerSpeeds still = {0.0f, 0.0f, 0.0f, 0.0f};

static int failed, checked;

static void check(const char *what, int ok) {
    checked++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static int near(double a, double b, double within) {
    return fabs(a - b) <= within;
}

static unsigned char byte_of(double v) {
    double b = 128.0 + v * 127.0;
    return (unsigned char)(b < 0.0 ? 0.0 : b > 255.0 ? 255.0 : floor(b + 0.5));
}

static PadSticks left_at(double x, double y) {
    PadSticks s = centred;
    s.left_x = byte_of(x);
    s.left_y = byte_of(y);
    return s;
}

static PadSticks right_at(double x, double y) {
    PadSticks s = centred;
    s.right_x = byte_of(x);
    s.right_y = byte_of(y);
    return s;
}

/* Player_Move: the step a frame from the walk and strafe speeds, before the heading turns it */
static void game_step(PlayerSpeeds speeds, double *across, double *along) {
    float angle = atan2f(fabsf(speeds.walk), fabsf(speeds.strafe));
    *across = speeds.strafe * cosf(angle);
    *along = speeds.walk * sinf(angle);
}

static void test_held(void) {
    unsigned short held;

    held = twin_stick_held(0, 0, centred, defaults);
    check("centred sticks hold nothing", held == 0);

    /* Input_ReadPort's left stick left: d-pad left, which turns; twin-stick steps left instead */
    held = twin_stick_held(PAD_LEFT, 0, left_at(-1.0, 0.0), defaults);
    check("left stick left: step left, no turn", held == PAD_L1);

    held = twin_stick_held(PAD_UP, 0, left_at(0.0, -1.0), defaults);
    check("left stick up: forward", held == PAD_UP);

    /* the right stick's x was nothing on the PS2 */
    held = twin_stick_held(0, 0, right_at(1.0, 0.0), defaults);
    check("right stick right: turn right", held == PAD_RIGHT);

    held = twin_stick_held(PAD_R2, 0, right_at(0.0, -1.0), defaults);
    check("right stick up: look up (look_down_button, L2), not the PS2's R2", held == PAD_L2);

    /* a real d-pad press is kept, the stick's own bit dropped */
    held = twin_stick_held(PAD_LEFT | PAD_CROSS, PAD_LEFT | PAD_CROSS, centred, defaults);
    check("the real d-pad and buttons are kept", held == (PAD_LEFT | PAD_CROSS));

    /* a little past the deadzone counts, under half throw (the PS2's) too */
    held = twin_stick_held(0, 0, left_at(0.0, -0.3), defaults);
    check("a third of the throw walks", held == PAD_UP);
    held = twin_stick_held(0, 0, left_at(0.0, -0.1), defaults);
    check("inside the deadzone nothing", held == 0);

    /* the game's own controls option moves the buttons */
    {
        MoveButtons swapped = defaults;
        swapped.step_left = PAD_LEFT;
        swapped.turn_left = PAD_L1;
        held = twin_stick_held(PAD_LEFT, 0, left_at(-1.0, 0.0), swapped);
        check("the game's chosen step button", held == PAD_LEFT);
    }
}

static void test_walk(void) {
    PlayerSpeeds speeds;
    double across, along;
    int degrees;
    int all_ok = 1;

    speeds = twin_stick_speeds(still, left_at(0.0, -1.0), limits, defaults, PAD_UP);
    check("full forward: the top speed at once", near(speeds.walk, limits.top_speed, 1e-3) && speeds.strafe == 0.0f);

    speeds = twin_stick_speeds(still, left_at(0.0, 0.575), limits, defaults, PAD_DOWN);
    check("half back: half the top speed", near(speeds.walk, -0.5 * limits.top_speed, 0.3));

    /* every direction at full throw comes out of the game's diagonal at the top speed, the stick's way */
    for (degrees = 0; degrees < 360; degrees += 5) {
        double a = degrees * PI / 180.0;
        double x = sin(a), y = -cos(a); /* 0 forward, 90 right */
        unsigned short buttons = twin_stick_held(0, 0, left_at(x, y), defaults);
        double heading;

        speeds = twin_stick_speeds(still, left_at(x, y), limits, defaults, buttons);
        game_step(speeds, &across, &along);
        heading = atan2(across, along);
        if (!near(hypot(across, along), limits.top_speed, 0.6) ||
            !near(fmod(heading - a + 3.0 * PI, 2.0 * PI) - PI, 0.0, 0.02)) {
            printf("     %d degrees: speed %.2f heading %.1f\n", degrees, hypot(across, along), heading * 180.0 / PI);
            all_ok = 0;
        }
    }
    check("every direction at the top speed, where the stick points", all_ok);

    /* the game's masks took the walk away: the speeds are the game's */
    speeds = twin_stick_speeds(still, left_at(0.0, -1.0), limits, defaults, 0);
    check("masked: not walked", speeds.walk == 0.0f);
}

static void test_turn_and_look(void) {
    PlayerSpeeds speeds;
    PlayerSpeeds after = {0.0f, 0.0f, 0.005f, 0.005f}; /* what the game's own easing did */

    speeds = twin_stick_speeds(after, right_at(1.0, 0.0), limits, defaults, PAD_RIGHT);
    check("full right: the top turn at once, no easing", near(speeds.turn, limits.top_turn, 1e-6));

    speeds = twin_stick_speeds(after, centred, limits, defaults, 0);
    check("let go: the turn stops dead", speeds.turn == 0.0f);
    speeds = twin_stick_speeds(after, centred, limits, defaults, 0);
    check("then the game's own again", speeds.turn == after.turn);

    speeds = twin_stick_speeds(after, right_at(0.5, 0.0), limits, defaults, PAD_RIGHT);
    check("half right turns slower", speeds.turn > 0.0f && speeds.turn < 0.5f * limits.top_turn);

    /* the d-pad takes over from the stick: the game's speed, eased */
    speeds = twin_stick_speeds(after, centred, limits, defaults, PAD_LEFT);
    check("a d-pad turn after the stick: the game's", speeds.turn == after.turn);

    /* up raises the view: look_down_button's way (R2 on the PS2's stick) */
    check("right stick up holds look_down_button", twin_stick_held(0, 0, right_at(0.0, -1.0), defaults) == PAD_L2);
    speeds = twin_stick_speeds(after, right_at(0.0, -1.0), limits, defaults, PAD_L2);
    check("full up: the top look, up, at once", near(speeds.look, limits.top_look, 1e-6));
    speeds = twin_stick_speeds(after, right_at(0.0, 1.0), limits, defaults, PAD_R2);
    check("full down: the top look, down", near(speeds.look, -limits.top_look, 1e-6));
    speeds = twin_stick_speeds(after, centred, limits, defaults, 0);
    check("let go: the look stops dead", speeds.look == 0.0f);

    /* looking held off (the view levelling): the game's */
    speeds = twin_stick_speeds(after, right_at(0.0, 1.0), limits, defaults, 0);
    check("look masked: the game's", speeds.look == after.look);
}

static void test_setting(void) {
    check("on by default", port_twin_stick_on());
    port_options.input = "script.pad";
    check("off while a pad script plays", !port_twin_stick_on());
    port_options.input = NULL;
    port_twin_stick_set(0);
    check("off when set off", !port_twin_stick_on());
    port_twin_stick_set(1);
}

int main(void) {
    test_held();
    test_walk();
    test_turn_and_look();
    test_setting();
    printf("%d of %d checks passed\n", checked - failed, checked);
    return failed != 0;
}
