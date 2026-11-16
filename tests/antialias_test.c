/* The anti-aliasing methods' table (port/include/port/antialias.h): every method has a name and a label, each name
   finds its own method again, and anything else finds none.

     antialias_test */
#include <stdio.h>
#include <string.h>

#include "port/antialias.h"

static int failures = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "antialias_test: %s\n", what);
        failures++;
    }
}

int main(void) {
    for (int m = 0; m < PORT_AA_COUNT; m++) {
        const PortAntiAlias *a = port_antialias(m);
        check(a != NULL && a->name != NULL && a->label != NULL, "a method without a name or label");
        if (a != NULL && a->name != NULL) {
            check(port_antialias_from_name(a->name) == m, "a name that does not find its own method");
        }
    }
    check(port_antialias(PORT_AA_OFF) != NULL && strcmp(port_antialias(PORT_AA_OFF)->name, "off") == 0,
          "PORT_AA_OFF is not \"off\" (settings.json and --antialias spell it so)");
    check(port_antialias(PORT_AA_FXAA) != NULL && strcmp(port_antialias(PORT_AA_FXAA)->name, "fxaa") == 0,
          "PORT_AA_FXAA is not \"fxaa\"");
    check(port_antialias(PORT_AA_SMAA) != NULL && strcmp(port_antialias(PORT_AA_SMAA)->name, "smaa") == 0,
          "PORT_AA_SMAA is not \"smaa\"");
    check(port_antialias(PORT_AA_SMAA_T2X) != NULL && strcmp(port_antialias(PORT_AA_SMAA_T2X)->name, "smaa_t2x") == 0,
          "PORT_AA_SMAA_T2X is not \"smaa_t2x\"");
    check(port_antialias(-1) == NULL && port_antialias(PORT_AA_COUNT) == NULL, "a method past the table");
    check(port_antialias_from_name(NULL) == -1, "no name finds a method");
    check(port_antialias_from_name("msaa") == -1 && port_antialias_from_name("") == -1 &&
              port_antialias_from_name("FXAA") == -1,
          "an unknown name finds a method");
    if (failures == 0) {
        printf("antialias_test: %d methods ok\n", PORT_AA_COUNT);
    }
    return failures == 0 ? 0 : 1;
}
