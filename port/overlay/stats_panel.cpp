// The extended stats mod's drawing (port/include/port/stats.h, docs/port/EXTENDED_STATS.md), with Dear
// ImGui inside the settings overlay's frame (port/overlay/overlay.cpp calls in at four places):
//   - its lines in the game's own menus (port/overlay/stats_menus.cpp), shown whenever the menu is up;
//   - the ailments and timed effects running, by the minimap while playing (port/overlay/stats_timers.cpp);
//   - the stats panel: what the game keeps about the player and never shows, with bars toward the next
//     level, the next point of Physical and Magic Power, the weapon's and the spells' levels, and the
//     equipment's wear; shown and hidden by its key (F6);
//   - the hit log (bottom left: each hit dealt and taken, the real damage) and the health bar of the
//     creature last hit (top middle);
//   - its section of the Gameplay page.
// Everything runs on the game's thread at the vblank. The panel never takes the keyboard, mouse or
// controller from the game: it takes input only while the overlay is open (to move it).
//
// KF4_STATS_PANEL_AT=vblank opens the panel at that vblank (tests: with KF4_OVERLAY_SHOT=closed@N its
// picture is taken with the game's).
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "imgui.h"

#include "stats_menus.h"
#include "stats_timers.h"

extern "C" {
#include "port/overlay.h"
#include "port/platform.h"
#include "port/settings.h"
#include "port/stats.h"
}

namespace {

const ImVec4 colour_title(0.95f, 0.8f, 0.45f, 1.0f);
const ImVec4 colour_dim(0.62f, 0.62f, 0.62f, 1.0f);
const ImVec4 colour_good(0.55f, 0.85f, 0.55f, 1.0f);
const ImVec4 colour_bad(0.95f, 0.45f, 0.4f, 1.0f);

const char *const element_names[9] = {"Slash", "Strike", "Stab", "Fire", "Earth", "Wind", "Water", "Light", "Dark"};
// the port's names for the six timed ailments, by what each does (docs/port/EXTENDED_STATS.md)
const char *const ailment_names[6] = {"Poison", "Paralysis", "Darkness", "Curse", "Slow", "Magic seal"};
const char *const slot_names[PORT_STATS_SLOTS] = {"Weapon", "Shield", "Helm",     "Armour", "Gloves",
                                                  "Boots",  "Ring",   "Bracelet", "Amulet"};

const unsigned long long LOG_VBLANKS = 60 * 5;    // a hit stays in the log 5 seconds
const unsigned long long TARGET_VBLANKS = 60 * 6; // the target's bar, 6 seconds after the last hit
const int LOG_LINES = 7;

PortStatsSnapshot snap;   // the game's thread's, read at the vblank (port_stats_overlay_update)
std::atomic<int> wanted;  // something of the mod is drawn this vblank
unsigned long long open_at = ~0ull;
bool open_at_read;

float seconds(int frames) {
    return (float)frames / (float)PORT_STATS_GAME_FPS;
}

void bar(float fraction, const char *text, const ImVec4 &colour) {
    if (fraction < 0.0f)
        fraction = 0.0f;
    if (fraction > 1.0f)
        fraction = 1.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, ImGui::GetFontSize() * 1.15f), text);
    ImGui::PopStyleColor();
}

void title(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour_title);
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}

void level_section(const PortStatsSnapshot &s) {
    char text[96];

    title("Level");
    ImGui::Text("Level %d", s.level);
    ImGui::SameLine();
    ImGui::TextColored(colour_dim, "experience %d%s", s.experience, s.fortune ? " (x2 now)" : "");
    if (s.level_to > 0) {
        int span = s.level_to - s.level_from;
        std::snprintf(text, sizeof(text), "%d to level %d", s.level_to - s.experience, s.level + 1);
        bar(span > 0 ? (float)(s.experience - s.level_from) / (float)span : 1.0f, text, ImVec4(0.75f, 0.6f, 0.25f, 1));
        ImGui::TextColored(colour_dim, "Next: HP +%d  MP +%d  Physical +%d  Magic +%d", s.next_hp, s.next_mp,
                           s.next_power, s.next_magic);
    } else {
        ImGui::TextColored(colour_dim, "The last level");
    }
}

