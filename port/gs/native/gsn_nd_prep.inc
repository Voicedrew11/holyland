// ---- Native draw: the frame's draws' work kept for its first picture between (rc/own) ------------------
// Included by gsn_native_draw.inc after NdState (inside gsn.cpp's namespace).
//
// A frame's first picture between (--frame-rate above 30) is drawn from the frame's packets again
// (frame_interp.c's walk; gsn.cpp, "Pictures between on the GPU"): the same draws in the same order, from the
// same data. Each native draw worked its record out again (nd_const: VU1's set-up), and each of its batches'
// corners (the plan: nd_plan, nd280_plan_cached and nd280_corners), which come out the same as the frame's but
// for the record's number in the corners (the picture's records are its own, with the moves: nd_model_flags).
// The plans' own caches keep two a batch (the world's camera and the reflection's), so a mesh drawn more than
// twice a frame (the zombies, the trees) was planned again for the picture. About a fifth of the recording at
// the pond (NATIVE_DRAW_P8.md, "The frame's own draws kept for the picture").
//
// So the frame keeps them as it draws (nd_prep_*), by the draw's place among the frame's native draws, and the
// first picture between of that frame takes them: the record as the set-up made it (its moves are the picture's,
// worked out as before), and each batch's corners, their record number changed to the picture's. A draw or batch
// that is not the frame's (another draw at that place, another mesh or batch, PRIM, or its mesh at another place
// in the arena) is worked out as before, and counted. KF4_ND_PREP=0 (tests): never taken; KF4_ND_PREP_CHECK=1:
// taken and worked out both, the differences counted (--stats).

// What a batch's material is resolved from: the GS's registers it is drawn with, its vertices' alphas, and what the
// answer can change with (the table, the CLUT, the scene's shape; memcmp-able: memset first). The register part is
// what a picture between's call is checked by (PcdKept::regs, gsn_pc_draw.inc).
struct PcdKey {
    uint64_t frame, zbuf, test, alpha, scissor, tex0, tex1, clamp, miptbp1, miptbp2, texa, fogcol;
    uint32_t vs, prim, fba, colclamp, pabe, scanmsk, as_min, as_max;
    uint64_t clut; // (the CLUT mirror's contents, hashed: pcd_clut_hash)
    uint32_t sx, sy, samples, wide, width, height, fcopy;
    int32_t lod, cutout, aniso, pad;
};
struct PcdMaterial;

// One call the PC renderer (P7, gsn_pc_draw.inc) made for a batch, a 0x280 pass or a 0x700 strip, kept in the frame for
// its pictures between: what pcd_finish drew, less what is the picture's. The frame keeps each one in order as it draws
// (NdPrepFrame::pcd_calls); a picture between whose draw at that place was kept whole makes the frame's calls again
// with its own record instead of resolving materials and planning.
enum { PCD_CALL_DRAWN, PCD_CALL_NOTHING, PCD_CALL_UNKICKED }; // (UNKICKED: the batch not kicked, or all clipped: no PRIM)
struct PcdKept {
    PcdKey regs;                            // the registers and vertex alphas the material is resolved from, as the frame had
                                            // them: the picture's must be the same, or the call is worked out again
    std::shared_ptr<const PcdMaterial> mat; // the frame's material, held (only materials that are kept)
    BufferHandle tris;                      // pcd.tris, or the pcd.kicked buffer its list went into (never written again)
    uint32_t first = 0, n = 0;              // its triangles in tris
    uint32_t instance_bits = 0;             // the instance's bits above the record (gouraud 1<<30, context2 1<<28, sphere 1<<29)
    uint8_t vs = 0;                         // PCD_VS_000, _280, _700
    uint8_t what = PCD_CALL_DRAWN;          // PCD_CALL_*
};

