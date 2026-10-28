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

// ---- P5: a taken draw's kept batches drawn again in its frame's pictures between ------------------------------------
// A picture between walks the frame's packets again (rc/own: nd_prep_*), and a draw the PC world took was worked out
// again there: its corners copied (nd_prep_corners), its batch's state, texture and blending (sa_draw_batch), to come
// to the frame's key. So the frame keeps, for each draw the PC world took whose every batch it kept (by the draw's place
// among the frame's native draws, nd_prep's), what pc_world_keep was given: the key, the texture, the corners as drawn
// (the triangles' slopes in their alpha), and the triangles it left out, outside the scissor. A picture's draw at that
// place with the same value makes its record (nd_draw: the picture's moves) and gives the frame's batches to
// pc_world_keep with that record in their corners, into the picture's own FRAME and ZBUF (the buffer shown); its plans
// and batches are not worked out. A triangle the frame left out is drawn where sa_draw_batch would keep it in a
// picture (in view at t 0), in its place among the batch's: the frame keeps every triangle of a kept batch, in order,
// its slope worked out (sa_slope). A batch the frame drew nothing of has no key: when one of its triangles is in view
// at t 0, the draw is worked out as before.
// A draw's batches are flushed at its end with --renderer fast (sa_draw_end), so that each kept batch is one draw's.
// KF4_PC_BETWEEN=0 (tests): worked out as before.

void sa_note_first_z(bool first);                              // (gsn_standalone.inc)
uint32_t sa_slope(const Vtx *v, bool world);
void nd_box_at_0_of(const Vtx *v, uint32_t count, int32_t *box); // (gsn_native_draw.inc)

// One batch of a taken draw, as the frame's sa_draw_batch ended it: kept (pc_world_keep's), or nothing drawn
struct PcKept {
    PcKey key;
    uint32_t ctxt;          // (the context of its FRAME and ZBUF: a picture draws into its own)
    uint32_t acquire_rows;  // sa_acquire's rows (the scissor's last row and one); 0: the batch ended before it
    uint32_t row0, row1;    // the scissor's rows within the target (a picture's rows: sa_draw_batch's d0, d1)
    uint32_t z_written;
    ImageHandle texture;
    BufferHandle arena;
    bool drawn;             // kept (a key); else nothing drawn (every triangle outside the scissor, or none at all)
    uint32_t at, n;         // its corners, every triangle in order (as drawn: slopes in their alpha), in PcFrameKept::corners
    uint32_t out_at, out_n; // the triangles left out (outside the scissor), in PcFrameKept::outs
    float cull[4];          // the scissor a pixel wider each way, as the triangles were tested
};
struct PcDrawKept {
    uint32_t first = 0, count = 0; // its batches in PcFrameKept::kept
    uint32_t met = 0;               // the batches sa_draw_batch met (a draw whose every one ended in kept is whole)
    uint32_t record = 0;            // the draw's record (the corners' record field)
    bool whole = false;
};
struct PcFrameKept {
    uint32_t serial = 0xFFFFFFFFu;
    std::vector<PcDrawKept> draws;
    std::vector<PcKept> kept;
    std::vector<Vtx> corners;
    std::vector<uint32_t> outs;
    void clear() {
        serial = 0xFFFFFFFFu;
        draws.clear();
        kept.clear();
        corners.clear();
        outs.clear();
    }
};
struct PcBetween {
    int enabled = -1;
    PcFrameKept frames[2];           // (as nd_prep.frames: the same index)
    std::vector<Vtx> batch;          // (the frame's) the batch's corners as sa_draw_batch met them
    bool batch_on = false;           // and the batch is a taken draw's, to be kept
    const PcDrawKept *taking = nullptr; // (a picture's) the draw being drawn from the frame's kept batches
    const PcFrameKept *taking_frame = nullptr;
    std::vector<Vtx> corners;        // (a picture's) a batch's corners with the picture's record
    uint64_t draws_taken = 0, batches_taken = 0, corners_taken = 0, batches_out = 0, left_out = 0;
    uint64_t not_whole = 0, left_out_in_view = 0, no_frame = 0, not_acquired = 0;
} pc_between;

bool pc_between_on() {
    if (pc_between.enabled < 0) {
        const char *v = getenv("KF4_PC_BETWEEN");
        pc_between.enabled = v && *v ? atoi(v) != 0 : 1;
    }
    return pc_between.enabled != 0 && pc_world_mode;
}

// (sa_draw_end) Whether a taken draw's batches end with it: while frames are kept for their pictures between (nd_prep's),
// in the frame and in its pictures alike
bool pc_between_cuts() {
    return pc_between_on() && (interp.active ? nd_prep.taking != nullptr : nd_prep.filling != nullptr);
}

