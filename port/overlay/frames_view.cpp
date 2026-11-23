// The profiler's window (Shift+P, frames_view.h).
//
// refresh copies what the window shows out of the record (zones::with_record) at most REFRESH_SECONDS apart, or at
// once when a picture is picked: the bars, the window's averages, the spikes and the picked picture with its spans.
// Everything drawn is read from that copy, so the record's lock is held only while copying.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"

#include "frames_view.h"
#include "profile_run_view.h"
#include "port/frame_profiler.h"
#include "port/zone_record.hpp"

extern "C" {
#include "port/host_load.h"
#include "port/platform.h"
#include "port/profile.h"
#include "port/version.h"
#include "port/zones.h"
}

namespace {

const double REFRESH_SECONDS = 0.25;
const size_t BARS_MOST = 1500;     // pictures in the graph
const size_t SPIKES_SHOWN = 40;
const double MARGIN_SHARE = 0.25;  // of a picture's length, either side of it on the timeline
const int DEPTH_SHOWN = 12;        // rows of nesting a lane shows at most
const double SHOWN_EVERY_SECONDS = 0.5; // the pictures shown a second, read this often for their graph
const int SHOWN_HISTORY = 60;           // readings of it kept: half a minute

// ---- the copy -------------------------------------------------------------------------------------------------

struct Bar {
    uint64_t index;
    float ms, gpu_ms;
};

// A zone's time: a picture's, or a picture's average over the window
struct Row {
    int lane;
    ZoneId id;
    double self_ms, incl_ms, calls, max_self_ms;
};

struct LaneSum {
    std::string name;
    bool gpu;
    double ms[ZONE_KINDS]; // (a picture's average over the window)
};

struct Picked {
    bool have = false;
    zones::Frame frame;
    uint64_t from_ns = 0, to_ns = 0;          // the spans copied: the picture and a margin
    std::vector<std::vector<zones::Span>> spans; // (by lane)
};

struct Copy {
    std::vector<Bar> bars;
    std::vector<LaneSum> lanes;
    std::vector<Row> window_rows;
    size_t window_frames = 0;
    double avg_ms = 0, p99_ms = 0, max_ms = 0, per_second = 0, spike_ms = 0;
    std::vector<Bar> spikes; // (newest first)
    Picked picked;
    uint64_t lost = 0, unmatched = 0;
};

// ---- the window's state (the thread that shows the picture's) --------------------------------------------------

struct State {
    bool shown = false;       // (as last drawn: frames_view_show sets wanted_shown from the game's thread)
    Copy copy;
    // the pictures the window showed a second (port_video_pictures_per_second), every SHOWN_EVERY_SECONDS
    float shown_history[SHOWN_HISTORY] = {};
    int shown_next = 0, shown_count = 0;
    double shown_read_s = -1;
    double refreshed_s = -1;
    bool dirty = true;
    int64_t picked = -1;      // a frame's index, or -1: the newest done
    int64_t kept = -1;        // the frame the record keeps the spans of (the picked one)
    bool paused = false;
    float window_seconds = 3.0f;
    float spike_setting_ms = 0.0f; // 0: twice the median and 2 ms
    int gpu_detail = 0;
    int table_source = 0;     // 0: the picked picture; 1: averaged over the window
    int lane_filter = -1;     // -1: every lane
    char filter[64] = "";
    double zoom = 1.0;        // the timeline's: 1 the picture and its margins whole
    double centre = 0.5;      // the timeline's middle, a share of what was copied
    bool show_waits = true;
    // the timeline's rows a lane, the most it has needed (never fewer: the rows keep their places as pictures change)
    std::vector<int> lane_rows;
};

State state;
std::atomic<bool> wanted_shown{false};

// ---- small helpers ------------------------------------------------------------------------------------------

double ms_of(uint64_t ns) {
    return double(ns) / 1e6;
}

const char *kind_name(ZoneKind kind) {
    static const char *const names[ZONE_KINDS] = {"work", "wait", "gpu"};
    return unsigned(kind) < ZONE_KINDS ? names[kind] : "?";
}

// A zone's colour: its own hue; waits grey, the GPU's warm
ImU32 zone_colour(ZoneId id, float alpha = 1.0f) {
    float hue = std::fmod(float(id) * 0.6180339f, 1.0f);
    switch (port_zone_kind(id)) {
    case ZONE_WAIT:
        return ImColor::HSV(hue, 0.08f, 0.55f, alpha);
    case ZONE_GPU:
        return ImColor::HSV(0.02f + hue * 0.12f, 0.65f, 0.92f, alpha);
    default:
        return ImColor::HSV(0.40f + hue * 0.45f, 0.50f, 0.85f, alpha);
    }
}

double percentile(std::vector<double> values, double share) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(double(values.size()) * share))];
}