// (--renderer fast) The world's snapshot: a draw kept whole made again in a picture between from what the frame kept, its
// calls put back at its place in the chain without the walk (PC_RENDERER.md, "The world off the native GS"; gsn_pc_draw.inc,
// "Snapshot"). Besides its calls (PcdKept) the frame keeps what the draw began with, the CLUT loads among its calls and
// what its writes left: a picture whose registers at the draw are the frame's makes the same writes, so it loads the
// same CLUTs (done again: they read VRAM), draws the calls with their materials (checked against what they read: the
// CLUT and the table's answer) and leaves the registers as the frame's writes left them, without writing them.
// The registers a snapshot's draw is checked by at its start (pcd_entry_regs); FRAME and ZBUF less their bases
enum { PCD_ENTRY_REGS = 33 };
// A TEX0 write among a kept draw's calls: the CLUT load it starts (clut_load, sa_clut_load), with TEXCLUT as it was
struct PcdLoad {
    uint64_t tex0, texclut;
    uint32_t before_call; // (the draw's call it came before: the calls kept before it)
};
struct PcdSnapDraw {
    RdDraw draw;                                 // the draw as the frame fetched it (a picture's at that place is the same:
    RdCamera camera;                             // its record is made from these, rd_fetch not asked)
    std::array<uint64_t, PCD_ENTRY_REGS> entry;  // the registers it began with (pcd_entry_regs)
    uint32_t loads_at = 0, loads_n = 0;          // its CLUT loads, in NdPrepFrame::pcd_loads
    uint32_t writes_at = 0, writes_n = 0;        // the registers its writes left, in NdPrepFrame::pcd_writes
    bool usable = false;                         // (pcd_snapshot_keep_end: its writes and calls can be made so)
};

// A batch's corners as the frame's plan made them (a pass of it: 0x280's second, sphere-mapped)
struct NdPrepBatch {
    uint32_t mesh, batch, pass; // which: the mesh's id, the batch's number in it, the pass
    uint32_t eprim, base, record; // what they were made with: PRIM as drawn, the mesh's place in the arena, the record
    uint32_t at, n;             // the corners, in NdPrepFrame::corners
    bool all_clipped;           // (0x280: every vertex clipped, no strip)
    uint64_t extra;             // (the water's: what else its corners are made with, its alpha and XYOFFSET)           // (0x280: every vertex clipped, no strip)
};
struct NdPrepDraw {
    uint64_t value;            // the draw (PORT_GS_REG_NATIVE_DRAW's value)
    uint32_t program;
    uint32_t k_at, k_size;     // its record as the set-up made it (before the moves), in NdPrepFrame::consts
    uint32_t first, count;     // its batches
    // (P7) every batch of it drawn by the PC renderer from its mesh, and its calls kept: pcd_calls from calls_at, calls_n
    // of them; arena_gen (the arena's) as the draw began
    bool pcd_whole;
    uint32_t calls_at, calls_n;
    uint32_t arena_gen;
    int32_t snap;               // (--renderer fast) its snapshot in NdPrepFrame::pcd_snaps, or -1
};
// What the frame's scene batch (sa_draw_batch) worked out from its corners alone, for the same batch in a picture
// between (sa_memo_*, gsn_standalone.inc): a batch whose corners are the frame's kept ones (NdPrepFrame::corners) from
// at, n of them, and nothing else, found by them and its scissor
struct SaMemo {
    uint32_t at, n;            // its corners before any was left out, in NdPrepFrame::corners
    uint32_t scissor[4];       // sx0, sy0, x1, y1 (x1, y1 within the target)
    uint32_t as_min, as_max;   // its corners' alphas, rows
    float vy0, vy1;
    bool second_pass, skinned, all_native;
    uint32_t culled_at, culled_n; // the triangles left out (outside the scissor), in sa_u32
    bool split;                   // its runs were cut where triangles overlap
    uint32_t runs_at, runs_n;     // and where (sa_u32)
    int slopes;                   // 0 none kept, 1 the Z rule's, 2 by the world: each kept triangle's in sa_bytes
    uint32_t slopes_at;
};
struct NdPrepFrame {
    uint32_t serial = 0xFFFFFFFFu; // the frame's (RD_ND_FRAME's), none: 0xFFFFFFFF
    bool whole = false;            // kept from the frame's first draw on
    std::vector<NdPrepDraw> draws;
    std::vector<NdPrepBatch> batches;
    std::vector<Vtx> corners;
    std::vector<uint8_t> consts;
    std::vector<PcdKept> pcd_calls; // (P7: the calls of the draws kept whole, in order)
    std::vector<PcdSnapDraw> pcd_snaps;                     // (the snapshot: NdPrepDraw::snap)
    std::vector<PcdLoad> pcd_loads;
    std::vector<std::pair<uint32_t, uint64_t>> pcd_writes; // (address, value)
    // (the scene's batches: SaMemo)
    std::vector<SaMemo> sa_memos;
    std::unordered_map<uint32_t, uint32_t> sa_index; // (by at)
    std::vector<uint32_t> sa_u32;
    std::vector<uint8_t> sa_bytes;
    void clear() {
        serial = 0xFFFFFFFFu;
        whole = false;
        draws.clear();
        batches.clear();
        corners.clear();
        consts.clear();
        pcd_calls.clear();
        pcd_snaps.clear();
        pcd_loads.clear();
        pcd_writes.clear();
        sa_memos.clear();
        sa_index.clear();
        sa_u32.clear();
        sa_bytes.clear();
    }
};
struct NdPrep {
    int enabled = -1, check = -1;
    NdPrepFrame frames[2]; // the frame kept last and the one before (a picture can come after the next frame began)
    int last = 1;          // the one kept last
    uint32_t unused = 8;   // frames since a picture between was drawn from the packets (8 or more: none is kept)
    NdPrepFrame *filling = nullptr; // the frame being drawn
    NdPrepFrame *taking = nullptr;  // the frame a picture between is drawing
    uint32_t next = 0;              // (taking) the picture's next draw
    int32_t draw = -1;              // the draw under way: its place in filling's or taking's draws (-1: none)
    uint32_t batch = 0;             // (taking) the draw's next batch
    // gs->verts as the kept corners of one frame (run_frame's corners from run_at, run_n of them) and nothing else, or
    // not (run_at -1): a batch of them is found by them (SaMemo)
    NdPrepFrame *run_frame = nullptr;
    uint32_t run_serial = 0xFFFFFFFFu;
    int64_t run_at = -1;
    uint32_t run_n = 0;
    // --stats: draws whose record was taken, worked out again (and why); batches likewise; check differences
    uint64_t taken_draws = 0, taken_batches = 0, taken_corners = 0;
    uint64_t again_draws[4] = {}; // 0 no frame kept, 1 another draw at its place, 2 another program, 3 (unused)
    uint64_t again_batches[4] = {}; // 0 no draw taken, 1 another batch, 2 PRIM, 3 the arena
    uint64_t check_differ[3] = {}; // records, corners, 0x280 batches all clipped
} nd_prep;