void power_section(const PortStatsSnapshot &s) {
    char text[96];

    title("Physical and Magic Power");
    ImGui::Text("Physical %d", s.power_base);
    if (s.power != s.power_base) {
        ImGui::SameLine();
        ImGui::TextColored(colour_dim, "(%d in effect)", s.power);
    }
    if (s.power_base < 999) {
        std::snprintf(text, sizeof(text), "%d / 100 weapon hits", s.power_uses);
        bar((float)s.power_uses / 100.0f, text, ImVec4(0.7f, 0.35f, 0.3f, 1));
    }
    ImGui::Text("Magic %d", s.magic_base);
    if (s.magic != s.magic_base) {
        ImGui::SameLine();
        ImGui::TextColored(colour_dim, "(%d in effect)", s.magic);
    }
    if (s.magic_base < 999) {
        std::snprintf(text, sizeof(text), "%d / 100 spells", s.magic_uses);
        bar((float)s.magic_uses / 100.0f, text, ImVec4(0.35f, 0.4f, 0.8f, 1));
    }
    if (s.power_fill_frames > 0)
        ImGui::TextColored(colour_dim, "Weapon gauge fills in %.1f s", seconds(s.power_fill_frames));
    if (s.magic_fill_frames > 0)
        ImGui::TextColored(colour_dim, "Magic gauge (%s) fills in %.1f s", s.charging_magic, seconds(s.magic_fill_frames));
    ImGui::TextColored(s.weight > s.max_weight ? colour_bad : colour_dim, "Weight %d.%d / %d.%d lb%s", s.weight / 10,
                       s.weight % 10, s.max_weight / 10, s.max_weight % 10,
                       s.weight > s.max_weight ? ": slowed, no running" : "");
}

void weapon_section(const PortStatsSnapshot &s) {
    const PortStatsItem &w = s.items[0];
    char text[96];

    if (w.none)
        return;
    title("Weapon level");
    ImGui::Text("%s", w.name);
    ImGui::SameLine();
    ImGui::TextColored(colour_dim, "level %d / 3", w.weapon_level);
    // Player_CountWeaponUse: the level rises at 180 and 480 hits
    if (w.weapon_level < 3) {
        int from = w.weapon_level == 1 ? 0 : 180, to = w.weapon_level == 1 ? 180 : 480;
        std::snprintf(text, sizeof(text), "%d / %d hits", w.weapon_uses, to);
        bar((float)(w.weapon_uses - from) / (float)(to - from), text, ImVec4(0.7f, 0.55f, 0.3f, 1));
    }
}

