/* quicksave.c: PC-only. The game's side of quick saves (port/include/port/quicksave.h).

   Save: Game_WriteSaveImage (core/game.c) writes the game into save_image as the save screen does
   before a memory card save (ui/trade.c, after PlayTime_Update); the image goes to a file instead of
   the card. save_image is put back as it was afterwards.

   Load: the image goes into save_image as a memory card load leaves it (save/save_card.c,
   SaveCard_StepLoad: save_loaded = 1, game_status.bit9 = 1). In play that ends the main loop
   (GameLoop_Run), and main (main.c) then runs GameLoop_ResetSystems, which reads the image back
   (Game_ReadSaveImage), and GameLoop_Run on the saved map (saved_map_tag), which leaves the player
   where the image says: the image holds the position and the rotation (PlayerState 0x00 to 0x20). On
   the title screen, Title_Run returns as the title's own Load does when a save was loaded. Once the
   saved map is set up, GameLoop_Run calls Quick_RestoreNeighbour, which asks the game's background
   load for the neighbouring map that was loaded when the save was taken (the file keeps which).

   The patched game calls Quick_PollGame once a frame at the end of the main loop, and Quick_PollTitle
   once a frame on the title screens; both return at once when nothing is asked for. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "core/game.h"
#include "map/map.h"
#include "map_object/map_object.h"
#include "player/player.h"
#include "gfx/screen.h"
#include "ui/message.h"
#include "collision/collision.h"
#include "script/script.h"
#include "port/platform.h"
#include "port/automap.h"
#include "port/weapon_sets.h"
#include "port/quicksave.h"
#include "port/teleport.h"

#define SAVE_IMAGE_SIZE 0x20000
#define SAVE_IMAGE_MAGIC 0x15400
#define MAP_ORIGIN_COUNT 32 /* MapSlot.origins: 0x200 bytes of the map file (Map_ParseFile) */

extern u8 save_image[SAVE_IMAGE_SIZE];
extern int save_loaded;
extern GameStatus game_status;
extern PlayerState player_state;
extern MapSlot map_slots[2];
extern int current_map_slot;
extern ScreenFade screen_fade;
extern int map_load_committed;
extern sptr D_00447000[4];
extern int fade_frames_left;
extern ClockTime current_play_time;
extern MapLoadState map_load_state;

extern void (*map_load_steps[])(void);

void Entity_UpdatePool(void);
void GameLoop_RunSoundLoads(void);
void Sound_Update(void);
int Collision_FindFloorBelow(f32 reach, void *point_arg);

void Game_WriteSaveImage(void);
int Game_CheckSaveImage(void);
void PlayTime_Update(void);

/* a load was handed to the game: the first vblank of play checks where the game put the player */
static int placed_check;
/* and the neighbouring map is to be loaded again (Quick_RestoreNeighbour) */
static int neighbour_due;
static PortQuickInfo loaded_info;
/* a dead end's way in (port_map_way_in): the request the game's load steps read while it loads */
static MapLoadRequest way_in;

/* The neighbouring map. The game keeps the map the player is in in one slot (current_map_slot) and
   the map next to it in the other, and MapLoad_Update (map/map_load.c) streams it in two stages:
   - it asks for a neighbour only when the record the player stands on (PlayerState.map_index) names
     one of the current map's origins (MapRecordInfo.neighbour_origin_index: a map and where it
     joins). The load frees the other slot and reads the file in the background (steps 0 to 4), then
     rests at step 5;
   - it sets the map up (steps 6 to 8: MapLoad_SetUpMap, MapLoad_ActivateMap; only then is it drawn
     and walked on) when the player stands on a record that needs it (needs_neighbour_map), at the
     joins, finishing the read there and then if it is not done.
   The records between name no map and keep whatever was loaded last, so what is loaded depends on
   the way the player came, which the save image does not hold: a memory card save is taken at a save
   point, where it does not matter. A quick save keeps it in its PC block, and a quick load puts it
   back through the same steps.

   Which origin led to what the other slot holds or is loading (-1: none), and whether it is set up.
   A load that is running or resting has its request (or the one queued after it); a finished one kept
   the origin's position in map_load_state.position (MapLoad_ActivateMap), negated once the maps
   swapped (Map_MakeCurrent): either way that is the current map's origin to it, as the two maps'
   origins meet. */
