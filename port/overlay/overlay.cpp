// The developers' overlay (port/include/port/overlay.h): ` (the backtick) opens a menu bar and the Settings window,
// and so does F1 (Shift+F1 on the Vulkan presenter, where F1 is the player menu's: port/include/port/player_ui.h,
// which this passes events to first),
// drawn with Dear ImGui (third_party/imgui) over the game's picture, on the window's SDL_Renderer or by
// the Vulkan presenter (port/platform/present_vk.cpp, --presenter vulkan) with ImGui's Vulkan backend. The
// pages (the table `pages`): Developer, for testing, and General (the overlay's own size, the settings file). The
// settings players change are the player menu's (port/ui/), not this overlay's.
//
// While it is open the keyboard, the mouse and the controller go to it, not to the game; the game
// runs on unless the player menu's "Pause while open" is on (port/platform/kernel.c holds it).
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
#include "port/cinematic.h"
#include "port/controls.h"
#include "port/twin_stick.h"
#include "port/walk_keys.h"
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
    overlay_open = value; // (the game's input and the mouse follow: port_overlay_holds_input)
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

// The PS2 picture fixes the player menu has not (Graphics > PS2 video has the others): paraLLEl-GS's scanout's two,
// each on by default (Developer > Renderer, --renderer gs only)
void ps2_picture_section() {
    if (port_options.renderer != PORT_RENDERER_GS)
        return;
    PortSettings &s = port_settings;
    ImGui::TextUnformatted("paraLLEl-GS's scanout");
    struct Switch {
        const char *label;
        const char *tip;
        int *setting;
        int *option;
    } switches[] = {
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
}

void key_name(int action, char *out, size_t size) {
    port_controls_binding_label(port_controls_binding(action, 0), out, size);
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
                  "options: unpaced=%d game_speed=%.2f fov=%d deinterlace=%d progressive=%d anti_blur=%d adapt_width=%d "
                  "no_audio=%d device_clock=%d\n",
                  port_options.fast, (double)port_game_speed(), port_options.fov, port_options.deinterlace,
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

void developer_page() {
    ImGui::TextWrapped("For testing and bug reports.");
    speed_section();
    game_section();
    teleport_section();
    save_states_section();
    renderer_section();
    debug_tools();
}

// ---- General -------------------------------------------------------------------------------------
void overlay_section() {
    PortSettings &s = port_settings;

    ImGui::SeparatorText("Overlay");
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
    {"Developer", "developer", developer_page},
    {"General", "general", general_page},
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
// Pages' ids that are gone (November 2026's reorganizations), so older scripts still open one
const struct {
    const char *old_id;
    const char *id;
} page_aliases[] = {{"audio", "general"},    {"interface", "general"}, {"savestates", "developer"},
                    {"mods", "developer"},   {"debug", "developer"},   {"display", "general"},
                    {"graphics", "general"}, {"controls", "general"},  {"gameplay", "general"},
                    {"saves", "developer"}};

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
        // (F1 gets here only as Shift+F1, or with no player menu; ` is not typed into a text field of the overlay)
        if (key == SDL_SCANCODE_F1 || (key == SDL_SCANCODE_GRAVE && !ImGui::GetIO().WantTextInput) ||
            (overlay_open && key == SDL_SCANCODE_ESCAPE && !ImGui::GetIO().WantTextInput)) {
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
        (event->key.scancode == SDL_SCANCODE_F1 || event->key.scancode == SDL_SCANCODE_GRAVE ||
         event->key.scancode == SDL_SCANCODE_F11))
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

extern "C" int port_overlay_holds_input(void) {
    // The layers over the game, top first. The player menu (F1) is over everything, the full map and the extended
    // stats included (the Vulkan presenter draws it after ImGui), and has the input while it is open; under it the
    // map keeps its own state and has the input back when the menu closes.
    if (port_player_ui_open())
        return 1;
    if (!ready)
        return 0; // (no ImGui: the full map is not drawn, and its key alone closes it, through the game's input)
    return overlay_open || port_automap_full_open() || (profiler_visible() && imgui_typing.load());
}

extern "C" const char *port_overlay_key(void) {
    return port_player_ui_ready() ? "Shift+F1 or `" : "F1 or `";
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
    // (typing in the profiler's text field blocks the game's keys: port_overlay_holds_input)
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