double gpu_ms_of(const zones::Frame &f, const zones::Record &r) {
    double ms = 0;
    for (int lane = 0; lane < r.lanes(); lane++) {
        ms += f.lane_ms(lane, ZONE_GPU);
    }
    return ms;
}

// ---- refresh: the record copied --------------------------------------------------------------------------------

void copy_bars(const zones::Record &r, Copy &c) {
    const auto &frames = r.frames();
    size_t first = frames.size() > BARS_MOST ? frames.size() - BARS_MOST : 0;
    for (size_t i = first; i < frames.size(); i++) {
        c.bars.push_back({frames[i].index, float(frames[i].ms()), float(gpu_ms_of(frames[i], r))});
    }
}

// The averages over the last window_seconds of pictures, the rows by zone, the spikes over the threshold
void copy_window(const zones::Record &r, Copy &c) {
    const auto &frames = r.frames();
    if (frames.empty()) {
        return;
    }
    uint64_t newest_end = frames.back().end_ns;
    uint64_t window_ns = uint64_t(double(state.window_seconds) * 1e9);
    std::vector<double> lengths;
    std::unordered_map<uint32_t, Row> rows;
    c.lanes.assign(size_t(r.lanes()), LaneSum{});
    for (int lane = 0; lane < r.lanes(); lane++) {
        c.lanes[size_t(lane)].name = r.lane_name(lane);
        c.lanes[size_t(lane)].gpu = r.lane_gpu(lane);
    }
    uint64_t oldest_start = newest_end;
    for (auto it = frames.rbegin(); it != frames.rend() && it->end_ns + window_ns >= newest_end; ++it) {
        const zones::Frame &f = *it;
        lengths.push_back(f.ms());
        oldest_start = f.start_ns;
        for (int lane = 0; lane < r.lanes(); lane++) {
            for (int k = 0; k < ZONE_KINDS; k++) {
                c.lanes[size_t(lane)].ms[k] += f.lane_ms(lane, ZoneKind(k));
            }
        }
        for (const zones::ZoneTime &z : f.zones) {
            Row &row = rows[uint32_t(z.lane) << 16 | z.id];
            row.lane = z.lane;
            row.id = z.id;
            row.self_ms += ms_of(z.self_ns);
            row.incl_ms += ms_of(z.incl_ns);
            row.calls += z.calls;
            row.max_self_ms = std::max(row.max_self_ms, ms_of(z.self_ns));
        }
    }
    double n = double(lengths.size());
    c.window_frames = lengths.size();
    for (LaneSum &l : c.lanes) {
        for (double &ms : l.ms) {
            ms /= n;
        }
    }
    for (auto &kv : rows) {
        Row row = kv.second;
        row.self_ms /= n;
        row.incl_ms /= n;
        row.calls /= n;
        c.window_rows.push_back(row);
    }
    double sum = 0;
    for (double ms : lengths) {
        sum += ms;
    }
    c.avg_ms = sum / n;
    c.p99_ms = percentile(lengths, 0.99);
    c.max_ms = percentile(lengths, 1.0);
    c.per_second = newest_end > oldest_start ? n / (double(newest_end - oldest_start) / 1e9) : 0;
    c.spike_ms = state.spike_setting_ms > 0 ? state.spike_setting_ms : 2.0 * percentile(lengths, 0.5) + 2.0;
    for (auto it = frames.rbegin(); it != frames.rend() && c.spikes.size() < SPIKES_SHOWN; ++it) {
        if (it->ms() > c.spike_ms) {
            c.spikes.push_back({it->index, float(it->ms()), float(gpu_ms_of(*it, r))});
        }
    }
}