bool nd_prep_on() {
    if (nd_prep.enabled < 0) {
        const char *v = getenv("KF4_ND_PREP");
        nd_prep.enabled = v && *v ? atoi(v) != 0 : 1;
        const char *c = getenv("KF4_ND_PREP_CHECK");
        nd_prep.check = c && *c ? atoi(c) != 0 : 0;
    }
    return nd_prep.enabled != 0;
}

// RD_ND_FRAME (nd_register, before nd.serial is changed): a frame's draws begin, or a picture between's of a frame
void nd_prep_frame(uint32_t serial) {
    nd_prep.draw = -1;
    nd_prep.taking = nullptr;
    if (!nd_prep_on()) {
        nd_prep.filling = nullptr;
        return;
    }
    if (!interp.active) {
        if (serial == nd.serial) {
            nd_prep.filling = nullptr; // (the same frame drawn again: a field's, or exact: kept as it was)
            return;
        }
        // a new frame: kept in the older of the two, while frames have pictures between drawn from their packets (at 30,
        // or a CPU's pictures, none is taken: nothing kept)
        if (nd_prep.unused >= 8) {
            nd_prep.filling = nullptr;
            return;
        }
        nd_prep.unused++;
        nd_prep.last ^= 1;
        NdPrepFrame *f = &nd_prep.frames[nd_prep.last];
        f->clear();
        f->serial = serial;
        f->whole = true;
        nd_prep.filling = f;
        return;
    }
    // (a picture's draws are not the frame's: nd_prep_draw_begin keeps none while a picture is drawn, and a frame begun
    // before it goes on after it)
    nd_prep.unused = 0;
    for (NdPrepFrame &f : nd_prep.frames) {
        if (f.whole && f.serial == serial) {
            nd_prep.taking = &f;
            nd_prep.next = 0;
        }
    }
}

// A frame's draws kept no longer (a state loaded, the GS's VRAM replaced, the arena gone)
void pc_between_forget(); // (gsn_pc_world.inc: the frames' kept batches)
void nd_prep_forget() {
    pc_between_forget();
    for (NdPrepFrame &f : nd_prep.frames) {
        f.clear();
    }
    nd_prep.filling = nd_prep.taking = nullptr;
    nd_prep.draw = -1;
}

