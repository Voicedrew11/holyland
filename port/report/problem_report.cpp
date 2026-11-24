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

#include "report_text.hpp"
#include "system_facts.hpp"
#include "zip_archive.hpp"

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
constexpr std::uintmax_t kSaveFileBytes = 8u << 20; // a save file bigger than this is not a save
constexpr std::uintmax_t kSavesBytes = 32u << 20;
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

std::string timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

std::optional<std::string> read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> bytes_of(const std::string &text) {
    return {text.begin(), text.end()};
}

bool fresh(const fs::path &path, std::chrono::hours age) {
    std::error_code error;
    const auto written = fs::last_write_time(path, error);
    return !error && fs::file_time_type::clock::now() - written < age;
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
    t.stamp = timestamp();
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
    t.environment = report::environment_facts();
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

struct Entries {
    std::vector<report::ZipEntry> list;
    std::uintmax_t save_bytes = 0;
    const std::vector<report::Redaction> *words;

    void text(const std::string &name, const std::string &body) {
        list.push_back({name, bytes_of(report::redact(body, *words))});
    }
    // a save file as it is, within the saves' budget
    void save(const std::string &name, const fs::path &path) {
        std::error_code error;
        const std::uintmax_t size = fs::file_size(path, error);
        if (error || size > kSaveFileBytes || save_bytes + size > kSavesBytes)
            return;
        if (auto body = read_text(path)) {
            save_bytes += body->size();
            list.push_back({name, bytes_of(*body)});
        }
    }
    void save_folder(const std::string &prefix, const fs::path &folder) {
        std::error_code error;
        for (const fs::directory_entry &entry : fs::directory_iterator(folder, error)) {
            if (entry.is_regular_file(error))
                save(prefix + entry.path().filename().string(), entry.path());
        }
    }
};

void add_logs(Entries &e, const Taken &t, std::string &notes) {
    if (!t.game_log.empty()) {
        if (auto log = read_text(t.game_log))
            e.text("logs/game.log", report::head_and_tail(*log, kLogHeadLines, kLogTailBytes));
        else
            notes += "No log: " + t.game_log.string() + " could not be read\n";
        const fs::path previous = t.game_log.parent_path() / "game.prev.log";
        if (fresh(previous, kPreviousLogAge)) {
            if (auto log = read_text(previous))
                e.text("logs/game.prev.log", report::head_and_tail(*log, kLogHeadLines, kPreviousLogTailBytes));
        }
    }
    if (auto log = read_text(t.build_log))
        e.text("logs/build.log", report::head_and_tail(*log, 40, kBuildLogTailBytes));
}

void add_saves(Entries &e, const Taken &t, std::string &notes) {
    if (quick_save_written(t.parts)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // (its write finished: it is put in place whole)
        e.save("saves/quick-now.kf4quick", t.parts / "quick-now.kf4quick");
    } else {
        notes += "No quick save where you stood: the game could not save then (a menu, a message or an event)\n";
    }
    if (!t.saves_folder.empty())
        e.save_folder("saves/", t.saves_folder);
    e.save_folder("saves/quicksaves/", t.quick_folder);
}

std::vector<report::ZipEntry> gather(const Taken &t) {
    std::string notes = t.notes;
    Entries e{{}, 0, &t.words};
    e.list.push_back({"README.txt", bytes_of(kReadme)});
    e.text("system.txt", t.system);
    e.text("environment.txt", t.environment);
    if (auto settings = read_text(t.settings))
        e.text("settings.json", *settings);
    if (auto state_json = read_text(t.parts / "game_state.json"))
        e.text("game_state.json", *state_json);
    add_logs(e, t, notes);
    if (t.picture_asked && wait_for([&] { return picture_written(t.parts); }, kPictureWait)) {
        if (auto png = read_text(t.parts / "0000.png"))
            e.list.push_back({"screenshot.png", bytes_of(*png)});
    } else if (t.picture_asked) {
        notes += "No screenshot: the picture was not written in time\n";
    }
    if (t.quick_save_asked)
        wait_for([&] { return quick_save_written(t.parts); }, kQuickSaveWait);
    add_saves(e, t, notes);
    e.text("game.txt", t.run + (notes.empty() ? "" : "\nNot in this report:\n" + notes));
    return e.list;
}

bool write_whole(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    const fs::path part = path.string() + ".tmp";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        if (!out.flush())
            return false;
    }
    std::error_code error;
    fs::rename(part, path, error);
    return !error;
}

// A folder as a file:// link, each byte that needs it escaped
std::string folder_link(const fs::path &folder) {
    std::string path = folder.generic_string();
    std::string link = path.front() == '/' ? "file://" : "file:///";
    for (char c : path) {
        const auto b = static_cast<unsigned char>(c);
        if (c == '/' || c == ':' || b >= 0x80)
            link += c;
        else
            link += report::url_encode(std::string_view(&c, 1));
    }
    return link;
}

void open_form(const std::string &url, const fs::path &zip) {
    const char *no_open = std::getenv("KF4_REPORT_NO_OPEN");
    if (no_open != nullptr && no_open[0] == '1') {
        std::ofstream(zip.string() + ".url.txt") << url << "\n";
        return;
    }
    if (!SDL_OpenURL(url.c_str()))
        std::fprintf(stderr, "[report] the browser did not open: %s\n", SDL_GetError());
    if (!SDL_OpenURL(folder_link(zip.parent_path()).c_str()))
        std::fprintf(stderr, "[report] the folder did not open: %s\n", SDL_GetError());
}

void make_report(const Taken &t) {
    const std::vector<report::ZipEntry> entries = gather(t);
    const fs::path zip = t.reports / ("holyland-report-" + t.stamp + ".zip");
    const auto archive = report::zip_archive(entries, std::time(nullptr));
    std::error_code error;
    fs::remove_all(t.parts, error);
    if (!archive || !write_whole(zip, *archive)) {
        set_message("The report could not be saved in " + t.reports.string() + ".");
        return;
    }
    const std::string details = t.summary + "Report file: " + zip.filename().string() + "\n";
    const std::string url = report::issue_url(
        {UPDATE_REPOSITORY, "", "", report::redact(details, t.words)}, kLinkBytes);
    open_form(url, zip);
    set_message("Saved " + zip.string() + ". Drag it into the issue form your browser opened.");
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
