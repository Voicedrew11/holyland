// The update notice in the overlay (update_badge.h). The rules are port/platform/update_notice.c, shared with
// the launcher; this file reads the two files and draws.
#include "update_badge.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "cJSON.h"
#include "imgui.h"

extern "C" {
#include "port/settings.h"
#include "port/update_notice.h"
#include "port/version.h"
}

#if __has_include("build_rev.h")
#include "build_rev.h" // KF4_BUILD_REV (tools/build_rev.cmake)
#endif
#ifndef KF4_BUILD_REV
#define KF4_BUILD_REV "unknown"
#endif

namespace {

constexpr const char *kProductName = "Darker Reality"; // what players see it called (launcher/product.h)
constexpr unsigned kPollEveryMs = 5000;
constexpr unsigned kPollForMs = 60000;

UpdateRelease release;
std::string skipped;
bool due;
bool hidden;      // "Later": until the next start
bool show_window; // the release's window
bool polled_once;
unsigned next_poll_ms;
unsigned first_poll_ms;

std::string data_path(const char *name) {
    return std::string(port_data_dir()) + "/" + name;
}

// a small text file whole; "" when missing (the files are a few hundred bytes)
std::string read_small(const std::string &path) {
    std::string text;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return text;
    char buffer[4096];
    size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    text.assign(buffer, got);
    return text;
}

void read_skipped() {
    skipped.clear();
    std::string text = read_small(data_path("launcher.json"));
    cJSON *root = cJSON_Parse(text.c_str());
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "skipped");
    if (cJSON_IsString(item))
        skipped = item->valuestring;
    cJSON_Delete(root);
}

// written whole, then put in place, so a crash never leaves half a file (as settings.json)
bool replace_file(const std::string &from, const std::string &to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

void skip_this_version() {
    std::string path = data_path("launcher.json");
    std::string old = read_small(path);
    char text[4096];
    if (!update_skip_in_launcher_json(old.empty() ? nullptr : old.c_str(), release.tag, text, sizeof(text)))
        return;
    std::string temp = path + ".tmp";
    FILE *f = std::fopen(temp.c_str(), "wb");
    if (f == nullptr)
        return;
    bool written = std::fputs(text, f) >= 0;
    written = std::fclose(f) == 0 && written;
    if (!written || !replace_file(temp, path)) {
        std::remove(temp.c_str());
        return;
    }
    skipped = release.tag;
    due = false;
}

} // namespace

bool update_badge_poll(unsigned now_ms) {
    if (!polled_once) {
        polled_once = true;
        first_poll_ms = now_ms;
    } else if ((int)(now_ms - next_poll_ms) < 0 || now_ms - first_poll_ms > kPollForMs || due) {
        return false;
    }
    next_poll_ms = now_ms + kPollEveryMs;
    std::string text = read_small(data_path("update.json"));
    update_cache_parse(text.empty() ? nullptr : text.c_str(), &release);
    read_skipped();
    due = update_notice_due(&release, port_version, skipped.c_str()) != 0;
    if (due)
        std::fprintf(stderr, "[update] %s %s is out (this is %s): %s\n", kProductName, release.tag, port_version, release.url);
    return due;
}

const char *update_badge_note() {
    static char note[128];
    std::snprintf(note, sizeof(note), "Update available: %s (F1 for details)", release.tag);
    return note;
}

void update_badge_menu_item() {
    if (!due || hidden)
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.3f, 1.0f));
    if (ImGui::BeginMenu("Update available!")) {
        ImGui::PopStyleColor();
        if (ImGui::MenuItem("What's new..."))
            show_window = true;
        if (ImGui::MenuItem("Open download page"))
            SDL_OpenURL(release.url);
        if (ImGui::MenuItem("Skip this version"))
            skip_this_version();
        if (ImGui::MenuItem("Hide until next start"))
            hidden = true;
        ImGui::EndMenu();
    } else {
        ImGui::PopStyleColor();
    }
}

void update_badge_window() {
    if (!show_window || !due)
        return;
    if (!ImGui::Begin("Update available", &show_window, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%s %s is out. You have %s.", kProductName, release.tag, port_version);
    ImGui::TextDisabled("Download it from the release page and install it over this one.");
    ImGui::TextDisabled("Your settings and saves are kept. Quick saves carry over; save states do not.");
    ImGui::Separator();
    if (ImGui::Button("Download page")) {
        SDL_OpenURL(release.url);
        show_window = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip this version")) {
        skip_this_version();
        show_window = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Later")) {
        hidden = true;
        show_window = false;
    }
    ImGui::End();
}

const char *update_badge_version_line() {
    static char line[128];
    if (port_version[0] != 0)
        std::snprintf(line, sizeof(line), "%s %s", kProductName, port_version);
    else
        std::snprintf(line, sizeof(line), "%s (developer's build %s)", kProductName, KF4_BUILD_REV);
    return line;
}