// (the frame's) The frame being kept, or null (nd_prep's: none at 30)
PcFrameKept *pc_kept_frame() {
    if (!pc_between_on() || interp.active || !nd_prep.filling || nd_prep.draw < 0) {
        return nullptr;
    }
    PcFrameKept &f = pc_between.frames[nd_prep.filling - nd_prep.frames];
    if (f.serial != nd_prep.filling->serial) {
        f.clear();
        f.serial = nd_prep.filling->serial;
    }
    if (f.draws.size() <= size_t(nd_prep.draw)) {
        f.draws.resize(size_t(nd_prep.draw) + 1);
    }
    return &f;
}

// (nd_draw, the frame's) a 0x000 draw's record
void pc_kept_record(uint32_t record) {
    if (PcFrameKept *f = pc_kept_frame()) {
        f->draws[size_t(nd_prep.draw)].record = record;
    }
}

// (sa_draw_batch's start, a taken draw's batch in the frame) its corners, all of them
void pc_kept_met() {
    pc_between.batch_on = false;
    PcFrameKept *f = pc_kept_frame();
    if (!f) {
        return;
    }
    f->draws[size_t(nd_prep.draw)].met++;
    pc_between.batch = gs->verts;
    pc_between.batch_on = true;
}

// The batch met (pc_kept_met), ended in sa_draw_batch: its entry, with every triangle of the batch met in order and
// those left out (outside cull, as sa_draw_batch tests them). drawn: the triangles kept as drawn (their slopes in their
// alpha), the others' worked out likewise (slopes: sa_draw_batch's, 0 none, 1, 2 by the world); null: nothing drawn
PcKept *pc_kept_end(const std::vector<Vtx> *drawn, int slopes, const float *cull) {
    PcFrameKept *f = pc_between.batch_on ? pc_kept_frame() : nullptr;
    pc_between.batch_on = false;
    if (!f) {
        return nullptr;
    }
    PcDrawKept &d = f->draws[size_t(nd_prep.draw)];
    const std::vector<Vtx> &v = pc_between.batch;
    size_t outs = 0;
    for (size_t i = 0; cull && i + 2 < v.size(); i += 3) {
        float tx0 = std::min(v[i].x, std::min(v[i + 1].x, v[i + 2].x)), tx1 = std::max(v[i].x, std::max(v[i + 1].x, v[i + 2].x));
        float ty0 = std::min(v[i].y, std::min(v[i + 1].y, v[i + 2].y)), ty1 = std::max(v[i].y, std::max(v[i + 1].y, v[i + 2].y));
        outs += tx1 < cull[0] || tx0 > cull[2] || ty1 < cull[1] || ty0 > cull[3];
    }
    if ((d.count != 0 && d.first + d.count != f->kept.size()) || (drawn && drawn->size() + outs * 3 != v.size())) {
        return nullptr; // (never: a draw's batches end one after another, its corners the batch met's less those left out)
    }
    if (d.count == 0) {
        d.first = uint32_t(f->kept.size());
    }
    f->kept.emplace_back();
    d.count++;
    PcKept &kp = f->kept.back();
    memset(&kp.key, 0, sizeof(kp.key));
    kp.ctxt = kp.acquire_rows = kp.row0 = kp.row1 = kp.z_written = 0;
    kp.drawn = drawn != nullptr;
    kp.at = uint32_t(f->corners.size());
    kp.n = drawn || cull ? uint32_t(v.size()) / 3 * 3 : 0; // (none drawn at all: no corners)
    kp.out_at = uint32_t(f->outs.size());
    memset(kp.cull, 0, sizeof(kp.cull));
    if (cull) {
        memcpy(kp.cull, cull, sizeof(kp.cull));
    }
    size_t next = 0; // (drawn's next triangle)
    for (size_t i = 0; i + 2 < kp.n; i += 3) {
        bool out = false;
        if (cull) {
            float tx0 = std::min(v[i].x, std::min(v[i + 1].x, v[i + 2].x)), tx1 = std::max(v[i].x, std::max(v[i + 1].x, v[i + 2].x));
            float ty0 = std::min(v[i].y, std::min(v[i + 1].y, v[i + 2].y)), ty1 = std::max(v[i].y, std::max(v[i + 1].y, v[i + 2].y));
            out = tx1 < cull[0] || tx0 > cull[2] || ty1 < cull[1] || ty0 > cull[3];
        }
        if (!drawn || out) {
            uint32_t extra = slopes ? sa_slope(&v[i], slopes == 2) : 0;
            for (int k = 0; k < 3; k++) {
                Vtx c = v[i + k];
                c.rgba = (c.rgba & 0xFFFFFFu) | extra << 24;
                f->corners.push_back(c);
            }
            if (out) {
                f->outs.push_back(uint32_t(i / 3));
            }
            continue;
        }
        f->corners.insert(f->corners.end(), drawn->begin() + ptrdiff_t(next), drawn->begin() + ptrdiff_t(next) + 3);
        next += 3;
    }
    kp.out_n = uint32_t(f->outs.size()) - kp.out_at;
    return &kp;
}