// The picked picture (or the newest), its spans kept in the record while picked
void copy_picked(zones::Record &r, Copy &c) {
    const auto &frames = r.frames();
    if (frames.empty()) {
        return;
    }
    const zones::Frame *f = state.picked >= 0 ? r.frame(uint64_t(state.picked)) : &frames.back();
    if (f == nullptr) {
        state.picked = -1; // (gone from the record: the newest again)
        f = &frames.back();
    }
    if (state.picked >= 0 && state.kept != state.picked) {
        if (state.kept >= 0) {
            r.let_go_frame(uint64_t(state.kept));
        }
        uint64_t margin = uint64_t(double(f->end_ns - f->start_ns) * MARGIN_SHARE);
        r.keep_frame(uint64_t(state.picked), margin);
        state.kept = state.picked;
    }
    Picked &p = c.picked;
    p.have = true;
    p.frame = *f;
    uint64_t margin = uint64_t(double(f->end_ns - f->start_ns) * MARGIN_SHARE);
    p.from_ns = f->start_ns > margin ? f->start_ns - margin : 0;
    p.to_ns = f->end_ns + margin;
    for (int lane = 0; lane < r.lanes(); lane++) {
        p.spans.push_back(r.spans(lane, p.from_ns, p.to_ns));
    }
}

void refresh() {
    double now = port_wall_seconds();
    if (state.paused && !state.dirty) {
        return;
    }
    if (!state.dirty && now - state.refreshed_s < REFRESH_SECONDS) {
        return;
    }
    Copy c;
    bool copied = zones::with_record([&](zones::Record &r) {
        copy_bars(r, c);
        copy_window(r, c);
        copy_picked(r, c);
        c.lost = r.lost();
        c.unmatched = r.unmatched();
    });
    if (!copied) {
        return; // (the record busy: the next picture tries again)
    }
    state.copy = std::move(c);
    state.refreshed_s = now;
    state.dirty = false;
}

void pick(int64_t index) {
    if (index != state.picked) {
        state.picked = index;
        state.zoom = 1.0;
        state.centre = 0.5;
        state.dirty = true;
    }
}

// ---- the parts drawn --------------------------------------------------------------------------------------------

void draw_toolbar() {
    ImGui::Checkbox("Pause", &state.paused);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Hold what is shown still to read it (the record goes on)");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderFloat("window", &state.window_seconds, 0.5f, 10.0f, "%.1f s")) {
        state.dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::SliderFloat("spike", &state.spike_setting_ms, 0.0f, 50.0f,
                           state.spike_setting_ms <= 0 ? "auto" : "%.1f ms")) {
        state.dirty = true;
    }
    ImGui::SameLine();
    static const char *const details[] = {"GPU: by kind", "GPU: by shader state", "GPU: each draw (slow)"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
    state.gpu_detail = port_profile_gpu_detail();
    if (ImGui::Combo("##gpu detail", &state.gpu_detail, details, 3)) {
        port_profile_gpu_set_detail(state.gpu_detail);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Timestamps at each change of kind of work, of shader state, or at every draw.\n"
                          "More timestamps cost the GPU time: compare totals by kind.");
    }
    ImGui::SameLine();
    if (state.picked >= 0 && ImGui::Button("Newest")) {
        pick(-1);
    }
}

// The screen's pictures a second: one reading every SHOWN_EVERY_SECONDS into the half minute's graph
void read_shown() {
    double now = port_wall_seconds();
    if (now - state.shown_read_s < SHOWN_EVERY_SECONDS) {
        return;
    }
    state.shown_read_s = now;
    state.shown_history[state.shown_next] = float(port_video_pictures_per_second());
    state.shown_next = (state.shown_next + 1) % SHOWN_HISTORY;
    state.shown_count = std::min(state.shown_count + 1, SHOWN_HISTORY);
}

