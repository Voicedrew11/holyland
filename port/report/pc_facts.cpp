// What a report says about this PC (pc_facts.hpp)
#include "pc_facts.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <thread>

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
#else
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

long long kilobytes_after(const char *path, std::string_view key) {
    return std::atoll(line_after(path, key).c_str());
}
#endif

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

const char *instruction_sets() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f"))
        return "AVX2, AVX-512";
    if (__builtin_cpu_supports("avx2"))
        return "AVX2";
    if (__builtin_cpu_supports("avx"))
        return "AVX, no AVX2";
    return __builtin_cpu_supports("sse4.2") ? "SSE4.2, no AVX" : "no SSE4.2";
#else
    return "not x86";
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

std::string cpu_facts() {
    return format("CPU: %s (%u threads; %s)\n", cpu_name().c_str(), std::thread::hardware_concurrency(),
                  instruction_sets());
}

std::string memory_facts() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
        return "Memory: ?\n";
    return format("Memory: %llu MB, %llu MB free\n", (unsigned long long)(status.ullTotalPhys >> 20),
                  (unsigned long long)(status.ullAvailPhys >> 20));
#else
    return format("Memory: %lld MB, %lld MB free\n", kilobytes_after("/proc/meminfo", "MemTotal:") / 1024,
                  kilobytes_after("/proc/meminfo", "MemAvailable:") / 1024);
#endif
}

std::string process_memory_facts(const char *what) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return {};
    return format("%s: %llu MB now, %llu MB at most\n", what, (unsigned long long)(counters.WorkingSetSize >> 20),
                  (unsigned long long)(counters.PeakWorkingSetSize >> 20));
#else
    return format("%s: %lld MB now, %lld MB at most\n", what, kilobytes_after("/proc/self/status", "VmRSS:") / 1024,
                  kilobytes_after("/proc/self/status", "VmHWM:") / 1024);
#endif
}

std::string disk_facts(const std::filesystem::path &folder) {
    std::error_code error;
    const std::filesystem::space_info space = std::filesystem::space(folder, error);
    if (error)
        return "Free space in the data folder: ?\n";
    return format("Free space in the data folder: %llu MB\n", (unsigned long long)(space.available >> 20));
}

std::string this_command_line() {
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

std::string environment_facts(const std::string &command_line) {
    std::string out = "Command line: " + command_line + "\n\nVariables:\n";
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
