// The desktop systems' build (build_strategy.h): the release's source configured with CMake and built with
// Ninja, with the tools the release ships, as the developers' release preset builds it (CMakePresets.json
// "release": LTO, no frame pointers), then the game's files kept and the rest removed.
#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "build_log.h"
#include "build_strategy.h"
#include "launcher_files.h"
#include "platform.h"

namespace launcher {

namespace {

class LocalCMakeBuild final : public BuildStrategy {
public:
    BuildResult build(const Platform &platform, const BuildRequest &request, BuildProgress &progress) const override;
};

// runs one step, its output into the log; "" when it worked, else why not
std::string run_step(const Platform &platform, const ProcessSpec &spec, BuildProgress &progress) {
    std::string why;
    std::string command;
    for (const std::string &arg : spec.argv)
        command += (command.empty() ? "" : " ") + arg;
    progress.log_line("> " + command);
    auto process = platform.start(spec, why);
    if (!process)
        return "Could not start " + spec.argv[0] + ": " + why;
    progress.running(process.get());
    while (auto line = process->read_line()) {
        progress.log_line(*line);
        if (auto step = ninja_step(*line))
            progress.steps(step->done, step->total);
    }
    int code = process->wait();
    progress.running(nullptr);
    if (progress.cancelled())
        return "Cancelled.";
    if (code != 0)
        return "The build stopped with an error (code " + std::to_string(code) + ").";
    return {};
}

// The game's Vulkan layers (the game's CMakeLists.txt: vk_layers/ beside kf4, port/gs/pc/pcr_sync2.h), copied into
// the same folder beside the kept program; "" when it worked or the build made none, else why not
std::string keep_vulkan_layers(const fs::path &obj, const fs::path &build_dir) {
    const fs::path layers = obj / "vk_layers";
    std::error_code error;
    if (!fs::is_directory(layers, error))
        return {};
    const fs::path kept = build_dir / "vk_layers";
    fs::create_directories(kept, error);
    if (error)
        return "Cannot make " + kept.u8string() + ": " + error.message();
    for (fs::directory_iterator it(layers, error), end; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error))
            continue;
        fs::copy_file(it->path(), kept / it->path().filename(), fs::copy_options::overwrite_existing, error);
        if (error)
            return "Cannot copy " + it->path().u8string() + ": " + error.message();
    }
    if (error)
        return "Cannot list " + layers.u8string() + ": " + error.message();
    return {};
}

std::string path_option(const char *name, const fs::path &path) {
    return std::string("-D") + name + "=" + path.generic_u8string();
}

BuildResult LocalCMakeBuild::build(const Platform &platform, const BuildRequest &request,
                                   BuildProgress &progress) const {
    std::string why;
    auto tools = platform.toolchain(request.content, why);
    if (!tools)
        // (the release has them: the install lost some, an update cut short or an antivirus, as a player's 0.1.4 did)
        return {false, "This install of Holyland is incomplete (" + why +
                           "). Install the release again from its download page."};
    fs::path source = request.content / "src";
    fs::path obj = request.build_dir / "obj";
    std::error_code error;
    fs::create_directories(obj, error);
    if (error)
        return {false, "Cannot make the build folder " + obj.u8string() + ": " + error.message()};

    progress.stage("Preparing the build");
    ProcessSpec configure;
    configure.argv = {tools->cmake.u8string(), "-G", "Ninja", "-S", source.u8string(), "-B", obj.u8string(),
                      path_option("CMAKE_MAKE_PROGRAM", tools->ninja), path_option("CMAKE_C_COMPILER", tools->c_compiler),
                      path_option("CMAKE_CXX_COMPILER", tools->cxx_compiler), "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                      "-DKF4_LTO=ON", "-DKF4_FRAME_POINTERS=OFF", "-DKF4_PLAYER_BUILD=ON", "-DKF4_TESTS=OFF", "-DKF4_DECOMP_PREPATCHED=ON",
                      "-DKF4_VERSION=" + request.version};
    configure.argv.insert(configure.argv.end(), tools->cmake_options.begin(), tools->cmake_options.end());
    configure.working_dir = request.build_dir;
    configure.env = tools->env;
    configure.capture_output = true;
    if (std::string failed = run_step(platform, configure, progress); !failed.empty())
        return {false, failed};

    progress.stage("Building the game");
    ProcessSpec compile;
    compile.argv = {tools->cmake.u8string(), "--build", obj.u8string(), "--target", "kf4"};
    compile.working_dir = request.build_dir;
    compile.env = tools->env;
    compile.capture_output = true;
    if (std::string failed = run_step(platform, compile, progress); !failed.empty())
        return {false, failed};

    progress.stage("Finishing");
    std::vector<fs::path> keep = platform.game_files(obj);
    if (keep.empty())
        return {false, "The build finished but made no game program."};
    keep.insert(keep.end(), tools->runtime_files.begin(), tools->runtime_files.end());
    for (const fs::path &file : keep) {
        fs::path copy = request.build_dir / file.filename();
        fs::copy_file(file, copy, fs::copy_options::overwrite_existing, error);
        if (error)
            return {false, "Cannot copy " + file.u8string() + ": " + error.message()};
        // a copy of the release's read-only file stays removable (Windows will not delete a read-only one)
        fs::permissions(copy, fs::perms::owner_write, fs::perm_options::add, error);
    }
    if (std::string failed = keep_vulkan_layers(obj, request.build_dir); !failed.empty())
        return {false, failed};
    // the objects are big and never used again. An antivirus may still be reading the new program: a few tries, and
    // what is left after them goes at the next start (session.cpp, remove_old_builds)
    std::string left = remove_tree(obj);
    for (int tries = 1; !left.empty() && tries < 4; tries++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        left = remove_tree(obj);
    }
    if (!left.empty())
        progress.log_line("(" + left + "; it goes at the next start)");
    return {true, {}};
}

} // namespace

const BuildStrategy &local_cmake_build() {
    static const LocalCMakeBuild strategy;
    return strategy;
}

} // namespace launcher