double target_per_second() {
    // (at the PS2's 30 the window still shows a picture each vblank: 60)
    return port_frame_rate_hz() > 60 ? double(port_frame_rate_hz()) : 60.0;
}

// What the screen got: pictures shown against those it could show, the half minute's graph, the pipelines a draw
// waited for since the start
void draw_screen() {
    read_shown();
    double target = target_per_second();
    float latest = state.shown_count ? state.shown_history[(state.shown_next - 1 + SHOWN_HISTORY) % SHOWN_HISTORY] : 0;
    if (!port_build_optimized) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1), "This build is not optimized: it runs several times slower");
    }
    ImGui::Text("Pictures shown: %.0f a second of %.0f", double(latest), target);
    // (the PC under the game: a CPU slowing down, from heat or power saving, or the game's memory growing)
    const PortHostLoad load = port_host_load();
    char cpu[96] = "CPU: speed unknown";
    if (load.cpu_mhz > 0 && load.cpu_limit_mhz > 0) {
        std::snprintf(cpu, sizeof(cpu), "CPU: %.0f MHz (base %.0f, held to %.0f)", load.cpu_mhz, load.cpu_most_mhz,
                      load.cpu_limit_mhz);
    } else if (load.cpu_limit_mhz > 0) {
        std::snprintf(cpu, sizeof(cpu), "CPU: base %.0f MHz, held to %.0f", load.cpu_most_mhz, load.cpu_limit_mhz);
    } else if (load.cpu_mhz > 0) {
        std::snprintf(cpu, sizeof(cpu), "CPU: %.0f MHz (most %.0f)", load.cpu_mhz, load.cpu_most_mhz);
    }
    ImGui::Text("%s   Game's memory: %.0f MB", cpu, load.memory_mb);
    float ordered[SHOWN_HISTORY];
    for (int i = 0; i < state.shown_count; i++) {
        ordered[i] = state.shown_history[(state.shown_next - state.shown_count + i + SHOWN_HISTORY) % SHOWN_HISTORY];
    }
    ImGui::PlotLines("##shown", ordered, state.shown_count, 0, "pictures shown a second, last 30 s", 0.0f,
                     float(target * 1.1), ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 3));
    ProfileCompiles compiles = port_profile_compiles();
    if (compiles.count == 0) {
        ImGui::TextUnformatted("Pipelines made while a draw waited: none");
    } else {
        ImGui::Text("Pipelines made while a draw waited: %llu, %.1f ms in all, the longest %.1f ms",
                    (unsigned long long)compiles.count, double(compiles.ns) / 1e6, double(compiles.longest_ns) / 1e6);
    }
}

