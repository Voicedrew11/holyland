// ---- The PC renderer's draws (--renderer fast, docs/port/PC_RENDERER.md, "P7: the world off the GS's batches") ------------
// (included by gsn_standalone.inc after gsn_pc_world.inc, in gsn.cpp's namespace; the GS's thread)
//
// A world draw of program 0x000 (the map's records, its objects, the items, the gold, the picked item) is drawn by the PC
// renderer from its meshes on the GPU: one draw a batch, its triangles a list on the GPU made once a mesh (pcd.tris), its
// draw's record (nd_draw's NdConst) its instance, and pc_000.vert doing VU1's maths, its clipping (the ADC that leaves a
// triangle out) and a layer's Z slope. Nothing of the batch is planned on the CPU: no corners, no batch of the GS's, no
// runs, no ties' Z-first draws, no texture rows from the vertices (the texture is read whole, as a picture between's).
// A skinned model's (program 0x280: the creatures, the arm and what it holds) keeps the CPU's plan (nd280_plan_cached,
// cached and four lanes at a time), which says whether the run draws at all (and so which registers it writes) and which
// triangles VU1 kicks: those go into a list each time (pcd.kicked, 16 bytes a triangle), each pass a draw by its record
// with pc_280.vert; no corners, no batch of the GS's, no skinned groups. The effects' (program 0x700) likewise: their plan
// says whether the strip is kicked and which triangles, pc_700.vert draws them; a layer's Z slope from the plan's places.
//
// What stopped a renderer apart from the GS (PC_RENDERER.md, "P2, revised") is where a draw's material is known: its
// texture is TEX0 read against VRAM as the chain has it at that point, its alpha test and blend the chain's registers
// then. So the material is resolved here, where the GS's thread meets the draw in the chain's order (sa_texture from the
// page-ownership table, P1's RGBA8 image, held by the recording), and the draw made at once into the scene's colour and
// depth, in the chain's order with everything else: nothing is kept for later, and nothing can read another map's pages.
//
// A picture between walks the frame's packets again (frame_interp.c), and its draws here are the frame's with the
// picture's record (ND_C_MOVING and the models' moves). The frame keeps each call it makes here, per native draw, as
// nd_prep keeps the draw's record and corners (PcdKept, gsn_nd_prep.inc): a picture whose draw at that place is the
// frame's, kept whole, makes the frame's calls again, with its own record, and resolves no material and plans nothing
// (pcd_between_takes, pcd_between_call). The materials are held by those calls; the triangles are lists the frame made,
// never written again. A call whose registers differ from the frame's, or whose texture the table gives otherwise now
// (pcd_table_same), or a draw whose arena changed, is worked out again as above, and counted in --stats. KF4_PC_KEPT=0:
// none kept nor taken, fast as before. Most such draws are not walked at all in a picture: the frame's snapshot of them
// makes them at their place in the chain ("Snapshot", below; KF4_PC_SNAPSHOT=0: walked).
//
// A texture the GPU's sampler cannot stand in for (a render target's, the reflection's, the scrolling textures', one decoded
// from VRAM, a region clamp) is read as the scene reads it, with the GS's sampling (gs_frag.h: the raw texels and the
// CLUT, or the target's image); such a material is resolved again each time, not kept (its image can change under it).
// A draw into a named target (the reflection's 256 x 256: gsn_scene_targets.inc) is drawn there, as the scene's batches
// are; in a picture between that keeps the reflection as the frame drew it, not at all.
//
// A batch it cannot draw as the GS would (pcd_decline's reasons: shader blending, two passes of the alpha test, DATE)
// goes on to the GS's batch as before (nd_draw), with nothing of it drawn here.
// KF4_PC_DRAW=0 (tests): none taken, fast as before.

enum {
    PCD_WHY_PRIM,     // not triangles
    PCD_WHY_FRAME,    // not the scene's frame (a named target, a format the scene does not draw)
    PCD_WHY_TEST,     // a Z test not GEQUAL, DATE, two alpha-test passes
    PCD_WHY_BLEND,    // blending Vulkan's cannot do as the GS (the shader's: PABE, no COLCLAMP, FBA, As above 1, a mask)
    PCD_WHY_TEXTURE,  // a texture not from the table, or not for the GPU's sampler
    PCD_WHY_TARGET,   // the scene could not take the frame, or its samples need sample shading
    PCD_WHY_PROGRAM,  // no program (a cut-out at every sample without sample shading)
    PCD_WHY_COUNT
};
const char *const pcd_why_names[PCD_WHY_COUNT] = {"not triangles", "not the scene's frame", "its tests", "its blending",
                                                  "its texture", "the scene's target", "no program"};

struct PcdKeyHash {
    size_t operator()(const PcdKey &k) const {
        const uint64_t *w = reinterpret_cast<const uint64_t *>(&k);
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < sizeof(PcdKey) / 8; i++) {
            h = (h ^ w[i]) * 0x100000001b3ull;
        }
        return size_t(h ^ (h >> 29));
    }
};
struct PcdKeyEqual {
    bool operator()(const PcdKey &a, const PcdKey &b) const {
        return memcmp(&a, &b, sizeof(PcdKey)) == 0;
    }
};
static_assert(sizeof(PcdKey) % 8 == 0, "PcdKey is hashed a word at a time");

// A batch's material resolved: declined (why), or how it is drawn
struct PcdMaterial {
    int why = -1;              // a PCD_WHY_*, or -1: drawn
    bool kept = true;          // (false: resolved again each time: it reads a target or VRAM)
    Push push = {};            // (all but inv_size and the jitter: per batch)
    ImageHandle image;         // P1's RGBA8 image (held while the material is kept), or none
    StockSampler sampler = StockSampler::NearestClamp;
    // (the GS's sampling, gs_frag.h) the raw texels (binding 0), a target's image (1), the CLUT (2)
    ImageHandle raw_image;     // (the table's decode the raw texels are of: held)
    const ImageView *raw = nullptr, *target = nullptr;
    bool want_clut = false;
    std::array<uint32_t, 256> clut = {};
    Program *program = nullptr;
    bool blended = false, zwrite = false, depth_test = true;
    FixedBlend blend = {};
    uint32_t write_mask = 0;
    uint64_t tex_serial = 0; // (a texture from the table: its decode's serial, what a kept call checks the table's answer by)
    // (pcd_table_same) the table's answer found the same at this sat.gen, with the frame copy and the scene's image as then
    mutable uint64_t table_gen = 0;
    mutable const void *table_fcopy = nullptr, *table_color = nullptr;
};

// The state the last draw made here left bound, and where the calls were then: while nothing else is added
// (recording_adds, backend_sends), only what differs is set again, and a batch that goes on from the last one's
// triangles with the same state makes the last draw longer
struct PcdBound {
    uint64_t adds = 0, sends = 0;
    bool valid = false;
    const Program *program = nullptr;
    const ImageView *view = nullptr, *raw = nullptr, *target = nullptr;
    StockSampler sampler = StockSampler::NearestClamp;
    bool want_clut = false;
    std::array<uint32_t, 256> clut = {};
    const Buffer *arena = nullptr, *draws = nullptr, *tris = nullptr;
    bool blended = false, zwrite = false, depth_test = true;
    FixedBlend blend = {};
    uint32_t fix = 0, write_mask = 0;
    VkViewport viewport = {};
    VkRect2D scissor = {};
    Push push = {};
    uint32_t instance = 0, end = 0; // (the last draw's record and gouraud, and where its triangles end in tris)
};

struct PcDraws {
    int enabled = -1;
    NdBuffer tris;                                 // the batches' triangles, a uvec4 each (pc_000.vert's Tris)
    NdBuffer kicked;                               // the skinned passes' and effects' triangles VU1 kicks, each time
    uint32_t tris_arena_gen = 0;                   // the arena (nd.arena_gen) the triangles name the vertices of
    std::unordered_map<uint64_t, uint32_t> placed; // a batch's first triangle in tris, by its mesh and number
    Shader *vs[3] = {};
    Program *programs[3][2] = {};    // (reading P1's RGBA8 texture: gs_hw.frag, sa_sample_hw.frag)
    Program *programs_gs[3][2] = {}; // (the GS's sampling: gs.frag, sa_sample.frag)
    PcdMaterial unkept;              // (a material resolved again each time: the one being drawn) // [PCD_VS_*][a cut-out shaded at every sample]: pc_*.vert with gs_hw.frag, sa_sample_hw.frag
    std::unordered_map<PcdKey, std::shared_ptr<const PcdMaterial>, PcdKeyHash, PcdKeyEqual> materials; // (held by the calls kept)
    uint64_t materials_gen = 0;           // (sat.gen when they were resolved)
    uint64_t clut_gen = 0, clut_hash = 0; // (pcd_clut_hash's: sat.clut_gen when it hashed the mirror)
    PcdBound bound;
    uint64_t draws = 0, batches = 0, triangles = 0, nothing = 0, declined[PCD_WHY_COUNT] = {};
    uint64_t passes280 = 0, triangles280 = 0, strips700 = 0, triangles700 = 0;
    uint64_t resolved = 0, found = 0, calls = 0, grown = 0;
    int kept = -1;                        // KF4_PC_KEPT
    bool between_on = false;              // a picture's draw whose calls are the frame's, taken in order (pcd_between_takes)
    uint32_t between_at = 0, between_end = 0;
    uint64_t kept_draws = 0, kept_calls = 0, kept_fell_back = 0;
    uint64_t kept_again[4] = {};          // draws worked out again: 0 no frame kept, 1 not whole, 2 the arena changed,
                                          // 3 registers differed, a call declined or its table's answer was not the frame's
    bool where_place = false;             // (pcd_where's 2: nothing drawn for the place: the kept reflection, an empty scissor)
    // (the snapshot: NdPrepFrame::pcd_snaps) KF4_PC_SNAPSHOT; in the frame, the registers its kept draw writes; in a
    // picture, the frame's snapshot of the draw met (pcd_snapshot_find) and the draw's value
    int snapshot = -1;
    std::bitset<256> written;
    const PcdSnapDraw *snap = nullptr;
    uint64_t snap_value = 0;
    uint64_t snap_draws = 0, snap_calls = 0, snap_loads = 0;
    uint64_t snap_unkept[2] = {}; // (the frame) draws kept whole without a snapshot: 0 a write, 1 nothing for its place
    uint32_t snap_unkept_address = 0; // (the last write's)
    uint64_t snap_again[6] = {}; // draws walked again: 0 not kept so, 1 the registers at its start, 2 vertices pending,
                                 // 3 a CLUT on a target's pages, 4 a CLUT not the frame's, 5 the place or the table's answer
} pcd;
enum { PCD_VS_000, PCD_VS_280, PCD_VS_700 }; // (pcd.programs: pc_000.vert, pc_280.vert, pc_700.vert)

