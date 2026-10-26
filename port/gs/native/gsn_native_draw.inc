// ---- Native draw: the game's draws at its own level, hosted in the GS's stream ----------------------
// (docs/port/NATIVE_DRAW.md, "Hosted mode"; port/include/port/render.h.) Included by gsn.cpp, inside its
// namespace, before flush_batch: the native GS's own code, kept in a file of its own.
//
// The patched game queues, where it would have sent a model part's packets, a GS register write to
// PORT_GS_REG_NATIVE_DRAW (0x7B) naming a draw in the renderer core's side buffer: the part's meshes (its
// data blocks, parsed once) and what VU1's fixed block would have held for it. Met here, in order with
// the rest of the frame, it is drawn the way VU1 and the GS would have drawn the packets: for each
// batch (one VU1 run) the A+D registers the run kicks are written as the GS gets them (TEX0, TEX1, ALPHA
// ... and FOGCOL at the set-up), PRIM from the strip's tag, and the batch's triangles join the GS's
// batch as the PS2 path's would: the batch goes on over runs, blocks and parts until a register changes
// or something else is drawn, and then flush_batch draws it like any other: the same targets, texture
// cache, blending, Z and layer rules, render passes and recording for the pictures between. (Those
// decide by the batch: drawn in other batches than the PS2 path's, a band of a blended floor came out a
// level apart.) Only the vertices differ: shaders/nd_000.vert does what VU1's program 0x000 did
// (port/platform/vu1_hle.c is its specification, its arithmetic too), from the meshes' vertices on the
// GPU and the draws' constants.
//
// flush_batch decides much from the vertices (the rows a texture is read to, the scissor, which
// triangles overlap and so go in runs, the layers, the rows written). So the batch's triangles are also
// worked out here on the CPU, as far as their places go (the "plan", in VU1's arithmetic: positions, Z,
// the clipping that gives VU1's ADC, alpha and ST from the data), and gs->verts holds them; each
// corner's fog and RGB say which vertex the shader is to make of it, and with which draw's constants.
// What is drawn is the shader's.
//
// On the GPU: the meshes' vertices in one buffer (the arena: a mesh is put there when first drawn, and
// stays), the draws' constants in another (a record a draw, appended), both written by the CPU in place
// (host-visible), never over what a draw already reads. A batch's corners all name the same two.
//
// --native-draw-compare queues the PS2 path's packets as well, between RD_ND_ALT_BEGIN and _END after
// the native draws: the GS draws one of the two (gsn_set_native_draw), so two runs, identical in
// everything the game does, draw the frame each way.

void flush_batch();
void gs_write(uint32_t address, uint64_t value);
// (program 0x280's draws: gsn_nd_280.inc, included at the end; these are its hooks below)
void nd280_flush(bool begin);
Program *nd280_program(Program *p);
const Buffer *nd280_batch_draws();
void nd280_draw(const RdDraw &d, const RdCamera &cam);
void nd_strip_draw(const RdDraw &d); // (a PATH2 strip's draws, the trail: gsn_nd_strip.inc)
struct NdConst;
void nd5a0_const(NdConst &k); // (program 0x5A0's records, the shadows: gsn_nd_5a0.inc)
Program *nd_strip_program(Program *p);
void nd700_draw(const RdDraw &d, const RdCamera &cam); // (program 0x700's, the effects: gsn_nd_700.inc)
bool nd700_corner(const Vtx &v);
Program *nd700_program(Program *p);
// (the reflection's rippled water: gsn_nd_water.inc, included at the end; these are its hooks below)
void ndw_flush(bool begin);
bool ndw_batch();
bool ndw_pending();
Program *ndw_program(Program *p);
void ndw_draw(const RdDraw &d, const RdCamera &cam);
void nd_pass_draw(const RdDraw &d); // (a screen pass: gsn_nd_pass.inc)
// (standalone, P8: whether a draw goes to the scene's targets: gsn_standalone.inc)
void sa_draw_begin(uint64_t value);
void sa_draw_end();
bool sa_waiting();
Program *nd_pass_program(Program *p);

// A model part's move in the pictures between (P2-5; nd_between): its matrix in the frame before (P) and in
// this frame (C), each split into a rotation R and the rest S (R S the linear part: a scale, or a shear), and
// the translation T. At the picture's time t (u = 1 - t) the part is R_C Q(u) S(u) p + T(u), Q(u) the turn
// from R_C to R_P (R_P = R_C Q(1)) by u of its angle, S(u) = S_C + (S_P - S_C) u, T(u) = T_C + (T_P - T_C) u:
// the rotation by quaternion slerp, the rest along the line (NATIVE_DRAW.md 3.5). The shader adds
// R_C (Q(u) S(u) - S_C) p + (T_P - T_C) u p.w to where the part is in this frame, which is exactly 0 at t 1.
struct NdBetween {
    float rot[12];  // R_C, a qword a column (w 0)
    float s[12];    // S_C, a qword a column
    float ds[12];   // S_P - S_C
    float axis[4];  // Q(1) in R_C's frame: its axis, and its angle (0 to pi) in w
    float dt[4];    // T_P - T_C
};
static_assert(sizeof(NdBetween) == 11 * 16, "NdBetween is the shaders' Between");

// nd_000.vert's Draw (std430, a record a draw): matrices are VU layout, a qword a column; the vertex
// matrix, the light directions and the point lights as VU1's set-up leaves them (nd_draw: its arithmetic)
struct NdConst {
    float vertex[16];    // VU1 0x14-0x17: world to screen by the part's matrix
    float model[16];     // the part to the world, the world to view, view to screen (pictures between:
                         // the shader takes the screen's sx, sy, cx, cy and cz; the others are for P1-4 and later)
    float view[16];
    float screen[16];
    float light_dir[16]; // vf05-vf07: the light directions by the part's rotation (a column each)
    float light_col[16];
    float fog[4];
    float point_pos[16]; // in the model's space, as VU1's set-up leaves them; w the falloff
    float point_col[16];
    float obj[4];        // (ND_C_OBJ) the part's move from the frame before to this one, in the world
    int32_t info[4];     // the point lights, XYOFFSET's X and Y (12.4), ND_C_* flags
    NdBetween bt;        // (ND_C_MODEL) the part's own move from the frame before (nd_between)
    uint32_t wave[16];   // (ND_C_WAVE) the 16 waves' offsets to S (low 16 bits) and T (high), RdDraw.parent's
};
static_assert(sizeof(NdConst) == 50 * 16, "NdConst is nd_000.vert's Draw");
enum : int32_t {
    ND_C_MOVING = 0x1, // a picture between: moved by the picture's camera (set 1's Picture), see nd_section
    ND_C_OBJ = 0x2,    // ... and with its model, t of the way from its place in the frame before (obj)
    ND_C_MODEL = 0x4,  // ... and with its model, by its matrices between the frame before's and this one's (bt)
    ND_C_EYE = 0x8,    // (with ND_C_MODEL) the matrices are the eye's (the arm): not moved by the camera
    ND_C_WAVE = 0x10,  // the wave UVs on the GPU (RD_DRAW_WAVE_UVS, P6-2): S and T moved by their wave's offsets
    ND_C_FAR = 0x20,   // at the far plane (RD_DRAW_FAR_Z: the modern sky, P3-2): Z 0, in the frame and between
};
// A corner (Vtx): its fog -1 less the vertex's number in the arena (below 0: a native corner, where the
// PS2 path's fog is 0-255, so that a batch can hold both kinds: nd_draw_range), and for program 0x280's
// a further 2^22 less (nd280_corner_vertex: a batch holds 0x000's and 0x280's corners too, as the PS2
// path's batch would); its RGB the kick's number less the vertex's (bits 0-5), IIP (bit 6) and the draw's
// record (bits 7-23); its alpha the vertex's (flush_batch's)
const uint32_t ND_CORNER_GOURAUD = 0x40, ND_CORNER_DRAW_SHIFT = 7, ND_DRAWS_MAX = 1u << 17;
const uint32_t ND_ARENA_MAX = 1u << 22; // (vertices, or 0x280's slots: a corner names one in 22 bits)
float nd_corner_vertex(uint32_t at) {
    return -1.0f - float(at); // (exact: the arena has 2^22 vertices at most)
}
float nd280_corner_vertex(uint32_t slot) {
    return -1.0f - float(ND_ARENA_MAX) - float(slot); // (exact: 2^23 at most; nd_280.vert takes it back)
}
// a corner's kind: the PS2 path's, program 0x000's or program 0x280's
enum { ND_KIND_PS2, ND_KIND_000, ND_KIND_280 };
int nd_corner_kind(const Vtx &v) {
    return v.fog >= 0.0f ? ND_KIND_PS2 : v.fog > -1.0f - float(ND_ARENA_MAX) ? ND_KIND_000 : ND_KIND_280;
}

