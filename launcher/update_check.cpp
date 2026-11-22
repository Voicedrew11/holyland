// The update check's thread (update_check.h)
#include "update_check.h"

#include <chrono>
#include <cstdlib>
#include <ctime>

#include "launcher_files.h"
#include "platform.h"
#include "product.h"

namespace launcher {

namespace {

std::string api_url() {
    // a test answer instead of GitHub's (docs/port/LAUNCHER.md, "Testing the notice")
    if (const char *url = std::getenv("KF4_UPDATE_URL"); url != nullptr && *url != 0)
        return url;
    return "https://api.github.com/repos/" UPDATE_REPOSITORY "/releases/latest";
}

std::int64_t now_s() {
    return (std::int64_t)std::time(nullptr);
}

} // namespace

UpdateCheck::~UpdateCheck() {
    if (thread_.joinable())
        thread_.join();
}

void UpdateCheck::start(const Platform &platform, const DataPaths &paths, const LauncherChoices &choices,
                        std::string running_version) {
    std::string cache_path = paths.update_cache().string();
    UpdateRelease cached{};
    auto text = read_file(paths.update_cache());
    update_cache_parse(text ? text->c_str() : nullptr, &cached);
    UpdateCheckMode mode = update_check_mode(std::getenv("KF4_UPDATE_CHECK"), choices.update_check ? 1 : 0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release_ = cached;
        running_version_ = std::move(running_version);
    }
    if (mode == UPDATE_CHECK_OFF || !update_check_due(mode, &cached, now_s())) {
        done_ = true;
        return;
    }
    thread_ = std::thread([this, &platform, cache_path] { run(platform, cache_path); });
}

// the update check's thread
void UpdateCheck::run(const Platform &platform, std::string cache_path) {
    std::string agent = std::string(kProductId) + "-launcher/" + (running_version_.empty() ? std::string("dev") : running_version_);
    auto answer = platform.http_get(api_url(), agent, 10000);
    UpdateRelease fresh{};
    if (answer && update_release_parse(answer->c_str(), now_s(), &fresh)) {
        char text[1024];
        if (update_cache_format(&fresh, text, sizeof(text)))
            write_file_atomically(fs::u8path(cache_path), std::string(text) + "\n");
        std::lock_guard<std::mutex> lock(mutex_);
        release_ = fresh;
    }
    // no answer: the cached one stands, and the next start asks again
    done_ = true;
}

void UpdateCheck::finish_within(int ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done_ && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

} // namespace launcher
