/* holyland_mod.h: what a Holyland mod's plugin builds against (API 1).

   A mod is a folder in Holyland's mods folder (<data folder>/mods/<mod>). Its mod.json names it, says what it
   needs, declares its settings and keys, and names its plugin for each system:

     {
       "name": "Journal", "version": "1.0", "author": "...", "description": "...",
       "api": 1,
       "plugin": { "windows-x64": "bin/windows-x64/journal.dll", "linux-x64": "bin/linux-x64/journal.so" },
       "settings": [ { "key": "show_all", "label": "Show Everything", "help": "...", "type": "toggle", "default": 0 },
                     { "key": "size", "label": "Size", "type": "choice", "choices": ["Small", "Large"], "default": 0 },
                     { "key": "speed", "label": "Speed", "type": "number", "min": 1, "max": 8, "step": 1, "default": 2 } ],
       "keys": [ { "key": "open", "label": "Open the Journal", "default": "B" } ]
     }

   Holyland loads the plugin (a .dll on Windows, a .so on Linux) at start when the player has turned the mod on (a mod
   with a plugin starts off) and calls its one export:

     HL_EXPORT int hl_mod_init(const HlApi *api, HlMod *mod);

   which returns HL_API_MAJOR when it is ready, or 0 to step aside. In it the plugin keeps `api` and `mod` and asks
   for the events it wants (api->on). Holyland never unloads a plugin while the game runs.

   Threads: the events say where they are called. HL_THREAD_GAME is the game's own thread (its code runs there, on
   fibers): only there may a plugin call the game's functions or touch its memory. HL_THREAD_UI is the thread that
   draws the menus at each picture (the player menu, RmlUi).

   Stability: everything in HlApi keeps its meaning for the whole of API 1; later minor versions only add to the end
   of the table (check api->minor, or api->size, before using what a later minor added). The game's own functions
   and structures (api->symbol, hooks) are the decompilation's and may change with any Holyland release: a mod that
   uses them is built for the Holyland it names in mod.json ("holyland": "0.2.1"), and Holyland warns when that
   differs. docs/port/MODDING.md. */
#ifndef HOLYLAND_MOD_H
#define HOLYLAND_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HL_API_MAJOR 1
#define HL_API_MINOR 0

#if defined(_WIN32)
#define HL_EXPORT __declspec(dllexport)
#else
#define HL_EXPORT __attribute__((visibility("default")))
#endif

typedef struct HlMod HlMod; /* this mod, as Holyland knows it (opaque) */

/* ---- Events ----------------------------------------------------------------------------------------------- */
typedef enum {
    HL_EVENT_START = 1,     /* game: all mods are loaded and the disc is open, before the game's first frame */
    HL_EVENT_FRAME,         /* game: each game frame, after the pad is read (30 or 60 a second) */
    HL_EVENT_DRAW,          /* game: each game frame, last, over the HUD: the GS draws queued here are on top */
    HL_EVENT_TITLE,         /* game: the title screen was reached (the next game may be a new one) */
    HL_EVENT_NEW_GAME,      /* game: a game began from the title without a load */
    HL_EVENT_SAVE_WRITTEN,  /* game: a save was written: HlSave */
    HL_EVENT_SAVE_LOADED,   /* game: a save is being loaded: HlSave */
    HL_EVENT_UI_POLL,       /* ui: each picture, before the menus decide what they draw: HlUiPoll */
    HL_EVENT_UI_INPUT,      /* ui: a key, button, mouse or controller event (an SDL_Event): return 1 to take it */
    HL_EVENT_UI_BEFORE,     /* ui: each picture, before RmlUi's layout: HlUiSize */
    HL_EVENT_UI_AFTER,      /* ui: each picture, after RmlUi's layout: HlUiSize */
    HL_EVENT_SHUTDOWN       /* Holyland is closing (from whichever thread ends it): keep what is to be kept; the game
                               is not to be called */
} HlEvent;

/* HL_EVENT_SAVE_*: which save. Keep the mod's own data for it in a file named by `key` in api->data_folder(mod)
   ("save1" for the first of the save points' three saves, "quick_<file>" for a quick save) */
typedef struct {
    const char *key;
    int slot; /* 1 to 3 for the save points' saves, 0 for a quick save */
} HlSave;

typedef struct {
    int menu_open; /* the player menu or another page is open */
} HlUiPoll;

