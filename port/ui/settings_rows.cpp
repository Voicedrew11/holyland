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
#include "port/cinematic.h"
#include "port/controls.h"
#include "port/hands.h"
#include "port/host.h"
#include "port/live_settings.h"
#include "port/platform.h"
#include "port/quicksave.h"
#include "port/scene_depth.h"
#include "port/settings.h"
#include "port/stats.h"
#include "port/twin_stick.h"
#include "port/walk_keys.h"
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
    {"448p", 0},          {"720p", 1},          {"896p", 2},          {"900p", 3},
    {"1080p", 4},         {"1440p", 5},         {"2160p", 6},         {"2560 x 1440", 7},
    {"2560 x 1600", 8},   {"3440 x 1440", 9},   {"3840 x 2160", 10},  {"3840 x 2400", 11}};
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

const Choice shapes[] = {{"Keep", ASPECT_4_3}, {"Stretch", ASPECT_STRETCH}};
int get_shape() {
    return port_settings.aspect;
}
void set_shape(int aspect) {
    port_settings.aspect = aspect;
    changed();
}

const Choice scalings[] = {
    {"Nearest", PORT_SCALING_SHARP}, {"Bilinear", PORT_SCALING_SMOOTH}, {"Bicubic", PORT_SCALING_BEST}};
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

const Choice frame_rates[] = {{"30", 30},   {"60", 60},   {"120", 120},
                              {"144", 144}, {"165", 165}, {"240", 240},
                              {"Monitor refresh rate", PORT_FRAME_RATE_MONITOR}, {"Unlocked", PORT_FRAME_RATE_UNLOCKED}};
