// Updating Holyland in place (docs/port/LAUNCHER.md, "Updates"), as OpenCE's updater does: the release's file for
// this way of installing (platform.h's Installation) is downloaded, checked against the release's SHA256SUMS, and
// SHA256SUMS against its Ed25519 signature by the project's release key; then put in place, and the new launcher
// started. The game only asks (update-request.json, port/include/port/update_notice.h); the launcher does it once
// the game has quit. The rules are port/platform/update_notice.c's, shared with the game.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "platform.h"

namespace launcher {

struct DataPaths;

using ReleaseKey = std::array<std::uint8_t, 32>; // an Ed25519 public key

// Where an update comes from: GitHub's release, or a test's (KF4_UPDATE_URL with KF4_UPDATE_KEY)
struct UpdateSource {
    std::string release_url; // GitHub's answer about the one release (.../releases/tags/<tag>)
    std::string url_prefix;  // what every file's URL in it must start with
    ReleaseKey key;          // what SHA256SUMS must be signed with
};
UpdateSource update_source(std::string_view tag);

// update-request.json: the release the game asked for (the player chose Update). Read and removed; nullopt when
// there is none.
std::optional<std::string> take_update_request(const DataPaths &paths);

// What an update is doing, told from its own thread
class UpdateProgress {
public:
    virtual ~UpdateProgress() = default;
    virtual void stage(std::string_view what) = 0;
    // false stops it
    virtual bool bytes(std::uint64_t done_bytes, std::uint64_t total_bytes) = 0;
};

// The release `tag`'s update for `installation`: made from the release's delta from `installed_version` when it
// has one and it works (update_delta.h), else its whole file downloaded to installation.download_path(); checked
// either way. What replace_and_start takes, or an empty path with why, with nothing left behind. Any thread.
Installation::Made fetch_checked_update(const Platform &platform, const Installation &installation,
                                        const UpdateSource &source, std::string_view tag,
                                        std::string_view installed_version, std::string_view user_agent,
                                        UpdateProgress &progress);

// One update, its download and checks on a thread of its own. The rest on the main thread.
class UpdateRun final : private UpdateProgress {
public:
    enum class State { working, ready, failed };
    struct View {
        State state;
        std::string stage;
        std::uint64_t done_bytes, total_bytes;
        std::string why; // failed: why
    };

    // `installed_version`: this launcher's (launcher_version()), whose delta is looked for; "" for none
    UpdateRun(const Platform &platform, const Installation &installation, std::string tag,
              std::string installed_version, std::string user_agent);
    UpdateRun(const UpdateRun &) = delete;
    UpdateRun &operator=(const UpdateRun &) = delete;
    ~UpdateRun(); // stops the download and waits for it

    View view() const;
    // ready: put in place and the new launcher started with `args`; "" when it has started (exit now), else why
    std::string install(const std::vector<std::string> &args);

private:
    void stage(std::string_view what) override;
    bool bytes(std::uint64_t done_bytes, std::uint64_t total_bytes) override;

    const Installation &installation_;
    mutable std::mutex mutex_;
    View view_{State::working, "Starting", 0, 0, {}};
    fs::path update_; // ready: what replace_and_start takes
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

} // namespace launcher
