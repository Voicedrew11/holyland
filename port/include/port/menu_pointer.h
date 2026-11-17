/* The mouse in the game's menus (game/menu_pointer.c): point at a line to bring the game's cursor to it, click
   it to choose it, as in a PC program. Only with the Verdite layout, the mouse let go (port/sdk/controls.c:
   outside play) and no pad script playing or recording, so scripted runs stay the same.

   The pointer moves the cursor with the game's own presses: each menu frame it adds the up / down (or left /
   right) step toward the line pointed at to the menu's buttons, so a screen's cursor moves as a pad would move
   it, with its sounds and model loads. A click on a line presses cross once the cursor is on it; a click on
   nothing does nothing. Right click is still circle (back), and the wheel up / down.

   All on the game's thread. */
#ifndef PORT_MENU_POINTER_H
#define PORT_MENU_POINTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* what the menu does this frame for the pointer */
typedef struct {
    int step;    /* -1: a step to the line before (up, or left), 1: to the line after, 0: none */
    int confirm; /* 1: press cross (the cursor is on the line clicked) */
} MenuPointerMove;

/* (the patched menus) a menu frame starts, drawn on a screen of width x height pixels (the menus' 640 x 224
   field, the title's 640 x 448 frame); the lines given until the next start are hit-tested at the next */
void port_menu_pointer_begin(int width, int height);
/* (the patched menus) the part of line `line` (0 on) just drawn, from x0, y0 to x1, y1 in that screen's pixels;
   a line may be given several parts */
void port_menu_pointer_line(int line, int x0, int y0, int x1, int y1);
/* (the patched menus, once a menu frame, after port_menu_pointer_begin and before the screen reads its buttons)
   the cursor is on line `cursor_line`: what to press for the pointer, from the lines of the last frame */
MenuPointerMove port_menu_pointer_step(int cursor_line);
/* (port/sdk/controls.c) a screen that takes pointing is shown: the left button is not cross there */
int port_menu_pointer_active(void);

/* (port/sdk/controls.c, once a vblank) where the mouse is over the menus' picture, as a fraction of the 4:3 2D
   (0..1 across and down; outside: off the picture), whether it moved since the last, and the left button's
   presses since the last (each counted once) */
void port_menu_pointer_mouse(double x, double y, int moved, int clicks);

#ifdef __cplusplus
}
#endif

#endif
