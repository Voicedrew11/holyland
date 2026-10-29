// ---- Native draw: VU1's program 0x280, the skinned models (P2-2) -------------------------------------
// Included at the end of gsn_native_draw.inc (inside gsn.cpp's namespace), whose top says how a draw goes;
// this is the same for a draw of program 0x280 (RdDraw.program 0x280, meshes of RD_VF_FLOAT_SKIN:
// render.cpp, take_batch_280). port/platform/vu1_hle_280.c is the specification, shaders/nd_280.vert
// the vertex program. What differs from 0x000's:
// - two matrix sets: the part's (A: VU1 0x04) and its parent's (B: 0x08), each vertex taking one by its
//   ST's w (rigid skinning). The set-up (nd280_const) is nd_const's for each set;
// - the colour: the draw's scale and bias (VU1 0x64-0x65) make each vertex's u from its colour, with
//   the program's carry of the RGB from vertex to vertex (nd_280.vert);
// - the run ends without a strip when VU1's count of clipped vertices reaches n (its vertex 0 counted
//   twice): the A+D packet at TOP+0xA0 is kicked, no PRIM, no strip, no second pass (nd280_plan);
// - every vertex gets ADC when the tag's w is odd and vertex 0 is clipped;
// - the second, sphere-mapped pass (the "phong" shine) when the batch has one: the A+D packet at
//   TOP+0xA3 is kicked, then the same XYZF2s again under the tag at TOP+1 with ST from the sphere
//   matrices and the normals, white with vertex 1's alpha (blended as its ALPHA says, Cs + Cd As).
// Corners are nd_000.vert's (a corner's fog -1 less the vertex's place in the arena: nd_corner_vertex,
// so that a batch holds them and the PS2 path's corners together, drawn a call a kind by nd_draw_range;
// its RGB the kick and the draw's record) but for the arena's eight qwords a vertex, the fog a further 2^22
// less (nd280_corner_vertex) and the corner's bits 7-21 (the record), 22 (the context whose XYOFFSET) and
// 23 (the second pass). Their records are a buffer of their own; a batch holds 0x280's and 0x000's
// corners as the PS2 path's would (F-mixed), and nd_draw_range draws each stretch with its program and
// its records.
// Pictures between (P2-5): as nd_draw's of the models' kinds (nd_model_flags), each matrix set moved by its
// own matrices (nd_280.vert's moved: the vertex's set's Between).

// nd_280.vert's Draw (std430, a record a draw): matrices VU layout, a qword a column; [0] set A, [1] set B
struct Nd280Const {
    float vertex[2][16];    // VU1 0x14-0x17, 0x1C-0x1F
    float light_dir[2][16]; // VU1 0x18-0x1A, 0x20-0x22 (the fourth column not read)
    float light_col[16];    // vf08-vf11 (VU1 0x10-0x13)
    float sphere[2][16];    // VU1 0x2C-0x2E, 0x30-0x32 (0x24-0x26 and 0x28-0x2A copied by the set-up)
    float fog[4];           // VU1 0x3C
    float scale[4], bias[4]; // VU1 0x72, 0x73 (0x64, 0x65 copied)
    float point_pos[8][4];  // VU1 0x4A-0x4D (set A's space), 0x53-0x56 (set B's); w the falloff
    float point_col[4][4];  // VU1 0x4E-0x51
    int32_t info[4];        // the point lights, ND_C_* flags
    int32_t offset[4];      // XYOFFSET_1's X and Y, XYOFFSET_2's (12.4)
    float screen[16];       // view to screen (pictures between: sx, sy, cx, cy and cz, as nd_000.vert's)
    NdBetween bt[2];        // (ND_C_MODEL) set A's and set B's moves from the frame before (nd_between)
};
static_assert(sizeof(Nd280Const) == 71 * 16, "Nd280Const is nd_280.vert's Draw");
const uint32_t ND280_CORNER_CONTEXT2 = 1u << 22, ND280_CORNER_SPHERE = 1u << 23, ND280_RECORDS = 4096;
const uint32_t ND280_SLOTS = 2; // (arena places a vertex: eight qwords)

struct Nd280State {
    bool pending = false; // gs->verts holds 0x280's corners
    NdBuffer draws;       // the records
    const Buffer *batch_draws = nullptr; // the records the pending corners name (and the batch being drawn)
    std::unordered_map<Program *, Program *> programs;
    Shader *vs = nullptr;
} nd280;

// (NdFlush, the hook in gsn_native_draw.inc: after nd.active is set, and when the flush ends)
void nd280_flush(bool begin) {
    if (begin) {
        nd280.pending = false;
    }
}

// (nd_draw_range, the hook) the records the batch's 0x280 corners name
const Buffer *nd280_batch_draws() {
    return nd280.batch_draws;
}

// (nd_draw_range, the hook: a stretch of 0x280 corners) the GS's program p with nd_280.vert
Program *nd280_program(Program *p) {
    auto it = nd280.programs.find(p);
    if (it != nd280.programs.end()) {
        return it->second;
    }
    if (!nd280.vs) {
        ResourceLayout vert = {};
        vert.input_mask = (1u << 2) | (1u << 4);
        vert.push_constant_size = sizeof(Push);
        vert.sets[1].storage_buffer_mask = 1u;
        vert.sets[1].meta[0].array_size = 1;
        vert.sets[2].storage_buffer_mask = 3u;
        vert.sets[2].meta[0].array_size = 1;
        vert.sets[2].meta[1].array_size = 1;
        nd280.vs = gs->device->request_shader(gsn_spv_nd_280_vert, sizeof(gsn_spv_nd_280_vert), &vert);
    }
    Program *np = gs->device->request_program(nd280.vs, p->get_shader(ShaderStage::Fragment));
    nd280.programs[p] = np;
    return np;
}