const uint32_t N_ND_MAX = 40;          // (a batch's vertices at most: RdBatch)
const uint32_t ND_VERTEX_BYTES = 64;   // (RdMeshData: four qwords a vertex)

struct NdBuffer {
    BufferHandle buffer;
    uint8_t *map = nullptr;
    uint32_t used = 0, room = 0; // vertices or records
};

// A draw's identity (RdDraw's kind and id: stable from frame to frame), the section it is drawn in, and
// which draw of that identity and section in the frame it is (the held weapon 0x2E draws its parts twice;
// a model drawn in the reflection and in the world is two): nd_key
struct NdKey {
    uint32_t k[6];
    bool operator==(const NdKey &o) const {
        return memcmp(k, o.k, sizeof(k)) == 0;
    }
};
struct NdKeyHash {
    size_t operator()(const NdKey &key) const {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t v : key.k) {
            h = (h ^ v) * 0x100000001b3ull;
        }
        return size_t(h ^ (h >> 29));
    }
};
enum { ND_SECTION_NONE, ND_SECTION_WORLD, ND_SECTION_REFLECT, ND_SECTION_OTHER, ND_SECTION_ARM };

// Where a draw's part was in a frame, for the pictures between of the frame after it: its matrix (VU1
// 0x04), its parent's (0x08, program 0x280's set B) and the camera's world to view
struct NdPlace {
    float model[16], parent[16], view[16];
};
// A strip's points in a frame (the trail: gsn_nd_strip.inc), X | Y << 16 (12.4) and RGBA each
struct NdStripPoints {
    std::vector<uint32_t> xy, rgba;
};

// A batch's last plan (nd_plan), by its mesh and number: what it was planned from, and its corners
struct NdPlanned {
    const int32_t *data = nullptr; // (the mesh's vertices)
    uint64_t offset = 0;           // XYOFFSET
    uint32_t gouraud = 0;
    float vm[16] = {};             // the vertex matrix (compared bit for bit)
    bool waved = false;            // the waves' offsets (the wave UVs on the GPU)
    uint32_t wave[16] = {};
    std::vector<Vtx> corners;
};
// (two a batch: the world's camera and the reflection's, on the mirror maps)
struct NdPlannedPair {
    NdPlanned plan[2];
    uint32_t older = 0;
};
// (batches kept at most, all forgotten when there are more: a frame of 000 has about 850, a plan about
// 2 KB)
const size_t ND_PLANNED_MAX = 1u << 12;
const float ND_MAX_MOVE = 400.0f; // (frame_interp.c's INTERP_MAX_MOVE: more is not a move but a jump)

// What nd_box_at_0 needs of a draw's record (a copy of NdConst's, on the CPU): how it moves, its
// projection (sx, sy, cx, cy, as nd_000.vert's moved takes them), XYOFFSET (pixels), its obj, and cz
// (nd_z_at_0; 0x000's records only)
struct NdBoxInfo {
    int32_t flags;
    float sx, sy, cx, cy, ox, oy;
    float obj[3];
    float cz;
};

struct NdState {
    bool active = false;   // the batch flush_batch is drawing has native corners (and in a picture between, only those)
    bool pending = false;  // gs->verts holds native corners
    bool corners_active = false; // the batch flush_batch is drawing holds native corners (nd_batch_3d)
    bool skipping = false; // between RD_ND_ALT_BEGIN and _END, drawing the native draws
    bool strip_pending = false, strip_active = false; // (a picture between's strip: gsn_nd_strip.inc's batch)
    // (a picture between's screen pass: gsn_nd_pass.inc's batch; its kicks queued uncounted)
    bool pass_pending = false, pass_active = false, pass_kicking = false;
    std::unordered_map<Program *, Program *> pass_programs; // (and the GS's programs with nd_pass.vert)
    Shader *pass_vs = nullptr;
    BufferHandle pass_record; // (the pass's NdPassBetween, in a picture between)
    std::unordered_map<Program *, Program *> strip_programs; // (and the GS's programs with nd_strip.vert)
    Shader *strip_vs = nullptr;
    NdBuffer arena;        // the meshes' vertices
    std::unordered_map<RdMesh, uint32_t> placed; // a mesh's first vertex in the arena
    NdBuffer draws;        // the draws' records
    std::vector<NdBoxInfo> boxes; // (nd_box_at_0) what a picture between's box needs of each record in draws
    // the buffers the pending corners name (batch_draws: 0x000's records, when pending_000; 0x280's are
    // nd280's)
    const Buffer *batch_arena = nullptr, *batch_draws = nullptr;
    bool pending_000 = false; // gs->verts holds program 0x000's corners
    // nd.draws' records of program 0x5A0 (the shadows: nd_corner_colour), 1 each; cleared when nd.draws is
    // replaced (nd_new_buffer)
    std::vector<uint8_t> of5a0;
    std::unordered_map<Program *, Program *> programs; // the GS's programs, with nd_000.vert
    Shader *vs = nullptr;
    BufferHandle dummy;
    std::vector<std::shared_ptr<const RdMeshData>> fetched;
    std::unordered_map<uint64_t, NdPlannedPair> planned; // (nd_plan)
    // Pictures between (--frame-rate). Where the frame's marks say the draws are (frame_interp.c's
    // sections, PORT_GS_REG_FRAME_INTERP): only the world's and the reflection's move with the camera, as
    // the PS2 path's vertices do. And each draw's part's place (its matrix's translation) by its identity,
    // in this frame and the one before: a part that moved between them (the sky, which follows the
    // player: map records with flags bit 31) is drawn t of the way between its two places, as
    // frame_interp.c's object_between draws the PS2 path's sky.
    // The models' parts (objects, the picked item, entities, the arm: nd_model_move) are paired so too, by
    // their whole matrices, and drawn between their two places; the trail between its two strips.
    int section = 0;       // ND_SECTION_*
    uint32_t serial = 0xFFFFFFFFu; // the frame whose draws `now` holds (RD_ND_FRAME's)
    std::unordered_map<NdKey, NdPlace, NdKeyHash> now, before;
    std::unordered_map<NdKey, uint32_t, NdKeyHash> seen; // (nd_key: the draws of an identity so far in the frame)
    std::unordered_map<NdKey, NdStripPoints, NdKeyHash> strip_now, strip_before;
    std::unordered_map<NdKey, RdPass, NdKeyHash> pass_now, pass_before; // (the screen passes': gsn_nd_pass.inc)
} nd;
volatile int nd_mode = GSN_NATIVE_DRAW_NATIVE; // (gsn_set_native_draw)

#include "gsn_nd_prep.inc" // (pictures between: the frame's draws' records and corners kept for its first one)

// (flush_batch: whether its batch is native corners, for the whole of it)
struct NdFlush {
    NdFlush() {
        nd.active = nd.pending || nd.pass_pending;
        nd.corners_active = nd.pending;
        nd.strip_active = nd.strip_pending;
        nd.pass_active = nd.pass_pending;
        nd.pending = nd.strip_pending = nd.pending_000 = nd.pass_pending = false;
        nd280_flush(true); // (0x280's: gsn_nd_280.inc)
        ndw_flush(true);   // (the water's: gsn_nd_water.inc)
    }
    ~NdFlush() {
        nd.active = nd.corners_active = nd.strip_active = nd.pass_active = false;
        nd280_flush(false);
        ndw_flush(false);
    }
};

// Whether flush_batch's batch holds native corners: the 3D of a model or a map record, whatever its
// corners look like, so flat_2d leaves the batch alone. The PS2 path's 2D snap (at --native-scale above 1
// or with MSAA, flat_2d moves the corners of a triangle it takes for half a rectangle: one Z, one colour, its
// corners those of its box) also catches the small triangles of an entity's sphere pass (Q 1, white, one Z)
// and moves them to pixel edges; native corners were never moved (their RGB is their encoding), and a PS2-path
// corner in a batch with native ones is not moved now either. The owner's decision 1 (2026-10-23): native
// draws need not reproduce that snap. A screen pass's batch (nd.pass_active) is the PS2 path's 2D: snapped.
bool nd_batch_3d() {
    return nd.corners_active;
}

// The program p (one of the GS's, its fragment shader) with nd_000.vert, while a native batch is drawn
Program *nd_program(Program *p) {
    if (!nd.active || !p) {
        return p;
    }
    auto it = nd.programs.find(p);
    if (it != nd.programs.end()) {
        return it->second;
    }
    if (!nd.vs) {
        ResourceLayout vert = {};
        vert.input_mask = (1u << 2) | (1u << 4);
        vert.push_constant_size = sizeof(Push);
        vert.sets[1].storage_buffer_mask = 1u;
        vert.sets[1].meta[0].array_size = 1;
        vert.sets[2].storage_buffer_mask = 3u;
        vert.sets[2].meta[0].array_size = 1;
        vert.sets[2].meta[1].array_size = 1;
        nd.vs = gs->device->request_shader(gsn_spv_nd_000_vert, sizeof(gsn_spv_nd_000_vert), &vert);
    }
    Program *np = gs->device->request_program(nd.vs, p->get_shader(ShaderStage::Fragment));
    nd.programs[p] = np;
    return np;
}

