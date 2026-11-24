/* Quick saves (port/platform/quicksave.c, game/quicksave.c): the game's progress in the game's own save
   format, taken anywhere in play and loaded through the game's own load path, so any build of the
   program can read them (save states, port/include/port/savestate.h, load only in the build that
   made them).

   A quick save is the image Game_WriteSaveImage writes for a save point's save (it holds the map, the
   player's position, heading and look, HP and MP, the stats, the items, the flags of the world), with
   a small PC header. Loading puts that image where loading a save puts it and leaves the game's
   main loop as a card load does, so the game restarts the session from it (GameLoop_ResetSystems,
   GameLoop_Run on the saved map). The save point's saves (game/saves.c) are not touched either way.

   A quick save to a slot also takes an exact save state beside it, and a load from the slot uses
   that when the build that made it loads it (port_quick_load_exact): everything comes back as it was,
   the enemies where they stood, an event half played. After an update the state no longer loads and
   the image is loaded instead, with the live world of the two map slots put back (PortQuickMap): the
   enemies killed stay dead and the items taken stay taken. */
#ifndef PORT_QUICKSAVE_H
#define PORT_QUICKSAVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_QUICK_SLOTS 10
#define PORT_QUICK_SPAWNS 128     /* entity_pool's per map slot */
#define PORT_QUICK_FIXED_ITEMS 64 /* fixed_items' per map slot */

/* One set-up map's live world as the save image does not hold it (game/quicksave.c). The game sets a
   map's enemies and fixed items up afresh each time it sets the map up: every spawn may come back as
   the player nears it (EntityAi_ShouldRespawn), and the fixed items are rolled again
   (MapItem_RollFixedItems). While a map stays set up, a spawn killed with ENTITY_FLAG_NO_RESPAWN stays
   dead and a herb picked stays picked. */
#define PORT_QUICK_SPAWN_DEAD 1u       /* not alive, or dying */
#define PORT_QUICK_SPAWN_NO_RESPAWN 2u /* ENTITY_FLAG_NO_RESPAWN */
#define PORT_QUICK_SPAWN_ROLLED 4u     /* ENTITY_FLAG_100: its respawn roll was taken while the player was near */
typedef struct {
    int32_t tag;          /* MapSlot.tag */
    int32_t spawn_count;  /* map_spawn_counts: a load checks it against the map's, as a check of the file */
    int32_t item_count;   /* map_fixed_item_counts: the same */
    uint64_t items_lying; /* bit i: fixed item i is lying in the map (on lying_items) */
    uint8_t spawns[PORT_QUICK_SPAWNS]; /* PORT_QUICK_SPAWN_ bits per spawn record */
} PortQuickMap;

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
    /* The two map slots' live world, which the game makes again each time it sets a map up, and so
       which loading the save image alone would make again: map_count 0 for a file older than it. */
    int32_t map_count;
    PortQuickMap maps[2];
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
/* (PORT_QUICK_LOAD, before the game's own load) A quick save slot keeps an exact save state beside
   its image (quick<N>.kf4state, port/include/port/savestate.h), taken a vblank after it. When this
   build made that state, after the image, the load is handed to the save states and this returns 1:
   the whole game comes back as it was. Otherwise (another build: after an update) 0, and the game
   loads the image. */
int port_quick_load_exact(void);
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