void draw_summary() {
    const Copy &c = state.copy;
    ImGui::Text("%.1f pictures/s over %zu pictures: %.2f ms average, %.2f p99, %.2f most", c.per_second,
                c.window_frames, c.avg_ms, c.p99_ms, c.max_ms);
    if (c.lost || c.unmatched) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1), "events lost %llu (the rings outran the reader), unmatched %llu",
                           (unsigned long long)c.lost, (unsigned long long)c.unmatched);
    }
    if (!ImGui::BeginTable("##lanes", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("lane (ms a picture)");
    ImGui::TableSetupColumn("working");
    ImGui::TableSetupColumn("waiting");
    ImGui::TableSetupColumn("GPU busy");
    ImGui::TableHeadersRow();
    for (const LaneSum &l : c.lanes) { // (every lane, idle ones too: the rows keep their places)
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(l.name.c_str());
        for (int k = 0; k < ZONE_KINDS; k++) {
            ImGui::TableNextColumn();
            if (l.ms[k] > 0) {
                ImGui::Text("%.3f", l.ms[k]);
            }
        }
    }
    ImGui::EndTable();
}

// A bar a picture: its length (blue), the GPU's busy time in it (orange), the screen's budget (a line)
void draw_graph() {
    const Copy &c = state.copy;
    float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFontSize() * 7;
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##graph", ImVec2(width, height));
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(20, 20, 24, 200));
    if (c.bars.empty()) {
        return;
    }
    size_t shown = std::min(c.bars.size(), size_t(width / 2));
    size_t first = c.bars.size() - shown;
    float top_ms = float(std::max(c.p99_ms * 1.5, c.spike_ms * 1.1));
    float bar_w = width / float(shown);
    for (size_t i = first; i < c.bars.size(); i++) {
        const Bar &b = c.bars[i];
        float x = at.x + float(i - first) * bar_w;
        float h = std::min(b.ms / top_ms, 1.0f) * height, g = std::min(b.gpu_ms / top_ms, 1.0f) * height;
        bool picked = int64_t(b.index) == state.picked;
        ImU32 colour = picked ? IM_COL32(255, 255, 255, 255)
                       : b.ms > c.spike_ms ? IM_COL32(230, 80, 70, 255)
                                           : IM_COL32(90, 150, 230, 255);
        draw->AddRectFilled(ImVec2(x, at.y + height - h), ImVec2(x + std::max(bar_w - 1, 1.0f), at.y + height), colour);
        draw->AddRectFilled(ImVec2(x + bar_w * 0.25f, at.y + height - g),
                            ImVec2(x + std::max(bar_w * 0.75f, bar_w * 0.25f + 1), at.y + height),
                            IM_COL32(240, 150, 50, 220));
    }
    float budget = float(1000.0 / target_per_second());
    if (budget < top_ms) {
        float y = at.y + height - budget / top_ms * height;
        draw->AddLine(ImVec2(at.x, y), ImVec2(at.x + width, y), IM_COL32(120, 220, 120, 180));
    }
    char scale[64];
    std::snprintf(scale, sizeof(scale), "%.1f ms", top_ms);
    draw->AddText(ImVec2(at.x + 4, at.y + 2), IM_COL32(200, 200, 200, 200), scale);
    if (!ImGui::IsItemHovered()) {
        return;
    }
    size_t i = first + size_t((ImGui::GetIO().MousePos.x - at.x) / bar_w);
    if (i >= c.bars.size()) {
        return;
    }
    const Bar &b = c.bars[i];
    ImGui::SetTooltip("picture %llu: %.2f ms, GPU busy %.2f ms\nclick: its timeline and zones",
                      (unsigned long long)b.index, b.ms, b.gpu_ms);
    if (ImGui::IsItemClicked()) {
        pick(int64_t(b.index));
    }
}

void draw_spikes() {
    const Copy &c = state.copy;
    char title[96];
    std::snprintf(title, sizeof(title), "Spikes over %.1f ms (%zu)###spikes", c.spike_ms, c.spikes.size());
    if (!ImGui::CollapsingHeader(title)) {
        return;
    }
    for (const Bar &b : c.spikes) {
        char label[96];
        std::snprintf(label, sizeof(label), "picture %llu: %.2f ms (GPU %.2f)", (unsigned long long)b.index, b.ms,
                      b.gpu_ms);
        if (ImGui::Selectable(label, int64_t(b.index) == state.picked)) {
            pick(int64_t(b.index));
        }
    }
}

int lane_depth(const std::vector<zones::Span> &spans) {
    int depth = 0;
    for (const zones::Span &s : spans) {
        depth = std::max(depth, int(s.depth) + 1);
    }
    return std::min(depth, DEPTH_SHOWN);
}

