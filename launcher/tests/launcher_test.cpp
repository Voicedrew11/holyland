// The launcher's parts that need no window or network (docs/port/LAUNCHER.md): SHA-256 and the build key,
// Ninja's progress lines, the disc's verdict and an extracted disc's folder, launcher.json, text put in the pages, the
// first start's order of pages, and the local build's steps run against a pretend system.
//
//   launcher_test
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "build_key.h"
#include "build_log.h"
#include "build_strategy.h"
#include "disc_check.h"
#include "launcher_files.h"
#include "platform.h"
#include "rml_text.h"
#include "self_update.h"
#include "setup_flow.h"
#include "update_delta.h"

extern "C" {
#include "monocypher-ed25519.h"
}

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
    check("its region said", other.message.find("Japanese") != std::string::npos &&
                                 other.message.find("SLUS-20318") != std::string::npos);
    check("European", judge_disc("BOOT2 = cdrom0:\\SLES_512.34;1", true).message.find("European") != std::string::npos);
    DiscCheck incomplete = judge_disc(us, false);
    check("the USA disc without KF4.DAT: an incomplete copy", incomplete.verdict == DiscVerdict::not_kf4 &&
                                                                 incomplete.message.find("incomplete") != std::string::npos);
    DiscCheck another_game = judge_disc("BOOT2 = cdrom0:\\SLUS_209.46;1", false);
    check("another game, named", another_game.verdict == DiscVerdict::not_kf4 &&
                                     another_game.message.find("not King's Field IV") != std::string::npos &&
                                     another_game.message.find("SLUS_209.46") != std::string::npos);
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
    fs::path dir = temp_dir("holyland_launcher_test_disc");
    check("empty folder", check_disc(dir).verdict == DiscVerdict::not_kf4);
    write(dir / "system.cnf", "BOOT2 = cdrom0:\\SLUS_203.18;1\n");
    write(dir / "Data/kf4.dat", "x");
    check("an extracted disc, any case", check_disc(dir).verdict == DiscVerdict::ok);
    check("its SYSTEM.CNF means the folder", check_disc(dir / "system.cnf").verdict == DiscVerdict::ok &&
                                                 disc_meant(dir / "system.cnf") == dir);
    check("missing", check_disc(dir / "nothing.iso").verdict == DiscVerdict::missing);
    write(dir / "fake.iso", std::string(40000, 'x'));
    check("not an image", check_disc(dir / "fake.iso").verdict == DiscVerdict::unreadable);
    write(dir / "fake.chd", "MComprHD" + std::string(40000, 'x'));
    DiscCheck broken = check_disc(dir / "fake.chd");
    check("a broken CHD says so", broken.verdict == DiscVerdict::unreadable && broken.message.find("CHD") != std::string::npos);
    fs::remove_all(dir);
}

// tests/data's CHD: a disc image with SYSTEM.CNF ("BOOT2", booting nothing) and DATA\KF4.DAT, read through it
void test_disc_chd() {
    DiscCheck chd = check_disc(fs::path(TEST_DATA_DIR) / "disc_dvd.chd");
    check("a CHD is read: its files are found", chd.verdict == DiscVerdict::other_release);
}

void test_choices() {
    fs::path dir = temp_dir("holyland_launcher_test_choices");
    fs::path file = dir / "launcher.json";
    LauncherChoices none = read_choices(file);
    check("defaults", none.disc.empty() && none.update_check && none.skipped.empty());
    write(file, "{\"future_key\": 7, \"disc\": \"/old\"}");
    check("setup not done at first", !none.setup_done);
    LauncherChoices mine{"/d/kf4.iso", false, "v0.2.0", true};
    check("written", write_choices(file, mine));
    LauncherChoices back = read_choices(file);
    check("read back", back.disc == mine.disc && !back.update_check && back.skipped == "v0.2.0" && back.setup_done);
    check("unknown keys kept", read_file(file).value_or("").find("future_key") != std::string::npos);
    write(file, "{broken");
    check("broken means defaults", read_choices(file).update_check && read_choices(file).disc.empty());
    fs::remove_all(dir);
}