// RD_ND_DRAW met (nd_register): the draw's place
void nd_prep_draw_begin(uint64_t value) {
    nd_prep.draw = -1;
    nd_prep.batch = 0;
    if (nd_prep.filling && !interp.active) {
        NdPrepFrame &f = *nd_prep.filling;
        NdPrepDraw d = {};
        d.value = value;
        d.program = 0xFFFFFFFFu;
        d.first = uint32_t(f.batches.size());
        d.snap = -1;
        f.draws.push_back(d);
        nd_prep.draw = int32_t(f.draws.size() - 1);
        return;
    }
    if (interp.active) {
        if (!nd_prep.taking) {
            nd_prep.again_draws[0]++;
            return;
        }
        uint32_t i = nd_prep.next++;
        if (i >= nd_prep.taking->draws.size() || nd_prep.taking->draws[i].value != value) {
            nd_prep.again_draws[1]++;
            return;
        }
        nd_prep.draw = int32_t(i);
    }
}
void nd_prep_draw_end() {
    nd_prep.draw = -1;
}

// Kept corners of frame f (at, n of them) joined gs->verts at `before`
void nd_prep_run(const NdPrepFrame *f, size_t before, uint32_t at, uint32_t n) {
    if (before == 0) {
        nd_prep.run_frame = const_cast<NdPrepFrame *>(f);
        nd_prep.run_serial = f->serial;
        nd_prep.run_at = at;
        nd_prep.run_n = n;
    } else if (nd_prep.run_at >= 0 && nd_prep.run_frame == f && before == nd_prep.run_n && at == nd_prep.run_at + nd_prep.run_n) {
        nd_prep.run_n += n;
    } else {
        nd_prep.run_at = -1;
    }
}

// Whether gs->verts is a run of frame f's kept corners and nothing else (its corners from nd_prep.run_at)
bool nd_prep_run_is(const NdPrepFrame *f) {
    return f && nd_prep.run_at >= 0 && nd_prep.run_frame == f && f->serial == nd_prep.run_serial &&
           nd_prep.run_n == gs->verts.size();
}

// (the frame's draw) its record as the set-up made it
void nd_prep_keep_const(uint32_t program, const void *k, size_t size) {
    if (!nd_prep.filling || nd_prep.draw < 0 || interp.active) {
        return;
    }
    NdPrepFrame &f = *nd_prep.filling;
    NdPrepDraw &d = f.draws[size_t(nd_prep.draw)];
    d.program = program;
    d.k_at = uint32_t(f.consts.size());
    d.k_size = uint32_t(size);
    f.consts.insert(f.consts.end(), static_cast<const uint8_t *>(k), static_cast<const uint8_t *>(k) + size);
}

// (a picture's draw) the frame's record for it, as the set-up made it: false when it is not the frame's (worked out
// again)
bool nd_prep_take_const(uint32_t program, void *k, size_t size) {
    if (!interp.active || nd_prep.draw < 0 || !nd_prep.taking) {
        return false;
    }
    const NdPrepFrame &f = *nd_prep.taking;
    const NdPrepDraw &d = f.draws[size_t(nd_prep.draw)];
    if (d.program != program || d.k_size != size) {
        nd_prep.again_draws[2]++;
        nd_prep.draw = -1; // (nothing of it taken)
        return false;
    }
    memcpy(k, f.consts.data() + d.k_at, size);
    nd_prep.taken_draws++;
    return true;
}

// (the frame's draw) a batch's corners as made, gs->verts from `before` (pass 0 or 1); or (pass 2) a 0x280 batch, whether
// all its vertices are clipped
void nd_prep_keep_batch(uint32_t mesh, uint32_t batch, uint32_t pass, uint32_t eprim, uint32_t base, uint32_t record,
                        size_t before, bool all_clipped = false, uint64_t extra = 0) {
    if (!nd_prep.filling || nd_prep.draw < 0 || interp.active) {
        return;
    }
    NdPrepFrame &f = *nd_prep.filling;
    NdPrepBatch b = {mesh, batch, pass, eprim, base, record, uint32_t(f.corners.size()), 0, all_clipped, extra};
    if (pass < 2) { // (pass 2: 0x280's batch itself, whether every vertex is clipped: no corners)
        b.n = uint32_t(gs->verts.size() - before);
        f.corners.insert(f.corners.end(), gs->verts.begin() + before, gs->verts.end());
        nd_prep_run(&f, before, b.at, b.n);
    }
    f.batches.push_back(b);
    f.draws[size_t(nd_prep.draw)].count++;
}

