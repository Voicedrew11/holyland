/* The automap mod's port side (port/include/port/automap.h, docs/port/AUTOMAP.md): its settings and command
   line, the full map's open state, and the files that keep the fog of war per save. The game's side is
   game/automap.c, the drawing port/overlay/automap_view.cpp. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

#include "cJSON.h"
#include "port/file_replace.h"
#include "port/automap.h"
#include "port/platform.h"
#include "port/settings.h"

PortAutomapSettings port_automap = {
    0,  /* enabled */
    1,  /* minimap */
    0,  /* top right */
    24, /* size */
    30, /* reach: 3000 units to the edge */
    85, /* opacity */
    92, /* full map background */
    0, 0, 0, 0, 0,
};

static int enabled_forced, cheats_forced;
static int full_open;
static int dirty;
static double dirty_since = -1.0;
static int loaded_since_title = 1; /* a save was loaded since the title was reached */
static int at_title;

int port_automap_option(int argc, char **argv, int *i) {
    (void)argc;
    if (strcmp(argv[*i], "--automap") == 0) {
        port_automap.enabled = enabled_forced = 1;
        return 1;
    }
    if (strcmp(argv[*i], "--automap-cheats") == 0) {
        port_automap.enabled = enabled_forced = 1;
        port_automap.reveal = port_automap.items = port_automap.enemies = port_automap.secrets = port_automap.hints = 1;
        cheats_forced = 1;
        return 1;
    }
    return 0;
}

/* ---- settings.json ------------------------------------------------------------------------------ */
static int read_bool(const cJSON *object, const char *key, int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);

    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static int read_int(const cJSON *object, const char *key, int fallback, int low, int high) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    int value = cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;

    return value < low ? low : value > high ? high : value;
}

void port_automap_settings_read(const void *mods_json) {
    const cJSON *mods = mods_json;
    PortAutomapSettings *s = &port_automap;

    if (!enabled_forced) {
        s->enabled = read_bool(mods, "automap", s->enabled);
    }
    s->minimap = read_bool(mods, "automap_minimap", s->minimap);
    s->minimap_corner = read_int(mods, "automap_minimap_corner", s->minimap_corner, 0, 3);
    s->minimap_size = read_int(mods, "automap_minimap_size", s->minimap_size, 10, 45);
    s->minimap_zoom = read_int(mods, "automap_minimap_zoom", s->minimap_zoom, 20, 120);
    s->opacity = read_int(mods, "automap_opacity", s->opacity, 20, 100);
    s->full_opacity = read_int(mods, "automap_full_opacity", s->full_opacity, 40, 100);
    if (!cheats_forced) {
        s->reveal = read_bool(mods, "automap_reveal", s->reveal);
        s->items = read_bool(mods, "automap_items", s->items);
        s->enemies = read_bool(mods, "automap_enemies", s->enemies);
        s->secrets = read_bool(mods, "automap_secrets", s->secrets);
        s->hints = read_bool(mods, "automap_hints", s->hints);
    }
}

static void put(cJSON *parent, const char *key, cJSON *value) {
    if (cJSON_GetObjectItemCaseSensitive(parent, key) != NULL) {
        cJSON_ReplaceItemInObjectCaseSensitive(parent, key, value);
    } else {
        cJSON_AddItemToObject(parent, key, value);
    }
}

void port_automap_settings_write(void *mods_json) {
    cJSON *mods = mods_json;
    const PortAutomapSettings *s = &port_automap;

    put(mods, "automap", cJSON_CreateBool(s->enabled));
    put(mods, "automap_minimap", cJSON_CreateBool(s->minimap));
    put(mods, "automap_minimap_corner", cJSON_CreateNumber(s->minimap_corner));
    put(mods, "automap_minimap_size", cJSON_CreateNumber(s->minimap_size));
    put(mods, "automap_minimap_zoom", cJSON_CreateNumber(s->minimap_zoom));
    put(mods, "automap_opacity", cJSON_CreateNumber(s->opacity));
    put(mods, "automap_full_opacity", cJSON_CreateNumber(s->full_opacity));
    put(mods, "automap_reveal", cJSON_CreateBool(s->reveal));
    put(mods, "automap_items", cJSON_CreateBool(s->items));
    put(mods, "automap_enemies", cJSON_CreateBool(s->enemies));
    put(mods, "automap_secrets", cJSON_CreateBool(s->secrets));
    put(mods, "automap_hints", cJSON_CreateBool(s->hints));
}

void port_automap_settings_changed(void) {
    if (!port_automap.enabled) {
        full_open = 0;
    }
    port_settings_changed();
}

/* ---- The full map ------------------------------------------------------------------------------- */
int port_automap_full_open(void) {
    return full_open && port_automap.enabled;
}