// VU1's arithmetic on float bits (vu_float.h): a VU float with exponent 255 is a number, never touched
// as a host float
uint32_t vb_mul(uint32_t a, uint32_t b) {
    return vu_float_mul(a, b);
}
uint32_t vb_add(uint32_t a, uint32_t b) {
    return vu_float_add(a, b);
}
uint32_t vb(float f) {
    return vu_float_bits(f);
}
// M v (MULAx ACC; MADDAy; MADDAz; MADDw), m a qword a column
void vb_transform4(uint32_t out[4], const float *m, const uint32_t v[4]) {
    for (int r = 0; r < 4; r++) {
        uint32_t acc = vb_mul(vb(m[r]), v[0]);
        acc = vb_add(acc, vb_mul(vb(m[4 + r]), v[1]));
        acc = vb_add(acc, vb_mul(vb(m[8 + r]), v[2]));
        out[r] = vb_add(acc, vb_mul(vb(m[12 + r]), v[3]));
    }
}

// A vertex's set: its ST's w, as ILW loads it
int nd280_set(const int32_t *q) {
    return (uint32_t(q[11]) & 0xFFFF) != 0;
}

// A vertex's first pass as VU1 works it out (vu1_hle_280.c's vertex_as): q its eight qwords. xyz: FTOI4 of
// the projected place; stq: ST by 1/w; the clip judgement; the alpha as the GS takes it (u's: its colour's
// alpha by the scale, plus the bias, at most 255, FTOI0; the low 8 bits)
void nd280_plan_vertex(const int32_t *q, const Nd280Const &k, int32_t xyz[3], float stq[3], bool &clip, uint32_t &alpha) {
    uint32_t p[4] = {uint32_t(q[0]), uint32_t(q[1]), uint32_t(q[2]), uint32_t(q[3])}, c[4];
    vb_transform4(c, k.vertex[nd280_set(q)], p);
    uint32_t qq = vu_float_div(0x3F800000u, c[3]);
    uint32_t x = vb_mul(c[0], qq), y = vb_mul(c[1], qq), z = vb_mul(c[2], qq);
    // VU1's judgement (vu1_hle_ops.h, judge and clipped): past the guard band in X or Y, or Z below 0
    uint32_t rx = vu_float_sub(x, 0x45000000u), ry = vu_float_sub(y, 0x45000000u); // (- 2048)
    clip = (rx & 0x7FFFFFFFu) > 0x44FFE000u || (ry & 0x7FFFFFFFu) > 0x44FFE000u ||    // (2047)
           ((z & 0x80000000u) && (z & 0x7FFFFFFFu) > 0x007FFFFFu);
    xyz[0] = vu_float_ftoi(x, 4);
    xyz[1] = vu_float_ftoi(y, 4);
    xyz[2] = vu_float_ftoi(z, 4);
    for (int i = 0; i < 3; i++) {
        stq[i] = vu_float_from_bits(vb_mul(uint32_t(q[8 + i]), qq));
    }
    uint32_t u = vb_add(vb_mul(uint32_t(q[15]), vb(k.scale[3])), vb(k.bias[3]));
    alpha = uint32_t(vu_float_ftoi(vu_float_mini(u, 0x437F0000u), 0)) & 0xFF; // (lit.w is 1: 1 * u)
}

// The second pass's S and T (vu1_hle_280.c's sphere_vertex): ((sphere matrix * normal).xy + 1 * 1) * 0.5
void nd280_sphere_st(const int32_t *q, const Nd280Const &k, float st[2]) {
    const float *m = k.sphere[nd280_set(q)];
    for (int r = 0; r < 2; r++) {
        uint32_t acc = vb_mul(vb(m[r]), uint32_t(q[4]));
        acc = vb_add(acc, vb_mul(vb(m[4 + r]), uint32_t(q[5])));
        acc = vb_add(acc, vb_mul(vb(m[8 + r]), uint32_t(q[6])));
        acc = vb_add(acc, vb_mul(0x3F800000u, 0x3F800000u));
        st[r] = vu_float_from_bits(vb_mul(acc, 0x3F000000u));
    }
}

// A batch planned (vu1_hle_280.c's run: which vertices are clipped, ADC, whether the run ends without a
// strip): q its first vertex's eight qwords
struct Nd280Plan {
    uint32_t n = 0;
    int32_t xyz[N_ND_MAX][3];
    float stq[N_ND_MAX][3], sphere[N_ND_MAX][2];
    uint32_t alpha[N_ND_MAX];
    bool clip[N_ND_MAX], adc[N_ND_MAX]; // adc: VU1's ADC of the kick (clipping's and the tag's; not the data's)
    bool all_clipped = false;           // the count of clipped vertices reached n: no strip, no second pass
};

void nd280_plan(const int32_t *q, const RdBatch &b, const Nd280Const &k, Nd280Plan &p) {
    p.n = b.count;
    uint32_t count = 0;
    for (uint32_t i = 0; i < p.n; i++) {
        const int32_t *v = q + size_t(i) * 32;
        nd280_plan_vertex(v, k, p.xyz[i], p.stq[i], p.clip[i], p.alpha[i]);
        if (b.sphere) {
            nd280_sphere_st(v, k, p.sphere[i]);
        }
        count += p.clip[i];
    }
    // ("vertex -1": vertex 0's clip seen again before the loop, by the count, ADC and the tag's w)
    count += p.clip[0];
    bool adc_tag = (uint16_t(b.tag[1] >> 32) & 1) && p.clip[0];
    for (uint32_t i = 0; i < p.n; i++) {
        p.adc[i] = p.clip[i] || (i >= 1 ? p.clip[i - 1] : p.clip[0]) || (i >= 2 && p.clip[i - 2]) || adc_tag;
    }
    p.all_clipped = int16_t(uint16_t(count - p.n)) >= 0;
}

