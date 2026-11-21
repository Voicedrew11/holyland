// The settings overlay (port/include/port/overlay.h): F1 opens a menu bar and the Settings window (Shift+F1 on the
// Vulkan presenter, where F1 is the player menu's: port/include/port/player_ui.h, which this passes events to first),
// drawn with Dear ImGui (third_party/imgui) over the game's picture, on the window's SDL_Renderer or by
// the Vulkan presenter (port/platform/present_vk.cpp, --presenter vulkan) with ImGui's Vulkan backend. The
// pages (the table `pages`): Display, Graphics, Controls, Gameplay, Saves, General, and Developer for
// testing. Settings every player wants as the port has them are not offered; they stay in settings.json.
//
// While it is open the keyboard, the mouse and the controller go to it, not to the game; the game
// runs on unless "Pause the game while the overlay is open" is on (port/platform/kernel.c holds it).
// Settings are kept in settings.json (port/overlay/settings.c). Everything here runs on the game's
// thread, at the vblank (port_pad_poll's events, port_video_present's drawing).
//
// KF4_OVERLAY_SHOT=page@vblank,... opens the overlay on that page at that vblank, even with a pad
// script, and writes the window's whole picture a few vblanks later as overlay_<page>.png in
// --dump-dir: pictures of each page, for checking it draws, without anyone at the screen. ui-<tab>[:keys] opens the
// player menu (port/include/port/player_ui.h) on that tab instead and presses those keys once
// (ui-display:down.down.confirm), written as overlay_ui-<tab>[:keys].png.
#include <SDL3/SDL.h>

#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#ifdef KF4_HAVE_VULKAN_PRESENTER
#include "backends/imgui_impl_vulkan.h"
#endif
#include "imgui.h"

#include "automap_view.h" // the automap mod
#include "profiler.h"     // Shift+P
#include "update_badge.h" // a new release, found by the launcher

extern "C" {
#include "port/antialias.h"
#include "port/adaptive_sync.h"
#include "port/audio.h"
#include "port/automap.h"
#include "port/hands.h"
#include "port/cheats.h"
#include "port/controls.h"
#include "port/twin_stick.h"
#include "port/debug.h"
#include "port/overlay.h"
#include "port/player_ui.h"
#include "port/platform.h"
#include "port/present_vk.h"
#include "port/host.h"
#include "port/live_settings.h"
#include "port/quicksave.h"
#include "port/render.h"
#include "port/savestate.h"
#include "port/settings.h"
#include "port/stats.h"
#include "port/teleport.h"
}

namespace {

const ImVec4 colour_note(0.95f, 0.8f, 0.45f, 1.0f);
const ImVec4 colour_good(0.55f, 0.85f, 0.55f, 1.0f);

SDL_Window *window;
SDL_Renderer *renderer; // NULL on the Vulkan presenter
bool ready;
bool drawn; // the Vulkan presenter: this vblank's draw lists are ImGui's draw data
bool overlay_open;
// ImGui's WantTextInput at the last frame drawn (the drawing thread), for the game's thread
std::atomic<bool> imgui_typing{false};
bool show_settings = true;
bool show_about;
int page; // an index in `pages`
float applied_scale = 1.0f; // the layout's scale: the slider's value, taken when it is let go, times the screen's
float applied_ui_scale = 1.0f; // the slider's value in applied_scale
std::string ini_path;
Uint32 toast_until;
const char *(*toast_text)(void) = port_state_message; // the message the toast shows

// the Developer page's rates, sampled once a second
struct Rates {
    double wall;
    uint64_t vblanks;
    unsigned frames;
    PortStats stats;
};
Rates last_sample;
double vblanks_per_second, frames_per_second;
double vu1_ms, gif_ms, vsync_ms, vu1_runs, vu1_insns;

// KF4_OVERLAY_SHOT
struct Shot {
    int page;
    uint64_t vblank;
    std::string ui_tab; // ui-<tab>: the player menu (port/include/port/player_ui.h) open on that tab
};
std::vector<Shot> shots;
size_t shot_next;
bool shot_mode;

// ---- Looks ---------------------------------------------------------------------------------------
void load_font() {
    ImGuiIO &io = ImGui::GetIO();
    static const char *const candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    };
    for (const char *path : candidates) {
        if (FILE *file = std::fopen(path, "rb")) {
            std::fclose(file);
            if (io.Fonts->AddFontFromFileTTF(path, 18.0f))
                return;
        }
    }
    io.Fonts->AddFontDefault();
}

// Windows' display scaling (150% is 1.5), as the window is in the screen's own pixels
// (port/platform/video.c); 1 elsewhere
float screen_scale() {
#ifdef _WIN32
    float scale = window != NULL ? SDL_GetWindowDisplayScale(window) : 0.0f;
    if (scale > 1.0f)
        return scale;
#endif
    return 1.0f;
}

void apply_style() {
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.94f;
    applied_ui_scale = port_settings.ui_scale;
    applied_scale = applied_ui_scale * screen_scale();
    style.ScaleAllSizes(applied_scale);
    style.FontScaleMain = applied_scale;
}

void tooltip(const char *text) {
    ImGui::SetItemTooltip("%s", text);
}

// ---- Opening and closing -------------------------------------------------------------------------
void set_open(bool value) {
    if (overlay_open == value)
        return;
    overlay_open = value;
    port_controls_block(overlay_open);
    if (overlay_open) {
        port_player_ui_show(nullptr); // one menu at a time: the player menu closes under this one
    } else {
        port_controls_listen_cancel();
        port_settings_save();
    }
}

void changed() {
    port_settings_changed();
}

// ---- Display -------------------------------------------------------------------------------------
void window_size_combo() {
    PortSettings &s = port_settings;
    // window sizes: the common screen heights, and the PS2's 448 lines once and twice, as wide as
    // the picture's shape (width 0); then monitors' own sizes, wide (16:9), 16:10 and ultrawide
    // (21:9), which the picture is fitted into
    struct Size {
        int width, height;
        const char *note;
    };
    static const Size sizes[] = {
        {0, 448, " (PS2 lines)"}, {0, 720, ""},   {0, 896, " (PS2 lines x2)"}, {0, 900, ""},
        {0, 1080, ""},            {0, 1440, ""},  {0, 2160, ""},               {2560, 1440, " (16:9)"},
        {2560, 1600, " (16:10)"}, {3440, 1440, " (21:9)"}, {3840, 2160, " (16:9)"}, {3840, 2400, " (16:10)"},
    };
    const int count = (int)(sizeof(sizes) / sizeof(sizes[0]));
    const bool wide = port_options.widescreen != 0;
    SDL_Rect screen = {0, 0, 0, 0};
    char label[64];
    int current = -1;

    SDL_GetDisplayBounds(window != NULL ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay(), &screen);
    auto width_of = [&](int w, int h) { return w > 0 ? w : wide ? (h * 16 + 8) / 9 : (h * 4 + 2) / 3; };
    // a height whose width the shape gives is left out where a monitor's size is the same
    auto duplicate = [&](int i) {
        for (int j = 0; sizes[i].width == 0 && j < count; j++) {
            if (sizes[j].width != 0 && sizes[j].width == width_of(0, sizes[i].height) &&
                sizes[j].height == sizes[i].height) {
                return true;
            }
        }
        return false;
    };
    const int window_width = width_of(s.window_width, s.window_height);
    for (int i = 0; i < count; i++) {
        if (!duplicate(i) && width_of(sizes[i].width, sizes[i].height) == window_width &&
            sizes[i].height == s.window_height) {
            current = i;
        }
    }
    auto size_label = [&](int i) {
        snprintf(label, sizeof(label), "%d x %d%s", width_of(sizes[i].width, sizes[i].height), sizes[i].height,
                 sizes[i].note);
        return label;
    };
    ImGui::BeginDisabled(s.fullscreen != 0);
    char shown[64];
    if (current >= 0) {
        snprintf(shown, sizeof(shown), "%s", size_label(current));
    } else {
        snprintf(shown, sizeof(shown), "%d x %d", window_width, s.window_height);
    }
    if (ImGui::BeginCombo("Window size", shown)) {
        for (int i = 0; i < count; i++) {
            // sizes bigger than the screen are left out: full screen is for those
            if (i != current && (duplicate(i) || (screen.h > 0 && (sizes[i].height > screen.h ||
                                                                    width_of(sizes[i].width, sizes[i].height) > screen.w)))) {
                continue;
            }
            if (ImGui::Selectable(size_label(i), i == current)) {
                s.window_width = sizes[i].width;
                s.window_height = sizes[i].height;
                port_video_apply_settings();
                changed();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    tooltip("The window's size; its width follows the picture's shape (4:3, or 16:9 with widescreen).\n"
            "The window can also be resized by dragging its edges. Full screen always uses the monitor's\n"
            "own resolution (1920 x 1080, 2560 x 1440, ...) and fits the picture to it.");
}

void scaling_controls() {
    PortSettings &s = port_settings;
    static const char *const scalings[] = {"Sharp pixels", "Smooth (bilinear)", "Best"};
    int scaling = s.scaling;
    if (ImGui::Combo("Scaling to the window", &scaling, scalings, 3)) {
        s.scaling = scaling;
        s.smooth = scaling != PORT_SCALING_SHARP;
        changed();
    }
    tooltip("How the picture is fitted to the window.\nSharp pixels: each pixel repeated, blocky.\n"
            "Smooth: bilinear filtering; made much smaller, fine detail flickers and edges step.\n"
            "Best (the default): a cubic filter on linear light that averages every pixel covered when\n"
            "made smaller and keeps edges sharp when made larger. (--present-scaling sharp|smooth|best)");
    if (port_options.present_scaling >= 0 && port_options.present_scaling != s.scaling) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(for this run: command line)");
    }
}

// Beside anti-aliasing (graphics_page): the presenter's sharpening, after the picture is scaled to the window
void sharpen_slider() {
    PortSettings &s = port_settings;
    float sharpen = s.sharpen;
    if (ImGui::SliderFloat("Sharpen", &sharpen, 0.0f, 1.0f, sharpen > 0 ? "%.2f" : "off")) {
        s.sharpen = sharpen < 0.02f ? 0.0f : sharpen;
        changed();
    }
    tooltip("Contrast-adaptive sharpening after the scaling to the window (so after anti-aliasing): fine\n"
            "detail crisper, hard edges left alone. Brings back some of the texture detail FXAA softens.\n"
            "0: off (the default). It uses the best scaling. (--present-sharpen 0 to 1)");
}

void frame_rate_combo() {
    static const int rates[] = {30, 60, 120, 144, 165, 240, PORT_FRAME_RATE_MONITOR, PORT_FRAME_RATE_UNLOCKED};
    static const char *const names[] = {"30 (the PS2's)", "60", "120", "144", "165", "240", "The screen's refresh rate",
                                        "Unlocked"};
    const int count = int(sizeof(rates) / sizeof(rates[0]));
    char shown[48];
    int at = -1;

    for (int i = 0; i < count; i++)
        at = rates[i] == port_options.frame_rate ? i : at;
    if (port_options.frame_rate == PORT_FRAME_RATE_MONITOR) {
        snprintf(shown, sizeof(shown), "The screen's refresh rate (%d)", port_frame_rate_hz());
    } else if (port_options.frame_rate == PORT_FRAME_RATE_UNLOCKED) {
        snprintf(shown, sizeof(shown), "Unlocked (%.0f now)", port_video_pictures_per_second());
    } else {
        snprintf(shown, sizeof(shown), "%d", port_options.frame_rate);
    }
    if (ImGui::BeginCombo("Pictures a second", shown)) {
        for (int i = 0; i < count; i++) {
            if (ImGui::Selectable(names[i], i == at))
                port_live_set(PORT_LIVE_FRAME_RATE, rates[i]);
        }
        ImGui::EndCombo();
    }
    tooltip("30: the PS2's 30 pictures a second in play.\nMore: pictures between each two of the game's, the "
            "camera, models, the HUD and fades where\nthey are at that time, so turning and walking look "
            "smooth. The game still runs at 30: what\nit does is the same, and the picture is no later. The "
            "menus are as they were. Applies at once.\nUnlocked: as many as the renderer keeps up with. A "
            "fixed rate that is not the screen's judders\nwithout adaptive sync. (--frame-rate "
            "30|60|120|144|240|monitor|unlocked)");
    if (port_options.frame_rate != port_settings.frame_rate) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(for this run: command line)");
    }
}

// VSync, adaptive sync and unlocked's tearing as one choice; settings.json keeps the three as they were
enum Sync { SYNC_OFF, SYNC_VSYNC, SYNC_ADAPTIVE };

Sync sync_now() {
    if (port_options.adaptive_sync)
        return SYNC_ADAPTIVE;
    // unlocked shows the newest picture each refresh (mailbox) unless tearing is allowed; other rates follow VSync
    if (port_options.frame_rate == PORT_FRAME_RATE_UNLOCKED)
        return port_options.tearing ? SYNC_OFF : SYNC_VSYNC;
    return port_settings.vsync ? SYNC_VSYNC : SYNC_OFF;
}

void sync_set(Sync sync) {
    PortSettings &s = port_settings;
    s.adaptive_sync = port_options.adaptive_sync = sync == SYNC_ADAPTIVE;
    if (sync != SYNC_ADAPTIVE) {
        s.vsync = sync == SYNC_VSYNC;
        s.tearing = port_options.tearing = sync == SYNC_OFF;
    }
    port_video_apply_settings();
    changed();
}

void sync_combo() {
    static const char *const names[] = {"Off (least latency, may tear)", "VSync (no tearing)",
                                        "Adaptive sync (FreeSync, G-Sync)"};
    int sync = sync_now();
    if (ImGui::Combo("Sync", &sync, names, 3))
        sync_set(Sync(sync));
    tooltip("Off: each picture is shown at once, part way down the screen if need be (tearing).\n"
            "VSync: each picture waits for the screen's refresh; unlocked, the screen shows the newest\n"
            "picture at each refresh. On a 60 Hz screen a picture is shown twice about every 17 seconds.\n"
            "Adaptive sync: for a VRR screen, which refreshes when each picture comes, so any rate is\n"
            "shown evenly, without tearing or judder; every rate is kept a little under the screen's.\n"
            "VRR must be on for the screen and the system too (on Linux the desktop's display settings;\n"
            "most only for a full-screen window). (--adaptive-sync, --tearing)");
    if (port_options.adaptive_sync && port_video_refresh_rate() > 0) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(up to %d a second)", adaptive_sync_cap_hz(port_video_refresh_rate()));
    }
    if (port_options.adaptive_sync != port_settings.adaptive_sync) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(for this run: command line)");
    }
}

