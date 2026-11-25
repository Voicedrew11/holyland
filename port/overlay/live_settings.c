/* The settings that change while the game runs (port/include/port/live_settings.h): one setter each, in a table by
   name, which the overlay's switches and KF4_SET_AT's test changes both call. A setter clamps what it was given to
   the range that setting has, and returns the value that took effect. */
#include "port/live_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/antialias.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/settings.h"
#include "port/widescreen.h"

/* a shape: PORT_SHAPE_ (port/include/port/widescreen.h) */
static double set_widescreen(double shape) {
    int whole = shape < 0 ? PORT_SHAPE_4_3 : shape >= PORT_SHAPE_COUNT ? PORT_SHAPE_COUNT - 1 : (int)shape;
    port_settings.widescreen = port_options.widescreen = whole;
    port_video_apply_settings();
    return port_options.widescreen;
}

static double set_native_widescreen(double on) {
    port_settings.native_widescreen = port_options.native_widescreen = on != 0;
    port_gs_set_native_scale(port_options.native_scale);
    return port_options.native_widescreen;
}

/* the rest below take a whole number: the value comes in as a double with the game speed's quarters, so
   each one that wants a whole number says so */
static double set_native_scale(double scale) {
    int whole = scale < 0 ? 0 : scale > 4 ? 4 : (int)scale;
    port_settings.native_scale = whole;
    port_gs_set_native_scale(whole);
    return whole;
}

static double set_frame_rate(double rate) {
    port_settings.frame_rate = port_options.frame_rate = (int)rate;
    return port_options.frame_rate;
}

/* settings.json and the player menu's 1x to 2x in quarters (PORT_GAME_SPEED_LOW to PORT_GAME_SPEED_HIGH), given
   here as the speed itself (1.25: a quarter faster); --game-speed takes the same range, for one run.
   port/platform/kernel.c re-anchors the vblank pacing when the clock a vblank takes changes. The clock
   runs the speed only while the play simulates: a pause, a message, a menu or a movie keeps the PS2's
   1x (the game's side marks the clock: game/frame_interp.c, port_clock_played in port/platform/video.c). */
static double set_game_speed(double speed) {
    speed = speed < PORT_GAME_SPEED_LOW ? PORT_GAME_SPEED_LOW : speed > PORT_GAME_SPEED_HIGH ? PORT_GAME_SPEED_HIGH : speed;
    float made = (float)speed;

    /* the game's and the renderer's threads read it (port_game_speed, port/platform/video.c) */
    port_settings.game_speed = made;
    __atomic_store(&port_options.game_speed, &made, __ATOMIC_RELAXED);
    return made;
}

static double set_draw_distance(double distance) {
    int whole = distance < 0 ? 0 : distance >= PORT_DRAW_DISTANCE_COUNT ? PORT_DRAW_DISTANCE_COUNT - 1 : (int)distance;
    port_settings.draw_distance = port_options.draw_distance = whole;
    return whole;
}

/* settings.json keeps on (every kind the renderer draws) or off; a choice of kinds is for this run */
static double set_native_draw(double kinds) {
    port_gs_set_native_draw((int)kinds);
    if (port_options.native_draw == 0 || port_options.native_draw == port_gs_native_kinds()) {
        port_settings.native_draw = port_options.native_draw != 0;
    }
    return port_options.native_draw;
}

static double set_sky(double sky) {
    port_settings.native_sky = port_options.native_sky = sky == PORT_SKY_PS2 ? PORT_SKY_PS2 : PORT_SKY_MODERN;
    return port_options.native_sky;
}

static double set_antialias(double method) {
    int whole = (int)method;
    port_settings.antialias = port_antialias(whole) ? whole : PORT_AA_OFF;
    port_gs_set_antialias(port_settings.antialias);
    return port_settings.antialias;
}

static double set_font(double font) {
    port_settings.font = font != 0;
    port_gs_set_font(port_settings.font);
    return port_settings.font;
}

static double set_glyphs(double glyphs) {
    int whole = (int)glyphs;
    port_settings.glyphs = port_glyphs_name(whole) != NULL ? whole : 0;
    port_gs_set_glyphs(port_settings.glyphs);
    return port_settings.glyphs;
}

static double set_native_2d(double smooth) {
    port_settings.native_2d = smooth != 0;
    port_gs_set_native_2d(smooth != 0);
    return port_settings.native_2d;
}

typedef struct {
    const char *name;
    double (*set)(double value); /* the value that took effect, after any clamp */
} LiveSetting;

