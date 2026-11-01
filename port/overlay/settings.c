/* settings.json (port/include/port/settings.h): the player's settings, in the user's data folder.

     {
       "version":  3,
       "display":  {"window_height": 896, "window_width": 0, "fullscreen": false, "aspect": "4:3", "vsync": false, "smooth": true,
                    "deinterlace": true, "progressive": true, "anti_blur": true, "adapt_width": true,
                    "whole_frame": true, "seamless_scroll": true, "flicker_filter": false,
                    "widescreen": false, "widescreen_hud": "corner", "frame_rate": 30, "frame_rate_cap": 0, "tearing": false,
                    "renderer": "native",
                    "draw_distance": "1", "draw_distance_sky": "ps2",
                    "native_scale": "auto", "native_widescreen": true, "native_msaa": 1, "native_lod": "scale", "native_aniso": 1,
                    "native_cutout_aa": "ssaa", "native_2d": "smooth",
                    "native_colour": "exact", "native_layer_z": "tolerant", "native_draw": "off", "native_sky": "modern",
                    "native_shine": "whole",
                    "presenter": "vulkan"},
       "audio":    {"volume": 100, "mute": false},
       "controls": {"layout": "ps2", "mouse_sensitivity": 1.0, "invert_y": false,
                    "menu_mouse": ["cross", "circle", "cross"], "menu_side_keys": true,
                    "bindings": {"verdite": {"square": ["key:Space", "mouse:left", "pad:x"]}}},
       "mods":     {"myus_necklace": false, "separate_run": false},
       "overlay":  {"pause_when_open": false, "show_fps": false, "scale": 1.0, "save_slot": 0, "quick_slot": 0}
     }

   "bindings" holds only the actions that differ from their layout's defaults (port/sdk/controls.c),
   each as its three slots: two keys or mouse buttons, and a controller button. Keys this version does
   not know are kept as they are. A missing or broken file means the defaults.

   "version" is the file's: 2 from stage 4 (docs/port/STEP3.md), when the native GS and the Vulkan
   presenter became the defaults. Every save writes every setting out, so a file from before (no
   "version") says "renderer": "gs" and "presenter": "sdl" whether the player chose them or never
   looked: there those two values are read as the old defaults, and the new ones are used (and the
   file written again, as version 2). A file from before that says "native", "both" or "vulkan" is
   kept as it is; from version 2 on, "gs" and "sdl" are the player's choice and kept too.

   3 from October 2026, when "native_scale" got "auto" (port_gs_native_scale: the picture drawn about as
   tall as the window shows it) and it and "native_widescreen": true became the defaults. A file from
   before says "native_scale": 1 and "native_widescreen": false whether the player chose them or not:
   there the two are read as the new defaults. Other scales, and native widescreen on, are kept. */
#define SETTINGS_VERSION 3
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

#include "cJSON.h"
#include "port/audio.h"
#include "port/automap.h"
#include "port/weapon_sets.h"
#include "port/controls.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/settings.h"
#include "port/stats.h"

PortSettings port_settings = {
    .window_height = 896,
    .smooth = 1,
    .scaling = PORT_SCALING_BEST,
    .sharpen = 0,
    .deinterlace = 1,
    .progressive = 1,
    .anti_blur = 1,
    .adapt_width = 1,
    .whole_frame = 1,
    .seamless_scroll = 1,
    .frame_rate = 30,
    .volume = 100,
    .layout = "ps2",
    .ui_scale = 1.0f,
    .renderer = PORT_RENDERER_NATIVE,
    .presenter = PORT_PRESENTER_VULKAN,
    .native_lod = 1, /* GSN_LOD_SCALE */
    .native_scale = 0, /* auto */
    .native_widescreen = 1,
    .native_aniso = 1,
    .native_cutout_aa = 1, /* GSN_CUTOUT_AA_SSAA */
    .native_2d = 1, /* GSN_2D_SMOOTH */
    .native_layer_z = 1, /* GSN_LAYER_Z_TOLERANT */
    .native_sky = PORT_SKY_MODERN,
};

static int active;
static cJSON *document;
static int dirty;
static double dirty_since;

