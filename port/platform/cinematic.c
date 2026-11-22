/* The trailer's cheats' switches (port/include/port/cinematic.h): the cinematic camera's and enemies ignoring you,
   the flight's feel, and the keys handed from the vblank to the game. All on the game's thread. */
#include "port/cinematic.h"

static struct {
    int enabled;
    int enemies_ignore;
    int speed;
    float move_smoothing_s;
    float look_smoothing_s;
    int toggles;  /* presses of the key not taken yet */
    int vertical; /* -1, 0, 1 */
    int fast;
    int flying;
} cine = {0, 0, 600, 0.35f, 0.25f, 0, 0, 0, 0};

static float clamp_seconds(float s) {
    if (!(s >= PORT_CINEMATIC_SMOOTHING_MIN_S)) {
        return PORT_CINEMATIC_SMOOTHING_MIN_S; /* (NaN too) */
    }
    return s > PORT_CINEMATIC_SMOOTHING_MAX_S ? PORT_CINEMATIC_SMOOTHING_MAX_S : s;
}

int port_cinematic_enabled(void) {
    return cine.enabled;
}

void port_cinematic_set_enabled(int on) {
    cine.enabled = on != 0;
    cine.toggles = 0; /* (off: the flight lands at the game's next pass, game/cinematic.c) */
}

int port_enemies_ignore(void) {
    return cine.enemies_ignore;
}

void port_enemies_ignore_set(int on) {
    cine.enemies_ignore = on != 0;
}

int port_cinematic_speed(void) {
    return cine.speed;
}

void port_cinematic_set_speed(int units_per_s) {
    cine.speed = units_per_s < PORT_CINEMATIC_SPEED_MIN   ? PORT_CINEMATIC_SPEED_MIN
                 : units_per_s > PORT_CINEMATIC_SPEED_MAX ? PORT_CINEMATIC_SPEED_MAX
                                                          : units_per_s;
}

float port_cinematic_move_smoothing_s(void) {
    return cine.move_smoothing_s;
}

void port_cinematic_set_move_smoothing_s(float seconds) {
    cine.move_smoothing_s = clamp_seconds(seconds);
}

float port_cinematic_look_smoothing_s(void) {
    return cine.look_smoothing_s;
}

void port_cinematic_set_look_smoothing_s(float seconds) {
    cine.look_smoothing_s = clamp_seconds(seconds);
}

void port_cinematic_key_toggle(void) {
    if (cine.enabled) {
        cine.toggles++;
    }
}

void port_cinematic_set_held(int vertical, int fast) {
    cine.vertical = vertical > 0 ? 1 : vertical < 0 ? -1 : 0;
    cine.fast = fast != 0;
}

int port_cinematic_take_toggle(void) {
    if (cine.toggles == 0) {
        return 0;
    }
    cine.toggles--;
    return 1;
}

int port_cinematic_vertical(void) {
    return cine.vertical;
}

int port_cinematic_fast(void) {
    return cine.fast;
}

int port_cinematic_flying(void) {
    return cine.flying;
}

void port_cinematic_set_flying(int on) {
    cine.flying = on != 0;
}
