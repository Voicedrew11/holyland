// One run of the launcher (docs/port/LAUNCHER.md, "What it does"): the disc, a build when the release
// has none yet, then the game. The window is opened only when the player has something to do or see.
// Main thread, except the build's thread (BuildRun in session.cpp).
#pragma once

#include <string>
#include <vector>

namespace launcher {

struct LaunchOptions {
    bool show_launcher = false;          // --launcher: the launcher's window instead of going straight in
    bool build_only = false;             // --build-only: build this release's game if needed, no window, no disc
    bool progress_lines = false;         // --prepare: as --build-only, with "progress 0.40 Building the game" lines
                                         // on stdout (RecompLauncher's setup step, recomplauncher.json)
    std::string disc;                    // --disc PATH, or a path given alone
    std::vector<std::string> game_args;  // after "--": passed to the game
    std::vector<std::string> args;       // every argument as given: the new launcher's, after an update
};

// the launcher's exit code: the game's, or 0 when the player closed the launcher, or 1 on an error
int run_session(const LaunchOptions &options);

// "0.2.0", or "" in a developer's build
const char *launcher_version();

} // namespace launcher
