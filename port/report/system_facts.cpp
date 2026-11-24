// What a problem report says about this PC and this run (system_facts.hpp)
#include "system_facts.hpp"

#include "pc_facts.hpp"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

extern "C" {
#include "port/platform.h"
#include "port/present_vk.h"
#include "port/settings.h"
#include "port/version.h"
}

#if __has_include("build_rev.h")
#include "build_rev.h" // KF4_BUILD_REV (tools/build_rev.cmake)
#endif
#ifndef KF4_BUILD_REV
#define KF4_BUILD_REV "unknown"
#endif

namespace report {
namespace {

std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    return line;
}

std::string build_facts() {
    std::string out = "Holyland: ";
    out += port_version[0] != 0 ? port_version : "a developer's build";
    out += format(" (%s)\n", KF4_BUILD_REV);
#ifdef KF4_PLAYER_BUILD
    out += "Build: a player's build";
#else
    out += "Build: a developer's build";
#endif
    out += port_build_optimized ? ", optimized" : ", NOT optimized (a debug build: slow)";
#if defined(__clang__)
    out += format("; clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    out += format("; gcc %s\n", __VERSION__);
#else
    out += "\n";
#endif
    const int sdl = SDL_GetVersion();
    out += format("SDL: %d.%d.%d (%s)\n", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl), SDL_VERSIONNUM_MICRO(sdl),
                  SDL_GetRevision());
    return out;
}

std::string gpu_facts() {
    char text[1024];
    port_vk_describe(text, sizeof(text));
    return text;
}

std::string display_facts() {
    std::string out;
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    for (int k = 0; displays != nullptr && k < count; k++) {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(displays[k]);
        const char *name = SDL_GetDisplayName(displays[k]);
        out += format("Display %d: %s", k + 1, name != nullptr ? name : "?");
        if (mode != nullptr) {
            out += format(", %d x %d at %.2f Hz, pixel density %.2f", mode->w, mode->h, double(mode->refresh_rate),
                          double(mode->pixel_density));
        }
        out += format(", scale %.2f%s\n", double(SDL_GetDisplayContentScale(displays[k])),
                      displays[k] == SDL_GetPrimaryDisplay() ? " (primary)" : "");
    }
    SDL_free(displays);
    return out.empty() ? "Displays: none found\n" : out;
}

std::string window_facts() {
    std::string out;
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    for (int k = 0; windows != nullptr && k < count; k++) {
        int w = 0, h = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(windows[k], &w, &h);
        SDL_GetWindowSizeInPixels(windows[k], &pw, &ph);
        const SDL_WindowFlags flags = SDL_GetWindowFlags(windows[k]);
        const SDL_DisplayMode *exclusive = SDL_GetWindowFullscreenMode(windows[k]);
        const char *display = SDL_GetDisplayName(SDL_GetDisplayForWindow(windows[k]));
        out += format("Window: %d x %d (%d x %d pixels), on %s, %s%s\n", w, h, pw, ph, display ? display : "?",
                      (flags & SDL_WINDOW_FULLSCREEN) ? (exclusive != nullptr ? "exclusive fullscreen" : "fullscreen")
                                                      : "windowed",
                      (flags & SDL_WINDOW_MAXIMIZED) ? ", maximized" : "");
    }
    SDL_free(windows);
    const char *video = SDL_GetCurrentVideoDriver();
    out += format("Video driver: %s\n", video != nullptr ? video : "none");
    return out;
}

std::string sound_facts() {
    const char *driver = SDL_GetCurrentAudioDriver();
    std::string out = format("Sound driver: %s", driver != nullptr ? driver : "none");
    SDL_AudioSpec spec{};
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames)) {
        out += format("; default output %d Hz, %d channels, %d frames a buffer", spec.freq, spec.channels, frames);
    }
    return out + "\n";
}

std::string controller_facts() {
    std::string out;
    int count = 0;
    SDL_JoystickID *pads = SDL_GetGamepads(&count);
    for (int k = 0; pads != nullptr && k < count; k++) {
        const char *name = SDL_GetGamepadNameForID(pads[k]);
        out += format("Controller %d: %s (%s, %04x:%04x)\n", k + 1, name != nullptr ? name : "?",
                      SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(pads[k])),
                      unsigned(SDL_GetJoystickVendorForID(pads[k])), unsigned(SDL_GetJoystickProductForID(pads[k])));
    }
    SDL_free(pads);
    return out.empty() ? "Controllers: none\n" : out;
}

const char *renderer_name(int renderer) {
    switch (renderer) {
    case PORT_RENDERER_SCENE:
        return "scene";
    case PORT_RENDERER_PC:
        return "pc";
    case PORT_RENDERER_GS:
        return "gs (paraLLEl-GS)";
    }
    return "?";
}

std::string frame_rate_name(int rate, int cap) {
    if (rate == PORT_FRAME_RATE_UNLOCKED)
        return cap > 0 ? format("unlocked, at most %d a second", cap) : "unlocked";
    if (rate == PORT_FRAME_RATE_MONITOR)
        return "the screen's refresh";
    return format("%d a second", rate);
}

} // namespace

std::string system_facts() {
    return build_facts() + os_facts() + cpu_facts() + memory_facts() + process_memory_facts("The game") + gpu_facts() +
           display_facts() + window_facts() + sound_facts() + controller_facts() + disk_facts(port_data_dir());
}

std::string system_summary() {
    std::string out = build_facts() + os_facts() + cpu_facts();
    char gpu[1024];
    port_vk_describe(gpu, sizeof(gpu));
    const char *end = std::strchr(gpu, '\n');
    out.append(gpu, end != nullptr ? size_t(end - gpu + 1) : std::strlen(gpu));
    out += format("Renderer: %s; frame rate %s\n", renderer_name(port_options.renderer),
                  frame_rate_name(port_options.frame_rate, port_options.frame_rate_cap).c_str());
    return out;
}

std::string run_facts() {
    std::string out = format("Running for: %.0f s\nVblanks: %llu\n", port_wall_seconds(),
                             (unsigned long long)port_vblank_count());
    std::string disc = port_options.disc != nullptr ? port_options.disc : "?";
    const size_t slash = disc.find_last_of("/\\");
    out += "Disc: " + (slash == std::string::npos ? disc : disc.substr(slash + 1)) + "\n";
    out += format("Renderer: %s\n", renderer_name(port_options.renderer));
    out += format("Presenter: %s\n", port_options.presenter == PORT_PRESENTER_VULKAN ? "Vulkan" : "SDL");
    out += "Frame rate: " + frame_rate_name(port_options.frame_rate, port_options.frame_rate_cap) + "\n";
    out += format("Screen refresh seen: %d Hz\n", port_video_refresh_rate());
    out += format("Pad script: %s\n", port_options.input != nullptr ? "yes" : "no");
    return out;
}

} // namespace report
