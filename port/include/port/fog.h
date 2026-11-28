/* The fog: how the 3D world is fogged, by number and by name (settings.json's display "fog", --fog, the player menu's
   Fog row, KF4_SET_AT's fog), and the maths of the volumetric fog the scene renderer draws (port/gs/pc/shaders/
   pcr_prim.h's SCN_FOG_VOLUME, docs/port/PC_RENDERER.md "Volumetric fog"). A mode is one entry here.

   The PS2's fog: each vertex gets the GS's fog value F = near + far / w (w its distance along the view; the terms are
   its model's fog block, VU1 0x3C's z and w: game/draw_distance.c), clamped to 0..255, 255 no fog, 0 all fog colour,
   and the triangle's colour is blended toward the fog colour by it.

   The volumetric fog keeps each draw's own F at each pixel as its base (each map record's block, its colour, where it
   starts and ends) and varies it by the density along the view's ray: what F leaves of a colour, F / 255, is the light
   that comes through a fog of optical depth -ln(F / 255); a fog m times as dense has m times that depth and leaves
   (F / 255)^m. m is the mean of a density factor (thicker low down, a drifting noise) along the ray; where it is 1 the
   fog is the PS2's, and where the PS2's fog is whole (F 0) it stays whole. */
#ifndef PORT_FOG_H
#define PORT_FOG_H

#include <math.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PORT_FOG_PS2,        /* each vertex's fog, the PS2's: the default */
    PORT_FOG_VOLUMETRIC, /* worked out at each pixel through a fog that varies in space and moves (the scene renderer) */
    PORT_FOG_COUNT
};

typedef struct {
    const char *name;  /* settings.json's and the command line's */
    const char *label; /* the player menu's */
} PortFog;

static inline const PortFog *port_fog_mode(int mode) {
    static const PortFog modes[PORT_FOG_COUNT] = {
        {"ps2", "PS2"},               /* PORT_FOG_PS2 (in the enum's order: C++ includes this too) */
        {"volumetric", "Volumetric"}, /* PORT_FOG_VOLUMETRIC */
    };
    return mode >= 0 && mode < PORT_FOG_COUNT ? &modes[mode] : NULL;
}

/* The mode named name, or -1 for none */
static inline int port_fog_from_name(const char *name) {
    for (int m = 0; name != NULL && m < PORT_FOG_COUNT; m++) {
        if (strcmp(port_fog_mode(m)->name, name) == 0) {
            return m;
        }
    }
    return -1;
}

/* A map's fog block's two terms: F = near + far / w (MapFogBlockValues unk08 and unk0C, after --draw-distance) */
typedef struct {
    float near_term, far_term;
} PortFogTerms;

/* The PS2's fog value at distance w (w > 0), clamped as the GS clamps it: 255 no fog, 0 all fog colour */
static inline float port_fog_ps2_value(PortFogTerms fog, float w) {
    float f = fog.near_term + fog.far_term / w;
    return f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f;
}

/* How much of a colour at distance w the PS2's fog leaves: F / 255 */
static inline float port_fog_ps2_left(PortFogTerms fog, float w) {
    return port_fog_ps2_value(fog, w) / 255.0f;
}

/* What the volumetric fog leaves of a colour where the PS2's fog leaves left (F / 255), the ray's mean density m: left^m
   (pcr_prim.h's scn_fog_volume) */
static inline float port_fog_volume_left(float left, float m) {
    if (left <= 0.0f || left >= 1.0f || m == 1.0f) {
        return left; /* (all fog, none, or the PS2's) */
    }
    return powf(left, m);
}

/* The volumetric fog's look, which the player menu's sliders under Fog set (settings.json's display "fog_look"): each
   a whole number in the slider's units, one entry here. The scene renderer reads them as they are now
   (port_fog_look_now, port/host.h) and turns them into the shader's terms below. */
enum {
    PORT_FOG_LOOK_THICKNESS,  /* percent: the ray's mean density times this (100: as the noise and height make it) */
    PORT_FOG_LOOK_MIST,       /* percent: fog added along the ray where the PS2 has none, by the map's fog colour */
    PORT_FOG_LOOK_PATCHINESS, /* percent: how much the noise thickens and thins it */
    PORT_FOG_LOOK_PATCH_SIZE, /* world units (the eye is 220 over the floor): a noise cell */
    PORT_FOG_LOOK_LOW_LYING,  /* percent: how much thicker it is below the eye than above (0: the same) */
    PORT_FOG_LOOK_DRIFT,      /* percent: how fast the noise drifts */
    PORT_FOG_LOOK_COUNT
};

typedef struct {
    const char *name;  /* settings.json's key in display "fog_look" */
    const char *label; /* the player menu's */
    int low, high, step, standard;
} PortFogLookField;

static inline const PortFogLookField *port_fog_look_field(int field) {
    static const PortFogLookField fields[PORT_FOG_LOOK_COUNT] = {
        {"thickness", "Fog Thickness", 25, 400, 5, 100},     /* PORT_FOG_LOOK_THICKNESS (in the enum's order) */
        {"mist", "Fog Mist", 0, 200, 5, 0},                  /* PORT_FOG_LOOK_MIST */
        {"patchiness", "Fog Patchiness", 0, 100, 5, 60},     /* PORT_FOG_LOOK_PATCHINESS */
        {"patch_size", "Fog Patch Size", 300, 6000, 100, 1500}, /* PORT_FOG_LOOK_PATCH_SIZE */
        {"low_lying", "Fog Low-Lying", 0, 100, 5, 50},       /* PORT_FOG_LOOK_LOW_LYING */
        {"drift", "Fog Drift Speed", 0, 500, 10, 100},       /* PORT_FOG_LOOK_DRIFT */
    };
    return field >= 0 && field < PORT_FOG_LOOK_COUNT ? &fields[field] : NULL;
}

/* value kept within its field's range */
static inline int port_fog_look_clamped(int field, int value) {
    const PortFogLookField *f = port_fog_look_field(field);
    if (f == NULL) {
        return 0;
    }
    return value < f->low ? f->low : value > f->high ? f->high : value;
}

/* The mist's optical depth per world unit along the ray at density 1, from its percent: at 100%, 2000 units of mist
   leave 1 / e of a colour */
static inline float port_fog_mist_per_unit(int percent) {
    return (float)percent / 100.0f / 2000.0f;
}

/* The height term the shader thins the fog by (1 / the height, in noise cells, it thins by e over), from Low-Lying's
   percent: 50% is the first look's 0.35 cells */
static inline float port_fog_low_lying_per_cell(int percent) {
    return (float)percent / 100.0f * (2.0f / 0.35f);
}

/* What the volumetric fog leaves with the look's terms: left^m (the PS2's fog m times as dense) and the mist's
   optical depth mist_depth (its per unit times the ray's length times m) on top (pcr_prim.h's scn_fog_volume) */
static inline float port_fog_look_left(float left, float m, float mist_depth) {
    if (left <= 0.0f) {
        return 0.0f; /* (the PS2's whole fog) */
    }
    float base = port_fog_volume_left(left, m);
    return mist_depth > 0.0f ? base * expf(-mist_depth) : base;
}

#ifdef __cplusplus
}
#endif

#endif