bool sa_kind(const RdDraw &d);
void sa_note_first_z(bool first);
bool sa_jitter_draw();

bool pcd_on() {
    if (pcd.enabled < 0) {
        const char *v = getenv("KF4_PC_DRAW");
        pcd.enabled = v && *v ? atoi(v) != 0 : 1;
    }
    return pcd.enabled != 0 && pc_world_mode && sa.on;
}

// KF4_PC_KEPT=0 (tests): the frame keeps no calls, and a picture makes none of them again: fast as before
bool pcd_kept_on() {
    if (pcd.kept < 0) {
        const char *v = getenv("KF4_PC_KEPT");
        pcd.kept = v && *v ? atoi(v) != 0 : 1;
    }
    return pcd.kept != 0;
}

bool pcd_between_on() {
    return pcd.between_on;
}

// (sa_draw_begin) Whether the PC renderer draws the draw's batches itself (those it can: pcd_batch)
bool pcd_takes(const RdDraw &d) {
    bool kind000 = d.kind == RD_KIND_MAP_RECORD || d.kind == RD_KIND_MAP_OBJECT || d.kind == RD_KIND_ITEM ||
                   d.kind == RD_KIND_GOLD || d.kind == RD_KIND_PICKED;
    bool kind280 = d.kind == RD_KIND_ENTITY || d.kind == RD_KIND_ARM;
    return pcd_on() && ((d.program == 0 && kind000) || (d.program == 0x280 && kind280) ||
                        (d.program == 0x700 && d.kind == RD_KIND_EFFECT)) && sa_kind(d);
}

void pcd_programs() {
    if (pcd.vs[0] || !sa.programs_hw[SA_VS_000][0]) {
        return;
    }
    ResourceLayout vert = {};
    vert.push_constant_size = sizeof(Push);
    vert.sets[1].storage_buffer_mask = 1u;
    vert.sets[1].meta[0].array_size = 1;
    vert.sets[2].storage_buffer_mask = 7u;
    for (int b = 0; b < 3; b++) {
        vert.sets[2].meta[b].array_size = 1;
    }
    // (as sa_programs: the jitter's build only with an upscaler's jitter)
    bool jitter = sa_jitter_on();
    pcd.vs[PCD_VS_000] = jitter ? gs->device->request_shader(gsn_spv_pc_000_jitter_vert, sizeof(gsn_spv_pc_000_jitter_vert), &vert)
                                : gs->device->request_shader(gsn_spv_pc_000_vert, sizeof(gsn_spv_pc_000_vert), &vert);
    pcd.vs[PCD_VS_280] = jitter ? gs->device->request_shader(gsn_spv_pc_280_jitter_vert, sizeof(gsn_spv_pc_280_jitter_vert), &vert)
                                : gs->device->request_shader(gsn_spv_pc_280_vert, sizeof(gsn_spv_pc_280_vert), &vert);
    pcd.vs[PCD_VS_700] = jitter ? gs->device->request_shader(gsn_spv_pc_700_jitter_vert, sizeof(gsn_spv_pc_700_jitter_vert), &vert)
                                : gs->device->request_shader(gsn_spv_pc_700_vert, sizeof(gsn_spv_pc_700_vert), &vert);
    for (int k = 0; k < 3; k++) {
        for (int s = 0; s < 2; s++) {
            if (Program *hw = sa.programs_hw[SA_VS_000][s]) {
                pcd.programs[k][s] = gs->device->request_program(pcd.vs[k], hw->get_shader(ShaderStage::Fragment));
            }
            if (Program *own = sa.programs[SA_VS_000][s]) {
                pcd.programs_gs[k][s] = gs->device->request_program(pcd.vs[k], own->get_shader(ShaderStage::Fragment));
            }
        }
    }
}

// The batch's triangles in pcd.tris (placed there the first time it is drawn into this arena): its first
void nd_new_buffer(NdBuffer &b, uint32_t room, uint32_t size, bool named);
uint32_t pcd_place(const RdMeshData &m, const RdBatch &b, uint32_t base) {
    if (pcd.tris_arena_gen != nd.arena_gen) {
        pcd.placed.clear(); // (a new arena: the meshes are placed in it again, at other numbers)
        pcd.tris_arena_gen = nd.arena_gen;
    }
    uint64_t key = uint64_t(m.id) << 20 | uint64_t(&b - m.batches.data());
    auto it = pcd.placed.find(key);
    if (it != pcd.placed.end()) {
        return it->second;
    }
    uint32_t n = uint32_t(b.triangles.size() / 3);
    if (!pcd.tris.buffer || pcd.tris.used + n > pcd.tris.room) {
        // (full: a new buffer, the batches placed in it again as they are drawn; what a draw reads is never written again)
        nd_new_buffer(pcd.tris, std::max(pcd.tris.room ? pcd.tris.room * 2 : 1u << 16, n), 16, false);
        pcd.placed.clear();
    }
    uint32_t at = pcd.tris.used;
    uint32_t *out = reinterpret_cast<uint32_t *>(pcd.tris.map) + size_t(at) * 4;
    uint32_t first = base + b.first;
    for (uint32_t t = 0; t < n; t++) {
        uint32_t kick = b.triangles[t * 3 + 2];
        out[t * 4 + 0] = first + b.triangles[t * 3 + 0];
        out[t * 4 + 1] = first + b.triangles[t * 3 + 1];
        out[t * 4 + 2] = first + kick;
        out[t * 4 + 3] = (first + kick) | std::min(kick, 2u) << 28;
    }
    pcd.tris.used += n;
    pcd.placed[key] = at;
    return at;
}

// The least and most alpha of the batch's vertices as VU1 leaves them and the GS takes them (nd_plan_batch: the low 8
// bits of the vertex's, at most 255, so a negative alpha wraps)
void pcd_alpha_range(const RdMeshData &m, const RdBatch &b, uint32_t *lo, uint32_t *hi) {
    *lo = 255;
    *hi = 0;
    for (uint32_t k = 0; k < b.count; k++) {
        uint32_t a = uint32_t(std::min(m.vertices[(b.first + k) * 16 + 15], 255)) & 0xFF;
        *lo = std::min(*lo, a);
        *hi = std::max(*hi, a);
    }
}

// The CLUT mirror's contents (sa_clut_load's: the colours, which halfwords are the GS's or a pack's), hashed again only
// when it changed: a skinned model's parts load two CLUTs in turn, and each material keeps the one it was made with
uint64_t pcd_clut_hash() {
    if (pcd.clut_gen != sat.clut_gen) {
        uint64_t h = 0xcbf29ce484222325ull;
        auto mix = [&](const void *p, size_t n) {
            const uint64_t *w = static_cast<const uint64_t *>(p);
            for (size_t i = 0; i < n / 8; i++) {
                h = (h ^ w[i]) * 0x100000001b3ull;
                h ^= h >> 31;
            }
        };
        mix(sat.clut, sizeof(sat.clut));
        mix(sat.clut_gs, sizeof(sat.clut_gs));
        mix(sat.clut_packed, sizeof(sat.clut_packed));
        pcd.clut_hash = h;
        pcd.clut_gen = sat.clut_gen;
    }
    return pcd.clut_hash;
}

// The alpha test's passes as sa_draw_batch makes them (every pixel, those that pass, those that fail, each with its colour
// mask and Z write), from TEST, ZBUF and FRAME: how many, and the first in *one (when not null)
struct PcdPass {
    uint32_t select, mask;
    bool zwrite;
};
int pcd_passes(uint64_t test, uint64_t zbuf, uint64_t frame, PcdPass *one) {
    bool ate = bits(test, 0, 1);
    uint32_t atst = bits(test, 1, 3), afail = bits(test, 12, 2);
    uint32_t fpsm = bits(frame, 24, 6);
    uint32_t fbmsk = uint32_t(frame >> 32) | (fpsm == GSN_PSMCT24 ? 0xFF000000u : 0u);
    uint32_t write_mask = 0;
    for (int i = 0; i < 4; i++) {
        write_mask |= ((fbmsk >> (i * 8)) & 0xFF) != 0xFF ? 1u << i : 0u;
    }
    bool zwrite = !bits(zbuf, 32, 1) && bits(test, 16, 1); // (ZTE 0: no Z at all)
    PcdPass passes[2];
    int n = 0;
    if (!ate || atst == 1) {
        passes[n++] = {0, write_mask, zwrite};
    } else {
        if (atst != 0) {
            passes[n++] = {1, write_mask, zwrite};
        }
        uint32_t fail_mask = afail == 1 ? write_mask : afail == 3 ? write_mask & 7 : 0;
        bool fail_z = afail == 2 && zwrite;
        if (fail_mask || fail_z) {
            passes[n++] = {atst == 0 ? 0u : 2u, fail_mask, fail_z};
        }
    }
    if (one && n) {
        *one = passes[0];
    }
    return n;
}

// (kept calls) The frame's draw being kept whole, when it is one: none in a picture, and none with KF4_PC_KEPT=0
NdPrepDraw *pcd_kept_draw() {
    if (interp.active || !pcd_kept_on() || !nd_prep.filling || nd_prep.draw < 0) {
        return nullptr;
    }
    NdPrepDraw &d = nd_prep.filling->draws[size_t(nd_prep.draw)];
    return d.pcd_whole ? &d : nullptr;
}

// (kept calls) A batch the PC renderer did not draw, so the frame's draw is not kept whole: the GS's batch draws it
void pcd_keep_fail() {
    if (NdPrepDraw *d = pcd_kept_draw()) {
        d->pcd_whole = false;
    }
}

bool pcd_decline(int why) {
    pcd.declined[why]++;
    pcd_keep_fail();
    return false;
}

