// The PC renderer's pipelines made ahead when MSAA's samples change (pcr_pipelines.h)
#include "pcr_pipelines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "command_buffer.hpp"
#include "thread_id.hpp"

// (pcr_pipeline_seed.cpp, made by cmake/pipeline_seed.cmake from port/gs/pc/pipeline_seed.bin)
extern const unsigned char pcr_pipeline_seed[];
extern const size_t pcr_pipeline_seed_size;

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
    VkFormat colour_format = VK_FORMAT_UNDEFINED, depth_format = VK_FORMAT_UNDEFINED; // (the attachments')
};

// A pipeline drawn with in an earlier run, as kept in the data folder's list (pc_pipeline_list.bin): what Granite
// made it from, with its program named by the canvas's index of it (warm_watch's: the same across builds, where the
// program's code may not be) and its render pass by its attachments; plain bytes, zeroed where unused
struct Record {
    uint32_t role;       // (warm_watch's index; EXTRA_ROLE: the expand pass)
    uint32_t per_sample; // (1: the program's variant that draws only with MSAA)
    uint32_t samples;
    uint32_t colours, depth, subpasses;
    VkFormat colour_format, depth_format;
    RenderPassInfo::Subpass subpass[2];
    PipelineState static_state;
    PotentialState potential_static_state;
    VertexAttribState attribs[VULKAN_NUM_VERTEX_ATTRIBS];
    VkDeviceSize strides[VULKAN_NUM_VERTEX_BUFFERS];
    VkVertexInputRate input_rates[VULKAN_NUM_VERTEX_BUFFERS];
    uint32_t subpass_index;
    uint32_t subgroup_size_tag;
};
constexpr uint32_t EXTRA_ROLE = 1000;
constexpr size_t RECORDS_MOST = 4096;           // (the list's length at most: the oldest dropped past it)
constexpr size_t CACHE_BYTES_MOST = 256u << 20; // (a cache file larger than this is not read: made afresh)

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
    // the programs by role (warm_watch's index), and each program's role (true: its MSAA-only variant)
    std::unordered_map<uint32_t, WarmProgram> roles;
    std::unordered_map<Program *, std::pair<uint32_t, bool>> role_of;
    WarmDevice traits; // (warm_watch's)
} warm;

// The pipelines kept between runs (pcr_pipelines.h, "Between runs"): the Vulkan pipeline cache's file and the list of
// pipelines drawn with, the list's pipelines made ahead on threads of their own once the canvas's programs are made
struct Store {
    std::mutex lock;
    Device *device = nullptr;
    bool open = false;
    std::string cache_path, list_path;
    std::vector<Record> records;              // (the list: loaded, then each new pipeline drawn with)
    std::unordered_set<uint64_t> record_keys; // (each record's bytes hashed)
    bool records_dirty = false;
    std::atomic<uint64_t> compiles{0};        // (pipelines made, any thread: the cache has new ones)
    uint64_t compiles_saved = 0;
    // made ahead: once, after the programs are known
    bool ahead_on = true, ahead_started = false;
    std::atomic<bool> ahead_stop{false}; // (at the end: the threads making ahead stop after the pipeline each is making)
    std::vector<std::thread> ahead;
    std::atomic<uint64_t> ahead_made{0}, ahead_skipped{0}, ahead_done{0};
    std::atomic<int64_t> ahead_ns{0}, ahead_wall_ns{0};
    std::atomic<size_t> ahead_total{0};
    std::atomic<unsigned> ahead_threads{0};
    std::chrono::steady_clock::time_point ahead_start;
    std::mutex hashes_lock;
    std::unordered_set<uint64_t> hashes; // (the pipelines asked of the threads making ahead: each made once)
    // made before play (precompile): asked for with the settings, started once the canvas's programs are known too
    bool pre_on = true, pre_asked = false;
    std::atomic<bool> pre_wanted{false}, pre_gave_up{false};
    uint32_t pre_samples = 1;
    bool pre_high = false;
    std::chrono::steady_clock::time_point pre_asked_at;
    size_t pre_seed = 0, pre_list = 0; // (the pipelines asked for: from the seed for these settings, from the list)
    // attachments of the formats and samples the list's passes have (made by the threads making ahead)
    std::mutex images_lock;
    std::map<std::tuple<VkFormat, VkFormat, uint32_t>, std::pair<ImageHandle, ImageHandle>> images;
    // the saver: every SAVE_EVERY, the files written again when something is new
    std::thread saver;
    std::condition_variable wake;
    bool stopping = false;
    uint64_t loaded_cache_bytes = 0, saves = 0;
    size_t loaded_records = 0;
    const char *cache_state = "off";
} store;

