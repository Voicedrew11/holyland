// The profiler panel's "Functions and GPU" section (profile_run_view.h).
//
// step (the game's thread) copies the sampler's report and the GPU's rows, names demangled, into the view's own
// tables under a lock; draw (the thread that shows the picture) reads them under it. The buttons start, stop and
// save the run from draw's thread (each takes the sampler's lock briefly; Stop waits for its thread to end).
#include <cxxabi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"

#include "profile_run_view.h"

extern "C" {
#include "port/profile.h"
#include "port/profile_run.h"
#include "port/sampler.h"
}

namespace {

const int SHOWN_FUNCTIONS = 25;
const int SHOWN_SHADERS = 20;
const size_t SHOWN_NAME = 70; // characters of a name in the table (the whole in its tooltip)

struct Function {
    std::string name;
    uint64_t self, inclusive;
};

struct Thread {
    int tid;
    std::string name;
    double cpu_ms;
    uint64_t samples;
    bool signals_blocked;
    std::vector<Function> functions;
};

struct GpuRow {
    std::string name;
    double ms_per_picture;
    uint64_t intervals;
};

// (step's, read by draw: under lock)
std::mutex lock;
double seconds;
uint64_t missed;
std::vector<Thread> threads;
std::vector<GpuRow> kinds, shaders;
double gpu_ms_per_picture;
uint64_t pictures;
std::string saved_path;

// (draw's own)
int chosen_tid = -1; // -1: the busiest
bool by_self = true;

std::string demangled(const char *name) {
    int status = 0;
    char *plain = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status != 0 || plain == nullptr) {
        return name;
    }
    std::string out = plain;
    std::free(plain);
    return out;
}

void take_sampler_report(std::vector<Thread> &out, double &run_seconds, uint64_t &run_missed) {
    static std::unique_ptr<SamplerReport> report(new SamplerReport());
    out.clear();
    if (!sampler_report(report.get())) {
        return;
    }
    run_seconds = report->seconds;
    run_missed = report->missed;
    for (int i = 0; i < report->thread_count; i++) {
        const SamplerThread &t = report->threads[i];
        Thread shown = {t.tid, t.name, t.cpu_ms, t.samples, t.signals_blocked != 0, {}};
        for (int k = 0; k < t.function_count; k++) {
            shown.functions.push_back({demangled(t.functions[k].name), t.functions[k].self, t.functions[k].inclusive});
        }
        out.push_back(std::move(shown));
    }
}

void take_gpu_rows(std::vector<GpuRow> &kind_rows, std::vector<GpuRow> &shader_rows, double &ms_per_picture,
                   uint64_t &picture_count) {
    static ProfileGpuRow rows[PROFILE_GPU_ROWS];
    int n = port_profile_gpu_rows(rows, PROFILE_GPU_ROWS);
    picture_count = port_profile_gpu_pictures();
    double per = picture_count ? 1.0 / double(picture_count) : 0.0;
    kind_rows.clear();
    shader_rows.clear();
    ms_per_picture = 0;
    for (int i = 0; i < n; i++) {
        GpuRow row = {rows[i].name, double(rows[i].ns) / 1e6 * per, rows[i].intervals};
        if (rows[i].level == PROFILE_GPU_KIND) {
            ms_per_picture += row.ms_per_picture;
            kind_rows.push_back(row);
        } else {
            shader_rows.push_back(row);
        }
    }
}

const Thread *chosen_thread() {
    if (threads.empty()) {
        return nullptr;
    }
    for (const Thread &t : threads) {
        if (t.tid == chosen_tid) {
            return &t;
        }
    }
    const Thread *busiest = &threads[0];
    for (const Thread &t : threads) {
        busiest = t.samples > busiest->samples ? &t : busiest;
    }
    return busiest;
}

std::string thread_label(const Thread &t) {
    char label[96];
    std::snprintf(label, sizeof(label), "%s (%d): %.0f ms CPU, %.0f%% of a core%s", t.name.empty() ? "?" : t.name.c_str(),
                  t.tid, t.cpu_ms, seconds > 0 ? t.cpu_ms / 10.0 / seconds : 0.0,
                  t.signals_blocked ? ", not sampled" : "");
    return label;
}