// (flush_batch, a native batch's draw: the arena; the draws' records are bound a stretch at a time, a
// kind's own: nd_draw_range)
const Buffer *sa_jitter_frame_picture();
void sa_camera_seen(const RdCamera &cam, int32_t ofx, int32_t ofy);

void nd_bind(Cmd &c) {
    if (nd.batch_arena) {
        c.set_storage_buffer(2, 0, *nd.batch_arena);
    }
    if (!interp.active) {
        // (a picture between's Picture is bound by its kicks' pass, recorded; else nothing moves: zeros, or with an
        // upscaler the frame's own sub-pixel jitter, gsn_standalone.inc's sa_jitter_frame)
        if (const Buffer *jitter = sa_jitter_frame_picture()) {
            c.set_storage_buffer(1, 0, *jitter);
            return;
        }
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

// A buffer of room units of size bytes, mapped for the CPU to write (what a draw reads is never written
// again: a full buffer is replaced, the old one kept by what still names it). named: the pending corners
// name the old one, so the batch is drawn first (split where the PS2 path's is not: nd_buffer_room)
void nd700_records_gone();
void nd_new_buffer(NdBuffer &b, uint32_t room, uint32_t size, bool named) {
    if (named) {
        flush_batch();
    }
    if (&b == &nd.draws) {
        // (what is known of the old buffer's records goes with it: Granite pools its Buffer objects, so a
        // new buffer can come back at a freed one's address, and a flag kept by the address would then
        // name the new buffer's records)
        nd.of5a0.clear();
        nd700_records_gone();
    }
    if (b.buffer && b.map) {
        gs->device->unmap_host_buffer(*b.buffer, MEMORY_ACCESS_WRITE_BIT);
    }
    BufferCreateInfo info = {};
    info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
    info.size = VkDeviceSize(room) * size;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    b.buffer = gs->device->create_buffer(info, nullptr);
    b.map = static_cast<uint8_t *>(gs->device->map_host_buffer(*b.buffer, MEMORY_ACCESS_WRITE_BIT));
    b.used = 0;
    b.room = room;
}

// Whether a buffer is to be replaced before n more units go in: when they do not fit, or, while no pending
// corner names it, when it is past ND_BUFFER_EARLY of its room. A batch can only name one arena and one
// buffer of records of each program, so a buffer replaced while the batch names it splits the batch
// where the PS2 path's goes on (the records filling up in the middle of a map record's or an entity's
// batch: 8 batches more than off on 016 and on 009 by vblank 2,000); replaced early, between batches, it
// seldom is (F-mixed)
const uint32_t ND_BUFFER_EARLY = 4; // (a quarter of the room left)
bool nd_buffer_room(const NdBuffer &b, uint32_t n, bool named) {
    return !b.buffer || b.used + n > b.room || (!named && b.used + n > b.room - b.room / ND_BUFFER_EARLY);
}

// The mesh's first vertex in the arena, put there if it is not yet
uint32_t nd_place(const RdMeshData &m) {
    auto it = nd.placed.find(m.id);
    if (it != nd.placed.end()) {
        return it->second;
    }
    uint32_t n = uint32_t(m.vertices.size() * sizeof(int32_t) / ND_VERTEX_BYTES);
    if (nd_buffer_room(nd.arena, n, nd.pending)) {
        // (full: a new arena, the meshes put in it again as they are drawn)
        uint32_t room = std::max(nd.arena.room ? nd.arena.room * 2 : 1u << 19, n);
        room = std::min(room, ND_ARENA_MAX); // (64 MB at most: a corner names a vertex in 22 bits)
        nd_new_buffer(nd.arena, room, ND_VERTEX_BYTES, nd.pending);
        nd.placed.clear();
        gsn_stats.nd_arenas++;
    }
    uint32_t at = nd.arena.used;
    memcpy(nd.arena.map + size_t(at) * ND_VERTEX_BYTES, m.vertices.data(), size_t(n) * ND_VERTEX_BYTES);
    nd.arena.used += n;
    nd.placed[m.id] = at;
    gsn_stats.nd_mesh_uploads++;
    return at;
}

// VU1's arithmetic (port/include/port/vu_float.h: the interpreter's, bit for bit), on floats
float vu_mul(float a, float b) {
    return vu_float_from_bits(vu_float_mul(vu_float_bits(a), vu_float_bits(b)));
}
float vu_add(float a, float b) {
    return vu_float_from_bits(vu_float_add(vu_float_bits(a), vu_float_bits(b)));
}
float vu_sub(float a, float b) {
    return vu_float_from_bits(vu_float_sub(vu_float_bits(a), vu_float_bits(b)));
}
// M v, as MULAx ACC; MADDAy; MADDAz; MADDw (each product rounded, then the sum): m a qword a column
void vu_transform4(float *out, const float *m, const float *v) {
    for (int r = 0; r < 4; r++) {
        float acc = vu_mul(m[r], v[0]);
        acc = vu_add(acc, vu_mul(m[4 + r], v[1]));
        acc = vu_add(acc, vu_mul(m[8 + r], v[2]));
        out[r] = vu_add(acc, vu_mul(m[12 + r], v[3]));
    }
}

// A vertex's place as VU1 works it out (vu1_hle.c's vertex: its operations, VU1's arithmetic; the same as
// nd_000.vert's): q the mesh's vertex (four qwords), vm the vertex matrix. xyz: FTOI4 of the projected
// place; stq: ST by 1/w; the clip judgement. (vu_float.h's operations: nd_plan_vertex is the same, faster)
void nd_plan_vertex_vu(const int32_t *q, const float *vm, int32_t xyz[3], float stq[3], bool &clip) {
    float p[4] = {float(q[0]), float(q[1]), float(q[2]), float(q[3])};
    float c[4];
    vu_transform4(c, vm, p);
    float qq = vu_float_from_bits(vu_float_div(0x3F800000u, vu_float_bits(c[3])));
    float x = vu_mul(c[0], qq), y = vu_mul(c[1], qq), z = vu_mul(c[2], qq);
    // VU1's clip judgement: past the guard band (2047 about the 2048 middle) in X or Y, or Z below 0
    float rx = vu_sub(x, 2048.0f), ry = vu_sub(y, 2048.0f);
    clip = !(fabsf(rx) <= 2047.0f && fabsf(ry) <= 2047.0f) ||
           (vu_float_bits(z) > 0x80000000u && (vu_float_bits(z) & 0x7F800000u) != 0);
    xyz[0] = vu_float_ftoi(vu_float_bits(x), 4);
    xyz[1] = vu_float_ftoi(vu_float_bits(y), 4);
    xyz[2] = vu_float_ftoi(vu_float_bits(z), 4);
    stq[0] = vu_mul(float(q[8]) / 4096.0f, qq);
    stq[1] = vu_mul(float(q[9]) / 4096.0f, qq);
    stq[2] = vu_mul(float(q[10]) / 4096.0f, qq);
}

// The plan's cost (P1-6: 2.7-3.5 s of the GS thread per 2,000 vblanks, about 130 ns a vertex, most of it
// vu_float.h's operations one at a time): the same arithmetic, bit for bit, in doubles. A VU float is
// kept as the double of its value. A product of two 24-bit significands is exact in a double; a sum of
// two is exact when their exponents are at most 28 apart, and further apart the double's rounding stays
// below the 24 bits kept, unless it takes the smaller term away whole (VU1 then takes a unit off the
// larger when their signs differ). Each result is truncated to 24 bits: VU1's rounding toward zero.
// What a double does otherwise is marked bad, and the vertex is worked out again by vu_float.h: a sum
// that lost a term, a result VU1 makes +-0 or its largest (below 2^-126, from 2^128), FTOI's limit, and
// matrix entries VU1 reads otherwise than the PC (denormals, exponent 255: nd_fast_matrix).
struct NdFast {
    bool bad = false;
    static double trunc24(double v) {
        uint64_t d;
        memcpy(&d, &v, 8);
        d &= ~((uint64_t(1) << 29) - 1);
        memcpy(&v, &d, 8);
        return v;
    }
    double range(double v) {
        double a = fabs(v);
        bad |= (a < 0x1p-126 && a != 0.0) || !(a < 0x1p128);
        return v;
    }
    double mul(double a, double b) {
        return range(trunc24(a * b));
    }
    double add(double a, double b) {
        double s = a + b;
        bad |= (s == a && b != 0.0) || (s == b && a != 0.0);
        return range(trunc24(s));
    }
    int32_t ftoi4(double v) {
        bad |= !(fabs(v) < 0x1p27);
        return bad ? 0 : int32_t(v * 16.0);
    }
};

// whether a vertex matrix's entries are read by the PC as by VU1 (no denormals, no exponent 255)
bool nd_fast_matrix(const float *vm) {
    bool ok = true;
    for (int i = 0; i < 16; i++) {
        uint32_t e = vu_float_bits(vm[i]) & 0x7F800000u;
        ok &= e != 0x7F800000u && (e != 0 || (vu_float_bits(vm[i]) & 0x007FFFFFu) == 0);
    }
    return ok;
}

// nd_plan_vertex_vu, in doubles where they give the same (vm_ok: nd_fast_matrix(vm))
void nd_plan_vertex(const int32_t *q, const float *vm, int32_t xyz[3], float stq[3], bool &clip, bool vm_ok) {
    if (vm_ok) {
        NdFast f;
        double p[4] = {double(float(q[0])), double(float(q[1])), double(float(q[2])), double(float(q[3]))};
        // (MULAx ACC, then MADDAy, MADDAz, MADDw: a column at a time, the four rows together)
        double c[4];
        for (int r = 0; r < 4; r++) {
            c[r] = f.mul(vm[r], p[0]);
        }
        for (int col = 1; col < 4; col++) {
            double t[4];
            for (int r = 0; r < 4; r++) {
                t[r] = f.mul(vm[col * 4 + r], p[col]);
            }
            for (int r = 0; r < 4; r++) {
                c[r] = f.add(c[r], t[r]);
            }
        }
        // (DIV: by 0 VU1's largest; else as vu_float_div, a double's quotient truncated)
        f.bad |= c[3] == 0.0;
        double qq = f.range(NdFast::trunc24(1.0 / c[3]));
        double x = f.mul(c[0], qq), y = f.mul(c[1], qq), z = f.mul(c[2], qq);
        double rx = f.add(x, -2048.0), ry = f.add(y, -2048.0);
        xyz[0] = f.ftoi4(x);
        xyz[1] = f.ftoi4(y);
        xyz[2] = f.ftoi4(z);
        double s = f.mul(double(float(q[8]) / 4096.0f), qq), t = f.mul(double(float(q[9]) / 4096.0f), qq);
        double w = f.mul(double(float(q[10]) / 4096.0f), qq);
        if (!f.bad) {
            // (z below 0: VU1's sign set and exponent not 0; a result VU1 would make +-0 is bad above)
            clip = !(fabs(rx) <= 2047.0 && fabs(ry) <= 2047.0) || z < 0.0;
            stq[0] = float(s);
            stq[1] = float(t);
            stq[2] = float(w);
            return;
        }
    }
    nd_plan_vertex_vu(q, vm, xyz, stq, clip);
}

void nd_plan_vertex(const int32_t *q, const float *vm, int32_t xyz[3], float stq[3], bool &clip) {
    nd_plan_vertex(q, vm, xyz, stq, clip, nd_fast_matrix(vm));
}

// A vertex's S and T moved by its wave (the wave UVs on the GPU, P6-2): its ST's w the wave, its S and T the
// vertex's own, each moved by the wave's 16-bit offset in 16 bits as Wobble_ApplyWaveUvs writes them, then
// read back as VIF1's signed UNPACK does
void nd_wave_st(int32_t *q, const uint32_t *wave) {
    uint32_t w = wave[uint32_t(q[11]) & 15];
    q[8] = int32_t(int16_t(uint16_t(uint32_t(q[8]) + (w & 0xFFFF))));
    q[9] = int32_t(int16_t(uint16_t(uint32_t(q[9]) + (w >> 16))));
}

// nd_plan's work: the batch's corners (fog: the vertex's number in the batch; RGB without the
// record); wave: the waves' offsets, or null
void nd_plan_batch(const RdMeshData &m, const RdBatch &b, const float *vm, uint64_t offset, uint32_t gouraud,
                   std::vector<Vtx> &corners, const uint32_t *wave = nullptr) {
    int32_t ox = int32_t(bits(offset, 0, 16)), oy = int32_t(bits(offset, 32, 16));
    Vtx v[N_ND_MAX];
    bool clip[N_ND_MAX];
    gsn_stats.nd_plan_vertices += b.count;
    bool vm_ok = nd_fast_matrix(vm);
    for (uint32_t k = 0; k < b.count; k++) {
        const int32_t *q = &m.vertices[(b.first + k) * 16];
        int32_t waved[16];
        if (wave) {
            memcpy(waved, q, sizeof(waved));
            nd_wave_st(waved, wave);
            q = waved;
        }
        int32_t xyz[3];
        float stq[3];
        nd_plan_vertex(q, vm, xyz, stq, clip[k], vm_ok);
        v[k].x = float(int32_t(uint32_t(xyz[0]) & 0xFFFF) - ox) / 16.0f;
        v[k].y = float(int32_t(uint32_t(xyz[1]) & 0xFFFF) - oy) / 16.0f;
        v[k].z = uint32_t(xyz[2] >> 4) & 0xFFFFFF;
        // the alpha as VU1 leaves it (1 * the vertex's, at most 255, FTOI0) and the GS takes it: its low 8
        // bits, so a negative alpha wraps (map 000's floor layers have vertices at -1400: 0x88). Clamped
        // at 0, a layer's batch had an As range of 0-128 where the PS2 path's had 0-218, and flush_batch
        // blended it with Vulkan's blending where the PS2 path's took the shader's: a band of the floor a
        // level or two apart.
        int32_t a = std::min(q[15], 255);
        v[k].rgba = (uint32_t(a) & 0xFF) << 24;
        v[k].s = stq[0];
        v[k].t = stq[1];
        v[k].q = stq[2];
    }
    corners.clear();
    for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
        uint32_t t[3] = {b.triangles[i], b.triangles[i + 1], b.triangles[i + 2]};
        uint32_t kick = t[2];
        // (VU1's ADC: the kick's clip and the two kicks' before it, whichever vertices the triangle has)
        if (clip[kick] || (kick >= 1 && clip[kick - 1]) || (kick >= 2 && clip[kick - 2])) {
            continue;
        }
        for (int j = 0; j < 3; j++) {
            Vtx w = v[t[j]];
            if (!gouraud) {
                w.rgba = v[kick].rgba; // (flat: the kick's colour, as add_triangle has it)
            }
            w.fog = float(t[j]);
            w.rgba |= (kick - t[j]) | gouraud;
            corners.push_back(w);
        }
    }
}

// The batch's triangles planned on the CPU into gs->verts (see the top): those VU1 would kick. base: the
// batch's first vertex in the arena; record: its draw's. A batch planned already by the same vertex
// matrix (the camera standing still; the frame drawn again for a picture between) is taken as it was:
// nd.planned keeps each batch's last plan, its corners' fog the vertex's number in the batch and their
// RGB without the record.
void nd_plan(const RdMeshData &m, const RdBatch &b, const float *vm, uint32_t prim, uint32_t base, uint32_t record,
             const uint32_t *wave = nullptr) {
    uint64_t offset = gs->reg[R_XYOFFSET_1 + ((prim >> 9) & 1)];
    uint32_t gouraud = ((prim >> 3) & 1) ? ND_CORNER_GOURAUD : 0;
    if (nd.planned.size() >= ND_PLANNED_MAX) {
        nd.planned.clear();
    }
    NdPlannedPair &pair = nd.planned[uint64_t(m.id) << 20 | uint64_t(&b - m.batches.data())];
    auto same = [&](const NdPlanned &e) {
        return e.data == m.vertices.data() && e.offset == offset && e.gouraud == gouraud && memcmp(e.vm, vm, sizeof(e.vm)) == 0 &&
               e.waved == (wave != nullptr) && (!wave || memcmp(e.wave, wave, sizeof(e.wave)) == 0);
    };
    uint32_t which = same(pair.plan[0]) ? 0 : same(pair.plan[1]) ? 1 : 2;
    if (which == 2) {
        which = pair.older;
    }
    pair.older = which ^ 1;
    NdPlanned &e = pair.plan[which];
    if (!same(e)) {
        e.data = m.vertices.data();
        e.offset = offset;
        e.gouraud = gouraud;
        memcpy(e.vm, vm, sizeof(e.vm));
        e.waved = wave != nullptr;
        if (wave) {
            memcpy(e.wave, wave, sizeof(e.wave));
        }
        nd_plan_batch(m, b, vm, offset, gouraud, e.corners, wave);
    } else {
        gsn_stats.nd_plan_reused += b.count;
    }
    size_t at = gs->verts.size(), n = e.corners.size();
    gs->verts.resize(at + n);
    Vtx *out = gs->verts.data() + at;
    for (size_t i = 0; i < n; i++) {
        Vtx w = e.corners[i];
        w.fog = nd_corner_vertex(base + b.first + uint32_t(w.fog));
        w.rgba |= record << ND_CORNER_DRAW_SHIFT;
        out[i] = w;
    }
    if (interp.active) {
        for (size_t i = 0; i < n; i++) {
            interp.vsrc.push_back({GSN_IM_NONE, GSN_IM_NONE, GSN_IM_NONE, uint32_t(i % 3)});
        }
    }
}

// A draw's record: VU1's set-up (vu1_hle.c, setup), in its arithmetic: the vertex matrix (VU1 0x00 by
// 0x04), the light directions by the part's rotation (0x0C-0x0F by 0x04-0x06's columns), the point lights
// into the part's space: less its translation, then by its rotation with the off-diagonal signs turned
// (the set-up's own maths: the transpose only for a turn about one axis, as the game's models are). Of
// info only the point lights' count: the rest is the GS's
// (nd_const's VU arithmetic, kept by what it was worked out from, compared bit for bit: the picture drawn from
// the frame's packets again, and the parts standing still, take it as it was)
struct NdConstMemo {
    float in[64]; // the model, the camera's world_screen, the light directions, the point lights
    float vertex[16], light_dir[16], point_pos[16];
    bool valid = false;
};
NdConstMemo nd_const_memo[2048];

void nd_const(NdConst &k, const RdDraw &d, const RdCamera &cam) {
    const float *m = &d.model[0][0];
    float in[64];
    memcpy(in, d.model, 64);
    memcpy(in + 16, cam.world_screen, 64);
    memcpy(in + 32, d.light_dir, 64);
    memcpy(in + 48, d.point_pos, 64);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 64; i += 2) {
        uint64_t v;
        memcpy(&v, in + i, 8);
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    NdConstMemo &memo = nd_const_memo[h & 2047];
    if (memo.valid && memcmp(memo.in, in, sizeof(in)) == 0) {
        memcpy(k.vertex, memo.vertex, 64);
        memcpy(k.light_dir, memo.light_dir, 64);
        memcpy(k.point_pos, memo.point_pos, 64);
        memcpy(k.model, d.model, 64);
        memcpy(k.view, cam.view, 64);
        memcpy(k.screen, cam.screen, 64);
        memcpy(k.light_col, d.light_col, 64);
        memcpy(k.fog, d.fog, 16);
        for (int l = 0; l < 4; l++) {
            memcpy(&k.point_col[l * 4], d.point_col[l], 16);
        }
        k.info[0] = std::max(0, std::min(4, int32_t(int16_t(d.point_count))));
        return;
    }
    for (int col = 0; col < 4; col++) {
        vu_transform4(&k.vertex[col * 4], &cam.world_screen[0][0], &m[col * 4]);
        vu_transform4(&k.light_dir[col * 4], &d.light_dir[0][0], &m[col * 4]);
    }
    memcpy(k.model, d.model, 64);
    memcpy(k.view, cam.view, 64);
    memcpy(k.screen, cam.screen, 64);
    memcpy(k.light_col, d.light_col, 64);
    memcpy(k.fog, d.fog, 16);
    int32_t lights = std::max(0, std::min(4, int32_t(int16_t(d.point_count))));
    for (int l = 0; l < 4; l++) {
        // (as the set-up does it, to the sign of a 0: the translation as 0 + it, the turned signs as 0 - them)
        float p[3] = {vu_sub(d.point_pos[l][0], vu_add(0.0f, m[12])), vu_sub(d.point_pos[l][1], vu_add(0.0f, m[13])),
                      vu_sub(d.point_pos[l][2], vu_add(0.0f, m[14]))};
        float r0[3] = {vu_add(0.0f, m[0]), vu_sub(0.0f, m[1]), vu_sub(0.0f, m[2])};
        float r1[3] = {vu_sub(0.0f, m[4]), vu_add(0.0f, m[5]), vu_sub(0.0f, m[6])};
        float r2[3] = {vu_sub(0.0f, m[8]), vu_sub(0.0f, m[9]), vu_add(0.0f, m[10])};
        for (int i = 0; i < 3; i++) {
            float acc = vu_mul(r0[i], p[0]);
            acc = vu_add(acc, vu_mul(r1[i], p[1]));
            k.point_pos[l * 4 + i] = vu_add(acc, vu_mul(r2[i], p[2]));
        }
        k.point_pos[l * 4 + 3] = d.point_pos[l][3];
        memcpy(&k.point_col[l * 4], d.point_col[l], 16);
    }
    k.info[0] = lights;
    memcpy(memo.in, in, sizeof(in));
    memcpy(memo.vertex, k.vertex, 64);
    memcpy(memo.light_dir, k.light_dir, 64);
    memcpy(memo.point_pos, k.point_pos, 64);
    memo.valid = true;
}

// ---- Pictures between: the models' own moves (P2-5) ----
// (nd_key) a draw's identity, the section it is in and which of the draws of both in the frame
NdKey nd_key(const RdDraw &d) {
    NdKey key = {{d.kind, d.id[0], d.id[1], d.id[2], uint32_t(nd.section), 0}};
    key.k[5] = nd.seen[key]++;
    return key;
}

// (the frame's own draw) where the draw's part is, for the frame after's pictures between
void nd_remember(const NdKey &key, const RdDraw &d, const RdCamera &cam) {
    NdPlace &p = nd.now[key];
    memcpy(p.model, d.model, 64);
    memcpy(p.parent, d.parent, 64);
    memcpy(p.view, cam.view, 64);
}

// The kinds that are moved by their own matrices (nd_model_flags); the map records and the items lying
// about keep P1-3's move (the sky by its translation)
bool nd_model_kind(uint32_t kind) {
    return kind == RD_KIND_MAP_OBJECT || kind == RD_KIND_PICKED || kind == RD_KIND_ENTITY || kind == RD_KIND_ARM ||
           kind == RD_KIND_GOLD || kind == RD_KIND_SHADOW;
}

// a b, matrices of VU layout (a qword a column), in doubles
void nd_mul(double *out, const float *a, const float *b) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            double sum = 0.0;
            for (int i = 0; i < 4; i++) {
                sum += double(a[i * 4 + row]) * double(b[col * 4 + i]);
            }
            out[col * 4 + row] = sum;
        }
    }
}

