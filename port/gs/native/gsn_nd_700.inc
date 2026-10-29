// ---- Native draw: VU1's program 0x700, the effects (P4-1) ------------------------------------------------
// Included at the end of gsn_native_draw.inc (inside gsn.cpp's namespace), whose top says how a draw goes;
// this is the same for a draw of program 0x700 (RdDraw.program 0x700: the effects, the sfx.dat models, their
// meshes RD_VF_PACKED16 as 0x000's: render.cpp, take_batch). port/platform/vu1_hle_700.c is the
// specification, shaders/nd_700.vert the vertex program. What differs from 0x000's:
// - no vertex colour, no point lights: one colour for the model (VU1 0x64, vf28 after the set-up), so the
//   alpha of every corner is the draw's (nd700_alpha);
// - the texture animation: when the A+D packet's TEX0 (TOP+0xA2, its second write) has the TBP0 that VU1
//   0x65.w names, the qword at 0x66 is written instead (the flipbook's frame; nd700_ad);
// - the loop runs n + 1 times: a "vertex n" past the strip (the position qword at TOP+1+4n, RdBatch::past) is
//   clip-tested too and counted among the clipped, and the strip (PRIM and its triangles) is kicked only when
//   fewer than n of the n + 1 are clipped (the A+D packet always is). Vertex n can decide it only when
//   exactly one vertex of the strip is unclipped, and then the strip draws nothing (every triangle has a
//   clipped corner within its three kicks): what it changes is whether PRIM is written. A block that did not
//   write that qword itself leaves it unknown here (every block of the game's effects: the oracle over the
//   tour's 2,341 runs of 0x700), and it is taken as clipped (the strip not kicked): the one run of them with a
//   single vertex unclipped (map 037) had vertex n clipped;
// - "vertex -1" (the qword at the run before's vi07 + 1) only matters with a tag whose w is set, which the
//   parser refuses.
// The corners are nd_000.vert's, in the same buffers: the arena, and nd.draws' records (NdConst: the model's
// colour in point_col[0]), so a batch holds 0x000's, 0x700's and the PS2 path's corners together as the PS2
// path's batch would (mixed batches). Which records are 0x700's is kept here (nd700.of), and
// nd_draw_range draws each stretch of one program with its own (nd700_corner, nd700_program).
// Pictures between: as the models' kinds (nd_model_flags, P2-5), paired by identity (RD_KIND_EFFECT: the
// effect's model and part) and moved by their own matrices; nd_700.vert takes the depth from the
// position's w (16).

struct Nd700State {
    std::vector<uint8_t> of;          // of nd.draws' records (in the buffer `records`): 1 for a draw of 0x700
    const Buffer *records = nullptr;
    std::unordered_map<Program *, Program *> programs;
    Shader *vs = nullptr;
} nd700;

// (nd_new_buffer: nd.draws replaced) the flags go with the old buffer
void nd700_records_gone() {
    nd700.of.clear();
    nd700.records = nullptr;
}

// (nd_draw_range, the hook) whether a native corner is of a 0x700 draw (its record's)
bool nd700_corner(const Vtx &v) {
    if (nd_corner_kind(v) != ND_KIND_000 || nd700.records != nd.batch_draws) {
        return false;
    }
    uint32_t record = (v.rgba & 0xFFFFFFu) >> ND_CORNER_DRAW_SHIFT;
    return record < nd700.of.size() && nd700.of[record];
}

// (nd_draw_range, the hook: a stretch of 0x700 corners) the GS's program p with nd_700.vert
Program *nd700_program(Program *p) {
    if (!p) {
        return p;
    }
    auto it = nd700.programs.find(p);
    if (it != nd700.programs.end()) {
        return it->second;
    }
    if (!nd700.vs) {
        ResourceLayout vert = {};
        vert.input_mask = (1u << 2) | (1u << 4);
        vert.push_constant_size = sizeof(Push);
        vert.sets[1].storage_buffer_mask = 1u;
        vert.sets[1].meta[0].array_size = 1;
        vert.sets[2].storage_buffer_mask = 3u;
        vert.sets[2].meta[0].array_size = 1;
        vert.sets[2].meta[1].array_size = 1;
        nd700.vs = gs->device->request_shader(gsn_spv_nd_700_vert, sizeof(gsn_spv_nd_700_vert), &vert);
    }
    Program *np = gs->device->request_program(nd700.vs, p->get_shader(ShaderStage::Fragment));
    nd700.programs[p] = np;
    return np;
}