void spells_section(const PortStatsSnapshot &s) {
    char text[64];

    if (s.spell_count == 0)
        return;
    title("Spell levels");
    if (ImGui::BeginTable("spells", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (int i = 0; i < s.spell_count; i++) {
            const PortStatsSpell &sp = s.spells[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", sp.name);
            ImGui::TableNextColumn();
            // Player_CountMagicUse: level 2 at 50 uses, 3 at 150
            if (sp.level >= 3) {
                ImGui::TextColored(colour_dim, "level 3");
            } else {
                int from = sp.level == 1 ? 0 : 50, to = sp.level == 1 ? 50 : 150;
                std::snprintf(text, sizeof(text), "%d: %d / %d", sp.level, sp.experience, to);
                bar((float)(sp.experience - from) / (float)(to - from), text, ImVec4(0.35f, 0.4f, 0.8f, 1));
            }
        }
        ImGui::EndTable();
    }
}

void durability_section(const PortStatsSnapshot &s) {
    char text[64];

    title("Wear");
    if (ImGui::BeginTable("wear", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (int i = 0; i < PORT_STATS_SLOTS; i++) {
            const PortStatsItem &it = s.items[i];
            if (it.none || !it.wears)
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", it.name);
            ImGui::TableNextColumn();
            // LevelRecord.level: its values count at that percent; it wears down to 50
            if (it.wear_per_point > 0 && it.durability > 50)
                std::snprintf(text, sizeof(text), "%d%%  (%d/%d)", it.durability, it.wear, it.wear_per_point);
            else
                std::snprintf(text, sizeof(text), "%d%%", it.durability);
            ImVec4 colour = it.durability >= 90 ? ImVec4(0.4f, 0.65f, 0.4f, 1)
                            : it.durability >= 70 ? ImVec4(0.75f, 0.65f, 0.3f, 1)
                                                  : ImVec4(0.8f, 0.35f, 0.3f, 1);
            bar((float)(it.durability - 50) / 50.0f, text, colour);
        }
        ImGui::EndTable();
    }
}

void ailment_section(const PortStatsSnapshot &s) {
    title("Ailments");
    if (ImGui::BeginTable("ailments", 3, ImGuiTableFlags_SizingStretchProp)) {
        for (int i = 0; i < 6; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", ailment_names[i]);
            ImGui::TableNextColumn();
            ImGui::TextColored(s.ailment_defence[i] > 0 ? colour_good : colour_dim, "defence %d%%", s.ailment_defence[i]);
            ImGui::TableNextColumn();
            if (s.ailment_frames[i] > 0) {
                // timed_flags wear by ailment_wear_thirds / 3 a frame (Demon Armor, Purity Amulet)
                int frames = s.ailment_frames[i] * 3 / (s.ailment_wear_thirds > 0 ? s.ailment_wear_thirds : 3);
                ImGui::TextColored(colour_bad, "%.0f s left", seconds(frames));
            }
        }
        ImGui::EndTable();
    }
    if (s.ailment_wear_thirds != 3)
        ImGui::TextColored(colour_dim, "Ailments wear off %s", s.ailment_wear_thirds < 3 ? "1.5x slower" : "faster (x0.75)");
}

void body_section(const PortStatsSnapshot &s) {
    bool any = s.hp_regen_period || s.hp_drain_period || s.mp_regen_period || s.mp_drain_period || s.damage_to_hp ||
               s.damage_to_mp || s.drain_curse || s.fire_taken_percent != 100;
    if (!any)
        return;
    title("Recovery and drain");
    // Player_UpdateStatus: one point every period frames (150 / the items' total, Player_SumWornBonuses)
    if (s.hp_regen_period)
        ImGui::TextColored(colour_good, "HP +1 every %.1f s", seconds(s.hp_regen_period));
    if (s.hp_drain_period)
        ImGui::TextColored(colour_bad, "HP -1 every %.1f s", seconds(s.hp_drain_period));
    if (s.mp_regen_period)
        ImGui::TextColored(colour_good, "MP +1 every %.1f s", seconds(s.mp_regen_period));
    if (s.mp_drain_period)
        ImGui::TextColored(colour_bad, "MP -1 every %.1f s", seconds(s.mp_drain_period));
    if (s.damage_to_hp)
        ImGui::TextColored(colour_good, "%d%% of damage dealt heals HP", s.damage_to_hp);
    if (s.damage_to_mp)
        ImGui::TextColored(colour_good, "%d%% of damage dealt restores MP", s.damage_to_mp);
    if (s.drain_curse)
        ImGui::TextColored(colour_bad, "Drained: Physical and Magic halved, HP and MP -1 a second");
    if (s.fire_taken_percent != 100)
        ImGui::TextColored(colour_bad, "Fire damage taken %d%%", s.fire_taken_percent);
}

void effects_section(const PortStatsSnapshot &s) {
    if (s.effect_count == 0 && s.timer_count == 0)
        return;
    title("Hidden effects");
    for (int i = 0; i < s.effect_count; i++) {
        const PortStatsEffect &e = s.effects[i];
        ImGui::TextColored(colour_dim, "%s:", e.item);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", e.text);
    }
    for (int i = 0; i < s.timer_count; i++) {
        const PortStatsTimer &t = s.timers[i];
        ImGui::Text("%s", t.text);
        ImGui::SameLine();
        ImGui::TextColored(colour_dim, "%.0f s", seconds(t.frames));
    }
}

void panel(bool overlay_open) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    float column = em * 17.0f;

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - em * 0.6f, viewport->WorkPos.y + em * 0.6f),
                            ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.86f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoCollapse;
    if (!overlay_open)
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav;
    if (!ImGui::Begin("Extended stats", nullptr, flags)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTable("columns", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthFixed, column);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthFixed, column);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        level_section(snap);
        power_section(snap);
        weapon_section(snap);
        spells_section(snap);
        ImGui::TableNextColumn();
        if (port_stats_options.durability)
            durability_section(snap);
        ailment_section(snap);
        body_section(snap);
        effects_section(snap);
        ImGui::EndTable();
    }
    ImGui::End();
}

