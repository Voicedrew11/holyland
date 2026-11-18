/* Quick saves (port/platform/quicksave.c, game/quicksave.c): the game's progress in the game's own save
   format, taken anywhere in play and loaded through the game's own load path, so any build of the
   program can read them (save states, port/include/port/savestate.h, load only in the build that
   made them).

   A quick save is the image Game_WriteSaveImage writes for a save point's save (it holds the map, the
   player's position, heading and look, HP and MP, the stats, the items, the flags of the world), with
   a small PC header. Loading puts that image where loading a save puts it and leaves the game's
   main loop as a card load does, so the game restarts the session from it (GameLoop_ResetSystems,
   GameLoop_Run on the saved map). The save point's saves (game/saves.c) are not touched either way. */
#ifndef PORT_QUICKSAVE_H
#define PORT_QUICKSAVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_QUICK_SLOTS 10

/* What a quick save says about itself (the file's PC block), for the overlay and the checks */
typedef struct {
    int64_t time;          /* when it was written, seconds since 1970 */
    int32_t map_tag;       /* the map the player was in (MapSlot.tag of the current slot) */
    float position[3];     /* the player's position in that map */
    float rotation[3];     /* x the look up and down, y the heading */
    int32_t hp, hp_max, mp, mp_max;
    int32_t level, gold;
    uint32_t play_seconds; /* the game's play time */
    /* The neighbouring map the game had loaded, or was loading, into its other map slot: which of the
       current map's 32 origins (MapSlot.origins) leads to it, its map number, and whether it was set
       up (in play) or only read. A file older than these has neighbour_known 0, and the game works
       the neighbour out from the position instead. */
    int32_t neighbour_known;
    int32_t neighbour_origin; /* 0..31, or -1: none */
    int32_t neighbour_map;    /* MapLoadRequest.map, or -1 */
    int32_t neighbour_set_up; /* 1: set up (MapLoad_SetUpMap on), 0: read, or being read, only */
    /* A file a tool made, not the game (tools/map_tour.py: any map, any place): the PC block's flags
       bit 1. Loading it puts the player on the floor under (or else over) the position, as the
       game would not, and works the neighbour out from there. Never set by a quick save. */
    int32_t made_up;
} PortQuickInfo;

/* Asks for a quick save to `slot`, or a load from it, taken where the game can (in play for a save;
   in play or on the title screen for a load). Safe from the window's events and the overlay. A
   request that the game has not taken within a second is dropped, with a message saying why. */
void port_quick_request_save(int slot);
void port_quick_request_load(int slot);
/* A quick save to a file (the flash recorder's folder: port/debug/debug.c); path is copied */
void port_quick_request_save_file(const char *path);
/* A teleport (port/include/port/teleport.h): the game's image as it is now, with the map and the
   player's place changed, loaded as a made-up file is. patient: the command line's, which waits. */
void port_quick_request_teleport(int map, const float position[3], float heading, const char *label, int patient);

/* 1 and what slot `slot` holds, or 0 when it is empty or not a quick save */
int port_quick_info(int slot, PortQuickInfo *info);

/* The last thing a request did, for the overlay and the log */
const char *port_quick_message(void);

/* The slot F2 and F4 use (the overlay's Quick saves choice) */
int port_quick_slot(void);

/* ---- The port's side ------------------------------------------------------------------------- */

/* Reads --quick-save-at and --quick-load ([VBLANK:]SLOT|FILE,...) (port/platform/main_host.c) */
void port_quick_init(void);
/* (the kernel, at each vblank) moves the command line's requests on, and drops stale ones */
void port_quick_vblank(void);

/* ---- The game's side (game/quicksave.c calls these) ------------------------------------------ */
#define PORT_QUICK_NONE 0
#define PORT_QUICK_SAVE 1
#define PORT_QUICK_LOAD 2
#define PORT_QUICK_TELEPORT 3 /* port/include/port/teleport.h */

/* What is asked for now (PORT_QUICK_) */
int port_quick_wanted(void);
/* The game cannot do it now: why. A key's request is dropped; the command line's waits. */
void port_quick_refuse(const char *why);
/* Writes the save image and the info as the requested slot or file: 1 when written */
int port_quick_write(const uint8_t *image, size_t size, const PortQuickInfo *info);
/* Reads and checks the requested slot or file: the image (malloc'd, the caller frees it) and its info,
   or NULL with the message set. */
uint8_t *port_quick_read(size_t *size, PortQuickInfo *info);
/* The game took the loaded image (ok) or refused it (the image's own check failed) */
void port_quick_loaded(int ok);
/* (the first vblank of play after a load) where the game put the player, checked against the info */
void port_quick_placed(const PortQuickInfo *now);
/* (PORT_QUICK_TELEPORT) the map, place and heading asked for */
void port_quick_teleport_target(int *map, float position[3], float *heading);
/* the game took the teleport: its image is in place for the restart, with this info (made_up) */
void port_quick_teleported(const PortQuickInfo *info);
/* (a made-up file) the game moved the player before the first frame: the check expects him there */
void port_quick_moved(const float position[3]);
/* (game/quicksave.c, called by port_quick_vblank) does that check when it is due */
void kf4_quick_vblank(void);

#ifdef __cplusplus
}
#endif

#endif
