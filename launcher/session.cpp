// One run of the launcher (session.h)
#include "session.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>

#include "build_key.h"
#include "build_strategy.h"
#include "disc_check.h"
#include "launcher_files.h"
#include "platform.h"
#include "product.h"
#include "rml_text.h"
#include "self_update.h"
#include "setup_flow.h"
#include "setup_pages.h"
#include "update_check.h"
#include "window.h"

#ifndef HOLYLAND_VERSION
#define HOLYLAND_VERSION ""
#endif

namespace launcher {

const char *launcher_version() {
    return HOLYLAND_VERSION;
}

namespace {

constexpr std::size_t kLogLinesShown = 14;
constexpr int kUpdateWaitMs = 3000;


// The build's progress, written by the build's thread and read by the window
class BuildRun final : public BuildProgress {
public:
    explicit BuildRun(const fs::path &log_path) : log_(log_path, std::ios::trunc) {}

    void stage(std::string_view what) override {
        std::lock_guard<std::mutex> lock(mutex_);
        stage_ = what;
        done_steps_ = total_steps_ = 0;
    }
    void steps(int done, int total) override {
        std::lock_guard<std::mutex> lock(mutex_);
        done_steps_ = done;
        total_steps_ = total;
    }
    void log_line(std::string_view line) override {
        std::lock_guard<std::mutex> lock(mutex_);
        log_ << line << '\n';
        tail_.emplace_back(line);
        if (tail_.size() > kLogLinesShown)
            tail_.pop_front();
    }
    bool cancelled() const override { return cancelled_; }
    void running(ChildProcess *process) override {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = process;
        if (process_ != nullptr && cancelled_)
            process_->stop();
    }

    // main thread
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        if (process_ != nullptr)
            process_->stop();
    }
    void finish(BuildResult result) {
        std::lock_guard<std::mutex> lock(mutex_);
        log_ << (result.ok ? "[launcher] the build is done" : "[launcher] " + result.message) << '\n';
        log_.flush();
        result_ = std::move(result);
    }
    struct View {
        std::string stage;
        int done, total;
        std::deque<std::string> tail;
        std::optional<BuildResult> result;
    };
    View view() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return View{stage_, done_steps_, total_steps_, tail_, result_};
    }

private:
    mutable std::mutex mutex_;
    std::ofstream log_;
    std::string stage_ = "Starting";
    int done_steps_ = 0, total_steps_ = 0;
    std::deque<std::string> tail_;
    std::atomic<bool> cancelled_{false};
    ChildProcess *process_ = nullptr;
    std::optional<BuildResult> result_;
};

// The build's progress on the console (--build-only), from the build's own thread
class ConsoleBuild final : public BuildProgress {
public:
    explicit ConsoleBuild(const fs::path &log_path) : log_(log_path, std::ios::trunc) {}
    void stage(std::string_view what) override { std::fprintf(stderr, "[%s] %.*s\n", kProductId, (int)what.size(), what.data()); }
    void steps(int done, int total) override {
        int tenth = done * 10 / total;
        if (tenth != last_tenth_) {
            last_tenth_ = tenth;
            std::fprintf(stderr, "[%s] %d / %d\n", kProductId, done, total);
        }
    }
    void log_line(std::string_view line) override { log_ << line << '\n'; }
    bool cancelled() const override { return false; }
    void running(ChildProcess *) override {}

private:
    std::ofstream log_;
    int last_tenth_ = -1;
};

// play, quit and restarted end the window: the game, nothing, or the new release started (this launcher exits)
enum class Screen { setup, building, build_failed, home, updating, update_failed, play, quit, restarted };

class Session final : private DiscKeeper {
public:
    Session(const Platform &platform, DataPaths paths, const LaunchOptions &options)
        : platform_(platform), paths_(std::move(paths)), options_(options) {}

    int run();
    int build_only();

private:
    // what is needed before play; the screen to show, or play when nothing is
    Screen first_screen();
    // the setup's pages this start needs: the disc when there is none to use, the options before the first game
    std::vector<std::unique_ptr<SetupPage>> setup_pages();
    // after the setup: on to the build, the game, or the launcher's home
    Screen after_setup();
    bool build_ready() const;
    fs::path build_dir() const { return paths_.builds / key_; }
    fs::path game_path() const { return build_dir() / platform_.game_program(); }

    // DiscKeeper, for the setup's disc page
    bool keep_disc(const fs::path &disc) override;
    const std::string &why_not() const override { return disc_error_; }
    std::optional<fs::path> pick_disc_the_old_way() override { return platform_.pick_disc(); }

