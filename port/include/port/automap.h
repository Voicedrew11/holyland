/* The automap mod (docs/port/AUTOMAP.md): a floor plan of the map the player is in, made from the map's
   own collision meshes, that fills in as the player explores (the fog of war, kept per save), with markers
   for what is in it, drawn over the game by the overlay (Dear ImGui) as a full map on its key (M) and as a
   corner minimap. Off by default, like every mod; the Mods page's "Map" section, settings.json's "mods".

   Three parts:
   - game/automap.c, the game's side: reads the maps, objects, entities and items from the game's own
     structures once a game frame (Automap_Poll, from Quick_PollGame) and builds what is shown;
   - port/platform/automap.c: the settings, the command line, the files (the fog of war, per save), and
     whether the full map is open;
   - port/overlay/automap_view.cpp: the drawing, the hover labels and the Mods page's section.
   Everything runs on the game's thread (the overlay draws at the vblank), so nothing is locked. */
#ifndef PORT_AUTOMAP_H
#define PORT_AUTOMAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Settings (settings.json "mods", the Mods page; port/platform/automap.c) --------------------- */
typedef struct {
    int enabled;          /* the mod: off by default */
    int minimap;          /* the corner minimap while playing */
    int minimap_corner;   /* 0 top right, 1 bottom right, 2 top left, 3 bottom left */
    int minimap_size;     /* its side, percent of the picture's height (10 to 45) */
    int minimap_zoom;     /* world units from its centre to its edge, in hundreds (20 to 120) */
    int opacity;          /* the minimap's opacity, percent (20 to 100) */
    int full_opacity;     /* the full map's background opacity, percent (40 to 100) */
    /* the cheats, meant to stand in for an online guide: off by default */
    int reveal;           /* the whole map, explored or not */
    int items;            /* items on the ground, and what containers and chests hold, through the fog */
    int enemies;          /* the live enemies, named, through the fog */
    int secrets;          /* secrets and traps (fading walls, shooters, pitfalls), through the fog */
    int hints;            /* what each lock needs, and the hand-written hints for locks and puzzles */
} PortAutomapSettings;
extern PortAutomapSettings port_automap;

/* the command line: --automap (on for this run), --automap-cheats (every cheat, for tests) */
int port_automap_option(int argc, char **argv, int *i);
/* settings.json: `mods` is the "mods" object (cJSON *); read fills port_automap where the command line
   left it alone, write puts the keys in */
void port_automap_settings_read(const void *mods);
void port_automap_settings_write(void *mods);
/* a setting changed on the Mods page */
void port_automap_settings_changed(void);

/* ---- The full map ----------------------------------------------------------------------------- */
int port_automap_full_open(void);
void port_automap_set_full_open(int open); /* opens only in play, with the mod on */
/* the game is held while the full map is open (port/platform/kernel.c through port_overlay_paused) */
int port_automap_holds_game(void);

/* ---- What the game's side builds (game/automap.c) ---------------------------------------------- */
#define PORT_AUTOMAP_CELL 50.0f /* world units a cell of the floor plan */
#define PORT_AUTOMAP_LAYERS 4   /* floors one above the other in a cell, at most */
#define PORT_AUTOMAP_NO_Y 0x7FFF

/* what a layer is */
enum {
    PORT_AUTOMAP_NONE,
    PORT_AUTOMAP_FLOOR,
    PORT_AUTOMAP_WATER,  /* CollisionMesh.water_strips */
    PORT_AUTOMAP_POISON, /* hazard_strips, effects 1 to 3 (ground_effect_kind) */
    PORT_AUTOMAP_FIRE,   /* effect 4: 10 HP a frame (lava; the sea on some maps, DEEP) */
    PORT_AUTOMAP_HURT,   /* effect 5 and others: 1 HP a frame and wears what is worn (acid) */
    PORT_AUTOMAP_DEATH,  /* effect 0: falling onto it kills (the void under the map, deep water) */
    PORT_AUTOMAP_DEEP,   /* effect 4 where it is the sea (game/automap_guide.c): it drowns */
};
#define PORT_AUTOMAP_WALL 1   /* layer flag: a wall rises from this floor here */
#define PORT_AUTOMAP_WALKED 2 /* layer flag: the player has stood here */
#define PORT_AUTOMAP_FAR 4    /* layer flag: a DEATH layer far from any floor: not drawn */