static int neighbour_origin(int *map, int *set_up) {
    MapSlot *current = &map_slots[current_map_slot & 1];
    MapSlot *other = &map_slots[!(current_map_slot & 1)];
    MapLoadRequest *request = map_load_state.next_request != NULL ? map_load_state.next_request : map_load_state.request;
    MapLoadRequest *origins = (MapLoadRequest *)current->origins;
    int i, by_map = -1;

    *map = -1;
    *set_up = 0;
    if (origins == NULL) {
        return -1;
    }
    if (request != NULL) {
        i = (int)(request - origins);
        if (i < 0 || i >= MAP_ORIGIN_COUNT) {
            return -1;
        }
        *map = request->map;
        *set_up = map_load_state.next_request == NULL && map_load_state.step >= 6; /* being set up */
        return i;
    }
    if (!other->loaded) {
        return -1;
    }
    for (i = 0; i < MAP_ORIGIN_COUNT; i++) {
        if ((u8)origins[i].map == other->tag) {
            if (origins[i].x == map_load_state.position.x && origins[i].y == map_load_state.position.y &&
                origins[i].z == map_load_state.position.z) {
                by_map = i;
                break;
            }
            if (by_map < 0) {
                by_map = i;
            }
        }
    }
    if (by_map >= 0) {
        *map = origins[by_map].map;
        *set_up = 1;
    }
    return by_map;
}

/* Whether the point is in a record's box, grown by margin on every side */
static int in_box(MapRecordInfo *record, f32 *point, f32 margin) {
    MapRecordBounds *box = (MapRecordBounds *)record;
    int axis;

    for (axis = 0; axis < 3; axis++) {
        if (point[axis] < box->lowest[axis] - margin || point[axis] > box->highest[axis] + margin) {
            return 0;
        }
    }
    return 1;
}

/* A file from before the neighbour was kept: the origin named by the record the player stands in,
   read only, as the game has it when he walks onto that record (the join's records further on set it
   up); or else that of the nearest record that names one (box to point), the way he came, set up: past
   those records the game has it set up, except between them and the join, where setting it up early
   only puts it in place sooner. Records overlap (the joins' records are thin floors laid over the
   room's), so all are looked at. */
static int neighbour_from_position(int *set_up) {
    MapSlot *slot = &map_slots[current_map_slot & 1];
    f32 *point = &player_state.position.x;
    f32 best = 0.0f;
    int i, axis, found = -1, standing = -1;

    for (i = 0; i < slot->header->record_count; i++) {
        MapRecordInfo *record = (MapRecordInfo *)slot->records + i;
        MapRecordBounds *box = (MapRecordBounds *)record;
        f32 distance = 0.0f;

        if (record->neighbour_origin_index == 0xFF) {
            continue;
        }
        if (standing < 0 && in_box(record, point, 100.0f)) {
            standing = record->neighbour_origin_index;
        }
        for (axis = 0; axis < 3; axis++) {
            f32 out = point[axis] < box->lowest[axis] ? box->lowest[axis] - point[axis]
                    : point[axis] > box->highest[axis] ? point[axis] - box->highest[axis] : 0.0f;
            distance += out * out;
        }
        if (found < 0 || distance < best) {
            best = distance;
            found = record->neighbour_origin_index;
        }
    }
    *set_up = standing < 0;
    return standing >= 0 ? standing : found;
}

/* A made-up file (tools/map_tour.py) names a place, not where a player stood: puts the player on the
   floor of the loaded map's records nearest under it (Collision_FindFloorBelow, as a dropped item is
   put down), or else the nearest over it, so that he neither falls nor stands in the ground. */
static void put_on_floor(void) {
    f32 point[4] = {player_state.position.x, player_state.position.y, player_state.position.z, 0.0f};
    f32 asked = point[1];
    int step, found = Collision_FindFloorBelow(4000.0f, point);

    for (step = 1; !found && step <= 8; step++) {
        point[1] = asked - 500.0f * step; /* y grows downward: look 500 higher each time */
        found = Collision_FindFloorBelow(500.0f, point);
    }
    if (!found) {
        port_event("quick load: made up: no floor within 4000 under or over %.1f,%.1f,%.1f", point[0], asked, point[2]);
        return;
    }
    player_state.position.y = COLLISION_RESULT->point[1];
    port_quick_moved(&player_state.position.x);
    port_event("quick load: made up: on the floor at %.1f,%.1f,%.1f (asked %.1f)", player_state.position.x,
               player_state.position.y, player_state.position.z, asked);
}