// The alpha of every vertex as VU1 leaves it and the GS takes it: lit.w (0 + 1) by the colour's w, at most
// 255 (MINI), FTOI0; the low 8 bits
uint32_t nd700_alpha(const float colour[4]) {
    uint32_t a = vu_float_mul(0x3F800000u, vu_float_bits(colour[3]));
    return uint32_t(vu_float_ftoi(vu_float_mini(a, 0x437F0000u), 0)) & 0xFF;
}

// The A+D writes the run kicks (register, value ...): the batch's, with TEX0 replaced by VU1 0x66 when its
// TBP0 is 0x65.w (vu1_hle_700.c: TOP+0xA2.x & 0x3FFF against 0x65.w's low 16 bits; the qword at 0x66 put at
// TOP+0xA2 whole, its register and value). 1 if it was replaced
bool nd700_ad(const RdBatch &b, const float c65[4], const float c66[4], std::vector<uint64_t> &out) {
    out = b.ad;
    if (b.ad.size() < 4) {
        return false; // (TOP+0xA2 is not in the packet: not kicked)
    }
    uint32_t w65 = vu_float_bits(c65[3]);
    if (uint16_t(b.ad[3] & 0x3FFF) != uint16_t(w65)) {
        return false;
    }
    uint32_t q[4];
    for (int i = 0; i < 4; i++) {
        q[i] = vu_float_bits(c66[i]);
    }
    out[2] = q[2] & 0xFF;
    out[3] = uint64_t(q[0]) | uint64_t(q[1]) << 32;
    return true;
}

// A batch planned (vu1_hle_700.c's run): the places, the clipping and ADC of each vertex, vertex n's clipping
// and whether the strip is kicked
struct Nd700Plan {
    uint32_t n = 0;
    int32_t xyz[N_ND_MAX][3];
    float stq[N_ND_MAX][3];
    bool clip[N_ND_MAX], adc[N_ND_MAX]; // adc: clipping's (the kick or either of the two before it is clipped)
    bool clip_past = false;             // vertex n's (true when not known)
    uint32_t unclipped = 0;             // of the strip's n
    bool kick = false;
};

void nd700_plan(const RdMeshData &m, const RdBatch &b, const float *vm, Nd700Plan &p) {
    p.n = b.count;
    bool vm_ok = nd_fast_matrix(vm);
    p.unclipped = 0;
    for (uint32_t k = 0; k < p.n; k++) {
        nd_plan_vertex(&m.vertices[size_t(b.first + k) * 16], vm, p.xyz[k], p.stq[k], p.clip[k], vm_ok);
        p.unclipped += !p.clip[k];
    }
    p.clip_past = true;
    if (b.past_known) {
        int32_t q[16] = {b.past[0], b.past[1], b.past[2], b.past[3]};
        int32_t xyz[3];
        float stq[3];
        nd_plan_vertex(q, vm, xyz, stq, p.clip_past, vm_ok);
    }
    // (vi06: n less the clipped of the n + 1 vertices, kicked when above 0)
    p.kick = int32_t(p.unclipped) - int32_t(p.clip_past) > 0;
    for (uint32_t k = 0; k < p.n; k++) {
        p.adc[k] = p.clip[k] || (k >= 1 && p.clip[k - 1]) || (k >= 2 && p.clip[k - 2]);
    }
}

