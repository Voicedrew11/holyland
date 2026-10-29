// ---- The pipeline set (included into gsn.cpp after gsn_pipeline_warm.inc; docs/port/PC_RENDERER.md, smooth play step 1)
// Every pipeline the native GS's draws can ask for at the settings in use, worked out from the code instead of found
// by playing (the lists of gsn_pipeline_warm.inc have only what a tour happened to draw, at the settings it ran with).
//
// With Vulkan's dynamic state (dynamic_state_choice: depth, stencil, culling, topology within its class, blending and
// the colour write mask) Granite tells a pipeline apart only by its program, the render pass it draws in (the
// attachments' formats and the subpass), its vertex layout and a few bits of state (conservative rasterization). So
// each place that draws is an entry of ps_sites[], which names the pipelines it can ask for; the start makes them all
// on the warm-up's threads (pw_run), with those of the lists, before play.
//
// The samples are not in Granite's hash of a render pass (msaa_subpass), so a pipeline is found by the hash of one
// made at other samples: a site names its passes with the samples its draws will have, from the same functions the
// draws take them from (sa_samples_for, sa_depth_format_wanted, colour_format_of), at the settings wanted (the
// targets take them at a vblank, after the start).
//
// A pipeline a draw makes in play that is not in the set is a place missing from the table, or a pass named wrong:
// --stats counts them, and KF4_GSN_WARM=set (the set alone made ahead, not the lists) logs each one with its shaders
// and pass. Without blending dynamic (a device without VK_EXT_extended_dynamic_state3's) the blend state is in the
// hash and the set is not worked out: the lists alone, as before.

// The subpass of a pass: none (one sample), msaa_subpass's, scratch_subpass's (shader blending in the pass), or
// resolve_subpass's (the multisampled colour resolved into a copy as the pass ends)
enum class PsSubpass { none, msaa, scratch, resolve };

// A render pass as a draw begins it: its colour attachments (a scratch's or a resolve's second is the first's
// format; a resolve's of one sample), its depth (VK_FORMAT_UNDEFINED: none), the samples of the others, and its
// subpass
struct PsPass {
    VkFormat colour = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    uint32_t samples = 1;
    PsSubpass subpass = PsSubpass::none;
};

// The vertex layouts the native GS draws with: none (a full-screen pass), gsn.cpp's Vtx, or native draw's Corner
// (the 0x280 and 0x700 programs: rgba and the vertex's index)
enum class PsVertices { none, vtx, corner };

// One pipeline a draw or dispatch can ask for (a compute one: its program alone)
struct PsPipeline {
    Program *program = nullptr;
    PsPass pass;
    PsVertices vertices = PsVertices::none;
    bool conservative = false;
    bool compute = false;
};

// A place that draws: the pipelines it can ask for at the settings in use, added to out (null programs: none made at
// these settings, left out)
struct PsSite {
    const char *name;
    void (*list)(std::vector<PsPipeline> &out);
};

struct PipelineSet {
    std::unordered_set<uint64_t> hashes;     // the set's pipelines, by Granite's hash
    std::atomic<uint64_t> outside{0};        // made by draws in play, not in the set
    std::atomic<uint64_t> inside_late{0};    // made by draws in play, in the set but not built yet
    size_t on_lists = 0;                     // the set's pipelines that a list has too (the set's hash checked so)
    std::string by_site;                     // (--stats) each site's pipelines new to the set
} ps;

// KF4_GSN_WARM=set: the set alone made ahead, not the lists' (what the set misses, made by draws in play and logged)
bool ps_only() {
    static const bool only = [] {
        const char *env = getenv("KF4_GSN_WARM");
        return env && strcmp(env, "set") == 0;
    }();
    return only;
}

// ---- The sites ------------------------------------------------------------------------------------------------------