typedef struct {
    int width, height; /* the window, in pixels */
} HlUiSize;

/* An event's handler: `data` is the event's (above) or NULL; the return value matters only for HL_EVENT_UI_INPUT */
typedef int (*HlHandler)(HlMod *mod, HlEvent event, void *data, void *user);

/* ---- The API ---------------------------------------------------------------------------------------------- */
typedef struct HlApi {
    uint32_t size;  /* sizeof(HlApi) as Holyland has it */
    uint16_t major; /* HL_API_MAJOR */
    uint16_t minor; /* HL_API_MINOR or later */

    /* -- About this mod and Holyland -- */
    void (*log)(HlMod *mod, const char *format, ...); /* to the log, as "[<mod>] ..." */
    const char *(*mod_id)(HlMod *mod);                /* its folder's name ("kf4-journal") */
    const char *(*mod_folder)(HlMod *mod);            /* its folder, to read its own files */
    const char *(*data_folder)(HlMod *mod);           /* <data folder>/mod-data/<mod>/, made: its own files to write */
    const char *(*holyland_version)(void);            /* "0.2.1" */

    /* -- Other mods -- */
    int (*mod_active)(const char *mod_id);            /* 1 when that mod is on and was applied */
    /* the path of `file` in the last applied mod that has it ("journal.json"), NULL when none has; good until the
       next call */
    const char *(*find_file)(const char *file);

    /* -- Its settings and keys (mod.json) -- */
    int (*setting)(HlMod *mod, const char *key);              /* toggle 0/1, choice index, number; 0 unknown */
    void (*set_setting)(HlMod *mod, const char *key, int value);
    int (*key_pressed)(HlMod *mod, const char *key);          /* presses since the last call (in play or not) */
    int (*key_held)(HlMod *mod, const char *key);             /* 1 while held */
    /* the key's names as bound now ("B"), for prompts; "" none */
    const char *(*key_name)(HlMod *mod, const char *key);

    /* -- Events -- */
    void (*on)(HlMod *mod, HlEvent event, HlHandler handler, void *user);

    /* -- The game (HL_THREAD_GAME only) -- */
    void *(*game_memory)(uint32_t address);           /* a game (EE) address as a pointer; NULL outside its memory */
    uint32_t (*game_address)(const void *pointer);    /* and back; 0 outside it */
    /* Memory in game RAM, zeroed, 64-byte aligned: what the game's own code is given (a model, the file it was read
       from, packets it sends) must be here, not in the plugin's own variables or the C heap. 16 MB for all mods;
       never freed; saved in save states with the rest of game RAM. NULL when there is no more. */
    void *(*game_alloc)(HlMod *mod, size_t size);
    /* a function or variable of the game or of Holyland by name ("Model_SetUpSkinned", "game_status"), NULL none */
    void *(*symbol)(const char *name);
    /* Replaces the game function `name` with `replacement` (the same signature); *original receives what to call for
       the function as it was (the game's own, or an earlier mod's replacement). Returns 1, or 0 when the function
       is not there or cannot be replaced (inlined into its callers). Only in hl_mod_init or HL_EVENT_START. */
    int (*hook)(HlMod *mod, const char *name, void *replacement, void **original);

    /* -- The menus (HL_THREAD_UI) -- */
    void *(*ui_context)(void); /* the player menu's Rml::Context (RmlUi), for the mod's own documents */
    /* The mod's page is open (1) or closed (0). While it is open the player menu is hidden and doesn't open, the
       mod gets HL_EVENT_UI_INPUT, HL_EVENT_UI_BEFORE and HL_EVENT_UI_AFTER, and the game keeps running (to hold the
       world, the mod does so on the game's thread). Refused (0) while the player menu or another mod's page is open. */
    int (*ui_set_open)(HlMod *mod, int open);
    float (*ui_scale)(void); /* the menus' pixels per RmlUi unit, for pointer input */
    /* 1 when the input event (an SDL_Event, as HL_EVENT_UI_INPUT gives) presses one of the key's bindings: a page
       closing on its own key */
    int (*key_is)(HlMod *mod, const char *key, const void *sdl_event);
} HlApi;

/* The plugin's one export */
typedef int (*HlModInit)(const HlApi *api, HlMod *mod);
#define HL_MOD_INIT_NAME "hl_mod_init"

#ifdef __cplusplus
}
#endif

#endif