// The plan's corners into gs->verts (as nd_plan_batch's; the alpha the draw's): base the batch's first vertex
// in the arena
void nd700_corners(const RdBatch &b, const Nd700Plan &p, uint32_t prim, uint32_t alpha, uint32_t base, uint32_t record) {
    uint64_t offset = gs->reg[R_XYOFFSET_1 + ((prim >> 9) & 1)];
    int32_t ox = int32_t(bits(offset, 0, 16)), oy = int32_t(bits(offset, 32, 16));
    uint32_t gouraud = ((prim >> 3) & 1) ? ND_CORNER_GOURAUD : 0;
    for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
        uint32_t t[3] = {b.triangles[i], b.triangles[i + 1], b.triangles[i + 2]};
        uint32_t kick = t[2];
        if (p.adc[kick]) {
            continue;
        }
        for (int j = 0; j < 3; j++) {
            Vtx w;
            w.x = float(int32_t(uint32_t(p.xyz[t[j]][0]) & 0xFFFF) - ox) / 16.0f;
            w.y = float(int32_t(uint32_t(p.xyz[t[j]][1]) & 0xFFFF) - oy) / 16.0f;
            w.z = uint32_t(p.xyz[t[j]][2] >> 4) & 0xFFFFFF;
            w.rgba = alpha << 24 | (kick - t[j]) | gouraud | record << ND_CORNER_DRAW_SHIFT;
            w.s = p.stq[t[j]][0];
            w.t = p.stq[t[j]][1];
            w.q = p.stq[t[j]][2];
            w.fog = nd_corner_vertex(base + b.first + t[j]);
            gs->verts.push_back(w);
            if (interp.active) {
                interp.vsrc.push_back({GSN_IM_NONE, GSN_IM_NONE, GSN_IM_NONE, uint32_t(j)});
            }
        }
    }
}

// A draw's record: nd_const's (VU1 0x700's set-up is 0x000's for the vertex matrix and the light
// directions: view * model, the lights by the model's columns), no point lights, the model's colour (0x64)
// in point_col[0]
void nd700_const(NdConst &k, const RdDraw &d, const RdCamera &cam) {
    RdDraw e = d;
    e.point_count = 0;
    nd_const(k, e, cam);
    memset(k.point_pos, 0, sizeof(k.point_pos));
    memset(k.point_col, 0, sizeof(k.point_col));
    memcpy(k.point_col, d.colour[0], 16);
    k.info[0] = 0;
}

// (nd_draw, the hook: a draw of program 0x700) as nd_draw draws 0x000's
// (--renderer fast: a strip drawn by the PC renderer, gsn_pc_draw.inc)
bool pcd_strip_700(const RdBatch &b, const Nd700Plan &p, uint32_t alpha, uint32_t base, uint32_t record);
bool sa_pcd_draw();

