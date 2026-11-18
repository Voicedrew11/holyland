/* hands_cross.c: PC-only. The hands mod's cross (game/hands_cross.h): three cards at the bottom left of play,
   the spell on the left, the weapon on the right, the hotkey in use between them and lower, each with its item's
   own picture from the menus.

   The pictures: each item and spell has a menu model (menu/select2/iNNNN.mod by the item's file number,
   menu/select/mNNNN.mod by the spell's number), one square with a 256 x 128 picture on it, drawn square (the
   picture is stored at half height). A card reads its model from the disc when what it shows changes and keeps
   the picture at half size, 128 x 64, 32 KB.

   The drawing: after the game's HUD (Hud_Draw), in its packet chain, as one VIF DIRECT packet of GIF data, in two
   passes. First every card's frame, cut from the HUD's own frame picture (gage.tx2, still in GS memory from
   Hud_Draw): the stone and gold edge of its long bar for the sides and corners, over a dark inside the scene shows
   through. Then the pictures: each sent to GS memory where gage.tx2 is (the game sends gage.tx2 again before it draws
   the HUD each frame; nothing else draws from there) and drawn as a textured sprite before the next is sent. GS
   context 1, as the HUD: alpha blending (Cs - Cd) * As + Cd and the HUD's test (no Z written). Laid out in its own
   units and placed in the HUD's 640 x 448 by the player's corner and size (port_hands.corner, .scale); marked for
   --widescreen as the HUD against the side it keeps to, so it stays at the 16:9 picture's edge.

   The cross fades to a quarter after a few seconds with no hand key pressed (port_hands.fade); a card whose
   contents changed glows warm, and one whose key did nothing flashes red. Every card, empty or not, is drawn at the
   cross's alpha. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "gfx/texture.h"
#include "item/item.h"
#include "map/map.h"
#include "player/player.h"
#include "port/hands.h"
#include "port/host.h"
#include "port/platform.h"
#include "hands_cross.h"

extern PlayerState player_state;
extern u8 item_counts[ITEM_COUNT];
extern LevelDef item_name_defs[ITEM_COUNT];
extern char game_options[0x10];
extern MapDrawState map_draw_state;
extern int packet_quadwords_used; /* gfx/packet_buffer.c */
extern int packet_buffers_size;
u32 *Packet_Alloc(int quadwords);
void Packet_AppendToChain(u32 *packet, u32 *packet_end);
void GsDraw_QueueFlush(void);
void TextureTable_QueueAll(TextureTableHeader *table);
extern sptr menu_heap;
sptr Message_GetFontTexture(sptr font_base); /* ui/message.c: font.tmr, the game's font */
void Widescreen_Mark(int what);

/* ---- The layout, in the HUD's 640 x 448 (Hud_InitSprite: x + 1728, y + 1824 in GS pixels) ---------------- */
#define GS_X0 1728
#define GS_Y0 1824
#define CARD_W 70        /* three cards and their gaps: the HUD frame's width (218) */
#define CARD_GAP 3
#define CROSS_W (3 * CARD_W + 2 * CARD_GAP)
#define SIDE_MARGIN 18   /* under the HUD's ring (its frame is drawn 16 in, the ring's edge a little further) */
#define END_MARGIN 16    /* the HUD's frame starts 16 from the top: so 16 from the top or bottom */
#define HUD_BOTTOM 106   /* the HUD's frame: 128 texels drawn at 0.7 down, from 16 */

/* Each corner: which edges the cross keeps to, how far from the top when at the top, and how widescreen moves it */
typedef struct {
    int right;     /* against the right edge, else the left */
    int bottom;    /* against the bottom, else at `top` */
    float top;
    int widescreen; /* PORT_WIDESCREEN_HUD_LEFT or _RIGHT */
} CornerPlace;

