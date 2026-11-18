/* The hands mod's port side (port/include/port/hands.h): its switch and command line, the keys' presses, and
   the hands kept per save, as the automap keeps its explored maps (port/platform/automap.c): hands/live.txt
   in the data folder is the playthrough's, and each save gets a copy (card_NN.txt for the memory card's save
   file NN, quick_<file>.txt for a quick save), brought back when that save is loaded. A save made without
   one brings back the weapon sets mod's copy of it (weapon_sets/, that mod's folder) when there is one, else
   empty hands; a new game starts with empty ones. The game's side is game/hands.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

#include "cJSON.h"
#include "port/hands.h"
#include "port/platform.h"
#include "port/settings.h"

PortHandsOptions port_hands = {0, 0, 1};
HandsState port_hands_state;

static unsigned long long pressed_at[HANDS_KEY_COUNT];
static int started;
static int loaded_since_title = 1; /* a save was loaded since the title was reached */
static int at_title;

int port_hands_option(int argc, char **argv, int *i) {
    (void)argc;
    if (strcmp(argv[*i], "--hands") == 0 || strcmp(argv[*i], "--weapon-sets") == 0) {
        port_hands.enabled = port_hands.forced = 1;
        return 1;
    }
    return 0;
}

/* ---- settings.json ------------------------------------------------------------------------------- */
void port_hands_settings_read(const void *mods_json) {
    const cJSON *mods = mods_json;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(mods, "hands");
    const cJSON *fade = cJSON_GetObjectItemCaseSensitive(mods, "hands_fade");

    if (!cJSON_IsBool(item)) {
        item = cJSON_GetObjectItemCaseSensitive(mods, "weapon_sets"); /* the mod it replaces */
    }
    if (!port_hands.forced && cJSON_IsBool(item)) {
        port_hands.enabled = cJSON_IsTrue(item);
    }
    if (cJSON_IsBool(fade)) {
        port_hands.fade = cJSON_IsTrue(fade);
    }
}

static void put(cJSON *mods, const char *name, int on) {
    cJSON *value = cJSON_CreateBool(on);

    if (cJSON_GetObjectItemCaseSensitive(mods, name) != NULL) {
        cJSON_ReplaceItemInObjectCaseSensitive(mods, name, value);
    } else {
        cJSON_AddItemToObject(mods, name, value);
    }
}

void port_hands_settings_write(void *mods_json) {
    cJSON *mods = mods_json;

    put(mods, "hands", port_hands.enabled);
    put(mods, "hands_fade", port_hands.fade);
    cJSON_DeleteItemFromObjectCaseSensitive(mods, "weapon_sets");
}

void port_hands_set_enabled(int on) {
    port_hands.enabled = on != 0;
    port_settings_changed();
}

void port_hands_set_fade(int on) {
    port_hands.fade = on != 0;
    port_settings_changed();
}

/* ---- The keys ------------------------------------------------------------------------------------ */
void port_hands_key(HandsKey key) {
    if (port_hands.enabled && key >= 0 && key < HANDS_KEY_COUNT) {
        pressed_at[key] = port_vblank_count() + 1; /* never 0 */
    }
}

unsigned long long port_hands_pressed(HandsKey key) {
    return key >= 0 && key < HANDS_KEY_COUNT ? pressed_at[key] : 0;
}

void port_hands_take_press(HandsKey key) {
    if (key >= 0 && key < HANDS_KEY_COUNT) {
        pressed_at[key] = 0;
    }
}

static const char *const event_names[HANDS_KEY_COUNT] = {
    [HANDS_KEY_WEAPON] = "weapon_swap", /* the weapon sets mod's name */
    [HANDS_KEY_SPELL] = "spell_swap",
    [HANDS_KEY_HOTKEY_NEXT] = "hotkey_next",
    [HANDS_KEY_HOTKEY_PREVIOUS] = "hotkey_previous",
};

const char *port_hands_event_name(HandsKey key) {
    return key >= 0 && key < HANDS_KEY_COUNT ? event_names[key] : "?";
}

int port_hands_event_key(const char *name) {
    int key;

    for (key = 0; key < HANDS_KEY_COUNT; key++) {
        if (strcmp(name, event_names[key]) == 0) {
            return key;
        }
    }
    return -1;
}

