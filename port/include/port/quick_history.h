/* The quick save history (port/platform/quick_history.c, its rule in quick_history_rules.c): every quick save to
   a slot (F2, the menu's Quick Save) is also kept as a file of its own in <data folder>/quicksaves/history/, so that
   a quick save taken at a bad moment (HP low, poisoned, falling) never leaves the player with no way back. The ten
   slots are not touched by it.

   The history keeps the newest saves and thins the older ones out, so that pressing F2 again and again replaces only
   the newest few and an older save is still there (port_quick_history_choose):
     - the newest PORT_QUICK_HISTORY_NEWEST, however close together;
     - the newest save of each PORT_QUICK_HISTORY_SPAN_S of play time, for the last PORT_QUICK_HISTORY_SPANS spans
       that have one;
     - the last save on a map before a save on another, for the last PORT_QUICK_HISTORY_MAP_LEAVES of those older
       than the newest.
   At most PORT_QUICK_HISTORY_MAX files (the three overlap, so fewer in practice).

   A file is named history-<n>.kf4quick, n counting up from 1 with each save (the newest has the highest n), a quick
   save file like a slot's (port/platform/quicksave.c), with a copy of the slot's exact save state beside it
   (history-<n>.kf4state, about 13 MB), so loading it gives back everything as it was, enemies where they stood,
   as a slot's load does. After an update, or for a save kept before the history kept states, the game's save image
   is loaded instead, with the live world of the maps put back. The state and the automap's and the hands' files
   kept with each one are removed with it.

   All on the game's thread (the menu's rows and the quick saves both run there). */
#ifndef PORT_QUICK_HISTORY_H
#define PORT_QUICK_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#include "port/quicksave.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_QUICK_HISTORY_NEWEST 3
#define PORT_QUICK_HISTORY_SPAN_S 600 /* ten minutes of play */
#define PORT_QUICK_HISTORY_SPANS 6
#define PORT_QUICK_HISTORY_MAP_LEAVES 4
#define PORT_QUICK_HISTORY_MAX (PORT_QUICK_HISTORY_NEWEST + PORT_QUICK_HISTORY_SPANS + PORT_QUICK_HISTORY_MAP_LEAVES)

/* ---- The rule (quick_history_rules.c: no files, for its test) -------------------------------------------------- */
typedef struct {
    uint32_t play_seconds; /* the game's play time when it was saved */
    int32_t map_tag;       /* the map it was saved on */
} PortQuickHistoryEntry;

/* Which of `count` entries, newest first, the history keeps: keep[i] is set to 1 or 0. Returns how many are kept
   (at most PORT_QUICK_HISTORY_MAX). */
int port_quick_history_choose(const PortQuickHistoryEntry *entries, int count, uint8_t *keep);

/* ---- The files (quick_history.c) --------------------------------------------------------------------------- */
/* (port/platform/quicksave.c, saving to a slot) the path the save's copy is written to: the next history file */
void port_quick_history_next_path(char *out, size_t size);
/* (port/platform/quicksave.c) the copy was written there: the history is thinned by the rule */
void port_quick_history_added(void);

/* How many saves it holds, and the index'th one's info, newest first (0: the newest). 0 when there is none. */
int port_quick_history_count(void);
int port_quick_history_info(int index, PortQuickInfo *info);
/* Asks for a load of the index'th (as port_quick_request_load does for a slot) */
void port_quick_history_request_load(int index);

#ifdef __cplusplus
}
#endif

#endif