static const CornerPlace corner_places[PORT_HANDS_CORNER_COUNT] = {
    [PORT_HANDS_BOTTOM_LEFT] = {0, 1, 0, PORT_WIDESCREEN_HUD_LEFT},
    [PORT_HANDS_BOTTOM_RIGHT] = {1, 1, 0, PORT_WIDESCREEN_HUD_RIGHT},
    [PORT_HANDS_TOP_RIGHT] = {1, 0, END_MARGIN, PORT_WIDESCREEN_HUD_RIGHT},
    [PORT_HANDS_TOP_LEFT] = {0, 0, HUD_BOTTOM + 8, PORT_WIDESCREEN_HUD_LEFT},
};

/* each card shape's height (CARD_W wide): square is about square on the screen, whose pixels are a little narrow */
static const float card_heights[PORT_HANDS_CARDS_COUNT] = {
    [PORT_HANDS_CARDS_TALL] = 80,
    [PORT_HANDS_CARDS_SQUARE] = 70,
};

/* each layout's drop of the hotkey's card, the middle one, below the other two */
static const float middle_drops[PORT_HANDS_LAYOUT_COUNT] = {
    [PORT_HANDS_LAYOUT_CROSS] = 24,
    [PORT_HANDS_LAYOUT_ROW] = 0,
};

enum { CARD_SPELL, CARD_HOTKEY, CARD_WEAPON, CARD_COUNT }; /* left to right */

static const HandKind card_hands[CARD_COUNT] = {
    [CARD_SPELL] = HAND_SPELL,
    [CARD_HOTKEY] = HAND_HOTKEY,
    [CARD_WEAPON] = HAND_WEAPON,
};

/* ---- The timing, in vblanks (60 a second) -------------------------------------------------------------- */
#define IDLE_VBLANKS 240   /* 4 s with no hand key pressed, then */
#define FADE_VBLANKS 48    /* 0.8 s down to */
#define FADED 0.25f        /* a quarter */
#define PULSE_VBLANKS 9    /* a changed card pulses for 150 ms */
#define FLASH_VBLANKS 15   /* a refused one flashes for 250 ms */
#define INSIDE_ALPHA 0x68  /* a card's dark inside, of the cross's alpha (0x80 opaque): about 80%, so lava and
                              torchlight behind it do not show through as colour */

/* ---- The pictures --------------------------------------------------------------------------------------- */
#define PICTURE_W 128 /* the model's 256 x 128 at half size */
#define MARGIN 0.08f  /* of the picture left out on each side */
#define PICTURE_H 64
#define MODEL_FILE_MAX 0x25800      /* the menu's read buffer (Menu_OpenModelFile) */
/* GS memory: the HUD's frame picture (gage.tx2, hud_sprites[0]: TBP0 0x3640, 256 x 128 PSMT8, 32 KB), a page boundary;
   each card's picture goes there in turn, drawn before the next is sent (the GS does them in order) */
#define PICTURE_TBP 0x3640

/* ---- The frame: pieces of gage.tx2 (hud_sprites[0]: 256 x 128 PSMT8, its CLUT with it) -------------------- */
/* hud_sprites[0]'s TEX0 with the CLUT always loaded (CLD 1): the HUD's sprites after it load their own */
#define GAGE_TEX0 ((0x4007DA85E1313640ull & ~(7ull << 61)) | (1ull << 61))
#define GAGE_GREY 0x5A    /* hud_sprites[0]'s colour */
#define GAGE_CLAMP 5      /* CLAMP_1 as Hud_Draw draws it: clamped both ways (the bar's edge is the picture's) */
#define EDGE_TEXELS 9     /* the bar's edge: stone, then a gold line three texels wide */
#define TEXEL_PX 0.8f     /* the HUD's frame is drawn at 0.85 across, 0.7 down */
#define EDGE_PX (EDGE_TEXELS * TEXEL_PX)

/* A piece of gage.tx2, in texels; u0 > u1 or v0 > v1 draws it mirrored */
typedef struct {
    float u0, v0, u1, v1;
} Piece;

/* the long bar's top right: its corner, its right side, its top; the other sides are these mirrored */
static const Piece edge_corner = {247, 20, 256, 29};
static const Piece edge_side = {247, 32, 256, 56};
static const Piece edge_top = {110, 20, 190, 29};