void nd700_draw(const RdDraw &d, const RdCamera &cam) {
    gsn_stats.nd_draws++;
    bool pc_draw = sa_pcd_draw(); // (--renderer fast: gsn_pc_draw.inc)
    if (nd_buffer_room(nd.draws, 1, nd.pending_000)) {
        nd_new_buffer(nd.draws, 4096, sizeof(NdConst), nd.pending_000);
        nd.boxes.clear();
    }
    uint32_t record = nd.draws.used++;
    if (nd700.records != nd.draws.buffer.get()) {
        nd700.of.clear();
        nd700.records = nd.draws.buffer.get();
    }
    if (nd700.of.size() <= record) {
        nd700.of.resize(record + 1, 0);
    }
    nd700.of[record] = 1;
    NdConst k;
    nd700_const(k, d, cam);
    uint64_t offset = gs->reg[R_XYOFFSET_1 + ((effective_prim() >> 9) & 1)];
    k.info[1] = int32_t(bits(offset, 0, 16));
    k.info[2] = int32_t(bits(offset, 32, 16));
    // pictures between: paired by identity and moved by their own matrices, as the models' kinds (P2-5)
    NdKey key = nd_key(d);
    memset(k.obj, 0, sizeof(k.obj));
    memset(&k.bt, 0, sizeof(k.bt));
    memset(k.wave, 0, sizeof(k.wave)); // (0x000's wave UVs: not 0x700's)
    k.info[3] = 0;
    if (!interp.active) {
        nd_remember(key, d, cam);
    } else {
        k.info[3] = nd_model_flags(key, d, cam, &k.bt, nullptr);
    }
    memcpy(nd.draws.map + size_t(record) * sizeof(NdConst), &k, sizeof(k));
    if (interp.active) {
        nd.boxes.resize(record + 1); // (nd_box_at_0: ND_C_MODEL, the whole target)
        nd.boxes[record] = {k.info[3], k.screen[0], k.screen[5], k.screen[8], k.screen[9], float(k.info[1]) / 16.0f,
                            float(k.info[2]) / 16.0f, {k.obj[0], k.obj[1], k.obj[2]}};
    }
    uint32_t alpha = nd700_alpha(d.colour[0]);
    if (pc_draw) {
        pcd_draw_begin(); // (gsn_pc_draw.inc: the frame keeps its calls; a picture takes the frame's, when it kept them)
    }
    Nd700Plan p;
    std::vector<uint64_t> ad;
    for (const auto &mesh : nd.fetched) {
        if (mesh->format != RD_VF_PACKED16) {
            gsn_stats.nd_unsupported++;
            if (pc_draw) {
                pcd_keep_fail(); // (not kept whole: the GS's batches draw what the PC renderer did not)
            }
            continue;
        }
        uint32_t base = nd_place(*mesh);
        for (const RdBatch &b : mesh->batches) {
            if (b.start != RD_PROGRAM_CONTINUE && b.start != 0x700) {
                gsn_stats.nd_unsupported++; // (the adapter sends only meshes of 0x700's: rd_mesh_runs_program)
                if (pc_draw) {
                    pcd_keep_fail(); // (not kept whole)
                }
                continue;
            }
            // what the run kicks: FOGCOL at an MSCAL (the set-up), the A+D packet (TEX0 perhaps replaced),
            // then the strip unless n of the n + 1 vertices were clipped
            if (b.start != RD_PROGRAM_CONTINUE) {
                gs_write(R_FOGCOL, d.fogcol);
            }
            nd700_ad(b, d.colour[1], d.colour[2], ad);
            for (size_t i = 0; i + 1 < ad.size(); i += 2) {
                gs_write(uint32_t(ad[i]), ad[i + 1]);
            }
            // (a picture's kept call of this strip, made again: whether it is kicked is the frame's, and so is its plan's
            // triangles; the plan is not made)
            if (const PcdKept *kept = pc_draw ? pcd_between_next() : nullptr) {
                if (kept->what == PCD_CALL_UNKICKED) {
                    gsn_stats.nd_batches++;
                    continue;
                }
                gs_write(R_PRIM, uint32_t((b.tag[0] >> 47) & 0x7FF));
                if (pcd_between_call(*kept, *nd.draws.buffer, record)) {
                    gsn_stats.nd_batches++;
                    continue;
                }
            }
            {
                Timer plan(gsn_stats.ns_nd_plan);
                nd700_plan(*mesh, b, k.vertex, p);
            }
            gsn_stats.nd_batches++;
            gsn_stats.nd_plan_vertices += b.count;
            if (!p.kick) {
                if (pc_draw) {
                    pcd_unkicked(); // (gsn_pc_draw.inc: its call, kept or the frame's)
                }
                continue;
            }
            gs_write(R_PRIM, uint32_t((b.tag[0] >> 47) & 0x7FF));
            if (pc_draw && pcd_strip_700(b, p, alpha, base, record)) {
                continue; // (drawn by the PC renderer from the mesh: gsn_pc_draw.inc)
            }
            // (as nd_draw's: the strip joins the batch, 0x000's and the PS2 path's corners in it too; a batch of
            // other buffers is drawn first, and in a picture between a batch of the PS2 path's: nd_kick)
            if (!gs->verts.empty() &&
                ((interp.active && !nd.pending) ||
                 (nd.pending && (ndw_pending() || nd.batch_arena != nd.arena.buffer.get() ||
                                 (nd.pending_000 && nd.batch_draws != nd.draws.buffer.get()))))) {
                flush_batch();
            }
            uint32_t eprim = effective_prim();
            if (gs->verts.empty()) {
                gs->batch_prim = eprim;
                gs->batch_sprites = false;
            }
            size_t before = gs->verts.size();
            nd700_corners(b, p, eprim, alpha, base, record);
            if (gs->verts.size() > before) {
                nd.pending = nd.pending_000 = true;
            }
            nd.batch_arena = nd.arena.buffer.get();
            nd.batch_draws = nd.draws.buffer.get();
        }
    }
    if (pc_draw) {
        pcd_draw_end();
    }
    gs->queue_count = 0;
}