// A material resolved from the GS's state now (the scene has its frame: sa_acquire): the texture from the table for
// the GPU's sampler, the blending, the layers' depth, the program (sa_draw_batch's rules)
PcdMaterial pcd_resolve(const PcdKey &key, uint32_t ctxt) {
    PcdMaterial r;
    uint32_t prim = key.prim;
    bool tme = (prim >> 4) & 1, fge = (prim >> 5) & 1, abe = (prim >> 6) & 1, fst = (prim >> 8) & 1;
    uint64_t test = key.test, alpha = key.alpha, tex0 = key.tex0, zbuf = key.zbuf;
    uint32_t fpsm = bits(key.frame, 24, 6);
    uint32_t fbmsk = uint32_t(key.frame >> 32) | (fpsm == GSN_PSMCT24 ? 0xFF000000u : 0u);
    bool ate = bits(test, 0, 1);
    uint32_t atst = bits(test, 1, 3);
    uint32_t ba = bits(alpha, 0, 2), bb = bits(alpha, 2, 2), bc = bits(alpha, 4, 2), bd = bits(alpha, 6, 2);
    uint32_t fix = bits(alpha, 32, 8);
    Target *color = sa.frame_target;
    Push &push = r.push;
    push.zscale = float(depth_scale(bits(zbuf, 24, 4) | 0x30));
    push.zmax = depth_max(bits(zbuf, 24, 4) | 0x30);
    push.test = uint32_t(test);
    push.fogcol = uint32_t(key.fogcol) & 0xFFFFFF;
    push.alpha = ba | bb << 2 | bc << 4 | bd << 6 | fix << 8;
    push.texa = uint32_t(key.texa & 0x80FF) | (bits(key.texa, 32, 8) << 16);
    push.fbmsk = fbmsk;
    push.misc = (key.fba ? M_FBA : 0) | (key.colclamp ? M_COLCLAMP : 0) | (fpsm == GSN_PSMCT24 ? M_DST24 : 0) |
                (key.scanmsk << M_SCANMSK_SHIFT) | (key.wide ? M_HIGH : 0) | sa.sy << M_SCALE_SHIFT | sa.sx << M_SCALE_X_SHIFT;
    PcdPass pass = {};
    pcd_passes(test, zbuf, key.frame, &pass);
    push.flags = (fge ? F_FGE : 0) | pass.select << F_PASS_SHIFT;

    // the texture: the table's, for the GPU's sampler (gsn_rgba.inc), read whole
    SaTexture st;
    bool gs_sampling = false;
    if (tme) {
        sa_reads_whole = true;
        sa_texture(st, push, ctxt, tex0, key.tex1, key.clamp, fst, color);
        sa_reads_whole = false;
        if (st.own_named || st.reads_own) {
            r.why = PCD_WHY_TEXTURE; // (the frame it draws into: the scene's copy of it, sa_draw_batch's)
            return r;
        }
        r.tex_serial = st.decoded && !st.reads_target ? st.decoded->serial : 0;
        RgbaSampling hw = st.reads_target ? RgbaSampling() : rgba_sampling(st, key.tex1, key.clamp);
        if (hw.usable) {
            rgba_configure();
            r.image = rgba_entry(st, push.texa).image;
            r.sampler = hw.sampler;
            push.lod_bias = lod_mode == GSN_LOD_FULL ? -64.0f : 0.0f;
        } else {
            // the GS's sampling, as the scene's batch binds it (sa_draw_batch's bind_run)
            if (!st.reads_target) {
                rgba_kept_gs(hw);
            }
            gs_sampling = true;
            r.kept = st.decoded != nullptr && !st.reads_target;
            r.raw_image = st.table;
            r.raw = st.raw_view;
            r.target = st.target_view;
            r.want_clut = st.want_clut;
            if (st.want_clut) {
                memcpy(r.clut.data(), st.clut, sizeof(st.clut));
            }
        }
    }

    // blending: Vulkan's where it does the GS's; As above pc_world_alpha_over is the shader's (declined)
    uint32_t as_min = key.as_min, as_max = key.as_max;
    uint32_t a_lo = as_min, a_hi = as_max, as_hi = as_max;
    if (tme && bits(tex0, 34, 1)) {
        uint32_t tfx = bits(tex0, 35, 2), tex_alpha_max = st.tex_alpha_max;
        uint32_t tpsm = bits(tex0, 20, 6);
        if (tpsm == GSN_PSMCT24) {
            tex_alpha_max = bits(key.texa, 0, 8);
        } else if (psm_is_16(tpsm)) {
            tex_alpha_max = std::max(bits(key.texa, 0, 8), bits(key.texa, 32, 8));
        }
        as_hi = tfx == 0 ? std::min(255u, (as_max * tex_alpha_max) >> 7) : tfx == 2 ? std::min(255u, as_max + tex_alpha_max)
                                                                              : tex_alpha_max;
        a_lo = tfx == 0 ? (as_min * st.tex_lo) >> 7 : tfx == 2 ? std::min(255u, as_min + st.tex_lo) : st.tex_lo;
        a_hi = tfx == 0 ? std::min(255u, (as_max * st.tex_hi) >> 7) : tfx == 2 ? std::min(255u, as_max + st.tex_hi) : st.tex_hi;
    }
    r.blend = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0};
    if (abe) {
        fixed_blend(ba, bb, bc, bd, fix, &r.blend); // (pcd_batch: true)
    }
    if (abe && bc == 0 && !r.blend.premul && as_hi > pc_world_alpha_over()) {
        r.why = PCD_WHY_BLEND;
        return r;
    }
    bool blend_is_source = abe && ba == 0 && bb == 1 && bc == 0 && bd == 1 && a_lo == 0x80 && a_hi == 0x80;
    r.blended = abe && !blend_is_source;
    if (r.blended && r.blend.premul) {
        push.flags |= r.blend.premul;
    } else if (r.blended && bc == 0 && ba != bb) {
        push.flags |= F_ALPHA_FACTOR;
    }
    // the layers' depth (gsn_pc_world.inc's pc_material_draw): opaque exact; blended within the tie's slack, Z written so
    bool same_twice = !abe || (ba == 0 && bb == 1 && bd == 1 && (bc == 0 ? a_lo == 0x80 && a_hi == 0x80 : bc == 2 && fix == 0x80));
    // (a skinned model's: its ties exact, its shine on its first pass's very corners, sa_draw_batch's skin_eps 0)
    if (r.blended && key.vs != PCD_VS_280) {
        float slack_layer = 1.0f, slack_tie = 0.5f, world_eps = 2.0f; // (sa_draw_batch's defaults: KF4_SA_SLACK, KF4_SA_WORLD)
        bool sloped = zwrite_any(zbuf, test) && !same_twice;
        push.zbias = sloped ? slack_layer : slack_tie;
        push.zslope = sloped ? 1.0f : 0.0f;
        if (sloped && world_eps > 0.0f) {
            push.zbias = 0.0f;
            push.zslope = -world_eps;
        }
    }
    // --native-cutout-aa: a cut-out shaded at every sample (sa_draw_batch's test)
    bool per_sample = false;
    if (st.cut_texels && bits(tex0, 34, 1) && st.cut_lo < st.cut_hi && st.cut_clear * 4 >= st.cut_all && !fst &&
        cutout_aa_mode == GSN_CUTOUT_AA_SSAA) {
        uint32_t tfx = bits(tex0, 35, 2), ref = bits(test, 4, 8);
        uint32_t lo = tfx == 0 ? (as_min * st.cut_lo) >> 7 : tfx == 2 ? std::min(255u, as_min + st.cut_lo) : st.cut_lo;
        uint32_t hi = tfx == 0 ? std::min(255u, (as_max * st.cut_hi) >> 7) : tfx == 2 ? std::min(255u, as_max + st.cut_hi) : st.cut_hi;
        bool splits = false;
        if (ate) {
            switch (atst) {
            case 2:
            case 5:
                splits = lo < ref && hi >= ref;
                break;
            case 3:
            case 6:
                splits = lo <= ref && hi > ref;
                break;
            case 4:
            case 7:
                splits = lo <= ref && ref <= hi && lo < hi;
                break;
            }
        }
        per_sample = splits || (abe && bc == 0 && ba != bb && lo == 0 && hi > 0);
    }
    r.program = (gs_sampling ? pcd.programs_gs : pcd.programs)[key.vs][per_sample ? 1 : 0];
    if (!r.program) {
        r.why = PCD_WHY_PROGRAM;
        return r;
    }
    r.zwrite = pass.zwrite;
    r.write_mask = pass.mask & (fpsm == GSN_PSMCT24 ? 7u : 15u);
    // the Z test: ALWAYS (or ZTE 0), else GEQUAL for GEQUAL and GREATER, as the scene ties them (sa_draw_batch)
    r.depth_test = bits(test, 16, 1) && bits(test, 17, 2) != 1;
    return r;
}