// (pf/skin) nd280_plan with the host's floats: VU1's arithmetic is IEEE's rounded toward zero, with denormals read and
// written as zero (vu_float.h), so with the SSE unit set so (MXCSR: round toward zero, DAZ, FTZ) each multiply, add and
// divide gives VU1's bits, four lanes at a time. Not where VU1's own rules differ: a value with exponent 255 (a number to
// VU1, an infinity or NaN here), a result that large (VU1's largest is 0x7FFFFFFF, the host's FLT_MAX), a division by
// zero, an FTOI past the integers: any value with exponent 254 or more, read or made, or a divisor with exponent 0, and
// the batch is planned by nd280_plan. KF4_ND280_PLAN_CHECK=1 (tests) plans every batch both ways and counts those that
// differ ([stats] native draw).
#if defined(__clang__) && (defined(__x86_64__) || defined(_M_X64))
#define ND280_PLAN_FAST 1
#include <immintrin.h>
uint64_t nd280_plan_fast_n = 0, nd280_plan_fast_slow = 0, nd280_plan_check_differ = 0;

static inline __m128 nd280_bits(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return _mm_castsi128_ps(_mm_set_epi32(int32_t(d), int32_t(c), int32_t(b), int32_t(a)));
}

// lanes with exponent 254 or 255
static inline __m128i nd280_big(__m128 v) {
    __m128i e = _mm_and_si128(_mm_castps_si128(v), _mm_set1_epi32(0x7F800000));
    return _mm_cmpgt_epi32(e, _mm_set1_epi32(0x7EFFFFFF));
}

