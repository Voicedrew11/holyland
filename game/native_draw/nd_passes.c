/* nd_passes.c: PC-only. Native draw's kind "passes" (--native-draw passes, PORT_ND_PASSES; P7-1 of
   docs/port/NATIVE_DRAW_TASKS.md, NATIVE_DRAW.md 1.3): the screen passes GameLoop_DrawFrame draws after the
   world, as data instead of GIF packets.

   On the PS2 path each pass is a few register lists and GIF packets of sprites (the brightness, a tint and
   the fills in ten 64-pixel strips; the flicker filter in ten or twenty; the view-turn shift in twenty; the
   wobble grid in 70 cells), each passed on to the GS by VU1's program 0x276. Natively the call site hands
   the pass and its parameters to the renderer core (rd_pass), and queues a PORT_GS_REG_NATIVE_DRAW write
   naming it at the very place of those packets; the renderer draws it, as the packets would have
   drawn them (the PC renderer does not draw passes yet: PORT_ND_PASSES is off under it), and leaves the GS's registers as they would have left them. VU1 runs nothing for it.

   Two kinds of call site (core.patch):
   - a whole call, in GameLoop_DrawFrame: `if (!port_native_draw(PORT_ND_PASSES) || !NativeDraw_X(...))
     { Game_X(...); }`. NativeDraw_X returns 1 when the core took the pass, 0 when the game draws it.
   - inside a function whose game state must stay where it is (Game_DrawFade, Game_ShiftScreenByViewTurn,
     Game_DrawWobblingScreen: the fade's colour, the turn's offset, the grid's springs are computed by the
     game; only the drawing is the pass's): `if (!port_native_draw(PORT_ND_PASSES) || !NativeDraw_X(...))
     { the drawing }` and then `NativeDraw_PassEnd();`. NativeDraw_X returns 1 when the core took the pass
     (the drawing is left out) and 0 when the game draws it.

   Nothing here changes what the game does: it reads the game's state and writes only the packet chain;
   with the switch off it is not called (NativeDraw_PassEnd is, and does nothing then). */
#include "common.h"
#include "gfx/gs.h"
#include "port/render.h"

#include <string.h>

void NativeDraw_QueueMark(u32 command, u64 bits); /* (native_draw.c) */

void Game_ApplyBrightness(int brightness);
void Game_TintScreen(int red, int green, int blue);
void Game_BlendScreenOffsetDown(void);
void Game_BlendScreenOffsetUpDown(void);
void Game_BlendPreviousFrame(int alpha);
void Game_DrawHazeLayers(void);
void GsDraw_FillScreen(int red, int green, int blue, int alpha, s64 blend);
u32 fptoui(float);

extern int context1_buffer_half;
extern ScreenRect full_screen_rect;
extern u32 haze_layer_count;
extern int haze_layer_depths[8];
extern GsColor haze_sprite_color;

/* the pass begun: its kind, call site and the frame it reads (the game's own choice of buffer: most
   passes read the frame being drawn, at 0x1180 when context1_buffer_half is 0) */
static void pass_begin(RdPass *p, u32 pass, u32 site) {
    memset(p, 0, sizeof(*p));
    p->pass = pass;
    p->site = site;
    p->frame_tbp = context1_buffer_half ? 0 : 0x1180;
}

/* the pass into the side buffer, and its mark in the chain; 1 if the core took it, 0 if not (nothing queued:
   the caller draws it the PS2 way) */
static int pass_queue(const RdPass *p) {
    u64 handle = rd_pass(p);

    if (handle == 0) {
        rd_stats.declined++;
        return 0;
    }
    NativeDraw_QueueMark(RD_ND_DRAW, handle & 0xFFFFFFFFFFFFFFull);
    return 1;
}

/* After an inside call site's drawing. The PS2 path's packets are not queued beside a native pass, so there
   is nothing to close; the patched call sites still call it. */
void NativeDraw_PassEnd(void) {
}

/* Game_ApplyBrightness(brightness): the frame times brightness / 128 (the option, game_options[10]); last */
int NativeDraw_Brightness(int brightness) {
    RdPass p;

    pass_begin(&p, RD_PASS_SCALE, RD_PASS_SITE_BRIGHTNESS);
    p.arg[0] = p.arg[1] = p.arg[2] = (u8)brightness; /* (the sprite's colour bytes) */
    return pass_queue(&p);
}

/* Game_TintScreen(red, green, blue): under water, the frame times the colour / 128 */
int NativeDraw_TintScreen(int red, int green, int blue) {
    RdPass p;

    pass_begin(&p, RD_PASS_SCALE, RD_PASS_SITE_TINT);
    p.arg[0] = (u8)red;
    p.arg[1] = (u8)green;
    p.arg[2] = (u8)blue;
    return pass_queue(&p);
}

/* GsDraw_FillScreen(red, green, blue, alpha, blend) from GameLoop_DrawFrame: a flat colour blended over the
   screen (blend: the whole ALPHA register) */
