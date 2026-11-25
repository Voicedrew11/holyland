/* hud_packet.c: PC-only. Drawing over the game's HUD with the GS's own primitives (game/hud_packet.h). */
#include <string.h>

#include "common.h"
#include "gfx/texture.h"
#include "port/platform.h"
#include "hud_packet.h"

extern int packet_quadwords_used; /* gfx/packet_buffer.c */
extern int packet_buffers_size;
extern sptr menu_heap;
u32 *Packet_Alloc(int quadwords);
void Packet_AppendToChain(u32 *packet, u32 *packet_end);
void GsDraw_QueueFlush(void);
void TextureTable_QueueAll(TextureTableHeader *table);
sptr Message_GetFontTexture(sptr font_base); /* ui/message.c: font.tmr, the game's font */

#define HUD_GS_X0 1728
#define HUD_GS_Y0 1824

#define GS_PRIM 0x00
#define GS_RGBAQ 0x01
#define GS_ST 0x02
#define GS_UV 0x03
#define GS_XYZ2 0x05
#define GS_ALPHA_1 0x42
#define GS_TEST_1 0x47
#define GS_BITBLTBUF 0x50
#define GS_TRXPOS 0x51
#define GS_TRXREG 0x52
#define GS_TRXDIR 0x53

#define PRIM_SPRITE 6
#define PRIM_TME 0x10
#define PRIM_ABE 0x40
#define PRIM_FST 0x100

/* ---- The packet ------------------------------------------------------------------------------------------------ */
int HudPacket_Fits(int quadwords) {
    return packet_quadwords_used + quadwords + HUD_QUADWORDS_FIXED + 64 <= packet_buffers_size / 16;
}

void HudPacket_Begin(HudPacket *p, int quadwords, HudPlace place) {
    quadwords += HUD_QUADWORDS_FIXED;
    GsDraw_QueueFlush(); /* the VU1's own GIF packets out before ours (as TextureTable_QueueAll does) */
    /* a DMA tag, a quadword of VIF codes (NOP, NOP, NOP, DIRECT: as the game's own packets, the VIF codes in the data),
       the GIF data, and a quadword for the chain's next tag */
    p->start = Packet_Alloc(2 + quadwords + 1);
    p->at = (u64 *)(p->start + 8);
    p->end = p->at + quadwords * 2;
    p->tag = NULL;
    p->place = place;
    HudPacket_Register(p, GS_ALPHA_1, 0x44);   /* (Cs - Cd) * As + Cd */
    HudPacket_Register(p, GS_TEST_1, 0x31001); /* the HUD's: no Z written */
}

void HudPacket_End(HudPacket *p) {
    int quadwords;

    HudPacket_Register(p, HUD_GS_CLAMP_1, 0); /* as Hud_Draw leaves them */
    HudPacket_Register(p, GS_ALPHA_1, 0x48);
    p->tag[0] |= (u64)1 << 15; /* EOP */
    quadwords = (int)(p->at - (u64 *)(p->start + 8)) / 2;
    if (p->at > p->end) {
        port_event("hud packet: ran over (%d quadwords)", quadwords); /* (cannot be: the caller's count) */
    }
    p->start[0] = 0x10000000u | (u32)(quadwords + 1); /* DMA cnt */
    p->start[1] = p->start[2] = p->start[3] = 0;
    p->start[4] = p->start[5] = p->start[6] = 0;      /* VIF NOP */
    p->start[7] = 0x50000000u | (u32)quadwords;       /* VIF DIRECT */
    Packet_AppendToChain(p->start, p->start + 8 + quadwords * 4);
}

u64 hud_rgba(int r, int g, int b, int a) {
    return (u64)(r & 0xFF) | (u64)(g & 0xFF) << 8 | (u64)(b & 0xFF) << 16 | (u64)(a & 0xFF) << 24 | (u64)0x3F800000 << 32;
}

/* An A+D register write; the tag in front of a run of them counts them */
void HudPacket_Register(HudPacket *p, int reg, u64 value) {
    if (p->tag == NULL) {
        p->tag = p->at;
        p->tag[0] = 0;
        p->tag[1] = 0xE; /* REGS: A+D */
        p->at += 2;
    }
    p->at[0] = value;
    p->at[1] = (u64)reg;
    p->at += 2;
    p->tag[0] += 1;            /* NLOOP */
    p->tag[0] |= (u64)1 << 60; /* NREG 1 */
}

/* a point of the drawing, in its own units, as the GS's XYZ2 */
static u64 xyz(const HudPacket *p, float x, float y) {
    u64 gx = (u64)(int)((HUD_GS_X0 + p->place.x + x * p->place.scale) * 16.0f + 0.5f) & 0xFFFF;
    u64 gy = (u64)(int)((HUD_GS_Y0 + p->place.y + y * p->place.scale) * 16.0f + 0.5f) & 0xFFFF;

    return gx | gy << 16;
}

