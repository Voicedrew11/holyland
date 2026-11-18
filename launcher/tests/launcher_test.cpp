// The launcher's parts that need no window or network (docs/port/LAUNCHER.md): SHA-256 and the build key,
// Ninja's progress lines, the disc's verdict and an extracted disc's folder, launcher.json, and the local
// build's steps run against a pretend system.
//
//   launcher_test
#include <cstdio>
#include <deque>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "build_key.h"
#include "build_log.h"
#include "build_strategy.h"
#include "disc_check.h"
#include "launcher_files.h"
#include "platform.h"

using namespace launcher;

namespace {

int failed, checked;

void check(const char *what, bool ok) {
    checked++;
    if (!ok)
        failed++;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

std::string sha(std::string_view text) {
    Sha256Hasher hasher;
    hasher.add(text);
    return to_hex(hasher.finish());
}

void test_sha256() {
    check("sha256 empty", sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("sha256 abc", sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("sha256 two blocks", sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                                   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1000000, 'a');
    check("sha256 a million a's", sha(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    Sha256Hasher pieces;
    pieces.add("ab");
    pieces.add("");
    pieces.add("c");
    check("sha256 in pieces", to_hex(pieces.finish()) == sha("abc"));
}

void test_build_key() {
    std::string a = build_key("0.2.0", "src/a.c 1234\n", "linux:llvm-22");
    check("key is 16 hex digits", a.size() == 16 && a.find_first_not_of("0123456789abcdef") == std::string::npos);
    check("key is stable", a == build_key("0.2.0", "src/a.c 1234\n", "linux:llvm-22"));
    check("version changes it", a != build_key("0.2.1", "src/a.c 1234\n", "linux:llvm-22"));
    check("source changes it", a != build_key("0.2.0", "src/a.c 1235\n", "linux:llvm-22"));
    check("tools change it", a != build_key("0.2.0", "src/a.c 1234\n", "linux:llvm-23"));
    check("parts do not run together", build_key("1", "2", "3") != build_key("12", "", "3"));
}

void test_ninja() {
    auto step = ninja_step("[12/3456] Building C object CMakeFiles/kf4.dir/port/x.c.o");
    check("a step", step && step->done == 12 && step->total == 3456);
    check("not a step", !ninja_step("-- Configuring done") && !ninja_step("") && !ninja_step("[a/b] x"));
    check("done past total", !ninja_step("[5/4] x"));
    check("total 0", !ninja_step("[0/0] x"));
    check("no closing", !ninja_step("[1/2"));
    check("huge numbers refused", !ninja_step("[99999999999/99999999999] x"));
}

void test_disc_verdict() {
    std::string us = "BOOT2 = cdrom0:\\SLUS_203.18;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
    check("the USA release", judge_disc(us, true).verdict == DiscVerdict::ok && judge_disc(us, true).boot == "SLUS_203.18");
    check("lower case", judge_disc("boot2=cdrom0:\\slus_203.18;1", true).verdict == DiscVerdict::ok);
    check("no KF4.DAT", judge_disc(us, false).verdict == DiscVerdict::not_kf4);
    check("no SYSTEM.CNF", judge_disc("", true).verdict == DiscVerdict::not_kf4);
    DiscCheck other = judge_disc("BOOT2 = cdrom0:\\SLPS_250.00;1", true);
    check("another release, named", other.verdict == DiscVerdict::other_release &&
                                        other.message.find("SLPS_250.00") != std::string::npos);
    check("no BOOT2", judge_disc("VER = 1.00", true).verdict == DiscVerdict::other_release);
}

fs::path temp_dir(const char *name) {
    fs::path dir = fs::temp_directory_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path &file, const std::string &text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

void test_disc_folder() {
    fs::path dir = temp_dir("darker_reality_launcher_test_disc");
    check("empty folder", check_disc(dir).verdict == DiscVerdict::not_kf4);
    write(dir / "system.cnf", "BOOT2 = cdrom0:\\SLUS_203.18;1\n");
    write(dir / "Data/kf4.dat", "x");
    check("an extracted disc, any case", check_disc(dir).verdict == DiscVerdict::ok);
    check("its SYSTEM.CNF means the folder", check_disc(dir / "system.cnf").verdict == DiscVerdict::ok &&
                                                 disc_meant(dir / "system.cnf") == dir);
    check("missing", check_disc(dir / "nothing.iso").verdict == DiscVerdict::missing);
    write(dir / "fake.iso", std::string(40000, 'x'));
    check("not an image", check_disc(dir / "fake.iso").verdict == DiscVerdict::unreadable);
    fs::remove_all(dir);
}

void test_choices() {
    fs::path dir = temp_dir("darker_reality_launcher_test_choices");
    fs::path file = dir / "launcher.json";
    LauncherChoices none = read_choices(file);
    check("defaults", none.disc.empty() && none.update_check && none.skipped.empty());
    write(file, "{\"future_key\": 7, \"disc\": \"/old\"}");
    LauncherChoices mine{"/d/kf4.iso", false, "v0.2.0"};
    check("written", write_choices(file, mine));
    LauncherChoices back = read_choices(file);
    check("read back", back.disc == mine.disc && !back.update_check && back.skipped == "v0.2.0");
    check("unknown keys kept", read_file(file).value_or("").find("future_key") != std::string::npos);
    write(file, "{broken");
    check("broken means defaults", read_choices(file).update_check && read_choices(file).disc.empty());
    fs::remove_all(dir);
}

// ---- The local build against a pretend system ----------------------------------------------------

struct Script {
    std::vector<std::string> lines;
    int code = 0;
    bool makes_game = false; // the build step: writes obj/kf4 and a DLL
};

class FakeProcess final : public ChildProcess {
public:
    explicit FakeProcess(Script script) : script_(std::move(script)), lines_(script_.lines.begin(), script_.lines.end()) {}
    std::optional<std::string> read_line() override {
        if (lines_.empty())
            return std::nullopt;
        std::string line = lines_.front();
        lines_.pop_front();
        return line;
    }
    void stop() override {
        lines_.clear();
        stopped = true;
    }
    int wait() override { return stopped ? 143 : script_.code; }
    bool stopped = false;

private:
    Script script_;
    std::deque<std::string> lines_;
};

class FakePlatform final : public Platform {
public:
    std::vector<Script> scripts;
    mutable std::vector<std::vector<std::string>> started;
    bool have_tools = true;
    fs::path runtime_file; // Toolchain::runtime_files, when set

    std::string_view name() const override { return "test"; }
    fs::path content_dir() const override { return {}; }
    std::optional<Toolchain> toolchain(const fs::path &, std::string &why) const override {
        if (!have_tools) {
            why = "cmake is missing";
            return std::nullopt;
        }
        Toolchain tools;
        tools.cmake = "/t/cmake";
        tools.ninja = "/t/ninja";
        tools.c_compiler = "/t/clang";
        tools.cxx_compiler = "/t/clang++";
        tools.cmake_options = {"-DCMAKE_TOOLCHAIN_FILE=/t/linux.cmake"};
        tools.id = "test";
        if (!runtime_file.empty())
            tools.runtime_files = {runtime_file};
        return tools;
    }
    std::string_view game_program() const override { return "kf4"; }
    std::vector<fs::path> game_files(const fs::path &build_dir) const override {
        std::vector<fs::path> files;
        std::error_code error;
        for (const char *name : {"kf4", "extra.dll"})
            if (fs::exists(build_dir / name, error))
                files.push_back(build_dir / name);
        return files;
    }
    const BuildStrategy &build_strategy() const override { return local_cmake_build(); }
    std::unique_ptr<ChildProcess> start(const ProcessSpec &spec, std::string &) const override {
        std::size_t n = started.size();
        started.push_back(spec.argv);
        Script script = n < scripts.size() ? scripts[n] : Script{};
        if (script.makes_game) {
            // the build folder is the one after "--build"
            fs::path obj = spec.argv.size() > 2 ? fs::path(spec.argv[2]) : fs::path();
            write(obj / "kf4", "game");
            write(obj / "extra.dll", "dll");
        }
        return std::make_unique<FakeProcess>(script);
    }
    std::optional<fs::path> pick_disc() const override { return std::nullopt; }
    std::optional<std::string> http_get(std::string_view, std::string_view, int) const override { return std::nullopt; }
};

class Recorder final : public BuildProgress {
public:
    std::vector<std::string> stages, log;
    int done = 0, total = 0;
    bool cancel_at_step = false;
    bool cancel = false;
    void stage(std::string_view what) override { stages.emplace_back(what); }
    void steps(int d, int t) override {
        done = d;
        total = t;
        if (cancel_at_step)
            cancel = true;
    }
    void log_line(std::string_view line) override { log.emplace_back(line); }
    bool cancelled() const override { return cancel; }
    void running(ChildProcess *process) override {
        if (process != nullptr && cancel)
            process->stop();
    }
};

bool has(const std::vector<std::string> &args, const std::string &arg) {
    for (const std::string &a : args)
        if (a == arg)
            return true;
    return false;
}

void test_local_build() {
    fs::path dir = temp_dir("darker_reality_launcher_test_build");
    BuildRequest request{"/content", dir / "abc", "0.2.0"};

    FakePlatform platform;
    write(dir / "tools/libSDL2-2.0.so.0", "sdl");
    fs::permissions(dir / "tools/libSDL2-2.0.so.0", fs::perms::owner_write, fs::perm_options::remove);
    platform.runtime_file = dir / "tools/libSDL2-2.0.so.0";
    platform.scripts = {Script{{"-- Configuring done"}, 0, false},
                        Script{{"[1/3] cc a.c", "[2/3] cc b.c", "[3/3] link kf4"}, 0, true}};
    Recorder progress;
    BuildResult result = local_cmake_build().build(platform, request, progress);
    check("build works", result.ok);
    check("two steps run", platform.started.size() == 2);
    check("configured as a release", platform.started.size() == 2 && has(platform.started[0], "-DKF4_LTO=ON") &&
                                         has(platform.started[0], "-DKF4_DECOMP_PREPATCHED=ON") &&
                                         has(platform.started[0], "-DKF4_VERSION=0.2.0") &&
                                         has(platform.started[0], "-DKF4_TESTS=OFF") &&
                                         has(platform.started[0], "-DCMAKE_TOOLCHAIN_FILE=/t/linux.cmake"));
    check("builds the game only", platform.started.size() == 2 && has(platform.started[1], "kf4") &&
                                      has(platform.started[1], "--build"));
    check("steps reported", progress.done == 3 && progress.total == 3);
    check("game kept, objects gone", fs::exists(dir / "abc/kf4") && fs::exists(dir / "abc/extra.dll") &&
                                         !fs::exists(dir / "abc/obj"));
    check("output logged", progress.log.size() >= 4);
    std::error_code error;
    check("the tools' runtime files beside the game, removable",
          fs::exists(dir / "abc/libSDL2-2.0.so.0") &&
              (fs::status(dir / "abc/libSDL2-2.0.so.0", error).permissions() & fs::perms::owner_write) != fs::perms::none);
    fs::permissions(dir / "tools/libSDL2-2.0.so.0", fs::perms::owner_write, fs::perm_options::add, error);

    FakePlatform failing;
    failing.scripts = {Script{{"CMake Error: no compiler"}, 1, false}};
    Recorder p2;
    BuildResult bad = local_cmake_build().build(failing, BuildRequest{"/content", dir / "def", "0.2.0"}, p2);
    check("configure failure stops it", !bad.ok && failing.started.size() == 1 &&
                                            bad.message.find("code 1") != std::string::npos);

    FakePlatform no_tools;
    no_tools.have_tools = false;
    Recorder p3;
    BuildResult missing = local_cmake_build().build(no_tools, BuildRequest{"/content", dir / "ghi", ""}, p3);
    check("missing tools said", !missing.ok && missing.message.find("cmake is missing") != std::string::npos);

    FakePlatform no_game;
    no_game.scripts = {Script{}, Script{{"[1/1] x"}, 0, false}};
    Recorder p4;
    BuildResult empty = local_cmake_build().build(no_game, BuildRequest{"/content", dir / "jkl", ""}, p4);
    check("no program made is a failure", !empty.ok);

    FakePlatform cancelling;
    cancelling.scripts = {Script{}, Script{{"[1/9] a", "[2/9] b", "[3/9] c"}, 0, true}};
    Recorder p5;
    p5.cancel_at_step = true;
    BuildResult cancelled = local_cmake_build().build(cancelling, BuildRequest{"/content", dir / "mno", ""}, p5);
    check("cancel stops it", !cancelled.ok && cancelled.message == "Cancelled.");
    fs::remove_all(dir);
}

} // namespace

int main() {
    test_sha256();
    test_build_key();
    test_ninja();
    test_disc_verdict();
    test_disc_folder();
    test_choices();
    test_local_build();
    std::printf("%d/%d passed\n", checked - failed, checked);
    return failed != 0;
}