void hit_log() {
    PortStatsHit hits[LOG_LINES];
    unsigned long long now = port_vblank_count();
    int n = port_stats_hits(hits, LOG_LINES, now > LOG_VBLANKS ? now - LOG_VBLANKS : 0);
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    ImFont *font = ImGui::GetFont();
    float size = viewport->Size.y / 34.0f > 15.0f ? viewport->Size.y / 34.0f : 15.0f;
    float x = viewport->Pos.x + size * 0.8f;
    float y = viewport->Pos.y + viewport->Size.y - size * 1.6f;

    for (int i = n - 1; i >= 0; i--) {
        const PortStatsHit &h = hits[i];
        unsigned long long age = now - h.vblank;
        float alpha = age + 40 > LOG_VBLANKS ? (float)(LOG_VBLANKS - age) / 40.0f : 1.0f;
        char text[96];
        ImU32 colour;
        if (h.kind == PORT_STATS_HIT_DEALT) {
            std::snprintf(text, sizeof(text), "%d damage%s  (%d/%d)", h.amount, h.killed ? ", killed" : "", h.left, h.max);
            colour = IM_COL32(255, 235, 170, (int)(255 * alpha));
        } else {
            std::snprintf(text, sizeof(text), "-%d HP%s  (%d/%d)", h.amount,
                          h.kind == PORT_STATS_HIT_OTHER ? " (not a hit)" : "", h.left, h.max);
            colour = h.kind == PORT_STATS_HIT_TAKEN ? IM_COL32(255, 120, 110, (int)(255 * alpha))
                                                    : IM_COL32(200, 150, 230, (int)(255 * alpha));
        }
        ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        draw->AddRectFilled(ImVec2(x - size * 0.3f, y - size * 0.1f), ImVec2(x + extent.x + size * 0.3f, y + extent.y + size * 0.1f),
                            IM_COL32(0, 0, 0, (int)(130 * alpha)), size * 0.2f);
        draw->AddText(font, size, ImVec2(x, y), colour, text);
        y -= size * 1.3f;
    }
}

void target_bar() {
    PortStatsHit t;
    if (!port_stats_target(&t, TARGET_VBLANKS) || t.max <= 0)
        return;
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    ImFont *font = ImGui::GetFont();
    float size = viewport->Size.y / 36.0f > 14.0f ? viewport->Size.y / 36.0f : 14.0f;
    float width = viewport->Size.x * 0.3f, height = size * 0.7f;
    float x = viewport->Pos.x + (viewport->Size.x - width) * 0.5f, y = viewport->Pos.y + size * 1.2f;
    float fraction = (float)t.left / (float)t.max;
    char text[64];

    if (fraction > 1.0f)
        fraction = 1.0f;
    draw->AddRectFilled(ImVec2(x - 2, y - 2), ImVec2(x + width + 2, y + height + 2), IM_COL32(0, 0, 0, 170), 3.0f);
    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width * fraction, y + height), IM_COL32(190, 50, 40, 230), 2.0f);
    std::snprintf(text, sizeof(text), "%d / %d", t.left, t.max);
    ImVec2 extent = font->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.0f, text);
    draw->AddText(font, size * 0.8f, ImVec2(x + (width - extent.x) * 0.5f, y + height + size * 0.15f),
                  IM_COL32(255, 235, 200, 255), text);
}

bool panel_shown() {
    return port_stats_options.on && port_stats_options.panel && snap.valid && port_stats_panel_open();
}

} // namespace

// Once a vblank on the game's thread, before the overlay decides whether it draws: the hits watched,
// the game's state read, and whether anything of the mod is drawn
extern "C" void port_stats_overlay_update(void) {
    port_stats_track();
    if (!port_stats_options.on) {
        wanted = 0;
        return;
    }
    if (!open_at_read) {
        const char *at = std::getenv("KF4_STATS_PANEL_AT");
        if (at != nullptr)
            open_at = std::strtoull(at, nullptr, 0);
        open_at_read = true;
    }
    if (open_at != ~0ull && port_vblank_count() >= open_at) {
        port_stats_set_panel_open(1);
        open_at = ~0ull;
    }
    port_stats_sample(&snap);
    stats_menus_step(snap);
    stats_timers_step(snap);
    int want = panel_shown() || stats_menus_wanted(snap) || stats_timers_wanted();
    if (!want && port_stats_options.damage && snap.valid) {
        PortStatsHit h;
        unsigned long long now = port_vblank_count();
        want = port_stats_hits(&h, 1, now > LOG_VBLANKS ? now - LOG_VBLANKS : 0) > 0 ||
               port_stats_target(&h, TARGET_VBLANKS);
    }
    static const bool log = std::getenv("KF4_STATS_LOG") != nullptr;
    if (log && want != wanted.load(std::memory_order_relaxed))
        std::fprintf(stderr, "[stats] vblank %llu: the mod %s\n", port_vblank_count(),
                     want ? "draws (the overlay is up)" : "draws nothing");
    wanted = want;
}