    void start_build();
    void remove_old_builds();
    // the window from `screen` until it closes; play, quit or restarted
    Screen show_window(Screen screen);
    int play();
    // an update to `tag` begun (self_update.h); a build going is stopped
    void start_update(std::string tag);
    void show_game_stopped(int code);

    // a frame of each screen; returns the next screen
    Screen draw(Screen screen, Window &window);
    Screen draw_setup(Window &window);
    Screen draw_building(Window &window);
    Screen draw_build_failed(Window &window);
    Screen draw_home(Window &window);
    Screen draw_updating(Window &window);
    Screen draw_update_failed(Window &window);

    const Platform &platform_;
    DataPaths paths_;
    LaunchOptions options_;
    LauncherChoices choices_;
    UpdateCheck updates_;
    fs::path content_;
    std::string key_;
    std::string disc_error_;
    std::unique_ptr<SetupFlow> setup_;
    std::unique_ptr<Installation> installation_; // nullptr when this copy cannot update itself (why: no_update_why_)
    std::string no_update_why_;
    std::string update_tag_;
    std::unique_ptr<UpdateRun> update_;
    std::string update_why_; // update_failed: why
    std::unique_ptr<BuildRun> build_;
    std::thread build_thread_;
};

// the text of every line of the release's manifest, or "" in a developer's content
std::string manifest_of(const fs::path &content) {
    return read_file(content / "manifest.txt").value_or("");
}

int Session::run() {
    choices_ = read_choices(paths_.choices());
    std::error_code error;
    fs::create_directories(paths_.data, error);
    installation_ = platform_.installation(no_update_why_);
    if (installation_)
        installation_->remove_leftovers();
    updates_.start(platform_, paths_, choices_, launcher_version());
    content_ = platform_.content_dir();
    std::string why;
    auto tools = platform_.toolchain(content_, why);
    key_ = build_key(launcher_version(), manifest_of(content_), tools ? tools->id : "none");
    if (!options_.disc.empty()) {
        keep_disc(fs::u8path(options_.disc));
    } else if (!choices_.disc.empty()) {
        if (!keep_disc(fs::u8path(choices_.disc)))
            disc_error_ = "The disc you chose last time is gone or changed. " + disc_error_;
    }

    Screen screen = first_screen();
    // (an ask the last launcher did not see: it was ended while the game ran)
    if (auto tag = take_update_request(paths_)) {
        start_update(*tag);
        screen = Screen::updating;
    }
    int code = 0;
    for (;;) {
        if (screen != Screen::play)
            screen = show_window(screen);
        if (screen != Screen::play)
            break;
        code = play();
        // the player chose Update in the game, which then quit
        auto tag = take_update_request(paths_);
        if (!tag)
            break;
        start_update(*tag);
        screen = Screen::updating;
    }
    updates_.finish_within(kUpdateWaitMs);
    return screen == Screen::restarted ? 0 : code;
}

Screen Session::show_window(Screen screen) {
    std::string why;
    std::string title = std::string(kProductName) + " " + (launcher_version()[0] ? launcher_version() : "(developer's build)");
    auto window = Window::open(title, why);
    if (!window) {
        std::fprintf(stderr, "[%s] cannot open the launcher's window: %s\n", kProductId, why.c_str());
        return Screen::quit;
    }
    // (the build needs no disc: it runs while the player goes through the setup)
    if (screen == Screen::building || (screen == Screen::setup && !build_ready()))
        start_build();
    while (screen != Screen::play && screen != Screen::quit && screen != Screen::restarted) {
        if (!window->begin_frame()) {
            if (build_)
                build_->cancel();
            screen = Screen::quit;
        } else {
            screen = draw(screen, *window);
            window->end_frame();
        }
    }
    if (build_thread_.joinable())
        build_thread_.join();
    return screen;
}

int Session::build_only() {
    std::error_code error;
    fs::create_directories(paths_.data, error);
    content_ = platform_.content_dir();
    std::string why;
    auto tools = platform_.toolchain(content_, why);
    key_ = build_key(launcher_version(), manifest_of(content_), tools ? tools->id : "none");
    if (build_ready()) {
        std::fprintf(stderr, "[%s] already built: %s\n", kProductId, game_path().u8string().c_str());
        return 0;
    }
    fs::remove_all(build_dir(), error);
    ConsoleBuild progress(paths_.build_log());
    BuildResult result =
        platform_.build_strategy().build(platform_, BuildRequest{content_, build_dir(), launcher_version()}, progress);
    if (!result.ok) {
        std::fprintf(stderr, "[%s] %s (the whole output: %s)\n", kProductId, result.message.c_str(),
                     paths_.build_log().u8string().c_str());
        return 1;
    }
    remove_old_builds();
    std::fprintf(stderr, "[%s] built: %s\n", kProductId, game_path().u8string().c_str());
    return 0;
}

