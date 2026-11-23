/* port/sdk/walk_keys.c: W A S D as the game's own walking buttons, the same with twin-stick controls on or off
   (port/sdk/twin_stick.c), and the ladders reading them as the d-pad's.

     walk_keys_test */
#include <stdio.h>

#include "port/platform.h"
#include "port/twin_stick.h"
#include "port/walk_keys.h"

PortOptions port_options; /* no pad script */

#define PAD_UP 0x0010
#define PAD_RIGHT 0x0020
#define PAD_DOWN 0x0040
#define PAD_LEFT 0x0080
#define PAD_L2 0x0100
#define PAD_R2 0x0200
#define PAD_L1 0x0400
#define PAD_R1 0x0800
#define PAD_TRIANGLE 0x1000

/* Input_SetDefaultControls */
static const MoveButtons defaults = {PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_L1, PAD_R1, PAD_R2, PAD_L2};
static const PadSticks centred = {0x80, 0x80, 0x80, 0x80};

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

/* The keys' buttons, instant WASD acceleration off: the stick left at rest */
static unsigned short bits(unsigned int keys, int gameplay, MoveButtons buttons) {
    int axis[4] = {0x80, 0x80, 0x80, 0x80};
    unsigned short out = walk_keys_read(keys, gameplay, buttons, 0, axis);

    if (axis[0] != 0x80 || axis[1] != 0x80 || axis[2] != 0x80 || axis[3] != 0x80) {
        printf("     a stick moved: %d %d %d %d\n", axis[0], axis[1], axis[2], axis[3]);
        return 0xFFFF;
    }
    return out;
}

static void test_bits(void) {
    MoveButtons game = port_walk_keys_buttons();
    MoveButtons swapped = defaults;

    check("the game's buttons until it says: Input_SetDefaultControls's",
          game.forward == PAD_UP && game.back == PAD_DOWN && game.step_left == PAD_L1 && game.step_right == PAD_R1);
    check("nothing held, nothing", bits(0, 1, defaults) == 0);
    check("W in play: forward (d-pad up)", bits(WALK_KEY_FORWARD, 1, defaults) == PAD_UP);
    check("S in play: back (d-pad down)", bits(WALK_KEY_BACK, 1, defaults) == PAD_DOWN);
    check("A in play: step left (L1), not a turn", bits(WALK_KEY_LEFT, 1, defaults) == PAD_L1);
    check("D in play: step right (R1), not a turn", bits(WALK_KEY_RIGHT, 1, defaults) == PAD_R1);
    check("W + A in play: forward and step left",
          bits(WALK_KEY_FORWARD | WALK_KEY_LEFT, 1, defaults) == (PAD_UP | PAD_L1));
    check("A out of play: the d-pad's left (the menus' cursor)", bits(WALK_KEY_LEFT, 0, defaults) == PAD_LEFT);
    check("W S A D out of play: the d-pad",
          bits(WALK_KEY_FORWARD | WALK_KEY_BACK | WALK_KEY_LEFT | WALK_KEY_RIGHT, 0, defaults) ==
              (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT));

    /* the game's controls option moved the buttons: the keys follow it */
    swapped.forward = PAD_TRIANGLE;
    swapped.step_left = PAD_LEFT;
    port_walk_keys_set_buttons(swapped);
    check("the game's own forward and step buttons, as it set them",
          bits(WALK_KEY_FORWARD | WALK_KEY_LEFT, 1, port_walk_keys_buttons()) == (PAD_TRIANGLE | PAD_LEFT));
    port_walk_keys_set_buttons(defaults);
}

