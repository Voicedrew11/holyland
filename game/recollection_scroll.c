/* PC: the game's Recollection page (Menu_UpdateRecollectionPage, the chronicle's pages and the people's words) scrolls
   when its text is longer than the seven lines its box holds: up and down move it a line, and the screen's arrows
   (the list's triangles) show where there is more. For the retranslation's fuller text (port/platform/mods.c).
   The game's long-text drawing leaves out the lines outside the window (menu_text_first_line, ui/menu_draw.c). */
#include "common.h"
#include "gfx/packet.h"
#include "input/pad.h"
#include "ui/menu_draw.h"

#define PAGE_LINES 7

extern s32 menu_text_first_line;
extern s32 menu_text_line_count;
extern s32 menu_text_lines;
void Sound_PlayMenu(int);
extern u16 menu_buttons;
extern u32 image_part_count;
extern SpritePacket *image_parts;
int Menu_SetBlendMode(u8);
void MenuSprite_DrawPlain(sptr owner, int index, int param);

static s32 shown_id = -1; /* the message the page shows */
static s32 top;           /* its first line shown */
static s32 lines;         /* its lines, as drawn last */

/* Before the page's text is drawn: the window, moved by up and down */
void RecollectionScroll_Begin(s32 message) {
    if (message != shown_id) {
        shown_id = message;
        top = 0;
        lines = 0;
    }
    if ((menu_buttons & PAD_DOWN) && top + PAGE_LINES < lines) {
        top++;
        Sound_PlayMenu(1);
    } else if ((menu_buttons & PAD_UP) && top > 0) {
        top--;
        Sound_PlayMenu(1);
    }
    menu_text_first_line = top;
    menu_text_line_count = PAGE_LINES;
}

/* After it: the window off again, and the arrows where there is more. The arrows are the list screen's (the parts
   it shows by slot, handler 2, that are drawn added on: the upper one points up), drawn here as they are. */
void RecollectionScroll_End(void) {
    s32 up = -1, down = -1;
    u32 i;

    lines = menu_text_lines;
    menu_text_first_line = 0;
    menu_text_line_count = 0x7FFF;
    for (i = 0; i < image_part_count; i++) {
        if (image_parts[i].handler_index != 2 || image_parts[i].blend_mode != 1 || image_parts[i].kind != 0) {
            continue;
        }
        if (up < 0) {
            up = i;
        } else if (down < 0) {
            down = i;
        }
    }
    if (up < 0 || down < 0) {
        return;
    }
    if (image_parts[down].xyz0.Y + image_parts[down].xyz1.Y < image_parts[up].xyz0.Y + image_parts[up].xyz1.Y) {
        s32 swap = up;
        up = down;
        down = swap;
    }
    if (top > 0) {
        Menu_SetBlendMode(1);
        MenuSprite_DrawPlain((sptr)&image_parts[up], 0, 0);
    }
    if (top + PAGE_LINES < lines) {
        Menu_SetBlendMode(1);
        MenuSprite_DrawPlain((sptr)&image_parts[down], 0, 0);
    }
    Menu_SetBlendMode(0);
}