constexpr auto SAVE_EVERY = std::chrono::seconds(10);

uint64_t record_key(const Record &r) {
    Util::Hasher h;
    h.data(reinterpret_cast<const uint32_t *>(&r), sizeof(r));
    return h.get();
}

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
    if (info.num_color_attachments && info.color_attachments[0]) {
        d.colour_format = info.color_attachments[0]->get_format();
    }
    if (info.depth_stencil) {
        d.depth_format = info.depth_stencil->get_format();
    }
    std::lock_guard<std::mutex> hold(warm.lock);
    warm.passes.emplace(uint64_t(hash), d);
}

void remember(const DeferredPipelineCompile &compile, const PassDesc &pass);

void on_compile(Device *, const DeferredPipelineCompile &compile, bool compute, int64_t ns, bool sync, bool ok) {
    if (ok) {
        store.compiles++;
    }
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
    auto pass = warm.passes.find(uint64_t(compile.compatible_render_pass->get_hash()));
    if (pass != warm.passes.end()) {
        remember(compile, pass->second);
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
    auto start = std::chrono::steady_clock::now();
    Device &device = *warm.device;
    std::atomic<bool> extra_made{false};
    std::atomic<size_t> next{0};
    // (on half the cores, the game playing meanwhile: with the pipelines made before play kept too, a round has
    // hundreds to make)
    auto work = [&] {
        on_warm_thread = true;
        Util::register_thread_index(0); // (as start_ahead's threads)
        for (size_t i = next++; i < kept.size(); i = next++) {
            const Kept &k = kept[i];
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
            if (samples > 1 && extra && d.colours == 1 && !d.depth && d.subpasses == 0 && !extra_made.exchange(true)) {
                make(device, k.compile, extra, pass);
            }
        }
    };
    unsigned cores = std::thread::hardware_concurrency();
    unsigned threads = std::max(1u, std::min(unsigned(kept.size() / 4 + 1), cores / 2));
    std::vector<std::thread> helpers;
    for (unsigned t = 1; t < threads; t++) {
        helpers.emplace_back(work);
    }
    work();
    for (std::thread &t : helpers) {
        t.join();
    }
    warm.ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    warm.rounds++;
    std::lock_guard<std::mutex> hold(warm.lock);
    warm.warmed[samples] = serial + 1;
    warm.running = false;
}

// ---- Between runs ----

// (warm.lock held) A pipeline drawn with, added to the list unless it is there
void remember(const DeferredPipelineCompile &compile, const PassDesc &pass) {
    auto role = warm.role_of.find(compile.program);
    if (role == warm.role_of.end()) {
        return;
    }
    Record r;
    memset(&r, 0, sizeof(r));
    r.role = role->second.first;
    r.per_sample = role->second.second ? 1 : 0;
    r.samples = compile.compatible_render_pass->get_sample_count(compile.subpass_index);
    r.colours = pass.colours;
    r.depth = pass.depth ? 1 : 0;
    r.subpasses = pass.subpasses;
    r.colour_format = pass.colour_format;
    r.depth_format = pass.depth_format;
    for (uint32_t i = 0; i < pass.subpasses && i < 2; i++) {
        memcpy(&r.subpass[i], &pass.subpass[i], sizeof(r.subpass[i]));
    }
    memcpy(&r.static_state, &compile.static_state, sizeof(r.static_state));
    memcpy(&r.potential_static_state, &compile.potential_static_state, sizeof(r.potential_static_state));
    memcpy(r.attribs, compile.attribs, sizeof(r.attribs));
    memcpy(r.strides, compile.strides, sizeof(r.strides));
    memcpy(r.input_rates, compile.input_rates, sizeof(r.input_rates));
    r.subpass_index = compile.subpass_index;
    r.subgroup_size_tag = compile.subgroup_size_tag;
    std::lock_guard<std::mutex> hold(store.lock);
    if (!store.open || !store.record_keys.insert(record_key(r)).second) {
        return;
    }
    store.records.push_back(r);
    store.records_dirty = true;
}

// The list's file: a header (the layout of what the records hold, which a Granite or compiler change may alter: then
// the file is not read) and the records
struct ListHeader {
    char magic[8] = {'K', 'F', '4', 'P', 'C', 'P', 'L', '1'};
    uint32_t record_size = sizeof(Record);
    uint32_t state_size = sizeof(PipelineState);
    uint32_t potential_size = sizeof(PotentialState);
    uint32_t subpass_size = sizeof(RenderPassInfo::Subpass);
    uint32_t attribs = VULKAN_NUM_VERTEX_ATTRIBS;
    uint32_t buffers = VULKAN_NUM_VERTEX_BUFFERS;
    uint32_t count = 0;
    uint32_t reserved = 0;
};

// The cache's file: a header naming the GPU and driver it was made by (a cache is only good for those; Granite checks
// the cache's UUID and its bytes' hash besides), then what Granite gives (get_pipeline_cache_data)
struct CacheHeader {
    char magic[8] = {'K', 'F', '4', 'P', 'C', 'P', 'C', '1'};
    uint32_t vendor = 0, device = 0, driver = 0, api = 0;
    uint64_t bytes = 0;
};

std::vector<uint8_t> read_file(const std::string &path, size_t most) {
    std::vector<uint8_t> bytes;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return bytes;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && size_t(size) <= most && fseek(f, 0, SEEK_SET) == 0) {
            bytes.resize(size_t(size));
            if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
                bytes.clear();
            }
        }
    }
    fclose(f);
    return bytes;
}