// The timeline's wheel (zoom about the mouse), drag (move) and double-click (whole)
void timeline_input(float left, float width) {
    if (!ImGui::IsItemHovered()) {
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    double span = 1.0 / state.zoom;
    double mouse_share = state.centre - span / 2 + double((io.MousePos.x - left) / width) * span;
    if (io.MouseWheel != 0) {
        state.zoom = std::clamp(state.zoom * std::pow(1.25, double(io.MouseWheel)), 1.0, 20000.0);
        double after = 1.0 / state.zoom;
        state.centre = mouse_share + (state.centre - mouse_share) * after / span;
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        state.centre -= double(io.MouseDelta.x / width) * span;
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        state.zoom = 1.0;
        state.centre = 0.5;
    }
    double half = 0.5 / state.zoom;
    state.centre = std::clamp(state.centre, half, 1.0 - half);
}

// Every lane with something in the picture: its zones nested by depth, on one time axis
void draw_timeline() {
    const Picked &p = state.copy.picked;
    if (!p.have) {
        ImGui::TextDisabled("No picture done yet.");
        return;
    }
    ImGui::Text("Picture %llu: %.3f ms%s. Wheel: zoom; drag: move; double-click: whole.",
                (unsigned long long)p.frame.index, p.frame.ms(), state.picked < 0 ? " (the newest)" : "");
    float row_h = ImGui::GetFontSize() + 3, label_w = ImGui::GetFontSize() * 9;
    // (a lane keeps the rows it has needed since the window opened: nothing moves as the pictures change)
    state.lane_rows.resize(std::max(state.lane_rows.size(), p.spans.size()), 0);
    int rows = 0;
    for (size_t lane = 0; lane < state.lane_rows.size(); lane++) {
        int depth = lane < p.spans.size() ? lane_depth(p.spans[lane]) : 0;
        state.lane_rows[lane] = std::max(state.lane_rows[lane], depth);
        rows += state.lane_rows[lane] + (state.lane_rows[lane] ? 1 : 0); // (a gap row between lanes)
    }
    // A box of its own height (drag its bottom edge to change it), scrolled inside; the wheel zooms
    if (!ImGui::BeginChild("##timeline_box", ImVec2(0, ImGui::GetFontSize() * 22),
                           ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY, ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::EndChild();
        return;
    }
    float width = ImGui::GetContentRegionAvail().x, area_w = width - label_w;
    float height = std::max(float(rows), 1.0f) * row_h + row_h;
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    timeline_input(at.x + label_w, area_w);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(16, 16, 20, 210));

    double span_share = 1.0 / state.zoom;
    double total_ns = double(p.to_ns - p.from_ns);
    double view_from = double(p.from_ns) + (state.centre - span_share / 2) * total_ns;
    double view_ns = span_share * total_ns;
    auto x_of = [&](uint64_t ns) { return at.x + label_w + float((double(ns) - view_from) / view_ns) * area_w; };
    float left = at.x + label_w, right = at.x + width;
    // the picture's edges, and a scale
    for (uint64_t edge : {p.frame.start_ns, p.frame.end_ns}) {
        float x = x_of(edge);
        if (x >= left && x <= right) {
            draw->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + height), IM_COL32(120, 220, 120, 200));
        }
    }
    char text[160];
    std::snprintf(text, sizeof(text), "%.3f ms across", view_ns / 1e6);
    draw->AddText(ImVec2(left + 4, at.y + 1), IM_COL32(200, 200, 200, 220), text);

    const zones::Span *hovered = nullptr;
    int hovered_lane = -1;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    float y = at.y + row_h;
    static const std::vector<zones::Span> none;
    for (size_t lane = 0; lane < state.lane_rows.size(); lane++) {
        const auto &spans = lane < p.spans.size() ? p.spans[lane] : none;
        int depth = state.lane_rows[lane];
        if (depth == 0) {
            continue;
        }
        const std::string &name = lane < state.copy.lanes.size() ? state.copy.lanes[lane].name : std::string("?");
        draw->AddText(ImVec2(at.x + 4, y), IM_COL32(220, 220, 220, 255), name.c_str());
        draw->PushClipRect(ImVec2(left, y), ImVec2(right, y + float(depth) * row_h), true);
        std::vector<float> last_x(size_t(depth), -1e9f); // (spans under a pixel: one drawn a pixel a row)
        for (const zones::Span &s : spans) {
            if (s.depth >= depth) {
                continue;
            }
            float x0 = std::max(x_of(s.start_ns), left - 1), x1 = std::min(x_of(s.end_ns), right + 1);
            if (x1 < left || x0 > right) {
                continue;
            }
            float &last = last_x[s.depth];
            if (x1 - x0 < 1.0f && x0 < last + 1.0f) {
                continue;
            }
            last = x1;
            float top = y + float(s.depth) * row_h;
            ImVec2 a(x0, top), b(std::max(x1, x0 + 1), top + row_h - 1);
            draw->AddRectFilled(a, b, zone_colour(s.id, port_zone_kind(s.id) == ZONE_WAIT && !state.show_waits ? 0.2f : 1.0f));
            if (b.x - a.x > 30) {
                draw->PushClipRect(a, b, true);
                draw->AddText(ImVec2(a.x + 2, a.y), IM_COL32(10, 10, 10, 255), port_zone_name(s.id));
                draw->PopClipRect();
            }
            if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) {
                hovered = &s;
                hovered_lane = int(lane);
            }
        }
        draw->PopClipRect();
        y += float(depth + 1) * row_h;
    }
    if (hovered != nullptr && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\n%s, %s\n%.4f ms (from %.4f ms into the picture)", port_zone_name(hovered->id),
                          state.copy.lanes[size_t(hovered_lane)].name.c_str(), kind_name(port_zone_kind(hovered->id)),
                          ms_of(hovered->end_ns - hovered->start_ns),
                          (double(hovered->start_ns) - double(p.frame.start_ns)) / 1e6);
    }
    ImGui::EndChild();
}