/* ---- The data folder -------------------------------------------------------------------------- */
const char *port_data_dir(void) {
    static char path[1024];
    const char *base;

    if (path[0] != 0) {
        return path;
    }
    base = getenv("KF4_DATA_DIR");
    if (base != NULL && *base != 0) {
        snprintf(path, sizeof(path), "%s", base);
        return path;
    }
#ifdef _WIN32
    base = getenv("APPDATA");
    if (base != NULL && *base != 0) {
        snprintf(path, sizeof(path), "%s\\holyland", base);
        return path;
    }
#else
    base = getenv("XDG_CONFIG_HOME");
    if (base != NULL && *base != 0) {
        snprintf(path, sizeof(path), "%s/holyland", base);
        return path;
    }
    base = getenv("HOME");
    if (base != NULL && *base != 0) {
        snprintf(path, sizeof(path), "%s/.config/holyland", base);
        return path;
    }
#endif
    snprintf(path, sizeof(path), "holyland");
    return path;
}

const char *port_settings_path(void) {
    static char path[1100];

    if (path[0] == 0) {
        snprintf(path, sizeof(path), "%s/settings.json", port_data_dir());
    }
    return path;
}

/* Makes the folder and the ones above it */
static void make_folders(const char *folder) {
    char path[1024];
    size_t i;

    snprintf(path, sizeof(path), "%s", folder);
    for (i = 1; path[i] != 0; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            char c = path[i];

            if (path[i - 1] == ':') {
                continue; /* C:\ */
            }
            path[i] = 0;
#ifdef _WIN32
            _mkdir(path);
#else
            mkdir(path, 0755);
#endif
            path[i] = c;
        }
    }
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

/* ---- Reading ---------------------------------------------------------------------------------- */
static int read_bool(const cJSON *object, const char *key, int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static double read_number(const cJSON *object, const char *key, double fallback, double low, double high) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    double value = cJSON_IsNumber(item) ? item->valuedouble : fallback;

    return value < low ? low : value > high ? high : value;
}

/* settings.json's "renderer" for PORT_RENDERER_ */
static const char *renderer_name(int renderer) {
    static const char *const names[] = {"gs", "native", "both", "modern", "fast"};
    return renderer >= 0 && renderer <= PORT_RENDERER_FAST ? names[renderer] : "gs";
}