typedef struct {
    int16_t y;    /* the floor's height (y grows downward); PORT_AUTOMAP_NO_Y: no layer */
    uint8_t kind; /* PORT_AUTOMAP_FLOOR ... */
    uint8_t flags;
} PortAutomapLayer;

typedef struct {
    int map;            /* the map's number (MapSlot.tag), -1: none */
    float x0, z0;       /* the corner of cell (0, 0): cell (i, j) covers x0 + i * CELL ... */
    int width, height;  /* cells */
    const PortAutomapLayer *layers; /* width * height * PORT_AUTOMAP_LAYERS, the layers of each cell from the highest */
    const int16_t *solid_top, *solid_bottom; /* cells with no floor: the wall there, top and bottom y (NO_Y: none) */
    const uint8_t *seen;                     /* width * height: a bit per layer seen */
    unsigned version;   /* changes whenever any of it does */
} PortAutomapPlan;

/* NULL until a map is built (the mod off, or not in play yet) */
const PortAutomapPlan *port_automap_plan(void);

/* markers */
enum {
    PORT_AUTOMAP_MARK_DOOR,
    PORT_AUTOMAP_MARK_LOCKED_DOOR,
    PORT_AUTOMAP_MARK_CHEST,
    PORT_AUTOMAP_MARK_CONTAINER, /* barrels, pots, boxes */
    PORT_AUTOMAP_MARK_REMAINS,   /* bones and gravestones that hold something */
    PORT_AUTOMAP_MARK_EXAMINE,   /* things only looked at: corpses, statues, flags */
    PORT_AUTOMAP_MARK_ITEM,      /* an item lying on the ground */
    PORT_AUTOMAP_MARK_SAVE,      /* a save point */
    PORT_AUTOMAP_MARK_EXIT,      /* the way into another map: a strip (w, h) */
    PORT_AUTOMAP_MARK_PERSON,
    PORT_AUTOMAP_MARK_ENEMY,
    PORT_AUTOMAP_MARK_TRAP,      /* shooters and pitfalls */
    PORT_AUTOMAP_MARK_SECRET,    /* walls that fade away */
    PORT_AUTOMAP_MARK_SWITCH,
    PORT_AUTOMAP_MARK_PEDESTAL,  /* pedestals and guide stone markers */
    PORT_AUTOMAP_MARK_PASSAGE,   /* symbols of passage (where the wands take you) */
    PORT_AUTOMAP_MARK_LADDER,
    PORT_AUTOMAP_MARK_LIFT,      /* sinking platforms */
    PORT_AUTOMAP_MARK_KINDS
};
#define PORT_AUTOMAP_MARK_DONE 1   /* opened, emptied, pressed */
#define PORT_AUTOMAP_MARK_HOLDS 2  /* holds an item or gold not taken yet (shown with the items cheat) */
#define PORT_AUTOMAP_MARK_CHEAT 4  /* shown through the fog only because of a cheat */
#define PORT_AUTOMAP_MARK_HINT 8   /* has a hand-written hint (the hints cheat) */
#define PORT_AUTOMAP_MARK_TRAPPED 16
#define PORT_AUTOMAP_MARK_ABSENT 32 /* a person the story has not brought here now (the secrets cheat), or on a
                                       browsed map an enemy's place: drawn faint */
#define PORT_AUTOMAP_MARK_GIVEN 64  /* an item out of sight that the story hands over (a script's pickup): drawn faint */
#define PORT_AUTOMAP_MARK_BROWSE 128 /* a way into a map the full map can show (port_automap_browse): clicked, it does */
#define PORT_AUTOMAP_MARK_MAYBE 256 /* on a browsed map: there only by a chance roll as the player walks in: drawn faint */

