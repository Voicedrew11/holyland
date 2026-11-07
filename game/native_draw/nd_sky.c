/* nd_sky.c: PC-only. The sky as a kind of native draw of its own (--native-draw sky, PORT_ND_SKY; P3-1 and
   P3-2 of docs/port/NATIVE_DRAW_TASKS.md, NATIVE_DRAW.md 1.2.3 and A1).

   The sky is not a pass of its own: it is the map record with flag bit 31 (MapDrawRecord.flags < 0), on
   maps 000, 002, 004, 013, 021, 027, 098 and 099, one per map. func_00112BD0 puts its model at the
   player's feet each frame, and Map_DrawRecords (or Map_DrawLateRecords) draws it in file order with the
   other records: Z GEQUAL, Z written, fog on (its PS2 fog kept by DrawDistance_SkyFog). Natively it is
   drawn as a map record (NativeDraw_Posed, identity RD_KIND_MAP_RECORD and the record's address, which is
   what pictures between pair it by); the switch is its own, so the sky can go back to the PS2 path while
   the rest of the map stays native, or the other way round.

   --sky (port_native_sky()) says how:
   - ps2: the dome at the feet with its real Z (A1's "PS2" column), exactly the PS2 path's picture. Its Z
     is its distance, 4,446 units straight up to 24,252 at the rim, so map geometry that lies beyond it
     from the eye is hidden by it (P0-3: on 002, 004, 013 and 027).
   - modern (the default, P3-2): where the sky is the frame's first draw (its first map record: 002, 013,
     021, 027, 098 and 099, whose skies are first in their lists), it is drawn as a modern sky, centred on
     the eye and at the far plane. A copy of its model is put at the eye (the camera the world is drawn
     from: world_view_matrix, so the head's bob and the shakes are in it), rotation and scale the file's;
     its Z test is ALWAYS and its draws carry RD_DRAW_FAR_Z (every corner's Z 0, the Z buffer's clear
     value): it lays down the far plane whatever the Z buffer held, and everything after it passes over it,
     so nothing is hidden by the dome any more. Its place on the screen, fog, colour and clouds are worked
     out as before; from 220 units higher its horizon and its rim (where it fades into the fog's colour)
     are about half a degree higher, a level or a few at the rim.
     Where the game draws other records before the sky (000, whose sky is 32nd of 35; 004, 3rd of 6) it stays
     the PS2's, at the feet with its real Z: at the far plane its clouds would turn into hard black shapes over
     the canyon's silhouettes (P3-2's survey, NATIVE_DRAW_TASKS.md).
     What the far plane shows that the PS2's dome hid (the same survey): the tops of far hills and a tree's
     trunk on 002, 013 and 027, a little taller than the dome cut them.
   The PS2 path's sky (sky not native) is always the PS2's. */
#include "common.h"
#include "model/model.h"
#include "map/map.h"
#include "gfx/gs.h"
#include "port/render.h"
#include "port/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int NativeDraw_Posed(ModelInstance *model, int kind, const void *owner); /* (native_draw.c) */
int NativeDraw_PosedWithFlags(ModelInstance *model, int kind, const void *owner, u32 flags);
void Packet_BeginGsRegisters(void);
void Packet_AddGsRegister(int reg, s64 value);
void Packet_EndGsRegisters(void);

extern MapSlot map_slots[];
extern f32 world_view_matrix[16]; /* the world to the view (GameLoop_SetCameraMatrices): VU layout, a column a qword */

/* the map records drawn since the frame began (NativeDraw_SkyFrame), whichever way */
static int records_drawn;

/* At the start of GameLoop_DrawFrame (NativeDraw_BeginFrame) */
void NativeDraw_SkyFrame(void) {
    records_drawn = 0;
}

/* the --native-draw kind a map record belongs to: the sky (flag bit 31) or the map */
static int record_kind(const MapDrawRecord *record) {
    return record->flags < 0 ? PORT_ND_SKY : PORT_ND_MAP;
}

