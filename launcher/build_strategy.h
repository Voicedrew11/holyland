// How a playable game is made on a system (docs/port/LAUNCHER.md, "Building"). The desktop systems build
// it from the release's source with its shipped tools (LocalCMakeBuild, build_local.cpp). A system that
// cannot run a compiler (iOS, Android) gets its own strategy later; nothing else in the launcher changes.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace launcher {

namespace fs = std::filesystem;
class Platform;
class ChildProcess;

struct BuildRequest {
    fs::path content;   // the release's content/
    fs::path build_dir; // builds/<key>/: empty or missing; the game goes here
    std::string version; // the release's ("0.2.0"); "" for a developer's
};

// What a build reports while it runs. Called from the build's thread.
class BuildProgress {
public:
    virtual ~BuildProgress() = default;
    virtual void stage(std::string_view what) = 0;  // "Preparing the build", "Compiling"
    virtual void steps(int done, int total) = 0;    // the compiler's steps, when known
    virtual void log_line(std::string_view line) = 0;
    virtual bool cancelled() const = 0;
    // the program the build is waiting for (nullptr between them), so that a cancel can stop it at once
    // from the main thread; it stays valid until the next call
    virtual void running(ChildProcess *process) = 0;
};

struct BuildResult {
    bool ok = false;
    std::string message; // why not, for the player
};

class BuildStrategy {
public:
    virtual ~BuildStrategy() = default;
    // runs on the build's thread (not the main one)
    virtual BuildResult build(const Platform &platform, const BuildRequest &request, BuildProgress &progress) const = 0;
};

// the desktop systems' strategy: CMake and Ninja with the shipped compilers
const BuildStrategy &local_cmake_build();

} // namespace launcher