std::vector<const Function *> sorted_functions(const Thread &t) {
    std::vector<const Function *> out;
    for (const Function &f : t.functions) {
        out.push_back(&f);
    }
    std::stable_sort(out.begin(), out.end(), [](const Function *a, const Function *b) {
        return by_self ? a->self > b->self : a->inclusive > b->inclusive;
    });
    if (out.size() > size_t(SHOWN_FUNCTIONS)) {
        out.resize(SHOWN_FUNCTIONS);
    }
    return out;
}

std::string short_name(const std::string &name) {
    return name.size() <= SHOWN_NAME ? name : name.substr(0, SHOWN_NAME - 3) + "...";
}

// The section as text, for the clipboard (lock held)
std::string run_text() {
    std::string text;
    char line[400];
    std::snprintf(line, sizeof(line), "holyland profile run: %.1f s, %llu pictures, GPU %.2f ms a picture\n", seconds,
                  (unsigned long long)pictures, gpu_ms_per_picture);
    text += line;
    for (const Thread &t : threads) {
        if (t.samples == 0) {
            continue;
        }
        text += "\n" + thread_label(t) + ", " + std::to_string(t.samples) + " samples\n  incl%   self%\n";
        for (const Function &f : t.functions) {
            std::snprintf(line, sizeof(line), "  %5.1f  %5.1f  %s\n", 100.0 * double(f.inclusive) / double(t.samples),
                          100.0 * double(f.self) / double(t.samples), f.name.c_str());
            text += line;
        }
    }
    text += "\nGPU ms a picture, by kind of work\n";
    for (const GpuRow &r : kinds) {
        std::snprintf(line, sizeof(line), "  %7.3f  %s\n", r.ms_per_picture, r.name.c_str());
        text += line;
    }
    if (!shaders.empty()) {
        text += "GPU ms a picture, by shader state\n";
        for (const GpuRow &r : shaders) {
            std::snprintf(line, sizeof(line), "  %7.3f  %8llu  %s\n", r.ms_per_picture, (unsigned long long)r.intervals,
                          r.name.c_str());
            text += line;
        }
    }
    return text;
}

void draw_controls(bool overlay_open) {
    bool recording = port_profile_run_recording() != 0;
    if (!overlay_open) {
        ImGui::TextDisabled(recording ? "Recording. F1 for the mouse: Stop, Save, Copy" : "F1 for the mouse: Start");
        return;
    }
    if (ImGui::Button(recording ? "Stop" : "Start")) {
        if (recording) {
            port_profile_run_stop();
        } else {
            port_profile_run_start();
        }
    }
    ImGui::SameLine();
    bool detail = port_profile_gpu_detail() != 0;
    if (ImGui::Checkbox("GPU by shader state", &detail)) {
        port_profile_gpu_set_detail(detail);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The PC renderer times each change of shader state: a timestamp at each, which costs a\n"
                          "little GPU time and can shift the totals a bit");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        char path[1100];
        port_profile_run_default_path(path, int(sizeof(path)));
        std::string written = port_profile_run_save(path) ? path : std::string("(could not write ") + path + ")";
        std::lock_guard<std::mutex> hold(lock);
        saved_path = written;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        std::lock_guard<std::mutex> hold(lock);
        ImGui::SetClipboardText(run_text().c_str());
    }
}