// "Holyland 0.2.0", or "Holyland (developer's build)"
std::string version_line() {
    return std::string(kProductName) + (launcher_version()[0] ? std::string(" ") + launcher_version() : " (developer's build)");
}

// a log's lines as paragraphs
template <typename Lines> std::string log_rml(const Lines &lines) {
    std::string rml;
    for (const std::string &line : lines)
        rml += "<p>" + rml_escaped(line) + "</p>";
    return rml;
}

void open_folder(const fs::path &folder) {
    if (!SDL_OpenURL(("file:///" + folder.generic_u8string()).c_str()))
        std::fprintf(stderr, "[%s] cannot open %s: %s\n", kProductId, folder.u8string().c_str(), SDL_GetError());
}

Screen Session::first_screen() {
    setup_ = std::make_unique<SetupFlow>(setup_pages());
    if (!setup_->finished())
        return Screen::setup;
    if (options_.show_launcher)
        return Screen::home;
    return build_ready() ? Screen::play : Screen::building;
}

// The pages, in order: a new page is a line here (setup_pages.h)
std::vector<std::unique_ptr<SetupPage>> Session::setup_pages() {
    std::vector<std::unique_ptr<SetupPage>> pages;
    if (!disc_error_.empty() || choices_.disc.empty())
        pages.push_back(make_disc_page(*this));
    if (!choices_.setup_done) {
        // the options to pick before the first game: a line each, such as pages.push_back(make_..._page(...));
    }
    return pages;
}

Screen Session::after_setup() {
    setup_.reset();
    if (!choices_.setup_done) {
        choices_.setup_done = true;
        write_choices(paths_.choices(), choices_);
    }
    if (options_.show_launcher)
        return Screen::home;
    if (build_) // started with the setup: done, failed or still going, its screen says which
        return Screen::building;
    if (build_ready())
        return Screen::play;
    start_build();
    return Screen::building;
}

bool Session::build_ready() const {
    std::error_code error;
    return fs::is_regular_file(game_path(), error);
}

bool Session::keep_disc(const fs::path &path) {
    DiscCheck check = check_disc(path);
    if (check.verdict != DiscVerdict::ok) {
        disc_error_ = check.message;
        return false;
    }
    disc_error_.clear();
    std::string meant = disc_meant(path).u8string();
    if (choices_.disc != meant) {
        choices_.disc = meant;
        write_choices(paths_.choices(), choices_);
    }
    return true;
}

void Session::start_build() {
    if (build_thread_.joinable()) { // a build already going (Rebuild from home while the setup's runs): it starts over
        build_->cancel();
        build_thread_.join();
    }
    std::error_code error;
    fs::remove_all(build_dir(), error);
    build_ = std::make_unique<BuildRun>(paths_.build_log());
    BuildRequest request{content_, build_dir(), launcher_version()};
    BuildRun *run = build_.get();
    const Platform &platform = platform_;
    // the build's thread
    build_thread_ = std::thread([run, request, &platform] {
        BuildResult result = platform.build_strategy().build(platform, request, *run);
        run->finish(std::move(result));
    });
}

void Session::remove_old_builds() {
    std::error_code error;
    for (fs::directory_iterator it(paths_.builds, error), end; !error && it != end; it.increment(error)) {
        if (it->path().filename() != key_ && it->is_directory(error)) {
            std::error_code ignored;
            fs::remove_all(it->path(), ignored);
        }
    }
}

int Session::play() {
    ProcessSpec spec;
    spec.argv = {game_path().string(), "--disc", choices_.disc};
    spec.argv.insert(spec.argv.end(), options_.game_args.begin(), options_.game_args.end());
    // the player's working folder, so that paths given after "--" mean what they meant there
    spec.capture_output = true;
    spec.stop_with_children = false;
    std::string why;
    auto game = platform_.start(spec, why);
    if (!game) {
        std::fprintf(stderr, "[%s] cannot start the game: %s\n", kProductId, why.c_str());
        return 1;
    }
    // the game's messages: on the launcher's console, and in game.log for a bug report
    std::ofstream log(paths_.game_log(), std::ios::trunc);
    while (auto line = game->read_line()) {
        std::fprintf(stdout, "%s\n", line->c_str());
        log << *line << '\n';
    }
    int code = game->wait();
    if (code != 0) {
        // (in the log too: a crash the game cannot report itself, a stack overflow or a fast fail, leaves only this)
        std::string stopped = std::string("[") + kProductId + "] the game stopped: " + platform_.describe_exit(code);
        std::fprintf(stdout, "%s\n", stopped.c_str());
        log << stopped << '\n';
    }
    std::fflush(stdout);
    log.close();
    if (code != 0)
        show_game_stopped(code);
    return code;
}