void port_automap_set_full_open(int open) {
    PortAutomapPlayer player;

    if (open) {
        port_automap_player(&player);
        if (!port_automap.enabled || !player.in_play || port_automap_plan() == NULL) {
            return;
        }
    }
    if (full_open && !open) {
        port_automap_save_live(0);
    }
    if (!open) {
        port_automap_browse_home(); /* closed, the map shows the player's own when it opens again */
    }
    full_open = open;
}

int port_automap_holds_game(void) {
    return port_automap_full_open();
}

/* ---- The files ---------------------------------------------------------------------------------- */
static void folder(char *out, size_t size) {
    snprintf(out, size, "%s/automap", port_data_dir());
#ifdef _WIN32
    _mkdir(port_data_dir());
    _mkdir(out);
#else
    mkdir(port_data_dir(), 0755);
    mkdir(out, 0755);
#endif
}

static int write_state(const char *name) {
    char dir[1024], path[1200], temp[1300];
    uint8_t *data = NULL;
    size_t size;
    FILE *file;
    int ok;

    Automap_Flush();
    size = Automap_Serialize(&data);
    if (data == NULL) {
        return 0;
    }
    folder(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(temp, sizeof(temp), "%s.part", path);
    file = fopen(temp, "wb");
    ok = file != NULL && fwrite(data, 1, size, file) == size;
    if (file != NULL) {
        ok = fclose(file) == 0 && ok;
    }
    free(data);
    if (ok) {
        ok = port_replace_file(temp, path);
    }
    if (!ok) {
        remove(temp);
        fprintf(stderr, "[automap] cannot write %s\n", path);
    }
    return ok;
}

/* 1 when the file was there and read */
static int read_state(const char *name) {
    char dir[1024], path[1200];
    uint8_t *data;
    long size;
    FILE *file;

    folder(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = size > 0 ? malloc((size_t)size) : NULL;
    if (data == NULL || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    Automap_Deserialize(data, (size_t)size);
    free(data);
    return 1;
}

void port_automap_mark_dirty(void) {
    if (!dirty) {
        dirty_since = port_wall_seconds();
    }
    dirty = 1;
}

void port_automap_save_live(int force) {
    if (!port_automap.enabled || (!dirty && !force)) {
        return;
    }
    if (!force && port_wall_seconds() - dirty_since < 5.0) {
        return;
    }
    if (write_state("live.bin")) {
        dirty = 0;
    }
}

static void at_exit(void) {
    if (dirty) {
        port_automap_save_live(1);
    }
}

/* (game/automap.c, once, as the mod first runs) live.bin into the game's side */
void port_automap_start(void) {
    static int started;

    if (started) {
        return;
    }
    started = 1;
    atexit(at_exit);
    if (read_state("live.bin")) {
        fprintf(stderr, "[automap] the explored maps read from automap/live.bin\n");
    }
}

/* the game's save `number` (1 to 3: port/include/port/save_files.h): automap/save<number>.bin */
void port_automap_save_file(int number, int writing) {
    char file[64];

    if (!port_automap.enabled) {
        return;
    }
    snprintf(file, sizeof(file), "save%d.bin", number);
    if (writing) {
        if (write_state(file)) {
            port_event("automap: the explored maps kept with save %d", number);
        }
        port_automap_save_live(1);
    } else {
        loaded_since_title = 1;
        if (read_state(file)) {
            port_event("automap: the explored maps of save %d brought back", number);
            port_automap_mark_dirty();
        }
    }
}

static void quick_name(const char *path, char *out, size_t size) {
    const char *base = path, *p;

    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    snprintf(out, size, "quick_%s.bin", base);
}

void port_automap_quick_saved(const char *path) {
    char file[300];

    if (!port_automap.enabled) {
        return;
    }
    quick_name(path, file, sizeof(file));
    write_state(file);
    port_automap_save_live(1);
}

/* the file kept with a quick save that is gone (whether or not the mod is on now: it may have been then) */
void port_automap_quick_removed(const char *path) {
    char dir[1024], name[300], file[1400];

    quick_name(path, name, sizeof(name));
    folder(dir, sizeof(dir));
    snprintf(file, sizeof(file), "%s/%s", dir, name);
    remove(file); /* (none is fine: the mod was off) */
}

void port_automap_quick_loading(const char *path) {
    char file[300];

    if (!port_automap.enabled) {
        return;
    }
    loaded_since_title = 1;
    quick_name(path, file, sizeof(file));
    if (read_state(file)) {
        port_event("automap: the explored maps of %s brought back", file);
        port_automap_mark_dirty();
    }
}

void port_automap_state_loaded(void) {
    at_title = 0;
    loaded_since_title = 1;
    port_automap_mark_dirty();
}

/* (game/automap.c) the title was reached: is the next game a new one? */
void port_automap_title(void) {
    if (!at_title) {
        at_title = 1;
        loaded_since_title = 0;
    }
}

int port_automap_new_game(void) {
    int fresh = at_title && !loaded_since_title;

    at_title = 0;
    loaded_since_title = 1;
    return fresh;
}