// A matrix's linear part split into a rotation r and the rest s (r s; s is r's transpose times it),
// r[row][col]: its first column's direction, the second's square to it, the third their cross (so a mirror
// is s's)
bool nd_split(const double *m, double r[3][3], double s[3][3]) {
    double c[3][3]; // (columns)
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            c[j][i] = m[j * 4 + i];
        }
    }
    double n0 = sqrt(c[0][0] * c[0][0] + c[0][1] * c[0][1] + c[0][2] * c[0][2]);
    if (!(n0 > 1e-12) || !std::isfinite(n0)) {
        return false;
    }
    double r0[3] = {c[0][0] / n0, c[0][1] / n0, c[0][2] / n0};
    double d = r0[0] * c[1][0] + r0[1] * c[1][1] + r0[2] * c[1][2];
    double u[3] = {c[1][0] - d * r0[0], c[1][1] - d * r0[1], c[1][2] - d * r0[2]};
    double n1 = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(n1 > 1e-12 * n0) || !std::isfinite(n1)) {
        return false;
    }
    double r1[3] = {u[0] / n1, u[1] / n1, u[2] / n1};
    double r2[3] = {r0[1] * r1[2] - r0[2] * r1[1], r0[2] * r1[0] - r0[0] * r1[2], r0[0] * r1[1] - r0[1] * r1[0]};
    const double *rc[3] = {r0, r1, r2};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i][j] = rc[j][i];
            s[i][j] = rc[i][0] * c[j][0] + rc[i][1] * c[j][1] + rc[i][2] * c[j][2];
            if (!std::isfinite(s[i][j])) {
                return false;
            }
        }
    }
    return true;
}