// Any thread (the pictures' thread asks whether the overlay is up): only the flag the vblank set
extern "C" int port_stats_overlay_wants(void) {
    return wanted.load(std::memory_order_relaxed);
}

extern "C" void port_stats_overlay_fonts(void) {
    stats_menus_fonts();
}

extern "C" void port_stats_overlay_draw(int overlay_open, int x, int y, int width, int height) {
    if (!port_stats_options.on || !snap.valid)
        return;
    uint32_t mxcsr = port_host_float_begin(); // the numbers are formatted in the host's rounding
    stats_menus_draw(snap, x, y, width, height);
    stats_timers_draw(x, y, width, height);
    if (port_stats_options.damage && !snap.in_menu) {
        hit_log();
        target_bar();
    }
    if (panel_shown())
        panel(overlay_open != 0);
    port_host_float_end(mxcsr);
}

extern "C" void port_stats_overlay_mods(void) {
    PortStatsOptions &o = port_stats_options;
    bool on = o.on != 0;

    ImGui::SeparatorText("Extended stats");
    if (ImGui::Checkbox("Extended stats in the game's menus", &on)) {
        port_stats_set_on(on);
        o.menus = 1; // the menus' lines are what the mod is (extended_stats_menus stays in settings.json)
        port_settings_changed();
    }
    ImGui::SetItemTooltip("What the game keeps about you and never shows, in its own menus. It only reads the game.\n"
                          "(--extended-stats)");
    if (o.forced) {
        ImGui::SameLine();
        ImGui::TextColored(colour_title, "(on for this run: command line)");
    }
    ImGui::Indent();
    ImGui::PushStyleColor(ImGuiCol_Text, colour_dim);
    ImGui::TextWrapped("The main menu and Status: the experience to the next level, the hits and spells to the next "
                       "Physical and Magical point, the weapon's level, the ailments' time left. Equipment: the durability "
                       "of each piece, the weapon's level, the hidden effects of what you wear. Item and Arrange: what "
                       "the item under the cursor does.");
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(!o.on);
    bool panel = o.panel != 0, damage = o.damage != 0, durability = o.durability != 0, timers = o.timers != 0;
    if (ImGui::Checkbox("Equipment durability", &durability)) {
        o.durability = durability;
        port_settings_changed();
    }
    ImGui::SetItemTooltip("The percent each piece's values count at, worn down by hits (the smith mends it).");
    if (ImGui::Checkbox("Ailment timers on screen", &timers)) {
        o.timers = timers;
        port_settings_changed();
    }
    ImGui::SetItemTooltip("While you play, the ailments running and the timed effects of spells and items (Power\n"
                          "Seed, Widda Needle ...), each with its seconds left, beside the minimap (above it in a\n"
                          "bottom corner, below it in a top one), or in its corner with the minimap off.");
    if (ImGui::Checkbox("Hit log and the target's health", &damage)) {
        o.damage = damage;
        port_settings_changed();
    }
    ImGui::SetItemTooltip("Each hit you deal or take, with its real damage and the HP left (bottom left), and the\n"
                          "health bar of the creature you last hit (top).");
    if (ImGui::Checkbox("Stats panel on its key (F6)", &panel)) {
        o.panel = panel;
        port_settings_changed();
    }
    ImGui::SetItemTooltip("Everything in one free-floating panel, shown and hidden by its key (F6, rebind it on the Controls\n"
                          "page: \"Show / hide the stats panel\"); it can be moved while the overlay is open.");
    ImGui::SameLine();
    bool open = port_stats_panel_open() != 0;
    ImGui::BeginDisabled(!o.panel);
    if (ImGui::Checkbox("Shown now", &open))
        port_stats_set_panel_open(open);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::Unindent();
}