typedef struct {
    int kind;
    int flags;
    float x, y, z;        /* where (a door: its hinge side's middle; an exit: its strip's corner) */
    float angle;          /* a door: the way it spans, radians (0 along +x); others: unused */
    float length;         /* a door: its width; an exit: its strip's width along x */
    float depth;          /* an exit: its strip's depth along z */
    int map;              /* an exit: the map it leads to */
    float to[3];          /* an exit: its middle in that map's own frame (where the browser shows that map from) */
    char text[256];       /* the hover label: the first line is what it is, then a line each for more */
} PortAutomapMarker;

int port_automap_markers(const PortAutomapMarker **list);

/* the player, in world units; heading: the game's (PlayerState.rotation.y) */
typedef struct {
    float x, y, z, heading;
    int map;
    int in_play;   /* the game was polled a moment ago and the player is in control */
    int shown;     /* the minimap may be shown (in play or talking, no menu) */
    char title[96]; /* the map's name */
} PortAutomapPlayer;
void port_automap_player(PortAutomapPlayer *out);

/* ---- The map browser (game/automap.c): another map on the full map ---------------------------------
   Clicking a way into another map (a PORT_AUTOMAP_MARK_EXIT marker with PORT_AUTOMAP_MARK_BROWSE) shows that map,
   built from its file on the disc and the save's tables, read only; its ways are clickable in turn. The
   browsed maps are a history: back returns to the one before (the last, to the player's own), home to the
   player's own; closing the full map goes home. The minimap always shows the player's map. */
/* shows `map` from the point x, y, z in its own frame (PortAutomapMarker.to): 1 when it is shown (the player's
   own map goes home); 0 when it cannot be (not explored yet without the reveal cheat, or its file unread) */
int port_automap_browse(int map, float x, float y, float z);
void port_automap_browse_back(void);
void port_automap_browse_home(void);
/* the map browsed now, -1: the player's own is shown */
int port_automap_browsing(void);
const PortAutomapPlan *port_automap_browse_plan(void);
int port_automap_browse_markers(const PortAutomapMarker **list);
/* the browsed map as the full map shows it: x, y, z the point it is shown from (its slice's height), map, title */
void port_automap_browse_focus(PortAutomapPlayer *out);
/* (KF4_AUTOMAP_BROWSE's "self", for tests) the player's own map browsed as if it were loaded in no slot */
int port_automap_browse_self_test(void);

/* ---- The files: the fog of war, per save (port/platform/automap.c) ------------------------------
   The explored part of every map is kept in the data folder (automap/live.bin), and copied beside each
   save as it is made: one of the game's saves (automap/save<N>.bin, N its number) and a quick save
   (automap/quick_<file>.bin). Loading a save brings its copy back (or keeps the map as it is, for a
   save made without the mod); a new game starts with nothing explored. */
void port_automap_save_file(int number, int writing); /* game/saves.c: save `number` (1 to 3) written or loaded */
void port_automap_quick_saved(const char *path);       /* quicksave.c: a quick save was written */
void port_automap_quick_loading(const char *path);     /* quicksave.c: a quick save is being loaded */
/* the game's side keeps the explored state; these move it to and from bytes */
size_t Automap_Serialize(uint8_t **out);   /* malloc'd */
void Automap_Deserialize(const uint8_t *data, size_t size); /* NULL: forget everything */
void Automap_Flush(void);                  /* the current map's state into the stored ones */
/* (port side) write live.bin now, if anything changed */
void port_automap_save_live(int force);
void port_automap_mark_dirty(void);
/* live.bin into the game's side, once (game/automap.c, as the mod first runs) */
void port_automap_start(void);
/* the title screen was reached (Quick_PollTitle); the next game started without a load is a new one */
void port_automap_title(void);
/* (game/automap.c, the first frame in play after the title) 1: a new game, nothing explored yet */
int port_automap_new_game(void);

#ifdef __cplusplus
}
#endif

#endif