// A pass of colour and depth (either VK_FORMAT_UNDEFINED: none) at samples, plain or with the scratch
PsPass ps_pass(VkFormat colour, VkFormat depth, uint32_t samples, bool scratch) {
    PsPass p;
    p.colour = colour;
    p.depth = depth;
    p.samples = samples;
    p.subpass = scratch ? PsSubpass::scratch : samples > 1 ? PsSubpass::msaa : PsSubpass::none;
    return p;
}

// The scene's passes (sa_begin_pass): its colour and depth at samples, plain or with the scratch
PsPass ps_scene_pass(uint32_t samples, bool scratch) {
    return ps_pass(colour_format_of(wanted_colour), sa_depth_format_wanted(), samples, scratch);
}

// A GS target's passes (begin_pass): its colour, with its Z buffer or not, at the targets' samples or one
PsPass ps_target_pass(uint32_t samples, bool depth, bool scratch) {
    return ps_pass(colour_format_of(wanted_colour), depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED, samples, scratch);
}

// The same multisampled, resolved into a copy for the next run of a draw that blends in the shader (COPY_BY_PASS)
PsPass ps_target_resolve_pass(uint32_t samples, bool depth) {
    PsPass p = ps_target_pass(samples, depth, false);
    p.subpass = PsSubpass::resolve;
    return p;
}

// The sample counts targets are drawn at: the targets' (MSAA), and one (the targets' own images)
std::vector<uint32_t> ps_target_samples() {
    std::vector<uint32_t> counts = {1};
    if (wanted_samples > 1) {
        counts.push_back(uint32_t(wanted_samples));
    }
    return counts;
}

// The sample counts the scene is drawn at: its own (sa_samples_for), and one (a named target's: sat_acquire)
std::vector<uint32_t> ps_scene_samples() {
    uint32_t scene = sa_samples_for(wanted_samples);
    std::vector<uint32_t> counts = {scene};
    if (scene > 1) {
        counts.push_back(1);
    }
    return counts;
}

void ps_add(std::vector<PsPipeline> &out, Program *program, const PsPass &pass, PsVertices vertices,
            bool conservative = false) {
    if (!program) {
        return;
    }
    PsPipeline p;
    p.program = program;
    p.pass = pass;
    p.vertices = vertices;
    p.conservative = conservative;
    out.push_back(p);
}

// The scene's draws (--renderer modern and fast): its batches (sa_draw_batch, sa_draw_range), the skinned groups
// (sa_group_*), the PC world's materials (pc_material_draw) and the ties' Z-first draws, in the scene's pass at its
// samples and at one (a named target's: sat_acquire), with the scratch or not (a draw keeps the pass under way); and
// shader blending in the pass, its runs' copies (conservative, by their own triangles) and their draws reading the
// scratch, in the pass with it
void ps_scene_draws(std::vector<PsPipeline> &out) {
    if (!sa_mode) {
        return;
    }
    sa_programs(); // (at the start, before the GS's thread draws: gsn_pipeline_cache)
    for (uint32_t samples : ps_scene_samples()) {
        int ms = samples > 1 ? 1 : 0;
        for (bool scratch : {false, true}) {
            PsPass pass = ps_scene_pass(samples, scratch);
            for (int k = 0; k < 3; k++) {
                for (int shading = 0; shading < 2; shading++) {
                    ps_add(out, sa.programs[k][shading], pass, PsVertices::vtx);
                    ps_add(out, sa.programs_hw[k][shading], pass, PsVertices::vtx);
                }
                ps_add(out, sa.programs_z[k], pass, PsVertices::vtx);
                if (scratch) {
                    ps_add(out, sa.programs_in[k][ms], pass, PsVertices::vtx);
                    ps_add(out, sa.programs_in_hw[k][ms], pass, PsVertices::vtx);
                    ps_add(out, sa.programs_copy[k][ms], pass, PsVertices::vtx, true);
                }
            }
        }
    }
}

