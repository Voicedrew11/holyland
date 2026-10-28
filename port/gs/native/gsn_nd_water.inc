// ---- Native draw: the reflection's rippled water (P6-1) ---------------------------------------------
// Included at the end of gsn_native_draw.inc (inside gsn.cpp's namespace), whose top says how a draw goes.
// A draw of program RD_PROGRAM_RIPPLE (game/native_draw/nd_water.c) is a part of the water record that
// Wobble_DrawRippledModel (gfx/wobble.c) draws before the record itself on the maps with a reflection
// (008, 009, 014): no VU1 program, the EE's maths in VU0's arithmetic, which this does on the CPU (the plan,
// for flush_batch) and shaders/nd_water.vert on the GPU, from the record's own meshes (RD_VF_PACKED16:
// only the positions and the data's ADC are read), the part's matrix and the 32 sines of the frame:
//   each block of a part is a strip of its vertices (PRIM from the chain: a strip, textured, blended);
//   a vertex's place: FTOI4 of the world to screen matrix by the part's (VU arithmetic: Matrix_Multiply
//     and Vu0_ProjectToScreen3) times (x, y, z, 1), divided by w; Z the GS gets that >> 4 (XYZ2: 32 bits);
//   its ADC: its place (Z >> 4 above 0xFFFF0, X or Y beyond 16 bits) or one of the two before it in the
//     block out of bounds, or the data's (the normal's w, bit 15);
//   its S and T: the place of (x, y + sine * 5, z, 1) (the game's floats: sine * 5, then the sum; the sine
//     wobble_sines[|(x + z)^3 % 32|]), (X - 0x6C00) / 10240 and (Y - 0x7200) / 7168 in the game's floats,
//     which round toward zero (the EE's FPU, as the port's game threads: ndw_round); Q 1;
//   its colour: RGBAQ's (the map's tint, written by the chain before it).
// Corners are nd_000.vert's (a corner's fog -1 less the vertex's place in the arena, so the batch of the
// strips is decided by flush_batch from the CPU's places as the PS2 path's would be; its RGB the kick and
// the draw's record), but their records are a buffer of their own: a batch of water corners is never one
// of 0x000's or 0x280's (the PS2 path's strips are a batch of their own too: the chain writes TEX0, ALPHA,
// CLAMP before them and CLAMP after).
// Pictures between: the water is in the world's section: each place (the vertex's and the moved one's) is
// moved by the picture's camera as nd_000.vert moves a vertex (gs_replay.comp's moved()), and S and T are
// made from the moved one's new place: the reflection, drawn again by the camera between into its target,
// stays where it is under the water.

// nd_water.vert's Draw (std430, a record a draw)
struct NdWaterConst {
    float vertex[16]; // the world to screen matrix by the part's (VU arithmetic), a qword a column
    float screen[16]; // view to screen (pictures between: sx, sy, cx, cy, cz as nd_000.vert's)
    float sines[32];  // wobble_sines
    int32_t info[4];  // XYOFFSET's X and Y (12.4) of the strip's context, ND_C_* flags, RGBAQ's RGBA
    float fog[4];     // the FOG register's fog (XYZ2 takes it), as a float in x
};
static_assert(sizeof(NdWaterConst) == 18 * 16, "NdWaterConst is nd_water.vert's Draw");
const uint32_t NDW_RECORDS = 1024;
const int32_t NDW_C_NEAREST = 0x100; // (info's flags) the game's floats round to nearest (--fpu-round nearest)

struct NdWaterState {
    bool pending = false; // the pending native corners are water's
    bool active = false;  // the batch flush_batch is drawing is
    NdBuffer draws;       // the records
    std::unordered_map<Program *, Program *> programs;
    Shader *vs = nullptr;
} ndw;

// (NdFlush, the hook in gsn_native_draw.inc: after nd.active is set, and when the flush ends)
void ndw_flush(bool begin) {
    ndw.active = begin && nd.active && ndw.pending;
    if (begin) {
        ndw.pending = false;
    }
}