// ---- The vertex oracle for program 0x700 (kf4-gsreplay --nd-oracle: tools/gsreplay/nd_oracle_700.c) ------
// As gsn_nd_280.inc's for 0x280: nd_700.vert built with ND_ORACLE writes what each corner gives the GS; the
// batch is made by the parser's own code from VU1 memory as the run found it (rd_batch_from_vu: vertex n
// known), the record by the CPU's set-up (an MSCAL's, from the fixed block) or from the registers the set-up
// before left (an MSCNT's), the plan, the A+D writes and the kick by nd700_plan and nd700_ad.

} // namespace (gsn.cpp's: its C entry points below are outside it)
#include "gsn_nd700_oracle.h"
namespace {

void nd700_oracle_const(NdConst &k, RdDraw &d, const GsnNd700OracleDraw &o, GsnNd700OracleBatch &r) {
    memset(&k, 0, sizeof(k));
    d = RdDraw{};
    memcpy(d.colour[1], o.mem[0x65], 16);
    memcpy(d.colour[2], o.mem[0x66], 16);
    if (o.mscal) {
        RdCamera cam = {};
        memcpy(cam.world_screen, o.mem[0x00], 64);
        memcpy(d.model, o.mem[0x04], 64);
        memcpy(d.light_dir, o.mem[0x0C], 64);
        memcpy(d.light_col, o.mem[0x10], 64);
        memcpy(d.fog, o.mem[0x3C], 16);
        memcpy(d.colour[0], o.mem[0x64], 16);
        nd700_const(k, d, cam);
        memcpy(r.setup_vertex, k.vertex, 64);
        memcpy(r.setup_light_dir, k.light_dir, 48);
    } else {
        memcpy(k.vertex, o.vf[1], 64);
        memcpy(k.light_dir, o.vf[5], 48);
        memcpy(k.light_col, o.vf[8], 64);
        memcpy(d.colour[0], o.vf[28], 16);
        memcpy(k.point_col, o.vf[28], 16);
    }
    memcpy(k.fog, o.mem[0x3C], 16);
}

bool nd700_oracle_batch(const GsnNd700OracleDraw &o, RdMeshData &mesh) {
    static uint32_t mem[1024][4];
    memset(mem, 0, sizeof(mem));
    for (uint32_t i = 0; i < GSN_ND700_ORACLE_BATCH_QWORDS; i++) {
        memcpy(mem[(o.top + i) & 0x3FF], o.batch[i], 16);
    }
    mesh = RdMeshData();
    return rd_batch_from_vu(mem, o.top, o.mscal ? 0x700 : RD_PROGRAM_CONTINUE, mesh);
}

// The blocks VIF1 was sent, parsed (gsn_nd700_oracle_block), for the runs they start
struct {
    RdMeshData mesh;
    size_t next = 0;
} nd700_block;

} // namespace

extern "C" int gsn_nd700_oracle_block(const void *words, uint32_t qwords, uint32_t program, const char **why) {
    RdMeshData &m = nd700_block.mesh;
    m = RdMeshData();
    nd700_block.next = 0;
    *why = "";
    if (!rd_parse_block(words, qwords, RD_VF_PACKED16, m)) {
        *why = rd_mesh_why();
        m = RdMeshData();
        return 0;
    }
    // (only a block whose runs are 0x700's: an MSCAL 0x700, or an MSCNT that goes on with it)
    for (const RdBatch &b : m.batches) {
        if ((b.start == RD_PROGRAM_CONTINUE ? (&b == &m.batches[0] ? program : 0x700u) : b.start) != 0x700) {
            m = RdMeshData();
            return 0;
        }
    }
    return int(m.batches.size());
}