// The GS's batches in a GS target's pass (flush_batch): with the target's Z buffer, Z clamped (zclamp: a batch with
// one), Z only (fine_z's first draws), cut-outs shaded at every sample (multisampled); without it, the plain program;
// shader blending from the scratch, in a pass with it; and multisampled, in a pass resolving the copy the next run
// blends from (COPY_BY_PASS). Native draw's corners in them (nd_draw_range): the screen
// passes and the weapon's trail (2D: nd_pass.vert, nd_strip.vert) over the plain and blending programs; and without
// the scene (--renderer native), the models' own programs (0x000, 0x280, 0x700, the water) over those with Z
void ps_gs_batches(std::vector<PsPipeline> &out) {
    for (uint32_t samples : ps_target_samples()) {
        bool ms = samples > 1;
        for (int kind = 0; kind < (ms ? 3 : 2); kind++) {
            bool scratch = kind == 1, resolve = kind == 2;
            PsPass flat = resolve ? ps_target_resolve_pass(samples, false) : ps_target_pass(samples, false, scratch);
            std::vector<Program *> plain = {gs->program};
            if (scratch) {
                plain.push_back(ms ? gs->program_in_ms : gs->program_in);
            }
            for (Program *base : plain) {
                ps_add(out, base, flat, PsVertices::vtx);
                ps_add(out, base ? nd_pass_program(base) : nullptr, flat, PsVertices::vtx);
                ps_add(out, base ? nd_strip_program(base) : nullptr, flat, PsVertices::vtx);
            }
            PsPass z = resolve ? ps_target_resolve_pass(samples, true) : ps_target_pass(samples, true, scratch);
            std::vector<Program *> with_z = {gs->program_zclamp, gs->program_zonly};
            if (ms) {
                with_z.insert(with_z.end(), {gs->program_zclamp_sample, gs->program_zonly_sample});
            }
            if (scratch) {
                with_z.push_back(ms ? gs->program_zclamp_in_ms : gs->program_zclamp_in);
                if (ms) {
                    with_z.push_back(gs->program_zclamp_sample_in_ms);
                }
            }
            for (Program *base : with_z) {
                ps_add(out, base, z, PsVertices::vtx);
                if (base && !sa_mode) {
                    ps_add(out, nd_program_of(base), z, PsVertices::vtx);
                    ps_add(out, nd280_program(base), z, PsVertices::vtx);
                    ps_add(out, nd700_program(base), z, PsVertices::vtx);
                    ps_add(out, ndw_program(base), z, PsVertices::vtx);
                }
            }
        }
    }
}

// The GS's own draws in the scene's pass (sa_draw_range's PS2 corners and the water's): the plain program, or as
// native draw's screen passes and trail have it; with Z clamped (the scene's 3D the GS's way, sa.gs3d), shaded at
// every sample or not; the water's corners with Z clamped (sa.gs_water) and the reflection's water (sat_water_program)
void ps_gs_in_scene(std::vector<PsPipeline> &out) {
    if (!sa_mode) {
        return;
    }
    sa_programs();
    for (uint32_t samples : ps_scene_samples()) {
        for (bool scratch : {false, true}) {
            PsPass pass = ps_scene_pass(samples, scratch);
            ps_add(out, gs->program, pass, PsVertices::vtx);
            ps_add(out, nd_pass_program(gs->program), pass, PsVertices::vtx);
            ps_add(out, nd_strip_program(gs->program), pass, PsVertices::vtx);
            for (Program *base : {gs->program_zclamp, gs->program_zclamp_sample}) {
                ps_add(out, base, pass, PsVertices::vtx);
                ps_add(out, base ? ndw_program(base) : nullptr, pass, PsVertices::vtx);
            }
            ps_add(out, sat_water_program(false), pass, PsVertices::vtx);
            ps_add(out, sat_water_program(true), pass, PsVertices::vtx);
        }
    }
}

