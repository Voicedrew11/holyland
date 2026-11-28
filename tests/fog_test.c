/* The fog's modes and maths (port/include/port/fog.h): every mode has a name and a label that finds it again; the PS2's
   fog value is its terms' and clamped as the GS clamps it; and the volumetric fog is the PS2's where its density is 1,
   thicker where it is denser (twice as dense: what is left squared), and leaves the PS2's no fog and whole fog alone.

     fog_test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "port/fog.h"

static int failures = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "fog_test: %s\n", what);
        failures++;
    }
}

static int near_to(float a, float b, float within) {
    return fabsf(a - b) <= within;
}

int main(void) {
    for (int m = 0; m < PORT_FOG_COUNT; m++) {
        const PortFog *f = port_fog_mode(m);
        check(f != NULL && f->name != NULL && f->label != NULL, "a mode without a name or label");
        if (f != NULL && f->name != NULL) {
            check(port_fog_from_name(f->name) == m, "a name that does not find its own mode");
        }
    }
    check(strcmp(port_fog_mode(PORT_FOG_PS2)->name, "ps2") == 0, "PORT_FOG_PS2 is not \"ps2\" (settings.json's)");
    check(strcmp(port_fog_mode(PORT_FOG_VOLUMETRIC)->name, "volumetric") == 0, "PORT_FOG_VOLUMETRIC is not \"volumetric\"");
    check(port_fog_mode(-1) == NULL && port_fog_mode(PORT_FOG_COUNT) == NULL, "a mode past the table");
    check(port_fog_from_name(NULL) == -1 && port_fog_from_name("") == -1 && port_fog_from_name("PS2") == -1,
          "an unknown name finds a mode");

    /* the PS2's: F = -255 + 255000 / w, no fog to w 500, all fog from 1000 (map 024's kind: a fog that ends) */
    PortFogTerms ends = {-255.0f, 255000.0f};
    check(port_fog_ps2_value(ends, 400.0f) == 255.0f && port_fog_ps2_value(ends, 2000.0f) == 0.0f,
          "the PS2's value is not clamped to 0..255");
    check(near_to(port_fog_ps2_left(ends, 750.0f), (-255.0f + 340.0f) / 255.0f, 1e-5f), "the PS2's value between");
    PortFogTerms tint = {200.0f, 0.0f}; /* (one value everywhere) */
    check(near_to(port_fog_ps2_left(tint, 10.0f), 200.0f / 255.0f, 1e-6f) &&
              near_to(port_fog_ps2_left(tint, 1e6f), 200.0f / 255.0f, 1e-6f),
          "a fog of one value is not the same everywhere");

    /* the volumetric fog: the PS2's where the density is 1, at every distance */
    for (int i = 0; i < 40; i++) {
        float w = 300.0f + 25.0f * (float)i;
        float left = port_fog_ps2_left(ends, w);
        check(port_fog_volume_left(left, 1.0f) == left, "the volumetric fog at density 1 is not the PS2's");
        if (left > 0.0f && left < 1.0f) {
            check(port_fog_volume_left(left, 0.3f) > left, "a thinner fog does not leave more");
            check(port_fog_volume_left(left, 1.7f) < left, "a denser fog does not leave less");
        }
    }
    float left = port_fog_ps2_left(ends, 800.0f);
    check(near_to(port_fog_volume_left(left, 2.0f), left * left, 1e-6f), "twice as dense does not leave its square");
    check(port_fog_volume_left(0.0f, 0.2f) == 0.0f, "the PS2's whole fog thinned");
    check(port_fog_volume_left(1.0f, 3.0f) == 1.0f, "no fog thickened");

    if (failures == 0) {
        printf("fog_test: ok\n");
    }
    return failures != 0;
}