// (sa_draw_batch, the frame's batch kept: pc_world_keep's) the batch kept for the pictures between
void pc_kept_batch(const PcKey &k, uint32_t ctxt, const ImageHandle &texture, const BufferHandle &arena,
                   const std::vector<Vtx> &corners, int slopes, const float *cull, uint32_t acquire_rows, uint32_t row0,
                   uint32_t row1, uint32_t z_written) {
    PcKept *kp = pc_kept_end(&corners, slopes, cull);
    if (!kp) {
        return;
    }
    kp->key = k;
    kp->ctxt = ctxt;
    kp->acquire_rows = acquire_rows;
    kp->row0 = row0;
    kp->row1 = row1;
    kp->z_written = z_written;
    kp->texture = texture;
    kp->arena = arena;
}

// (sa_draw_batch, a taken draw's batch in the frame of which nothing is drawn: every triangle outside the scissor (cull),
// which a picture's camera can bring in, or none at all: cull null) kept as such
void pc_kept_nothing(uint32_t ctxt, uint32_t acquire_rows, const float *cull) {
    if (PcKept *kp = pc_kept_end(nullptr, 0, cull)) {
        kp->ctxt = ctxt;
        kp->acquire_rows = acquire_rows;
    }
}

// (sa_draw_end, the frame's taken draw ended) whether every batch ended in pc_kept_end
void pc_kept_draw_end() {
    if (PcFrameKept *f = pc_kept_frame()) {
        PcDrawKept &d = f->draws[size_t(nd_prep.draw)];
        d.whole = d.met == d.count;
    }
}

// Whether triangle t of a batch the frame left out (outside the scissor) is in view at t 0, as sa_draw_batch tests them
// in a picture between: its box at t 1 widened by where its corners are at t 0 (nd_box_at_0)
bool pc_left_out_in_view(const PcFrameKept &f, const PcKept &kp, uint32_t t, uint32_t change) {
    const Vtx *in = f.corners.data() + kp.at + size_t(t) * 3;
    Vtx v[3] = {in[0], in[1], in[2]};
    for (Vtx &c : v) {
        c.rgba ^= change;
    }
    float tx0 = std::min(v[0].x, std::min(v[1].x, v[2].x)), tx1 = std::max(v[0].x, std::max(v[1].x, v[2].x));
    float ty0 = std::min(v[0].y, std::min(v[1].y, v[2].y)), ty1 = std::max(v[0].y, std::max(v[1].y, v[2].y));
    int32_t box[4] = {int32_t(floorf(tx0)) - 1, int32_t(floorf(ty0)) - 1, int32_t(ceilf(tx1)) + 2, int32_t(ceilf(ty1)) + 2};
    nd_box_at_0_of(v, 3, box);
    return !(float(box[2]) < kp.cull[0] || float(box[0]) > kp.cull[2] || float(box[3]) < kp.cull[1] ||
             float(box[1]) > kp.cull[3]);
}

// (nd_draw, a picture's 0x000 draw the PC world takes, its record made: nd.boxes has it) whether its batches are the
// frame's kept ones: then nd_draw makes none, and pc_between_draw gives them to the PC world
bool pc_between_takes(uint32_t record) {
    pc_between.taking = nullptr;
    if (!pc_between_on() || !interp.active || !sa.pc_take || !nd_prep.taking || nd_prep.draw < 0) {
        return false;
    }
    const PcFrameKept &f = pc_between.frames[nd_prep.taking - nd_prep.frames];
    if (f.serial != nd_prep.taking->serial || size_t(nd_prep.draw) >= f.draws.size()) {
        pc_between.no_frame++;
        return false;
    }
    const PcDrawKept &d = f.draws[size_t(nd_prep.draw)];
    if (!d.whole) {
        pc_between.not_whole++;
        return false;
    }
    uint32_t change = (d.record ^ record) << ND_CORNER_DRAW_SHIFT;
    for (uint32_t i = 0; i < d.count; i++) {
        const PcKept &kp = f.kept[d.first + i];
        if (kp.drawn && kp.arena.get() != nd.arena.buffer.get()) {
            pc_between.not_whole++;
            return false;
        }
        for (uint32_t j = 0; !kp.drawn && j < kp.out_n; j++) {
            if (pc_left_out_in_view(f, kp, f.outs[kp.out_at + j], change)) {
                pc_between.left_out_in_view++;
                return false;
            }
        }
    }
    pc_between.taking = &d;
    pc_between.taking_frame = &f;
    return true;
}

