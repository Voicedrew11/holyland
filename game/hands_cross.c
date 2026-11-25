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
#include "hud_packet.h"

extern PlayerState player_state;
extern u8 item_counts[ITEM_COUNT];
extern LevelDef item_name_defs[ITEM_COUNT];
extern char game_options[0x10];
extern MapDrawState map_draw_state;
void Widescreen_Mark(int what);

/* ---- The layout, in the HUD's 640 x 448 (Hud_InitSprite: x + 1728, y + 1824 in GS pixels) ---------------- */
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

#define EDGE_PX HUD_GAGE_EDGE /* the frame's stone and gold edge (gage.tx2's long bar) */

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

/* Where this frame's cross is and its shape: its top left in the HUD's 640 x 448, its scale, its cards' height
   and the middle card's drop. Everything below is laid out in the cross's own units (CROSS_W wide, card_h + drop
   high, from its top left), which the packet places. */
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

/* ---- The cards ------------------------------------------------------------------------------------------------ */
static HudBox card_box(int card) {
    HudBox box;

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

/* the card's box, one larger while it pulses */
static HudBox drawn_box(int card) {
    HudBox b = card_box(card);
    float grow = since(hands_feedback.changed_vblank[card_hands[card]], PULSE_VBLANKS) < 1.0f ? 1.0f : 0.0f;

    b.x0 -= grow, b.y0 -= grow, b.x1 += grow, b.y1 += grow;
    return b;
}

/* The card's frame: a dark inside the scene shows through, the bar's stone and gold edge round it, warmer while
   it pulses, red while refused. gage.tx2 in use. */
static void draw_card_frame(HudPacket *g, int card, float alpha) {
    HandKind kind = card_hands[card];
    HudBox b = drawn_box(card);
    float pulse = 1.0f - since(hands_feedback.changed_vblank[kind], PULSE_VBLANKS);
    float flash = 1.0f - since(hands_feedback.refused_vblank[kind], FLASH_VBLANKS);
    int a = (int)(0x80 * alpha);
    u64 stone = hud_rgba(HUD_GAGE_GREY + (int)(0x38 * pulse), HUD_GAGE_GREY + (int)(0x1C * pulse), HUD_GAGE_GREY, a);
    HudBox inside = {b.x0 + EDGE_PX, b.y0 + EDGE_PX, b.x1 - EDGE_PX, b.y1 - EDGE_PX};

    HudPacket_Rect(g, inside, hud_rgba(0x08, 0x07, 0x06, a * INSIDE_ALPHA / 0x80));
    HudPacket_GageEdge(g, b, stone);
    if (flash > 0.0f) {
        /* over the gold line, the inner third of the edge */
        float in = EDGE_PX / 3.0f;
        u64 red = hud_rgba(0xC8, 0x20, 0x18, (int)(a * flash));

        HudPacket_Frame(g, (HudBox){inside.x0 - in, inside.y0 - in, inside.x1 + in, inside.y1 + in}, red);
        HudPacket_Frame(g, (HudBox){inside.x0 - in + 1, inside.y0 - in + 1, inside.x1 + in - 1, inside.y1 + in - 1}, red);
    }
}

/* The card's picture, square (it is stored at half height), as large as fits inside the frame */
static void draw_card_picture(HudPacket *g, int card, float alpha) {
    CardPicture *p = &pictures[card];
    HudBox b = drawn_box(card);
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
    HudPacket_Image(g, PICTURE_TBP, PICTURE_W, PICTURE_H, p->pixels);
    HudPacket_Register(g, HUD_GS_TEXFLUSH, 0);
    /* TEX0: TBP0, TBW 2, PSMCT32, TW 7 (128), TH 6 (64), TCC (RGBA), TFX modulate */
    HudPacket_Register(g, HUD_GS_TEX0_1, (u64)PICTURE_TBP | (u64)(PICTURE_W / 64) << 14 | (u64)7 << 26 | (u64)6 << 30 | (u64)1 << 34);
    /* the middle of the picture: the pictures leave a margin round the item, which the card's edge does already */
    HudPacket_Texels(g, PICTURE_W * MARGIN, PICTURE_H * MARGIN, PICTURE_W * (1 - MARGIN), PICTURE_H * (1 - MARGIN),
                     (HudBox){x0, y0, x0 + side, y0 + side}, hud_rgba(bright, bright, bright, (int)(0x80 * alpha)));
}

/* ---- The count: the hotkey's items carried, in the game's own font ---------------------------------------------- */
#define COUNT_DIGITS 4

/* the hotkey's count, -1 none to show (a spell, an empty hotkey) */
static int hotkey_count(void) {
    const Hand *hand = &port_hands_state.hands[HAND_HOTKEY];
    int value = hand->slots[hand->out];

    return HOTKEY_TYPE(value) == HOTKEY_ITEM ? item_counts[HOTKEY_NUMBER(value)] : -1;
}

/* The count at the bottom right inside the hotkey's card, white with a dark shadow as over the scene. The font is
   in GS memory (Hands_DrawCross queued it). */
static void draw_count(HudPacket *g, int count, float alpha) {
    HudBox b = drawn_box(CARD_HOTKEY);
    char digits[COUNT_DIGITS + 1];

    snprintf(digits, sizeof(digits), "%d", count);
    HudPacket_UseFont(g);
    /* the digits reach the cell's bottom: clear of the gold line */
    HudPacket_Text(g, digits, b.x1 - EDGE_PX - 1.0f - hud_text_width(digits, 1.0f), b.y1 - EDGE_PX - HUD_GLYPH_H, 1.0f,
                   hud_rgba(0x80, 0x80, 0x80, (int)(0x80 * alpha)));
}

/* the quadwords the cross's packet can take at most: each card's frame (its inside, its edge and the flash's two
   frames), each picture with its registers, and the count */
#define CROSS_QUADWORDS                                                                                              \
    (HUD_QUADWORDS_USE + CARD_COUNT * (HUD_QUADWORDS_RECT + HUD_QUADWORDS_GAGE_EDGE + 2 * HUD_QUADWORDS_FRAME) +    \
     CARD_COUNT * (HUD_QUADWORDS_IMAGE(PICTURE_W, PICTURE_H) + 2 + HUD_QUADWORDS_TEXELS) + HUD_QUADWORDS_USE +       \
     HUD_QUADWORDS_TEXT(COUNT_DIGITS))

void Hands_DrawCross(void) {
    HudPacket g;
    float alpha;
    int card;
    int count;

    if (!port_hands.enabled || game_options[9] == 0 || map_draw_state.hud_shown == 0) {
        return;
    }
    update_pictures();
    if (!HudPacket_Fits(CROSS_QUADWORDS)) {
        return;
    }
    alpha = cross_alpha();
    count = hotkey_count();
    place = place_of(&port_hands);
    Widescreen_Mark(corner_places[port_hands.corner].widescreen);
    if (count >= 0) {
        HudPacket_QueueFont();
    }
    HudPacket_Begin(&g, CROSS_QUADWORDS, (HudPlace){place.x, place.y, place.scale});
    /* the frames first, while gage.tx2 is in GS memory; the pictures go over it */
    HudPacket_UseGage(&g);
    for (card = 0; card < CARD_COUNT; card++) {
        draw_card_frame(&g, card, alpha);
    }
    for (card = 0; card < CARD_COUNT; card++) {
        draw_card_picture(&g, card, alpha);
    }
    if (count >= 0) {
        draw_count(&g, count, alpha);
    }
    HudPacket_End(&g);
    Widescreen_Mark(PORT_WIDESCREEN_3D);
}
