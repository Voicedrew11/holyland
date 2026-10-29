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
// A picture between walks the frame's packets again as before (frame_interp.c), and its draws here are the frame's with
// the picture's record (ND_C_MOVING and the models' moves): the same few calls, which the replays move on the GPU.
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

// What a batch's material is resolved from: the GS's registers it is drawn with, its vertices' alphas, and what the
// answer can change with (the table, the CLUT, the scene's shape; memcmp-able: memset first)
struct PcdKey {
    uint64_t frame, zbuf, test, alpha, scissor, tex0, tex1, clamp, miptbp1, miptbp2, texa, fogcol;
    uint32_t vs, prim, fba, colclamp, pabe, scanmsk, as_min, as_max;
    uint64_t clut; // (the CLUT mirror's contents, hashed: pcd_clut_hash)
    uint32_t sx, sy, samples, wide, width, height, fcopy;
    int32_t lod, cutout, aniso, pad;
};
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
    std::unordered_map<PcdKey, PcdMaterial, PcdKeyHash, PcdKeyEqual> materials;
    uint64_t materials_gen = 0;           // (sat.gen when they were resolved)
    uint64_t clut_gen = 0, clut_hash = 0; // (pcd_clut_hash's: sat.clut_gen when it hashed the mirror)
    PcdBound bound;
    uint64_t draws = 0, batches = 0, triangles = 0, nothing = 0, declined[PCD_WHY_COUNT] = {};
    uint64_t passes280 = 0, triangles280 = 0, strips700 = 0, triangles700 = 0;
    uint64_t resolved = 0, found = 0, calls = 0, grown = 0;
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