// Written whole to a file beside, then put in its place (a run stopped while writing leaves the old file)
bool write_file(const std::string &path, const void *head, size_t head_size, const void *body, size_t body_size) {
    std::string temp = path + ".new";
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(head, 1, head_size, f) == head_size && (body_size == 0 || fwrite(body, 1, body_size, f) == body_size);
    ok = fclose(f) == 0 && ok;
    std::error_code error;
    if (ok) {
        std::filesystem::rename(temp, path, error);
        ok = !error;
    }
    if (!ok) {
        std::filesystem::remove(temp, error);
    }
    return ok;
}

// (store.lock held)
void load_list() {
    std::vector<uint8_t> bytes = read_file(store.list_path, sizeof(ListHeader) + RECORDS_MOST * sizeof(Record));
    ListHeader want, got;
    if (bytes.size() < sizeof(got)) {
        return;
    }
    memcpy(&got, bytes.data(), sizeof(got));
    if (memcmp(got.magic, want.magic, sizeof(want.magic)) != 0 || got.record_size != want.record_size ||
        got.state_size != want.state_size || got.potential_size != want.potential_size ||
        got.subpass_size != want.subpass_size || got.attribs != want.attribs || got.buffers != want.buffers ||
        bytes.size() != sizeof(got) + size_t(got.count) * sizeof(Record)) {
        fprintf(stderr, "[pcr] pipeline list %s is not of this build's layout: started afresh\n", store.list_path.c_str());
        return;
    }
    for (uint32_t i = 0; i < got.count; i++) {
        Record r;
        memcpy(&r, bytes.data() + sizeof(got) + size_t(i) * sizeof(Record), sizeof(r));
        if (r.subpasses > 2 || r.colours > 1 || r.depth > 1 || r.samples == 0 || r.samples > 8 ||
            (r.samples & (r.samples - 1)) != 0) {
            continue;
        }
        if (store.record_keys.insert(record_key(r)).second) {
            store.records.push_back(r);
        }
    }
    store.loaded_records = store.records.size();
}

