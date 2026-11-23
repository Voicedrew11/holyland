/* The menus' hint rows (game/hint_rows.c): a row of hints moved apart by its wide keys' extra width, keeping its
   middle, laid out from the last frame; other rows and hints drawn whole left where they are. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "port/hint_keys.h"

static int failures;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        if (!(condition)) {                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                    \
            failures++;                                                                                        \
        }                                                                                                      \
    } while (0)

static int near(float a, float b) {
    return fabsf(a - b) < 0.001f;
}

/* one frame of a row of three hints 128 apart (the middle one 20 wider), and one of another row; their shifts */
static void frame(float extra_middle, float shifts[4]) {
    shifts[0] = port_hint_row_shift(-212, 86, 102, 191, 128, 0);
    shifts[1] = port_hint_row_shift(-84, 86, 102, 63, 64, extra_middle);
    shifts[2] = port_hint_row_shift(44, 86, 102, 320, 64, 0);
    shifts[3] = port_hint_row_shift(44, 20, 36, 320, 96, 0);
}

int main(void) {
    float shifts[4];

    /* (the first frame: nothing laid out yet) */
    frame(20, shifts);
    CHECK(near(shifts[0], 0) && near(shifts[1], 0) && near(shifts[2], 0));
    /* (the next: the row grows 10 each way, the hints after the wide one moved past it) */
    frame(20, shifts);
    CHECK(near(shifts[0], -10));
    CHECK(near(shifts[1], -10));
    CHECK(near(shifts[2], 10));
    CHECK(near(shifts[3], 0));
    /* (the key made narrow again: back where the disc has them a frame later) */
    frame(0, shifts);
    CHECK(near(shifts[2], 10));
    frame(0, shifts);
    CHECK(near(shifts[0], 0) && near(shifts[1], 0) && near(shifts[2], 0));

    if (failures) {
        fprintf(stderr, "hint_rows: %d failure(s)\n", failures);
        return 1;
    }
    printf("hint_rows: all passed\n");
    return 0;
}
