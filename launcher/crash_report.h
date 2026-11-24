// The crash report: the "game stopped" page's Report the problem (session.cpp). The game cannot report its own crash,
// so the launcher, which kept its log, makes the zip the game's Report an Issue would (port/report/,
// docs/port/PROBLEM_REPORTS.md) from what is left: <data>/reports/holyland-crash-<time>.zip, and the issue form's link,
// titled with how the game ended. No window here: tested in tests/launcher_test.cpp.
//
//   README.txt       what is in it
//   crash.txt        how the game ended, the launcher's version, the game's command line
//   system.txt       the PC (port/report/pc_facts.hpp); the GPU is in the log's first lines
//   environment.txt  the game's command line and the variables that change the game or the GPU's driver
//   settings.json    the settings file
//   logs/game.log    the run that stopped: its first lines and its end
//   logs/game.prev.log, logs/build.log   the run before it (from the last day), the end of the game's build
//   saves/           the save points' saves and the quick saves
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "launcher_files.h"

namespace launcher {

struct CrashFacts {
    std::string ended;            // how the game ended (Platform::describe_exit)
    std::string game_command_line;
    std::string launcher_version; // "" for a developer's build
};

struct CrashReport {
    std::filesystem::path zip;
    std::string url; // the issue form, filled in
};

// The zip written, and the form's link; nullopt when the zip could not be written
std::optional<CrashReport> make_crash_report(const DataPaths &paths, const CrashFacts &facts);

} // namespace launcher