// b: the move of a part from prev (the frame before) to cur (this frame), matrices of VU layout; with views,
// of the part in the eye's space (view_cur cur, view_prev prev: the arm). False when it is not a move
// between: a jump of ND_MAX_MOVE or more, a turn of nearly half way round, or a matrix with no rotation in it
bool nd_between(NdBetween &b, const float *cur, const float *prev, const float *view_cur, const float *view_prev) {
    double c[16], p[16];
    if (view_cur) {
        nd_mul(c, view_cur, cur);
        nd_mul(p, view_prev, prev);
    } else {
        for (int i = 0; i < 16; i++) {
            c[i] = cur[i];
            p[i] = prev[i];
        }
    }
    double dt[3] = {p[12] - c[12], p[13] - c[13], p[14] - c[14]};
    double d2 = dt[0] * dt[0] + dt[1] * dt[1] + dt[2] * dt[2];
    if (!(d2 < double(ND_MAX_MOVE) * ND_MAX_MOVE)) {
        return false;
    }
    double rc[3][3], sc[3][3], rp[3][3], sp[3][3];
    if (!nd_split(c, rc, sc) || !nd_split(p, rp, sp)) {
        return false;
    }
    // the turn q from rc to rp in rc's frame (rp = rc q): rc's transpose times rp, as a quaternion, the
    // shorter way round
    double q[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            q[i][j] = rc[0][i] * rp[0][j] + rc[1][i] * rp[1][j] + rc[2][i] * rp[2][j];
        }
    }
    double w, x, y, z, tr = q[0][0] + q[1][1] + q[2][2];
    if (tr > 0.0) {
        double t = sqrt(tr + 1.0) * 2.0;
        w = t / 4.0, x = (q[2][1] - q[1][2]) / t, y = (q[0][2] - q[2][0]) / t, z = (q[1][0] - q[0][1]) / t;
    } else if (q[0][0] > q[1][1] && q[0][0] > q[2][2]) {
        double t = sqrt(1.0 + q[0][0] - q[1][1] - q[2][2]) * 2.0;
        w = (q[2][1] - q[1][2]) / t, x = t / 4.0, y = (q[0][1] + q[1][0]) / t, z = (q[0][2] + q[2][0]) / t;
    } else if (q[1][1] > q[2][2]) {
        double t = sqrt(1.0 + q[1][1] - q[0][0] - q[2][2]) * 2.0;
        w = (q[0][2] - q[2][0]) / t, x = (q[0][1] + q[1][0]) / t, y = t / 4.0, z = (q[1][2] + q[2][1]) / t;
    } else {
        double t = sqrt(1.0 + q[2][2] - q[0][0] - q[1][1]) * 2.0;
        w = (q[1][0] - q[0][1]) / t, x = (q[0][2] + q[2][0]) / t, y = (q[1][2] + q[2][1]) / t, z = t / 4.0;
    }
    if (w < 0.0) {
        w = -w, x = -x, y = -y, z = -z;
    }
    double vn = sqrt(x * x + y * y + z * z);
    double angle = 2.0 * atan2(vn, w);
    if (!std::isfinite(angle) || angle > 3.0) {
        return false;
    }
    memset(&b, 0, sizeof(b));
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) {
            b.rot[col * 4 + row] = float(rc[row][col]);
            b.s[col * 4 + row] = float(sc[row][col]);
            b.ds[col * 4 + row] = float(sp[row][col] - sc[row][col]);
        }
        b.dt[col] = float(dt[col]);
    }
    if (vn > 1e-12) {
        b.axis[0] = float(x / vn);
        b.axis[1] = float(y / vn);
        b.axis[2] = float(z / vn);
        b.axis[3] = float(angle);
    } else {
        b.axis[0] = 1.0f;
    }
    return true;
}