bool nd280_plan_fast(const int32_t *q, const RdBatch &b, const Nd280Const &k, Nd280Plan &p) {
#pragma STDC FENV_ACCESS ON
    // the inputs read once: the matrices, the colour's scale and bias
    __m128i big = _mm_setzero_si128();
    __m128 col[2][4], sph[2][3];
    for (int s = 0; s < 2; s++) {
        for (int c = 0; c < 4; c++) {
            col[s][c] = _mm_loadu_ps(&k.vertex[s][4 * c]);
            big = _mm_or_si128(big, nd280_big(col[s][c]));
        }
        for (int c = 0; c < 3; c++) {
            sph[s][c] = _mm_loadu_ps(&k.sphere[s][4 * c]);
            if (b.sphere) {
                big = _mm_or_si128(big, nd280_big(sph[s][c]));
            }
        }
    }
    __m128 sb = _mm_set_ps(0.0f, 0.0f, k.bias[3], k.scale[3]);
    big = _mm_or_si128(big, nd280_big(sb));
    if (_mm_movemask_epi8(big)) {
        return false;
    }
    unsigned int csr = _mm_getcsr();
    _mm_setcsr((csr & ~0x6000u) | 0x6000u | 0x8000u | 0x0040u); // (round toward zero, FTZ, DAZ)
    const __m128 one = _mm_set1_ps(1.0f), half = _mm_set1_ps(0.5f), m2048 = _mm_set1_ps(2048.0f);
    const float scale = k.scale[3], bias = k.bias[3];
    bool ok = true;
    p.n = b.count;
    uint32_t count = 0;
    for (uint32_t i = 0; i < p.n && ok; i++) {
        const int32_t *v = q + size_t(i) * 32;
        int set = nd280_set(v);
        __m128 pos = nd280_bits(uint32_t(v[0]), uint32_t(v[1]), uint32_t(v[2]), uint32_t(v[3]));
        __m128 st_in = nd280_bits(uint32_t(v[8]), uint32_t(v[9]), uint32_t(v[10]), uint32_t(v[15]));
        __m128i vbig = _mm_or_si128(nd280_big(pos), nd280_big(st_in));
        // c = M v, as vb_transform4: (((m0 v0 + m1 v1) + m2 v2) + m3 v3), each step rounded
        __m128 v0 = _mm_shuffle_ps(pos, pos, 0x00), v1 = _mm_shuffle_ps(pos, pos, 0x55);
        __m128 v2 = _mm_shuffle_ps(pos, pos, 0xAA), v3 = _mm_shuffle_ps(pos, pos, 0xFF);
        __m128 m0 = _mm_mul_ps(col[set][0], v0), m1 = _mm_mul_ps(col[set][1], v1);
        __m128 m2 = _mm_mul_ps(col[set][2], v2), m3 = _mm_mul_ps(col[set][3], v3);
        __m128 acc = _mm_add_ps(m0, m1);
        __m128 acc2 = _mm_add_ps(acc, m2);
        __m128 c = _mm_add_ps(acc2, m3);
        vbig = _mm_or_si128(vbig, _mm_or_si128(_mm_or_si128(nd280_big(m0), nd280_big(m1)),
                                              _mm_or_si128(nd280_big(m2), nd280_big(m3))));
        vbig = _mm_or_si128(vbig, _mm_or_si128(_mm_or_si128(nd280_big(acc), nd280_big(acc2)), nd280_big(c)));
        __m128 cw = _mm_shuffle_ps(c, c, 0xFF);
        uint32_t cw_bits = uint32_t(_mm_cvtsi128_si32(_mm_castps_si128(cw)));
        if ((cw_bits & 0x7F800000u) == 0) {
            ok = false; // (a divisor with exponent 0: VU1's largest float)
            break;
        }
        __m128 qq = _mm_div_ps(one, cw);
        __m128 xyz = _mm_mul_ps(c, qq);
        __m128 stq = _mm_mul_ps(st_in, qq); // (lane 3: unused)
        __m128 r = _mm_sub_ps(xyz, m2048);
        __m128 u = _mm_add_ps(_mm_mul_ps(_mm_shuffle_ps(st_in, st_in, 0xFF), _mm_set1_ps(scale)), _mm_set1_ps(bias));
        vbig = _mm_or_si128(vbig, _mm_or_si128(_mm_or_si128(nd280_big(qq), nd280_big(xyz)),
                                              _mm_or_si128(nd280_big(r), nd280_big(u))));
        vbig = _mm_or_si128(vbig, nd280_big(_mm_mul_ps(_mm_shuffle_ps(st_in, st_in, 0xFF), _mm_set1_ps(scale))));
        vbig = _mm_or_si128(vbig, nd280_big(_mm_shuffle_ps(stq, stq, 0xA4))); // (lanes 0-2)
        if (b.sphere) {
            __m128 nrm = nd280_bits(uint32_t(v[4]), uint32_t(v[5]), uint32_t(v[6]), 0);
            vbig = _mm_or_si128(vbig, nd280_big(nrm));
            __m128 s0 = _mm_mul_ps(sph[set][0], _mm_shuffle_ps(nrm, nrm, 0x00));
            __m128 s1 = _mm_mul_ps(sph[set][1], _mm_shuffle_ps(nrm, nrm, 0x55));
            __m128 s2 = _mm_mul_ps(sph[set][2], _mm_shuffle_ps(nrm, nrm, 0xAA));
            __m128 a0 = _mm_add_ps(s0, s1);
            __m128 a1 = _mm_add_ps(a0, s2);
            __m128 a2 = _mm_add_ps(a1, _mm_mul_ps(one, one));
            __m128 sst = _mm_mul_ps(a2, half);
            vbig = _mm_or_si128(vbig, _mm_or_si128(_mm_or_si128(nd280_big(s0), nd280_big(s1)), nd280_big(s2)));
            vbig = _mm_or_si128(vbig, _mm_or_si128(_mm_or_si128(nd280_big(a0), nd280_big(a1)),
                                                  _mm_or_si128(nd280_big(a2), nd280_big(sst))));
            float sf[4];
            _mm_storeu_ps(sf, sst);
            p.sphere[i][0] = sf[0];
            p.sphere[i][1] = sf[1];
        }
        if (_mm_movemask_epi8(vbig)) {
            ok = false;
            break;
        }
        uint32_t xb[4], rb[4];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(xb), _mm_castps_si128(xyz));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rb), _mm_castps_si128(r));
        // VU1's judgement and FTOI4 (nd280_plan_vertex's)
        p.clip[i] = (rb[0] & 0x7FFFFFFFu) > 0x44FFE000u || (rb[1] & 0x7FFFFFFFu) > 0x44FFE000u ||
                    ((xb[2] & 0x80000000u) && (xb[2] & 0x7FFFFFFFu) > 0x007FFFFFu);
        for (int j = 0; j < 3; j++) {
            if (((xb[j] >> 23) & 0xFF) >= 154) {
                ok = false; // (FTOI4 at the integers' limit)
            }
        }
        __m128i xi = _mm_cvttps_epi32(_mm_mul_ps(xyz, _mm_set1_ps(16.0f)));
        int32_t xs[4];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(xs), xi);
        p.xyz[i][0] = xs[0];
        p.xyz[i][1] = xs[1];
        p.xyz[i][2] = xs[2];
        float sf[4];
        _mm_storeu_ps(sf, stq);
        p.stq[i][0] = sf[0];
        p.stq[i][1] = sf[1];
        p.stq[i][2] = sf[2];
        uint32_t ub = uint32_t(_mm_cvtsi128_si32(_mm_castps_si128(u)));
        uint32_t um = vu_float_mini(ub, 0x437F0000u);
        if (((um >> 23) & 0xFF) >= 158) {
            ok = false;
        }
        p.alpha[i] = uint32_t(int32_t(vu_float_from_bits(um))) & 0xFF; // (FTOI0: truncated)
        count += p.clip[i];
    }
    _mm_setcsr(csr);
    if (!ok) {
        nd280_plan_fast_slow++;
        return false;
    }
    count += p.clip[0];
    bool adc_tag = (uint16_t(b.tag[1] >> 32) & 1) && p.clip[0];
    for (uint32_t i = 0; i < p.n; i++) {
        p.adc[i] = p.clip[i] || (i >= 1 ? p.clip[i - 1] : p.clip[0]) || (i >= 2 && p.clip[i - 2]) || adc_tag;
    }
    p.all_clipped = int16_t(uint16_t(count - p.n)) >= 0;
    nd280_plan_fast_n++;
    return true;
}
#endif

// (nd280_plan_cached) The plan: the host's floats where they give VU1's bits, else VU1's arithmetic
void nd280_plan_any(const int32_t *q, const RdBatch &b, const Nd280Const &k, Nd280Plan &p) {
#ifdef ND280_PLAN_FAST
    static int check = -1;
    if (check < 0) {
        check = getenv("KF4_ND280_PLAN_CHECK") && atoi(getenv("KF4_ND280_PLAN_CHECK")) > 0;
    }
    if (nd280_plan_fast(q, b, k, p)) {
        if (check) {
            static Nd280Plan e;
            nd280_plan(q, b, k, e);
            bool same = e.n == p.n && e.all_clipped == p.all_clipped;
            for (uint32_t i = 0; i < p.n && same; i++) {
                same = memcmp(e.xyz[i], p.xyz[i], 12) == 0 && memcmp(e.stq[i], p.stq[i], 12) == 0 &&
                       e.alpha[i] == p.alpha[i] && e.clip[i] == p.clip[i] && e.adc[i] == p.adc[i] &&
                       (!b.sphere || memcmp(e.sphere[i], p.sphere[i], 8) == 0);
            }
            if (!same) {
                nd280_plan_check_differ++;
                p = e;
            }
        }
        return;
    }
#endif
    nd280_plan(q, b, k, p);
}

