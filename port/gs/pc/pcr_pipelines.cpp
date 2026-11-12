// The PC renderer's pipelines made ahead when MSAA's samples change (pcr_pipelines.h)
#include "pcr_pipelines.h"

#include <stdio.h>
#include <stdlib.h>

#include <array>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "command_buffer.hpp"

namespace Vulkan {
// (cmake/granite_pipeline_warm.inc)
extern void (*holyland_on_compile)(Device *device, const DeferredPipelineCompile &compile, bool compute, int64_t ns,
                                   bool sync, bool ok);
extern void (*holyland_on_render_pass)(Util::Hash hash, const RenderPassInfo &info);
} // namespace Vulkan

using namespace Vulkan;

namespace pcr {

namespace {

// A compatible render pass's attachments (the canvas's: one colour attachment or none, a depth one or none) and
// subpasses: made again with other samples from them
struct PassDesc {
    uint32_t colours = 0;
    bool depth = false;
    uint32_t subpasses = 0;
    std::array<RenderPassInfo::Subpass, 2> subpass{};
};

struct Kept {
    DeferredPipelineCompile compile;
    uint64_t pass = 0;
};

struct Warm {
    std::mutex lock;
    Device *device = nullptr;
    std::unordered_map<Program *, WarmProgram> programs; // (by the program and by its per-sample variant)
    Program *extra = nullptr;
    std::unordered_map<uint64_t, PassDesc> passes;
    std::unordered_map<uint64_t, Kept> kept;
    uint64_t serial = 0;                  // (pipelines kept so far)
    std::array<uint64_t, 9> warmed{};     // (by samples: serial + 1 at the last round made, 0 none)
    std::atomic<bool> running{false};
    std::atomic<uint64_t> made{0}, rounds{0};
    std::atomic<int64_t> ns{0};
    const bool log = getenv("KF4_PC_WARM_LOG") != nullptr; // (each pipeline made, on stderr)
} warm;

thread_local bool on_warm_thread = false;

void on_render_pass(Util::Hash hash, const RenderPassInfo &info) {
    if (info.num_color_attachments > 1 || info.num_subpasses > 2) {
        return;
    }
    PassDesc d;
    d.colours = info.num_color_attachments;
    d.depth = info.depth_stencil != nullptr;
    d.subpasses = info.num_subpasses;
    for (uint32_t i = 0; i < info.num_subpasses; i++) {
        d.subpass[i] = info.subpasses[i];
    }
    std::lock_guard<std::mutex> hold(warm.lock);
    warm.passes.emplace(uint64_t(hash), d);
}

void on_compile(Device *, const DeferredPipelineCompile &compile, bool compute, int64_t ns, bool sync, bool ok) {
    if (compute || !ok) {
        return;
    }
    std::lock_guard<std::mutex> hold(warm.lock);
    if (warm.log) {
        auto pr = warm.programs.find(compile.program);
        auto pa = warm.passes.find(uint64_t(compile.compatible_render_pass->get_hash()));
        fprintf(stderr, "[warm] %s %s program %p (base %p), colours %u depth %u subpasses %u, samples %u: %.1f ms\n",
                on_warm_thread ? "ahead" : "draw", sync ? "sync" : "async", (void *)compile.program,
                pr != warm.programs.end() ? (void *)pr->second.program : nullptr,
                pa != warm.passes.end() ? pa->second.colours : 9, pa != warm.passes.end() ? pa->second.depth : 9,
                pa != warm.passes.end() ? pa->second.subpasses : 9,
                compile.compatible_render_pass->get_sample_count(compile.subpass_index), double(ns) / 1e6);
    }
    if (on_warm_thread) {
        return;
    }
    if (!warm.programs.count(compile.program) || warm.kept.count(uint64_t(compile.hash))) {
        return;
    }
    warm.kept[uint64_t(compile.hash)] = {compile, uint64_t(compile.compatible_render_pass->get_hash())};
    warm.serial++;
}

// One pipeline made from compile with program and the pass, unless made already
void make(Device &device, DeferredPipelineCompile compile, Program *program, const RenderPass &pass) {
    compile.program = program;
    compile.layout = program->get_pipeline_layout();
    compile.compatible_render_pass = &pass;
    compile.cache = CommandBuffer::holyland_pipeline_cache(&device);
    CommandBuffer::holyland_rehash(compile, false);
    if (program->get_pipeline(compile.hash).pipeline != VK_NULL_HANDLE) {
        return;
    }
    if (CommandBuffer::build_graphics_pipeline(&device, compile, CommandBuffer::CompileMode::Sync).pipeline) {
        warm.made++;
    }
}

// (the warm thread) Every pipeline kept made at samples, its program's variants with it; colour and depth: images of
// the canvas's formats at samples, the passes' attachments
void round(uint32_t samples, std::vector<Kept> kept, std::unordered_map<uint64_t, PassDesc> passes,
           std::unordered_map<Program *, WarmProgram> programs, Program *extra, const ImageView *colour,
           const ImageView *depth, uint64_t serial) {
    on_warm_thread = true;
    auto start = std::chrono::steady_clock::now();
    Device &device = *warm.device;
    bool extra_made = false;
    for (const Kept &k : kept) {
        auto p = passes.find(k.pass);
        auto prog = programs.find(k.compile.program);
        if (p == passes.end() || prog == programs.end() || (p->second.depth && !depth)) {
            continue;
        }
        const PassDesc &d = p->second;
        RenderPassInfo info;
        info.num_color_attachments = d.colours;
        if (d.colours) {
            info.color_attachments[0] = colour;
        }
        info.depth_stencil = d.depth ? depth : nullptr;
        info.num_subpasses = d.subpasses;
        info.subpasses = d.subpasses ? d.subpass.data() : nullptr;
        const RenderPass &pass = CommandBuffer::holyland_render_pass(&device, info);
        make(device, k.compile, prog->second.program, pass);
        if (samples > 1 && prog->second.per_sample) {
            make(device, k.compile, prog->second.per_sample, pass);
        }
        if (samples > 1 && extra && !extra_made && d.colours == 1 && !d.depth && d.subpasses == 0) {
            make(device, k.compile, extra, pass);
            extra_made = true;
        }
    }
    warm.ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    warm.rounds++;
    std::lock_guard<std::mutex> hold(warm.lock);
    warm.warmed[samples] = serial + 1;
    warm.running = false;
}

} // namespace

void warm_watch(Device &device, const WarmProgram *programs, uint32_t count, Program *extra) {
    std::lock_guard<std::mutex> hold(warm.lock);
    warm.device = &device;
    for (uint32_t i = 0; i < count; i++) {
        warm.programs[programs[i].program] = programs[i];
        if (programs[i].per_sample) {
            warm.programs[programs[i].per_sample] = programs[i];
        }
    }
    if (extra) {
        warm.extra = extra;
    }
    Vulkan::holyland_on_compile = on_compile;
    Vulkan::holyland_on_render_pass = on_render_pass;
}

bool warm_ready(uint32_t samples, const ImageView &colour, const ImageView *depth) {
    if (samples >= warm.warmed.size()) {
        return true;
    }
    std::unique_lock<std::mutex> hold(warm.lock);
    if (warm.warmed[samples] == warm.serial + 1) {
        return true;
    }
    if (warm.running) {
        return false;
    }
    std::vector<Kept> kept;
    kept.reserve(warm.kept.size());
    for (const auto &k : warm.kept) {
        kept.push_back(k.second);
    }
    warm.running = true;
    // (let go: a thread object left to the end of the program would stop it there; warm_stop waits for it instead)
    std::thread(round, samples, std::move(kept), warm.passes, warm.programs, warm.extra, &colour, depth, warm.serial)
        .detach();
    return false;
}

void warm_stop() {
    while (warm.running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void warm_print_counts() {
    if (warm.rounds.load() == 0) {
        return;
    }
    fprintf(stderr, "[stats] pc pipelines made ahead (MSAA's samples changed): %llu in %llu rounds, %.1f ms; %zu kept\n",
            (unsigned long long)warm.made.load(), (unsigned long long)warm.rounds.load(), double(warm.ns.load()) / 1e6,
            warm.kept.size());
}

} // namespace pcr