void test_rml_escaped() {
    check("plain text kept", rml_escaped("King's Field IV") == "King's Field IV");
    check("markup escaped", rml_escaped("<p a=\"b\">x & y</p>") == "&lt;p a=&quot;b&quot;&gt;x &amp; y&lt;/p&gt;");
    check("empty", rml_escaped("").empty());
}

struct CountingPage final : SetupPage {
    PageAnswer next_answer = PageAnswer::stay;
    int drawn = 0;
    PageAnswer draw(Window &) override {
        drawn++;
        return next_answer;
    }
};

void test_setup_flow() {
    check("no pages: finished at once", SetupFlow({}).finished());
    std::vector<std::unique_ptr<SetupPage>> pages;
    pages.push_back(std::make_unique<CountingPage>());
    pages.push_back(std::make_unique<CountingPage>());
    auto *first = static_cast<CountingPage *>(pages[0].get());
    SetupFlow flow(std::move(pages));
    check("starts on the first page", !flow.finished() && &flow.page() == first && flow.page_number() == 0);
    check("two pages", flow.page_count() == 2);
    check("no going back from the first", !flow.can_go_back());
    flow.answer(PageAnswer::stay);
    check("stay stays", flow.page_number() == 0);
    flow.answer(PageAnswer::next);
    check("next goes on", flow.page_number() == 1 && &flow.page() != first && flow.can_go_back());
    flow.back();
    check("back goes back", flow.page_number() == 0 && &flow.page() == first);
    flow.answer(PageAnswer::next);
    flow.answer(PageAnswer::next);
    check("the last answer finishes", flow.finished() && !flow.can_go_back());
    flow.answer(PageAnswer::next);
    flow.back();
    check("finished stays finished", flow.finished());
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
            write(obj / "vk_layers/VkLayer_khronos_synchronization2.json", "manifest");
            write(obj / "vk_layers/libVkLayer_khronos_synchronization2.so", "layer");
        }
        return std::make_unique<FakeProcess>(script);
    }
    std::string describe_exit(int code) const override { return "code " + std::to_string(code); }
    std::optional<fs::path> pick_disc() const override { return std::nullopt; }
    std::map<std::string, std::string> web; // a pretend network: what each URL answers
    std::optional<std::string> http_get(std::string_view url, std::string_view, int) const override {
        auto it = web.find(std::string(url));
        if (it == web.end())
            return std::nullopt;
        return it->second;
    }
    std::string http_download(std::string_view url, const fs::path &to, std::uint64_t max_bytes, std::string_view,
                              const DownloadProgress &progress) const override {
        auto it = web.find(std::string(url));
        if (it == web.end())
            return "no such file";
        if (it->second.size() > max_bytes)
            return "the file is larger than expected";
        write(to, it->second);
        if (progress && !progress(it->second.size(), it->second.size())) {
            std::error_code error;
            fs::remove(to, error);
            return "stopped";
        }
        return {};
    }
    std::unique_ptr<Installation> installation(std::string &why) const override {
        why = "a test";
        return nullptr;
    }
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
    fs::path dir = temp_dir("holyland_launcher_test_build");
    BuildRequest request{"/content", dir / "abc", "0.2.0"};

    FakePlatform platform;
    write(dir / "tools/libSDL3.so.0", "sdl");
    fs::permissions(dir / "tools/libSDL3.so.0", fs::perms::owner_write, fs::perm_options::remove);
    platform.runtime_file = dir / "tools/libSDL3.so.0";
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
    check("the Vulkan layers kept in vk_layers/ beside the game",
          fs::exists(dir / "abc/vk_layers/VkLayer_khronos_synchronization2.json") &&
              fs::exists(dir / "abc/vk_layers/libVkLayer_khronos_synchronization2.so"));
    check("output logged", progress.log.size() >= 4);
    std::error_code error;
    check("the tools' runtime files beside the game, removable",
          fs::exists(dir / "abc/libSDL3.so.0") &&
              (fs::status(dir / "abc/libSDL3.so.0", error).permissions() & fs::perms::owner_write) != fs::perms::none);
    fs::permissions(dir / "tools/libSDL3.so.0", fs::perms::owner_write, fs::perm_options::add, error);

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

