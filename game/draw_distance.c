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
