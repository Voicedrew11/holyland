// ---- Pipelines made ahead (included into gsn.cpp; docs/port/RENDERER.md, "Pipelines made ahead") -------------------
// A Vulkan pipeline is made the first time a draw needs it. Made from nothing (a fresh data folder, a new build whose
// shaders changed, a new driver) one of the GS's big fragment shaders takes the driver seconds, and the GS's thread
// waits: the picture stood still for 1-3 seconds the first time an area was walked into. So every pipeline made is
// written down (what Granite made it from: its shaders, render pass, state), in a list shipped in the program
// (gsn_pipeline_list.h, from tours of the maps: tools/pipeline_list.py) and in one in the data folder (pipelines
// made in play that the shipped list did not have). At the start, the pipelines of the list for these settings are
// made on threads of their own, behind the intro and the title: first those the pipeline cache has (made at once),
// then the rest ("Building shaders" in the window, gsn_pipelines_progress). The game waits for them only when play
// starts (port/platform/kernel.c). A draw that needs a pipeline not made yet still makes it then (as before).
//
// Each line of a list: "S hash layout" (a shader's resource layout), "R hash pass" (a compatible render pass: its
// attachments), "G tag hash vs fs pass subpass subgroup state potential attribs strides rates" (a graphics pipeline)
// or "C tag hash cs subgroup state potential" (a compute one), the blobs in hex with their trailing zero bytes left
// out. Everything is checked by Granite's own hash when made again: a line from another build that no longer makes
// the same pipeline (a shader changed, Granite's state moved) is skipped.

#include "gsn_pipeline_list.h" // (the shipped list: gsn_pipeline_list[])

// (gsn_pipeline_set.inc: every pipeline the draws can ask for, worked out from the code)
struct PwJob;
void ps_jobs(std::vector<PwJob> &jobs, std::set<uint64_t> &taken, std::vector<ImageHandle> &images);
void ps_on_play_compile(const DeferredPipelineCompile &c, bool compute, int64_t ns);
bool ps_only();

// a compatible render pass: what Device::request_render_pass hashes, and the samples it makes the pass with
struct PwPass {
    uint32_t colours;
    uint32_t colour_format[VULKAN_NUM_ATTACHMENTS], colour_samples[VULKAN_NUM_ATTACHMENTS];
    uint32_t lazy; // (attachments in transient images: a bit each, the depth's after the colours')
    uint32_t depth_format, depth_samples;
    uint32_t base_layer, layers, subpasses;
    RenderPassInfo::Subpass subpass[4];
};
struct PwEntry {
    std::string tag;
    uint64_t hash = 0;
    bool compute = false;
    uint64_t shader[2] = {};
    uint64_t pass = 0;
    uint32_t subpass = 0, subgroup = 0;
    PipelineState state = {};
    PotentialState potential = {};
    VertexAttribState attribs[VULKAN_NUM_VERTEX_ATTRIBS] = {};
    VkDeviceSize strides[VULKAN_NUM_VERTEX_BUFFERS] = {};
    VkVertexInputRate rates[VULKAN_NUM_VERTEX_BUFFERS] = {};
    bool shipped = false;
};
struct PipelineWarm {
    std::mutex lock;
    std::unordered_map<uint64_t, std::vector<uint8_t>> shaders; // a shader's ResourceLayout, serialized
    std::unordered_map<uint64_t, PwPass> passes;
    std::vector<PwEntry> entries;
    std::set<std::pair<std::string, uint64_t>> known; // (tag, hash)
    std::string path;     // the data folder's list
    size_t saved = 0;     // entries not shipped when it was last written
    size_t unshipped = 0; // entries not shipped now
    // the start's work
    std::atomic<int> total{0}, done{0}, building{0}, ready{0}, stale{0};
    std::atomic<bool> running{false}, stop{false};
    double seconds_check = 0, seconds_build = 0;
    // made in play: by a draw that waited (and those not on any list)
    std::atomic<uint64_t> play_made{0}, play_ns{0}, play_new{0};
    std::atomic<uint64_t> made{0}; // every pipeline made (the cache and the list are written a while after it moves)
    uint64_t made_saved = 0;
} pw;
thread_local bool pw_starting; // (pw_start's thread, holding pw.lock: Granite's hooks leave it be)