// In a picture between, how a draw of the models' kinds moves (ND_C_*): in the world with the camera and,
// paired with its draw in the frame before, between its two places (a: its matrix's move; b: its parent's,
// program 0x280's set B); in the reflection with the camera only (as the PS2 path's); the arm and the held
// item between their two places in the eye's space, the camera's move left out (NATIVE_DRAW.md 3.5: "the
// arm is interpolated in eye space"); elsewhere, or the arm not paired, as the frame has it
int32_t nd_model_flags(const NdKey &key, const RdDraw &d, const RdCamera &cam, NdBetween *a, NdBetween *b) {
    bool eye = nd.section == ND_SECTION_ARM && d.kind == RD_KIND_ARM;
    if (!eye && nd.section != ND_SECTION_WORLD && nd.section != ND_SECTION_REFLECT) {
        return 0;
    }
    int32_t flags = eye ? 0 : ND_C_MOVING;
    auto it = nd.before.find(key);
    if (nd.section == ND_SECTION_REFLECT || it == nd.before.end()) {
        return flags;
    }
    const NdPlace &was = it->second;
    if (memcmp(was.model, d.model, 64) == 0 && (!b || memcmp(was.parent, d.parent, 64) == 0) &&
        (!eye || memcmp(was.view, cam.view, 64) == 0)) {
        return flags; // (it did not move)
    }
    const float *vc = eye ? &cam.view[0][0] : nullptr, *vp = eye ? was.view : nullptr;
    // (set B, the parent's: a part with none sends a slot of no matrix, never read: no move then)
    if (b && !nd_between(*b, &d.parent[0][0], was.parent, vc, vp)) {
        memset(b, 0, sizeof(*b));
    }
    if (!nd_between(*a, &d.model[0][0], was.model, vc, vp)) {
        return flags;
    }
    gsn_stats.nd_moved_models++;
    return ND_C_MOVING | ND_C_MODEL | (eye ? ND_C_EYE : 0);
}

void nd_draw(uint64_t value) {
    RdDraw d;
    RdCamera cam;
    if (!rd_fetch(value, d, cam, nd.fetched)) {
        gsn_stats.nd_lost++;
        return;
    }
    if (d.program == 0x280) {
        nd280_draw(d, cam); // (gsn_nd_280.inc)
        nd.fetched.clear();
        return;
    }
    if (d.program == RD_PROGRAM_GS_STRIP) {
        nd_strip_draw(d); // (gsn_nd_strip.inc)
        nd.fetched.clear();
        return;
    }
    if (d.program == 0x700) {
        nd700_draw(d, cam); // (gsn_nd_700.inc)
        nd.fetched.clear();
        return;
    }
    if (d.program == RD_PROGRAM_RIPPLE) {
        ndw_draw(d, cam); // (gsn_nd_water.inc)
        nd.fetched.clear();
        return;
    }
    if (d.program == RD_PROGRAM_PASS) {
        nd_pass_draw(d); // (gsn_nd_pass.inc)
        nd.fetched.clear();
        return;
    }
    if (d.program != 0 && d.program != 0x5A0) {
        gsn_stats.nd_unsupported++;
        return;
    }
    gsn_stats.nd_draws++;
    // the draw's record (nd_const: VU1's set-up)
    if (nd_buffer_room(nd.draws, 1, nd.pending_000)) {
        nd_new_buffer(nd.draws, 4096, sizeof(NdConst), nd.pending_000);
        nd.boxes.clear();
    }
    uint32_t record = nd.draws.used++;
    NdConst k;
    if (!nd_prep_take_const(d.program, &k, sizeof(k)) || nd_prep.check) { // (rc/own: the frame's, in its first picture)
        NdConst again;
        memset(&again, 0, sizeof(again)); // (what nd_const leaves is set below: zero, so that the frame's and the picture's compare)
        nd_const(again, d, cam);
        if (d.program == 0x5A0) {
            nd5a0_const(again);
        }
        if (nd_prep.draw >= 0 && interp.active && memcmp(&again, &k, sizeof(k)) != 0) {
            nd_prep.check_differ[0]++;
        }
        k = again;
        nd_prep_keep_const(d.program, &k, sizeof(k));
    }
    if (d.program == 0x5A0) {
        // (the shadows: 0x000's maths without the lights and the fog, gsn_nd_5a0.inc: nd5a0_const above)
        if (nd.of5a0.size() <= record) {
            nd.of5a0.resize(record + 1, 0);
        }
        nd.of5a0[record] = 1;
    }
    const float *m = &d.model[0][0];
    // (the XYOFFSET of the context the draw's PRIM picks: the game's draws keep both contexts' the same)
    uint64_t offset = gs->reg[R_XYOFFSET_1 + ((effective_prim() >> 9) & 1)];
    k.info[1] = int32_t(bits(offset, 0, 16));
    k.info[2] = int32_t(bits(offset, 32, 16));
    if (!interp.active) {
        sa_camera_seen(cam, k.info[1], k.info[2]); // (upscaling: the frame's camera, for the composite's motion)
    }
    // pictures between: the world and the reflection move with the picture's camera; a part of the world
    // that moved since the frame before (the sky) also with its model, as frame_interp.c's object_between
    NdKey key = nd_key(d);
    memset(k.obj, 0, sizeof(k.obj));
    memset(&k.bt, 0, sizeof(k.bt));
    k.info[3] = 0;
    if (!interp.active) {
        nd_remember(key, d, cam);
    } else if (nd_model_kind(d.kind)) {
        k.info[3] = nd_model_flags(key, d, cam, &k.bt, nullptr); // (the models': P2-5)
    } else if (nd.section == ND_SECTION_WORLD || nd.section == ND_SECTION_REFLECT) {
        k.info[3] = ND_C_MOVING;
        auto it = nd.before.find(key);
        if (nd.section == ND_SECTION_WORLD && it != nd.before.end()) {
            const float *was = &it->second.model[12];
            float move[3] = {m[12] - was[0], m[13] - was[1], m[14] - was[2]};
            float d2 = move[0] * move[0] + move[1] * move[1] + move[2] * move[2];
            if (d2 > 0.0f && d2 < ND_MAX_MOVE * ND_MAX_MOVE) {
                memcpy(k.obj, move, sizeof(move));
                k.info[3] |= ND_C_OBJ;
            }
        }
    }
    // the wave UVs on the GPU: the waves' offsets (P6-2)
    const uint32_t *wave = nullptr;
    memset(k.wave, 0, sizeof(k.wave));
    if (d.flags & RD_DRAW_WAVE_UVS) {
        memcpy(k.wave, d.parent, sizeof(k.wave));
        k.info[3] |= ND_C_WAVE;
        wave = k.wave;
    }
    // at the far plane (the modern sky, P3-2): Z 0, the plan's corners as the shader's (below)
    if (d.flags & RD_DRAW_FAR_Z) {
        k.info[3] |= ND_C_FAR;
    }
    memcpy(nd.draws.map + size_t(record) * sizeof(NdConst), &k, sizeof(k));
    if (interp.active) {
        nd.boxes.resize(record + 1);
        nd.boxes[record] = {k.info[3], k.screen[0], k.screen[5], k.screen[8], k.screen[9], float(k.info[1]) / 16.0f,
                            float(k.info[2]) / 16.0f, {k.obj[0], k.obj[1], k.obj[2]}, k.screen[10]};
    }
    for (const auto &mesh : nd.fetched) {
        uint32_t base = nd_place(*mesh);
        for (const RdBatch &b : mesh->batches) {
            // what the VU1 run kicks before its strip (each write flushes the batch as on the PS2 path, when
            // it changes something)
            if (b.start != RD_PROGRAM_CONTINUE) {
                gs_write(R_FOGCOL, d.fogcol); // (an MSCAL: the set-up kicks FOGCOL, VU1 0x3D)
            }
            for (size_t i = 0; i + 1 < b.ad.size(); i += 2) {
                gs_write(uint32_t(b.ad[i]), b.ad[i + 1]);
            }
            gs_write(R_PRIM, uint32_t((b.tag[0] >> 47) & 0x7FF));
            // the strip's triangles join the batch, the PS2 path's corners in it too (a batch of other
            // buffers is drawn first; and in a picture between, a batch of the PS2 path's: nd_kick)
            // (0x280's corners pending join too: nd_draw_range draws each kind with its program and records)
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
            {
                Timer plan(gsn_stats.ns_nd_plan);
                // (rc/own: in the frame's first picture between, the frame's corners)
                const NdPrepBatch *taken = nd_prep_take_batch(mesh->id, uint32_t(&b - mesh->batches.data()), 0, eprim, base);
                if (taken) {
                    nd_prep_corners(*taken, record);
                }
                if (!taken || nd_prep.check) {
                    size_t again = gs->verts.size();
                    nd_plan(*mesh, b, k.vertex, eprim, base, record, wave);
                    if (taken) {
                        nd_prep_check(before, again);
                    }
                    nd_prep_keep_batch(mesh->id, uint32_t(&b - mesh->batches.data()), 0, eprim, base, record, before);
                }
            }
            if (k.info[3] & ND_C_FAR) {
                for (size_t i = before; i < gs->verts.size(); i++) {
                    gs->verts[i].z = 0;
                }
            }
            if (gs->verts.size() > before) {
                nd.pending = nd.pending_000 = true;
            }
            nd.batch_arena = nd.arena.buffer.get();
            nd.batch_draws = nd.draws.buffer.get();
            gsn_stats.nd_batches++;
        }
    }
    // (the GS's vertex queue as the strip left it does not matter: the next draw writes PRIM)
    gs->queue_count = 0;
    nd.fetched.clear();
}

