// The player menu's settings, as data (port/ui/settings_rows.hpp). Each row's get / set does what the settings
// overlay's control did (port/overlay/overlay.cpp), so settings.json and the command line work as they did.
#include "settings_rows.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string_view>

extern "C" {
#include "port/antialias.h"
#include "port/controls.h"
#include "port/host.h"
#include "port/live_settings.h"
#include "port/platform.h"
#include "port/quicksave.h"
#include "port/settings.h"
#include "port/twin_stick.h"
}

namespace ui {

namespace {

void changed() {
    port_settings_changed();
}

const Choice on_off[] = {{"Off", 0}, {"On", 1}};

// ---- Display --------------------------------------------------------------------------------------------------------
int get_fullscreen() {
    return port_settings.fullscreen != 0;
}
void set_fullscreen(int on) {
    port_settings.fullscreen = on;
    port_video_apply_settings();
    changed();
}

const Choice shapes[] = {{"Keep its shape", ASPECT_4_3}, {"Stretch to the window", ASPECT_STRETCH}};
int get_shape() {
    return port_settings.aspect;
}
void set_shape(int aspect) {
    port_settings.aspect = aspect;
    changed();
}

const Choice scalings[] = {
    {"Sharp pixels", PORT_SCALING_SHARP}, {"Smooth", PORT_SCALING_SMOOTH}, {"Best", PORT_SCALING_BEST}};
int get_scaling() {
    return port_settings.scaling;
}
void set_scaling(int scaling) {
    port_settings.scaling = scaling;
    port_settings.smooth = scaling != PORT_SCALING_SHARP;
    changed();
}
bool scaling_overridden() {
    return port_options.present_scaling >= 0 && port_options.present_scaling != port_settings.scaling;
}

const Choice frame_rates[] = {{"30, as on the PS2", 30}, {"60", 60},   {"120", 120},
                              {"144", 144},              {"165", 165}, {"240", 240},
                              {"The screen's rate", PORT_FRAME_RATE_MONITOR}, {"Unlocked", PORT_FRAME_RATE_UNLOCKED}};
int get_frame_rate() {
    return port_options.frame_rate;
}
void set_frame_rate(int rate) {
    port_live_set(PORT_LIVE_FRAME_RATE, rate);
}
void describe_frame_rate(int rate, char *out, size_t size) {
    if (rate == PORT_FRAME_RATE_MONITOR) {
        std::snprintf(out, size, "The screen's rate (%d)", port_frame_rate_hz());
    } else if (rate == PORT_FRAME_RATE_UNLOCKED) {
        std::snprintf(out, size, "Unlocked (%.0f now)", port_video_pictures_per_second());
    } else {
        out[0] = 0; // (the choice's label)
        (void)size;
    }
}
bool frame_rate_overridden() {
    return port_options.frame_rate != port_settings.frame_rate;
}

bool unlocked() {
    return port_options.frame_rate == PORT_FRAME_RATE_UNLOCKED;
}
int get_cap() {
    return port_options.frame_rate_cap;
}
void set_cap(int cap) {
    cap = cap > 0 && cap < 60 ? 60 : cap;
    port_settings.frame_rate_cap = port_options.frame_rate_cap = cap;
    changed();
}
void describe_cap(int cap, char *out, size_t size) {
    if (cap > 0) {
        std::snprintf(out, size, "%d a second", cap);
    } else {
        std::snprintf(out, size, "None");
    }
}

// VSync, adaptive sync and unlocked's tearing as one choice; settings.json keeps the three as they were
enum Sync { SYNC_OFF, SYNC_VSYNC, SYNC_ADAPTIVE };
const Choice syncs[] = {{"Off", SYNC_OFF}, {"VSync", SYNC_VSYNC}, {"Adaptive sync", SYNC_ADAPTIVE}};
int get_sync() {
    if (port_options.adaptive_sync) {
        return SYNC_ADAPTIVE;
    }
    if (port_options.frame_rate == PORT_FRAME_RATE_UNLOCKED) {
        return port_options.tearing ? SYNC_OFF : SYNC_VSYNC;
    }
    return port_settings.vsync ? SYNC_VSYNC : SYNC_OFF;
}
void set_sync(int sync) {
    PortSettings &s = port_settings;
    s.adaptive_sync = port_options.adaptive_sync = sync == SYNC_ADAPTIVE;
    if (sync != SYNC_ADAPTIVE) {
        s.vsync = sync == SYNC_VSYNC;
        s.tearing = port_options.tearing = sync == SYNC_OFF;
    }
    port_video_apply_settings();
    changed();
}
bool sync_overridden() {
    return port_options.adaptive_sync != port_settings.adaptive_sync;
}

const SettingRow display_rows[] = {
    {"Window", "", RowKind::heading},
    {"Full screen", "The game on the whole screen, borderless, at the screen's own resolution. F11 also switches it.",
     RowKind::toggle, on_off, {}, get_fullscreen, set_fullscreen},
    {"Picture shape",
     "Keep its shape: the picture keeps 4:3 (or 16:9 with widescreen), with black bars at the sides or top.\n"
     "Stretch: the picture fills the window, and is distorted.",
     RowKind::choice, shapes, {}, get_shape, set_shape},
    {"Scaling",
     "How the picture is fitted to the window.\nSharp pixels: each pixel repeated, blocky.\nSmooth: bilinear "
     "filtering.\nBest: a cubic filter that keeps edges sharp when made larger and averages every pixel when made "
     "smaller.",
     RowKind::choice, scalings, {}, get_scaling, set_scaling, nullptr, nullptr, nullptr, nullptr, scaling_overridden},
    {"Frame rate", "", RowKind::heading},
    {"Pictures a second",
     "30: the PS2's.\nMore: pictures are made between each two of the game's, so turning and walking are smooth. The "
     "game still runs at 30, and what it does is the same.\nUnlocked: as many as your computer keeps up with.",
     RowKind::choice, frame_rates, {}, get_frame_rate, set_frame_rate, nullptr, describe_frame_rate, nullptr, nullptr,
     frame_rate_overridden},
    {"Unlocked's limit", "Unlocked never goes faster than this. None: no limit.", RowKind::number, {}, {0, 2000, 10},
     get_cap, set_cap, nullptr, describe_cap, unlocked},
    {"Sync",
     "Off: each picture is shown at once, which can tear.\nVSync: each picture waits for the screen's refresh: no "
     "tearing.\nAdaptive sync: for a FreeSync or G-Sync screen, which refreshes when each picture comes. It must be "
     "on in your system's display settings too.",
     RowKind::choice, syncs, {}, get_sync, set_sync, nullptr, nullptr, nullptr, nullptr, sync_overridden},
};

// ---- Graphics -------------------------------------------------------------------------------------------------------
int get_widescreen() {
    return port_options.widescreen != 0;
}
void set_widescreen(int on) {
    port_live_set(PORT_LIVE_WIDESCREEN, on);
}

const Choice huds[] = {{"In the corner", 0}, {"Centred, as at 4:3", 1}};
bool widescreen_on() {
    return port_options.widescreen != 0;
}
int get_hud() {
    return port_options.widescreen_hud_centred != 0;
}
void set_hud(int centred) {
    port_settings.widescreen_hud_centred = port_options.widescreen_hud_centred = centred;
    changed();
}

bool pc_renderer() {
    return port_options.renderer != PORT_RENDERER_GS;
}
const Choice scales[] = {{"Auto", 0}, {"1x (640 x 448)", 1}, {"2x (1280 x 896)", 2}, {"3x (1920 x 1344)", 3},
                         {"4x (2560 x 1792)", 4}};
int get_scale() {
    return port_options.native_scale;
}
void set_scale(int scale) {
    port_live_set(PORT_LIVE_NATIVE_SCALE, scale);
}
void describe_scale(int scale, char *out, size_t size) {
    if (scale >= 1) {
        std::snprintf(out, size, "%dx (%d x %d)", scale, 640 * scale, 448 * scale);
    } else {
        std::snprintf(out, size, "Auto (%dx)", port_gs_native_scale());
    }
}

// (in port/antialias.h's order: a method is one entry there)
const std::array<Choice, PORT_AA_COUNT> antialiases = [] {
    std::array<Choice, PORT_AA_COUNT> choices = {};
    for (int m = 0; m < PORT_AA_COUNT; m++) {
        choices[size_t(m)] = {port_antialias(m)->label, m};
    }
    return choices;
}();
int get_antialias() {
    return port_options.antialias;
}
void set_antialias(int method) {
    port_live_set(PORT_LIVE_ANTIALIAS, method);
}
bool antialias_overridden() {
    return port_options.antialias != port_settings.antialias;
}

int get_sharpen() {
    return int(std::lround(port_settings.sharpen * 100.0f));
}
void set_sharpen(int percent) {
    port_settings.sharpen = float(percent) / 100.0f;
    changed();
}
void describe_sharpen(int percent, char *out, size_t size) {
    if (percent > 0) {
        std::snprintf(out, size, "%d%%", percent);
    } else {
        std::snprintf(out, size, "Off");
    }
}

// The texture detail (mipmap level) and anisotropic filtering as one choice: value lod * 100 + aniso
const Choice texture_qualities[] = {
    {"PS2's", 0 * 100 + 1}, {"Standard", 1 * 100 + 1}, {"High", 1 * 100 + 16}, {"Maximum", 2 * 100 + 16}};
int get_textures() {
    return port_options.native_lod * 100 + port_options.native_aniso;
}
void set_textures(int value) {
    port_gs_set_native_lod(value / 100);
    port_gs_set_native_aniso(value % 100);
    port_settings.native_lod = value / 100;
    port_settings.native_aniso = port_options.native_aniso;
    changed();
}
void describe_textures(int value, char *out, size_t size) {
    out[0] = 0; // (the choice's label, when one is this)
    for (const Choice &choice : texture_qualities) {
        if (choice.value == value) {
            return;
        }
    }
    std::snprintf(out, size, "Custom (settings file)");
}

const Choice distances[] = {{"1x, as on the PS2", PORT_DRAW_DISTANCE_1},
                            {"1.5x", PORT_DRAW_DISTANCE_1_5},
                            {"2x", PORT_DRAW_DISTANCE_2},
                            {"4x", PORT_DRAW_DISTANCE_4},
                            {"No distance fog", PORT_DRAW_DISTANCE_MAX}};
int get_distance() {
    return port_options.draw_distance;
}
void set_distance(int distance) {
    port_live_set(PORT_LIVE_DRAW_DISTANCE, distance);
}
bool distance_overridden() {
    return port_options.draw_distance != port_settings.draw_distance;
}
bool distance_longer() {
    return port_options.draw_distance != PORT_DRAW_DISTANCE_1;
}
int get_sky_fog() {
    return port_options.draw_distance_sky != 0;
}
void set_sky_fog(int on) {
    port_settings.draw_distance_sky = port_options.draw_distance_sky = on;
    changed();
}

const Choice skies[] = {{"Modern", PORT_SKY_MODERN}, {"As on the PS2", PORT_SKY_PS2}};
int get_sky() {
    return port_options.native_sky;
}
void set_sky(int sky) {
    port_live_set(PORT_LIVE_SKY, sky);
}
bool sky_overridden() {
    return port_options.native_sky != port_settings.native_sky;
}

int get_reflection_clip() {
    return port_options.reflection_clip != 0;
}
void set_reflection_clip(int on) {
    port_settings.reflection_clip = port_options.reflection_clip = on;
    changed();
}
bool reflection_clip_overridden() {
    return port_options.reflection_clip != port_settings.reflection_clip;
}

const SettingRow graphics_rows[] = {
    {"Picture", "", RowKind::heading},
    {"Widescreen (16:9)",
     "Off: the PS2's 4:3 picture.\nOn: the view is widened to 16:9, with more at the sides. The menus, messages "
     "and the HUD keep their 4:3 shape.",
     RowKind::toggle, on_off, {}, get_widescreen, set_widescreen},
    {"HUD", "Where the widescreen picture puts the gauges of the top left.", RowKind::choice, huds, {}, get_hud,
     set_hud, nullptr, nullptr, widescreen_on},
    {"Resolution",
     "The game is drawn at this many times the PS2's 640 x 448.\nAuto: the nearest to the height the window shows "
     "the picture at (1080: 2x, 1440: 3x, 2160: 4x).",
     RowKind::choice, scales, {}, get_scale, set_scale, nullptr, describe_scale, pc_renderer},
    {"Anti-aliasing",
     "Smooths the stair-steps along edges.\nFXAA: cheap, softens fine detail a little.\nSMAA: sharper, costs a "
     "little more.\nSMAA T2x: steadier thin edges in motion.",
     RowKind::choice, antialiases, {}, get_antialias, set_antialias, nullptr, nullptr, pc_renderer, nullptr,
     antialias_overridden},
    {"Sharpen", "Crisper fine detail after the picture is scaled to the window; brings back texture detail that "
                "anti-aliasing softens.",
     RowKind::number, {}, {0, 100, 5}, get_sharpen, set_sharpen, nullptr, describe_sharpen, pc_renderer},
    {"Texture quality",
     "How sharp textures stay with distance.\nPS2's: blurred far away at higher resolutions.\nStandard: as the PS2 "
     "would at this resolution.\nHigh: also sharp at grazing angles (anisotropic filtering).\nMaximum: the finest "
     "everywhere; far surfaces can shimmer.",
     RowKind::choice, texture_qualities, {}, get_textures, set_textures, nullptr, describe_textures, pc_renderer},
    {"World", "", RowKind::heading},
    {"Draw distance",
     "How far you see through the fog.\n1x: the PS2's fog.\nFurther: the fog starts and ends that much further "
     "away.\nNo distance fog: also removes the mist that is part of some places' look.",
     RowKind::choice, distances, {}, get_distance, set_distance, nullptr, nullptr, nullptr, nullptr,
     distance_overridden},
    {"The sky's fog too", "Push the sky's fog out with the draw distance, which lightens dark skies.",
     RowKind::toggle, on_off, {}, get_sky_fog, set_sky_fog, nullptr, nullptr, distance_longer},
    {"Sky",
     "Modern: the sky drawn behind everything, so far hills show beyond it.\nAs on the PS2: the sky's dome at its "
     "real distance, hiding what lies beyond it.",
     RowKind::choice, skies, {}, get_sky, set_sky, nullptr, nullptr, nullptr, nullptr, sky_overridden},
    {"Cut reflections at the water",
     "On: what is under the water is not in its reflection.\nOff: as on the PS2, which mirrors everything, a "
     "zombie's feet sticking up above the pond.",
     RowKind::toggle, on_off, {}, get_reflection_clip, set_reflection_clip, nullptr, nullptr, nullptr, nullptr,
     reflection_clip_overridden},
};

// ---- Controls -------------------------------------------------------------------------------------------------------
int get_twin_stick() {
    return port_twin_stick_setting() != 0;
}
void set_twin_stick(int on) {
    port_twin_stick_set(on);
    changed();
}

int get_separate_run() {
    return port_options.separate_run != 0;
}
void set_separate_run(int on) {
    port_settings.separate_run = port_options.separate_run = on;
    changed();
}
bool separate_run_overridden() {
    return port_options.separate_run != port_settings.separate_run;
}

int get_sensitivity() {
    return int(std::lround(port_controls_mouse_sensitivity() * 100.0));
}
void set_sensitivity(int percent) {
    port_controls_set_mouse(double(percent) / 100.0, port_controls_mouse_invert());
    changed();
}
void describe_sensitivity(int percent, char *out, size_t size) {
    std::snprintf(out, size, "%.2fx", double(percent) / 100.0);
}
int get_invert() {
    return port_controls_mouse_invert() != 0;
}
void set_invert(int on) {
    port_controls_set_mouse(port_controls_mouse_sensitivity(), on);
    changed();
}

const SettingRow controls_rows[] = {
    {"Twin-stick controls",
     "The left stick walks and steps sideways, the right stick turns and looks. Off: the PS2's sticks, the left one "
     "the d-pad.",
     RowKind::toggle, on_off, {}, get_twin_stick, set_twin_stick},
    {"Separate run key",
     "Run while Left Shift (or a controller's left stick press) is held. The action button then only talks, picks "
     "up and opens.",
     RowKind::toggle, on_off, {}, get_separate_run, set_separate_run, nullptr, nullptr, nullptr, nullptr,
     separate_run_overridden},
    {"Mouse", "", RowKind::heading},
    {"Mouse sensitivity", "How far the view turns for the mouse's movement.", RowKind::number, {}, {25, 400, 5},
     get_sensitivity, set_sensitivity, nullptr, describe_sensitivity},
    {"Invert up and down", "Moving the mouse up looks down.", RowKind::toggle, on_off, {}, get_invert, set_invert},
};

// ---- Gameplay -------------------------------------------------------------------------------------------------------
int get_necklace() {
    return port_options.myus_necklace != 0;
}
void set_necklace(int on) {
    port_settings.myus_necklace = port_options.myus_necklace = on;
    changed();
}
bool necklace_overridden() {
    return port_options.myus_necklace != port_settings.myus_necklace;
}
int get_jolt() {
    return port_options.no_hit_jolt != 0;
}
void set_jolt(int on) {
    port_settings.no_hit_jolt = port_options.no_hit_jolt = on;
    changed();
}
bool jolt_overridden() {
    return port_options.no_hit_jolt != port_settings.no_hit_jolt;
}

const SettingRow gameplay_rows[] = {
    {"Changes to how the game plays", "", RowKind::heading},
    {"Myu's Necklace always",
     "What glows while Myu's Necklace is worn always glows, so the amulet slot is free for another.", RowKind::toggle,
     on_off, {}, get_necklace, set_necklace, nullptr, nullptr, nullptr, nullptr, necklace_overridden},
    {"No camera jolt from hits",
     "A hard hit no longer throws the view down. It still flashes, rumbles and knocks you back.", RowKind::toggle,
     on_off, {}, get_jolt, set_jolt, nullptr, nullptr, nullptr, nullptr, jolt_overridden},
};

// ---- Saves ----------------------------------------------------------------------------------------------------------
const std::array<Choice, PORT_QUICK_SLOTS> quick_slots = [] {
    static char names[PORT_QUICK_SLOTS][16];
    std::array<Choice, PORT_QUICK_SLOTS> choices = {};
    for (int slot = 0; slot < PORT_QUICK_SLOTS; slot++) {
        std::snprintf(names[slot], sizeof(names[slot]), "Slot %d", slot);
        choices[size_t(slot)] = {names[slot], slot};
    }
    return choices;
}();
int get_quick_slot() {
    return port_settings.quick_slot;
}
void set_quick_slot(int slot) {
    port_settings.quick_slot = slot;
    changed();
}
void describe_quick_slot(int slot, char *out, size_t size) {
    PortQuickInfo info;
    if (port_quick_info(slot, &info)) {
        std::snprintf(out, size, "Slot %d: level %d, HP %d/%d", slot, info.level, info.hp, info.hp_max);
    } else {
        std::snprintf(out, size, "Slot %d: empty", slot);
    }
}
void quick_save() {
    port_quick_request_save(port_settings.quick_slot);
}
void quick_load() {
    port_quick_request_load(port_settings.quick_slot);
}
bool quick_slot_full() {
    PortQuickInfo info;
    return port_quick_info(port_settings.quick_slot, &info) != 0;
}

const SettingRow saves_rows[] = {
    {"Quick saves", "", RowKind::heading},
    {"Quick save slot",
     "Quick saves are the game's own save, taken where you stand, and load in any version of Darker Reality. The "
     "quick save and quick load keys (F2 and F4 unless changed) use this slot. The save points' three saves are "
     "left as they are.",
     RowKind::choice, quick_slots, {}, get_quick_slot, set_quick_slot, nullptr, describe_quick_slot},
    {"Quick save now", "Saves to the slot above when the menu closes (in play: not in menus, messages or events).",
     RowKind::action, {}, {}, nullptr, nullptr, quick_save},
    {"Quick load now", "Loads the slot above when the menu closes. Enemies come back as when you enter a map.",
     RowKind::action, {}, {}, nullptr, nullptr, quick_load, nullptr, nullptr, quick_slot_full},
};

// ---- Sound and the menu ---------------------------------------------------------------------------------------------
int get_volume() {
    return port_settings.volume;
}
void set_volume(int volume) {
    port_settings.volume = volume;
    port_settings_apply_audio();
    changed();
}
void describe_percent(int percent, char *out, size_t size) {
    std::snprintf(out, size, "%d%%", percent);
}
int get_mute() {
    return port_settings.mute != 0;
}
void set_mute(int on) {
    port_settings.mute = on;
    port_settings_apply_audio();
    changed();
}
int get_pause() {
    return port_settings.pause_when_open != 0;
}
void set_pause(int on) {
    port_settings.pause_when_open = on;
    changed();
}
int get_fps() {
    return port_options.show_fps != 0;
}
void set_fps(int on) {
    port_settings.show_fps = port_options.show_fps = on;
    changed();
}

const SettingRow general_rows[] = {
    {"Sound", "", RowKind::heading},
    {"Volume", "", RowKind::number, {}, {0, 100, 5}, get_volume, set_volume, nullptr, describe_percent},
    {"Mute", "", RowKind::toggle, on_off, {}, get_mute, set_mute},
    {"This menu", "", RowKind::heading},
    {"Pause while open", "The game holds still while this menu is open.", RowKind::toggle, on_off, {}, get_pause,
     set_pause},
    {"Frame-rate counter", "The pictures shown a second, in the top right corner. F3 shows and hides it in play.",
     RowKind::toggle, on_off, {}, get_fps, set_fps},
};

const SettingTab tabs[] = {
    {"display", "Display", display_rows}, {"graphics", "Graphics", graphics_rows},
    {"controls", "Controls", controls_rows}, {"gameplay", "Gameplay", gameplay_rows},
    {"saves", "Saves", saves_rows},       {"general", "Sound & Menu", general_rows},
};

bool row_shown(const SettingRow &row) {
    return row.shown == nullptr || row.shown();
}

// The index in the catalogue of the tab's n-th shown row
int catalogue_index(int tab, int shown) {
    std::span<const SettingRow> rows = tabs[tab].rows;
    for (size_t i = 0; i < rows.size(); i++) {
        if (row_shown(rows[i]) && shown-- == 0) {
            return int(i);
        }
    }
    return -1;
}

int choice_index(const SettingRow &row, int value) {
    for (size_t i = 0; i < row.choices.size(); i++) {
        if (row.choices[i].value == value) {
            return int(i);
        }
    }
    return -1;
}

} // namespace

std::span<const SettingTab> setting_tabs() {
    return tabs;
}

int SettingsRows::tab_count() const {
    return int(std::size(tabs));
}

int SettingsRows::row_count(int tab) const {
    int count = 0;
    for (const SettingRow &row : tabs[tab].rows) {
        count += row_shown(row);
    }
    return count;
}

const SettingTab &SettingsRows::tab(int tab) const {
    return tabs[tab];
}

const SettingRow &SettingsRows::row(int tab, int row) const {
    return tabs[tab].rows[size_t(catalogue_index(tab, row))];
}

RowShape SettingsRows::shape(int tab, int index) const {
    const SettingRow &r = row(tab, index);
    RowShape shape = {r.kind, r.enabled == nullptr || r.enabled(), int(r.choices.size()), 0};
    if (r.kind == RowKind::choice || r.kind == RowKind::toggle) {
        shape.current = choice_index(r, r.get());
    }
    return shape;
}

void SettingsRows::choose(int tab, int index, int choice) {
    const SettingRow &r = row(tab, index);
    if (choice >= 0 && size_t(choice) < r.choices.size() && r.set != nullptr) {
        r.set(r.choices[size_t(choice)].value);
    }
}

void SettingsRows::step(int tab, int index, int delta) {
    const SettingRow &r = row(tab, index);
    if (r.kind != RowKind::number || r.set == nullptr) {
        return;
    }
    // to the next whole step, so a value between steps (from settings.json) lands on one
    int value = r.get();
    int steps = (value - r.range.min) / r.range.step + (delta > 0 ? 1 : ((value - r.range.min) % r.range.step ? 0 : -1));
    int next = r.range.min + steps * r.range.step;
    next = next < r.range.min ? r.range.min : next > r.range.max ? r.range.max : next;
    if (next != value) {
        r.set(next);
    }
}

void SettingsRows::activate(int tab, int index) {
    const SettingRow &r = row(tab, index);
    if (r.act != nullptr) {
        r.act();
    }
}

std::string SettingsRows::value_text(int tab, int index) const {
    const SettingRow &r = row(tab, index);
    char text[96] = "";
    switch (r.kind) {
    case RowKind::choice:
    case RowKind::toggle: {
        // the row's own text when it gives one ("Auto (2x)"), else the chosen choice's label
        int value = r.get();
        if (r.describe != nullptr) {
            r.describe(value, text, sizeof(text));
        }
        int at = choice_index(r, value);
        if (text[0] == 0 && at >= 0) {
            return r.choices[size_t(at)].label;
        }
        return text;
    }
    case RowKind::number:
        if (r.describe != nullptr) {
            r.describe(r.get(), text, sizeof(text));
        } else {
            std::snprintf(text, sizeof(text), "%d", r.get());
        }
        return text;
    case RowKind::action:
    case RowKind::heading:
        break;
    }
    return text;
}

float SettingsRows::number_fraction(int tab, int index) const {
    const SettingRow &r = row(tab, index);
    if (r.kind != RowKind::number || r.range.max <= r.range.min) {
        return 0.0f;
    }
    float f = float(r.get() - r.range.min) / float(r.range.max - r.range.min);
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

int SettingsRows::tab_of(const char *id) const {
    for (size_t i = 0; i < std::size(tabs); i++) {
        if (std::string_view(tabs[i].id) == id) {
            return int(i);
        }
    }
    return -1;
}

} // namespace ui