std::string pw_hex(const void *data, size_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    while (size && !p[size - 1]) {
        size--;
    }
    std::string s(size * 2, '0');
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        s[i * 2] = digits[p[i] >> 4];
        s[i * 2 + 1] = digits[p[i] & 15];
    }
    return s.empty() ? "-" : s;
}
bool pw_unhex(const std::string &s, void *data, size_t size) {
    uint8_t *p = static_cast<uint8_t *>(data);
    memset(p, 0, size);
    if (s == "-") {
        return true;
    }
    if (s.size() % 2 || s.size() / 2 > size) {
        return false;
    }
    for (size_t i = 0; i < s.size() / 2; i++) {
        unsigned v;
        if (sscanf(s.c_str() + i * 2, "%2x", &v) != 1) {
            return false;
        }
        p[i] = uint8_t(v);
    }
    return true;
}

// the settings a pipeline was made at: those that decide which pipelines there are (the samples, cut-out AA, the
// colour's precision, the scene and its samples)
std::string pw_tag() {
    char tag[64];
    snprintf(tag, sizeof(tag), "m%u.c%d.k%u.s%d.x%u", unsigned(wanted_samples), int(cutout_aa_mode), unsigned(wanted_colour),
             int(sa_mode), unsigned(sa_wanted_samples));
    return tag;
}

std::string pw_line(const PwEntry &e) {
    char head[160];
    if (e.compute) {
        snprintf(head, sizeof(head), "C %s %016llx %016llx %u ", e.tag.c_str(), (unsigned long long)e.hash,
                 (unsigned long long)e.shader[0], e.subgroup);
        return head + pw_hex(&e.state, sizeof(e.state)) + " " + pw_hex(&e.potential, sizeof(e.potential));
    }
    snprintf(head, sizeof(head), "G %s %016llx %016llx %016llx %016llx %u %u ", e.tag.c_str(), (unsigned long long)e.hash,
             (unsigned long long)e.shader[0], (unsigned long long)e.shader[1], (unsigned long long)e.pass, e.subpass,
             e.subgroup);
    return head + pw_hex(&e.state, sizeof(e.state)) + " " + pw_hex(&e.potential, sizeof(e.potential)) + " " +
           pw_hex(e.attribs, sizeof(e.attribs)) + " " + pw_hex(e.strides, sizeof(e.strides)) + " " +
           pw_hex(e.rates, sizeof(e.rates));
}

// one line of a list (pw.lock held)
void pw_read_line(const std::string &line, bool shipped) {
    std::vector<std::string> f;
    size_t at = 0;
    while (at < line.size()) {
        size_t end = line.find(' ', at);
        if (end == std::string::npos) {
            end = line.size();
        }
        if (end > at) {
            f.push_back(line.substr(at, end - at));
        }
        at = end + 1;
    }
    if (f.empty()) {
        return;
    }
    auto u64 = [](const std::string &s) { return (uint64_t)strtoull(s.c_str(), nullptr, 16); };
    if (f[0] == "S" && f.size() == 3) {
        std::vector<uint8_t> layout(ResourceLayout::serialization_size());
        if (pw_unhex(f[2], layout.data(), layout.size())) {
            pw.shaders.emplace(u64(f[1]), layout);
        }
    } else if (f[0] == "R" && f.size() == 3) {
        PwPass p;
        if (pw_unhex(f[2], &p, sizeof(p))) {
            pw.passes.emplace(u64(f[1]), p);
        }
    } else if ((f[0] == "G" && f.size() == 13) || (f[0] == "C" && f.size() == 7)) {
        PwEntry e;
        e.compute = f[0] == "C";
        e.tag = f[1];
        e.hash = u64(f[2]);
        e.shader[0] = u64(f[3]);
        bool ok;
        if (e.compute) {
            e.subgroup = uint32_t(strtoul(f[4].c_str(), nullptr, 10));
            ok = pw_unhex(f[5], &e.state, sizeof(e.state)) && pw_unhex(f[6], &e.potential, sizeof(e.potential));
        } else {
            e.shader[1] = u64(f[4]);
            e.pass = u64(f[5]);
            e.subpass = uint32_t(strtoul(f[6].c_str(), nullptr, 10));
            e.subgroup = uint32_t(strtoul(f[7].c_str(), nullptr, 10));
            ok = pw_unhex(f[8], &e.state, sizeof(e.state)) && pw_unhex(f[9], &e.potential, sizeof(e.potential)) &&
                 pw_unhex(f[10], e.attribs, sizeof(e.attribs)) && pw_unhex(f[11], e.strides, sizeof(e.strides)) &&
                 pw_unhex(f[12], e.rates, sizeof(e.rates));
        }
        e.shipped = shipped;
        if (ok && pw.known.emplace(e.tag, e.hash).second) {
            pw.entries.push_back(e);
            pw.unshipped += !shipped;
        }
    }
}