// ---- An update's download and checks against a pretend network -------------------------------------------------

class FakeInstallation final : public Installation {
public:
    explicit FakeInstallation(fs::path dir) : dir_(std::move(dir)) {}
    bool takes_deltas = false;
    std::string made_from;       // the delta made_from_delta was given, read
    std::string makes;           // what a delta "makes" (written to download_path()); "" fails
    std::string_view asset_suffix() const override { return "-x86_64.AppImage"; }
    fs::path download_path() const override { return dir_ / "new.partial"; }
    std::string replace_and_start(const fs::path &, const std::vector<std::string> &) const override { return {}; }
    void remove_leftovers() const override {}
    fs::path delta_download_path() const override { return takes_deltas ? dir_ / "new.delta" : fs::path(); }
    Made made_from_delta(const fs::path &delta, std::string_view whole_sha256) const override {
        const_cast<FakeInstallation *>(this)->made_from = read_file(delta).value_or("");
        std::error_code error;
        fs::remove(delta, error);
        if (makes.empty() || sha(makes) != whole_sha256)
            return {{}, "a test's delta that does not work"};
        write(download_path(), makes);
        return {download_path(), {}};
    }

private:
    fs::path dir_;
};

class QuietUpdate final : public UpdateProgress {
public:
    std::vector<std::string> stages;
    void stage(std::string_view what) override { stages.emplace_back(what); }
    bool bytes(std::uint64_t, std::uint64_t) override { return true; }
};

std::string hex_of(const std::uint8_t *bytes, std::size_t size) {
    std::string hex;
    char two[3];
    for (std::size_t i = 0; i < size; i++) {
        std::snprintf(two, sizeof(two), "%02x", bytes[i]);
        hex += two;
    }
    return hex;
}

