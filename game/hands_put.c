/* hands_put.c: PC-only. The hands mod's "Put in" box on the equipment screen (game/hands_put.h).

   Drawn as the menus draw their lists: a dark flat bar for the box (MenuDraw_Bar), the menus' font for the text
   (MenuText_DrawLine: Latin characters are the codes 0x0800 + (character - 0x20), as game/automap.c reads them),
   and the item list's pale yellow cursor bar, pulsing, under the line chosen. Places are in the menus' picture,
   640 x 448, turned into the GS's 12.4 coordinates (the menus' corner is at 0x6C00, 0x7900, a field high). The
   box sits over the item's model in the screen's left panel (x 37 to 286), centred on it. */
#include <string.h>

#include "common.h"
#include "input/pad.h"
#include "ui/menu.h"
#include "ui/menu_draw.h"
#include "ui/menu_point.h"
#include "port/hands.h"
#include "port/platform.h"
#include "hands_put.h"

extern ColorBytes menu_draw_color;
s32 Sound_PlayMenu(s32);
int Menu_SetBlendMode(u8);

#define SOUND_CONFIRM 0 /* Sound_PlayMenu: the menus' Cross */
#define SOUND_CURSOR 1  /* a move */
#define SOUND_BACK 2    /* Circle */
#define SOUND_REFUSED 3 /* Cross where nothing can be done */

/* the box, in the menus' 640 x 448 */
#define BOX_LEFT 46
#define BOX_WIDTH 230
#define BOX_MIDDLE_Y 230 /* the model's middle */
#define BOX_PAD 10
#define LINE_HEIGHT 22   /* a line of the menus' lists (0xB0 in the GS's y, a field) */
#define TITLE_GAP 6
#define NUMBER_X 14      /* from the box's left */
#define NAME_X 36
#define NAME_GLYPHS 15   /* the menus' Latin glyphs are 10 wide: up to x 186 */
#define MARK_X (BOX_WIDTH - 24)
#define DEPTH 0xFFFFFF   /* in front of the screen's models */

#define FADE_STEP 0.25f  /* the box's fade in and out, a menu frame */

static struct {
    int open;             /* up and taking the buttons (fading in or shown) */
    float alpha;          /* 0 to 1 */
    HandKind kind;
    int value;            /* the item to put */
    int cursor;           /* the slot chosen */
    int equip_through;    /* this frame's Cross is the box's: the game equips */
    float pulse;          /* the cursor bar's alpha, 0x10 to 0x20 and back */
    int pulse_falling;
} box;

static s32 gs_x(int x) {
    return (0x6C0 + x) * 16;
}

static s32 gs_y(int y) {
    return (0x790 + y / 2) * 16;
}

static int box_height(int lines) {
    return BOX_PAD + LINE_HEIGHT + TITLE_GAP + lines * LINE_HEIGHT + BOX_PAD;
}

static int box_top(int lines) {
    return BOX_MIDDLE_Y - box_height(lines) / 2;
}

static int line_top(int lines, int line) {
    return box_top(lines) + BOX_PAD + LINE_HEIGHT + TITLE_GAP + line * LINE_HEIGHT;
}

/* ---- Opening, the buttons ---------------------------------------------------------------------------- */

void HandsPut_Open(HandKind kind, int value) {
    Hands_FollowGame();
    box.open = 1;
    box.kind = kind;
    box.value = value;
    box.cursor = port_hands_state.hands[kind].out;
    box.pulse = 0x10;
    box.pulse_falling = 0;
    Sound_PlayMenu(SOUND_CONFIRM);
}

void HandsPut_Close(void) {
    box.open = 0;
    box.alpha = 0.0f;
    box.equip_through = 0;
}

static void shut(void) {
    box.open = 0; /* fades out in HandsPut_Draw */
}

/* Cross on the slot chosen: what it does (hand_put); 1 when the game must see the Cross and equip */
static int put(void) {
    Hand *hand = &port_hands_state.hands[box.kind];

    switch (hand_put(hand, box.kind, box.cursor, box.value)) {
    case HAND_PUT_EQUIP:
        shut();
        return 1;
    case HAND_PUT_KEEP:
        hand->slots[box.cursor] = box.value;
        port_hands_changed();
        port_event("hands: slot %d of hand %d keeps %d (put in)", box.cursor + 1, (int)box.kind, box.value);
        Sound_PlayMenu(SOUND_CONFIRM);
        shut();
        return 0;
    case HAND_PUT_ELSEWHERE:
        Sound_PlayMenu(SOUND_REFUSED);
        return 0;
    case HAND_PUT_NOTHING:
    default:
        Sound_PlayMenu(SOUND_CONFIRM);
        shut();
        return 0;
    }
}

u16 HandsPut_Buttons(u16 buttons) {
    int lines;

    box.equip_through = 0;
    if (!box.open) {
        return box.alpha > 0.0f ? 0 : buttons; /* fading out: nothing until it is gone */
    }
    lines = hand_slot_count(box.kind);
    if (buttons & PAD_UP) {
        box.cursor = (box.cursor + lines - 1) % lines;
        Sound_PlayMenu(SOUND_CURSOR);
    } else if (buttons & PAD_DOWN) {
        box.cursor = (box.cursor + 1) % lines;
        Sound_PlayMenu(SOUND_CURSOR);
    }
    if (buttons & PAD_CROSS) {
        if (put()) {
            box.equip_through = 1;
            return PAD_CROSS;
        }
    } else if (buttons & (PAD_CIRCLE | PAD_TRIANGLE | PAD_SQUARE)) {
        Sound_PlayMenu(SOUND_BACK);
        shut();
    }
    return 0;
}

int HandsPut_TakeEquip(void) {
    int through = box.equip_through;

    box.equip_through = 0;
    return through;
}