// (nd_draw, after the draw's registers: a picture's draw pc_between_takes took) the frame's kept batches given to the
// PC world with the picture's record, and sa_flush's and sa_draw_batch's work around a kept batch done for each
void pc_between_draw(uint32_t record) {
    const PcDrawKept *d = pc_between.taking;
    const PcFrameKept *f = pc_between.taking_frame;
    pc_between.taking = nullptr;
    if (!d) {
        return;
    }
    pc_between.draws_taken++;
    uint32_t change = (d->record ^ record) << ND_CORNER_DRAW_SHIFT;
    for (uint32_t i = 0; i < d->count; i++) {
        const PcKept &kp = f->kept[d->first + i];
        uint64_t frame = gs->reg[R_FRAME_1 + kp.ctxt], zbuf = gs->reg[R_ZBUF_1 + kp.ctxt];
        const PcFrame fr = {bits(frame, 0, 9) * 32, bits(frame, 16, 6), bits(frame, 24, 6), bits(zbuf, 0, 9) * 32,
                            bits(zbuf, 24, 4) | 0x30};
        // (sa_flush's) a batch into the reflection the picture keeps as the frame drew it
        if (sat_kept(fr.fbp, fr.fbw, fr.fpsm)) {
            sa_group_draw();
            sa_stats.kept_reflection++;
            continue;
        }
        sa.fcopy.fresh = false;
        sa.first_z_before.swap(sa.first_z);
        sa.first_z_before_on = sa.first_z_on;
        sa.first_z_on = false;
        // (sa_draw_batch's)
        if (sa_group_pending() && (sa_group.fbp != fr.fbp || sa_group.fbw != fr.fbw || sa_group.fpsm != fr.fpsm ||
                                   sa_group.zbp != fr.zbp || sa_group.zpsm != fr.zpsm)) {
            sa_group_draw();
        }
        pc_world_flush_for(fr.fbp, fr.fbw, fr.fpsm, fr.zbp, fr.zpsm);
        if (kp.acquire_rows && !sa_acquire(fr.fbp, fr.fbw, fr.fpsm, fr.zbp, fr.zpsm, kp.acquire_rows)) {
            pc_between.not_acquired++; // (never: the frame's batch had it)
            continue;
        }
        if (!kp.drawn) {
            pc_between.batches_out++; // (nothing drawn: none of it in view at t 0 or 1, or none at all)
            continue;
        }
        // (every triangle, those the frame left out too: the GPU clips them; a picture's sa_draw_batch tests them at t 0,
        // which cost more than drawing them)
        uint32_t n = kp.n;
        pc_between.corners.resize(n);
        const Vtx *in = f->corners.data() + kp.at;
        for (uint32_t j = 0; j < n; j++) {
            pc_between.corners[j] = in[j];
            pc_between.corners[j].rgba ^= change;
        }
        pc_between.left_out += kp.out_n;
        PcKey k = kp.key;
        k.records = nd.draws.buffer.get();
        k.arena = nd.arena.buffer.get();
        std::vector<uint32_t> &runs = sa.runs;
        runs.assign({0u, n});
        sa_group_draw_parts();
        pc_world_keep(k, fr, kp.texture, nd.draws.buffer, nd.arena.buffer, pc_between.corners, runs);
        sa.last_shadow = false;
        sa.z_written |= kp.z_written != 0;
        sa.rows0 = std::min(sa.rows0, kp.row0);
        sa.rows1 = std::max(sa.rows1, kp.row1);
        sa.drew = true;
        sa.scissor_rows = std::max(sa.scissor_rows, kp.row1);
        sa_note_first_z(false);
        sa_stats.batches++;
        sa_stats.triangles += n / 3;
        gsn_stats.primitives += n / 3;
        pc_between.batches_taken++;
        pc_between.corners_taken += n;
    }
}

// (nd_prep_forget's) the frames' kept batches let go
void pc_between_forget() {
    for (PcFrameKept &f : pc_between.frames) {
        f.clear();
    }
    pc_between.taking = nullptr;
}

void pc_between_print_stats() {
    const PcBetween &b = pc_between;
    fprintf(stderr, "[stats] pc world, pictures between: %llu draws (%llu batches, %llu corners with %llu triangles the frame "
            "left out at the scissor; %llu batches drawn nothing of) drawn from the frame's kept batches; worked out again: "
            "%llu not every batch kept, %llu a triangle of a batch the frame drew nothing of in view, %llu no frame kept; "
            "%llu not acquired\n",
            (unsigned long long)b.draws_taken, (unsigned long long)b.batches_taken, (unsigned long long)b.corners_taken,
            (unsigned long long)b.left_out, (unsigned long long)b.batches_out, (unsigned long long)b.not_whole, (unsigned long long)b.left_out_in_view,
            (unsigned long long)b.no_frame, (unsigned long long)b.not_acquired);
}