void Session::show_game_stopped(int code) {
    std::string why;
    auto window = Window::open(kProductName, why);
    if (!window)
        return;
    std::deque<std::string> tail;
    if (auto text = read_file(paths_.game_log())) {
        std::size_t at = 0;
        while (at < text->size()) {
            std::size_t end = text->find('\n', at);
            if (end == std::string::npos)
                end = text->size();
            tail.emplace_back(text->substr(at, end - at));
            if (tail.size() > kLogLinesShown)
                tail.pop_front();
            at = end + 1;
        }
    }
    while (window->begin_frame()) {
        window->show_page("stopped.rml");
        window->set_text("why", "(" + platform_.describe_exit(code) + ")");
        window->set_rml("log", log_rml(tail));
        window->scroll_to_end("log");
        if (window->take_action("open-folder"))
            open_folder(paths_.data);
        if (window->take_action("close"))
            break;
        window->end_frame();
    }
}

Screen Session::draw(Screen screen, Window &window) {
    Screen next = screen;
    switch (screen) {
    case Screen::setup:
        next = draw_setup(window);
        break;
    case Screen::building:
        next = draw_building(window);
        break;
    case Screen::build_failed:
        next = draw_build_failed(window);
        break;
    case Screen::home:
        next = draw_home(window);
        break;
    case Screen::updating:
        next = draw_updating(window);
        break;
    case Screen::update_failed:
        next = draw_update_failed(window);
        break;
    case Screen::play:
    case Screen::quit:
    case Screen::restarted:
        break;
    }
    return next;
}

// The logo, the page, and at the foot the version, the page's place in the row and the way back
Screen Session::draw_setup(Window &window) {
    window.show_page("setup.rml");
    if (window.take_action("back"))
        setup_->back();
    setup_->answer(setup_->page().draw(window));
    if (setup_->finished())
        return after_setup();
    window.set_text("version", version_line());
    std::string dots;
    for (std::size_t i = 0; setup_->page_count() > 1 && i < setup_->page_count(); i++)
        dots += i == setup_->page_number() ? "<span class=\"dot here\"></span>" : "<span class=\"dot\"></span>";
    window.set_rml("dots", dots);
    window.set_classes("back", setup_->can_go_back() ? "button" : "button hidden");
    return Screen::setup;
}

Screen Session::draw_building(Window &window) {
    BuildRun::View view = build_->view();
    if (view.result) {
        if (build_thread_.joinable())
            build_thread_.join();
        if (!view.result->ok)
            return Screen::build_failed;
        remove_old_builds();
        return options_.show_launcher ? Screen::home : Screen::play;
    }
    if (window.take_action("cancel")) {
        build_->cancel();
        if (build_thread_.joinable())
            build_thread_.join();
        return Screen::quit;
    }
    window.show_page("building.rml");
    if (view.total > 0) {
        window.set_text("stage", view.stage + ": " + std::to_string(view.done) + " of " + std::to_string(view.total));
        window.set_rml("bar", "<div style=\"width: " + std::to_string(view.done * 100 / view.total) + "%\"></div>");
    } else {
        window.set_text("stage", view.stage);
        window.set_rml("bar", "");
    }
    window.set_rml("log", log_rml(view.tail));
    window.scroll_to_end("log");
    return Screen::building;
}

Screen Session::draw_build_failed(Window &window) {
    BuildRun::View view = build_->view();
    if (window.take_action("try-again")) {
        start_build();
        return Screen::building;
    }
    if (window.take_action("copy-log")) {
        if (auto text = read_file(paths_.build_log()))
            SDL_SetClipboardText(text->c_str());
    }
    if (window.take_action("open-folder"))
        open_folder(paths_.data);
    if (window.take_action("close"))
        return Screen::quit;
    window.show_page("failed.rml");
    window.set_text("message", view.result ? view.result->message : "");
    window.set_rml("log", log_rml(view.tail));
    window.scroll_to_end("log");
    return Screen::build_failed;
}