// A batch's last plans (two: the world's camera and the reflection's), by its mesh and number, with what
// they were planned from (compared bit for bit): the frame drawn again for a picture between, or a model
// standing still, takes its plan as it was
struct Nd280Planned {
    const int32_t *data = nullptr;
    float vertex[2][16], sphere[2][16];
    float scale, bias;
    bool sphered = false;
    Nd280Plan plan;
};
struct Nd280PlannedPair {
    Nd280Planned e[2];
    uint32_t older = 0;
};
std::unordered_map<uint64_t, Nd280PlannedPair> nd280_planned;

const Nd280Plan &nd280_plan_cached(const RdMeshData &m, const RdBatch &b, const Nd280Const &k) {
    if (nd280_planned.size() >= ND_PLANNED_MAX) {
        nd280_planned.clear();
    }
    const int32_t *q = &m.vertices[size_t(b.first) * 32];
    Nd280PlannedPair &pair = nd280_planned[uint64_t(m.id) << 20 | uint64_t(&b - m.batches.data())];
    auto same = [&](const Nd280Planned &e) {
        return e.data == q && e.sphered == b.sphere && memcmp(e.vertex, k.vertex, sizeof(e.vertex)) == 0 &&
               (!b.sphere || memcmp(e.sphere, k.sphere, sizeof(e.sphere)) == 0) &&
               memcmp(&e.scale, &k.scale[3], 4) == 0 && memcmp(&e.bias, &k.bias[3], 4) == 0;
    };
    uint32_t which = same(pair.e[0]) ? 0 : same(pair.e[1]) ? 1 : 2;
    if (which == 2) {
        which = pair.older;
    }
    pair.older = which ^ 1;
    Nd280Planned &e = pair.e[which];
    if (!same(e)) {
        e.data = q;
        e.sphered = b.sphere;
        memcpy(e.vertex, k.vertex, sizeof(e.vertex));
        memcpy(e.sphere, k.sphere, sizeof(e.sphere));
        memcpy(&e.scale, &k.scale[3], 4);
        memcpy(&e.bias, &k.bias[3], 4);
        nd280_plan_any(q, b, k, e.plan);
        gsn_stats.nd_plan_vertices += b.count;
    } else {
        gsn_stats.nd_plan_reused += b.count;
    }
    return e.plan;
}

// A pass's corners into gs->verts: base the batch's first slot in the arena
void nd280_corners(const RdBatch &b, const Nd280Plan &p, bool sphere, uint32_t prim, uint32_t base, uint32_t record) {
    uint32_t context = (prim >> 9) & 1;
    uint64_t offset = gs->reg[R_XYOFFSET_1 + context];
    int32_t ox = int32_t(bits(offset, 0, 16)), oy = int32_t(bits(offset, 32, 16));
    uint32_t gouraud = ((prim >> 3) & 1) ? ND_CORNER_GOURAUD : 0;
    uint32_t flags = record << ND_CORNER_DRAW_SHIFT | (context ? ND280_CORNER_CONTEXT2 : 0) | (sphere ? ND280_CORNER_SPHERE : 0);
    for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
        uint32_t t[3] = {b.triangles[i], b.triangles[i + 1], b.triangles[i + 2]};
        uint32_t kick = t[2];
        if (p.adc[kick]) {
            continue;
        }
        for (int j = 0; j < 3; j++) {
            uint32_t v = gouraud ? t[j] : kick; // (flat: the kick's colour, as add_triangle has it)
            Vtx w;
            w.x = float(int32_t(uint32_t(p.xyz[t[j]][0]) & 0xFFFF) - ox) / 16.0f;
            w.y = float(int32_t(uint32_t(p.xyz[t[j]][1]) & 0xFFFF) - oy) / 16.0f;
            w.z = uint32_t(p.xyz[t[j]][2] >> 4) & 0xFFFFFF;
            w.rgba = (sphere ? p.alpha[1] : p.alpha[v]) << 24;
            w.s = sphere ? p.sphere[t[j]][0] : p.stq[t[j]][0];
            w.t = sphere ? p.sphere[t[j]][1] : p.stq[t[j]][1];
            w.q = sphere ? 1.0f : p.stq[t[j]][2];
            w.fog = nd280_corner_vertex(base + ND280_SLOTS * (b.first + t[j]));
            w.rgba |= (kick - t[j]) | gouraud | flags;
            gs->verts.push_back(w);
            if (interp.active) {
                interp.vsrc.push_back({GSN_IM_NONE, GSN_IM_NONE, GSN_IM_NONE, uint32_t(j)});
            }
        }
    }
}