/* The eye in the world: the camera the world is drawn from. world_view_matrix takes a point p to R p + t
   (the columns of R its first three qwords, t its fourth), so the eye, at the view's origin, is
   R^-1 (0 - t). 0 if R cannot be inverted (never in play). */
static int eye_position(f32 eye[3]) {
    const f32 *m = world_view_matrix;
    /* R's rows r and columns c: m[c * 4 + r] */
    double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    double t0 = -m[12], t1 = -m[13], t2 = -m[14];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;

    if (!(det > 1e-12 || det < -1e-12)) {
        return 0;
    }
    eye[0] = (f32)((A * t0 + (c * h - b * i) * t1 + (b * f - c * e) * t2) / det);
    eye[1] = (f32)((B * t0 + (a * i - c * g) * t1 + (c * d - a * f) * t2) / det);
    eye[2] = (f32)((C * t0 + (b * g - a * h) * t1 + (a * e - b * d) * t2) / det);
    return 1;
}

/* Whether the sky record is its map's first record (file order: a draw list is the file's records in order,
   those not seen left out), so that it is drawn first in every frame its slot is the first drawn */
static int first_in_file(const MapDrawRecord *record) {
    static const MapDrawRecord *logged;
    int s;

    for (s = 0; s < 2; s++) {
        if (map_slots[s].loaded && map_slots[s].records != NULL) {
            long long index = ((const char *)record - map_slots[s].records) / 0x80;

            if (index >= 0 && index < 0x1000) {
                if (logged != record && getenv("KF4_ND_LOG")) {
                    logged = record;
                    fprintf(stderr, "[native draw] the sky: record %lld of slot %d\n", index, s);
                }
                return index == 0;
            }
        }
    }
    return 0;
}

static void queue_test(s64 test) {
    Packet_BeginGsRegisters();
    Packet_AddGsRegister(GS_REG_TEST_1, test);
    Packet_EndGsRegisters();
}

/* The sky record, natively; first: no map record drawn before it in this frame. --sky ps2, or modern where
   it is not first: as every map record (its model is already at the feet, its fog block already the
   PS2's). --sky modern, first: a copy of its model put at the eye, drawn at the far plane with Map_DrawRecords'
   TEST (0x5000D) made Z ALWAYS around it (see the top). The game's model is not touched (the CPU's near cut
   and the next frame read it). */
static int sky_record(ModelInstance *model, const MapDrawRecord *record, int first) {
    ModelInstance at_eye;
    f32 eye[3];
    int drawn;

    if (port_native_sky() != PORT_SKY_MODERN || !eye_position(eye)) {
        return NativeDraw_Posed(model, RD_KIND_MAP_RECORD, record);
    }
    if (!first_in_file(record) || !first) {
        return NativeDraw_Posed(model, RD_KIND_MAP_RECORD, record); /* (where other records come first, 000 and 004: the PS2's) */
    }
    memcpy(&at_eye, model, sizeof(at_eye));
    ((f32 *)at_eye.model_matrix)[12] = eye[0];
    ((f32 *)at_eye.model_matrix)[13] = eye[1];
    ((f32 *)at_eye.model_matrix)[14] = eye[2];
    queue_test(0x3000D);
    drawn = NativeDraw_PosedWithFlags(&at_eye, RD_KIND_MAP_RECORD, record, RD_DRAW_FAR_Z);
    queue_test(0x5000D);
    return drawn;
}

/* Map_DrawRecords' and Map_DrawLateRecords' call site (behind port_native_draw(PORT_ND_MAP | PORT_ND_SKY)):
   the record's draw_parts, natively if its kind (record_kind) is on. 1 if drawn; 0 if its kind is off or
   it cannot be drawn natively (NativeDraw_Posed), and nothing was queued: the caller draws it the PS2 way. */
int NativeDraw_MapRecord(ModelInstance *model, const MapDrawRecord *record) {
    int kind = record_kind(record);
    int first = records_drawn == 0;

    records_drawn++;
    if (!port_native_draw(kind)) {
        return 0;
    }
    if (kind == PORT_ND_SKY) {
        return sky_record(model, record, first);
    }
    return NativeDraw_Posed(model, RD_KIND_MAP_RECORD, record);
}