static const char *read_string(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int action_from_id(const char *id) {
    int action;

    for (action = 0; id != NULL && action < ACTION_COUNT; action++) {
        if (strcmp(id, port_controls_action_id(action)) == 0) {
            return action;
        }
    }
    return -1;
}

static void read_controls(const cJSON *controls) {
    const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(controls, "bindings");
    const cJSON *menu = cJSON_GetObjectItemCaseSensitive(controls, "menu_mouse");
    const char *layout = read_string(controls, "layout");
    int i;

    if (layout != NULL && port_controls_layout_from_name(layout) >= 0) {
        snprintf(port_settings.layout, sizeof(port_settings.layout), "%s", layout);
    }
    port_controls_set_mouse(read_number(controls, "mouse_sensitivity", 1.0, 0.05, 10.0),
                            read_bool(controls, "invert_y", 0));
    port_controls_set_menu_side_keys(read_bool(controls, "menu_side_keys", 1));
    for (i = 0; i < 3 && cJSON_IsArray(menu); i++) {
        const cJSON *item = cJSON_GetArrayItem(menu, i);

        if (cJSON_IsString(item)) {
            port_controls_set_menu_mouse(i, strcmp(item->valuestring, "none") == 0 ? -1
                                                                                     : action_from_id(item->valuestring));
        }
    }
    for (i = 0; i < CONTROLS_LAYOUTS; i++) {
        const cJSON *table = cJSON_GetObjectItemCaseSensitive(bindings, port_controls_layout_name(i));
        const cJSON *entry;

        cJSON_ArrayForEach(entry, table) {
            int action = action_from_id(entry->string);
            int slot;

            if (action < 0 || !cJSON_IsArray(entry)) {
                fprintf(stderr, "[settings] controls: no action called \"%s\"\n", entry->string);
                continue;
            }
            for (slot = 0; slot < BINDING_SLOTS; slot++) {
                const cJSON *item = cJSON_GetArrayItem(entry, slot);
                int binding;

                if (cJSON_IsString(item) && port_controls_binding_parse(item->valuestring, &binding)) {
                    port_controls_bind(i, action, slot, binding);
                } else if (item != NULL) {
                    fprintf(stderr, "[settings] controls: %s: not a key or button\n", entry->string);
                }
            }
        }
    }
}

int port_settings_active(void) {
    return active;
}

void port_settings_load(void) {
    PortSettings *s = &port_settings;
    const cJSON *display;
    const cJSON *audio;
    const cJSON *overlay;
    const char *aspect;
    int version;
    int migrated = 0;
    char *text = NULL;
    long size = 0;
    FILE *file;

#ifdef KF4_HAVE_SDL
    active = (!port_options.headless && port_options.input == NULL) || getenv("KF4_OVERLAY_SHOT") != NULL;
#endif
    if (!active) {
        return;
    }
    file = fopen(port_settings_path(), "rb");
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fseek(file, 0, SEEK_SET);
        text = calloc(1, (size_t)(size > 0 ? size : 0) + 1);
        if (text != NULL && size > 0 && fread(text, 1, (size_t)size, file) != (size_t)size) {
            size = 0;
        }
        fclose(file);
    }
    document = text != NULL ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(document)) {
        if (file != NULL) {
            fprintf(stderr, "[settings] %s is not valid JSON: the defaults are used, and the file is left as it is\n",
                    port_settings_path());
            active = 2; /* not written over */
        }
        cJSON_Delete(document);
        document = cJSON_CreateObject();
    }

    version = (int)read_number(document, "version", 1, 1, 1000000);
    display = cJSON_GetObjectItemCaseSensitive(document, "display");
    /* the first settings files kept a multiple of 448 */
    s->window_height = (int)read_number(display, "window_scale", s->window_height / 448, 1, 4) * 448;
    s->window_height = (int)read_number(display, "window_height", s->window_height, 448, 4320);
    s->window_width = (int)read_number(display, "window_width", s->window_width, 0, 7680);
    s->fullscreen = read_bool(display, "fullscreen", s->fullscreen);
    aspect = read_string(display, "aspect");
    if (aspect != NULL) {
        s->aspect = strcmp(aspect, "stretch") == 0 ? ASPECT_STRETCH : ASPECT_4_3;
    }
    s->vsync = read_bool(display, "vsync", s->vsync);
    s->smooth = read_bool(display, "smooth", s->smooth);
    {
        /* "scaling": "sharp", "smooth" or "best"; files from before it had "smooth" alone: true is now
           the best scaling (the presenter's own filter), false sharp pixels as before */
        const char *scaling = read_string(display, "scaling");

        if (scaling != NULL) {
            s->scaling = strcmp(scaling, "sharp") == 0    ? PORT_SCALING_SHARP
                         : strcmp(scaling, "smooth") == 0 ? PORT_SCALING_SMOOTH
                                                          : PORT_SCALING_BEST;
        } else {
            s->scaling = s->smooth ? PORT_SCALING_BEST : PORT_SCALING_SHARP;
        }
        s->smooth = s->scaling != PORT_SCALING_SHARP;
        s->sharpen = (float)read_number(display, "sharpen", s->sharpen, 0, 1);
    }
    s->deinterlace = read_bool(display, "deinterlace", s->deinterlace);
    s->progressive = read_bool(display, "progressive", s->progressive);
    s->anti_blur = read_bool(display, "anti_blur", s->anti_blur);
    s->adapt_width = read_bool(display, "adapt_width", s->adapt_width);
    s->whole_frame = read_bool(display, "whole_frame", s->whole_frame);
    s->seamless_scroll = read_bool(display, "seamless_scroll", s->seamless_scroll);
    s->flicker_filter = read_bool(display, "flicker_filter", s->flicker_filter);
    s->widescreen = read_bool(display, "widescreen", s->widescreen);
    {
        const char *hud = read_string(display, "widescreen_hud");

        if (hud != NULL) {
            s->widescreen_hud_centred = strcmp(hud, "centred") == 0;
        }
    }
    {
        /* 30 (the PS2's), a number of pictures a second, "monitor" or "unlocked" (PORT_FRAME_RATE_) */
        const char *rate = read_string(display, "frame_rate");

        if (rate != NULL) {
            s->frame_rate = strcmp(rate, "monitor") == 0    ? PORT_FRAME_RATE_MONITOR
                            : strcmp(rate, "unlocked") == 0 ? PORT_FRAME_RATE_UNLOCKED
                                                            : s->frame_rate;
        } else {
            s->frame_rate = (int)read_number(display, "frame_rate", s->frame_rate, 30, 500);
        }
        s->frame_rate_cap = (int)read_number(display, "frame_rate_cap", s->frame_rate_cap, 0, 1000);
        s->frame_rate_cap = s->frame_rate_cap > 0 && s->frame_rate_cap < 60 ? 60 : s->frame_rate_cap;
        s->tearing = read_bool(display, "tearing", s->tearing);
    }
    {
        const char *draw_distance = read_string(display, "draw_distance");
        const char *sky = read_string(display, "draw_distance_sky");

        if (draw_distance != NULL && port_draw_distance_parse(draw_distance) >= 0) {
            s->draw_distance = port_draw_distance_parse(draw_distance);
        }
        if (sky != NULL) {
            s->draw_distance_sky = strcmp(sky, "push") == 0;
        }
    }
    {
        const char *renderer = read_string(display, "renderer");

        if (renderer != NULL && version < 2 && strcmp(renderer, "gs") == 0) {
            migrated = 1; /* the old default, written out: the new default (port_settings above) */
        } else if (renderer != NULL) {
            s->renderer = strcmp(renderer, "native") == 0   ? PORT_RENDERER_NATIVE
                          : strcmp(renderer, "both") == 0   ? PORT_RENDERER_BOTH
                          : strcmp(renderer, "modern") == 0 ? PORT_RENDERER_MODERN
                          : strcmp(renderer, "fast") == 0   ? PORT_RENDERER_FAST
                                                            : PORT_RENDERER_GS;
        }
    }
    {
        const char *scale = read_string(display, "native_scale");

        if (scale != NULL) {
            s->native_scale = strcmp(scale, "auto") == 0 ? 0 : s->native_scale;
        } else {
            s->native_scale = (int)read_number(display, "native_scale", s->native_scale, 0, 4);
        }
        if (version < 3 && s->native_scale == 1) {
            s->native_scale = 0; /* the old default, written out: auto */
            migrated = 1;
        }
    }
    s->native_widescreen = read_bool(display, "native_widescreen", s->native_widescreen);
    if (version < 3 && !s->native_widescreen) {
        s->native_widescreen = 1; /* (as above) */
        migrated = 1;
    }
    s->native_msaa = (int)read_number(display, "native_msaa", s->native_msaa, 1, 8);
    s->native_aniso = (int)read_number(display, "native_aniso", s->native_aniso, 1, 16);
    {
        const char *lod = read_string(display, "native_lod");

        if (lod != NULL) {
            s->native_lod = strcmp(lod, "ps2") == 0 ? 0 : strcmp(lod, "full") == 0 ? 2 : 1;
        }
    }
    {
        const char *cutout = read_string(display, "native_cutout_aa");

        if (cutout != NULL) {
            s->native_cutout_aa = strcmp(cutout, "off") == 0 ? 0 : 1;
        }
    }
    {
        const char *flat = read_string(display, "native_2d");

        if (flat != NULL) {
            s->native_2d = strcmp(flat, "ps2") == 0 ? 0 : 1;
        }
    }
    {
        const char *colour = read_string(display, "native_colour");

        if (colour != NULL) {
            s->native_colour = strcmp(colour, "high") == 0 ? 1 : 0;
        }
    }
    {
        const char *layer_z = read_string(display, "native_layer_z");

        if (layer_z != NULL) {
            s->native_layer_z = strcmp(layer_z, "ps2") == 0 ? 0 : 1;
        }
    }
    {
        const char *shine = read_string(display, "native_shine");

        if (shine != NULL) {
            s->native_shine_exact = strcmp(shine, "exact") == 0;
        }
    }
    {
        /* "on": every kind native draw has accepted (PORT_ND_ACCEPTED), "off" the PS2 path */
        const char *native_draw = read_string(display, "native_draw");

        if (native_draw != NULL) {
            s->native_draw = strcmp(native_draw, "on") == 0;
        }
    }
    {
        const char *sky = read_string(display, "native_sky");

        if (sky != NULL) {
            s->native_sky = strcmp(sky, "ps2") == 0 ? PORT_SKY_PS2 : PORT_SKY_MODERN;
        }
    }
    {
        const char *presenter = read_string(display, "presenter");

        if (presenter != NULL && version < 2 && strcmp(presenter, "sdl") == 0) {
            migrated = 1;
        } else if (presenter != NULL) {
            s->presenter = strcmp(presenter, "vulkan") == 0 ? PORT_PRESENTER_VULKAN : PORT_PRESENTER_SDL;
        }
    }
    audio = cJSON_GetObjectItemCaseSensitive(document, "audio");
    s->volume = (int)read_number(audio, "volume", s->volume, 0, 100);
    s->mute = read_bool(audio, "mute", s->mute);
    read_controls(cJSON_GetObjectItemCaseSensitive(document, "controls"));
    s->myus_necklace = read_bool(cJSON_GetObjectItemCaseSensitive(document, "mods"), "myus_necklace", s->myus_necklace);
    s->separate_run = read_bool(cJSON_GetObjectItemCaseSensitive(document, "mods"), "separate_run", s->separate_run);
    port_stats_settings_read(cJSON_GetObjectItemCaseSensitive(document, "mods")); /* the extended stats mod */
    port_automap_settings_read(cJSON_GetObjectItemCaseSensitive(document, "mods")); /* the automap mod */
    port_weapon_sets_settings_read(cJSON_GetObjectItemCaseSensitive(document, "mods")); /* the weapon sets mod */
    overlay = cJSON_GetObjectItemCaseSensitive(document, "overlay");
    s->pause_when_open = read_bool(overlay, "pause_when_open", s->pause_when_open);
    s->show_fps = read_bool(overlay, "show_fps", s->show_fps);
    s->ui_scale = (float)read_number(overlay, "scale", s->ui_scale, 0.75, 2.0);
    s->save_slot = (int)read_number(overlay, "save_slot", s->save_slot, 0, 9);
    s->quick_slot = (int)read_number(overlay, "quick_slot", s->quick_slot, 0, 9);

    /* the command line wins for this run: a switch it turned off stays off */
    if (port_options.deinterlace) {
        port_options.deinterlace = s->deinterlace;
    }
    if (port_options.progressive) {
        port_options.progressive = s->progressive;
    }
    if (port_options.anti_blur) {
        port_options.anti_blur = s->anti_blur;
    }
    if (port_options.adapt_width) {
        port_options.adapt_width = s->adapt_width;
    }
    if (port_options.whole_frame) {
        port_options.whole_frame = s->whole_frame;
    }
    if (port_options.seamless_scroll) {
        port_options.seamless_scroll = s->seamless_scroll;
    }
    if (!port_options.flicker_filter_forced) {
        port_options.flicker_filter = s->flicker_filter;
    }
    if (!port_options.widescreen_forced) {
        port_options.widescreen = s->widescreen;
        port_options.widescreen_hud_centred = s->widescreen_hud_centred;
    }
    if (!port_options.myus_necklace_forced) {
        port_options.myus_necklace = s->myus_necklace;
    }
    if (!port_options.separate_run_forced) {
        port_options.separate_run = s->separate_run;
    }
    if (!port_options.show_fps_forced) {
        port_options.show_fps = s->show_fps;
    }
    if (!port_options.frame_rate_forced) {
        port_options.frame_rate = s->frame_rate;
    }
    if (!port_options.frame_rate_cap_forced) {
        port_options.frame_rate_cap = s->frame_rate_cap;
    }
    if (!port_options.tearing_forced) {
        port_options.tearing = s->tearing;
    }
    if (!port_options.draw_distance_forced) {
        port_options.draw_distance = s->draw_distance;
    }
    if (!port_options.draw_distance_sky_forced) {
        port_options.draw_distance_sky = s->draw_distance_sky;
    }
    if (!port_options.renderer_forced) {
        /* (modern and fast: the native GS drawing the scene; main_host.c's renderer_implies does the rest) */
        port_options.renderer = s->renderer >= PORT_RENDERER_MODERN ? PORT_RENDERER_NATIVE : s->renderer;
        port_options.renderer_modern = s->renderer >= PORT_RENDERER_MODERN;
        port_options.renderer_fast = s->renderer == PORT_RENDERER_FAST;
    }
    if (!port_options.native_scale_forced) {
        port_options.native_scale = s->native_scale;
    }
    if (!port_options.native_widescreen_forced) {
        port_options.native_widescreen = s->native_widescreen;
    }
    if (!port_options.native_msaa_forced) {
        port_options.native_msaa = s->native_msaa;
    }
    if (!port_options.native_lod_forced) {
        port_options.native_lod = s->native_lod;
    }
    if (!port_options.native_aniso_forced) {
        port_options.native_aniso = s->native_aniso;
    }
    if (!port_options.native_cutout_aa_forced) {
        port_options.native_cutout_aa = s->native_cutout_aa;
    }
    if (!port_options.native_2d_forced) {
        port_options.native_2d = s->native_2d;
    }
    if (!port_options.native_colour_forced) {
        port_options.native_colour = s->native_colour;
    }
    if (!port_options.native_layer_z_forced) {
        port_options.native_layer_z = s->native_layer_z;
    }
    if (!port_options.native_shine_exact_forced) {
        port_options.native_shine_exact = s->native_shine_exact;
    }
    if (!port_options.native_sky_forced) {
        port_options.native_sky = s->native_sky;
    }
    if (!port_options.presenter_forced) {
        port_options.presenter = s->presenter;
    }
    if (!port_options.native_draw_forced) {
        port_options.native_draw = s->native_draw ? PORT_ND_ACCEPTED : 0;
    }
    if (port_options.controls == NULL) {
        port_options.controls = s->layout;
    }
    port_settings_apply_audio();
    if (file == NULL) {
        dirty = 1; /* the first run writes the defaults, so the file is there to find */
    }
    if (migrated && active == 1) {
        dirty = 1;
        fprintf(stderr, "[settings] from before the native GS was the default: renderer \"%s\", presenter \"%s\" "
                        "(the overlay's Debug page, or --renderer gs / --presenter sdl, go back)\n",
                renderer_name(s->renderer),
                s->presenter == PORT_PRESENTER_VULKAN ? "vulkan" : "sdl");
    }
    fprintf(stderr, "[settings] %s%s\n", port_settings_path(), file == NULL ? " (new)" : "");
    atexit(port_settings_save);
}