void draw_functions() {
    if (!sampler_available()) {
        ImGui::TextDisabled("Functions: Linux only (on Windows, tools/sampler/profile.py)");
        return;
    }
    const Thread *t = chosen_thread();
    if (t == nullptr) {
        ImGui::TextDisabled("No samples yet: Start a run");
        return;
    }
    ImGui::Text("%.1f s sampled%s", seconds, missed ? " (some requests unanswered)" : "");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26.0f);
    if (ImGui::BeginCombo("Thread", thread_label(*t).c_str())) {
        for (const Thread &each : threads) {
            if (ImGui::Selectable(thread_label(each).c_str(), each.tid == t->tid)) {
                chosen_tid = each.tid;
            }
        }
        ImGui::EndCombo();
    }
    if (t->samples == 0) {
        ImGui::TextDisabled(t->signals_blocked ? "It blocks the profiler's signal: CPU time only" : "Never caught running");
        return;
    }
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("functions", 3, flags)) {
        return;
    }
    ImGui::TableSetupColumn("self");
    ImGui::TableSetupColumn("incl.");
    ImGui::TableSetupColumn("function (of the thread's CPU time)");
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetColumnIndex(0);
    if (ImGui::Selectable(by_self ? "self v" : "self", false, ImGuiSelectableFlags_DontClosePopups)) {
        by_self = true;
    }
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Selectable(by_self ? "incl." : "incl. v", false, ImGuiSelectableFlags_DontClosePopups)) {
        by_self = false;
    }
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted("function, by share of the thread's CPU time");
    for (const Function *f : sorted_functions(*t)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%5.1f%%", 100.0 * double(f->self) / double(t->samples));
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%5.1f%%", 100.0 * double(f->inclusive) / double(t->samples));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(short_name(f->name).c_str());
        if (f->name.size() > SHOWN_NAME && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", f->name.c_str());
        }
    }
    ImGui::EndTable();
}

void draw_gpu_rows(const char *id, const std::vector<GpuRow> &rows, size_t most, bool with_intervals) {
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(id, with_intervals ? 3 : 2, flags)) {
        return;
    }
    ImGui::TableSetupColumn("ms");
    if (with_intervals) {
        ImGui::TableSetupColumn("runs");
    }
    ImGui::TableSetupColumn(with_intervals ? "shader state" : "kind of work");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < rows.size() && i < most; i++) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%.3f", rows[i].ms_per_picture);
        int column = 1;
        if (with_intervals) {
            ImGui::TableSetColumnIndex(column++);
            ImGui::Text("%llu", (unsigned long long)rows[i].intervals);
        }
        ImGui::TableSetColumnIndex(column);
        ImGui::TextUnformatted(short_name(rows[i].name).c_str());
        if (rows[i].name.size() > SHOWN_NAME && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", rows[i].name.c_str());
        }
    }
    ImGui::EndTable();
}

void draw_gpu() {
    if (kinds.empty()) {
        ImGui::TextDisabled("GPU: no times yet (a renderer with timestamps: pc, native, modern, fast)");
        return;
    }
    ImGui::Text("GPU: %.2f ms a picture over %llu pictures", gpu_ms_per_picture, (unsigned long long)pictures);
    draw_gpu_rows("gpu kinds", kinds, kinds.size(), false);
    if (!shaders.empty()) {
        ImGui::TextDisabled("By shader state (ms a picture; runs: intervals timed), the top %d", SHOWN_SHADERS);
        draw_gpu_rows("gpu shaders", shaders, SHOWN_SHADERS, true);
    }
}

} // namespace

void profile_run_view_step() {
    std::vector<Thread> new_threads;
    std::vector<GpuRow> new_kinds, new_shaders;
    double run_seconds = 0, ms = 0;
    uint64_t run_missed = 0, picture_count = 0;
    take_sampler_report(new_threads, run_seconds, run_missed);
    take_gpu_rows(new_kinds, new_shaders, ms, picture_count);
    std::lock_guard<std::mutex> hold(lock);
    threads = std::move(new_threads);
    kinds = std::move(new_kinds);
    shaders = std::move(new_shaders);
    seconds = run_seconds;
    missed = run_missed;
    gpu_ms_per_picture = ms;
    pictures = picture_count;
}

void profile_run_view_draw(bool overlay_open) {
    if (port_profile_run_recording()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once); // (a run under way when the panel first shows: open)
    }
    if (!ImGui::CollapsingHeader("Functions and GPU (profile run)")) {
        return;
    }
    draw_controls(overlay_open);
    std::lock_guard<std::mutex> hold(lock);
    if (!saved_path.empty()) {
        ImGui::TextDisabled("Saved: %s", saved_path.c_str());
    }
    draw_functions();
    ImGui::Separator();
    draw_gpu();
}