void test_update_fetch() {
    fs::path dir = temp_dir("holyland_launcher_test_update");
    std::uint8_t seed[32], secret[64], other_secret[64];
    UpdateSource source{"https://api.github.com/r/tags/v0.3.0", "https://github.com/", {}};
    ReleaseKey other{};
    for (int i = 0; i < 32; i++)
        seed[i] = (std::uint8_t)i;
    crypto_ed25519_key_pair(secret, source.key.data(), seed); // (it wipes the seed)
    for (int i = 0; i < 32; i++)
        seed[i] = (std::uint8_t)(100 + i);
    crypto_ed25519_key_pair(other_secret, other.data(), seed);

    const std::string game = "the new AppImage's bytes";
    const std::string base = "https://github.com/r/releases/download/v0.3.0/";
    std::string sums = sha(game) + "  H-0.3.0-x86_64.AppImage\n" + sha("zip") + "  H-0.3.0-win-x64.zip\n";
    auto sign = [&sums](const std::uint8_t *key) {
        std::uint8_t signature[64];
        crypto_ed25519_sign(signature, key, reinterpret_cast<const std::uint8_t *>(sums.data()), sums.size());
        return hex_of(signature, 64) + "\n";
    };
    std::string release = "{\"tag_name\":\"v0.3.0\",\"assets\":["
                          "{\"name\":\"H-0.3.0-x86_64.AppImage\",\"browser_download_url\":\"" + base + "H-0.3.0-x86_64.AppImage\"},"
                          "{\"name\":\"SHA256SUMS\",\"browser_download_url\":\"" + base + "SHA256SUMS\"},"
                          "{\"name\":\"SHA256SUMS.sig\",\"browser_download_url\":\"" + base + "SHA256SUMS.sig\"}]}";
    FakePlatform platform;
    FakeInstallation installation(dir);
    platform.web[source.release_url] = release;
    platform.web[base + "H-0.3.0-x86_64.AppImage"] = game;
    platform.web[base + "SHA256SUMS"] = sums;
    platform.web[base + "SHA256SUMS.sig"] = sign(secret);
    std::error_code error;

    QuietUpdate progress;
    std::string why = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress).why;
    auto got = read_file(installation.download_path());
    check("a good update is downloaded and passes", why.empty() && got && *got == game);
    check("its stages are told", progress.stages.size() == 4);
    check("the checksums' file is not left", !fs::exists(dir / "new.partial.sums", error));
    fs::remove(installation.download_path(), error);

    platform.web[base + "H-0.3.0-x86_64.AppImage"] = "something else";
    why = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress).why;
    check("a changed file is refused and removed",
          why.find("checksum differs") != std::string::npos && !fs::exists(installation.download_path(), error));
    platform.web[base + "H-0.3.0-x86_64.AppImage"] = game;

    platform.web[base + "SHA256SUMS.sig"] = sign(other_secret);
    why = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress).why;
    check("a signature by another key is refused, before the download",
          why.find("signature is not good") != std::string::npos && !fs::exists(installation.download_path(), error));

    platform.web[base + "SHA256SUMS.sig"] = sign(secret);
    platform.web[base + "SHA256SUMS"] = sums + "\n";
    why = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress).why;
    check("checksums changed after signing are refused", why.find("signature is not good") != std::string::npos);
    platform.web[base + "SHA256SUMS"] = sums;

    platform.web[base + "SHA256SUMS.sig"] = "not hex\n";
    why = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress).why;
    check("a broken signature is refused", why.find("signature is not good") != std::string::npos);

    platform.web[source.release_url] = "{\"tag_name\":\"v0.3.0\",\"assets\":["
                                       "{\"name\":\"H-0.3.0-x86_64.AppImage\",\"browser_download_url\":\"" + base +
                                       "H-0.3.0-x86_64.AppImage\"}]}";
    why = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress).why;
    check("an unsigned release is refused", why.find("not signed") != std::string::npos);

    platform.web[source.release_url] = release;
    why = fetch_checked_update(platform, installation, source, "v0.4.0", "0.2.0", "test", progress).why;
    check("another release's answer is refused", why.find("no file for this computer") != std::string::npos);

    platform.web.erase(source.release_url);
    why = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress).why;
    check("no network", why.find("could not be reached") != std::string::npos);

    // a delta from 0.2.0 beside the whole file (update_delta.h)
    const std::string delta = "the delta's bytes";
    std::string delta_sums = sums + sha(delta) + "  H-0.3.0-x86_64.AppImage.from-0.2.0.delta\n";
    sums = delta_sums;
    std::string with_delta = release;
    with_delta.insert(with_delta.rfind(']'), ",{\"name\":\"H-0.3.0-x86_64.AppImage.from-0.2.0.delta\","
                                            "\"browser_download_url\":\"" + base + "H-0.3.0-x86_64.AppImage.from-0.2.0.delta\"}");
    platform.web[source.release_url] = with_delta;
    platform.web[base + "SHA256SUMS"] = sums;
    platform.web[base + "SHA256SUMS.sig"] = sign(secret);
    platform.web[base + "H-0.3.0-x86_64.AppImage.from-0.2.0.delta"] = delta;
    installation.takes_deltas = true;
    installation.makes = game;
    platform.web[base + "H-0.3.0-x86_64.AppImage"] = "the whole file is not to be downloaded";
    progress.stages.clear();
    Installation::Made made =
        fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress);
    got = read_file(made.update);
    check("the delta is used when there is one", made.why.empty() && installation.made_from == delta && got &&
                                                     *got == game);
    check("the delta's download is not left", !fs::exists(dir / "new.delta", error));
    fs::remove(installation.download_path(), error);
    platform.web[base + "H-0.3.0-x86_64.AppImage"] = game;

    installation.made_from.clear();
    made = fetch_checked_update(platform, installation, source, "v0.3.0", "0.1.0", "test", progress);
    got = read_file(made.update);
    check("no delta from this version: the whole file", made.why.empty() && installation.made_from.empty() && got &&
                                                          *got == game);
    fs::remove(installation.download_path(), error);

    made = fetch_checked_update(platform, installation, source, "v0.3.0", "", "test", progress);
    check("a build with no version takes no delta", made.why.empty() && installation.made_from.empty());
    fs::remove(installation.download_path(), error);

    installation.makes.clear();
    made = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress);
    got = read_file(made.update);
    check("a delta that does not work: the whole file instead", made.why.empty() && installation.made_from == delta &&
                                                                    got && *got == game);
    fs::remove(installation.download_path(), error);

    installation.makes = game;
    installation.made_from.clear();
    platform.web[base + "H-0.3.0-x86_64.AppImage.from-0.2.0.delta"] = "a changed delta";
    made = fetch_checked_update(platform, installation, source, "v0.3.0", "0.2.0", "test", progress);
    got = read_file(made.update);
    check("a delta whose checksum differs is not used", made.why.empty() && installation.made_from.empty() && got &&
                                                            *got == game && !fs::exists(dir / "new.delta", error));
    fs::remove_all(dir, error);
}

