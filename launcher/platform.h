// What the launcher needs from the system it runs on (docs/port/LAUNCHER.md, "Platforms"). One
// implementation per system, in platform/<system>.cpp; CMake compiles exactly one. A new system (macOS,
// and later iOS or Android) is a new file here and a line in CMakeLists.txt, nothing else.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace launcher {

namespace fs = std::filesystem;
class BuildStrategy;

// The tools a release ships for building the game (content/toolchain/), as the build runs them
struct Toolchain {
    fs::path cmake;
    fs::path ninja;
    fs::path c_compiler;
    fs::path cxx_compiler;
    std::vector<std::string> cmake_options; // "-DNAME=VALUE" the system adds: sysroot, SDL3_DIR
    std::vector<std::pair<std::string, std::string>> env; // set for the build's programs (PATH)
    std::vector<fs::path> runtime_files;                  // put beside the built game (Linux: its SDL3)
    std::string id;                         // names these tools in the build key
};

// A program the launcher started: a build step, or the game
class ChildProcess {
public:
    virtual ~ChildProcess() = default;
    // the next line of its output (stdout and stderr together), without the line end; nullopt once it
    // has closed them. Only for a process started with capture_output.
    virtual std::optional<std::string> read_line() = 0;
    // ends it and everything it started (a build's compilers)
    virtual void stop() = 0;
    // waits for it to end; its exit code, or -1 if it could not be waited for
    virtual int wait() = 0;
};

struct ProcessSpec {
    std::vector<std::string> argv; // argv[0] is the program's path
    fs::path working_dir;
    std::vector<std::pair<std::string, std::string>> env; // set on top of the launcher's own
    bool capture_output = false;
    // stop() ends what it started too (a build's compilers). Off for the game, which stays in the
    // launcher's group so that Ctrl+C in a terminal reaches it.
    bool stop_with_children = true;
};

// How far a download is: the bytes so far and the whole (0 when not known). Returning false stops it. On the
// downloading thread.
using DownloadProgress = std::function<bool(std::uint64_t done_bytes, std::uint64_t total_bytes)>;

// How this copy of the launcher was installed, which says how an update replaces it (docs/port/LAUNCHER.md,
// "Updates"). One implementation per way of installing: an AppImage, Windows' installer, Windows' zip.
class Installation {
public:
    virtual ~Installation() = default;
    // the end of the name of the release's file for this way ("-x86_64.AppImage", "-win-x64-setup.exe")
    virtual std::string_view asset_suffix() const = 0;
    // where the download goes: on the same file system as what it replaces, so a rename puts it in place
    virtual fs::path download_path() const = 0;
    // the checked download put in place and the new launcher started with `args`; "" once it has started (this
    // one then exits at once), else why not, with the old install left as it was
    virtual std::string replace_and_start(const fs::path &downloaded, const std::vector<std::string> &args) const = 0;
    // what the last update left behind (the old files, a half download); at every start
    virtual void remove_leftovers() const = 0;
};

class Platform {
public:
    virtual ~Platform() = default;

    virtual std::string_view name() const = 0; // "linux", "windows"

    // the release's content/ folder (source, tools, manifest); KF4_CONTENT_DIR overrides it
    virtual fs::path content_dir() const = 0;

    // the shipped tools in `content`, or the ones on PATH when KF4_TOOLCHAIN=system (a developer's
    // run); nullopt, with the reason in `why`, when they are not there
    virtual std::optional<Toolchain> toolchain(const fs::path &content, std::string &why) const = 0;

    // the game's program name in a build folder: "kf4", "kf4.exe"
    virtual std::string_view game_program() const = 0;

    // the files a finished build keeps beside the game's program (Windows: its DLLs)
    virtual std::vector<fs::path> game_files(const fs::path &build_dir) const = 0;

    // how a playable game is made on this system
    virtual const BuildStrategy &build_strategy() const = 0;

    virtual std::unique_ptr<ChildProcess> start(const ProcessSpec &spec, std::string &why) const = 0;

    // a stopped game's exit code (ChildProcess::wait's, not 0) in words for its log: "code 0xC00000FD, a stack
    // overflow". The code alone when the system's table does not know it.
    virtual std::string describe_exit(int code) const = 0;

    // the system's file chooser for the disc (an .iso or an extracted disc's SYSTEM.CNF); nullopt when
    // cancelled or there is none (the launcher's typed path is then the way)
    virtual std::optional<fs::path> pick_disc() const = 0;

    // GET over HTTPS; nullopt on any failure (no network, an error status, the time running out)
    virtual std::optional<std::string> http_get(std::string_view url, std::string_view user_agent,
                                                int timeout_ms) const = 0;

    // GET over HTTPS into the file `to`, at most `max_bytes` (a test's file:// works on Linux); "" when it is all
    // there, else why not, with nothing left at `to`
    virtual std::string http_download(std::string_view url, const fs::path &to, std::uint64_t max_bytes,
                                      std::string_view user_agent, const DownloadProgress &progress) const = 0;

    // this copy's way of being installed; nullptr, with why in `why`, when it cannot update itself (a developer's
    // build, a folder it may not write)
    virtual std::unique_ptr<Installation> installation(std::string &why) const = 0;

    // Windows: print into the console the launcher was started from, if any
    virtual void attach_console() const {}
};

const Platform &this_platform();

// the environment's value, or "" (a shared helper for the platform files)
std::string env_or_empty(const char *name);

} // namespace launcher