void port_settings_apply_audio(void) {
    port_audio_set_volume(port_settings.mute ? 0.0f : (float)port_settings.volume / 100.0f);
}

/* ---- Writing ---------------------------------------------------------------------------------- */
static void put(cJSON *parent, const char *key, cJSON *value) {
    if (cJSON_GetObjectItemCaseSensitive(parent, key) != NULL) {
        cJSON_ReplaceItemInObjectCaseSensitive(parent, key, value);
    } else {
        cJSON_AddItemToObject(parent, key, value);
    }
}

static cJSON *section(const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(document, key);

    if (!cJSON_IsObject(item)) {
        item = cJSON_CreateObject();
        put(document, key, item);
    }
    return item;
}

static void write_controls(cJSON *controls) {
    cJSON *bindings = cJSON_CreateObject();
    cJSON *menu = cJSON_CreateArray();
    int layout;
    int i;

    put(controls, "layout", cJSON_CreateString(port_settings.layout));
    put(controls, "mouse_sensitivity", cJSON_CreateNumber(port_controls_mouse_sensitivity()));
    put(controls, "invert_y", cJSON_CreateBool(port_controls_mouse_invert()));
    for (i = 0; i < 3; i++) {
        int action = port_controls_menu_mouse(i);

        cJSON_AddItemToArray(menu, cJSON_CreateString(action < 0 ? "none" : port_controls_action_id(action)));
    }
    put(controls, "menu_mouse", menu);
    put(controls, "menu_side_keys", cJSON_CreateBool(port_controls_menu_side_keys()));
    for (layout = 0; layout < CONTROLS_LAYOUTS; layout++) {
        cJSON *table = cJSON_CreateObject();
        int action;

        for (action = 0; action < ACTION_COUNT; action++) {
            cJSON *slots;
            int slot;

            if (port_controls_is_default(layout, action)) {
                continue;
            }
            slots = cJSON_CreateArray();
            for (slot = 0; slot < BINDING_SLOTS; slot++) {
                char text[64];

                port_controls_binding_text(port_controls_binding(layout, action, slot), text, sizeof(text));
                cJSON_AddItemToArray(slots, cJSON_CreateString(text));
            }
            cJSON_AddItemToObject(table, port_controls_action_id(action), slots);
        }
        cJSON_AddItemToObject(bindings, port_controls_layout_name(layout), table);
    }
    put(controls, "bindings", bindings);
}

