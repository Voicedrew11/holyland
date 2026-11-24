/* Mods' plugins and the API they are given (port/include/port/mod_api.h, modding/include/holyland_mod.h). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef KF4_HAVE_SDL
#include <SDL3/SDL.h>
#endif

#include "port/controls.h"
#include "port/host.h"
#include "port/mod_api.h"
#include "port/mods.h"
#include "port/platform.h"
#include "port/settings.h"
#include "port/version.h"

#define HANDLERS 48

void port_address_add_module(const void *inside); /* port/platform/ee_address.c */

struct HlMod {
    PortMod *info;
    void *library;
    char data_folder[1200];
    int handler_count;
    struct {
        HlEvent event;
        HlHandler handler;
        void *user;
    } handlers[HANDLERS];
};

static HlMod plugins[64];
static int plugin_count;
static HlMod *page_mod; /* the mod whose page is open (ui_set_open) */
static int started;     /* HL_EVENT_START sent */

/* ---- The API's functions ---------------------------------------------------------------------------------- */
static void api_log(HlMod *mod, const char *format, ...) {
    char line[2048];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(stderr, "[%s] %s\n", mod != NULL ? mod->info->id : "mod", line);
}

static const char *api_mod_id(HlMod *mod) {
    return mod->info->id;
}

static const char *api_mod_folder(HlMod *mod) {
    return mod->info->folder;
}

static void make_folders(const char *folder) {
    char path[1200];
    size_t i;

    snprintf(path, sizeof(path), "%s", folder);
    for (i = 1; path[i] != 0; i++) {
        if ((path[i] == '/' || path[i] == '\\') && path[i - 1] != ':') {
            char c = path[i];

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

static const char *api_data_folder(HlMod *mod) {
    if (!mod->data_folder[0]) {
        snprintf(mod->data_folder, sizeof(mod->data_folder), "%s/mod-data/%s", port_data_dir(), mod->info->id);
        make_folders(mod->data_folder);
    }
    return mod->data_folder;
}

static const char *api_holyland_version(void) {
    return port_version[0] ? port_version : "dev";
}

static int api_mod_active(const char *id) {
    return port_mods_active(id);
}

static const char *api_find_file(const char *file) {
    return port_mods_find(file);
}

static PortModSetting *find_setting(HlMod *mod, const char *key) {
    int i;

    for (i = 0; key != NULL && i < mod->info->setting_count; i++) {
        if (strcmp(mod->info->settings[i].key, key) == 0) {
            return &mod->info->settings[i];
        }
    }
    return NULL;
}

static int api_setting(HlMod *mod, const char *key) {
    PortModSetting *set = find_setting(mod, key);

    return set != NULL ? set->value : 0;
}

static void api_set_setting(HlMod *mod, const char *key, int value) {
    PortModSetting *set = find_setting(mod, key);

    if (set != NULL) {
        set->value = value < set->min ? set->min : value > set->max ? set->max : value;
        port_settings_changed();
    }
}

static int find_action(HlMod *mod, const char *key) {
    int i;

    for (i = 0; key != NULL && i < mod->info->key_count; i++) {
        if (strcmp(mod->info->keys[i].key, key) == 0) {
            return mod->info->keys[i].action;
        }
    }
    return -1;
}

static int api_key_pressed(HlMod *mod, const char *key) {
    return port_controls_mod_presses(find_action(mod, key));
}

static int api_key_held(HlMod *mod, const char *key) {
    return port_controls_mod_held(find_action(mod, key));
}

static const char *api_key_name(HlMod *mod, const char *key) {
    static char names[4][64];
    static int turn;
    int action = find_action(mod, key), binding;
    char *out = names[turn++ & 3];

    out[0] = 0;
    if (action >= 0 && (binding = port_controls_binding(action, 0)) != BINDING_NONE) {
        port_controls_binding_text(binding, out, sizeof(names[0]));
    }
    return out;
}

static void api_on(HlMod *mod, HlEvent event, HlHandler handler, void *user) {
    if (mod->handler_count >= HANDLERS || handler == NULL) {
        api_log(mod, "too many event handlers (%d), one left out", HANDLERS);
        return;
    }
    mod->handlers[mod->handler_count].event = event;
    mod->handlers[mod->handler_count].handler = handler;
    mod->handlers[mod->handler_count].user = user;
    mod->handler_count++;
}

static void *api_game_memory(uint32_t address) {
    if (address == 0) {
        return NULL;
    }
    if (address < EE_RAM_SIZE) {
        return ee_ram + address;
    }
    if (address >= EE_SCRATCHPAD_ADDRESS && address < EE_SCRATCHPAD_ADDRESS + sizeof(ee_scratchpad)) {
        return ee_scratchpad + (address - EE_SCRATCHPAD_ADDRESS);
    }
    return ee_static_ptr_of(address);
}

static uint32_t api_game_address(const void *pointer) {
    EEWord word = 0;

    return pointer != NULL && ee_word_try(pointer, &word) ? word : 0;
}

/* The mods' part of game RAM: its top 16 MB (the PS2's 32 MB, then room the port keeps; port/include/port/host.h) */
#define MOD_ARENA_START 0x3000000u
#define MOD_ARENA_END EE_RAM_SIZE
static uint32_t arena_next = MOD_ARENA_START;

static void *api_game_alloc(HlMod *mod, size_t size) {
    uint32_t at = (arena_next + 63u) & ~63u;
    void *block;

    if (size == 0 || size > MOD_ARENA_END - at) {
        api_log(mod, "game_alloc: no room for %zu bytes (%u left of the mods' 16 MB)", size, MOD_ARENA_END - at);
        return NULL;
    }
    arena_next = at + (uint32_t)size;
    block = ee_ram + at;
    memset(block, 0, size);
    return block;
}

static void *api_symbol(const char *name) {
    return port_mod_symbol(name);
}

static int api_hook(HlMod *mod, const char *name, void *replacement, void **original) {
    if (started) {
        api_log(mod, "hook %s: too late (only in hl_mod_init or HL_EVENT_START)", name);
        return 0;
    }
    return port_mod_hook(mod->info->id, name, replacement, original);
}

static void *api_ui_context(void) {
    return port_player_ui_context();
}

static int api_ui_set_open(HlMod *mod, int open) {
    return port_mod_page_set(mod, open);
}

static float api_ui_scale(void) {
    return port_player_ui_scale();
}

static int api_key_is(HlMod *mod, const char *key, const void *sdl_event) {
#ifdef KF4_HAVE_SDL
    const SDL_Event *e = sdl_event;
    int action = find_action(mod, key), slot;

    if (action < 0 || e == NULL) {
        return 0;
    }
    for (slot = 0; slot < BINDING_SLOTS; slot++) {
        int binding = port_controls_binding(action, slot), code = BINDING_CODE(binding);

        switch (BINDING_KIND(binding)) {
        case BINDING_KEY:
            if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && (int)e->key.scancode == code) {
                return 1;
            }
            break;
        case BINDING_MOUSE:
            if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && (int)e->button.button == code) {
                return 1;
            }
            break;
        case BINDING_PAD:
            if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && (int)e->gbutton.button == code) {
                return 1;
            }
            break;
        default:
            break;
        }
    }
#else
    (void)mod, (void)key, (void)sdl_event;
#endif
    return 0;
}