// The data folder's list, written again when it has more (the shaders and passes its lines name, then its lines)
void pw_save() {
    std::string text;
    {
        std::lock_guard<std::mutex> hold(pw.lock);
        if (pw.path.empty() || pw.unshipped == pw.saved) {
            return;
        }
        std::set<uint64_t> shaders, passes;
        std::string lines;
        for (const PwEntry &e : pw.entries) {
            if (e.shipped) {
                continue;
            }
            shaders.insert(e.shader[0]);
            if (!e.compute) {
                shaders.insert(e.shader[1]);
                passes.insert(e.pass);
            }
            lines += pw_line(e) + "\n";
        }
        text = "# holyland: pipelines made in play that the shipped list does not have (gsn_pipeline_warm.inc)\n";
        for (uint64_t h : shaders) {
            auto it = pw.shaders.find(h);
            if (it != pw.shaders.end()) {
                char head[32];
                snprintf(head, sizeof(head), "S %016llx ", (unsigned long long)h);
                text += head + pw_hex(it->second.data(), it->second.size()) + "\n";
            }
        }
        for (uint64_t h : passes) {
            auto it = pw.passes.find(h);
            if (it != pw.passes.end()) {
                char head[32];
                snprintf(head, sizeof(head), "R %016llx ", (unsigned long long)h);
                text += head + pw_hex(&it->second, sizeof(it->second)) + "\n";
            }
        }
        text += lines;
        pw.saved = pw.unshipped;
    }
    std::string temp = pw.path + ".new";
    if (FILE *f = fopen(temp.c_str(), "wb")) {
        bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
        written = fclose(f) == 0 && written;
        if (written) {
            remove(pw.path.c_str());
            rename(temp.c_str(), pw.path.c_str());
        }
    }
}

// ---- Granite's hooks (cmake/granite_pipeline_warm.inc) -----------------------------------------------------------
void pw_on_render_pass(Util::Hash hash, const RenderPassInfo &info) {
    if (pw_starting) {
        return; // (made again from the list: it has it)
    }
    PwPass p = {};
    if (info.num_color_attachments > VULKAN_NUM_ATTACHMENTS || info.num_subpasses > 4) {
        return;
    }
    p.colours = info.num_color_attachments;
    for (unsigned i = 0; i < info.num_color_attachments; i++) {
        const Image &image = info.color_attachments[i]->get_image();
        if (image.get_swapchain_layout() != VK_IMAGE_LAYOUT_UNDEFINED) {
            return; // (the window's: not made again)
        }
        p.colour_format[i] = info.color_attachments[i]->get_format();
        p.colour_samples[i] = image.get_create_info().samples;
        p.lazy |= (image.get_create_info().domain == ImageDomain::Transient) << i;
    }
    if (info.depth_stencil) {
        const Image &image = info.depth_stencil->get_image();
        p.depth_format = info.depth_stencil->get_format();
        p.depth_samples = image.get_create_info().samples;
        p.lazy |= (image.get_create_info().domain == ImageDomain::Transient) << info.num_color_attachments;
    }
    p.base_layer = info.num_layers > 1 ? info.base_layer : 0;
    p.layers = info.num_layers;
    p.subpasses = info.num_subpasses;
    for (unsigned i = 0; i < info.num_subpasses; i++) {
        p.subpass[i] = info.subpasses[i];
    }
    std::lock_guard<std::mutex> hold(pw.lock);
    pw.passes.emplace(hash, p);
}

