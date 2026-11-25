/* stats_bar.c: PC-only. The extended stats mod's health bar of the creature last hit (game/stats_bar.h): a long bar
   framed as the HUD's own (gage.tx2's stone and gold edge round a dark inside), filled with the HUD bars' picture in
   red, and the HP left and the most written across its middle in the game's font. Drawn as the HUD is (game/hud_packet.h), where the
   player chose (port_stats_options.bar_place): at the top middle (moved right of the HUD where they would touch),
   under the HUD, at the top right or at the bottom middle, 16 from the edge as the HUD's frame is.

   It shows for a few seconds after the creature was last hit, then fades; the HP the last hit took stays lighter on
   the bar for a moment.

   KF4_STATS_BAR_TEST=LEFT/MAX shows it all the time as if a creature with that HP had just been hit (to see it
   without fighting). */
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "map/map.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/stats.h"
#include "hud_packet.h"
#include "stats_bar.h"

extern char game_options[0x10];
extern MapDrawState map_draw_state;
void Widescreen_Mark(int what);

/* ---- The layout, in the bar's own units (HUD pixels at scale 1) ------------------------------------------------- */
#define BAR_W 200.0f
#define HUD_FRAME_RIGHT 234.0f /* the HUD's frame: 0xDA wide, from 16 */
#define BESIDE_GAP 8.0f
#define MIDDLE_X ((HUD_WIDTH - BAR_W) / 2)
/* at 4:3 (or with the widescreen HUD centred) the top's bar goes as near the middle as clears the HUD's frame */
#define BESIDE_HUD_X (HUD_FRAME_RIGHT + BESIDE_GAP)
#define TEXT_SIZE 0.7f                        /* the font's glyphs at 7 x 14 */
#define BAR_H (2.0f * HUD_GAGE_EDGE + HUD_GLYPH_H * TEXT_SIZE) /* the edge round an inside as tall as the numbers */
#define HUD_FRAME_BOTTOM 106.0f /* the HUD's frame: 128 texels drawn at 0.7 down, from 16 */
#define BELOW_GAP 8.0f

/* Each place: where the bar goes in the HUD's 640 x 448 and how --widescreen moves it */
typedef struct {
    float x, y;
    int widescreen;
} BarPlace;

static const BarPlace bar_places[PORT_STATS_BAR_PLACE_COUNT] = {
    [PORT_STATS_BAR_TOP] = {MIDDLE_X, HUD_MARGIN, PORT_WIDESCREEN_2D},
    [PORT_STATS_BAR_TOP_LEFT] = {HUD_MARGIN, HUD_FRAME_BOTTOM + BELOW_GAP, PORT_WIDESCREEN_HUD_LEFT},
    [PORT_STATS_BAR_TOP_RIGHT] = {HUD_WIDTH - HUD_MARGIN - BAR_W, HUD_MARGIN, PORT_WIDESCREEN_HUD_RIGHT},
    [PORT_STATS_BAR_BOTTOM] = {(HUD_WIDTH - BAR_W) / 2, HUD_HEIGHT - HUD_MARGIN - BAR_H, PORT_WIDESCREEN_2D},
};

/* ---- The timing, in vblanks (60 a second) ------------------------------------------------------------------------ */
#define SHOWN_VBLANKS (60 * 6) /* the bar stays 6 s after the last hit, */
#define FADE_VBLANKS 30        /* the last half second fading */
#define LOST_VBLANKS 36        /* the HP the last hit took shows lighter for 0.6 s */

/* ---- The colours (the GS's: 0x80 is full) -------------------------------------------------------------------------- */
#define INSIDE_ALPHA 0x68 /* as the hands mod's cards: lava and torchlight behind do not show through */

#define TEXT_CHARS 11 /* an int's digits */
#define BAR_QUADWORDS                                                                                               \
    (2 * HUD_QUADWORDS_USE + HUD_QUADWORDS_RECT + HUD_QUADWORDS_GAGE_EDGE + 2 * HUD_QUADWORDS_TEXELS + 2 +          \
     HUD_QUADWORDS_USE + HUD_QUADWORDS_TEXT(2 * TEXT_CHARS + 1))

/* KF4_STATS_BAR_TEST's target, read once; 0 when unset */
static int test_target(PortStatsHit *out) {
    static int read, left, most;

    if (!read) {
        const char *test = getenv("KF4_STATS_BAR_TEST");

        read = 1;
        if (test == NULL || sscanf(test, "%d/%d", &left, &most) != 2 || most <= 0) {
            most = 0;
        }
    }
    if (most <= 0) {
        return 0;
    }
    out->kind = PORT_STATS_HIT_DEALT;
    out->amount = most / 10;
    out->left = left;
    out->max = most;
    out->killed = left <= 0;
    out->vblank = port_vblank_count();
    out->target = 1;
    return 1;
}