// (store.lock held) Granite's pipeline cache made, from the file where it is this GPU's and driver's
void load_cache() {
    Device &device = *store.device;
    const VkPhysicalDeviceProperties &props = device.get_gpu_properties();
    std::vector<uint8_t> bytes = read_file(store.cache_path, sizeof(CacheHeader) + CACHE_BYTES_MOST);
    CacheHeader want, got;
    want.vendor = props.vendorID;
    want.device = props.deviceID;
    want.driver = props.driverVersion;
    want.api = props.apiVersion;
    const uint8_t *payload = nullptr;
    size_t payload_size = 0;
    if (bytes.size() > sizeof(got)) {
        memcpy(&got, bytes.data(), sizeof(got));
        if (memcmp(got.magic, want.magic, sizeof(want.magic)) == 0 && got.vendor == want.vendor &&
            got.device == want.device && got.driver == want.driver && got.api == want.api &&
            got.bytes == bytes.size() - sizeof(got)) {
            payload = bytes.data() + sizeof(got);
            payload_size = size_t(got.bytes);
        } else {
            fprintf(stderr, "[pcr] pipeline cache %s was made by another GPU or driver: started afresh\n",
                    store.cache_path.c_str());
        }
    }
    // (Granite copies what it is given; its own check, the cache's UUID and hash, may refuse it and start empty)
    if (!device.init_pipeline_cache(payload, payload_size)) {
        store.cache_state = "none (no Vulkan pipeline cache made)";
        return;
    }
    store.loaded_cache_bytes = payload_size;
    store.cache_state = payload ? "read" : "new";
}

// (store.lock held) The two files written again where anything is new since they were
void save_locked() {
    if (!store.open) {
        return;
    }
    Device &device = *store.device;
    uint64_t compiles = store.compiles.load();
    if (compiles != store.compiles_saved) {
        size_t size = device.get_pipeline_cache_size();
        if (size && size <= CACHE_BYTES_MOST) {
            std::vector<uint8_t> data(size);
            if (device.get_pipeline_cache_data(data.data(), size)) {
                const VkPhysicalDeviceProperties &props = device.get_gpu_properties();
                CacheHeader head;
                head.vendor = props.vendorID;
                head.device = props.deviceID;
                head.driver = props.driverVersion;
                head.api = props.apiVersion;
                head.bytes = size;
                if (write_file(store.cache_path, &head, sizeof(head), data.data(), size)) {
                    store.saves++;
                }
            }
        }
        store.compiles_saved = compiles; // (not tried again until something new: a failing write fails again)
    }
    if (store.records_dirty) {
        if (store.records.size() > RECORDS_MOST) {
            store.records.erase(store.records.begin(), store.records.end() - RECORDS_MOST);
        }
        ListHeader head;
        head.count = uint32_t(store.records.size());
        write_file(store.list_path, &head, sizeof(head), store.records.data(), store.records.size() * sizeof(Record));
        store.records_dirty = false;
    }
}

void saver() {
    std::unique_lock<std::mutex> hold(store.lock);
    while (!store.stopping) {
        store.wake.wait_for(hold, SAVE_EVERY);
        if (!store.stopping) {
            save_locked();
        }
    }
}

bool format_ok(Device &device, VkFormat format, VkFormatFeatureFlags2 need);

