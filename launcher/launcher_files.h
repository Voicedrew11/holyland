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
    fs::path game_prev_log() const { return data / "game.prev.log"; } // (the run before)
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

// a file's SHA-256 in hex; "" when it cannot be read
std::string file_sha256(const fs::path &file);

// `from` renamed to `to` in one step, replacing a `to` that is there. On Windows tried again for up to a second while
// another program has either file open (a virus scanner opening a file the moment it is written, a cloud folder's
// sync): the open lasts milliseconds, and without the retry an update or a write fails outright. As the game's
// port_replace_file (port/include/port/file_replace.h), with UTF-16 paths.
bool move_replacing(const fs::path &from, const fs::path &to);

// a file written whole and then put in place, so a crash never leaves half of it
bool write_file_atomically(const fs::path &file, const std::string &text);

// `dir` and everything in it removed, read-only files and folders made writable first (the game's build makes its copy
// of the decomp read-only: CMakeLists.txt, decomp_src/); "" when nothing of it is left, else why not (on Windows, a
// file another program has open, such as an antivirus scanning a new kf4.exe, cannot be deleted until it is closed)
std::string remove_tree(const fs::path &dir);

} // namespace launcher
