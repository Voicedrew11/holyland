// The problem report (port/include/port/problem_report.h): taken on the game's thread, zipped on a thread of its own
// ("problem report"), the issue form opened from there.
#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pc_facts.hpp"
#include "report_bundle.hpp"
#include "report_text.hpp"
#include "system_facts.hpp"

extern "C" {
#include "port/debug.h"
#include "port/player_ui.h"
#include "port/present_vk.h"
#include "port/problem_report.h"
#include "port/quicksave.h"
#include "port/save_files.h"
#include "port/settings.h"
#include "port/update_notice.h"
}

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kLinkBytes = 8000;            // GitHub refuses a much longer link
constexpr std::size_t kLogHeadLines = 300;          // a log's start: the build, the GPU, the options
constexpr std::size_t kLogTailBytes = 3u << 20;     // and its end: what happened last
constexpr std::size_t kPreviousLogTailBytes = 1u << 20;
constexpr std::size_t kBuildLogTailBytes = 256u << 10;
constexpr auto kPictureWait = std::chrono::seconds(3);
constexpr auto kQuickSaveWait = std::chrono::seconds(8);
constexpr auto kPreviousLogAge = std::chrono::hours(24);

constexpr const char *kReadme =
    "A Holyland problem report (made by the game's Report an Issue, F1 > General).\n\n"
    "system.txt        the build, the system, the CPU, memory, the GPU, displays, window, sound, controllers\n"
    "game.txt          the run: how long, the options; and what the report could not take\n"
    "game_state.json   the game's state (the F10 key's dump)\n"
    "settings.json     the settings file\n"
    "environment.txt   the command line and the variables that change the game, SDL or the GPU's driver\n"
    "logs/             this run's log, the run before it, the launcher's build log\n"
    "screenshot.png    the last picture shown\n"
    "saves/            the save points' saves, the quick saves, and quick-now.kf4quick taken for the report\n\n"
    "Load quick-now.kf4quick with --quick-load FILE. The home folder, the user's and the computer's names and e-mail\n"
    "addresses are taken out of the texts.\n";

// What the game's thread took, for the report's thread
struct Taken {
    std::string stamp;
    fs::path reports, parts;
    bool picture_asked = false;
    bool quick_save_asked = false;
    std::string notes; // game.txt's lines about what could not be taken
    std::string system, summary, run, environment;
    std::vector<report::Redaction> words;
    fs::path settings, game_log, saves_folder, quick_folder, build_log;
};

struct State {
    std::mutex lock;
    std::string message; // (under lock)
    std::atomic<bool> busy{false};
};
State state;

void set_message(const std::string &text) {
    std::fprintf(stderr, "[report] %s\n", text.c_str());
    std::lock_guard<std::mutex> hold(state.lock);
    state.message = text;
}

// ---- The game's thread ------------------------------------------------------------------------------------------

void take_game_state(const Taken &taken) {
    const fs::path path = taken.parts / "game_state.json";
    std::FILE *file = std::fopen(path.string().c_str(), "w");
    if (file == nullptr)
        return;
    port_debug_write_state(file);
    std::fclose(file);
}

Taken take() {
    Taken t;
    t.stamp = report::report_timestamp();
    t.reports = fs::path(port_data_dir()) / "reports";
    t.parts = t.reports / ("holyland-report-" + t.stamp + ".parts");
    std::error_code error;
    fs::create_directories(t.parts, error);
    char why[256] = "";
    t.picture_asked = port_vk_flash_write_newest(t.parts.string().c_str(), why, sizeof(why)) != 0;
    if (!t.picture_asked)
        t.notes += std::string("No screenshot: ") + why + "\n";
    port_quick_request_save_file((t.parts / "quick-now.kf4quick").string().c_str());
    t.quick_save_asked = true;
    take_game_state(t);
    t.system = report::system_facts();
    t.summary = report::system_summary();
    t.run = report::run_facts();
    t.environment = report::environment_facts(report::this_command_line());
    t.words = report::private_words();
    t.settings = port_settings_path();
    const char *log = std::getenv("KF4_GAME_LOG"); // (set by the launcher, which writes it)
    if (log != nullptr && log[0] != 0)
        t.game_log = log;
    else
        t.notes += "No log: the game was not started by the launcher, which keeps it\n";
    if (port_saves_available())
        t.saves_folder = port_saves_folder();
    t.quick_folder = fs::path(port_data_dir()) / "quicksaves";
    t.build_log = fs::path(port_data_dir()) / "build.log";
    return t;
}

// ---- The report's thread ------------------------------------------------------------------------------------------