// PORT_GS_REG_NATIVE_DRAW
void nd_register(uint64_t value) {
    switch (RD_ND_COMMAND(value)) {
    case RD_ND_DRAW:
        if (nd_mode == GSN_NATIVE_DRAW_NATIVE) {
            nd_prep_draw_begin(value); // (rc/own: the frame's draw kept for its pictures between, or a picture's taken)
            sa_draw_begin(value); // (standalone: a batch holds the scene's corners or none)
            nd_draw(value);
            sa_draw_end();
            nd_prep_draw_end();
        }
        break;
    case RD_ND_ALT_BEGIN:
        flush_batch();
        nd.skipping = nd_mode == GSN_NATIVE_DRAW_NATIVE;
        break;
    case RD_ND_ALT_END:
        nd.skipping = false;
        break;
    case RD_ND_FRAME:
        rd_consumer_frame(value);
        nd_prep_frame(uint32_t(value) & 0xFFFFFF); // (rc/own)
        nd.section = ND_SECTION_NONE;
        nd.seen.clear();
        // (a frame's packets drawn again, for a picture between or exact, are the same frame)
        if (!interp.active && (uint32_t(value) & 0xFFFFFF) != nd.serial) {
            nd.serial = uint32_t(value) & 0xFFFFFF;
            nd.before.swap(nd.now);
            nd.now.clear();
            nd.strip_before.swap(nd.strip_now);
            nd.strip_now.clear();
            nd.pass_before.swap(nd.pass_now);
            nd.pass_now.clear();
        }
        break;
    }
}

// PORT_GS_REG_FRAME_INTERP (game/frame_interp.c's marks, as frame_interp.c's mark() reads them): where
// the draws after it are. The world (projection 0, mode 0) and the reflection (mode 1) move with the
// camera in a picture between; the arm moves only as it moves (nd_model_flags); models before the eye and
// what follows the world stay.
void sa_group_draw();
void nd_mark(uint64_t value) {
    sa_group_draw(); // (standalone, pf/skin: a skinned model's parts kept are drawn before a picture between begins or ends)
    uint32_t index = uint32_t(value >> 32) & 0xFF, v = uint32_t(value);
    if (index != PORT_FRAME_INTERP_HEADER) {
        return;
    }
    if (v == PORT_FRAME_INTERP_ARM) {
        nd.section = ND_SECTION_ARM;
    } else if (v == PORT_FRAME_INTERP_END) {
        nd.section = ND_SECTION_OTHER;
    } else if ((v & 0xFF) != 0) {
        nd.section = (v >> 8) == 1 ? ND_SECTION_REFLECT : ND_SECTION_OTHER;
    } else {
        nd.section = (v >> 8) == 2 ? ND_SECTION_OTHER : ND_SECTION_WORLD;
    }
}

// (run_box, in a picture between: a batch of native corners) The pixels a run of native corners can cover
// at any time between the frame shown (t 0) and this one (t 1): what flush_batch copies for its shader
// blending, and the render pass's part of the target (COPY_BY_PASS). The corners are where the frame has
// them (t 1, box); nd_000.vert moves them, so add where they are at t 0, as interp_box_at_0 does for the
// PS2 path's kicks: nd_000.vert's moved at the frame's t 0 camera (frame->move0, mid0) with the part's obj
// back at the frame before's, and the same margin for the turn on the way. Left out, a run that moved out
// of the frame's box blended with the copy where nothing had been copied (pitching on the sky maps: the
// dome's lowest rows black and noisy, P3-1's rim band). The models' own moves (ND_C_MODEL), program
// 0x280's corners and the strip's are not worked out here: such a run may cover the whole target. False
// when the batch is not native corners (the PS2 path's: interp_box_at_0).
// (nd_box_at_0, nd_z_at_0) Where a moving native corner of program 0x000 (record b) is at the frame's t 0:
// nd_000.vert's moved at the frame's t 0 camera (frame->move0, mid0) with the part's obj back at the frame
// before's. Its place in GS pixels less XYOFFSET (px, py) and its Z (pz: the corner's less (Z - cz) dm.z /
// m.z); false when it is dropped there, or drawn where it is.
bool nd_corner_at_0(const Vtx &w, const NdBoxInfo &b, float *px, float *py, double *pz) {
    const float *r = interp.frame->move0, *mid = interp.frame->mid0;
    // (the place in the frame's view from X, Y and 1 / q, then v + dm)
    double depth = 1.0 / double(w.q);
    if (!(w.q > 0.0f) || !std::isfinite(depth) || b.sx == 0.0f || b.sy == 0.0f) {
        return false; // (as it is)
    }
    double v[3] = {(double(w.x) + b.ox - b.cx) * depth / b.sx, (double(w.y) + b.oy - b.cy) * depth / b.sy, depth};
    double m[3];
    for (int j = 0; j < 3; j++) {
        m[j] = v[j] + r[j * 4] * v[0] + r[j * 4 + 1] * v[1] + r[j * 4 + 2] * v[2] + r[j * 4 + 3];
        if (b.flags & ND_C_OBJ) {
            m[j] -= mid[j * 4] * b.obj[0] + mid[j * 4 + 1] * b.obj[1] + mid[j * 4 + 2] * b.obj[2];
        }
    }
    if (!(m[2] > 0.0)) {
        return false; // (dropped)
    }
    double nx = b.sx * m[0] / m[2] + b.cx, ny = b.sy * m[1] / m[2] + b.cy;
    if (!(fabs(nx - 2048.0) <= 2047.0 && fabs(ny - 2048.0) <= 2047.0)) {
        return false; // (dropped)
    }
    *px = float(nx - b.ox);
    *py = float(ny - b.oy);
    *pz = double(w.z) - (double(w.z) - double(b.cz)) * (m[2] - v[2]) / m[2];
    return true;
}