void port_settings_save(void) {
    const PortSettings *s = &port_settings;
    cJSON *display;
    cJSON *audio;
    cJSON *overlay;
    char temporary[1200];
    char *text;
    FILE *file;

    if (!active || !dirty) {
        return;
    }
    dirty = 0;
    if (active == 2) {
        return; /* a broken file is left for the player to mend */
    }
    put(document, "version", cJSON_CreateNumber(SETTINGS_VERSION));
    display = section("display");
    put(display, "window_height", cJSON_CreateNumber(s->window_height));
    put(display, "window_width", cJSON_CreateNumber(s->window_width));
    cJSON_DeleteItemFromObjectCaseSensitive(display, "window_scale");
    put(display, "fullscreen", cJSON_CreateBool(s->fullscreen));
    put(display, "aspect", cJSON_CreateString(s->aspect == ASPECT_STRETCH ? "stretch" : "4:3"));
    put(display, "vsync", cJSON_CreateBool(s->vsync));
    put(display, "smooth", cJSON_CreateBool(s->smooth));
    put(display, "scaling", cJSON_CreateString(s->scaling == PORT_SCALING_SHARP    ? "sharp"
                                               : s->scaling == PORT_SCALING_SMOOTH ? "smooth"
                                                                                   : "best"));
    put(display, "sharpen", cJSON_CreateNumber(s->sharpen));
    put(display, "deinterlace", cJSON_CreateBool(s->deinterlace));
    put(display, "progressive", cJSON_CreateBool(s->progressive));
    put(display, "anti_blur", cJSON_CreateBool(s->anti_blur));
    put(display, "adapt_width", cJSON_CreateBool(s->adapt_width));
    put(display, "whole_frame", cJSON_CreateBool(s->whole_frame));
    put(display, "seamless_scroll", cJSON_CreateBool(s->seamless_scroll));
    put(display, "flicker_filter", cJSON_CreateBool(s->flicker_filter));
    put(display, "widescreen", cJSON_CreateBool(s->widescreen));
    put(display, "widescreen_hud", cJSON_CreateString(s->widescreen_hud_centred ? "centred" : "corner"));
    put(display, "frame_rate", s->frame_rate == PORT_FRAME_RATE_MONITOR    ? cJSON_CreateString("monitor")
                               : s->frame_rate == PORT_FRAME_RATE_UNLOCKED ? cJSON_CreateString("unlocked")
                                                                           : cJSON_CreateNumber(s->frame_rate));
    put(display, "frame_rate_cap", cJSON_CreateNumber(s->frame_rate_cap));
    put(display, "tearing", cJSON_CreateBool(s->tearing));
    put(display, "draw_distance", cJSON_CreateString(port_draw_distance_name(s->draw_distance)));
    put(display, "draw_distance_sky", cJSON_CreateString(s->draw_distance_sky ? "push" : "ps2"));
    put(display, "native_scale", s->native_scale >= 1 ? cJSON_CreateNumber(s->native_scale) : cJSON_CreateString("auto"));
    put(display, "native_widescreen", cJSON_CreateBool(s->native_widescreen));
    put(display, "native_msaa", cJSON_CreateNumber(s->native_msaa));
    put(display, "native_aniso", cJSON_CreateNumber(s->native_aniso));
    put(display, "native_lod", cJSON_CreateString(s->native_lod == 0 ? "ps2" : s->native_lod == 2 ? "full" : "scale"));
    put(display, "native_cutout_aa", cJSON_CreateString(s->native_cutout_aa == 0 ? "off" : "ssaa"));
    put(display, "native_2d", cJSON_CreateString(s->native_2d == 0 ? "ps2" : "smooth"));
    put(display, "native_colour", cJSON_CreateString(s->native_colour == 1 ? "high" : "exact"));
    put(display, "native_layer_z", cJSON_CreateString(s->native_layer_z == 0 ? "ps2" : "tolerant"));
    put(display, "native_draw", cJSON_CreateString(s->native_draw ? "on" : "off"));
    put(display, "native_shine", cJSON_CreateString(s->native_shine_exact ? "exact" : "whole"));
    put(display, "native_sky", cJSON_CreateString(s->native_sky == PORT_SKY_PS2 ? "ps2" : "modern"));
    put(display, "renderer", cJSON_CreateString(renderer_name(s->renderer)));
    put(display, "presenter", cJSON_CreateString(s->presenter == PORT_PRESENTER_VULKAN ? "vulkan" : "sdl"));
    audio = section("audio");
    put(audio, "volume", cJSON_CreateNumber(s->volume));
    put(audio, "mute", cJSON_CreateBool(s->mute));
    write_controls(section("controls"));
    put(section("mods"), "myus_necklace", cJSON_CreateBool(s->myus_necklace));
    put(section("mods"), "separate_run", cJSON_CreateBool(s->separate_run));
    port_stats_settings_write(section("mods")); /* the extended stats mod */
    port_automap_settings_write(section("mods")); /* the automap mod */
    port_weapon_sets_settings_write(section("mods")); /* the weapon sets mod */
    overlay = section("overlay");
    put(overlay, "pause_when_open", cJSON_CreateBool(s->pause_when_open));
    put(overlay, "show_fps", cJSON_CreateBool(s->show_fps));
    put(overlay, "scale", cJSON_CreateNumber((double)(int)(s->ui_scale * 100.0f + 0.5f) / 100.0));
    put(overlay, "save_slot", cJSON_CreateNumber(s->save_slot));
    put(overlay, "quick_slot", cJSON_CreateNumber(s->quick_slot));

    text = cJSON_Print(document);
    if (text == NULL) {
        return;
    }
    make_folders(port_data_dir());
    /* written whole, then put in place, so a crash never leaves half a file */
    snprintf(temporary, sizeof(temporary), "%s.new", port_settings_path());
    file = fopen(temporary, "wb");
    if (file == NULL) {
        fprintf(stderr, "[settings] cannot write %s: %s\n", temporary, strerror(errno));
        cJSON_free(text);
        return;
    }
    fputs(text, file);
    fputc('\n', file);
    fclose(file);
    cJSON_free(text);
#ifdef _WIN32
    if (!MoveFileExA(temporary, port_settings_path(), MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "[settings] cannot replace %s\n", port_settings_path());
    }
#else
    if (rename(temporary, port_settings_path()) != 0) {
        fprintf(stderr, "[settings] cannot replace %s: %s\n", port_settings_path(), strerror(errno));
    }
#endif
}

void port_settings_changed(void) {
    dirty = 1;
    dirty_since = port_wall_seconds();
}

void port_settings_poll(void) {
    if (dirty && port_wall_seconds() - dirty_since > 1.0) {
        port_settings_save();
    }
}
