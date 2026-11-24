// What a problem report says about this PC and this run (system_facts.hpp)
#include "system_facts.hpp"

#include <SDL3/SDL.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
extern char **environ;
#endif

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

std::string env_or(const char *name, const char *otherwise) {
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != 0 ? value : otherwise;
}

#ifndef _WIN32
// The first line of a file that starts with `key`, after it; "" when none
std::string line_after(const char *path, std::string_view key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, key.size(), key) == 0)
            return line.substr(key.size());
    }
    return {};
}

std::string unquoted(std::string text) {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        return text.substr(1, text.size() - 2);
    return text;
}
#endif

#ifdef _WIN32
std::string narrow(const wchar_t *text) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string out(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    out.resize(size_t(bytes - 1));
    return out;
}
#endif

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

std::string os_facts() {
#ifdef _WIN32
    std::string out = "System: Windows";
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto get_version = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (get_version != nullptr && get_version(&info) == 0)
        out += format(" %lu.%lu build %lu", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
    using WineVersionFn = const char *(CDECL *)(void);
    auto wine = ntdll ? reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
    if (wine != nullptr)
        out += format(" (Wine %s)", wine());
    return out + "\n";
#else
    std::string out = "System: " + unquoted(line_after("/etc/os-release", "PRETTY_NAME="));
    utsname name{};
    if (uname(&name) == 0)
        out += format(" (%s %s %s)", name.sysname, name.release, name.machine);
    out += "\nSession: " + env_or("XDG_SESSION_TYPE", "?") + ", desktop " + env_or("XDG_CURRENT_DESKTOP", "?");
    if (std::getenv("GAMESCOPE_WAYLAND_DISPLAY") != nullptr)
        out += ", gamescope";
    if (std::getenv("APPIMAGE") != nullptr)
        out += ", from the AppImage";
    if (std::getenv("FLATPAK_ID") != nullptr)
        out += ", in a Flatpak";
    return out + "\n";
#endif
}

std::string cpu_name() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int regs[12] = {};
    unsigned int top = __get_cpuid_max(0x80000000u, nullptr);
    if (top >= 0x80000004u) {
        for (unsigned int k = 0; k < 3; k++)
            __get_cpuid(0x80000002u + k, &regs[k * 4], &regs[k * 4 + 1], &regs[k * 4 + 2], &regs[k * 4 + 3]);
        std::string name(reinterpret_cast<const char *>(regs), sizeof(regs));
        name.resize(std::strlen(name.c_str()));
        const size_t first = name.find_first_not_of(' ');
        if (first == std::string::npos)
            return "?";
        return name.substr(first, name.find_last_not_of(' ') - first + 1);
    }
#endif
    return "?";
}

std::string cpu_facts() {
    std::string out = "CPU: " + cpu_name();
    out += format(" (%d threads; ", SDL_GetNumLogicalCPUCores());
    out += SDL_HasAVX2() ? "AVX2" : SDL_HasAVX() ? "AVX, no AVX2" : SDL_HasSSE42() ? "SSE4.2, no AVX" : "no SSE4.2";
    out += SDL_HasAVX512F() ? ", AVX-512)\n" : ")\n";
    return out;
}

std::string memory_facts() {
    std::string out = format("Memory: %d MB", SDL_GetSystemRAM());
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        out += format(", %llu MB free", (unsigned long long)(status.ullAvailPhys >> 20));
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        out += format("; the game: %llu MB now, %llu MB at most", (unsigned long long)(counters.WorkingSetSize >> 20),
                      (unsigned long long)(counters.PeakWorkingSetSize >> 20));
    }
#else
    const std::string available = line_after("/proc/meminfo", "MemAvailable:");
    if (!available.empty())
        out += format(", %lld MB free", std::atoll(available.c_str()) / 1024);
    const std::string now = line_after("/proc/self/status", "VmRSS:");
    const std::string most = line_after("/proc/self/status", "VmHWM:");
    if (!now.empty())
        out += format("; the game: %lld MB now, %lld MB at most", std::atoll(now.c_str()) / 1024,
                      std::atoll(most.c_str()) / 1024);
#endif
    return out + "\n";
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

