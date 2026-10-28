// The profiler's panel (Shift+P, port/include/port/profile.h): twice a second, a reading of the counters every thread
// adds to while it is open, and what the last two readings say: the pictures shown against those the screen could
// show, what bounds them, each thread's busy share of a core, the GPU's time a picture against its budget, the
// pictures a second over the last half minute, and the last stalls with what they did. "Copy report" (with the
// settings open, F1: the mouse is then the overlay's) puts the same as text on the clipboard.
//
// profiler_key and profiler_step run on the game's thread (the window's events, the vblank); profiler_draw on the
// thread that shows the picture. What step works out is handed to draw under a lock.
#include <SDL.h>

#include <cstdio>
#include <mutex>
#include <string>

#include "imgui.h"

#include "profiler.h"

extern "C" {
#include "port/platform.h"
#include "port/profile.h"
}

namespace {

const double SAMPLE_SECONDS = 0.5;
const int HISTORY = 60; // readings in the graph: half a minute

// (the game's thread)
bool shown;
ProfileCounters last;
bool have_last;

// (step's, read by draw: under lock)
std::mutex lock;
ProfileReport report;
bool have_report;
double shown_per_second, target_per_second;
float history[HISTORY];
int history_next, history_count;

// One row of the table: a stage's busy share; indented rows are part of the row above
struct Row {
    const char *name;
    double share;
    bool part;
};

int rows_of(const ProfileReport &r, Row *rows) {
    const double *s = r.share;
    int n = 0;
    rows[n++] = {"Game thread", s[PROFILE_GAME], false};
    rows[n++] = {"disc reads", s[PROFILE_DISC], true};
    rows[n++] = {"DMA / VU1 thread", s[PROFILE_DMA] - s[PROFILE_RING_WAIT], false};
    rows[n++] = {"waiting for the GS (ring full)", s[PROFILE_RING_WAIT], true};
    rows[n++] = {"GS thread", s[PROFILE_GS] - s[PROFILE_GS_WAIT], false};
    rows[n++] = {"pictures between", s[PROFILE_GS_INTERP], true};
    rows[n++] = {"waiting for back end / GPU", s[PROFILE_GS_WAIT], true};
    rows[n++] = {"Back end thread (Vulkan calls)", s[PROFILE_BACKEND], false};
    rows[n++] = {"waiting for the GPU", s[PROFILE_BACKEND_WAIT], true};
    rows[n++] = {"Pipelines made while a draw waited", s[PROFILE_COMPILE], false};
    rows[n++] = {"Presenter waiting (vsync, GPU)", s[PROFILE_PRESENT_WAIT], false};
    rows[n++] = {"GPU", s[PROFILE_GPU], false};
    return n;
}
const int MOST_ROWS = 16;

std::string verdict_text(const ProfileReport &r) {
    char line[200];
    if (r.bound == PROFILE_BOUND_NONE || r.bound == PROFILE_BOUND_BURSTS) {
        std::snprintf(line, sizeof(line), "Bound by: %s", port_profile_bound_name(r.bound));
    } else {
        std::snprintf(line, sizeof(line), "Bound by: %s (%.0f%% busy)", port_profile_bound_name(r.bound),
                      r.bound_share * 100.0);
    }
    return line;
}

// The pipelines made while a draw waited, since the start: one line for all of them
std::string compiles_text() {
    ProfileCompiles c = port_profile_compiles();
    char line[160];
    if (c.count == 0) {
        return "Pipelines made in play: none";
    }
    std::snprintf(line, sizeof(line), "Pipelines made in play: %llu, %.1f ms in all, the longest %.1f ms",
                  (unsigned long long)c.count, double(c.ns) / 1e6, double(c.longest_ns) / 1e6);
    return line;
}

// The report as text, for the clipboard (lock held)
std::string report_text() {
    char line[256];
    std::string text = "holyland profiler (Shift+P)\n";
    std::snprintf(line, sizeof(line), "pictures shown %.1f/s of %.0f; drawn %.1f/s; game %.1f vblanks/s\n",
                  shown_per_second, target_per_second, report.pictures_per_second, report.vblanks_per_second);
    text += line;
    text += verdict_text(report) + "\n";
    std::snprintf(line, sizeof(line), "GPU %.2f ms a picture (budget %.2f ms)\n", report.gpu_ms_per_picture,
                  target_per_second > 0 ? 1000.0 / target_per_second : 0.0);
    text += line;
    Row rows[MOST_ROWS];
    int n = rows_of(report, rows);
    for (int i = 0; i < n; i++) {
        std::snprintf(line, sizeof(line), "%s%-34s %5.1f%%\n", rows[i].part ? "  " : "", rows[i].name,
                      rows[i].share * 100.0);
        text += line;
    }
    text += compiles_text() + "\n";
    char hitches[PROFILE_HITCHES][PROFILE_HITCH_TEXT];
    int count = port_profile_hitches(hitches, PROFILE_HITCHES);
    text += count ? "stalls, newest first:\n" : "no stalls\n";
    for (int i = 0; i < count; i++) {
        text += "  ";
        text += hitches[i];
        text += "\n";
    }
    return text;
}

ImVec4 share_colour(double share) {
    if (share >= 0.85) {
        return ImVec4(0.95f, 0.40f, 0.35f, 1.0f);
    }
    if (share >= 0.6) {
        return ImVec4(0.95f, 0.78f, 0.35f, 1.0f);
    }
    return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
}

void set_shown(bool show) {
    shown = show;
    port_profile_set_on(shown);
    have_last = false;
    if (!shown) {
        std::lock_guard<std::mutex> hold(lock);
        have_report = false;
        history_count = history_next = 0;
    }
}

} // namespace