int get_frame_rate() {
    return port_options.frame_rate;
}
void set_frame_rate(int rate) {
    port_live_set(PORT_LIVE_FRAME_RATE, rate);
}
void describe_frame_rate(int rate, char *out, size_t size) {
    if (rate == PORT_FRAME_RATE_MONITOR) {
        std::snprintf(out, size, "Monitor (%d)", port_frame_rate_hz());
    } else if (rate == PORT_FRAME_RATE_UNLOCKED) {
        std::snprintf(out, size, "Unlocked (%.0f fps)", port_video_pictures_per_second());
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
const Choice syncs[] = {{"Off", SYNC_OFF}, {"On", SYNC_VSYNC}, {"Adaptive", SYNC_ADAPTIVE}};
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

int get_fps() {
    return port_options.show_fps != 0;
}
void set_fps(int on) {
    port_settings.show_fps = port_options.show_fps = on;
    changed();
}

const SettingRow display_rows[] = {
    {"Window", "", RowKind::heading},
    {"Fullscreen", "Borderless, at the screen's resolution. F11 also switches it.", RowKind::toggle, on_off, {},
     get_fullscreen, set_fullscreen},
    {"Window size",
     "The window's size when not fullscreen. The width follows the aspect ratio (4:3, or 16:9 with widescreen). The "
     "window can also be resized by dragging its edges.",
     RowKind::choice, window_size_choices, {}, get_window_size, set_window_size, nullptr, describe_window_size, nullptr,
     windowed},
    {"Aspect ratio", "Keep: 4:3 (16:9 with widescreen), with black bars.\nStretch: fill the window.", RowKind::choice,
     shapes, {}, get_shape, set_shape},
    {"Scaling filter",
     "How the picture is scaled to the window.\nNearest: blocky pixels.\nBilinear: smooth.\nBicubic: sharp "
     "edges, clean downscaling.",
     RowKind::choice, scalings, {}, get_scaling, set_scaling, nullptr, nullptr, nullptr, nullptr, scaling_overridden},
    {"Frame rate", "", RowKind::heading},
    {"Frame rate",
     "30 is the PS2's. Higher rates draw frames between the game's own; the game itself still runs at 30.",
     RowKind::choice, frame_rates, {}, get_frame_rate, set_frame_rate, nullptr, describe_frame_rate, nullptr, nullptr,
     frame_rate_overridden},
    {"Vertical sync",
     "Off: can tear.\nOn: waits for the screen's refresh.\nAdaptive: FreeSync or G-Sync (must be on in your system's "
     "display settings too).",
     RowKind::choice, syncs, {}, get_sync, set_sync, nullptr, nullptr, nullptr, nullptr, sync_overridden},
    {"FPS counter", "Top right corner. F3 shows and hides it in play.", RowKind::toggle, on_off, {}, get_fps,
     set_fps},
};

// ---- Graphics -------------------------------------------------------------------------------------------------------
int get_widescreen() {
    return port_options.widescreen != 0;
}
void set_widescreen(int on) {
    port_live_set(PORT_LIVE_WIDESCREEN, on);
}

const Choice huds[] = {{"Corner", 0}, {"Centred", 1}};
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
int get_fov() {
    return port_options.fov;
}
void set_fov(int fov) {
    port_settings.fov = port_options.fov = fov;
    changed();
}
void describe_fov(int fov, char *out, size_t size) {
    if (fov == 64) {
        std::snprintf(out, size, "64 (default)");
    } else {
        std::snprintf(out, size, "%d", fov);
    }
}
bool fov_overridden() {
    return port_options.fov != port_settings.fov;
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

// (gsn.h's GSN_FONT_*)
const Choice fonts[] = {{"IPA Mincho", 1}, {"Original", 0}};
int get_font() {
    return port_options.font;
}
void set_font(int font) {
    port_live_set(PORT_LIVE_FONT, font);
}
bool font_overridden() {
    return port_options.font != port_settings.font;
}

// (port_options.native_aniso: 1 off, 2 to 16 taps)
const Choice anisotropies[] = {{"Off", 1}, {"2x", 2}, {"4x", 4}, {"8x", 8}, {"16x", 16}};
int get_aniso() {
    return port_options.native_aniso;
}
void set_aniso(int taps) {
    port_gs_set_native_aniso(taps);
    port_settings.native_aniso = port_options.native_aniso;
    changed();
}

// (port_options.native_lod: 0 the PS2's mipmap levels, 1 the level for the resolution, 2 the whole texture)
const Choice texture_lods[] = {{"PS2", 0}, {"Auto", 1}, {"Highest", 2}};
int get_texture_lod() {
    return port_options.native_lod;
}
void set_texture_lod(int lod) {
    port_gs_set_native_lod(lod);
    port_settings.native_lod = lod;
    changed();
}

const Choice distances[] = {{"1x", PORT_DRAW_DISTANCE_1},
                            {"1.5x", PORT_DRAW_DISTANCE_1_5},
                            {"2x", PORT_DRAW_DISTANCE_2},
                            {"4x", PORT_DRAW_DISTANCE_4},
                            {"No fog", PORT_DRAW_DISTANCE_MAX}};
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

const Choice skies[] = {{"Modern", PORT_SKY_MODERN}, {"PS2", PORT_SKY_PS2}};
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

// The PS2's TV picture fixes: on, the PC's picture; off, the PS2's. The flicker
// filter is the other way round: on is the PS2's own blur.
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
int get_flicker_filter() {
    return port_options.flicker_filter != 0;
}
void set_flicker_filter(int on) {
    port_settings.flicker_filter = port_options.flicker_filter = on;
    changed();
}
bool flicker_filter_overridden() {
    return port_options.flicker_filter != port_settings.flicker_filter;
}

// (port/scene_depth.h) The scene renderer's depth: settings.json's, which the renderer takes as it opens
const Choice scene_depths[] = {{port_scene_depth(PORT_SCENE_DEPTH_EXACT)->label, PORT_SCENE_DEPTH_EXACT},
                               {port_scene_depth(PORT_SCENE_DEPTH_NEARER)->label, PORT_SCENE_DEPTH_NEARER},
                               {port_scene_depth(PORT_SCENE_DEPTH_UNSETTLED)->label, PORT_SCENE_DEPTH_UNSETTLED},
                               {port_scene_depth(PORT_SCENE_DEPTH_GPU)->label, PORT_SCENE_DEPTH_GPU}};
static_assert(std::size(scene_depths) == PORT_SCENE_DEPTH_COUNT);
int get_scene_depth() {
    return port_settings.scene_depth;
}
void set_scene_depth(int mode) {
    port_settings.scene_depth = mode;
    changed();
}
bool scene_renderer() {
    return port_options.renderer == PORT_RENDERER_SCENE;
}
bool scene_depth_overridden() {
    return port_options.scene_depth_forced != 0;
}
const char *scene_depth_note() {
    if (port_options.scene_depth_forced || port_settings.scene_depth == port_options.scene_depth) {
        return nullptr;
    }
    static char note[96];
    const PortSceneDepth *now = port_scene_depth(port_options.scene_depth);
    std::snprintf(note, sizeof(note), "Restart the game to use it. Now: %s.", now ? now->label : "?");
    return note;
}

const SettingRow graphics_rows[] = {
    {"Rendering", "", RowKind::heading},
    {"Internal resolution",
     "A multiple of the PS2's 640 x 448.\nAuto: picked from the window's height (1080: 2x, 1440: 3x, 2160: 4x).",
     RowKind::choice, scales, {}, get_scale, set_scale, nullptr, describe_scale, pc_renderer},
    {"Anti-aliasing", "FXAA: cheapest, a little soft.\nSMAA: sharper.\nSMAA T2x: steadier thin edges in motion.",
     RowKind::choice, antialiases, {}, get_antialias, set_antialias, nullptr, nullptr, pc_renderer, nullptr,
     antialias_overridden},
    {"Sharpening", "Applied after scaling to the window; brings back detail anti-aliasing softens.", RowKind::number,
     {}, {0, 100, 5}, get_sharpen, set_sharpen, nullptr, describe_sharpen, pc_renderer},
    {"Anisotropic filtering",
     "Smooth, sharp textures on floors and walls seen at a grazing angle or far off, without shimmer.\nOff: as the PS2 "
     "draws them.",
     RowKind::choice, anisotropies, {}, get_aniso, set_aniso, nullptr, nullptr, pc_renderer},
    {"Texture LOD",
     "Which mipmap level textures use with distance.\nPS2: blurry far away at high resolutions.\nAuto: the level for "
     "this resolution.\nHighest: always the full texture; far surfaces can shimmer.\nWith anisotropic filtering on, "
     "the 3D's textures pick their own level.",
     RowKind::choice, texture_lods, {}, get_texture_lod, set_texture_lod, nullptr, nullptr, pc_renderer},
    {"Font", "IPA Mincho: a sharp font in the same style.\nOriginal: the game's own, blurry when enlarged.",
     RowKind::choice, fonts, {}, get_font, set_font, nullptr, nullptr, pc_renderer, nullptr, font_overridden},
    {"Depth",
     "How surfaces close together are sorted. From the next start.\nExact: the PS2's depth at each pixel; least "
     "z-fighting.\nLayers in one pass: as Exact, a little faster.\nLayers unsettled: as Exact, cheaper; a surface drawn "
     "after a layer on it can lose.\nGPU: the old depth; fastest, more z-fighting.",
     RowKind::choice, scene_depths, {}, get_scene_depth, set_scene_depth, nullptr, nullptr, scene_renderer, nullptr,
     scene_depth_overridden, scene_depth_note},
    {"View", "", RowKind::heading},
    {"Widescreen", "16:9 view with more at the sides. Menus and messages stay 4:3.", RowKind::toggle, on_off, {},
     get_widescreen, set_widescreen},
    {"HUD position", "Where the health and magic gauges go in widescreen.", RowKind::choice, huds, {}, get_hud, set_hud,
     nullptr, nullptr, widescreen_on},
    {"Field of view", "Horizontal, in degrees at 4:3. 64 is the PS2's.", RowKind::number, {}, {30, 120, 1}, get_fov,
     set_fov, nullptr, describe_fov, nullptr, nullptr, fov_overridden},
    {"World", "", RowKind::heading},
    {"Draw distance", "How far the fog starts. 1x is the PS2's. No fog also removes some places' mist.",
     RowKind::choice, distances, {}, get_distance, set_distance, nullptr, nullptr, nullptr, nullptr,
     distance_overridden},
    {"Sky fog distance", "Moves the sky's fog out with the draw distance too; lightens dark skies.", RowKind::toggle,
     on_off, {}, get_sky_fog, set_sky_fog, nullptr, nullptr, distance_longer},
    {"Sky", "Modern: drawn behind everything, so far hills show.\nPS2: the dome at its real distance, hiding what is "
            "beyond it.",
     RowKind::choice, skies, {}, get_sky, set_sky, nullptr, nullptr, nullptr, nullptr, sky_overridden},
    {"Reflection clipping", "Hides what is under the water from its reflection. Off: the PS2's.", RowKind::toggle,
     on_off, {}, get_reflection_clip, set_reflection_clip, nullptr, nullptr, nullptr, nullptr,
     reflection_clip_overridden},
    {"PS2 video", "", RowKind::heading},
    {"Deinterlace", "Weaves the two 224-line fields into one picture. Off: each field alone, which breaks up text.",
     RowKind::toggle, on_off, {}, get_deinterlace, set_deinterlace, nullptr, nullptr, nullptr, nullptr,
     deinterlace_overridden},
    {"Progressive scan", "Reads frame-mode pictures whole. Off: a field at a time, as the PS2.", RowKind::toggle,
     on_off, {}, get_progressive, set_progressive, nullptr, nullptr, nullptr, nullptr, progressive_overridden},
    {"Interlace comb removal",
     "Removes the comb that trails moving things while the view turns, and shows movies and menus whole.",
     RowKind::toggle, on_off, {}, get_whole_frame, set_whole_frame, nullptr, nullptr, nullptr, nullptr,
     whole_frame_overridden},
    {"Seamless texture scrolling", "Clouds, lava and water scroll without the PS2's seam.", RowKind::toggle, on_off,
     {}, get_seamless_scroll, set_seamless_scroll, nullptr, nullptr, nullptr, nullptr, seamless_scroll_overridden},
    {"Flicker filter", "The PS2's blend of each row with the one above, made for a TV.", RowKind::toggle, on_off, {},
     get_flicker_filter, set_flicker_filter, nullptr, nullptr, nullptr, nullptr, flicker_filter_overridden},
};

// ---- Controls -------------------------------------------------------------------------------------------------------
int get_twin_stick() {
    return port_twin_stick_setting() != 0;
}
void set_twin_stick(int on) {
    port_twin_stick_set(on);
    changed();
}

int get_instant_wasd() {
    return port_walk_keys_instant_setting() != 0;
}
void set_instant_wasd(int on) {
    port_walk_keys_set_instant(on);
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
    "The button this click presses in the game's menus, the pause and conversations. In play, clicks follow Key "
    "bindings.";

const SettingRow controls_rows[] = {
    {"Twin-stick controls", "Left stick moves and strafes, right stick turns and looks. Off: the PS2's layout.",
     RowKind::toggle, on_off, {}, get_twin_stick, set_twin_stick},
    {"Instant WASD acceleration",
     "W A S D walk at full speed at once and turn round at once (the left stick pushed all the way, with twin-stick "
     "controls). Off: they speed up and slow down as the game's walking always has. The keyboard only.",
     RowKind::toggle, on_off, {}, get_instant_wasd, set_instant_wasd},
    {"Separate run key",
     "Run while Left Shift (or the left stick press) is held. The action button then only talks, picks up and opens.",
     RowKind::toggle, on_off, {}, get_separate_run, set_separate_run, nullptr, nullptr, nullptr, nullptr,
     separate_run_overridden},
    {"Mouse", "", RowKind::heading},
    {"Mouse sensitivity", "", RowKind::number, {}, {25, 400, 5}, get_sensitivity, set_sensitivity, nullptr,
     describe_sensitivity},
    {"Invert Y axis", "", RowKind::toggle, on_off, {}, get_invert, set_invert},
    {"Low-latency mouse look",
     "Frames between the game's show the mouse's turn as soon as it is read. Off: the turn waits for the game's next "
     "frame.",
     RowKind::toggle, on_off, {}, get_mouse_lead, set_mouse_lead},
    {"Mouse in menus", "", RowKind::heading},
    {"Left click", menu_mouse_help, RowKind::choice, menu_presses, {}, get_menu_left, set_menu_left},
    {"Right click", menu_mouse_help, RowKind::choice, menu_presses, {}, get_menu_right, set_menu_right},
    {"Middle click", menu_mouse_help, RowKind::choice, menu_presses, {}, get_menu_middle, set_menu_middle},
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
    {ACTION_UP, "PS2 buttons"},   {ACTION_LEFT_STICK_UP, "Walking and looking"}, {ACTION_RUN, "Play"},
    {ACTION_QUICK_SAVE, "Saves"}, {ACTION_GS_CAPTURE, "Debugging"}, {ACTION_SHOW_FPS, "Display and mods"},
};

const auto binding_rows = [] {
    std::array<SettingRow, 1 + std::size(binding_groups) + ACTION_COUNT> rows = {};
    size_t at = 0;
    rows[at++] = {"Reset to defaults", "Every key and button back to its default, and the mouse settings too.",
                  RowKind::action,
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
const Choice door_speeds[] = {{"1x", 1}, {"2x", 2}, {"3x", 3}, {"4x", 4},
                              {"5x", 5}, {"6x", 6}, {"7x", 7}, {"8x", 8}};
int get_door_speed() {
    return port_options.door_speed;
}
void set_door_speed(int speed) {
    port_settings.door_speed = port_options.door_speed = speed;
    changed();
}
bool door_speed_overridden() {
    return port_options.door_speed != port_settings.door_speed;
}
const Choice game_speeds[] = {{"1x, as on the PS2", 100}, {"1.25x", 125}, {"1.5x", 150}, {"1.75x", 175}, {"2x", 200}};
int get_game_speed() { // (the row's value in hundredths: the menu can only hold whole numbers)
    return (int)std::lround(port_options.game_speed * 100.0f);
}
void set_game_speed(int hundreds) {
    port_live_set(PORT_LIVE_GAME_SPEED, hundreds / 100.0); // (the live setter takes the speed itself)
}
bool game_speed_overridden() {
    return port_options.game_speed != port_settings.game_speed;
}

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
int get_jolt() { // on: the PS2's jolt (no_hit_jolt off)
    return port_options.no_hit_jolt == 0;
}
void set_jolt(int on) {
    port_settings.no_hit_jolt = port_options.no_hit_jolt = !on;
    changed();
}
bool jolt_overridden() {
    return port_options.no_hit_jolt != port_settings.no_hit_jolt;
}

int get_auto_loot_gold() {
    return port_options.auto_loot_gold != 0;
}
void set_auto_loot_gold(int on) {
    port_settings.auto_loot_gold = port_options.auto_loot_gold = on;
    changed();
}
bool auto_loot_gold_overridden() {
    return port_options.auto_loot_gold != port_settings.auto_loot_gold;
}
int get_equipped_mark() {
    return port_options.equipped_mark != 0;
}
void set_equipped_mark(int on) {
    port_settings.equipped_mark = port_options.equipped_mark = on;
    changed();
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
    std::snprintf(out, size, "%d%%", percent);
}
int get_minimap_reach() {
    return port_automap.minimap_zoom;
}
void set_minimap_reach(int hundreds) {
    port_automap.minimap_zoom = hundreds;
    map_changed();
}
void describe_minimap_reach(int hundreds, char *out, size_t size) {
    std::snprintf(out, size, "%d00", hundreds);
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
bool update_due() {
    return update_badge_due();
}
const char *update_summary() {
    return update_badge_summary();
}


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
int get_enemies_ignore() {
    return port_enemies_ignore();
}
void set_enemies_ignore(int on) {
    port_enemies_ignore_set(on); // (never saved: one run's)
}
int get_cinematic() {
    return port_cinematic_enabled();
}
void set_cinematic(int on) {
    port_cinematic_set_enabled(on); // (never saved: one run's)
}
bool cinematic_on() {
    return port_cinematic_enabled() != 0;
}
int get_cinematic_speed() {
    return port_cinematic_speed();
}
void set_cinematic_speed(int units_per_s) {
    port_cinematic_set_speed(units_per_s);
}
// the smoothing rows count hundredths of a second
int get_move_smoothing() {
    return int(std::lround(port_cinematic_move_smoothing_s() * 100.0f));
}
void set_move_smoothing(int hundredths) {
    port_cinematic_set_move_smoothing_s(float(hundredths) / 100.0f);
}
int get_look_smoothing() {
    return int(std::lround(port_cinematic_look_smoothing_s() * 100.0f));
}
void set_look_smoothing(int hundredths) {
    port_cinematic_set_look_smoothing_s(float(hundredths) / 100.0f);
}
void describe_hundredths_s(int hundredths, char *out, size_t size) {
    std::snprintf(out, size, "%.2f s", double(hundredths) / 100.0);
}
const char cheat_help[] = "Applied once you are in play (not in a menu or message). A save made after keeps it.";


// ---- The tabs' rows (after every get / set above) ------------------------------------------------------------------
const SettingRow gameplay_rows[] = {
    {"Door speed", "How fast doors and wall panels open and close. 1x is the PS2's.", RowKind::choice, door_speeds,
     {}, get_door_speed, set_door_speed, nullptr, nullptr, nullptr, nullptr, door_speed_overridden},
    {"Game speed",
     "The gameplay runs this many times faster. Music, sound, menus, and cinematics are unaffected.",
     RowKind::choice, game_speeds, {}, get_game_speed, set_game_speed, nullptr, nullptr, nullptr, nullptr,
     game_speed_overridden},
    {"Camera jolt on hit", "A hard hit throws the view down. Off: it still flashes, rumbles and knocks you back.",
     RowKind::toggle, on_off, {}, get_jolt, set_jolt, nullptr, nullptr, nullptr, nullptr, jolt_overridden},
    {"Auto-loot gold", "Gold dropped by an enemy you kill is picked up at once.", RowKind::toggle, on_off, {},
     get_auto_loot_gold, set_auto_loot_gold, nullptr, nullptr, nullptr, nullptr, auto_loot_gold_overridden},
    {"Mark equipped items", "Shops and the blacksmith show (E) after the items you have equipped.", RowKind::toggle,
     on_off, {}, get_equipped_mark, set_equipped_mark},
    {"Hands and hotkeys", "", RowKind::heading},
    {"Hands and hotkeys",
     "Two weapons in the right hand, two spells in the left, and five hotkeys. The d-pad swaps them (Right weapon, "
     "Left spell, Up and Down hotkey; keyboard: Key bindings); Select uses the hotkey. Cross on an item in the "
     "equipment screen puts it in a slot.",
     RowKind::toggle, on_off, {}, get_hands, set_hands, nullptr, nullptr, nullptr, nullptr, hands_overridden},
    {"Fade when idle", "Fades to a quarter after a few seconds without a hand key.", RowKind::toggle, on_off, {},
     get_hands_fade, set_hands_fade, nullptr, nullptr, nullptr, hands_on},
    {"HUD size", "", RowKind::number, {},
     {int(PORT_HANDS_SCALE_MIN * 100.0f + 0.5f), int(PORT_HANDS_SCALE_MAX * 100.0f + 0.5f), 5}, get_hands_size,
     set_hands_size, nullptr, describe_percent, nullptr, hands_on},
    {"HUD position", "Top left puts it under the health and magic gauges.", RowKind::choice, hands_corners, {},
     get_hands_corner, set_hands_corner, nullptr, nullptr, nullptr, hands_on},
    {"HUD layout", "Cross: the hotkey's card a little lower.\nRow: the three in line.", RowKind::choice, hands_layouts,
     {}, get_hands_layout, set_hands_layout, nullptr, nullptr, nullptr, hands_on},
    {"Card shape", "", RowKind::choice, hands_cards, {}, get_hands_cards, set_hands_cards, nullptr, nullptr, nullptr,
     hands_on},
};

const SettingRow interface_rows[] = {
    {"Map", "", RowKind::heading},
    {"Automap",
     "A map of where you have been, kept with each save. Opened by its key (M unless changed); the game waits while "
     "it is open.",
     RowKind::toggle, on_off, {}, get_automap, set_automap},
    {"Minimap", "", RowKind::toggle, on_off, {}, get_minimap, set_minimap, nullptr, nullptr, nullptr, automap_on},
    {"Minimap position", "", RowKind::choice, minimap_corners, {}, get_minimap_corner, set_minimap_corner, nullptr,
     nullptr, nullptr, minimap_on},
    {"Minimap size", "Percent of the picture.", RowKind::number, {}, {10, 45, 1}, get_minimap_size, set_minimap_size,
     nullptr, describe_minimap_size, nullptr, minimap_on},
    {"Minimap range", "How far from you the minimap shows, in game units.", RowKind::number, {}, {20, 120, 5},
     get_minimap_reach, set_minimap_reach, nullptr, describe_minimap_reach, nullptr, minimap_on},
    {"Minimap opacity", "", RowKind::number, {}, {20, 100, 5}, get_minimap_opacity, set_minimap_opacity, nullptr,
     describe_percent, nullptr, minimap_on},
    {"Full map opacity", "How much the full map's background hides the game.", RowKind::number, {}, {40, 100, 5},
     get_full_opacity, set_full_opacity, nullptr, describe_percent, nullptr, automap_on},
    {"Extended stats", "", RowKind::heading},
    {"Extended stats",
     "Hidden numbers in the game's menus: experience to the next level, hits and spells to the next Physical and "
     "Magical point, durability, hidden effects of equipment, what an item does.",
     RowKind::toggle, on_off, {}, get_stats, set_stats, nullptr, nullptr, nullptr, nullptr, stats_overridden},
    {"Equipment durability", "Each piece's condition in percent; hits wear it down, the smith mends it.",
     RowKind::toggle, on_off, {}, get_durability, set_durability, nullptr, nullptr, nullptr, stats_on},
    {"Ailment timers", "Running ailments and timed spell and item effects, with seconds left, beside the minimap.",
     RowKind::toggle, on_off, {}, get_timers, set_timers, nullptr, nullptr, nullptr, stats_on},
    {"Hit log", "Each hit's real damage and the HP left (bottom left), and the last enemy hit's health bar (top).",
     RowKind::toggle, on_off, {}, get_damage, set_damage, nullptr, nullptr, nullptr, stats_on},
    {"Stats panel", "Everything in one panel, on its key (F6 unless changed).", RowKind::toggle, on_off, {},
     get_stats_panel, set_stats_panel, nullptr, nullptr, nullptr, stats_on},
};

const SettingRow general_rows[] = {
    {"Update available", "", RowKind::heading, {}, {}, nullptr, nullptr, nullptr, nullptr, update_due},
    {"Open download page", "Opens the release's page in your browser; install it over this one.", RowKind::action,
     {}, {}, nullptr, nullptr, update_badge_open_page, nullptr, update_due, nullptr, nullptr, update_summary},
    {"Skip this version", "No more notices until the next version.", RowKind::action, {}, {}, nullptr, nullptr,
     update_badge_skip, nullptr, update_due, nullptr, nullptr, update_summary},
    {"Audio", "", RowKind::heading},
    {"Volume", "", RowKind::number, {}, {0, 100, 5}, get_volume, set_volume, nullptr, describe_percent},
    {"Mute", "", RowKind::toggle, on_off, {}, get_mute, set_mute},
    {"Quick save", "", RowKind::heading},
    {"Quick save slot",
     "The slot F2 and F4 use (unless changed). Quick saves are the game's own save, taken where you stand, and load in "
     "any version. The save points' saves are separate.",
     RowKind::choice, quick_slots, {}, get_quick_slot, set_quick_slot, nullptr, describe_quick_slot},
    {"Quick save", "Saves when the menu closes (in play, not in menus, messages or events).", RowKind::action, {}, {},
     nullptr, nullptr, quick_save},
    {"Quick load", "Loads when the menu closes. Enemies come back as when entering a map.", RowKind::action, {}, {},
     nullptr, nullptr, quick_load, nullptr, nullptr, quick_slot_full},
    {"Menu", "", RowKind::heading},
    {"Pause while open", "The game holds still while this menu is open.", RowKind::toggle, on_off, {}, get_pause,
     set_pause},
};

const SettingRow cheats_rows[] = {
    {"Cheats", "", RowKind::heading},
    {"Invincible", "No HP lost. Off again at the next start.", RowKind::toggle, on_off, {}, get_invincible,
     set_invincible},
    {"Enemies ignore you", "Enemies never notice you, and one you hit goes back to what it was doing. Off again at "
     "the next start.",
     RowKind::toggle, on_off, {}, get_enemies_ignore, set_enemies_ignore},
    {"Myu's Necklace always on", "Its effect without wearing it, so the amulet slot is free.", RowKind::toggle, on_off,
     {}, get_necklace, set_necklace, nullptr, nullptr, nullptr, nullptr, necklace_overridden},
    {"Cinematic camera", "", RowKind::heading},
    {"Cinematic camera",
     "For filming. V or the controller's mute button flies the camera through walls and back down (unless changed "
     "in Key Bindings): it goes where you look, X and Left Ctrl rise and sink, Run is faster. The speed and the turn "
     "ease in and out, nothing can hurt you while flying, and no notice shows. Off again at the next start.",
     RowKind::toggle, on_off, {}, get_cinematic, set_cinematic},
    {"Flight speed", "Units a second; Run flies four times as fast.", RowKind::number, {},
     {PORT_CINEMATIC_SPEED_MIN, PORT_CINEMATIC_SPEED_MAX, 100}, get_cinematic_speed, set_cinematic_speed, nullptr,
     nullptr, nullptr, cinematic_on},
    {"Move smoothing", "How long the flight takes to reach the speed asked for, and to coast down again.",
     RowKind::number, {}, {5, 200, 5}, get_move_smoothing, set_move_smoothing, nullptr, describe_hundredths_s,
     nullptr, cinematic_on},
    {"Look smoothing", "How far the view trails the mouse or the stick. It arrives late, never short.",
     RowKind::number, {}, {5, 200, 5}, get_look_smoothing, set_look_smoothing, nullptr, describe_hundredths_s,
     nullptr, cinematic_on},
    {"Map", "", RowKind::heading},
    {"Reveal map", "Every floor, explored or not. Needs the automap.", RowKind::toggle, on_off, {}, get_reveal,
     set_reveal, nullptr, nullptr, nullptr, automap_on},
    {"Show items", "Items on the ground and in containers, and enemy drops, through the fog.", RowKind::toggle,
     on_off, {}, get_map_items, set_map_items, nullptr, nullptr, nullptr, automap_on},
    {"Show enemies", "Live enemies with their names and HP, through the fog.", RowKind::toggle, on_off, {},
     get_map_enemies, set_map_enemies, nullptr, nullptr, nullptr, automap_on},
    {"Show traps and secrets", "Traps and hidden or vanishing walls, through the fog.", RowKind::toggle, on_off, {},
     get_map_secrets, set_map_secrets, nullptr, nullptr, nullptr, automap_on},
    {"Show lock hints", "What each lock needs and whether you have it, and notes for puzzles (a blue ?).",
     RowKind::toggle, on_off, {}, get_map_hints, set_map_hints, nullptr, nullptr, nullptr, automap_on},
    {"Give", "", RowKind::heading},
    {"Spell level", "The level spells are given at (1 to 3). Known spells change to it too.", RowKind::number, {},
     {1, 3, 1}, get_spell_level, set_spell_level},
    {"Give all spells", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_spells, nullptr, nullptr, nullptr,
     nullptr, cheat_message},
    {"Give all weapons and armour", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_equipment, nullptr,
     nullptr, nullptr, nullptr, cheat_message},
    {"Give all items (99 each)", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_items, nullptr, nullptr,
     nullptr, nullptr, cheat_message},
    {"Max magic", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_magic, nullptr, nullptr, nullptr, nullptr,
     cheat_message},
    {"Give everything", cheat_help, RowKind::action, {}, {}, nullptr, nullptr, give_all, nullptr, nullptr, nullptr,
     nullptr, cheat_message},
};

const SettingTab tabs[] = {
    {"display", "Display", display_rows},       {"graphics", "Graphics", graphics_rows},
    {"controls", "Controls", controls_rows},    {"keys", "Key Bindings", binding_rows},
    {"gameplay", "Gameplay", gameplay_rows},    {"interface", "Interface", interface_rows},
    {"general", "General", general_rows},       {"cheats", "Cheats", cheats_rows},
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