static const HlApi api = {
    sizeof(HlApi), HL_API_MAJOR, HL_API_MINOR,
    api_log, api_mod_id, api_mod_folder, api_data_folder, api_holyland_version,
    api_mod_active, api_find_file,
    api_setting, api_set_setting, api_key_pressed, api_key_held, api_key_name,
    api_on,
    api_game_memory, api_game_address, api_game_alloc, api_symbol, api_hook,
    api_ui_context, api_ui_set_open, api_ui_scale, api_key_is,
};

static void shutdown_event(void) {
    port_mod_event(HL_EVENT_SHUTDOWN, NULL);
}

/* ---- Loading ---------------------------------------------------------------------------------------------- */
static void *open_library(const char *path, char *why, size_t size) {
#ifdef _WIN32
    wchar_t wide[1200];
    HMODULE library;

    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1200) == 0) {
        snprintf(why, size, "the path is too long");
        return NULL;
    }
    /* (its own folder searched for the DLLs it needs) */
    library = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (library == NULL) {
        snprintf(why, size, "Windows error %lu", (unsigned long)GetLastError());
    }
    return library;
#else
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);

    if (library == NULL) {
        snprintf(why, size, "%s", dlerror());
    }
    return library;
#endif
}

static void *library_symbol(void *library, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)library, name);
#else
    return dlsym(library, name);
#endif
}

void port_mod_plugins_load(void) {
    int i;

    for (i = 0; i < port_mods_count() && plugin_count < 64; i++) {
        PortMod *info = port_mods_get(i);
        HlMod *mod;
        HlModInit init;
        char why[512] = "";
        int answer;

        if (!info->applied || !info->has_plugin) {
            continue;
        }
        if (!info->plugin[0]) {
            fprintf(stderr, "[mods] %s: its plugin is not built for this system; its files and text only\n", info->id);
            continue;
        }
        if (info->api != HL_API_MAJOR) {
            fprintf(stderr, "[mods] %s: made for API %d, Holyland has API %d: its plugin is not loaded\n", info->id,
                    info->api, HL_API_MAJOR);
            continue;
        }
        if (info->holyland[0] && strcmp(info->holyland, api_holyland_version()) != 0) {
            fprintf(stderr, "[mods] %s: made for Holyland %s, this is %s: if it uses the game's own functions, they "
                            "may have changed\n", info->id, info->holyland, api_holyland_version());
        }
        mod = &plugins[plugin_count];
        memset(mod, 0, sizeof(*mod));
        mod->info = info;
        mod->library = open_library(info->plugin, why, sizeof(why));
        if (mod->library == NULL) {
            fprintf(stderr, "[mods] %s: %s could not be loaded: %s\n", info->id, info->plugin, why);
            continue;
        }
        init = (HlModInit)library_symbol(mod->library, HL_MOD_INIT_NAME);
        if (init == NULL) {
            fprintf(stderr, "[mods] %s: %s has no %s\n", info->id, info->plugin, HL_MOD_INIT_NAME);
            continue;
        }
        port_address_add_module((const void *)init); /* (its buffers given EE addresses, as the program's) */
        plugin_count++;
        answer = init(&api, mod);
        if (answer != HL_API_MAJOR) {
            fprintf(stderr, "[mods] %s: its plugin stepped aside (%d)\n", info->id, answer);
            mod->handler_count = 0;
            continue;
        }
        fprintf(stderr, "[mods] %s: plugin loaded (%s)\n", info->id, info->plugin);
    }
    if (plugin_count > 0) {
        atexit(shutdown_event);
    }
}

