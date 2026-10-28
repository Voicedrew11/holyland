// The settings overlay (port/include/port/overlay.h): F1 opens a menu bar and the Settings window,
// drawn with Dear ImGui (third_party/imgui) over the game's picture, on the window's SDL_Renderer or by
// the Vulkan presenter (port/platform/present_vk.cpp, --presenter vulkan) with ImGui's Vulkan backend. The
// pages are those of KF1 Enhanced and KF3-Enhanced: Display, Audio, Controls, Save states, Debug and
// Interface. Every way the PC differs from the PS2 is a switch that starts as the port has it.
//
// While it is open the keyboard, the mouse and the controller go to it, not to the game; the game
// runs on unless "Pause the game while the overlay is open" is on (port/platform/kernel.c holds it).
// Settings are kept in settings.json (port/overlay/settings.c). Everything here runs on the game's
// thread, at the vblank (port_pad_poll's events, port_video_present's drawing).
//
// KF4_OVERLAY_SHOT=page@vblank,... opens the overlay on that page at that vblank, even with a pad
// script, and writes the window's whole picture a few vblanks later as overlay_<page>.png in
// --dump-dir: pictures of each page, for checking it draws, without anyone at the screen.
#include <SDL.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#ifdef KF4_HAVE_VULKAN_PRESENTER
#include "backends/imgui_impl_vulkan.h"
#endif
#include "imgui.h"

#include "automap_view.h" // the automap mod
#include "profiler.h"     // Shift+P

extern "C" {
#include "port/audio.h"
#include "port/automap.h"
#include "port/weapon_sets.h"
#include "port/cheats.h"
#include "port/controls.h"
#include "port/debug.h"
#include "port/overlay.h"
#include "port/platform.h"
#include "port/present_vk.h"
#include "port/host.h"
#include "port/quicksave.h"
#include "port/render.h"
#include "port/savestate.h"
#include "port/settings.h"
#include "port/stats.h"
#include "port/teleport.h"
}

namespace {

enum Page { PAGE_DISPLAY, PAGE_AUDIO, PAGE_CONTROLS, PAGE_SAVE_STATES, PAGE_MODS, PAGE_DEBUG, PAGE_INTERFACE, PAGE_COUNT };
const char *const page_names[PAGE_COUNT] = {"Display", "Audio", "Controls", "Save states", "Mods", "Debug", "Interface"};
const char *const page_ids[PAGE_COUNT] = {"display", "audio", "controls", "savestates", "mods", "debug", "interface"};

const ImVec4 colour_note(0.95f, 0.8f, 0.45f, 1.0f);
const ImVec4 colour_good(0.55f, 0.85f, 0.55f, 1.0f);

SDL_Window *window;
SDL_Renderer *renderer; // NULL on the Vulkan presenter
bool ready;
bool drawn; // the Vulkan presenter: this vblank's draw lists are ImGui's draw data
bool overlay_open;
bool show_settings = true;
bool show_about;
int page;
float applied_scale = 1.0f; // the layout's scale: the slider's value, taken when it is let go, times the screen's
float applied_ui_scale = 1.0f; // the slider's value in applied_scale
std::string ini_path;
Uint32 toast_until;
const char *(*toast_text)(void) = port_state_message; // the message the toast shows

// the Debug page's rates, sampled once a second
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
    float dpi = 0.0f;
    if (window != NULL && SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(window), NULL, &dpi, NULL) == 0 && dpi > 96.0f)
        return dpi / 96.0f;
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
    if (!overlay_open) {
        port_controls_listen_cancel();
        port_settings_save();
    }
}

void changed() {
    port_settings_changed();
}