// The full-screen copies (shaders/msaa.vert): a target's picture and Z into its multisampled images and back
// (msaa_copy_pass), the copy a run blends from made in the pass (blend_copy_in_pass); and the scene's (sa_copy_pass):
// its colour and Z taken from the GS's targets and given back, resampled at another scale (sa_resample_pass)
void ps_copies(std::vector<PsPipeline> &out) {
    VkFormat colour = colour_format_of(wanted_colour);
    for (uint32_t samples : ps_target_samples()) {
        bool ms = samples > 1;
        if (ms) {
            ps_add(out, gs->program_ms_load, ps_pass(colour, VK_FORMAT_UNDEFINED, samples, false), PsVertices::none);
            ps_add(out, gs->program_ms_load_depth, ps_pass(VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, samples, false),
                   PsVertices::none);
        } else {
            ps_add(out, gs->program_ms_depth, ps_pass(VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT, 1, false), PsVertices::none);
        }
        for (bool depth : {false, true}) {
            ps_add(out, ms ? gs->program_pass_resolve : gs->program_pass_copy, ps_target_pass(samples, depth, true),
                   PsVertices::none);
        }
    }
    if (!sa_mode) {
        return;
    }
    sa_programs();
    for (uint32_t samples : ps_scene_samples()) {
        int ms = samples > 1 ? 1 : 0;
        for (VkFormat depth : {VK_FORMAT_D32_SFLOAT, sa_depth_format_wanted()}) {
            PsPass z = ps_pass(VK_FORMAT_UNDEFINED, depth, samples, false);
            ps_add(out, ms ? sa.program_depth_ms : sa.program_depth, z, PsVertices::none);
            for (int src_ms = 0; src_ms < 2; src_ms++) {
                ps_add(out, sa.program_resample[1][src_ms], z, PsVertices::none);
            }
        }
        PsPass c = ps_pass(colour, VK_FORMAT_UNDEFINED, samples, false);
        if (ms) {
            ps_add(out, sa.program_copy_ms, c, PsVertices::none);
        }
        for (int src_ms = 0; src_ms < 2; src_ms++) {
            ps_add(out, sa.program_resample[0][src_ms], c, PsVertices::none);
        }
    }
}

// The picture shown: the scanout's lines from a target (narrow_pass), the movies' fields woven (deinterlace), and
// the picture scaled to the window and sharpened (present_scale), into the scanout's 8-bit images
void ps_scanout(std::vector<PsPipeline> &out) {
    PsPass pass = ps_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, 1, false);
    for (Program *program : {gs->program_narrow, gs->program_deinterlace, gs->program_present_scale,
                             gs->program_present_sharpen}) {
        ps_add(out, program, pass, PsVertices::none);
    }
}

// The pictures between drawn from the recording: the replay's compute programs (interp_replay)
void ps_replay(std::vector<PsPipeline> &out) {
    for (Program *program : {gs->program_replay_kicks, gs->program_replay_corners}) {
        if (program) {
            PsPipeline p;
            p.program = program;
            p.compute = true;
            out.push_back(p);
        }
    }
}

// (in the order they are made at the start: what the title and the menus draw first, the world after)
const PsSite ps_sites[] = {
    {"the scanout", ps_scanout},
    {"the full-screen copies", ps_copies},
    {"the GS's batches", ps_gs_batches},
    {"the replay", ps_replay},
    {"the GS's draws in the scene", ps_gs_in_scene},
    {"the scene's draws", ps_scene_draws},
};

// ---- The set made -------------------------------------------------------------------------------------------------

// A compatible render pass made from a pass's shape (images of a pixel, held in images until the set's jobs are
// made), with the subpass the draws' passes have (msaa_subpass, scratch_subpass)
const RenderPass *ps_render_pass(const PsPass &p, std::vector<ImageHandle> &images) {
    auto make = [&](VkFormat format, uint32_t samples) -> const ImageView * {
        ImageCreateInfo info = ImageCreateInfo::render_target(1, 1, format);
        info.samples = VkSampleCountFlagBits(samples);
        info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!format_has_depth_or_stencil_aspect(format)) {
            info.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        }
        ImageHandle image = gs->device->create_image(info);
        if (!image) {
            return nullptr;
        }
        images.push_back(image);
        return &image->get_view();
    };
    RenderPassInfo rp;
    bool two = p.subpass == PsSubpass::scratch || p.subpass == PsSubpass::resolve;
    rp.num_color_attachments = p.colour != VK_FORMAT_UNDEFINED ? (two ? 2 : 1) : 0;
    for (uint32_t i = 0; i < rp.num_color_attachments; i++) {
        uint32_t samples = p.subpass == PsSubpass::resolve && i == 1 ? 1 : p.samples;
        if (!(rp.color_attachments[i] = make(p.colour, samples))) {
            return nullptr;
        }
    }
    if (p.depth != VK_FORMAT_UNDEFINED && !(rp.depth_stencil = make(p.depth, p.samples))) {
        return nullptr;
    }
    RenderPassInfo::Subpass subpass;
    if (p.subpass == PsSubpass::scratch) {
        scratch_subpass(rp, subpass, p.samples > 1);
    } else if (p.subpass == PsSubpass::resolve) {
        resolve_subpass(rp, subpass);
    } else if (p.subpass == PsSubpass::msaa) {
        msaa_subpass(rp, subpass);
    }
    return &CommandBuffer::holyland_render_pass(gs->device, rp);
}

// What Granite makes a pipeline from, as a Cmd's draw would have it: Granite's opaque state with the dynamic state's
// bits, a triangle list (the dynamic topology's class), and the vertex layout (a dispatch's: its program alone)
DeferredPipelineCompile ps_compile(const PsPipeline &p, const RenderPass *pass) {
    DeferredPipelineCompile c;
    memset(&c, 0, sizeof(c));
    c.program = p.program;
    c.layout = p.program->get_pipeline_layout();
    c.compatible_render_pass = pass;
    c.subpass_index = 0;
    c.cache = CommandBuffer::holyland_pipeline_cache(gs->device);
    const auto &features = gs->device->get_device_features();
    c.subgroup_size_tag = features.vk13_props.minSubgroupSize | features.vk13_props.maxSubgroupSize << 8;
    auto &s = c.static_state.state;
    DynamicState d = dynamic_state_choice();
    s.dynamic_ds = d.ds;
    s.dynamic_blend = d.blend;
    if (p.compute) {
        CommandBuffer::holyland_rehash(c, true);
        return c;
    }
    s.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    s.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    s.write_mask = ~0u;
    s.conservative_raster = p.conservative;
    auto attrib = [&](uint32_t i, VkFormat format, uint32_t offset) { c.attribs[i] = {0, format, offset}; };
    if (p.vertices == PsVertices::vtx) {
        attrib(0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vtx, x));
        attrib(1, VK_FORMAT_R32_UINT, offsetof(Vtx, z));
        attrib(2, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vtx, rgba));
        attrib(3, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, s));
        attrib(4, VK_FORMAT_R32_SFLOAT, offsetof(Vtx, fog));
        c.strides[0] = sizeof(Vtx);
    } else if (p.vertices == PsVertices::corner) {
        attrib(2, VK_FORMAT_R8G8B8A8_UINT, 0);
        attrib(4, VK_FORMAT_R32_SFLOAT, sizeof(uint32_t));
        c.strides[0] = 2 * sizeof(uint32_t);
    }
    c.input_rates[0] = VK_VERTEX_INPUT_RATE_VERTEX;
    CommandBuffer::holyland_rehash(c, false);
    return c;
}

// (pw_start, pw.lock held) the set's jobs for the settings in use, those whose hashes are not in taken (the lists'),
// added to jobs; the passes' images held in images
void ps_jobs(std::vector<PwJob> &jobs, std::set<uint64_t> &taken, std::vector<ImageHandle> &images) {
    DynamicState d = dynamic_state_choice();
    if (!d.ds || !d.blend) {
        return;
    }
    std::set<uint64_t> listed; // (the lists' at these settings)
    std::string tag = pw_tag();
    for (const PwEntry &e : pw.entries) {
        if (e.tag == tag) {
            listed.insert(e.hash);
        }
    }
    for (const PsSite &site : ps_sites) {
        std::vector<PsPipeline> pipelines;
        site.list(pipelines);
        size_t before = ps.hashes.size();
        for (const PsPipeline &p : pipelines) {
            const RenderPass *pass = p.compute ? nullptr : ps_render_pass(p.pass, images);
            if (!pass && !p.compute) {
                continue;
            }
            PwJob j;
            j.compile = ps_compile(p, pass);
            j.compute = p.compute;
            if (!ps.hashes.insert(j.compile.hash).second) {
                continue;
            }
            ps.on_lists += listed.count(j.compile.hash);
            if (taken.insert(j.compile.hash).second) {
                jobs.push_back(j);
            }
        }
        ps.by_site += (ps.by_site.empty() ? "" : ", ") + std::string(site.name) + " " +
                      std::to_string(ps.hashes.size() - before);
    }
}

// The name of a shader of the program's (by its SPIR-V's hash), for the log of pipelines outside the set
const char *ps_shader_name(uint64_t hash) {
    static std::unordered_map<uint64_t, const char *> names;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const auto &spv : gsn_spv_all) {
            names.emplace(Shader::hash(spv.code, spv.size), spv.name);
        }
    });
    auto it = names.find(hash);
    return it != names.end() ? it->second : "?";
}

// (pw_on_compile) a pipeline a draw made in play: counted against the set, and with KF4_GSN_WARM=set logged when
// the set does not have it (its shaders and its pass's attachments: formats and samples)
void ps_on_play_compile(const DeferredPipelineCompile &c, bool compute, int64_t ns) {
    if (ps.hashes.empty()) {
        return;
    }
    if (ps.hashes.count(c.hash)) {
        ps.inside_late++;
        return;
    }
    ps.outside++;
    if (!ps_only()) {
        return;
    }
    if (compute) {
        const Shader *cs = c.program->get_shader(ShaderStage::Compute);
        fprintf(stderr, "[gsn] a pipeline outside the set was made in play (%.1f ms): compute %s\n", double(ns) / 1e6,
                cs ? ps_shader_name(cs->get_hash()) : "-");
        return;
    }
    const Shader *vs = c.program->get_shader(ShaderStage::Vertex), *fs = c.program->get_shader(ShaderStage::Fragment);
    std::string pass = "?";
    if (c.compatible_render_pass) {
        std::lock_guard<std::mutex> hold(pw.lock);
        auto it = pw.passes.find(c.compatible_render_pass->get_hash());
        if (it != pw.passes.end()) {
            const PwPass &p = it->second;
            char text[160];
            // (formats from the pass of this hash first seen; its samples, which the hash leaves out, from this one)
            int n = snprintf(text, sizeof(text), "colour");
            for (uint32_t i = 0; i < p.colours && n < int(sizeof(text)); i++) {
                n += snprintf(text + n, sizeof(text) - n, " %u", p.colour_format[i]);
            }
            snprintf(text + n, sizeof(text) - n, ", depth %u, %u subpasses; %u samples", p.depth_format, p.subpasses,
                     c.compatible_render_pass->get_sample_count(c.subpass_index));
            pass = text;
        }
    }
    fprintf(stderr, "[gsn] a pipeline outside the set was made in play (%.1f ms): %s + %s, %s%s\n", double(ns) / 1e6,
            vs ? ps_shader_name(vs->get_hash()) : "-", fs ? ps_shader_name(fs->get_hash()) : "-", pass.c_str(),
            c.static_state.state.conservative_raster ? ", conservative" : "");
}
