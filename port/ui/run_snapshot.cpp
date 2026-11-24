// The profiler overlay's copy of a profile run (run_snapshot.hpp).
#include "run_snapshot.hpp"

#include <cxxabi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

extern "C" {
#include "port/profile.h"
#include "port/profile_run.h"
#include "port/sampler.h"
}

namespace ui {

namespace {

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

void read_sampler(RunSnapshot &run) {
    static const std::unique_ptr<SamplerReport> report = std::make_unique<SamplerReport>(); // (large: kept)
    if (!sampler_report(report.get())) {
        return;
    }
    run.seconds = report->seconds;
    run.missed = report->missed;
    for (int i = 0; i < report->thread_count && i < SAMPLER_THREADS; i++) {
        const SamplerThread &t = report->threads[i];
        SampledThread shown = {t.tid, t.name, t.cpu_ms, t.samples, t.signals_blocked != 0, {}};
        for (int k = 0; k < t.function_count && k < SAMPLER_FUNCTIONS; k++) {
            shown.functions.push_back({demangled(t.functions[k].name), t.functions[k].self, t.functions[k].inclusive});
        }
        std::stable_sort(shown.functions.begin(), shown.functions.end(),
                         [](const SampledFunction &a, const SampledFunction &b) { return a.self > b.self; });
        run.threads.push_back(std::move(shown));
    }
    std::stable_sort(run.threads.begin(), run.threads.end(),
                     [](const SampledThread &a, const SampledThread &b) { return a.samples > b.samples; });
}

void read_gpu(RunSnapshot &run) {
    static ProfileGpuRow rows[PROFILE_GPU_ROWS];
    int n = port_profile_gpu_rows(rows, PROFILE_GPU_ROWS);
    run.pictures = port_profile_gpu_pictures();
    double per = run.pictures ? 1.0 / double(run.pictures) : 0.0;
    for (int i = 0; i < n; i++) {
        GpuWork work = {rows[i].name, double(rows[i].ns) / 1e6 * per, rows[i].intervals};
        if (rows[i].level == PROFILE_GPU_KIND) {
            run.gpu_ms_per_picture += work.ms_per_picture;
            run.kinds.push_back(work);
        } else {
            run.shaders.push_back(work);
        }
    }
    auto by_ms = [](const GpuWork &a, const GpuWork &b) { return a.ms_per_picture > b.ms_per_picture; };
    std::stable_sort(run.kinds.begin(), run.kinds.end(), by_ms);
    std::stable_sort(run.shaders.begin(), run.shaders.end(), by_ms);
}

} // namespace

RunSnapshot read_profile_run() {
    RunSnapshot run;
    run.recording = port_profile_run_recording() != 0;
    run.sampler_available = sampler_available() != 0;
    read_sampler(run);
    read_gpu(run);
    return run;
}

std::string run_text(const RunSnapshot &run) {
    std::string text;
    char line[400];
    std::snprintf(line, sizeof(line), "Holyland profile run: %.1f s, %llu pictures, GPU %.2f ms a picture\n", run.seconds,
                  (unsigned long long)run.pictures, run.gpu_ms_per_picture);
    text += line;
    for (const SampledThread &t : run.threads) {
        if (t.samples == 0) {
            continue;
        }
        std::snprintf(line, sizeof(line), "\n%s (%d): %.0f ms CPU, %llu samples\n  incl%%   self%%\n",
                      t.name.empty() ? "?" : t.name.c_str(), t.tid, t.cpu_ms, (unsigned long long)t.samples);
        text += line;
        for (const SampledFunction &f : t.functions) {
            std::snprintf(line, sizeof(line), "  %5.1f  %5.1f  %s\n", 100.0 * double(f.inclusive) / double(t.samples),
                          100.0 * double(f.self) / double(t.samples), f.name.c_str());
            text += line;
        }
    }
    text += "\nGPU ms a picture, by kind of work\n";
    for (const GpuWork &w : run.kinds) {
        std::snprintf(line, sizeof(line), "  %7.3f  %s\n", w.ms_per_picture, w.name.c_str());
        text += line;
    }
    if (!run.shaders.empty()) {
        text += "GPU ms a picture, by shader state\n";
        for (const GpuWork &w : run.shaders) {
            std::snprintf(line, sizeof(line), "  %7.3f  %8llu  %s\n", w.ms_per_picture, (unsigned long long)w.intervals,
                          w.name.c_str());
            text += line;
        }
    }
    return text;
}

} // namespace ui