// Waits until done() or the time is up: done()'s last answer
template <typename Done> bool wait_for(Done done, std::chrono::milliseconds most) {
    const auto until = std::chrono::steady_clock::now() + most;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

bool picture_written(const fs::path &parts) {
    int pictures = 0, w = 0, h = 0;
    double megabytes = 0;
    std::error_code error;
    return port_vk_flash_info(&pictures, &w, &h, &megabytes) == 0 && fs::exists(parts / "0000.png", error);
}

bool quick_save_written(const fs::path &parts) {
    std::error_code error;
    return fs::exists(parts / "quick-now.kf4quick", error);
}

void add_logs(report::ReportBundle &bundle, const Taken &t, std::string &notes) {
    if (!t.game_log.empty()) {
        if (!bundle.add_log("logs/game.log", t.game_log, kLogHeadLines, kLogTailBytes))
            notes += "No log: " + t.game_log.string() + " could not be read\n";
        const fs::path previous = t.game_log.parent_path() / "game.prev.log";
        if (report::written_within(previous, kPreviousLogAge))
            bundle.add_log("logs/game.prev.log", previous, kLogHeadLines, kPreviousLogTailBytes);
    }
    bundle.add_log("logs/build.log", t.build_log, 40, kBuildLogTailBytes);
}

void add_saves(report::ReportBundle &bundle, const Taken &t, std::string &notes) {
    if (quick_save_written(t.parts)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // (its write finished: it is put in place whole)
        bundle.add_save("saves/quick-now.kf4quick", t.parts / "quick-now.kf4quick");
    } else {
        notes += "No quick save where you stood: the game could not save then (a menu, a message or an event)\n";
    }
    if (!t.saves_folder.empty())
        bundle.add_save_folder("saves/", t.saves_folder);
    bundle.add_save_folder("saves/quicksaves/", t.quick_folder);
}

report::ReportBundle gather(const Taken &t) {
    std::string notes = t.notes;
    report::ReportBundle bundle(t.words);
    bundle.add_text("README.txt", kReadme);
    bundle.add_text("system.txt", t.system);
    bundle.add_text("environment.txt", t.environment);
    if (auto settings = report::read_text(t.settings))
        bundle.add_text("settings.json", *settings);
    if (auto state_json = report::read_text(t.parts / "game_state.json"))
        bundle.add_text("game_state.json", *state_json);
    add_logs(bundle, t, notes);
    if (t.picture_asked && wait_for([&] { return picture_written(t.parts); }, kPictureWait))
        bundle.add_file("screenshot.png", t.parts / "0000.png");
    else if (t.picture_asked)
        notes += "No screenshot: the picture was not written in time\n";
    if (t.quick_save_asked)
        wait_for([&] { return quick_save_written(t.parts); }, kQuickSaveWait);
    add_saves(bundle, t, notes);
    bundle.add_text("game.txt", t.run + (notes.empty() ? "" : "\nNot in this report:\n" + notes));
    return bundle;
}

void open_form(const std::string &url, const fs::path &zip) {
    if (report::keep_link_instead(zip, url))
        return;
    if (!SDL_OpenURL(url.c_str()))
        std::fprintf(stderr, "[report] the browser did not open: %s\n", SDL_GetError());
    if (!SDL_OpenURL(report::file_link(zip.parent_path().generic_string()).c_str()))
        std::fprintf(stderr, "[report] the folder did not open: %s\n", SDL_GetError());
}

void make_report(const Taken &t) {
    const report::ReportBundle bundle = gather(t);
    std::error_code error;
    fs::remove_all(t.parts, error);
    const std::optional<fs::path> zip = bundle.write(t.reports, "holyland-report-" + t.stamp);
    if (!zip) {
        set_message("The report could not be saved in " + t.reports.string() + ".");
        return;
    }
    const std::string details = t.summary + "Report file: " + zip->filename().string() + "\n";
    const std::string url = report::issue_url({UPDATE_REPOSITORY, "", "", bundle.redacted(details)}, kLinkBytes);
    open_form(url, *zip);
    set_message("Saved " + zip->string() + ". Drag it into the issue form your browser opened.");
}

int report_thread(void *data) {
    std::unique_ptr<Taken> taken(static_cast<Taken *>(data));
    make_report(*taken);
    state.busy.store(false);
    return 0;
}

} // namespace

extern "C" void port_report_start(void) {
    if (state.busy.exchange(true)) {
        set_message("A report is already being made.");
        return;
    }
    set_message("Making the report...");
    auto taken = std::make_unique<Taken>(take());
    port_player_ui_close(); // (the quick save is taken in play, and the browser gets the screen)
    SDL_Thread *thread = SDL_CreateThread(report_thread, "problem report", taken.get());
    if (thread == nullptr) {
        set_message(std::string("The report could not start: ") + SDL_GetError());
        state.busy.store(false);
        return;
    }
    taken.release(); // (the thread's now)
    SDL_DetachThread(thread);
}

extern "C" const char *port_report_message(void) {
    static std::string shown; // (the game's thread's copy)
    std::lock_guard<std::mutex> hold(state.lock);
    shown = state.message;
    return shown.c_str();
}