void display_page() {
    PortSettings &s = port_settings;

    ImGui::SeparatorText("Window");
    window_size_combo();
    bool fullscreen = s.fullscreen != 0;
    if (ImGui::Checkbox("Full screen (borderless)", &fullscreen)) {
        s.fullscreen = fullscreen;
        port_video_apply_settings();
        changed();
    }
    tooltip("Also F11.");
    const char *const aspects[] = {port_options.widescreen ? "Keep 16:9" : "Keep 4:3, as on a TV (PS2)",
                                   "Stretch to the window"};
    if (ImGui::Combo("Picture shape", &s.aspect, aspects, 2))
        changed();
    tooltip("Keep: the picture keeps its shape, with black bars at the sides or top.\nStretch fills the window "
            "and distorts the picture.");
    scaling_controls();

    ImGui::SeparatorText("Frame rate");
    frame_rate_combo();
    sync_combo();
}

// ---- Graphics ------------------------------------------------------------------------------------
// The texture detail (mipmap level) and anisotropic filtering as one choice; "custom" for any other mix
struct TextureQuality {
    const char *name;
    int lod;   // port_options.native_lod: 0 the PS2's, 1 matched to the resolution, 2 finest
    int aniso; // port_options.native_aniso
};
const TextureQuality texture_qualities[] = {
    {"PS2's", 0, 1}, {"Standard", 1, 1}, {"High (anisotropic 16x)", 1, 16}, {"Maximum (finest, 16x)", 2, 16}};
const int TEXTURE_QUALITY_COUNT = int(sizeof(texture_qualities) / sizeof(texture_qualities[0]));

void texture_quality_combo() {
    int at = -1;
    for (int i = 0; i < TEXTURE_QUALITY_COUNT; i++) {
        if (texture_qualities[i].lod == port_options.native_lod && texture_qualities[i].aniso == port_options.native_aniso)
            at = i;
    }
    if (ImGui::BeginCombo("Texture quality", at >= 0 ? texture_qualities[at].name : "Custom (settings file)")) {
        for (int i = 0; i < TEXTURE_QUALITY_COUNT; i++) {
            if (!ImGui::Selectable(texture_qualities[i].name, i == at))
                continue;
            port_gs_set_native_lod(texture_qualities[i].lod);
            port_gs_set_native_aniso(texture_qualities[i].aniso);
            port_settings.native_lod = texture_qualities[i].lod;
            port_settings.native_aniso = port_options.native_aniso;
            changed();
        }
        ImGui::EndCombo();
    }
    tooltip("How sharp textures stay with distance.\nPS2's: the PS2's mipmap levels, made for 640 x 448: far "
            "surfaces blur at a higher resolution.\nStandard (the default): the level the PS2 would pick at "
            "this resolution.\nHigh: also anisotropic filtering, so floors and walls seen at a grazing angle "
            "stay sharp;\na little GPU time.\nMaximum: the whole texture at every distance, sharpest, but far "
            "surfaces can shimmer.\nOnly mipmapped surfaces (about two thirds of the maps). (--native-lod, "
            "--native-aniso)");
}

