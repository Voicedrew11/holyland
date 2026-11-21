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
#include "port/automap.h"
#include "port/cheats.h"
#include "port/controls.h"
#include "port/hands.h"
#include "port/host.h"
#include "port/live_settings.h"
#include "port/platform.h"
#include "port/quicksave.h"
#include "port/settings.h"
#include "port/stats.h"
#include "port/twin_stick.h"
}

#include "../overlay/update_badge.h"

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

// The window's sizes (as the overlay's): the screen heights and the PS2's 448 lines once and twice, as wide as the
// picture's shape (width 0); then monitors' own sizes, which the picture is fitted into. A choice's value is its place.
struct WindowSize {
    int width, height;
};
const WindowSize window_sizes[] = {{0, 448},     {0, 720},     {0, 896},     {0, 900},     {0, 1080},    {0, 1440},
                                   {0, 2160},    {2560, 1440}, {2560, 1600}, {3440, 1440}, {3840, 2160}, {3840, 2400}};
const Choice window_size_choices[] = {
    {"448 lines (the PS2's)", 0}, {"720 lines", 1},            {"896 lines (the PS2's twice)", 2},
    {"900 lines", 3},             {"1080 lines", 4},           {"1440 lines", 5},
    {"2160 lines", 6},            {"2560 x 1440 (16:9)", 7},   {"2560 x 1600 (16:10)", 8},
    {"3440 x 1440 (21:9)", 9},    {"3840 x 2160 (16:9)", 10},  {"3840 x 2400 (16:10)", 11}};
