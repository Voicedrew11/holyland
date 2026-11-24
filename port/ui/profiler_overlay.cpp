// The profiler overlay (profiler_overlay.hpp).
//
// update reads the frame profiler's record (zones::with_record, never waited for: a busy record is tried again at the
// next picture) every READ_SECONDS and the profile run every RUN_READ_SECONDS, then writes the document's parts from
// those copies, each given to RmlUi only when its text changed. Held (Shift+O), nothing is read again.
#include "profiler_overlay.hpp"

#include <SDL3/SDL.h>

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "frame_snapshot.hpp"
#include "profiler_charts.hpp"
#include "run_snapshot.hpp"

#include "port/frame_profiler.h"

extern "C" {
#include "port/host_load.h"
#include "port/platform.h"
#include "port/profile.h"
#include "port/profile_run.h"
#include "port/version.h"
#include "port/zones.h"
}

namespace ui {

namespace {

const double READ_SECONDS = 0.25;
const double RUN_READ_SECONDS = 0.5;
const double SHOWN_EVERY_SECONDS = 0.5; // the pictures shown a second, read this often for their chart
const size_t SHOWN_HISTORY = 60;        // readings of it kept: half a minute
const double COPIED_NOTE_SECONDS = 3.0;

// The timeline's plot in rows (profiler.rcss: #timeline-plot, 248dp): the first for the time scale, the rest the
// lanes', each at most DEPTH_MOST deep (fewer when the lanes would not fit)
const int TIMELINE_ROWS = 24;
const int DEPTH_MOST = 8;
const float LABEL_SHARE = 0.035f; // a box at least this wide shows its zone's name
const size_t LABELS_MOST = 160;
const int TICKS_WANTED = 8;

// The tables' lines, titles and column heads included: as many as fit (profiler.rcss: a line 13dp), and at least
// those of a window 780dp high
const int TABLE_LINES_LEAST = 23;
const float TABLE_LINE_DP = 13.0f;
const int THREADS_LEAST = 3;
const int FUNCTIONS_A_THREAD = 5;

const char *const GPU_DETAILS[] = {"by kind of work", "by shader state", "each draw (slow)"};

// ---- text ----------------------------------------------------------------------------------------------------------

std::string format(const char *pattern, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *pattern, ...) {
    char text[512];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(text, sizeof(text), pattern, args);
    va_end(args);
    return text;
}

// Text as RML: its markup characters escaped
std::string escaped(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

// RmlUi drops a text of spaces alone between two tags and runs of spaces collapse: outside the tags, such a space
// becomes a space's width (<sp>) and two or more spaces a gap (<g>)
std::string spaced(std::string_view rml) {
    std::string out;
    out.reserve(rml.size() + 64);
    bool in_tag = false;
    for (size_t i = 0; i < rml.size();) {
        char c = rml[i];
        if (in_tag || c != ' ') {
            in_tag = c == '<' ? true : c == '>' ? false : in_tag;
            out += c;
            i++;
            continue;
        }
        size_t end = rml.find_first_not_of(' ', i);
        end = end == std::string_view::npos ? rml.size() : end;
        bool alone = i > 0 && rml[i - 1] == '>' && end < rml.size() && rml[end] == '<';
        out += end - i >= 2 ? "<g></g>" : alone ? "<sp></sp>" : " ";
        i = end;
    }
    return out;
}

std::string number(double value, const char *pattern = "%.3f", const char *cls = nullptr) {
    std::string out = cls ? format("<n class=\"%s\">", cls) : std::string("<n>");
    return out + format(pattern, value) + "</n>";
}

// (blank when nothing: a lane that never waits)
std::string number_or_blank(double value, const char *pattern = "%.3f") {
    return value > 0 ? number(value, pattern) : std::string("<n></n>");
}

std::string heading(const char *title, const std::string &dim = {}) {
    return std::string("<h>") + title + (dim.empty() ? "" : "  <dim>" + dim + "</dim>") + "</h>";
}

std::string row(const std::string &inner, const char *cls = "r") {
    return std::string("<div class=\"") + cls + "\">" + inner + "</div>";
}

// ---- a zone's colour: its own hue; waits grey, the GPU's warm ------------------------------------------------------

Rml::Colourb hsv(float h, float s, float v) {
    float r = v, g = v, b = v;
    if (s > 0) {
        h = std::fmod(h, 1.0f) * 6.0f;
        int i = int(h);
        float f = h - float(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        switch (i) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        default: r = v, g = p, b = q; break;
        }
    }
    return Rml::Colourb(Rml::byte(r * 255), Rml::byte(g * 255), Rml::byte(b * 255), 255);
}

Rml::Colourb zone_colour(ZoneId id) {
    float hue = std::fmod(float(id) * 0.6180339f, 1.0f);
    switch (port_zone_kind(id)) {
    case ZONE_WAIT: return hsv(hue, 0.08f, 0.55f);
    case ZONE_GPU: return hsv(0.02f + hue * 0.12f, 0.65f, 0.92f);
    default: return hsv(0.40f + hue * 0.45f, 0.50f, 0.85f);
    }
}

std::string css_colour(Rml::Colourb c) {
    return format("#%02x%02x%02x", c.red, c.green, c.blue);
}

const char *kind_name(ZoneKind kind) {
    static const char *const names[ZONE_KINDS] = {"work", "wait", "gpu"};
    return unsigned(kind) < ZONE_KINDS ? names[kind] : "?";
}

double target_per_second() {
    // (at the PS2's 30 the window still shows a picture each vblank: 60)
    return port_frame_rate_hz() > 60 ? double(port_frame_rate_hz()) : 60.0;
}

std::string lane_name(const FrameSnapshot &f, int lane) {
    return size_t(lane) < f.lanes.size() ? f.lanes[size_t(lane)].name : std::string("?");
}

} // namespace

// ---- the overlay's state -------------------------------------------------------------------------------------------

struct ProfilerOverlay::State {
    Rml::ElementDocument *document = nullptr;
    bool shown = false;
    bool held = false;
    TimelineChoice choice = TimelineChoice::newest;
    FrameReader reader;
    FrameSnapshot frames;
    RunSnapshot run;
    double read_s = -1, run_read_s = -1, shown_read_s = -1;
    bool fresh = true; // read (or a key changed what shows) since the parts were written
    std::vector<float> shown_history; // oldest first
    std::vector<int> timeline_rows;   // a lane's rows, the most it has needed since shown (rows keep their places)
    std::string saved;                // the run's last Save: where, or why not
    double copied_s = -1;
    int table_lines = TABLE_LINES_LEAST; // (as laid out at the last picture)
    std::map<std::string, std::string> parts; // what each element was last given
};

namespace {

using State = ProfilerOverlay::State;

void set_part(State &s, const char *id, std::string_view text) {
    std::string rml = spaced(text);
    std::string &last = s.parts[id];
    if (last == rml) {
        return;
    }
    if (Rml::Element *element = s.document->GetElementById(id)) {
        element->SetInnerRML(rml);
    }
    last = std::move(rml);
}

// ---- reading -------------------------------------------------------------------------------------------------------

void read_shown(State &s, double now) {
    if (now - s.shown_read_s < SHOWN_EVERY_SECONDS) {
        return;
    }
    s.shown_read_s = now;
    s.shown_history.push_back(float(port_video_pictures_per_second()));
    if (s.shown_history.size() > SHOWN_HISTORY) {
        s.shown_history.erase(s.shown_history.begin());
    }
    s.fresh = true;
}

void read(State &s) {
    double now = port_wall_seconds();
    read_shown(s, now);
    if (now - s.read_s >= READ_SECONDS &&
        zones::with_record([&](zones::Record &r) { s.frames = s.reader.read(r, s.choice); })) {
        s.read_s = now;
        s.fresh = true;
    }
    if (now - s.run_read_s >= RUN_READ_SECONDS) {
        s.run = read_profile_run();
        s.run_read_s = now;
        s.fresh = true;
    }
}

// ---- the head ------------------------------------------------------------------------------------------------------

std::string cpu_text() {
    const PortHostLoad load = port_host_load();
    std::string cpu = "CPU: speed unknown";
    if (load.cpu_mhz > 0 && load.cpu_limit_mhz > 0) {
        cpu = format("CPU %.0f MHz <dim>(base %.0f, held to %.0f)</dim>", load.cpu_mhz, load.cpu_most_mhz,
                     load.cpu_limit_mhz);
    } else if (load.cpu_limit_mhz > 0) {
        cpu = format("CPU <dim>base</dim> %.0f MHz<dim>, held to</dim> %.0f", load.cpu_most_mhz, load.cpu_limit_mhz);
    } else if (load.cpu_mhz > 0) {
        cpu = format("CPU %.0f MHz <dim>(most %.0f)</dim>", load.cpu_mhz, load.cpu_most_mhz);
    }
    return cpu + format("   <dim>game's memory</dim> %.0f MB", load.memory_mb);
}

std::string screen_rml(const State &s) {
    float latest = s.shown_history.empty() ? 0.0f : s.shown_history.back();
    std::string out = format("<div>Pictures shown <lit>%.0f</lit> a second <dim>of %.0f</dim>", double(latest),
                             target_per_second());
    if (!port_build_optimized) {
        out += "   <warn>not an optimized build: several times slower</warn>";
    }
    out += "</div><div>" + cpu_text() + "</div><div>";
    const PortClockCost clock = port_clock_cost();
    out += clock.read_ns >= PORT_CLOCK_SLOW_NS
               ? format("<warn>Clock %.0f ns a read (%.3f MHz): slow</warn>", clock.read_ns, clock.counter_mhz)
               : format("Clock %.0f ns a read <dim>(%.3f MHz)</dim>", clock.read_ns, clock.counter_mhz);
    ProfileCompiles compiles = port_profile_compiles();
    out += compiles.count == 0 ? std::string("   <dim>pipelines made while a draw waited:</dim> none")
                               : format("   <warn>pipelines made while a draw waited: %llu, %.1f ms, longest %.1f</warn>",
                                        (unsigned long long)compiles.count, double(compiles.ns) / 1e6,
                                        double(compiles.longest_ns) / 1e6);
    return out + "</div>";
}

std::string summary_rml(const State &s) {
    const FrameSnapshot &f = s.frames;
    if (f.bars.empty()) {
        return "<div><dim>Recording: the first pictures are on their way</dim></div>"
               "<div><dim>(a picture is done once the GPU's times are in)</dim></div>";
    }
    std::string out = format("<div><dim>Last %.0f s:</dim> <lit>%.1f</lit> pictures a second <dim>over %zu</dim></div>",
                             FrameReader::WINDOW_SECONDS, f.per_second, f.window_frames);
    out += format("<div><lit>%.2f</lit> ms average   <lit>%.2f</lit> p99   <lit>%.2f</lit> most</div>", f.avg_ms,
                  f.p99_ms, f.most_ms);
    out += format("<div>Spikes over %.1f ms: %zu", f.spike_ms, f.spikes.size());
    if (f.lost || f.unmatched) {
        out += format("   <warn>events lost %llu, unmatched %llu</warn>", (unsigned long long)f.lost,
                      (unsigned long long)f.unmatched);
    }
    return out + "</div>";
}

std::string title_rml(const State &s) {
    return std::string("Profiler") + (s.held ? "<div><warn>held</warn></div>" : "");
}

void write_shown(State &s) {
    float top = float(target_per_second() * 1.1);
    for (float v : s.shown_history) {
        top = std::max(top, v);
    }
    set_chart(s.document->GetElementById("shown"), ColumnsChart{s.shown_history, SHOWN_HISTORY, top, float(target_per_second())});
    set_part(s, "shown-words",
             format("<div class=\"at\" style=\"left: 3dp; top: 1dp;\">pictures shown a second, last 30 s</div>"
                    "<div class=\"at right\" style=\"right: 3dp; top: 1dp;\">%.0f</div>",
                    double(top)));
}

// ---- the bars ------------------------------------------------------------------------------------------------------

void write_bars(State &s) {
    const FrameSnapshot &f = s.frames;
    float budget = float(1000.0 / target_per_second());
    float top = float(std::max({f.p99_ms * 1.5, f.spike_ms * 1.1, double(budget) * 1.5}));
    int64_t marked = f.timeline.have ? int64_t(f.timeline.frame.index) : -1;
    set_chart(s.document->GetElementById("bars"), BarsChart{f.bars, top, float(f.spike_ms), budget, marked});
    std::string words = format("<div class=\"at\" style=\"left: 3dp; top: 1dp;\">%.1f ms</div>", double(top));
    words += format("<div class=\"at right\" style=\"right: 3dp; top: %.1f%%;\">%.2f ms: %.0f a second</div>",
                    std::max(0.0, 100.0 * (1.0 - double(budget / top))), double(budget), target_per_second());
    words += "<div class=\"at right\" style=\"right: 3dp; top: 1dp;\">a bar a picture:  <ice>its length</ice>   "
             "<gold>GPU busy</gold>   <warn>a spike</warn>   <lit>the timeline's</lit>   "
             "<span style=\"color: #8fd0a0;\">the screen's time</span></div>";
    set_part(s, "bars-words", words);
}

// ---- the timeline --------------------------------------------------------------------------------------------------

// Each lane's rows in the plot (0: not shown), fitted to it
std::vector<int> fit_rows(State &s) {
    const TimelinePicture &t = s.frames.timeline;
    s.timeline_rows.resize(std::max(s.timeline_rows.size(), t.spans.size()), 0);
    for (size_t lane = 0; lane < t.spans.size(); lane++) {
        int depth = 0;
        for (const zones::Span &span : t.spans[lane]) {
            depth = std::max(depth, int(span.depth) + 1);
        }
        s.timeline_rows[lane] = std::max(s.timeline_rows[lane], depth);
    }
    std::vector<int> rows(s.timeline_rows.size(), 0);
    for (int cap = DEPTH_MOST; cap >= 1; cap--) {
        int total = 0;
        for (size_t lane = 0; lane < rows.size(); lane++) {
            rows[lane] = std::min(s.timeline_rows[lane], cap);
            total += rows[lane];
        }
        if (total <= TIMELINE_ROWS - 1) {
            return rows;
        }
    }
    // (more lanes than rows: the first that fit, one row each)
    int left = TIMELINE_ROWS - 1;
    for (int &r : rows) {
        r = r > 0 && left > 0 ? 1 : 0;
        left -= r;
    }
    return rows;
}

// A step of 1, 2 or 5 times a power of ten giving about TICKS_WANTED ticks across ms
double tick_step(double ms) {
    double raw = ms / TICKS_WANTED, power = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (raw <= m * power) {
            return m * power;
        }
    }
    return 10.0 * power;
}

std::string percent(double share) {
    return format("%.3f%%", share * 100.0);
}

void write_timeline(State &s) {
    const FrameSnapshot &f = s.frames;
    const TimelinePicture &t = f.timeline;
    if (!t.have) {
        set_part(s, "timeline-head", "<dim>Timeline: no picture done yet</dim>");
        set_chart(s.document->GetElementById("timeline"), TimelineChart{});
        set_part(s, "timeline-names", "");
        set_part(s, "timeline-words", "");
        return;
    }
    double across_ns = double(t.to_ns - t.from_ns);
    auto share_of = [&](uint64_t ns) { return float((double(ns) - double(t.from_ns)) / across_ns); };
    set_part(s, "timeline-head",
             format("<ice>Timeline</ice>   picture %llu: <lit>%.3f ms</lit> <dim>(%s)</dim>   <dim>%.3f ms across; "
                    "the green lines are its presents; each lane's zones nested under the one they ran in</dim>",
                    (unsigned long long)t.frame.index, t.frame.ms(),
                    s.choice == TimelineChoice::newest ? "the newest" : "the slowest of the last 3 s", across_ns / 1e6));

    std::vector<int> rows = fit_rows(s);
    TimelineChart chart;
    chart.rows = TIMELINE_ROWS;
    chart.lines = {share_of(t.frame.start_ns), share_of(t.frame.end_ns)};
    std::string names, words;
    // the time scale on the first row: ms from the picture's start
    double step = tick_step(across_ns / 1e6);
    double start_ms = (double(t.frame.start_ns) - double(t.from_ns)) / 1e6;
    for (double ms = -std::floor(start_ms / step) * step; ms <= across_ns / 1e6 - start_ms; ms += step) {
        float share = float((start_ms + ms) / (across_ns / 1e6));
        chart.ticks.push_back(share);
        words += format("<div class=\"at\" style=\"left: %s; top: 0;\">%+g ms</div>", percent(share).c_str(), ms);
    }
    int at_row = 1;
    size_t labels = 0;
    for (size_t lane = 0; lane < rows.size(); lane++) {
        if (rows[lane] == 0) {
            continue;
        }
        names += format("<div class=\"lane-name\" style=\"top: %s;\">%s</div>",
                        percent(double(at_row) / TIMELINE_ROWS).c_str(), escaped(lane_name(f, int(lane))).c_str());
        for (const zones::Span &span : lane < t.spans.size() ? t.spans[lane] : std::vector<zones::Span>{}) {
            if (span.depth >= rows[lane]) {
                continue;
            }
            int box_row = at_row + span.depth;
            float from = share_of(span.start_ns), to = share_of(span.end_ns);
            chart.boxes.push_back({from, to, box_row, zone_colour(span.id)});
            float shown_from = std::max(from, 0.0f), shown_to = std::min(to, 1.0f);
            if (shown_to - shown_from >= LABEL_SHARE && labels < LABELS_MOST) {
                labels++;
                words += format("<div class=\"span\" style=\"left: %s; width: %s; top: %s;\">%s</div>",
                                percent(shown_from).c_str(), percent(shown_to - shown_from).c_str(),
                                percent(double(box_row) / TIMELINE_ROWS).c_str(),
                                escaped(port_zone_name(span.id)).c_str());
            }
        }
        at_row += rows[lane];
    }
    set_chart(s.document->GetElementById("timeline"), std::move(chart));
    set_part(s, "timeline-names", names);
    set_part(s, "timeline-words", words);
}

// ---- the tables ----------------------------------------------------------------------------------------------------

std::string lanes_rml(const State &s) {
    const FrameSnapshot &f = s.frames;
    std::string out = heading("Lanes", "ms a picture");
    out += row("<ln class=\"wide\"></ln><n>work</n><n>wait</n><n>GPU</n>", "r top");
    int lines = 2;
    for (const LaneTimes &l : f.lanes) {
        out += row("<ln class=\"wide\">" + escaped(l.name) + "</ln>" + number_or_blank(l.ms[ZONE_WORK]) +
                   number_or_blank(l.ms[ZONE_WAIT]) + number_or_blank(l.ms[ZONE_GPU]));
        lines++;
    }
    out += "<div class=\"gap\"></div>" + heading("Spikes", format("over %.1f ms, newest first", f.spike_ms));
    lines += 2;
    out += row("<n class=\"wide\">picture</n><n>ms</n><n>GPU</n>", "r top");
    lines++;
    if (f.spikes.empty()) {
        out += row("<dim>none in the record</dim>");
    }
    for (size_t i = 0; i < f.spikes.size() && lines < s.table_lines; i++, lines++) {
        const FrameBar &b = f.spikes[i];
        out += row(number(double(b.index), "%.0f", "wide") + number(b.ms, "%.2f") + number(b.gpu_ms, "%.2f"));
    }
    return out;
}

std::string zones_rml(const State &s) {
    const FrameSnapshot &f = s.frames;
    std::string out = heading("Zones", format("ms a picture over the last %.0f s; pic: the timeline's picture",
                                              FrameReader::WINDOW_SECONDS));
    out += row("<n>self</n><n>incl.</n><n class=\"narrow\">calls</n><n>most</n><n>pic</n><n class=\"narrow\">%</n>"
               "<ln>lane</ln>zone",
               "r top");
    int shown = 0;
    for (const ZoneTimes &z : f.zones) {
        if (shown++ >= s.table_lines - 2) {
            break;
        }
        std::string name = "<span style=\"color: " + css_colour(zone_colour(z.id)) + ";\">" +
                           escaped(port_zone_name(z.id)) + "</span>";
        if (port_zone_kind(z.id) != ZONE_WORK) {
            name += format(" <off>%s</off>", kind_name(port_zone_kind(z.id)));
        }
        out += row(number(z.self_ms) + number(z.incl_ms) + number(z.calls, "%.1f", "narrow") + number(z.most_self_ms) +
                   number_or_blank(z.picture_self_ms) +
                   number(f.avg_ms > 0 ? z.self_ms / f.avg_ms * 100.0 : 0.0, "%.1f", "narrow") + "<ln>" +
                   escaped(lane_name(f, z.lane)) + "</ln>" + name);
    }
    if (f.zones.empty()) {
        out += row("<dim>no zones in the window</dim>");
    }
    return out;
}

std::string functions_rml(const State &s) {
    const RunSnapshot &run = s.run;
    std::string out = heading("Functions", "the profile run's: share of each thread's CPU time");
    if (!run.sampler_available) {
        return out + row("<dim>Linux only (on Windows: tools/sampler/profile.py)</dim>");
    }
    std::string status = run.recording ? format("<warn>recording</warn> %.1f s", run.seconds)
                         : run.seconds > 0 ? format("stopped after %.1f s", run.seconds)
                                           : std::string("<dim>no run yet: Shift+H starts one</dim>");
    if (run.missed) {
        status += "  <dim>(some samples missed)</dim>";
    }
    if (!s.saved.empty()) {
        status += "  <dim>saved: " + escaped(s.saved) + "</dim>";
    }
    out += row(status);
    if (!run.threads.empty()) {
        out += row("<n>self</n><n>incl.</n><dim>function, the busiest threads first</dim>", "r top");
    }
    int shown = 0;
    for (const SampledThread &t : run.threads) {
        if (shown >= std::max(THREADS_LEAST, (s.table_lines - 3) / (1 + FUNCTIONS_A_THREAD))) {
            break;
        }
        if (t.samples == 0) {
            continue;
        }
        shown++;
        out += row(format("<ice>%s</ice> <dim>(%d): %.0f ms CPU, %.0f%% of a core</dim>",
                          escaped(t.name.empty() ? "?" : t.name).c_str(), t.tid, t.cpu_ms,
                          run.seconds > 0 ? t.cpu_ms / 10.0 / run.seconds : 0.0));
        for (size_t i = 0; i < t.functions.size() && int(i) < FUNCTIONS_A_THREAD; i++) {
            const SampledFunction &fn = t.functions[i];
            out += row(number(100.0 * double(fn.self) / double(t.samples), "%.1f%%") +
                       number(100.0 * double(fn.inclusive) / double(t.samples), "%.1f%%") + escaped(fn.name));
        }
    }
    return out;
}

std::string gpu_rml(const State &s) {
    const RunSnapshot &run = s.run;
    int detail = std::clamp(port_profile_gpu_detail(), 0, 2);
    std::string out = heading("GPU", format("timed %s", GPU_DETAILS[detail]));
    if (run.kinds.empty()) {
        return out + row("<dim>no times yet: a profile run records them (Shift+H)</dim>");
    }
    out += row(format("<lit>%.2f</lit> ms a picture <dim>over %llu pictures</dim>", run.gpu_ms_per_picture,
                      (unsigned long long)run.pictures));
    int lines = 2;
    for (const GpuWork &w : run.kinds) {
        if (lines >= s.table_lines) {
            break;
        }
        out += row(number(w.ms_per_picture) + escaped(w.name));
        lines++;
    }
    if (!run.shaders.empty() && lines + 2 < s.table_lines) {
        out += heading("By shader state", "ms, runs");
        lines++;
        for (const GpuWork &w : run.shaders) {
            if (lines >= s.table_lines) {
                break;
            }
            out += row(number(w.ms_per_picture) + number(double(w.intervals), "%.0f") + escaped(w.name));
            lines++;
        }
    }
    return out;
}

std::string keys_rml(const State &s) {
    int detail = std::clamp(port_profile_gpu_detail(), 0, 2);
    bool copied = s.copied_s >= 0 && port_wall_seconds() - s.copied_s < COPIED_NOTE_SECONDS;
    return std::string("<key>Shift+P</key>hide   ") + "<key>Shift+O</key>" + (s.held ? "go on" : "hold") + "   " +
           "<key>Shift+U</key>timeline: " + (s.choice == TimelineChoice::newest ? "the slowest" : "the newest") + "   " +
           "<key>Shift+Y</key>GPU " + GPU_DETAILS[(detail + 1) % 3] + "   " + "<key>Shift+H</key>" +
           (s.run.recording ? "stop the run" : "start a run") + "   " + "<key>Shift+B</key>save the run   " +
           "<key>Shift+N</key>" + (copied ? "<lit>copied</lit>" : "copy everything") +
           "   <dim>The game keeps its keys, mouse and controller.</dim>";
}

// The tables' lines that fit their column, as laid out
int fitting_lines(const State &s) {
    Rml::Element *column = s.document->GetElementById("lanes");
    Rml::Context *context = s.document->GetContext();
    if (column == nullptr || context == nullptr) {
        return TABLE_LINES_LEAST;
    }
    float line_px = TABLE_LINE_DP * context->GetDensityIndependentPixelRatio();
    return std::max(TABLE_LINES_LEAST, int((column->GetClientHeight() - line_px * 0.5f) / line_px));
}

void write(State &s) {
    s.table_lines = fitting_lines(s);
    set_part(s, "title", title_rml(s));
    set_part(s, "screen", screen_rml(s));
    set_part(s, "summary", summary_rml(s));
    write_shown(s);
    write_bars(s);
    write_timeline(s);
    set_part(s, "lanes", lanes_rml(s));
    set_part(s, "zones", zones_rml(s));
    set_part(s, "functions", functions_rml(s));
    set_part(s, "gpu", gpu_rml(s));
    set_part(s, "keys", keys_rml(s));
}

// ---- copied as text ------------------------------------------------------------------------------------------------

std::string everything_text(const State &s) {
    const FrameSnapshot &f = s.frames;
    std::string text = format("Holyland profiler: last %.0f s, %.1f pictures a second over %zu: %.2f ms average, "
                              "%.2f p99, %.2f most; spikes over %.1f ms: %zu\n",
                              FrameReader::WINDOW_SECONDS, f.per_second, f.window_frames, f.avg_ms, f.p99_ms,
                              f.most_ms, f.spike_ms, f.spikes.size());
    text += "\nlane\twork ms\twait ms\tGPU ms (a picture)\n";
    for (const LaneTimes &l : f.lanes) {
        text += format("%s\t%.4f\t%.4f\t%.4f\n", l.name.c_str(), l.ms[ZONE_WORK], l.ms[ZONE_WAIT], l.ms[ZONE_GPU]);
    }
    text += "\nlane\tzone\tkind\tself ms\tincl ms\tcalls\tmost self ms\n";
    for (const ZoneTimes &z : f.zones) {
        text += format("%s\t%s\t%s\t%.4f\t%.4f\t%.1f\t%.4f\n", lane_name(f, z.lane).c_str(), port_zone_name(z.id),
                       kind_name(port_zone_kind(z.id)), z.self_ms, z.incl_ms, z.calls, z.most_self_ms);
    }
    text += "\nspikes: picture\tms\tGPU ms\n";
    for (const FrameBar &b : f.spikes) {
        text += format("%llu\t%.3f\t%.3f\n", (unsigned long long)b.index, double(b.ms), double(b.gpu_ms));
    }
    return text + "\n" + run_text(s.run);
}

// ---- the keys ------------------------------------------------------------------------------------------------------

void toggle_run() {
    if (port_profile_run_recording()) {
        port_profile_run_stop();
    } else {
        port_profile_run_start();
    }
}

void save_run(State &s) {
    char path[1100];
    port_profile_run_default_path(path, int(sizeof(path)));
    s.saved = port_profile_run_save(path) ? path : std::string("could not write ") + path;
}

void copy_everything(State &s) {
    if (SDL_SetClipboardText(everything_text(s).c_str())) {
        s.copied_s = port_wall_seconds();
    } else {
        std::fprintf(stderr, "[profiler] the clipboard did not take the text: %s\n", SDL_GetError());
    }
}

} // namespace

ProfilerOverlay::ProfilerOverlay() : state_(std::make_unique<State>()) {
}

ProfilerOverlay::~ProfilerOverlay() = default;

bool ProfilerOverlay::load(Rml::Context &context) {
    state_->document = context.LoadDocument("profiler.rml");
    if (state_->document == nullptr) {
        std::fprintf(stderr, "[ui] the profiler's document did not load: no profiler\n");
        return false;
    }
    return true;
}

bool ProfilerOverlay::event(const SDL_Event &event) {
    if (state_->document == nullptr || event.type != SDL_EVENT_KEY_DOWN || event.key.repeat ||
        !(event.key.mod & SDL_KMOD_SHIFT) || (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
        return false;
    }
    State &s = *state_;
    if (event.key.scancode == SDL_SCANCODE_P) {
        show(!s.shown);
        return true;
    }
    if (!s.shown) {
        return false;
    }
    switch (event.key.scancode) {
    case SDL_SCANCODE_O: s.held = !s.held; break;
    case SDL_SCANCODE_U:
        s.choice = s.choice == TimelineChoice::newest ? TimelineChoice::slowest : TimelineChoice::newest;
        s.read_s = -1; // (at once)
        break;
    case SDL_SCANCODE_Y: port_profile_gpu_set_detail((std::clamp(port_profile_gpu_detail(), 0, 2) + 1) % 3); break;
    case SDL_SCANCODE_H:
        toggle_run();
        s.run_read_s = -1;
        break;
    case SDL_SCANCODE_B: save_run(s); break;
    case SDL_SCANCODE_N: copy_everything(s); break;
    default: return false;
    }
    s.fresh = true;
    return true;
}

void ProfilerOverlay::show(bool shown) {
    State &s = *state_;
    if (s.document == nullptr || shown == s.shown) {
        return;
    }
    s.shown = shown;
    port_frame_profiler_want(shown ? 1 : -1);
    if (shown) {
        s.timeline_rows.clear();
        s.read_s = s.run_read_s = -1;
        s.fresh = true;
        s.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        return;
    }
    s.document->Hide();
    // (the record busy: the slowest picture's spans are let go at the next read instead)
    zones::with_record([&](zones::Record &r) { s.reader.forget(r); });
}

bool ProfilerOverlay::shown() const {
    return state_->shown;
}

void ProfilerOverlay::update() {
    State &s = *state_;
    if (!s.shown) {
        return;
    }
    if (!s.held) {
        read(s);
    }
    if (s.fresh) {
        write(s);
        s.fresh = false;
    }
}

} // namespace ui