extern "C" int gsn_nd700_oracle_block_check(const GsnNd700OracleDraw *run, int *past, const char **what) {
    RdMeshData &m = nd700_block.mesh;
    *past = 0;
    if (nd700_block.next >= m.batches.size()) {
        return -1;
    }
    const RdBatch &a = m.batches[nd700_block.next++];
    RdMeshData own;
    *what = "";
    if (!nd700_oracle_batch(*run, own)) {
        *what = "the run's own batch is refused";
        return 0;
    }
    const RdBatch &b = own.batches[0];
    // vertex n: 1 the block knows it and has it right, 2 the block does not know it, 3 known but not VU1's
    *past = !a.past_known ? 2 : memcmp(a.past, b.past, 16) == 0 ? 1 : 3;
    if (a.start != b.start) {
        *what = "MSCAL or MSCNT";
    } else if (memcmp(a.tag, b.tag, 16) != 0) {
        *what = "the tag";
    } else if (a.ad != b.ad) {
        *what = "the A+D packet";
    } else if (a.count != b.count || a.triangles != b.triangles) {
        *what = "the triangles";
    } else if (memcmp(&m.vertices[size_t(a.first) * 16], &own.vertices[0], size_t(a.count) * 64) != 0) {
        *what = "the vertices";
    }
    return **what == 0;
}