// The batch's draw, with only the state that differs from the last draw's set again (pcd.bound), or the last draw made
// longer
void pcd_draw(const PcdMaterial &mat, const Push &push, uint32_t fix, const VkViewport &viewport, const VkRect2D &scissor,
              const Buffer &records, const Buffer &tris, uint32_t first, uint32_t n, uint32_t instance) {
    PcdBound &b = pcd.bound;
    const ImageView *view = mat.image ? &mat.image->get_view() : &gs->dummy_rgba->get_view();
    const ImageView *raw = mat.raw ? mat.raw : &gs->dummy_raw->get_view();
    const ImageView *target = mat.target ? mat.target : &gs->dummy_rgba->get_view();
    bool clut_same = b.want_clut == mat.want_clut && (!mat.want_clut || b.clut == mat.clut);
    bool same = b.valid && b.adds == recording_adds && b.sends == backend_sends() && gs->in_pass &&
                gs->pass_color == &SA_MARKER;
    Cmd &c = cmd();
    if (same && b.program == mat.program && b.view == view && b.sampler == mat.sampler && b.raw == raw && b.target == target &&
        clut_same && b.arena == nd.arena.buffer.get() &&
        b.draws == &records && b.tris == &tris && b.blended == mat.blended &&
        (!mat.blended || (memcmp(&b.blend, &mat.blend, sizeof(FixedBlend)) == 0 && b.fix == fix)) && b.zwrite == mat.zwrite &&
        b.depth_test == mat.depth_test &&
        b.write_mask == mat.write_mask && memcmp(&b.viewport, &viewport, sizeof(viewport)) == 0 &&
        memcmp(&b.scissor, &scissor, sizeof(scissor)) == 0 && memcmp(&b.push, &push, sizeof(Push)) == 0 &&
        b.instance == instance && b.end == first && c.grow_last_draw(n * 3)) {
        b.end = first + n;
        b.adds = recording_adds;
        pcd.grown++;
        return;
    }
    sa_begin_pass(false);
    int kind = sat_bound() ? GPU_SA_REFL : GPU_SA_DRAW;
    if (gpu_cur != kind) {
        gpu_enter(kind);
    }
    same = same && b.adds == recording_adds; // (a pass begun or a mark: everything set again)
    if (!same) {
        c.set_opaque_state();
        c.set_texture(0, 3, gs->dummy_rgba->get_view(), StockSampler::NearestClamp);
        c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        c.set_cull_mode(VK_CULL_MODE_NONE);
        if (!interp.active) {
            // (a picture between's Picture is bound by its kicks' pass, recorded; the frame's: its jitter, or zeros)
            if (const Buffer *jitter = sa_jitter_frame_picture()) {
                c.set_storage_buffer(1, 0, *jitter);
            } else {
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
        }
    }
    if (!same || b.arena != nd.arena.buffer.get() || b.draws != &records || b.tris != &tris) {
        c.set_storage_buffer(2, 0, *nd.arena.buffer);
        c.set_storage_buffer(2, 1, records);
        c.set_storage_buffer(2, 2, tris);
    }
    if (!same || b.view != view || b.sampler != mat.sampler) {
        c.set_texture(0, 6, *view, mat.sampler); // (the stream keeps the image)
    }
    if (!same || b.raw != raw) {
        c.set_texture(0, 0, *raw, StockSampler::NearestClamp);
    }
    if (!same || b.target != target) {
        c.set_texture(0, 1, *target, StockSampler::NearestClamp);
    }
    if (!same || !clut_same) {
        static const uint32_t no_clut[256] = {};
        c.set_storage_data(0, 2, mat.want_clut ? mat.clut.data() : no_clut, 1024);
    }
    if (!same || b.zwrite != mat.zwrite || b.depth_test != mat.depth_test) {
        c.set_depth_test(true, mat.zwrite);
        c.set_depth_compare(mat.depth_test ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS);
    }
    if (!same || memcmp(&b.viewport, &viewport, sizeof(viewport)) != 0) {
        c.set_viewport(viewport);
    }
    if (!same || memcmp(&b.scissor, &scissor, sizeof(scissor)) != 0) {
        c.set_scissor(scissor);
    }
    if (!same || b.program != mat.program) {
        c.set_program(mat.program);
    }
    if (mat.blended && (!same || !b.blended || memcmp(&b.blend, &mat.blend, sizeof(FixedBlend)) != 0 || b.fix != fix)) {
        float constants[4] = {float(fix) / 128.0f, float(fix) / 128.0f, float(fix) / 128.0f, 128.0f / 255.0f};
        c.set_blend_enable(true);
        c.set_blend_constants(constants);
        c.set_blend_factors(mat.blend.src, (push.flags & F_ALPHA_FACTOR) ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE,
                            mat.blend.dst, VK_BLEND_FACTOR_ZERO);
        c.set_blend_op(mat.blend.op, VK_BLEND_OP_ADD);
    } else if (!mat.blended && (!same || b.blended)) {
        c.set_blend_enable(false);
    }
    if (!same || b.write_mask != mat.write_mask) {
        c.set_color_write_mask(mat.write_mask);
    }
    if (!same || memcmp(&b.push, &push, sizeof(Push)) != 0) {
        c.push_constants(&push, 0, sizeof(Push));
    }
    c.draw(n * 3, first * 3, instance);
    pcd.calls++;
    gsn_stats.draws++;
    b.valid = true;
    b.program = mat.program;
    b.view = view;
    b.raw = raw;
    b.target = target;
    b.want_clut = mat.want_clut;
    if (mat.want_clut) {
        b.clut = mat.clut;
    }
    b.sampler = mat.sampler;
    b.arena = nd.arena.buffer.get();
    b.draws = &records;
    b.tris = &tris;
    b.blended = mat.blended;
    b.blend = mat.blend;
    b.fix = fix;
    b.zwrite = mat.zwrite;
    b.depth_test = mat.depth_test;
    b.write_mask = mat.write_mask;
    b.viewport = viewport;
    b.scissor = scissor;
    b.push = push;
    b.instance = instance;
    b.end = first + n;
    b.adds = recording_adds;
    b.sends = backend_sends();
}

// A batch (or pass) the PC renderer takes, prepared: its material and where it is drawn. regs is the key the material is
// resolved from before anything was drawn (a picture between's call is checked by it); held is the material when it is
// kept (a call kept with it holds it), mat where it is
struct PcdTake {
    std::shared_ptr<const PcdMaterial> held;
    const PcdMaterial *mat = nullptr;
    PcdKey regs = {};
    uint8_t vs = 0;
    Push push = {};
    VkViewport viewport = {};
    VkRect2D scissor = {};
    uint32_t sy0 = 0, y1 = 0;
    bool gouraud = false, context2 = false;
};

// (pcd_prepare's first part: the GS's registers as the batch is drawn with them, its vertices' alphas as_min to as_max)
// The key its material is resolved from: -1 when the batch goes on, else why it is declined. Draws nothing and flushes
// nothing (what came before it in the chain is drawn by pcd_where, after these checks)
int pcd_key_regs(uint32_t vs, uint32_t as_min, uint32_t as_max, PcdKey &key) {
    uint32_t prim = effective_prim();
    uint32_t type = prim & 7;
    if (type < 3 || type > 5) {
        return PCD_WHY_PRIM;
    }
    uint32_t ctxt = (prim >> 9) & 1;
    bool abe = (prim >> 6) & 1;
    memset(&key, 0, sizeof(key));
    key.vs = vs;
    key.prim = prim & 0x7FF;
    key.frame = gs->reg[R_FRAME_1 + ctxt];
    key.zbuf = gs->reg[R_ZBUF_1 + ctxt];
    key.test = gs->reg[R_TEST_1 + ctxt];
    key.alpha = gs->reg[R_ALPHA_1 + ctxt];
    key.scissor = gs->reg[R_SCISSOR_1 + ctxt];
    uint32_t fbp = bits(key.frame, 0, 9) * 32, fbw = bits(key.frame, 16, 6), fpsm = bits(key.frame, 24, 6);
    uint32_t sx0 = bits(key.scissor, 0, 11), sx1 = bits(key.scissor, 16, 11);
    uint32_t sy0 = bits(key.scissor, 32, 11), sy1 = bits(key.scissor, 48, 11);
    // (sa_draw_batch's: the scene's frame, or a named target's, T3)
    if (((fpsm != GSN_PSMCT32 && fpsm != GSN_PSMCT24) || fbw * 64 < 512 || sx1 < sx0 || sy1 < sy0) &&
        !(sat_scene_frame(fbw, fpsm) && sx1 >= sx0 && sy1 >= sy0)) {
        return PCD_WHY_FRAME;
    }
    uint64_t test = key.test;
    bool date = bits(test, 14, 1);
    // (one pass of the alpha test, sa_draw_batch's)
    if (date || pcd_passes(test, key.zbuf, key.frame, nullptr) > 1) {
        return PCD_WHY_TEST;
    }
    uint32_t fbmsk = uint32_t(key.frame >> 32);
    bool partial_mask = false;
    for (int i = 0; i < 4; i++) {
        uint32_t mk = (fbmsk >> (i * 8)) & 0xFF;
        partial_mask |= mk != 0 && mk != 0xFF;
    }
    key.alpha = key.alpha & 0xFF000000FFull;
    key.fba = bits(gs->reg[R_FBA_1 + ctxt], 0, 1);
    key.colclamp = bits(gs->reg[R_COLCLAMP], 0, 1);
    key.pabe = bits(gs->reg[R_PABE], 0, 1);
    uint32_t bc = bits(key.alpha, 4, 2);
    FixedBlend blend;
    if (partial_mask ||
        (abe && (key.pabe || !key.colclamp || (bc == 0 && key.fba) || sat_blend_exact() ||
                 !fixed_blend(bits(key.alpha, 0, 2), bits(key.alpha, 2, 2), bc, bits(key.alpha, 6, 2), bits(key.alpha, 32, 8), &blend)))) {
        return PCD_WHY_BLEND;
    }
    pcd_programs();
    if (!pcd.programs[vs][0]) {
        return PCD_WHY_PROGRAM;
    }
    if ((key.prim >> 4) & 1) {
        key.tex0 = gs->reg[R_TEX0_1 + ctxt];
        key.tex1 = gs->reg[R_TEX1_1 + ctxt];
        key.clamp = gs->reg[R_CLAMP_1 + ctxt];
        key.miptbp1 = gs->reg[R_MIPTBP1_1 + ctxt];
        key.miptbp2 = gs->reg[R_MIPTBP2_1 + ctxt];
    }
    key.texa = gs->reg[R_TEXA];
    key.clut = psm_is_palette(bits(key.tex0, 20, 6)) ? pcd_clut_hash() : 0;
    key.fogcol = gs->reg[R_FOGCOL];
    key.scanmsk = bits(gs->reg[R_SCANMSK], 0, 2);
    key.as_min = as_min;
    key.as_max = as_max;
    return -1;
}

// (pcd_prepare's second part) What came before the batch in the chain is drawn first, the scene's frame taken, and the
// batch's place worked out (t's viewport and scissor, and key's places, which the material is resolved with): 0 declined
// (the GS's batch takes it, as before); 1 where it is drawn; 2 nothing to draw (ZTST NEVER, an empty scissor)
int pcd_where(PcdKey &key, PcdTake &t) {
    uint32_t fbp = bits(key.frame, 0, 9) * 32, fbw = bits(key.frame, 16, 6), fpsm = bits(key.frame, 24, 6);
    uint32_t zbp = bits(key.zbuf, 0, 9) * 32, zpsm = bits(key.zbuf, 24, 4) | 0x30;
    uint32_t sx0 = bits(key.scissor, 0, 11), sx1 = bits(key.scissor, 16, 11);
    uint32_t sy0 = bits(key.scissor, 32, 11), sy1 = bits(key.scissor, 48, 11);
    uint64_t test = key.test;
    bool zte = bits(test, 16, 1);
    uint32_t ztst = zte ? bits(test, 17, 2) : 1;
    if (!gs->verts.empty()) {
        flush_batch();
    }
    sa_group_draw();
    pc_world_flush();
    pcd.where_place = true;
    if (interp.active && sat_kept(fbp, fbw, fpsm)) {
        sa_stats.kept_reflection++;
        return 2; // (sa_flush's: a picture between leaves the reflection as the frame drew it)
    }
    pcd.where_place = false;
    if (ztst == 0 || pcd_passes(test, key.zbuf, key.frame, nullptr) == 0) {
        pcd.nothing++;
        return 2; // (ZTST NEVER, or an alpha test that writes nothing: nothing)
    }
    if (!sa_acquire(fbp, fbw, fpsm, zbp, zpsm, sy1 + 1) || (sa.samples > 1 && !sample_shading)) {
        return pcd_decline(PCD_WHY_TARGET);
    }
    Target *color = sa.frame_target;
    uint32_t x1 = std::min(sx1 + 1, color->width), y1 = std::min(sy1 + 1, color->height);
    if (sx0 >= x1 || sy0 >= y1) {
        pcd.nothing++;
        pcd.where_place = true;
        return 2; // (an empty scissor: nothing)
    }
    key.sx = sa.sx;
    key.sy = sa.sy;
    key.samples = sa.samples;
    key.wide = wide(*sa.color);
    key.width = color->width;
    key.height = color->height;
    key.fcopy = sa.fcopy.valid ? sa.fcopy.dbp | 0x80000000u : 0;
    key.lod = lod_mode;
    key.cutout = cutout_aa_mode;
    key.aniso = aniso_log2;
    t.viewport = {0.0f, 0.0f, float(color->width * sa.sx), float(color->height * sa.sy), 0.0f, 1.0f};
    t.scissor = {{int32_t(sx0 * sa.sx), int32_t(sy0 * sa.sy)}, {(x1 - sx0) * sa.sx, (y1 - sy0) * sa.sy}};
    t.sy0 = sy0;
    t.y1 = y1;
    return 1;
}

// (the batch's push constants: its material's, with the scene's inverse size and the jitter as they are now)
void pcd_take_push(PcdTake &t, const PcdMaterial &mat) {
    t.push = mat.push;
    t.push.inv_size[0] = 1.0f / float(sa.frame_target->width);
    t.push.inv_size[1] = 1.0f / float(sa.frame_target->height);
    if (sa_jitter_draw()) {
        t.push.misc |= M_JITTER;
    }
}

// (pcd_prepare's third part) The batch's material, found by its key or resolved (a declined one declines the batch): held
// when kept, else resolved again each time
int pcd_material(PcdKey &key, PcdTake &t) {
    uint32_t ctxt = (key.prim >> 9) & 1;
    // the material: resolved once while the table, the CLUT and the registers stay the same
    if (pcd.materials_gen != sat.gen || pcd.materials.size() > 4096) {
        pcd.materials.clear();
        pcd.materials_gen = sat.gen;
    }
    // (the CLUT as it is once what came before the batch is drawn: the key's, pcd_key_regs', is the one a picture checks)
    key.clut = psm_is_palette(bits(key.tex0, 20, 6)) ? pcd_clut_hash() : 0;
    auto found = pcd.materials.find(key);
    if (found != pcd.materials.end()) {
        t.held = found->second;
        pcd.found++;
    } else {
        pcd.unkept = pcd_resolve(key, ctxt);
        pcd.resolved++;
        if (pcd.unkept.kept) {
            t.held = pcd.materials.emplace(key, std::make_shared<const PcdMaterial>(std::move(pcd.unkept))).first->second;
        } else {
            t.held.reset();
        }
    }
    t.mat = t.held ? t.held.get() : &pcd.unkept;
    const PcdMaterial &mat = *t.mat;
    if (mat.why >= 0) {
        return pcd_decline(mat.why);
    }
    if (mat.image) {
        rgba.batches++;
    }
    pcd_take_push(t, mat);
    return 1;
}

// The registers a picture between's call is checked by (PcdKept::regs): the key as pcd_key_regs has it, less what the
// picture has of its own: FRAME's base and width and ZBUF's base (a picture is shown in its own buffer, sa_acquire from
// the registers now), the scissor (pcd_where works where it is drawn out now) and the scene's places (pcd_place_keep).
// The format of FRAME and ZBUF stays: the material is resolved with it
PcdKey pcd_regs_of(PcdKey k) {
    k.frame &= ~(uint64_t(0x1FF) | uint64_t(0x3F) << 16);
    k.zbuf &= ~uint64_t(0x1FF);
    k.scissor = 0;
    k.sx = k.sy = k.samples = k.wide = k.width = k.height = k.fcopy = 0;
    k.lod = k.cutout = k.aniso = 0;
    return k;
}

// The scene's place pcd_where worked out for a drawn batch (the sizes the material is resolved with), kept with the
// registers, and checked with them
void pcd_place_keep(PcdKey &regs, const PcdKey &key) {
    regs.sx = key.sx;
    regs.sy = key.sy;
    regs.samples = key.samples;
    regs.wide = key.wide;
    regs.width = key.width;
    regs.height = key.height;
    regs.fcopy = key.fcopy;
    regs.lod = key.lod;
    regs.cutout = key.cutout;
    regs.aniso = key.aniso;
}
bool pcd_place_same(const PcdKey &regs, const PcdKey &key) {
    return regs.sx == key.sx && regs.sy == key.sy && regs.samples == key.samples && regs.wide == key.wide &&
           regs.width == key.width && regs.height == key.height && regs.fcopy == key.fcopy && regs.lod == key.lod &&
           regs.cutout == key.cutout && regs.aniso == key.aniso;
}

// A batch (or pass) the PC renderer takes, prepared: 0 declined (the GS's batch takes it, as before); 1 prepared (t); 2
// nothing to draw. The registers are checked first, then what came before is drawn and the place worked out, then the
// material (pcd_key_regs, pcd_where, pcd_material); a picture between makes a kept call with the second part only
int pcd_prepare(int vs, uint32_t as_min, uint32_t as_max, PcdTake &t) {
    PcdKey key;
    int why = pcd_key_regs(uint32_t(vs), as_min, as_max, key);
    if (why >= 0) {
        return pcd_decline(why);
    }
    t.regs = pcd_regs_of(key);
    t.vs = uint8_t(vs);
    t.gouraud = (key.prim >> 3) & 1;
    t.context2 = ((key.prim >> 9) & 1) != 0;
    int where = pcd_where(key, t);
    if (where != 1) {
        return where;
    }
    pcd_place_keep(t.regs, key);
    return pcd_material(key, t);
}

// (kept calls, the frame) One call made for a batch, a pass or a strip, kept in the draw being kept whole: DRAWN with t's
// material and triangles, NOTHING and UNKICKED without them. Nothing is kept in a picture, nor with KF4_PC_KEPT=0
void pcd_keep_call(const PcdTake &t, const BufferHandle &tris, uint32_t first, uint32_t n, uint32_t instance, uint8_t what) {
    NdPrepDraw *d = pcd_kept_draw();
    if (!d) {
        return;
    }
    if (what == PCD_CALL_DRAWN && !t.held) {
        pcd_keep_fail(); // (its material is resolved again each time: not kept)
        return;
    }
    if (what == PCD_CALL_NOTHING && pcd.where_place && d->snap >= 0) {
        PcdSnapDraw &s = nd_prep.filling->pcd_snaps[size_t(d->snap)];
        pcd.snap_unkept[1] += s.usable;
        s.usable = false; // (nothing for its place: a picture's may differ)
    }
    PcdKept k;
    k.regs = t.regs;
    k.mat = t.held;
    k.tris = tris;
    k.first = first;
    k.n = n;
    k.instance_bits = instance & 0xF0000000u; // (the record is below 1 << 28)
    k.vs = t.vs;
    k.what = what;
    nd_prep.filling->pcd_calls.push_back(std::move(k));
    d->calls_n++;
}

// (kept calls, the frame) A batch the frame did not kick, or drew nothing of (every vertex clipped): no call, no PRIM
void pcd_keep_unkicked() {
    pcd_keep_call(PcdTake(), BufferHandle(), 0, 0, 0, PCD_CALL_UNKICKED);
}

// The prepared batch's n triangles from first in tris, by records' instance, into the scene's colour and depth; and what
// sa_draw_batch notes of a batch drawn. In the frame, the call is kept (pcd_keep_call)
void pcd_finish(const PcdTake &t, const Buffer &records, const BufferHandle &tris, uint32_t first, uint32_t n,
                 uint32_t instance) {
    if (n) {
        pcd_draw(*t.mat, t.push, bits(t.regs.alpha, 32, 8), t.viewport, t.scissor, records, *tris, first, n, instance);
    }
    pcd_keep_call(t, tris, first, n, instance, PCD_CALL_DRAWN);
    gsn_stats.primitives += n;
    sa.last_shadow = false;
    sa.z_written |= t.mat->zwrite;
    sa.rows0 = std::min(sa.rows0, t.sy0);
    sa.rows1 = std::max(sa.rows1, t.y1);
    sa.drew = true;
    sa.scissor_rows = std::max(sa.scissor_rows, t.y1);
    sa_note_first_z(false);
    sa_stats.batches++;
    sa_stats.triangles += n;
}

// ---- Kept calls: the frame's calls made again in a picture between (gsn_nd_prep.inc's PcdKept) -----------------------

// (a picture's draw, at its start) Whether the frame's draw at this place was kept whole, with the table and the arena as
// it had them: its calls are then taken in order (pcd_between_next)
bool pcd_between_takes() {
    pcd.between_on = false;
    if (!pcd_kept_on()) {
        return false;
    }
    if (!nd_prep.taking || nd_prep.draw < 0) {
        pcd.kept_again[0]++;
        return false;
    }
    const NdPrepDraw &d = nd_prep.taking->draws[size_t(nd_prep.draw)];
    if (!d.pcd_whole) {
        pcd.kept_again[1]++;
        return false;
    }
    if (d.arena_gen != nd.arena_gen) {
        pcd.kept_again[2]++;
        return false;
    }
    pcd.between_on = true;
    pcd.between_at = d.calls_at;
    pcd.between_end = d.calls_at + d.calls_n;
    pcd.kept_draws++;
    return true;
}

void pcd_snapshot_keep_begin(NdPrepFrame &f, NdPrepDraw &d, const RdDraw &draw, const RdCamera &camera);
void pcd_snapshot_keep_end(NdPrepFrame &f, NdPrepDraw &d);

// (nd_draw, 0x280, 0x700, for a draw the PC renderer takes: its start, nothing written yet) In the frame, the draw is kept
// whole from here, and its snapshot begun; a picture takes its calls, when the frame kept them
void pcd_draw_begin(const RdDraw &draw, const RdCamera &camera) {
    if (interp.active) {
        pcd_between_takes();
        return;
    }
    if (!pcd_kept_on() || !nd_prep.filling || nd_prep.draw < 0) {
        return;
    }
    NdPrepDraw &d = nd_prep.filling->draws[size_t(nd_prep.draw)];
    d.pcd_whole = true;
    d.calls_at = uint32_t(nd_prep.filling->pcd_calls.size());
    d.calls_n = 0;
    d.arena_gen = nd.arena_gen;
    pcd_snapshot_keep_begin(*nd_prep.filling, d, draw, camera);
}

// (the draw's end) The frame's draw is kept whole only while its arena is the one it began with (a picture's batches name
// the arena's vertices); and a picture's draw is no longer taken
void pcd_draw_end() {
    pcd.between_on = false;
    NdPrepDraw *d = pcd_kept_draw();
    if (d && d->arena_gen != nd.arena_gen) {
        d->pcd_whole = false;
    }
    if (d) {
        pcd_snapshot_keep_end(*nd_prep.filling, *d);
    }
}

// (a picture's kept draw) The draw is worked out again from here: its calls are not taken any more
void pcd_between_stop() {
    if (pcd.between_on) {
        pcd.between_on = false;
        pcd.kept_again[3]++;
    }
}

// (a picture's kept draw) The frame's next call, in order: null when the draw is not taken (or its calls are used up)
const PcdKept *pcd_between_next() {
    if (!pcd.between_on) {
        return nullptr;
    }
    if (pcd.between_at >= pcd.between_end) {
        pcd_between_stop();
        return nullptr;
    }
    return &nd_prep.taking->pcd_calls[pcd.between_at++];
}

// (a picture's kept draw, a batch the frame did not draw) The frame's next call is that it was not drawn, or the draw is
// worked out again from here
void pcd_between_unkicked() {
    const PcdKept *k = pcd_between_next();
    if (k && k->what != PCD_CALL_UNKICKED) {
        pcd_between_stop();
    }
}

bool pcd_table_same(const PcdKept &k);

// (a picture's kept draw, one of its calls) The frame's call made again into the picture's scene, with the picture's
// record: its registers and the place worked out as the frame's (pcd_key_regs, pcd_where), its material the frame's. False
// (the draw worked out again from here) when the registers differ from the frame's, or the batch is declined now
bool pcd_between_call(const PcdKept &k, const Buffer &records, uint32_t record) {
    PcdKey key;
    if (pcd_key_regs(k.vs, k.regs.as_min, k.regs.as_max, key) >= 0) {
        pcd_between_stop();
        return false;
    }
    PcdKey now = pcd_regs_of(key), then = pcd_regs_of(k.regs);
    if (memcmp(&now, &then, sizeof(PcdKey)) != 0) {
        pcd_between_stop();
        return false;
    }
    PcdTake t;
    t.held = k.mat;
    t.mat = k.mat.get();
    t.regs = k.regs;
    t.vs = k.vs;
    int where = pcd_where(key, t);
    if (where == 0 || (where == 1 && (k.what != PCD_CALL_DRAWN || !pcd_place_same(k.regs, key) || !pcd_table_same(k)))) {
        pcd_between_stop();
        return false;
    }
    if (where == 2) {
        return true; // (nothing: as the frame's call was, or the reflection kept in this picture)
    }
    pcd_take_push(t, *k.mat);
    pcd_finish(t, records, k.tris, k.first, k.n, record | k.instance_bits);
    pcd.kept_calls++;
    return true;
}

// (a kept call, in a picture, after pcd_where) Whether the table gives the call's texture as it gave the frame's: the same
// decode (its serial), from the table and not a target. What the material was resolved from is otherwise the call's
// registers, the CLUT and the place, each checked by the caller. Found once a material while the table, the frame copy
// and the scene's image stay as they were
bool pcd_table_same(const PcdKept &k) {
    const PcdMaterial &m = *k.mat;
    if (!((k.regs.prim >> 4) & 1)) {
        return true; // (no texture)
    }
    if (!m.tex_serial) {
        return false;
    }
    const void *fcopy = sa.fcopy.valid ? sa.fcopy.image.get() : nullptr;
    if (m.table_gen == sat.gen && m.table_fcopy == fcopy && m.table_color == sa.color.get()) {
        return true;
    }
    // (sa_texture as pcd_resolve asks it, with the mipmaps' registers as the call had them)
    uint32_t ctxt = (k.regs.prim >> 9) & 1;
    uint64_t mip1 = gs->reg[R_MIPTBP1_1 + ctxt], mip2 = gs->reg[R_MIPTBP2_1 + ctxt];
    gs->reg[R_MIPTBP1_1 + ctxt] = k.regs.miptbp1;
    gs->reg[R_MIPTBP2_1 + ctxt] = k.regs.miptbp2;
    SaTexture st;
    Push push = {};
    sa_reads_whole = true;
    sa_texture(st, push, ctxt, k.regs.tex0, k.regs.tex1, k.regs.clamp, (k.regs.prim >> 8) & 1, sa.frame_target);
    sa_reads_whole = false;
    gs->reg[R_MIPTBP1_1 + ctxt] = mip1;
    gs->reg[R_MIPTBP2_1 + ctxt] = mip2;
    if (!st.decoded || st.reads_target || st.decoded->serial != m.tex_serial) {
        return false;
    }
    m.table_gen = sat.gen;
    m.table_fcopy = fcopy;
    m.table_color = sa.color.get();
    return true;
}

// ---- Snapshot: a kept draw made again in a picture between without the walk (gsn_nd_prep.inc's PcdSnapDraw) ----------
//
// A picture's draw whose frame's draw was kept whole is made from the frame's snapshot: no rd_fetch, no batch's register
// writes, no per-call checks of the registers. What the frame's draw did is the same in the picture when it starts from
// the same registers (pcd_entry_regs: the writes are the meshes' and the draw's, the same data), so only what a call reads
// besides the registers is checked: the CLUT its TEX0 writes loaded (the loads made again, they read VRAM), the place
// (pcd_where once: every call of a snapshot's draw has the frame, Z buffer and scissor it began with) and the table's
// answer for its texture (pcd_table_same). Then the calls are drawn and the registers set as the writes left them.
// Anything else, or a check that fails, walks the draw as before (its loads undone first). KF4_PC_SNAPSHOT=0: never.

bool pcd_snapshot_on() {
    if (pcd.snapshot < 0) {
        const char *v = getenv("KF4_PC_SNAPSHOT");
        pcd.snapshot = v && *v ? atoi(v) != 0 : 1;
    }
    return pcd.snapshot != 0 && pcd_kept_on();
}

// The registers a snapshot's draw starts from, in this order; FRAME and ZBUF less their bases (and FRAME's width), which
// a picture has of its own (the scissor too: not here), as pcd_regs_of
const uint8_t pcd_entry_addresses[PCD_ENTRY_REGS] = {
    R_PRIM,      R_TEX0_1,    R_TEX0_2,    R_CLAMP_1,  R_CLAMP_2,  R_TEX1_1,   R_TEX1_2,    R_XYOFFSET_1, R_XYOFFSET_2,
    R_PRMODECONT, R_PRMODE,   R_TEXCLUT,   R_SCANMSK,  R_MIPTBP1_1, R_MIPTBP1_2, R_MIPTBP2_1, R_MIPTBP2_2, R_TEXA,
    R_FOGCOL,    R_ALPHA_1,   R_ALPHA_2,   R_DIMX,     R_DTHE,     R_COLCLAMP, R_TEST_1,    R_TEST_2,     R_PABE,
    R_FBA_1,     R_FBA_2,     R_FRAME_1,   R_FRAME_2,  R_ZBUF_1,   R_ZBUF_2};
std::array<uint64_t, PCD_ENTRY_REGS> pcd_entry_regs() {
    std::array<uint64_t, PCD_ENTRY_REGS> r;
    for (int i = 0; i < PCD_ENTRY_REGS; i++) {
        uint32_t a = pcd_entry_addresses[i];
        uint64_t v = gs->reg[a];
        if (a == R_FRAME_1 || a == R_FRAME_2) {
            v &= ~(uint64_t(0x1FF) | uint64_t(0x3F) << 16);
        } else if (a == R_ZBUF_1 || a == R_ZBUF_2) {
            v &= ~uint64_t(0x1FF);
        }
        r[size_t(i)] = v;
    }
    return r;
}

// Whether a register a kept draw writes can be left as the writes left it (pcd_snapshot_keep_end): not a vertex's, a
// transfer's or the port's (they do more than set the register), nor the frame, Z buffer or scissor (a picture's own)
bool pcd_snapshot_writable(uint32_t address) {
    switch (address) {
    case R_RGBAQ:
    case R_ST:
    case R_UV:
    case R_FOG:
    case R_XYZF2:
    case R_XYZ2:
    case R_XYZF3:
    case R_XYZ3:
    case R_BITBLTBUF:
    case R_TRXPOS:
    case R_TRXREG:
    case R_TRXDIR:
    case R_HWREG:
    case R_FRAME_1:
    case R_FRAME_2:
    case R_ZBUF_1:
    case R_ZBUF_2:
    case R_SCISSOR_1:
    case R_SCISSOR_2:
    case 0x7B:
    case 0x7C:
    case 0x7D:
    case 0x7E:
        return false;
    }
    return address < 0x7B;
}

// (the frame) The snapshot of the draw being kept whole, when it is still one
PcdSnapDraw *pcd_snapshot_filling() {
    NdPrepDraw *d = pcd_kept_draw();
    if (!d || d->snap < 0) {
        return nullptr;
    }
    PcdSnapDraw &s = nd_prep.filling->pcd_snaps[size_t(d->snap)];
    return s.usable ? &s : nullptr;
}

// (pcd_draw_begin, the frame) The draw's snapshot begun: what it starts from
void pcd_snapshot_keep_begin(NdPrepFrame &f, NdPrepDraw &d, const RdDraw &draw, const RdCamera &camera) {
    d.snap = -1;
    if (!pcd_snapshot_on()) {
        return;
    }
    d.snap = int32_t(f.pcd_snaps.size());
    f.pcd_snaps.emplace_back();
    PcdSnapDraw &s = f.pcd_snaps.back();
    s.draw = draw;
    s.camera = camera;
    s.entry = pcd_entry_regs();
    s.loads_at = uint32_t(f.pcd_loads.size());
    s.writes_at = uint32_t(f.pcd_writes.size());
    s.usable = true;
    pcd.written.reset();
}

// (nd_draw, nd280_draw, nd700_draw: a batch's register writes, as the VU1 run kicks them) The write, and in the frame's
// kept draw, noted for its snapshot: which registers it wrote, and the CLUT loads its TEX0 writes start
void pcd_batch_write(uint32_t address, uint64_t value) {
    gs_write(address, value);
    PcdSnapDraw *s = pcd_snapshot_filling();
    if (!s) {
        return;
    }
    if (!pcd_snapshot_writable(address)) {
        s->usable = false;
        pcd.snap_unkept[0]++;
        pcd.snap_unkept_address = address;
        return;
    }
    pcd.written.set(address);
    if (address == R_TEX0_1 || address == R_TEX0_2 || address == R_TEX2_1 || address == R_TEX2_2) {
        uint32_t tex0 = address == R_TEX0_1 || address == R_TEX2_1 ? R_TEX0_1 : R_TEX0_2;
        pcd.written.set(tex0);
        NdPrepFrame &f = *nd_prep.filling;
        f.pcd_loads.push_back({gs->reg[tex0], gs->reg[R_TEXCLUT], f.draws[size_t(nd_prep.draw)].calls_n});
        s->loads_n++;
    }
}

// (pcd_draw_end, the frame) The draw's snapshot ended: the registers its writes left (those written, and those changed by
// a write's own effects: TEX0's mipmap addresses), when the draw is still kept whole
void pcd_snapshot_keep_end(NdPrepFrame &f, NdPrepDraw &d) {
    if (d.snap < 0) {
        return;
    }
    PcdSnapDraw &s = f.pcd_snaps[size_t(d.snap)];
    s.usable = s.usable && d.pcd_whole;
    if (!s.usable) {
        d.snap = -1;
        return;
    }
    std::array<uint64_t, PCD_ENTRY_REGS> now = pcd_entry_regs();
    for (int i = 0; i < PCD_ENTRY_REGS; i++) {
        if (now[size_t(i)] != s.entry[size_t(i)]) {
            pcd.written.set(pcd_entry_addresses[i]);
        }
    }
    for (uint32_t a = 0; a < 256; a++) {
        if (pcd.written.test(a)) {
            f.pcd_writes.push_back({a, gs->reg[a]});
            s.writes_n++;
        }
    }
}

// (nd_register, a picture's draw, before sa_draw_begin) The frame's snapshot of the draw met, when the frame kept one and
// the registers are as the frame's draw began with them: the draw is then made from it (pcd.snap; nd_fetch takes its
// draw and camera, pcd_snapshot_draw its calls)
void pcd_snapshot_find(uint64_t value) {
    pcd.snap = nullptr;
    if (!interp.active || !nd_prep.taking || nd_prep.draw < 0 || !pcd_snapshot_on()) {
        return;
    }
    const NdPrepFrame &f = *nd_prep.taking;
    const NdPrepDraw &d = f.draws[size_t(nd_prep.draw)];
    if (d.snap < 0 || !d.pcd_whole || d.arena_gen != nd.arena_gen) {
        pcd.snap_again[0] += d.pcd_whole;
        return;
    }
    const PcdSnapDraw &s = f.pcd_snaps[size_t(d.snap)];
    // (and no fill waiting for the scene's batch: sat_blend_exact, which a call's registers would decline by)
    if (pcd_entry_regs() != s.entry || sat_fill_waiting()) {
        pcd.snap_again[1]++;
        return;
    }
    pcd.snap = &s;
    pcd.snap_value = value;
}

// (nd_register, the draw's end)
void pcd_snapshot_done() {
    pcd.snap = nullptr;
}

// (nd_draw, sa_draw_begin) The draw and its meshes: the snapshot's draw (no meshes: its calls are kept), or rd_fetch's
bool nd_fetch(uint64_t value, RdDraw &d, RdCamera &cam, std::vector<std::shared_ptr<const RdMeshData>> &meshes) {
    if (pcd.snap) {
        d = pcd.snap->draw;
        cam = pcd.snap->camera;
        meshes.clear();
        return true;
    }
    return rd_fetch(value, d, cam, meshes);
}

// Whether a CLUT load's pages are any target's (clut_load's claim_pages would then write them back, an act among the draws)
bool pcd_clut_targeted(uint64_t tex0) {
    uint32_t lo, count;
    buffer_pages(bits(tex0, 37, 14), 1, bits(tex0, 51, 4) == 0 ? GSN_PSMCT32 : GSN_PSMCT16, 16, 16, &lo, &count);
    for (auto &t : gs->targets) {
        uint32_t tlo, tcount;
        buffer_pages(t->bp, t->bw, t->psm, t->width, t->height, &tlo, &tcount);
        if (pages_overlap(lo, count, tlo, tcount)) {
            return true;
        }
    }
    return false;
}

// The CLUT as the GS and the table have it, kept to undo a snapshot's loads (a draw walked after all)
struct PcdClutState {
    uint16_t gs_clut[512];
    uint32_t gs_cbp[2];
    uint16_t sat_clut[512];
    uint32_t sat_cbp[2];
    uint64_t sat_gs[8], sat_packed[8];
    uint64_t texclut;
};
void pcd_clut_save(PcdClutState &c) {
    memcpy(c.gs_clut, gs->clut, sizeof(c.gs_clut));
    memcpy(c.gs_cbp, gs->clut_cbp, sizeof(c.gs_cbp));
    memcpy(c.sat_clut, sat.clut, sizeof(c.sat_clut));
    memcpy(c.sat_cbp, sat.clut_cbp, sizeof(c.sat_cbp));
    memcpy(c.sat_gs, sat.clut_gs, sizeof(c.sat_gs));
    memcpy(c.sat_packed, sat.clut_packed, sizeof(c.sat_packed));
    c.texclut = gs->reg[R_TEXCLUT];
}
// (the counts are not put back: what was loaded is another CLUT to what keeps by them, which loads again)
void pcd_clut_restore(const PcdClutState &c) {
    memcpy(gs->clut, c.gs_clut, sizeof(c.gs_clut));
    memcpy(gs->clut_cbp, c.gs_cbp, sizeof(c.gs_cbp));
    memcpy(sat.clut, c.sat_clut, sizeof(c.sat_clut));
    memcpy(sat.clut_cbp, c.sat_cbp, sizeof(c.sat_cbp));
    memcpy(sat.clut_gs, c.sat_gs, sizeof(c.sat_gs));
    memcpy(sat.clut_packed, c.sat_packed, sizeof(c.sat_packed));
    gs->reg[R_TEXCLUT] = c.texclut;
    gs->clut_version++;
    sat.clut_gen++;
}

// A kept call's registers with what a picture has of its own as the registers have it now (its context's FRAME and ZBUF
// bases, FRAME's width, the scissor): the key pcd_where works the place out from
PcdKey pcd_key_live(const PcdKey &regs) {
    uint32_t ctxt = (regs.prim >> 9) & 1;
    PcdKey key = regs;
    key.frame |= gs->reg[R_FRAME_1 + ctxt] & (uint64_t(0x1FF) | uint64_t(0x3F) << 16);
    key.zbuf |= gs->reg[R_ZBUF_1 + ctxt] & uint64_t(0x1FF);
    key.scissor = gs->reg[R_SCISSOR_1 + ctxt];
    return key;
}
// Whether pcd_where works the same place out for two keys: the same frame, Z buffer and scissor
bool pcd_where_same(const PcdKey &a, const PcdKey &b) {
    const uint64_t frame = uint64_t(0x1FF) | uint64_t(0x3F) << 16 | uint64_t(0x3F) << 24;
    const uint64_t zbuf = uint64_t(0x1FF) | uint64_t(0xF) << 24;
    return (a.frame & frame) == (b.frame & frame) && (a.zbuf & zbuf) == (b.zbuf & zbuf) && a.scissor == b.scissor;
}

// A snapshot's draw walked after all: its meshes fetched for the walk
bool pcd_snapshot_walk(int why) {
    pcd.snap_again[why]++;
    pcd.snap = nullptr;
    RdDraw d;
    RdCamera cam;
    rd_fetch(pcd.snap_value, d, cam, nd.fetched);
    return false;
}

// (nd_draw, nd280_draw, nd700_draw: a picture's draw, its record written, before its batches) The draw made from the
// frame's snapshot (pcd_snapshot_find), its calls by records' record: true, or false and the draw is walked (its meshes
// in nd.fetched). Nothing is drawn unless all of it is
bool pcd_snapshot_draw(bool pc_draw, const Buffer &records, uint32_t record) {
    if (!pcd.snap) {
        return false;
    }
    if (!pc_draw) {
        return pcd_snapshot_walk(0);
    }
    const PcdSnapDraw &s = *pcd.snap;
    const NdPrepFrame &f = *nd_prep.taking;
    const NdPrepDraw &d = f.draws[size_t(nd_prep.draw)];
    const PcdKept *calls = f.pcd_calls.data() + d.calls_at;
    const PcdLoad *loads = f.pcd_loads.data() + s.loads_at;
    // (the walk draws the GS's vertices pending at its first call, pcd_where, or at a write that changes a register: with
    // the registers and the CLUT it began with either way; with no call, only the walk knows. The scene's kept parts
    // after the loads before the first call, as pcd_where)
    uint32_t first = d.calls_n;
    for (uint32_t i = 0; i < d.calls_n && first == d.calls_n; i++) {
        first = calls[i].what != PCD_CALL_UNKICKED ? i : first;
    }
    if (!gs->verts.empty() && first == d.calls_n) {
        return pcd_snapshot_walk(2);
    }
    for (uint32_t i = 0; i < s.loads_n; i++) {
        if (pcd_clut_targeted(loads[i].tex0)) {
            return pcd_snapshot_walk(3);
        }
    }
    if (first < d.calls_n && !gs->verts.empty()) {
        flush_batch();
    }
    // the CLUT loads, in order among the calls; each call's CLUT checked as the frame's after the loads before it
    PcdClutState saved;
    if (s.loads_n) {
        pcd_clut_save(saved);
    }
    uint32_t next_load = 0;
    auto load_to = [&](uint32_t call) {
        for (; next_load < s.loads_n && loads[next_load].before_call <= call; next_load++) {
            gs->reg[R_TEXCLUT] = loads[next_load].texclut;
            settle_upload();
            clut_load(loads[next_load].tex0);
            sa_clut_load(loads[next_load].tex0);
            pcd.snap_loads++;
        }
    };
    auto undo = [&](int why) {
        if (s.loads_n) {
            pcd_clut_restore(saved);
        }
        return pcd_snapshot_walk(why);
    };
    for (uint32_t i = 0; i < d.calls_n; i++) {
        load_to(i);
        if (i == first) {
            sa_group_draw();
            pc_world_flush();
        }
        const PcdKept &k = calls[i];
        if (k.what == PCD_CALL_DRAWN && psm_is_palette(bits(k.regs.tex0, 20, 6)) && pcd_clut_hash() != k.regs.clut) {
            return undo(4);
        }
    }
    load_to(d.calls_n);
    // the place, once: every drawn call's frame, Z buffer and scissor (its context's, the picture's own bases) the first's
    // (pcd_where's), then each call's place and table
    PcdTake t;
    int where = 2;
    const PcdKept *drawn = nullptr;
    PcdKey key = {};
    for (uint32_t i = 0; i < d.calls_n; i++) {
        const PcdKept &k = calls[i];
        if (k.what != PCD_CALL_DRAWN) {
            continue;
        }
        PcdKey now = pcd_key_live(k.regs);
        if (!drawn) {
            drawn = &k;
            key = now;
        } else if (!pcd_where_same(now, key)) {
            return undo(5);
        }
    }
    if (drawn) {
        where = pcd_where(key, t);
        if (where == 0) {
            return undo(5);
        }
        for (uint32_t i = 0; where == 1 && i < d.calls_n; i++) {
            const PcdKept &k = calls[i];
            if (k.what == PCD_CALL_DRAWN && (!pcd_place_same(k.regs, key) || !pcd_table_same(k))) {
                return undo(5);
            }
        }
    }
    // the calls
    for (uint32_t i = 0; where == 1 && i < d.calls_n; i++) {
        const PcdKept &k = calls[i];
        if (k.what != PCD_CALL_DRAWN) {
            continue;
        }
        t.held = k.mat;
        t.mat = k.mat.get();
        t.regs = k.regs;
        t.vs = k.vs;
        pcd_take_push(t, *k.mat);
        pcd_finish(t, records, k.tris, k.first, k.n, record | k.instance_bits);
        pcd.snap_calls++;
    }
    // the registers as the writes left them
    for (uint32_t i = 0; i < s.writes_n; i++) {
        const std::pair<uint32_t, uint64_t> &w = f.pcd_writes[s.writes_at + i];
        gs->reg[w.first] = w.second;
    }
    gs->queue_count = 0;
    pcd.snap_draws++;
    return true;
}

// (nd_draw, a taken draw's batch, its registers written as the GS gets them) Draws the batch from its mesh (base: its
// first vertex in the arena) by its draw's record, or false: the GS's batch takes it, as before
bool pcd_batch(const RdMeshData &m, const RdBatch &b, uint32_t base, uint32_t record) {
    uint32_t as_min, as_max;
    pcd_alpha_range(m, b, &as_min, &as_max);
    PcdTake t;
    int took = pcd_prepare(PCD_VS_000, as_min, as_max, t);
    uint32_t instance = record | (t.gouraud ? 1u << 30 : 0u);
    if (took != 1) {
        if (took == 2) {
            pcd_keep_call(t, BufferHandle(), 0, 0, instance, PCD_CALL_NOTHING);
        }
        return took == 2;
    }
    uint32_t first = pcd_place(m, b, base);
    uint32_t n = uint32_t(b.triangles.size() / 3);
    pcd_finish(t, *nd.draws.buffer, pcd.tris.buffer, first, n, instance);
    pcd.batches++;
    pcd.triangles += n;
    return true;
}

// Room for n triangles more in pcd.kicked: the first's place (a full buffer is replaced: what a draw reads is never
// written again)
uint32_t pcd_kicked_room(uint32_t n) {
    if (!pcd.kicked.buffer || pcd.kicked.used + n > pcd.kicked.room) {
        nd_new_buffer(pcd.kicked, std::max(pcd.kicked.room ? pcd.kicked.room : 1u << 16, n), 16, false);
    }
    return pcd.kicked.used;
}

// (nd280_draw, a skinned batch's pass, its registers written: the run draws, p its plan) Draws the pass's triangles VU1
// kicks (the plan's ADC) by the draw's record, or false: the GS's batch takes it, as before
bool pcd_pass_280(const RdBatch &b, const Nd280Plan &p, bool sphere, uint32_t base, uint32_t record, const Buffer &records) {
    uint32_t as_min = 255, as_max = 0;
    for (uint32_t i = 0; i < p.n; i++) {
        uint32_t a = sphere ? p.alpha[1] : p.alpha[i];
        as_min = std::min(as_min, a);
        as_max = std::max(as_max, a);
    }
    PcdTake t;
    int took = pcd_prepare(PCD_VS_280, as_min, as_max, t);
    uint32_t instance = record | (t.context2 ? 1u << 28 : 0u) | (sphere ? 1u << 29 : 0u) | (t.gouraud ? 1u << 30 : 0u);
    if (took != 1) {
        if (took == 2) {
            pcd_keep_call(t, BufferHandle(), 0, 0, instance, PCD_CALL_NOTHING);
        }
        return took == 2;
    }
    // the triangles VU1 kicks, each the arena's slots of its corners and of its kick
    uint32_t n = 0;
    for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
        n += !p.adc[b.triangles[i + 2]];
    }
    uint32_t first = pcd_kicked_room(n);
    uint32_t *out = reinterpret_cast<uint32_t *>(pcd.kicked.map) + size_t(first) * 4;
    uint32_t slot0 = base + ND280_SLOTS * b.first;
    for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
        uint32_t kick = b.triangles[i + 2];
        if (p.adc[kick]) {
            continue;
        }
        out[0] = slot0 + ND280_SLOTS * b.triangles[i];
        out[1] = slot0 + ND280_SLOTS * b.triangles[i + 1];
        out[2] = slot0 + ND280_SLOTS * kick;
        out[3] = slot0 + ND280_SLOTS * kick;
        out += 4;
    }
    pcd.kicked.used += n;
    pcd_finish(t, records, pcd.kicked.buffer, first, n, instance);
    pcd.passes280++;
    pcd.triangles280 += n;
    return true;
}

