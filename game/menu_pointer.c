/* menu_pointer.c: PC-only. The mouse in the game's menus (port/include/port/menu_pointer.h).

   The lines a menu frame draws are kept; the next frame hit-tests the mouse against them. Moving the mouse onto
   a line makes it the target; the cursor is stepped toward it, one step a frame, until it is there (or a few
   frames pass with the cursor still, as on a line the screen skips). Keys and the pad still move the cursor
   while the mouse is still. A click on a line makes it the target and presses cross once the cursor is on it.

   Nothing here reads the game or the port: it is fed the mouse (port/sdk/controls.c) and the menus' lines
   (the patched ui/menu_draw.c), so tests/menu_pointer_test.c runs it alone. Game thread only. */
#include <stdio.h>
#include <stdlib.h>

#include "port/menu_pointer.h"

#define MAX_LINE_PARTS 256
#define ACTIVE_VBLANKS 4 /* a screen that stepped this many vblanks ago still takes pointing */
#define STUCK_FRAMES 30  /* frames with the cursor still before the target is given up (a fade takes ~10) */

typedef struct {
    int line;
    int x0, y0, x1, y1;
} LinePart;

typedef struct {
    LinePart parts[MAX_LINE_PARTS];
    int count;
    int width, height;
} LineSet;

static struct {
    LineSet sets[2];       /* the frame being drawn, and the last one, hit-tested */
    int drawing;           /* which of sets is being drawn */
    double mouse_x;        /* the mouse, as a fraction of the 4:3 2D */
    double mouse_y;
    int moved;             /* the mouse moved since the last step */
    int clicks;            /* left presses since the last step */
    int target;            /* the line the cursor is stepped toward (-1: none) */
    int confirm_target;    /* 1: cross is pressed once the cursor is on the target */
    int stuck;             /* frames the cursor stayed where it was, short of the target */
    int last_cursor;
    int vblanks_since_step;
    int log;               /* KF4_MENU_POINTER_LOG */
    int log_checked;
} mp = {.target = -1, .vblanks_since_step = ACTIVE_VBLANKS + 1};

static int logging(void) {
    if (!mp.log_checked) {
        const char *value = getenv("KF4_MENU_POINTER_LOG");

        mp.log = value != NULL && value[0] != '\0' && value[0] != '0';
        mp.log_checked = 1;
    }
    return mp.log;
}

void port_menu_pointer_begin(int width, int height) {
    mp.drawing ^= 1;
    mp.sets[mp.drawing].count = 0;
    mp.sets[mp.drawing].width = width;
    mp.sets[mp.drawing].height = height;
}

void port_menu_pointer_line(int line, int x0, int y0, int x1, int y1) {
    LineSet *set = &mp.sets[mp.drawing];
    LinePart *part;

    if (line < 0 || set->count >= MAX_LINE_PARTS) {
        return;
    }
    part = &set->parts[set->count++];
    part->line = line;
    part->x0 = x0 < x1 ? x0 : x1;
    part->x1 = x0 < x1 ? x1 : x0;
    part->y0 = y0 < y1 ? y0 : y1;
    part->y1 = y0 < y1 ? y1 : y0;
}

/* the line under the mouse in the last frame's lines (-1: none); the smallest part wins where they overlap */
static int line_pointed(void) {
    const LineSet *set = &mp.sets[mp.drawing ^ 1];
    double x = mp.mouse_x * set->width;
    double y = mp.mouse_y * set->height;
    long best_area = -1;
    int best = -1;
    int i;

    for (i = 0; i < set->count; i++) {
        const LinePart *part = &set->parts[i];
        long area = (long)(part->x1 - part->x0) * (part->y1 - part->y0);

        if (x < part->x0 || x >= part->x1 || y < part->y0 || y >= part->y1) {
            continue;
        }
        if (best_area < 0 || area < best_area) {
            best_area = area;
            best = part->line;
        }
    }
    return best;
}

static void log_frame(int cursor_line, int pointed, MenuPointerMove move) {
    const LineSet *set = &mp.sets[mp.drawing ^ 1];
    static int last_count = -1;
    int i;

    if (set->count != last_count) {
        fprintf(stderr, "[menu_pointer] %d parts on %dx%d:", set->count, set->width, set->height);
        for (i = 0; i < set->count; i++) {
            const LinePart *part = &set->parts[i];

            fprintf(stderr, " %d:%d,%d-%d,%d", part->line, part->x0, part->y0, part->x1, part->y1);
        }
        fprintf(stderr, "\n");
        last_count = set->count;
    }
    if (move.step != 0 || move.confirm || (mp.moved && pointed >= 0)) {
        fprintf(stderr, "[menu_pointer] cursor %d pointed %d target %d step %d confirm %d\n", cursor_line, pointed,
                mp.target, move.step, move.confirm);
    }
}

MenuPointerMove port_menu_pointer_step(int cursor_line) {
    MenuPointerMove move = {0, 0};
    int pointed = line_pointed();
    int distance;

    mp.vblanks_since_step = 0;
    if (mp.clicks > 0 && pointed >= 0) {
        mp.target = pointed;
        mp.confirm_target = 1;
        mp.stuck = 0;
    } else if (mp.moved && pointed >= 0 && !mp.confirm_target && pointed != mp.target) {
        mp.target = pointed;
        mp.stuck = 0;
    }
    mp.clicks = 0;
    if (mp.target >= 0) {
        distance = mp.target - cursor_line;
        if (distance == 0) {
            move.confirm = mp.confirm_target;
            mp.target = -1;
            mp.confirm_target = 0;
        } else if (cursor_line == mp.last_cursor && ++mp.stuck > STUCK_FRAMES) {
            mp.target = -1; /* (a line the cursor does not stop on, or a list that wraps elsewhere) */
            mp.confirm_target = 0;
        } else {
            if (cursor_line != mp.last_cursor) {
                mp.stuck = 0;
            }
            move.step = distance < 0 ? -1 : 1;
        }
    }
    mp.last_cursor = cursor_line;
    if (logging()) {
        log_frame(cursor_line, pointed, move);
    }
    mp.moved = 0;
    return move;
}

int port_menu_pointer_active(void) {
    return mp.vblanks_since_step <= ACTIVE_VBLANKS;
}

void port_menu_pointer_mouse(double x, double y, int moved, int clicks) {
    mp.mouse_x = x;
    mp.mouse_y = y;
    mp.moved |= moved;
    mp.clicks += clicks;
    if (mp.vblanks_since_step <= ACTIVE_VBLANKS) {
        mp.vblanks_since_step++;
    }
    if (mp.vblanks_since_step > ACTIVE_VBLANKS) {
        /* (no pointing screen: nothing waits for one) */
        mp.target = -1;
        mp.confirm_target = 0;
        mp.clicks = 0;
    }
}