/* ---- The files ----------------------------------------------------------------------------------- */
static void folder(char *out, size_t size) {
    snprintf(out, size, "%s/hands", port_data_dir());
#ifdef _WIN32
    _mkdir(port_data_dir());
    _mkdir(out);
#else
    mkdir(port_data_dir(), 0755);
    mkdir(out, 0755);
#endif
}

static int write_hands(const char *name) {
    char dir[1024], path[1200], temp[1300], line[256];
    FILE *file;
    int ok;

    if (hands_format(&port_hands_state, line, sizeof(line)) == 0) {
        return 0;
    }
    folder(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(temp, sizeof(temp), "%s.part", path);
    file = fopen(temp, "w");
    ok = file != NULL && fputs(line, file) >= 0;
    if (file != NULL) {
        ok = fclose(file) == 0 && ok;
    }
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(temp, path) == 0;
#endif
    }
    if (!ok) {
        remove(temp);
        fprintf(stderr, "[hands] cannot write %s\n", path);
    }
    return ok;
}

/* The hands from the file in `sub` (hands, or weapon_sets: the mod this one replaces); 0, and the hands left
   alone, when it is not there or not one. Nothing is made to read. */
static int read_file(const char *sub, const char *name) {
    char path[1200], line[256];
    FILE *file;
    int ok;

    snprintf(path, sizeof(path), "%s/%s/%s", port_data_dir(), sub, name);
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    ok = fgets(line, sizeof(line), file) != NULL && hands_parse(line, &port_hands_state);
    fclose(file);
    if (!ok) {
        fprintf(stderr, "[hands] %s is not the hands' file, left alone\n", path);
    }
    return ok;
}

static int read_hands(const char *name) {
    return read_file("hands", name) || read_file("weapon_sets", name);
}

static void start(void) {
    char line[256];

    if (started) {
        return;
    }
    started = 1;
    if (read_hands("live.txt") && hands_format(&port_hands_state, line, sizeof(line)) > 0) {
        fprintf(stderr, "[hands] read from the live file: %s", line);
    }
}

void port_hands_changed(void) {
    started = 1; /* the hands as they are now are the ones kept, not live.txt's */
    if (port_hands.enabled) {
        write_hands("live.txt");
    }
}

/* a save's copy, or empty hands when the save has none */
static void bring_back(const char *name, const char *what) {
    start();
    loaded_since_title = 1;
    if (read_hands(name)) {
        port_event("hands: those of %s brought back", what);
    } else {
        memset(&port_hands_state, 0, sizeof(port_hands_state));
        port_event("hands: %s has none, they are empty", what);
    }
    write_hands("live.txt");
}

/* /BASLUS-20318S/BASLUS-20318G05: the game's save file 05 */
void port_hands_card_file(const char *name, int writing) {
    const char *g;
    char file[64], what[64];

    if (!port_hands.enabled || name == NULL || (g = strstr(name, "BASLUS-20318G")) == NULL) {
        return;
    }
    g += strlen("BASLUS-20318G");
    if (g[0] < '0' || g[0] > '9' || g[1] < '0' || g[1] > '9' || g[2] != 0) {
        return;
    }
    snprintf(file, sizeof(file), "card_%c%c.txt", g[0], g[1]);
    snprintf(what, sizeof(what), "memory card save %c%c", g[0], g[1]);
    if (writing) {
        start();
        if (write_hands(file)) {
            port_event("hands: kept with %s", what);
        }
    } else {
        bring_back(file, what);
    }
}

static void quick_name(const char *path, char *out, size_t size) {
    const char *base = path, *p;

    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    snprintf(out, size, "quick_%s.txt", base);
}

void port_hands_quick_saved(const char *path) {
    char file[300];

    if (!port_hands.enabled) {
        return;
    }
    start();
    quick_name(path, file, sizeof(file));
    write_hands(file);
}

void port_hands_quick_loading(const char *path) {
    char file[300];

    if (!port_hands.enabled) {
        return;
    }
    quick_name(path, file, sizeof(file));
    bring_back(file, file);
}

/* (game/quicksave.c) the title was reached: is the next game a new one? */
void port_hands_title(void) {
    if (port_hands.enabled) {
        start();
    }
    if (!at_title) {
        at_title = 1;
        loaded_since_title = 0;
    }
}

int port_hands_new_game(void) {
    int fresh = at_title && !loaded_since_title;

    at_title = 0;
    loaded_since_title = 1;
    return fresh;
}