// ---- Deltas: a file patch and a tree (update_delta.h) -----------------------------------------------------------

void test_delta_file() {
    fs::path dir = temp_dir("holyland_launcher_test_delta_file");
    fs::path data = fs::path(TEST_DATA_DIR) / "delta";
    fs::path out = dir / "made";
    std::string whole = file_sha256(data / "new.bin");
    std::error_code error;
    check("delta's name", delta_name("H-0.3.0-x86_64.AppImage", "0.2.0") == "H-0.3.0-x86_64.AppImage.from-0.2.0.delta");

    std::string why = apply_file_patch(data / "old.bin", data / "new.bin.patch", out, whole, kUpdateMaxBytes);
    check("a file patch makes the new file", why.empty() && read_file(out) == read_file(data / "new.bin"));
    fs::remove(out, error);

    why = apply_file_patch(data / "new.bin", data / "new.bin.patch", out, whole, kUpdateMaxBytes);
    check("a patch against another file is refused, nothing left", !why.empty() && !fs::exists(out, error));

    why = apply_file_patch(data / "old.bin", data / "new.bin.patch", out, sha("other"), kUpdateMaxBytes);
    check("a patch that makes another file is refused",
          why.find("checksum differs") != std::string::npos && !fs::exists(out, error));

    why = apply_file_patch(data / "old.bin", data / "new.bin.patch", out, whole, 1000);
    check("a patch making more than the limit is refused", !why.empty() && !fs::exists(out, error));

    std::string patch = read_file(data / "new.bin.patch").value_or("");
    write(dir / "short.patch", patch.substr(0, patch.size() / 2));
    why = apply_file_patch(data / "old.bin", dir / "short.patch", out, whole, kUpdateMaxBytes);
    check("a patch cut short is refused", !why.empty() && !fs::exists(out, error));

    write(dir / "long.patch", patch + patch);
    why = apply_file_patch(data / "old.bin", dir / "long.patch", out, whole, kUpdateMaxBytes);
    check("a patch with more after its end is refused", !why.empty() && !fs::exists(out, error));

    write(dir / "junk.patch", "not zstd at all");
    why = apply_file_patch(data / "old.bin", dir / "junk.patch", out, whole, kUpdateMaxBytes);
    check("a patch that is not one is refused", !why.empty() && !fs::exists(out, error));
    fs::remove_all(dir, error);
}

