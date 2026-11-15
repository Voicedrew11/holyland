/* draw_distance.c: PC-only. The game's side of --draw-distance (port_draw_distance(),
   port/include/port/host.h).

   What ends the view. func_00112BD0 picks the map records to draw each frame by a half-plane behind
   the player and the visibility table of the record the player stands on (Map_TestFlagBlockBit); the
   map header's draw distance (0x7C) would cut by distance as well, but it is 0 in all 64 maps. The
   objects, entities and items are drawn on the records drawn, with no distance cut of their own. The
   projection's far plane (GameLoop_SetCameraMatrices: 65535, Z 0 at about 70,000) lies beyond what the
   fog leaves. So what ends the view is the fog.

   The fog: a model's fog block (MapFogBlockValues, VU1 0x3C) gives the GS's fog value as
   near + far / w (w the distance along the view, clamped to 0..255, 255 no fog), so the same fog k
   times as far, f(w / k), is near + k * far / w: only the far term is scaled. A block of constant fog
   (far 0: a tint) stays as it is. The game copies each map's blocks into map_draw_state every frame
   (func_00112BD0), and the models point into that copy, so the copy is where they are scaled; the
   current block (map_draw_state.current_fog_block) fades towards the copy's over 30 frames, so a change
   of the switch in play eases in. With the fog further, the far plane moves out with it.

   The sky: the map records that follow the player (flags bit 31: a dome or backdrop at a fixed distance
   around the player) keep the PS2's fog unless port_draw_distance_sky(): pushing their fog out does not
   show anything further away, it only lightens the sky (map 000's dark sky shows its clouds).

   Behind the player: func_00112BD0 also leaves out the records wholly behind a line through the player across their
   heading. The frame never shows them, but the pictures between (--frame-rate over 30) are the frame turned on to the
   camera between and the mouse's turn (port/include/port/mouse_lead.h), in a fast spin 50 degrees and more past the
   frame's view: there the records left out were missing until the game's next frame (the world popping in at the
   screen's edge). With pictures between they are drawn too (DrawDistance_KeepBehind), but not marked in view: what
   in view decides (the map objects drawn and aimed at, their appear effects) is the PS2's, and so is the mirror mode
   (a mirror or rippled water behind is still left out). Behind the eye in the frame, VU1 clips all they send. */
#include "common.h"
#include "map/map.h"

#include <string.h>

/* the blocks of map_draw_state just copied from a map: their far terms times port_draw_distance() */
void DrawDistance_ScaleFog(void *blocks, int count) {
    MapFogBlockValues *block = blocks;
    f32 factor = port_draw_distance();
    int i;

    if (factor == 1.0f) {
        return;
    }
    for (i = 0; i < count; i++) {
        block[i].unk0C *= factor;
    }
}

/* the fog block a sky record (one that follows the player) is drawn with: the one it was given, or,
   with its fog kept, the map's own (the block before scaling, ps2) */
void *DrawDistance_SkyFog(void *given, void *ps2) {
    if (port_draw_distance() == 1.0f || port_draw_distance_sky()) {
        return given;
    }
    return ps2;
}

/* the projection's far plane: the PS2's, or that times port_draw_distance_far() */
f32 DrawDistance_FarZ(f32 farz) {
    f32 factor = port_draw_distance_far();

    return factor == 1.0f ? farz : farz * factor;
}

/* a map record wholly behind the player, its flags (MapDrawRecord.flags): drawn too (into the draw lists, not in
   view), for the pictures between; never a mirror (flags 2) or rippled water (0x200) */
int DrawDistance_KeepBehind(s32 flags) {
    return port_frame_interp() && !(flags & 0x202);
}

/* The map records drawn behind the player this frame (DrawDistance_KeepBehind's), so that their map objects are drawn
   too: MapObject_DrawAll draws an object only when it is in view, which a record behind is not. Kept by address in
   an open table, cleared each frame by its generation; when full, the rest's objects are not drawn (as on the PS2). */
#define BEHIND_ROOM 4096 /* (a power of two, past twice the records of two maps) */

static uintptr_t behind_records[BEHIND_ROOM];
static u32 behind_generation[BEHIND_ROOM];
static u32 behind_now = 1;

static u32 behind_slot(uintptr_t record) {
    return (u32)((record >> 4) * 2654435761u) & (BEHIND_ROOM - 1);
}

/* func_00112BD0, before it lists the records: none behind yet */
void DrawDistance_BeginBehind(void) {
    behind_now++;
    if (behind_now == 0) { /* (wrapped: every slot's generation is old again) */
        memset(behind_generation, 0, sizeof(behind_generation));
        behind_now = 1;
    }
}

/* func_00112BD0: a record behind the player listed to be drawn */
void DrawDistance_NoteBehind(const void *record) {
    uintptr_t key = (uintptr_t)record;
    u32 i = behind_slot(key);
    int n;

    for (n = 0; n < BEHIND_ROOM; n++, i = (i + 1) & (BEHIND_ROOM - 1)) {
        if (behind_generation[i] != behind_now) {
            behind_generation[i] = behind_now;
            behind_records[i] = key;
            return;
        }
        if (behind_records[i] == key) {
            return;
        }
    }
}

/* MapObject_DrawAll: an object not in view drawn all the same, for the pictures between: one in view with its map
   record (no linked point owner of its own, as MapObject_UpdateAll decides) whose record is drawn behind the player */
int DrawDistance_ObjectBehind(const MapObject *object) {
    uintptr_t key = (uintptr_t)object->map_record;
    u32 i;
    int n;

    if (key == 0 || (object->source->linked_point_owner != 0 && object->source->linked_point_owner != 0xFF)) {
        return 0;
    }
    i = behind_slot(key);
    for (n = 0; n < BEHIND_ROOM && behind_generation[i] == behind_now; n++, i = (i + 1) & (BEHIND_ROOM - 1)) {
        if (behind_records[i] == key) {
            return 1;
        }
    }
    return 0;
}