Screen Session::draw_home(Window &window) {
    if (window.take_action("play")) {
        if (build_ready())
            return Screen::play;
        options_.show_launcher = false; // after the build, play
        if (!build_thread_.joinable()) // (else the setup's build is going, or done and not yet seen)
            start_build();
        return Screen::building;
    }
    if (window.take_action("change-disc")) {
        std::vector<std::unique_ptr<SetupPage>> disc_only;
        disc_only.push_back(make_disc_page(*this));
        setup_ = std::make_unique<SetupFlow>(std::move(disc_only));
        return Screen::setup;
    }
    if (window.take_action("rebuild")) {
        start_build();
        return Screen::building;
    }
    if (window.take_action("open-folder"))
        open_folder(paths_.data);
    if (window.take_action("toggle-updates")) {
        choices_.update_check = !choices_.update_check;
        write_choices(paths_.choices(), choices_);
    }
    window.show_page("home.rml");
    window.set_text("disc", "Disc: " + choices_.disc);
    window.set_text("updates-check", choices_.update_check ? "Check for updates: on" : "Check for updates: off");
    window.set_text("version", version_line());
    return Screen::home;
}

void Session::start_update(std::string tag) {
    if (build_thread_.joinable()) { // (its result is not needed: the new release builds its own)
        build_->cancel();
        build_thread_.join();
    }
    build_.reset();
    update_.reset();
    update_tag_ = std::move(tag);
    update_why_.clear();
    if (!installation_) {
        update_why_ = "This copy of Holyland cannot update itself: " + no_update_why_ + ".";
        return;
    }
    std::fprintf(stderr, "[%s] updating to %s\n", kProductId, update_tag_.c_str());
    std::string agent = std::string(kProductId) + "-launcher/" + (launcher_version()[0] ? launcher_version() : "dev");
    update_ = std::make_unique<UpdateRun>(platform_, *installation_, update_tag_, launcher_version(), agent);
}

// "12.3 MB"
std::string megabytes(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return text;
}

Screen Session::draw_updating(Window &window) {
    if (!update_)
        return Screen::update_failed;
    UpdateRun::View view = update_->view();
    if (view.state == UpdateRun::State::failed) {
        update_why_ = view.why;
        std::fprintf(stderr, "[%s] the update stopped: %s\n", kProductId, update_why_.c_str());
        return Screen::update_failed;
    }
    if (view.state == UpdateRun::State::ready) {
        std::string why = update_->install(options_.args);
        if (why.empty()) {
            std::fprintf(stderr, "[%s] %s is in place; starting it\n", kProductId, update_tag_.c_str());
            return Screen::restarted;
        }
        update_why_ = why;
        std::fprintf(stderr, "[%s] the update could not be put in place: %s\n", kProductId, why.c_str());
        return Screen::update_failed;
    }
    if (window.take_action("cancel")) {
        update_.reset();
        return Screen::home;
    }
    window.show_page("updating.rml");
    window.set_text("heading", std::string("Updating to ") + kProductName + " " + update_tag_);
    if (view.total_bytes > 0) {
        window.set_text("stage", view.stage + ": " + megabytes(view.done_bytes) + " of " + megabytes(view.total_bytes));
        window.set_rml("bar", "<div style=\"width: " + std::to_string(view.done_bytes * 100 / view.total_bytes) + "%\"></div>");
    } else {
        window.set_text("stage", view.done_bytes > 0 ? view.stage + ": " + megabytes(view.done_bytes) : view.stage);
        window.set_rml("bar", "");
    }
    return Screen::updating;
}

Screen Session::draw_update_failed(Window &window) {
    if (window.take_action("try-again")) {
        start_update(update_tag_);
        return Screen::updating;
    }
    if (window.take_action("download-page")) {
        std::string page = "https://github.com/" UPDATE_REPOSITORY "/releases/tag/" + update_tag_;
        if (!SDL_OpenURL(page.c_str()))
            std::fprintf(stderr, "[%s] cannot open %s: %s\n", kProductId, page.c_str(), SDL_GetError());
    }
    if (window.take_action("play")) {
        update_.reset();
        if (build_ready())
            return Screen::play;
        options_.show_launcher = false;
        start_build();
        return Screen::building;
    }
    if (window.take_action("close"))
        return Screen::quit;
    window.show_page("update_failed.rml");
    window.set_text("message", update_why_);
    return Screen::update_failed;
}

} // namespace

int run_session(const LaunchOptions &options) {
    const Platform &platform = this_platform();
    platform.attach_console();
    std::fprintf(stderr, "[%s] %s\n", kProductId, launcher_version()[0] ? launcher_version() : "developer's build");
    auto paths = data_paths();
    if (!paths) {
        std::fprintf(stderr, "[%s] the data folder's path is too long (KF4_DATA_DIR, APPDATA or HOME)\n", kProductId);
        return 1;
    }
    Session session(platform, *paths, options);
    return options.build_only ? session.build_only() : session.run();
}

} // namespace launcher
