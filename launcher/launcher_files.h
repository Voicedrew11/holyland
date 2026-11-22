// The launcher's own files in the data folder (port/include/port/update_notice.h has their layout):
// launcher.json (the player's choices) and the paths of the rest.
#pragma once

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace launcher {

namespace fs = std::filesystem;

struct LauncherChoices {
    std::string disc;          // "" until chosen
    bool update_check = true;  // "Check for updates at launch"
    std::string skipped;       // the release the player chose to skip
    bool setup_done = false;   // the first start's setup (setup_flow.h) was gone through: its option pages are not shown again
};

struct DataPaths {
    fs::path data;    // settings.json, saves, launcher.json, update.json, logs
    fs::path builds;  // builds/<key>/
    fs::path choices() const { return data / "launcher.json"; }
    fs::path update_cache() const { return data / "update.json"; }
    fs::path update_request() const { return data / "update-request.json"; } // the game's ask (self_update.h)
    fs::path build_log() const { return data / "build.log"; }
    fs::path game_log() const { return data / "game.log"; }
};

// The same folders the game uses (port/platform/data_dir.c); nullopt if a path is too long
std::optional<DataPaths> data_paths();

// launcher.json; the defaults when missing or broken
LauncherChoices read_choices(const fs::path &file);

// launcher.json written whole and then put in place; keys this version does not know are kept
bool write_choices(const fs::path &file, const LauncherChoices &choices);

// fopen with a path in any script (Windows takes it as UTF-16); nullptr on failure
std::FILE *open_file(const fs::path &file, const char *mode);

// a small file whole, or nullopt
std::optional<std::string> read_file(const fs::path &file);

// a file written whole and then put in place, so a crash never leaves half of it
bool write_file_atomically(const fs::path &file, const std::string &text);

} // namespace launcher