// A draw's record: VU1's set-up (vu1_hle_280.c, setup) for each matrix set, as nd_const does it for
// program 0x000's (the vertex matrix, the light directions, the point lights: the same maths); the light
// colours, fog, sphere matrices, colour scale and bias as they are
void nd280_const(Nd280Const &k, const RdDraw &d, const RdCamera &cam) {
    memset(&k, 0, sizeof(k));
    RdDraw set = d;
    for (int s = 0; s < 2; s++) {
        NdConst c;
        memcpy(set.model, s ? d.parent : d.model, sizeof(set.model));
        nd_const(c, set, cam);
        memcpy(k.vertex[s], c.vertex, 64);
        memcpy(k.light_dir[s], c.light_dir, 64);
        memcpy(k.point_pos[4 * s], c.point_pos, 64);
        k.info[0] = c.info[0];
        memcpy(k.sphere[s], d.turn[s], 64);
    }
    memcpy(k.light_col, d.light_col, 64);
    memcpy(k.fog, d.fog, 16);
    memcpy(k.scale, d.colour[0], 16);
    memcpy(k.bias, d.colour[1], 16);
    memcpy(k.point_col, d.point_col, 64);
}

// (nd_draw, the hook: a draw of program 0x280) as nd_draw draws 0x000's
// (--renderer fast: a pass drawn by the PC renderer, gsn_pc_draw.inc)
bool pcd_pass_280(const RdBatch &b, const Nd280Plan &p, bool sphere, uint32_t base, uint32_t record, const Buffer &records);

void nd280_draw(const RdDraw &d, const RdCamera &cam) {
    gsn_stats.nd_draws++;
    bool pc_draw = sa_pcd_draw(); // (--renderer fast: gsn_pc_draw.inc)
    if (nd_buffer_room(nd280.draws, 1, nd280.pending)) {
        nd_new_buffer(nd280.draws, ND280_RECORDS, sizeof(Nd280Const), nd280.pending);
    }
    uint32_t record = nd280.draws.used++;
    Nd280Const k;
    if (!nd_prep_take_const(0x280, &k, sizeof(k)) || nd_prep.check) { // (rc/own: the frame's, in its first picture)
        Nd280Const again;
        nd280_const(again, d, cam);
        if (nd_prep.draw >= 0 && interp.active && memcmp(&again, &k, sizeof(k)) != 0) {
            nd_prep.check_differ[0]++;
        }
        k = again;
        nd_prep_keep_const(0x280, &k, sizeof(k));
    }
    for (int c = 0; c < 2; c++) {
        uint64_t offset = gs->reg[R_XYOFFSET_1 + c];
        k.offset[2 * c] = int32_t(bits(offset, 0, 16));
        k.offset[2 * c + 1] = int32_t(bits(offset, 32, 16));
    }
    // pictures between: by the camera and the part's two matrix sets' own moves (nd_model_flags)
    memcpy(k.screen, cam.screen, 64);
    NdKey key = nd_key(d);
    if (!interp.active) {
        nd_remember(key, d, cam);
    } else {
        k.info[1] = nd_model_flags(key, d, cam, &k.bt[0], &k.bt[1]);
    }
    memcpy(nd280.draws.map + size_t(record) * sizeof(Nd280Const), &k, sizeof(k));
    for (const auto &mesh : nd.fetched) {
        if (mesh->format != RD_VF_FLOAT_SKIN) {
            gsn_stats.nd_unsupported++;
            continue;
        }
        uint32_t base = nd_place(*mesh);
        for (const RdBatch &b : mesh->batches) {
            // what the run kicks: FOGCOL at an MSCAL (the set-up), the A+D packet; then, unless every vertex
            // is clipped, the strip (PRIM from its tag), and the second pass's A+D packet and strip
            if (b.start != RD_PROGRAM_CONTINUE) {
                gs_write(R_FOGCOL, d.fogcol);
            }
            for (size_t i = 0; i + 1 < b.ad.size(); i += 2) {
                gs_write(uint32_t(b.ad[i]), b.ad[i + 1]);
            }
            // (rc/own: in the frame's first picture between, the frame's corners: the plan only when one is not the frame's)
            uint32_t bno = uint32_t(&b - mesh->batches.data());
            const NdPrepBatch *clipped = nd_prep_take_batch(mesh->id, bno, 2, 0, base);
            const Nd280Plan *p = nullptr;
            if (!clipped || nd_prep.check) {
                p = &nd280_plan_cached(*mesh, b, k);
                if (clipped && clipped->all_clipped && !p->all_clipped) {
                    nd_prep.check_differ[2]++;
                }
                nd_prep_keep_batch(mesh->id, bno, 2, 0, base, 0, 0, p->all_clipped);
            }
            gsn_stats.nd_batches++;
            if (p ? p->all_clipped : clipped->all_clipped) {
                continue;
            }
            if (pc_draw && !p) {
                p = &nd280_plan_cached(*mesh, b, k); // (the PC renderer's triangles are the plan's: pcd_pass_280)
            }
            for (int pass = 0; pass < 1 + b.sphere; pass++) {
                if (pass) {
                    for (size_t i = 0; i + 1 < b.ad2.size(); i += 2) {
                        gs_write(uint32_t(b.ad2[i]), b.ad2[i + 1]);
                    }
                }
                gs_write(R_PRIM, uint32_t(((pass ? b.tag2[0] : b.tag[0]) >> 47) & 0x7FF));
                if (pc_draw && pcd_pass_280(b, *p, pass != 0, base, record, *nd280.draws.buffer)) {
                    continue; // (drawn by the PC renderer from the mesh: gsn_pc_draw.inc)
                }
                // (as nd_draw's: the strip joins the batch, the PS2 path's corners and 0x000's in it too; a
                // batch of other buffers is drawn first, and in a picture between a batch of the PS2
                // path's: nd_kick)
                if (!gs->verts.empty() &&
                    ((interp.active && !nd.pending) ||
                     (nd.pending && (ndw_pending() || nd.batch_arena != nd.arena.buffer.get() ||
                                     (nd280.pending && nd280.batch_draws != nd280.draws.buffer.get()))))) {
                    flush_batch();
                }
                uint32_t eprim = effective_prim();
                if (gs->verts.empty()) {
                    gs->batch_prim = eprim;
                    gs->batch_sprites = false;
                }
                size_t before = gs->verts.size();
                const NdPrepBatch *taken = nd_prep_take_batch(mesh->id, bno, uint32_t(pass), eprim, base);
                if (taken) {
                    nd_prep_corners(*taken, record);
                }
                if (!taken || nd_prep.check) {
                    size_t again = gs->verts.size();
                    if (!p) {
                        p = &nd280_plan_cached(*mesh, b, k);
                    }
                    nd280_corners(b, *p, pass != 0, eprim, base, record);
                    if (taken) {
                        nd_prep_check(before, again);
                    }
                    nd_prep_keep_batch(mesh->id, bno, uint32_t(pass), eprim, base, record, before);
                }
                if (gs->verts.size() > before) {
                    nd.pending = nd280.pending = true;
                }
                nd.batch_arena = nd.arena.buffer.get();
                nd280.batch_draws = nd280.draws.buffer.get();
            }
        }
    }
    gs->queue_count = 0;
}

