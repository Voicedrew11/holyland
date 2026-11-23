// Updating Holyland in place (self_update.h)
#include "self_update.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "build_key.h"
#include "launcher_files.h"
#include "platform.h"
#include "release_key.h"
#include "update_delta.h"

extern "C" {
#include "monocypher-ed25519.h"
#include "port/update_notice.h"
}

namespace launcher {

namespace {

constexpr int kAnswerTimeoutMs = 20000;
constexpr std::uint64_t kSumsMaxBytes = 64 * 1024;

// a small file of the release (SHA256SUMS and its .sig), through a file beside the download: http_get asks for
// GitHub's API answers, and these are plain files on GitHub's file host
std::optional<std::string> fetch_small(const Platform &platform, const std::string &url, const fs::path &through,
                                       std::string_view agent) {
    std::string why = platform.http_download(url, through, kSumsMaxBytes, agent, nullptr);
    if (!why.empty())
        return std::nullopt;
    auto text = read_file(through);
    std::error_code error;
    fs::remove(through, error);
    return text;
}

bool signed_by(const ReleaseKey &key, const std::string &text, const std::string &signature_hex) {
    unsigned char signature[64];
    if (!update_hex_bytes(signature_hex.c_str(), signature, sizeof(signature)))
        return false;
    return crypto_ed25519_check(signature, key.data(), reinterpret_cast<const std::uint8_t *>(text.data()),
                                text.size()) == 0;
}

// a URL of one of the release's files, by its name's end
struct Asset {
    std::string name, url;
};
std::optional<Asset> pick(const std::string &release, std::string_view tag, const char *name_end,
                          const UpdateSource &source) {
    char name[256], url[1024];
    if (!update_asset_pick(release.c_str(), std::string(tag).c_str(), name_end, source.url_prefix.c_str(), name,
                           sizeof(name), url, sizeof(url)))
        return std::nullopt;
    return Asset{name, url};
}

void remove_quietly(const fs::path &file) {
    std::error_code error;
    fs::remove(file, error);
}

} // namespace

UpdateSource update_source(std::string_view tag) {
    // a test's release (docs/port/LAUNCHER.md, "Testing an update"): its answer, its files and its key, together
    const char *test_url = std::getenv("KF4_UPDATE_URL");
    const char *test_key = std::getenv("KF4_UPDATE_KEY");
    if (test_url != nullptr && *test_url != 0) {
        UpdateSource source{test_url, "file://", {}};
        if (test_key == nullptr || !update_hex_bytes(test_key, source.key.data(), source.key.size()))
            source.key = {}; // no key: nothing is signed by all zeros, so nothing is put in place
        return source;
    }
    return UpdateSource{"https://api.github.com/repos/" UPDATE_REPOSITORY "/releases/tags/" + std::string(tag),
                        "https://github.com/", kReleaseKey};
}

std::optional<std::string> take_update_request(const DataPaths &paths) {
    auto text = read_file(paths.update_request());
    if (!text)
        return std::nullopt;
    remove_quietly(paths.update_request());
    char tag[64];
    if (!update_request_parse(text->c_str(), tag, sizeof(tag)))
        return std::nullopt;
    return std::string(tag);
}

namespace {

// The release's delta from `installed_version` made into the update, when the release has one, this way of
// installing takes it and it works; an empty path otherwise (the whole file is downloaded then)
fs::path update_from_delta(const Platform &platform, const Installation &installation, const UpdateSource &source,
                           const std::string &release, std::string_view tag, std::string_view installed_version,
                           const Asset &whole, const std::string &sums, std::string_view whole_sha256,
                           std::string_view user_agent, UpdateProgress &progress) {
    fs::path to = installation.delta_download_path();
    if (installed_version.empty() || to.empty())
        return {};
    std::string name = delta_name(whole.name, installed_version);
    auto delta = pick(release, tag, name.c_str(), source);
    char expected[65];
    if (!delta || !update_sums_lookup(sums.c_str(), delta->name.c_str(), expected))
        return {}; // none from this version: too old, or a release made before deltas
    progress.stage("Downloading " + delta->name);
    std::string why = platform.http_download(delta->url, to, kUpdateMaxBytes, user_agent,
                                             [&progress](std::uint64_t done, std::uint64_t total) {
                                                 return progress.bytes(done, total);
                                             });
    if (why.empty() && file_sha256(to) != expected)
        why = "the delta is not the release's file (its checksum differs)";
    if (!why.empty()) {
        remove_quietly(to);
        std::fprintf(stderr, "[update] the delta did not download (%s); the whole file instead\n", why.c_str());
        return {};
    }
    progress.stage("Making the update from " + delta->name);
    Installation::Made made = installation.made_from_delta(to, whole_sha256);
    remove_quietly(to);
    if (made.update.empty())
        std::fprintf(stderr, "[update] the delta did not work (%s); the whole file instead\n", made.why.c_str());
    else
        std::fprintf(stderr, "[update] made from %s\n", delta->name.c_str());
    return made.update;
}

} // namespace

Installation::Made fetch_checked_update(const Platform &platform, const Installation &installation,
                                        const UpdateSource &source, std::string_view tag,
                                        std::string_view installed_version, std::string_view user_agent,
                                        UpdateProgress &progress) {
    auto failed = [](std::string why) { return Installation::Made{{}, std::move(why)}; };
    progress.stage("Asking GitHub about " + std::string(tag));
    auto release = platform.http_get(source.release_url, user_agent, kAnswerTimeoutMs);
    if (!release)
        return failed("GitHub could not be reached (no network?).");
    std::string suffix(installation.asset_suffix());
    auto asset = pick(*release, tag, suffix.c_str(), source);
    auto sums_asset = pick(*release, tag, UPDATE_SUMS_ASSET, source);
    auto signature_asset = pick(*release, tag, UPDATE_SIGNATURE_ASSET, source);
    if (!asset)
        return failed("The release " + std::string(tag) + " has no file for this computer (" + suffix + ").");
    if (!sums_asset || !signature_asset)
        return failed("The release " + std::string(tag) + " is not signed, so it cannot be installed from here.");

    progress.stage("Checking the release's signature");
    fs::path to = installation.download_path();
    fs::path small = to;
    small += ".sums";
    auto sums = fetch_small(platform, sums_asset->url, small, user_agent);
    auto signature = fetch_small(platform, signature_asset->url, small, user_agent);
    if (!sums || !signature)
        return failed("The release's list of checksums could not be downloaded.");
    if (!signed_by(source.key, *sums, *signature))
        return failed("The release's signature is not good: it was not made by Holyland's release key. Nothing was changed.");
    char expected[65];
    if (!update_sums_lookup(sums->c_str(), asset->name.c_str(), expected))
        return failed("The release's checksums do not list " + asset->name + ".");

    fs::path from_delta = update_from_delta(platform, installation, source, *release, tag, installed_version, *asset,
                                            *sums, expected, user_agent, progress);
    if (!from_delta.empty())
        return {from_delta, {}};

    progress.stage("Downloading " + asset->name);
    std::string why = platform.http_download(asset->url, to, kUpdateMaxBytes, user_agent,
                                             [&progress](std::uint64_t done, std::uint64_t total) {
                                                 return progress.bytes(done, total);
                                             });
    if (!why.empty())
        return failed("The download stopped: " + why);

    progress.stage("Checking the download");
    if (file_sha256(to) != expected) {
        remove_quietly(to);
        return failed("The download is not the release's file (its checksum differs). Nothing was changed.");
    }
    return {to, {}};
}

UpdateRun::UpdateRun(const Platform &platform, const Installation &installation, std::string tag,
                     std::string installed_version, std::string user_agent)
    : installation_(installation) {
    // the update's thread
    thread_ = std::thread([this, &platform, tag = std::move(tag), version = std::move(installed_version),
                           agent = std::move(user_agent)] {
        Installation::Made made =
            fetch_checked_update(platform, installation_, update_source(tag), tag, version, agent, *this);
        std::lock_guard<std::mutex> lock(mutex_);
        view_.state = made.update.empty() ? State::failed : State::ready;
        view_.why = std::move(made.why);
        update_ = std::move(made.update);
    });
}

UpdateRun::~UpdateRun() {
    stopping_ = true;
    if (thread_.joinable())
        thread_.join();
}

UpdateRun::View UpdateRun::view() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return view_;
}

std::string UpdateRun::install(const std::vector<std::string> &args) {
    if (view().state != State::ready)
        return "The update is not ready.";
    if (thread_.joinable())
        thread_.join();
    std::string why = installation_.replace_and_start(update_, args);
    if (!why.empty()) {
        std::error_code error;
        fs::remove_all(update_, error);
    }
    return why;
}

void UpdateRun::stage(std::string_view what) {
    std::lock_guard<std::mutex> lock(mutex_);
    view_.stage = what;
    view_.done_bytes = view_.total_bytes = 0;
}

bool UpdateRun::bytes(std::uint64_t done_bytes, std::uint64_t total_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    view_.done_bytes = done_bytes;
    view_.total_bytes = total_bytes;
    return !stopping_;
}

} // namespace launcher
