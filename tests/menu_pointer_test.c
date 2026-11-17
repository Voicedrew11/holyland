/* The menus' pointer (game/menu_pointer.c): hit-testing the last frame's lines, stepping the cursor to the line
   pointed at, a click confirmed only once the cursor is there, and keys left alone while the mouse is still. */
#include <stdio.h>
#include <stdlib.h>

#include "port/menu_pointer.h"

static int failures;

#define CHECK(condition)                                                                                       \
    do {                                                                                                       \
        if (!(condition)) {                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                    \
            failures++;                                                                                        \
        }                                                                                                      \
    } while (0)

/* a menu frame of five lines, 24 pixels apart, from y 40, x 40 to 232, as the System screen draws them */
static MenuPointerMove frame(int cursor_line) {
    MenuPointerMove move;
    int line;

    port_menu_pointer_begin(640, 224);
    move = port_menu_pointer_step(cursor_line);
    for (line = 0; line < 5; line++) {
        port_menu_pointer_line(line, 40, 40 + 24 * line, 232, 64 + 24 * line);
    }
    return move;
}

/* the mouse at a pixel of the 640 x 224 screen */
static void mouse_at(int x, int y, int moved, int clicks) {
    port_menu_pointer_mouse((x + 0.5) / 640.0, (y + 0.5) / 224.0, moved, clicks);
}

static void test_hover_steps_to_line(void) {
    MenuPointerMove move;
    int cursor = 0;
    int frames;

    frame(cursor); /* the lines are drawn once */
    mouse_at(100, 40 + 24 * 3 + 5, 1, 0);
    for (frames = 0; frames < 10; frames++) {
        move = frame(cursor);
        CHECK(!move.confirm);
        cursor += move.step;
        mouse_at(100, 40 + 24 * 3 + 5, 0, 0);
    }
    CHECK(cursor == 3);
}

static void test_still_mouse_leaves_keys_alone(void) {
    MenuPointerMove move;

    mouse_at(100, 40 + 5, 1, 0);
    move = frame(2);
    CHECK(move.step == -1);
    move = frame(0); /* reached */
    CHECK(move.step == 0);
    mouse_at(100, 40 + 5, 0, 0);
    move = frame(4); /* the keys moved the cursor; the mouse did not move */
    CHECK(move.step == 0 && !move.confirm);
}

static void test_click_confirms_on_arrival(void) {
    MenuPointerMove move;

    mouse_at(100, 40 + 24 * 2 + 1, 1, 1);
    move = frame(0);
    CHECK(move.step == 1 && !move.confirm);
    move = frame(1);
    CHECK(move.step == 1 && !move.confirm);
    move = frame(2);
    CHECK(move.step == 0 && move.confirm);
    mouse_at(100, 40 + 24 * 2 + 1, 0, 0);
    move = frame(2);
    CHECK(!move.confirm); /* once */
}

static void test_click_on_nothing(void) {
    MenuPointerMove move;

    mouse_at(400, 100, 1, 1);
    move = frame(1);
    CHECK(move.step == 0 && !move.confirm);
    move = frame(1);
    CHECK(move.step == 0 && !move.confirm);
}

static void test_unreachable_line_given_up(void) {
    MenuPointerMove move;
    int frames;
    int stepped = 0;

    mouse_at(100, 40 + 24 * 4 + 1, 1, 1);
    for (frames = 0; frames < 100; frames++) {
        move = frame(1); /* the screen never moves its cursor */
        stepped += move.step != 0;
        CHECK(!move.confirm);
        mouse_at(100, 40 + 24 * 4 + 1, 0, 0);
    }
    CHECK(stepped > 0 && stepped < 100);
}

static void test_active_only_while_stepping(void) {
    int vblanks;

    frame(0);
    mouse_at(100, 50, 0, 0);
    CHECK(port_menu_pointer_active());
    for (vblanks = 0; vblanks < 10; vblanks++) {
        mouse_at(100, 50, 0, 0);
    }
    CHECK(!port_menu_pointer_active());
}

int main(void) {
    test_hover_steps_to_line();
    test_still_mouse_leaves_keys_alone();
    test_click_confirms_on_arrival();
    test_click_on_nothing();
    test_unreachable_line_given_up();
    test_active_only_while_stepping();
    if (failures != 0) {
        fprintf(stderr, "menu_pointer_test: %d failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("menu_pointer_test: ok\n");
    return EXIT_SUCCESS;
}