static u64 uv(float u, float v) {
    return (u64)(int)(u * 16.0f) | (u64)(int)(v * 16.0f) << 16;
}

/* ---- Primitives ------------------------------------------------------------------------------------------------- */
void HudPacket_Rect(HudPacket *p, HudBox box, u64 colour) {
    HudPacket_Register(p, GS_PRIM, PRIM_SPRITE | PRIM_ABE);
    HudPacket_Register(p, GS_RGBAQ, colour);
    HudPacket_Register(p, GS_XYZ2, xyz(p, box.x0, box.y0));
    HudPacket_Register(p, GS_XYZ2, xyz(p, box.x1, box.y1));
}

void HudPacket_Frame(HudPacket *p, HudBox b, u64 colour) {
    HudPacket_Rect(p, (HudBox){b.x0, b.y0, b.x1, b.y0 + 1}, colour);
    HudPacket_Rect(p, (HudBox){b.x0, b.y1 - 1, b.x1, b.y1}, colour);
    HudPacket_Rect(p, (HudBox){b.x0, b.y0 + 1, b.x0 + 1, b.y1 - 1}, colour);
    HudPacket_Rect(p, (HudBox){b.x1 - 1, b.y0 + 1, b.x1, b.y1 - 1}, colour);
}

void HudPacket_Texels(HudPacket *p, float u0, float v0, float u1, float v1, HudBox box, u64 colour) {
    HudPacket_Register(p, GS_PRIM, PRIM_SPRITE | PRIM_TME | PRIM_ABE | PRIM_FST);
    HudPacket_Register(p, GS_RGBAQ, colour);
    HudPacket_Register(p, GS_UV, uv(u0, v0));
    HudPacket_Register(p, GS_XYZ2, xyz(p, box.x0, box.y0));
    HudPacket_Register(p, GS_UV, uv(u1, v1));
    HudPacket_Register(p, GS_XYZ2, xyz(p, box.x1, box.y1));
}

/* a GIF IMAGE transfer to GS memory at `tbp` */
void HudPacket_Image(HudPacket *p, int tbp, int width, int height, const u32 *pixels) {
    int quadwords = width * height * 4 / 16;

    HudPacket_Register(p, GS_BITBLTBUF, ((u64)tbp << 32) | ((u64)(width / 64) << 48));
    HudPacket_Register(p, GS_TRXPOS, 0);
    HudPacket_Register(p, GS_TRXREG, (u64)width | ((u64)height << 32));
    HudPacket_Register(p, GS_TRXDIR, 0);
    p->tag = NULL;
    p->at[0] = (u64)quadwords | ((u64)2 << 58); /* FLG IMAGE */
    p->at[1] = 0;
    p->at += 2;
    memcpy(p->at, pixels, (size_t)quadwords * 16);
    p->at += quadwords * 2;
}

/* ---- gage.tx2 ----------------------------------------------------------------------------------------------------- */
/* hud_sprites[0]'s TEX0 with the CLUT always loaded (CLD 1): the HUD's sprites after it load their own */
#define GAGE_TEX0 ((0x4007DA85E1313640ull & ~(7ull << 61)) | (1ull << 61))
#define GAGE_CLAMP 5 /* CLAMP_1 as Hud_Draw draws it: clamped both ways (the bar's edge is the picture's) */

void HudPacket_UseGage(HudPacket *p) {
    HudPacket_Register(p, HUD_GS_TEXFLUSH, 0);
    HudPacket_Register(p, HUD_GS_TEX0_1, GAGE_TEX0);
    HudPacket_Register(p, HUD_GS_TEX1_1, 0x60); /* bilinear up and down */
    HudPacket_Register(p, HUD_GS_CLAMP_1, GAGE_CLAMP);
}

/* A piece of gage.tx2, in texels; u0 > u1 or v0 > v1 draws it mirrored */
typedef struct {
    float u0, v0, u1, v1;
} Piece;

/* the long bar's top right: its corner, its right side, its top; the other sides are these mirrored */
static const Piece edge_corner = {247, 20, 256, 29};
static const Piece edge_side = {247, 32, 256, 56};
static const Piece edge_top = {110, 20, 190, 29};

static Piece mirrored(Piece piece, int across, int down) {
    Piece m = piece;

    if (across) {
        m.u0 = piece.u1, m.u1 = piece.u0;
    }
    if (down) {
        m.v0 = piece.v1, m.v1 = piece.v0;
    }
    return m;
}

static void piece(HudPacket *p, Piece texels, float x0, float y0, float x1, float y1, u64 colour) {
    HudPacket_Texels(p, texels.u0, texels.v0, texels.u1, texels.v1, (HudBox){x0, y0, x1, y1}, colour);
}