// (a picture's draw) the frame's batch for this one: null when it is not (another mesh, batch or pass, PRIM, or the
// mesh at another place in the arena: planned again). The next is the draw's next batch either way.
const NdPrepBatch *nd_prep_take_batch(uint32_t mesh, uint32_t batch, uint32_t pass, uint32_t eprim, uint32_t base,
                                      uint64_t extra = 0) {
    if (!interp.active || nd_prep.draw < 0 || !nd_prep.taking) {
        if (interp.active) {
            nd_prep.again_batches[0]++;
        }
        return nullptr;
    }
    const NdPrepFrame &f = *nd_prep.taking;
    const NdPrepDraw &d = f.draws[size_t(nd_prep.draw)];
    uint32_t j = nd_prep.batch++;
    if (j >= d.count) {
        nd_prep.again_batches[1]++;
        return nullptr;
    }
    const NdPrepBatch &b = f.batches[d.first + j];
    if (b.mesh != mesh || b.batch != batch || b.pass != pass) {
        nd_prep.again_batches[1]++;
        return nullptr;
    }
    if (b.all_clipped) {
        nd_prep.taken_batches++;
        return &b; // (0x280: no PRIM, no corners)
    }
    if (b.eprim != eprim || b.extra != extra) {
        nd_prep.again_batches[2]++;
        return nullptr;
    }
    if (b.base != base) {
        nd_prep.again_batches[3]++;
        return nullptr;
    }
    nd_prep.taken_batches++;
    return &b;
}

// A batch's corners into gs->verts, as the frame's plan made them, with the picture's record (bits 7 up of the RGB:
// a corner's record field, which nothing else shares)
void nd_prep_corners(const NdPrepBatch &b, uint32_t record) {
    const NdPrepFrame &f = *nd_prep.taking;
    uint32_t change = (b.record ^ record) << ND_CORNER_DRAW_SHIFT;
    size_t at = gs->verts.size();
    nd_prep_run(&f, at, b.at, b.n);
    gs->verts.resize(at + b.n);
    Vtx *out = gs->verts.data() + at;
    const Vtx *in = f.corners.data() + b.at;
    for (uint32_t i = 0; i < b.n; i++) {
        out[i] = in[i];
        out[i].rgba ^= change;
    }
    size_t vs = interp.vsrc.size();
    interp.vsrc.resize(vs + b.n);
    std::array<uint32_t, 4> *vo = interp.vsrc.data() + vs;
    for (uint32_t i = 0; i < b.n; i++) {
        vo[i] = {GSN_IM_NONE, GSN_IM_NONE, GSN_IM_NONE, i % 3};
    }
    nd_prep.taken_corners += b.n;
}

// KF4_ND_PREP_CHECK: the corners taken (gs->verts from `taken`, n) against those worked out again (from `again`);
// those worked out are kept (the taken ones dropped), so the picture is as before either way
void nd_prep_check(size_t taken, size_t again) {
    size_t n = again - taken;
    if (gs->verts.size() - again != n || memcmp(gs->verts.data() + taken, gs->verts.data() + again, n * sizeof(Vtx)) != 0) {
        nd_prep.check_differ[1]++;
    }
    gs->verts.erase(gs->verts.begin() + ptrdiff_t(taken), gs->verts.begin() + ptrdiff_t(again));
    interp.vsrc.resize(interp.vsrc.size() - n);
}

void nd_prep_print_stats() {
    if (!nd_prep.taken_draws && !nd_prep.again_draws[0] && !nd_prep.again_draws[1] && !nd_prep.again_draws[2]) {
        return;
    }
    fprintf(stderr,
            "[stats] native draw, pictures between: %llu draws' records and %llu batches (%llu corners) taken from the "
            "frame's draws; worked out again: draws %llu (no frame kept), %llu (another draw), %llu (another program); "
            "batches %llu (draw not taken), %llu (another batch), %llu (PRIM), %llu (the arena)",
            (unsigned long long)nd_prep.taken_draws, (unsigned long long)nd_prep.taken_batches,
            (unsigned long long)nd_prep.taken_corners, (unsigned long long)nd_prep.again_draws[0],
            (unsigned long long)nd_prep.again_draws[1], (unsigned long long)nd_prep.again_draws[2],
            (unsigned long long)nd_prep.again_batches[0], (unsigned long long)nd_prep.again_batches[1],
            (unsigned long long)nd_prep.again_batches[2], (unsigned long long)nd_prep.again_batches[3]);
    if (nd_prep.check) {
        fprintf(stderr, "; checked: %llu records, %llu batches' corners, %llu batches all clipped differ",
                (unsigned long long)nd_prep.check_differ[0], (unsigned long long)nd_prep.check_differ[1],
                (unsigned long long)nd_prep.check_differ[2]);
    }
    fprintf(stderr, "\n");
}