// ---- The vertex oracle for program 0x280 (kf4-gsreplay --nd-oracle: tools/gsreplay/nd_oracle.c) ------
// As gsn_nd_oracle.inc's for 0x000: nd_280.vert built with ND_ORACLE writes what each corner gives the
// GS; the batch is made by the parser's own code from VU1 memory as the run found it
// (rd_batch_280_from_vu), the record by the CPU's set-up (an MSCAL's, from the fixed block) or from the
// registers and memory the set-up before left (an MSCNT's), the plan by nd280_plan.

} // namespace (gsn.cpp's: its C entry points below are outside it)
#include "gsn_nd280_oracle.h"
namespace {

// A run's record as the shader takes it, and the CPU set-up's results to compare (an MSCAL's)
void nd280_oracle_const(Nd280Const &k, const GsnNd280OracleDraw &o, GsnNd280OracleBatch &r) {
    memset(&k, 0, sizeof(k));
    if (o.mscal) {
        RdDraw d = {};
        RdCamera cam = {};
        memcpy(cam.world_screen, o.mem[0x00], 64);
        memcpy(d.model, o.mem[0x04], 64);
        memcpy(d.parent, o.mem[0x08], 64);
        memcpy(d.light_dir, o.mem[0x0C], 64);
        memcpy(d.light_col, o.mem[0x10], 64);
        memcpy(d.turn[0], o.mem[0x24], 64);
        memcpy(d.turn[1], o.mem[0x28], 64);
        memcpy(d.fog, o.mem[0x3C], 16);
        int32_t w;
        memcpy(&w, &o.mem[0x40][3], 4);
        d.point_count = w;
        memcpy(d.point_pos, o.mem[0x41], 64);
        memcpy(d.point_col, o.mem[0x45], 64);
        memcpy(d.colour[0], o.mem[0x64], 16);
        memcpy(d.colour[1], o.mem[0x65], 16);
        nd280_const(k, d, cam);
        // (as VU memory has them after the set-up: 0x14-0x17, 0x1C-0x1F; 0x18-0x1A, 0x20-0x22; 0x4A-0x4D,
        // 0x53-0x56; 0x2C-0x2E, 0x30-0x32; 0x72, 0x73)
        for (int s = 0; s < 2; s++) {
            memcpy(r.setup_vertex[s], k.vertex[s], 64);
            memcpy(r.setup_light_dir[s], k.light_dir[s], 48);
            memcpy(r.setup_point_pos[s], k.point_pos[4 * s], 64);
            memcpy(r.setup_sphere[s], k.sphere[s], 48);
        }
        memcpy(r.setup_colour[0], k.scale, 16);
        memcpy(r.setup_colour[1], k.bias, 16);
    } else {
        for (int s = 0; s < 2; s++) {
            memcpy(k.vertex[s], o.mem[s ? 0x1C : 0x14], 64);
            memcpy(k.light_dir[s], o.mem[s ? 0x20 : 0x18], 48);
            memcpy(k.point_pos[4 * s], o.mem[s ? 0x53 : 0x4A], 64);
            memcpy(k.sphere[s], o.mem[s ? 0x30 : 0x2C], 48);
        }
        memcpy(k.light_col, o.vf[8], 64);
        memcpy(k.point_col, o.mem[0x4E], 64);
        memcpy(k.scale, o.mem[0x72], 16);
        memcpy(k.bias, o.mem[0x73], 16);
        int32_t w;
        memcpy(&w, &o.mem[0x49][3], 4);
        k.info[0] = std::max(0, std::min(4, int32_t(int16_t(w))));
    }
    memcpy(k.fog, o.mem[0x3C], 16);
}

// the batch of a run from its memory (the parser's code)
bool nd280_oracle_batch(const GsnNd280OracleDraw &o, RdMeshData &mesh) {
    static uint32_t mem[1024][4];
    memset(mem, 0, sizeof(mem));
    for (uint32_t i = 0; i < GSN_ND280_ORACLE_BATCH_QWORDS; i++) {
        memcpy(mem[(o.top + i) & 0x3FF], o.batch[i], 16);
    }
    mesh = RdMeshData();
    return rd_batch_280_from_vu(mem, o.top, o.mscal ? 0x280 : RD_PROGRAM_CONTINUE, mesh);
}

// The blocks VIF1 was sent, parsed (gsn_nd280_oracle_block), for the runs they start
struct {
    RdMeshData mesh;
    size_t next = 0;
} nd280_block;

} // namespace