void pw_note_shader(const Shader *s) {
    if (pw.shaders.count(s->get_hash())) {
        return;
    }
    std::vector<uint8_t> layout(ResourceLayout::serialization_size());
    if (s->get_layout().serialize(layout.data(), layout.size())) {
        pw.shaders.emplace(s->get_hash(), layout);
    }
}

void pw_on_compile(Device *device, const DeferredPipelineCompile &c, bool compute, int64_t ns, bool sync, bool ok) {
    if (!ok || !gs || device != gs->device) {
        return; // (a failed one (one not in the cache, asked for so), or paraLLEl-GS's)
    }
    pw.made++;
    if (!sync) {
        return; // (the start's own)
    }
    pw.play_made++;
    pw.play_ns += uint64_t(ns);
    if (port_profile_on()) {
        port_profile_add(PROFILE_COMPILE, uint64_t(ns));
    }
    hitch_compiles++;
    hitch_compile_ns += uint64_t(ns);
    ps_on_play_compile(c, compute, ns);
    PwEntry e;
    e.tag = pw_tag();
    e.hash = c.hash;
    e.compute = compute;
    e.subgroup = c.subgroup_size_tag;
    e.state = c.static_state;
    e.potential = c.potential_static_state;
    std::lock_guard<std::mutex> hold(pw.lock);
    if (pw.known.count({e.tag, e.hash})) {
        return;
    }
    if (compute) {
        const Shader *cs = c.program->get_shader(ShaderStage::Compute);
        if (!cs) {
            return;
        }
        pw_note_shader(cs);
        e.shader[0] = cs->get_hash();
    } else {
        const Shader *vs = c.program->get_shader(ShaderStage::Vertex), *fs = c.program->get_shader(ShaderStage::Fragment);
        if (!vs || !fs || !c.compatible_render_pass || !pw.passes.count(c.compatible_render_pass->get_hash())) {
            return; // (mesh shaders, or a pass in the window's image: not ours)
        }
        pw_note_shader(vs);
        pw_note_shader(fs);
        e.shader[0] = vs->get_hash();
        e.shader[1] = fs->get_hash();
        e.pass = c.compatible_render_pass->get_hash();
        e.subpass = c.subpass_index;
        memcpy(e.attribs, c.attribs, sizeof(e.attribs));
        memcpy(e.strides, c.strides, sizeof(e.strides));
        memcpy(e.rates, c.input_rates, sizeof(e.rates));
    }
    pw.known.emplace(e.tag, e.hash);
    pw.entries.push_back(e);
    pw.unshipped++;
    pw.play_new++;
    if (!pw.path.empty() && !ps_only()) {
        fprintf(stderr, "[gsn] a pipeline not on the list was made in play (%.1f ms): kept in the data folder's list\n",
                double(ns) / 1e6);
    }
}

// ---- At the start -------------------------------------------------------------------------------------------------
// A shader by its hash: the one Granite has, or made from the program's SPIR-V with the layout the list kept
Shader *pw_shader(uint64_t hash) {
    if (Shader *s = gs->device->request_shader_by_hash(hash)) {
        return s;
    }
    auto layout = pw.shaders.find(hash);
    if (layout == pw.shaders.end()) {
        return nullptr;
    }
    for (const auto &spv : gsn_spv_all) {
        if (Shader::hash(spv.code, spv.size) == hash) {
            ResourceLayout l;
            if (!l.unserialize(layout->second.data(), layout->second.size())) {
                return nullptr;
            }
            return gs->device->request_shader(spv.code, spv.size, &l);
        }
    }
    return nullptr; // (a shader of another build)
}