void HudPacket_GageEdge(HudPacket *p, HudBox b, u64 colour) {
    float ix0 = b.x0 + HUD_GAGE_EDGE, iy0 = b.y0 + HUD_GAGE_EDGE, ix1 = b.x1 - HUD_GAGE_EDGE, iy1 = b.y1 - HUD_GAGE_EDGE;

    piece(p, mirrored(edge_corner, 1, 0), b.x0, b.y0, ix0, iy0, colour);
    piece(p, edge_corner, ix1, b.y0, b.x1, iy0, colour);
    piece(p, mirrored(edge_corner, 1, 1), b.x0, iy1, ix0, b.y1, colour);
    piece(p, mirrored(edge_corner, 0, 1), ix1, iy1, b.x1, b.y1, colour);
    piece(p, edge_top, ix0, b.y0, ix1, iy0, colour);
    piece(p, mirrored(edge_top, 0, 1), ix0, iy1, ix1, b.y1, colour);
    piece(p, mirrored(edge_side, 1, 0), b.x0, iy0, ix0, iy1, colour);
    piece(p, edge_side, ix1, iy0, b.x1, iy1, colour);
}

/* ---- The bars' fill ------------------------------------------------------------------------------------------------ */
/* hud_sprites[1]'s TEX0 (0x3780, 64 x 64 PSMT8), its CLUT always loaded */
#define BAR_TEX0 ((0x8007DC8599307780ull & ~(7ull << 61)) | (1ull << 61))

void HudPacket_UseBarFill(HudPacket *p) {
    HudPacket_Register(p, HUD_GS_TEXFLUSH, 0);
    HudPacket_Register(p, HUD_GS_TEX0_1, BAR_TEX0);
    HudPacket_Register(p, HUD_GS_TEX1_1, 0x60);
    HudPacket_Register(p, HUD_GS_CLAMP_1, 0); /* as Hud_Draw draws the bars */
}

/* ---- The font ------------------------------------------------------------------------------------------------------ */
/* font.tmr's second page, where its half-width glyphs are: 10 x 20 texels each, 50 a row from t 460 of 512 */
#define FONT_TEX0 0x4007FC2665423B40ull
#define GLYPH_COLUMNS 50
#define GLYPH_TOP 460
#define FONT_SIZE 512.0f

void HudPacket_QueueFont(void) {
    /* its page (0x3B40) and CLUTs (0x3FE0) are clear of gage.tx2 (0x3640 to 0x3800 and its CLUTs at 0x3ED4 and 0x3EE4),
       and the game's text sends it again before drawing from it */
    TextureTable_QueueAll((TextureTableHeader *)Message_GetFontTexture(menu_heap));
}

void HudPacket_UseFont(HudPacket *p) {
    HudPacket_Register(p, HUD_GS_TEXFLUSH, 0);
    HudPacket_Register(p, HUD_GS_TEX0_1, FONT_TEX0);
    HudPacket_Register(p, HUD_GS_TEX1_1, 0); /* as the game's text */
    HudPacket_Register(p, HUD_GS_CLAMP_1, 0);
}

static u64 st(float s, float t) {
    union {
        float f;
        u32 u;
    } a, b;

    a.f = s, b.f = t;
    return (u64)a.u | (u64)b.u << 32;
}

/* one half-width glyph (an ASCII character) with its top left at x, y */
static void glyph(HudPacket *p, char c, float x, float y, float size, u64 colour) {
    int index = c < 0x20 || c > 0x7E ? 0 : c - 0x20;
    float s0 = (float)(index % GLYPH_COLUMNS) * HUD_GLYPH_W / FONT_SIZE;
    float t0 = ((float)GLYPH_TOP + (float)(index / GLYPH_COLUMNS) * HUD_GLYPH_H) / FONT_SIZE;

    HudPacket_Register(p, GS_PRIM, PRIM_SPRITE | PRIM_TME | PRIM_ABE); /* ST, as the game's text */
    HudPacket_Register(p, GS_RGBAQ, colour);
    HudPacket_Register(p, GS_ST, st(s0, t0));
    HudPacket_Register(p, GS_XYZ2, xyz(p, x, y));
    HudPacket_Register(p, GS_ST, st(s0 + HUD_GLYPH_W / FONT_SIZE, t0 + HUD_GLYPH_H / FONT_SIZE));
    HudPacket_Register(p, GS_XYZ2, xyz(p, x + HUD_GLYPH_W * size, y + HUD_GLYPH_H * size));
}

float hud_text_width(const char *text, float size) {
    return (float)strlen(text) * HUD_GLYPH_W * size;
}

float HudPacket_Text(HudPacket *p, const char *text, float x, float y, float size, u64 colour) {
    u64 shadow = hud_rgba(0, 0, 0, (int)((colour >> 24) & 0xFF) * 3 / 4);
    float advance = HUD_GLYPH_W * size;
    int i;

    for (i = 0; text[i] != '\0'; i++) {
        float gx = x + (float)i * advance;

        if (text[i] != ' ') {
            glyph(p, text[i], gx + 1.0f, y + 1.0f, size, shadow);
            glyph(p, text[i], gx, y, size, colour);
        }
    }
    return hud_text_width(text, size);
}
