// The layer over the game's picture (port/include/port/overlay.h): Dear ImGui (third_party/imgui) for what is still
// drawn with it, the automap mod's full map and minimap and the extended stats mod's lines and panel, on the window's
// SDL_Renderer or by the Vulkan presenter (port/platform/present_vk.cpp, --presenter vulkan) with ImGui's Vulkan
// backend. Events go to the player menu first (port/include/port/player_ui.h, RmlUi), which also shows the notices.
// The developers' ImGui window (Shift+F1) is gone: its teleport is the player menu's Cheats tab.
//
// Everything here runs on the game's thread, at the vblank (port_pad_poll's events, port_video_present's drawing).
//
// KF4_OVERLAY_SHOT=what@vblank,... writes the window's whole picture at that vblank (a few vblanks later, once laid
// out), even with a pad script, into --dump-dir: pictures without anyone at the screen. ui-<tab>[:keys] opens the
// player menu on that tab and presses those keys once (ui-display:down.down.confirm), written as
// overlay_ui-<tab>[:keys].png; closed, the menu closed (overlay_closed.png); map, mini, menu and profiler the full map,
// the minimap, the game's menu with the extended stats and the profiler, one a vblank (overlay_<what>_<vblank>.png).
#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#ifdef KF4_HAVE_VULKAN_PRESENTER
#include "backends/imgui_impl_vulkan.h"
#endif
#include "imgui.h"

#include "automap_view.h" // the automap mod
#include "update_badge.h" // a new release, found by the launcher

extern "C" {
#include "port/automap.h"
#include "port/overlay.h"
#include "port/player_ui.h"
#include "port/platform.h"
#include "port/present_vk.h"
#include "port/settings.h"
#include "port/stats.h"
}

namespace {

SDL_Window *window;
SDL_Renderer *renderer; // NULL on the Vulkan presenter
bool ready;
bool drawn; // the Vulkan presenter: this vblank's draw lists are ImGui's draw data
float applied_ui_scale = 1.0f; // settings.json's overlay scale, as the style was last made with it
std::string ini_path;

// KF4_OVERLAY_SHOT: what a picture shows
enum class ShotKind { ui, closed, map, mini, menu, profiler };

const struct {
    const char *name;
    ShotKind kind;
} shot_names[] = {{"closed", ShotKind::closed}, {"map", ShotKind::map},   {"mini", ShotKind::mini},
                  {"menu", ShotKind::menu},     {"profiler", ShotKind::profiler}};

struct Shot {
    ShotKind kind;
    uint64_t vblank;
    std::string name;   // in the picture's file name
    std::string ui_tab; // ShotKind::ui: the player menu open on that tab
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
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
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
    const float scale = applied_ui_scale * screen_scale();
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
}

// ---- KF4_OVERLAY_SHOT ----------------------------------------------------------------------------
// One item, name@vblank, added to `shots`; a name that is no picture is reported and left out
void parse_shot(const std::string &item) {
    size_t sign = item.find('@');
    if (sign == std::string::npos)
        return;
    std::string name = item.substr(0, sign);
    Shot shot = {ShotKind::ui, std::strtoull(item.c_str() + sign + 1, nullptr, 0), name, ""};
    if (name.rfind("ui-", 0) == 0) { // the player menu on a tab
        shot.ui_tab = name.substr(3);
        shots.push_back(shot);
        return;
    }
    for (const auto &known : shot_names) {
        if (name == known.name) {
            shot.kind = known.kind;
            shots.push_back(shot);
            return;
        }
    }
    std::fprintf(stderr, "[overlay] KF4_OVERLAY_SHOT: no picture called %s (closed, map, mini, menu, profiler, "
                         "ui-<tab>)\n",
                 name.c_str());
}

void parse_shots(const char *text) {
    std::string spec = text;
    size_t at = 0;

    while (at < spec.size()) {
        size_t end = spec.find(',', at);
        parse_shot(spec.substr(at, end == std::string::npos ? std::string::npos : end - at));
        at = end == std::string::npos ? spec.size() : end + 1;
    }
    shot_mode = !shots.empty();
}

// Sets the picture up at its vblank; it is taken 4 vblanks on, once laid out
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
    automap_view_open_for_shot(shot.kind == ShotKind::map);
    port_player_ui_profiler_show(shot.kind == ShotKind::profiler);
}

bool shot_due() {
    return ready && shot_next < shots.size() && port_vblank_count() >= shots[shot_next].vblank + SHOT_SETTLE;
}