// Attachments of a record's formats and samples, made as the canvas makes its own for the pipelines made ahead
// (make_warm_images): the render passes made from them are the canvas's (a compatible pass is named by its
// attachments' formats, samples and layout, ImageLayout::General). Made by any thread: never drawn into, so made
// with no first layout (VK_IMAGE_LAYOUT_UNDEFINED), whose change Granite would record in a command buffer of the
// thread's own
std::pair<ImageHandle, ImageHandle> &images_for(Device &device, const Record &r) {
    std::lock_guard<std::mutex> hold(store.images_lock);
    auto &slot = store.images[{r.colour_format, r.depth_format, r.samples}];
    if (!slot.first && r.colours && format_ok(device, r.colour_format, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) {
        ImageCreateInfo info = ImageCreateInfo::render_target(16, 16, r.colour_format);
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.samples = VkSampleCountFlagBits(r.samples);
        slot.first = device.create_image(info, nullptr);
    }
    if (!slot.second && r.depth && format_ok(device, r.depth_format, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        ImageCreateInfo info = ImageCreateInfo::render_target(16, 16, r.depth_format);
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        info.samples = VkSampleCountFlagBits(r.samples);
        slot.second = device.create_image(info, nullptr);
    }
    return slot;
}

bool format_ok(Device &device, VkFormat format, VkFormatFeatureFlags2 need) {
    return format != VK_FORMAT_UNDEFINED && device.image_format_is_supported(format, need);
}

// (a thread making ahead) One record's pipeline made, through the cache
void make_ahead(Device &device, const Record &r, const std::unordered_map<uint32_t, WarmProgram> &roles) {
    auto role = roles.find(r.role);
    Program *program = nullptr;
    if (role != roles.end()) {
        program = r.per_sample ? role->second.per_sample : role->second.program;
    }
    if (!program || (r.colours && !format_ok(device, r.colour_format, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) ||
        (r.depth && !format_ok(device, r.depth_format, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        store.ahead_skipped++;
        return;
    }
    const VkPhysicalDeviceLimits &limits = device.get_gpu_properties().limits;
    if (!(r.samples & limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts)) {
        store.ahead_skipped++;
        return;
    }
    auto &images = images_for(device, r);
    if ((r.colours && !images.first) || (r.depth && !images.second)) {
        store.ahead_skipped++;
        return;
    }
    RenderPassInfo info;
    info.num_color_attachments = r.colours;
    if (r.colours) {
        info.color_attachments[0] = &images.first->get_view();
    }
    info.depth_stencil = r.depth ? &images.second->get_view() : nullptr;
    info.num_subpasses = r.subpasses;
    info.subpasses = r.subpasses ? r.subpass : nullptr;
    const RenderPass &pass = CommandBuffer::holyland_render_pass(&device, info);
    DeferredPipelineCompile compile;
    memset(&compile, 0, sizeof(compile));
    memcpy(&compile.static_state, &r.static_state, sizeof(r.static_state));
    memcpy(&compile.potential_static_state, &r.potential_static_state, sizeof(r.potential_static_state));
    memcpy(compile.attribs, r.attribs, sizeof(r.attribs));
    memcpy(compile.strides, r.strides, sizeof(r.strides));
    memcpy(compile.input_rates, r.input_rates, sizeof(r.input_rates));
    compile.subpass_index = r.subpass_index;
    compile.subgroup_size_tag = r.subgroup_size_tag;
    {
        // (records that differ only where their pipelines do not, e.g. in state made dynamic: made once)
        DeferredPipelineCompile named = compile;
        named.program = program;
        named.layout = program->get_pipeline_layout();
        named.compatible_render_pass = &pass;
        CommandBuffer::holyland_rehash(named, false);
        std::lock_guard<std::mutex> hold(store.hashes_lock);
        if (!store.hashes.insert(uint64_t(named.hash)).second) {
            return;
        }
    }
    uint64_t before = warm.made.load();
    auto start = std::chrono::steady_clock::now();
    make(device, compile, program, pass);
    store.ahead_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    if (warm.made.load() != before) {
        warm.made--; // (counted as made ahead between runs, not for a change of samples)
        store.ahead_made++;
    }
    // kept as a pipeline drawn with is (on_compile): made again at a new count if MSAA's samples change
    compile.program = program;
    compile.layout = program->get_pipeline_layout();
    compile.compatible_render_pass = &pass;
    compile.cache = CommandBuffer::holyland_pipeline_cache(&device);
    CommandBuffer::holyland_rehash(compile, false);
    std::lock_guard<std::mutex> hold(warm.lock);
    uint64_t pass_hash = uint64_t(pass.get_hash());
    if (warm.programs.count(program) && warm.passes.count(pass_hash) && !warm.kept.count(uint64_t(compile.hash))) {
        warm.kept[uint64_t(compile.hash)] = {compile, pass_hash};
        warm.serial++;
    }
}

// (store.lock and warm.lock held) Records' pipelines made on threads of their own, in their order: the list's while
// the game runs (a draw that wants one not made yet makes it itself, as without the list), or before play
// (start_precompile: the seed's and the list's, on every core but one, while the game waits)
void start_ahead(std::vector<Record> list, unsigned threads) {
    if (store.ahead_started || !store.open || list.empty() || !warm.device) {
        return;
    }
    store.ahead_started = true;
    auto records = std::make_shared<std::vector<Record>>(std::move(list));
    auto roles = std::make_shared<std::unordered_map<uint32_t, WarmProgram>>(warm.roles);
    auto next = std::make_shared<std::atomic<size_t>>(0);
    store.ahead_total = records->size();
    store.ahead_start = std::chrono::steady_clock::now();
    Device *device = warm.device;
    for (unsigned t = 0; t < threads; t++) {
        store.ahead.emplace_back([=] {
            on_warm_thread = true;
            // (Granite's compile log names a thread by its index: these threads request no command buffer, the only
            // other use of it, so the canvas thread's index stands in, as an unregistered thread's would)
            Util::register_thread_index(0);
            for (size_t i = (*next)++; i < records->size() && !store.ahead_stop; i = (*next)++) {
                make_ahead(*device, (*records)[i], *roles);
                if (++store.ahead_done == records->size()) {
                    store.ahead_wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - store.ahead_start)
                                              .count();
                    if (warm.log || getenv("KF4_PC_PIPELINE_LOG") || store.pre_wanted) {
                        fprintf(stderr,
                                "[pcr] pipelines made ahead: %llu made, %llu skipped, of %zu asked for, in %.1f ms on "
                                "%u threads\n",
                                (unsigned long long)store.ahead_made.load(),
                                (unsigned long long)store.ahead_skipped.load(), records->size(),
                                double(store.ahead_wall_ns) / 1e6, threads);
                    }
                }
            }
        });
    }
    store.ahead_threads = threads;
}

// ---- Made before play: the seed ----

// The seed (pcr_pipeline_seed.cpp, made by cmake/pipeline_seed.cmake from port/gs/pc/pipeline_seed.bin, which
// tools/pipeline_seed.py makes from tours of the game): the canvas's pipelines drawn with, each as the list keeps it,
// with the MSAA samples of the run it was drawn in. Renderer state only (programs by the canvas's index, Granite's
// state, attachment formats), none of the game's data. Drawn with KF4_PC_DYNAMIC=0 (no dynamic state), so that every
// state a draw had is there; with the device's dynamic state put in, those that differ only there are one pipeline.
struct SeedEntry {
    uint32_t run_samples; // (the run's MSAA: 1, or the count the canvas's passes drew at)
    uint32_t reserved;
    Record record;
};
struct SeedHeader {
    char magic[8] = {'K', 'F', '4', 'P', 'C', 'P', 'S', '1'};
    uint32_t entry_size = sizeof(SeedEntry);
    uint32_t state_size = sizeof(PipelineState);
    uint32_t potential_size = sizeof(PotentialState);
    uint32_t subpass_size = sizeof(RenderPassInfo::Subpass);
    uint32_t attribs = VULKAN_NUM_VERTEX_ATTRIBS;
    uint32_t buffers = VULKAN_NUM_VERTEX_BUFFERS;
    uint32_t count = 0;
    uint32_t reserved = 0;
};

bool is_colour_format(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R16G16B16A16_UNORM;
}

bool is_depth_format(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT;
}

// (warm.lock held) The seed's pipelines for these settings and this device: those of the runs at 1x, and with MSAA
// those of the runs with MSAA (at the count asked); the canvas's formats and dynamic state put in. Empty when the seed is not of this
// build's layout (Granite's state changed: tools/pipeline_seed.py makes it again).
std::vector<Record> seed_for(uint32_t samples, bool high) {
    std::vector<Record> out;
    if (pcr_pipeline_seed_size < sizeof(SeedHeader)) {
        return out;
    }
    SeedHeader want, got;
    memcpy(&got, pcr_pipeline_seed, sizeof(got));
    if (memcmp(got.magic, want.magic, sizeof(want.magic)) != 0 || got.entry_size != want.entry_size ||
        got.state_size != want.state_size || got.potential_size != want.potential_size ||
        got.subpass_size != want.subpass_size || got.attribs != want.attribs || got.buffers != want.buffers ||
        pcr_pipeline_seed_size != sizeof(got) + size_t(got.count) * sizeof(SeedEntry)) {
        fprintf(stderr, "[pcr] the pipeline seed is not of this build's layout (tools/pipeline_seed.py): not used\n");
        return out;
    }
    const WarmDevice &t = warm.traits;
    VkFormat colour = high && t.high_colour ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    std::unordered_set<uint64_t> keys;
    for (uint32_t i = 0; i < got.count; i++) {
        SeedEntry e;
        memcpy(&e, pcr_pipeline_seed + sizeof(got) + size_t(i) * sizeof(SeedEntry), sizeof(e));
        Record &r = e.record;
        // (the runs at 1x: their passes drawn at 1x, which a run with MSAA draws some of too (Z-only and 2D ones); the
        // runs with MSAA only with MSAA)
        if ((e.run_samples > 1 && samples <= 1) || (r.per_sample && samples <= 1)) {
            continue;
        }
        if (samples > 1 && e.run_samples > 1 && r.samples == e.run_samples) {
            r.samples = samples;
        }
        if (r.colours && is_colour_format(r.colour_format)) {
            r.colour_format = colour;
        }
        if (r.depth && is_depth_format(r.depth_format) && t.depth_format != VK_FORMAT_UNDEFINED) {
            r.depth_format = t.depth_format;
        }
        r.static_state.state.dynamic_ds = t.dynamic_ds ? 1 : 0;
        r.static_state.state.dynamic_blend = t.dynamic_ds && t.dynamic_blend ? 1 : 0;
        if (keys.insert(record_key(r)).second) {
            out.push_back(r);
        }
    }
    return out;
}

// (store.lock and warm.lock held) Made before play, once both the settings (precompile) and the programs
// (warm_watch) are known: the seed's for these settings, then the list's
void start_precompile() {
    if (!store.pre_asked || store.ahead_started || !warm.device) {
        return;
    }
    std::vector<Record> list = seed_for(store.pre_samples, store.pre_high);
    store.pre_seed = list.size();
    store.pre_list = store.records.size();
    list.insert(list.end(), store.records.begin(), store.records.end());
    unsigned cores = std::thread::hardware_concurrency();
    {
        fprintf(stderr, "[pcr] pipelines made before play: %zu from the seed (samples %u, colour %s), %zu from the list\n",
                store.pre_seed, store.pre_samples, store.pre_high ? "high" : "exact", store.pre_list);
    }
    start_ahead(std::move(list), cores > 2 ? cores - 1 : 1);
    if (!store.ahead_started) {
        store.pre_wanted = false; // (nothing to make)
    }
}

} // namespace

void warm_watch(Device &device, const WarmProgram *programs, uint32_t count, Program *extra, const WarmDevice &traits) {
    std::lock_guard<std::mutex> hold_store(store.lock);
    std::lock_guard<std::mutex> hold(warm.lock);
    warm.device = &device;
    warm.traits = traits;
    for (uint32_t i = 0; i < count; i++) {
        warm.programs[programs[i].program] = programs[i];
        warm.roles[i] = programs[i];
        warm.role_of[programs[i].program] = {i, false};
        if (programs[i].per_sample) {
            warm.programs[programs[i].per_sample] = programs[i];
            warm.role_of[programs[i].per_sample] = {i, true};
        }
    }
    if (extra) {
        warm.extra = extra;
        warm.roles[EXTRA_ROLE] = {extra, nullptr};
        warm.role_of[extra] = {EXTRA_ROLE, false};
    }
    Vulkan::holyland_on_compile = on_compile;
    Vulkan::holyland_on_render_pass = on_render_pass;
    if (store.pre_on) {
        start_precompile(); // (when the settings came first)
    } else if (store.ahead_on) {
        start_ahead(store.records, std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 4)));
    }
}

void precompile(uint32_t samples, bool colour_high) {
    std::lock_guard<std::mutex> hold_store(store.lock);
    std::lock_guard<std::mutex> hold(warm.lock);
    if (!store.open || !store.pre_on || store.pre_asked) {
        return;
    }
    store.pre_asked = true;
    store.pre_wanted = true;
    store.pre_samples = samples;
    store.pre_high = colour_high;
    store.pre_asked_at = std::chrono::steady_clock::now();
    start_precompile(); // (when the programs came first)
}

bool precompile_running() {
    if (!store.pre_wanted || store.pre_gave_up) {
        return false;
    }
    if (store.ahead_threads == 0) {
        // (asked for, the canvas's programs not made yet: they are as the renderer starts; not waited for past 20 s)
        std::lock_guard<std::mutex> hold(store.lock);
        if (store.ahead_threads == 0 && store.pre_wanted &&
            std::chrono::steady_clock::now() - store.pre_asked_at > std::chrono::seconds(20)) {
            store.pre_gave_up = true;
            fprintf(stderr, "[pcr] the pipelines before play did not start: not waited for\n");
            return false;
        }
        return store.pre_wanted;
    }
    return store.ahead_done.load() < store.ahead_total.load() && !store.ahead_stop;
}

bool precompile_progress(int *done, int *total) {
    if (!precompile_running()) {
        *done = *total = 0;
        return false;
    }
    *done = int(store.ahead_done.load());
    *total = int(store.ahead_total.load());
    return true;
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

void store_open(Device &device, const char *cache_path) {
    const char *off = getenv("KF4_PC_PIPELINE_CACHE");
    if ((off && strcmp(off, "0") == 0) || !cache_path || !*cache_path) {
        return;
    }
    const char *ahead = getenv("KF4_PC_WARM_AHEAD");
    std::lock_guard<std::mutex> hold(store.lock);
    if (store.open) {
        return;
    }
    store.device = &device;
    store.cache_path = cache_path;
    store.list_path = (std::filesystem::path(cache_path).parent_path() / "pc_pipeline_list.bin").string();
    std::error_code made;
    std::filesystem::create_directories(std::filesystem::path(cache_path).parent_path(), made);
    store.ahead_on = !(ahead && strcmp(ahead, "0") == 0);
    const char *pre = getenv("KF4_PC_PRECOMPILE");
    store.pre_on = store.ahead_on && !(pre && strcmp(pre, "0") == 0);
    load_cache();
    load_list();
    store.open = true;
    // (the hooks from here: the list is kept from the first pipeline drawn with; warm_watch sets them too)
    Vulkan::holyland_on_compile = on_compile;
    Vulkan::holyland_on_render_pass = on_render_pass;
    store.saver = std::thread(saver);
    if (warm.log || getenv("KF4_PC_PIPELINE_LOG")) {
        fprintf(stderr, "[pcr] pipeline cache %s: %s (%llu bytes), %zu pipelines listed to make ahead\n",
                store.cache_path.c_str(), store.cache_state, (unsigned long long)store.loaded_cache_bytes,
                store.loaded_records);
    }
}

void store_close() {
    {
        std::lock_guard<std::mutex> hold(store.lock);
        if (!store.open || store.stopping) {
            return;
        }
        store.stopping = true;
    }
    store.ahead_stop = true;
    store.wake.notify_all();
    if (store.saver.joinable()) {
        store.saver.join();
    }
    // (the threads making ahead finish the pipeline each is making: its making cannot be stopped)
    for (std::thread &t : store.ahead) {
        if (t.joinable()) {
            t.join();
        }
    }
    std::lock_guard<std::mutex> hold(store.lock);
    save_locked();
    store.open = false;
}

void warm_print_counts() {
    if (!store.cache_path.empty()) {
        fprintf(stderr,
                "[stats] pc pipelines between runs: cache %s (%llu bytes read), %zu listed (%zu from before); made ahead "
                "%llu of %zu (%llu skipped), %.1f ms making, %.1f ms from the first to the last; %llu cache saves\n",
                store.cache_state, (unsigned long long)store.loaded_cache_bytes, store.records.size(),
                store.loaded_records, (unsigned long long)store.ahead_made.load(), store.ahead_total.load(),
                (unsigned long long)store.ahead_skipped.load(), double(store.ahead_ns.load()) / 1e6,
                double(store.ahead_wall_ns.load()) / 1e6, (unsigned long long)store.saves);
    }
    if (warm.rounds.load() == 0) {
        return;
    }
    fprintf(stderr, "[stats] pc pipelines made ahead (MSAA's samples changed): %llu in %llu rounds, %.1f ms; %zu kept\n",
            (unsigned long long)warm.made.load(), (unsigned long long)warm.rounds.load(), double(warm.ns.load()) / 1e6,
            warm.kept.size());
}

} // namespace pcr