bool pcd_decline(int why) {
    pcd.declined[why]++;
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
void pcd_draw(const PcdMaterial &mat, const Push &push, const VkViewport &viewport, const VkRect2D &scissor,
              const Buffer &records, const Buffer &tris, uint32_t first, uint32_t n, uint32_t instance) {
    PcdBound &b = pcd.bound;
    const ImageView *view = mat.image ? &mat.image->get_view() : &gs->dummy_rgba->get_view();
    const ImageView *raw = mat.raw ? mat.raw : &gs->dummy_raw->get_view();
    const ImageView *target = mat.target ? mat.target : &gs->dummy_rgba->get_view();
    bool clut_same = b.want_clut == mat.want_clut && (!mat.want_clut || b.clut == mat.clut);
    uint32_t fix = bits(gs->reg[R_ALPHA_1 + ((effective_prim() >> 9) & 1)], 32, 8);
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

// A batch (or pass) the PC renderer takes, prepared: its material and where it is drawn
struct PcdTake {
    const PcdMaterial *mat = nullptr;
    Push push = {};
    VkViewport viewport = {};
    VkRect2D scissor = {};
    uint32_t sy0 = 0, y1 = 0;
    bool gouraud = false, context2 = false;
};

// (a taken draw's batch, its registers written as the GS gets them) The material of the batch about to be drawn with the
// GS's registers now, its vertices' alphas as_min to as_max, by vertex program vs; what came before it drawn and the scene's
// frame taken. 0: declined (the GS's batch takes it, as before); 1: prepared (t); 2: nothing to draw (ZTST NEVER, an
// empty scissor)
int pcd_prepare(int vs, uint32_t as_min, uint32_t as_max, PcdTake &t) {
    uint32_t prim = effective_prim();
    uint32_t type = prim & 7;
    if (type < 3 || type > 5) {
        return pcd_decline(PCD_WHY_PRIM);
    }
    uint32_t ctxt = (prim >> 9) & 1;
    bool abe = (prim >> 6) & 1;
    PcdKey key;
    memset(&key, 0, sizeof(key));
    key.vs = uint32_t(vs);
    key.prim = prim & 0x7FF;
    key.frame = gs->reg[R_FRAME_1 + ctxt];
    key.zbuf = gs->reg[R_ZBUF_1 + ctxt];
    key.test = gs->reg[R_TEST_1 + ctxt];
    key.alpha = gs->reg[R_ALPHA_1 + ctxt];
    key.scissor = gs->reg[R_SCISSOR_1 + ctxt];
    uint32_t fbp = bits(key.frame, 0, 9) * 32, fbw = bits(key.frame, 16, 6), fpsm = bits(key.frame, 24, 6);
    uint32_t zbp = bits(key.zbuf, 0, 9) * 32, zpsm = bits(key.zbuf, 24, 4) | 0x30;
    uint32_t sx0 = bits(key.scissor, 0, 11), sx1 = bits(key.scissor, 16, 11);
    uint32_t sy0 = bits(key.scissor, 32, 11), sy1 = bits(key.scissor, 48, 11);
    // (sa_draw_batch's: the scene's frame, or a named target's, T3)
    if (((fpsm != GSN_PSMCT32 && fpsm != GSN_PSMCT24) || fbw * 64 < 512 || sx1 < sx0 || sy1 < sy0) &&
        !(sat_scene_frame(fbw, fpsm) && sx1 >= sx0 && sy1 >= sy0)) {
        return pcd_decline(PCD_WHY_FRAME);
    }
    uint64_t test = key.test;
    bool zte = bits(test, 16, 1), date = bits(test, 14, 1);
    uint32_t ztst = zte ? bits(test, 17, 2) : 1;
    // (one pass of the alpha test, sa_draw_batch's)
    if (date || pcd_passes(test, key.zbuf, key.frame, nullptr) > 1) {
        return pcd_decline(PCD_WHY_TEST);
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
        return pcd_decline(PCD_WHY_BLEND);
    }
    pcd_programs();
    if (!pcd.programs[vs][0]) {
        return pcd_decline(PCD_WHY_PROGRAM);
    }

    // What came before it in the chain is drawn first: the GS's batch, a skinned model's parts, the PC world's kept
    if (!gs->verts.empty()) {
        flush_batch();
    }
    sa_group_draw();
    pc_world_flush();
    if (interp.active && sat_kept(fbp, fbw, fpsm)) {
        sa_stats.kept_reflection++;
        return 2; // (sa_flush's: a picture between leaves the reflection as the frame drew it)
    }
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
        return 2; // (an empty scissor: nothing)
    }

    // the material: resolved once while the table, the CLUT and the registers stay the same
    if (pcd.materials_gen != sat.gen || pcd.materials.size() > 4096) {
        pcd.materials.clear();
        pcd.materials_gen = sat.gen;
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
    const PcdMaterial *found_mat = nullptr;
    auto found = pcd.materials.find(key);
    if (found != pcd.materials.end()) {
        found_mat = &found->second;
        pcd.found++;
    } else {
        pcd.unkept = pcd_resolve(key, ctxt);
        pcd.resolved++;
        found_mat = &pcd.unkept;
        if (pcd.unkept.kept) {
            found_mat = &pcd.materials.emplace(key, std::move(pcd.unkept)).first->second;
        }
    }
    const PcdMaterial &mat = *found_mat;
    if (mat.why >= 0) {
        return pcd_decline(mat.why);
    }
    if (mat.image) {
        rgba.batches++;
    }
    t.mat = &mat;
    t.push = mat.push;
    t.push.inv_size[0] = 1.0f / float(color->width);
    t.push.inv_size[1] = 1.0f / float(color->height);
    if (sa_jitter_draw()) {
        t.push.misc |= M_JITTER;
    }
    t.viewport = {0.0f, 0.0f, float(color->width * sa.sx), float(color->height * sa.sy), 0.0f, 1.0f};
    t.scissor = {{int32_t(sx0 * sa.sx), int32_t(sy0 * sa.sy)}, {(x1 - sx0) * sa.sx, (y1 - sy0) * sa.sy}};
    t.sy0 = sy0;
    t.y1 = y1;
    t.gouraud = (prim >> 3) & 1;
    t.context2 = ctxt != 0;
    return 1;
}

// The prepared batch's n triangles from first in tris, by records' instance, into the scene's colour and depth; and what
// sa_draw_batch notes of a batch drawn
void pcd_finish(const PcdTake &t, const Buffer &records, const Buffer &tris, uint32_t first, uint32_t n, uint32_t instance) {
    if (n) {
        pcd_draw(*t.mat, t.push, t.viewport, t.scissor, records, tris, first, n, instance);
    }
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

// (nd_draw, a taken draw's batch, its registers written as the GS gets them) Draws the batch from its mesh (base: its
// first vertex in the arena) by its draw's record, or false: the GS's batch takes it, as before
bool pcd_batch(const RdMeshData &m, const RdBatch &b, uint32_t base, uint32_t record) {
    uint32_t as_min, as_max;
    pcd_alpha_range(m, b, &as_min, &as_max);
    PcdTake t;
    int took = pcd_prepare(PCD_VS_000, as_min, as_max, t);
    if (took != 1) {
        return took == 2;
    }
    uint32_t first = pcd_place(m, b, base);
    uint32_t n = uint32_t(b.triangles.size() / 3);
    pcd_finish(t, *nd.draws.buffer, *pcd.tris.buffer, first, n, record | (t.gouraud ? 1u << 30 : 0u));
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
    if (took != 1) {
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
    uint32_t instance = record | (t.context2 ? 1u << 28 : 0u) | (sphere ? 1u << 29 : 0u) | (t.gouraud ? 1u << 30 : 0u);
    pcd_finish(t, records, *pcd.kicked.buffer, first, n, instance);
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
    if (took != 1) {
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
    pcd_finish(t, *nd.draws.buffer, *pcd.kicked.buffer, first, n, record | (t.gouraud ? 1u << 30 : 0u));
    pcd.strips700++;
    pcd.triangles700 += n;
    return true;
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
}
