/* The settings that change while the game runs (port/include/port/live_settings.h): one setter each, in a table by
   name, which the overlay's switches and KF4_SET_AT's test changes both call. */
#include "port/live_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/host.h"
#include "port/platform.h"
#include "port/settings.h"

static void set_widescreen(int on) {
    port_settings.widescreen = port_options.widescreen = on != 0;
    port_video_apply_settings();
}

static void set_native_widescreen(int on) {
    port_settings.native_widescreen = port_options.native_widescreen = on != 0;
    port_gs_set_native_scale(port_options.native_scale);
}

static void set_native_scale(int scale) {
    scale = scale < 0 ? 0 : scale > 4 ? 4 : scale;
    port_settings.native_scale = scale;
    port_gs_set_native_scale(scale);
}

static void set_frame_rate(int rate) {
    port_settings.frame_rate = port_options.frame_rate = rate;
}

static void set_draw_distance(int distance) {
    distance = distance < 0 ? 0 : distance >= PORT_DRAW_DISTANCE_COUNT ? PORT_DRAW_DISTANCE_COUNT - 1 : distance;
    port_settings.draw_distance = port_options.draw_distance = distance;
}

/* settings.json keeps on (every kind the renderer draws) or off; a choice of kinds is for this run */
static void set_native_draw(int kinds) {
    port_gs_set_native_draw(kinds);
    if (port_options.native_draw == 0 || port_options.native_draw == port_gs_native_kinds()) {
        port_settings.native_draw = port_options.native_draw != 0;
    }
}

static void set_sky(int sky) {
    port_settings.native_sky = port_options.native_sky = sky == PORT_SKY_PS2 ? PORT_SKY_PS2 : PORT_SKY_MODERN;
}

static void set_msaa(int samples) {
    port_settings.native_msaa = samples == 2 || samples == 4 || samples == 8 ? samples : 1;
    port_gs_set_native_msaa(port_settings.native_msaa);
}

static void set_cutout_aa(int on) {
    port_settings.native_cutout_aa = on != 0;
    port_gs_set_native_cutout_aa(on != 0);
}

static void set_colour(int high) {
    port_settings.native_colour = high != 0;
    port_gs_set_native_colour(high != 0);
}

static void set_native_2d(int smooth) {
    port_settings.native_2d = smooth != 0;
    port_gs_set_native_2d(smooth != 0);
}

typedef struct {
    const char *name;
    void (*set)(int value);
} LiveSetting;

static const LiveSetting live_settings[PORT_LIVE_COUNT] = {
    [PORT_LIVE_WIDESCREEN] = {"widescreen", set_widescreen},
    [PORT_LIVE_NATIVE_WIDESCREEN] = {"native_widescreen", set_native_widescreen},
    [PORT_LIVE_NATIVE_SCALE] = {"native_scale", set_native_scale},
    [PORT_LIVE_FRAME_RATE] = {"frame_rate", set_frame_rate},
    [PORT_LIVE_DRAW_DISTANCE] = {"draw_distance", set_draw_distance},
    [PORT_LIVE_NATIVE_DRAW] = {"native_draw", set_native_draw},
    [PORT_LIVE_SKY] = {"sky", set_sky},
    [PORT_LIVE_MSAA] = {"msaa", set_msaa},
    [PORT_LIVE_CUTOUT_AA] = {"cutout_aa", set_cutout_aa},
    [PORT_LIVE_COLOUR] = {"colour", set_colour},
    [PORT_LIVE_NATIVE_2D] = {"native_2d", set_native_2d},
};

void port_live_set(PortLiveSetting setting, int value) {
    if ((unsigned)setting >= PORT_LIVE_COUNT) {
        return;
    }
    live_settings[setting].set(value);
    port_settings_changed();
}

/* ---- KF4_SET_AT ---- */
#define MOST_CHANGES 256

typedef struct {
    unsigned long long vblank;
    PortLiveSetting setting;
    int value;
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
    long value = strtol(sign + 1, &end, 0);
    if (setting < 0 || *end != 0 || sign[1] == 0) {
        fprintf(stderr, "[live] KF4_SET_AT: \"%s\": no such setting or not a number\n", text);
        return 0;
    }
    out->setting = (PortLiveSetting)setting;
    out->value = (int)value;
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
        fprintf(stderr, "[live] vblank %llu: %s = %d\n", now, live_settings[c->setting].name, c->value);
        port_live_set(c->setting, c->value);
    }
}