static float fraction_of(int hp, int most) {
    float f = (float)hp / (float)most;

    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

/* 1, falling to 0 over the last FADE_VBLANKS the bar is shown */
static float bar_alpha(unsigned long long since_hit) {
    if (since_hit + FADE_VBLANKS <= SHOWN_VBLANKS) {
        return 1.0f;
    }
    return (float)(SHOWN_VBLANKS - since_hit) / (float)FADE_VBLANKS;
}

/* the bar: its frame, the HP left in red, the HP the last hit took lighter after it */
static void draw_bar(HudPacket *p, const PortStatsHit *t, float alpha, unsigned long long since_hit) {
    HudBox box = {0, 0, BAR_W, BAR_H};
    HudBox inside = {box.x0 + HUD_GAGE_EDGE, box.y0 + HUD_GAGE_EDGE, box.x1 - HUD_GAGE_EDGE, box.y1 - HUD_GAGE_EDGE};
    float width = inside.x1 - inside.x0;
    float left = fraction_of(t->left, t->max);
    float before = fraction_of(t->left + t->amount, t->max);
    int a = (int)(0x80 * alpha);

    HudPacket_UseGage(p);
    HudPacket_Rect(p, inside, hud_rgba(0x08, 0x07, 0x06, a * INSIDE_ALPHA / 0x80));
    HudPacket_GageEdge(p, box, hud_rgba(HUD_GAGE_GREY, HUD_GAGE_GREY, HUD_GAGE_GREY, a));
    HudPacket_UseBarFill(p);
    if (left > 0.0f) {
        HudPacket_Texels(p, 0, 0, HUD_BAR_TEXELS_W * left, HUD_BAR_TEXELS_H,
                         (HudBox){inside.x0, inside.y0, inside.x0 + width * left, inside.y1}, hud_rgba(0x80, 0x1C, 0x10, a));
    }
    if (before > left && since_hit < LOST_VBLANKS) {
        int lost_a = (int)((float)a * (1.0f - (float)since_hit / (float)LOST_VBLANKS));

        HudPacket_Texels(p, HUD_BAR_TEXELS_W * left, 0, HUD_BAR_TEXELS_W * before, HUD_BAR_TEXELS_H,
                         (HudBox){inside.x0 + width * left, inside.y0, inside.x0 + width * before, inside.y1},
                         hud_rgba(0xA0, 0x80, 0x60, lost_a));
    }
}

/* "left/most" across the bar's middle, in the game's font, over the fill. The slash stays at the middle, the HP left
   ending at it and the most starting after it, so the numbers stay put as the HP falls instead of the whole line
   moving to stay centred. */
static void draw_numbers(HudPacket *p, const PortStatsHit *t, float alpha) {
    float y = HUD_GAGE_EDGE;
    char left[TEXT_CHARS + 1], most[TEXT_CHARS + 1];
    float slash_x = (BAR_W - hud_text_width("/", TEXT_SIZE)) / 2;
    u64 colour = hud_rgba(0x80, 0x78, 0x68, (int)(0x80 * alpha));

    snprintf(left, sizeof(left), "%d", t->left < 0 ? 0 : t->left);
    snprintf(most, sizeof(most), "%d", t->max);
    HudPacket_UseFont(p);
    HudPacket_Text(p, left, slash_x - hud_text_width(left, TEXT_SIZE), y, TEXT_SIZE, colour);
    slash_x += HudPacket_Text(p, "/", slash_x, y, TEXT_SIZE, colour);
    HudPacket_Text(p, most, slash_x, y, TEXT_SIZE, colour);
}

void StatsBar_Draw(void) {
    BarPlace place = bar_places[port_stats_options.bar_place];
    unsigned long long since_hit;
    PortStatsHit t;
    HudPacket p;
    float alpha;

    if (!port_stats_options.on || !port_stats_options.bar || game_options[9] == 0 || map_draw_state.hud_shown == 0) {
        return;
    }
    if (!test_target(&t) && (!port_stats_target(&t, SHOWN_VBLANKS) || t.max <= 0)) {
        return;
    }
    since_hit = port_vblank_count() - t.vblank;
    alpha = bar_alpha(since_hit);
    if (alpha <= 0.0f || !HudPacket_Fits(BAR_QUADWORDS)) {
        return;
    }
    if (port_stats_options.bar_place == PORT_STATS_BAR_TOP && !port_widescreen_hud_at_edge() && place.x < BESIDE_HUD_X) {
        place.x = BESIDE_HUD_X;
    }
    Widescreen_Mark(place.widescreen);
    HudPacket_QueueFont();
    HudPacket_Begin(&p, BAR_QUADWORDS, (HudPlace){place.x, place.y, 1.0f});
    draw_bar(&p, &t, alpha, since_hit);
    draw_numbers(&p, &t, alpha);
    HudPacket_End(&p);
    Widescreen_Mark(PORT_WIDESCREEN_3D);
}