static Piece mirrored(Piece p, int across, int down) {
    Piece m = p;

    if (across) {
        m.u0 = p.u1, m.u1 = p.u0;
    }
    if (down) {
        m.v0 = p.v1, m.v1 = p.v0;
    }
    return m;
}

typedef struct {
    int key;                               /* what it shows: the model's file (MODEL_KEY), 0 nothing */
    int loaded;                            /* the picture was read */
    u32 pixels[PICTURE_W * PICTURE_H];     /* RGBA, the GS's alpha (0x80 opaque) */
} CardPicture;

static CardPicture pictures[CARD_COUNT];

#define MODEL_KEY(spell, number) (((spell) ? 0x10000 : 0x20000) | (number))

/* The model of what the card shows: its key, 0 nothing */
static int model_key(HandKind kind) {
    int value = port_hands_state.hands[kind].slots[port_hands_state.hands[kind].out];

    switch (kind) {
    case HAND_WEAPON:
        return value > 0 && value < 70 ? MODEL_KEY(0, item_name_defs[ITEM_WEAPON_FIRST + value].file_number) : 0;
    case HAND_SPELL:
        return value > 0 && value < 91 ? MODEL_KEY(1, value) : 0;
    default:
        if (HOTKEY_TYPE(value) == HOTKEY_SPELL) {
            return MODEL_KEY(1, HOTKEY_NUMBER(value));
        }
        return value != 0 ? MODEL_KEY(0, item_name_defs[HOTKEY_NUMBER(value)].file_number) : 0;
    }
}

static u32 read_u32(const unsigned char *at) {
    return (u32)at[0] | (u32)at[1] << 8 | (u32)at[2] << 16 | (u32)at[3] << 24;
}

/* The picture of a model file read whole: the TextureHeader at the file's second offset (word 3), its TEX0 a
   256 x 128 PSMCT32 image right after it. 0 when the file is not one. */
static int take_picture(const unsigned char *file, size_t size, u32 *pixels) {
    size_t texture;
    u32 tex0;
    int x, y;

    if (size < 0x10) {
        return 0;
    }
    texture = read_u32(file + 0x0C);
    if (texture > size || size - texture < 0x20 + 256u * 128u * 4u) {
        return 0;
    }
    tex0 = read_u32(file + texture + 0x10);
    if (((tex0 >> 20) & 0x3F) != 0 || ((tex0 >> 26) & 0xF) != 8 || (tex0 >> 30) != 3 || (read_u32(file + texture + 0x14) & 3) != 1) {
        return 0; /* not PSMCT32 (PSM 0), 256 (TW 8) by 128 (TH 7: bits 30 and 31 of the low word, 32 and 33) */
    }
    file += texture + 0x20;
    /* each 2 x 2 of the picture as one: the average, the alpha with it */
    for (y = 0; y < PICTURE_H; y++) {
        for (x = 0; x < PICTURE_W; x++) {
            const unsigned char *a = file + ((size_t)(y * 2) * 256 + (size_t)x * 2) * 4;
            const unsigned char *b = a + 256 * 4;
            u32 out = 0;
            int c;

            for (c = 0; c < 4; c++) {
                u32 sum = (u32)a[c] + a[c + 4] + b[c] + b[c + 4];

                out |= ((sum + 2) / 4) << (c * 8);
            }
            pixels[y * PICTURE_W + x] = out;
        }
    }
    return 1;
}

/* The card's picture from the disc (port_disc_read takes the disc's lock, so it can come between the game's own
   reads); 0 when there is none */
