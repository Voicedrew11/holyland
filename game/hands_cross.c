/* hands_cross.c: PC-only. The hands mod's cross (game/hands_cross.h): three cards at the bottom left of play,
   the spell on the left, the weapon on the right, the hotkey in use between them and lower, each with its item's
   own picture from the menus.

   The pictures: each item and spell has a menu model (menu/select2/iNNNN.mod by the item's file number,
   menu/select/mNNNN.mod by the spell's number), one square with a 256 x 128 picture on it, drawn square (the
   picture is stored at half height). A card reads its model from the disc when what it shows changes and keeps
   the picture at half size, 128 x 64, 32 KB.

   The drawing: after the game's HUD (Hud_Draw), in its packet chain, as one VIF DIRECT packet of GIF data: each
   card's frame as flat sprites, then its picture sent to GS memory and drawn as a textured sprite. The pictures go
   where the HUD's frame picture (gage.tx2) is, which the game sends again before it draws the HUD each frame; nothing
   else draws from there. Each card's picture is sent there and drawn before the next card's is sent. GS context 1, as the HUD: alpha blending (Cs - Cd) * As + Cd and the HUD's test (no
   Z written). Laid out in the HUD's 640 x 448 and marked as the HUD for --widescreen, so it moves with the HUD.

   The cross fades to a quarter after a few seconds with no hand key pressed (port_hands.fade); a card whose
   contents changed pulses, and one whose key did nothing flashes red. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
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
void Widescreen_Mark(int what);

/* ---- The layout, in the HUD's 640 x 448 (Hud_InitSprite: x + 1728, y + 1824 in GS pixels) ---------------- */
#define GS_X0 1728
#define GS_Y0 1824
#define CARD_W 52
#define CARD_H 60
#define CARD_GAP 3
#define CROSS_LEFT 16    /* the HUD's frame starts 16 in */
#define CROSS_BOTTOM 432 /* and 16 from the top: so 16 from the bottom */
#define MIDDLE_DROP 24   /* the hotkey's card is this much lower */

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

/* ---- The pictures --------------------------------------------------------------------------------------- */
#define PICTURE_W 128 /* the model's 256 x 128 at half size */
#define MARGIN 0.08f  /* of the picture left out on each side */
#define PICTURE_H 64
#define MODEL_FILE_MAX 0x25800      /* the menu's read buffer (Menu_OpenModelFile) */
/* GS memory: the HUD's frame picture (gage.tx2, hud_sprites[0]: TBP0 0x3640, 256 x 128 PSMT8, 32 KB), a page boundary;
   each card's picture goes there in turn, drawn before the next is sent (the GS does them in order) */
#define PICTURE_TBP 0x3640

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
#define GS_UV 0x03
#define GS_XYZ2 0x05
#define GS_TEX0_1 0x06
#define GS_TEX1_1 0x14
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

