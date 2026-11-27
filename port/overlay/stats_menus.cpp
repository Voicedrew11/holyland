// The extended stats mod's lines in the game's own menus (port/include/port/stats.h,
// docs/port/EXTENDED_STATS.md): drawn with Dear ImGui into the empty places of the menu screens, in a
// serif close to the game's text, with no window around them.
//
// - Main menu and Status: under "Condition", in the box of Exp ... Condition: the experience to the next
//   level, the weapon hits and spells to the next Physical and Magical point, the hits to the next weapon
//   level, and the ailments running with their time left.
// - Equipment: in the box of the player's figure, around it: the weapon's level and the ailments above,
//   each piece's wear on the left, the hidden effects of what is worn on the right.
// - Item and Arrange: at the foot of the box of the item's model, what the item under the cursor does.
//
// The places are the menus' own (the part files menu/*_para.dat, read in game RAM; MenuSprite_Draw draws
// each part in the frame at X - 1728, (Y - 1936) * 2): they are given here in the 640 x 448 picture of
// the menus, which the window shows whole at 4:3 and, with widescreen, squeezed into its middle three
// quarters (port/platform/gs.c). They are the same with every renderer and presenter.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#ifdef KF4_HAVE_PLAYER_UI
#include "../ui/ui_assets.hpp" // the menu's Mincho, carried inside the game
#endif

#include "stats_menus.h"

extern "C" {
#include "port/host.h"
#include "port/stats.h"
}

namespace {

ImFont *serif; // the menus' font (stats_menus_fonts), else ImGui's own

// The colours of the game's menu text (a light grey) and its frames (a pale blue)
const ImVec4 colour_text(0.86f, 0.85f, 0.81f, 1.0f);
const ImVec4 colour_label(0.66f, 0.66f, 0.64f, 1.0f);
const ImVec4 colour_head(0.62f, 0.70f, 0.82f, 1.0f);
const ImVec4 colour_bad(0.88f, 0.52f, 0.46f, 1.0f);
const ImVec4 colour_good(0.64f, 0.80f, 0.60f, 1.0f);

const char *const ailment_names[6] = {"Poison", "Paralysis", "Darkness", "Curse", "Slow", "Magic seal"};

// ---- The places (the 640 x 448 picture of the menus) -----------------------------------------------
struct Box {
    float x0, y0, x1, y1;
};

// Main menu: the box of Exp ... Condition (main_para.dat part 15), the labels from x 352 (part 38), the
// values to x 574 (part 30), Condition (part 31) to y 306, the bottom border from 360 (part 39). The
// ailment icons (parts 0-5, MenuSprite_ShowIfFlagSet) at x 457-576, y 288-362, each while its ailment
// runs, are kept clear of (port_stats_sample gives the ones shown).
const Box main_box = {352, 309, 574, 364};
// Status: the left box (status_para.dat part 1), the labels from x 82 (part 69), the values to x 302
// (part 51), Condition (part 62) to y 318, the bottom border from 370 (part 8); the icons (parts 10-15) at
// x 195-302, y 298-370.
const Box status_box = {82, 321, 302, 372};
// Equipment: the box of the figure (equip_para.dat part 0) inside its borders (parts 21, 3 and 2: x 324 to
// 617, from y 36), the weight's band from y 334 (part 9). The figure stands about x 448-525 with its head
// from y 79, its hands (with a weapon and a shield) reaching x 409-547 at y 180-245.
const Box equip_top = {334, 44, 608, 78};
const Box equip_left = {334, 88, 404, 330};
const Box equip_right = {552, 88, 608, 330};
// Item and Arrange: the box of the model (item_para.dat part 1, recol_para.dat part 23: x 391-621,
// y 68-252); the lines go at its foot, at most up to y 180
const Box item_box = {395, 180, 617, 250};

// ---- The window's place of the picture ---------------------------------------------------------------
struct Frame {
    float x0, y0, sx, sy; // window = x0 + frame x * sx