/* Runs the game's load steps for map_load_state.request to where they rest (step 5, read), or on through
   the set-up, as MapLoad_Update does when the player stands where the neighbour is needed (without
   drawing: the screen is black, about to fade in) */
static void run_neighbour_load(int set_up) {
    int steps = 0;

    while (map_load_state.step != 5 && steps++ < 100000) {
        map_load_steps[map_load_state.step]();
        Entity_UpdatePool();
        GameLoop_RunSoundLoads();
        Sound_Update();
    }
    if (map_load_state.step == 5 && set_up) {
        map_load_state.step = 6;
        while (map_load_state.step != 0) {
            map_load_steps[map_load_state.step]();
        }
    }
}

/* (GameLoop_Run, once a loaded save's map is set up, before the first frame) a quick load asks for
   the neighbouring map the game had, as MapLoad_Update asks for one, and runs the game's load steps to
   where they were: read (resting at step 5), or on through the set-up, as MapLoad_Update does when the
   player stands where the neighbour is needed (without drawing: the screen is black, about to fade
   in). A memory card load does nothing here. A made-up file's player is put on the floor first. */
void Quick_RestoreNeighbour(void) {
    MapSlot *current = &map_slots[current_map_slot & 1];
    MapLoadRequest *origins = (MapLoadRequest *)current->origins;
    int origin, map, set_up;

    if (!neighbour_due) {
        return;
    }
    neighbour_due = 0;
    if (loaded_info.made_up) {
        put_on_floor();
    }
    if (loaded_info.neighbour_known) {
        origin = loaded_info.neighbour_origin;
        map = loaded_info.neighbour_map;
        set_up = loaded_info.neighbour_set_up;
    } else {
        origin = neighbour_from_position(&set_up);
        map = origin >= 0 && origins != NULL ? origins[origin].map : -1;
    }
    if (origin < 0 && map_load_state.step == 0) {
        /* a dead end (port_map_way_in): the map the game came from stays loaded, set up */
        int from;
        float position[3];

        if (port_map_way_in(current->tag, &from, position)) {
            way_in.x = position[0];
            way_in.y = position[1];
            way_in.z = position[2];
            way_in.map = (u16)from;
            map_load_state.request = &way_in;
            run_neighbour_load(1);
            port_event("quick load: map %02X is a dead end: the map it is entered from, %02X, %s", current->tag, from,
                       map_load_state.step == 0 ? "set up" : "not read (the drive stayed busy)");
            return;
        }
    }
    if (origin < 0 || origin >= MAP_ORIGIN_COUNT || origins == NULL || origins[origin].map != map ||
        (u8)map == current->tag || map_load_state.step != 0) {
        port_event("quick load: no neighbouring map%s", origin >= 0 ? " (the saved one is not this map's)" : "");
        return;
    }
    map_load_state.request = &origins[origin];
    run_neighbour_load(set_up);
    port_event("quick load: neighbouring map %02X (origin %d%s) %s", map, origin,
               loaded_info.neighbour_known ? "" : ", from the position",
               map_load_state.step == 0 ? "set up" : map_load_state.step == 5 ? "read" : "not read (the drive stayed busy)");
}


static void fill_info(PortQuickInfo *info) {
    memset(info, 0, sizeof(*info));
    info->map_tag = map_slots[current_map_slot & 1].tag;
    info->position[0] = player_state.position.x;
    info->position[1] = player_state.position.y;
    info->position[2] = player_state.position.z;
    info->rotation[0] = player_state.rotation.x;
    info->rotation[1] = player_state.rotation.y;
    info->rotation[2] = player_state.rotation.z;
    info->hp = player_state.hp_now;
    info->hp_max = player_state.hp_max;
    info->mp = player_state.mp_now;
    info->mp_max = player_state.mp_max;
    info->level = player_state.level;
    info->gold = player_state.gold;
    /* as PlayTime_AsSeconds counts it: mday is the hundreds of hours, in 99s */
    info->play_seconds = current_play_time.sec + (current_play_time.min + (current_play_time.hour + current_play_time.mday * 99u) * 60u) * 60u;
    info->neighbour_known = 1;
    info->neighbour_origin = neighbour_origin(&info->neighbour_map, &info->neighbour_set_up);
}