// A compatible render pass made again from its attachments (images of a pixel, let go after); null if it does
// not come out with the hash it was kept under
const RenderPass *pw_render_pass(uint64_t hash, std::vector<ImageHandle> &images) {
    auto it = pw.passes.find(hash);
    if (it == pw.passes.end()) {
        return nullptr;
    }
    const PwPass &p = it->second;
    if (p.layers > 1) {
        return nullptr;
    }
    auto make = [&](uint32_t format, uint32_t samples, bool lazy) -> const ImageView * {
        ImageCreateInfo info = ImageCreateInfo::render_target(1, 1, VkFormat(format));
        info.samples = VkSampleCountFlagBits(samples ? samples : 1);
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (lazy) {
            info.domain = ImageDomain::Transient;
            info.usage = (format_has_depth_or_stencil_aspect(VkFormat(format)) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                                : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                         VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        }
        ImageHandle image = gs->device->create_image(info);
        if (!image) {
            return nullptr;
        }
        images.push_back(image);
        return &image->get_view();
    };
    RenderPassInfo info;
    info.num_color_attachments = p.colours;
    for (uint32_t i = 0; i < p.colours; i++) {
        info.color_attachments[i] = make(p.colour_format[i], p.colour_samples[i], (p.lazy >> i) & 1);
        if (!info.color_attachments[i]) {
            return nullptr;
        }
    }
    if (p.depth_format) {
        info.depth_stencil = make(p.depth_format, p.depth_samples, (p.lazy >> p.colours) & 1);
        if (!info.depth_stencil) {
            return nullptr;
        }
    }
    info.base_layer = p.base_layer;
    info.num_layers = p.layers;
    info.num_subpasses = p.subpasses;
    info.subpasses = p.subpasses ? p.subpass : nullptr;
    const RenderPass &rp = CommandBuffer::holyland_render_pass(gs->device, info);
    return rp.get_hash() == hash ? &rp : nullptr;
}

struct PwJob {
    DeferredPipelineCompile compile;
    bool compute;
};

void pw_run(std::vector<PwJob> jobs, unsigned threads) {
    auto check_start = std::chrono::steady_clock::now();
    // first those the pipeline cache has (made without compiling: at once), then the rest
    std::vector<size_t> missing;
    std::mutex missing_lock;
    std::atomic<size_t> next{0};
    auto pass = [&](bool build, const std::vector<size_t> &which) {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++) {
            pool.emplace_back([&, build] {
                Util::register_thread_index(0); // (no command buffers on these threads: only Granite's log reads it)
                for (;;) {
                    size_t i = next++;
                    if (i >= which.size() || pw.stop.load()) {
                        break;
                    }
                    PwJob &j = jobs[which[i]];
                    if (j.compile.program->get_pipeline(j.compile.hash).pipeline != VK_NULL_HANDLE) {
                        build ? pw.done++ : pw.ready++; // (a draw made it meanwhile)
                        continue;
                    }
                    auto mode = build ? CommandBuffer::CompileMode::AsyncThread : CommandBuffer::CompileMode::FailOnCompileRequired;
                    Pipeline p = j.compute ? CommandBuffer::build_compute_pipeline(gs->device, j.compile, mode)
                                           : CommandBuffer::build_graphics_pipeline(gs->device, j.compile, mode);
                    if (build) {
                        pw.done++;
                    } else if (p.pipeline != VK_NULL_HANDLE) {
                        pw.ready++;
                    } else {
                        std::lock_guard<std::mutex> hold(missing_lock);
                        missing.push_back(which[i]);
                    }
                }
            });
        }
        for (auto &t : pool) {
            t.join();
        }
    };
    std::vector<size_t> all(jobs.size());
    for (size_t i = 0; i < all.size(); i++) {
        all[i] = i;
    }
    pass(false, all);
    std::sort(missing.begin(), missing.end()); // (in the list's order: the title's first, then the maps')
    auto build_start = std::chrono::steady_clock::now();
    pw.seconds_check = std::chrono::duration<double>(build_start - check_start).count();
    pw.total = int(missing.size());
    pw.building = !missing.empty();
    if (!missing.empty()) {
        fprintf(stderr, "[gsn] building shaders: %zu of %zu pipelines are not in the pipeline cache\n", missing.size(),
                jobs.size());
        next = 0;
        pass(true, missing);
        pw.seconds_build = std::chrono::duration<double>(std::chrono::steady_clock::now() - build_start).count();
        fprintf(stderr, "[gsn] building shaders: %d made in %.1f s\n", pw.done.load(), pw.seconds_build);
    }
    pw.building = false;
    pw.running = false;
}

