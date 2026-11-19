// The PC renderer's pipelines kept between runs and made ahead (pcr_pipelines.h)
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

extern "C" {
#include "port/crash_note.h"
#include "port/profile.h"
#include "port/zones.h"
}

// (pcr_pipeline_seed.cpp, made by cmake/pipeline_seed.cmake from port/gs/pc/pipeline_seed.bin)
extern const unsigned char pcr_pipeline_seed[];
extern const size_t pcr_pipeline_seed_size;

namespace Vulkan {
// (cmake/granite_pipeline_warm.inc)
extern void (*kf4_on_compile)(Device *device, const DeferredPipelineCompile &compile, bool compute, int64_t ns,
                                   bool sync, bool ok);
extern void (*kf4_on_render_pass)(Util::Hash hash, const RenderPassInfo &info);
} // namespace Vulkan

using namespace Vulkan;

namespace pcr {

namespace {

// A compatible render pass's attachments (the canvas's: one colour attachment or none, a depth one or none) and
// subpasses, as a record names them
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
    uint32_t role;       // (warm_watch's index)
    uint32_t per_sample; // (1: a variant shaded at every sample, of the MSAA the port had: skipped)
    uint32_t samples;    // (the pass's: 1; more, of the MSAA the port had: skipped)
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
constexpr size_t RECORDS_MOST = 4096;           // (the list's length at most: the oldest dropped past it)
constexpr size_t CACHE_BYTES_MOST = 256u << 20; // (a cache file larger than this is not read: made afresh)

struct Warm {
    std::mutex lock;
    Device *device = nullptr;
    std::unordered_map<uint64_t, PassDesc> passes;
    const bool log = getenv("KF4_PC_WARM_LOG") != nullptr; // (each pipeline made, on stderr)
    // the programs by role (warm_watch's index), and each program's role
    std::unordered_map<uint32_t, Program *> roles;
    std::unordered_map<Program *, uint32_t> role_of;
    std::vector<Program *> compute_programs; // (default state, for startup only)
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
    std::chrono::steady_clock::time_point pre_asked_at;
    size_t pre_seed = 0, pre_list = 0; // (the pipelines asked for: from the seed for these settings, from the list)
    std::vector<FullscreenPipeline> pre_fullscreen;
    // attachments of the formats the list's passes have (made by the threads making ahead)
    std::mutex images_lock;
    std::map<std::pair<VkFormat, VkFormat>, std::pair<ImageHandle, ImageHandle>> images;
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
    if (sync && !on_warm_thread && ns > 0) {
        // (a draw waited for it: Shift+P's count of them, and the frame profiler's zone on the drawing thread)
        static const ZoneId zone = port_zone_id("pipeline made while a draw waited", ZONE_WAIT);
        port_profile_compile(uint64_t(ns));
        uint64_t now = port_profile_now_ns();
        port_zone_after(zone, now > uint64_t(ns) ? now - uint64_t(ns) : 0);
    }
    if (compute) {
        if (warm.log && ok) {
            fprintf(stderr, "[warm] %s %s compute %016llx: %.1f ms\n", on_warm_thread ? "ahead" : "dispatch",
                    sync ? "sync" : "async", (unsigned long long)compile.hash, double(ns) / 1e6);
        }
        return;
    }
    if (!ok) {
        return;
    }
    std::lock_guard<std::mutex> hold(warm.lock);
    auto pa = warm.passes.find(uint64_t(compile.compatible_render_pass->get_hash()));
    if (warm.log) {
        auto ro = warm.role_of.find(compile.program);
        fprintf(stderr, "[warm] %s %s program %p (role %d), colours %u depth %u subpasses %u: %.1f ms, hash %016llx\n",
                on_warm_thread ? "ahead" : "draw", sync ? "sync" : "async", (void *)compile.program,
                ro != warm.role_of.end() ? int(ro->second) : -1, pa != warm.passes.end() ? pa->second.colours : 9,
                pa != warm.passes.end() ? pa->second.depth : 9, pa != warm.passes.end() ? pa->second.subpasses : 9,
                double(ns) / 1e6, (unsigned long long)compile.hash);
    }
    if (on_warm_thread || pa == warm.passes.end()) {
        return;
    }
    remember(compile, pa->second);
}

// One pipeline made from compile with program and the pass, unless made already
void make(Device &device, DeferredPipelineCompile compile, Program *program, const RenderPass &pass) {
    compile.program = program;
    compile.layout = program->get_pipeline_layout();
    compile.compatible_render_pass = &pass;
    compile.cache = CommandBuffer::kf4_pipeline_cache(&device);
    CommandBuffer::kf4_rehash(compile, false);
    if (program->get_pipeline(compile.hash).pipeline != VK_NULL_HANDLE) {
        return;
    }
    if (CommandBuffer::build_graphics_pipeline(&device, compile, CommandBuffer::CompileMode::Sync).pipeline) {
        store.ahead_made++;
    }
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
    r.role = role->second;
    r.samples = 1;
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
        if (r.subpasses > 2 || r.colours > 1 || r.depth > 1 || r.samples != 1 || r.per_sample != 0) {
            continue; // (a multisampled pass's: of the MSAA the port had)
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

// (store.lock held) Whether threads are making pipelines ahead now
bool making_ahead() {
    return store.ahead_started && store.ahead_done.load() < store.ahead_total.load() && !store.ahead_stop;
}

// (its own thread) The files written every SAVE_EVERY. Not while pipelines are made ahead: the cache read out
// (vkGetPipelineCacheData) while a dozen threads add to it is allowed, but it is where a driver's own locking is
// tested hardest (a player's crash in AMD's Windows driver at 76% of "Preparing shaders", 2026-11-19), for no gain:
// the next tick after they are done writes them.
void saver() {
    std::unique_lock<std::mutex> hold(store.lock);
    while (!store.stopping) {
        store.wake.wait_for(hold, SAVE_EVERY);
        if (!store.stopping && !making_ahead()) {
            save_locked();
        }
    }
}

bool format_ok(Device &device, VkFormat format, VkFormatFeatureFlags2 need);

// Attachments of a record's formats, made as the canvas makes its own: the render passes made from them are the
// canvas's (a compatible pass is named by its attachments' formats, samples and layout, ImageLayout::General). Made by any thread: never drawn into, so made
// with no first layout (VK_IMAGE_LAYOUT_UNDEFINED), whose change Granite would record in a command buffer of the
// thread's own
std::pair<ImageHandle, ImageHandle> &images_for(Device &device, const Record &r) {
    std::lock_guard<std::mutex> hold(store.images_lock);
    auto &slot = store.images[{r.colour_format, r.depth_format}];
    if (!slot.first && r.colours && format_ok(device, r.colour_format, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) {
        ImageCreateInfo info = ImageCreateInfo::render_target(16, 16, r.colour_format);
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        slot.first = device.create_image(info, nullptr);
    }
    if (!slot.second && r.depth && format_ok(device, r.depth_format, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        ImageCreateInfo info = ImageCreateInfo::render_target(16, 16, r.depth_format);
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        slot.second = device.create_image(info, nullptr);
    }
    return slot;
}

bool format_ok(Device &device, VkFormat format, VkFormatFeatureFlags2 need) {
    return format != VK_FORMAT_UNDEFINED && device.image_format_is_supported(format, need);
}

// (a thread making ahead) One record's pipeline made, through the cache
void make_ahead(Device &device, const Record &r, Program *program) {
    if (!program || r.samples != 1 || r.per_sample != 0 ||
        (r.colours && !format_ok(device, r.colour_format, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)) ||
        (r.depth && !format_ok(device, r.depth_format, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT))) {
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
    const RenderPass &pass = CommandBuffer::kf4_render_pass(&device, info);
    DeferredPipelineCompile compile;
    memset(&compile, 0, sizeof(compile));
    memcpy(&compile.static_state, &r.static_state, sizeof(r.static_state));
    memcpy(&compile.potential_static_state, &r.potential_static_state, sizeof(r.potential_static_state));
    memcpy(compile.attribs, r.attribs, sizeof(r.attribs));
    memcpy(compile.strides, r.strides, sizeof(r.strides));
    memcpy(compile.input_rates, r.input_rates, sizeof(r.input_rates));
    // (a record of an older build's vertex input: the inputs its program reads and it has none for, the canvas's)
    uint32_t inputs = program->get_pipeline_layout()->get_resource_layout().attribute_mask;
    bool filled = false;
    for (uint32_t i = 0; i < VULKAN_NUM_VERTEX_ATTRIBS; i++) {
        if ((inputs & (1u << i)) && compile.attribs[i].format == VK_FORMAT_UNDEFINED &&
            warm.traits.attribs[i].format != VK_FORMAT_UNDEFINED) {
            compile.attribs[i] = warm.traits.attribs[i];
            filled = true;
        }
    }
    if (filled && warm.traits.vertex_stride != 0) {
        compile.strides[0] = warm.traits.vertex_stride;
    }
    compile.subpass_index = r.subpass_index;
    compile.subgroup_size_tag = r.subgroup_size_tag;
    {
        // (records that differ only where their pipelines do not, e.g. in state made dynamic: made once)
        DeferredPipelineCompile named = compile;
        named.program = program;
        named.layout = program->get_pipeline_layout();
        named.compatible_render_pass = &pass;
        CommandBuffer::kf4_rehash(named, false);
        std::lock_guard<std::mutex> hold(store.hashes_lock);
        if (!store.hashes.insert(uint64_t(named.hash)).second) {
            return;
        }
    }
    auto start = std::chrono::steady_clock::now();
    make(device, compile, program, pass);
    store.ahead_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

// The plane pass dispatches with a fresh command buffer's default compute state.
void make_compute_ahead(Device &device, Program *program) {
    DeferredPipelineCompile compile{};
    compile.program = program;
    compile.layout = program->get_pipeline_layout();
    compile.cache = CommandBuffer::kf4_pipeline_cache(&device);
    CommandBuffer::kf4_rehash(compile, true);
    if (program->get_pipeline(compile.hash).pipeline != VK_NULL_HANDLE) {
        return;
    }
    auto start = std::chrono::steady_clock::now();
    if (CommandBuffer::build_compute_pipeline(&device, compile, CommandBuffer::CompileMode::Sync).pipeline) {
        store.ahead_made++;
    }
    store.ahead_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

struct GraphicsJob {
    Record record;
    Program *program;
};

Record fullscreen_record(VkFormat format) {
    Record r{};
    r.samples = 1;
    r.colours = 1;
    r.colour_format = format;
    // The AA and scaling passes use opaque state with culling and depth disabled.
    auto &state = r.static_state.state;
    state.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.cull_mode = VK_CULL_MODE_NONE;
    state.depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.write_mask = ~0u;
    return r;
}

// (store.lock and warm.lock held) Build the list in the background or the startup work while the game waits.
void start_ahead(const std::vector<Record> &list, std::vector<Program *> compute,
                 std::span<const FullscreenPipeline> fullscreen, unsigned threads) {
    const size_t total = list.size() + compute.size() + fullscreen.size();
    if (store.ahead_started || !store.open || total == 0 || !warm.device) {
        return;
    }
    store.ahead_started = true;
    auto graphics = std::make_shared<std::vector<GraphicsJob>>();
    graphics->reserve(list.size() + fullscreen.size());
    for (const FullscreenPipeline &pass : fullscreen) {
        graphics->push_back({fullscreen_record(pass.format), pass.program});
    }
    for (const Record &record : list) {
        auto role = warm.roles.find(record.role);
        graphics->push_back({record, role == warm.roles.end() ? nullptr : role->second});
    }
    auto compute_programs = std::make_shared<std::vector<Program *>>(std::move(compute));
    auto next = std::make_shared<std::atomic<size_t>>(0);
    store.ahead_total = total;
    store.ahead_start = std::chrono::steady_clock::now();
    Device *device = warm.device;
    for (unsigned t = 0; t < threads; t++) {
        store.ahead.emplace_back([=] {
            on_warm_thread = true;
            // (Granite's compile log names a thread by its index: these threads request no command buffer, the only
            // other use of it, so the canvas thread's index stands in, as an unregistered thread's would)
            Util::register_thread_index(0);
            for (size_t i = (*next)++; i < total && !store.ahead_stop; i = (*next)++) {
                char note[192];
                if (i < compute_programs->size()) {
                    snprintf(note, sizeof note, "making pipeline %zu of %zu ahead (compute program %zu)", i + 1, total,
                             i);
                    port_crash_note_set(note);
                    make_compute_ahead(*device, (*compute_programs)[i]);
                } else {
                    const GraphicsJob &job = (*graphics)[i - compute_programs->size()];
                    const auto &st = job.record.static_state.state;
                    snprintf(note, sizeof note,
                             "making pipeline %zu of %zu ahead (role %u, colour format %d, depth format %d, depth test "
                             "%u compare %u, stencil %u, blend %u, dynamic %u%u, record %016llx)",
                             i + 1, total, unsigned(job.record.role), int(job.record.colour_format),
                             int(job.record.depth ? job.record.depth_format : VK_FORMAT_UNDEFINED),
                             unsigned(st.depth_test), unsigned(st.depth_compare), unsigned(st.stencil_test),
                             unsigned(st.blend_enable), unsigned(st.dynamic_ds), unsigned(st.dynamic_blend),
                             (unsigned long long)record_key(job.record));
                    port_crash_note_set(note);
                    make_ahead(*device, job.record, job.program);
                }
                port_crash_note_set("");
                if (++store.ahead_done == total) {
                    store.ahead_wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - store.ahead_start)
                                              .count();
                    if (warm.log || getenv("KF4_PC_PIPELINE_LOG") || store.pre_wanted) {
                        fprintf(stderr,
                                "[pcr] pipelines made ahead: %llu made, %llu skipped, of %zu asked for, in %.1f ms on "
                                "%u threads\n",
                                (unsigned long long)store.ahead_made.load(),
                                (unsigned long long)store.ahead_skipped.load(), total,
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
// with the MSAA samples of the run it was drawn in (the port had MSAA until 2026-11-16: only the runs at 1 are
// used). Renderer state only (programs by the canvas's index, Granite's
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

// (R16G16B16A16: a seed record of colour high's, removed 2026-11-14; made at 8 bits)
bool is_colour_format(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R16G16B16A16_UNORM;
}

bool is_depth_format(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT;
}

// (pcr_pipelines.h's WarmDevice::decal_roles) A seed record of a batch program's that tests Z GEQUAL, as drawn by
// pcr_canvas.cpp's settle_decal_depth: each state there sets, on the state the record has; none when it is not such a
// record
std::vector<Record> decal_states(const Record &r, const WarmDevice &t) {
    std::vector<Record> out;
    const auto &st = r.static_state.state;
    if (r.role >= t.decal_roles || !r.depth || !st.depth_test || st.depth_compare != VK_COMPARE_OP_GREATER_OR_EQUAL) {
        return out;
    }
    auto stencil = [](Record &d, bool on, VkCompareOp compare, VkStencilOp pass) {
        auto &ds = d.static_state.state;
        ds.stencil_test = on ? 1 : 0;
        ds.stencil_front_compare_op = ds.stencil_back_compare_op = compare;
        ds.stencil_front_pass = ds.stencil_back_pass = pass;
        ds.stencil_front_fail = ds.stencil_back_fail = VK_STENCIL_OP_KEEP;
        ds.stencil_front_depth_fail = ds.stencil_back_depth_fail = VK_STENCIL_OP_KEEP;
    };
    Record mark = r; // (mark_decal_won: the stencil's test on in the passes that write Z, its ops set in each)
    stencil(mark, st.depth_write != 0, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE);
    out.push_back(mark);
    Record reset = r; // (the depth reset)
    reset.role = t.depth_reset_first + r.role % t.decal_programs;
    reset.per_sample = 0;
    reset.static_state.state.depth_write = 1;
    reset.static_state.state.depth_compare = VK_COMPARE_OP_ALWAYS;
    reset.static_state.state.blend_enable = 0;
    reset.static_state.state.write_mask = 0;
    stencil(reset, true, VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_ZERO);
    out.push_back(reset);
    Record again = reset; // (the passes again, with the decal's own depth)
    again.role = r.role % t.decal_programs;
    again.per_sample = r.per_sample;
    again.static_state.state.depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    again.static_state.state.stencil_test = 0;
    out.push_back(again);
    return out;
}

// (the depth rule) Whether a record is of a batch program's (roles 0-9, 11-20) that tests Z GEQUAL or GREATER, drawn
// with the rule's program instead; rule: that record
bool rule_record(const Record &r, const WarmDevice &t, Record &rule) {
    const auto &st = r.static_state.state;
    bool tests = r.depth && st.depth_test &&
                 (st.depth_compare == VK_COMPARE_OP_GREATER_OR_EQUAL || st.depth_compare == VK_COMPARE_OP_GREATER ||
                  (t.rule_always && st.depth_compare == VK_COMPARE_OP_ALWAYS && st.depth_write));
    bool batch = r.role < 10 || (r.role >= 11 && r.role < 21);
    if (t.rule_first == 0 || !tests || !batch) {
        return false;
    }
    uint32_t kind = r.role < 5 ? 0 : r.role < 10 ? 1 : r.role < 16 ? 2 : 3; // (plain, early, the runs' two)
    uint32_t v = r.role < 10 ? r.role % 5 : (r.role - 11) % 5;
    rule = r;
    rule.role = t.rule_first + 5 * kind + v;
    return true;
}

// (the depth rule's own copy, WarmDevice::zclear_role) The copy's clear in a record's pass: pcr_canvas.cpp's
// clear_depth_copy's state (set_opaque_state, then no culling, no depth or stencil test, no blend, nothing written)
bool zclear_record(const Record &r, const WarmDevice &t, Record &z) {
    if (t.zclear_role == 0 || !r.depth) {
        return false;
    }
    memset(&z, 0, sizeof(z));
    z.role = t.zclear_role;
    z.samples = r.samples;
    z.colours = r.colours;
    z.depth = r.depth;
    z.subpasses = r.subpasses;
    z.colour_format = r.colour_format;
    z.depth_format = r.depth_format;
    memcpy(z.subpass, r.subpass, sizeof(z.subpass));
    z.subpass_index = r.subpass_index;
    z.subgroup_size_tag = r.subgroup_size_tag;
    auto &st = z.static_state.state;
    st.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    st.cull_mode = VK_CULL_MODE_NONE;
    st.depth_compare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    st.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    st.write_mask = 0;
    st.dynamic_ds = r.static_state.state.dynamic_ds;
    st.dynamic_blend = r.static_state.state.dynamic_blend;
    return true;
}

// (warm.lock held) The seed's pipelines for this device: those of the runs without MSAA, the canvas's formats and
// dynamic state put in. Empty when the seed is not of this build's layout (Granite's state changed:
// tools/pipeline_seed.py makes it again).
std::vector<Record> seed_for_device() {
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
    std::unordered_set<uint64_t> keys;
    for (uint32_t i = 0; i < got.count; i++) {
        SeedEntry e;
        memcpy(&e, pcr_pipeline_seed + sizeof(got) + size_t(i) * sizeof(SeedEntry), sizeof(e));
        Record &r = e.record;
        // (the runs with MSAA, of the MSAA the port had: not used)
        if (e.run_samples != 1 || r.samples != 1 || r.per_sample != 0) {
            continue;
        }
        if (r.depth && is_depth_format(r.depth_format) && t.depth_format != VK_FORMAT_UNDEFINED) {
            r.depth_format = t.depth_format;
        }
        r.static_state.state.dynamic_ds = t.dynamic_ds ? 1 : 0;
        r.static_state.state.dynamic_blend = t.dynamic_ds && t.dynamic_blend ? 1 : 0;
        if (r.colours && is_colour_format(r.colour_format)) {
            r.colour_format = VK_FORMAT_R8G8B8A8_UNORM; // (the colour surfaces' format)
        }
        Record rule;
        if (rule_record(r, t, rule)) {
            r = rule; // (the program drawn with it: the rule's)
        }
        if (keys.insert(record_key(r)).second) {
            out.push_back(r);
        }
        Record z;
        if (zclear_record(r, t, z) && keys.insert(record_key(z)).second) {
            out.push_back(z);
        }
        if (t.date_mark_role != 0 && r.role == 10) {
            Record mark = r; // (DATE's set-up, marking the copy: its state)
            mark.role = t.date_mark_role;
            if (keys.insert(record_key(mark)).second) {
                out.push_back(mark);
            }
        }
        for (const Record &d : decal_states(r, t)) {
            if (keys.insert(record_key(d)).second) {
                out.push_back(d);
            }
        }
    }
    return out;
}

// (store.lock and warm.lock held) Made before play, once both the settings (precompile) and the programs
// (warm_watch) are known: fixed compute, active post-processing, the seed for these settings, and the list.
void start_precompile() {
    if (!store.pre_asked || store.ahead_started || !warm.device) {
        return;
    }
    std::vector<Record> list = seed_for_device();
    store.pre_seed = list.size();
    store.pre_list = store.records.size();
    list.insert(list.end(), store.records.begin(), store.records.end());
    unsigned cores = std::thread::hardware_concurrency();
    {
        fprintf(stderr, "[pcr] pipelines made before play: %zu from the seed, %zu from the list, %zu fixed compute, "
                        "%zu post-processing\n",
                store.pre_seed, store.pre_list, warm.compute_programs.size(), store.pre_fullscreen.size());
    }
    start_ahead(list, warm.compute_programs, store.pre_fullscreen, cores > 2 ? cores - 1 : 1);
    store.pre_fullscreen.clear();
    if (!store.ahead_started) {
        store.pre_wanted = false; // (nothing to make)
    }
}

} // namespace

void warm_watch(Device &device, Program *const *programs, uint32_t count, const WarmDevice &traits,
                std::span<Program *const> compute_programs) {
    std::lock_guard<std::mutex> hold_store(store.lock);
    std::lock_guard<std::mutex> hold(warm.lock);
    warm.device = &device;
    warm.traits = traits;
    for (uint32_t i = 0; i < count; i++) {
        if (programs[i]) {
            warm.roles[i] = programs[i];
            warm.role_of[programs[i]] = i;
        }
    }
    warm.compute_programs.clear();
    for (Program *program : compute_programs) {
        if (program) {
            warm.compute_programs.push_back(program);
        }
    }
    Vulkan::kf4_on_compile = on_compile;
    Vulkan::kf4_on_render_pass = on_render_pass;
    if (store.pre_on) {
        start_precompile(); // (when the settings came first)
    } else if (store.ahead_on) {
        start_ahead(store.records, {}, {}, std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 4)));
    }
}

void precompile(std::vector<FullscreenPipeline> fullscreen) {
    std::lock_guard<std::mutex> hold_store(store.lock);
    std::lock_guard<std::mutex> hold(warm.lock);
    if (!store.open || !store.pre_on || store.pre_asked) {
        return;
    }
    store.pre_asked = true;
    store.pre_fullscreen = std::move(fullscreen);
    store.pre_wanted = true;
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
    Vulkan::kf4_on_compile = on_compile;
    Vulkan::kf4_on_render_pass = on_render_pass;
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
}

} // namespace pcr