static_assert(std::size(window_sizes) == std::size(window_size_choices));
int window_width(int width, int height) {
    if (width > 0) {
        return width;
    }
    return port_options.widescreen ? (height * 16 + 8) / 9 : (height * 4 + 2) / 3;
}
int get_window_size() {
    for (size_t i = 0; i < std::size(window_sizes); i++) {
        if (window_sizes[i].width == port_settings.window_width && window_sizes[i].height == port_settings.window_height) {
            return int(i);
        }
    }
    return -1;
}
void set_window_size(int index) {
    if (index < 0 || size_t(index) >= std::size(window_sizes)) {
        return;
    }
    port_settings.window_width = window_sizes[index].width;
    port_settings.window_height = window_sizes[index].height;
    port_video_apply_settings();
    changed();
}
void describe_window_size(int index, char *out, size_t size) {
    int width = port_settings.window_width, height = port_settings.window_height;
    if (index >= 0) {
        width = window_sizes[index].width;
        height = window_sizes[index].height;
    }
    std::snprintf(out, size, index >= 0 ? "%d x %d" : "%d x %d (resized)", window_width(width, height), height);
}
bool windowed() {
    return port_settings.fullscreen == 0;
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
    {"Window size",
     "The window's size when not full screen. Its width follows the picture's shape (4:3, or 16:9 with widescreen); "
     "the monitors' own sizes have the picture fitted inside.\nThe window can also be resized by dragging its edges. "
     "Full screen always uses the screen's own resolution.",
     RowKind::choice, window_size_choices, {}, get_window_size, set_window_size, nullptr, describe_window_size, nullptr,
     windowed},
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

// The PS2's TV picture fixes (as the overlay's Developer page): on, the PC's picture; off, the PS2's. The flicker
// filter is the other way round: the PS2's own blur is the option on.
int get_deinterlace() {
    return port_options.deinterlace != 0;
}
void set_deinterlace(int on) {
    port_settings.deinterlace = port_options.deinterlace = on;
    changed();
}
bool deinterlace_overridden() {
    return port_options.deinterlace != port_settings.deinterlace;
}
int get_progressive() {
    return port_options.progressive != 0;
}
void set_progressive(int on) {
    port_settings.progressive = port_options.progressive = on;
    changed();
}
bool progressive_overridden() {
    return port_options.progressive != port_settings.progressive;
}
int get_whole_frame() {
    return port_options.whole_frame != 0;
}
void set_whole_frame(int on) {
    port_settings.whole_frame = port_options.whole_frame = on;
    changed();
}
bool whole_frame_overridden() {
    return port_options.whole_frame != port_settings.whole_frame;
}
int get_seamless_scroll() {
    return port_options.seamless_scroll != 0;
}
void set_seamless_scroll(int on) {
    port_settings.seamless_scroll = port_options.seamless_scroll = on;
    changed();
}
bool seamless_scroll_overridden() {
    return port_options.seamless_scroll != port_settings.seamless_scroll;
}
int get_no_flicker_filter() {
    return port_options.flicker_filter == 0;
}
void set_no_flicker_filter(int on) {
    port_settings.flicker_filter = port_options.flicker_filter = !on;
    changed();
}
bool flicker_filter_overridden() {
    return port_options.flicker_filter != port_settings.flicker_filter;
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
    {"The PS2's picture", "", RowKind::heading},
    {"Weave the two fields",
     "The game draws each field as its own 224-line picture, half a line apart.\nOn: the two are woven into one, as a "
     "TV shows them.\nOff: each field alone, which breaks up text.",
     RowKind::toggle, on_off, {}, get_deinterlace, set_deinterlace, nullptr, nullptr, nullptr, nullptr,
     deinterlace_overridden},
    {"Whole-frame pictures whole",
     "On: a frame-mode interlaced picture is read whole.\nOff: a field at a time, as the PS2 scans it out.",
     RowKind::toggle, on_off, {}, get_progressive, set_progressive, nullptr, nullptr, nullptr, nullptr,
     progressive_overridden},
    {"No interlace comb",
     "On: the picture the game draws over every other line while the view turns (for the other field of a TV) is "
     "left out, and the movies' and menus' pictures are shown whole.\nOff: as on the PS2, a comb trailing what moves.",
     RowKind::toggle, on_off, {}, get_whole_frame, set_whole_frame, nullptr, nullptr, nullptr, nullptr,
     whole_frame_overridden},
    {"Scrolling textures without a seam",
     "On: the sky's clouds, the lava and the water repeat evenly.\nOff: as on the PS2, with a line where the clouds or "
     "the lava warp.",
     RowKind::toggle, on_off, {}, get_seamless_scroll, set_seamless_scroll, nullptr, nullptr, nullptr, nullptr,
     seamless_scroll_overridden},
    {"No flicker filter",
     "On: the rows as the game drew them.\nOff: as on the PS2, each row blended half with the row above, against an "
     "interlaced TV's flicker.",
     RowKind::toggle, on_off, {}, get_no_flicker_filter, set_no_flicker_filter, nullptr, nullptr, nullptr, nullptr,
     flicker_filter_overridden},
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

int get_mouse_lead() {
    return port_controls_mouse_lead() != 0;
}
void set_mouse_lead(int on) {
    port_controls_set_mouse_lead(on);
    changed();
}

// What each mouse button presses outside play: one of the face buttons or none (an ACTION_, -1 none)
const Choice menu_presses[] = {{"Nothing", -1},          {"Cross", ACTION_CROSS},  {"Circle", ACTION_CIRCLE},
                               {"Square", ACTION_SQUARE}, {"Triangle", ACTION_TRIANGLE}, {"Start", ACTION_START},
                               {"Select", ACTION_SELECT}};
int get_menu_left() {
    return port_controls_menu_mouse(0);
}
void set_menu_left(int action) {
    port_controls_set_menu_mouse(0, action);
    changed();
}
int get_menu_right() {
    return port_controls_menu_mouse(1);
}
void set_menu_right(int action) {
    port_controls_set_menu_mouse(1, action);
    changed();
}
int get_menu_middle() {
    return port_controls_menu_mouse(2);
}
void set_menu_middle(int action) {
    port_controls_set_menu_mouse(2, action);
    changed();
}
const char menu_mouse_help[] =
    "Outside play (the game's menus, the pause, talking) the mouse is let go, and its buttons press these. In play, "
    "with the mouse held, its buttons are the key bindings'.";

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
    {"Instant mouse look",
     "On: the pictures between the game's show the mouse's turn the moment it is read, so the view follows the hand "
     "with the least delay.\nOff: the turn shows with the game's next frame, as before.",
     RowKind::toggle, on_off, {}, get_mouse_lead, set_mouse_lead},
    {"The mouse in menus", "", RowKind::heading},
    {"Left button", menu_mouse_help, RowKind::choice, menu_presses, {}, get_menu_left, set_menu_left},
    {"Right button", menu_mouse_help, RowKind::choice, menu_presses, {}, get_menu_right, set_menu_right},
    {"Middle button", menu_mouse_help, RowKind::choice, menu_presses, {}, get_menu_middle, set_menu_middle},
};

// ---- Key bindings ---------------------------------------------------------------------------------------------------
void reset_bindings() {
    port_controls_reset();
    changed();
}

const char binding_help[] =
    "Each action has two slots for keys or mouse buttons and one for a controller button.\nLeft and Right choose the "
    "slot; Enter (or a click on it) waits for the next key or button, which goes there. Delete (or a right click) "
    "empties the slot. F1 or five seconds stop the waiting.";

// The section's titles, before the action they come before (port/include/port/controls.h's order)
struct BindingGroup {
    int first_action;
    const char *title;
};
const BindingGroup binding_groups[] = {
    {ACTION_UP, "The PS2's buttons"}, {ACTION_LEFT_STICK_UP, "The sticks"}, {ACTION_RUN, "Play"},
    {ACTION_QUICK_SAVE, "Saves"},    {ACTION_GS_CAPTURE, "Debugging"},    {ACTION_SHOW_FPS, "The screen and the mods"},
};

const auto binding_rows = [] {
    std::array<SettingRow, 1 + std::size(binding_groups) + ACTION_COUNT> rows = {};
    size_t at = 0;
    rows[at++] = {"Restore the default bindings",
                  "Puts every key and button back as it was, and the mouse's settings with them.", RowKind::action,
                  {}, {}, nullptr, nullptr, reset_bindings};
    size_t group = 0;
    for (int action = 0; action < ACTION_COUNT; action++) {
        if (group < std::size(binding_groups) && binding_groups[group].first_action == action) {
            rows[at++] = {binding_groups[group++].title, "", RowKind::heading};
        }
        SettingRow row = {port_controls_action_label(action), binding_help, RowKind::binding};
        row.action = action;
        rows[at++] = row;
    }
    return rows;
}();


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

int get_stats() {
    return port_stats_options.on != 0;
}
void set_stats(int on) {
    port_stats_set_on(on);
    port_stats_options.menus = 1; // the menus' lines are what the mod is (extended_stats_menus stays in settings.json)
    changed();
}
bool stats_overridden() {
    return port_stats_options.forced != 0;
}
bool stats_on() {
    return port_stats_options.on != 0;
}
int get_durability() {
    return port_stats_options.durability != 0;
}
void set_durability(int on) {
    port_stats_options.durability = on;
    changed();
}
int get_timers() {
    return port_stats_options.timers != 0;
}
void set_timers(int on) {
    port_stats_options.timers = on;
    changed();
}
int get_damage() {
    return port_stats_options.damage != 0;
}
void set_damage(int on) {
    port_stats_options.damage = on;
    changed();
}
int get_stats_panel() {
    return port_stats_options.panel != 0;
}
void set_stats_panel(int on) {
    port_stats_options.panel = on;
    changed();
}

const SettingRow gameplay_rows[] = {
    {"Changes to how the game plays", "", RowKind::heading},
    {"Myu's Necklace always",
     "What glows while Myu's Necklace is worn always glows, so the amulet slot is free for another.", RowKind::toggle,
     on_off, {}, get_necklace, set_necklace, nullptr, nullptr, nullptr, nullptr, necklace_overridden},
    {"No camera jolt from hits",
     "A hard hit no longer throws the view down. It still flashes, rumbles and knocks you back.", RowKind::toggle,
     on_off, {}, get_jolt, set_jolt, nullptr, nullptr, nullptr, nullptr, jolt_overridden},
    {"Extended stats", "", RowKind::heading},
    {"Extended stats in the menus",
     "What the game keeps about you and never shows, in its own menus: the experience to the next level, the hits "
     "and spells to the next Physical and Magical point, each piece's durability, the hidden effects of what you "
     "wear, what an item does. It only reads the game.",
     RowKind::toggle, on_off, {}, get_stats, set_stats, nullptr, nullptr, nullptr, nullptr, stats_overridden},
    {"Equipment durability", "The percent each piece's values count at, worn down by hits (the smith mends it).",
     RowKind::toggle, on_off, {}, get_durability, set_durability, nullptr, nullptr, nullptr, stats_on},
    {"Ailment timers on screen",
     "While you play, the ailments running and the timed effects of spells and items, each with its seconds left, "
     "beside the minimap (or in its corner with the minimap off).",
     RowKind::toggle, on_off, {}, get_timers, set_timers, nullptr, nullptr, nullptr, stats_on},
    {"Hit log and the target's health",
     "Each hit you deal or take, with its real damage and the HP left (bottom left), and the health bar of the "
     "creature you last hit (top).",
     RowKind::toggle, on_off, {}, get_damage, set_damage, nullptr, nullptr, nullptr, stats_on},
    {"Stats panel on its key",
     "Everything in one panel, shown and hidden by its key (F6 unless changed in Key bindings).", RowKind::toggle,
     on_off, {}, get_stats_panel, set_stats_panel, nullptr, nullptr, nullptr, stats_on},
};

// ---- Map ------------------------------------------------------------------------------------------------------------
void map_changed() {
    port_automap_settings_changed();
}
int get_automap() {
    return port_automap.enabled != 0;
}
void set_automap(int on) {
    port_automap.enabled = on;
    map_changed();
}
bool automap_on() {
    return port_automap.enabled != 0;
}
bool minimap_on() {
    return port_automap.enabled && port_automap.minimap;
}
int get_minimap() {
    return port_automap.minimap != 0;
}
void set_minimap(int on) {
    port_automap.minimap = on;
    map_changed();
}
const Choice minimap_corners[] = {{"Top right", 0}, {"Bottom right", 1}, {"Top left", 2}, {"Bottom left", 3}};
int get_minimap_corner() {
    return port_automap.minimap_corner;
}
void set_minimap_corner(int corner) {
    port_automap.minimap_corner = corner;
    map_changed();
}
int get_minimap_size() {
    return port_automap.minimap_size;
}
void set_minimap_size(int percent) {
    port_automap.minimap_size = percent;
    map_changed();
}
void describe_minimap_size(int percent, char *out, size_t size) {
    std::snprintf(out, size, "%d%% of the picture", percent);
}
int get_minimap_reach() {
    return port_automap.minimap_zoom;
}
void set_minimap_reach(int hundreds) {
    port_automap.minimap_zoom = hundreds;
    map_changed();
}
void describe_minimap_reach(int hundreds, char *out, size_t size) {
    std::snprintf(out, size, "%d00 units", hundreds);
}
int get_minimap_opacity() {
    return port_automap.opacity;
}
void set_minimap_opacity(int percent) {
    port_automap.opacity = percent;
    map_changed();
}
int get_full_opacity() {
    return port_automap.full_opacity;
}
void set_full_opacity(int percent) {
    port_automap.full_opacity = percent;
    map_changed();
}
int get_reveal() {
    return port_automap.reveal != 0;
}
void set_reveal(int on) {
    port_automap.reveal = on;
    map_changed();
}
int get_map_items() {
    return port_automap.items != 0;
}
void set_map_items(int on) {
    port_automap.items = on;
    map_changed();
}
int get_map_enemies() {
    return port_automap.enemies != 0;
}
void set_map_enemies(int on) {
    port_automap.enemies = on;
    map_changed();
}
int get_map_secrets() {
    return port_automap.secrets != 0;
}
void set_map_secrets(int on) {
    port_automap.secrets = on;
    map_changed();
}
int get_map_hints() {
    return port_automap.hints != 0;
}
void set_map_hints(int on) {
    port_automap.hints = on;
    map_changed();
}
void describe_percent(int percent, char *out, size_t size) {
    std::snprintf(out, size, "%d%%", percent);
}

const SettingRow map_rows[] = {
    {"The map and the minimap", "", RowKind::heading},
    {"Automap",
     "A map of where you have been, filled in as you explore and kept with each save. Its key (M unless changed in "
     "Key bindings) opens it; the game waits while it is open.",
     RowKind::toggle, on_off, {}, get_automap, set_automap},
    {"Minimap", "A small map in a corner of the picture while you play.", RowKind::toggle, on_off, {}, get_minimap,
     set_minimap, nullptr, nullptr, nullptr, automap_on},
    {"Minimap corner", "", RowKind::choice, minimap_corners, {}, get_minimap_corner, set_minimap_corner, nullptr,
     nullptr, nullptr, minimap_on},
    {"Minimap size", "", RowKind::number, {}, {10, 45, 1}, get_minimap_size, set_minimap_size, nullptr,
     describe_minimap_size, nullptr, minimap_on},
    {"Minimap reach", "How far the minimap shows from where you stand.", RowKind::number, {}, {20, 120, 5},
     get_minimap_reach, set_minimap_reach, nullptr, describe_minimap_reach, nullptr, minimap_on},
    {"Minimap opacity", "", RowKind::number, {}, {20, 100, 5}, get_minimap_opacity, set_minimap_opacity, nullptr,
     describe_percent, nullptr, minimap_on},
    {"Full map background", "How much the full map hides the game behind it.", RowKind::number, {}, {40, 100, 5},
     get_full_opacity, set_full_opacity, nullptr, describe_percent, nullptr, automap_on},
    {"Map help", "", RowKind::heading},
    {"Reveal the whole map", "Every floor of the map, explored or not. What you explore is still kept.",
     RowKind::toggle, on_off, {}, get_reveal, set_reveal, nullptr, nullptr, nullptr, automap_on},
    {"Show items",
     "Items lying on the ground, and what chests, containers and bones hold, through the fog; what enemies may drop.",
     RowKind::toggle, on_off, {}, get_map_items, set_map_items, nullptr, nullptr, nullptr, automap_on},
    {"Show enemies", "The live enemies, named, with their HP, through the fog.", RowKind::toggle, on_off, {},
     get_map_enemies, set_map_enemies, nullptr, nullptr, nullptr, automap_on},
    {"Show secrets and traps", "Traps (shooters, pitfalls) and walls that appear or fade away, through the fog.",
     RowKind::toggle, on_off, {}, get_map_secrets, set_map_secrets, nullptr, nullptr, nullptr, automap_on},
    {"Show locks and hints",
     "What each lock needs (and whether you have it), and the guide's notes for locks and puzzles (a blue ? on the "
     "map).",
     RowKind::toggle, on_off, {}, get_map_hints, set_map_hints, nullptr, nullptr, nullptr, automap_on},
};

// ---- Hands ----------------------------------------------------------------------------------------------------------
int get_hands() {
    return port_hands.enabled != 0;
}
void set_hands(int on) {
    port_hands_set_enabled(on);
}
bool hands_overridden() {
    return port_hands.forced != 0;
}
bool hands_on() {
    return port_hands.enabled != 0;
}
int get_hands_fade() {
    return port_hands.fade != 0;
}
void set_hands_fade(int on) {
    port_hands_set_fade(on);
}
int get_hands_size() {
    return int(std::lround(port_hands.scale * 100.0f));
}
void set_hands_size(int percent) {
    port_hands_set_scale(float(percent) / 100.0f);
}
const Choice hands_corners[] = {{"Bottom left", PORT_HANDS_BOTTOM_LEFT},
                                {"Bottom right", PORT_HANDS_BOTTOM_RIGHT},
                                {"Top right", PORT_HANDS_TOP_RIGHT},
                                {"Top left", PORT_HANDS_TOP_LEFT}};
static_assert(std::size(hands_corners) == PORT_HANDS_CORNER_COUNT);
int get_hands_corner() {
    return int(port_hands.corner);
}
void set_hands_corner(int corner) {
    port_hands_set_corner(PortHandsCorner(corner));
}
const Choice hands_layouts[] = {{"Cross", PORT_HANDS_LAYOUT_CROSS}, {"Row", PORT_HANDS_LAYOUT_ROW}};
static_assert(std::size(hands_layouts) == PORT_HANDS_LAYOUT_COUNT);
int get_hands_layout() {
    return int(port_hands.layout);
}
void set_hands_layout(int layout) {
    port_hands_set_layout(PortHandsLayout(layout));
}
const Choice hands_cards[] = {{"Tall", PORT_HANDS_CARDS_TALL}, {"Square", PORT_HANDS_CARDS_SQUARE}};
static_assert(std::size(hands_cards) == PORT_HANDS_CARDS_COUNT);
int get_hands_cards() {
    return int(port_hands.cards);
}
void set_hands_cards(int cards) {
    port_hands_set_cards(PortHandsCards(cards));
}

const SettingRow hands_rows[] = {
    {"Hands and hotkeys",
     "Two weapons in the right hand, two spells in the left, and five hotkeys, shown as a cross of cards. In play the "
     "controller's d-pad swaps them (Right the weapon, Left the spell, Down and Up the hotkey); on the keyboard, the "
     "keys in Key bindings. Select uses the hotkey. Choose what each slot holds on the equipment screen: Cross on an "
     "item asks which slot.",
     RowKind::toggle, on_off, {}, get_hands, set_hands, nullptr, nullptr, nullptr, nullptr, hands_overridden},
    {"Fade the cross when idle", "After a few seconds with no hand key pressed the cross fades to a quarter.",
     RowKind::toggle, on_off, {}, get_hands_fade, set_hands_fade, nullptr, nullptr, nullptr, hands_on},
    {"Cross size", "The cross's size: its cards, pictures and count together.", RowKind::number, {},
     {int(PORT_HANDS_SCALE_MIN * 100.0f + 0.5f), int(PORT_HANDS_SCALE_MAX * 100.0f + 0.5f), 5}, get_hands_size,
     set_hands_size, nullptr, describe_percent, nullptr, hands_on},
    {"Cross corner", "The corner of the screen the cross is in. Top left puts it under the health and magic gauges.",
     RowKind::choice, hands_corners, {}, get_hands_corner, set_hands_corner, nullptr, nullptr, nullptr, hands_on},
    {"Cross layout", "Cross: the hotkey's card, the middle one, a little lower.\nRow: the three in line.",
     RowKind::choice, hands_layouts, {}, get_hands_layout, set_hands_layout, nullptr, nullptr, nullptr, hands_on},
    {"Card shape", "Tall cards, or square ones.", RowKind::choice, hands_cards, {}, get_hands_cards, set_hands_cards,
     nullptr, nullptr, nullptr, hands_on},
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
     "Quick saves are the game's own save, taken where you stand, and load in any version of Holyland. The "
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

bool update_due() {
    return update_badge_due();
}
const char *update_summary() {
    return update_badge_summary();
}

const SettingRow general_rows[] = {
    {"An update is out", "", RowKind::heading, {}, {}, nullptr, nullptr, nullptr, nullptr, update_due},
    {"Open the download page",
     "The release's page in your browser. Download it and install it over this one.", RowKind::action, {}, {},
     nullptr, nullptr, update_badge_open_page, nullptr, update_due, nullptr, nullptr, update_summary},
    {"Skip this version", "No more notices for this version; the next one is shown.", RowKind::action, {}, {}, nullptr,
     nullptr, update_badge_skip, nullptr, update_due, nullptr, nullptr, update_summary},
    {"Sound", "", RowKind::heading},
    {"Volume", "", RowKind::number, {}, {0, 100, 5}, get_volume, set_volume, nullptr, describe_percent},
    {"Mute", "", RowKind::toggle, on_off, {}, get_mute, set_mute},
    {"This menu", "", RowKind::heading},
    {"Pause while open", "The game holds still while this menu is open.", RowKind::toggle, on_off, {}, get_pause,
     set_pause},
    {"Frame-rate counter", "The pictures shown a second, in the top right corner. F3 shows and hides it in play.",
     RowKind::toggle, on_off, {}, get_fps, set_fps},
};

// ---- Cheats ---------------------------------------------------------------------------------------------------------
int get_invincible() {
    return port_options.invincible != 0;
}
void set_invincible(int on) {
    port_options.invincible = on; // (never saved: one run's)
}
int get_spell_level() {
    return port_cheat_spell_level();
}
void set_spell_level(int level) {
    port_cheat_set_spell_level(level);
}
void give_spells() {
    port_cheat_request(PORT_CHEAT_SPELLS);
}
void give_equipment() {
    port_cheat_request(PORT_CHEAT_EQUIPMENT);
}
void give_items() {
    port_cheat_request(PORT_CHEAT_ITEMS);
}
void give_magic() {
    port_cheat_request(PORT_CHEAT_MAGIC);
}
void give_all() {
    port_cheat_request(PORT_CHEAT_SPELLS | PORT_CHEAT_EQUIPMENT | PORT_CHEAT_ITEMS | PORT_CHEAT_MAGIC);
}
const char *cheat_message() {
    return port_cheat_message();
}
const char cheat_help[] = "A one-off change to your character, made once you are in play (not in a menu or a "
                          "message). It lasts as any progress does, and a save made after it keeps it.";

const SettingRow cheats_rows[] = {
    {"For this run", "", RowKind::heading},
    {"Invincible", "No HP lost, whatever hits you. Off again at the next start.", RowKind::toggle, on_off, {},
     get_invincible, set_invincible},
    {"Give", "", RowKind::heading},
    {"Spell level",
     "The level the spells are given at, 1 to 3. Each level is its own spell, with its own strength and length. A "
     "spell already known goes to this level too.",
     RowKind::number, {}, {1, 3, 1}, get_spell_level, set_spell_level},
    {"Every spell", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_spells, nullptr, nullptr, nullptr,
     nullptr, cheat_message},
    {"Every weapon and armour", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_equipment, nullptr, nullptr,
     nullptr, nullptr, cheat_message},
    {"Every item, 99 of each", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_items, nullptr, nullptr,
     nullptr, nullptr, cheat_message},
    {"Magic at its highest", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_magic, nullptr, nullptr,
     nullptr, nullptr, cheat_message},
    {"All of these", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_all, nullptr, nullptr, nullptr,
     nullptr, cheat_message},
};

const SettingTab tabs[] = {
    {"display", "Display", display_rows},      {"graphics", "Graphics", graphics_rows},
    {"controls", "Controls", controls_rows},   {"keys", "Key Bindings", binding_rows},
    {"gameplay", "Gameplay", gameplay_rows},   {"map", "Map", map_rows},
    {"hands", "Hands", hands_rows},            {"saves", "Saves", saves_rows},
    {"general", "Sound & Menu", general_rows}, {"cheats", "Cheats", cheats_rows},
};

bool row_shown(const SettingRow &row) {
    if (row.action >= 0 && !port_controls_action_used(row.action)) {
        return false; // a binding of a mod that is off
    }
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
    } else if (r.kind == RowKind::binding) {
        shape.choice_count = BINDING_SLOTS;
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

void SettingsRows::listen(int tab, int index, int slot) {
    const SettingRow &r = row(tab, index);
    if (r.action >= 0 && slot >= 0 && slot < BINDING_SLOTS) {
        port_controls_listen(r.action, slot);
    }
}

void SettingsRows::clear(int tab, int index, int slot) {
    const SettingRow &r = row(tab, index);
    if (r.action >= 0 && slot >= 0 && slot < BINDING_SLOTS) {
        port_controls_bind(r.action, slot, BINDING_NONE);
        changed();
    }
}

bool SettingsRows::listening(int tab, int index, int slot) const {
    int action = -1, at = -1;
    return port_controls_listening(&action, &at) && action == row(tab, index).action && at == slot;
}

std::string SettingsRows::slot_text(int tab, int index, int slot) const {
    if (listening(tab, index, slot)) {
        return slot == BINDING_PAD_SLOT ? "Press a button..." : "Press a key...";
    }
    int binding = port_controls_binding(row(tab, index).action, slot);
    if (binding == BINDING_NONE) {
        return "-";
    }
    char text[64] = "";
    port_controls_binding_label(binding, text, sizeof(text));
    return text;
}

bool SettingsRows::changed_from_default(int tab, int index) const {
    const SettingRow &r = row(tab, index);
    return r.action >= 0 && !port_controls_is_default(r.action);
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
    case RowKind::binding:
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
