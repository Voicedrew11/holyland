/* hint_rows.c: PC-only. The menus' hint rows made room for wide keys (port/include/port/hint_keys.h,
   port_hint_row_shift).

   The hints drawn are kept; when one already kept is drawn again (or MAX_HINTS are), the frame is over and its hints become the rows
   the next frame is laid out by. A hint's row is the hints drawn at the same rows of the screen; it is moved by the
   extra width of the hints before it in its row, less half the row's extra, so the row grows both ways.

   Nothing here reads the game or the port, so tests/hint_rows_test.c runs it alone. Game thread only. */
#include "port/hint_keys.h"

#define MAX_HINTS 32 /* (a screen draws at most a dozen) */

typedef struct {
    float x0, y0, y1, u0, v0;
    float extra;
} Hint;

typedef struct {
    Hint hints[MAX_HINTS];
    int count;
} HintSet;

static struct {
    HintSet sets[2]; /* the frame being drawn, and the last one, laid out by */
    int drawing;     /* which of sets is being drawn */
} rows;

static int same_hint(const Hint *a, const Hint *b) {
    return a->x0 == b->x0 && a->y0 == b->y0 && a->y1 == b->y1 && a->u0 == b->u0 && a->v0 == b->v0;
}

static int same_row(const Hint *a, const Hint *b) {
    return a->y0 == b->y0 && a->y1 == b->y1;
}

/* Whether the hint was given already in the frame being drawn */
static int drawn_already(const HintSet *set, const Hint *hint) {
    for (int i = 0; i < set->count; i++) {
        if (same_hint(&set->hints[i], hint)) {
            return 1;
        }
    }
    return 0;
}

/* The hint's move in its row of the last frame (0 when the last frame had no such row) */
static float shift_in(const HintSet *last, const Hint *hint) {
    float before = 0.0f, total = 0.0f;
    for (int i = 0; i < last->count; i++) {
        const Hint *other = &last->hints[i];
        if (!same_row(other, hint)) {
            continue;
        }
        total += other->extra;
        if (other->x0 < hint->x0) {
            before += other->extra;
        }
    }
    return before - total / 2;
}

float port_hint_row_shift(float x0, float y0, float y1, float u0, float v0, float extra) {
    const Hint hint = {x0, y0, y1, u0, v0, extra};
    HintSet *drawing = &rows.sets[rows.drawing];

    /* (a full set ends the frame too: hints sliding in are never the same twice) */
    if (drawn_already(drawing, &hint) || drawing->count == MAX_HINTS) {
        rows.drawing ^= 1;
        drawing = &rows.sets[rows.drawing];
        drawing->count = 0;
    }
    drawing->hints[drawing->count++] = hint;
    return shift_in(&rows.sets[rows.drawing ^ 1], &hint);
}