// A triangle's Z slope from its corners' places (FTOI4's X, Y and Z), as sa_slope has it by the world (a layer's colour
// pass, pc.zslope below 0): 32 log2(1 + g), g the Z's change across a GS pixel
uint32_t pcd_slope_world(const int32_t *a, const int32_t *b, const int32_t *c) {
    auto x = [](const int32_t *v) { return float(int32_t(uint32_t(v[0]) & 0xFFFF)) / 16.0f; };
    auto y = [](const int32_t *v) { return float(int32_t(uint32_t(v[1]) & 0xFFFF)) / 16.0f; };
    auto z = [](const int32_t *v) { return float(uint32_t(v[2] >> 4) & 0xFFFFFF); };
    float ax = x(b) - x(a), ay = y(b) - y(a), bx = x(c) - x(a), by = y(c) - y(a), az = z(b) - z(a), bz = z(c) - z(a);
    float det = ax * by - ay * bx;
    if (det == 0.0f) {
        return 0;
    }
    float dzdx = (az * by - bz * ay) / det, dzdy = (bz * ax - az * bx) / det;
    float g = sqrtf(dzdx * dzdx + dzdy * dzdy);
    return uint32_t(std::min(32.0f * log2f(1.0f + g) + 0.5f, 255.0f));
}