// The window's picture (NULL: it could not be read back) as the next test picture
void shot_write(const uint32_t *pixels, int width, int height) {
    const Shot &shot = shots[shot_next];
    if (pixels != nullptr) {
        const char *dir = port_options.dump_dir ? port_options.dump_dir : ".";
        char path[4096];
        if (shot.kind == ShotKind::ui || shot.kind == ShotKind::closed) // one picture each
            std::snprintf(path, sizeof(path), "%s/overlay_%s.png", dir, shot.name.c_str());
        else // the map's, the menu's and the profiler's: one a vblank
            std::snprintf(path, sizeof(path), "%s/overlay_%s_%llu.png", dir, shot.name.c_str(),
                          (unsigned long long)shot.vblank);
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
    // window places (the extended stats panel's) are kept beside settings.json; not for test pictures, which start
    // the same each time
    ini_path = std::string(port_data_dir()) + "/overlay.ini";
    io.IniFilename = shot_mode ? nullptr : ini_path.c_str();
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
        std::fprintf(stderr, "[overlay] Dear ImGui did not start: no automap or extended stats drawn\n");
        return;
    }
    ready = true;
}

extern "C" int port_overlay_event(const void *sdl_event) {
    const SDL_Event *event = static_cast<const SDL_Event *>(sdl_event);

    // the player menu first (F1 and its keys)
    if (port_player_ui_event(event))
        return 1;
    if (!ready)
        return 0;
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat && event->key.scancode == SDL_SCANCODE_F11) {
        port_video_set_fullscreen(!port_video_fullscreen());
        port_settings_changed();
        return 1;
    }
    if (event->type == SDL_EVENT_KEY_UP && event->key.scancode == SDL_SCANCODE_F11)
        return 1;
    // the automap mod's full map: its key closes it, and it has the mouse while it is open
    if (automap_view_event(event))
        return 1;
    if (port_automap_full_open()) {
        ImGui_ImplSDL3_ProcessEvent(event);
        return is_input_event(event->type);
    }
    // nothing else takes input (and ImGui's queue is only emptied by a frame drawn)
    return 0;
}

extern "C" int port_overlay_visible(void) {
    return port_player_ui_visible() ||
           (ready && (shot_pending() || port_stats_overlay_wants() || automap_view_visible()));
}

extern "C" int port_overlay_each_picture(void) {
    return port_player_ui_visible() || (ready && shot_pending());
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
    return port_player_ui_open();
}

extern "C" int port_overlay_holds_input(void) {
    // The layers over the game, top first. The player menu (F1) is over everything, the full map and the extended
    // stats included (the Vulkan presenter draws it after ImGui), and has the input while it is open; under it the
    // map keeps its own state and has the input back when the menu closes.
    if (port_player_ui_open())
        return 1;
    if (!ready)
        return 0; // (no ImGui: the full map is not drawn, and its key alone closes it, through the game's input)
    return port_automap_full_open();
}

extern "C" int port_overlay_paused(void) {
    // (the first-start setup holds the game still whatever the setting: it opens over the game's start)
    bool pause = port_settings.pause_when_open || port_player_ui_setup_open();
    return ready && !shot_mode && ((port_player_ui_open() && pause) || port_automap_holds_game());
}

extern "C" void port_overlay_vblank(void) {
    if (!ready)
        return;
    port_stats_overlay_update(); // the extended stats mod (port/overlay/stats_panel.cpp)
}

extern "C" void port_overlay_draw(int x, int y, int width, int height) {
    drawn = false;
    if (!ready)
        return;
    port_settings_poll();
    // a new release the launcher found: the title screen offers it in a pop-up (port_update_title)
    update_badge_poll((Uint32)SDL_GetTicks());
    shot_open();
    if (!port_overlay_visible())
        return;
    if (port_settings.ui_scale != applied_ui_scale)
        apply_style();

    if (renderer != nullptr) {
        ImGui_ImplSDLRenderer3_NewFrame();
    } else {
#ifdef KF4_HAVE_VULKAN_PRESENTER
        ImGui_ImplVulkan_NewFrame();
#endif
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    automap_view_draw(x, y, width, height);      // the automap mod
    port_stats_overlay_draw(x, y, width, height); // the extended stats mod (port/overlay/stats_panel.cpp)
    ImGui::Render();
    if (renderer == nullptr) {
        drawn = true; // the presenter records it, and takes the test picture
        return;
    }
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    shot_take();
}