static u64 xyz(float x, float y) {
    u64 gx = (u64)(int)((GS_X0 + x) * 16.0f + 0.5f) & 0xFFFF;
    u64 gy = (u64)(int)((GS_Y0 + y) * 16.0f + 0.5f) & 0xFFFF;

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

    box.x0 = CROSS_LEFT + card * (CARD_W + CARD_GAP);
    box.x1 = box.x0 + CARD_W;
    box.y1 = CROSS_BOTTOM - (card == CARD_HOTKEY ? 0 : MIDDLE_DROP);
    box.y0 = box.y1 - CARD_H;
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

static void draw_card(Gif *g, int card, float alpha) {
    HandKind kind = card_hands[card];
    CardPicture *p = &pictures[card];
    Box b = card_box(card);
    float pulse = 1.0f - since(hands_feedback.changed_vblank[kind], PULSE_VBLANKS);
    float flash = 1.0f - since(hands_feedback.refused_vblank[kind], FLASH_VBLANKS);
    float grow = pulse > 0.0f ? 1.0f : 0.0f;
    int a = (int)(0x80 * alpha);
    int lift = (int)(0x30 * pulse);
    float side;

    b.x0 -= grow, b.y0 -= grow, b.x1 += grow, b.y1 += grow;
    /* the card: dark, a light edge, a dark line round it; red while refused */
    gif_rect(g, b.x0 + 1, b.y0 + 1, b.x1 - 1, b.y1 - 1, rgba(0x08 + lift / 2, 0x0A + lift / 2, 0x10 + lift / 2, a * 5 / 8));
    gif_frame(g, b.x0, b.y0, b.x1, b.y1, rgba(0, 0, 0, a * 3 / 4));
    if (flash > 0.0f) {
        gif_frame(g, b.x0 + 1, b.y0 + 1, b.x1 - 1, b.y1 - 1, rgba(0xC8, 0x20, 0x18, a));
    } else {
        gif_frame(g, b.x0 + 1, b.y0 + 1, b.x1 - 1, b.y1 - 1, rgba(0x70 + lift, 0x78 + lift, 0x88 + lift, a * 3 / 4));
    }
    if (!p->loaded) {
        return;
    }
    /* the picture, square (it is stored at half height), as large as fits inside the edge */
    side = (b.x1 - b.x0) - 4.0f;
    if (b.y1 - b.y0 - 4.0f < side) {
        side = b.y1 - b.y0 - 4.0f;
    }
    {
        float x0 = (b.x0 + b.x1 - side) * 0.5f, y0 = (b.y0 + b.y1 - side) * 0.5f;
        int tbp = PICTURE_TBP;
        int bright = 0x80 + lift;

        gif_picture(g, tbp, p->pixels);
        gif_ad(g, GS_TEXFLUSH, 0);
        /* TEX0: TBP0, TBW 2, PSMCT32, TW 7 (128), TH 6 (64), TCC (RGBA), TFX modulate */
        gif_ad(g, GS_TEX0_1, (u64)tbp | (u64)(PICTURE_W / 64) << 14 | (u64)7 << 26 | (u64)6 << 30 | (u64)1 << 34);
        gif_ad(g, GS_TEX1_1, 0x60); /* bilinear up and down */
        gif_ad(g, GS_PRIM, PRIM_SPRITE | PRIM_TME | PRIM_ABE | PRIM_FST);
        gif_ad(g, GS_RGBAQ, rgba(bright, bright, bright, a));
        /* the middle of the picture: the pictures leave a margin round the item, which the card's edge does already */
        gif_ad(g, GS_UV, (u64)(PICTURE_W * 16 * MARGIN) | (u64)(PICTURE_H * 16 * MARGIN) << 16);
        gif_ad(g, GS_XYZ2, xyz(x0, y0));
        gif_ad(g, GS_UV, (u64)(PICTURE_W * 16 * (1 - MARGIN)) | (u64)(PICTURE_H * 16 * (1 - MARGIN)) << 16);
        gif_ad(g, GS_XYZ2, xyz(x0 + side, y0 + side));
    }
}

/* the quadwords the cross's packet can take at most: the frames, and the pictures with their registers */
#define FRAME_QUADWORDS (CARD_COUNT * (1 + 9 * 4) + 8)
#define PICTURE_QUADWORDS (PICTURE_W * PICTURE_H * 4 / 16 + 16)
#define CROSS_QUADWORDS (FRAME_QUADWORDS + CARD_COUNT * PICTURE_QUADWORDS + 8)

static void note_count(float alpha) {
    PortHandsCross *c = &port_hands_cross;
    int value = port_hands_state.hands[HAND_HOTKEY].slots[port_hands_state.hands[HAND_HOTKEY].out];
    Box b = card_box(CARD_HOTKEY);

    c->vblank = port_vblank_count();
    c->alpha = alpha;
    c->count = HOTKEY_TYPE(value) == HOTKEY_ITEM ? item_counts[HOTKEY_NUMBER(value)] : -1;
    c->count_x = b.x1 - 3.0f;
    c->count_y = b.y1 - 2.0f;
}

void Hands_DrawCross(void) {
    u32 *start;
    Gif g;
    float alpha;
    int quadwords;
    int card;

    if (!port_hands.enabled || game_options[9] == 0 || map_draw_state.hud_shown == 0) {
        return;
    }
    update_pictures();
    /* a packet buffer that could not take the cross leaves it out (the game's would overflow, and hang) */
    if (packet_quadwords_used + CROSS_QUADWORDS + 64 > packet_buffers_size / 16) {
        return;
    }
    alpha = cross_alpha();
    Widescreen_Mark(PORT_WIDESCREEN_HUD_LEFT);
    GsDraw_QueueFlush(); /* the VU1's own GIF packets out before ours (as TextureTable_QueueAll does) */
    /* a DMA tag, a quadword of VIF codes (NOP, NOP, NOP, DIRECT: as the game's own packets, the VIF codes in the data),
       the GIF data, and a quadword for the chain's next tag */
    start = Packet_Alloc(2 + CROSS_QUADWORDS + 1);
    g.at = (u64 *)(start + 8);
    g.end = g.at + CROSS_QUADWORDS * 2;
    g.tag = NULL;
    gif_ad(&g, GS_ALPHA_1, 0x44); /* (Cs - Cd) * As + Cd */
    gif_ad(&g, GS_TEST_1, 0x31001); /* the HUD's: no Z written */
    for (card = 0; card < CARD_COUNT; card++) {
        draw_card(&g, card, alpha);
    }
    gif_ad(&g, GS_ALPHA_1, 0x48); /* as Hud_Draw leaves it */
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
    note_count(alpha);
}