void test_delta_tree() {
    fs::path dir = temp_dir("holyland_launcher_test_delta_tree");
    fs::path installed = dir / "installed", staged = dir / "staged";
    std::error_code error;
    auto setup = [&](const std::string &listing) {
        fs::remove_all(installed, error);
        fs::remove_all(staged, error);
        write(installed / "holyland.exe", "old launcher");
        write(installed / "content/src/same.c", "unchanged");
        write(installed / "content/src/gone.c", "removed in the new release");
        write(installed / "content/toolchain/bin/clang", "the big tools");
        write(staged / "holyland.exe", "new launcher");
        write(staged / "content/src/added.c", "new in this release");
        write(staged / kDeltaListing, listing);
    };
    std::string listing = sha("new launcher") + "  holyland.exe\n" + sha("unchanged") + "  content/src/same.c\n" +
                          sha("new in this release") + "  content/src/added.c\n" + sha("the big tools") +
                          " *content/toolchain/bin/clang\n";

    setup(listing);
    std::string why = complete_tree(staged, installed);
    std::size_t files = 0;
    for (fs::recursive_directory_iterator it(staged, error), end; !error && it != end; it.increment(error))
        files += it->is_regular_file() ? 1 : 0;
    check("a tree delta makes the whole new tree",
          why.empty() && files == 4 && read_file(staged / "content/src/same.c") == std::optional<std::string>("unchanged") &&
              read_file(staged / "content/toolchain/bin/clang") == std::optional<std::string>("the big tools") &&
              read_file(staged / "holyland.exe") == std::optional<std::string>("new launcher") &&
              !fs::exists(staged / "content/src/gone.c", error) && !fs::exists(staged / kDeltaListing, error));

    setup(listing);
    write(installed / "content/src/same.c", "changed by hand");
    why = complete_tree(staged, installed);
    check("an install that is not the old release's is refused", why.find("checksum differs") != std::string::npos);

    setup(listing);
    fs::remove(installed / "content/src/same.c", error);
    why = complete_tree(staged, installed);
    check("an install missing a kept file is refused", !why.empty());

    setup(listing);
    write(staged / "content/src/unlisted.c", "?");
    why = complete_tree(staged, installed);
    check("a delta holding a file its list does not is refused", why.find("unlisted.c") != std::string::npos);

    setup(listing);
    write(staged / "holyland.exe", "a changed launcher");
    why = complete_tree(staged, installed);
    check("a delta's own file that is not its list's is refused", why.find("checksum differs") != std::string::npos);

    for (const char *bad : {"../outside", "/etc/passwd", "content/../../x", "content\\x", "C:/x", "./x", "a//b"}) {
        setup(listing + sha("x") + "  " + bad + "\n");
        why = complete_tree(staged, installed);
        check((std::string("a listed path outside the tree is refused: ") + bad).c_str(),
              why.find("outside its folder") != std::string::npos);
    }

    setup("not a listing\n");
    check("a broken list is refused", !complete_tree(staged, installed).empty());
    setup(listing + listing);
    check("a file listed twice is refused", complete_tree(staged, installed).find("twice") != std::string::npos);
    setup(listing);
    fs::remove(staged / kDeltaListing, error);
    check("a delta without its list is refused", !complete_tree(staged, installed).empty());
    fs::remove_all(dir, error);
}

void test_update_request() {
    fs::path dir = temp_dir("holyland_launcher_test_request");
    DataPaths paths{dir, dir / "builds"};
    check("no ask", !take_update_request(paths));
    write(paths.update_request(), "{\"tag\": \"v0.3.0\"}");
    auto tag = take_update_request(paths);
    std::error_code error;
    check("the game's ask, read once", tag && *tag == "v0.3.0" && !fs::exists(paths.update_request(), error));
    write(paths.update_request(), "{\"tag\": \"preview\"}");
    check("an ask for no version is dropped", !take_update_request(paths) && !fs::exists(paths.update_request(), error));
    fs::remove_all(dir, error);
}

int main() {
    test_sha256();
    test_build_key();
    test_ninja();
    test_disc_verdict();
    test_disc_folder();
    test_disc_chd();
    test_choices();
    test_rml_escaped();
    test_setup_flow();
    test_local_build();
    test_update_fetch();
    test_delta_file();
    test_delta_tree();
    test_update_request();
    std::printf("%d/%d passed\n", checked - failed, checked);
    return failed != 0;
}