// (nd700_draw, an effect's strip that is kicked, its registers written: p its plan, alpha every corner's) Draws the
// triangles VU1 kicks (the plan's ADC) by the draw's record, or false: the GS's batch takes it, as before
bool pcd_strip_700(const RdBatch &b, const Nd700Plan &p, uint32_t alpha, uint32_t base, uint32_t record) {
    PcdTake t;
    int took = pcd_prepare(PCD_VS_700, alpha, alpha, t);
    uint32_t instance = record | (t.gouraud ? 1u << 30 : 0u);
    if (took != 1) {
        if (took == 2) {
            pcd_keep_call(t, BufferHandle(), 0, 0, instance, PCD_CALL_NOTHING);
        }
        return took == 2;
    }
    uint32_t n = 0;
    for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
        n += !p.adc[b.triangles[i + 2]];
    }
    uint32_t first = pcd_kicked_room(n);
    uint32_t *out = reinterpret_cast<uint32_t *>(pcd.kicked.map) + size_t(first) * 4;
    uint32_t at = base + b.first;
    bool sloped = t.push.zslope < 0.0f; // (sa_draw_batch's world layers; a Z-rule slope is not the effects')
    for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
        uint32_t t0 = b.triangles[i], t1 = b.triangles[i + 1], kick = b.triangles[i + 2];
        if (p.adc[kick]) {
            continue;
        }
        uint32_t slope = sloped ? pcd_slope_world(p.xyz[t0], p.xyz[t1], p.xyz[kick]) : 0;
        out[0] = at + t0;
        out[1] = at + t1;
        out[2] = at + kick;
        out[3] = (at + kick) | slope << 24;
        out += 4;
    }
    pcd.kicked.used += n;
    pcd_finish(t, *nd.draws.buffer, pcd.kicked.buffer, first, n, instance);
    pcd.strips700++;
    pcd.triangles700 += n;
    return true;
}

