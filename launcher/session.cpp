// One run of the launcher (session.h)
#include "session.h"

#include <SDL3/SDL.h>

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
#include "imgui.h"
#include "launcher_files.h"
#include "platform.h"
#include "product.h"
#include "update_check.h"
#include "window.h"

#ifndef DARKER_REALITY_VERSION
#define DARKER_REALITY_VERSION ""
#endif

namespace launcher {

const char *launcher_version() {
    return DARKER_REALITY_VERSION;
}

namespace {

constexpr std::size_t kLogLinesShown = 14;
constexpr int kUpdateWaitMs = 3000;

constexpr ImVec4 kGold(1.0f, 0.82f, 0.3f, 1.0f);
constexpr ImVec4 kRed(1.0f, 0.45f, 0.4f, 1.0f);
constexpr ImVec4 kDim(0.65f, 0.68f, 0.65f, 1.0f);

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

enum class Screen { choose_disc, building, build_failed, home, play, quit };

class Session {
public:
    Session(const Platform &platform, DataPaths paths, const LaunchOptions &options)
        : platform_(platform), paths_(std::move(paths)), options_(options) {}

    int run();
    int build_only();

private:
    // what is needed before play; the screen to show, or play when nothing is
    Screen first_screen();
    bool build_ready() const;
    fs::path build_dir() const { return paths_.builds / key_; }
    fs::path game_path() const { return build_dir() / platform_.game_program(); }

    void use_disc(const fs::path &path);
    void start_build();
    void remove_old_builds();
    int play();
    void show_game_stopped(int code);

    // a frame of each screen; returns the next screen
    Screen draw(Screen screen);
    Screen draw_choose_disc();
    Screen draw_building();
    Screen draw_build_failed();
    Screen draw_home();
    void draw_menu_bar();
    void draw_update_window();

    const Platform &platform_;
    DataPaths paths_;
    LaunchOptions options_;
    LauncherChoices choices_;
    UpdateCheck updates_;
    fs::path content_;
    std::string key_;
    std::string disc_error_;
    std::string typed_disc_;
    bool update_window_ = false;
    bool update_window_shown_ = false;
    bool update_hidden_ = false;
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
    updates_.start(platform_, paths_, choices_, launcher_version());
    content_ = platform_.content_dir();
    std::string why;
    auto tools = platform_.toolchain(content_, why);
    key_ = build_key(launcher_version(), manifest_of(content_), tools ? tools->id : "none");
    if (!options_.disc.empty()) {
        use_disc(fs::u8path(options_.disc));
    } else if (!choices_.disc.empty()) {
        use_disc(fs::u8path(choices_.disc));
        if (!disc_error_.empty())
            disc_error_ = "The disc you chose last time is gone or changed. " + disc_error_;
    }