// (nd_draw_range and nd_box_at_0, the hooks) whether the batch flush_batch is drawing is of water corners
bool ndw_batch() {
    return ndw.active;
}

// (nd_draw, nd280_draw, nd700_draw: the hooks) whether the pending corners are water's, a batch of their own
bool ndw_pending() {
    return ndw.pending;
}

// (nd_program, the hook: while a batch of water corners is drawn) the GS's program p with nd_water.vert
Program *ndw_program(Program *p) {
    auto it = ndw.programs.find(p);
    if (it != ndw.programs.end()) {
        return it->second;
    }
    if (!ndw.vs) {
        ResourceLayout vert = {};
        vert.input_mask = (1u << 2) | (1u << 4);
        vert.push_constant_size = sizeof(Push);
        vert.sets[1].storage_buffer_mask = 1u;
        vert.sets[1].meta[0].array_size = 1;
        vert.sets[2].storage_buffer_mask = 3u;
        vert.sets[2].meta[0].array_size = 1;
        vert.sets[2].meta[1].array_size = 1;
        ndw.vs = gs->device->request_shader(gsn_spv_nd_water_vert, sizeof(gsn_spv_nd_water_vert), &vert);
    }
    Program *np = gs->device->request_program(ndw.vs, p->get_shader(ShaderStage::Fragment));
    ndw.programs[p] = np;
    return np;
}

// Vu0_ProjectToScreen's place of (x, y, z, 1) by vm (VU arithmetic: vu_float.h), FTOI4's three
void ndw_place(const float *vm, float x, float y, float z, int32_t out[3]) {
    float p[4] = {x, y, z, 1.0f}, c[4];
    vu_transform4(c, vm, p);
    uint32_t q = vu_float_div(0x3F800000u, vu_float_bits(c[3]));
    for (int i = 0; i < 3; i++) {
        out[i] = vu_float_ftoi(vu_float_mul(vu_float_bits(c[i]), q), 4);
    }
}

// A number the game's own float code makes, from its exact value (a double): the game's threads round
// toward zero, as the EE's FPU (PORT_MXCSR_EE, port/platform/fiber.c), or to nearest with --fpu-round
// nearest (the GS's thread rounds to nearest)
float ndw_round(double v, bool nearest) {
    float f = float(v);
    if (!nearest && double(f) != v && fabs(double(f)) > fabs(v)) {
        f = nextafterf(f, 0.0f);
    }
    return f;
}

// The sine a vertex at x, z takes (Wobble_DrawRippledModel's WOBBLE: |(x + z)^3 % 32|, in 32-bit ints)
uint32_t ndw_sine_index(int32_t x, int32_t z) {
    uint32_t i = uint32_t(x) + uint32_t(z);
    int32_t c = int32_t(i * i * i);
    uint32_t magnitude = c < 0 ? 0u - uint32_t(c) : uint32_t(c);
    return magnitude & 31u;
}