void graphics_page() {
    PortSettings &s = port_settings;
    const bool pc = port_options.renderer != PORT_RENDERER_GS;

    ImGui::SeparatorText("Picture");
    bool widescreen = port_options.widescreen != 0;
    if (ImGui::Checkbox("Widescreen (16:9)", &widescreen))
        port_live_set(PORT_LIVE_WIDESCREEN, widescreen);
    tooltip("Off: the PS2's 4:3 picture.\nOn: the game's view is widened to 16:9 (the same height, more at the "
            "sides), and the\nmenus, the message boxes and the HUD keep their 4:3 shape. Applies at once. "
            "(--widescreen)");
    if (widescreen) {
        static const char *const huds[] = {"In the corner of the 16:9 picture", "Centred, where it is at 4:3"};
        int hud = port_options.widescreen_hud_centred != 0;
        ImGui::Indent();
        if (ImGui::Combo("HUD", &hud, huds, 2)) {
            s.widescreen_hud_centred = port_options.widescreen_hud_centred = hud;
            changed();
        }
        tooltip("Where the widescreen picture puts the HUD (the gauges of the top left).");
        ImGui::Unindent();
    }
    if (pc) {
        int scale = port_options.native_scale;
        char scale_name[32];
        if (scale >= 1) {
            snprintf(scale_name, sizeof(scale_name), "%dx", scale);
        } else {
            snprintf(scale_name, sizeof(scale_name), "Auto (%dx)", port_gs_native_scale());
        }
        if (ImGui::SliderInt("Resolution", &scale, 0, 4, scale_name, ImGuiSliderFlags_AlwaysClamp))
            port_live_set(PORT_LIVE_NATIVE_SCALE, scale);
        tooltip("The game is drawn at this many times the PS2's 640 x 448 (2x: 1280 x 896, 4x: 2560 x 1792).\n"
                "1x is the PS2's picture. Auto (the default, all the way left): the multiple of 448 lines\n"
                "nearest the height the window shows the picture at (1080: 2x, 1440: 3x, 2160: 4x), at most\n"
                "2x on an integrated GPU. Applies from the next picture. (--native-scale auto|1-4)");
        const char *aa_labels[PORT_AA_COUNT];
        for (int m = 0; m < PORT_AA_COUNT; m++)
            aa_labels[m] = port_antialias(m)->label;
        int aa = port_options.antialias;
        if (ImGui::Combo("Anti-aliasing", &aa, aa_labels, PORT_AA_COUNT))
            port_live_set(PORT_LIVE_ANTIALIAS, aa);
        tooltip("Smooths the stair-steps along edges, on the finished picture at the resolution above, before\n"
                "it is scaled to the window. FXAA: cheap, but it softens fine detail and the 2D (text, the HUD)\n"
                "a little too. SMAA: finds edges by their shape, softens less, costs a little more.\n"
                "SMAA T2x: SMAA on two pictures drawn half a pixel apart, the last one moved to where things are\n"
                "now: steadier thin edges in motion. Off is the picture as drawn. Applies from the next picture.\n"
                "(--antialias off|fxaa|smaa|smaa_t2x)");
        if (port_options.antialias != s.antialias) {
            ImGui::SameLine();
            ImGui::TextColored(colour_note, "(for this run: command line)");
        }
        sharpen_slider();
        texture_quality_combo();
    }

    ImGui::SeparatorText("World");
    static const char *const distances[PORT_DRAW_DISTANCE_COUNT] = {"1x (PS2)", "1.5x", "2x", "4x", "Max (no distance fog)"};
    int distance = port_options.draw_distance;
    if (ImGui::Combo("Draw distance", &distance, distances, PORT_DRAW_DISTANCE_COUNT))
        port_live_set(PORT_LIVE_DRAW_DISTANCE, distance);
    tooltip("1x: the PS2's fog.\nFurther: the fog starts and ends that many times as far away, so more of "
            "the world\nshows through it. Max removes the distance fog, also the mist that is part of a\n"
            "place's look (the Holy Forest). Where the game draws nothing, the background shows in\n"
            "the fog's colour. Applies at once. (--draw-distance)");
    if (port_options.draw_distance != s.draw_distance) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(for this run: command line)");
    }
    if (port_options.draw_distance != PORT_DRAW_DISTANCE_1) {
        ImGui::Indent();
        bool sky_fog = port_options.draw_distance_sky != 0;
        if (ImGui::Checkbox("Push the sky's fog out too", &sky_fog)) {
            s.draw_distance_sky = port_options.draw_distance_sky = sky_fog;
            changed();
        }
        tooltip("Off: the sky keeps the PS2's fog (the sky is a dome around you, not further away).\nOn: its "
                "fog goes out as well, which lightens it: the dark sky of the Village Ruins\nshows its clouds. "
                "(--draw-distance-sky push)");
        ImGui::Unindent();
    }
    static const char *const skies[] = {"PS2 (at the feet, its real distance)", "Modern (around the eye, behind everything)"};
    int sky = port_options.native_sky == PORT_SKY_PS2 ? 0 : 1;
    if (ImGui::Combo("Sky", &sky, skies, 2))
        port_live_set(PORT_LIVE_SKY, sky == 0 ? PORT_SKY_PS2 : PORT_SKY_MODERN);
    tooltip("Modern (the default): the sky's dome is centred on the eye and drawn behind everything, as a\n"
            "modern game's sky: what lies beyond it shows (the tops of far hills on a few maps).\nPS2: the dome "
            "at the feet with its real distance, as the PS2 draws it: it hides what lies\nbeyond it. Applies at "
            "once. (--sky modern|ps2)");
    if (port_options.native_sky != s.native_sky) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(for this run: command line)");
    }
    bool clip = port_options.reflection_clip != 0;
    if (ImGui::Checkbox("Cut the water's reflection at the water", &clip)) {
        s.reflection_clip = port_options.reflection_clip = clip;
        changed();
    }
    tooltip("On (the default): what is under the water, a zombie's feet in a pond, is not in its\nreflection. "
            "Off: as on the PS2, which mirrors the whole scene, the feet sticking up above\nthe water. With "
            "native draw only. Applies at once. (--reflection-clip on|off)");
    if (port_options.reflection_clip != s.reflection_clip) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(for this run: command line)");
    }
}

// The PS2's TV picture fixes, each on by default (Developer > Renderer): all off shows the picture as the PS2 sent it
void ps2_picture_section() {
    PortSettings &s = port_settings;
    ImGui::TextUnformatted("The PS2's picture: fixes for what it made for a TV");
    struct Switch {
        const char *label;
        const char *tip;
        int *setting;
        int *option;
        bool ps2_when_on; /* the option on is the PS2's way: the box shows it off */
        bool gs_only;     /* paraLLEl-GS's scanout's (--renderer gs): the PC renderer has no such step */
    } switches[] = {
        {"Weave the two fields (deinterlace)",
         "The game draws each field as its own 224-line picture, half a line apart.\nOn: the two are woven "
         "into one, as a TV shows them. Off: each field alone,\nwhich breaks up text. (--no-deinterlace)",
         &s.deinterlace, &port_options.deinterlace},
        {"Read a whole-frame picture whole (progressive)",
         "On: a frame-mode interlaced picture is read whole.\nOff: a field at a time, as the GS scans it out. "
         "(--no-progressive)",
         &s.progressive, &port_options.progressive},
        {"Undo the blur of blended fields (anti-blur)",
         "On: undoes the blur of a game that blends its two read circuits.\nOff: keeps it, as on a TV. "
         "(--no-anti-blur)",
         &s.anti_blur, &port_options.anti_blur, false, true},
        {"Picture width follows the game's (adapt width)",
         "On: the picture is scanned out at the game's width.\nOff: at the TV's width. (--no-adapt-width)",
         &s.adapt_width, &port_options.adapt_width, false, true},
        {"Whole frames only (no interlace comb)",
         "On: the game leaves out the picture between it draws over every other line while the view\n"
         "turns, for the other field of an interlaced TV, which on a PC screen is a comb trailing what\n"
         "moves, and shows the movies' and the menus' pictures whole. Off: as on the PS2. (--no-whole-frame)",
         &s.whole_frame, &port_options.whole_frame},
        {"Scrolling textures without a seam",
         "On: the sky's clouds, the lava and the water repeat evenly. Off: as on the PS2, with a line\n"
         "where the clouds or the lava warp. (--no-seamless-scroll)",
         &s.seamless_scroll, &port_options.seamless_scroll},
        {"No flicker filter (no up-and-down blur)",
         "On (the default): the rows as the game drew them. Off: as on the PS2, each row blended half\n"
         "with the row above, against an interlaced TV's flicker. (--flicker-filter)",
         &s.flicker_filter, &port_options.flicker_filter, true},
    };
    for (Switch &item : switches) {
        if (item.gs_only && port_options.renderer != PORT_RENDERER_GS)
            continue;
        bool value = (*item.option != 0) != item.ps2_when_on;
        if (ImGui::Checkbox(item.label, &value)) {
            *item.setting = *item.option = value != item.ps2_when_on;
            changed();
        }
        tooltip(item.tip);
        if (*item.option != *item.setting) {
            ImGui::SameLine();
            ImGui::TextColored(colour_note, "(off for this run: command line)");
        }
    }
    if (ImGui::Button("All off: as the PS2 shows it")) {
        for (Switch &item : switches)
            *item.setting = *item.option = item.ps2_when_on;
        changed();
    }
    ImGui::SameLine();
    if (ImGui::Button("All on: the PC's picture")) {
        for (Switch &item : switches)
            *item.setting = *item.option = !item.ps2_when_on;
        changed();
    }
}

// ---- Controls ------------------------------------------------------------------------------------
void binding_button(int action, int slot) {
    int listen_action, listen_slot;
    bool listening = port_controls_listening(&listen_action, &listen_slot) && listen_action == action &&
                     listen_slot == slot;
    char label[96];

    if (listening) {
        std::snprintf(label, sizeof(label), "%s###b%d_%d", slot == BINDING_PAD_SLOT ? "Press a button..." : "Press a key...",
                      action, slot);
    } else {
        char name[64];
        port_controls_binding_label(port_controls_binding(action, slot), name, sizeof(name));
        std::snprintf(label, sizeof(label), "%s###b%d_%d", name, action, slot);
    }
    if (listening)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    if (ImGui::Button(label, ImVec2(-FLT_MIN, 0)))
        port_controls_listen(action, slot);
    if (listening)
        ImGui::PopStyleColor();
    if (!listening && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        port_controls_bind(action, slot, BINDING_NONE);
        changed();
    }
}

// The mouse's buttons outside play: which pad button each presses
void mouse_in_menus() {
    static const int menu_actions[] = {-1, ACTION_CROSS, ACTION_CIRCLE, ACTION_SQUARE, ACTION_TRIANGLE,
                                       ACTION_START, ACTION_SELECT};
    static const char *const menu_labels[] = {"Nothing", "Cross", "Circle", "Square", "Triangle", "Start", "Select"};
    static const char *const buttons[3] = {"Left button", "Right button", "Middle button"};
    for (int button = 0; button < 3; button++) {
        int current = 0;
        for (int i = 0; i < 7; i++)
            if (menu_actions[i] == port_controls_menu_mouse(button))
                current = i;
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("Triangle").x * 2.0f);
        if (ImGui::Combo(buttons[button], &current, menu_labels, 7)) {
            port_controls_set_menu_mouse(button, menu_actions[current]);
            changed();
        }
    }
}

