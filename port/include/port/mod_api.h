/* Mods' plugins and the API they are given (modding/include/holyland_mod.h; port/platform/mod_api.c).

   At start, after the mods' files and text are applied (port_mods_open), port_mod_plugins_load loads the plugin of
   each mod that is on and names one for this system, and calls its hl_mod_init. Holyland then tells the plugins what
   happens through events: the game's side from game/mod_events.c (each frame, the end of each frame, the title, a
   new game) and game/saves.c and port/platform/quicksave.c (saves); the menus' side from port/ui/player_ui.cpp. */
#ifndef PORT_MOD_API_H
#define PORT_MOD_API_H

#include "holyland_mod.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the plugins of the mods that are on (after port_mods_open) */
void port_mod_plugins_load(void);
/* 1 when at least one plugin is loaded */
int port_mod_plugins_any(void);

/* An event to every plugin that asked for it; the result is the first handler's that returned non-zero
   (HL_EVENT_UI_INPUT: taken), 0 when none did. HL_EVENT_UI_INPUT, HL_EVENT_UI_BEFORE and HL_EVENT_UI_AFTER go to
   the mod whose page is open only. */
int port_mod_event(HlEvent event, void *data);

/* (game/saves.c, port/platform/quicksave.c) the game's save `slot` (1 to 3) written or loaded; a quick save's file
   written or loading */
void port_mod_save_file(int slot, int writing);
void port_mod_quick_saved(const char *path);
void port_mod_quick_loading(const char *path);
/* (game/quicksave.c) the title; (game/mod_events.c) in play: HL_EVENT_TITLE once, HL_EVENT_NEW_GAME when the game
   reached play from the title without a load */
void port_mod_title(void);
void port_mod_in_play(void);

/* The player menu's side (port/ui/player_ui.cpp): the mod whose page is open, NULL none */
HlMod *port_mod_page_open(void);
int port_mod_page_set(HlMod *mod, int open); /* refused (0) while the menu or another mod's page is open */
/* (port/ui/player_ui.cpp) the menu's own answers for the API */
void *port_player_ui_context(void);
int port_player_ui_menu_open(void);
float port_player_ui_scale(void);

/* Hooks (port/platform/mod_hooks.c): `name`'s function replaced, *original what to call for it as it was */
int port_mod_hook(const char *mod_id, const char *name, void *replacement, void **original);
/* A game function this mod hooked that other mods hooked too: they chain, and work together when each calls its
   original (port/platform/mod_hooks.c) */
typedef struct {
    char function[96];
    char others[256]; /* the other mods' ids, one a line */
} PortModHookShare;
/* The functions `mod_id` hooked that other mods hooked too, up to `max` of them: how many (any thread) */
int port_mod_hooks_shared(const char *mod_id, PortModHookShare *out, int max);
/* A function or variable of the program by name, NULL none */
void *port_mod_symbol(const char *name);

#ifdef __cplusplus
}
#endif

#endif