    Screen screen = first_screen();
    if (screen != Screen::play) {
        std::string title = std::string(kProductName) + " " + (launcher_version()[0] ? launcher_version() : "(developer's build)");
        auto window = Window::open(title, why);
        if (!window) {
            std::fprintf(stderr, "[%s] cannot open the launcher's window: %s\n", kProductId, why.c_str());
            return 1;
        }
        if (screen == Screen::building)
            start_build();
        while (screen != Screen::play && screen != Screen::quit) {
            if (!window->begin_frame()) {
                if (build_)
                    build_->cancel();
                screen = Screen::quit;
            } else {
                window->begin_panel();
                screen = draw(screen);
                window->end_panel();
                draw_update_window();
                window->end_frame();
            }
        }
        if (build_thread_.joinable())
            build_thread_.join();
    }
    int code = screen == Screen::play ? play() : 0;
    updates_.finish_within(kUpdateWaitMs);
    return code;
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

Screen Session::first_screen() {
    if (!disc_error_.empty() || choices_.disc.empty())
        return Screen::choose_disc;
    if (options_.show_launcher)
        return Screen::home;
    return build_ready() ? Screen::play : Screen::building;
}

bool Session::build_ready() const {
    std::error_code error;
    return fs::is_regular_file(game_path(), error);
}

void Session::use_disc(const fs::path &path) {
    DiscCheck check = check_disc(path);
    if (check.verdict != DiscVerdict::ok) {
        disc_error_ = check.message;
        typed_disc_ = path.u8string();
        return;
    }
    disc_error_.clear();
    std::string meant = disc_meant(path).u8string();
    if (choices_.disc != meant) {
        choices_.disc = meant;
        write_choices(paths_.choices(), choices_);
    }
}

void Session::start_build() {
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
    bool open = true;
    while (open && window->begin_frame()) {
        window->begin_panel();
        ImGui::BeginMenuBar();
        ImGui::TextUnformatted(kProductName);
        ImGui::EndMenuBar();
        ImGui::TextColored(kRed, "The game stopped unexpectedly (%s).", platform_.describe_exit(code).c_str());
        ImGui::TextWrapped("Its last messages are below; all of them are in %s. Please include that file in a "
                           "bug report.", paths_.game_log().u8string().c_str());
        ImGui::BeginChild("log", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
        for (const std::string &line : tail)
            ImGui::TextUnformatted(line.c_str());
        ImGui::EndChild();
        if (ImGui::Button("Open the log's folder"))
            SDL_OpenURL(("file:///" + paths_.data.generic_u8string()).c_str());
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            open = false;
        window->end_panel();
        window->end_frame();
    }
}

Screen Session::draw(Screen screen) {
    draw_menu_bar();
    switch (screen) {
    case Screen::choose_disc:
        return draw_choose_disc();
    case Screen::building:
        return draw_building();
    case Screen::build_failed:
        return draw_build_failed();
    case Screen::home:
        return draw_home();
    case Screen::play:
    case Screen::quit:
        break;
    }
    return screen;
}

void Session::draw_menu_bar() {
    if (!ImGui::BeginMenuBar())
        return;
    ImGui::TextUnformatted(kProductName);
    ImGui::TextColored(kDim, "%s", launcher_version()[0] ? launcher_version() : "developer's build");
    auto notice = updates_.notice();
    if (notice && !update_hidden_) {
        if (!update_window_shown_) {
            update_window_ = true; // once per start, as Verdite's
            update_window_shown_ = true;
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("Update available!").x - 40);
        ImGui::PushStyleColor(ImGuiCol_Text, kGold);
        if (ImGui::BeginMenu("Update available!")) {
            ImGui::PopStyleColor();
            if (ImGui::MenuItem("Open download page"))
                SDL_OpenURL(notice->url);
            if (ImGui::MenuItem("Skip this version")) {
                updates_.skip();
                choices_.skipped = notice->tag;
                write_choices(paths_.choices(), choices_);
            }
            if (ImGui::MenuItem("Hide until next start"))
                update_hidden_ = true;
            ImGui::EndMenu();
        } else {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndMenuBar();
}

void Session::draw_update_window() {
    auto notice = updates_.notice();
    if (!update_window_ || !notice || update_hidden_)
        return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Update available", &update_window_,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%s %s is out. You have %s.", kProductName, notice->tag, launcher_version());
    ImGui::TextColored(kDim, "Download it from the release page and install it over this one.");
    ImGui::TextColored(kDim, "Your settings and saves are kept.");
    if (ImGui::Button("Download page")) {
        SDL_OpenURL(notice->url);
        update_window_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip this version")) {
        updates_.skip();
        choices_.skipped = notice->tag;
        write_choices(paths_.choices(), choices_);
        update_window_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Later"))
        update_window_ = false;
    ImGui::End();
}

Screen Session::draw_choose_disc() {
    ImGui::TextUnformatted("Choose your King's Field IV disc");
    ImGui::TextWrapped("%s plays the game from your own copy of the disc: King's Field: The Ancient City, "
                       "the USA release (SLUS-20318). Choose an ISO or CHD image of it, or the SYSTEM.CNF of a "
                       "disc you copied into a folder.", kProductName);
    if (!disc_error_.empty())
        ImGui::TextColored(kRed, "%s", disc_error_.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Choose the disc...")) {
        if (auto chosen = platform_.pick_disc())
            use_disc(*chosen);
    }
    ImGui::Spacing();
    ImGui::TextColored(kDim, "Or type its path:");
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "%s", typed_disc_.c_str());
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Use this").x - 40);
    if (ImGui::InputText("##path", buffer, sizeof(buffer)))
        typed_disc_ = buffer;
    ImGui::SameLine();
    if (ImGui::Button("Use this") && !typed_disc_.empty())
        use_disc(fs::u8path(typed_disc_));
    if (!disc_error_.empty() || choices_.disc.empty())
        return Screen::choose_disc;
    if (options_.show_launcher)
        return Screen::home;
    if (build_ready())
        return Screen::play;
    start_build();
    return Screen::building;
}

Screen Session::draw_building() {
    BuildRun::View view = build_->view();
    if (view.result) {
        if (build_thread_.joinable())
            build_thread_.join();
        if (!view.result->ok)
            return Screen::build_failed;
        remove_old_builds();
        return options_.show_launcher ? Screen::home : Screen::play;
    }
    ImGui::TextUnformatted("Building the game on your computer");
    ImGui::TextWrapped("This happens once for each release and takes a few minutes. The game's code is "
                       "compiled here from source, so no part of the game is downloaded.");
    ImGui::Spacing();
    ImGui::TextUnformatted(view.stage.c_str());
    if (view.total > 0) {
        char label[32];
        std::snprintf(label, sizeof(label), "%d / %d", view.done, view.total);
        ImGui::ProgressBar((float)view.done / (float)view.total, ImVec2(-1, 0), label);
    } else {
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "");
    }
    ImGui::BeginChild("log", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    for (const std::string &line : view.tail)
        ImGui::TextColored(kDim, "%s", line.c_str());
    ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (ImGui::Button("Cancel")) {
        build_->cancel();
        if (build_thread_.joinable())
            build_thread_.join();
        return Screen::quit;
    }
    return Screen::building;
}

Screen Session::draw_build_failed() {
    BuildRun::View view = build_->view();
    ImGui::TextColored(kRed, "The game could not be built.");
    ImGui::TextWrapped("%s", view.result ? view.result->message.c_str() : "");
    ImGui::TextWrapped("The whole build's output is in %s. Please include that file in a bug report.",
                       paths_.build_log().u8string().c_str());
    ImGui::BeginChild("log", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
    for (const std::string &line : view.tail)
        ImGui::TextUnformatted(line.c_str());
    ImGui::EndChild();
    if (ImGui::Button("Try again")) {
        start_build();
        return Screen::building;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy the log")) {
        if (auto text = read_file(paths_.build_log()))
            SDL_SetClipboardText(text->c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Open the log's folder"))
        SDL_OpenURL(("file:///" + paths_.data.generic_u8string()).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Close"))
        return Screen::quit;
    return Screen::build_failed;
}

Screen Session::draw_home() {
    ImGui::TextUnformatted("King's Field IV");
    ImGui::TextColored(kDim, "Disc: %s", choices_.disc.c_str());
    ImGui::TextColored(kDim, build_ready() ? "Built for this release." : "Not built yet: Play builds it first.");
    ImGui::Spacing();
    if (ImGui::Button("Play", ImVec2(160 * ImGui::GetIO().FontGlobalScale, 0))) {
        if (build_ready())
            return Screen::play;
        options_.show_launcher = false; // after the build, play
        start_build();
        return Screen::building;
    }
    ImGui::SameLine();
    if (ImGui::Button("Change disc")) {
        typed_disc_ = choices_.disc;
        choices_.disc.clear();
        return Screen::choose_disc;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rebuild")) {
        start_build();
        return Screen::building;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open data folder"))
        SDL_OpenURL(("file:///" + paths_.data.generic_u8string()).c_str());
    ImGui::Spacing();
    if (ImGui::Checkbox("Check for updates at launch", &choices_.update_check))
        write_choices(paths_.choices(), choices_);
    return Screen::home;
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