// (gsn_pipeline_cache, with the pipeline cache read) the lists read, and the start's work begun on threads of its own
void pw_start(const std::string &list_path) {
    std::vector<PwJob> jobs;
    {
        std::lock_guard<std::mutex> hold(pw.lock);
        pw_starting = true;
        struct Done {
            ~Done() { pw_starting = false; }
        } done;
        pw.path = list_path;
        for (const char *line : gsn_pipeline_list) {
            pw_read_line(line, true);
        }
        pw.saved = pw.unshipped = 0;
        if (FILE *f = fopen(list_path.c_str(), "rb")) {
            std::string text;
            char buffer[65536];
            size_t n;
            while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
                text.append(buffer, n);
            }
            fclose(f);
            size_t at = 0;
            while (at < text.size()) {
                size_t end = text.find('\n', at);
                if (end == std::string::npos) {
                    end = text.size();
                }
                std::string line = text.substr(at, end - at);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                pw_read_line(line, false);
                at = end + 1;
            }
        }
        pw.saved = pw.unshipped;
        const char *env = getenv("KF4_GSN_WARM"); // (0: nothing made ahead; the lists are still kept. set: the set alone)
        if (env && strcmp(env, "0") == 0) {
            return;
        }
        // this settings' pipelines, made again from their lines and checked by their hashes
        std::string tag = pw_tag();
        std::set<uint64_t> taken;
        std::vector<ImageHandle> images;
        VkPipelineCache cache = CommandBuffer::holyland_pipeline_cache(gs->device);
        for (const PwEntry &e : pw.entries) {
            if (ps_only() || e.tag != tag || !taken.insert(e.hash).second) {
                continue;
            }
            PwJob j;
            memset(&j.compile, 0, sizeof(j.compile));
            j.compute = e.compute;
            Program *program = nullptr;
            if (e.compute) {
                Shader *cs = pw_shader(e.shader[0]);
                program = cs ? gs->device->request_program(cs) : nullptr;
            } else {
                Shader *vs = pw_shader(e.shader[0]), *fs = pw_shader(e.shader[1]);
                program = vs && fs ? gs->device->request_program(vs, fs) : nullptr;
                j.compile.compatible_render_pass = program ? pw_render_pass(e.pass, images) : nullptr;
                if (!j.compile.compatible_render_pass) {
                    program = nullptr;
                }
            }
            if (!program) {
                pw.stale++;
                continue;
            }
            j.compile.program = program;
            j.compile.layout = program->get_pipeline_layout();
            j.compile.static_state = e.state;
            j.compile.potential_static_state = e.potential;
            memcpy(j.compile.attribs, e.attribs, sizeof(e.attribs));
            memcpy(j.compile.strides, e.strides, sizeof(e.strides));
            memcpy(j.compile.input_rates, e.rates, sizeof(e.rates));
            j.compile.subpass_index = e.subpass;
            j.compile.cache = cache;
            j.compile.subgroup_size_tag = e.subgroup;
            CommandBuffer::holyland_rehash(j.compile, e.compute);
            if (j.compile.hash != e.hash) {
                pw.stale++; // (Granite would not look it up so: another build's)
                continue;
            }
            jobs.push_back(j);
        }
        ps_jobs(jobs, taken, images); // (the set's not on the lists)
    }
    if (jobs.empty()) {
        return;
    }
    unsigned hw = std::thread::hardware_concurrency();
    unsigned threads = hw > 6 ? std::min(hw - 4, 8u) : 2;
    if (const char *t = getenv("KF4_GSN_WARM_THREADS")) {
        threads = std::max(1, atoi(t));
    }
    pw.running = true;
    std::thread(pw_run, std::move(jobs), threads).detach();
}