void controls_page() {
    PortSettings &s = port_settings;
    int listen_action, listen_slot;
    static int was_listening;
    bool listening = port_controls_listening(&listen_action, &listen_slot);

    // a binding was made (or cleared) since the last frame
    if (was_listening && !listening)
        changed();
    was_listening = listening;

    ImGui::TextWrapped("One set of controls: W A S D walk and step sideways, the mouse turns and looks (held in "
                       "play, let go in the menus), Space attacks, Q casts, E uses, Escape opens the menu and goes "
                       "back. A controller works at the same time.");

    bool twin_stick = port_twin_stick_setting() != 0;
    if (ImGui::Checkbox("Twin-stick controls", &twin_stick)) {
        port_twin_stick_set(twin_stick);
        changed();
    }
    tooltip("The left stick walks and steps sideways, the right stick turns and looks, each as far as it\n"
            "is pushed, up to the game's own speeds. Off, the sticks are the PS2's: the left one is the\n"
            "d-pad (walk and turn), the right one only looks up and down.");
    bool separate_run = port_options.separate_run != 0;
    if (ImGui::Checkbox("Separate run key", &separate_run)) {
        s.separate_run = port_options.separate_run = separate_run;
        changed();
    }
    tooltip("Run while Run's key (Left Shift, or a controller's left stick press) is held, instead of\n"
            "while the action button is: the action button then only talks, picks up and opens.\n"
            "Applies at once. (--separate-run)");
    if (port_options.separate_run != s.separate_run) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(on for this run: command line)");
    }

    ImGui::SeparatorText("Mouse");
    float sensitivity = (float)port_controls_mouse_sensitivity();
    bool invert = port_controls_mouse_invert() != 0;
    if (ImGui::SliderFloat("Mouse sensitivity", &sensitivity, 0.25f, 4.0f, "%.2fx",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        port_controls_set_mouse(sensitivity, invert);
        changed();
    }
    if (ImGui::Checkbox("Invert mouse up / down", &invert)) {
        port_controls_set_mouse(sensitivity, invert);
        changed();
    }
    bool menus_open = ImGui::TreeNode("Mouse buttons in menus");
    tooltip("Outside play (menus, pause, talking) the mouse presses these. In play, with the mouse\ncaptured, "
            "its buttons are the bindings below.");
    if (menus_open) {
        mouse_in_menus();
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Bindings");
    ImGui::TextWrapped("Click a slot and press a key, mouse button or controller button. Delete or a right click "
                       "clears it; F1 or five seconds cancel. A controller's sticks are the pad's sticks.");
    if (listening) {
        ImGui::TextColored(colour_note, "Waiting for %s, slot %d...", port_controls_action_label(listen_action),
                           listen_slot + 1);
    }
    // the table scrolls in what is left of the page, and the page scrolls when that is too little
    float table_height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.4f;
    if (table_height < ImGui::GetFrameHeightWithSpacing() * 8)
        table_height = ImGui::GetFrameHeightWithSpacing() * 8;
    if (ImGui::BeginTable("bindings", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0, table_height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int action = 0; action < ACTION_COUNT; action++) {
            if (!port_controls_action_used(action))
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (port_controls_is_default(action))
                ImGui::TextUnformatted(port_controls_action_label(action));
            else
                ImGui::TextColored(colour_note, "%s *", port_controls_action_label(action));
            for (int slot = 0; slot < BINDING_SLOTS; slot++) {
                ImGui::TableNextColumn();
                binding_button(action, slot);
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Defaults")) {
        port_controls_reset();
        changed();
    }
    tooltip("Puts the keys, buttons and mouse settings back as they were.");
    ImGui::SameLine();
    ImGui::TextDisabled("* changed from the default");
}

// ---- Saves ---------------------------------------------------------------------------------------
void key_name(int action, char *out, size_t size) {
    port_controls_binding_label(port_controls_binding(action, 0), out, size);
}

void saves_page() {
    PortSettings &s = port_settings;
    char save_key[64], load_key[64];

    key_name(ACTION_GAME_SAVE, save_key, sizeof(save_key));
    key_name(ACTION_GAME_LOAD, load_key, sizeof(load_key));
    ImGui::SeparatorText("Quick saves");
    ImGui::TextWrapped("Use these to play: they load in any build of the program, so they survive updates. %s "
                       "saves to the chosen slot and %s loads it, in play (not in menus, messages or events); "
                       "a load works on the title screen too.",
                       save_key, load_key);
    tooltip("A quick save is the game's own save (what a save point writes), taken where you "
            "stand: the map, your position, heading and look, HP and MP, level, stats, items, magic, gold and the "
            "world's flags (doors, chests, events, items picked up).\n"
            "Loading restarts the map from it, as loading a save does: enemies come back as when "
            "you enter a map, and what lasts only a while (ailments' timers, the power and magic gauges, "
            "spell effects) starts afresh. The save point's three saves are left as they are.");
    if (ImGui::BeginTable("quick", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Saved", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (int slot = 0; slot < PORT_QUICK_SLOTS; slot++) {
            char label[32];
            PortQuickInfo info;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::snprintf(label, sizeof(label), "Quick %d##quick", slot);
            if (ImGui::Selectable(label, s.quick_slot == slot, ImGuiSelectableFlags_SpanAllColumns)) {
                s.quick_slot = slot;
                changed();
            }
            ImGui::TableNextColumn();
            if (port_quick_info(slot, &info)) {
                ImGui::Text("map %02X, level %d, HP %d/%d", info.map_tag, info.level, info.hp, info.hp_max);
                ImGui::TableNextColumn();
                char when[64] = "saved";
                time_t time = (time_t)info.time;
                if (struct tm *local = std::localtime(&time))
                    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", local);
                ImGui::TextUnformatted(when);
            } else {
                ImGui::TextDisabled("empty");
                ImGui::TableNextColumn();
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Quick save"))
        port_quick_request_save(s.quick_slot);
    ImGui::SameLine();
    PortQuickInfo info;
    ImGui::BeginDisabled(!port_quick_info(s.quick_slot, &info));
    if (ImGui::Button("Quick load"))
        port_quick_request_load(s.quick_slot);
    ImGui::EndDisabled();
    if (port_overlay_paused())
        ImGui::TextColored(colour_note, "The game is paused: a request is taken when the overlay closes.");
    ImGui::TextDisabled("%s", port_quick_message());
}

// Save states (Developer): the whole running game, exact but for this build only
void save_states_section() {
    PortSettings &s = port_settings;
    char save_key[64], load_key[64];

    if (!ImGui::CollapsingHeader("Save states"))
        return;
    key_name(ACTION_QUICK_SAVE, save_key, sizeof(save_key));
    key_name(ACTION_QUICK_LOAD, load_key, sizeof(load_key));
    ImGui::TextWrapped("A save state is the whole running game, written to a file and read back: exact, but it "
                       "loads only in the build that made it, so an update makes it useless. %s saves to the "
                       "quick slot and %s loads it (Controls rebinds them).",
                       save_key, load_key);
    ImGui::Spacing();
    if (ImGui::BeginTable("slots", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Saved", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (int slot = 0; slot < 10; slot++) {
            char label[48];
            char when[64] = "empty";
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (slot == 0)
                std::snprintf(label, sizeof(label), "Quick slot");
            else
                std::snprintf(label, sizeof(label), "Slot %d", slot);
            if (ImGui::Selectable(label, s.save_slot == slot, ImGuiSelectableFlags_SpanAllColumns)) {
                s.save_slot = slot;
                changed();
            }
            ImGui::TableNextColumn();
            if (port_state_exists(slot)) {
                time_t time = (time_t)port_state_time(slot);
                struct tm *local = std::localtime(&time);
                if (local != nullptr)
                    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", local);
                else
                    std::snprintf(when, sizeof(when), "saved");
                ImGui::TextUnformatted(when);
            } else {
                ImGui::TextDisabled("%s", when);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    char button[48];
    std::snprintf(button, sizeof(button), "Save to %s", s.save_slot == 0 ? "the quick slot" : "the slot");
    if (ImGui::Button(button))
        port_state_request_save(s.save_slot);
    ImGui::SameLine();
    ImGui::BeginDisabled(!port_state_exists(s.save_slot));
    if (ImGui::Button("Load it"))
        port_state_request_load(s.save_slot);
    ImGui::EndDisabled();
    if (port_overlay_paused())
        ImGui::TextColored(colour_note, "The game is paused: a request is taken when the overlay closes.");
    ImGui::Spacing();
    ImGui::TextDisabled("%s", port_state_message());
}

// ---- Debug ---------------------------------------------------------------------------------------
void sample_rates() {
    double now = port_wall_seconds();
    double span = now - last_sample.wall;
    PortGameReadout game;

    if (span < 1.0)
        return;
    kf4_game_readout(&game);
    uint64_t vblanks = port_vblank_count();
    PortStats stats = port_stats;
    if (last_sample.wall > 0.0) {
        double per_second = port_ticks_per_second();
        vblanks_per_second = (double)(vblanks - last_sample.vblanks) / span;
        frames_per_second = (double)(game.frame - last_sample.frames) / span;
        vu1_ms = (double)(stats.vu1_ticks - last_sample.stats.vu1_ticks) / per_second * 1000.0 / span;
        gif_ms = (double)(stats.gif_ticks - last_sample.stats.gif_ticks) / per_second * 1000.0 / span;
        vsync_ms = (double)(stats.vsync_ticks - last_sample.stats.vsync_ticks) / per_second * 1000.0 / span;
        vu1_runs = (double)(stats.vu1_runs - last_sample.stats.vu1_runs) / span;
        vu1_insns = (double)(stats.vu1_insns - last_sample.stats.vu1_insns) / span;
    }
    last_sample.wall = now;
    last_sample.vblanks = vblanks;
    last_sample.frames = game.frame;
    last_sample.stats = stats;
}

std::string bug_report() {
    char line[512];
    std::string text = std::string(update_badge_version_line()) + " (King's Field IV PC port)\n";

    std::snprintf(line, sizeof(line), "vblank %llu, %.1f pictures/s, %.2f vblanks/s, %.1f game frames/s\n",
                  (unsigned long long)port_vblank_count(), port_video_pictures_per_second(), vblanks_per_second, frames_per_second);
    text += line;
    kf4_game_state(line, sizeof(line));
    text += line;
    text += "\n";
    std::snprintf(line, sizeof(line),
                  "options: unpaced=%d deinterlace=%d progressive=%d anti_blur=%d adapt_width=%d "
                  "no_audio=%d device_clock=%d\n",
                  port_options.fast, port_options.deinterlace,
                  port_options.progressive, port_options.anti_blur, port_options.adapt_width, port_options.no_audio,
                  port_audio_device_clock());
    text += line;
    std::snprintf(line, sizeof(line),
                  "renderer: %s scale=%d (%dx) widescreen=%d sharper_widescreen=%d native_draw=0x%x frame_rate=%d "
                  "draw_distance=%d sky=%d presenter=%d\n",
                  port_options.renderer == PORT_RENDERER_GS ? "gs" : port_options.renderer == PORT_RENDERER_PC ? "pc" : "scene",
                  port_options.native_scale,
                  port_gs_native_scale(), port_options.widescreen, port_options.native_widescreen,
                  port_options.native_draw, port_options.frame_rate, port_options.draw_distance, port_options.native_sky,
                  port_video_presenter());
    text += line;
    return text;
}

// Native draw's kinds (docs/port/NATIVE_DRAW.md): the game's 3D drawn by the PC renderer's shaders, or each kind by
// VU1 and its packets as on the PS2, one by one
void native_draw_kinds() {
    static const struct {
        const char *name;
        int bits;
        const char *what;
    } kinds[] = {{"Map", PORT_ND_MAP, "The map's own geometry (Map_DrawRecords, Map_DrawLateRecords) but the sky."},
                 {"Sky", PORT_ND_SKY, "The sky's dome (the map record with flag bit 31), drawn as Display's Sky says."},
                 {"Items", PORT_ND_ITEMS, "The items lying in the maps (MapItem_DrawAll)."},
                 {"Picked item", PORT_ND_PICKED, "The item being picked up, after its depth clear."},
                 {"Map objects", PORT_ND_OBJECTS, "Doors, chests, levers ... (Model_DrawPosedWithAlpha, MapObject_DrawAll)."},
                 {"Creatures", PORT_ND_SKINNED, "Enemies and NPCs (Entity_DrawAll, the draw list)."},
                 {"Arm", PORT_ND_ARM, "The arm, the held weapon and its trail (Player_DrawArm)."},
                 {"Gold", PORT_ND_GOLD, "The gold lying in the maps (Gold_Draw, the draw list)."},
                 {"Effects", PORT_ND_EFFECTS, "Fire, glows, magic (the draw list's effects, VU1 program 0x700)."},
                 {"Shadows", PORT_ND_SHADOWS, "The creatures' shadows (Entity_DrawPoolModels, VU1 program 0x5A0)."},
                 {"Water", PORT_ND_WATER, "The reflection's rippled water on maps 008, 009 and 014 (Wobble_DrawRippledModel)."},
                 {"Screen passes", PORT_ND_PASSES,
                  "The brightness, tints, fills and fades, the flicker filter, the frame before, haze, the wobble."}};
    const int drawable = port_gs_native_kinds();
    if (drawable == 0) {
        ImGui::TextDisabled("Native draw: needs the PC renderer.");
        return;
    }
    ImGui::TextUnformatted("Native draw: the game's 3D by the renderer's own shaders");
    tooltip("On (every kind, the default): the game's 3D is drawn by the PC renderer's vertex shaders doing VU1's\n"
            "maths; between the game's frames moving models move smoothly. A kind turned off is drawn as on the\n"
            "PS2: VU1 and its packets. All on or all off is saved; a choice of kinds is for this run.\n"
            "(--native-draw on|off|KINDS)");
    int kinds_on = port_options.native_draw;
    int shown = 0;
    ImGui::PushID("native_draw"); // (the kinds' labels, "Map" above all, also name widgets elsewhere in the window)
    for (const auto &k : kinds) {
        if ((drawable & k.bits) == 0)
            continue;
        if (shown++ % 5 != 0)
            ImGui::SameLine();
        bool on = (kinds_on & k.bits) != 0;
        if (ImGui::Checkbox(k.name, &on))
            kinds_on = on ? kinds_on | k.bits : kinds_on & ~k.bits;
        tooltip(k.what);
    }
    ImGui::PopID();
    if (ImGui::Button("All (the default)"))
        kinds_on = drawable;
    ImGui::SameLine();
    if (ImGui::Button("None (the PS2's way)"))
        kinds_on = 0;
    if (kinds_on != port_options.native_draw)
        port_live_set(PORT_LIVE_NATIVE_DRAW, kinds_on);
    ImGui::TextDisabled("Not drawn natively yet: the water's ripple and the screen passes (VU1's way).");
    if (port_options.native_draw)
        ImGui::TextDisabled("%llu draws queued, %llu lost, %llu meshes held, %llu parsed", (unsigned long long)rd_stats.draws,
                            (unsigned long long)rd_stats.lost, (unsigned long long)rd_stats.meshes_held,
                            (unsigned long long)rd_stats.meshes_parsed);
}

// The renderer, for testing (docs/port/PC_RENDERER.md): who draws this run, native draw's kinds, the presenter, and
// the settings the PC renderer does not apply yet
void renderer_section() {
    if (!ImGui::CollapsingHeader("Renderer"))
        return;
    if (port_options.renderer == PORT_RENDERER_GS)
        ImGui::TextColored(colour_note, "This run: paraLLEl-GS (--renderer gs), the exact GS, to compare pictures with.");
    else
        ImGui::TextUnformatted("This run: the PC renderer.");
    tooltip("The PC renderer is the one players get. --renderer gs runs paraLLEl-GS instead, the accurate GS\n"
            "emulator, to compare pictures against; it is never chosen here or in settings.json.");
    native_draw_kinds();
    ImGui::Spacing();
    static const char *const presenters[] = {"SDL_Renderer (sdl)", "Vulkan, on the renderer's device (vulkan, the default)"};
    int presenter = port_video_presenter();
    if (port_options.presenter == PORT_PRESENTER_VULKAN && presenter != PORT_PRESENTER_VULKAN)
        ImGui::Text("Presenter this run: %s (vulkan %s)", presenters[presenter],
                    port_options.renderer == PORT_RENDERER_GS ? "needs the PC renderer" : "did not start");
    else
        ImGui::Text("Presenter this run: %s", presenters[presenter]);
    int next_presenter = port_settings.presenter;
    if (ImGui::Combo("Presenter, from the next start", &next_presenter, presenters, 2)) {
        port_settings.presenter = next_presenter;
        port_settings_changed();
    }
    tooltip("How the picture reaches the window. SDL_Renderer copies each picture through the CPU into\n"
            "a texture. Vulkan presents on the renderer's own device: the picture goes to\n"
            "the window without leaving the GPU (faster, most of all at higher resolutions). Vulkan needs\n"
            "the PC renderer (not --renderer gs); without it, or if it fails, the window is\n"
            "SDL_Renderer's. --presenter on the command line wins over this.");
    ImGui::Spacing();
    ps2_picture_section();
}

// The debugging tools (port/include/port/debug.h): files for a bug report, in the captures folder
void debug_tools() {
    char keys[3][64];
    const int actions[3] = {ACTION_GS_CAPTURE, ACTION_STATE_DUMP, ACTION_AUDIO_TAIL};
    for (int i = 0; i < 3; i++)
        port_controls_binding_label(port_controls_binding(actions[i], 0), keys[i],
                                    sizeof(keys[i]));

    if (!ImGui::CollapsingHeader("Debugging tools"))
        return;
    ImGui::BeginDisabled(!port_gs_enabled() || port_debug_gs_capture_busy());
    if (ImGui::Button("GS frame capture"))
        port_debug_request_gs_capture();
    ImGui::EndDisabled();
    tooltip("The GS's and VU1's state and everything drawn in the next two vblanks, for kf4-gsreplay\n"
            "(docs/port/README.md, \"Debugging\"). Any build reads it.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", keys[0]);
    ImGui::SameLine();
    if (ImGui::Button("Game state dump"))
        port_debug_write_state_dump();
    tooltip("The map, the player, the entities and map objects near, the story's flags and the last\n"
            "events, as JSON.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", keys[1]);
    ImGui::SameLine();
    if (ImGui::Button("Last 20 s of sound"))
        port_debug_write_audio_tail();
    tooltip("The sound as the game made it (before the volume), as a WAV.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", keys[2]);
#ifdef KF4_HAVE_VULKAN_PRESENTER
    // the flash recorder (port/include/port/present_vk.h): the last pictures shown, kept on the GPU
    {
        char key[64];
        port_controls_binding_label(port_controls_binding(ACTION_FLASH_RECORD, 0), key, sizeof(key));
        bool on = port_options.flash_seconds > 0;
        if (ImGui::Checkbox("Flash recorder", &on))
            port_options.flash_seconds = on ? 2.0f : 0.0f;
        tooltip("Keeps the last 2 seconds of pictures shown (each scaled to 540 rows) on the GPU. Its key\n"
                "writes them as numbered PNGs into a folder of their own in the captures folder, with a GS\n"
                "capture of the next vblanks, a quick save and a save state: press it right after seeing a\n"
                "picture flash wrong. tools/flash_find.py finds the pictures that differ from both\n"
                "neighbours. Costs one small copy a picture (--flash-recorder SECONDS,HEIGHT). Not saved.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!on);
        if (ImGui::Button("Write the last pictures"))
            port_debug_flash_record();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", key);
        int pictures, width, height;
        double megabytes;
        bool writing = port_vk_flash_info(&pictures, &width, &height, &megabytes) != 0;
        if (on && width > 0)
            ImGui::TextDisabled("%s%d pictures of %d x %d kept (%.0f MB of GPU memory)", writing ? "writing; " : "", pictures,
                                width, height, megabytes);
    }
#endif
    ImGui::TextWrapped("Folder: %s", port_debug_folder());
    ImGui::TextWrapped("%s", port_debug_message());
}

// Teleport (port/include/port/teleport.h): any map, at a place read from its file, keeping the progress
void teleport_section() {
    static int map_index = -1;
    static int place = 0;
    static int places_for = -1; // the map `places` is of
    static int place_count = 0;
    static PortTeleportPlace places[PORT_TELEPORT_PLACES];
    int count = port_teleport_map_count();

    if (!ImGui::CollapsingHeader("Teleport"))
        return;
    if (count == 0) {
        ImGui::TextDisabled("No maps found on the disc.");
        return;
    }
    if (map_index < 0 || map_index >= count) {
        // the map the player is in, to start with
        PortGameReadout game;
        kf4_game_readout(&game);
        map_index = 0;
        for (int i = 0; i < count; i++)
            if (port_teleport_map(i) == (game.map_tag & 0xFF))
                map_index = i;
    }
    int map = port_teleport_map(map_index);
    char label[96];
    std::snprintf(label, sizeof(label), "%03d %s", map, port_teleport_map_name(map));
    if (ImGui::BeginCombo("Map", label, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < count; i++) {
            int m = port_teleport_map(i);
            std::snprintf(label, sizeof(label), "%03d %s", m, port_teleport_map_name(m));
            if (ImGui::Selectable(label, i == map_index)) {
                map_index = i;
                place = 0;
            }
            if (i == map_index)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    map = port_teleport_map(map_index);
    if (places_for != map) {
        place_count = port_teleport_places(map, places);
        places_for = map;
    }
    if (place >= place_count)
        place = 0;
    if (place_count > 0) {
        std::snprintf(label, sizeof(label), "%d: %s", place, places[place].label);
        if (ImGui::BeginCombo("Place", label)) {
            for (int i = 0; i < place_count; i++) {
                std::snprintf(label, sizeof(label), "%d: %s (%.0f, %.0f, %.0f)", i, places[i].label,
                              places[i].position[0], places[i].position[1], places[i].position[2]);
                if (ImGui::Selectable(label, i == place))
                    place = i;
            }
            ImGui::EndCombo();
        }
        tooltip("start: where the map puts you when you arrive without a door or stairs.\n"
                "item N: where one of the map's own items lies.\n"
                "floor N: the middle of a large floor (N is the map record it belongs to).\n"
                "You are put on the floor under the place.");
    }
    ImGui::BeginDisabled(place_count == 0);
    if (ImGui::Button("Teleport##go"))
        port_teleport_request(map, place);
    ImGui::EndDisabled();
    tooltip("Restarts the game on that map, as loading a save does, with your items, stats and the\n"
            "world's flags as they are now. Taken in play, like a quick load. A map you would not\n"
            "reach yet may not work as it does in a real playthrough.");
    ImGui::SameLine();
    ImGui::TextDisabled("--teleport %d:%d", map, place);
    if (port_overlay_paused())
        ImGui::TextColored(colour_note, "The game is paused: the teleport is taken when the overlay closes.");
    ImGui::TextDisabled("%s", port_quick_message());
}

// ---- Gameplay ------------------------------------------------------------------------------------
// A mod's checkbox, kept in settings.json and port_options, with the command line's note
void mod_checkbox(const char *label, int *setting, int *option, const char *tip) {
    bool on = *option != 0;
    if (ImGui::Checkbox(label, &on)) {
        *setting = *option = on;
        changed();
    }
    tooltip(tip);
    if (*option != *setting) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(on for this run: command line)");
    }
}

void gameplay_page() {
    PortSettings &s = port_settings;

    ImGui::TextWrapped("Optional changes to how the game plays. All are off by default; with them off, the game plays as it does on the PS2.");
    ImGui::SeparatorText("Tweaks");
    mod_checkbox("Myu's Necklace effect without wearing it", &s.myus_necklace, &port_options.myus_necklace,
                 "Objects that glow while Myu's Necklace is worn always glow, so the amulet slot\nis free for another "
                 "amulet. Applies at once. (--myus-necklace)");
    mod_checkbox("No camera jolt when enemies hit you", &s.no_hit_jolt, &port_options.no_hit_jolt,
                 "A hard hit no longer throws the view down: the camera stays where you aimed it. The hit\n"
                 "still flashes red, rumbles and knocks you back. Applies at once. (--no-hit-jolt)");
    port_stats_overlay_mods(); // the extended stats mod (port/overlay/stats_panel.cpp)
    automap_view_mods_section(); // the automap mod (port/overlay/automap_view.cpp)
    hands_view_mods_section(); // the hands mod (port/overlay/hands_view.cpp)
}

// ---- Developer -----------------------------------------------------------------------------------
void speed_section() {
    if (!ImGui::CollapsingHeader("Speed", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::Text("Pictures shown: %.1f a second", port_video_pictures_per_second());
    tooltip("The new pictures the window presented, whichever thread showed them (the game's, or the\n"
            "pictures between's own at --frame-rate above 60).");
    ImGui::Text("Vblanks: %.2f a second (%.0f%% of the PS2's 59.94)", vblanks_per_second,
                vblanks_per_second * 100.0 / 59.94);
    ImGui::Text("Game frames: %.1f a second", frames_per_second);
    bool stats = port_options.stats != 0;
    if (ImGui::Checkbox("Count VU1's, the GIF's and the scanout's time (--stats)", &stats))
        port_options.stats = stats;
    tooltip("Counts the time spent running VU1, handing packets to the GS and scanning out.\nIt costs a little "
            "time itself, so it is off unless asked for.");
    if (port_options.stats) {
        ImGui::Text("VU1: %.1f ms a second, %.0f runs, %.2f M instructions", vu1_ms, vu1_runs, vu1_insns / 1e6);
        ImGui::Text("GIF hand-off: %.1f ms a second    Scanout: %.1f ms a second", gif_ms, vsync_ms);
        ImGui::TextDisabled("VU1 cycles %llu, PATH1 hash %016llx", (unsigned long long)port_stats.vu1_cycles,
                            (unsigned long long)port_stats.path1_hash);
    }
}

void game_section() {
    PortGameReadout game;
    kf4_game_readout(&game);

    if (!ImGui::CollapsingHeader("Game", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::Text("Map: %03d %s (tag %02X, slot %d)%s", game.map_tag & 0xFF, port_teleport_map_name(game.map_tag & 0xFF),
                game.map_tag & 0xFF, game.map_slot, game.map_loaded ? "" : " (not loaded)");
    ImGui::Text("Position: %.2f, %.2f, %.2f   heading %.3f", game.x, game.y, game.z, game.heading);
    ImGui::Text("HP %d / %d    MP %d / %d    Level %d    Gold %d", game.hp, game.hp_max, game.mp, game.mp_max,
                game.level, game.gold);
    ImGui::TextDisabled("Frame %u   vblank %llu   status %08X   menu %d", game.frame,
                        (unsigned long long)port_vblank_count(), game.status, game.menu);
    ImGui::Spacing();
    if (ImGui::Button("Copy for a bug report"))
        ImGui::SetClipboardText(bug_report().c_str());
    tooltip("Copies the numbers above, the map and position, and this run's options.");
}

void cheats_section() {
    if (!ImGui::CollapsingHeader("Cheats"))
        return;
    bool invincible = port_options.invincible != 0;
    if (ImGui::Checkbox("Invincible", &invincible))
        port_options.invincible = invincible;
    tooltip("You lose no HP: hits still land, flash and knock you back, but take nothing, and falls,\n"
            "deadly ground and scripted deaths do not kill. Poison and other ailments still show.\n"
            "For testing areas a new character could not survive. Not saved: off at the next start\n"
            "(--invincible turns it on from the start).");
    if (ImGui::Button("Learn all spells"))
        port_cheat_request(PORT_CHEAT_SPELLS);
    tooltip("Every spell of the six schools, at the level beside. They show in Equipment > Magic and\n"
            "the Item menu's magic once a menu is opened again.");
    ImGui::SameLine();
    {
        int level = port_cheat_spell_level();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::SliderInt("level", &level, 1, 3))
            port_cheat_set_spell_level(level);
        tooltip("Each level is its own spell, with its own strength, effect and length (Earth Heal 1 heals\n"
                "for under 3 seconds; Earth Heal 3 also gives a 30 second boost, and its glow lasts as\n"
                "long). Learning again sets every spell to this level.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Add all equipment"))
        port_cheat_request(PORT_CHEAT_EQUIPMENT);
    tooltip("One of every weapon, shield, helm, armour, gauntlet, boot, ring, bracelet and amulet.");
    ImGui::SameLine();
    if (ImGui::Button("Add all items"))
        port_cheat_request(PORT_CHEAT_ITEMS);
    tooltip("99 of every carried item (one of a one-of-a-kind item): potions, herbs, crystals, wands,\n"
            "arrows, maps. Not the keys, which belong to the story.");
    if (ImGui::Button("Max MP and Magic Power"))
        port_cheat_request(PORT_CHEAT_MAGIC);
    tooltip("MP and Magic Power at 999, the game's own cap, and MP full. Magic Power is what makes\n"
            "the magic gauge fill quickly.");
    ImGui::SameLine();
    if (ImGui::Button("All of these"))
        port_cheat_request(PORT_CHEAT_SPELLS | PORT_CHEAT_EQUIPMENT | PORT_CHEAT_ITEMS | PORT_CHEAT_MAGIC);
    tooltip("They change your game as progress does: a save made afterwards keeps them.");
    if (port_cheat_message()[0])
        ImGui::TextDisabled("%s", port_cheat_message());
}

void developer_page() {
    ImGui::TextWrapped("For testing and bug reports.");
    speed_section();
    game_section();
    cheats_section();
    teleport_section();
    save_states_section();
    renderer_section();
    debug_tools();
}

// ---- General -------------------------------------------------------------------------------------
void audio_section() {
    PortSettings &s = port_settings;

    ImGui::SeparatorText("Audio");
    if (ImGui::SliderInt("Volume", &s.volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        port_settings_apply_audio();
        changed();
    }
    bool mute = s.mute != 0;
    if (ImGui::Checkbox("Mute", &mute)) {
        s.mute = mute;
        port_settings_apply_audio();
        changed();
    }
    if (port_options.no_audio)
        ImGui::TextColored(colour_note, "No sound device this run (--no-audio).");
}

void overlay_section() {
    PortSettings &s = port_settings;

    ImGui::SeparatorText("Overlay");
    bool pause = s.pause_when_open != 0;
    if (ImGui::Checkbox("Pause the game while the overlay is open", &pause)) {
        s.pause_when_open = pause;
        changed();
    }
    tooltip("Off: the game runs on behind the overlay.");
    bool fps = port_options.show_fps != 0;
    char fps_key[64];
    port_controls_binding_label(port_controls_binding(ACTION_SHOW_FPS, 0), fps_key,
                                sizeof(fps_key));
    if (ImGui::Checkbox("Frame-rate counter", &fps)) {
        s.show_fps = port_options.show_fps = fps;
        changed();
    }
    tooltip("The pictures the window shows a second, in the picture's top right corner, with the\n"
            "overlay closed too; below it the game's own frames a second when they differ. Its key\n"
            "shows and hides it in play. (--show-fps)");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", fps_key);
    static float scale = -1.0f;
    if (!ImGui::IsAnyItemActive() || scale < 0.0f)
        scale = s.ui_scale;
    ImGui::SliderFloat("Overlay size", &scale, 0.75f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        s.ui_scale = std::round(scale * 20.0f) / 20.0f;
        changed();
    }
    tooltip("Applies when you let go of the slider.");
}

void general_page() {
    audio_section();
    overlay_section();
    ImGui::SeparatorText("Settings file");
    ImGui::TextWrapped("%s", port_settings_path());
    ImGui::TextDisabled("Options on the command line override it for one run.");
}

// ---- Windows -------------------------------------------------------------------------------------
// The Settings window's pages, in the sidebar's order; the id names a page for KF4_OVERLAY_SHOT
struct PageEntry {
    const char *name;
    const char *id;
    void (*draw)();
};
const PageEntry pages[] = {
    {"Display", "display", display_page},    {"Graphics", "graphics", graphics_page},
    {"Controls", "controls", controls_page}, {"Gameplay", "gameplay", gameplay_page},
    {"Saves", "saves", saves_page},          {"General", "general", general_page},
    {"Developer", "developer", developer_page},
};
const int PAGE_COUNT = int(sizeof(pages) / sizeof(pages[0]));

void settings_window() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            shot_mode ? ImGuiCond_Always : ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    // as large as the scale asks, within the window
    ImVec2 size(700 * applied_scale, 560 * applied_scale);
    size.x = size.x < viewport->WorkSize.x - 16 ? size.x : viewport->WorkSize.x - 16;
    size.y = size.y < viewport->WorkSize.y - 16 ? size.y : viewport->WorkSize.y - 16;
    ImGui::SetNextWindowSize(size, shot_mode ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &show_settings)) {
        ImGui::End();
        return;
    }
    ImGui::BeginChild("pages", ImVec2(130 * applied_scale, 0), ImGuiChildFlags_Borders);
    for (int i = 0; i < PAGE_COUNT; i++)
        if (ImGui::Selectable(pages[i].name, page == i))
            page = i;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page");
    pages[page].draw();
    ImGui::EndChild();
    ImGui::End();
}

void about_window() {
    if (!ImGui::Begin("About", &show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("Holyland: King's Field IV on the PC, built from the kfiv-decomp decompilation");
    ImGui::TextDisabled("%s", update_badge_version_line());
    ImGui::Separator();
    ImGui::BulletText("The game's code: kfiv-decomp (Voicedrew11 and contributors)");
    ImGui::BulletText("The GS: paraLLEl-GS (Arntzen Software, LGPL-3.0+)");
    ImGui::BulletText("The SPU2: ico-audio, from ico-pc (MIT)");
    ImGui::BulletText("The Verdite layout: Verdite2 (Voicedrew11, MIT)");
    ImGui::BulletText("Dear ImGui (MIT), cJSON (MIT), SDL3 (zlib)");
    ImGui::BulletText("The menu: RmlUi (MIT), FreeType (FreeType Licence)");
    ImGui::BulletText("Its fonts: Cinzel (Natanael Gama) and Spectral (Production Type), SIL OFL 1.1");
    ImGui::End();
}

void menu_bar() {
    if (!ImGui::BeginMainMenuBar())
        return;
    ImGui::MenuItem("Settings", nullptr, &show_settings);
    ImGui::MenuItem("About", nullptr, &show_about);
    update_badge_menu_item();
    char status[96];
    std::snprintf(status, sizeof(status), "%s to close%s", port_overlay_key(),
                  port_overlay_paused() ? "   (game paused)" : "");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(status).x - 16);
    ImGui::TextDisabled("%s", status);
    ImGui::EndMainMenuBar();
}

// The last save state request's message, a while after its key was pressed
void draw_toast() {
    Uint32 now = (Uint32)SDL_GetTicks();
    if (toast_until == 0 || (int32_t)(toast_until - now) <= 0)
        return;
    const char *text = toast_text();
    float alpha = (int32_t)(toast_until - now) < 400 ? (float)(int32_t)(toast_until - now) / 400.0f : 1.0f;
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImFont *font = ImGui::GetFont();
    float size = viewport->Size.y / 30.0f > 18.0f ? viewport->Size.y / 30.0f : 18.0f;
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    ImVec2 at((viewport->Size.x - extent.x) * 0.5f, viewport->Size.y * 0.2f);
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(at.x - size * 0.6f, at.y - size * 0.3f),
                        ImVec2(at.x + extent.x + size * 0.6f, at.y + extent.y + size * 0.3f),
                        IM_COL32(0, 0, 0, (int)(150 * alpha)), size * 0.3f);
    draw->AddText(font, size, at, IM_COL32(255, 235, 170, (int)(255 * alpha)), text);
}

// "Preparing shaders": the renderer's pipelines made before play (port_gs_shaders_building), the game waiting
bool building_shown() {
    int done, total;
    return port_gs_shaders_building(&done, &total) != 0 && total > 0;
}

void draw_building() {
    int done, total;
    if (!port_gs_shaders_building(&done, &total) || total <= 0)
        return;
    char text[64];
    std::snprintf(text, sizeof(text), "Preparing shaders... %d%%", done * 100 / total);
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImFont *font = ImGui::GetFont();
    float size = viewport->Size.y / 30.0f > 18.0f ? viewport->Size.y / 30.0f : 18.0f;
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    ImVec2 at((viewport->Size.x - extent.x) * 0.5f, (viewport->Size.y - extent.y) * 0.5f);
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(at.x - size * 0.5f, at.y - size * 0.25f),
                        ImVec2(at.x + extent.x + size * 0.5f, at.y + extent.y + size * 0.25f), IM_COL32(0, 0, 0, 150),
                        size * 0.3f);
    draw->AddText(font, size, at, IM_COL32(255, 235, 170, 255), text);
}

// ---- KF4_OVERLAY_SHOT ----------------------------------------------------------------------------
// The pages' ids before November 2026's reorganization, so older scripts still find a page
const struct {
    const char *old_id;
    const char *id;
} page_aliases[] = {{"audio", "general"}, {"interface", "general"}, {"savestates", "saves"},
                    {"mods", "gameplay"}, {"debug", "developer"}};

int page_index(const char *id) {
    for (int i = 0; i < PAGE_COUNT; i++)
        if (std::strcmp(pages[i].id, id) == 0)
            return i;
    return -1;
}

void parse_shots(const char *text) {
    std::string spec = text;
    size_t at = 0;

    while (at < spec.size()) {
        size_t end = spec.find(',', at);
        std::string item = spec.substr(at, end == std::string::npos ? std::string::npos : end - at);
        size_t sign = item.find('@');
        at = end == std::string::npos ? spec.size() : end + 1;
        if (sign == std::string::npos)
            continue;
        std::string name = item.substr(0, sign);
        Shot shot = {-1, std::strtoull(item.c_str() + sign + 1, nullptr, 0), ""};
        if (name.rfind("ui-", 0) == 0) { // the player menu on a tab, the overlay closed
            shot.page = PAGE_COUNT;
            shot.ui_tab = name.substr(3);
            shots.push_back(shot);
            continue;
        }
        for (int i = 0; i < PAGE_COUNT; i++)
            if (name == pages[i].id)
                shot.page = i;
        for (const auto &alias : page_aliases)
            if (name == alias.old_id)
                shot.page = page_index(alias.id);
        if (name == "closed")
            shot.page = PAGE_COUNT;
        if (name == "map") // the automap mod's full map, the overlay closed
            shot.page = PAGE_COUNT + 1;
        if (name == "mini") // the automap mod's minimap: the overlay closed, one picture a vblank
            shot.page = PAGE_COUNT + 2;
        if (name == "menu") // the game's menu with the extended stats mod's lines: the overlay closed, one a vblank
            shot.page = PAGE_COUNT + 3;
        if (name == "profiler") // the profiler's panel (Shift+P), the overlay closed: one a vblank
            shot.page = PAGE_COUNT + 4;
        if (shot.page < 0) {
            std::fprintf(stderr, "[overlay] KF4_OVERLAY_SHOT: no page called %s (display, graphics, controls, "
                                 "gameplay, saves, general, developer, closed, map, mini, menu, profiler, ui-<tab>)\n",
                         name.c_str());
            continue;
        }
        shots.push_back(shot);
    }
    shot_mode = !shots.empty();
}

// Opens the page at its vblank; the picture is taken 4 vblanks on, once the windows have their sizes
const uint64_t SHOT_SETTLE = 4;

void shot_open() {
    if (shot_next >= shots.size())
        return;
    const Shot &shot = shots[shot_next];
    if (port_vblank_count() < shot.vblank)
        return;
    // the player menu opened (and its keys pressed) once a picture, not each vblank until it is taken
    static size_t ui_shown_for = SIZE_MAX;
    if (ui_shown_for != shot_next) {
        ui_shown_for = shot_next;
        if (!shot.ui_tab.empty() ? !port_player_ui_show(shot.ui_tab.c_str())
                                 : port_player_ui_open() && !port_player_ui_show(nullptr))
            std::fprintf(stderr, "[overlay] KF4_OVERLAY_SHOT: no player menu tab %s\n", shot.ui_tab.c_str());
    }
    automap_view_open_for_shot(shot.page == PAGE_COUNT + 1);
    profiler_show_for_shot(shot.page == PAGE_COUNT + 4);
    if (shot.page >= PAGE_COUNT) {
        overlay_open = false;
    } else {
        overlay_open = true;
        show_settings = true;
        page = shot.page;
    }
}

bool shot_due() {
    return ready && shot_next < shots.size() && port_vblank_count() >= shots[shot_next].vblank + SHOT_SETTLE;
}

// The window's picture (NULL: it could not be read back) as the next test picture
void shot_write(const uint32_t *pixels, int width, int height) {
    const Shot &shot = shots[shot_next];
    if (pixels != nullptr) {
        char path[4096];
        if (shot.page > PAGE_COUNT) // the automap's, the menu's and the profiler's: one a vblank
            std::snprintf(path, sizeof(path), "%s/overlay_%s_%llu.png", port_options.dump_dir ? port_options.dump_dir : ".",
                          shot.page == PAGE_COUNT + 1   ? "map"
                          : shot.page == PAGE_COUNT + 2 ? "mini"
                          : shot.page == PAGE_COUNT + 3 ? "menu"
                                                        : "profiler",
                          (unsigned long long)shot.vblank);
        else // (pages has the settings' pages only)
            std::snprintf(path, sizeof(path), "%s/overlay_%s%s.png", port_options.dump_dir ? port_options.dump_dir : ".",
                          shot.ui_tab.empty() ? "" : "ui-",
                          !shot.ui_tab.empty()      ? shot.ui_tab.c_str()
                          : shot.page == PAGE_COUNT ? "closed"
                                                    : pages[shot.page].id);
        if (port_png_write(path, pixels, (uint32_t)width, (uint32_t)height))
            std::fprintf(stderr, "[overlay] vblank %llu: %dx%d, %s\n", (unsigned long long)port_vblank_count(), width,
                         height, path);
    }
    shot_next++;
    // the last picture taken, settings.json is written back at exit, so a test sees it round-trip
    if (shot_next == shots.size())
        port_settings_changed();
}

void shot_take() {
    if (!shot_due())
        return;
    int width = 0, height = 0;
    SDL_Surface *read = SDL_RenderReadPixels(renderer, nullptr);
    SDL_Surface *rgba = read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_ABGR8888) : nullptr;
    if (rgba == nullptr) {
        std::fprintf(stderr, "[overlay] cannot read the window back: %s\n", SDL_GetError());
        shot_write(nullptr, 0, 0);
    } else {
        width = rgba->w;
        height = rgba->h;
        std::vector<uint32_t> pixels((size_t)width * height);
        for (int y = 0; y < height; y++)
            std::memcpy(&pixels[(size_t)y * width], static_cast<const uint8_t *>(rgba->pixels) + (size_t)y * rgba->pitch,
                        (size_t)width * 4);
        shot_write(pixels.data(), width, height);
    }
    SDL_DestroySurface(rgba);
    SDL_DestroySurface(read);
}

bool shot_pending() {
    return shot_next < shots.size() && port_vblank_count() >= shots[shot_next].vblank;
}

bool is_mouse_event(Uint32 type) {
    return type == SDL_EVENT_MOUSE_MOTION || type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP ||
           type == SDL_EVENT_MOUSE_WHEEL;
}

bool is_input_event(Uint32 type) {
    switch (type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return true;
    default:
        return false;
    }
}

} // namespace

// ---- The port's side -----------------------------------------------------------------------------
extern "C" void port_overlay_init(void *sdl_window, void *sdl_renderer) {
    if (!port_settings_active())
        return;
    window = static_cast<SDL_Window *>(sdl_window);
    renderer = static_cast<SDL_Renderer *>(sdl_renderer);
#ifdef KF4_HAVE_PLAYER_UI
    if (renderer == nullptr)
        port_player_ui_init(window); // the player menu: the Vulkan presenter draws it
#endif
    if (const char *spec = std::getenv("KF4_OVERLAY_SHOT"))
        parse_shots(spec);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    // window places are kept beside settings.json; not for test pictures, which start the same each time
    ini_path = std::string(port_data_dir()) + "/overlay.ini";
    io.IniFilename = shot_mode ? nullptr : ini_path.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    load_font();
    port_stats_overlay_fonts(); // the extended stats mod's font for its lines in the menus
    apply_style();
    bool started;
    if (renderer != nullptr) {
        started = ImGui_ImplSDL3_InitForSDLRenderer(window, renderer) && ImGui_ImplSDLRenderer3_Init(renderer);
    } else {
#ifdef KF4_HAVE_VULKAN_PRESENTER
        started = ImGui_ImplSDL3_InitForVulkan(window) && port_vk_imgui_init();
#else
        started = false;
#endif
    }
    if (!started) {
        std::fprintf(stderr, "[overlay] Dear ImGui did not start: no overlay\n");
        return;
    }
    ready = true;
    std::fprintf(stderr, "[overlay] %s opens the developers' overlay\n", port_overlay_key());
}

extern "C" int port_overlay_event(const void *sdl_event) {
    const SDL_Event *event = static_cast<const SDL_Event *>(sdl_event);

    // the player menu first (F1 and its keys), unless this overlay is open (Shift+F1, or F1 without the menu)
    if (!overlay_open && port_player_ui_event(event))
        return 1;
    if (!ready)
        return 0;
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        SDL_Scancode key = event->key.scancode;
        if (key == SDL_SCANCODE_F1 || (overlay_open && key == SDL_SCANCODE_ESCAPE && !ImGui::GetIO().WantTextInput)) {
            set_open(!overlay_open);
            return 1;
        }
        if (key == SDL_SCANCODE_F11) {
            port_settings.fullscreen = !port_settings.fullscreen;
            port_video_apply_settings();
            changed();
            return 1;
        }
        if (!ImGui::GetIO().WantTextInput && profiler_key(*event))
            return 1;
    }
    if (event->type == SDL_EVENT_KEY_UP &&
        (event->key.scancode == SDL_SCANCODE_F1 || event->key.scancode == SDL_SCANCODE_F11))
        return 1;
    // the automap mod's full map: its key closes it, and it has the mouse while it is open
    if (!overlay_open && automap_view_event(event))
        return 1;
    if (!overlay_open && port_automap_full_open()) {
        ImGui_ImplSDL3_ProcessEvent(event);
        return is_input_event(event->type);
    }
    // the profiler, the overlay closed: the mouse is its, and the keys while one of its text fields is typed in
    if (!overlay_open && profiler_visible()) {
        if (is_mouse_event(event->type)) {
            ImGui_ImplSDL3_ProcessEvent(event);
            return 1;
        }
        if (is_input_event(event->type) && imgui_typing.load()) {
            ImGui_ImplSDL3_ProcessEvent(event);
            return 1;
        }
        if (event->type == SDL_EVENT_KEY_UP)
            ImGui_ImplSDL3_ProcessEvent(event); // (a key let go after typing stopped is let go in ImGui too)
        return 0;
    }
    // closed, it takes nothing more (and ImGui's queue is only emptied by a frame drawn)
    if (!overlay_open)
        return 0;
    ImGui_ImplSDL3_ProcessEvent(event);
    return is_input_event(event->type);
}

extern "C" int port_overlay_visible(void) {
    return port_player_ui_visible() || (ready && (overlay_open || shot_pending() || port_stats_overlay_wants() ||
                     (toast_until != 0 && (int32_t)(toast_until - (Uint32)SDL_GetTicks()) > 0) || building_shown() ||
                     automap_view_visible() || profiler_visible()));
}

extern "C" void *port_overlay_draw_data(void) {
    return drawn ? ImGui::GetDrawData() : nullptr;
}

extern "C" int port_overlay_shot_due(void) {
    return shot_due();
}

extern "C" void port_overlay_shot(const unsigned int *rgba, int width, int height) {
    if (shot_due())
        shot_write(rgba, width, height);
}

extern "C" int port_overlay_open(void) {
    return (ready && overlay_open) || port_player_ui_open();
}

extern "C" int port_overlay_holds_mouse(void) {
    return ready && !overlay_open && profiler_visible();
}

extern "C" const char *port_overlay_key(void) {
    return port_player_ui_ready() ? "Shift+F1" : "F1";
}

extern "C" int port_overlay_paused(void) {
    bool open = overlay_open || port_player_ui_open();
    return ready && !shot_mode && ((open && port_settings.pause_when_open) || port_automap_holds_game());
}

extern "C" void port_overlay_notify_state(void) {
    toast_text = port_state_message;
    toast_until = (Uint32)SDL_GetTicks() + 2500;
}

extern "C" void port_overlay_notify_quick(void) {
    toast_text = port_quick_message;
    toast_until = (Uint32)SDL_GetTicks() + 3000;
}

extern "C" void port_overlay_notify_debug(void) {
    toast_text = port_debug_message;
    toast_until = (Uint32)SDL_GetTicks() + 4000;
}

extern "C" void port_overlay_vblank(void) {
    if (!ready)
        return;
    port_stats_overlay_update(); // the extended stats mod (port/overlay/stats_panel.cpp)
    profiler_step();             // Shift+P (port/overlay/profiler.cpp)
    // typing in the profiler's text field (the overlay and the player menu closed): the game gets no keys meanwhile
    static bool typing;
    bool now_typing = port_overlay_holds_mouse() && !port_player_ui_open() && imgui_typing.load();
    if (now_typing != typing) {
        typing = now_typing;
        if (!overlay_open && !port_player_ui_open()) // (those block the game themselves, and let go when closed)
            port_controls_block(typing);
    }
}

extern "C" void port_overlay_draw(int x, int y, int width, int height) {
    drawn = false;
    if (!ready)
        return;
    sample_rates();
    port_settings_poll();
    // a new release the launcher found: a note when nothing else is showing
    static bool update_note_pending;
    if (update_badge_poll((Uint32)SDL_GetTicks()))
        update_note_pending = true;
    if (update_note_pending && !building_shown()) {
        update_note_pending = false;
        toast_text = update_badge_note;
        toast_until = (Uint32)SDL_GetTicks() + 6000;
    }
    shot_open();
    if (!port_overlay_visible())
        return;
    if (port_settings.ui_scale != applied_ui_scale)
        apply_style();

    // the controller does not steer the overlay while a slot waits for its button, nor until that
    // button is let go (else the press that binds it would also press the slot again)
    static bool pad_quiet;
    bool listening = port_controls_listening(nullptr, nullptr) != 0;
    ImGuiIO &io = ImGui::GetIO();
    if (listening)
        pad_quiet = true;
    if (pad_quiet)
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    if (renderer != nullptr) {
        ImGui_ImplSDLRenderer3_NewFrame();
    } else {
#ifdef KF4_HAVE_VULKAN_PRESENTER
        ImGui_ImplVulkan_NewFrame();
#endif
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (pad_quiet && !listening) {
        bool held = false;
        for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; key++)
            held |= ImGui::IsKeyDown((ImGuiKey)key);
        pad_quiet = held;
    }
    automap_view_draw(x, y, width, height); // the automap mod, under the overlay's windows
    draw_toast();
    draw_building();
    port_stats_overlay_draw(overlay_open, x, y, width, height); // the extended stats mod (port/overlay/stats_panel.cpp)
    profiler_draw();                                            // Shift+P (port/overlay/profiler.cpp)
    if (overlay_open) {
        menu_bar();
        if (show_settings)
            settings_window();
        if (show_about)
            about_window();
        update_badge_window();
    }
    (void)x, (void)y, (void)width, (void)height;
    imgui_typing.store(ImGui::GetIO().WantTextInput);
    ImGui::Render();
    if (renderer == nullptr) {
        drawn = true; // the presenter records it, and takes the test picture
        return;
    }
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    shot_take();
}