// The rows the table shows: the picked picture's or the window's, filtered
std::vector<Row> table_rows() {
    std::vector<Row> rows;
    if (state.table_source == 1) {
        rows = state.copy.window_rows;
    } else if (state.copy.picked.have) {
        for (const zones::ZoneTime &z : state.copy.picked.frame.zones) {
            rows.push_back({z.lane, z.id, ms_of(z.self_ns), ms_of(z.incl_ns), double(z.calls), ms_of(z.self_ns)});
        }
    }
    std::string filter = state.filter;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const Row &r) {
                                  if (state.lane_filter >= 0 && r.lane != state.lane_filter) {
                                      return true;
                                  }
                                  if (!state.show_waits && port_zone_kind(r.id) == ZONE_WAIT) {
                                      return true;
                                  }
                                  std::string name = port_zone_name(r.id);
                                  std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                                  return !filter.empty() && name.find(filter) == std::string::npos;
                              }),
               rows.end());
    return rows;
}

void sort_rows(std::vector<Row> &rows, const ImGuiTableSortSpecs *specs) {
    if (specs == nullptr || specs->SpecsCount == 0) {
        std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.self_ms > b.self_ms; });
        return;
    }
    const ImGuiTableColumnSortSpecs &s = specs->Specs[0];
    bool up = s.SortDirection == ImGuiSortDirection_Ascending;
    auto key = [&](const Row &r) -> double {
        switch (s.ColumnIndex) {
        case 3: return r.self_ms;
        case 4: return r.incl_ms;
        case 5: return r.calls;
        case 6: return r.max_self_ms;
        default: return 0.0;
        }
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const Row &a, const Row &b) {
        if (s.ColumnIndex == 0 && a.lane != b.lane) {
            return up ? a.lane < b.lane : a.lane > b.lane;
        }
        if (s.ColumnIndex == 1 || s.ColumnIndex == 2) {
            int c = s.ColumnIndex == 1 ? std::strcmp(port_zone_name(a.id), port_zone_name(b.id))
                                       : int(port_zone_kind(a.id)) - int(port_zone_kind(b.id));
            return up ? c < 0 : c > 0;
        }
        return up ? key(a) < key(b) : key(a) > key(b);
    });
}

std::string table_text(const std::vector<Row> &rows) {
    std::string text = "lane\tzone\tkind\tself ms\tincl ms\tcalls\tmost self ms\n";
    char line[320];
    for (const Row &r : rows) {
        const char *lane = size_t(r.lane) < state.copy.lanes.size() ? state.copy.lanes[size_t(r.lane)].name.c_str() : "?";
        std::snprintf(line, sizeof(line), "%s\t%s\t%s\t%.4f\t%.4f\t%.1f\t%.4f\n", lane, port_zone_name(r.id),
                      kind_name(port_zone_kind(r.id)), r.self_ms, r.incl_ms, r.calls, r.max_self_ms);
        text += line;
    }
    return text;
}