bool nd_box_at_0(uint32_t first, uint32_t count, int32_t *box) {
    if (!nd.active || nd.pass_active) { // (a screen pass's corners are the CPU's: interp_box_at_0)
        return false;
    }
    auto whole = [&]() {
        box[0] = box[1] = -1;
        box[2] = box[3] = 2048 + 2;
    };
    if (nd.strip_active || ndw_batch()) {
        whole(); // (the strip's, and the water's: their records are not nd.boxes')
        return true;
    }
    float most = 0.0f;
    bool moved = false;
    for (uint32_t i = first; i < first + count && i < gs->verts.size(); i++) {
        const Vtx &w = gs->verts[i];
        int kind = nd_corner_kind(w);
        if (kind == ND_KIND_PS2) {
            continue;
        }
        uint32_t record = (w.rgba & 0xFFFFFFu) >> ND_CORNER_DRAW_SHIFT;
        if (kind != ND_KIND_000 || record >= nd.boxes.size()) {
            whole();
            return true;
        }
        const NdBoxInfo &b = nd.boxes[record];
        if (!(b.flags & ND_C_MOVING)) {
            continue;
        }
        if (b.flags & ND_C_MODEL) {
            whole();
            return true;
        }
        moved = true;
        float px, py;
        double pz;
        if (!nd_corner_at_0(w, b, &px, &py, &pz)) {
            continue;
        }
        box[0] = std::min(box[0], int32_t(floorf(px)) - 1);
        box[1] = std::min(box[1], int32_t(floorf(py)) - 1);
        box[2] = std::max(box[2], int32_t(ceilf(px)) + 2);
        box[3] = std::max(box[3], int32_t(ceilf(py)) + 2);
        most = std::max(most, std::max(fabsf(px - w.x), fabsf(py - w.y)));
    }
    if (moved) {
        int32_t margin = 4 + int32_t(ceilf(most * 0.25f));
        box[0] -= margin;
        box[1] -= margin;
        box[2] += margin;
        box[3] += margin;
    }
    return true;
}

// (flush_batch's depth bounds of a two-phase tie, in a picture between) A native batch's Z at t 0 too, as
// interp_z_at_0 the PS2 path's: lo and hi widened to take in where nd_000.vert moves its corners at the
// frame's t 0, and a quarter of the furthest a Z went more each way (the camera's path is not a line
// between its ends). 0: not a native batch (the PS2 path's: interp_z_at_0); 1: done; -1: not known here
// (the models' own moves, 0x280's, 0x700's, the strip's, the water's, a screen pass's): no bounds.
int nd_z_at_0(double *lo, double *hi) {
    if (!nd.active) {
        return 0;
    }
    if (nd.pass_active || nd.strip_active || ndw_batch()) {
        return -1;
    }
    double zl = INFINITY, zh = -INFINITY;
    for (const Vtx &w : gs->verts) {
        uint32_t record = (w.rgba & 0xFFFFFFu) >> ND_CORNER_DRAW_SHIFT;
        if (nd_corner_kind(w) != ND_KIND_000 || nd700_corner(w) || record >= nd.boxes.size()) {
            return -1;
        }
        const NdBoxInfo &b = nd.boxes[record];
        if (!(b.flags & ND_C_MOVING)) {
            continue; // (where it is: its Z is the batch's own)
        }
        if (b.flags & ND_C_MODEL) {
            return -1;
        }
        float px, py;
        double pz;
        if (nd_corner_at_0(w, b, &px, &py, &pz)) {
            zl = std::min(zl, pz);
            zh = std::max(zh, pz);
        }
    }
    if (zl > zh) {
        return 1;
    }
    double far = std::max({0.0, *lo - zl, zh - *hi});
    *lo = std::min(*lo, zl) - far * 0.25;
    *hi = std::max(*hi, zh) + far * 0.25;
    return 1;
}

// A register write between RD_ND_ALT_BEGIN and _END while the native draws are drawn: left out, but a
// vertex kick still counts among the frame's kicks (a picture between's moves are one a kick)
void nd_skip(uint32_t address) {
    gsn_stats.nd_skipped++;
    if (interp.active && (address == R_XYZF2 || address == R_XYZ2 || address == R_XYZF3 || address == R_XYZ3)) {
        if (interp.kick < interp.frame->kicks) {
            interp.kick_list[interp.kick] = InterpKick{};
        }
        interp.kick++;
    }
}

// A vertex kick of the PS2 path's while native corners are pending. In a frame its triangle joins them,
// as on the PS2 path (the batch is decided once, as the PS2 path's, and drawn in a call a kind:
// nd_draw_range; split, its runs and blending could be decided otherwise: map 009 at 4x, a pixel of a
// seam a level apart in onoff mode). In a picture between, the native corners are drawn first: the PS2
// path's are then the GPU's (gs_replay_corners.comp), from a vertex buffer of their own.
void nd_kick() {
    if (((nd.pending || (nd.pass_pending && !nd.pass_kicking)) && interp.active) || sa_waiting()) {
        flush_batch(); // (standalone: the scene's corners never join the PS2 path's)
    }
}

// (flush_batch, a silhouette: whether a batch is one colour) The colour a corner is drawn with, when the CPU
// knows it: a PS2 path's corner's own; a native corner of a program 0x5A0 record's (the shadows, whose RGB
// nd_000.vert makes an exact 0: gsn_nd_5a0.inc) 0, with the plan's alpha. False for any other native corner
// (its RGB is the corner's encoding, its colour the shader's)
bool nd_corner_colour(const Vtx &v, uint32_t *rgba) {
    int kind = nd_corner_kind(v);
    if (kind == ND_KIND_PS2) {
        *rgba = v.rgba;
        return true;
    }
    uint32_t record = (v.rgba & 0xFFFFFFu) >> ND_CORNER_DRAW_SHIFT;
    if (kind != ND_KIND_000 || nd.batch_draws != nd.draws.buffer.get() || record >= nd.of5a0.size() ||
        !nd.of5a0[record] || ndw_batch() || nd700_corner(v)) {
        return false;
    }
    *rgba = v.rgba & 0xFF000000u;
    return true;
}

// (flush_batch's draw call: n corners from first, of a pass of the run whose vertices start at gs->verts[run],
// with the program base, the GS's.) A batch with native corners: each stretch of one kind in its own call,
// in order: the PS2 path's with base, program 0x000's with nd_000.vert (nd_program) and its records,
// program 0x700's with nd_700.vert (nd700_program; 0x000's records), program 0x280's with nd_280.vert
// (nd280_program) and its records. The batch was decided once over all of them, as the PS2 path's would be
// (0x000's and 0x280's corners together: a posed model and an entity, F-mixed)
void nd_pass_bind_tris(Cmd &c, uint32_t run, uint32_t end); // (gsn_nd_pass.inc)
void nd_draw_range(Cmd &c, Program *base, uint32_t n, uint32_t first, uint32_t run) {
    if (!nd.active) {
        c.draw(n, first);
        return;
    }
    const Vtx *v = gs->verts.data() + run;
    const Buffer *draws = nullptr;
    for (uint32_t end = first + n; first < end;) {
        int kind = nd_corner_kind(v[first]);
        bool effect = kind == ND_KIND_000 && nd700_corner(v[first]);
        uint32_t to = first + 3;
        while (to < end && nd_corner_kind(v[to]) == kind && (kind != ND_KIND_000 || nd700_corner(v[to]) == effect)) {
            to += 3;
        }
        if (kind == ND_KIND_PS2) {
            c.set_program(nd.strip_active ? nd_strip_program(base) : nd.pass_active ? nd_pass_program(base) : base);
            if (nd.pass_active) {
                nd_pass_bind_tris(c, run, to); // (its triangles, for nd_pass.vert: gsn_nd_pass.inc)
            }
        } else {
            // (a batch of the water's corners is theirs alone: their records, nd.batch_draws, nd_water.vert)
            const Buffer *want = kind == ND_KIND_280 ? nd280_batch_draws() : nd.batch_draws;
            c.set_program(kind == ND_KIND_280 ? nd280_program(base)
                          : ndw_batch()       ? ndw_program(base)
                          : effect            ? nd700_program(base)
                                              : nd_program(base));
            if (want && want != draws) {
                c.set_storage_buffer(2, 1, *want);
                draws = want;
            }
        }
        c.draw(to - first, first);
        first = to;
    }
    c.set_program(base);
}

#include "gsn_nd_oracle.inc" // (the vertex oracle: kf4-gsreplay --nd-oracle)
#include "gsn_nd_280.inc"    // (program 0x280's draws, and its oracle)
#include "gsn_nd_strip.inc"  // (a PATH2 strip's draws: the trail)
#include "gsn_nd_700.inc"    // (program 0x700's draws, the effects, and its oracle)
#include "gsn_nd_5a0.inc"    // (program 0x5A0's: the shadows)
#include "gsn_nd_water.inc"  // (the reflection's rippled water)
#include "gsn_nd_pass.inc"   // (the screen passes' draws)