// (nd280_draw, nd700_draw: a batch the draw does not kick, or all of whose vertices are clipped) Its call: kept in the
// frame, or the frame's next one in a picture
void pcd_unkicked() {
    if (interp.active) {
        pcd_between_unkicked();
    } else {
        pcd_keep_unkicked();
    }
}

void pcd_print_stats() {
    fprintf(stderr, "[stats] pc draws: %llu draws, %llu batches (%llu triangles) drawn from their meshes in %llu draw calls "
            "(%llu batches made the last one longer), %llu nothing; materials resolved %llu, found again %llu; ",
            (unsigned long long)pcd.draws, (unsigned long long)pcd.batches, (unsigned long long)pcd.triangles,
            (unsigned long long)pcd.calls, (unsigned long long)pcd.grown, (unsigned long long)pcd.nothing,
            (unsigned long long)pcd.resolved, (unsigned long long)pcd.found);
    fprintf(stderr, "drawn from their plans: skinned passes %llu (%llu triangles), effects' strips %llu (%llu triangles); "
            "declined to the GS's batches:",
            (unsigned long long)pcd.passes280, (unsigned long long)pcd.triangles280, (unsigned long long)pcd.strips700,
            (unsigned long long)pcd.triangles700);
    for (int i = 0; i < PCD_WHY_COUNT; i++) {
        if (pcd.declined[i]) {
            fprintf(stderr, " %s %llu", pcd_why_names[i], (unsigned long long)pcd.declined[i]);
        }
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "[stats] pc draws, pictures between (KF4_PC_KEPT): %llu draws (%llu calls) made again from the frame's "
            "kept calls; worked out again: %llu no frame kept, %llu not whole, %llu the table or arena changed, %llu from a "
            "call on (its registers differed, or it declined)\n",
            (unsigned long long)pcd.kept_draws, (unsigned long long)pcd.kept_calls, (unsigned long long)pcd.kept_again[0],
            (unsigned long long)pcd.kept_again[1], (unsigned long long)pcd.kept_again[2], (unsigned long long)pcd.kept_again[3]);
    fprintf(stderr, "[stats] pc draws, snapshot (KF4_PC_SNAPSHOT): %llu draws (%llu calls, %llu CLUT loads) made from the "
            "frame's snapshot without the walk; walked: %llu not kept so, %llu registers at its start, %llu vertices "
            "pending, %llu a CLUT on a target, %llu a CLUT not the frame's, %llu the place or the table's answer; the frame's "
            "draws kept whole without one: %llu a write (last 0x%x), %llu nothing for its place\n",
            (unsigned long long)pcd.snap_draws, (unsigned long long)pcd.snap_calls, (unsigned long long)pcd.snap_loads,
            (unsigned long long)pcd.snap_again[0], (unsigned long long)pcd.snap_again[1], (unsigned long long)pcd.snap_again[2],
            (unsigned long long)pcd.snap_again[3], (unsigned long long)pcd.snap_again[4], (unsigned long long)pcd.snap_again[5],
            (unsigned long long)pcd.snap_unkept[0], pcd.snap_unkept_address, (unsigned long long)pcd.snap_unkept[1]);
}
