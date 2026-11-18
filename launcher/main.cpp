// Darker Reality, the launcher (launcher/product.h; docs/port/LAUNCHER.md). Builds King's Field IV's PC port on the player's computer
// from the release's source and tools, then plays it.
//
//   darker-reality [--launcher | --build-only] [--disc PATH | PATH] [-- game options...]
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "product.h"
#include "session.h"

namespace {

void usage() {
    std::printf("%s [--launcher | --build-only] [--disc PATH | PATH] [-- game options...]\n"
                "  --launcher    show the launcher (disc, rebuild, update checks) instead of starting the game\n"
                "  --disc PATH   the disc to play: an .iso or .chd, or an extracted disc's folder\n"
                "  --build-only  build the game for this release if it is not built, with no window (tests, CI)\n"
                "  --version     print the version\n"
                "  -- ...        everything after is passed to the game (kf4 --help lists it)\n",
                launcher::kProductId);
}

} // namespace

int main(int argc, char **argv) {
    SDL_SetMainReady();
    launcher::LaunchOptions options;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--") == 0) {
            options.game_args.assign(argv + i + 1, argv + argc);
            break;
        }
        if (std::strcmp(argv[i], "--launcher") == 0) {
            options.show_launcher = true;
        } else if (std::strcmp(argv[i], "--build-only") == 0) {
            options.build_only = true;
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

#ifdef _WIN32
// a window program (no console of its own): Windows starts it here
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif
