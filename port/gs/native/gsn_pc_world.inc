// ---- The PC renderer's world (--renderer fast, docs/port/PC_RENDERER.md, P2) -------------------------------------------
// (included by gsn_standalone.inc after the skinned group, in gsn.cpp's namespace; the GS's thread)
// The scene's map records, objects, items and gold (program 0x000: opaque, alpha-tested, and blended where Vulkan's
// blending does the GS's; in the scene's frame, read by the GPU's sampler where gsn_rgba.inc made one) are kept as sa_draw_batch meets them and drawn by material when something else comes, as a
// skinned model's parts are (sa_group_draw). A material is one state (its texture, its corners' pushes, masks and
// scissor, its records): one draw for each material instead of one for each batch. KF4_PC_ORDER=0 (default): a batch
// joins the last material when its state is the same, the chain's order kept; 1: the first material of the same state.
// KF4_PC_TIES=0 (default): a material is drawn once, with its last colour mask, Z written, GEQUAL, no Z-first ties;
// 1: the batch's own draws, in order, as sa_draw_batch has them. The kept corners are never drawn before what came
// earlier in the chain: a batch the scene draws, a group's part, a picture between, a read of the frame and every other
// "something else comes" flush them first (sa_group_draw, sa_draw_batch).

volatile int pc_world_mode = 0; // --renderer fast (gsn_set_pc_world)

// A batch's state: what the draws of a material are bound and pushed with (memcmp-able: memset first)
struct PcKey {
    Push pushes[4];
    uint32_t masks[4], zwrites[4];
    int32_t ndraws;
    uint32_t z_written; // the batch's Z written (ZBUF's mask)
    uint32_t per_sample;
    int32_t scissor[4];
    uint32_t viewport[2];
    int32_t vs;            // its corners' vertex program (SA_VS_000, SA_VS_700: pc_corners_vs)
    const ImageView *rgba; // the batch's RGBA8 texture (gsn_rgba.inc), or none
    StockSampler sampler;
    const Buffer *records, *arena;
    // Vulkan's blending as the scene's (sa_draw_batch: blend_on with fixed_blend's factors), or none
    uint32_t blend_on, alpha_factor, fix;
    VkBlendFactor src, dst;
    VkBlendOp op;
};

// The frame target the kept batches draw into (sa_group's)
struct PcFrame {
    uint32_t fbp, fbw, fpsm, zbp, zpsm;
};

struct PcMaterial {
    PcKey key;
    std::vector<Vtx> corners;
    std::vector<uint32_t> runs; // where each of the scene's runs starts in corners, and corners' end (sa_draw_batch's runs)
    ImageHandle texture; // the RGBA8 image the key's view is of (held: nothing is freed before the flush)
    BufferHandle records; // the key's records, held likewise
};

struct PcWorld {
    std::vector<PcMaterial> materials;
    PcFrame frame = {};
    BufferHandle arena; // the key's arena (every material of a flush shares it)
    int order = -1, ties = -1; // (KF4_PC_ORDER, KF4_PC_TIES: read once)
    uint64_t batches_kept = 0, materials_drawn = 0, flushes = 0, draws = 0, flushes_for_target = 0;
} pc_world;

void pc_world_settings() {
    if (pc_world.order >= 0) {
        return;
    }
    const char *o = getenv("KF4_PC_ORDER"), *t = getenv("KF4_PC_TIES");
    pc_world.order = o && *o ? atoi(o) : 0;
    pc_world.ties = t && *t ? atoi(t) : 0;
}

// The draws the PC renderer takes: program 0x000's of the map's records, the map's objects, the items and the gold
// (KF4_PC_KINDS, a mask of RD_KIND_* bits, for tests). Program 0x700's effects can be taken too (KF4_PC_KINDS=0x9E, the
// effect's bit 7 with the others): measured at maps 000-017 they saved nothing (an effect has no Z-first draw to leave
// out, and is a material of its own), so they are not by default (PC_RENDERER.md, P6)
bool pc_world_takes(uint32_t kind, uint32_t program) {
    static int mask = -1;
    if (mask < 0) {
        const char *v = getenv("KF4_PC_KINDS");
        mask = v && *v ? int(strtol(v, nullptr, 0)) :
                         (1 << RD_KIND_MAP_RECORD) | (1 << RD_KIND_MAP_OBJECT) | (1 << RD_KIND_ITEM) | (1 << RD_KIND_GOLD);
    }
    return kind < 31 && ((mask >> kind) & 1) && (program == 0 || (program == 0x700 && kind == RD_KIND_EFFECT));
}

// The vertex program of a batch's corners when they are all of one the PC renderer draws: program 0x000's (SA_VS_000) or
// 0x700's effects (SA_VS_700); else -1 (a skinned model's, the PS2 path's, or a mix)
int pc_corners_vs(const std::vector<Vtx> &corners) {
    if (corners.empty() || nd_corner_kind(corners[0]) != ND_KIND_000) {
        return -1;
    }
    bool effect = nd700_corner(corners[0]);
    for (const Vtx &v : corners) {
        if (nd_corner_kind(v) != ND_KIND_000 || nd700_corner(v) != effect) {
            return -1;
        }
    }
    return effect ? SA_VS_700 : SA_VS_000;
}

bool pc_frame_equal(const PcFrame &a, const PcFrame &b) {
    return a.fbp == b.fbp && a.fbw == b.fbw && a.fpsm == b.fpsm && a.zbp == b.zbp && a.zpsm == b.zpsm;
}

// (sa_draw_batch's) A batch the PC renderer takes: its key, its corners and the buffers its records are in. A batch for
// another frame target or arena flushes what is kept first.
void pc_world_keep(const PcKey &k, const PcFrame &frame, const ImageHandle &texture, const BufferHandle &records,
                   const BufferHandle &arena, const std::vector<Vtx> &corners, const std::vector<uint32_t> &runs) {
    PcWorld &w = pc_world;
    pc_world_settings();
    bool same = pc_frame_equal(w.frame, frame) && w.arena.get() == arena.get();
    if (!w.materials.empty() && !same) {
        pc_world_flush();
    }
    w.frame = frame;
    w.arena = arena;
    PcMaterial *m = nullptr;
    if (w.order == 0) {
        if (!w.materials.empty() && memcmp(&w.materials.back().key, &k, sizeof(k)) == 0) {
            m = &w.materials.back();
        }
    } else {
        for (PcMaterial &a : w.materials) {
            if (memcmp(&a.key, &k, sizeof(k)) == 0) {
                m = &a;
                break;
            }
        }
    }
    if (!m) {
        w.materials.emplace_back();
        m = &w.materials.back();
        m->key = k;
        m->texture = texture;
        m->records = records;
    }
    // (runs: 0, the cuts, the batch's end; the material's own begin at 0 and end at its corners' end)
    uint32_t at = uint32_t(m->corners.size());
    if (m->runs.empty()) {
        m->runs.push_back(0);
    }
    for (size_t r = 1; r < runs.size(); r++) {
        m->runs.push_back(at + runs[r]);
    }
    m->corners.insert(m->corners.end(), corners.begin(), corners.end());
    w.batches_kept++;
}

// One material's draws: the batch's own draws run by run, as sa_draw_batch has them (KF4_PC_TIES=1), or one draw with the
// last colour mask
void pc_material_draw(Cmd &c, const PcMaterial &m, uint32_t first) {
    const PcKey &k = m.key;
    PcWorld &w = pc_world;
    uint32_t count = uint32_t(m.corners.size());
    c.set_texture(0, 0, gs->dummy_raw->get_view(), StockSampler::NearestClamp);
    c.set_texture(0, 1, gs->dummy_rgba->get_view(), StockSampler::NearestClamp);
    c.set_texture(0, 3, gs->dummy_rgba->get_view(), StockSampler::NearestClamp);
    if (k.rgba) {
        c.set_texture(0, 6, *k.rgba, k.sampler); // (gsn_rgba.inc: the GSN_HW_SAMPLER program)
    }
    static const uint32_t no_clut[256] = {};
    c.set_storage_data(0, 2, no_clut, 1024);
    c.set_depth_compare(VK_COMPARE_OP_GREATER_OR_EQUAL);
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_viewport({0.0f, 0.0f, float(k.viewport[0]), float(k.viewport[1]), 0.0f, 1.0f});
    c.set_scissor({{k.scissor[0], k.scissor[1]}, {uint32_t(k.scissor[2]), uint32_t(k.scissor[3])}});
    c.set_program(k.rgba ? sa.programs_hw[k.vs][k.per_sample ? 1 : 0] : sa.programs[k.vs][k.per_sample ? 1 : 0]);
    c.set_storage_buffer(2, 1, *m.records.get());
    if (k.blend_on) {
        float constants[4] = {float(k.fix) / 128.0f, float(k.fix) / 128.0f, float(k.fix) / 128.0f, 128.0f / 255.0f};
        c.set_blend_enable(true);
        c.set_blend_constants(constants);
        c.set_blend_factors(k.src, k.alpha_factor ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE, k.dst, VK_BLEND_FACTOR_ZERO);
        c.set_blend_op(k.op, VK_BLEND_OP_ADD);
    } else {
        c.set_blend_enable(false);
    }
    if (pc_world.ties == 1) {
        // run by run, each run's draws (its Z first, then its colour within the slack), as sa_draw_batch draws them
        for (size_t r = 0; r + 1 < m.runs.size(); r++) {
            for (int i = 0; i < k.ndraws; i++) {
                c.set_color_write_mask(k.masks[i]);
                c.set_depth_test(true, k.zwrites[i] != 0);
                c.push_constants(&k.pushes[i], 0, sizeof(Push));
                c.draw(m.runs[r + 1] - m.runs[r], first + m.runs[r]);
                sa_stats.draws++;
                gsn_stats.draws++;
                w.draws++;
            }
        }
        return;
    }
    // the last draw with a colour mask (or the last draw): its push; Z written by the batch. An opaque batch exact, without
    // its layer's bias. A blended one keeps the bias: a layer passes over the surface it lies on, and writes its Z biased
    // so (the GS writes its own). Measured (PC_RENDERER.md, P2's layers): the bias written costs a line a pixel wide at
    // the edges of cut-outs in front of what is drawn after them (the braziers' legs against the fire's glow); its Z
    // exact instead lost a layer that lies a unit off its surface (map 000's rock over the tower's top), and the scene's
    // way (its Z first, then its colour within the slack, run by run) was a draw a triangle, slower than P1
    int pick = k.ndraws - 1;
    for (int i = 0; i < k.ndraws; i++) {
        if (k.masks[i]) {
            pick = i;
        }
    }
    Push p = k.pushes[pick];
    if (!k.blend_on) {
        p.zbias = 0.0f;
        p.zslope = 0.0f;
    }
    c.set_color_write_mask(k.masks[pick]);
    c.set_depth_test(true, k.z_written != 0);
    c.push_constants(&p, 0, sizeof(Push));
    c.draw(count, first);
    sa_stats.draws++;
    gsn_stats.draws++;
    w.draws++;
}

