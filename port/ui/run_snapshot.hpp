// What the profiler overlay (profiler_overlay.hpp, Shift+P) shows of a profile run (port/include/port/profile_run.h):
// each thread's functions by share of its CPU time (the sampler's, port/include/port/sampler.h; names demangled) and
// the GPU's time a picture by kind of work and by shader state (port/include/port/profile.h).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct SampledFunction {
    std::string name;
    uint64_t self, inclusive; // samples
};

struct SampledThread {
    int tid;
    std::string name;
    double cpu_ms;
    uint64_t samples;
    bool signals_blocked; // it blocks the sampler's signal: CPU time only
    std::vector<SampledFunction> functions; // by self samples, most first
};

struct GpuWork {
    std::string name;
    double ms_per_picture;
    uint64_t intervals; // timed
};

struct RunSnapshot {
    bool recording = false;
    bool sampler_available = false; // (Linux only)
    double seconds = 0;
    uint64_t missed = 0;              // sample requests unanswered
    std::vector<SampledThread> threads; // by samples, most first
    std::vector<GpuWork> kinds;       // by ms, most first
    std::vector<GpuWork> shaders;     // (with the GPU's detail on) by ms, most first
    double gpu_ms_per_picture = 0;
    uint64_t pictures = 0;
};

// The run as it is now (cheap enough twice a second: the sampler's report and the GPU's table are copied)
RunSnapshot read_profile_run();

// The run's tables as text, for the clipboard
std::string run_text(const RunSnapshot &run);

} // namespace ui