/* Why the player is not in control now (NULL: he is); the cheats (game/cheats.c) ask too */
const char *Quick_WhyNotInPlay(void) {
    if (game_status.bit9) {
        return "the game is leaving play";
    }
    if (player_state.death_timer != 0) {
        return "the player is dying";
    }
    if (!map_slots[current_map_slot & 1].loaded) {
        return "no map is loaded";
    }
    if (game_status.player_control == 0 || (game_status.update_hold_flags & 1)) {
        return "an event has the player's control";
    }
    if (game_status.paused || (game_status.bit12 && game_status.bit13)) {
        return "the game is paused";
    }
    if (Message_IsBoxOpen()) {
        return "a message is open";
    }
    if (screen_fade.step != 0 || map_load_committed != 0) {
        return "the map is changing";
    }
    if (D_00447000[3] != 0 && fade_frames_left > 0) {
        return "the screen is fading";
    }
    if (game_status.bit2 || game_status.bit3 || game_status.bit4 || game_status.save_requested || game_status.bit8) {
        return "a menu is opening";
    }
    return NULL;
}

/* Why the world cannot be saved now (NULL: it can): as Quick_WhyNotInPlay, and not while a script the game set
   off in play is still running (Script_EventRunning, script/script.c). The image holds what such a script has
   done so far but not the script, so it would load with the rest of it never done: a door it opened that it was
   to close (the Dark Knight's on map 036, which then never shows his text nor sets him on), a lever half thrown.
   A memory card save is taken at a save point, where no such script runs. */
static const char *why_not_now(void) {
    static char text[96];
    const char *why = Quick_WhyNotInPlay();
    int script;

    if (why != NULL) {
        return why;
    }
    script = Script_EventRunning();
    if (script != 0) {
        snprintf(text, sizeof(text), "an event is still under way (map script %d: a door, a lever, a trap)", script);
        return text;
    }
    return NULL;
}

static void quick_save(void) {
    PortQuickInfo info;
    u8 *kept = malloc(SAVE_IMAGE_SIZE);
    u32 size;
    const char *why = why_not_now();

    if (why != NULL || kept == NULL) {
        free(kept);
        port_quick_refuse(why != NULL ? why : "out of memory");
        return;
    }
    memcpy(kept, save_image, SAVE_IMAGE_SIZE);
    fill_info(&info);
    PlayTime_Update();
    Game_WriteSaveImage();
    fill_info(&info); /* the play time just read */
    size = *(u32 *)(save_image + 4);
    /* the blocks, then their byte sums (one per 128 bytes) */
    port_quick_write(save_image, size + size / 128, &info);
    memcpy(save_image, kept, SAVE_IMAGE_SIZE);
    free(kept);
}

/* Puts a quick save's image where a memory card load puts it: 1 when the game's check passes */
static int quick_load_image(void) {
    PortQuickInfo info;
    size_t size;
    u8 *image = port_quick_read(&size, &info);
    u8 *kept;

    if (image == NULL) {
        return 0;
    }
    kept = malloc(SAVE_IMAGE_SIZE);
    if (kept == NULL || size > SAVE_IMAGE_SIZE) {
        free(image);
        free(kept);
        port_quick_loaded(0);
        return 0;
    }
    memcpy(kept, save_image, SAVE_IMAGE_SIZE);
    memset(save_image, 0, SAVE_IMAGE_SIZE); /* as ui/menu_screens.c clears it before a card load */
    memcpy(save_image, image, size);
    free(image);
    if (*(u32 *)save_image != SAVE_IMAGE_MAGIC || !Game_CheckSaveImage()) {
        memcpy(save_image, kept, SAVE_IMAGE_SIZE);
        free(kept);
        port_quick_loaded(0);
        return 0;
    }
    free(kept);
    save_loaded = 1;
    placed_check = 1;
    neighbour_due = 1;
    loaded_info = info;
    port_quick_loaded(1);
    return 1;
}

/* (port/platform/quicksave.c, each vblank) GameLoop_Run clears save_loaded once it has loaded the map,
   and the first vblank after is the top of the main loop's first frame: the player stands where the
   game put him, before the frame moves him */
void kf4_quick_vblank(void) {
    if (placed_check && !save_loaded) {
        PortQuickInfo now;

        placed_check = 0;
        fill_info(&now);
        port_quick_placed(&now);
    }
}

/* Restarts the session from the image in save_image (save_loaded set): leaves the main loop as a card
   load does, and main then restarts the session from the image. A dying player's game over and the
   ending are not shown */
static void leave_for_image(void) {
    player_state.death_timer = 0;
    game_status.bits10 = 0;
    game_status.bit9 = 1;
}

