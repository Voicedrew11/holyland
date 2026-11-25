// Holyland, the launcher (launcher/product.h; docs/port/LAUNCHER.md). Builds King's Field IV's PC port on the player's computer
// from the release's source and tools, then plays it.
//
//   holyland [--launcher | --build-only | --prepare] [--disc PATH | PATH] [-- game options...]

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "product.h"
#include "session.h"

namespace {

void usage() {
    std::printf("%s [--launcher | --build-only | --prepare] [--disc PATH | PATH] [-- game options...]\n"
                "  --launcher    show the launcher (disc, rebuild, update checks) instead of starting the game\n"
                "  --disc PATH   the disc to play: an .iso or .chd, or an extracted disc's folder\n"
                "  --build-only  build the game for this release if it is not built, with no window (tests, CI)\n"
                "  --prepare     --build-only, with its progress as \"progress 0.40 TEXT\" lines (RecompLauncher's setup)\n"
                "  --version     print the version\n"
                "  -- ...        everything after is passed to the game (kf4 --help lists it)\n",
                launcher::kProductId);
}

} // namespace

// the launcher, given its arguments as UTF-8
static int launcher_main(int argc, char **argv) {
    launcher::LaunchOptions options;
    options.args.assign(argv + 1, argv + argc);
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--") == 0) {
            options.game_args.assign(argv + i + 1, argv + argc);
            break;
        }
        if (std::strcmp(argv[i], "--launcher") == 0) {
            options.show_launcher = true;
        } else if (std::strcmp(argv[i], "--build-only") == 0) {
            options.build_only = true;
        } else if (std::strcmp(argv[i], "--prepare") == 0) {
            options.build_only = true;
            options.progress_lines = true;
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            options.disc = argv[++i];
        } else if (std::strcmp(argv[i], "--version") == 0) {
            const char *version = launcher::launcher_version();
            std::printf("%s %s\n", launcher::kProductName, version[0] ? version : "(developer's build)");
            return 0;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] != '-' && options.disc.empty()) {
            options.disc = argv[i];
        } else {
            std::fprintf(stderr, "%s: unknown option %s\n", launcher::kProductId, argv[i]);
            usage();
            return 2;
        }
    }
    return launcher::run_session(options);
}

namespace {

#ifdef _WIN32
std::wstring widen(const std::string &text) {
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(n > 1 ? (std::size_t)n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), n);
    return out;
}

// the command line as UTF-8, whatever the code page (__argv is in the ANSI one); empty if Windows does not say
std::vector<std::string> utf8_arguments() {
    std::vector<std::string> args;
    int count = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr)
        return args;
    for (int i = 0; i < count; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(n > 1 ? (std::size_t)n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), n, nullptr, nullptr);
        args.push_back(std::move(text));
    }
    LocalFree(wide);
    return args;
}

#endif

// a last word instead of libc++'s bare "terminating" (a window program has no console to show it)
int report_stop(const char *why) {
    std::fprintf(stderr, "[%s] stopped: %s\n", launcher::kProductId, why);
#ifdef _WIN32
    std::wstring text = widen(std::string(launcher::kProductName) + " stopped: " + why);
    std::wstring title = widen(launcher::kProductName);
    MessageBoxW(nullptr, text.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
#endif
    return 1;
}

int guarded_main(int argc, char **argv) {
    try {
        return launcher_main(argc, argv);
    } catch (const std::exception &error) {
        return report_stop(error.what());
    }
}

} // namespace

int main(int argc, char **argv) {
    return guarded_main(argc, argv);
}

#ifdef _WIN32
// a window program (no console of its own): Windows starts it here
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::vector<std::string> args = utf8_arguments();
    if (args.empty())
        return guarded_main(__argc, __argv);
    std::vector<char *> argv;
    for (std::string &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    return guarded_main((int)args.size(), argv.data());
}
#endif