int port_mod_plugins_any(void) {
    return plugin_count > 0;
}

/* ---- Events ----------------------------------------------------------------------------------------------- */
static int send_to(HlMod *mod, HlEvent event, void *data) {
    int i;

    for (i = 0; i < mod->handler_count; i++) {
        if (mod->handlers[i].event == event) {
            int taken = mod->handlers[i].handler(mod, event, data, mod->handlers[i].user);

            if (taken) {
                return taken;
            }
        }
    }
    return 0;
}

int port_mod_event(HlEvent event, void *data) {
    int i;

    if (plugin_count == 0) {
        return 0;
    }
    if (!started && event != HL_EVENT_START && event != HL_EVENT_SHUTDOWN && event != HL_EVENT_UI_POLL &&
        event != HL_EVENT_UI_INPUT && event != HL_EVENT_UI_BEFORE && event != HL_EVENT_UI_AFTER) {
        /* (the game's thread, before its first event) */
        started = 1;
        port_mod_event(HL_EVENT_START, NULL);
    }
    if (event == HL_EVENT_UI_INPUT || event == HL_EVENT_UI_BEFORE || event == HL_EVENT_UI_AFTER) {
        return page_mod != NULL ? send_to(page_mod, event, data) : 0;
    }
    for (i = 0; i < plugin_count; i++) {
        int taken = send_to(&plugins[i], event, data);

        if (taken) {
            return taken;
        }
    }
    return 0;
}

static int at_title, loaded_since_title;

static void save_event(HlEvent event, const char *key, int slot) {
    HlSave save = {key, slot};

    if (event == HL_EVENT_SAVE_LOADED) {
        loaded_since_title = 1;
    }
    port_mod_event(event, &save);
}

void port_mod_save_file(int slot, int writing) {
    char key[16];

    snprintf(key, sizeof(key), "save%d", slot);
    save_event(writing ? HL_EVENT_SAVE_WRITTEN : HL_EVENT_SAVE_LOADED, key, slot);
}

static void quick_key(const char *path, char *out, size_t size) {
    const char *base = path, *p;

    for (p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    snprintf(out, size, "quick_%s", base);
}

void port_mod_quick_saved(const char *path) {
    char key[300];

    quick_key(path, key, sizeof(key));
    save_event(HL_EVENT_SAVE_WRITTEN, key, 0);
}

void port_mod_quick_loading(const char *path) {
    char key[300];

    quick_key(path, key, sizeof(key));
    save_event(HL_EVENT_SAVE_LOADED, key, 0);
}

/* ---- The menus -------------------------------------------------------------------------------------------- */
HlMod *port_mod_page_open(void) {
    return page_mod;
}

int port_mod_page_set(HlMod *mod, int open) {
    if (open) {
        if (page_mod == mod) {
            return 1;
        }
        if (page_mod != NULL || port_player_ui_menu_open()) {
            return 0;
        }
        page_mod = mod;
        return 1;
    }
    if (page_mod == mod) {
        page_mod = NULL;
    }
    return 1;
}

/* ---- The title and a new game ------------------------------------------------------------------------------- */
void port_mod_title(void) { /* (game/quicksave.c, each frame at the title) */
    if (!at_title) {
        at_title = 1;
        loaded_since_title = 0;
        port_mod_event(HL_EVENT_TITLE, NULL);
    }
}

void port_mod_in_play(void) { /* (game/mod_events.c) */
    if (at_title) {
        at_title = 0;
        if (!loaded_since_title) {
            port_mod_event(HL_EVENT_NEW_GAME, NULL);
        }
    }
}

#ifndef KF4_HAVE_PLAYER_UI
/* (no player menu in this build: no pages for the mods) */
void *port_player_ui_context(void) {
    return NULL;
}

int port_player_ui_menu_open(void) {
    return 0;
}

float port_player_ui_scale(void) {
    return 1.0f;
}
#endif