    ImVec2 at(float x, float y) const {
        return ImVec2(std::floor(x0 + x * sx + 0.5f), std::floor(y0 + y * sy + 0.5f));
    }
};

Frame frame_of(int x, int y, int width, int height) {
    ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    float fx = scale.x > 0.0f ? scale.x : 1.0f, fy = scale.y > 0.0f ? scale.y : 1.0f;
    float px = (float)x / fx, py = (float)y / fy, pw = (float)width / fx, ph = (float)height / fy;
    if (pw < 32.0f || ph < 32.0f) {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        px = vp->Pos.x, py = vp->Pos.y, pw = vp->Size.x, ph = vp->Size.y;
    }
    Frame f;
    if (port_widescreen()) { // the menus keep their 4:3 shape in the middle (port/platform/gs.c, "Widescreen")
        float squeeze = port_widescreen_squeeze();
        f.x0 = px + pw * (1.0f - squeeze) / 2.0f;
        f.sx = pw * squeeze / 640.0f;
    } else {
        f.x0 = px;
        f.sx = pw / 640.0f;
    }
    f.y0 = py;
    f.sy = ph / 448.0f;
    return f;
}

// ---- Rows, fitted to a place ----------------------------------------------------------------------
enum RowKind { ROW_PAIR, ROW_TEXT, ROW_HEAD };

struct Row {
    RowKind kind;
    std::string label, value; // ROW_PAIR: label left, value right; ROW_TEXT, ROW_HEAD: label alone
    ImVec4 colour = colour_text;
    float fraction = -1.0f; // ROW_PAIR: a bar under it, 0 to 1
    bool divider_before = false; // a short rule between equipment effect groups
};

struct Block {
    Box box;
    std::vector<Row> rows;
    bool centre = false; // ROW_TEXT centred
    bool shade = false;  // a dark band behind (over the item's model)
    bool no_wrap = false; // smaller rather than a line wrapped
    std::vector<Box> avoid; // to keep clear of
};

const float PITCH = 1.18f; // a line's height, in font sizes

ImFont *font() {
    return serif != nullptr ? serif : ImGui::GetFont();
}

float text_width(float size, const std::string &text) {
    return font()->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
}

// A block laid out at a size: every piece of text in its place, and the bars. Each line takes the width
// its place has at its height, short of what it must keep clear of (the game's ailment icons).
struct Piece {
    std::string text;
    ImVec2 at;
    ImVec4 colour;
};

struct Bar {
    float x0, x1, y, height, fraction;
};

struct Layout {
    std::vector<Piece> pieces;
    std::vector<Bar> bars;
    float top = 0.0f, bottom = 0.0f;
    bool fits = true;
};

// `force`: lay out whatever happens (a pair too wide puts its value on a second line); otherwise a pair
// too wide, a line that does not fit across (no_wrap) or rows past the place's foot make it not fit
Layout layout(const Block &b, const Frame &f, float size, bool force) {
    Layout out;
    ImVec2 lo = f.at(b.box.x0, b.box.y0), hi = f.at(b.box.x1, b.box.y1);
    float pitch = std::floor(size * PITCH + 0.5f), gap = size * 0.6f, y = lo.y;
    std::vector<ImVec4> avoid;
    for (const Box &a : b.avoid) {
        ImVec2 a0 = f.at(a.x0, a.y0), a1 = f.at(a.x1, a.y1);
        avoid.push_back(ImVec4(a0.x - gap, a0.y, a1.x, a1.y));
    }
    // the right end of the line from y
    auto right_at = [&](float top) {
        float right = hi.x;
        for (const ImVec4 &a : avoid)
            if (a.y < top + pitch && a.w > top && a.z > lo.x)
                right = std::min(right, a.x);
        return right;
    };
    auto room = [&]() { return y + pitch <= hi.y + pitch * 0.25f; };
    out.top = y;
    for (const Row &row : b.rows) {
        if (row.divider_before) {
            float spacing = std::max(2.0f, std::floor(size * 0.5f));
            if (y + spacing + pitch > hi.y + pitch * 0.25f) {
                out.fits = false;
                break;
            }
            float right = right_at(y);
            out.bars.push_back({lo.x, lo.x + (right - lo.x) * 0.65f,
                                std::floor(y + spacing * 0.5f),
                                std::max(1.0f, std::floor(size * 0.06f)), 1.0f});
            y += spacing;
        }
        if (!room()) {
            out.fits = false;
            break;
        }
        float right = right_at(y);
        if (row.kind == ROW_PAIR) {
            float lw = text_width(size, row.label), vw = text_width(size, row.value);
            out.pieces.push_back({row.label, ImVec2(lo.x, y), colour_label});
            if (lw + gap + vw > right - lo.x) {
                out.fits = false;
                if (!force)
                    break;
                y += pitch;
                if (!room())
                    break;
                right = right_at(y);
            }
            out.pieces.push_back({row.value, ImVec2(std::floor(right - vw), y), row.colour});
            if (row.fraction >= 0.0f)
                out.bars.push_back({lo.x, right, std::floor(y + pitch - std::max(2.0f, size * 0.12f)),
                                    std::max(1.0f, std::floor(size * 0.08f)), std::min(1.0f, std::max(0.0f, row.fraction))});
            y += pitch;
            continue;
        }
        const char *s = row.label.c_str(), *end = s + row.label.size();
        ImVec4 c = row.kind == ROW_HEAD ? colour_head : row.colour;
        while (s < end) {
            if (!room()) {
                out.fits = false;
                break;
            }
            right = right_at(y);
            const char *e = font()->CalcWordWrapPosition(size, s, end, right - lo.x);
            if (e <= s)
                e = s + 1;
            if (e < end && b.no_wrap) {
                out.fits = false;
                if (!force)
                    return out;
            }
            std::string text(s, e);
            while (!text.empty() && text.back() == ' ')
                text.pop_back();
            float x = b.centre ? std::floor(lo.x + (right - lo.x - text_width(size, text)) * 0.5f) : lo.x;
            out.pieces.push_back({text, ImVec2(x, y), c});
            y += pitch;
            s = e;
            while (s < end && (*s == ' ' || *s == '\n'))
                s++;
        }
    }
    out.bottom = y;
    if (b.centre) { // centred blocks sit at the foot of their place
        float shift = std::floor(hi.y - y);
        for (Piece &p : out.pieces)
            p.at.y += shift;
        for (Bar &bar : out.bars)
            bar.y += shift;
        out.top += shift;
        out.bottom += shift;
    }
    return out;
}

// The largest size from `most` down to `least` (window pixels) at which the block fits its place
float fit(const Block &b, const Frame &f, float most, float least) {
    for (float size = most; size > least; size -= 0.5f)
        if (layout(b, f, size, false).fits)
            return size;
    return least;
}

ImU32 colour(const ImVec4 &c, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

void text(ImDrawList *dl, float size, ImVec2 at, const ImVec4 &c, float alpha, const std::string &t) {
    float shadow = std::max(1.0f, std::floor(size / 14.0f));
    dl->AddText(font(), size, ImVec2(at.x + shadow, at.y + shadow), colour(ImVec4(0, 0, 0, 0.85f), alpha), t.c_str());
    dl->AddText(font(), size, at, colour(c, alpha), t.c_str());
}

// Draws a block at a size (what does not fit its place is left out)
void draw_block(ImDrawList *dl, const Frame &f, const Block &b, float size, float alpha) {
    Layout l = layout(b, f, size, true);
    if (l.pieces.empty())
        return;
    ImVec2 lo = f.at(b.box.x0, b.box.y0), hi = f.at(b.box.x1, b.box.y1);
    if (b.shade) {
        float top = l.top - size * PITCH * 0.6f;
        dl->AddRectFilledMultiColor(ImVec2(lo.x, top), ImVec2(hi.x, hi.y), colour(ImVec4(0, 0, 0, 0), alpha),
                                    colour(ImVec4(0, 0, 0, 0), alpha), colour(ImVec4(0, 0, 0, 0.6f), alpha),
                                    colour(ImVec4(0, 0, 0, 0.6f), alpha));
    }
    for (const Bar &bar : l.bars) {
        dl->AddRectFilled(ImVec2(bar.x0, bar.y), ImVec2(bar.x1, bar.y + bar.height), colour(ImVec4(1, 1, 1, 0.10f), alpha));
        dl->AddRectFilled(ImVec2(bar.x0, bar.y), ImVec2(bar.x0 + std::floor((bar.x1 - bar.x0) * bar.fraction), bar.y + bar.height),
                          colour(ImVec4(colour_head.x, colour_head.y, colour_head.z, 0.75f), alpha));
    }
    for (const Piece &p : l.pieces)
        text(dl, size, p.at, p.colour, alpha, p.text);
}

// ---- What each screen shows -------------------------------------------------------------------------
float seconds(int frames) {
    return (float)frames / (float)PORT_STATS_GAME_FPS;
}

std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *fmt, ...) {
    char buffer[160];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

Row pair(const std::string &label, const std::string &value, float fraction = -1.0f, ImVec4 c = colour_text) {
    Row r;
    r.kind = ROW_PAIR;
    r.label = label;
    r.value = value;
    r.fraction = fraction;
    r.colour = c;
    return r;
}

Row line(const std::string &label, ImVec4 c = colour_text) {
    Row r;
    r.kind = ROW_TEXT;
    r.label = label;
    r.colour = c;
    return r;
}

Row head(const std::string &label) {
    Row r;
    r.kind = ROW_HEAD;
    r.label = label;
    return r;
}

// The ailments running, with their time left ("Poison 50 s, Slow 12 s"); "" when none
std::string ailments_running(const PortStatsSnapshot &s) {
    std::string out;
    for (int i = 0; i < 6; i++) {
        if (s.ailment_frames[i] <= 0)
            continue;
        // timed_flags wear by ailment_wear_thirds / 3 a frame (Demon Armor, Purity Amulet)
        int frames = s.ailment_frames[i] * 3 / (s.ailment_wear_thirds > 0 ? s.ailment_wear_thirds : 3);
        if (!out.empty())
            out += ", ";
        out += format("%s %.0f s", ailment_names[i], std::ceil(seconds(frames)));
    }
    return out;
}

// Player_AddExperience, Player_CountWeaponUse, Player_CountMagicUse
void progress_rows(const PortStatsSnapshot &s, std::vector<Row> &rows, bool weapon) {
    if (s.level_to > 0) {
        int span = s.level_to - s.level_from;
        rows.push_back(pair("Next level", format("%d exp%s", s.level_to - s.experience, s.fortune ? " (x2)" : ""),
                            span > 0 ? (float)(s.experience - s.level_from) / (float)span : 1.0f));
    }
    if (s.power_base < 999)
        rows.push_back(pair("Next Physical", format("%d hits", 100 - s.power_uses), s.power_uses / 100.0f));
    if (s.magic_base < 999)
        rows.push_back(pair("Next Magical", format("%d spells", 100 - s.magic_uses), s.magic_uses / 100.0f));
    const PortStatsItem &w = s.items[0];
    if (weapon && !w.none) {
        // Player_CountWeaponUse: the weapon's level rises at 180 and 480 hits
        if (w.weapon_level < 3) {
            int from = w.weapon_level == 1 ? 0 : 180, to = w.weapon_level == 1 ? 180 : 480;
            rows.push_back(pair(format("Weapon level %d", w.weapon_level), format("%d hits", to - w.weapon_uses),
                                (float)(w.weapon_uses - from) / (float)(to - from)));
        } else {
            rows.push_back(pair("Weapon level", "3 (the most)"));
        }
    }
}

void main_or_status(ImDrawList *dl, const Frame &f, const PortStatsSnapshot &s, float alpha, bool status) {
    Block b;
    b.box = status ? status_box : main_box;
    for (int i = 0; i < s.menu.icon_count; i++) // keep clear of the game's ailment icons beside Condition
        b.avoid.push_back({s.menu.icons[i][0], s.menu.icons[i][1], s.menu.icons[i][2], s.menu.icons[i][3]});
    progress_rows(s, b.rows, true);
    std::string ill = ailments_running(s);
    if (!ill.empty())
        b.rows.push_back(line(ill, colour_bad));
    float size = fit(b, f, 13.0f * f.sy, 7.0f * f.sy);
    draw_block(dl, f, b, size, alpha);
}

void equipment(ImDrawList *dl, const Frame &f, const PortStatsSnapshot &s, float alpha) {
    Block top, left, right;
    top.box = equip_top;
    left.box = equip_left;
    right.box = equip_right;

    // above the figure: the weapon's level, the ailments and the defences against them
    const PortStatsItem &w = s.items[0];
    if (!w.none) {
        if (w.weapon_level < 3) {
            int from = w.weapon_level == 1 ? 0 : 180, to = w.weapon_level == 1 ? 180 : 480;
            top.rows.push_back(pair(format("Weapon level %d", w.weapon_level), format("%d / %d hits", w.weapon_uses, to),
                                    (float)(w.weapon_uses - from) / (float)(to - from)));
        } else {
            top.rows.push_back(pair("Weapon level 3", "the most"));
        }
    }
    std::string ill = ailments_running(s);
    top.rows.push_back(pair("Ailments", ill.empty() ? "none" : ill, -1.0f, ill.empty() ? colour_text : colour_bad));
    std::string guards;
    for (int i = 0; i < 6; i++) {
        if (s.ailment_defence[i] <= 0)
            continue;
        if (!guards.empty())
            guards += ", ";
        guards += format("%s %d%%", ailment_names[i], s.ailment_defence[i]);
    }
    if (!guards.empty())
        top.rows.push_back(pair("Guards", guards, -1.0f, colour_good));

    // left of the figure: each piece's wear (LevelRecord.level: the percent its values count at)
    static const char *const slots[PORT_STATS_SLOTS] = {"Weapon", "Shield", "Head",  "Chest", "Hands",
                                                        "Legs",   "Ring",   "Wrist", "Neck"};
    if (port_stats_options.durability) {
        left.rows.push_back(head("Durability"));
        bool any = false;
        for (int i = 0; i < PORT_STATS_SLOTS; i++) {
            const PortStatsItem &it = s.items[i];
            if (it.none || !it.wears)
                continue;
            ImVec4 c = it.durability >= 90 ? colour_text : it.durability >= 70 ? ImVec4(0.88f, 0.78f, 0.48f, 1) : colour_bad;
            left.rows.push_back(pair(slots[i], format("%d%%", it.durability), -1.0f, c));
            any = true;
        }
        if (!any)
            left.rows.push_back(line("nothing worn wears", colour_label));
    }

    // right of it: what is worn does that the game never says
    right.rows.push_back(head("Effects"));
    size_t before = right.rows.size();
    // Data bonuses and code-defined effects arrive separately. Keep the same item's
    // descriptions together, in first-seen order, with a rule only between items.
    std::vector<std::string> items;
    for (int i = 0; i < s.effect_count; i++) {
        std::string item = s.effects[i].item;
        if (!item.empty() && std::find(items.begin(), items.end(), item) != items.end())
            continue;
        items.push_back(item);
        Row first = line(s.effects[i].text);
        first.divider_before = right.rows.size() > before;
        right.rows.push_back(first);
        if (!item.empty())
            for (int j = i + 1; j < s.effect_count; j++)
                if (item == s.effects[j].item)
                    right.rows.push_back(line(s.effects[j].text));
    }
    // The following rates are totals across equipment, so keep them in their own group.
    size_t rates_begin = right.rows.size();
    if (s.hp_regen_period)
        right.rows.push_back(line(format("HP +1 every %.1f s", seconds(s.hp_regen_period)), colour_good));
    if (s.hp_drain_period)
        right.rows.push_back(line(format("HP -1 every %.1f s", seconds(s.hp_drain_period)), colour_bad));
    if (s.mp_regen_period)
        right.rows.push_back(line(format("MP +1 every %.1f s", seconds(s.mp_regen_period)), colour_good));
    if (s.mp_drain_period)
        right.rows.push_back(line(format("MP -1 every %.1f s", seconds(s.mp_drain_period)), colour_bad));
    if (rates_begin > before && right.rows.size() > rates_begin)
        right.rows[rates_begin].divider_before = true;
    if (right.rows.size() == before)
        right.rows.push_back(line("none", colour_label));

    // one size for the three, the largest each allows, but not under the least
    float most = 11.0f * f.sy, least = 6.5f * f.sy, size = most;
    for (const Block *b : {&top, &left})
        size = std::min(size, fit(*b, f, most, least));
    float effects = fit(right, f, size, least); // the effects may be smaller still
    draw_block(dl, f, top, size, alpha);
    draw_block(dl, f, left, size, alpha);
    draw_block(dl, f, right, std::min(size, effects), alpha);
}

void item(ImDrawList *dl, const Frame &f, const PortStatsSnapshot &s, float alpha) {
    const PortStatsMenu &m = s.menu;
    if (m.item < 0 || m.item_line_count == 0)
        return;
    Block b;
    b.box = item_box;
    b.centre = true;
    b.shade = true;
    for (int i = 0; i < m.item_line_count; i++)
        b.rows.push_back(line(m.item_lines[i]));
    b.no_wrap = true;
    float size = fit(b, f, 14.0f * f.sy, 7.0f * f.sy);
    draw_block(dl, f, b, size, alpha);
}

float shown_alpha; // eased toward the screen's fade, so a panel coming or going does not pop
int shown_screen;

} // namespace

void stats_menus_fonts() {
    ImGuiIO &io = ImGui::GetIO();
    // the player menu's Mincho (port/ui/res/fonts), the game's own letters' font, the same on every computer; the
    // overlay's own font stays the default (the first)
#ifdef KF4_HAVE_PLAYER_UI
    if (const UiAsset *mincho = ui_asset("fonts/HolylandMincho.ttf")) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false; // (the game's own array, never freed)
        serif = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(mincho->data), int(mincho->size), 24.0f,
                                               &config);
        if (serif != nullptr)
            return;
    }