// (nd_draw, the hook: a draw of RD_PROGRAM_RIPPLE) its strips planned and joined to the GS's batch
void ndw_draw(const RdDraw &d, const RdCamera &cam) {
    gsn_stats.nd_draws++;
    if (nd_buffer_room(ndw.draws, 1, ndw.pending)) {
        nd_new_buffer(ndw.draws, NDW_RECORDS, sizeof(NdWaterConst), ndw.pending);
    }
    uint32_t record = ndw.draws.used++;
    NdWaterConst k;
    memset(&k, 0, sizeof(k));
    const float *m = &d.model[0][0];
    // (rc/own: in the frame's first picture between, the frame's vertex matrix)
    if (!nd_prep_take_const(RD_PROGRAM_RIPPLE, k.vertex, sizeof(k.vertex)) || nd_prep.check) {
        float again[16];
        for (int col = 0; col < 4; col++) {
            vu_transform4(&again[col * 4], &cam.world_screen[0][0], &m[col * 4]);
        }
        if (nd_prep.draw >= 0 && interp.active && memcmp(again, k.vertex, sizeof(again)) != 0) {
            nd_prep.check_differ[0]++;
        }
        memcpy(k.vertex, again, sizeof(again));
        nd_prep_keep_const(RD_PROGRAM_RIPPLE, k.vertex, sizeof(k.vertex));
    }
    memcpy(k.screen, cam.screen, 64);
    memcpy(k.sines, d.point_pos, 64);
    memcpy(k.sines + 16, d.point_col, 64);
    bool nearest = (d.flags & RD_DRAW_ROUND_NEAREST) != 0;
    k.info[2] = nearest ? NDW_C_NEAREST : 0;
    k.info[3] = int32_t(uint32_t(gs->reg[R_RGBAQ]));
    k.fog[0] = float(bits(gs->reg[R_FOG], 56, 8));
    // pictures between: the world's (and, were it drawn there, the reflection's) move with the camera
    NdKey key = nd_key(d);
    if (!interp.active) {
        nd_remember(key, d, cam);
    } else if (nd.section == ND_SECTION_WORLD || nd.section == ND_SECTION_REFLECT) {
        k.info[2] |= ND_C_MOVING;
    }
    bool written = false;
    std::vector<int32_t> xyz;
    std::vector<float> st;
    std::vector<uint8_t> adc;
    for (const auto &mesh : nd.fetched) {
        if (mesh->format != RD_VF_PACKED16) {
            gsn_stats.nd_unsupported++;
            continue;
        }
        // (the block's vertices, in order: its batches' together; Wobble_DrawRippledModel reads them so)
        uint32_t n = uint32_t(mesh->vertices.size() / 16), total = 0;
        for (const RdBatch &b : mesh->batches) {
            total += b.count;
        }
        if (total != n || n == 0) {
            gsn_stats.nd_unsupported++;
            continue;
        }
        uint32_t base = nd_place(*mesh);
        gs_write(R_PRIM, 0x54); // (the strip's tag: PRE, PRIM a strip, textured, blended)
        uint32_t eprim = effective_prim();
        uint64_t offset = gs->reg[R_XYOFFSET_1 + ((eprim >> 9) & 1)];
        int32_t ox = int32_t(bits(offset, 0, 16)), oy = int32_t(bits(offset, 32, 16));
        if (!written) {
            k.info[0] = ox;
            k.info[1] = oy;
            memcpy(ndw.draws.map + size_t(record) * sizeof(NdWaterConst), &k, sizeof(k));
            written = true;
        }
        uint32_t gouraud = ((eprim >> 3) & 1) ? ND_CORNER_GOURAUD : 0;
        uint32_t alpha = uint32_t(gs->reg[R_RGBAQ]) & 0xFF000000u;
        // (rc/own: in the frame's first picture between, the frame's corners, made with the same alpha and XYOFFSET)
        uint64_t extra = uint64_t(alpha) | uint64_t(uint32_t(ox) & 0xFFFF) << 32 | uint64_t(uint32_t(oy) & 0xFFFF) << 48;
        const NdPrepBatch *taken = nd_prep_take_batch(mesh->id, 0, 0, eprim, base, extra);
        if (taken && !nd_prep.check) {
            if (!gs->verts.empty() && ((interp.active && !nd.pending) ||
                                       (nd.pending && (!ndw.pending || nd.batch_arena != nd.arena.buffer.get() ||
                                                       nd.batch_draws != ndw.draws.buffer.get())))) {
                flush_batch();
            }
            if (gs->verts.empty()) {
                gs->batch_prim = eprim;
                gs->batch_sprites = false;
            }
            size_t before = gs->verts.size();
            nd_prep_corners(*taken, record);
            if (gs->verts.size() > before) {
                nd.pending = ndw.pending = true;
            }
            nd.batch_arena = nd.arena.buffer.get();
            nd.batch_draws = ndw.draws.buffer.get();
            gsn_stats.nd_batches++;
            continue;
        }
        // the plan: each vertex's place, ADC, S and T, as the EE makes them
        xyz.resize(size_t(n) * 3);
        st.resize(size_t(n) * 2);
        adc.resize(n);
        uint32_t window = 0;
        for (uint32_t v = 0; v < n; v++) {
            const int32_t *q = &mesh->vertices[size_t(v) * 16];
            int32_t *p = &xyz[size_t(v) * 3];
            ndw_place(k.vertex, float(q[0]), float(q[1]), float(q[2]), p);
            p[2] >>= 4;
            bool out = uint32_t(p[2]) > 0xFFFF0u || ((p[0] | p[1]) & int32_t(0xFFFF0000u)) != 0;
            window |= out;
            adc[v] = window != 0 || (uint32_t(q[7]) & 0x8000u) != 0;
            window = (window << 1) & 7;
            // (the game's floats: the sine * 5, then + y; (X - 0x6C00) as a float, then / 10240. Each exact in
            // a double, then rounded as the game's thread rounds: the quotients of these divisors never lie
            // near enough a float's edge for the double's own rounding to matter)
            float lift = ndw_round(double(k.sines[ndw_sine_index(q[0], q[2])]) * 5.0, nearest);
            float y = ndw_round(double(q[1]) + double(lift), nearest);
            int32_t w[3];
            ndw_place(k.vertex, float(q[0]), y, float(q[2]), w);
            // (the EE's subtraction wraps: a place far off the screen is 0x80000000, ndw_place's saturation)
            int32_t sx = int32_t(uint32_t(w[0]) - 0x6C00u), sy = int32_t(uint32_t(w[1]) - 0x7200u);
            st[size_t(v) * 2] = ndw_round(double(ndw_round(double(sx), nearest)) / 10240.0, nearest);
            st[size_t(v) * 2 + 1] = ndw_round(double(ndw_round(double(sy), nearest)) / 7168.0, nearest);
        }
        // the strip's triangles join the batch (a batch of other corners or buffers is drawn first)
        if (!gs->verts.empty() && ((interp.active && !nd.pending) ||
                                   (nd.pending && (!ndw.pending || nd.batch_arena != nd.arena.buffer.get() ||
                                                   nd.batch_draws != ndw.draws.buffer.get())))) {
            flush_batch();
        }
        if (gs->verts.empty()) {
            gs->batch_prim = eprim;
            gs->batch_sprites = false;
        }
        size_t before = gs->verts.size();
        size_t taken_at = before;
        if (taken) { // (KF4_ND_PREP_CHECK: the frame's, then worked out again)
            nd_prep_corners(*taken, record);
            before = gs->verts.size();
        }
        for (uint32_t kick = 2; kick < n; kick++) {
            if (adc[kick]) {
                continue;
            }
            for (uint32_t j = 0; j < 3; j++) {
                uint32_t v = kick - 2 + j;
                Vtx c;
                c.x = float(int32_t(uint32_t(xyz[size_t(v) * 3]) & 0xFFFF) - ox) / 16.0f;
                c.y = float(int32_t(uint32_t(xyz[size_t(v) * 3 + 1]) & 0xFFFF) - oy) / 16.0f;
                c.z = uint32_t(xyz[size_t(v) * 3 + 2]);
                c.rgba = alpha | (kick - v) | gouraud | record << ND_CORNER_DRAW_SHIFT;
                c.s = st[size_t(v) * 2];
                c.t = st[size_t(v) * 2 + 1];
                c.q = 1.0f;
                c.fog = nd_corner_vertex(base + v);
                gs->verts.push_back(c);
                if (interp.active) {
                    interp.vsrc.push_back({GSN_IM_NONE, GSN_IM_NONE, GSN_IM_NONE, j});
                }
            }
        }
        if (taken) {
            nd_prep_check(taken_at, before);
            before = taken_at;
        }
        nd_prep_keep_batch(mesh->id, 0, 0, eprim, base, record, before, false, extra);
        if (gs->verts.size() > before) {
            nd.pending = ndw.pending = true;
        }
        nd.batch_arena = nd.arena.buffer.get();
        nd.batch_draws = ndw.draws.buffer.get();
        gsn_stats.nd_batches++;
    }
    if (!written) {
        memcpy(ndw.draws.map + size_t(record) * sizeof(NdWaterConst), &k, sizeof(k));
    }
    gs->queue_count = 0;
}