extern "C" int gsn_nd280_oracle_block(const void *words, uint32_t qwords, uint32_t program, const char **why) {
    RdMeshData &m = nd280_block.mesh;
    m = RdMeshData();
    nd280_block.next = 0;
    *why = "";
    if (!rd_parse_block(words, qwords, RD_VF_FLOAT_SKIN, m)) {
        *why = rd_mesh_why();
        m = RdMeshData();
        return 0;
    }
    // (only a block whose first run is 0x280's: an MSCAL 0x280, or an MSCNT that goes on with it)
    if (m.batches[0].start == RD_PROGRAM_CONTINUE && program != 0x280) {
        m = RdMeshData();
        return 0;
    }
    return int(m.batches.size());
}

extern "C" int gsn_nd280_oracle_block_check(const GsnNd280OracleDraw *run, const char **what) {
    RdMeshData &m = nd280_block.mesh;
    if (nd280_block.next >= m.batches.size()) {
        return -1;
    }
    const RdBatch &a = m.batches[nd280_block.next++];
    RdMeshData own;
    *what = "";
    if (!nd280_oracle_batch(*run, own)) {
        *what = "the run's own batch is refused";
        return 0;
    }
    const RdBatch &b = own.batches[0];
    if (a.start != b.start) {
        *what = "MSCAL or MSCNT";
    } else if (memcmp(a.tag, b.tag, 16) != 0 || a.sphere != b.sphere || (a.sphere && memcmp(a.tag2, b.tag2, 16) != 0)) {
        *what = "the tags or the second pass";
    } else if (a.ad != b.ad || a.ad2 != b.ad2) {
        *what = "the A+D packets";
    } else if (a.count != b.count || a.triangles != b.triangles) {
        *what = "the triangles";
    } else if (memcmp(&m.vertices[size_t(a.first) * 32], &own.vertices[0], size_t(a.count) * 128) != 0) {
        *what = "the vertices";
    }
    return **what == 0;
}

extern "C" int gsn_nd280_oracle_run(const GsnNd280OracleDraw *draws, uint32_t draw_count, GsnNd280OracleBatch *batches,
                                    GsnNd280OracleVertex *out) {
    if (!gsn_nd_oracle_ready() || draw_count == 0 || draw_count > (1u << 15)) {
        return 0;
    }
    ring_idle();
    Device &dev = *gs->device;
    // the batches (the parser's), the records (the set-up's), the plans; and the corners: a vertex's
    // first pass, then its second's if the batch has one; out the same, a vertex's passes (n * 2 a run)
    std::vector<Nd280Const> consts(draw_count);
    std::vector<int32_t> arena;
    struct Corner {
        uint32_t rgba;
        float vertex;
    };
    std::vector<Corner> corners;
    std::vector<uint32_t> corner_out;
    uint32_t out_at = 0;
    Nd280Plan p;
    for (uint32_t i = 0; i < draw_count; i++) {
        GsnNd280OracleBatch &r = batches[i];
        memset(&r, 0, sizeof(r));
        nd280_oracle_const(consts[i], draws[i], r);
        RdMeshData mesh;
        r.out = out_at;
        if (!nd280_oracle_batch(draws[i], mesh)) {
            r.why = rd_mesh_why();
            continue;
        }
        const RdBatch &b = mesh.batches[0];
        r.ok = 1;
        r.n = b.count;
        r.sphere = b.sphere;
        r.adc_tag_odd = (b.tag[1] >> 32) & 1;
        nd280_plan(mesh.vertices.data(), b, consts[i], p);
        r.all_clipped = p.all_clipped;
        uint32_t slot = uint32_t(arena.size() / 16);
        arena.insert(arena.end(), mesh.vertices.begin(), mesh.vertices.end());
        for (uint32_t k = 0; k < b.count; k++) {
            for (int pass = 0; pass < 2; pass++) {
                GsnNd280OracleVertex &v = out[out_at + 2 * k + pass];
                memset(&v, 0, sizeof(v));
                memcpy(v.plan_xyz, p.xyz[k], 12);
                if (pass) {
                    v.plan_stq[0] = b.sphere ? p.sphere[k][0] : 0.0f;
                    v.plan_stq[1] = b.sphere ? p.sphere[k][1] : 0.0f;
                    v.plan_stq[2] = 1.0f;
                } else {
                    memcpy(v.plan_stq, p.stq[k], 12);
                }
                v.plan_alpha = pass ? p.alpha[1] : p.alpha[k];
                v.plan_clip = p.clip[k];
                v.plan_adc = p.adc[k];
                if (pass && !b.sphere) {
                    continue;
                }
                corners.push_back({ND_CORNER_GOURAUD | i << ND_CORNER_DRAW_SHIFT | (pass ? ND280_CORNER_SPHERE : 0),
                                   nd280_corner_vertex(slot + ND280_SLOTS * k)});
                corner_out.push_back(out_at + 2 * k + pass);
            }
        }
        out_at += 2 * b.count;
    }
    if (corners.empty()) {
        return 1;
    }
    while (corners.size() % 3) {
        corners.push_back(corners[0]);
    }
    // the program
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
            dev.request_shader(gsn_spv_nd_280_oracle_vert, sizeof(gsn_spv_nd_280_oracle_vert), &vert),
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
    BufferHandle records = buffer(VkDeviceSize(draw_count) * sizeof(Nd280Const), consts.data(), BufferDomain::Device);
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
    const uint32_t *r = static_cast<const uint32_t *>(dev.map_host_buffer(*results, MEMORY_ACCESS_READ_BIT));
    for (size_t i = 0; i < corner_out.size(); i++) {
        memcpy(out[corner_out[i]].gpu, r + i * 8, 32);
    }
    dev.unmap_host_buffer(*results, MEMORY_ACCESS_READ_BIT);
    return 1;
}

namespace {