extern "C" int gsn_nd700_oracle_run(const GsnNd700OracleDraw *draws, uint32_t draw_count, GsnNd700OracleBatch *batches,
                                    GsnNd700OracleVertex *out) {
    if (!gsn_nd_oracle_ready() || draw_count == 0 || draw_count > (1u << 15)) {
        return 0;
    }
    ring_idle();
    Device &dev = *gs->device;
    std::vector<NdConst> consts(draw_count);
    std::vector<int32_t> arena;
    struct Corner {
        uint32_t rgba;
        float vertex;
    };
    std::vector<Corner> corners;
    std::vector<uint32_t> corner_out;
    uint32_t out_at = 0;
    Nd700Plan p;
    std::vector<uint64_t> ad;
    for (uint32_t i = 0; i < draw_count; i++) {
        GsnNd700OracleBatch &r = batches[i];
        memset(&r, 0, sizeof(r));
        RdDraw d;
        nd700_oracle_const(consts[i], d, draws[i], r);
        RdMeshData mesh;
        r.out = out_at;
        if (!nd700_oracle_batch(draws[i], mesh)) {
            r.why = rd_mesh_why();
            continue;
        }
        const RdBatch &b = mesh.batches[0];
        r.ok = 1;
        r.n = b.count;
        r.swapped = nd700_ad(b, d.colour[1], d.colour[2], ad);
        r.ad_count = uint32_t(std::min<size_t>(ad.size(), 32));
        memcpy(r.ad, ad.data(), r.ad_count * 8);
        nd700_plan(mesh, b, consts[i].vertex, p);
        r.kick = p.kick;
        r.one_left = p.unclipped == 1;
        r.clip_past = p.clip_past;
        uint32_t alpha = nd700_alpha(d.colour[0]);
        uint32_t slot = uint32_t(arena.size() / 16);
        arena.insert(arena.end(), mesh.vertices.begin(), mesh.vertices.end());
        for (uint32_t k = 0; k < b.count; k++) {
            GsnNd700OracleVertex &v = out[out_at + k];
            memset(&v, 0, sizeof(v));
            memcpy(v.plan_xyz, p.xyz[k], 12);
            memcpy(v.plan_stq, p.stq[k], 12);
            v.plan_alpha = alpha;
            v.plan_clip = p.clip[k];
            v.plan_adc = p.adc[k];
            corners.push_back({ND_CORNER_GOURAUD | i << ND_CORNER_DRAW_SHIFT, nd_corner_vertex(slot + k)});
            corner_out.push_back(out_at + k);
        }
        out_at += b.count;
    }
    if (corners.empty()) {
        return 1;
    }
    while (corners.size() % 3) {
        corners.push_back(corners[0]);
    }
    static Program *program;
    if (!program) {
        ResourceLayout vert = {};
        vert.input_mask = (1u << 2) | (1u << 4);
        vert.push_constant_size = sizeof(Push);
        vert.sets[1].storage_buffer_mask = 1u;
        vert.sets[1].meta[0].array_size = 1;
        vert.sets[2].storage_buffer_mask = 7u;
        for (int b = 0; b < 3; b++) {
            vert.sets[2].meta[b].array_size = 1;
        }
        ResourceLayout frag = {};
        frag.output_mask = 1;
        program = dev.request_program(
            dev.request_shader(gsn_spv_nd_700_oracle_vert, sizeof(gsn_spv_nd_700_oracle_vert), &vert),
            dev.request_shader(gsn_spv_nd_oracle_frag, sizeof(gsn_spv_nd_oracle_frag), &frag));
    }
    auto buffer = [&](VkDeviceSize size, const void *data, BufferDomain domain) {
        BufferCreateInfo info = {};
        info.domain = domain;
        info.size = std::max<VkDeviceSize>(size, 256);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        std::vector<uint8_t> zeros;
        if (!data || size < info.size) {
            zeros.assign(info.size, 0);
            if (data) {
                memcpy(zeros.data(), data, size);
            }
            data = zeros.data();
        }
        return dev.create_buffer(info, data);
    };
    uint32_t count = uint32_t(corners.size());
    BufferHandle mesh = buffer(VkDeviceSize(arena.size()) * 4, arena.data(), BufferDomain::Device);
    BufferHandle records = buffer(VkDeviceSize(draw_count) * sizeof(NdConst), consts.data(), BufferDomain::Device);
    BufferHandle picture = buffer(256, nullptr, BufferDomain::Device);
    BufferHandle results = buffer(VkDeviceSize(count) * 32, nullptr, BufferDomain::CachedHost);
    ImageHandle target = dev.create_image(ImageCreateInfo::render_target(1, 1, VK_FORMAT_R8G8B8A8_UNORM), nullptr);

    CommandBufferHandle c = dev.request_command_buffer();
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &target->get_view();
    rp.clear_attachments = 1;
    c->begin_render_pass(rp);
    c->set_opaque_state();
    c->set_program(program);
    c->set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c->set_cull_mode(VK_CULL_MODE_NONE);
    c->set_depth_test(false, false);
    c->set_storage_buffer(1, 0, *picture);
    c->set_storage_buffer(2, 0, *mesh);
    c->set_storage_buffer(2, 1, *records);
    c->set_storage_buffer(2, 2, *results);
    Push push = {};
    push.inv_size[0] = push.inv_size[1] = 1.0f / 4096.0f;
    push.zmax = 0xFFFFFFFFu;
    c->push_constants(&push, 0, sizeof(push));
    auto *cv = static_cast<Corner *>(c->allocate_vertex_data(0, VkDeviceSize(count) * sizeof(Corner), sizeof(Corner)));
    memcpy(cv, corners.data(), size_t(count) * sizeof(Corner));
    c->set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Corner, rgba));
    c->set_vertex_attrib(4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Corner, vertex));
    c->draw(count);
    c->end_render_pass();
    c->barrier(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
               VK_ACCESS_HOST_READ_BIT);
    Fence fence;
    dev.submit(c, &fence);
    fence->wait();
    const uint32_t *res = static_cast<const uint32_t *>(dev.map_host_buffer(*results, MEMORY_ACCESS_READ_BIT));
    for (size_t i = 0; i < corner_out.size(); i++) {
        memcpy(out[corner_out[i]].gpu, res + i * 8, 32);
    }
    dev.unmap_host_buffer(*results, MEMORY_ACCESS_READ_BIT);
    return 1;
}

namespace {