static int read_picture(int key, u32 *pixels) {
    char name[32];
    uint32_t lsn, size;
    unsigned char *file;
    int ok;

    if (key & 0x10000) {
        snprintf(name, sizeof(name), "menu/select/m%04d.mod", key & 0xFFFF);
    } else {
        snprintf(name, sizeof(name), "menu/select2/i%04d.mod", key & 0xFFFF);
    }
    if (!port_disc_find_member(name, &lsn, &size) || size == 0 || size > MODEL_FILE_MAX) {
        port_event("hands: no picture %s", name);
        return 0;
    }
    file = malloc(((size_t)size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE * DISC_SECTOR_SIZE);
    if (file == NULL) {
        return 0;
    }
    ok = port_disc_read(lsn, (size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE, file) && take_picture(file, size, pixels);
    free(file);
    if (!ok) {
        port_event("hands: %s is not a picture", name);
    }
    return ok;
}

static void update_pictures(void) {
    int card;

    for (card = 0; card < CARD_COUNT; card++) {
        CardPicture *p = &pictures[card];
        int key = model_key(card_hands[card]);

        if (key != p->key) {
            p->key = key;
            p->loaded = key != 0 && read_picture(key, p->pixels);
        }
    }
}

/* ---- The GIF data -------------------------------------------------------------------------------------------- */
#define GS_PRIM 0x00
#define GS_RGBAQ 0x01
#define GS_ST 0x02
#define GS_UV 0x03
#define GS_XYZ2 0x05
#define GS_TEX0_1 0x06
#define GS_TEX1_1 0x14
#define GS_CLAMP_1 0x08
#define GS_TEXFLUSH 0x3F
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

typedef struct {
    u64 *at;
    u64 *end;
    u64 *tag;  /* the A+D tag being filled (NULL: none) */
} Gif;

/* An A+D register write; the tag in front of a run of them counts them */
static void gif_ad(Gif *g, int reg, u64 value) {
    if (g->tag == NULL) {
        g->tag = g->at;
        g->tag[0] = 0;
        g->tag[1] = 0xE; /* REGS: A+D */
        g->at += 2;
    }
    g->at[0] = value;
    g->at[1] = (u64)reg;
    g->at += 2;
    g->tag[0] += 1;                     /* NLOOP */
    g->tag[0] |= (u64)1 << 60;          /* NREG 1 */
}

/* The picture as a GIF IMAGE transfer to GS memory at `tbp`, 128 wide (BW 2) */
static void gif_picture(Gif *g, int tbp, const u32 *pixels) {
    int quadwords = PICTURE_W * PICTURE_H * 4 / 16;

    gif_ad(g, GS_BITBLTBUF, ((u64)tbp << 32) | ((u64)(PICTURE_W / 64) << 48));
    gif_ad(g, GS_TRXPOS, 0);
    gif_ad(g, GS_TRXREG, (u64)PICTURE_W | ((u64)PICTURE_H << 32));
    gif_ad(g, GS_TRXDIR, 0);
    g->tag = NULL;
    g->at[0] = (u64)quadwords | ((u64)2 << 58); /* FLG IMAGE */
    g->at[1] = 0;
    g->at += 2;
    memcpy(g->at, pixels, (size_t)quadwords * 16);
    g->at += quadwords * 2;
}

static u64 rgba(int r, int g, int b, int a) {
    return (u64)(r & 0xFF) | (u64)(g & 0xFF) << 8 | (u64)(b & 0xFF) << 16 | (u64)(a & 0xFF) << 24 | (u64)0x3F800000 << 32;
}

/* Where this frame's cross is and its shape: its top left in the HUD's 640 x 448, its scale, its cards' height
   and the middle card's drop. Everything below is laid out in the cross's own units (CROSS_W wide, card_h + drop
   high, from its top left) and placed by xyz. */
typedef struct {
    float x, y, scale;
    float card_h, drop;
} Place;

static Place place = {SIDE_MARGIN, 448 - END_MARGIN - 104, 1.0f, 80, 24};

static Place place_of(const PortHandsOptions *o) {
    const CornerPlace *c = &corner_places[o->corner];
    Place p;

    p.scale = o->scale;
    p.card_h = card_heights[o->cards];
    p.drop = middle_drops[o->layout];
    p.x = c->right ? 640 - SIDE_MARGIN - CROSS_W * p.scale : SIDE_MARGIN;
    p.y = c->bottom ? 448 - END_MARGIN - (p.card_h + p.drop) * p.scale : c->top;
    return p;
}

/* a point of the cross, in its own units, as the GS's XYZ2 */
static u64 xyz(float x, float y) {
    u64 gx = (u64)(int)((GS_X0 + place.x + x * place.scale) * 16.0f + 0.5f) & 0xFFFF;
    u64 gy = (u64)(int)((GS_Y0 + place.y + y * place.scale) * 16.0f + 0.5f) & 0xFFFF;

    return gx | gy << 16;
}

static void gif_rect(Gif *g, float x0, float y0, float x1, float y1, u64 colour) {
    gif_ad(g, GS_PRIM, PRIM_SPRITE | PRIM_ABE);
    gif_ad(g, GS_RGBAQ, colour);
    gif_ad(g, GS_XYZ2, xyz(x0, y0));
    gif_ad(g, GS_XYZ2, xyz(x1, y1));
}

/* a frame one pixel wide inside the box */
static void gif_frame(Gif *g, float x0, float y0, float x1, float y1, u64 colour) {
    gif_rect(g, x0, y0, x1, y0 + 1, colour);
    gif_rect(g, x0, y1 - 1, x1, y1, colour);
    gif_rect(g, x0, y0 + 1, x0 + 1, y1 - 1, colour);
    gif_rect(g, x1 - 1, y0 + 1, x1, y1 - 1, colour);
}

/* ---- The cards ------------------------------------------------------------------------------------------------ */
typedef struct {
    float x0, y0, x1, y1;
} Box;

static Box card_box(int card) {
    Box box;

    box.x0 = (float)(card * (CARD_W + CARD_GAP));
    box.x1 = box.x0 + CARD_W;
    box.y0 = card == CARD_HOTKEY ? place.drop : 0.0f;
    box.y1 = box.y0 + place.card_h;
    return box;
}

/* 0 to 1: how far into `length` vblanks since `at` it is, 1 when past or never */
static float since(unsigned long long at, unsigned long long length) {
    unsigned long long now = port_vblank_count();

    if (at == 0 || now < at || now - at >= length) {
        return 1.0f;
    }
    return (float)(now - at) / (float)length;
}

/* the cross's fade: 1, down to FADED after a while with nothing pressed */
static float cross_alpha(void) {
    unsigned long long now = port_vblank_count();
    unsigned long long last = hands_feedback.pressed_vblank;
    int card;
    float t;

    for (card = 0; card < CARD_COUNT; card++) {
        if (hands_feedback.changed_vblank[card_hands[card]] > last) {
            last = hands_feedback.changed_vblank[card_hands[card]];
        }
    }
    if (!port_hands.fade || last == 0 || now < last + IDLE_VBLANKS) {
        return last == 0 && port_hands.fade && now > IDLE_VBLANKS ? FADED : 1.0f;
    }
    t = since(last + IDLE_VBLANKS, FADE_VBLANKS);
    return 1.0f - (1.0f - FADED) * t;
}

/* a piece of gage.tx2 drawn over x0, y0 to x1, y1 (GAGE_TEX0 set) */
static void gif_piece(Gif *g, Piece p, float x0, float y0, float x1, float y1, u64 colour) {
    gif_ad(g, GS_PRIM, PRIM_SPRITE | PRIM_TME | PRIM_ABE | PRIM_FST);
    gif_ad(g, GS_RGBAQ, colour);
    gif_ad(g, GS_UV, (u64)(int)(p.u0 * 16.0f) | (u64)(int)(p.v0 * 16.0f) << 16);
    gif_ad(g, GS_XYZ2, xyz(x0, y0));
    gif_ad(g, GS_UV, (u64)(int)(p.u1 * 16.0f) | (u64)(int)(p.v1 * 16.0f) << 16);
    gif_ad(g, GS_XYZ2, xyz(x1, y1));
}

/* the card's box, one larger while it pulses */
static Box drawn_box(int card) {
    Box b = card_box(card);
    float grow = since(hands_feedback.changed_vblank[card_hands[card]], PULSE_VBLANKS) < 1.0f ? 1.0f : 0.0f;

    b.x0 -= grow, b.y0 -= grow, b.x1 += grow, b.y1 += grow;
    return b;
}

/* The card's frame: a dark inside the scene shows through, the bar's stone and gold edge round it, warmer while
   it pulses, red while refused. GAGE_TEX0 set. */
static void draw_card_frame(Gif *g, int card, float alpha) {
    HandKind kind = card_hands[card];
    Box b = drawn_box(card);
    float pulse = 1.0f - since(hands_feedback.changed_vblank[kind], PULSE_VBLANKS);
    float flash = 1.0f - since(hands_feedback.refused_vblank[kind], FLASH_VBLANKS);
    int a = (int)(0x80 * alpha);
    u64 stone = rgba(GAGE_GREY + (int)(0x38 * pulse), GAGE_GREY + (int)(0x1C * pulse), GAGE_GREY, a);
    float ix0 = b.x0 + EDGE_PX, iy0 = b.y0 + EDGE_PX, ix1 = b.x1 - EDGE_PX, iy1 = b.y1 - EDGE_PX;

    gif_rect(g, ix0, iy0, ix1, iy1, rgba(0x08, 0x07, 0x06, a * INSIDE_ALPHA / 0x80));
    gif_piece(g, mirrored(edge_corner, 1, 0), b.x0, b.y0, ix0, iy0, stone);
    gif_piece(g, edge_corner, ix1, b.y0, b.x1, iy0, stone);
    gif_piece(g, mirrored(edge_corner, 1, 1), b.x0, iy1, ix0, b.y1, stone);
    gif_piece(g, mirrored(edge_corner, 0, 1), ix1, iy1, b.x1, b.y1, stone);
    gif_piece(g, edge_top, ix0, b.y0, ix1, iy0, stone);
    gif_piece(g, mirrored(edge_top, 0, 1), ix0, iy1, ix1, b.y1, stone);
    gif_piece(g, mirrored(edge_side, 1, 0), b.x0, iy0, ix0, iy1, stone);
    gif_piece(g, edge_side, ix1, iy0, b.x1, iy1, stone);
    if (flash > 0.0f) {
        /* over the gold line, the inner third of the edge */
        float in = EDGE_PX / 3.0f;

        gif_frame(g, ix0 - in, iy0 - in, ix1 + in, iy1 + in, rgba(0xC8, 0x20, 0x18, (int)(a * flash)));
        gif_frame(g, ix0 - in + 1, iy0 - in + 1, ix1 + in - 1, iy1 + in - 1, rgba(0xC8, 0x20, 0x18, (int)(a * flash)));
    }
}

/* The card's picture, square (it is stored at half height), as large as fits inside the frame */
static void draw_card_picture(Gif *g, int card, float alpha) {
    CardPicture *p = &pictures[card];
    Box b = drawn_box(card);
    float pulse = 1.0f - since(hands_feedback.changed_vblank[card_hands[card]], PULSE_VBLANKS);
    float side = (b.x1 - b.x0) - 2.0f * EDGE_PX;
    float x0, y0;
    int bright = 0x80 + (int)(0x30 * pulse);

    if (!p->loaded) {
        return;
    }
    if (b.y1 - b.y0 - 2.0f * EDGE_PX < side) {
        side = b.y1 - b.y0 - 2.0f * EDGE_PX;
    }
    x0 = (b.x0 + b.x1 - side) * 0.5f;
    y0 = (b.y0 + b.y1 - side) * 0.5f;
    gif_picture(g, PICTURE_TBP, p->pixels);
    gif_ad(g, GS_TEXFLUSH, 0);
    /* TEX0: TBP0, TBW 2, PSMCT32, TW 7 (128), TH 6 (64), TCC (RGBA), TFX modulate */
    gif_ad(g, GS_TEX0_1, (u64)PICTURE_TBP | (u64)(PICTURE_W / 64) << 14 | (u64)7 << 26 | (u64)6 << 30 | (u64)1 << 34);
    gif_ad(g, GS_PRIM, PRIM_SPRITE | PRIM_TME | PRIM_ABE | PRIM_FST);
    gif_ad(g, GS_RGBAQ, rgba(bright, bright, bright, (int)(0x80 * alpha)));
    /* the middle of the picture: the pictures leave a margin round the item, which the card's edge does already */
    gif_ad(g, GS_UV, (u64)(PICTURE_W * 16 * MARGIN) | (u64)(PICTURE_H * 16 * MARGIN) << 16);
    gif_ad(g, GS_XYZ2, xyz(x0, y0));
    gif_ad(g, GS_UV, (u64)(PICTURE_W * 16 * (1 - MARGIN)) | (u64)(PICTURE_H * 16 * (1 - MARGIN)) << 16);
    gif_ad(g, GS_XYZ2, xyz(x0 + side, y0 + side));
}

/* the quadwords the cross's packet can take at most: the frames, and the pictures with their registers */
/* each card's frame: its inside and 8 pieces, 6 registers each at most, and the flash's two frames of 4 rectangles
   of 4 */
#define FRAME_QUADWORDS (CARD_COUNT * (9 * 6 + 3 * 4 * 4) + 16)
#define PICTURE_QUADWORDS (PICTURE_W * PICTURE_H * 4 / 16 + 16)
#define CROSS_QUADWORDS (FRAME_QUADWORDS + CARD_COUNT * PICTURE_QUADWORDS + COUNT_QUADWORDS + 8)

/* ---- The count: the hotkey's items carried, in the game's own font (ui/message.c, Message_DrawGlyph) ---------- */
/* font.tmr's second page, where its half-width glyphs are: 10 x 20 texels each, 50 a row from t 460 of 512 */
#define FONT_TEX0 0x4007FC2665423B40ull
#define GLYPH_W 10
#define GLYPH_H 20
#define GLYPH_COLUMNS 50
#define GLYPH_TOP 460
#define FONT_SIZE 512.0f
#define COUNT_DIGITS 4
/* glyphs and their shadows, 6 registers each, and the registers round them */
#define COUNT_QUADWORDS (2 * COUNT_DIGITS * 6 + 8)

/* the hotkey's count, -1 none to show (a spell, an empty hotkey) */
static int hotkey_count(void) {
    const Hand *hand = &port_hands_state.hands[HAND_HOTKEY];
    int value = hand->slots[hand->out];

    return HOTKEY_TYPE(value) == HOTKEY_ITEM ? item_counts[HOTKEY_NUMBER(value)] : -1;
}

/* one half-width glyph of the font (an ASCII character) with its top left at x, y (FONT_TEX0 set) */
static void gif_glyph(Gif *g, char c, float x, float y, u64 colour) {
    int index = c - 0x20;
    float s0 = (float)(index % GLYPH_COLUMNS * GLYPH_W) / FONT_SIZE;
    float t0 = (float)(GLYPH_TOP + index / GLYPH_COLUMNS * GLYPH_H) / FONT_SIZE;
    float s1 = s0 + GLYPH_W / FONT_SIZE, t1 = t0 + GLYPH_H / FONT_SIZE;
    union {
        float f;
        u32 u;
    } a, b;

    gif_ad(g, GS_PRIM, PRIM_SPRITE | PRIM_TME | PRIM_ABE); /* ST, as the game's text */
    gif_ad(g, GS_RGBAQ, colour);
    a.f = s0, b.f = t0;
    gif_ad(g, GS_ST, (u64)a.u | (u64)b.u << 32);
    gif_ad(g, GS_XYZ2, xyz(x, y));
    a.f = s1, b.f = t1;
    gif_ad(g, GS_ST, (u64)a.u | (u64)b.u << 32);
    gif_ad(g, GS_XYZ2, xyz(x + GLYPH_W, y + GLYPH_H));
}

/* The count at the bottom right inside the hotkey's card, white with a dark shadow as over the scene. The font is
   in GS memory (Hands_DrawCross queued it). */
static void draw_count(Gif *g, int count, float alpha) {
    Box b = drawn_box(CARD_HOTKEY);
    char digits[COUNT_DIGITS + 1];
    int a = (int)(0x80 * alpha);
    int n = snprintf(digits, sizeof(digits), "%d", count);
    float x = b.x1 - EDGE_PX - 1.0f - (float)(n * GLYPH_W);
    float y = b.y1 - EDGE_PX - (float)GLYPH_H; /* the digits reach the cell's bottom: clear of the gold line */
    int i;

    gif_ad(g, GS_TEXFLUSH, 0);
    gif_ad(g, GS_TEX0_1, FONT_TEX0);
    gif_ad(g, GS_TEX1_1, 0); /* as the game's text */
    gif_ad(g, GS_CLAMP_1, 0);
    for (i = 0; i < n; i++) {
        gif_glyph(g, digits[i], x + (float)(i * GLYPH_W) + 1.0f, y + 1.0f, rgba(0, 0, 0, a * 3 / 4));
        gif_glyph(g, digits[i], x + (float)(i * GLYPH_W), y, rgba(0x80, 0x80, 0x80, a));
    }
}

void Hands_DrawCross(void) {
    u32 *start;
    Gif g;
    float alpha;
    int quadwords;
    int card;
    int count;

    if (!port_hands.enabled || game_options[9] == 0 || map_draw_state.hud_shown == 0) {
        return;
    }
    update_pictures();
    /* a packet buffer that could not take the cross leaves it out (the game's would overflow, and hang) */
    if (packet_quadwords_used + CROSS_QUADWORDS + 64 > packet_buffers_size / 16) {
        return;
    }
    alpha = cross_alpha();
    count = hotkey_count();
    place = place_of(&port_hands);
    Widescreen_Mark(corner_places[port_hands.corner].widescreen);
    GsDraw_QueueFlush(); /* the VU1's own GIF packets out before ours (as TextureTable_QueueAll does) */
    if (count >= 0) {
        /* the font to GS memory, as Message_Draw sends it: its page (0x3B40) and CLUTs (0x3FE0) are clear of
           gage.tx2 and the pictures (0x3640), and the game's text sends it again before drawing from it */
        TextureTable_QueueAll((TextureTableHeader *)Message_GetFontTexture(menu_heap));
    }
    /* a DMA tag, a quadword of VIF codes (NOP, NOP, NOP, DIRECT: as the game's own packets, the VIF codes in the data),
       the GIF data, and a quadword for the chain's next tag */
    start = Packet_Alloc(2 + CROSS_QUADWORDS + 1);
    g.at = (u64 *)(start + 8);
    g.end = g.at + CROSS_QUADWORDS * 2;
    g.tag = NULL;
    gif_ad(&g, GS_ALPHA_1, 0x44); /* (Cs - Cd) * As + Cd */
    gif_ad(&g, GS_TEST_1, 0x31001); /* the HUD's: no Z written */
    /* the frames first, while gage.tx2 is in GS memory; the pictures go over it */
    gif_ad(&g, GS_TEXFLUSH, 0);
    gif_ad(&g, GS_TEX0_1, GAGE_TEX0);
    gif_ad(&g, GS_TEX1_1, 0x60); /* bilinear up and down */
    gif_ad(&g, GS_CLAMP_1, GAGE_CLAMP);
    for (card = 0; card < CARD_COUNT; card++) {
        draw_card_frame(&g, card, alpha);
    }
    for (card = 0; card < CARD_COUNT; card++) {
        draw_card_picture(&g, card, alpha);
    }
    if (count >= 0) {
        draw_count(&g, count, alpha);
    }
    gif_ad(&g, GS_CLAMP_1, 0);    /* as Hud_Draw leaves them */
    gif_ad(&g, GS_ALPHA_1, 0x48);
    g.tag[0] |= (u64)1 << 15; /* EOP */
    quadwords = (int)(g.at - (u64 *)(start + 8)) / 2;
    if (g.at > g.end) {
        port_event("hands: the cross's packet ran over (%d quadwords)", quadwords); /* (cannot be: CROSS_QUADWORDS) */
    }
    start[0] = 0x10000000u | (u32)(quadwords + 1); /* DMA cnt */
    start[1] = start[2] = start[3] = 0;
    start[4] = start[5] = start[6] = 0;             /* VIF NOP */
    start[7] = 0x50000000u | (u32)quadwords;        /* VIF DIRECT */
    Packet_AppendToChain(start, start + 8 + quadwords * 4);
    Widescreen_Mark(PORT_WIDESCREEN_3D);
}