int NativeDraw_FillScreen(int red, int green, int blue, int alpha, s64 blend) {
    RdPass p;

    pass_begin(&p, RD_PASS_FILL, RD_PASS_SITE_FILL);
    p.arg[0] = (u8)red;
    p.arg[1] = (u8)green;
    p.arg[2] = (u8)blue;
    p.arg[3] = (u8)alpha;
    p.arg[4] = (u32)(u64)blend;
    p.arg[5] = (u32)((u64)blend >> 32);
    return pass_queue(&p);
}

/* Game_DrawFade's drawing (inside it: the fade's colour is its own), alpha above 0 */
int NativeDraw_Fade(int red, int green, int blue, int alpha) {
    RdPass p;

    pass_begin(&p, RD_PASS_FADE, RD_PASS_SITE_FADE);
    p.arg[0] = (u8)red;
    p.arg[1] = (u8)green;
    p.arg[2] = (u8)blue;
    p.arg[3] = (u8)alpha;
    return pass_queue(&p);
}

/* Game_BlendScreenOffsetDown (updown 0) or _UpDown (1): the flicker filter; its strips only with
   --flicker-filter, its registers either way (core.patch's change, kept) */
int NativeDraw_FlickerFilter(int updown) {
    RdPass p;

    pass_begin(&p, RD_PASS_FLICKER, RD_PASS_SITE_FLICKER);
    p.arg[0] = updown != 0;
    p.arg[1] = port_flicker_filter() != 0;
    return pass_queue(&p);
}

/* Game_BlendPreviousFrame(alpha): the frame shown before blended in (map_draw_state bit 40, or aiming) */
int NativeDraw_BlendPreviousFrame(int aiming, int alpha) {
    RdPass p;

    pass_begin(&p, RD_PASS_PREVIOUS, aiming ? RD_PASS_SITE_AIM : RD_PASS_SITE_PREVIOUS);
    p.frame_tbp = context1_buffer_half == 0 ? 0 : 0x1180; /* (the other buffer: the frame shown) */
    p.arg[0] = (u32)alpha;
    p.arg[1] = (u32)full_screen_rect.left;
    p.arg[2] = (u32)full_screen_rect.top;
    p.arg[3] = (u32)full_screen_rect.right;
    p.arg[4] = (u32)full_screen_rect.bottom;
    p.arg[5] = (u32)full_screen_rect.left; /* (its texels: the same rectangle) */
    p.arg[6] = (u32)full_screen_rect.top;
    p.arg[7] = (u32)full_screen_rect.right;
    p.arg[8] = (u32)full_screen_rect.bottom;
    NativeDraw_QueueMark(RD_ND_READS_SHOWN, 0); /* (for --frame-rate's pictures between: frame_interp.c) */
    return pass_queue(&p);
}

/* Game_ShiftScreenByViewTurn's twenty strips (inside it: the turn is the game's to measure): the first
   strip's sprite as the game made it, and the step from strip to strip */
int NativeDraw_ViewShift(const void *sprite, int step) {
    RdPass p;

    pass_begin(&p, RD_PASS_VIEW_SHIFT, RD_PASS_SITE_VIEW_SHIFT);
    memcpy(p.data, sprite, sizeof(GsSpritePacket));
    p.arg[0] = (u32)step;
    return pass_queue(&p);
}

/* Game_DrawHazeLayers (map_draw_state bit 44; nothing sets it): a full-screen sprite of the frame at each
   layer's depth */
int NativeDraw_HazeLayers(void) {
    RdPass p;
    u32 i;

    if (haze_layer_count == 0) {
        return 1; /* (it draws nothing, and writes nothing) */
    }
    pass_begin(&p, RD_PASS_HAZE, RD_PASS_SITE_HAZE);
    p.arg[0] = haze_layer_count;
    for (i = 0; i < 4; i++) {
        p.arg[1 + i] = (u32)haze_sprite_color.rgba[i];
    }
    for (i = 0; i < haze_layer_count && i < 8; i++) {
        p.data[i] = (u32)haze_layer_depths[i];
    }
    return pass_queue(&p);
}

/* Game_DrawWobblingScreen's cells (inside it, after the springs' step and its registers): the grabbed frame
   at 0x2BC0 drawn through the grid, each point's texel t of the way blend from where it was laid out to
   where it wobbled, as the packets' UV words have it */
int NativeDraw_WobbleGrid(int (*points)[11][2], int (*origin)[11][2], float blend, int red, int green, int blue,
                          int alpha) {
    RdPass p;
    int i, j;

    pass_begin(&p, RD_PASS_WOBBLE, RD_PASS_SITE_WOBBLE);
    p.arg[0] = (u32)red;
    p.arg[1] = (u32)green;
    p.arg[2] = (u32)blue;
    p.arg[3] = (u32)alpha;
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 11; j++) {
            /* (the game's expressions, as it writes each cell's corners) */
            int origin_x = origin[i][j][0], origin_y = origin[i][j][1];
            u32 u = origin_x + fptoui((points[i][j][0] - origin_x) * blend);
            u32 v = origin_y + fptoui((points[i][j][1] - origin_y) * blend);

            p.data[i * 11 + j] = (u & 0x3FFF) | (v & 0x3FFF) << 16;
        }
    }
    return pass_queue(&p);
}