void draw_table_controls(const std::vector<Row> &rows) {
    static const char *const sources[] = {"the picked picture", "averaged over the window"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
    ImGui::Combo("##source", &state.table_source, sources, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    const char *lane_label = state.lane_filter >= 0 && size_t(state.lane_filter) < state.copy.lanes.size()
                                 ? state.copy.lanes[size_t(state.lane_filter)].name.c_str()
                                 : "every lane";
    if (ImGui::BeginCombo("##lane", lane_label)) {
        if (ImGui::Selectable("every lane", state.lane_filter < 0)) {
            state.lane_filter = -1;
        }
        for (size_t i = 0; i < state.copy.lanes.size(); i++) {
            if (ImGui::Selectable(state.copy.lanes[i].name.c_str(), state.lane_filter == int(i))) {
                state.lane_filter = int(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    ImGui::InputTextWithHint("##filter", "filter zones", state.filter, sizeof(state.filter));
    ImGui::SameLine();
    ImGui::Checkbox("waits", &state.show_waits);
    ImGui::SameLine();
    if (ImGui::Button("Copy table")) {
        ImGui::SetClipboardText(table_text(rows).c_str());
    }
}

void draw_table() {
    std::vector<Row> rows = table_rows();
    draw_table_controls(rows);
    double frame_ms = state.table_source == 1 ? state.copy.avg_ms
                      : state.copy.picked.have ? state.copy.picked.frame.ms()
                                               : 0.0;
    ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    float height = ImGui::GetFontSize() * 20; // (scrolls: the profile run's section stays below it)
    if (!ImGui::BeginTable("##zones", 8, flags, ImVec2(0, height))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("lane");
    ImGui::TableSetupColumn("zone", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("kind");
    ImGui::TableSetupColumn("self ms", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("incl ms", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("calls", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("most self", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("% picture", ImGuiTableColumnFlags_NoSort);
    ImGui::TableHeadersRow();
    sort_rows(rows, ImGui::TableGetSortSpecs());
    ImGuiListClipper clipper;
    clipper.Begin(int(rows.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            const Row &r = rows[size_t(i)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(size_t(r.lane) < state.copy.lanes.size() ? state.copy.lanes[size_t(r.lane)].name.c_str()
                                                                            : "?");
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, zone_colour(r.id));
            ImGui::TextUnformatted(port_zone_name(r.id));
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kind_name(port_zone_kind(r.id)));
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", r.self_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", r.incl_ms);
            ImGui::TableNextColumn();
            ImGui::Text(state.table_source == 1 ? "%.1f" : "%.0f", r.calls);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", r.max_self_ms);
            ImGui::TableNextColumn();
            if (frame_ms > 0) {
                ImGui::Text("%.1f%%", r.self_ms / frame_ms * 100.0);
            }
        }
    }
    ImGui::EndTable();
}

} // namespace

void frames_view_show(bool show) {
    if (wanted_shown.exchange(show) != show) {
        port_frame_profiler_want(show ? 1 : -1);
    }
}

void frames_view_draw() {
    bool show = wanted_shown.load();
    if (show != state.shown) {
        state.shown = show;
        state.dirty = true;
    }
    if (!state.shown) {
        return;
    }
    refresh();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1000, 760), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (!ImGui::Begin("Profiler (Shift+P)", nullptr, flags)) {
        ImGui::End();
        return;
    }
    draw_screen();
    ImGui::Separator();
    draw_toolbar();
    if (state.copy.bars.empty()) {
        ImGui::TextUnformatted("Recording: the first pictures are on their way (a picture is done once the GPU's times are in).");
    } else {
        draw_summary();
        draw_graph();
        draw_spikes();
        if (ImGui::CollapsingHeader("Timeline", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_timeline();
        }
        if (ImGui::CollapsingHeader("Zones", ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_table();
        }
    }
    // (every function by the sampler, threads without zones and the driver's code too: what to give a zone next)
    profile_run_view_draw();
    ImGui::End();
}