bool profiler_key(const SDL_Event &event) {
    if (event.type != SDL_KEYDOWN || event.key.repeat || event.key.keysym.scancode != SDL_SCANCODE_P ||
        !(event.key.keysym.mod & KMOD_SHIFT)) {
        return false;
    }
    set_shown(!shown);
    return true;
}

void profiler_show_for_shot(bool show) {
    if (show != shown) {
        set_shown(show);
    }
}

bool profiler_visible() {
    return shown;
}

void profiler_step() {
    if (!shown) {
        return;
    }
    ProfileCounters now = port_profile_read();
    if (!have_last) {
        last = now;
        have_last = true;
        return;
    }
    if (double(now.now_ns - last.now_ns) / 1e9 < SAMPLE_SECONDS) {
        return;
    }
    double shown_rate = port_video_pictures_per_second();
    // (at the PS2's 30 the window still shows a picture each vblank: 60)
    double target = port_frame_rate_hz() > 60 ? double(port_frame_rate_hz()) : 60.0;
    ProfileReport r = port_profile_report(&last, &now, shown_rate, target);
    last = now;
    std::lock_guard<std::mutex> hold(lock);
    report = r;
    have_report = true;
    shown_per_second = shown_rate;
    target_per_second = target;
    history[history_next] = float(shown_rate);
    history_next = (history_next + 1) % HISTORY;
    if (history_count < HISTORY) {
        history_count++;
    }
}

void profiler_draw(bool overlay_open) {
    if (!shown) {
        return;
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!overlay_open) {
        flags |= ImGuiWindowFlags_NoInputs;
    }
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.8f);
    if (!ImGui::Begin("Profiler (Shift+P)", nullptr, flags)) {
        ImGui::End();
        return;
    }
    std::lock_guard<std::mutex> hold(lock);
    if (!have_report) {
        ImGui::TextUnformatted("Measuring...");
        ImGui::End();
        return;
    }
    const ProfileReport &r = report;
    ImGui::Text("Pictures shown: %.0f a second of %.0f", shown_per_second, target_per_second);
    ImGui::Text("Drawn %.0f a second; game %.1f vblanks a second", r.pictures_per_second, r.vblanks_per_second);
    ImVec4 verdict = r.bound == PROFILE_BOUND_NONE ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(0.95f, 0.78f, 0.35f, 1.0f);
    ImGui::TextColored(verdict, "%s", verdict_text(r).c_str());
    double budget_ms = target_per_second > 0 ? 1000.0 / target_per_second : 0.0;
    ImGui::TextColored(share_colour(budget_ms > 0 ? r.gpu_ms_per_picture / budget_ms : 0.0),
                       "GPU: %.2f ms a picture (budget %.2f ms)", r.gpu_ms_per_picture, budget_ms);
    ImGui::Separator();

    ImGui::TextDisabled("Busy share of a core (a thread at 100%% holds the rest back)");
    Row rows[MOST_ROWS];
    int n = rows_of(r, rows);
    float bar_width = ImGui::GetFontSize() * 10.0f;
    float bars_at = 0.0f; // (past the longest name)
    for (int i = 0; i < n; i++) {
        float width = ImGui::CalcTextSize(rows[i].name).x + (rows[i].part ? ImGui::GetStyle().IndentSpacing : 0.0f);
        bars_at = width > bars_at ? width : bars_at;
    }
    bars_at += ImGui::GetStyle().WindowPadding.x + ImGui::GetFontSize();
    for (int i = 0; i < n; i++) {
        double share = rows[i].share < 0.0 ? 0.0 : rows[i].share;
        if (rows[i].part) {
            ImGui::Indent();
            ImGui::TextDisabled("%s", rows[i].name);
        } else {
            ImGui::TextUnformatted(rows[i].name);
        }
        ImGui::SameLine(bars_at);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, share_colour(rows[i].part ? 0.0 : share));
        char label[32];
        std::snprintf(label, sizeof(label), "%.0f%%", share * 100.0);
        ImGui::ProgressBar(float(share > 1.0 ? 1.0 : share), ImVec2(bar_width, 0), label);
        ImGui::PopStyleColor();
        if (rows[i].part) {
            ImGui::Unindent();
        }
    }
    ImGui::Separator();

    float ordered[HISTORY];
    for (int i = 0; i < history_count; i++) {
        ordered[i] = history[(history_next - history_count + i + HISTORY) % HISTORY];
    }
    ImGui::PlotLines("##shown", ordered, history_count, 0, "pictures shown a second, last 30 s", 0.0f,
                     float(target_per_second * 1.1), ImVec2(bars_at + bar_width - ImGui::GetStyle().WindowPadding.x, 50));
    ImGui::Separator();

    ImGui::TextUnformatted(compiles_text().c_str());
    char hitches[PROFILE_HITCHES][PROFILE_HITCH_TEXT];
    int count = port_profile_hitches(hitches, PROFILE_HITCHES);
    ImGui::TextDisabled(count ? "Stalls (over two vblanks), newest first" : "No stalls while the panel was open");
    for (int i = 0; i < count; i++) {
        ImGui::BulletText("%s", hitches[i]);
    }
    ImGui::Separator();
    if (overlay_open) {
        if (ImGui::Button("Copy report")) {
            ImGui::SetClipboardText(report_text().c_str());
        }
    } else {
        ImGui::TextDisabled("F1 for the mouse: Copy report");
    }
    ImGui::End();
}
