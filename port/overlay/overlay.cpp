// The settings overlay (port/include/port/overlay.h): F1 opens a menu bar and the Settings window,
// drawn with Dear ImGui (third_party/imgui) on the window's SDL_Renderer over the game's picture. The
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
#include "imgui.h"

extern "C" {
#include "port/audio.h"
#include "port/controls.h"
#include "port/overlay.h"
#include "port/platform.h"
#include "port/savestate.h"
#include "port/settings.h"
}

namespace {

enum Page { PAGE_DISPLAY, PAGE_AUDIO, PAGE_CONTROLS, PAGE_SAVE_STATES, PAGE_DEBUG, PAGE_INTERFACE, PAGE_COUNT };
const char *const page_names[PAGE_COUNT] = {"Display", "Audio", "Controls", "Save states", "Debug", "Interface"};
const char *const page_ids[PAGE_COUNT] = {"display", "audio", "controls", "savestates", "debug", "interface"};

const ImVec4 colour_note(0.95f, 0.8f, 0.45f, 1.0f);
const ImVec4 colour_good(0.55f, 0.85f, 0.55f, 1.0f);

SDL_Window *window;
SDL_Renderer *renderer;
bool ready;
bool overlay_open;
bool show_settings = true;
bool show_about;
int page;
float applied_scale = 1.0f; // the layout's scale; the slider's value is taken when it is let go
std::string ini_path;
Uint32 toast_until;

// the Debug page's rates, sampled once a second
struct Rates {
    double wall;
    uint64_t vblanks;
    unsigned frames;
    unsigned pictures;
    PortStats stats;
};
Rates last_sample;
unsigned pictures;
double pictures_per_second, vblanks_per_second, frames_per_second;
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

void apply_style() {
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.94f;
    applied_scale = port_settings.ui_scale;
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
    // window heights: the common screen heights, and the PS2's 448 lines once and twice
    static const int heights[] = {448, 720, 896, 900, 1080, 1440, 2160};
    static const char *const notes[] = {" (PS2 lines)", "", " (PS2 lines x2)", "", "", "", ""};
    const int count = (int)(sizeof(heights) / sizeof(heights[0]));
    const bool wide = port_options.widescreen != 0;
    SDL_Rect usable = {0, 0, 0, 0};
    char label[64];
    int current = 0;

    SDL_GetDisplayUsableBounds(0, &usable);
    for (int i = 0; i < count; i++) {
        if (heights[i] <= s.window_height) {
            current = i;
        }
    }
    auto width_of = [&](int h) { return wide ? (h * 16 + 8) / 9 : (h * 4 + 2) / 3; };
    auto size_label = [&](int i) {
        snprintf(label, sizeof(label), "%d x %d%s", width_of(heights[i]), heights[i], notes[i]);
        return label;
    };
    ImGui::BeginDisabled(s.fullscreen != 0);
    char shown[64];
    if (heights[current] == s.window_height) {
        snprintf(shown, sizeof(shown), "%s", size_label(current));
    } else {
        snprintf(shown, sizeof(shown), "%d x %d", width_of(s.window_height), s.window_height);
    }
    if (ImGui::BeginCombo("Window size", shown)) {
        for (int i = 0; i < count; i++) {
            // sizes bigger than the screen are left out: full screen is for those
            if (i != current && usable.h > 0 && (heights[i] > usable.h || width_of(heights[i]) > usable.w)) {
                continue;
            }
            if (ImGui::Selectable(size_label(i), i == current)) {
                s.window_height = heights[i];
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
    bool smooth = s.smooth != 0;
    if (ImGui::Checkbox("Smooth scaling", &smooth)) {
        s.smooth = smooth;
        changed();
    }
    tooltip("On: the picture is scaled to the window with bilinear filtering.\nOff: sharp, blocky pixels.");
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
    };
    for (Switch &item : switches) {
        bool value = *item.option != 0;
        if (ImGui::Checkbox(item.label, &value)) {
            *item.setting = *item.option = value;
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
            *item.setting = *item.option = 0;
        changed();
    }
    ImGui::SameLine();
    if (ImGui::Button("All on: the PC's picture")) {
        for (Switch &item : switches)
            *item.setting = *item.option = 1;
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
void save_states_page() {
    PortSettings &s = port_settings;
    char save_key[64], load_key[64];

    port_controls_binding_label(port_controls_binding(port_controls_layout(), ACTION_QUICK_SAVE, 0), save_key,
                                sizeof(save_key));
    port_controls_binding_label(port_controls_binding(port_controls_layout(), ACTION_QUICK_LOAD, 0), load_key,
                                sizeof(load_key));
    ImGui::TextWrapped("A save state is the whole running game, written to a file and read back. %s saves to the "
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
        pictures_per_second = (pictures - last_sample.pictures) / span;
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
    last_sample.pictures = pictures;
    last_sample.stats = stats;
}

std::string bug_report() {
    char line[512];
    std::string text = "holyland (King's Field IV PC port)\n";

    std::snprintf(line, sizeof(line), "vblank %llu, %.1f pictures/s, %.2f vblanks/s, %.1f game frames/s\n",
                  (unsigned long long)port_vblank_count(), pictures_per_second, vblanks_per_second, frames_per_second);
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

void debug_page() {
    PortGameReadout game;
    kf4_game_readout(&game);

    ImGui::SeparatorText("Speed");
    ImGui::Text("Pictures shown: %.1f a second", pictures_per_second);
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

    ImGui::SeparatorText("Game");
    ImGui::Text("Map: %02X in slot %d%s", game.map_tag & 0xFF, game.map_slot, game.map_loaded ? "" : " (not loaded)");
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

// ---- Interface -----------------------------------------------------------------------------------
void interface_page() {
    PortSettings &s = port_settings;

    bool pause = s.pause_when_open != 0;
    if (ImGui::Checkbox("Pause the game while the overlay is open", &pause)) {
        s.pause_when_open = pause;
        changed();
    }
    tooltip("Off: the game runs on behind the overlay, as it does now.");
    bool fps = s.show_fps != 0;
    if (ImGui::Checkbox("Show the frame rate", &fps)) {
        s.show_fps = fps;
        changed();
    }
    tooltip("Pictures shown a second, in the window's bottom left corner.");
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
    const char *text = port_state_message();
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
        if (shot.page < 0) {
            std::fprintf(stderr, "[overlay] KF4_OVERLAY_SHOT: no page called %s (display, audio, controls, "
                                 "savestates, debug, interface, closed)\n",
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
    if (shot.page == PAGE_COUNT) {
        overlay_open = false;
    } else {
        overlay_open = true;
        show_settings = true;
        page = shot.page;
    }
}

void shot_take() {
    if (shot_next >= shots.size())
        return;
    const Shot &shot = shots[shot_next];
    if (port_vblank_count() < shot.vblank + SHOT_SETTLE)
        return;
    int width = 0, height = 0;
    SDL_GetRendererOutputSize(renderer, &width, &height);
    std::vector<uint32_t> pixels((size_t)width * height);
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ABGR8888, pixels.data(), width * 4) != 0) {
        std::fprintf(stderr, "[overlay] cannot read the window back: %s\n", SDL_GetError());
    } else {
        char path[4096];
        std::snprintf(path, sizeof(path), "%s/overlay_%s.png", port_options.dump_dir ? port_options.dump_dir : ".",
                      shot.page == PAGE_COUNT ? "closed" : page_ids[shot.page]);
        if (port_png_write(path, pixels.data(), (uint32_t)width, (uint32_t)height))
            std::fprintf(stderr, "[overlay] vblank %llu: %dx%d, %s\n", (unsigned long long)port_vblank_count(), width,
                         height, path);
    }
    shot_next++;
    // the last picture taken, settings.json is written back at exit, so a test sees it round-trip
    if (shot_next == shots.size())
        port_settings_changed();
}

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
    apply_style();
    if (!ImGui_ImplSDL2_InitForSDLRenderer(window, renderer) || !ImGui_ImplSDLRenderer2_Init(renderer)) {
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
    }
    if (event->type == SDL_KEYUP &&
        (event->key.keysym.scancode == SDL_SCANCODE_F1 || event->key.keysym.scancode == SDL_SCANCODE_F11))
        return 1;
    // closed, it takes nothing more (and ImGui's queue is only emptied by a frame drawn)
    if (!overlay_open)
        return 0;
    ImGui_ImplSDL2_ProcessEvent(event);
    return is_input_event(event->type);
}

extern "C" int port_overlay_visible(void) {
    return ready && (overlay_open || port_settings.show_fps || shot_pending() ||
                     (toast_until != 0 && (int32_t)(toast_until - SDL_GetTicks()) > 0));
}

extern "C" int port_overlay_open(void) {
    return ready && overlay_open;
}

extern "C" int port_overlay_paused(void) {
    return ready && overlay_open && port_settings.pause_when_open && !shot_mode;
}

extern "C" void port_overlay_notify_state(void) {
    toast_until = SDL_GetTicks() + 2500;
}

extern "C" void port_overlay_draw(void) {
    if (!ready)
        return;
    pictures++;
    sample_rates();
    port_settings_poll();
    shot_open();
    if (!port_overlay_visible())
        return;
    if (port_settings.ui_scale != applied_scale)
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

    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (pad_quiet && !listening) {
        bool held = false;
        for (int key = ImGuiKey_GamepadStart; key <= ImGuiKey_GamepadRStickDown; key++)
            held |= ImGui::IsKeyDown((ImGuiKey)key);
        pad_quiet = held;
    }
    if (port_settings.show_fps) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f fps", pictures_per_second);
        // the bottom left corner, clear of the game's HUD and the menu bar
        ImGui::GetForegroundDrawList()->AddText(ImVec2(8, ImGui::GetMainViewport()->Size.y - ImGui::GetFontSize() - 6),
                                                IM_COL32(255, 255, 120, 255), text);
    }
    draw_toast();
    if (overlay_open) {
        menu_bar();
        if (show_settings)
            settings_window();
        if (show_about)
            about_window();
    }
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
    shot_take();
}