std::string disk_facts() {
    std::error_code error;
    const std::filesystem::space_info space = std::filesystem::space(port_data_dir(), error);
    if (error)
        return "Free space in the data folder: ?\n";
    return format("Free space in the data folder: %llu MB\n", (unsigned long long)(space.available >> 20));
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

std::string command_line() {
#ifdef _WIN32
    return narrow(GetCommandLineW());
#else
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string line, arg;
    while (std::getline(in, arg, '\0')) {
        if (!line.empty())
            line += ' ';
        line += arg.find(' ') != std::string::npos ? "\"" + arg + "\"" : arg;
    }
    return line;
#endif
}

// The variables that change how the game, SDL, the Vulkan loader or a GPU driver run
bool telling_variable(std::string_view name) {
    static constexpr std::string_view prefixes[] = {
        "KF4_",   "SDL_",   "VK_",          "RADV_",     "MESA_",   "AMD_",   "ACO_",   "NV_",      "__GL_",
        "DXVK_",  "VKD3D_", "WINE",         "PROTON_",   "ENABLE_", "DISABLE_", "LD_PRELOAD", "XDG_SESSION_TYPE",
        "XDG_CURRENT_DESKTOP", "WAYLAND_DISPLAY", "DISPLAY", "GAMESCOPE", "STEAM_COMPAT", "APPIMAGE", "LANG",
        "LC_ALL", "DRI_PRIME", "__NV_", "__VK_", "MANGOHUD", "OBS_VKCAPTURE"};
    for (std::string_view prefix : prefixes) {
        if (name.compare(0, prefix.size(), prefix) == 0)
            return true;
    }
    return false;
}

// A computer's name the system gave it, which says nothing of who has it ("fedora", "steamdeck"), and would take a
// system's own name out of the report
bool default_computer_name(const std::string &host) {
    static constexpr std::string_view names[] = {"localhost", "steamdeck", "localhost.localdomain"};
    std::string lower;
    for (char c : host)
        lower += char(std::tolower(static_cast<unsigned char>(c)));
    for (std::string_view name : names) {
        if (lower == name)
            return true;
    }
#ifndef _WIN32
    return lower == unquoted(line_after("/etc/os-release", "ID=")); // (Fedora's "fedora", Ubuntu's "ubuntu")
#else
    return false;
#endif
}

} // namespace

std::string system_facts() {
    return build_facts() + os_facts() + cpu_facts() + memory_facts() + gpu_facts() + display_facts() + window_facts() +
           sound_facts() + controller_facts() + disk_facts();
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

std::string environment_facts() {
    std::string out = "Command line: " + command_line() + "\n\nVariables:\n";
#ifdef _WIN32
    wchar_t *block = GetEnvironmentStringsW();
    for (const wchar_t *at = block; at != nullptr && *at != 0; at += wcslen(at) + 1) {
        std::string entry = narrow(at);
        if (telling_variable(entry))
            out += entry + "\n";
    }
    if (block != nullptr)
        FreeEnvironmentStringsW(block);
#else
    for (char **at = environ; at != nullptr && *at != nullptr; at++) {
        if (telling_variable(*at))
            out += std::string(*at) + "\n";
    }
#endif
    return out;
}

std::vector<Redaction> private_words() {
    std::vector<Redaction> words;
#ifdef _WIN32
    const std::string home = env_or("USERPROFILE", "");
    const std::string user = env_or("USERNAME", "");
    char computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(computer);
    const std::string host = GetComputerNameA(computer, &size) ? computer : "";
    if (!home.empty()) { // (Windows writes it with either slash)
        std::string forward = home;
        for (char &c : forward)
            c = c == '\\' ? '/' : c;
        words.push_back({forward, "~", false});
    }
#else
    const std::string home = env_or("HOME", "");
    const std::string user = env_or("USER", env_or("LOGNAME", "").c_str());
    char computer[256] = {};
    const std::string host = gethostname(computer, sizeof(computer) - 1) == 0 ? computer : "";
#endif
    if (home.size() > 1)
        words.push_back({home, "~", false});
    if (!user.empty())
        words.push_back({user, "<user>", true});
    if (!host.empty() && !default_computer_name(host))
        words.push_back({host, "<computer>", true});
    return words;
}

} // namespace report