// What is kept drawn into the scene: by material, in the order they were made (sa_group_draw's pass)
void pc_world_flush() {
    PcWorld &w = pc_world;
    if (w.materials.empty()) {
        return;
    }
    size_t total = 0;
    for (const PcMaterial &m : w.materials) {
        total += m.corners.size();
    }
    if (sa.bound) {
        sa_begin_pass(false);
        gpu_enter(GPU_SA_DRAW);
        Cmd &c = cmd();
        c.set_opaque_state();
        Vtx *v = static_cast<Vtx *>(c.allocate_vertex_data(0, total * sizeof(Vtx), sizeof(Vtx)));
        size_t at = 0;
        for (const PcMaterial &m : w.materials) {
            memcpy(v + at, m.corners.data(), m.corners.size() * sizeof(Vtx));
            at += m.corners.size();
        }
        c.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vtx, x));
        c.set_vertex_attrib(1, 0, VK_FORMAT_R32_UINT, offsetof(Vtx, z));
        c.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vtx, rgba));
        c.set_vertex_attrib(3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, s));
        c.set_vertex_attrib(4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vtx, fog));
        c.set_storage_buffer(2, 0, *w.arena.get()); // (nd_bind's, for the arena the batches were made with)
        if (!interp.active && sa_jitter_frame_picture()) {
            c.set_storage_buffer(1, 0, *sa_jitter_frame_picture()); // (the frame's jitter: sa_jitter_frame)
        } else if (!interp.active) {
            if (!nd.dummy) {
                BufferCreateInfo info = {};
                info.domain = BufferDomain::Device;
                info.size = 256;
                info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                std::vector<uint8_t> zeros(256);
                nd.dummy = gs->device->create_buffer(info, zeros.data());
            }
            c.set_storage_buffer(1, 0, *nd.dummy);
        }
        size_t first = 0;
        for (const PcMaterial &m : w.materials) {
            pc_material_draw(c, m, uint32_t(first));
            first += m.corners.size();
        }
        w.flushes++;
        w.materials_drawn += w.materials.size();
    }
    w.materials.clear();
    w.arena.reset();
}

// (sa_draw_batch's, before it binds its frame) What is kept drawn first when the batch is for another frame target: the
// kept materials draw into the target bound when they are flushed, which must be theirs
void pc_world_flush_for(uint32_t fbp, uint32_t fbw, uint32_t fpsm, uint32_t zbp, uint32_t zpsm) {
    PcWorld &w = pc_world;
    PcFrame f = {fbp, fbw, fpsm, zbp, zpsm};
    if (!w.materials.empty() && !pc_frame_equal(w.frame, f)) {
        w.flushes_for_target++;
        pc_world_flush();
    }
}

void pc_world_print_stats() {
    const PcWorld &w = pc_world;
    fprintf(stderr, "[stats] pc world: batches kept %llu, materials %llu, flushes %llu (%llu for another target's batch), "
            "draws %llu (order %d, ties %d)\n",
            (unsigned long long)w.batches_kept, (unsigned long long)w.materials_drawn, (unsigned long long)w.flushes,
            (unsigned long long)w.flushes_for_target, (unsigned long long)w.draws, w.order < 0 ? 0 : w.order, w.ties < 0 ? 0 : w.ties);
}
