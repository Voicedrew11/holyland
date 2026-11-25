/* hud_packet.h: PC-only. Drawing over the game's HUD with the GS's own primitives, as the game draws its HUD: one
   VIF DIRECT packet of GIF data appended to the game's packet chain after Hud_Draw, GS context 1, the HUD's alpha
   blending ((Cs - Cd) * As + Cd) and test (no Z written). What is drawn is laid out in its own units and placed in the
   HUD's 640 x 448 by a HudPlace (its top left there and its scale).

   Its textures are the HUD's own, still in GS memory from Hud_Draw (gage.tx2: the frame's stone and gold edge and the
   bars' fill), and the game's font (font.tmr, which the caller queues before HudPacket_Begin).

   Used by the hands mod's cross (game/hands_cross.c) and the extended stats' enemy health bar (game/stats_bar.c).
   Everything runs on the game's thread, inside GameLoop_DrawFrame. */
#ifndef GAME_HUD_PACKET_H
#define GAME_HUD_PACKET_H

#include "common.h"

/* the HUD's 640 x 448 (Hud_InitSprite: x + 1728, y + 1824 in GS pixels) */
#define HUD_WIDTH 640
#define HUD_HEIGHT 448
#define HUD_MARGIN 16 /* the HUD's frame (the health orb's edge) is drawn 16 from the top and the left */

typedef struct {
    float x, y;  /* the drawing's top left in the HUD's 640 x 448 */
    float scale; /* its units to the HUD's pixels */
} HudPlace;

/* a box in the drawing's own units, from its top left */
typedef struct {
    float x0, y0, x1, y1;
} HudBox;

typedef struct {
    u32 *start;
    u64 *at, *end;
    u64 *tag; /* the A+D tag being filled (NULL: none) */
    HudPlace place;
} HudPacket;

/* The GS registers written (GIF A+D) */
#define HUD_GS_TEX0_1 0x06
#define HUD_GS_TEX1_1 0x14
#define HUD_GS_CLAMP_1 0x08
#define HUD_GS_TEXFLUSH 0x3F

/* The quadwords each call below can take at most, for HudPacket_Fits and HudPacket_Begin */
#define HUD_QUADWORDS_FIXED 12      /* Begin and End */
#define HUD_QUADWORDS_RECT 4
#define HUD_QUADWORDS_FRAME (4 * HUD_QUADWORDS_RECT)
#define HUD_QUADWORDS_TEXELS 6
#define HUD_QUADWORDS_USE 4         /* HudPacket_UseGage, _UseFont, _UseTexture */
#define HUD_QUADWORDS_GAGE_EDGE (8 * HUD_QUADWORDS_TEXELS)
#define HUD_QUADWORDS_TEXT(chars) (2 * (chars) * 6) /* each glyph and its shadow */
#define HUD_QUADWORDS_IMAGE(width, height) ((width) * (height) * 4 / 16 + 6)

/* Whether the game's packet buffer can take a packet of `quadwords` more (a full one would overflow, and hang) */
int HudPacket_Fits(int quadwords);
/* Starts a packet of at most `quadwords` (HudPacket_Fits said yes) drawn at `place`; the VU1's own GIF packets go out
   first. The caller marks it for --widescreen (Widescreen_Mark) and queues any texture before it. */
void HudPacket_Begin(HudPacket *p, int quadwords, HudPlace place);
/* Puts back what Hud_Draw leaves (CLAMP 0, ALPHA 0x48) and appends the packet to the game's chain */
void HudPacket_End(HudPacket *p);

/* the GS's RGBAQ: 0x80 is full colour and opaque */
u64 hud_rgba(int r, int g, int b, int a);

/* an A+D register write */
void HudPacket_Register(HudPacket *p, int reg, u64 value);

/* an untextured box */
void HudPacket_Rect(HudPacket *p, HudBox box, u64 colour);
/* a frame one HUD pixel wide inside the box */
void HudPacket_Frame(HudPacket *p, HudBox box, u64 colour);
/* texels u0, v0 to u1, v1 of the texture in use drawn over the box; u0 > u1 or v0 > v1 draws them mirrored */
void HudPacket_Texels(HudPacket *p, float u0, float v0, float u1, float v1, HudBox box, u64 colour);
/* a picture sent to GS memory at `tbp` (PSMCT32, `width` a multiple of 64) */
void HudPacket_Image(HudPacket *p, int tbp, int width, int height, const u32 *pixels);

/* ---- gage.tx2, the HUD's frame (hud_sprites[0]: 256 x 128 PSMT8), still in GS memory from Hud_Draw ---------- */
#define HUD_GAGE_GREY 0x5A                  /* hud_sprites[0]'s colour */
#define HUD_GAGE_EDGE (9 * 0.8f)            /* the long bar's edge (stone, then a gold line), in HUD pixels */
/* gage.tx2 in use, as Hud_Draw draws its frame (bilinear, clamped) */
void HudPacket_UseGage(HudPacket *p);
/* The HUD long bar's stone and gold edge round the box, HUD_GAGE_EDGE wide inside it (gage.tx2 in use) */
void HudPacket_GageEdge(HudPacket *p, HudBox box, u64 colour);

/* ---- The bars' fill (hud_sprites[1] and [2]: 64 x 64 PSMT8 at 0x3780, also gage.tx2's) --------------------------- */
#define HUD_BAR_TEXELS_W 64.0f
#define HUD_BAR_TEXELS_H 20.0f              /* the power bar's strip, from t 0; the magic bar's is below it */
void HudPacket_UseBarFill(HudPacket *p);

/* ---- The game's font (font.tmr's half-width glyphs, ui/message.c's Message_DrawGlyph) ---------------------------- */
#define HUD_GLYPH_W 10.0f
#define HUD_GLYPH_H 20.0f
/* font.tmr to GS memory (as Message_Draw sends it), before HudPacket_Begin */
void HudPacket_QueueFont(void);
void HudPacket_UseFont(HudPacket *p);
/* ASCII text with its top left at x, y, its glyphs `size` times the font's (font in use), with a dark shadow as the
   game's text over the scene; returns its width */
float HudPacket_Text(HudPacket *p, const char *text, float x, float y, float size, u64 colour);
float hud_text_width(const char *text, float size);

#endif