static const LiveSetting live_settings[PORT_LIVE_COUNT] = {
    [PORT_LIVE_WIDESCREEN] = {"widescreen", set_widescreen},
    [PORT_LIVE_NATIVE_WIDESCREEN] = {"native_widescreen", set_native_widescreen},
    [PORT_LIVE_NATIVE_SCALE] = {"native_scale", set_native_scale},
    [PORT_LIVE_FRAME_RATE] = {"frame_rate", set_frame_rate},
    [PORT_LIVE_DRAW_DISTANCE] = {"draw_distance", set_draw_distance},
    [PORT_LIVE_NATIVE_DRAW] = {"native_draw", set_native_draw},
    [PORT_LIVE_SKY] = {"sky", set_sky},
    [PORT_LIVE_ANTIALIAS] = {"antialias", set_antialias},
    [PORT_LIVE_NATIVE_2D] = {"native_2d", set_native_2d},
    [PORT_LIVE_FONT] = {"font", set_font},
    [PORT_LIVE_GLYPHS] = {"glyphs", set_glyphs},
    [PORT_LIVE_GAME_SPEED] = {"game_speed", set_game_speed},
};

double port_live_set(PortLiveSetting setting, double value) {
    if ((unsigned)setting >= PORT_LIVE_COUNT) {
        return value;
    }
    double made = live_settings[setting].set(value);
    port_settings_changed();
    return made;
}

/* ---- KF4_SET_AT ---- */
#define MOST_CHANGES 256

typedef struct {
    unsigned long long vblank;
    PortLiveSetting setting;
    double value;
} Change;

static Change changes[MOST_CHANGES];
static int change_count = -1; /* -1: KF4_SET_AT not read yet */
static int change_next;

static int find_setting(const char *name, size_t length) {
    for (int i = 0; i < PORT_LIVE_COUNT; i++) {
        if (strlen(live_settings[i].name) == length && strncmp(live_settings[i].name, name, length) == 0) {
            return i;
        }
    }
    return -1;
}

/* One VBLANK:NAME=VALUE, or 0 (said why) */
static int parse_change(const char *item, size_t length, Change *out) {
    char text[128];
    char *end;

    if (length >= sizeof(text)) {
        fprintf(stderr, "[live] KF4_SET_AT: an entry too long\n");
        return 0;
    }
    memcpy(text, item, length);
    text[length] = 0;
    char *colon = strchr(text, ':');
    char *sign = colon ? strchr(colon, '=') : NULL;
    if (!colon || !sign) {
        fprintf(stderr, "[live] KF4_SET_AT: \"%s\" is not VBLANK:NAME=VALUE\n", text);
        return 0;
    }
    out->vblank = strtoull(text, &end, 10);
    if (end != colon || colon == text) {
        fprintf(stderr, "[live] KF4_SET_AT: \"%s\": the vblank is not a number\n", text);
        return 0;
    }
    int setting = find_setting(colon + 1, (size_t)(sign - colon - 1));
    double value = strtod(sign + 1, &end);
    if (setting < 0 || *end != 0 || sign[1] == 0) {
        fprintf(stderr, "[live] KF4_SET_AT: \"%s\": no such setting or not a number\n", text);
        return 0;
    }
    out->setting = (PortLiveSetting)setting;
    out->value = value;
    return 1;
}

static int earlier(const void *a, const void *b) {
    unsigned long long x = ((const Change *)a)->vblank, y = ((const Change *)b)->vblank;
    return x < y ? -1 : x > y;
}

static void read_changes(void) {
    const char *spec = getenv("KF4_SET_AT");

    change_count = 0;
    while (spec && *spec && change_count < MOST_CHANGES) {
        const char *comma = strchr(spec, ',');
        size_t length = comma ? (size_t)(comma - spec) : strlen(spec);
        if (parse_change(spec, length, &changes[change_count])) {
            change_count++;
        }
        spec = comma ? comma + 1 : spec + length;
    }
    qsort(changes, (size_t)change_count, sizeof(changes[0]), earlier);
}

void port_live_vblank(void) {
    if (change_count < 0) {
        read_changes();
    }
    unsigned long long now = (unsigned long long)port_vblank_count();
    while (change_next < change_count && changes[change_next].vblank <= now) {
        const Change *c = &changes[change_next++];
        double made = port_live_set(c->setting, c->value);
        if (made != c->value) {
            fprintf(stderr, "[live] vblank %llu: %s = %g, not the asked %g\n", now,
                    live_settings[c->setting].name, made, c->value);
        } else {
            fprintf(stderr, "[live] vblank %llu: %s = %g\n", now, live_settings[c->setting].name, made);
        }
    }
}