/* ---- Drawing ------------------------------------------------------------------------------------------- */

/* `ascii` in the menus' codes: printable Latin characters only, ended by 0xFFFF */
static void encode(const char *ascii, u16 *out, size_t size) {
    size_t i;

    for (i = 0; ascii[i] != '\0' && i + 1 < size; i++) {
        unsigned char c = (unsigned char)ascii[i];

        out[i] = (u16)(0x0800 | (c >= 0x20 && c < 0x7F ? c - 0x20 : '?' - 0x20));
    }
    out[i] = 0xFFFF;
}

static ColorBytes faded(u8 red, u8 green, u8 blue, float alpha) {
    ColorBytes color;

    memset(&color, 0, sizeof(color));
    color.red = red;
    color.green = green;
    color.blue = blue;
    color.alpha = (u8)((float)menu_draw_color.alpha * alpha);
    return color;
}

static void bar(int x0, int y0, int x1, int y1, ColorBytes color) {
    MenuCursorBar rect;

    memset(&rect, 0, sizeof(rect));
    rect.first.f.lo16 = gs_x(x0);
    rect.first.f.mid16 = gs_y(y0);
    rect.first.f.mid24 = DEPTH;
    rect.second.f.lo16 = gs_x(x1);
    rect.second.f.mid16 = gs_y(y1);
    rect.second.f.mid24 = DEPTH;
    rect.red = color.red;
    rect.green = color.green;
    rect.blue = color.blue;
    rect.alpha = color.alpha;
    MenuDraw_Bar(&rect);
}

static void text_at(int x, int y, ColorBytes color, u16 *text, s32 most_glyphs) {
    GsPackedWord pen;

    pen.raw = 0;
    pen.f.lo16 = gs_x(x);
    pen.f.mid16 = gs_y(y);
    pen.f.mid24 = DEPTH;
    MenuText_SetPen(pen);
    MenuText_SetColor(color);
    MenuText_DrawLine(text, most_glyphs);
}

static void ascii_at(int x, int y, ColorBytes color, const char *ascii) {
    u16 text[32];

    encode(ascii, text, sizeof(text) / sizeof(text[0]));
    text_at(x, y, color, text, 31);
}

/* the item list's cursor bar (Menu_UpdateUseItem): 0x10 to 0x20 and back, over a second */
static float pulse(void) {
    box.pulse += box.pulse_falling ? -16.0f / 60.0f : 16.0f / 60.0f;
    if (box.pulse >= 0x20) {
        box.pulse = 0x20;
        box.pulse_falling = 1;
    } else if (box.pulse <= 0x10) {
        box.pulse = 0x10;
        box.pulse_falling = 0;
    }
    return box.pulse;
}

static void draw_line(int lines, int line, const Hand *hand) {
    int top = line_top(lines, line);
    const u16 *name = Hands_ValueName(box.kind, hand->slots[line]);
    char number[4];

    number[0] = (char)('1' + line);
    number[1] = '\0';
    ascii_at(BOX_LEFT + NUMBER_X, top, faded(0x80, 0x80, 0x80, box.alpha), number);
    if (name != NULL) {
        text_at(BOX_LEFT + NAME_X, top, faded(0x80, 0x80, 0x80, box.alpha), (u16 *)name, NAME_GLYPHS);
    } else {
        ascii_at(BOX_LEFT + NAME_X, top, faded(0x50, 0x50, 0x50, box.alpha), "-");
    }
    if (line == hand->out) {
        ascii_at(BOX_LEFT + MARK_X, top, faded(0x80, 0x80, 0x3C, box.alpha), "E");
    }
}

void HandsPut_Draw(void) {
    const Hand *hand;
    int lines;
    int top;
    int line;

    box.alpha += box.open ? FADE_STEP : -FADE_STEP;
    box.alpha = box.alpha < 0.0f ? 0.0f : box.alpha > 1.0f ? 1.0f : box.alpha;
    if (box.alpha <= 0.0f) {
        return;
    }
    hand = &port_hands_state.hands[box.kind];
    lines = hand_slot_count(box.kind);
    top = box_top(lines);
    Menu_SetBlendMode(0);
    /* the box: dark, with a faint edge */
    bar(BOX_LEFT, top, BOX_LEFT + BOX_WIDTH, top + box_height(lines), faded(0, 0, 0, box.alpha * 0.9f));
    bar(BOX_LEFT, top, BOX_LEFT + BOX_WIDTH, top + 2, faded(0x70, 0x70, 0x68, box.alpha * 0.35f));
    bar(BOX_LEFT, top + box_height(lines) - 2, BOX_LEFT + BOX_WIDTH, top + box_height(lines),
        faded(0x70, 0x70, 0x68, box.alpha * 0.35f));
    ascii_at(BOX_LEFT + NUMBER_X, top + BOX_PAD, faded(0x80, 0x80, 0x80, box.alpha), "Put in");
    /* the cursor bar under the chosen line, then the lines */
    bar(BOX_LEFT + 6, line_top(lines, box.cursor), BOX_LEFT + BOX_WIDTH - 6, line_top(lines, box.cursor) + LINE_HEIGHT,
        faded(0xFF, 0xFF, 0x78, box.alpha * pulse() / 128.0f));
    for (line = 0; line < lines; line++) {
        draw_line(lines, line, hand);
    }
    if (box.open) {
        MenuPad_PointCursor(box.cursor, MENU_AXIS_VERTICAL);
        MenuPad_PointRows(lines, gs_x(BOX_LEFT + 6), gs_y(line_top(lines, 0)), (BOX_WIDTH - 12) * 16,
                          LINE_HEIGHT / 2 * 16, LINE_HEIGHT / 2 * 16);
    }
}