// ---- Display -------------------------------------------------------------------------------------
void display_page() {
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

    SDL_GetDisplayBounds(window != NULL ? SDL_GetWindowDisplayIndex(window) : 0, &screen);
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
            "own resolution (1920 x 1080, 2560 x 1440, ...) and fits the picture to it.\n"
            "The game itself draws 640 x 448, as on the PS2, and is scaled up to it.");
    bool fullscreen = s.fullscreen != 0;
    if (ImGui::Checkbox("Full screen (borderless)", &fullscreen)) {
        s.fullscreen = fullscreen;
        port_video_apply_settings();
        changed();
    }
    tooltip("Also F11.");
    const char *const aspects[] = {port_options.widescreen ? "16:9, as on a widescreen TV" : "4:3, as on a TV (PS2)",
                                   "Stretch to the window"};
    if (ImGui::Combo("Picture shape", &s.aspect, aspects, 2))
        changed();
    tooltip("4:3 keeps the picture's shape with black bars at the sides or top.\nStretch fills the window "
            "and distorts the picture.\nWith widescreen on, the shape kept is 16:9.");
    bool widescreen = port_options.widescreen != 0;
    if (ImGui::Checkbox("Widescreen (16:9)", &widescreen)) {
        s.widescreen = port_options.widescreen = widescreen;
        port_video_apply_settings();
        changed();
    }
    tooltip("Off: the PS2's 4:3 picture.\nOn: the game's view is widened to 16:9 (the same height, more at the "
            "sides), and the\nmenus, the message boxes and the HUD keep their 4:3 shape. Applies at once. "
            "(--widescreen)");
    if (port_options.widescreen != s.widescreen) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(on for this run: command line)");
    }
    ImGui::BeginDisabled(!widescreen);
    static const char *const huds[] = {"In the corner of the 16:9 picture", "Centred, where it is at 4:3"};
    int hud = port_options.widescreen_hud_centred != 0;
    if (ImGui::Combo("HUD", &hud, huds, 2)) {
        s.widescreen_hud_centred = port_options.widescreen_hud_centred = hud;
        changed();
    }
    ImGui::EndDisabled();
    tooltip("Where the widescreen picture puts the HUD (the gauges of the top left).");
    {
        static const int rates[] = {30, 60, 120, 144, 165, 240, PORT_FRAME_RATE_MONITOR};
        static const char *const names[] = {"30 (the PS2's)", "60", "120", "144", "165", "240", "The screen's refresh rate"};
        char other[48];
        int at = -1, i;

        for (i = 0; i < 7; i++) {
            at = rates[i] == port_options.frame_rate ? i : at;
        }
        if (port_options.frame_rate == PORT_FRAME_RATE_MONITOR) {
            snprintf(other, sizeof(other), "The screen's refresh rate (%d)", port_frame_rate_hz());
        } else {
            snprintf(other, sizeof(other), "%d", port_options.frame_rate);
        }
        if (ImGui::BeginCombo("Pictures a second in play", other)) {
            for (i = 0; i < 7; i++) {
                if (ImGui::Selectable(names[i], i == at)) {
                    s.frame_rate = port_options.frame_rate = rates[i];
                    changed();
                }
            }
            ImGui::EndCombo();
        }
    }
    tooltip("30: the PS2's 30 pictures a second in play.\nMore: pictures between each two of the game's, "
            "the world seen from the camera on its way\nbetween them, models, the HUD's gauges, fades and "
            "scrolling textures where they are at that\ntime, so turning and walking look smooth. The game "
            "still runs at 30: what it does is the\nsame, and the picture is no later. What changes by "
            "frames (the orb's flames, flipbook\neffects) still changes 30 times a second. Above 60 the "
            "window shows each picture at its\ntime; with VSync, choose the screen's rate. The menus are "
            "as they were. Applies at once.\n(--frame-rate 30|60|120|144|240|monitor)");
    if (port_options.frame_rate != s.frame_rate) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(for this run: command line)");
    }
    static const char *const distances[PORT_DRAW_DISTANCE_COUNT] = {"1x (PS2)", "1.5x", "2x", "4x", "Max (no distance fog)"};
    int distance = port_options.draw_distance;
    if (ImGui::Combo("Draw distance", &distance, distances, PORT_DRAW_DISTANCE_COUNT)) {
        s.draw_distance = port_options.draw_distance = distance;
        changed();
    }
    tooltip("1x: the PS2's fog.\nFurther: the fog starts and ends that many times as far away, so more of "
            "the world\nshows through it (the far plane moves out with it). The game already draws\nwhat "
            "is behind the fog; it only hides it. Max removes the distance fog,\nalso the mist that is part "
            "of a place's look (the Holy Forest).\nWhere the game draws nothing (behind a turn the game "
            "never shows),\nthe background shows in the fog's colour. Applies at once. (--draw-distance)");
    if (port_options.draw_distance != s.draw_distance) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(for this run: command line)");
    }
    ImGui::BeginDisabled(port_options.draw_distance == PORT_DRAW_DISTANCE_1);
    bool sky = port_options.draw_distance_sky != 0;
    if (ImGui::Checkbox("Push the sky's fog out too", &sky)) {
        s.draw_distance_sky = port_options.draw_distance_sky = sky;
        changed();
    }
    ImGui::EndDisabled();
    tooltip("Off: the sky keeps the PS2's fog (the sky is a dome around you, not further away).\nOn: its fog "
            "goes out as well, which lightens it: the dark sky of the Village\nRuins shows its clouds. "
            "(--draw-distance-sky push)");
    {
        static const char *const scalings[] = {"Sharp pixels", "Smooth (bilinear)", "Best"};
        int scaling = s.scaling;
        if (ImGui::Combo("Scaling to the window", &scaling, scalings, 3)) {
            s.scaling = scaling;
            s.smooth = scaling != PORT_SCALING_SHARP;
            changed();
        }
        tooltip("How the picture is fitted to the window.\nSharp pixels: each pixel repeated, blocky.\n"
                "Smooth: bilinear filtering, as before. Made much smaller (a large native scale in a smaller\n"
                "window) it reads only 2 x 2 pixels for each, so fine detail flickers and edges step.\n"
                "Best (the default): the native GS scales the picture itself, with a cubic filter that\n"
                "averages every pixel the window's pixel covers when made smaller, and keeps edges sharp\n"
                "when made larger, on linear light. About 0.2 to 0.4 ms of the GPU a picture.\n"
                "(--present-scaling sharp|smooth|best)");
        if (port_options.present_scaling >= 0 && port_options.present_scaling != s.scaling) {
            ImGui::SameLine();
            ImGui::TextColored(colour_note, "(for this run: command line)");
        }
        float sharpen = s.sharpen;
        if (ImGui::SliderFloat("Sharpen", &sharpen, 0.0f, 1.0f, sharpen > 0 ? "%.2f" : "off")) {
            s.sharpen = sharpen < 0.02f ? 0.0f : sharpen;
            changed();
        }
        tooltip("Contrast-adaptive sharpening after the scaling: fine detail crisper, hard edges left\n"
                "alone. 0: off (the default). It uses the best scaling. (--present-sharpen 0 to 1)");
    }
    bool vsync = s.vsync != 0;
    if (ImGui::Checkbox("VSync", &vsync)) {
        s.vsync = vsync;
        port_video_apply_settings();
        changed();
    }
    tooltip("Waits for the screen's refresh to show each picture, which stops tearing.\nThe game keeps its "
            "own 59.94 Hz pace either way; on a 60 Hz screen a picture\nis shown twice about every 17 "
            "seconds.");

    ImGui::SeparatorText("The PS2's picture");
    ImGui::TextWrapped("How the PC turns the GS's output into a picture. Each is on by default; turn them all off "
                       "to see the GS's raw output, as PCSX2 shows it without its own fixes.");
    struct Switch {
        const char *label;
        const char *tip;
        int *setting;
        int *option;
        bool ps2_when_on; /* the option on is the PS2's way: the box shows it off */
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
         &s.anti_blur, &port_options.anti_blur},
        {"Picture width follows the game's (adapt width)",
         "On: the picture is scanned out at the game's width.\nOff: at the TV's width. (--no-adapt-width)",
         &s.adapt_width, &port_options.adapt_width},
        {"Whole frames only (no interlace comb)",
         "On: the game leaves out the picture between it draws over every other line while the view\n"
         "turns, for the other field of an interlaced TV, which on a PC screen is a comb trailing what\n"
         "moves (--frame-rate 60 draws real pictures between), and shows the movies' pictures whole\n"
         "instead of as two fields (from the next movie), and the menus, drawn a field at a time, as one\n"
         "moment each picture. Off: as on the PS2. (--no-whole-frame)",
         &s.whole_frame, &port_options.whole_frame},
        {"Scrolling textures without a seam",
         "On: the sky's clouds, the lava and the water repeat evenly. Off: as on the PS2, the game draws\n"
         "each with its last row and column half a texel short, which shows as a line where the clouds\n"
         "or the lava warp, clearer at a higher native resolution. (--no-seamless-scroll)",
         &s.seamless_scroll, &port_options.seamless_scroll},
        {"No flicker filter (no up-and-down blur)",
         "On (the default): the rows as the game drew them. Off: as on the PS2, every frame the game\n"
         "blends each row of the picture half with the row above, so that thin lines do not flicker\n"
         "between an interlaced TV's two fields; on a PC screen that blurs the picture up and down, and\n"
         "at a native GS resolution above 1x shows it twice, a GS row apart. (--flicker-filter)",
         &s.flicker_filter, &port_options.flicker_filter, true},
    };
    for (Switch &item : switches) {
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

// ---- Audio ---------------------------------------------------------------------------------------
void audio_page() {
    PortSettings &s = port_settings;

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
    tooltip("The volume changes what you hear only. The game's sound is made the same way,\nand --audio-dump "
            "writes it as made.");
    ImGui::Spacing();
    if (port_options.no_audio)
        ImGui::TextColored(colour_note, "No sound device this run (--no-audio).");
    else if (port_audio_device_clock())
        ImGui::TextDisabled("The sound plays on the sound device's clock, as the IOP did.");
    else
        ImGui::TextDisabled("The sound follows the game's clock.");
    ImGui::Spacing();
    ImGui::TextWrapped("Music and sound effects have one volume for now. Both are played by the same synthesizer "
                       "on the same voices of the sound chip; setting them apart needs a volume per voice in the "
                       "SPU2 (docs/port/AUDIO.md).");
}

// ---- Controls ------------------------------------------------------------------------------------
void binding_button(int layout, int action, int slot) {
    int listen_action, listen_slot;
    bool listening = port_controls_listening(&listen_action, &listen_slot) && listen_action == action &&
                     listen_slot == slot;
    char label[96];

    if (listening) {
        std::snprintf(label, sizeof(label), "%s###b%d_%d", slot == BINDING_PAD_SLOT ? "Press a button..." : "Press a key...",
                      action, slot);
    } else {
        char name[64];
        port_controls_binding_label(port_controls_binding(layout, action, slot), name, sizeof(name));
        std::snprintf(label, sizeof(label), "%s###b%d_%d", name, action, slot);
    }
    if (listening)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    if (ImGui::Button(label, ImVec2(-FLT_MIN, 0)))
        port_controls_listen(action, slot);
    if (listening)
        ImGui::PopStyleColor();
    if (!listening && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        port_controls_bind(layout, action, slot, BINDING_NONE);
        changed();
    }
}

void controls_page() {
    PortSettings &s = port_settings;
    int layout = port_controls_layout();
    int listen_action, listen_slot;
    static int was_listening;
    bool listening = port_controls_listening(&listen_action, &listen_slot);

    // a binding was made (or cleared) since the last frame
    if (was_listening && !listening)
        changed();
    was_listening = listening;

    if (ImGui::RadioButton("PS2 pad (keyboard as the pad)", layout == CONTROLS_PS2)) {
        port_controls_set_layout(CONTROLS_PS2);
        std::snprintf(s.layout, sizeof(s.layout), "%s", port_controls_layout_name(CONTROLS_PS2));
        changed();
    }
    tooltip("Arrows, W A S D and I J K L, Z X C V as the face buttons: the keyboard as a pad. (--controls ps2)");
    ImGui::SameLine();
    if (ImGui::RadioButton("Verdite (mouse look)", layout == CONTROLS_VERDITE)) {
        port_controls_set_layout(CONTROLS_VERDITE);
        std::snprintf(s.layout, sizeof(s.layout), "%s", port_controls_layout_name(CONTROLS_VERDITE));
        changed();
    }
    tooltip("The layout of Verdite, the KF2/KF3 ports: W A S D to walk and side-step,\nthe mouse to turn and "
            "look (Escape captures it), Space to attack. (--controls verdite)");
    layout = port_controls_layout();

    if (layout == CONTROLS_VERDITE) {
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
        static const int menu_actions[] = {-1, ACTION_CROSS, ACTION_CIRCLE, ACTION_SQUARE, ACTION_TRIANGLE,
                                           ACTION_START, ACTION_SELECT};
        static const char *const menu_labels[] = {"Nothing", "Cross", "Circle", "Square", "Triangle", "Start", "Select"};
        static const char *const buttons[3] = {"Left button in menus", "Right button in menus",
                                               "Middle button in menus"};
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
        tooltip("Outside play (menus, pause, talking) the mouse presses these. In play, with the mouse\ncaptured, "
                "its buttons are the bindings below.");
        bool side_keys = port_controls_menu_side_keys() != 0;
        if (ImGui::Checkbox("A / D move left / right in menus", &side_keys)) {
            port_controls_set_menu_side_keys(side_keys);
            changed();
        }
        tooltip("Outside play the keys of L1 / R1 (A / D) press d-pad left / right, and the keys of d-pad\n"
                "left / right (the arrows) press L1 / R1: A / D pick the equipment, turn a list's pages and\n"
                "change the options' values, and the arrows turn the weapon on the equipment screen. In play\n"
                "nothing changes, nor does a controller.");
    }

    ImGui::Spacing();
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
            if (!port_controls_action_used(layout, action))
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (port_controls_is_default(layout, action))
                ImGui::TextUnformatted(port_controls_action_label(action));
            else
                ImGui::TextColored(colour_note, "%s *", port_controls_action_label(action));
            for (int slot = 0; slot < BINDING_SLOTS; slot++) {
                ImGui::TableNextColumn();
                binding_button(layout, action, slot);
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Defaults")) {
        port_controls_reset(layout);
        changed();
    }
    tooltip("Puts this layout's keys, buttons and mouse settings back as they were.");
    ImGui::SameLine();
    ImGui::TextDisabled("* changed from the default");
}

// ---- Save states ---------------------------------------------------------------------------------
void key_name(int action, char *out, size_t size) {
    port_controls_binding_label(port_controls_binding(port_controls_layout(), action, 0), out, size);
}

void quick_saves_section() {
    PortSettings &s = port_settings;
    char save_key[64], load_key[64];

    key_name(ACTION_GAME_SAVE, save_key, sizeof(save_key));
    key_name(ACTION_GAME_LOAD, load_key, sizeof(load_key));
    ImGui::SeparatorText("Quick saves (for playing)");
    ImGui::TextWrapped("Use these to play: they load in any build of the program, so they survive updates. %s "
                       "saves to the chosen slot and %s loads it, in play (not in menus, messages or events); "
                       "a load works on the title screen too.",
                       save_key, load_key);
    tooltip("A quick save is the game's own save (what a save point writes to the memory card), taken where you "
            "stand: the map, your position, heading and look, HP and MP, level, stats, items, magic, gold and the "
            "world's flags (doors, chests, events, items picked up).\n"
            "Loading restarts the map from it, as loading a memory card save does: enemies come back as when "
            "you enter a map, and what lasts only a while (ailments' timers, the power and magic gauges, "
            "spell effects) starts afresh. Nothing is written to the memory card.");
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

void save_states_page() {
    PortSettings &s = port_settings;
    char save_key[64], load_key[64];

    quick_saves_section();
    ImGui::Spacing();
    key_name(ACTION_QUICK_SAVE, save_key, sizeof(save_key));
    key_name(ACTION_QUICK_LOAD, load_key, sizeof(load_key));
    ImGui::SeparatorText("Save states (for debugging)");
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
    std::string text = "holyland (King's Field IV PC port)\n";

    std::snprintf(line, sizeof(line), "vblank %llu, %.1f pictures/s, %.2f vblanks/s, %.1f game frames/s\n",
                  (unsigned long long)port_vblank_count(), port_video_pictures_per_second(), vblanks_per_second, frames_per_second);
    text += line;
    kf4_game_state(line, sizeof(line));
    text += line;
    text += "\n";
    std::snprintf(line, sizeof(line),
                  "options: controls=%s fast=%d deinterlace=%d progressive=%d anti_blur=%d adapt_width=%d "
                  "no_audio=%d device_clock=%d\n",
                  port_controls_layout_name(port_controls_layout()), port_options.fast, port_options.deinterlace,
                  port_options.progressive, port_options.anti_blur, port_options.adapt_width, port_options.no_audio,
                  port_audio_device_clock());
    text += line;
    return text;
}

// The renderer (docs/port/STEP3.md): which draws from the next start; with both, which is shown and how
// their pictures compare
void renderer_section() {
    static const char *const names[] = {"paraLLEl-GS (gs, the reference)", "Native GS (native, the default)",
                                        "Both, compared (both)", "Modern (modern)", "Fast, the PC renderer (fast)"};
    ImGui::SeparatorText("Renderer");
    int now = port_options.renderer_fast     ? PORT_RENDERER_FAST
              : port_options.renderer_modern ? PORT_RENDERER_MODERN
                                             : port_options.renderer;
    ImGui::Text("This run: %s", names[now]);
    int next = port_settings.renderer;
    if (ImGui::Combo("From the next start", &next, names, IM_ARRAYSIZE(names))) {
        port_settings.renderer = next;
        port_settings_changed();
    }
    tooltip("The native GS (step 3, the default) draws with the GPU's own rasteriser: faster, and the\n"
            "base for the higher resolutions, true widescreen and anti-aliasing. paraLLEl-GS emulates the\n"
            "PS2's GS and is the reference to compare with (and what draws if the native GS cannot start).\n"
            "Both runs the two side by side and compares the pictures every vblank (slower; for testing).\n"
            "Modern draws the scene (the map, the sky, the items) itself into its own targets, at the\n"
            "game's level; fast is modern with the PC renderer's parts built so far (textures read by the\n"
            "GPU's sampler: much less GPU time at high scales and anisotropy).\n"
            "--renderer on the command line wins over this.");
    if (port_options.renderer != PORT_RENDERER_GS) {
        int scale = port_options.native_scale;
        char scale_name[32];
        if (scale >= 1) {
            snprintf(scale_name, sizeof(scale_name), "%dx", scale);
        } else {
            snprintf(scale_name, sizeof(scale_name), "Auto (%dx)", port_gs_native_scale());
        }
        if (ImGui::SliderInt("Native GS resolution", &scale, 0, 4, scale_name)) {
            port_gs_set_native_scale(scale);
            port_settings.native_scale = scale;
            port_settings_changed();
        }
        tooltip("The native GS draws its frame buffers at this many times the PS2's 640x448 (2x: 1280x896,\n"
                "4x: 2560x1792). 1x is the PS2's picture; with --renderer both the comparison is made at 1x\n"
                "pixels either way. Auto (the default): the multiple of 448 lines nearest the height the\n"
                "window shows the picture at (1080: 2x, 1440: 3x, 2160: 4x), at most 2x on an integrated GPU.");
        bool wide = port_options.native_widescreen != 0;
        if (ImGui::Checkbox("Native widescreen", &wide)) {
            port_options.native_widescreen = port_settings.native_widescreen = wide;
            port_gs_set_native_scale(port_options.native_scale);
            port_settings_changed();
        }
        tooltip("With widescreen on: the native GS draws the 16:9 picture with more pixels across, about\n"
                "square ones, instead of the 640 columns the PS2 has stretched to 16:9 (at 1x 1280x448,\n"
                "2x 1920x896, 3x 2560x1344, 4x 3840x1792). The picture is the same, sharper across.\n"
                "Without widescreen it changes nothing. (--native-widescreen)");
        static const char *const msaa_names[] = {"Off", "2x", "4x", "8x"};
        int msaa_at = port_settings.native_msaa >= 8 ? 3 : port_settings.native_msaa >= 4 ? 2 : port_settings.native_msaa >= 2 ? 1 : 0;
        if (ImGui::Combo("Native GS anti-aliasing (MSAA)", &msaa_at, msaa_names, 4)) {
            port_settings.native_msaa = 1 << msaa_at;
            port_gs_set_native_msaa(port_settings.native_msaa);
            port_settings_changed();
        }
        if (port_settings.native_msaa != port_options.native_msaa) {
            ImGui::TextDisabled("This run: %s (from one count to another takes a restart, or the GPU has fewer)",
                                port_options.native_msaa > 1 ? msaa_names[port_options.native_msaa >= 8 ? 3 : port_options.native_msaa >= 4 ? 2 : 1]
                                                             : "Off");
        }
        tooltip("The native GS draws the 3D with this many samples a pixel, which smooths its edges (the 2D\n"
                "and the passes over the whole frame stay as they are). Off is the PS2's picture. Costs GPU\n"
                "time, more with the resolution above. Off and back on is live; from one count to another\n"
                "takes a restart. (--native-msaa)");
        bool cutout = port_options.native_cutout_aa != 0;
        if (ImGui::Checkbox("Native GS anti-aliasing: smooth cut-outs too", &cutout)) {
            port_gs_set_native_cutout_aa(cutout ? 1 : 0);
            port_settings.native_cutout_aa = cutout ? 1 : 0;
            port_settings_changed();
        }
        tooltip("With MSAA above: textures see-through in places (netting, foliage, fences, grates, hair) are\n"
                "shaded at every sample instead of once a pixel, so the edges inside them are smoothed and\n"
                "shimmer less (more so with more samples). Costs GPU time only where they are drawn.\n"
                "Without MSAA it changes nothing. (--native-cutout-aa ssaa|off)");
        bool smooth = port_options.native_2d != 0;
        if (ImGui::Checkbox("Native GS: smooth 2D at the larger resolution", &smooth)) {
            port_gs_set_native_2d(smooth ? 1 : 0);
            port_settings.native_2d = smooth ? 1 : 0;
            port_settings_changed();
        }
        tooltip("At a resolution above the PS2's: the text, the HUD and the menus' and title's pictures, which\n"
                "the game reads a texel a pixel, are read smoothly where they are magnified: letters with even,\n"
                "smooth edges. Off, each texel is a block, as the PS2's picture at 1x blown up. Nothing at 1x.\n"
                "(--native-2d smooth|ps2)");
        static const char *const lod_names[] = {"PS2's (ps2)", "Matched to the resolution (scale, the default)",
                                                "Finest everywhere (full)"};
        int lod = port_options.native_lod;
        if (ImGui::Combo("Native GS texture detail", &lod, lod_names, 3)) {
            port_gs_set_native_lod(lod);
            port_settings.native_lod = lod;
            port_settings_changed();
        }
        tooltip("Which of a texture's mipmap levels (its smaller copies; about half the maps have them) the\n"
                "native GS reads. The PS2 picks the level from the distance alone, for its 640x448, so at a\n"
                "higher resolution far surfaces blur where the PS2's were sharp. Matched: the level the PS2\n"
                "would pick at this resolution (at 1x, the PS2's own). PS2's: its level at any resolution.\n"
                "Finest: the whole texture at every distance (at 1x too), sharpest, but far surfaces can\n"
                "shimmer. (--native-lod)");
        static const char *const aniso_names[] = {"Off", "2x", "4x", "8x", "16x"};
        int aniso_at = port_options.native_aniso >= 16 ? 4 : port_options.native_aniso >= 8 ? 3 : port_options.native_aniso >= 4 ? 2 : port_options.native_aniso >= 2 ? 1 : 0;
        if (ImGui::Combo("Native GS anisotropic filtering", &aniso_at, aniso_names, 5)) {
            port_gs_set_native_aniso(1 << aniso_at);
            port_settings.native_aniso = port_options.native_aniso;
            port_settings_changed();
        }
        tooltip("Floors and walls seen at a grazing angle: the PS2 reads a texture's mipmap level from the\n"
                "distance alone, the same along the surface and across it, so they blur with distance. This\n"
                "reads up to this many times along a pixel's footprint on the texture, each at a finer level,\n"
                "for the detail across. Off is the PS2's picture. Costs GPU time on mipmapped surfaces.\n"
                "(--native-aniso)");
        static const char *const colour_names[] = {"PS2's 8 bits (exact, the default)", "High (16 bits, dithered)"};
        int colour = port_options.native_colour;
        if (ImGui::Combo("Native GS colour precision", &colour, colour_names, 2)) {
            port_gs_set_native_colour(colour);
            port_settings.native_colour = colour;
            port_settings_changed();
        }
        tooltip("How precisely the native GS keeps colours. The PS2's: 8 bits a channel, every step rounded as\n"
                "the GS rounds it, which shows as steps in smooth dark gradients (the sky, dim walls, glows).\n"
                "High: 16 bits a channel and the arithmetic without the roundings, the picture dithered to the\n"
                "screen's 8 bits, so the gradients are smooth. Alpha, and what the game reads back, stay the\n"
                "PS2's. (--native-colour)");
        static const char *const layer_z_names[] = {"PS2's (whole numbers)", "Tolerant (clean layers and seams, the default)"};
        int layer_z = port_options.native_layer_z;
        if (ImGui::Combo("Native GS depth", &layer_z, layer_z_names, 2)) {
            port_gs_set_native_layer_z(layer_z);
            port_settings.native_layer_z = layer_z;
            port_settings_changed();
        }
        tooltip("How the native GS keeps the 3D's depth. Much of a map is drawn twice, a surface and then a\n"
                "layer blended over it (a second texture, the darkness in the arches), and on the PS2 the\n"
                "layer fails the depth test in places: dark hatching, combs of teeth, a lace of light lines,\n"
                "very visible at a higher resolution. Where two surfaces meet at a shallow angle the PS2's\n"
                "whole-number depth makes a sawtooth. Tolerant keeps depth's fraction and lets a layer pass\n"
                "within a hair of the surface under it: clean layers and straight seams. PS2's is the PS2's\n"
                "picture. (--native-layer-z)");
        if (port_options.renderer_modern) {
            bool shine_exact = port_options.native_shine_exact != 0;
            if (ImGui::Checkbox("Ice shine: exact PS2 folds (slower)", &shine_exact)) {
                port_gs_set_native_shine_exact(shine_exact ? 1 : 0);
                port_settings.native_shine_exact = shine_exact ? 1 : 0;
                port_settings_changed();
            }
            tooltip("The ice creatures' shine (map 017) is blended in a shader. Where a part of a creature\n"
                    "folds over its own edge, the PS2 blends the shine twice along that edge. Off (the default):\n"
                    "the shine is drawn whole, once, and much faster (a few pixels along those edges are a little\n"
                    "darker). On: drawn in many small pieces, the PS2's picture exactly. Applies at once.\n"
                    "(--native-shine exact|whole)");
        }
        // (--native-draw on / off, saved; the kinds one by one are the Debug page's: docs/port/NATIVE_DRAW.md)
        bool native_draw = (port_options.native_draw & PORT_ND_ACCEPTED) == PORT_ND_ACCEPTED;
        if (ImGui::Checkbox("Native draw (experimental)", &native_draw)) {
            port_gs_set_native_draw(native_draw ? PORT_ND_ACCEPTED : 0, port_options.native_draw_compare);
            port_settings.native_draw = native_draw ? 1 : 0;
            port_settings_changed();
        }
        tooltip("The game's 3D (the map, the sky, items, objects, entities, the arm, gold, effects, shadows and\n"
                "the water) drawn at the game's level: the renderer core and vertex shaders doing VU1's maths,\n"
                "instead of VU1 and its packets. The picture is the PS2 path's (at a larger resolution the\n"
                "enemies' highlights are not snapped to pixel edges); between the game's frames at a higher\n"
                "frame rate, moving models move smoothly. The way to the native renderer's own targets (P8).\n"
                "Off is the PS2 path. Applies at once. (--native-draw on|off; the kinds one by one on the\n"
                "Debug page)");
        if (port_options.native_draw != 0 && port_options.native_draw != PORT_ND_ACCEPTED) {
            ImGui::SameLine();
            ImGui::TextColored(colour_note, "(this run: a choice of kinds, the Debug page)");
        }
    }
    static const char *const presenters[] = {"SDL_Renderer (sdl)", "Vulkan, on the native GS's device (vulkan, the default)"};
    int presenter = port_video_presenter();
    if (port_options.presenter == PORT_PRESENTER_VULKAN && presenter != PORT_PRESENTER_VULKAN)
        ImGui::Text("Presenter this run: %s (vulkan %s)", presenters[presenter],
                    port_options.renderer == PORT_RENDERER_GS ? "needs the native GS" : "did not start");
    else
        ImGui::Text("Presenter this run: %s", presenters[presenter]);
    int next_presenter = port_settings.presenter;
    if (ImGui::Combo("Presenter, from the next start", &next_presenter, presenters, 2)) {
        port_settings.presenter = next_presenter;
        port_settings_changed();
    }
    tooltip("How the picture reaches the window. SDL_Renderer copies each picture through the CPU into\n"
            "a texture. Vulkan presents on the native GS's own device: the native GS's picture goes to\n"
            "the window without leaving the GPU (faster, most of all at higher resolutions). Vulkan needs\n"
            "the native GS (renderer native or both); without it, or if it fails, the window is\n"
            "SDL_Renderer's. --presenter on the command line wins over this.");
    if (port_options.renderer == PORT_RENDERER_BOTH) {
        bool native = port_options.renderer_shown == PORT_RENDERER_NATIVE;
        ImGui::BeginDisabled(port_options.renderer_heatmap != 0);
        if (ImGui::Checkbox("Show the native GS's picture", &native))
            port_options.renderer_shown = native ? PORT_RENDERER_NATIVE : PORT_RENDERER_GS;
        ImGui::EndDisabled();
        bool heatmap = port_options.renderer_heatmap != 0;
        if (ImGui::Checkbox("Show where they differ (heat map)", &heatmap))
            port_options.renderer_heatmap = heatmap;
        tooltip("The window shows the displayed frame buffer's difference, pixel by pixel: grey where the\n"
                "two are within 1 in every channel, else coloured by the largest difference, from blue (2)\n"
                "through green and yellow (16) to red (64 and more). At the PS2's resolution. Not saved.\n"
                "(--renderer-shown heatmap)");
        const PortRendererDiff &d = port_renderer_diff;
        ImGui::Text("Last picture: %u pixels differ by more than 1, at most %u (mean %.3f)", d.last_pixels, d.last_max,
                    d.last_mean);
        ImGui::Text("%llu of %llu pictures differ; the largest difference %u, at vblank %llu",
                    (unsigned long long)d.frames_differ, (unsigned long long)d.frames, d.worst_max,
                    (unsigned long long)d.worst_vblank);
        tooltip("The frame buffer the display shows, as each renderer drew it (not the scanout, where the\n"
                "two place and weave the picture each their own way).");
    }
}

// The debugging tools (port/include/port/debug.h): files for a bug report, in the captures folder
void debug_tools() {
    char keys[3][64];
    const int actions[3] = {ACTION_GS_CAPTURE, ACTION_STATE_DUMP, ACTION_AUDIO_TAIL};
    for (int i = 0; i < 3; i++)
        port_controls_binding_label(port_controls_binding(port_controls_layout(), actions[i], 0), keys[i],
                                    sizeof(keys[i]));

    ImGui::SeparatorText("Debugging tools");
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
        port_controls_binding_label(port_controls_binding(port_controls_layout(), ACTION_FLASH_RECORD, 0), key, sizeof(key));
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

    ImGui::SeparatorText("Teleport");
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
    if (ImGui::Button("Teleport"))
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

// ---- Mods ----------------------------------------------------------------------------------------
void mods_page() {
    PortSettings &s = port_settings;

    ImGui::TextWrapped("Optional changes to how the game plays. All are off by default; with them off, the game plays as it does on the PS2.");
    ImGui::Spacing();
    bool necklace = port_options.myus_necklace != 0;
    if (ImGui::Checkbox("Myu's Necklace effect without wearing it", &necklace)) {
        s.myus_necklace = port_options.myus_necklace = necklace;
        changed();
    }
    tooltip("Objects that glow while Myu's Necklace is worn always glow, so the amulet slot\nis free for another "
            "amulet. Applies at once. (--myus-necklace)");
    if (port_options.myus_necklace != s.myus_necklace) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(on for this run: command line)");
    }
    bool separate_run = port_options.separate_run != 0;
    if (ImGui::Checkbox("Separate run key", &separate_run)) {
        s.separate_run = port_options.separate_run = separate_run;
        changed();
    }
    tooltip("Run while Left Shift (or a controller's left stick press) is held, instead of while the\naction "
            "button is: the action button then only talks, picks up and opens. Rebind it on the\nControls "
            "page (Run). Applies at once. (--separate-run)");
    if (port_options.separate_run != s.separate_run) {
        ImGui::SameLine();
        ImGui::TextColored(colour_note, "(on for this run: command line)");
    }
    port_stats_overlay_mods(); // the extended stats mod (port/overlay/stats_panel.cpp)
    automap_view_mods_section(); // the automap mod (port/overlay/automap_view.cpp)
    weapon_sets_view_mods_section(); // the weapon sets mod (port/overlay/weapon_sets_view.cpp)
}

void debug_page() {
    PortGameReadout game;
    kf4_game_readout(&game);

    ImGui::SeparatorText("Speed");
    ImGui::Text("Pictures shown: %.1f a second", port_video_pictures_per_second());
    tooltip("The new pictures the window presented, whichever thread showed them (the game's, or the\n"
            "pictures between's own at --frame-rate above 60).");
    ImGui::Text("Vblanks: %.2f a second (%.0f%% of the PS2's 59.94)", vblanks_per_second,
                vblanks_per_second * 100.0 / 59.94);
    ImGui::Text("Game frames: %.1f a second", frames_per_second);
    bool stats = port_options.stats != 0;
    if (ImGui::Checkbox("Count where the DMA controller's time goes (--stats)", &stats))
        port_options.stats = stats;
    tooltip("Counts the time spent running VU1, handing packets to the GS and scanning out.\nIt costs a little "
            "time itself, so it is off unless asked for.");
    if (port_options.stats) {
        ImGui::Text("VU1: %.1f ms a second, %.0f runs, %.2f M instructions", vu1_ms, vu1_runs, vu1_insns / 1e6);
        ImGui::Text("GIF hand-off: %.1f ms a second    Scanout: %.1f ms a second", gif_ms, vsync_ms);
        ImGui::TextDisabled("VU1 cycles %llu, PATH1 hash %016llx", (unsigned long long)port_stats.vu1_cycles,
                            (unsigned long long)port_stats.path1_hash);
    }

    ImGui::SeparatorText("Native draw (experimental)");
    {
        // (--native-draw, --native-draw-compare: docs/port/NATIVE_DRAW.md)
        static const struct {
            const char *name;
            int bits;
            const char *what;
        } kinds[] = {{"Items", PORT_ND_ITEMS, "The items lying in the maps (MapItem_DrawAll)."},
                     {"Map records", PORT_ND_MAP, "The map's own geometry (Map_DrawRecords, Map_DrawLateRecords) but the sky."},
                     {"Sky", PORT_ND_SKY, "The sky's dome (the map record with flag bit 31), drawn as the setting below says."},
                     {"Entities", PORT_ND_SKINNED, "Enemies and NPCs (Entity_DrawAll, the draw list)."},
                     {"Arm", PORT_ND_ARM, "The arm, the held weapon and its trail (Player_DrawArm)."},
                     {"Map objects", PORT_ND_OBJECTS, "Doors, chests, levers ... (Model_DrawPosedWithAlpha, MapObject_DrawAll)."},
                     {"Picked item", PORT_ND_PICKED, "The item being picked up, after its depth clear."},
                     {"Gold", PORT_ND_GOLD, "The gold lying in the maps (Gold_Draw, the draw list)."},
                     {"Effects", PORT_ND_EFFECTS, "Fire, glows, magic (the draw list's effects, VU1 program 0x700)."},
                     {"Shadows", PORT_ND_SHADOWS, "The entities' shadows (Entity_DrawPoolModels, VU1 program 0x5A0)."},
                     {"Water", PORT_ND_WATER, "The reflection's rippled water on maps 008, 009 and 014 (Wobble_DrawRippledModel)."},
                     {"Screen passes", PORT_ND_PASSES,
                      "The brightness, tints, fills and fades, the flicker filter, the frame before, haze, the wobble."}};
        int kinds_on = port_options.native_draw, compare = port_options.native_draw_compare;
        for (const auto &k : kinds) {
            bool on = (kinds_on & k.bits) != 0;
            if (ImGui::Checkbox(k.name, &on))
                kinds_on = on ? kinds_on | k.bits : kinds_on & ~k.bits;
            std::string tip = std::string(k.what) + " Drawn at the game's level: the renderer core and a vertex\n"
                              "shader doing VU1's maths, instead of VU1 and its packets (--native-draw). Off is the PS2 path.";
            tooltip(tip.c_str());
            ImGui::SameLine();
        }
        ImGui::NewLine();
        if (ImGui::Button("Accepted kinds (on)"))
            kinds_on = PORT_ND_ACCEPTED;
        tooltip("Every kind that has passed its checks, as --native-draw on and the Renderer section's\n"
                "switch below. Saved only from that switch.");
        ImGui::SameLine();
        if (ImGui::Button("None (off)"))
            kinds_on = 0;
        {
            // (--sky, settings.json display.native_sky: game/native_draw/nd_sky.c)
            static const char *const skies[] = {"PS2 (at the feet, its real distance)", "Modern (around the eye, behind everything)"};
            int sky = port_options.native_sky == PORT_SKY_PS2 ? 0 : 1;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
            if (ImGui::Combo("Native sky", &sky, skies, 2)) {
                port_options.native_sky = sky == 0 ? PORT_SKY_PS2 : PORT_SKY_MODERN;
                port_settings.native_sky = port_options.native_sky;
                changed();
            }
            tooltip("With the sky drawn natively (Sky above): how.\nModern: the dome is centred on the eye and drawn "
                    "behind everything, as a modern\ngame's sky: what lies beyond it shows (the tops of far hills on "
                    "a few maps).\nWhere the game draws other things before the sky (maps 000 and 004) it stays "
                    "the PS2's.\nPS2: the dome at the feet with its real distance, as the PS2 draws it: it hides\n"
                    "what lies beyond it. Applies at once. (--sky modern|ps2)");
            if (port_options.native_sky != port_settings.native_sky) {
                ImGui::SameLine();
                ImGui::TextColored(colour_note, "(for this run: command line)");
            }
        }
        const char *compares[] = {"Off", "Both queued, native drawn", "Both queued, PS2 path drawn"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        ImGui::Combo("Compare", &compare, compares, 3);
        tooltip("--native-draw-compare: the PS2 path's packets are queued as well (VU1 runs them), and the\n"
                "native GS draws one of the two: switch to see the difference in the same frame.");
        if (kinds_on != port_options.native_draw || compare != port_options.native_draw_compare)
            port_gs_set_native_draw(kinds_on, compare);
        if (port_options.native_draw)
            ImGui::TextDisabled("%llu draws queued, %llu lost, %llu meshes held, %llu parsed",
                                (unsigned long long)rd_stats.draws, (unsigned long long)rd_stats.lost,
                                (unsigned long long)rd_stats.meshes_held, (unsigned long long)rd_stats.meshes_parsed);
    }

    ImGui::SeparatorText("Game");
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
    ImGui::SeparatorText("Cheats for testing");
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
    teleport_section();
    renderer_section();
    debug_tools();
}

// ---- Interface -----------------------------------------------------------------------------------
void interface_page() {
    PortSettings &s = port_settings;

    bool pause = s.pause_when_open != 0;
    if (ImGui::Checkbox("Pause the game while the overlay is open", &pause)) {
        s.pause_when_open = pause;
        changed();
    }
    tooltip("Off: the game runs on behind the overlay, as it does now.");
    bool fps = port_options.show_fps != 0;
    char fps_key[64];
    port_controls_binding_label(port_controls_binding(port_controls_layout(), ACTION_SHOW_FPS, 0), fps_key,
                                sizeof(fps_key));
    if (ImGui::Checkbox("Frame-rate counter", &fps)) {
        s.show_fps = port_options.show_fps = fps;
        changed();
    }
    tooltip("The pictures the window shows a second, in the picture's top right corner, with the\n"
            "overlay closed too; below it the game's own frames a second when they differ (the game\n"
            "runs at 30, the pictures between are the PC's). The key (Controls page) shows and hides\n"
            "it in play. (--show-fps)");
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
    ImGui::TextDisabled("Applies when you let go of the slider.");
    ImGui::Spacing();
    ImGui::SeparatorText("Settings file");
    ImGui::TextWrapped("%s", port_settings_path());
    ImGui::TextDisabled("Options on the command line override it for one run.");
}

// ---- Windows -------------------------------------------------------------------------------------
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
        if (ImGui::Selectable(page_names[i], page == i))
            page = i;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("page");
    switch (page) {
    case PAGE_DISPLAY: display_page(); break;
    case PAGE_AUDIO: audio_page(); break;
    case PAGE_CONTROLS: controls_page(); break;
    case PAGE_SAVE_STATES: save_states_page(); break;
    case PAGE_MODS: mods_page(); break;
    case PAGE_DEBUG: debug_page(); break;
    default: interface_page(); break;
    }
    ImGui::EndChild();
    ImGui::End();
}

void about_window() {
    if (!ImGui::Begin("About", &show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("holyland: King's Field IV on the PC, built from the kfiv-decomp decompilation");
    ImGui::Separator();
    ImGui::BulletText("The game's code: kfiv-decomp (Voicedrew11 and contributors)");
    ImGui::BulletText("The GS: paraLLEl-GS (Arntzen Software, LGPL-3.0+)");
    ImGui::BulletText("The SPU2: ico-audio, from ico-pc (MIT)");
    ImGui::BulletText("The Verdite layout and mouse indicator: Verdite2 (Voicedrew11, MIT)");
    ImGui::BulletText("Dear ImGui (MIT), cJSON (MIT), SDL2 (zlib)");
    ImGui::End();
}

void menu_bar() {
    if (!ImGui::BeginMainMenuBar())
        return;
    ImGui::MenuItem("Settings", nullptr, &show_settings);
    ImGui::MenuItem("About", nullptr, &show_about);
    char status[96];
    std::snprintf(status, sizeof(status), "F1 to close%s", port_overlay_paused() ? "   (game paused)" : "");
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(status).x - 16);
    ImGui::TextDisabled("%s", status);
    ImGui::EndMainMenuBar();
}

// The last save state request's message, a while after its key was pressed
void draw_toast() {
    Uint32 now = SDL_GetTicks();
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

// "Building shaders": the native GS's pipelines not in the pipeline cache, made at the start (port_gs_shaders_building)
bool building_shown() {
    int done, total;
    return port_gs_shaders_building(&done, &total) != 0 && total > 0;
}

void draw_building() {
    int done, total;
    if (!port_gs_shaders_building(&done, &total) || total <= 0)
        return;
    char text[64];
    std::snprintf(text, sizeof(text), "Building shaders... %d/%d", done, total);
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImFont *font = ImGui::GetFont();
    float size = viewport->Size.y / 40.0f > 16.0f ? viewport->Size.y / 40.0f : 16.0f;
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    ImVec2 at(viewport->Size.x - extent.x - size * 1.2f, viewport->Size.y - extent.y - size * 1.0f);
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(at.x - size * 0.5f, at.y - size * 0.25f),
                        ImVec2(at.x + extent.x + size * 0.5f, at.y + extent.y + size * 0.25f), IM_COL32(0, 0, 0, 150),
                        size * 0.3f);
    draw->AddText(font, size, at, IM_COL32(255, 235, 170, 255), text);
}

// ---- KF4_OVERLAY_SHOT ----------------------------------------------------------------------------
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
        Shot shot = {-1, std::strtoull(item.c_str() + sign + 1, nullptr, 0)};
        for (int i = 0; i < PAGE_COUNT; i++)
            if (name == page_ids[i])
                shot.page = i;
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
            std::fprintf(stderr, "[overlay] KF4_OVERLAY_SHOT: no page called %s (display, audio, controls, "
                                 "savestates, mods, debug, interface, closed, map, mini, menu, profiler)\n",
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
        else // (page_ids has the settings' pages only)
            std::snprintf(path, sizeof(path), "%s/overlay_%s.png", port_options.dump_dir ? port_options.dump_dir : ".",
                          shot.page == PAGE_COUNT ? "closed" : page_ids[shot.page]);
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
    SDL_GetRendererOutputSize(renderer, &width, &height);
    std::vector<uint32_t> pixels((size_t)width * height);
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ABGR8888, pixels.data(), width * 4) != 0) {
        std::fprintf(stderr, "[overlay] cannot read the window back: %s\n", SDL_GetError());
        shot_write(nullptr, 0, 0);
    } else {
        shot_write(pixels.data(), width, height);
    }
}

#ifdef KF4_HAVE_VULKAN_PRESENTER
// The mouse indicator's squares (port_controls_draw) into ImGui's draw list, under the windows
void fill_rect(void *context, int x, int y, int width, int height, unsigned int rgba) {
    ImDrawList *list = static_cast<ImDrawList *>(context);
    ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    float sx = scale.x > 0.0f ? scale.x : 1.0f, sy = scale.y > 0.0f ? scale.y : 1.0f;
    list->AddRectFilled(ImVec2(x / sx, y / sy), ImVec2((x + width) / sx, (y + height) / sy), rgba);
}
#endif

bool shot_pending() {
    return shot_next < shots.size() && port_vblank_count() >= shots[shot_next].vblank;
}

bool is_input_event(Uint32 type) {
    switch (type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_CONTROLLERAXISMOTION:
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
        started = ImGui_ImplSDL2_InitForSDLRenderer(window, renderer) && ImGui_ImplSDLRenderer2_Init(renderer);
    } else {
#ifdef KF4_HAVE_VULKAN_PRESENTER
        started = ImGui_ImplSDL2_InitForVulkan(window) && port_vk_imgui_init();
#else
        started = false;
#endif
    }
    if (!started) {
        std::fprintf(stderr, "[overlay] Dear ImGui did not start: no overlay\n");
        return;
    }
    ready = true;
    std::fprintf(stderr, "[overlay] F1 opens the settings\n");
}

extern "C" int port_overlay_event(const void *sdl_event) {
    const SDL_Event *event = static_cast<const SDL_Event *>(sdl_event);

    if (!ready)
        return 0;
    if (event->type == SDL_KEYDOWN && !event->key.repeat) {
        SDL_Scancode key = event->key.keysym.scancode;
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
    if (event->type == SDL_KEYUP &&
        (event->key.keysym.scancode == SDL_SCANCODE_F1 || event->key.keysym.scancode == SDL_SCANCODE_F11))
        return 1;
    // the automap mod's full map: its key closes it, and it has the mouse while it is open
    if (!overlay_open && automap_view_event(event))
        return 1;
    if (!overlay_open && port_automap_full_open()) {
        ImGui_ImplSDL2_ProcessEvent(event);
        return is_input_event(event->type);
    }
    // closed, it takes nothing more (and ImGui's queue is only emptied by a frame drawn)
    if (!overlay_open)
        return 0;
    ImGui_ImplSDL2_ProcessEvent(event);
    return is_input_event(event->type);
}

extern "C" int port_overlay_visible(void) {
    return ready && (overlay_open || shot_pending() || port_stats_overlay_wants() ||
                     (toast_until != 0 && (int32_t)(toast_until - SDL_GetTicks()) > 0) || building_shown() ||
                     (renderer == nullptr && port_controls_indicator_shown()) || automap_view_visible() ||
                     weapon_sets_view_visible() || profiler_visible());
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
    return ready && overlay_open;
}

extern "C" int port_overlay_paused(void) {
    return ready && !shot_mode && ((overlay_open && port_settings.pause_when_open) || port_automap_holds_game());
}

extern "C" void port_overlay_notify_state(void) {
    toast_text = port_state_message;
    toast_until = SDL_GetTicks() + 2500;
}

extern "C" void port_overlay_notify_quick(void) {
    toast_text = port_quick_message;
    toast_until = SDL_GetTicks() + 3000;
}

extern "C" void port_overlay_notify_debug(void) {
    toast_text = port_debug_message;
    toast_until = SDL_GetTicks() + 4000;
}

extern "C" void port_overlay_vblank(void) {
    if (!ready)
        return;
    port_stats_overlay_update(); // the extended stats mod (port/overlay/stats_panel.cpp)
    profiler_step();             // Shift+P (port/overlay/profiler.cpp)
}

extern "C" void port_overlay_draw(int x, int y, int width, int height) {
    drawn = false;
    if (!ready)
        return;
    sample_rates();
    port_settings_poll();
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
        ImGui_ImplSDLRenderer2_NewFrame();
    } else {
#ifdef KF4_HAVE_VULKAN_PRESENTER
        ImGui_ImplVulkan_NewFrame();
#endif
    }
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (pad_quiet && !listening) {
        bool held = false;
        for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; key++)
            held |= ImGui::IsKeyDown((ImGuiKey)key);
        pad_quiet = held;
    }
    automap_view_draw(x, y, width, height); // the automap mod, under the overlay's windows
    weapon_sets_view_draw(x, y, width, height); // the weapon sets mod (port/overlay/weapon_sets_view.cpp)
    draw_toast();
    draw_building();
    port_stats_overlay_draw(overlay_open, x, y, width, height); // the extended stats mod (port/overlay/stats_panel.cpp)
    profiler_draw(overlay_open);                                // Shift+P (port/overlay/profiler.cpp)
    if (overlay_open) {
        menu_bar();
        if (show_settings)
            settings_window();
        if (show_about)
            about_window();
    }
#ifdef KF4_HAVE_VULKAN_PRESENTER
    // (SDL_Renderer's window draws the indicator itself, port/platform/video.c)
    if (renderer == nullptr)
        port_controls_draw(fill_rect, ImGui::GetBackgroundDrawList(), x, y, width, height);
#else
    (void)x, (void)y, (void)width, (void)height;
#endif
    ImGui::Render();
    if (renderer == nullptr) {
        drawn = true; // the presenter records it, and takes the test picture
        return;
    }
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    shot_take();
}
