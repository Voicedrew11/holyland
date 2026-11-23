/* The menus' hints with a key wider than a letter's (PC only): with the Keyboard glyphs (--glyphs keyboard), a hint
   whose button is bound to Tab, Enter, Shift and so on is drawn in parts, its button shown as the key at its width on
   a real keyboard and its words moved along by as much (port/gs/shared/button_pictures.h, hint_parts). The patched
   ui/menu_draw.c asks for each sprite of the hints' sheet; port/platform/gs.c answers. */
#ifndef PORT_HINT_KEYS_H
#define PORT_HINT_KEYS_H

#ifdef __cplusplus
extern "C" {
#endif

/* One part of a hint: the sheet's texels u0 to u1 across, v0 to v1 down, drawn from x0 to x1 across the screen */
typedef struct {
    float x0, x1, u0, u1, v0, v1;
} PortHintPart;
#define PORT_HINT_PARTS 3

/* The parts a sprite showing the hints' sheet's texels u0 to u1 across, v0 to v1 down, from x0 to x1 across the
   screen, is drawn in: how many it wrote, 0 when it is drawn as it is (game thread) */
int port_hint_parts(float x0, float x1, float u0, float u1, float v0, float v1, PortHintPart parts[PORT_HINT_PARTS]);

/* How far across a hint is moved so its row makes room for the wider keys in it and keeps its middle: the hint
   drawn from x0 across, y0 to y1 down, showing the sheet's texels from (u0, v0), `extra` wider than the sheet has it
   (0 for a hint drawn whole). Each hint is given as it is drawn; the rows are those of the last menu frame (a frame
   ends when a hint already given in it is given again), so a row changes a frame after its keys do (game thread) */
float port_hint_row_shift(float x0, float y0, float y1, float u0, float v0, float extra);

#ifdef __cplusplus
}
#endif

#endif
