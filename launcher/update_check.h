// Asking GitHub for the latest release (docs/port/LAUNCHER.md, "Updates"), on a thread of its own so the
// launcher and the game never wait for the network. The answer goes to update.json, which the game reads to offer
// the update at its title. The rules (how often, which release to tell about)
// are port/platform/update_notice.c, shared with the game's overlay.
#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

extern "C" {
#include "port/update_notice.h"
}

namespace launcher {

class Platform;
struct DataPaths;
struct LauncherChoices;

class UpdateCheck {
public:
    UpdateCheck() = default;
    UpdateCheck(const UpdateCheck &) = delete;
    UpdateCheck &operator=(const UpdateCheck &) = delete;
    ~UpdateCheck(); // waits for the thread

    // starts the check: the cache at once, GitHub when due. Main thread.
    void start(const Platform &platform, const DataPaths &paths, const LauncherChoices &choices,
               std::string running_version);

    // waits up to `ms` for the check to finish (before the launcher exits)
    void finish_within(int ms);

private:
    void run(const Platform &platform, std::string cache_path); // the check's thread

    mutable std::mutex mutex_;
    UpdateRelease release_{};
    std::string running_version_;
    std::atomic<bool> done_{false};
    std::thread thread_;
};

} // namespace launcher