#endif
    // else a serif from the system, like the game's menu text
    static const char *const candidates[] = {
        "C:/Windows/Fonts/timesbd.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/liberation-serif/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
        "/usr/share/fonts/dejavu-serif-fonts/DejaVuSerif.ttf",
        "/usr/share/fonts/TTF/DejaVuSerif.ttf",
    };
    for (const char *path : candidates) {
        if (FILE *file = std::fopen(path, "rb")) {
            std::fclose(file);
            serif = io.Fonts->AddFontFromFileTTF(path, 24.0f);
            if (serif != nullptr)
                return;
        }
    }
}

ImFont *stats_menus_font() {
    return serif;
}

bool stats_menus_wanted(const PortStatsSnapshot &s) {
    return port_stats_options.menus && s.valid && s.menu.screen != PORT_STATS_MENU_NONE;
}

void stats_menus_step(const PortStatsSnapshot &s) {
    float target = 0.0f;
    if (stats_menus_wanted(s) && !s.menu.covered)
        target = s.menu.shown;
    if (s.menu.screen != shown_screen) { // a new screen starts from its own fade
        shown_screen = s.menu.screen;
        shown_alpha = std::min(shown_alpha, target);
    }
    // the game's parts fade by a tenth a menu frame: follow them, a vblank at a time (not a picture at a
    // time, which would be four times as fast at 240 pictures a second)
    if (shown_alpha < target)
        shown_alpha = std::min(target, shown_alpha + 0.1f);
    else
        shown_alpha = std::max(target, shown_alpha - 0.2f);
}

void stats_menus_draw(const PortStatsSnapshot &s, int x, int y, int width, int height) {
    if (shown_alpha <= 0.01f || !stats_menus_wanted(s))
        return;
    Frame f = frame_of(x, y, width, height);
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    switch (s.menu.screen) {
    case PORT_STATS_MENU_MAIN:
        main_or_status(dl, f, s, shown_alpha, false);
        break;
    case PORT_STATS_MENU_STATUS:
        main_or_status(dl, f, s, shown_alpha, true);
        break;
    case PORT_STATS_MENU_EQUIP:
        equipment(dl, f, s, shown_alpha);
        break;
    case PORT_STATS_MENU_ITEMS:
    case PORT_STATS_MENU_ARRANGE:
        item(dl, f, s, shown_alpha);
        break;
    }
}