/* The keys' bits are the pad's own (pad_held): twin-stick keeps them, and with the stick at rest sets no speed */
static void test_twin_stick(void) {
    unsigned short keys = bits(WALK_KEY_FORWARD | WALK_KEY_LEFT, 1, defaults);
    PlayerSpeeds after = {4.8f, -3.6f, 0.01f, 0.0f}; /* the game's step from the keys' buttons */
    SpeedLimits limits = {12.0f, 0.06f, 0.04f};
    PlayerSpeeds speeds;
    int on;

    for (on = 0; on <= 1; on++) {
        unsigned short held = keys; /* twin-stick off: the game reads it as it is */

        port_twin_stick_set(on);
        if (port_twin_stick_on()) {
            held = twin_stick_held(held, keys, centred, defaults);
        }
        check(on ? "twin-stick on: W + A held as they are" : "twin-stick off: W + A held as they are", held == keys);
    }
    speeds = twin_stick_speeds(after, centred, limits, defaults, keys);
    check("keys with the stick at rest: the game's own walk and strafe (eased as the d-pad's)",
          speeds.walk == after.walk && speeds.strafe == after.strafe);

    /* a key and the controller's stick at once: the stick's bits are dropped only where the pad did not hold them */
    {
        PadSticks back = centred;
        unsigned short held;

        back.left_y = 0xFF;
        held = twin_stick_held(PAD_DOWN | PAD_L1, PAD_L1, back, defaults);
        check("A held with the stick back: step left and back", held == (PAD_L1 | PAD_DOWN));
    }
    port_twin_stick_set(1);
}

/* Issue #203's ladder reading keeps the pad's own buttons: W climbs, A or D step off */
static void test_ladder(void) {
    unsigned short w = bits(WALK_KEY_FORWARD, 1, defaults);
    unsigned short wa = bits(WALK_KEY_FORWARD | WALK_KEY_LEFT, 1, defaults);
    unsigned short d = bits(WALK_KEY_RIGHT, 1, defaults);

    check("ladder: W climbs", twin_stick_ladder_held(twin_stick_held(w, w, centred, defaults), w, centred, defaults) == PAD_UP);
    check("ladder: D steps off (step right held)",
          twin_stick_ladder_held(twin_stick_held(d, d, centred, defaults), d, centred, defaults) == PAD_R1);
    check("ladder: W + A both kept, as the d-pad and L1 were on the PS2",
          twin_stick_ladder_held(twin_stick_held(wa, wa, centred, defaults), wa, centred, defaults) == (PAD_UP | PAD_L1));
}

/* Instant WASD acceleration: the keys as before, the left stick at full throw and no buttons */
static void test_instant(void) {
    int axis[4] = {0x80, 0x80, 0x80, 0x80};
    unsigned short held;

    check("instant by default", port_walk_keys_instant_setting());
    check("instant in live play", port_walk_keys_instant());
    port_walk_keys_set_instant(0);
    check("setting off: eased in live play", !port_walk_keys_instant());
    port_walk_keys_set_instant(1);
    port_options.input = "script.pad";
    check("not while a pad script plays", !port_walk_keys_instant());
    port_options.input = NULL;
    port_options.record_input = "record.pad";
    check("not while one is recorded", !port_walk_keys_instant());
    port_options.record_input = NULL;

    held = walk_keys_read(WALK_KEY_FORWARD, 1, defaults, 1, axis);
    check("instant W: the left stick full up, no buttons, the right stick untouched",
          held == 0 && axis[3] == 0x00 && axis[2] == 0x80 && axis[0] == 0x80 && axis[1] == 0x80);
    held = walk_keys_read(WALK_KEY_BACK | WALK_KEY_LEFT, 1, defaults, 1, axis);
    check("instant S + A: full down and left", held == 0 && axis[3] == 0xFF && axis[2] == 0x00);
    held = walk_keys_read(WALK_KEY_FORWARD | WALK_KEY_BACK | WALK_KEY_RIGHT, 0, defaults, 1, axis);
    check("instant W + S + D: up and down cancel, full right", held == 0 && axis[3] == 0x80 && axis[2] == 0xFF);
    held = walk_keys_read(0, 1, defaults, 1, axis);
    check("instant, nothing held: the stick at rest", held == 0 && axis[3] == 0x80 && axis[2] == 0x80);
}

int main(void) {
    test_bits();
    test_instant();
    test_twin_stick();
    test_ladder();
    printf("%d of %d checks passed\n", checked - failed, checked);
    return failed != 0;
}