/* A teleport (port/include/port/teleport.h): the game written into save_image as a quick save writes it,
   but with the map and the player's place asked for, then taken as a made-up quick save is loaded:
   the game restarts the session on that map, and the player is put on the floor under the place.

   The image names the map by the current slot's tag, so the slot is given the new map's number while
   it is written. Its objects' flags (doors, chests, switches, levers) must not be: Game_WriteSaveImage
   first files every loaded map's live object flags under its slot's tag (MapObject_SaveAllFlags),
   which would put the map being left's under the new map's number, so that the new map started with
   the old one's states and the old one lost its own. So, as leaving a map does (MapObject_UnloadMap),
   the loaded maps' flags are first filed under their own numbers, by the game's MapObject_SaveAllFlags,
   and the image is then written with the slots marked not loaded, so that the write's own
   MapObject_SaveAllFlags files nothing again. The new map then starts from its own saved flags
   (MapObject_StartMap), as after a map change. */
static void teleport(void) {
    PortQuickInfo info;
    MapSlot *slot = &map_slots[current_map_slot];
    u8 kept_loaded[2] = {map_slots[0].loaded, map_slots[1].loaded};
    f32 kept_position[3] = {player_state.position.x, player_state.position.y, player_state.position.z};
    f32 kept_rotation[3] = {player_state.rotation.x, player_state.rotation.y, player_state.rotation.z};
    u8 kept_tag = slot->tag;
    const char *why = Quick_WhyNotInPlay();
    float position[3], heading;
    int map;

    if (why != NULL) {
        port_quick_refuse(why);
        return;
    }
    port_quick_teleport_target(&map, position, &heading);
    player_state.position.x = position[0];
    player_state.position.y = position[1];
    player_state.position.z = position[2];
    player_state.rotation.x = 0.0f;
    player_state.rotation.y = heading;
    player_state.rotation.z = 0.0f;
    MapObject_SaveAllFlags();
    map_slots[0].loaded = 0;
    map_slots[1].loaded = 0;
    slot->tag = (u8)map;
    PlayTime_Update();
    Game_WriteSaveImage();
    fill_info(&info);
    slot->tag = kept_tag;
    map_slots[0].loaded = kept_loaded[0];
    map_slots[1].loaded = kept_loaded[1];
    player_state.position.x = kept_position[0];
    player_state.position.y = kept_position[1];
    player_state.position.z = kept_position[2];
    player_state.rotation.x = kept_rotation[0];
    player_state.rotation.y = kept_rotation[1];
    player_state.rotation.z = kept_rotation[2];
    if (!Game_CheckSaveImage()) {
        port_quick_refuse("the game's check of the image failed");
        return;
    }
    info.made_up = 1;
    info.neighbour_known = 0;
    info.neighbour_origin = -1;
    info.neighbour_map = -1;
    save_loaded = 1;
    placed_check = 1;
    neighbour_due = 1;
    loaded_info = info;
    port_quick_teleported(&info);
    leave_for_image();
}

void Cheat_Poll(void);
void Automap_Poll(void);
void WeaponSets_Poll(void);

void Quick_PollGame(void) {
    int wanted = port_quick_wanted();

    Cheat_Poll(); /* the debug cheats (game/cheats.c) */
    Automap_Poll(); /* the automap mod (game/automap.c) */
    WeaponSets_Poll(); /* the weapon sets mod (game/weapon_sets.c) */

    if (wanted == PORT_QUICK_SAVE) {
        quick_save();
    } else if (wanted == PORT_QUICK_TELEPORT) {
        if (game_status.bit9) {
            port_quick_refuse("the game is leaving play");
            return;
        }
        teleport();
    } else if (wanted == PORT_QUICK_LOAD) {
        if (game_status.bit9) {
            port_quick_refuse("the game is leaving play");
            return;
        }
        if (quick_load_image()) {
            leave_for_image();
        }
    }
}

int Quick_PollTitle(void) {
    int wanted = port_quick_wanted();

    port_automap_title(); /* the automap mod: the next game may be a new one (game/automap.c) */
    port_weapon_sets_title(); /* the weapon sets mod: the same (game/weapon_sets.c) */
    if (wanted == PORT_QUICK_SAVE || wanted == PORT_QUICK_TELEPORT) {
        port_quick_refuse("no game is being played");
    } else if (wanted == PORT_QUICK_LOAD) {
        return quick_load_image();
    }
    return 0;
}
