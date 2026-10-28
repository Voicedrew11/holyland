// ---- Standalone (P8): the scene drawn into its own targets -------------------------------------------
// (docs/port/NATIVE_DRAW.md 3.4 and P8; docs/port/NATIVE_DRAW_P8.md.) Included by gsn.cpp after
// gsn_native_draw.inc, inside its namespace: the native GS's own code, kept in a file of its own.
//
// With --renderer modern (gsn_set_standalone) the native draws of the scene's kinds (every ported kind:
// programs 0x000, 0x280, 0x700, 0x5A0 and the trail's strip; sa_kind) are not drawn into the GS's targets with the GS's Z. They are drawn into the
// scene's own: a colour image as large as the frame target (the GS's colour format, so the GS's
// arithmetic, gs_frag.h, makes the same colours), multisampled as --native-msaa says, and a D32F depth
// whose numbers are the GS's Z before any rounding (larger nearer: reversed Z; shaders/nd_000.vert built
// as sa_000.vert). No rounding: a draw over a surface it lies on (the layers the game blends over its
// surfaces, signs, overlays) wins within a Z of it, Z first and then its colour (sa_draw_batch, "Ties"), as
// the GS's rounding made the later one win, without the GS's sawtooth and lace.
//
// Their textures come from where the game uploaded them, not from VRAM: the page-ownership table (below,
// "Who owns VRAM") records, in the chain's order, which upload (its bytes and where it went: the game RAM
// chunk the upload function sent) owns each 256-byte block, and which pages a render target or a copy
// wrote since. A texture whose blocks all belong to uploads is decoded once from those uploads alone,
// written into a scratch VRAM (gsn_vram.c's model) and read back as the GS samples it, and found again by
// the uploads it came from. A texture read from a render target (the scrolling textures' 128 x 128
// targets, the reflection, frame copies) is read from the named target's scene image, and a copy of the
// table's uploads is the table's (T3: gsn_scene_targets.inc); anything else from the GS's targets and VRAM.
//
// The scene and the GS's targets take turns ("islands"): the scene takes the frame target's picture and Z
// buffer when its first batch comes (sa_acquire: copies), and gives them back (sa_release: its colour
// copied, its depth as the GS keeps it) before anything else of the GS touches them: a batch the
// scene does not draw into the frame or Z buffer (or reading them), an upload or copy over them, a vblank
// (sa_gs_touches_scene, sa_transfer). The depth clear before the arm and the screen's alpha set before the
// shadows are the scene's own ops (sa_scene_op). So everything the scene does not draw (the PS2 path's
// draws left, the water, the passes, the 2D, the menus) is drawn by the GS over the scene's picture, in the
// chain's order: the 2D and the passes after the world are composited over it as the PS2 drew them. As
// more kinds move into the scene the turns get fewer, down to one a frame (the world, then the GS's 2D). (T2:
// the GS's clear taken as a clear of the scene's own; the GS's leftover 3D, the water, fills and the screen
// passes drawn into the scene; the scene at a scale and samples of its own, --scene-scale, --scene-msaa, and the
// picture shown made from both: gsn_composite.inc.)
//
// What the scene cannot draw yet goes back to the GS whole (sa_decline, counted by reason): a batch that
// needs the GS's shader blending (DATE, a partial mask, PABE, ...), one into another target (the
// reflection's 256 x 256, the scrolling textures'), one that reads its own target.

const Target SA_MARKER = {}; // (gs->pass_color while the scene's render pass is under way)

volatile int sa_mode = 0; // (gsn_set_standalone)
// --scene-scale and --scene-msaa (gsn_set_scene, T2): the scene's own scale (across | down << 8) and samples; 0 the
// GS's (--native-scale, --native-msaa). Taken when the scene next takes a frame.
volatile uint32_t sa_wanted_scale = 0, sa_wanted_samples = 0;
// --upscaler (gsn_set_upscaler, docs/port/UPSCALING.md): 0 off, 1 bilinear (sa_upscale)
volatile int sa_upscaler = 0;

// (upscaling, docs/port/UPSCALING.md) The scene's sub-pixel jitter for a temporal upscaler. Each picture shown, the
// frame's own and each picture between, takes the next step of the Halton (2, 3) sequence: within half a scene pixel
// each way, added to the positions of the scene's draws into its frame (the sa_*_jitter vertex shaders add the Picture's
// counts.zw, in NDC, where GSN_M_JITTER is set: not into a named target, the reflection). On with a temporal upscaler, or with
// KF4_UPSCALE_JITTER=1 to look at it; off, no draw's position changes.
struct SaJitter {
    uint32_t index = 0;
    uint32_t phases = 8;        // the steps before the sequence begins again
    uint32_t w = 0, h = 0;      // the scene's frame, in pixels
    float px[2] = {0, 0};       // this picture's jitter, in the scene's pixels
    float ndc[2] = {0, 0};      // ... in NDC
    BufferHandle frame_picture; // the frame's own picture's Picture (zeros but the jitter: as nd.dummy otherwise)
} sa_jit;

bool sa_jitter_on() {
    static int forced = -1;
    if (forced < 0) {
        const char *v = getenv("KF4_UPSCALE_JITTER");
        forced = v && *v == '1';
    }
    return forced || sa_upscaler >= 2;
}

float sa_halton(uint32_t i, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    while (i) {
        f /= float(base);
        r += f * float(i % base);
        i /= base;
    }
    return r;
}

// The next picture's jitter: Halton's (2, 3) sequence, its first `phases` steps, then again
void sa_jitter_step() {
    sa_jit.index = sa_jit.index % sa_jit.phases + 1;
    sa_jit.px[0] = sa_halton(sa_jit.index, 2) - 0.5f;
    sa_jit.px[1] = sa_halton(sa_jit.index, 3) - 0.5f;
    sa_jit.ndc[0] = sa_jit.w ? 2.0f * sa_jit.px[0] / float(sa_jit.w) : 0.0f;
    sa_jit.ndc[1] = sa_jit.h ? 2.0f * sa_jit.px[1] / float(sa_jit.h) : 0.0f;
}

// (sa_acquire, the scene cleared for a game frame's own picture: not in a picture between) A new picture: its jitter,
// and the Picture its draws bind (nd_bind)
void sa_jitter_frame(uint32_t w, uint32_t h, uint32_t sx, uint32_t gsx) {
    if (!sa_jitter_on()) {
        return;
    }
    sa_jit.w = w;
    sa_jit.h = h;
    // (FSR's count: 8 for each output pixel's share of a scene pixel, 8 x the scale's ratio squared; 8 at least)
    float ratio = sx < gsx ? float(gsx) / float(sx) : 1.0f;
    sa_jit.phases = std::max(8u, uint32_t(8.0f * ratio * ratio));
    sa_jitter_step();
    float data[64] = {}; // (256 bytes, as nd.dummy: the jitter in counts.zw, after move, mid, screen and zt, gs_replay.h's order)
    data[38] = sa_jit.ndc[0];
    data[39] = sa_jit.ndc[1];
    sa_jit.frame_picture = interp_buffer(sizeof(data), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, data);
}

// (interp_picture_now) A picture between: its own jitter, into its Picture (in NDC, then in the scene's pixels)
void sa_jitter_picture(float *jitter) {
    if (!sa_jitter_on() || !sa_jit.w) {
        jitter[0] = jitter[1] = jitter[2] = jitter[3] = 0.0f;
        return;
    }
    sa_jitter_step();
    jitter[0] = sa_jit.ndc[0];
    jitter[1] = sa_jit.ndc[1];
    jitter[2] = sa_jit.px[0];
    jitter[3] = sa_jit.px[1];
}

// (nd_bind, the frame's own draws) The Picture to bind: the frame's, with its jitter, or nothing (nd.dummy)
const Buffer *sa_jitter_frame_picture() {
    return sa_jitter_on() && sa_jit.frame_picture ? sa_jit.frame_picture.get() : nullptr;
}

// (sa_draw_batch) Whether this batch takes the picture's jitter: the scene's frame's, not a named target's
bool sa_jitter_draw();

// (upscaling) The world's camera, for the composite's motion (docs/port/UPSCALING.md): as a game frame's own draws
// have it (sa_camera_seen: native draw's records, the camera of mode 0, projection 0), and each picture between's,
// made of it by the picture's move (sa_camera_picture). Matrices row-major, [row * 4 + col]; the game's are VU layout
// (m[i] the i-th column). world_screen takes a world place to the GS's X, Y and Z times w (X = c.x / c.w ...).
struct SaCamera {
    bool valid = false;
    double world_screen[16] = {}, view[16] = {}, screen[16] = {};
    float origin[2] = {0, 0};                 // XYOFFSET, in GS pixels
    double picture[16] = {};                  // the last picture between's world_screen
    float picture_jitter[2] = {0, 0};         // ... and its jitter, in the scene's pixels
} sa_cam;

void sa_mat_mul(double *r, const double *a, const double *b) {
    double t[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) {
                s += a[i * 4 + k] * b[k * 4 + j];
            }
            t[i * 4 + j] = s;
        }
    }
    memcpy(r, t, sizeof(t));
}

// The inverse of a 4 x 4 (false: none)
bool sa_mat_inverse(double *r, const double *m) {
    double a[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            a[i][j] = m[i * 4 + j];
            a[i][j + 4] = i == j;
        }
    }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int i = c + 1; i < 4; i++) {
            if (fabs(a[i][c]) > fabs(a[p][c])) {
                p = i;
            }
        }
        if (fabs(a[p][c]) < 1e-300) {
            return false;
        }
        for (int j = 0; j < 8; j++) {
            std::swap(a[c][j], a[p][j]);
        }
        double d = a[c][c];
        for (int j = 0; j < 8; j++) {
            a[c][j] /= d;
        }
        for (int i = 0; i < 4; i++) {
            if (i != c) {
                double f = a[i][c];
                for (int j = 0; j < 8; j++) {
                    a[i][j] -= f * a[c][j];
                }
            }
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i * 4 + j] = a[i][j + 4];
        }
    }
    return true;
}

// (nd_draw, a game frame's own draws) The world's camera, as its draws have it
void sa_camera_seen(const RdCamera &cam, int32_t ofx, int32_t ofy) {
    if (!sa_mode || cam.mode != 0 || cam.projection != 0) {
        return;
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            sa_cam.world_screen[r * 4 + c] = cam.world_screen[c][r];
            sa_cam.view[r * 4 + c] = cam.view[c][r];
            sa_cam.screen[r * 4 + c] = cam.screen[c][r];
        }
    }
    sa_cam.origin[0] = float(ofx) / 16.0f;
    sa_cam.origin[1] = float(ofy) / 16.0f;
    sa_cam.valid = true;
}

// (interp_picture_now) A picture between's camera: the frame's view moved by the picture's move (rows of the rotation
// less the identity, each with its translation: the frame's view to the picture's), then the frame's view to screen
void sa_camera_picture(const float *move, const float *jitter_px) {
    double m[16] = {};
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            m[r * 4 + c] = double(move[r * 4 + c]) + (r == c ? 1.0 : 0.0);
        }
        m[r * 4 + 3] = move[r * 4 + 3];
    }
    m[15] = 1.0;
    double mv[16];
    sa_mat_mul(mv, m, sa_cam.view);
    sa_mat_mul(sa_cam.picture, sa_cam.screen, mv);
    sa_cam.picture_jitter[0] = jitter_px[0];
    sa_cam.picture_jitter[1] = jitter_px[1];
}

// Whether the composite works out the scene's motion: with a temporal upscaler, or to look at it
// (KF4_UPSCALE_SHOW=motion)
int sa_motion_show() {
    static int show = -1;
    if (show < 0) {
        const char *v = getenv("KF4_UPSCALE_SHOW");
        show = v && strcmp(v, "motion") == 0;
    }
    return show;
}
bool sa_motion_wanted() {
    return sa_upscaler >= 2 || sa_motion_show();
}

enum {
    SA_WHY_FRAME,    // another target than a frame of 512 pixels or more across, 32 or 24-bit
    SA_WHY_TARGET,   // the frame and Z buffer could not be had together
    SA_WHY_BLEND,    // the GS's blending in the shader (DATE, partial masks, PABE, no COLCLAMP, FBA by As ...)
    SA_WHY_OWN,      // its texture is the frame it draws into
    SA_WHY_SAMPLES,  // multisampled, on a GPU without sample-rate shading (the copies back need it)
    SA_WHY_COUNT
};
const char *const sa_why_names[SA_WHY_COUNT] = {"other target", "targets", "shader blending", "reads its frame",
                                               "no sample shading"};
enum {
    SA_TEX_TABLE,     // decoded from the uploads that own it
    SA_TEX_TARGET,    // a render target's (the GS's)
    SA_TEX_DRAWN,     // a part of it drawn into or copied over since its upload (the GS's VRAM)
    SA_TEX_UNOWNED,   // a part of it no upload wrote (never uploaded since the start, or a save state's)
    SA_TEX_EVICTED,   // an upload of it no longer kept
    SA_TEX_COUNT
};
const char *const sa_tex_names[SA_TEX_COUNT] = {"from the table", "a render target", "drawn or copied over",
                                               "not uploaded", "upload let go"};

// (every field a count: --stats takes them per map as differences, sa_vblank)
struct SaStats {
    uint64_t batches, draws, triangles, shader_blends, acquires, releases, frames_touched;
    uint64_t depth_clears, alpha_sets, shadow_batches, own_layers; // (the scene's ops, and the shadows by the stencil)
    // (T2) frames taken from the GS's clear (no copy), the GS's batches drawn into the scene (its 3D, the screen
    // passes, fills), its frame given to the GS and kept (a copy that reads it), pictures made by the composite
    uint64_t cleared, gs_3d, gs_passes, gs_fills, gs_water, published, resampled, composites, composite_ops, upscales, upscales_temporal, motions;
    uint64_t frame_copies, frame_copy_reads; // (the scene's copies of its frame elsewhere, and textures read from them)
    uint64_t declined[SA_WHY_COUNT];
    uint64_t textures[SA_TEX_COUNT];
    uint64_t decodes, decode_hits, uploads, upload_bytes, chunks_new, replayed_uploads;
    // the CLUT (TEX0's CLD loads, sa_clut_load; a batch's colours, sa_clut_colours)
    uint64_t clut_loads[SA_TEX_COUNT], clut_decodes, clut_table, clut_gs;
    // KF4_SA_VERIFY=1: the table's texture and CLUT against the GS's VRAM and CLUT buffer, for every batch
    uint64_t verify_same, verify_differ, verify_clut_same, verify_clut_differ;
    uint64_t verify_not[SA_TEX_COUNT];
    // texture packs (KF4_SA_PACK, KF4_SA_PACK_DUMP)
    uint64_t packed_chunks, dumped_chunks;
    uint64_t tables_loaded; // (a save state's or a capture's table)
    uint64_t kept_reflection; // (batches into the reflection left out by pictures between that keep the frame's: sat_kept)
    uint64_t zfirst_alone;    // (ties' Z-first draws by depth alone: sa_zfirst.frag)
    uint64_t shine_z_left;    // (perf/icy: second passes blended in the shader drawn without their Z-first draws)
};
SaStats sa_stats;

// ---- Who owns VRAM: the page-ownership table -------------------------------------------------------
// An upload's bytes and where they went (BITBLTBUF, TRXPOS, TRXREG): what an upload function sent
struct SaChunk {
    uint64_t bitbltbuf, trxpos, trxreg;
    std::vector<uint8_t> bytes;
    std::shared_ptr<const std::vector<uint8_t>> pack; // (KF4_SA_PACK: the bytes a texture pack gives it instead)
    uint64_t seq;  // the table's count when it was last uploaded
    uint64_t used; // the frame it was last read or uploaded in
    uint64_t hash; // (upload_hash: what it is found again by)
    uint64_t key;  // (sa_pack_key: what a texture pack knows it by)
    const std::vector<uint8_t> &data() const {
        return pack ? *pack : bytes;
    }
};
const uint32_t SA_BLOCKS = GSN_VRAM_BYTES / 256;
// Which nibbles of a block's 64 words: bit w * 8 + n is nibble n (bits 4n..4n+3) of word w. The formats
// that own part of each word (PSMCT24 and Z24 the low 24 bits, PSMT8H the top byte, PSMT4HL and HH a
// nibble of it), the 16-bit ones (a halfword), PSMT8 and PSMT4 own their pixels' nibbles only, so an
// upload of a CLUT's 16 entries or of the font's 4HL sheet is the owner of those and no more.
struct SaCover {
    uint64_t q[8];
    bool any() const {
        return (q[0] | q[1] | q[2] | q[3] | q[4] | q[5] | q[6] | q[7]) != 0;
    }
    uint64_t folded() const { // (the nibbles of any word: which parts it has)
        uint64_t f = q[0] | q[1] | q[2] | q[3] | q[4] | q[5] | q[6] | q[7];
        f |= f >> 32;
        f |= f >> 16;
        f |= f >> 8;
        return f & 0xFF;
    }
};
inline SaCover sa_and(const SaCover &a, const SaCover &b) {
    SaCover c;
    for (int i = 0; i < 8; i++) {
        c.q[i] = a.q[i] & b.q[i];
    }
    return c;
}
inline bool sa_same(const SaCover &a, const SaCover &b) {
    return memcmp(a.q, b.q, sizeof(a.q)) == 0;
}
const uint8_t SA_LO = 0x3F, SA_HI = 0xC0; // (the nibbles of the low 24 bits, of the top byte)
// The nibbles of each word a format's pixels own
uint8_t sa_parts(uint32_t psm) {
    switch (psm) {
    case GSN_PSMCT24:
    case GSN_PSMZ24:
        return SA_LO;
    case GSN_PSMT8H:
        return SA_HI;
    case GSN_PSMT4HL:
        return 0x40;
    case GSN_PSMT4HH:
        return 0x80;
    default:
        return 0xFF;
    }
}
// A block's uploads: each with the nibbles of it that are still its own (none shared: a later upload takes
// its nibbles from those before), in the order they came. An owner of id 0 holds no upload's bytes.
struct SaOwner {
    uint32_t id;
    uint64_t seq; // the table's count when it was written
    SaCover cover;
};
struct SaBlock {
    std::vector<SaOwner> owners;
};

// The table's state, kept apart from the rest of the scene's (struct Sa)
struct SaTable {
    std::vector<SaBlock> blocks;
    // the count when a page was last drawn into or copied over, in its low 24 bits and in its top byte
    uint64_t page_drawn_lo[512] = {}, page_drawn_hi[512] = {};
    size_t owners_most = 0;
    // bumped at every change of the owners or of the pages drawn (sa_owners' memo is good while it stays)
    uint64_t gen = 1;
    // the CLUT buffer as the table has it (sa_clut_load): TEX0's CLD loads mirrored, from the uploads
    uint16_t clut[512] = {};
    uint32_t clut_cbp[2] = {UINT32_MAX, UINT32_MAX};
    uint64_t clut_gs[8] = {};     // (bit k: halfword k is not the table's: loaded from the GS's VRAM, or not yet)
    uint64_t clut_packed[8] = {}; // (bit k: halfword k came from a texture pack's chunk)
    std::unordered_map<uint64_t, std::array<uint32_t, 256>> clut_cache; // (loads by what they came from)
    // texture packs: KF4_SA_PACK=DIR (chunks' bytes replaced by DIR/<key>.bin), KF4_SA_PACK_DUMP=DIR
    std::string pack_dir, dump_dir;
    std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> pack_cache;
    std::unordered_map<uint64_t, bool> dumped;
    int verify = 0; // (KF4_SA_VERIFY=1; 2 logs what differs, 3 also what is not the table's, 4 each TEX0 once)
    std::unordered_map<uint64_t, bool> verified_seen;
    // --stats per map (gsn_set_scene_map: the map the game is in, at the vblank)
    std::atomic<int> map{-1};
    int map_now = -1;
    SaStats at_vblank = {};
    std::vector<std::pair<int, SaStats>> per_map;
    std::mutex per_map_lock; // (--stats reads them on the main thread while the GS's may still run)
    bool env_read = false;
} sat;

// (perf/icy) a corner's place, bit for bit: x and y's bits, z
std::array<uint32_t, 3> sa_place_bits(const Vtx &v) {
    uint32_t x, y;
    memcpy(&x, &v.x, 4);
    memcpy(&y, &v.y, 4);
    return {x, y, v.z};
}

struct Sa {
    bool on = false;
    // the batch's corners, and the draw being met (nd_register): to the scene or not
    bool pending = false, in_draw = false, draw_on = false;
    // (pc world, gsn_pc_world.inc) the draw being met is the PC renderer's (its map records); the corners pending after
    // a draw are a taken draw's; the batch being drawn is (sa_flush: a batch's corners are drawn after their draws)
    bool pc_take = false, pending_pc = false, batch_pc = false;
    // every ported kind (T1): map records and the sky, objects, items, gold, entities, the arm and the held
    // weapon (and its trail), effects, shadows, the picked item, and (T3) the reflection's water, reading the
    // reflection the scene drew (gsn_scene_targets.inc). Not the screen passes (the GS's 2D). (KF4_SA_KINDS: a
    // mask of RD_KIND_ bits)
    uint32_t kinds = (1u << RD_KIND_MAP_RECORD) | (1u << RD_KIND_MAP_OBJECT) | (1u << RD_KIND_ITEM) | (1u << RD_KIND_GOLD) |
                     (1u << RD_KIND_ENTITY) | (1u << RD_KIND_ARM) | (1u << RD_KIND_EFFECT) | (1u << RD_KIND_SHADOW) |
                     (1u << RD_KIND_PICKED) | (1u << RD_KIND_WATER);
    int log = 0;                      // (KF4_SA_LOG=1: each batch, why one is declined and what ends a turn; 2: only the turns)
    float pixel[2] = {-1.0f, -1.0f};  // (KF4_SA_PIXEL=X,Y: the batches over that GS pixel)
    std::vector<std::shared_ptr<const RdMeshData>> fetched; // (sa_draw_begin's)
    // (sa_draw_batch's, kept from batch to batch for their room: globals, not a function's statics, which would be
    // made on the GS's thread and gone at the exit while it still draws)
    std::vector<uint32_t> runs;
    std::vector<Vtx> corners;
    std::vector<std::array<float, 4>> tri_boxes; // (sa_draw_batch's runs: each triangle's box)
    // the table
    uint64_t seq = 0, frame = 0;
    std::unordered_map<uint32_t, SaChunk> chunks;
    std::unordered_map<uint64_t, uint32_t> chunk_by_hash;
    std::deque<uint32_t> chunk_order;
    size_t chunk_bytes = 0;
    uint32_t next_id = 1;
    // the uploads of the frame, in gsn.cpp's uploads_now order, for a picture between's asking for them again
    struct Recorded {
        uint64_t bitbltbuf, trxpos, trxreg;
        uint32_t id;
    };
    std::vector<Recorded> uploads_now, uploads_frame;
    int recording = -1;
    std::vector<uint8_t> scratch; // the scratch VRAM a texture is decoded in
    // decoded textures by what they came from (sa_texture_key)
    struct Decoded {
        ImageHandle image;
        std::array<uint32_t, 256> texels;
        uint64_t serial = 0;               // (one for each decode, never again: gsn_rgba.inc's key)
        uint32_t width = 0, height = 0, levels = 0;
        std::vector<uint32_t> raw;         // (--native-sampler hardware) the raw texels, every level's back to back
    };
    uint64_t decode_serial = 0;
    std::unordered_map<uint64_t, Decoded> decoded;
    std::deque<uint64_t> decoded_order;
    size_t decoded_bytes = 0;
    // the scene's targets, and the GS's frame and Z buffer it has taken (bound)
    ImageHandle color, depth;
    ImageHandle copy; // (shader blending's copy of the scene's colour)
    uint32_t samples = 0;
    bool bound = false;
    uint32_t fbp = 0, fbw = 0, fpsm = 0, zbp = 0, zpsm = 0;
    Target *frame_target = nullptr, *z_target = nullptr;
    uint32_t rows0 = 0, rows1 = 0; // the rows the scene drew into since it took them (GS pixels)
    bool z_written = false;
    // The shadows' "each pixel darkened once" (A17): the scene's depth has a stencil (D32F S8, where the GPU has
    // it), cleared when the scene takes the frame; a run of shadow batches (Entity_DrawPoolModels' loop, after
    // its GsDraw_SetScreenAlpha(0)) draws where the stencil is not its number and writes it there: first
    // write wins, as DATE and FBA had it on the GS, without the shader's blending from a copy
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    bool stencil = false;
    uint32_t stencil_gen = 0; // the number of the last run of shadow batches since the stencil was cleared
    bool last_shadow = false; // the scene's last batch was a shadow's
    // (perf/icy) The scene's last batch, when it was a skinned model's first pass that wrote its Z exactly over every
    // pixel of its triangles (two-phase, GEQUAL, no pixel failing the alpha test): its corners' places (x, y as
    // bits, z), so that its second pass over the same corners can leave out its Z-first draw (sa_draw_batch,
    // "shine_z_there"). Taken into first_z_before and cleared at each batch (sa_flush).
    std::vector<std::array<uint32_t, 3>> first_z, first_z_before;
    bool first_z_on = false, first_z_before_on = false;
    // programs: [SA_VS_*][a cut-out shaded at every sample]
    Shader *vs = nullptr, *vs_280 = nullptr, *vs_700 = nullptr;
    Program *programs[3][2] = {};
    Program *programs_in[3][2] = {}; // (T8) [SA_VS_*][multisampled]: blending from the pass's scratch (gs_in*.frag)
    Program *programs_copy[3][2] = {}; // [SA_VS_*][multisampled]: a run's copy into the scratch by its own triangles
    Program *programs_z[3] = {};       // [SA_VS_*]: a tie's Z-first draw that no pixel can fail (sa_zfirst.frag)
    // (--native-sampler hardware, gsn_rgba.inc) programs and programs_in reading the batch's RGBA8 texture (the
    // GSN_HW_SAMPLER builds), and the batch's: whether it reads one, which, and with what sampler
    Program *programs_hw[3][2] = {};
    Program *programs_in_hw[3][2] = {};
    bool rgba_on = false;
    const ImageView *rgba_view = nullptr;
    ImageHandle rgba_image;  // (the image rgba_view is of: held for the PC world's material)
    StockSampler rgba_sampler = StockSampler::NearestClamp;
    Program *program_copy_ms = nullptr;
    Program *program_depth = nullptr, *program_depth_ms = nullptr;
    // (T2) the scene's scale (its pixels a GS pixel is, across and down) and samples as its targets have them now,
    // and the programs that take the GS's picture into a scene of another shape and give it back
    // (shaders/sa_resample.frag: [depth][multisampled source])
    uint32_t sx = 1, sy = 1;
    bool drew = false; // (drawn into since it took the frame)
    uint32_t scissor_rows = 0; // (the rows of the scissors it drew with since: the world's, 448)
    Program *program_resample[2][2] = {};
    ImageHandle own; // (a texture that is the frame the scene draws into: a copy of its colour, sa_draw_batch)
    // (T2) A local copy of the scene's frame elsewhere (GsDraw_CopyFrame's to 0x2BC0, which the wobble under water
    // and the MP drain read): the scene's picture as it was copied, at its scale, read for a texture there until
    // those pages are drawn into, uploaded to or copied over (sa_frame_copied; sa_texture)
    struct Copy {
        bool valid = false, fresh = false;
        uint32_t dbp, dbw, dpsm, dx, dy, sx, sy, w, h, scale_x, scale_y;
        ImageHandle image;
    } fcopy;
    // what the batch drawn is (sa_flush): a screen pass's (nd_pass_draw, sa_pass), the GS's own 3D (the PS2 path's
    // corners, sa_gs_batch)
    bool in_pass = false, gs3d = false, gs_fill = false, gs_water = false;
    // The GS's clear of a frame and its Z buffer (libgraph's at the swap: one colour, Z 0, the whole frame): while
    // nothing else of the GS has touched them since, the scene takes that frame by clearing its own targets the
    // same, with no copy (sa_acquire)
    struct Clear {
        bool valid;
        uint32_t fbp, fbw, fpsm, zbp, zpsm, rgba, z, rows;
    } clear = {};
} sa;
enum { SA_VS_000, SA_VS_280, SA_VS_700 }; // (sa.programs: sa_000.vert, sa_280.vert, sa_700.vert)

void sa_release();
void sa_sync();
// (T3: render targets by name, gsn_scene_targets.inc, included at the end)
struct SaTexture;
void sat_drawn(const Target &t, uint32_t y0, uint32_t y1);
bool sat_copied(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);
bool sat_texture(SaTexture &st, uint32_t tbp, uint32_t tbw, uint32_t tpsm, uint32_t cols, uint32_t rows, uint32_t levels);
bool sat_scene_frame(uint32_t fbw, uint32_t fpsm);
Program *sat_water_program(bool sample);
bool sat_acquire(uint32_t fbp, uint32_t fbw, uint32_t fpsm, uint32_t zbp, uint32_t zpsm, uint32_t need_h);
bool sat_release();
bool sat_bound(); // (a named target is the scene's frame now)
bool sat_kept(uint32_t bp, uint32_t bw, uint32_t psm); // (a picture between leaves the reflection as the frame drew it)
bool sat_stencil_number(uint32_t *gen);
bool sat_flush();
bool sat_fill();
bool sat_blend_exact();
void sat_gs_batch();
bool sat_transfer(uint32_t dir);
void sat_sync();
void sat_reset();
void sat_print_stats();

// The blocks of a rectangle of a buffer (bp, bw, psm), x0..x0+w by y0..y0+h, each with the nibbles of it
// the rectangle's pixels are
uint32_t sa_block_w(uint32_t psm) {
    return psm == GSN_PSMT4 ? 32 : psm == GSN_PSMT8 || psm_is_16(psm) ? 16 : 8;
}
uint32_t sa_block_h(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT4 ? 16 : 8;
}
template <class F> void sa_blocks_of(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                                     F &&f) {
    if (!gsn_psm_valid(psm) || w == 0 || h == 0) {
        return;
    }
    uint32_t bx = sa_block_w(psm), by = sa_block_h(psm);
    uint8_t parts = sa_parts(psm);
    SaCover whole;
    for (uint64_t &q : whole.q) {
        q = uint64_t(parts) * 0x0101010101010101ull;
    }
    uint32_t xa = x0 / bx * bx, ya = y0 / by * by;
    for (uint32_t y = ya; y < y0 + h; y += by) {
        for (uint32_t x = xa; x < x0 + w; x += bx) {
            uint32_t block = (gsn_vram_offset(psm, bp, bw, x & 2047, y & 2047, nullptr) / 256) % SA_BLOCKS;
            if (x >= x0 && x + bx <= x0 + w && y >= y0 && y + by <= y0 + h) {
                f(block, whole);
                continue;
            }
            // a block the rectangle has part of: its pixels' nibbles, one by one
            SaCover c = {};
            for (uint32_t py = std::max(y, y0); py < std::min(y + by, y0 + h); py++) {
                for (uint32_t px = std::max(x, x0); px < std::min(x + bx, x0 + w); px++) {
                    int nibble = -1;
                    uint32_t at = gsn_vram_offset(psm, bp, bw, px & 2047, py & 2047, &nibble) % 256;
                    uint32_t word = at / 4;
                    uint8_t m = psm_is_16(psm)       ? ((at & 2) ? 0xF0 : 0x0F)
                                : psm == GSN_PSMT8   ? uint8_t(3u << ((at & 3) * 2))
                                : psm == GSN_PSMT4   ? uint8_t(1u << ((at & 3) * 2 + (nibble > 0 ? 1 : 0)))
                                                     : parts;
                    c.q[word / 8] |= uint64_t(m) << (word % 8 * 8);
                }
            }
            f(block, c);
        }
    }
}

void sa_table_env();

void sa_table_reset() {
    sa_table_env();
    sat.blocks.assign(SA_BLOCKS, SaBlock{});
    sat.gen++;
    memset(sat.page_drawn_lo, 0, sizeof(sat.page_drawn_lo));
    memset(sat.page_drawn_hi, 0, sizeof(sat.page_drawn_hi));
    memset(sat.clut, 0, sizeof(sat.clut));
    sat.clut_cbp[0] = sat.clut_cbp[1] = UINT32_MAX;
    memset(sat.clut_gs, 0xFF, sizeof(sat.clut_gs));
    memset(sat.clut_packed, 0, sizeof(sat.clut_packed));
    sa.uploads_now.clear();
    sa.uploads_frame.clear();
    sa.recording = -1;
}

// (T2) Pages lo..lo+count-1 written: the scene's copy of its frame there let go
void sa_copy_written(uint32_t lo, uint32_t count) {
    if (!sa.fcopy.valid || sa.fcopy.fresh) { // (fresh: the copy of the frame itself, written as it is made)
        return;
    }
    uint32_t clo, ccount;
    buffer_pages(sa.fcopy.dbp, sa.fcopy.dbw, sa.fcopy.dpsm, sa.fcopy.dx + sa.fcopy.w, sa.fcopy.dy + sa.fcopy.h, &clo, &ccount);
    if (pages_overlap(lo, count, clo, ccount)) {
        sa.fcopy.valid = false;
    }
}

// The table's switches, read once (when modern is first turned on)
void sa_table_env() {
    if (sat.env_read) {
        return;
    }
    sat.env_read = true;
    const char *v = getenv("KF4_SA_VERIFY");
    sat.verify = v ? atoi(v) : 0;
    if ((v = getenv("KF4_SA_PACK")) && *v) {
        sat.pack_dir = v;
        fprintf(stderr, "[standalone] texture pack from %s\n", v);
    }
    if ((v = getenv("KF4_SA_PACK_DUMP")) && *v) {
        sat.dump_dir = v;
#ifdef _WIN32
        _mkdir(v);
#else
        mkdir(v, 0755);
#endif
        fprintf(stderr, "[standalone] the uploads of the scene's textures written to %s\n", v);
    }
}

// Pages lo..lo+count-1 drawn into or copied over, in the nibbles a format of psm has of each word
void sa_pages_drawn(uint32_t lo, uint32_t count, uint32_t psm) {
    uint8_t parts = sa_parts(psm);
    sa.seq++;
    sat.gen++;
    for (uint32_t k = 0; k < count && k < vram_pages; k++) {
        uint32_t page = (lo + k) % vram_pages;
        if (parts & SA_LO) {
            sat.page_drawn_lo[page] = sa.seq;
        }
        if (parts & SA_HI) {
            sat.page_drawn_hi[page] = sa.seq;
        }
    }
}

// (mark_dirty) Rows y0..y1-1 of a target drawn into: its pages are not their uploads' any more (a 24-bit
// target's only in the low 24 bits: PSMT8H and the 4-bit high formats in the top byte stay theirs)
void sa_drawn(const Target &t, uint32_t y0, uint32_t y1) {
    if (!sa.on || y1 <= y0) {
        return;
    }
    uint32_t lo, count;
    buffer_pages(t.bp, t.bw, t.psm, t.width, std::min(y1, t.height), &lo, &count);
    uint32_t first_row = y0 / page_height(t.psm);
    uint32_t across = std::max(std::max(t.bw, 1u) * 64 / page_width(t.psm), 1u);
    uint32_t first = std::min(first_row * across, count);
    sa_pages_drawn(lo + first, count - first, t.psm);
    sa_copy_written(lo, count);
    sat_drawn(t, y0, y1); // (T3: a named target's pages, or none's)
}

// (begin_transfer) A local-to-local copy's destination: its pages are not their uploads' any more
void sa_copied(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg) {
    if (!sa.on) {
        return;
    }
    uint32_t lo, count;
    buffer_pages(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), bits(bitbltbuf, 56, 6),
                 bits(trxpos, 32, 11) + bits(trxreg, 0, 12), bits(trxpos, 48, 11) + bits(trxreg, 32, 12), &lo, &count);
    if (sat_copied(bitbltbuf, trxpos, trxreg)) {
        sa_copy_written(lo, count); // (T2: the scene's copy of its frame there let go)
        return;                     // (T3: a copy of the table's uploads is the table's)
    }
    sa_pages_drawn(lo, count, bits(bitbltbuf, 56, 6));
    sa_copy_written(lo, count);
}

// The chunk a block list names, if still kept
SaChunk *sa_chunk(uint32_t id) {
    auto it = sa.chunks.find(id);
    return it == sa.chunks.end() ? nullptr : &it->second;
}

// The upload's nibbles are its own: taken from the uploads before it, which keep the rest
void sa_own(uint32_t id, SaChunk &c) {
    sa.seq++;
    sat.gen++;
    c.seq = sa.seq;
    uint64_t bitbltbuf = c.bitbltbuf;
    sa_blocks_of(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), bits(bitbltbuf, 56, 6), bits(c.trxpos, 32, 11),
                 bits(c.trxpos, 48, 11), bits(c.trxreg, 0, 12), bits(c.trxreg, 32, 12),
                 [&](uint32_t block, const SaCover &cover) {
                     std::vector<SaOwner> &owners = sat.blocks[block].owners;
                     size_t n = 0;
                     for (size_t i = 0; i < owners.size(); i++) {
                         SaOwner &o = owners[i];
                         bool left = false;
                         for (int k = 0; k < 8; k++) {
                             o.cover.q[k] &= ~cover.q[k];
                             left |= o.cover.q[k] != 0;
                         }
                         if (left) {
                             owners[n++] = o;
                         }
                     }
                     owners.resize(n);
                     owners.push_back({id, sa.seq, cover});
                     sat.owners_most = std::max(sat.owners_most, owners.size());
                 });
}

// A texture pack's name for a chunk: its format, its rectangle's size and its bytes (not where it went)
uint64_t sa_pack_key(uint64_t bitbltbuf, uint64_t trxreg, const uint8_t *data, size_t size) {
    PendingUpload p;
    p.bitbltbuf = bits(bitbltbuf, 56, 6);
    p.trxpos = 0;
    p.trxreg = trxreg & 0x00000FFF00000FFFull;
    return upload_hash(p, data, size);
}

// (KF4_SA_PACK) The pack's bytes for a chunk, if the pack has them: DIR/<key>.bin, as long as the chunk
std::shared_ptr<const std::vector<uint8_t>> sa_pack_bytes(uint64_t key, size_t size) {
    if (sat.pack_dir.empty()) {
        return nullptr;
    }
    auto found = sat.pack_cache.find(key);
    if (found == sat.pack_cache.end()) {
        std::shared_ptr<std::vector<uint8_t>> bytes;
        char path[1200];
        snprintf(path, sizeof(path), "%s/%016llx.bin", sat.pack_dir.c_str(), (unsigned long long)key);
        if (FILE *f = fopen(path, "rb")) {
            bytes = std::make_shared<std::vector<uint8_t>>();
            uint8_t buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
                bytes->insert(bytes->end(), buf, buf + got);
            }
            fclose(f);
            fprintf(stderr, "[standalone] texture pack: %s (%zu bytes)\n", path, bytes->size());
        }
        found = sat.pack_cache.emplace(key, bytes).first;
    }
    if (found->second && found->second->size() != size) {
        return nullptr; // (not the chunk's size: not used)
    }
    return found->second;
}

// (KF4_SA_PACK_DUMP) A chunk a scene texture or CLUT came from, written once: DIR/<key>.bin (its bytes as
// the upload sent them) and DIR/<key>.txt (its format and size; tools/texture_pack.py makes pictures of them)
void sa_pack_dump(const SaChunk &c) {
    if (sat.dump_dir.empty() || sat.dumped.count(c.key)) {
        return;
    }
    sat.dumped[c.key] = true;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%016llx.bin", sat.dump_dir.c_str(), (unsigned long long)c.key);
    if (FILE *f = fopen(path, "wb")) {
        fwrite(c.bytes.data(), 1, c.bytes.size(), f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/%016llx.txt", sat.dump_dir.c_str(), (unsigned long long)c.key);
    if (FILE *f = fopen(path, "w")) {
        fprintf(f, "psm=0x%02x w=%u h=%u bp=0x%x bw=%u x=%u y=%u map=%d\n", bits(c.bitbltbuf, 56, 6), bits(c.trxreg, 0, 12),
                bits(c.trxreg, 32, 12), bits(c.bitbltbuf, 32, 14), bits(c.bitbltbuf, 48, 6), bits(c.trxpos, 32, 11),
                bits(c.trxpos, 48, 11), sat.map_now);
        fclose(f);
    }
    sa_stats.dumped_chunks++;
}

// (complete_upload, upload_done) An upload whole: its bytes kept (once: found again by their hash, an
// upload sent every frame is the same chunk), its blocks its own. `hash` is upload_hash's of it (complete_upload
// has it already)
void sa_uploaded(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const uint8_t *data, size_t size, uint64_t hash) {
    if (!sa.on) {
        return;
    }
    sa_stats.uploads++;
    sa_stats.upload_bytes += size;
    {
        uint32_t lo, count;
        buffer_pages(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), bits(bitbltbuf, 56, 6),
                     bits(trxpos, 32, 11) + bits(trxreg, 0, 12), bits(trxpos, 48, 11) + bits(trxreg, 32, 12), &lo, &count);
        sa_copy_written(lo, count);
    }
    uint32_t id = 0;
    auto found = sa.chunk_by_hash.find(hash);
    if (found != sa.chunk_by_hash.end()) {
        SaChunk *c = sa_chunk(found->second);
        if (c && c->bitbltbuf == bitbltbuf && c->trxpos == trxpos && c->trxreg == trxreg && c->bytes.size() == size &&
            memcmp(c->bytes.data(), data, size) == 0) {
            id = found->second;
        }
    }
    if (!id) {
        id = sa.next_id++;
        if (!sa.next_id) {
            sa.next_id = 1;
        }
        SaChunk &c = sa.chunks[id];
        c.bitbltbuf = bitbltbuf;
        c.trxpos = trxpos;
        c.trxreg = trxreg;
        c.bytes.assign(data, data + size);
        c.hash = hash;
        c.used = sa.frame;
        if (!sat.pack_dir.empty() || !sat.dump_dir.empty()) {
            c.key = sa_pack_key(bitbltbuf, trxreg, data, size);
            c.pack = sa_pack_bytes(c.key, size);
            sa_stats.packed_chunks += c.pack ? 1 : 0;
        }
        sa.chunk_by_hash[hash] = id;
        sa.chunk_order.push_back(id);
        sa.chunk_bytes += size;
        sa_stats.chunks_new++;
        // the oldest not used for two frames let go beyond 256 MB (a block naming one gone is the GS's); those
        // in use go to the back, each looked at once at most
        for (size_t tries = sa.chunk_order.size(); sa.chunk_bytes > (256u << 20) && tries > 0; tries--) {
            uint32_t old = sa.chunk_order.front();
            sa.chunk_order.pop_front();
            SaChunk *o = sa_chunk(old);
            if (!o) {
                continue;
            }
            if (o->used + 2 >= sa.frame) {
                sa.chunk_order.push_back(old);
                continue;
            }
            sa.chunk_bytes -= o->bytes.size();
            auto h = sa.chunk_by_hash.find(o->hash);
            if (h != sa.chunk_by_hash.end() && h->second == old) {
                sa.chunk_by_hash.erase(h);
            }
            sa.chunks.erase(old);
        }
    }
    SaChunk &c = sa.chunks[id];
    c.used = sa.frame;
    sa_own(id, c);
    if (sa.recording >= 0 && size_t(sa.recording) < sa.uploads_now.size()) {
        sa.uploads_now[size_t(sa.recording)].id = id;
    }
    sa.recording = -1;
}

void sa_uploaded(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const uint8_t *data, size_t size) {
    if (!sa.on) {
        return;
    }
    PendingUpload p;
    p.bitbltbuf = bitbltbuf;
    p.trxpos = trxpos;
    p.trxreg = trxreg;
    sa_uploaded(bitbltbuf, trxpos, trxreg, data, size, upload_hash(p, data, size));
}

// (upload_done) An upload written as it came (settle_upload): its bytes read back from VRAM
void sa_uploaded_from_vram(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg) {
    if (!sa.on) {
        return;
    }
    uint32_t psm = bits(bitbltbuf, 56, 6);
    size_t qwords = (size_t(bits(trxreg, 0, 12)) * bits(trxreg, 32, 12) * gsn_psm_bits(psm) + 127) / 128;
    std::vector<uint8_t> bytes(qwords * 16);
    // (read as a local-to-host transfer of the destination: its source half made the destination's)
    uint64_t as_source = (bitbltbuf & ~uint64_t(0xFFFFFFFF)) | (bitbltbuf >> 32);
    uint64_t pos = (trxpos & ~uint64_t(0xFFFFFFFF)) | (trxpos >> 32);
    gsn_vram_read_image(gs->vram.data(), as_source, pos, trxreg, bytes.data(), qwords);
    sa_uploaded(bitbltbuf, trxpos, trxreg, bytes.data(), bytes.size());
}

// (begin_transfer, an upload: in gsn.cpp's uploads_now when it records one) and (vsync_now) the frame's
void sa_upload_begins(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, bool recorded) {
    if (!sa.on) {
        return;
    }
    sa.recording = -1;
    if (recorded) {
        sa.uploads_now.push_back({bitbltbuf, trxpos, trxreg, 0});
        sa.recording = int(sa.uploads_now.size() - 1);
    }
}
uint64_t sa_vblanks = 0;

// --stats per map: what the counts grew by since the last vblank goes to the map the game was in (into
// per_map; or, for --stats at the end, into a copy of it, the GS's thread perhaps still going)
void sa_stats_per_map(std::vector<std::pair<int, SaStats>> &per_map, SaStats &last) {
    const uint64_t *now = reinterpret_cast<const uint64_t *>(&sa_stats);
    uint64_t *was = reinterpret_cast<uint64_t *>(&last);
    int map = sat.map_now;
    auto slot = std::find_if(per_map.begin(), per_map.end(), [map](const std::pair<int, SaStats> &m) { return m.first == map; });
    if (slot == per_map.end()) {
        per_map.push_back({map, SaStats{}});
        slot = per_map.end() - 1;
    }
    uint64_t *into = reinterpret_cast<uint64_t *>(&slot->second);
    for (size_t i = 0; i < sizeof(SaStats) / sizeof(uint64_t); i++) {
        uint64_t n = now[i];
        into[i] += n - was[i];
        was[i] = n;
    }
}
static_assert(sizeof(SaStats) % sizeof(uint64_t) == 0, "SaStats: counts only");

// (gsn_set_scene_map, the game's thread) The map the game is in, taken at the next vblank
void sa_set_map(int map) {
    sat.map.store(map, std::memory_order_relaxed);
}

void sa_vblank(bool extra) {
    if (!sa.on) {
        return;
    }
    if (!extra) {
        sa.frame++;
        sa_vblanks++; // (the composite's: gsn_composite.inc)
        {
            std::lock_guard<std::mutex> hold(sat.per_map_lock);
            sa_stats_per_map(sat.per_map, sat.at_vblank);
            sat.map_now = sat.map.load(std::memory_order_relaxed);
        }
        if (!sa.uploads_now.empty()) {
            sa.uploads_frame.swap(sa.uploads_now);
            sa.uploads_now.clear();
            sa.recording = -1;
        }
    }
}
// (replay_upload) A picture between asks for the frame's k-th upload again: its blocks its own again
void sa_upload_replayed(uint32_t k) {
    if (!sa.on || k >= sa.uploads_frame.size()) {
        return;
    }
    const Sa::Recorded &r = sa.uploads_frame[k];
    SaChunk *c = r.id ? sa_chunk(r.id) : nullptr;
    if (c && c->bitbltbuf == r.bitbltbuf && c->trxpos == r.trxpos && c->trxreg == r.trxreg) {
        sa_own(r.id, *c);
        sa_stats.replayed_uploads++;
    } else {
        // (not known: its blocks the GS's)
        uint32_t lo, count;
        buffer_pages(bits(r.bitbltbuf, 32, 14), bits(r.bitbltbuf, 48, 6), bits(r.bitbltbuf, 56, 6),
                     bits(r.trxpos, 32, 11) + bits(r.trxreg, 0, 12), bits(r.trxpos, 48, 11) + bits(r.trxreg, 32, 12), &lo,
                     &count);
        sa_pages_drawn(lo, count, GSN_PSMCT32);
    }
}

// The uploads a buffer's rectangle comes from, or why not all of it does (SA_TEX_*): every nibble of it
// an upload's, and that upload's page not drawn into or copied over since in that nibble
// A rectangle's owners as sa_owners_walk found them (before the evictions), kept while the table is as it was
// (sat.gen): most batches of a frame ask for textures whose blocks no upload or draw changed since
struct SaOwnersMemo {
    uint64_t gen = 0;
    uint32_t bp, bw, psm, x0, y0, w, h;
    int why;
    std::vector<uint32_t> ids; // (the ids met, in the order met)
};
SaOwnersMemo sa_owners_memo[1024];
uint64_t sa_owners_hits = 0, sa_owners_walks = 0;

int sa_owners_walk(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                   std::vector<uint32_t> &ids) {
    int why = SA_TEX_TABLE;
    sa_blocks_of(bp, bw, psm, x0, y0, w, h, [&](uint32_t block, const SaCover &need) {
        if (why != SA_TEX_TABLE) {
            return;
        }
        uint32_t page = block / 32;
        SaCover covered = {};
        for (const SaOwner &o : sat.blocks[block].owners) {
            SaCover rel = sa_and(o.cover, need);
            if (!rel.any()) {
                continue;
            }
            uint64_t parts = rel.folded();
            if (!o.id || ((parts & SA_LO) && sat.page_drawn_lo[page] > o.seq) ||
                ((parts & SA_HI) && sat.page_drawn_hi[page] > o.seq)) {
                why = SA_TEX_DRAWN;
                return;
            }
            if (std::find(ids.begin(), ids.end(), o.id) == ids.end()) {
                ids.push_back(o.id);
            }
            for (int k = 0; k < 8; k++) {
                covered.q[k] |= rel.q[k];
            }
        }
        if (!sa_same(covered, need)) {
            uint64_t parts = need.folded();
            bool drawn = ((parts & SA_LO) && sat.page_drawn_lo[page]) || ((parts & SA_HI) && sat.page_drawn_hi[page]);
            why = drawn ? SA_TEX_DRAWN : SA_TEX_UNOWNED;
        }
    });
    return why;
}

int sa_owners(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
              std::vector<uint32_t> &ids) {
    if (!gsn_psm_valid(psm) || w == 0 || h == 0) {
        return SA_TEX_UNOWNED; // (no format the GS has: what VRAM gives, the GS's)
    }
    uint64_t hk = (uint64_t(bp) * 0x9E3779B1u) ^ (uint64_t(bw) << 7) ^ (uint64_t(psm) << 13) ^ (uint64_t(x0) << 19) ^
                  (uint64_t(y0) << 29) ^ (uint64_t(w) << 37) ^ (uint64_t(h) << 49);
    hk ^= hk >> 23;
    SaOwnersMemo &m = sa_owners_memo[(hk * 0x9E3779B97F4A7C15ull) >> 54];
    int why;
    if (m.gen == sat.gen && m.bp == bp && m.bw == bw && m.psm == psm && m.x0 == x0 && m.y0 == y0 && m.w == w && m.h == h) {
        sa_owners_hits++;
    } else {
        sa_owners_walks++;
        m.ids.clear();
        m.why = sa_owners_walk(bp, bw, psm, x0, y0, w, h, m.ids);
        m.gen = sat.gen;
        m.bp = bp, m.bw = bw, m.psm = psm, m.x0 = x0, m.y0 = y0, m.w = w, m.h = h;
    }
    why = m.why;
    // (as the walk would have added them to ids: those not in it already, in the order met)
    size_t had = ids.size();
    for (uint32_t id : m.ids) {
        if (std::find(ids.begin(), ids.begin() + had, id) == ids.begin() + had) {
            ids.push_back(id);
        }
    }
    for (uint32_t id : ids) {
        if (why == SA_TEX_TABLE && !sa_chunk(id)) {
            why = SA_TEX_EVICTED;
        }
    }
    return why;
}

// The chunks of ids written into the scratch VRAM, oldest first (the order they were last uploaded in)
void sa_scratch_write(std::vector<uint32_t> &ids) {
    std::sort(ids.begin(), ids.end(), [](uint32_t a, uint32_t b) { return sa_chunk(a)->seq < sa_chunk(b)->seq; });
    if (sa.scratch.empty()) {
        sa.scratch.assign(GSN_VRAM_BYTES, 0);
    }
    for (uint32_t id : ids) {
        const SaChunk &c = *sa_chunk(id);
        const std::vector<uint8_t> &bytes = c.data();
        GsnUpload u;
        gsn_upload_begin(&u, c.bitbltbuf, c.trxpos, c.trxreg);
        gsn_upload_data(&u, sa.scratch.data(), bytes.data(), bytes.size(), nullptr, nullptr);
        sa_pack_dump(c);
    }
}

// ---- The CLUT from the table -------------------------------------------------------------------------
// The GS's CLUT buffer (gsn.cpp's clut_load) mirrored: each load TEX0's CLD asks for (CLD 0-5, the 8-bit
// and 4-bit formats, CSM1 and CSM2, CSA) read from the uploads that own the CLUT's nibbles, written into
// the scratch VRAM as a texture is; or from the GS's VRAM when not all of them are the uploads'
// (those halfwords marked, clut_gs: a batch that reads one takes the GS's buffer whole).
// The entries a load reads from a VRAM (clut_load's addressing), as the CLUT's format has them
void sa_clut_read(const uint8_t *vram, uint64_t tex0, uint32_t *values) {
    clut_read_entries(vram, tex0, gs->reg[R_TEXCLUT], values);
}

// (gs_write, TEX0: after clut_load and the GS's flush) A CLUT load, mirrored
// (sa_clut_load) The last loads from the table, while it is unchanged: a skinned model's parts load two CLUTs in turn
// (the first pass's, the shine's), each one a load
struct SaClutMemo {
    uint64_t gen = 0, key = 0, texclut = 0;
    std::vector<uint32_t> ids;
    bool packed = false;
    uint32_t values[256];
};
SaClutMemo sa_clut_memo[4];
uint32_t sa_clut_memo_next = 0;

void sa_clut_load(uint64_t tex0) {
    if (!sa.on) {
        return;
    }
    uint32_t psm = bits(tex0, 20, 6), cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1);
    uint32_t csa = bits(tex0, 56, 5), cld = bits(tex0, 61, 3);
    if (!psm_is_palette(psm)) {
        return;
    }
    switch (cld) {
    case 1:
        break;
    case 2:
    case 3:
        sat.clut_cbp[cld - 2] = cbp;
        break;
    case 4:
    case 5:
        if (sat.clut_cbp[cld - 4] == cbp) {
            return;
        }
        sat.clut_cbp[cld - 4] = cbp;
        break;
    default:
        return;
    }
    bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
    uint32_t entries = eight ? 256 : 16;
    uint32_t gpsm = cpsm == 0 ? GSN_PSMCT32 : cpsm == 2 ? GSN_PSMCT16 : GSN_PSMCT16S;
    uint64_t texclut = gs->reg[R_TEXCLUT];
    uint32_t bw = csm == 0 ? 1 : bits(texclut, 0, 6);
    uint32_t x0 = csm == 0 ? 0 : bits(texclut, 6, 6) * 16, y0 = csm == 0 ? 0 : bits(texclut, 12, 10);
    uint32_t w = csm == 0 ? (eight ? 16 : 8) : entries, h = csm == 0 ? (eight ? 16 : 2) : 1;
    // (pf/skin) the last loads from the table (SaClutMemo)
    SaClutMemo *memo = sa_clut_memo;
    uint32_t &memo_next = sa_clut_memo_next;
    uint64_t memo_key = tex0 & ((uint64_t(0x3F) << 20) | (uint64_t(0x3FFF) << 37) | (uint64_t(0xF) << 51) | (uint64_t(1) << 55));
    uint64_t memo_texclut = csm ? texclut : 0;
    SaClutMemo *hit = nullptr;
    for (int k = 0; k < 4; k++) {
        SaClutMemo &m = memo[k];
        if (m.gen == sat.gen && m.key == memo_key && m.texclut == memo_texclut) {
            hit = &m;
        }
    }
    std::vector<uint32_t> ids;
    int why = SA_TEX_TABLE;
    if (hit) {
        for (uint32_t id : hit->ids) {
            if (!sa_chunk(id)) {
                hit->gen = 0; // (one let go since: worked out again)
                hit = nullptr;
                break;
            }
        }
    }
    if (!hit) {
        why = sa_owners(cbp, bw, gpsm, x0, y0, w, h, ids);
    }
    sa_stats.clut_loads[why]++;
    uint32_t values[256];
    bool packed = false;
    if (hit) {
        for (uint32_t id : hit->ids) {
            sa_chunk(id)->used = sa.frame;
        }
        packed = hit->packed;
        memcpy(values, hit->values, sizeof(values));
    } else if (why == SA_TEX_TABLE) {
        std::sort(ids.begin(), ids.end());
        uint64_t key = (tex0 & ((uint64_t(0x3FFF) << 37) | (uint64_t(0xF) << 51) | (uint64_t(1) << 55))) ^
                       (uint64_t(eight) << 63) ^ (csm ? texclut * 0x9E3779B97F4A7C15ull : 0);
        for (uint32_t id : ids) {
            key = (key ^ id) * 0x100000001B3ull;
            key ^= key >> 29;
            SaChunk *c = sa_chunk(id);
            c->used = sa.frame;
            packed |= c->pack != nullptr;
        }
        auto found = sat.clut_cache.find(key);
        if (found != sat.clut_cache.end()) {
            memcpy(values, found->second.data(), sizeof(values));
        } else {
            if (sa.scratch.empty()) {
                sa.scratch.assign(GSN_VRAM_BYTES, 0);
            }
            sa_blocks_of(cbp, bw, gpsm, x0, y0, w, h, [&](uint32_t block, const SaCover &) {
                memset(sa.scratch.data() + size_t(block) * 256, 0, 256);
            });
            sa_scratch_write(ids);
            sa_clut_read(sa.scratch.data(), tex0, values);
            if (sat.clut_cache.size() > 4096) {
                sat.clut_cache.clear();
            }
            std::array<uint32_t, 256> &slot = sat.clut_cache[key];
            memcpy(slot.data(), values, sizeof(values));
            sa_stats.clut_decodes++;
        }
        SaClutMemo &m = memo[memo_next++ % 4];
        m.gen = sat.gen;
        m.key = memo_key;
        m.texclut = memo_texclut;
        m.ids = ids;
        m.packed = packed;
        memcpy(m.values, values, sizeof(values));
    } else {
        sa_clut_read(gs->vram.data(), tex0, values); // (as clut_load read it: its pages claimed back to VRAM)
    }
    if (eight && cpsm == 0) {
        // (the whole buffer: halfword i the low half of entry i, 256 + i its high half; as below, at once)
        uint64_t gs_bits = why == SA_TEX_TABLE ? 0 : ~uint64_t(0), packed_bits = packed ? ~uint64_t(0) : 0;
        bool changed = false;
        for (uint32_t k = 0; k < 8; k++) {
            changed |= sat.clut_packed[k] != packed_bits;
        }
        for (uint32_t i = 0; i < 256 && !changed; i++) {
            changed = sat.clut[i] != (values[i] & 0xFFFF) || sat.clut[256 + i] != (values[i] >> 16);
        }
        if (changed && !gs->verts.empty() && (sa.in_draw ? sa.draw_on : sa.pending)) {
            flush_batch();
        }
        for (uint32_t i = 0; i < 256; i++) {
            sat.clut[i] = uint16_t(values[i]);
            sat.clut[256 + i] = uint16_t(values[i] >> 16);
        }
        for (uint32_t k = 0; k < 8; k++) {
            sat.clut_gs[k] = gs_bits;
            sat.clut_packed[k] = packed_bits;
        }
        return;
    }
    // (halfword, value) pairs; a scene batch waiting keeps the colours it was made with (the GS flushed before its
    // own load if its buffer changed; the mirror's colours can change alone where a texture pack's upload is in it)
    uint32_t at[512], value[512], n = 0;
    for (uint32_t i = 0; i < entries; i++) {
        if (cpsm == 0) {
            at[n] = eight ? i : (csa * 16 + i) & 0xFF;
            value[n++] = values[i] & 0xFFFF;
            at[n] = at[n - 1] + 256;
            value[n++] = values[i] >> 16;
        } else {
            at[n] = eight ? i : (csa * 16 + i) & 0x1FF;
            value[n++] = values[i] & 0xFFFF;
        }
    }
    bool changed = false;
    for (uint32_t k = 0; k < n && !changed; k++) {
        uint64_t bit = uint64_t(1) << (at[k] % 64);
        changed = sat.clut[at[k]] != value[k] || ((sat.clut_packed[at[k] / 64] & bit) != 0) != packed;
    }
    if (changed && !gs->verts.empty() && (sa.in_draw ? sa.draw_on : sa.pending)) {
        flush_batch();
    }
    for (uint32_t k = 0; k < n; k++) {
        uint64_t bit = uint64_t(1) << (at[k] % 64);
        sat.clut[at[k]] = uint16_t(value[k]);
        sat.clut_gs[at[k] / 64] = why == SA_TEX_TABLE ? sat.clut_gs[at[k] / 64] & ~bit : sat.clut_gs[at[k] / 64] | bit;
        sat.clut_packed[at[k] / 64] = packed ? sat.clut_packed[at[k] / 64] | bit : sat.clut_packed[at[k] / 64] & ~bit;
    }
}

// A batch's 256 colours (clut_colours' arithmetic) from the mirror; false (and the GS's buffer's colours)
// when a halfword it reads is not the table's. *packed: one came from a texture pack.
bool sa_clut_colours(uint64_t tex0, uint32_t *out, bool *packed) {
    uint32_t psm = bits(tex0, 20, 6), cpsm = bits(tex0, 51, 4), csa = bits(tex0, 56, 5);
    bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
    uint32_t count = eight ? 256 : 16;
    bool gs_owned = false;
    *packed = false;
    auto read = [&](uint32_t at) {
        uint64_t bit = uint64_t(1) << (at % 64);
        gs_owned |= (sat.clut_gs[at / 64] & bit) != 0;
        *packed |= (sat.clut_packed[at / 64] & bit) != 0;
        return sat.clut[at];
    };
    uint64_t texa = gs->reg[R_TEXA];
    for (uint32_t i = 0; i < count; i++) {
        if (cpsm == 0) {
            uint32_t at = eight ? i : ((csa & 15) * 16 + i) & 0xFF;
            out[i] = uint32_t(read(at)) | (uint32_t(read(256 + at)) << 16);
        } else {
            uint32_t at = eight ? i : (csa * 16 + i) & 0x1FF;
            uint32_t v = read(at);
            uint32_t r = (v & 0x1F) << 3, g = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3;
            uint32_t a = (v & 0x8000) ? bits(texa, 32, 8)
                         : (bits(texa, 15, 1) && (v & 0x7FFF) == 0) ? 0
                                                                    : bits(texa, 0, 8);
            out[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }
    for (uint32_t i = count; i < 256; i++) {
        out[i] = 0;
    }
    if (gs_owned) {
        clut_colours(tex0, out);
        *packed = false;
        return false;
    }
    return true;
}

// ---- Textures from their source ------------------------------------------------------------------
struct SaTexture {
    const ImageView *raw_view = nullptr, *target_view = nullptr;
    uint32_t src = SRC_CT32;
    int32_t src_offset[2] = {0, 0};
    uint32_t tscale = 0;
    uint32_t clut[256];
    bool want_clut = false;
    uint32_t tex_alpha_max = 255, tex_lo = 0, tex_hi = 255;
    bool cut_texels = false;
    uint32_t cut_lo = 255, cut_hi = 0;
    uint64_t cut_clear = 0, cut_all = 0;
    const uint32_t *texels = nullptr; // (the texture's texels by alpha, CLUT index or CT32's alpha, where known)
    bool reads_target = false, reads_own = false;
    bool own_named = false; // (reads_own: the named target bound, T3)
    ImageHandle table;      // (decoded from the table: its image, kept by a skinned group until it is drawn)
    const Sa::Decoded *decoded = nullptr; // (from the table: what gsn_rgba.inc makes its RGBA8 copy from)
};

// A texture (TEX0's, its mipmap levels', the part of level 0 the draws read) decoded from the uploads that
// own all of its blocks: written into the scratch VRAM, oldest first, over blocks made zero, and read back
// as decode_texture reads VRAM. Found again by what it is and the uploads it came from. nullptr when not
// all of it is the uploads' (why says why).
const Sa::Decoded *sa_table_texture(uint32_t tbw_psm_tw_th[4], uint32_t tbp, uint32_t levels, const uint32_t *mip_tbp,
                                     const uint32_t *mip_tbw, uint32_t cols, uint32_t rows, int *why) {
    uint32_t tbw = tbw_psm_tw_th[0], psm = tbw_psm_tw_th[1], tw = tbw_psm_tw_th[2], th = tbw_psm_tw_th[3];
    std::vector<uint32_t> ids;
    for (uint32_t l = 0; l < levels; l++) {
        *why = sa_owners(mip_tbp[l], mip_tbw[l], psm, 0, 0, std::max(1u, cols >> l), std::max(1u, rows >> l), ids);
        if (*why != SA_TEX_TABLE) {
            return nullptr;
        }
    }
    // (oldest first: the order they were uploaded in, last time each)
    std::sort(ids.begin(), ids.end(), [](uint32_t a, uint32_t b) { return sa_chunk(a)->seq < sa_chunk(b)->seq; });
    uint64_t key = texture_key(tbp, tbw, psm, tw, th, levels, mip_tbp, mip_tbw, cols, rows);
    for (uint32_t id : ids) {
        key = (key ^ id) * 0x100000001B3ull;
        key ^= key >> 29;
        sa_chunk(id)->used = sa.frame;
    }
    auto found = sa.decoded.find(key);
    if (found != sa.decoded.end()) {
        sa_stats.decode_hits++;
        return &found->second;
    }
    Timer timer(gsn_stats.ns_decode);
    if (sa.scratch.empty()) {
        sa.scratch.assign(GSN_VRAM_BYTES, 0);
    }
    uint8_t *vram = sa.scratch.data();
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t w = std::max(1u, (1u << tw) >> l), h = std::max(1u, (1u << th) >> l);
        sa_blocks_of(mip_tbp[l], mip_tbw[l], psm, 0, 0, w, h, [&](uint32_t block, const SaCover &) {
            memset(vram + size_t(block) * 256, 0, 256);
        });
    }
    sa_scratch_write(ids);
    VkFormat format = raw_format(psm);
    Sa::Decoded d;
    d.image = make_image(1u << tw, 1u << th, format, levels, false);
    d.texels.fill(0);
    d.serial = ++sa.decode_serial;
    d.width = 1u << tw;
    d.height = 1u << th;
    d.levels = levels;
    {
        Why why_(GSN_WHY_DECODE);
        end_pass();
    }
    transfer_barrier();
    std::vector<uint32_t> row;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t w = std::max(1u, (1u << tw) >> l), h = std::max(1u, (1u << th) >> l);
        void *out = cmd().update_image(*d.image, {0, 0, 0}, {w, h, 1}, w, h, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1});
        row.resize(w);
        for (uint32_t y = 0; y < h; y++) {
            size_t at = size_t(y) * w;
            gsn_vram_read_row(vram, psm, mip_tbp[l], mip_tbw[l], 0, y, w, row.data());
            if (hw_sampler) {
                d.raw.insert(d.raw.end(), row.begin(), row.end()); // (gsn_rgba.inc)
            }
            for (uint32_t x = 0; x < w; x++) {
                if (format == VK_FORMAT_R32_UINT) {
                    static_cast<uint32_t *>(out)[at + x] = row[x];
                    d.texels[row[x] >> 24]++;
                } else if (format == VK_FORMAT_R8_UINT) {
                    static_cast<uint8_t *>(out)[at + x] = uint8_t(row[x]);
                    d.texels[row[x] & 0xFF]++;
                } else {
                    static_cast<uint16_t *>(out)[at + x] = uint16_t(row[x]);
                }
            }
        }
        gsn_stats.texture_bytes += size_t(w) * h;
    }
    transfer_barrier();
    sa_stats.decodes++;
    sa.decoded_bytes += image_bytes(*d.image);
    sa.decoded_order.push_back(key);
    auto &slot = sa.decoded[key] = std::move(d);
    // the oldest let go beyond 2,048 of them or 256 MB (one in use keeps its image all the same)
    while (sa.decoded_order.size() > 2048 || sa.decoded_bytes > (256u << 20)) {
        auto oldest = sa.decoded.find(sa.decoded_order.front());
        if (oldest != sa.decoded.end() && &oldest->second != &slot) {
            sa.decoded_bytes -= image_bytes(*oldest->second.image);
            sa.decoded.erase(oldest);
        }
        sa.decoded_order.pop_front();
    }
    return &slot;
}

// (KF4_SA_VERIFY=1: flush_batch for the GS's batches, sa_texture for the scene's) A texture as the table
// has it against the GS's VRAM (its pages claimed back), every row of every level, and the CLUT the table's
// mirror gives against the GS's buffer; counted (sa_stats.verify_*), and with KF4_SA_VERIFY=2 each that
// differs logged. A chunk from a texture pack is not compared.
void sa_verify(uint64_t tex0, uint32_t levels, const uint32_t *mip_tbp, const uint32_t *mip_tbw, uint32_t cols,
               uint32_t rows) {
    if (!sa.on || !sat.verify) {
        return;
    }
    uint32_t psm = bits(tex0, 20, 6);
    std::vector<uint32_t> ids;
    int why = SA_TEX_TABLE;
    for (uint32_t l = 0; l < levels && why == SA_TEX_TABLE; l++) {
        why = sa_owners(mip_tbp[l], mip_tbw[l], psm, 0, 0, std::max(1u, cols >> l), std::max(1u, rows >> l), ids);
    }
    bool packed = false;
    for (uint32_t id : ids) {
        packed |= why == SA_TEX_TABLE && sa_chunk(id)->pack != nullptr;
    }
    if (why != SA_TEX_TABLE) {
        sa_stats.verify_not[why]++;
        if (sat.verify > 2) {
            fprintf(stderr, "[standalone] verify: map %d tex0 %016llx psm %02x tbp %x: %s\n", sat.map_now,
                    (unsigned long long)tex0, psm, mip_tbp[0], sa_tex_names[why]);
        }
    } else if (!packed) {
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t lo, count;
            buffer_pages(mip_tbp[l], mip_tbw[l], psm, std::max(1u, cols >> l), std::max(1u, rows >> l), &lo, &count);
            claim_pages(lo, count, nullptr, false);
        }
        if (sa.scratch.empty()) {
            sa.scratch.assign(GSN_VRAM_BYTES, 0);
        }
        for (uint32_t l = 0; l < levels; l++) {
            sa_blocks_of(mip_tbp[l], mip_tbw[l], psm, 0, 0, std::max(1u, cols >> l), std::max(1u, rows >> l),
                         [&](uint32_t block, const SaCover &) { memset(sa.scratch.data() + size_t(block) * 256, 0, 256); });
        }
        sa_scratch_write(ids);
        bool same = true;
        std::vector<uint32_t> a, b;
        for (uint32_t l = 0; l < levels && same; l++) {
            uint32_t w = std::max(1u, cols >> l), h = std::max(1u, rows >> l);
            a.resize(w);
            b.resize(w);
            for (uint32_t y = 0; y < h && same; y++) {
                gsn_vram_read_row(sa.scratch.data(), psm, mip_tbp[l], mip_tbw[l], 0, y, w, a.data());
                gsn_vram_read_row(gs->vram.data(), psm, mip_tbp[l], mip_tbw[l], 0, y, w, b.data());
                if (a != b) {
                    same = false;
                    if (sat.verify > 1) {
                        uint32_t x = 0;
                        while (a[x] == b[x]) {
                            x++;
                        }
                        fprintf(stderr, "[standalone] verify: map %d tex0 %016llx psm %02x tbp %x level %u: (%u, %u) table %x, VRAM %x (%zu uploads)\n",
                                sat.map_now, (unsigned long long)tex0, psm, mip_tbp[l], l, x, y, a[x], b[x], ids.size());
                    }
                }
            }
        }
        (same ? sa_stats.verify_same : sa_stats.verify_differ)++;
    }
    if (psm_is_palette(psm)) {
        uint32_t mine[256], theirs[256];
        bool clut_packed = false;
        if (sa_clut_colours(tex0, mine, &clut_packed) && !clut_packed) {
            clut_colours(tex0, theirs);
            bool same = memcmp(mine, theirs, sizeof(mine)) == 0;
            (same ? sa_stats.verify_clut_same : sa_stats.verify_clut_differ)++;
            if (!same && sat.verify > 1) {
                fprintf(stderr, "[standalone] verify: map %d tex0 %016llx: the CLUT differs\n", sat.map_now,
                        (unsigned long long)tex0);
            }
        } else if (sat.verify > 2) {
            fprintf(stderr, "[standalone] verify: map %d tex0 %016llx: the CLUT not the table's\n", sat.map_now,
                    (unsigned long long)tex0);
        }
    }
    if (sat.verify > 3 && sat.verified_seen.emplace(tex0 & ~(uint64_t(7) << 61), true).second) {
        // (each TEX0 once, CLD aside: what was verified)
        fprintf(stderr, "[standalone] verified: map %d tex0 %016llx psm %02x tbp %x cbp %x cpsm %u csm %u csa %u: %s\n",
                sat.map_now, (unsigned long long)tex0, psm, bits(tex0, 0, 14), bits(tex0, 37, 14), bits(tex0, 51, 4),
                bits(tex0, 55, 1), bits(tex0, 56, 5), sa_tex_names[why]);
    }
}

// The batch's texture (flush_batch's, "the texture"): from the table where all of it is the uploads';
// else the GS's (a render target, or VRAM)
void sa_texture(SaTexture &st, Push &push, uint32_t ctxt, uint64_t tex0, uint64_t tex1, uint64_t clamp, bool fst,
                const Target *color) {
    uint32_t tbp = bits(tex0, 0, 14), tbw = bits(tex0, 14, 6), tpsm = bits(tex0, 20, 6);
    uint32_t tw = std::min(bits(tex0, 26, 4), 10u), th = std::min(bits(tex0, 30, 4), 10u);
    push.flags |= F_TME | (fst ? F_FST : 0) | (bits(tex0, 35, 2) << F_TFX_SHIFT) | (bits(tex0, 34, 1) ? F_TCC : 0);
    push.tex_size = (1u << tw) | ((1u << th) << 16);
    push.clamp_lo = uint32_t(clamp & 0xFFFFFF);
    push.clamp_hi = bits(clamp, 24, 20);
    push.tex1 = bits(tex1, 5, 1) | (bits(tex1, 6, 3) << 1) | (bits(tex1, 2, 3) << 4) | (bits(tex1, 0, 1) << 7) |
                (bits(tex1, 19, 2) << 8);
    int32_t k = int32_t(bits(tex1, 32, 12) << 20) >> 20;
    push.lod_k = float(k) / 16.0f;
    uint32_t finer = std::min(sa.sx, sa.sy); // (the scene's scale: T2)
    int mode = lod_mode;
    push.lod_bias = mode == GSN_LOD_FULL ? -64.0f : mode == GSN_LOD_SCALE ? -log2f(float(finer)) : 0.0f;
    push.misc |= uint32_t(aniso_log2) << M_ANISO_SHIFT;
    st.raw_view = &gs->dummy_raw->get_view();
    st.target_view = &gs->dummy_rgba->get_view();

    // the rows and columns the batch reads (flush_batch's rows_read, cols_read; in a picture between all)
    auto read_extent = [&](bool across) -> uint32_t {
        uint32_t n = across ? 1u << tw : 1u << th;
        if (interp.active) {
            return n;
        }
        float lo = 0.0f, hi = 0.0f;
        for (const Vtx &v : gs->verts) {
            float c = across ? (fst ? v.s : v.q > 0.0f ? v.s / v.q * float(n) : -1.0f)
                             : (fst ? v.t : v.q > 0.0f ? v.t / v.q * float(n) : -1.0f);
            lo = std::min(lo, c);
            hi = std::max(hi, c);
        }
        // (a V less than a row above the top reads no row above its first, as flush_batch's rows_read: the frame
        // before blended in, Game_BlendPreviousFrame, has its sprite's corners half a texel out; taken as reading
        // the whole 1024 x 512, it missed the shown frame's target and decoded it from VRAM, written back first,
        // every frame: the knock-back of every hit at 12 game frames a second at 240)
        return (across ? lo >= 0.0f : lo > -1.0f) && hi < float(n) ? std::max(1u, uint32_t(ceilf(hi))) : n;
    };
    uint32_t levels = 1;
    uint32_t mip_tbp[7] = {tbp}, mip_tbw[7] = {tbw};
    uint32_t mmin = bits(tex1, 6, 3), mxl = bits(tex1, 2, 3);
    if (mmin >= 2 && mmin <= 5 && mxl > 0) {
        levels = std::min(mxl, 6u) + 1;
        uint64_t m1 = gs->reg[R_MIPTBP1_1 + ctxt], m2 = gs->reg[R_MIPTBP2_1 + ctxt];
        for (uint32_t l = 1; l < levels; l++) {
            uint64_t m = l <= 3 ? m1 : m2;
            uint32_t at = ((l - 1) % 3) * 20;
            mip_tbp[l] = bits(m, at, 14);
            mip_tbw[l] = bits(m, at + 14, 6);
        }
        levels = std::min(levels, std::max(tw, th) + 1);
    }
    uint32_t cols = 1u << tw, rows = 1u << th;
    if (levels == 1 && bits(clamp, 0, 2) != 3) {
        uint32_t pw = page_width(tpsm);
        cols = std::min(cols, (read_extent(true) + pw - 1) / pw * pw);
    }
    if (levels == 1 && bits(clamp, 2, 2) != 3) {
        uint32_t ph = page_height(tpsm);
        rows = std::min(rows, (read_extent(false) + ph - 1) / ph * ph);
    }

    // (T2) the scene's copy of its frame there (GsDraw_CopyFrame's), at the scene's scale
    {
        const Sa::Copy &k = sa.fcopy;
        if (sa.log == 3 && tbp == k.dbp) {
            fprintf(stderr, "[sa] texture at the frame's copy: valid %d tbw %u psm %u levels %u rows %u cols %u\n", k.valid, tbw, tpsm,
                    levels, rows, cols);
        }
        if (k.valid && tbp == k.dbp && tbw == k.dbw && tpsm == k.dpsm && levels == 1 &&
            k.image && k.image->get_width() == sa.color->get_width() && k.image->get_height() == sa.color->get_height()) {
            st.src = tpsm == GSN_PSMCT24 ? SRC_TARGET_CT24 : SRC_TARGET_CT32;
            st.reads_target = true;
            st.src_offset[0] = int32_t(k.sx) - int32_t(k.dx);
            st.src_offset[1] = int32_t(k.sy) - int32_t(k.dy);
            st.tscale = k.scale_x | k.scale_y << 16;
            st.target_view = &k.image->get_view();
            sa_stats.frame_copy_reads++;
            push.flags |= st.src << F_SRC_SHIFT;
            push.src_offset[0] = st.src_offset[0];
            push.src_offset[1] = st.src_offset[1];
            push.tscale = st.tscale;
            return;
        }
    }
    // (T3) a named target's: from the scene's image
    if (sat_texture(st, tbp, tbw, tpsm, cols, rows, levels)) {
        if (st.reads_own) {
            st.own_named = true; // (the named target bound itself: declined, sa_draw_batch)
        } else {
            push.flags |= st.src << F_SRC_SHIFT;
            push.src_offset[0] = st.src_offset[0];
            push.src_offset[1] = st.src_offset[1];
            push.tscale = st.tscale;
        }
        return;
    }
    // from the table
    int why = SA_TEX_TARGET;
    uint32_t shape[4] = {tbw, tpsm, tw, th};
    static int no_table = -1;
    if (no_table < 0) {
        no_table = getenv("KF4_SA_NO_TABLE") && atoi(getenv("KF4_SA_NO_TABLE")) > 0; // (tests: the GS's textures)
    }
    const Sa::Decoded *dec = no_table ? nullptr : sa_table_texture(shape, tbp, levels, mip_tbp, mip_tbw, cols, rows, &why);
    const uint32_t *texels = nullptr;
    if (dec) {
        st.raw_view = &dec->image->get_view();
        st.table = dec->image;
        st.decoded = dec;
        texels = dec->texels.data();
    } else {
        // the GS's: a render target (flush_batch's search), else VRAM
        Target *source = nullptr;
        for (auto &t : gs->targets) {
            if (t->depth || tbw != t->bw || tbp < t->bp) {
                continue;
            }
            uint32_t blocks = tbp - t->bp;
            if (psm_layout(tpsm) != psm_layout(t->psm) || blocks % 32 != 0) {
                continue;
            }
            uint32_t page = blocks / 32, across = std::max(t->bw, 1u);
            uint32_t x0 = (page % across) * page_width(t->psm), y0 = (page / across) * page_height(t->psm);
            if (y0 >= t->height) {
                continue;
            }
            // (the frame the scene draws into is its own copy: a pass reading a row past it, the flicker filter's
            // half-row offset, still reads the scene's picture, not VRAM's stale one: T2)
            if (y0 + read_extent(false) > t->height && t.get() != color) {
                break;
            }
            source = t.get();
            st.src_offset[0] = int32_t(x0);
            st.src_offset[1] = int32_t(y0);
            // (the frame the scene draws into is read from a copy of the scene's colour, at its scale: T2)
            st.tscale = t.get() == color ? sa.sx | sa.sy << 16 : t->sx | t->sy << 16;
            break;
        }
        if (source) {
            why = SA_TEX_TARGET;
            st.src = tpsm == GSN_PSMCT24 ? SRC_TARGET_CT24 : psm_is_16(tpsm) ? SRC_TARGET_CT16 : SRC_TARGET_CT32;
            st.reads_target = true;
            if (source == color) {
                st.reads_own = true; // (sa_draw_batch binds a copy of the scene's colour)
                sa_stats.textures[why]++;
                push.flags |= st.src << F_SRC_SHIFT;
                push.src_offset[0] = st.src_offset[0];
                push.src_offset[1] = st.src_offset[1];
                push.tscale = st.tscale;
                return;
            }
            Why why_(GSN_WHY_TEXTURE);
            msaa_settle(*source);
            st.target_view = &source->image->get_view();
        } else {
            for (uint32_t l = 0; l < levels; l++) {
                uint32_t lo, count;
                buffer_pages(mip_tbp[l], mip_tbw[l], tpsm, std::max(1u, cols >> l), std::max(1u, rows >> l), &lo, &count);
                claim_texture(mip_tbp[l], mip_tbw[l], tpsm, std::max(1u, (1u << tw) >> l), std::max(1u, (1u << th) >> l), lo,
                              count);
            }
            Texture *tex = get_texture(tbp, tbw, tpsm, tw, th, levels, mip_tbp, mip_tbw, cols, rows);
            st.raw_view = &tex->image->get_view();
            texels = tex->texels;
        }
        sa_stats.textures[why]++;
    }
    if (dec) {
        sa_stats.textures[SA_TEX_TABLE]++;
    }
    if (sat.verify && !st.reads_target) {
        sa_verify(tex0, levels, mip_tbp, mip_tbw, cols, rows);
    }
    if (!st.reads_target) {
        st.src = psm_is_palette(tpsm) ? SRC_CLUT : tpsm == GSN_PSMCT24 ? SRC_CT24 : psm_is_16(tpsm) ? SRC_CT16 : SRC_CT32;
        // (the CLUT from the table's mirror of the GS's buffer, where all it reads is the uploads')
        if (st.src == SRC_CLUT) {
            bool packed = false;
            if (sa_clut_colours(tex0, st.clut, &packed)) {
                sa_stats.clut_table++;
            } else {
                sa_stats.clut_gs++;
            }
            st.want_clut = true;
            st.tex_alpha_max = 0;
            for (uint32_t i = 0; i < 256; i++) {
                st.tex_alpha_max = std::max(st.tex_alpha_max, st.clut[i] >> 24);
            }
        }
        if (texels && (st.src == SRC_CLUT || st.src == SRC_CT32)) {
            st.texels = texels;
            st.tex_lo = 255;
            st.tex_hi = 0;
            for (uint32_t i = 0; i < 256; i++) {
                if (texels[i]) {
                    uint32_t a = st.src == SRC_CLUT ? st.clut[i] >> 24 : i;
                    st.tex_lo = std::min(st.tex_lo, a);
                    st.tex_hi = std::max(st.tex_hi, a);
                }
            }
            if (cutout_aa_mode != GSN_CUTOUT_AA_OFF && sa.samples > 1) {
                st.cut_texels = true;
                st.cut_lo = st.tex_lo;
                st.cut_hi = st.tex_hi;
                for (uint32_t i = 0; i < 256; i++) {
                    st.cut_all += texels[i];
                    st.cut_clear += (st.src == SRC_CLUT ? st.clut[i] >> 24 : i) == st.cut_lo ? texels[i] : 0;
                }
            }
        }
    }
    push.flags |= st.src << F_SRC_SHIFT;
    push.src_offset[0] = st.src_offset[0];
    push.src_offset[1] = st.src_offset[1];
    push.tscale = st.tscale;
}

// ---- The scene's targets, and its turns with the GS's ------------------------------------------------
ImageHandle sa_image(uint32_t width, uint32_t height, VkFormat format, uint32_t samples, VkImageUsageFlags more = 0) {
    bool depth = format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    ImageCreateInfo info = ImageCreateInfo::render_target(width, height, format);
    info.samples = VkSampleCountFlagBits(samples);
    info.usage = more | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT); // (T8: blending in the pass)
    info.layout = ImageLayout::General;
    info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
    return gs->device->create_image(info, nullptr);
}

void sa_programs() {
    if (sa.vs) {
        return;
    }
    ResourceLayout vert = {};
    vert.input_mask = (1u << 2) | (1u << 4);
    vert.push_constant_size = sizeof(Push);
    vert.sets[1].storage_buffer_mask = 1u;
    vert.sets[1].meta[0].array_size = 1;
    vert.sets[2].storage_buffer_mask = 3u;
    vert.sets[2].meta[0].array_size = 1;
    vert.sets[2].meta[1].array_size = 1;
    // (upscaling) With the picture's jitter, the shaders that add it (sa_*_jitter.vert); without, those that have no
    // code for it, so that their pipelines are those of the shipped list (gsn_pipeline_list.h) and of the caches
    if (sa_jitter_on()) {
        sa.vs = gs->device->request_shader(gsn_spv_sa_000_jitter_vert, sizeof(gsn_spv_sa_000_jitter_vert), &vert);
        sa.vs_280 = gs->device->request_shader(gsn_spv_sa_280_jitter_vert, sizeof(gsn_spv_sa_280_jitter_vert), &vert);
        sa.vs_700 = gs->device->request_shader(gsn_spv_sa_700_jitter_vert, sizeof(gsn_spv_sa_700_jitter_vert), &vert);
    } else {
        sa.vs = gs->device->request_shader(gsn_spv_sa_000_vert, sizeof(gsn_spv_sa_000_vert), &vert);
        sa.vs_280 = gs->device->request_shader(gsn_spv_sa_280_vert, sizeof(gsn_spv_sa_280_vert), &vert);
        sa.vs_700 = gs->device->request_shader(gsn_spv_sa_700_vert, sizeof(gsn_spv_sa_700_vert), &vert);
    }
    Shader *vss[3] = {sa.vs, sa.vs_280, sa.vs_700};
    for (int k = 0; k < 3; k++) {
        sa.programs[k][0] = gs->device->request_program(vss[k], gs->program->get_shader(ShaderStage::Fragment));
    }
    ResourceLayout frag = {};
    frag.output_mask = 1;
    frag.push_constant_size = sizeof(Push);
    frag.sets[0].sampled_image_mask = (1u << 0) | (1u << 1) | (1u << 3);
    frag.sets[0].storage_buffer_mask = 1u << 2; // (the CLUT: gs_frag.h)
    frag.sets[0].fp_mask = (1u << 1) | (1u << 3);
    for (int b : {0, 1, 2, 3}) {
        frag.sets[0].meta[b].array_size = 1;
    }
    for (int k = 0; k < 3; k++) {
        if (gs->program_in && gs->program_in_ms) {
            sa.programs_in[k][0] = gs->device->request_program(vss[k], gs->program_in->get_shader(ShaderStage::Fragment));
            sa.programs_in[k][1] = gs->device->request_program(vss[k], gs->program_in_ms->get_shader(ShaderStage::Fragment));
        }
    }
    // (--native-sampler hardware, gsn_rgba.inc) the same reading the batch's RGBA8 texture at binding 6; in the render
    // pass, the destination an input attachment as gsn.cpp's gs_in*.frag layouts have it (binding 5, a multisampled
    // target's 4, in place of binding 3)
    if (hw_sampler) {
        ResourceLayout hw = frag;
        hw.sets[0].sampled_image_mask |= 1u << 6;
        hw.sets[0].fp_mask |= 1u << 6;
        hw.sets[0].meta[6].array_size = 1;
        Shader *fs = gs->device->request_shader(gsn_spv_gs_hw_frag, sizeof(gsn_spv_gs_hw_frag), &hw);
        Shader *fs_sample = sample_shading ? gs->device->request_shader(gsn_spv_sa_sample_hw_frag, sizeof(gsn_spv_sa_sample_hw_frag), &hw) : nullptr;
        Shader *fs_in[2] = {};
        for (int ms = 0; ms < 2; ms++) {
            ResourceLayout in = hw;
            in.sets[0].sampled_image_mask &= ~(1u << 3);
            in.sets[0].fp_mask &= ~(1u << 3);
            uint32_t b = ms ? 4 : 5;
            in.sets[0].input_attachment_mask = 1u << b;
            in.sets[0].fp_mask |= 1u << b;
            in.sets[0].meta[b].array_size = 1;
            fs_in[ms] = ms ? gs->device->request_shader(gsn_spv_gs_in_ms_hw_frag, sizeof(gsn_spv_gs_in_ms_hw_frag), &in)
                           : gs->device->request_shader(gsn_spv_gs_in_hw_frag, sizeof(gsn_spv_gs_in_hw_frag), &in);
        }
        for (int k = 0; k < 3; k++) {
            sa.programs_hw[k][0] = gs->device->request_program(vss[k], fs);
            sa.programs_hw[k][1] = fs_sample ? gs->device->request_program(vss[k], fs_sample) : nullptr;
            for (int ms = 0; ms < 2; ms++) {
                sa.programs_in_hw[k][ms] = gs->device->request_program(vss[k], fs_in[ms]);
            }
        }
    }
    {
        ResourceLayout zf = {};
        Shader *fs = gs->device->request_shader(gsn_spv_sa_zfirst_frag, sizeof(gsn_spv_sa_zfirst_frag), &zf);
        for (int k = 0; k < 3; k++) {
            sa.programs_z[k] = gs->device->request_program(vss[k], fs);
        }
    }
    // (a run's copy by its own triangles: the target, an input attachment, into the scratch, colour attachment 1)
    if (gs->device->get_device_features().supports_conservative_rasterization) {
        for (int ms = 0; ms < 2; ms++) {
            ResourceLayout run_copy = {};
            run_copy.output_mask = 2;
            run_copy.push_constant_size = sizeof(Push);
            uint32_t b = ms ? 5 : 4;
            run_copy.sets[0].input_attachment_mask = 1u << b;
            run_copy.sets[0].fp_mask = 1u << b;
            run_copy.sets[0].meta[b].array_size = 1;
            Shader *fs = ms ? gs->device->request_shader(gsn_spv_sa_copy_run_ms_frag, sizeof(gsn_spv_sa_copy_run_ms_frag), &run_copy)
                            : gs->device->request_shader(gsn_spv_sa_copy_run_frag, sizeof(gsn_spv_sa_copy_run_frag), &run_copy);
            for (int k = 0; k < 3; k++) {
                sa.programs_copy[k][ms] = gs->device->request_program(vss[k], fs);
            }
        }
    }
    if (sample_shading) {
        Shader *fs = gs->device->request_shader(gsn_spv_sa_sample_frag, sizeof(gsn_spv_sa_sample_frag), &frag);
        for (int k = 0; k < 3; k++) {
            sa.programs[k][1] = gs->device->request_program(vss[k], fs);
        }
    }
    ResourceLayout copy_vert = {};
    ResourceLayout copy = {};
    copy.sets[0].sampled_image_mask = 1u;
    copy.sets[0].fp_mask = 1u;
    copy.sets[0].meta[0].array_size = 1;
    Shader *copy_vs = gs->device->request_shader(gsn_spv_msaa_vert, sizeof(gsn_spv_msaa_vert), &copy_vert);
    ResourceLayout colour = copy;
    colour.output_mask = 1;
    if (sample_shading) {
        sa.program_copy_ms = gs->device->request_program(
            copy_vs, gs->device->request_shader(gsn_spv_sa_copy_ms_frag, sizeof(gsn_spv_sa_copy_ms_frag), &colour));
    }
    ResourceLayout depth = copy;
    depth.push_constant_size = 2 * sizeof(float);
    sa.program_depth =
        gs->device->request_program(copy_vs, gs->device->request_shader(gsn_spv_sa_depth_frag, sizeof(gsn_spv_sa_depth_frag), &depth));
    if (sample_shading) {
        sa.program_depth_ms = gs->device->request_program(
            copy_vs, gs->device->request_shader(gsn_spv_sa_depth_ms_frag, sizeof(gsn_spv_sa_depth_ms_frag), &depth));
    }
    // (T2) the GS's picture and Z into a scene of another scale or samples, and back (sa_resample_pass)
    ResourceLayout resample = copy;
    resample.push_constant_size = 32;
    ResourceLayout resample_colour = resample;
    resample_colour.output_mask = 1;
    sa.program_resample[0][0] = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_sa_resample_frag, sizeof(gsn_spv_sa_resample_frag), &resample_colour));
    sa.program_resample[0][1] = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_sa_resample_ms_frag, sizeof(gsn_spv_sa_resample_ms_frag), &resample_colour));
    sa.program_resample[1][0] = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_sa_resample_depth_frag, sizeof(gsn_spv_sa_resample_depth_frag), &resample));
    sa.program_resample[1][1] = gs->device->request_program(
        copy_vs,
        gs->device->request_shader(gsn_spv_sa_resample_depth_ms_frag, sizeof(gsn_spv_sa_resample_depth_ms_frag), &resample));
}

// (flush_batch's) The frame and Z buffer a batch draws into, made as big as each other
bool sa_targets(uint32_t fbp, uint32_t fbw, uint32_t fpsm, uint32_t zbp, uint32_t zpsm, uint32_t need_h, Target **pc,
                Target **pz) {
    Target *color = get_target(fbp, fbw, fpsm, fbw * 64, need_h);
    Target *depth = nullptr;
    uint32_t w = color->width, h = color->height;
    for (int tries = 0; tries < 4; tries++) {
        depth = get_target(zbp, fbw, zpsm, w, h);
        w = std::max(w, depth->width);
        h = std::max(h, depth->height);
        color = get_target(fbp, fbw, fpsm, w, h);
        if (find_target(zbp, fbw, zpsm) == depth && color->width == depth->width && color->height == depth->height) {
            break;
        }
        depth = find_target(zbp, fbw, zpsm);
        if (!depth) {
            continue;
        }
        w = std::max(color->width, depth->width);
        h = std::max(color->height, depth->height);
    }
    if (find_target(zbp, fbw, zpsm) != depth || find_target(fbp, fbw, fpsm) != color || !depth ||
        color->image->get_width() != depth->image->get_width() || color->image->get_height() != depth->image->get_height()) {
        return false;
    }
    *pc = color;
    *pz = depth;
    return true;
}

// One of the copies (shaders/msaa.vert): src read at each pixel (or sample) of dst, over rows y0..y1 (clear:
// dst's depth and stencil cleared first, for the scene's depth taken)
void sa_copy_pass(const Image &dst, const Image &src, Program *program, bool depth, uint32_t y0, uint32_t y1,
                  const void *push, size_t push_size, bool clear = false) {
    end_pass();
    RenderPassInfo rp;
    RenderPassInfo::Subpass subpass;
    if (depth) {
        rp.depth_stencil = &dst.get_view();
        rp.op_flags = (clear ? RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT : RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT) |
                      RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
        rp.clear_depth_stencil = {0.0f, 0};
    } else {
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &dst.get_view();
        rp.load_attachments = 1;
        rp.store_attachments = 1;
    }
    if (dst.get_create_info().samples != VK_SAMPLE_COUNT_1_BIT) {
        msaa_subpass(rp, subpass);
    }
    rp.render_area = {{0, int32_t(y0)}, {dst.get_width(), y1 - y0}};
    Cmd &c = cmd();
    full_barrier();
    c.begin_render_pass(rp);
    c.set_opaque_state();
    c.set_program(program);
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_test(depth, depth);
    c.set_depth_compare(VK_COMPARE_OP_ALWAYS);
    VkViewport vp = {0.0f, 0.0f, float(dst.get_width()), float(dst.get_height()), 0.0f, 1.0f};
    c.set_viewport(vp);
    c.set_scissor(rp.render_area);
    c.set_texture(0, 0, src.get_view(), StockSampler::NearestClamp); // (a depth's: Granite samples its depth view)
    if (push) {
        c.push_constants(push, 0, push_size);
    }
    c.draw(3);
    c.end_render_pass();
    gsn_stats.passes++;
    barrier_fresh = false;
    full_barrier();
}

// (T2) src into dst's rows y0..y1 where the two differ in scale or samples (shaders/sa_resample.frag): rx, ry the
// source's pixels a destination pixel is; a depth taken under each pixel's middle (zscale: 0 as it is, else as
// sa_depth.frag rounds it; clear: dst's stencil cleared), a colour the mean of its pixels and samples
void sa_resample_pass(const Image &dst, const Image &src, bool depth, uint32_t y0, uint32_t y1, float rx, float ry, float zscale,
                      float zmax, bool clear = false) {
    struct {
        float ratio[2];
        int32_t last[2];
        int32_t samples;
        float zscale, zmax, pad;
    } push = {{rx, ry},
              {int32_t(src.get_width()) - 1, int32_t(src.get_height()) - 1},
              int32_t(src.get_create_info().samples),
              zscale,
              zmax,
              0.0f};
    bool ms = src.get_create_info().samples != VK_SAMPLE_COUNT_1_BIT;
    sa_copy_pass(dst, src, sa.program_resample[depth ? 1 : 0][ms ? 1 : 0], depth, y0, y1, &push, sizeof(push), clear);
    sa_stats.resampled++;
}

// (T2) The scene's scale and samples for a frame target: --scene-scale and --scene-msaa, or the GS's (the most
// samples the GPU has up to those wanted; more than one only with sample-rate shading, which the copies need)
void sa_shape(const Target &color, uint32_t *sx, uint32_t *sy, uint32_t *samples) {
    uint32_t w = sa_wanted_scale;
    *sx = (w & 0xFF) ? std::min(w & 0xFF, uint32_t(GSN_MAX_SCALE_X)) : color.sx;
    *sy = (w >> 8) ? std::min(w >> 8, 4u) : color.sy;
    uint32_t want = sa_wanted_samples ? sa_wanted_samples : target_samples;
    uint32_t n = 1;
    while (n * 2 <= want && n < 8 && (msaa_samples_have & (n * 2))) {
        n *= 2;
    }
    *samples = sample_shading || n == target_samples ? n : 1;
}

// (T2) Whether the scene has the GS frame target's shape (its scale and samples): its pictures are copied as they are
bool sa_same_shape(const Target &color) {
    return sa.sx == color.sx && sa.sy == color.sy && sa.samples == target_samples;
}

// The scene takes the frame's picture and Z buffer (all of them: what it draws over blends with them, and is
// tested against them)
bool sa_acquire(uint32_t fbp, uint32_t fbw, uint32_t fpsm, uint32_t zbp, uint32_t zpsm, uint32_t need_h) {
    if (sat_scene_frame(fbw, fpsm)) {
        return sat_acquire(fbp, fbw, fpsm, zbp, zpsm, need_h); // (T3: a named target: the scene's own)
    }
    if (sa.bound && sa.fbp == fbp && sa.fbw == fbw && sa.fpsm == fpsm && sa.zbp == zbp && sa.zpsm == zpsm &&
        find_target(fbp, fbw, fpsm) == sa.frame_target && find_target(zbp, fbw, zpsm) == sa.z_target &&
        sa.frame_target->height >= need_h) {
        return true;
    }
    sa_release();
    Target *color, *depth;
    if (!sa_targets(fbp, fbw, fpsm, zbp, zpsm, need_h, &color, &depth)) {
        return false;
    }
    sa_programs();
    // (T2) the scene's own scale and samples (--scene-scale, --scene-msaa), its targets made for them
    uint32_t sx, sy, samples;
    sa_shape(*color, &sx, &sy, &samples);
    uint32_t w = color->width * sx, h = color->height * sy;
    VkFormat format = color->image->get_format();
    if (!sa.color || sa.color->get_width() != w || sa.color->get_height() != h || sa.samples != samples ||
        sa.color->get_format() != format) {
        sa.color = sa_image(w, h, format, samples);
        sa.depth = sa_image(w, h, sa.depth_format, samples);
        sa.samples = samples;
    }
    sa.sx = sx;
    sa.sy = sy;
    bool same = sa_same_shape(*color);
    const Sa::Clear &k = sa.clear;
    bool cleared = k.valid && k.fbp == fbp && k.fbw == fbw && k.fpsm == fpsm && k.zbp == zbp && k.zpsm == zpsm;
    gpu_mark(GPU_OTHER);
    if (cleared) {
        // (T2) the frame and Z buffer hold only the GS's clear: the scene's cleared the same (no copy)
        {
            Why why(GSN_WHY_COPY);
            end_pass();
        }
        transfer_barrier();
        VkClearValue cv = {};
        for (int c = 0; c < 4; c++) {
            cv.color.float32[c] = float((k.rgba >> (c * 8)) & 0xFF) / 255.0f;
        }
        if (fpsm == GSN_PSMCT24) {
            cv.color.float32[3] = 0.0f; // (as a 24-bit frame's target holds it: not read)
        }
        cmd().clear_image(*sa.color, cv);
        VkClearValue dv = {};
        dv.depthStencil.depth = float(double(std::min(k.z, depth_max(zpsm))) * depth_scale(zpsm));
        dv.depthStencil.stencil = 0;
        cmd().clear_image(*sa.depth, dv);
        transfer_barrier();
        gpu_mark(GPU_COPY);
        sa.clear.valid = false;
        sa_stats.cleared++;
        if (!interp.active) {
            sa_jitter_frame(w, h, sx, color->sx); // (a game frame's own picture begins: its jitter, with an upscaler)
        }
    } else if (same) {
        {
            Why why(GSN_WHY_COPY);
            if (samples > 1) {
                msaa_ready(*color);
                msaa_ready(*depth);
            } else {
                msaa_settle(*color);
                msaa_settle(*depth);
            }
            end_pass();
        }
        transfer_barrier();
        const Image &cs = samples > 1 ? *color->ms : *color->image;
        const Image &zs = samples > 1 ? *depth->ms : *depth->image;
        cmd().copy_image(*sa.color, cs, {0, 0, 0}, {0, 0, 0}, {w, h, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        if (sa.stencil) {
            // (a depth with a stencil is not the GS's format: its depth written by a pass, sample for sample, and
            // its stencil cleared)
            transfer_barrier();
            float zpush[2] = {-1.0f, 1.0e30f}; // (as it is)
            sa_copy_pass(*sa.depth, zs, samples > 1 ? sa.program_depth_ms : sa.program_depth, true, 0, h, zpush, sizeof(zpush),
                         true);
        } else {
            cmd().copy_image(*sa.depth, zs, {0, 0, 0}, {0, 0, 0}, {w, h, 1}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                             {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1});
        }
        transfer_barrier();
        gpu_mark(GPU_COPY);
        gsn_stats.copy_bytes += uint64_t(w) * h * (wide(cs) ? 12 : 8) * samples;
    } else {
        // (T2) another scale or samples: each GS pixel repeated into the scene's pixels and samples
        {
            Why why(GSN_WHY_COPY);
            msaa_settle(*color);
            msaa_settle(*depth);
            end_pass();
        }
        float rx = float(color->sx) / float(sx), ry = float(color->sy) / float(sy);
        sa_resample_pass(*sa.color, *color->image, false, 0, h, rx, ry, 0.0f, 0.0f);
        sa_resample_pass(*sa.depth, *depth->image, true, 0, h, rx, ry, 0.0f, 0.0f, true);
        gpu_mark(GPU_COPY);
        gsn_stats.copy_bytes += uint64_t(w) * h * 8 * samples;
    }
    sa.stencil_gen = 0;
    sa.last_shadow = false;
    sa.bound = true;
    sa.fbp = fbp;
    sa.fbw = fbw;
    sa.fpsm = fpsm;
    sa.zbp = zbp;
    sa.zpsm = zpsm;
    sa.frame_target = color;
    sa.z_target = depth;
    sa.rows0 = UINT32_MAX;
    sa.rows1 = 0;
    sa.z_written = false;
    sa.drew = false;
    sa.scissor_rows = 0;
    sa_stats.acquires++;
    return true;
}

// The scene gives the frame's picture and Z buffer back to the GS: its colour over the rows it drew into,
// and its depth rounded to the GS's whole Z. keep (T2): the GS only reads them (a copy of the frame, a
// read-back): they are given it, and the scene keeps the frame. (At another scale or samples than the GS's,
// each GS pixel the mean of the scene's pixels and samples over it, and the scene's picture kept for the
// composite: gsn_composite.inc, sa_hold.)
void sa_hold(Target &color);
void sa_group_draw();
void sa_give(bool keep) {
    sa_group_draw(); // (pf/skin: a skinned model's parts kept are drawn before the scene's turn ends)
    if (!sa.bound || sat_release()) {
        return; // (T3: a named target bound: let go, nothing copied)
    }
    if (keep) {
        sa_stats.published++;
    } else {
        sa.bound = false;
        sa_stats.releases++;
    }
    {
        Why why(GSN_WHY_COPY);
        end_pass();
    }
    if (sa.rows1 <= sa.rows0 && (keep || !sa.drew)) {
        return; // (nothing drawn: the GS's are as they were)
    }
    bool copy = sa.rows1 > sa.rows0; // (else only the composite's hold: drawn and given already, keep)
    sa_stats.frames_touched += copy;
    Target *color = find_target(sa.fbp, sa.fbw, sa.fpsm), *depth = find_target(sa.zbp, sa.fbw, sa.zpsm);
    auto fits = [&](const Target *c) {
        return c->width * sa.sx == sa.color->get_width() && c->height * sa.sy == sa.color->get_height();
    };
    if (color != sa.frame_target || depth != sa.z_target || !color || !depth || !fits(color)) {
        // (the GS let a target go while the scene had it: a texture's pages claimed. Made again from VRAM; the
        // scene's picture is all of it)
        if (!sa_targets(sa.fbp, sa.fbw, sa.fpsm, sa.zbp, sa.zpsm, sa.color->get_height() / std::max(sa.sy, 1u), &color,
                        &depth) ||
            !fits(color)) {
            static int told;
            if (!told++) {
                fprintf(stderr, "[gsn] standalone: the frame target went while the scene had it; its picture is lost\n");
            }
            sa.bound = false;
            return;
        }
        sa.frame_target = color;
        sa.z_target = depth;
    }
    bool same = sa_same_shape(*color);
    uint32_t sy = color->sy;
    uint32_t gh = color->image->get_height();
    uint32_t y0 = std::min(sa.rows0 * sy, gh), y1 = std::min(sa.rows1 * sy, gh);
    uint32_t w = color->image->get_width();
    // (--native-layer-z tolerant keeps Z's fraction in the GS's Z buffer too: copied as it is, within the format)
    float zpush[2] = {float(depth_scale(sa.zpsm)) * (layer_z_mode == GSN_LAYER_Z_TOLERANT ? -1.0f : 1.0f),
                      float(depth_max(sa.zpsm))};
    float rx = float(sa.sx) / float(color->sx), ry = float(sa.sy) / float(color->sy);
    gpu_mark(GPU_OTHER);
    if (!copy) {
    } else if (target_samples > 1) {
        msaa_ready(*color);
        msaa_ready(*depth);
        if (same) {
            sa_copy_pass(*color->ms, *sa.color, sa.program_copy_ms, false, y0, y1, nullptr, 0);
        } else {
            sa_resample_pass(*color->ms, *sa.color, false, y0, y1, rx, ry, 0.0f, 0.0f);
        }
        if (sa.z_written) {
            if (same) {
                sa_copy_pass(*depth->ms, *sa.depth, sa.program_depth_ms, true, y0, y1, zpush, sizeof(zpush));
            } else {
                sa_resample_pass(*depth->ms, *sa.depth, true, y0, y1, rx, ry, zpush[0], zpush[1]);
            }
        }
        color->ms_newer = true;
        color->image_newer = false;
        if (sa.z_written) {
            depth->ms_newer = true;
            depth->image_newer = false;
        }
    } else {
        msaa_settle(*color);
        msaa_settle(*depth);
        if (same) {
            transfer_barrier();
            cmd().copy_image(*color->image, *sa.color, {0, int32_t(y0), 0}, {0, int32_t(y0), 0}, {w, y1 - y0, 1},
                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            transfer_barrier();
        } else {
            sa_resample_pass(*color->image, *sa.color, false, y0, y1, rx, ry, 0.0f, 0.0f);
        }
        if (sa.z_written) {
            if (same) {
                sa_copy_pass(*depth->image, *sa.depth, sa.program_depth, true, y0, y1, zpush, sizeof(zpush));
            } else {
                sa_resample_pass(*depth->image, *sa.depth, true, y0, y1, rx, ry, zpush[0], zpush[1]);
            }
        }
        msaa_image_written(*color);
        if (sa.z_written) {
            msaa_image_written(*depth);
        }
    }
    if (copy) {
        gpu_mark(GPU_MS_COPY);
        gsn_stats.copy_bytes += uint64_t(w) * (y1 - y0) * 8 * target_samples;
        mark_dirty(*color, sa.rows0, sa.rows1);
        if (sa.z_written) {
            mark_dirty(*depth, sa.rows0, sa.rows1);
        }
    }
    if (!same && !keep) {
        sa_hold(*color); // (the composite's: gsn_composite.inc)
    }
    if (keep) {
        sa.rows0 = UINT32_MAX; // (the GS has them all now; what is drawn after is given at the next)
        sa.rows1 = 0;
        sa.z_written = false;
    }
}
void sa_release() {
    sa_give(false);
}

// The scene's render pass, unless it is under way
void sa_begin_pass(bool scratch = false) {
    if (gs->in_pass && gs->pass_color == &SA_MARKER && (gs->pass_scratch || !scratch)) {
        return;
    }
    {
        Why why(GSN_WHY_TARGET);
        end_pass();
    }
    gpu_mark(GPU_OTHER);
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &sa.color->get_view();
    rp.load_attachments = 1;
    rp.store_attachments = 1;
    rp.depth_stencil = &sa.depth->get_view();
    rp.op_flags = RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT | RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
    RenderPassInfo::Subpass subpass;
    if (scratch) {
        // (T8: the copy a run blends from, in the pass: colour attachment 1, neither loaded nor kept)
        rp.num_color_attachments = 2;
        rp.color_attachments[1] = &blend_scratch(*sa.color).get_view();
        scratch_subpass(rp, subpass, sa.samples > 1);
    } else if (sa.samples > 1) {
        msaa_subpass(rp, subpass);
    }
    full_barrier();
    cmd().begin_render_pass(rp);
    gs->in_pass = true;
    gs->pass_color = const_cast<Target *>(&SA_MARKER);
    gs->pass_depth = const_cast<Target *>(&SA_MARKER);
    gs->pass_ms = sa.samples > 1;
    gs->pass_scratch = scratch;
    gs->pass_area = false;
    gsn_stats.passes++;
}

// (sa_draw_batch) whether the draw writes Z at all
bool zwrite_any(uint64_t zbuf, uint64_t test) {
    return !bits(zbuf, 32, 1) && bits(test, 16, 1);
}

// (sa_flush) Whether a batch of the GS's can touch a frame (fbp, fbw, fpsm, its first rows) and Z buffer (zbp, zpsm): its
// frame, its Z buffer (when it tests or writes Z) or its texture on their pages
bool sa_batch_touches(uint32_t fbp, uint32_t fbw, uint32_t fpsm, uint32_t zbp, uint32_t zpsm, uint32_t rows) {
    uint32_t tlo[2], tcount[2];
    buffer_pages(fbp, fbw, fpsm, std::max(fbw, 1u) * 64, rows, &tlo[0], &tcount[0]);
    buffer_pages(zbp, fbw, zpsm, std::max(fbw, 1u) * 64, rows, &tlo[1], &tcount[1]);
    uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
    uint64_t frame = gs->reg[R_FRAME_1 + ctxt], zbuf = gs->reg[R_ZBUF_1 + ctxt], test = gs->reg[R_TEST_1 + ctxt];
    uint64_t scissor = gs->reg[R_SCISSOR_1 + ctxt], tex0 = gs->reg[R_TEX0_1 + ctxt];
    uint32_t bw = bits(frame, 16, 6), h = bits(scissor, 48, 11) + 1;
    auto touches = [&](uint32_t bp, uint32_t w_, uint32_t psm, uint32_t w, uint32_t r) {
        uint32_t lo, count;
        buffer_pages(bp, w_, psm, w, r, &lo, &count);
        return pages_overlap(lo, count, tlo[0], tcount[0]) || pages_overlap(lo, count, tlo[1], tcount[1]);
    };
    if (touches(bits(frame, 0, 9) * 32, bw, bits(frame, 24, 6), std::max(bw, 1u) * 64, h)) {
        return true;
    }
    // (the Z buffer as flush_batch needs it: not for ZTST ALWAYS without a write, nor GEQUAL from the format's
    // largest Z, as the scrolling textures draw)
    uint32_t bzpsm = bits(zbuf, 24, 4) | 0x30, ztst = bits(test, 17, 2), zmin = UINT32_MAX;
    bool zwrite = !bits(zbuf, 32, 1) && (!bits(test, 0, 1) || bits(test, 1, 3) != 0 || bits(test, 12, 2) == 2);
    for (const Vtx &v : gs->verts) {
        zmin = std::min(zmin, v.z);
    }
    bool need_z = bits(test, 16, 1) && ztst != 0 &&
                  (zwrite || !(ztst == 1 || (ztst == 2 && std::min(zmin, depth_max(bzpsm)) == depth_max(bzpsm))));
    if (need_z && touches(bits(zbuf, 0, 9) * 32, bw, bzpsm, std::max(bw, 1u) * 64, h)) {
        return true;
    }
    if (((prim >> 4) & 1) && touches(bits(tex0, 0, 14), bits(tex0, 14, 6), bits(tex0, 20, 6), 1u << std::min(bits(tex0, 26, 4), 10u),
                                     1u << std::min(bits(tex0, 30, 4), 10u))) {
        return true;
    }
    return false;
}

// (sa_flush) Whether a batch of the GS's can touch what the scene has taken: its frame, its Z buffer (when it
// tests or writes Z) or its texture on their pages. One that cannot (the scrolling textures' sprites into their
// 128 x 128 targets, between the world's draws) is drawn by the GS while the scene keeps the frame
bool sa_gs_touches_scene() {
    if (find_target(sa.fbp, sa.fbw, sa.fpsm) != sa.frame_target || find_target(sa.zbp, sa.fbw, sa.zpsm) != sa.z_target) {
        return true;
    }
    return sa_batch_touches(sa.fbp, sa.fbw, sa.fpsm, sa.zbp, sa.zpsm, std::max(sa.frame_target->height, sa.z_target->height));
}

// (T2) Whether the batch's sprites cover the whole of the frame's first `rows` rows (0, 0 to fbw 64 across), the
// colour the same at every corner (rgba): a flat colour over the screen, or (textured) a picture over all of it
bool sa_sprites_cover(uint32_t fbw, uint32_t rows, uint32_t *rgba) {
    if (!gs->batch_sprites || gs->verts.empty()) {
        return false;
    }
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    double area = 0.0;
    const std::vector<Vtx> &v = gs->verts;
    *rgba = v[0].rgba;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        for (int k = 0; k < 3; k++) {
            if (v[i + k].rgba != *rgba) {
                return false;
            }
            x0 = std::min(x0, v[i + k].x);
            y0 = std::min(y0, v[i + k].y);
            x1 = std::max(x1, v[i + k].x);
            y1 = std::max(y1, v[i + k].y);
        }
        area += fabs(double(v[i + 1].x - v[i].x) * double(v[i + 2].y - v[i].y) -
                     double(v[i + 2].x - v[i].x) * double(v[i + 1].y - v[i].y)) /
                2.0;
    }
    // (a pixel is drawn where its integer point is inside, the top and left edges in: 0 to fbw 64 across, 0 to rows
    // down; the sprites tile their box)
    return x0 <= 0.0f && y0 <= 0.0f && ceilf(x1) >= float(fbw * 64) && ceilf(y1) >= float(rows) &&
           area >= double(x1 - x0) * double(y1 - y0) * 0.999;
}

// (T2, sa_flush) Whether a batch of the GS's goes into the scene, the scene keeping the frame (or taking it from
// the GS's clear): SA_GS_PASS a screen pass's (nd_pass_draw: the brightness, a tint, the flicker filter, the frame
// before blended in, haze, the wobble, a fill, a fade) while the scene has the frame; SA_GS_FILL a flat colour over
// the whole of the scissor (GsDraw_FillScreen from elsewhere: the screen's fade and flashes) while it has it;
// SA_GS_3D the GS's own 3D, its Z tested GEQUAL or GREATER (the PS2 path's map records native draw does not take).
// In a picture between their corners are moved on the GPU from the frame's kicks, as flush_batch draws them
// (gs_replay_corners.comp: sa_draw_batch, interp.vertices); a screen pass's are the CPU's (nd_pass.vert).
// SA_GS_WATER: the reflection's rippled water (RD_KIND_WATER, hosted: its own vertex program, nd_water.vert, reading
// the reflection's target, T3), drawn into the scene with it while the scene has the frame.
enum { SA_GS_NONE, SA_GS_3D, SA_GS_PASS, SA_GS_FILL, SA_GS_WATER };
bool ndw_batch();
int sa_gs_batch() {
    bool water = nd.active && !nd.pass_active && ndw_batch() && nd.corners_active;
    if (gs->verts.empty() || (nd.active && !nd.pass_active && !water)) {
        return SA_GS_NONE; // (native corners of a kind the scene does not take)
    }
    uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
    uint64_t frame = gs->reg[R_FRAME_1 + ctxt], zbuf = gs->reg[R_ZBUF_1 + ctxt], test = gs->reg[R_TEST_1 + ctxt];
    uint64_t scissor = gs->reg[R_SCISSOR_1 + ctxt];
    uint32_t fbp = bits(frame, 0, 9) * 32, fbw = bits(frame, 16, 6), fpsm = bits(frame, 24, 6);
    uint32_t zbp = bits(zbuf, 0, 9) * 32, zpsm = bits(zbuf, 24, 4) | 0x30;
    bool held = sa.bound && fbp == sa.fbp && fbw == sa.fbw && fpsm == sa.fpsm &&
                find_target(fbp, fbw, fpsm) == sa.frame_target && find_target(sa.zbp, sa.fbw, sa.zpsm) == sa.z_target;
    const Sa::Clear &k = sa.clear;
    bool cleared = !sa.bound && k.valid && fbp == k.fbp && fbw == k.fbw && fpsm == k.fpsm;
    if (!held && !cleared) {
        return SA_GS_NONE;
    }
    bool zte = bits(test, 16, 1);
    uint32_t ztst = zte ? bits(test, 17, 2) : 1;
    bool uses_z = zte && (ztst != 1 || !bits(zbuf, 32, 1));
    if (uses_z && (zbp != (held ? sa.zbp : k.zbp) || zpsm != (held ? sa.zpsm : k.zpsm))) {
        return SA_GS_NONE;
    }
    if (sa.in_pass) {
        return held ? SA_GS_PASS : SA_GS_NONE;
    }
    if (water) {
        return held && zte && (ztst == 2 || ztst == 3) ? SA_GS_WATER : SA_GS_NONE;
    }
    if (interp.active && interp.vsrc.size() != gs->verts.size()) {
        return SA_GS_NONE; // (never: every corner has its kick)
    }
    if (!gs->batch_sprites) {
        return zte && (ztst == 2 || ztst == 3) ? SA_GS_3D : SA_GS_NONE;
    }
    uint32_t rgba;
    if (held && !((prim >> 4) & 1) && bits(scissor, 0, 11) == 0 && bits(scissor, 32, 11) == 0 &&
        sa_sprites_cover(fbw, std::min(bits(scissor, 48, 11) + 1, sa.frame_target->height), &rgba)) {
        return SA_GS_FILL;
    }
    return SA_GS_NONE;
}

// (sa_draw_batch) Whether the batch is the shadows' (Entity_DrawPoolModels: program 0x5A0's records only)
bool sa_shadow_batch() {
    if (gs->verts.empty() || nd.batch_draws != nd.draws.buffer.get()) {
        return false;
    }
    for (const Vtx &v : gs->verts) {
        uint32_t record = (v.rgba & 0xFFFFFFu) >> ND_CORNER_DRAW_SHIFT;
        if (nd_corner_kind(v) != ND_KIND_000 || nd700_corner(v) || record >= nd.of5a0.size() || !nd.of5a0[record]) {
            return false;
        }
    }
    return true;
}

// (sa_draw_batch's draw call: n corners from first) Each stretch of one kind of corner with its program, as
// nd_draw_range does it: program 0x000's (and 0x5A0's) with sa_000.vert, 0x700's with sa_700.vert (0x000's
// records), 0x280's with sa_280.vert and its records, and the PS2 path's (the trail; the scene's ops) with the
// GS's own vertex program (gs.vert: their Z as the GS has it, in the scene's units; nd_strip.vert in a picture
// between). sample: a cut-out shaded at every sample. (T2) The GS's own 3D with shaders/gs_zclamp.frag (its Z kept
// fine, and the tie's slack); a screen pass in a picture between with nd_pass.vert, as nd_draw_range has it (the
// batch's corners are one vertex buffer).
void nd_pass_bind_tris(Cmd &c, uint32_t run, uint32_t end); // (gsn_nd_pass.inc)
// (in_pass, T8: the scene's own corners blend from the render pass's scratch, gs_in.frag / gs_in_ms.frag)
#include "gsn_rgba.inc" // (--native-sampler hardware: the scene's textures for the GPU's sampler)

// The scene's fragment program for a stretch of corners of vertex shader vs: the batch's RGBA8 texture read by the
// GPU's sampler where gsn_rgba.inc made one (the GSN_HW_SAMPLER builds), else gs_frag.h's own sampling
Program *sa_program(int vs, bool sample) {
    return sa.rgba_on && sa.programs_hw[vs][sample ? 1 : 0] ? sa.programs_hw[vs][sample ? 1 : 0] : sa.programs[vs][sample ? 1 : 0];
}
Program *sa_program_in(int vs, bool ms) {
    return sa.rgba_on && sa.programs_in_hw[vs][ms ? 1 : 0] ? sa.programs_in_hw[vs][ms ? 1 : 0] : sa.programs_in[vs][ms ? 1 : 0];
}

void sa_draw_range(Cmd &c, bool sample, uint32_t n, uint32_t first, bool in_pass = false, bool copy = false, bool zonly = false) {
    const Vtx *v = gs->verts.data();
    const Buffer *bound = nullptr;
    for (uint32_t end = first + n; first < end;) {
        int kind = nd_corner_kind(v[first]);
        bool effect = kind == ND_KIND_000 && nd700_corner(v[first]);
        uint32_t to = first + 3;
        while (to < end && nd_corner_kind(v[to]) == kind && (kind != ND_KIND_000 || nd700_corner(v[to]) == effect)) {
            to += 3;
        }
        if (kind == ND_KIND_PS2) {
            if (nd.pass_active) {
                c.set_program(nd_pass_program(gs->program));
                nd_pass_bind_tris(c, 0, to); // (the batch's corners are one vertex buffer: gl_VertexIndex from its first)
            } else if (sa.gs3d) {
                c.set_program(sample && gs->program_zclamp_sample ? gs->program_zclamp_sample : gs->program_zclamp);
            } else {
                c.set_program(nd.strip_active ? nd_strip_program(gs->program) : gs->program);
            }
        } else {
            const Buffer *want = kind == ND_KIND_280 ? nd280_batch_draws() : nd.batch_draws;
            if (sa.gs_water) {
                // (T2) the water's corners, hosted's nd_water.vert with gs_zclamp.frag (its Z kept fine, the slack)
                c.set_program(ndw_program(sample && gs->program_zclamp_sample ? gs->program_zclamp_sample : gs->program_zclamp));
            } else {
                int vs = kind == ND_KIND_280 ? SA_VS_280 : effect ? SA_VS_700 : SA_VS_000;
                c.set_program(ndw_batch() ? sat_water_program(sample) // (T3: the reflection's water, sa_water.vert)
                              : zonly     ? sa.programs_z[vs] // (a tie's Z-first draw by depth alone)
                              : copy      ? sa.programs_copy[vs][sa.samples > 1 ? 1 : 0]
                              : in_pass   ? sa_program_in(vs, sa.samples > 1)
                                          : sa_program(vs, sample));
            }
            if (want && want != bound) {
                c.set_storage_buffer(2, 1, *want);
                bound = want;
            }
        }
        c.draw(to - first, first);
        first = to;
    }
}

// The GS's sprites the scene draws itself, its ops (not the batch of a draw: the PS2 path's): the depth clear
// before the picked item and the arm (GsDraw_ClearDepth: Z 0 over the frame, every pixel failing the alpha test
// into the Z buffer only), and, while the scene has the frame, the screen's alpha set (GsDraw_SetScreenAlpha:
// black, every pixel failing the alpha test into the frame only, blended as Cd: its alpha written alone; before
// the shadows). Drawn into the scene as the GS would draw them, so the scene keeps the frame across them.
enum { SA_OP_NONE, SA_OP_CLEAR_DEPTH, SA_OP_ALPHA, SA_OP_FILL /* (T3: sat_fill) */ };
int sa_scene_op() {
    if (!gs->batch_sprites || nd.active || gs->verts.empty()) {
        return SA_OP_NONE;
    }
    uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
    uint64_t test = gs->reg[R_TEST_1 + ctxt];
    // (untextured; the alpha test NEVER, no DATE, ZTST ALWAYS)
    if (((prim >> 4) & 1) || !bits(test, 0, 1) || bits(test, 1, 3) != 0 || bits(test, 14, 1) || !bits(test, 16, 1) ||
        bits(test, 17, 2) != 1) {
        return SA_OP_NONE;
    }
    uint32_t afail = bits(test, 12, 2);
    if (afail == 2) {
        for (const Vtx &v : gs->verts) {
            if (v.z != 0) {
                return SA_OP_NONE;
            }
        }
        return SA_OP_CLEAR_DEPTH;
    }
    uint64_t frame = gs->reg[R_FRAME_1 + ctxt], zbuf = gs->reg[R_ZBUF_1 + ctxt], alpha = gs->reg[R_ALPHA_1 + ctxt];
    if (afail != 1 || !sa.bound || bits(frame, 0, 9) * 32 != sa.fbp || bits(frame, 16, 6) != sa.fbw ||
        bits(frame, 24, 6) != sa.fpsm || bits(zbuf, 0, 9) * 32 != sa.zbp || (bits(zbuf, 24, 4) | 0x30) != sa.zpsm ||
        !((prim >> 6) & 1) || bits(alpha, 0, 2) != 0 || bits(alpha, 2, 2) != 2 || bits(alpha, 4, 2) != 2 ||
        bits(alpha, 6, 2) != 1) {
        return SA_OP_NONE;
    }
    for (const Vtx &v : gs->verts) {
        if (v.rgba & 0xFFFFFFu) {
            return SA_OP_NONE;
        }
    }
    return SA_OP_ALPHA;
}

// ---- A batch drawn into the scene ----------------------------------------------------------------------
// A tie blended (Cs - Cd) As + Cd whose every pixel the alpha test passes has As 0x80 (a cut-out: its clear texels
// fail the test, the rest are 0x80): its colour comes out the same drawn twice, as same_twice's, so it needs no runs
// (sa_draw_batch, tie_runs). From the texture's alphas (st.texels) with the corners' one alpha; false where unknown.
static bool sa_passing_same_twice(const SaTexture &st, uint64_t tex0, uint64_t test, uint64_t alpha, bool abe,
                                  uint32_t as_min, uint32_t as_max, uint32_t select, int npasses) {
    if (!abe || bits(alpha, 0, 2) != 0 || bits(alpha, 2, 2) != 1 || bits(alpha, 4, 2) != 0 || bits(alpha, 6, 2) != 1 ||
        npasses != 1 || !st.texels || !bits(tex0, 34, 1) || as_min != as_max) {
        return false;
    }
    uint32_t tfx = bits(tex0, 35, 2), atst = bits(test, 1, 3), aref = bits(test, 4, 8);
    for (uint32_t i = 0; i < 256; i++) {
        if (!st.texels[i]) {
            continue;
        }
        uint32_t at = st.src == SRC_CLUT ? st.clut[i] >> 24 : i;
        uint32_t as = tfx == 0 ? std::min(255u, (as_min * at) >> 7) : tfx == 2 ? std::min(255u, as_min + at) : at;
        bool passes = select == 0 || atst == 1 || (atst == 2 && as < aref) || (atst == 3 && as <= aref) ||
                      (atst == 4 && as == aref) || (atst == 5 && as >= aref) || (atst == 6 && as > aref) ||
                      (atst == 7 && as != aref);
        if (passes && as != 0x80) {
            return false;
        }
    }
    return true;
}

// One of a batch's draws (sa_draw_batch, "the ties"): the alpha test's pass, the colour mask, Z written, the slack
struct SaDraw {
    uint32_t select, mask;
    bool zwrite;
    float zbias;
};

// ---- A skinned model's parts drawn together (pf/skin) -------------------------------------------------
// Program 0x280 kicks a model a part (an RdBatch, at most 40 vertices) at a time: its first pass (opaque, Z written,
// the part's TEX0), then its sphere-mapped second pass (the shine, Cs + Cd As: another TEX0 and ALPHA). So every part
// was two batches of its own, and the parts' first passes, whose state is mostly the same (the zombies' parts share one
// texture), never met: about 800 batches and 3,500 Vulkan draws a game frame at the poison pond. Each batch is still
// worked out as before (sa_draw_batch: the texture, the alpha range, the ties, the runs), but a part whose draws are the
// simple ones (a first pass drawn two-phase in one run, exact, every pixel of its triangles passing the alpha test; its
// second pass in one run) is kept, and drawn with the model's other such parts when something else comes (sa_group_draw):
// - the first passes of all of them in one draw for each of their two draws. Their colour within no slack, then Z
//   tested and written, over all of them, leaves what each part's two draws one after another leave: the second draw
//   writes, last, every pixel the first did, and the Z is only the second's;
// - then the second passes, last part first. Drawn one after another, a part's shine stays only where that part's
//   first pass was the last to draw (a later part drawn over it overwrites the shine), on that first pass's colour.
//   So each part's shine is drawn where its own first pass is at the front (its Z, exactly) and no later part's is:
//   after its shine each part marks the pixels where its first pass is at the front in the stencil with the group's
//   number (a number of its own, as the shadows' runs and the layers have, so nothing there has it yet), and a shine
//   is not drawn where the stencil has it. That keeps a later part's pixel at exactly the same depth (an edge two
//   parts' triangles share where they fold over each other) the later part's, as it was. The shines' own Z writes are
//   left out: they write the Z their first passes wrote, which is there.
// Kept from one 0x280 draw of the scene to the next (a model is a draw a bone); anything else first draws what is kept:
// another batch or draw, the scene's turn ending, a picture between's mark, a save state. KF4_SA_GROUP=0 (tests) draws
// each part as it comes.
// (perf/shine-whole) A second pass blended in the shader and drawn whole (the ice creatures' shine on map 017, Cs + Cd As
// with As above 0x80: --native-shine whole, the default) is kept too. Each part's shine blended from a copy of the
// pixels under it made just before it (in the render pass, by its own triangles: sa_copy_run.frag), so each part's
// first pass was a group of one and each shine a copy between two barriers: about 700 of each a picture at the icy
// corridor. Kept, the group's shines blend from one copy made after all its first passes, by all their triangles. That
// is the same picture: a pixel takes one part's shine at most (the marks above), the part whose first pass is the last
// to draw there, and the copy has that first pass's colour there, as the copy before that part's shine had. (Exact,
// --native-shine exact, the shine is drawn in runs, each its own copy, and ends the group, as before.)
struct SaGroupKey {
    Push pushes[4];
    uint32_t masks[4], zwrites[4];
    int32_t ndraws;
    uint32_t per_sample, ztst, blend_on, alpha_factor, fix;
    uint32_t shader_blend; // (a second pass blended in the shader, from the render pass's scratch: perf/shine-whole)
    VkBlendFactor src, dst;
    VkBlendOp op;
    int32_t scissor[4];
    uint32_t viewport[2];
    const ImageView *raw, *target;
    const Buffer *records, *arena;
    uint32_t want_clut;
    uint32_t clut[256];
};
struct SaGroupPass {
    SaGroupKey key;
    std::vector<Vtx> corners;
    bool on = false;
    ImageHandle texture;
};
struct SaGroupPart {
    uint32_t first[2], count[2]; // its passes' corners (count[1] 0: no second pass)
    float box[4];                // its first pass's place on the screen (GS pixels: x0, y0, x1, y1)
};
struct SaGroup {
    SaGroupPass pass[2]; // the first passes, the second passes
    std::vector<SaGroupPart> parts;
    std::vector<uint8_t> needs;
    BufferHandle records, arena;
    uint32_t fbp = 0, fbw = 0, fpsm = 0, zbp = 0, zpsm = 0;
    uint32_t gen = 0;           // the group's number in the stencil
    bool expect_second = false; // the last batch kept was a first pass: its second pass may follow
    uint32_t last_n = 0;
    float last_fog[2] = {};
    int enabled = -1;
    uint64_t parts_kept = 0, draws = 0, groups = 0;
} sa_group;

bool sa_group_pending() {
    return sa_group.pass[0].on;
}

// A pass's state (sa_draw_batch's) for the corners in vertex buffer 0
void sa_group_state(Cmd &c, const SaGroupKey &k) {
    if (k.shader_blend) {
        c.set_input_attachments(0, 4); // (the destination: the pass's scratch, gs_in.frag / gs_in_ms.frag)
    }
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_viewport({0.0f, 0.0f, float(k.viewport[0]), float(k.viewport[1]), 0.0f, 1.0f});
    c.set_scissor({{k.scissor[0], k.scissor[1]}, {uint32_t(k.scissor[2]), uint32_t(k.scissor[3])}});
    c.set_texture(0, 0, *k.raw, StockSampler::NearestClamp);
    c.set_texture(0, 1, *k.target, StockSampler::NearestClamp);
    c.set_texture(0, 3, gs->dummy_rgba->get_view(), StockSampler::NearestClamp);
    sa.rgba_on = false; // (a group's parts read their textures as the GS does: gsn_rgba.inc is for sa_draw_batch's)
    static const uint32_t no_clut[256] = {};
    c.set_storage_data(0, 2, k.want_clut ? k.clut : no_clut, 1024);
    c.set_depth_compare(k.ztst == 2 ? VK_COMPARE_OP_GREATER_OR_EQUAL : k.ztst == 3 ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_ALWAYS);
    if (k.blend_on) {
        float constants[4] = {float(k.fix) / 128.0f, float(k.fix) / 128.0f, float(k.fix) / 128.0f, 128.0f / 255.0f};
        c.set_blend_enable(true);
        c.set_blend_constants(constants);
        c.set_blend_factors(k.src, k.alpha_factor ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE, k.dst, VK_BLEND_FACTOR_ZERO);
        c.set_blend_op(k.op, VK_BLEND_OP_ADD);
    } else {
        c.set_blend_enable(false);
    }
    // (sa_draw_range's for a stretch of 0x280's corners)
    c.set_program(k.shader_blend ? sa.programs_in[SA_VS_280][sa.samples > 1 ? 1 : 0] : sa.programs[SA_VS_280][k.per_sample ? 1 : 0]);
    c.set_storage_buffer(2, 1, *k.records);
}

void sa_group_one(Cmd &c, const Push &p, uint32_t mask, bool zwrite, uint32_t count, uint32_t first) {
    c.set_color_write_mask(mask);
    c.set_depth_test(true, zwrite);
    c.push_constants(&p, 0, sizeof(Push));
    c.draw(count, first);
    sa_stats.draws++;
    gsn_stats.draws++;
    sa_group.draws++;
}

void pc_world_flush(); // (gsn_pc_world.inc)
bool pc_world_kind(uint32_t kind);
extern volatile int pc_world_mode;

// (--renderer fast) The first passes' first draw left out where it leaves nothing the second does not: its colour with no
// Z written, then the same draw exactly with Z (a part's slack is 0: skin_eps), over every pixel of their triangles (every
// pixel passes the alpha test: role 1's covers). Each pixel the first draw writes, the second writes again, last: a
// triangle that passed the first draw passes the second unless a nearer one wrote Z there first, which wrote its colour.
// So the second alone leaves the same colour and Z, for half the first passes' pixel work. (KF4_PC_GROUP_ONE=0: both.)
int sa_group_first_draw(const SaGroupKey &k) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("KF4_PC_GROUP_ONE");
        on = v && *v ? atoi(v) : 1;
    }
    if (!pc_world_mode || !on || k.ndraws != 2 || k.shader_blend || k.zwrites[0] || !k.zwrites[1] || k.masks[0] != k.masks[1]) {
        return 0;
    }
    Push a = k.pushes[0], b = k.pushes[1];
    if (a.zbias != 0.0f || a.zslope != 0.0f || b.zbias != 0.0f || b.zslope != 0.0f) {
        return 0; // (a slack: the first draw passes where the second does not)
    }
    a.zbias = a.zslope = b.zbias = b.zslope = 0.0f; // (-0 and 0 alike)
    return memcmp(&a, &b, sizeof(a)) == 0 ? 1 : 0;
}
// What is kept drawn: the first passes, then the second passes, last part first (above)
void sa_group_draw_parts() {
    SaGroup &g = sa_group;
    if (!g.pass[0].on) {
        return;
    }
    if (sa.bound) {
        const SaGroupKey &k1 = g.pass[0].key, &k2 = g.pass[1].key;
        bool blend_in = g.pass[1].on && k2.shader_blend; // (the shines blend from the render pass's scratch)
        sa_begin_pass(blend_in);
        gpu_enter(GPU_SA_GROUP + (sat_bound() ? GPU_SA_REFL - GPU_SA_DRAW : 0));
        Cmd &c = cmd();
        c.set_opaque_state();
        // the corners: the first passes', then the second passes'
        size_t n1 = g.pass[0].corners.size(), n2 = g.pass[1].on ? g.pass[1].corners.size() : 0;
        Vtx *v = static_cast<Vtx *>(c.allocate_vertex_data(0, (n1 + n2) * sizeof(Vtx), sizeof(Vtx)));
        memcpy(v, g.pass[0].corners.data(), n1 * sizeof(Vtx));
        if (n2) {
            memcpy(v + n1, g.pass[1].corners.data(), n2 * sizeof(Vtx));
        }
        c.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vtx, x));
        c.set_vertex_attrib(1, 0, VK_FORMAT_R32_UINT, offsetof(Vtx, z));
        c.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vtx, rgba));
        c.set_vertex_attrib(3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, s));
        c.set_vertex_attrib(4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vtx, fog));
        // (nd_bind's, for the arena the parts were made with)
        c.set_storage_buffer(2, 0, *k1.arena);
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
        sa_group_state(c, k1);
        for (int i = sa_group_first_draw(k1); i < k1.ndraws; i++) {
            sa_group_one(c, k1.pushes[i], k1.masks[i], k1.zwrites[i] != 0, uint32_t(n1), 0);
        }
        if (n2) {
            sa_group_state(c, k2);
            if (blend_in) {
                // the copy the shines blend from: the pixels under all of them, as all the first passes left them
                // (sa_draw_batch's run copy: conservative, by the shines' own triangles, into the scratch)
                gpu_enter(interp.active ? (sat_bound() ? GPU_REFL_COPY_BETWEEN : GPU_SA_COPY_BETWEEN)
                                        : GPU_SA_BLEND_COPY + (sat_bound() ? GPU_SA_REFL - GPU_SA_DRAW : 0));
                in_pass_barrier();
                c.set_program(sa.programs_copy[SA_VS_280][sa.samples > 1 ? 1 : 0]);
                c.set_conservative(true);
                c.set_depth_test(false, false);
                c.set_color_write_mask(0xF0); // (the scratch only)
                Push p = k2.pushes[k2.ndraws - 1];
                p.src_offset[0] = int32_t(sa.samples);
                p.lod_k = 255.0f;
                c.push_constants(&p, 0, sizeof(p));
                c.draw(uint32_t(n2), uint32_t(n1));
                sa_stats.draws++;
                gsn_stats.draws++;
                sa_group.draws++;
                in_pass_barrier();
                gsn_stats.blend_copies_in_pass++;
                c.set_conservative(false);
                c.set_program(sa.programs_in[SA_VS_280][sa.samples > 1 ? 1 : 0]);
                gpu_enter(GPU_SA_GROUP + (sat_bound() ? GPU_SA_REFL - GPU_SA_DRAW : 0));
            }
            // the mark: a first pass's triangles, untextured and no colour, where its Z is at the front
            Push mark = k1.pushes[k1.ndraws - 1];
            mark.flags &= ~(F_TME | (3u << F_PASS_SHIFT));
            // A part's mark is needed only where a shine before it could reach its pixels: a part before it with a
            // second pass whose place on the screen meets its own (the plan's corners, a pixel wider; in a picture
            // between the corners move on the GPU: every part). The shines between marks, which no needed mark
            // separates, are one draw: two of them never reach the same pixel at the same depth.
            size_t np = g.parts.size();
            std::vector<uint8_t> &needs = g.needs;
            needs.assign(np, 0);
            for (size_t j = 1; j < np; j++) {
                const SaGroupPart &b = g.parts[j];
                for (size_t i = 0; i < j && !needs[j]; i++) {
                    const SaGroupPart &a = g.parts[i];
                    needs[j] = a.count[1] && (interp.active || (a.box[0] <= b.box[2] + 1.0f && b.box[0] <= a.box[2] + 1.0f &&
                                                                a.box[1] <= b.box[3] + 1.0f && b.box[1] <= a.box[3] + 1.0f));
                }
            }
            uint32_t shine_lo = 0, shine_hi = 0, mark_lo = 0, mark_hi = 0; // (pending: corners of pass 1, of pass 0)
            auto shines = [&]() {
                if (shine_hi > shine_lo) {
                    c.set_stencil(true, VK_COMPARE_OP_NOT_EQUAL, VK_STENCIL_OP_KEEP, g.gen, 0xFF);
                    sa_group_one(c, k2.pushes[k2.ndraws - 1], k2.masks[k2.ndraws - 1], false, shine_hi - shine_lo,
                                 uint32_t(n1) + shine_lo);
                }
                shine_lo = shine_hi = 0;
            };
            auto marks = [&]() {
                if (mark_hi > mark_lo) {
                    c.set_stencil(true, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, g.gen, 0xFF);
                    sa_group_one(c, mark, 0, false, mark_hi - mark_lo, mark_lo);
                }
                mark_lo = mark_hi = 0;
            };
            for (size_t j = np; j-- > 0;) {
                const SaGroupPart &part = g.parts[j];
                if (part.count[1]) {
                    if (mark_hi > mark_lo) {
                        shines(); // (the later parts' shines, then their marks, before this one's)
                        marks();
                    }
                    if (shine_hi == shine_lo) {
                        shine_hi = part.first[1] + part.count[1];
                    }
                    shine_lo = part.first[1];
                }
                if (needs[j]) {
                    if (mark_hi == mark_lo) {
                        mark_hi = part.first[0] + part.count[0];
                    }
                    mark_lo = part.first[0]; // (with the parts between that need none: they meet no shine before)
                }
            }
            shines();
            c.set_stencil(false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0, 0xFF);
        }
        g.groups++;
    }
    for (SaGroupPass &p : g.pass) {
        p.on = false;
        p.corners.clear();
        p.texture.reset();
    }
    g.parts.clear();
    g.records.reset();
    g.arena.reset();
    g.expect_second = false;
}

// What is kept drawn, the PC renderer's world first (it comes before the skinned parts in the chain: gsn_pc_world.inc
// keeps the two apart, one pending at a time)
void sa_group_draw() {
    pc_world_flush();
    sa_group_draw_parts();
}

// ---- The PC renderer's world (--renderer fast, P2): the map records kept and drawn by material (gsn_pc_world.inc) ---
#include "gsn_pc_world.inc"

// ---- The frame's batches' work kept for its first picture between (rc/own) ------------------------------
// A frame's first picture between draws the frame's packets again (gsn.cpp, "Pictures between on the GPU"), and so
// its scene batches again: the same corners (native draw's: gsn_nd_prep.inc keeps them from the frame's draws), but
// for the record numbers in them. What sa_draw_batch works out from the corners alone comes out the same: their alphas
// and rows, which triangles are outside the scissor, where triangles overlap (the runs), each triangle's slope. The
// frame keeps it (an SaMemo, in the frame's NdPrepFrame), and the picture's batch with the same corners takes it. The
// picture keeps a triangle outside the scissor that was inside it at t 0 (nd_box_at_0): when one of the frame's left
// out is kept, the picture works the batch out itself. KF4_SA_MEMO=0 (tests): never taken; KF4_SA_MEMO_CHECK=1:
// taken and worked out both, the differences counted.
const uint32_t SA_MEMO_RECORD = 0x3FFF80u; // (a native corner's record: bits 7-21; NdPrepBatch's records under 2^15)
struct SaMemoState {
    int enabled = -1, check = 0;
    const SaMemo *use = nullptr; // (a picture) the frame's for this batch
    SaMemo keep = {};            // (a frame) what is being kept for this batch
    bool keeping = false;
    NdPrepFrame *into = nullptr; // (keeping) the frame's
    uint64_t taken = 0, missed[4] = {}; // --stats: taken; not: none kept, another batch, one left out kept, not kept corners alone
    uint64_t check_differ = 0, differ[8] = {};
} sa_memo;

bool sa_memo_enabled() {
    if (sa_memo.enabled < 0) {
        const char *v = getenv("KF4_SA_MEMO");
        sa_memo.enabled = v && *v ? atoi(v) != 0 : 1;
        const char *c = getenv("KF4_SA_MEMO_CHECK");
        sa_memo.check = c && *c ? atoi(c) != 0 : 0;
    }
    return sa_memo.enabled != 0 && nd_prep_on();
}

// At sa_draw_batch's start, a batch of native corners (scissor: sx0, sy0, x1, y1): in a frame, kept from here on (a batch
// of its kept corners alone); in a picture, the frame's found
void sa_memo_begin(const uint32_t *scissor) {
    sa_memo.use = nullptr;
    sa_memo.keeping = false;
    if (!sa_memo_enabled() || gs->verts.empty()) {
        return;
    }
    if (!interp.active) {
        if (nd_prep.filling && nd_prep_run_is(nd_prep.run_frame) && nd_prep.run_frame != nd_prep.taking) {
            sa_memo.keeping = true;
            sa_memo.into = nd_prep.run_frame;
            sa_memo.keep = {};
            sa_memo.keep.at = uint32_t(nd_prep.run_at);
            sa_memo.keep.n = nd_prep.run_n;
            memcpy(sa_memo.keep.scissor, scissor, sizeof(sa_memo.keep.scissor));
        }
        return;
    }
    const NdPrepFrame *f = nd_prep.taking;
    if (!nd_prep_run_is(f)) {
        sa_memo.missed[3]++;
        return;
    }
    auto it = f->sa_index.find(uint32_t(nd_prep.run_at));
    if (it == f->sa_index.end()) {
        sa_memo.missed[0]++;
        return;
    }
    const SaMemo &m = f->sa_memos[it->second];
    if (m.n != gs->verts.size() || memcmp(m.scissor, scissor, sizeof(m.scissor)) != 0) {
        sa_memo.missed[1]++;
        return;
    }
    sa_memo.use = &m;
}

// At the end of the batch's working out (a frame's): what was kept, for its picture
void sa_memo_end() {
    if (!sa_memo.keeping) {
        return;
    }
    sa_memo.keeping = false;
    NdPrepFrame &f = *sa_memo.into;
    f.sa_index[sa_memo.keep.at] = uint32_t(f.sa_memos.size());
    f.sa_memos.push_back(sa_memo.keep);
}

void sa_memo_print_stats() {
    if (!sa_memo.taken && !sa_memo.missed[0] && !sa_memo.missed[1] && !sa_memo.missed[2] && !sa_memo.missed[3]) {
        return;
    }
    fprintf(stderr, "[stats] standalone, pictures between: %llu batches took the frame's work on their corners; worked out "
                    "again: %llu not the frame's corners alone, %llu none kept (the frame's left all out), %llu another batch of "
                    "those corners, %llu a triangle left out kept",
            (unsigned long long)sa_memo.taken, (unsigned long long)sa_memo.missed[3], (unsigned long long)sa_memo.missed[0],
            (unsigned long long)sa_memo.missed[1], (unsigned long long)sa_memo.missed[2]);
    if (sa_memo.check) {
        fprintf(stderr, "; checked: %llu differ (", (unsigned long long)sa_memo.check_differ);
        for (int i = 0; i < 8; i++) {
            fprintf(stderr, "%s%llu", i ? " " : "", (unsigned long long)sa_memo.differ[i]);
        }
        fprintf(stderr, ")");
    }
    fprintf(stderr, "\n");
}

// (flush_batch, a batch of the scene's corners) Its GS state as flush_batch works it out, drawn with
// sa_000.vert into the scene's targets. False: declined (sa_stats.declined[*why]), for the GS to draw.
// (perf/icy) after a batch is drawn (or kept by the skinned groups): whether it was a first pass that wrote its Z exactly
// over every pixel of its triangles, and its corners' places (sa.first_z)
void sa_note_first_z(bool first) {
    sa.first_z_on = first;
    sa.first_z.clear();
    if (first) {
        for (const Vtx &v : gs->verts) {
            sa.first_z.push_back(sa_place_bits(v));
        }
    }
}

bool sa_draw_batch(int *why) {
    uint32_t prim = gs->batch_prim;
    uint32_t ctxt = (prim >> 9) & 1;
    bool tme = (prim >> 4) & 1, fge = (prim >> 5) & 1, abe = (prim >> 6) & 1, fst = (prim >> 8) & 1;
    uint64_t frame = gs->reg[R_FRAME_1 + ctxt], zbuf = gs->reg[R_ZBUF_1 + ctxt], test = gs->reg[R_TEST_1 + ctxt];
    uint64_t alpha = gs->reg[R_ALPHA_1 + ctxt], scissor = gs->reg[R_SCISSOR_1 + ctxt];
    uint64_t tex0 = gs->reg[R_TEX0_1 + ctxt], tex1 = gs->reg[R_TEX1_1 + ctxt], clamp = gs->reg[R_CLAMP_1 + ctxt];
    uint32_t fbp = bits(frame, 0, 9) * 32, fbw = bits(frame, 16, 6), fpsm = bits(frame, 24, 6);
    uint32_t fbmsk = uint32_t(frame >> 32);
    uint32_t zbp = bits(zbuf, 0, 9) * 32, zpsm = bits(zbuf, 24, 4) | 0x30;
    if (sa_group_pending() && (!sa.in_draw || !nd.active || nd.pass_active || sa_group.fbp != fbp ||
                               sa_group.fbw != fbw || sa_group.fpsm != fpsm || sa_group.zbp != zbp || sa_group.zpsm != zpsm)) {
        sa_group_draw(); // (pf/skin: not a part of the model's to keep)
    }
    bool zmsk = bits(zbuf, 32, 1);
    uint32_t sx0 = bits(scissor, 0, 11), sx1 = bits(scissor, 16, 11), sy0 = bits(scissor, 32, 11), sy1 = bits(scissor, 48, 11);
    bool ate = bits(test, 0, 1);
    uint32_t atst = bits(test, 1, 3), afail = bits(test, 12, 2);
    bool date = bits(test, 14, 1);
    uint32_t ztst = bits(test, 16, 1) ? bits(test, 17, 2) : 1;
    if (!bits(test, 16, 1)) {
        zmsk = true;
    }
    // the shadows (A17): each pixel darkened once by the stencil (first write wins), not DATE's test of the
    // alpha bit FBA sets (the screen's alpha cleared before them: DATM 0), which the shader would blend from
    // copies of the scene's colour, triangle run by triangle run
    bool shadow = date && sa.stencil && !bits(test, 15, 1) && sa_shadow_batch();
    if (shadow) {
        date = false;
    }
    if (((fpsm != GSN_PSMCT32 && fpsm != GSN_PSMCT24) || fbw * 64 < 512 || sx1 < sx0 || sy1 < sy0) &&
        !(sat_scene_frame(fbw, fpsm) && sx1 >= sx0 && sy1 >= sy0)) { // (T3: a named target's)
        *why = SA_WHY_FRAME;
        return false;
    }
    if (fpsm == GSN_PSMCT24) {
        fbmsk |= 0xFF000000u;
    }
    if (ztst == 0) {
        gs->verts.clear(); // (ZTST NEVER: nothing)
        return true;
    }
    if (!sa_acquire(fbp, fbw, fpsm, zbp, zpsm, sy1 + 1)) {
        *why = SA_WHY_TARGET;
        return false;
    }
    if (sa.samples > 1 && !sample_shading) {
        *why = SA_WHY_SAMPLES;
        return false;
    }
    Target *color = sa.frame_target;
    // (T2) a batch of the PS2 path's corners only (the GS's own 3D, a screen pass, a fill, the scene's ops): its
    // 2D's rectangles moved out to the edges of their GS pixels at a larger scale or with samples, as flush_batch
    // has them (flat_2d), so a sprite over the whole frame covers its first column and row
    bool ps2_only = !nd.active || (nd.pass_active && !nd.corners_active);
    bool flat = ps2_only && (sa.sx > 1 || sa.sy > 1 || sa.samples > 1) && !perspective_batch(gs->verts);
    // (a picture between: the GS's own batch's corners made at the picture's time on the GPU from its kicks, as
    // flush_batch draws them: into interp.vertices from interp.corner on, flat_2d's moves there too)
    bool gpu_corners = interp.active && !nd.active && (sa.gs3d || sa.gs_fill);
    uint32_t corners_at = 0;
    if (gpu_corners) {
        corners_at = interp.corner;
        if (interp.vsrc.size() != gs->verts.size() ||
            size_t(corners_at) + interp.vsrc.size() > size_t(interp.frame->kicks) * 3 + 5) {
            interp.broken = true; // (never)
            gpu_corners = false;
        } else {
            for (size_t i = 0; i < interp.vsrc.size(); i++) {
                uint32_t c4[4] = {interp.vsrc[i][0], interp.vsrc[i][1], interp.vsrc[i][2],
                                  interp.vsrc[i][3] | (flat ? uint32_t(IC_FLAT_2D) : 0u)};
                memcpy(interp.corner_map[1 + corners_at + i], c4, sizeof(c4)); // (after the count)
            }
            interp.corner += uint32_t(interp.vsrc.size());
        }
    }
    if (flat && !gpu_corners) {
        flat_2d(gs->verts);
    }
    if (sa.log == 1) {
        uint32_t zl = UINT32_MAX, zh = 0;
        for (const Vtx &v : gs->verts) {
            zl = std::min(zl, v.z);
            zh = std::max(zh, v.z);
        }
        fprintf(stderr, "[sa] batch %llu: fbp %#x zbp %#x zpsm %#x ztst %u zmsk %d abe %d alpha %#llx test %#llx z %u-%u n %zu\n",
                (unsigned long long)sa_stats.batches, fbp, zbp, zpsm, ztst, zmsk, abe, (unsigned long long)alpha,
                (unsigned long long)test, zl, zh, gs->verts.size());
    }
    // (rc/own) a batch of native corners: the frame's work on them kept, or in its picture between taken (sa_memo_*)
    uint32_t memo_scissor[4] = {sx0, sy0, std::min(sx1 + 1, color->width), std::min(sy1 + 1, color->height)};
    sa_memo.use = nullptr;
    sa_memo.keeping = false;
    if (nd.active && !nd.pass_active && !gpu_corners) {
        sa_memo_begin(memo_scissor);
    }
    const SaMemo *memo = sa_memo.use;
    const NdPrepFrame *memo_frame = nd_prep.taking;
    bool memo_check = memo && sa_memo.check;
    uint32_t as_max = 0, as_min = 255;
    float vy0 = gs->verts[0].y, vy1 = vy0;
    if (memo && !memo_check) {
        as_max = memo->as_max;
        as_min = memo->as_min;
        vy0 = memo->vy0;
        vy1 = memo->vy1;
    } else {
        for (const Vtx &v : gs->verts) {
            as_max = std::max(as_max, v.rgba >> 24);
            as_min = std::min(as_min, v.rgba >> 24);
            vy0 = std::min(vy0, v.y);
            vy1 = std::max(vy1, v.y);
        }
        if (memo_check && (as_max != memo->as_max || as_min != memo->as_min || vy0 != memo->vy0 || vy1 != memo->vy1)) {
            sa_memo.check_differ++, sa_memo.differ[0]++;
        }
    }
    sa_memo.keep.as_max = as_max;
    sa_memo.keep.as_min = as_min;
    sa_memo.keep.vy0 = vy0;
    sa_memo.keep.vy1 = vy1;
    if (interp.active) {
        interp_alpha_range(as_min, as_max);
    }

    Push push = {};
    push.inv_size[0] = 1.0f / float(color->width);
    push.inv_size[1] = 1.0f / float(color->height);
    push.zscale = float(depth_scale(zpsm));
    push.zmax = depth_max(zpsm);
    push.test = uint32_t(test);
    push.fogcol = uint32_t(gs->reg[R_FOGCOL]) & 0xFFFFFF;
    push.alpha = bits(alpha, 0, 2) | (bits(alpha, 2, 2) << 2) | (bits(alpha, 4, 2) << 4) | (bits(alpha, 6, 2) << 6) |
                 (bits(alpha, 32, 8) << 8);
    push.texa = uint32_t(gs->reg[R_TEXA] & 0x80FF) | (bits(gs->reg[R_TEXA], 32, 8) << 16);
    push.fbmsk = fbmsk;
    push.misc = (bits(gs->reg[R_FBA_1 + ctxt], 0, 1) ? M_FBA : 0) | (bits(gs->reg[R_COLCLAMP], 0, 1) ? M_COLCLAMP : 0) |
                (fpsm == GSN_PSMCT24 ? M_DST24 : 0) | (bits(gs->reg[R_PABE], 0, 1) ? M_PABE : 0) |
                (bits(gs->reg[R_SCANMSK], 0, 2) << M_SCANMSK_SHIFT);
    if (wide(*sa.color)) {
        push.misc |= M_HIGH;
    }
    push.flags = fge ? F_FGE : 0;

    SaTexture st;
    if (tme) {
        sa_texture(st, push, ctxt, tex0, tex1, clamp, fst, color);
        if (st.own_named) {
            *why = SA_WHY_OWN; // (a named target's draw reading that target: T3, the GS's)
            return false;
        }
        if (st.reads_own && interp.active) {
            // A frame kept for the pictures between (--frame-rate above 60): a pass that reads its own frame is the
            // GS's, after the turn (hosted's path: its own copy of the frame, made again for each picture). Drawn by
            // the scene, the pictures between of that frame came out black: GameLoop_DrawFrameWithoutHud's
            // brightness pass as a menu or a shop opens: a black flash, those pictures and the next scanout.
            *why = SA_WHY_OWN;
            return false;
        }
        if (st.reads_own) {
            sa_group_draw(); // (pf/skin: the parts kept are in the frame it reads)
            // (T2) the frame it draws into (a screen pass: the brightness, the flicker filter, haze): read from a
            // copy of the scene's colour taken now (resolved), at the scene's scale
            uint32_t W = sa.color->get_width(), H = sa.color->get_height();
            if (!sa.own || sa.own->get_width() != W || sa.own->get_height() != H || sa.own->get_format() != sa.color->get_format()) {
                sa.own = sa_image(W, H, sa.color->get_format(), 1);
            }
            Why why_(GSN_WHY_TEXTURE);
            if (sa.samples > 1) {
                msaa_resolve(*sa.own, *sa.color, {0, 0, 0}, {W, H, 1});
            } else {
                end_pass();
                transfer_barrier();
                cmd().copy_image(*sa.own, *sa.color, {0, 0, 0}, {0, 0, 0}, {W, H, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
                transfer_barrier();
            }
            gsn_stats.copy_bytes += uint64_t(W) * H * 4;
            st.target_view = &sa.own->get_view();
        }
    }
    // (--native-sampler hardware) the texture for the GPU's sampler, where it can stand in for the GS's (gsn_rgba.inc);
    // the level then from the pixel's footprint, so the GS's level's bias for the scale is not added (--native-lod full
    // still reads level 0)
    sa.rgba_on = false;
    if (tme && hw_sampler) {
        RgbaSampling hw = rgba_sampling(st, tex1, clamp);
        if (hw.usable) {
            rgba_configure();
            Rgba::Entry &re = rgba_entry(st, push.texa);
            sa.rgba_view = &re.image->get_view();
            sa.rgba_image = re.image;
            sa.rgba_sampler = hw.sampler;
            sa.rgba_on = true;
            rgba.batches++;
            push.lod_bias = lod_mode == GSN_LOD_FULL ? -64.0f : 0.0f;
        } else {
            rgba_kept_gs(hw);
        }
    }

    // blending: Vulkan's, as flush_batch has it; the GS's shader blending is the GS's (declined). (Not for a
    // texture that is a target, which flush_batch blends in the shader to the GS's rounding: that is for the
    // passes that read the frame again and again; the scene's 3D is drawn anew each frame, the lava over its
    // scrolling target a level off at most, as the other 3D's Vulkan blending is.)
    uint32_t ba = bits(alpha, 0, 2), bb = bits(alpha, 2, 2), bc = bits(alpha, 4, 2), bd = bits(alpha, 6, 2);
    uint32_t fix = bits(alpha, 32, 8);
    FixedBlend blend = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0};
    bool partial_mask = false;
    for (int i = 0; i < 4; i++) {
        uint32_t m = (fbmsk >> (i * 8)) & 0xFF;
        partial_mask |= m != 0 && m != 0xFF;
    }
    bool fixed = fixed_blend(ba, bb, bc, bd, fix, &blend);
    uint32_t tex_alpha_max = st.tex_alpha_max;
    uint32_t as_hi = as_max;
    if (tme && bits(tex0, 34, 1)) {
        uint32_t tpsm = bits(tex0, 20, 6);
        if (tpsm == GSN_PSMCT24) {
            tex_alpha_max = bits(gs->reg[R_TEXA], 0, 8);
        } else if (psm_is_16(tpsm)) {
            tex_alpha_max = std::max(bits(gs->reg[R_TEXA], 0, 8), bits(gs->reg[R_TEXA], 32, 8));
        }
        uint32_t tfx = bits(tex0, 35, 2);
        as_hi = tfx == 0 ? std::min(255u, (as_max * tex_alpha_max) >> 7) : tfx == 2 ? std::min(255u, as_max + tex_alpha_max)
                                                                                   : tex_alpha_max;
    }
    bool as_over = bc == 0 && !blend.premul && as_hi > 0x80;
    // what Vulkan's blending cannot do (DATE, a partial mask, PABE, no COLCLAMP, FBA with As, As above 1.0,
    // Ad or a FIX above 1.0) is blended in the shader, as flush_batch does: from a copy of the scene's colour
    // taken before each run of triangles that do not overlap (gs_frag.h's GSN_F_SHADER_BLEND)
    // (the shadows keep the GS's blending, in the shader: from one copy, as the stencil lets no triangle of the
    // run draw over another's pixel)
    // (T2) the GS's own sprites and draws reading a target (its fills, the screen passes) blend as flush_batch has
    // them, exactly, where Vulkan's blending would round where the GS truncates ((Cs - Cd) C + Cd): a level a draw,
    // which the passes over the whole frame carry on
    bool exact = fixed && (blend.premul != 0 || ba == bb);
    bool shader_blend = date || partial_mask || (shadow && abe) || (abe && sat_blend_exact()) ||
                        (abe && ((push.misc & M_PABE) || !(push.misc & M_COLCLAMP) || (bc == 0 && (push.misc & M_FBA)) ||
                                 as_over || !fixed || ((ps2_only || sa.gs_water) && (st.reads_target || gs->batch_sprites) && !exact)));
    if (shader_blend) {
        push.flags |= F_SHADER_BLEND | (date ? F_DATE : 0);
        if (!abe) {
            push.alpha = (2 << 0) | (2 << 2) | (2 << 4) | (0 << 6); // (0 - 0) * FIX + Cs: no blending
        }
        sa_stats.shader_blends++;
    } else if (abe && blend.premul) {
        push.flags |= blend.premul;
    } else if (abe && bc == 0 && !(ba == bb)) {
        push.flags |= F_ALPHA_FACTOR;
    }
    // Ties. On the GS a draw over a surface of (nearly) the same Z wins where the two round to the same Z,
    // and the data counts on it: a layer blended over a wall, a wall's overlay, the darkness in the arches,
    // a sign on its wall, the hills' edge against the lava. The scene keeps Z's fraction, so such a draw
    // would come out a hair in front or behind, in stripes along the two meshes' triangles. So a draw tested
    // GEQUAL (or GREATER) passes within a slack of what is there, as --native-layer-z tolerant has it
    // (gsn.cpp, "Z and layers"): its Z first, exactly (the nearer of its own and what was there), then its
    // colour within the slack, no Z written; a draw that writes no Z in one pass within it. The slack: one
    // Z for a draw that blends by an As not 0x80 everywhere, or adds (the layers, overlays, glows); half a Z
    // for the others (the GS's tie on average). Without the GS's rounding: no sawtooth where surfaces cross,
    // no lace where two meshes' triangles differ.
    uint32_t a_lo = as_min, a_hi = as_max;
    if (tme && bits(tex0, 34, 1)) {
        uint32_t tfx = bits(tex0, 35, 2);
        a_lo = tfx == 0 ? (as_min * st.tex_lo) >> 7 : tfx == 2 ? std::min(255u, as_min + st.tex_lo) : st.tex_lo;
        a_hi = tfx == 0 ? std::min(255u, (as_max * st.tex_hi) >> 7) : tfx == 2 ? std::min(255u, as_max + st.tex_hi) : st.tex_hi;
    }
    bool same_twice = !abe || (ba == 0 && bb == 1 && bd == 1 && (bc == 0 ? a_lo == 0x80 && a_hi == 0x80 : bc == 2 && fix == 0x80));
    static float slack_layer = -1.0f, slack_tie = -1.0f;
    if (slack_layer < 0.0f) {
        const char *v = getenv("KF4_SA_SLACK"); // (tests: the slacks in Z, LAYER[,TIE]; 1,0.5 by default)
        slack_layer = 1.0f;
        slack_tie = 0.5f;
        if (v && *v) {
            slack_layer = float(atof(v));
            const char *comma = strchr(v, ',');
            slack_tie = comma ? float(atof(comma + 1)) : slack_layer;
        }
    }
    bool ties = ztst == 2 || ztst == 3;
    float slack = !zwrite_any(zbuf, test) ? slack_tie : same_twice ? slack_tie : slack_layer;
    // A skinned model's second pass (program 0x280's sphere-mapped shine) is drawn at its first pass's very
    // corners: the same depth to the bit, so it needs no slack. With one it shone through onto a part a Z or
    // two in front of its own (the seated man's thigh under his coat, far save, 4x: a light band)
    bool second_pass = !gs->verts.empty();
    if (memo && !memo_check) {
        second_pass = memo->second_pass;
    } else {
        for (const Vtx &v : gs->verts) {
            if (nd_corner_kind(v) != ND_KIND_280 || !((v.rgba & 0xFFFFFFu) & ND280_CORNER_SPHERE)) {
                second_pass = false;
                break;
            }
        }
        sa_memo.differ[1] += memo_check && second_pass != memo->second_pass;
    }
    sa_memo.keep.second_pass = second_pass;
    if (second_pass) {
        slack = 0.0f;
    }
    // A skinned model's own parts (program 0x280's first pass: its armour pieces, its cloth, its hands, separate meshes
    // a fraction of a unit apart) tie by the world, not by Z: see skin_eps below.
    bool skinned = !gs->verts.empty() && !second_pass;
    if (memo && !memo_check) {
        skinned = memo->skinned;
    } else {
        for (size_t t = 0; skinned && t < gs->verts.size(); t++) {
            skinned = nd_corner_kind(gs->verts[t]) == ND_KIND_280;
        }
        sa_memo.differ[2] += memo_check && skinned != memo->skinned;
    }
    sa_memo.keep.skinned = skinned;
    // (T2) the GS's own 3D (the PS2 path's corners: a map record native draw does not take) has VU1's whole Z, the
    // scene's Z less its fraction: its colour passes within half a Z more (shaders/gs_zclamp.frag, its Z kept fine)
    float ps2_bias = 0.0f;
    if (sa.gs3d || sa.gs_water) {
        push.misc |= M_ZFINE;
        ps2_bias = 0.5f;
    }
    // the rows, the scissor; triangles wholly outside it left out. In a picture between the GPU moves native draw's
    // corners: one is left out only when outside at t 0 too (nd_box_at_0, as flush_batch has it; the models', the
    // strip's and the water's are kept, the GS's own corners too). Without this a batch of map records reaching far
    // past a widescreen frame's edge (the far save: 3,392 triangles, 85 in view) was drawn in a picture between in
    // 117 runs of its own, each blending from a copy of the whole scene (T8).
    uint32_t scale_x = sa.sx, scale_y = sa.sy; // (the scene's: T2)
    uint32_t x1 = std::min(sx1 + 1, color->width), y1 = std::min(sy1 + 1, color->height);
    if (sx0 >= x1 || sy0 >= y1) {
        gs->verts.clear();
        return true;
    }
    if (memo) {
        // (rc/own) the frame's batch left these out (outside the scissor): the picture keeps one inside it at t 0 (as
        // below), and then works the batch out itself
        float cx0 = float(sx0) - 1.0f, cy0 = float(sy0) - 1.0f, cx1 = float(x1) + 1.0f, cy1 = float(y1) + 1.0f;
        const Vtx *v = gs->verts.data();
        const uint32_t *out = memo_frame->sa_u32.data() + memo->culled_at;
        bool keeps = false;
        for (uint32_t j = 0; j < memo->culled_n && !keeps; j++) {
            size_t i = size_t(out[j]) * 3;
            float tx0 = std::min(v[i].x, std::min(v[i + 1].x, v[i + 2].x)), tx1 = std::max(v[i].x, std::max(v[i + 1].x, v[i + 2].x));
            float ty0 = std::min(v[i].y, std::min(v[i + 1].y, v[i + 2].y)), ty1 = std::max(v[i].y, std::max(v[i + 1].y, v[i + 2].y));
            bool native = nd_corner_kind(v[i]) != ND_KIND_PS2 && nd_corner_kind(v[i + 1]) != ND_KIND_PS2 &&
                          nd_corner_kind(v[i + 2]) != ND_KIND_PS2;
            int32_t box[4] = {int32_t(floorf(tx0)) - 1, int32_t(floorf(ty0)) - 1, int32_t(ceilf(tx1)) + 2, int32_t(ceilf(ty1)) + 2};
            if (!native || !nd_box_at_0(uint32_t(i), 3, box) ||
                !(float(box[2]) < cx0 || float(box[0]) > cx1 || float(box[3]) < cy0 || float(box[1]) > cy1)) {
                keeps = true;
            }
        }
        if (keeps) {
            memo = nullptr;
            sa_memo.missed[2]++;
        }
        memo_check = memo && sa_memo.check;
    }
    if (memo && !memo_check) {
        // (rc/own) the frame's triangles left out, left out (every other is inside the scissor: the frame's kept them)
        const uint32_t *out = memo_frame->sa_u32.data() + memo->culled_at;
        size_t kept = 0, n = gs->verts.size(), next = 0;
        Vtx *v = gs->verts.data();
        for (size_t i = 0; i + 2 < n; i += 3) {
            if (next < memo->culled_n && out[next] == i / 3) {
                next++;
                continue;
            }
            if (kept != i) {
                v[kept] = v[i];
                v[kept + 1] = v[i + 1];
                v[kept + 2] = v[i + 2];
            }
            kept += 3;
        }
        gs->verts.resize(kept);
        if (!kept) {
            return true;
        }
    } else if (!interp.active || (nd.active && !nd.pass_active && !gpu_corners)) {
        float cx0 = float(sx0) - 1.0f, cy0 = float(sy0) - 1.0f, cx1 = float(x1) + 1.0f, cy1 = float(y1) + 1.0f;
        size_t kept = 0, n = gs->verts.size();
        Vtx *v = gs->verts.data();
        if (sa_memo.keeping) {
            sa_memo.keep.culled_at = uint32_t(sa_memo.into->sa_u32.size());
        }
        for (size_t i = 0; i + 2 < n; i += 3) {
            float tx0 = std::min(v[i].x, std::min(v[i + 1].x, v[i + 2].x)), tx1 = std::max(v[i].x, std::max(v[i + 1].x, v[i + 2].x));
            float ty0 = std::min(v[i].y, std::min(v[i + 1].y, v[i + 2].y)), ty1 = std::max(v[i].y, std::max(v[i + 1].y, v[i + 2].y));
            if (tx1 < cx0 || tx0 > cx1 || ty1 < cy0 || ty0 > cy1) {
                if (!interp.active) {
                    if (sa_memo.keeping) {
                        sa_memo.into->sa_u32.push_back(uint32_t(i / 3));
                    }
                    continue;
                }
                bool native = nd_corner_kind(v[i]) != ND_KIND_PS2 && nd_corner_kind(v[i + 1]) != ND_KIND_PS2 &&
                              nd_corner_kind(v[i + 2]) != ND_KIND_PS2;
                int32_t box[4] = {int32_t(floorf(tx0)) - 1, int32_t(floorf(ty0)) - 1, int32_t(ceilf(tx1)) + 2, int32_t(ceilf(ty1)) + 2};
                if (!native || !nd_box_at_0(uint32_t(i), 3, box) ||
                    !(float(box[2]) < cx0 || float(box[0]) > cx1 || float(box[3]) < cy0 || float(box[1]) > cy1)) {
                    goto keep; // (where it is at t 0 is not known here, or is in view)
                }
                continue;
            }
        keep:
            if (kept != i) {
                v[kept] = v[i];
                v[kept + 1] = v[i + 1];
                v[kept + 2] = v[i + 2];
            }
            kept += 3;
        }
        if (sa_memo.keeping) {
            sa_memo.keep.culled_n = uint32_t(sa_memo.into->sa_u32.size() - sa_memo.keep.culled_at);
        }
        if (memo_check && kept != gs->verts.size() - size_t(memo->culled_n) * 3) {
            sa_memo.check_differ++, sa_memo.differ[3]++;
        }
        gs->verts.resize(kept);
        if (!kept) {
            return true;
        }
    }
    push.misc |= scale_y << M_SCALE_SHIFT | scale_x << M_SCALE_X_SHIFT;
    if (sa_jitter_draw()) {
        push.misc |= M_JITTER; // (upscaling: the scene's frame, not a named target, takes the picture's jitter)
    }
    uint32_t write_mask = 0;
    for (int i = 0; i < 4; i++) {
        if (shader_blend || ((fbmsk >> (i * 8)) & 0xFF) != 0xFF) {
            write_mask |= 1u << i;
        }
    }
    if (fpsm == GSN_PSMCT24) {
        write_mask &= 7;
    }

    // the alpha test's passes (flush_batch's), no tie rules
    struct Pass {
        uint32_t select, mask;
        bool zwrite;
    } passes[2];
    int npasses = 0;
    bool zwrite = !zmsk;
    if (!ate || atst == 1) {
        passes[npasses++] = {0, write_mask, zwrite};
    } else {
        if (atst != 0) {
            passes[npasses++] = {1, write_mask, zwrite};
        }
        uint32_t fail_mask = 0;
        bool fail_z = false;
        switch (afail) {
        case 1:
            fail_mask = write_mask;
            break;
        case 2:
            fail_z = zwrite;
            break;
        case 3:
            fail_mask = write_mask & 7;
            break;
        }
        if (fail_mask || fail_z) {
            passes[npasses++] = {atst == 0 ? 0u : 2u, fail_mask, fail_z};
        }
    }
    // --native-cutout-aa: a cut-out shaded at every sample (flush_batch's test)
    bool per_sample = false;
    if (st.cut_texels && bits(tex0, 34, 1) && st.cut_lo < st.cut_hi && st.cut_clear * 4 >= st.cut_all && !fst &&
        cutout_aa_mode == GSN_CUTOUT_AA_SSAA && sa.programs[SA_VS_000][1]) {
        uint32_t tfx = bits(tex0, 35, 2);
        uint32_t lo = tfx == 0 ? (as_min * st.cut_lo) >> 7 : tfx == 2 ? std::min(255u, as_min + st.cut_lo) : st.cut_lo;
        uint32_t hi = tfx == 0 ? std::min(255u, (as_max * st.cut_hi) >> 7) : tfx == 2 ? std::min(255u, as_max + st.cut_hi) : st.cut_hi;
        uint32_t ref = bits(test, 4, 8);
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
    if (sa.pixel[0] >= 0.0f) { // (KF4_SA_PIXEL=X,Y, debug: the batches over a GS pixel)
        float qx = sa.pixel[0], qy = sa.pixel[1];
        for (size_t t = 0; t + 2 < gs->verts.size(); t += 3) {
            const Vtx *v = &gs->verts[t];
            float d0 = (v[1].x - v[0].x) * (qy - v[0].y) - (v[1].y - v[0].y) * (qx - v[0].x);
            float d1 = (v[2].x - v[1].x) * (qy - v[1].y) - (v[2].y - v[1].y) * (qx - v[1].x);
            float d2 = (v[0].x - v[2].x) * (qy - v[2].y) - (v[0].y - v[2].y) * (qx - v[2].x);
            if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0)) {
                float den = d0 + d1 + d2; // (the pixel's Z and q, interpolated: d0 weighs v[2], d1 v[0], d2 v[1])
                float zi = den != 0.0f ? (d1 * float(v[0].z) + d2 * float(v[1].z) + d0 * float(v[2].z)) / den : 0.0f;
                float qi = den != 0.0f ? (d1 * v[0].q + d2 * v[1].q + d0 * v[2].q) / den : 0.0f;
                float ax_ = v[1].x - v[0].x, ay_ = v[1].y - v[0].y, bx_ = v[2].x - v[0].x, by_ = v[2].y - v[0].y;
                float az_ = float(v[1].z) - float(v[0].z), bz_ = float(v[2].z) - float(v[0].z), det_ = ax_ * by_ - ay_ * bx_;
                float zx_ = det_ != 0.0f ? (az_ * by_ - bz_ * ay_) / det_ : 0.0f, zy_ = det_ != 0.0f ? (bz_ * ax_ - az_ * bx_) / det_ : 0.0f;
                fprintf(stderr, "[sa px] at z %.3f q %g dzdx %.3f dzdy %.3f\n", zi, qi, zx_, zy_);
                fprintf(stderr, "[sa px] batch %llu tri %zu: z %u %u %u tex0 %#llx abe %d alpha %#llx test %#llx zbuf %#llx same %d slack %g a %u-%u\n",
                        (unsigned long long)sa_stats.batches, t / 3, v[0].z, v[1].z, v[2].z, (unsigned long long)tex0, abe,
                        (unsigned long long)alpha, (unsigned long long)test, (unsigned long long)zbuf, same_twice, slack, a_lo, a_hi);
            }
        }
    }
    // the ties (above): each pass's Z first, exact, then its colour within the slack
    SaDraw draws[4];
    int ndraws = 0;
    // A tie drawn in one pass that writes Z and comes out the same drawn twice (an opaque mesh on its surface): its
    // colour within the slack of what was there before the batch, no Z written, then the batch again Z tested and
    // written exactly, so it still wins over the surface it lies on (a sign, an overlay) and among its own triangles
    // the nearer wins (flush_batch's two_phase, --native-layer-z tolerant, fix/wall-blink 669bee0). With its Z first
    // and its colour within the slack of that, a batch's later triangles won over its earlier ones within half a Z:
    // on map 3's rope bridge, far off, a plank's lighter top faces over the faces in front of them, light blocks
    // that moved with the distance.
    bool two_phase = ties && same_twice && npasses == 1 && passes[0].zwrite && passes[0].mask && !second_pass && !shadow;
    if (two_phase) {
        draws[ndraws++] = {passes[0].select, passes[0].mask, false, slack};
        draws[ndraws++] = {passes[0].select, passes[0].mask, true, 0.0f};
    }
    // A skinned model's second pass blended in the shader (the ice creatures' shine on map 017: Cs + Cd As with As above
    // 0x80), drawn in runs: each run was its Z-first draw, the copy it blends from and its colour. Right after its own
    // first pass over the same corners, which wrote its Z exactly over every pixel of its triangles (sa.first_z), the
    // Z there is already at least each of its triangles' own: a Z-first draw (GEQUAL, Z written) writes only where its
    // Z equals what is there, the same value. So it is left out: the same Z and colour, a third fewer draws and
    // pipeline binds for the creatures (NATIVE_DRAW_P8.md, "The icy corridor"). (Fixed-function shines keep it: the
    // skinned groups take them with it, role 2 below.)
    bool after_first = second_pass && shader_blend && sa.first_z_before_on && sa.first_z_before.size() == gs->verts.size();
    for (size_t i = 0; after_first && i < gs->verts.size(); i++) {
        const Vtx &v = gs->verts[i];
        const std::array<uint32_t, 3> &f = sa.first_z_before[i];
        after_first = f == sa_place_bits(v);
    }
    bool shine_z_there = after_first && ztst == 2;
    sa_stats.shine_z_left += shine_z_there;
    for (int i = 0; i < npasses && !two_phase; i++) {
        if (ties && passes[i].zwrite && !shine_z_there) {
            draws[ndraws++] = {passes[i].select, 0, true, 0.0f};
        }
    }
    for (int i = 0; i < npasses && !two_phase; i++) {
        if (passes[i].mask || !ties) {
            draws[ndraws++] = {passes[i].select, passes[i].mask, !ties && passes[i].zwrite, ties ? slack : 0.0f};
        }
    }
    // two passes over overlapping triangles, or blending in the shader: in runs that do not overlap, so that
    // each sees those before it (not the shadows': a pixel is drawn once, by the stencil). And a tie that writes
    // Z whose colour does not come out the same drawn twice (flush_batch's tie_runs): drawn whole, its Z first
    // for every triangle, its back triangles' colour failed against its front ones' Z, so a blended mesh over
    // itself kept only its nearest layer (a tree's leaves on maps 004 and 013, the sky through them). In runs
    // each run's Z then its colour: a back triangle is blended before a front one over it, as on the GS. (Not a
    // skinned model's second pass: its triangles are its first pass's, whose Z is there; drawn whole, as the GS,
    // only the nearest gets the shine.)
    bool tie_runs = ties && zwrite && !same_twice && !second_pass &&
                    !sa_passing_same_twice(st, tex0, test, alpha, abe, as_min, as_max, passes[0].select, npasses);
    std::vector<uint32_t> &runs = sa.runs; // (kept from batch to batch: their room)
    runs.assign(1, 0);
    // (perf/shine-whole) A second pass blended in the shader right after its own first pass (after_first, above) is drawn
    // whole, not in runs, unless --native-shine exact (shine_exact). Where two of its triangles that overlap meet at exactly the same depth (a part folded over its own
    // edge) the GS blends both there; drawn whole, only one is. About 180 pixels of a 3x picture at the icy corridor
    // differ (at most 64 levels), and the creatures' runs (about 2,400 a picture, each a copy and two barriers) were
    // most of its cost: 140 pictures a second against 237 (NATIVE_DRAW_P8.md, "The icy corridor"). Modern by default,
    // the PS2's picture with the switch (the owner's call).
    bool shine_whole = after_first && !shine_exact;
    bool split = (npasses > 1 || shader_blend || tie_runs) && !shadow && !shine_whole;
    if (split && memo && memo->split && !memo_check) {
        // (rc/own) where the frame's batch's triangles overlap
        const uint32_t *cuts = memo_frame->sa_u32.data() + memo->runs_at;
        runs.insert(runs.end(), cuts, cuts + memo->runs_n);
    } else if (split) {
        uint32_t triangles = uint32_t(gs->verts.size() / 3), start = 0;
        // (rc/own) each triangle's box once (triangles_overlap's first test, the same floats), not once a pair
        std::vector<std::array<float, 4>> &boxes = sa.tri_boxes;
        boxes.resize(triangles);
        for (uint32_t t = 0; t < triangles; t++) {
            const Vtx *a = &gs->verts[t * 3];
            boxes[t] = {std::min(a[0].x, std::min(a[1].x, a[2].x)), std::max(a[0].x, std::max(a[1].x, a[2].x)),
                        std::min(a[0].y, std::min(a[1].y, a[2].y)), std::max(a[0].y, std::max(a[1].y, a[2].y))};
        }
        for (uint32_t t = 1; t < triangles; t++) {
            bool cut = t - start >= 256;
            const std::array<float, 4> &a = boxes[t];
            for (uint32_t u = start; u < t && !cut; u++) {
                const std::array<float, 4> &b = boxes[u];
                cut = !(a[1] <= b[0] || b[1] <= a[0] || a[3] <= b[2] || b[3] <= a[2]) &&
                      triangles_overlap(&gs->verts[t * 3], &gs->verts[u * 3]);
            }
            if (cut) {
                runs.push_back(t * 3);
                start = t;
            }
        }
        if (memo_check && memo->split &&
            (runs.size() - 1 != memo->runs_n ||
             !std::equal(runs.begin() + 1, runs.end(), memo_frame->sa_u32.begin() + memo->runs_at))) {
            sa_memo.check_differ++, sa_memo.differ[4]++;
        }
    }
    sa_memo.keep.split = split;
    if (sa_memo.keeping && split) {
        sa_memo.keep.runs_at = uint32_t(sa_memo.into->sa_u32.size());
        sa_memo.keep.runs_n = uint32_t(runs.size() - 1);
        sa_memo.into->sa_u32.insert(sa_memo.into->sa_u32.end(), runs.begin() + 1, runs.end());
    }
    runs.push_back(uint32_t(gs->verts.size()));
    size_t bytes = gs->verts.size() * sizeof(Vtx);
    // A layer's colour pass also passes within half its Z's change across a GS pixel more, at most a Z more
    // (--native-layer-z tolerant's slope: at a grazing angle the two meshes, a fraction of a pixel apart, are
    // further apart in Z; a tower's top seen from below). Per triangle, from its corners, in each corner's
    // alpha (which the GPU does not read otherwise): 255 is one Z (sa_000.vert, pc.zslope).
    bool sloped = slack == slack_layer && zwrite_any(zbuf, test) && !same_twice && !second_pass;
    // A layer of the scene's own corners passes by its real depth: within two world units of the surface it lies on,
    // across it, whatever the distance (sa_slack in gs_common.h; the corner's alpha carries the triangle's Z slope
    // instead). In Z (--native-layer-z tolerant's rule above, the GS's units) the same tolerance is a different
    // distance at each depth, as a Z is (Z = cz + az / depth: a Z is depth^2 / az world units): near, one Z (plus
    // the slope term) is a fraction of a unit and a layer's mesh a unit off its surface failed; far it is several,
    // and a model's own faces a few units behind its front came through. So a layer's coverage changed shape with
    // the distance: the rock over the tower's top on map 000 (a mesh cut apart from the bricks', at its edge 1.1
    // units behind them along the view) shrank and flickered walking up to it, and a chest's far end (a face 3.2
    // units behind its end panel along the view) showed through from afar. Two units: the tower's rock holds from
    // far to near (with 0.5 it still shrank, with 1 a notch of bricks stayed in one corner); the chest's far end is
    // its own triangles (below). KF4_SA_WORLD=EPS (tests; 0: the Z rule).
    static float world_eps = -1.0f;
    if (world_eps < 0.0f) {
        const char *v = getenv("KF4_SA_WORLD");
        world_eps = v && *v ? std::max(float(atof(v)), 0.0f) : 2.0f;
    }
    // A skinned model's parts tie exactly, by their real depth (KF4_SA_SKIN=EPS, tests: within EPS world units along the
    // view), not within the tie's half a Z nor a layer's two units. A Z is depth^2 / az units: far off, half a Z is most
    // of a unit, and a seated man is two or three Z deep from one end to the other. So from afar a part drawn after
    // another and lying under it passed over it within the slack: the armour on the arm and leg and the wrist over the
    // cloth drawn over them, and a bright spot on a hand or the neck where a part's shine came through with it (the
    // second pass is exact on its first pass's Z), changing as he rocked (the seated men in the cave by the brazier and
    // by the broken bridge, 4x). Close up the slack is a small fraction of a unit and they looked right. Exact is
    // nearest hosted at every distance tried: even 0.1 units let the shopkeeper's shirt through along his shoulder.
    static float skin_eps = -1.0f;
    if (skin_eps < 0.0f) {
        const char *v = getenv("KF4_SA_SKIN");
        skin_eps = v && *v ? std::max(float(atof(v)), 0.0f) : 0.0f;
    }
    float layer_eps = skinned ? skin_eps : world_eps;
    bool world = sloped && (skinned || world_eps > 0.0f) && !sa.gs3d && !sa.gs_water;
    bool all_native = true;
    if (memo && !memo_check) {
        all_native = memo->all_native;
    } else {
        for (size_t t = 0; all_native && t < gs->verts.size(); t++) {
            all_native = nd_corner_kind(gs->verts[t]) != ND_KIND_PS2;
        }
        sa_memo.differ[5] += memo_check && all_native != memo->all_native;
    }
    sa_memo.keep.all_native = all_native;
    world = world && all_native; // (gs.vert's corners keep the Z rule)
    std::vector<Vtx> &corners = sa.corners; // (kept from batch to batch: their room)
    corners.assign(gs->verts.begin(), gs->verts.end());
    // (rc/own) each triangle's slope as the frame's batch worked it out
    int slopes = sloped ? (world ? 2 : 1) : 0;
    const uint8_t *memo_slopes = memo && !memo_check && slopes && memo->slopes == slopes
                                     ? memo_frame->sa_bytes.data() + memo->slopes_at
                                     : nullptr;
    sa_memo.keep.slopes = slopes;
    if (sa_memo.keeping && slopes) {
        sa_memo.keep.slopes_at = uint32_t(sa_memo.into->sa_bytes.size());
        sa_memo.into->sa_bytes.resize(sa_memo.into->sa_bytes.size() + corners.size() / 3);
    }
    for (size_t t = 0; t + 2 < corners.size(); t += 3) {
        if (nd_corner_kind(corners[t]) == ND_KIND_PS2 || sa.gs_water) {
            continue; // (the PS2 path's: its alpha is its own, gs.vert takes no slack)
        }
        uint32_t extra = 0;
        if (memo_slopes) {
            extra = memo_slopes[t / 3];
        } else if (sloped) {
            const Vtx *v = &corners[t];
            float ax = v[1].x - v[0].x, ay = v[1].y - v[0].y, bx = v[2].x - v[0].x, by = v[2].y - v[0].y;
            float az = float(v[1].z) - float(v[0].z), bz = float(v[2].z) - float(v[0].z);
            float det = ax * by - ay * bx;
            if (det != 0.0f) {
                float dzdx = (az * by - bz * ay) / det, dzdy = (bz * ax - az * bx) / det;
                float e = std::min(0.5f * std::max(fabsf(dzdx), fabsf(dzdy)), 1.0f);
                extra = uint32_t(e * 255.0f + 0.5f);
                if (world) {
                    // (by its real depth: the Z's change across a GS pixel, g, as 32 log2(1 + g): up to 250 Z a pixel)
                    float g = sqrtf(dzdx * dzdx + dzdy * dzdy);
                    extra = uint32_t(std::min(32.0f * log2f(1.0f + g) + 0.5f, 255.0f));
                }
            }
        }
        for (int k = 0; k < 3; k++) {
            corners[t + k].rgba = (corners[t + k].rgba & 0xFFFFFFu) | extra << 24;
        }
        if (sa_memo.keeping && slopes) {
            sa_memo.into->sa_bytes[sa_memo.keep.slopes_at + t / 3] = uint8_t(extra);
        }
        if (memo_check && slopes && memo->slopes == slopes && memo_frame->sa_bytes[memo->slopes_at + t / 3] != uint8_t(extra)) {
            sa_memo.check_differ++, sa_memo.differ[6]++;
        }
    }
    sa_memo_end();
    sa_memo.taken += memo != nullptr;
    // (pf/skin) A skinned model's part whose draws are the simple ones is kept, to be drawn with the model's other
    // parts (sa_group_draw, above); anything else first draws what is kept
    if (sa_group.enabled < 0) {
        const char *v = getenv("KF4_SA_GROUP");
        sa_group.enabled = v && *v ? atoi(v) : 1;
    }
    // no pixel of the batch fails the alpha test: its alpha (the texture's alpha range, as the table has it, with the
    // corners' As) within what the test passes
    bool covers = !ate || atst == 1;
    {
        uint32_t aref = bits(test, 4, 8);
        switch (ate ? atst : 1) {
        case 2:
            covers = a_hi < aref;
            break;
        case 3:
            covers = a_hi <= aref;
            break;
        case 4:
            covers = a_lo == aref && a_hi == aref;
            break;
        case 5:
            covers = a_lo >= aref;
            break;
        case 6:
            covers = a_lo > aref;
            break;
        case 7:
            covers = aref < a_lo || aref > a_hi;
            break;
        }
    }
    // (the switches of blending in the render pass below: KF4_SA_BLEND_COPY=1, KF4_SA_RUN_COPY=0)
    static int blend_copy = -1;
    if (blend_copy < 0) {
        blend_copy = getenv("KF4_SA_BLEND_COPY") && atoi(getenv("KF4_SA_BLEND_COPY")) > 0;
    }
    static int run_copy_on = -1;
    if (run_copy_on < 0) {
        const char *v = getenv("KF4_SA_RUN_COPY");
        run_copy_on = v && *v ? atoi(v) : 1;
    }
    // (perf/shine-whole) a second pass blended in the shader and drawn whole, whose blend is made in the render pass from
    // a copy by its triangles (in_pass and run_copy below): kept, its shine blended with the group's (sa_group_draw)
    // Only single-sampled: with MSAA the copy is each pixel's samples' mean, and drawn part by part a part's copy at a
    // pixel its triangles share with another part's (a seam) has the parts' before it shined there and not the first
    // passes of those after it; kept, the group's copy has the first passes alone. About 1,100-2,100 pixels of a 3x
    // MSAA 2 picture at the icy corridor differ (seam pixels drawn part by part a part's shine brighter, up to 76
    // levels: NATIVE_DRAW_P8.md, "The ice shine drawn whole"). KF4_SA_SHINE_GROUP=0 (tests): never kept; 2: kept with
    // MSAA too.
    static int shine_group = -1;
    if (shine_group < 0) {
        const char *v = getenv("KF4_SA_SHINE_GROUP");
        shine_group = v && *v ? atoi(v) : 2; // (2, kept with MSAA too, by default: the owner's choice, no seam lines)
    }
    int ms_in = sa.samples > 1 ? 1 : 0;
    bool shine_kept = shine_whole && (shine_group == 2 || (shine_group == 1 && !ms_in)) && !blend_copy && run_copy_on && !per_sample && !sa.gs3d && !sa.gs_water && !ndw_batch() &&
                      sa.programs_in[SA_VS_000][ms_in] && sa.programs_in[SA_VS_280][ms_in] &&
                      sa.programs_copy[SA_VS_000][ms_in] && sa.programs_copy[SA_VS_280][ms_in] && !wide(*sa.color) && all_native;
    // (pc world) The map's records the PC renderer takes (gsn_pc_world.inc): kept, and drawn by material when something
    // else comes. runs.size() == 2 is own's test too (own needs a run of its own: more than one run). The texture is the
    // GPU's sampler's, with a program that reads it (never the GS's raw texture bound in its place).
    // A blend that leaves the source (Cs - Cd) As + Cd with As 0x80 at every pixel, as a_lo and a_hi have it: the GS writes
    // Cs, so the PC renderer draws it without blending (the map's records set ABE on their opaque surfaces too)
    bool blend_is_source = abe && ba == 0 && bb == 1 && bc == 0 && bd == 1 && a_lo == 0x80 && a_hi == 0x80;
    bool pc_keep = pc_world_mode && sa.batch_pc && nd.active && !nd.pass_active && !nd.strip_active && !ps2_only &&
                   !gpu_corners && !sa.gs3d && !sa.gs_fill && !sa.gs_water && !ndw_batch() && !sat_bound() &&
                   !shader_blend && !shadow && npasses == 1 && all_native && (!tme || sa.rgba_on) &&
                   (!sa.rgba_on || sa.programs_hw[SA_VS_000][per_sample ? 1 : 0]) && ztst == 2 &&
                   nd.batch_draws == nd.draws.buffer.get() && nd.batch_arena == nd.arena.buffer.get() && nd.batch_arena &&
                   !corners.empty() && pc_map_corners(corners);
    // (a blend Vulkan's blending does as the GS, the batch drawn whole in one draw: its triangles blended and Z tested in
    // their order, as on the GS, without the scene's Z-first draw and runs)
    bool pc_blend = abe && !blend_is_source;
    if (pc_keep) {
        // (the key is made before a skinned group draws: its state changes sa.rgba_on, sa_group_state)
        PcKey k;
        memset(&k, 0, sizeof(k));
        k.ndraws = ndraws;
        for (int i = 0; i < ndraws; i++) {
            Push p = push;
            p.flags |= draws[i].select << F_PASS_SHIFT;
            if (!pc_blend) {
                p.flags &= ~F_ALPHA_FACTOR; // (no blending: the alpha written is As, as the GS writes it)
            }
            p.zbias = draws[i].zbias + (draws[i].zbias > 0.0f || (ties && !draws[i].zwrite) ? ps2_bias : 0.0f);
            p.zslope = sloped && draws[i].zbias > 0.0f ? 1.0f : 0.0f;
            if ((world || (skinned && ties)) && draws[i].zbias > 0.0f) {
                p.zbias = 0.0f;
                p.zslope = -layer_eps;
            }
            k.pushes[i] = p;
            k.masks[i] = draws[i].mask;
            k.zwrites[i] = draws[i].zwrite;
        }
        k.z_written = zwrite;
        k.per_sample = per_sample;
        if (pc_blend) {
            k.blend_on = 1;
            k.alpha_factor = (push.flags & F_ALPHA_FACTOR) != 0;
            k.fix = fix;
            k.src = blend.src;
            k.dst = blend.dst;
            k.op = blend.op;
        }
        k.scissor[0] = int32_t(sx0 * scale_x);
        k.scissor[1] = int32_t(sy0 * scale_y);
        k.scissor[2] = int32_t((x1 - sx0) * scale_x);
        k.scissor[3] = int32_t((y1 - sy0) * scale_y);
        k.viewport[0] = color->width * scale_x;
        k.viewport[1] = color->height * scale_y;
        k.rgba = sa.rgba_on ? sa.rgba_view : nullptr;
        k.sampler = sa.rgba_on ? sa.rgba_sampler : StockSampler::NearestClamp;
        k.records = nd.batch_draws;
        k.arena = nd.batch_arena;
        ImageHandle texture = sa.rgba_on ? sa.rgba_image : ImageHandle();
        sa_group_draw_parts(); // (pf/skin: none is pending with the PC world's kept: sa_group_draw's invariant)
        PcFrame frame = {fbp, fbw, fpsm, zbp, zpsm};
        pc_world_keep(k, frame, texture, nd.draws.buffer, nd.arena.buffer, corners, runs);
        sa.last_shadow = false;
        sa.z_written |= zwrite;
        uint32_t d0 = interp.active ? sy0 : uint32_t(std::max(float(sy0), std::max(0.0f, floorf(vy0) - 1.0f)));
        uint32_t d1 = interp.active ? y1 : uint32_t(std::min(float(y1), std::max(0.0f, ceilf(vy1) + 2.0f)));
        sa.rows0 = std::min(sa.rows0, d0);
        sa.rows1 = std::max(sa.rows1, d1);
        sa.drew = true;
        sa.scissor_rows = std::max(sa.scissor_rows, y1);
        sa_note_first_z(false);
        sa_stats.batches++;
        sa_stats.triangles += gs->verts.size() / 3;
        gsn_stats.primitives += gs->verts.size() / 3;
        gs->verts.clear();
        gs->batch_sprites = false;
        return true;
    }
    int role = 0; // 1: a first pass kept, 2: a second pass kept
    if (sa_group.enabled && sa.in_draw && nd.active && !nd.pass_active && !nd.strip_active && !ps2_only && !gpu_corners &&
        !sa.gs3d && !sa.gs_fill && !sa.gs_water && !ndw_batch() && runs.size() == 2 && (!shader_blend || shine_kept) &&
        !shadow && st.table && !st.reads_target && (ndraws == 2 || (shine_kept && ndraws == 1)) &&
        nd280_batch_draws() == nd280.draws.buffer.get() &&
        nd.batch_arena == nd.arena.buffer.get() && nd.batch_arena) {
        // a first pass that covers every pixel of its triangles: no pixel fails the alpha test (covers)
        if (skinned && two_phase && ztst == 2 && skin_eps == 0.0f && covers && passes[0].mask && sa.stencil) {
            role = 1;
        } else if (second_pass && sa_group.pass[0].on && sa_group.expect_second && corners.size() == sa_group.last_n &&
                   corners[0].fog == sa_group.last_fog[0] && corners.back().fog == sa_group.last_fog[1] &&
                   nd280_batch_draws() == sa_group.records.get() && nd.batch_arena == sa_group.arena.get() && ztst == 2 &&
                   draws[ndraws - 1].mask && !draws[ndraws - 1].zwrite && (ndraws == 1 || (draws[0].mask == 0 && draws[0].zwrite))) {
            role = 2; // (its own first pass's corners, kept just before: its Z pass, then its colour; or its colour alone)
        }
    }
    if (role) {
        SaGroupKey k;
        memset(&k, 0, sizeof(k));
        k.ndraws = ndraws;
        for (int i = 0; i < ndraws; i++) {
            Push p = push;
            p.flags |= draws[i].select << F_PASS_SHIFT;
            p.zbias = draws[i].zbias + (draws[i].zbias > 0.0f || (ties && !draws[i].zwrite) ? ps2_bias : 0.0f);
            p.zslope = sloped && draws[i].zbias > 0.0f ? 1.0f : 0.0f;
            if ((world || (skinned && ties)) && draws[i].zbias > 0.0f) {
                p.zbias = 0.0f;
                p.zslope = -layer_eps;
            }
            k.pushes[i] = p;
            k.masks[i] = draws[i].mask;
            k.zwrites[i] = draws[i].zwrite;
        }
        k.per_sample = per_sample;
        k.ztst = ztst;
        k.blend_on = abe && !shader_blend;
        k.shader_blend = shader_blend;
        if (k.blend_on) {
            k.alpha_factor = (push.flags & F_ALPHA_FACTOR) != 0;
            k.fix = fix;
            k.src = blend.src;
            k.dst = blend.dst;
            k.op = blend.op;
        }
        k.scissor[0] = int32_t(sx0 * scale_x);
        k.scissor[1] = int32_t(sy0 * scale_y);
        k.scissor[2] = int32_t((x1 - sx0) * scale_x);
        k.scissor[3] = int32_t((y1 - sy0) * scale_y);
        k.viewport[0] = color->width * scale_x;
        k.viewport[1] = color->height * scale_y;
        k.raw = st.raw_view ? st.raw_view : &gs->dummy_raw->get_view();
        k.target = st.target_view ? st.target_view : &gs->dummy_rgba->get_view();
        k.records = nd280_batch_draws();
        k.arena = nd.batch_arena;
        k.want_clut = st.want_clut;
        if (st.want_clut) {
            memcpy(k.clut, st.clut, sizeof(k.clut));
        }
        SaGroupPass &g = sa_group.pass[role - 1];
        bool same_frame = sa_group.fbp == fbp && sa_group.fbw == fbw && sa_group.fpsm == fpsm && sa_group.zbp == zbp &&
                          sa_group.zpsm == zpsm;
        if (sa_group.pass[0].on && (!same_frame || (g.on && memcmp(&g.key, &k, sizeof(k)) != 0))) {
            sa_group_draw(); // (not as those kept: they are drawn first; a second pass then goes on as it came)
            if (role == 2) {
                role = 0;
            }
        }
        if (role == 1 && !sa_group.pass[0].on) {
            // a new group: a number of its own in the stencil (after 255, the frame taken again to clear it; a named
            // target's, one its earlier turns did not use: sat_stencil_number)
            if (sat_bound()) {
                if (!sat_stencil_number(&sa_group.gen)) {
                    role = 0;
                }
            } else {
                if (sa.stencil_gen >= 255) {
                    sa_release();
                    if (!sa_acquire(fbp, fbw, fpsm, zbp, zpsm, sy1 + 1) || sa.frame_target != color) {
                        *why = SA_WHY_TARGET;
                        return false;
                    }
                }
                sa.stencil_gen++;
                sa_group.gen = sa.stencil_gen;
            }
            sa_group.fbp = fbp;
            sa_group.fbw = fbw;
            sa_group.fpsm = fpsm;
            sa_group.zbp = zbp;
            sa_group.zpsm = zpsm;
            sa_group.records = nd280.draws.buffer;
            sa_group.arena = nd.arena.buffer;
        }
        if (role) {
            pc_world_flush(); // (pc world: what it kept is drawn before this part is kept)
            if (!g.on) {
                g.key = k;
                g.on = true;
                g.texture = st.table;
            }
            if (role == 1) {
                SaGroupPart part = {{uint32_t(g.corners.size()), 0}, {uint32_t(corners.size()), 0}, {}};
                part.box[0] = part.box[2] = corners[0].x;
                part.box[1] = part.box[3] = corners[0].y;
                for (const Vtx &v : corners) {
                    part.box[0] = std::min(part.box[0], v.x);
                    part.box[1] = std::min(part.box[1], v.y);
                    part.box[2] = std::max(part.box[2], v.x);
                    part.box[3] = std::max(part.box[3], v.y);
                }
                sa_group.parts.push_back(part);
            } else {
                sa_group.parts.back().first[1] = uint32_t(g.corners.size());
                sa_group.parts.back().count[1] = uint32_t(corners.size());
            }
            g.corners.insert(g.corners.end(), corners.begin(), corners.end());
            sa_group.parts_kept++;
            sa_group.expect_second = role == 1;
            sa_group.last_n = uint32_t(corners.size());
            sa_group.last_fog[0] = corners[0].fog;
            sa_group.last_fog[1] = corners.back().fog;
            sa.last_shadow = false;
            sa.z_written |= zwrite;
            uint32_t d0 = interp.active ? sy0 : uint32_t(std::max(float(sy0), std::max(0.0f, floorf(vy0) - 1.0f)));
            uint32_t d1 = interp.active ? y1 : uint32_t(std::min(float(y1), std::max(0.0f, ceilf(vy1) + 2.0f)));
            sa.rows0 = std::min(sa.rows0, d0);
            sa.rows1 = std::max(sa.rows1, d1);
            sa.drew = true;
            sa.scissor_rows = std::max(sa.scissor_rows, y1);
            sa_note_first_z(skinned && two_phase && ztst == 2 && covers && !shadow);
            sa_stats.batches++;
            sa_stats.triangles += gs->verts.size() / 3;
            gsn_stats.primitives += gs->verts.size() / 3;
            gs->verts.clear();
            gs->batch_sprites = false;
            return true;
        }
    } else {
        sa_group_draw();
    }
    pc_world_flush(); // (pc world: a batch the scene draws comes after what it kept)
    // a run of shadow batches: a number of its own in the stencil (cleared when the scene took the frame; after
    // 255 runs the frame taken again, to clear it)
    if (shadow && !sa.last_shadow && sa.stencil_gen >= 255) {
        sa_release();
        if (!sa_acquire(fbp, fbw, fpsm, zbp, zpsm, sy1 + 1) || sa.frame_target != color) {
            *why = SA_WHY_TARGET;
            return false;
        }
    }
    if (shadow && !sa.last_shadow) {
        sa.stencil_gen++;
    }
    // A layer by its real depth drawn in runs (its triangles over its own) is exact over its own triangles: a unit
    // lets a model's faces behind its front through at its silhouettes (the logs of the tower's campfire on map 000,
    // light seams along their edges; the chest's far end), where the GS's Z, rounded near, kept them apart. Each run's Z pass marks the stencil
    // with the batch's number where it wrote; its colour then passes exactly there (its own triangles: those of the
    // runs before, and its own Z) and by the world unit elsewhere (the surface it lies on). A number of its own, as
    // a run of shadow batches has (after 255, the frame taken again to clear it).
    bool own = world && runs.size() > 2 && sa.stencil && !sat_bound() && !shadow;
    if (own && sa.stencil_gen >= 255) {
        sa_release();
        if (!sa_acquire(fbp, fbw, fpsm, zbp, zpsm, sy1 + 1) || sa.frame_target != color) {
            *why = SA_WHY_TARGET;
            return false;
        }
    }
    if (own) {
        sa.stencil_gen++;
        sa_stats.own_layers++;
    }
    sa.last_shadow = shadow;
    sa_stats.shadow_batches += shadow;
    // (T8) Blending in the shader from the destination copied in the render pass (flush_batch's COPY_IN_PASS: the
    // pass's scratch attachment, read as an input attachment), not from a copy or an MSAA resolve between passes:
    // each run's copy ended the pass, with barriers around the copy, about 160 times a picture on map 009's poison
    // pond (its miners and zombies, As above 0x80, in the frame and in the reflection). Only the scene's own corners
    // (sa_000, sa_280 and sa_700 with gs_in.frag); a multisampled scene's samples are resolved into the scratch by
    // pass_resolve.frag (each channel the mean, half up; NVIDIA's vkCmdResolveImage rounds a half either way: a level,
    // where the two samples differ by an odd amount). KF4_SA_BLEND_COPY=1 (tests): the copies between passes.
    bool in_pass = shader_blend && !blend_copy && !per_sample && nd.active && !nd.pass_active && !nd.strip_active &&
                   !sa.gs3d && !sa.gs_water && !ndw_batch() && sa.programs_in[0][sa.samples > 1 ? 1 : 0] &&
                   !wide(*sa.color);
    in_pass = in_pass && all_native;
    bool copied_in_pass = false;
    uint64_t bound_pass = UINT64_MAX; // (the render pass the run's bindings were made in)
    int gpu_refl = sat_bound() ? GPU_SA_REFL - GPU_SA_DRAW : 0; // (KF4_GSN_GPU_TIME=2)
    int gpu_kind = (ps2_only || sa.gs3d || sa.gs_fill || sa.gs_water) ? GPU_SA_OP
                   : (shadow ? GPU_SA_SHADOW : shader_blend ? GPU_SA_BLEND_DRAW : per_sample ? GPU_SA_SAMPLE : GPU_SA_DRAW) + gpu_refl;
    // The copy a run blends from, where the run draws: made by the run's own triangles, with its vertex shader (so in a
    // picture between where they are moved to), rasterized conservatively: every pixel a triangle of it touches, all its
    // samples, the target's pixels as pass_copy.frag and pass_resolve.frag make them (sa_copy_run.frag). The run's draws
    // read the copy only at pixels its triangles cover, all of them within it: the same picture as the copy over the
    // run's box. In a picture between that box is the whole scene for a skinned model's run (the GPU moves its corners:
    // nd_box_at_0 cannot say where they are), so each run of the pond's blended zombies copied the whole scene (3.2 ms
    // of the GPU's 8.4 a picture). KF4_SA_RUN_COPY=0: the box (to compare).
    bool run_copy = in_pass && run_copy_on && sa.programs_copy[0][sa.samples > 1 ? 1 : 0];
    // The Z-first draws by depth alone. A tie's Z-first draw (no colour, Z written) of a pass that no pixel can fail
    // (no alpha test, or covers: the texture's alpha range, from the table, within what the test passes), with no DATE
    // and no SCANMSK, is drawn with sa_zfirst.frag: the same vertex shader, so the same pixels and depth, and nothing
    // the fragment shader could discard. gs.frag's discard (never taken there) made such a draw test and write its
    // depth late, after the shader, and the colour draw after it over the same pixels wait for it (pond, 3x MSAA 2:
    // NATIVE_DRAW_P8.md). Not the scene's ops, the PS2 path's corners (gs.vert's stretches keep gs.frag), the water,
    // the screen passes, the shadows. KF4_SA_ZFIRST=0: gs.frag's (to compare).
    static int zfirst_on = -1;
    if (zfirst_on < 0) {
        const char *v = getenv("KF4_SA_ZFIRST");
        zfirst_on = v && *v ? atoi(v) : 1;
    }
    bool zfirst_alone = zfirst_on && sa.programs_z[0] && !shadow && !(push.flags & F_DATE) &&
                        bits(push.misc, M_SCANMSK_SHIFT, 2) < 2 && gpu_kind != GPU_SA_OP && !nd.pass_active &&
                        !nd.strip_active && !sa.gs3d && !sa.gs_water && !ndw_batch();
    // the run's bindings (textures, the CLUT, the corners, the buffers): for the render pass under way
    auto bind_run = [&](Cmd &c) {
        c.set_texture(0, 0, st.raw_view ? *st.raw_view : gs->dummy_raw->get_view(), StockSampler::NearestClamp);
        c.set_texture(0, 1, st.target_view ? *st.target_view : gs->dummy_rgba->get_view(), StockSampler::NearestClamp);
        if (sa.rgba_on) {
            c.set_texture(0, 6, *sa.rgba_view, sa.rgba_sampler); // (gsn_rgba.inc: the GSN_HW_SAMPLER programs)
        }
        if (in_pass) {
            c.set_input_attachments(0, 4); // (the destination: the pass's scratch, gs_in.frag / gs_in_ms.frag)
        } else {
            c.set_texture(0, 3, shader_blend ? sa.copy->get_view() : gs->dummy_rgba->get_view(), StockSampler::NearestClamp);
        }
        static const uint32_t no_clut[256] = {};
        c.set_storage_data(0, 2, st.want_clut ? st.clut : no_clut, 1024);
        if (gpu_corners) {
            c.set_vertex_binding(0, *interp.vertices, VkDeviceSize(corners_at) * sizeof(Vtx), sizeof(Vtx));
        } else {
            void *vdata = c.allocate_vertex_data(0, bytes, sizeof(Vtx));
            memcpy(vdata, corners.data(), bytes);
        }
        c.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vtx, x));
        c.set_vertex_attrib(1, 0, VK_FORMAT_R32_UINT, offsetof(Vtx, z));
        c.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vtx, rgba));
        c.set_vertex_attrib(3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, s));
        c.set_vertex_attrib(4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vtx, fog));
        if (nd.active) {
            nd_bind(c); // (the arena: not for a batch of the PS2 path's corners only, whose buffers may be gone)
        }
    };
    for (size_t r = 0; r + 1 < runs.size(); r++) {
        uint32_t first = runs[r], count = runs[r + 1] - runs[r];
        if (shader_blend && run_copy) {
            sa_begin_pass(true);
            gpu_enter(interp.active ? (gpu_refl ? GPU_REFL_COPY_BETWEEN : GPU_SA_COPY_BETWEEN) : GPU_SA_BLEND_COPY + gpu_refl);
            Cmd &c = cmd();
            if (bound_pass != pass_serial) {
                bind_run(c);
                bound_pass = pass_serial;
            }
            in_pass_barrier();
            c.set_opaque_state();
            c.set_conservative(true);
            c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            c.set_cull_mode(VK_CULL_MODE_NONE);
            c.set_viewport({0.0f, 0.0f, float(color->width * scale_x), float(color->height * scale_y), 0.0f, 1.0f});
            c.set_scissor({{int32_t(sx0 * scale_x), int32_t(sy0 * scale_y)}, {(x1 - sx0) * scale_x, (y1 - sy0) * scale_y}});
            c.set_depth_test(false, false);
            c.set_blend_enable(false);
            c.set_color_write_mask(0xF0); // (the scratch only)
            Push p = push;
            p.flags |= draws[0].select << F_PASS_SHIFT;
            p.src_offset[0] = int32_t(sa.samples);
            p.lod_k = 255.0f;
            c.push_constants(&p, 0, sizeof(p));
            sa_draw_range(c, false, count, first, in_pass, true);
            in_pass_barrier();
            gsn_stats.blend_copies_in_pass++;
            copied_in_pass = true;
        } else if (shader_blend) {
            // the run's pixels as they are now, into the copy it blends with
            int32_t box[4] = {0, 0, int32_t(color->width), int32_t(color->height)};
            if (!interp.active || (nd.active && !nd.pass_active && !gpu_corners)) {
                run_box(first, count, box); // (a picture between's: at t 0 too, nd_box_at_0's, whole where not known)
            }
            uint32_t W = sa.color->get_width(), H = sa.color->get_height();
            int32_t x0 = std::max(box[0], 0) * int32_t(scale_x), y0 = std::max(box[1], 0) * int32_t(scale_y);
            int32_t bx1 = std::min(box[2] * int32_t(scale_x), int32_t(W)), by1 = std::min(box[3] * int32_t(scale_y), int32_t(H));
            if (!sa.copy || sa.copy->get_width() != W || sa.copy->get_height() != H ||
                sa.copy->get_format() != sa.color->get_format()) {
                sa.copy = sa_image(W, H, sa.color->get_format(), 1);
            }
            if (bx1 > x0 && by1 > y0 && in_pass) {
                // (T8) in the render pass: the run's pixels into the pass's scratch, as flush_batch's COPY_IN_PASS
                sa_begin_pass(true);
                gpu_enter(interp.active ? (gpu_refl ? GPU_REFL_COPY_BETWEEN : GPU_SA_COPY_BETWEEN) : GPU_SA_BLEND_COPY + gpu_refl);
                Cmd &c = cmd();
                in_pass_barrier();
                c.set_opaque_state();
                c.set_program(sa.samples > 1 ? gs->program_pass_resolve : gs->program_pass_copy);
                c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
                c.set_cull_mode(VK_CULL_MODE_NONE);
                c.set_depth_test(false, false);
                c.set_blend_enable(false);
                c.set_color_write_mask(0xF0); // (the scratch only)
                c.set_viewport({0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f});
                c.set_scissor({{x0, y0}, {uint32_t(bx1 - x0), uint32_t(by1 - y0)}});
                c.set_input_attachments(0, 4);
                if (sa.samples > 1) {
                    struct {
                        int32_t samples;
                        float scale;
                    } pp = {int32_t(sa.samples), 255.0f};
                    c.push_constants(&pp, 0, sizeof(pp));
                }
                c.draw(3);
                in_pass_barrier();
                gsn_stats.blend_copies_in_pass++;
                gsn_stats.copy_bytes += uint64_t(bx1 - x0) * uint64_t(by1 - y0) * 4;
                copied_in_pass = true;
            } else if (bx1 > x0 && by1 > y0) {
                Why why_(GSN_WHY_BLEND);
                if (sa.samples > 1) {
                    msaa_resolve(*sa.copy, *sa.color, {x0, y0, 0}, {uint32_t(bx1 - x0), uint32_t(by1 - y0), 1});
                } else {
                    end_pass();
                    transfer_barrier();
                    cmd().copy_image(*sa.copy, *sa.color, {x0, y0, 0}, {x0, y0, 0}, {uint32_t(bx1 - x0), uint32_t(by1 - y0), 1},
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
                    transfer_barrier();
                }
                gsn_stats.copy_bytes += uint64_t(bx1 - x0) * uint64_t(by1 - y0) * 4;
            }
        }
        bool fresh = !(gs->in_pass && gs->pass_color == &SA_MARKER) || copied_in_pass;
        copied_in_pass = false;
        sa_begin_pass(in_pass);
        Cmd &c = cmd();
        if (fresh || r == 0) {
            // (pf/skin) a run after the copy it blends from, in the same render pass: the textures, the CLUT, the
            // corners and the buffers bound for the run before are still bound (a pass's bindings last to its end):
            // only the pipeline's state again, which the copy changed
            bool keep = bound_pass == pass_serial;
            c.set_opaque_state();
            c.set_program(sa.programs[SA_VS_000][0]); // (each stretch's own: sa_draw_range)
            c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            c.set_cull_mode(VK_CULL_MODE_NONE);
            VkViewport vp = {0.0f, 0.0f, float(color->width * scale_x), float(color->height * scale_y), 0.0f, 1.0f};
            c.set_viewport(vp);
            c.set_scissor({{int32_t(sx0 * scale_x), int32_t(sy0 * scale_y)}, {(x1 - sx0) * scale_x, (y1 - sy0) * scale_y}});
            c.set_depth_compare(ztst == 2 ? VK_COMPARE_OP_GREATER_OR_EQUAL : ztst == 3 ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_ALWAYS);
            if (abe && !shader_blend) {
                float constants[4] = {float(fix) / 128.0f, float(fix) / 128.0f, float(fix) / 128.0f, 128.0f / 255.0f};
                c.set_blend_enable(true);
                c.set_blend_constants(constants);
                bool alpha_factor = (push.flags & F_ALPHA_FACTOR) != 0;
                c.set_blend_factors(blend.src, alpha_factor ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE, blend.dst,
                                    VK_BLEND_FACTOR_ZERO);
                c.set_blend_op(blend.op, VK_BLEND_OP_ADD);
            } else {
                c.set_blend_enable(false);
            }
            if (!keep) {
                bind_run(c);
            }
            bound_pass = pass_serial;
        }
        for (int i = 0; i < ndraws; i++) {
            Push p = push;
            p.flags |= draws[i].select << F_PASS_SHIFT;
            p.zbias = draws[i].zbias + (draws[i].zbias > 0.0f || (ties && !draws[i].zwrite) ? ps2_bias : 0.0f);
            p.zslope = sloped && draws[i].zbias > 0.0f ? 1.0f : 0.0f;
            if ((world || (skinned && ties)) && draws[i].zbias > 0.0f) {
                p.zbias = 0.0f; // (by its real depth: sa_slack's world units, no Z)
                p.zslope = -layer_eps; // (0: exact)
            }
            bool zonly = zfirst_alone && draws[i].mask == 0 && draws[i].zwrite && (draws[i].select == 0 || (draws[i].select == 1 && covers));
            sa_stats.zfirst_alone += zonly;
            gpu_enter(draws[i].mask == 0 && gpu_kind != GPU_SA_OP && !shadow
                          ? (draws[i].select && !zonly ? (gpu_refl ? GPU_REFL_ZFIRST_ATEST : GPU_SA_ZFIRST_ATEST) : (gpu_refl ? GPU_REFL_ZFIRST : GPU_SA_ZFIRST))
                          : gpu_kind);
            c.set_color_write_mask(draws[i].mask);
            c.set_depth_test(true, draws[i].zwrite);
            if (shadow) {
                // (where the stencil is not this run's number; a colour draw writes it there, the Z-first one not)
                c.set_stencil(true, VK_COMPARE_OP_NOT_EQUAL, draws[i].mask ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP,
                              sa.stencil_gen, 0xFF);
            }
            if (own && draws[i].zwrite) {
                c.set_stencil(true, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, sa.stencil_gen, 0xFF); // (where it wrote Z)
            } else if (own) {
                // its own triangles' pixels exactly, then the others by the world unit
                Push e = p;
                e.zbias = 0.0f;
                e.zslope = 0.0f;
                c.set_stencil(true, VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_KEEP, sa.stencil_gen, 0xFF);
                c.push_constants(&e, 0, sizeof(e));
                sa_draw_range(c, per_sample, count, first, in_pass);
                sa_stats.draws++;
                gsn_stats.draws++;
                c.set_stencil(true, VK_COMPARE_OP_NOT_EQUAL, VK_STENCIL_OP_KEEP, sa.stencil_gen, 0xFF);
            }
            c.push_constants(&p, 0, sizeof(p));
            sa_draw_range(c, per_sample, count, first, in_pass, false, zonly);
            sa_stats.draws++;
            gsn_stats.draws++;
        }
        if (own) {
            c.set_stencil(false, VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_KEEP, 0, 0xFF);
        }
    }
    sa.z_written |= zwrite;
    // the rows it can have drawn into (a picture between's: anywhere within the scissor)
    uint32_t d0 = interp.active ? sy0 : uint32_t(std::max(float(sy0), std::max(0.0f, floorf(vy0) - 1.0f)));
    uint32_t d1 = interp.active ? y1 : uint32_t(std::min(float(y1), std::max(0.0f, ceilf(vy1) + 2.0f)));
    sa.rows0 = std::min(sa.rows0, d0);
    sa.rows1 = std::max(sa.rows1, d1);
    sa.drew = true;
    sa.scissor_rows = std::max(sa.scissor_rows, y1);
    sa_note_first_z(skinned && two_phase && ztst == 2 && covers && !shadow);
    sa_stats.batches++;
    sa_stats.triangles += gs->verts.size() / 3;
    gsn_stats.primitives += gs->verts.size() / 3;
    gs->verts.clear();
    gs->batch_sprites = false;
    return true;
}

// (flush_batch, at its top, after NdFlush) A batch of the scene's corners is drawn into the scene; anything
// else the GS draws, the scene's turn over first. True: drawn (or nothing to draw).
void sa_note_gs_batch();
bool sa_flush() {
    if (!sa.on) {
        return false;
    }
    // (perf/icy) the batch before's first pass, for this one only
    sa.first_z_before.swap(sa.first_z);
    sa.first_z_before_on = sa.first_z_on;
    sa.first_z_on = false;
    if (interp.active) {
        // a batch into the reflection in a picture between that leaves it as the frame drew it (sat_kept): not drawn
        uint32_t ctxt = (gs->batch_prim >> 9) & 1;
        uint64_t frame = gs->reg[R_FRAME_1 + ctxt];
        if (sat_kept(bits(frame, 0, 9) * 32, bits(frame, 16, 6), bits(frame, 24, 6))) {
            sa_group_draw(); // (as a batch of another target draws the parts kept)
            gs->verts.clear();
            gs->batch_sprites = false;
            sa_stats.kept_reflection++;
            return true;
        }
    }
    sa.fcopy.fresh = false; // (the copy of the frame made: what writes there from now on lets it go)
    // (the batch of a draw of the scene's: its corners, or the trail's, the PS2 path's made by the draw; not a
    // screen pass's)
    bool scene = !nd.pass_active && (sa.in_draw ? sa.draw_on : sa.pending);
    sa.batch_pc = scene && (sa.in_draw ? sa.pc_take : sa.pending_pc);
    sa.pending = false;
    if ((!scene || !sa.in_draw) && sa.batch_pc) {
        sa_group_draw_parts(); // (pc world: a taken draw's corners: what the PC renderer keeps goes on being kept)
    } else if (!scene || !sa.in_draw) {
        sa_group_draw(); // (pf/skin: kept only from draw to draw)
    }
    int op = scene ? SA_OP_NONE : sa_scene_op();
    if (!scene && op == SA_OP_NONE && sat_fill()) {
        op = SA_OP_FILL; // (T3: a fill over the frame the scene has, as mode 1's tint between its two views)
    }
    if (op == SA_OP_CLEAR_DEPTH) {
        sa_stats.depth_clears++;
    } else if (op == SA_OP_ALPHA) {
        sa_stats.alpha_sets++;
    }
    if (!scene && op == SA_OP_NONE && sat_flush()) {
        return true; // (T3: a named target's clear, the scene's)
    }
    if (scene || op != SA_OP_NONE) {
        int why = SA_WHY_FRAME;
        bool drawn = sa_draw_batch(&why);
        sa.batch_pc = false;
        if (drawn) {
            return true;
        }
        sa_stats.declined[why]++;
        if (sa.log == 1) {
            fprintf(stderr, "[sa] declined: %s\n", sa_why_names[why]);
        }
    }
    // (T2) the GS's batches the scene draws itself: its 3D, the screen passes, fills over the screen
    int gs_kind = scene || op != SA_OP_NONE ? SA_GS_NONE : sa_gs_batch();
    if (gs_kind != SA_GS_NONE) {
        sa.gs3d = gs_kind == SA_GS_3D;
        sa.gs_fill = gs_kind == SA_GS_FILL;
        sa.gs_water = gs_kind == SA_GS_WATER;
        int why = SA_WHY_FRAME;
        bool drawn = sa_draw_batch(&why);
        sa.gs3d = sa.gs_fill = sa.gs_water = false;
        if (drawn) {
            (gs_kind == SA_GS_3D      ? sa_stats.gs_3d
             : gs_kind == SA_GS_PASS  ? sa_stats.gs_passes
             : gs_kind == SA_GS_WATER ? sa_stats.gs_water
                                      : sa_stats.gs_fills)++;
            if (sa.log == 3) {
                uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
                fprintf(stderr, "[sa] the GS's %s into the scene: prim %#x frame %#llx test %#llx alpha %#llx tex0 %#llx n %zu\n",
                        gs_kind == SA_GS_3D ? "3D" : gs_kind == SA_GS_PASS ? "pass" : "fill", prim,
                        (unsigned long long)gs->reg[R_FRAME_1 + ctxt], (unsigned long long)gs->reg[R_TEST_1 + ctxt],
                        (unsigned long long)gs->reg[R_ALPHA_1 + ctxt], (unsigned long long)gs->reg[R_TEX0_1 + ctxt],
                        gs->verts.size());
            }
            return true;
        }
        sa_stats.declined[why]++;
        if (sa.log == 1) {
            fprintf(stderr, "[sa] the GS's declined: %s\n", sa_why_names[why]);
        }
    }
    sat_gs_batch();     // (T3: a named target the scene drew that the batch touches is the GS's first)
    sa_note_gs_batch(); // (the GS draws it: its clear, and what it draws over a frame the scene gave it: gsn_composite.inc)
    if (sa.bound && !sa_gs_touches_scene()) {
        if (sa.log == 3) {
            uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
            fprintf(stderr, "[sa] the GS's elsewhere: prim %#x frame %#llx zbuf %#llx test %#llx tex0 %#llx n %zu\n", prim,
                    (unsigned long long)gs->reg[R_FRAME_1 + ctxt], (unsigned long long)gs->reg[R_ZBUF_1 + ctxt],
                    (unsigned long long)gs->reg[R_TEST_1 + ctxt], (unsigned long long)gs->reg[R_TEX0_1 + ctxt], gs->verts.size());
        }
        return false; // (the GS's, elsewhere in VRAM: the scene keeps the frame)
    }
    if (sa.log && sa.bound) {
        uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
        fprintf(stderr, "[sa] turn ends: a batch of the GS's: prim %#x frame %#llx test %#llx alpha %#llx tex0 %#llx "
                        "sprites %d pass %d native %d z %u n %zu\n",
                prim, (unsigned long long)gs->reg[R_FRAME_1 + ctxt], (unsigned long long)gs->reg[R_TEST_1 + ctxt],
                (unsigned long long)gs->reg[R_ALPHA_1 + ctxt], (unsigned long long)gs->reg[R_TEX0_1 + ctxt],
                gs->batch_sprites, nd.pass_active, nd.active, gs->verts.empty() ? 0u : gs->verts[0].z, gs->verts.size());
    }
    sa_release();
    return false;
}

// ---- The draws met (nd_register) -----------------------------------------------------------------------
bool sa_kind(const RdDraw &d) {
    bool program = d.program == 0 || d.program == 0x280 || d.program == 0x700 || d.program == 0x5A0 ||
                   d.program == RD_PROGRAM_GS_STRIP || d.program == RD_PROGRAM_RIPPLE;
    // (T5: the modern sky at the far plane, program 0x000's records: sa_000.vert gives them depth 0)
    uint32_t allowed = RD_DRAW_WAVE_UVS | (d.program == 0 ? uint32_t(RD_DRAW_FAR_Z) : 0u);
    return program && d.kind < 32 && ((sa.kinds >> d.kind) & 1) && (d.flags & ~allowed) == 0;
}

// Before a draw is met: whether it goes to the scene; a batch holds the scene's corners or none
void sa_draw_begin(uint64_t value) {
    sa.in_draw = false;
    sa.pc_take = false;
    if (!sa.on) {
        return;
    }
    RdDraw d;
    RdCamera cam;
    // (sa's, not a static here: one made on the GS's thread, after main's atexit(print_stats), was gone at
    // the exit while the GS's thread still drew: the heap broken at the end of a modern run)
    bool scene = rd_fetch(value, d, cam, sa.fetched) && sa_kind(d);
    sa.fetched.clear();
    sa.pc_take = pc_world_mode && scene && pc_world_kind(d.kind) && d.program == 0 && !(d.flags & RD_DRAW_FAR_Z);
    if (sa.pc_take) {
        sa_group_draw_parts(); // (pc world: the map's records are kept by the PC renderer, not drawn after the parts)
    } else if (!scene || d.program != 0x280) {
        sa_group_draw(); // (pf/skin: a model's parts are kept from one of its draws to the next)
    }
    if (!gs->verts.empty() && (scene != sa.pending || (sa.pending && sa.pc_take != sa.pending_pc))) {
        flush_batch();
    }
    sa.in_draw = true;
    sa.draw_on = scene;
}
void sa_draw_end() {
    if (!sa.on) {
        return;
    }
    sa.in_draw = false;
    sa.pending = !gs->verts.empty() && sa.draw_on;
    sa.pending_pc = sa.pending && sa.pc_take;
}

// (nd_kick) A PS2-path kick while the scene's corners wait: they are drawn first
bool sa_waiting() {
    return sa.on && sa.pending && !gs->verts.empty();
}

// (T2: nd_pass_draw, around a screen pass's drawing) Its batches are the pass's: drawn into the scene while it has the
// frame (sa_gs_batch), each its own batch
void sa_pass(bool begin) {
    if (!sa.on) {
        return;
    }
    flush_batch();
    sa.in_pass = begin;
}

// (begin_transfer) A transfer: the scene's turn ends when it can write the frame or Z buffer the scene has taken (an
// upload, or a copy, into them). One that only reads them (a copy of the frame elsewhere: GsDraw_CopyFrame's for the
// wobble, the menus' backdrop; a read-back) is given them, and the scene keeps the frame (T2: sa_give). An upload
// of a texture elsewhere in VRAM, as the models' and effects' are between the world's draws, leaves them alone: the
// scene's textures are the table's, and the GS's draws read VRAM as before. (And one that writes the frame or Z
// buffer the GS's clear is waiting in: the scene takes that frame by copying it, not clearing.)
void sa_transfer(uint32_t dir) {
    if (!sa.on) {
        return;
    }
    bool named_bound = sat_transfer(dir); // (T3: the named targets it touches; a named target bound is not the frame's turn)
    uint64_t bitbltbuf = gs->reg[R_BITBLTBUF], trxpos = gs->reg[R_TRXPOS], trxreg = gs->reg[R_TRXREG];
    uint32_t w = bits(trxreg, 0, 12), h = bits(trxreg, 32, 12);
    auto touches = [&](uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t fbp, uint32_t fbw,
                       uint32_t fpsm, uint32_t zbp, uint32_t zpsm, uint32_t rows) {
        uint32_t lo, count, tlo, tcount;
        buffer_pages(bp, bw, psm, x + w, y + h, &lo, &count);
        buffer_pages(fbp, fbw, fpsm, std::max(fbw, 1u) * 64, rows, &tlo, &tcount);
        if (pages_overlap(lo, count, tlo, tcount)) {
            return true;
        }
        buffer_pages(zbp, fbw, zpsm, std::max(fbw, 1u) * 64, rows, &tlo, &tcount);
        return pages_overlap(lo, count, tlo, tcount);
    };
    auto dst_touches = [&](uint32_t fbp, uint32_t fbw, uint32_t fpsm, uint32_t zbp, uint32_t zpsm, uint32_t rows) {
        return (dir == 0 || dir == 2) && touches(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), bits(bitbltbuf, 56, 6),
                                                 bits(trxpos, 32, 11), bits(trxpos, 48, 11), fbp, fbw, fpsm, zbp, zpsm, rows);
    };
    auto src_touches = [&](uint32_t fbp, uint32_t fbw, uint32_t fpsm, uint32_t zbp, uint32_t zpsm, uint32_t rows) {
        return (dir == 1 || dir == 2) && touches(bits(bitbltbuf, 0, 14), bits(bitbltbuf, 16, 6), bits(bitbltbuf, 24, 6),
                                                 bits(trxpos, 0, 11), bits(trxpos, 16, 11), fbp, fbw, fpsm, zbp, zpsm, rows);
    };
    sa.fcopy.fresh = false;
    const Sa::Clear &k = sa.clear;
    if (k.valid && (dir > 2 || dst_touches(k.fbp, k.fbw, k.fpsm, k.zbp, k.zpsm, k.rows))) {
        sa.clear.valid = false;
        if (sa.log) {
            fprintf(stderr, "[sa] the GS's clear written over: a transfer, dir %u\n", dir);
        }
    }
    if (!sa.bound || named_bound) {
        return;
    }
    bool held = find_target(sa.fbp, sa.fbw, sa.fpsm) == sa.frame_target && find_target(sa.zbp, sa.fbw, sa.zpsm) == sa.z_target;
    uint32_t rows = std::max(sa.frame_target->height, sa.z_target->height);
    if (!held || dir > 2 || dst_touches(sa.fbp, sa.fbw, sa.fpsm, sa.zbp, sa.zpsm, rows)) {
        sa_sync();
    } else if (src_touches(sa.fbp, sa.fbw, sa.fpsm, sa.zbp, sa.zpsm, rows)) {
        if (sa.log) {
            fprintf(stderr, "[sa] the GS reads the frame (a copy, a read-back): given it, the scene keeps it: dir %u bitbltbuf %#llx "
                            "trxpos %#llx trxreg %#llx\n",
                    dir, (unsigned long long)bitbltbuf, (unsigned long long)trxpos, (unsigned long long)trxreg);
        }
        sa_give(true);
        // (T2) a copy of the frame elsewhere: the scene's picture kept for what reads it there
        uint32_t ssax = bits(trxpos, 0, 11), ssay = bits(trxpos, 16, 11);
        if (dir == 2 && bits(bitbltbuf, 0, 14) == sa.fbp && bits(bitbltbuf, 16, 6) == sa.fbw && bits(bitbltbuf, 24, 6) == sa.fpsm &&
            bits(bitbltbuf, 56, 6) == sa.fpsm && ssax + w <= sa.frame_target->width &&
            ssay + h <= sa.frame_target->height) {
            Sa::Copy &k2 = sa.fcopy;
            uint32_t W = sa.color->get_width(), H = sa.color->get_height();
            if (!k2.image || k2.image->get_width() != W || k2.image->get_height() != H || k2.image->get_format() != sa.color->get_format()) {
                k2.image = sa_image(W, H, sa.color->get_format(), 1);
            }
            {
                Why why_(GSN_WHY_COPY);
                end_pass();
            }
            if (sa.samples > 1) {
                msaa_resolve(*k2.image, *sa.color, {0, 0, 0}, {W, H, 1});
            } else {
                transfer_barrier();
                cmd().copy_image(*k2.image, *sa.color, {0, 0, 0}, {0, 0, 0}, {W, H, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
                transfer_barrier();
            }
            k2.valid = k2.fresh = true;
            k2.dbp = bits(bitbltbuf, 32, 14);
            k2.dbw = bits(bitbltbuf, 48, 6);
            k2.dpsm = bits(bitbltbuf, 56, 6);
            k2.dx = bits(trxpos, 32, 11);
            k2.dy = bits(trxpos, 48, 11);
            k2.sx = ssax;
            k2.sy = ssay;
            k2.w = w;
            k2.h = h;
            k2.scale_x = sa.sx;
            k2.scale_y = sa.sy;
            sa_stats.frame_copies++;
        }
    }
}

// (vsync_now, take_snapshot, gsn_read_rect ...) The GS wants its targets: the scene's turn ends
void sa_sync() {
    if (sa.on) {
        if (sa.log && sa.bound) {
            fprintf(stderr, "[sa] turn ends: the GS's targets wanted (an upload, a copy, a picture ...)\n");
        }
        sa_release();
        sat_sync(); // (T3: the named targets the scene drew)
    }
}

void sa_held_forget(); // (gsn_composite.inc)
// (gsn_set_standalone, at a vblank) on or off
void sa_switch(bool on) {
    if (on == sa.on) {
        return;
    }
    sa_release();
    sa.on = on;
    sa.pending = sa.in_draw = false;
    if (on) {
        sa.log = getenv("KF4_SA_LOG") ? atoi(getenv("KF4_SA_LOG")) : 0;
        if (const char *px = getenv("KF4_SA_PIXEL")) {
            sa.pixel[0] = float(atof(px));
            sa.pixel[1] = strchr(px, ',') ? float(atof(strchr(px, ',') + 1)) : sa.pixel[0];
        }
        const char *kinds = getenv("KF4_SA_KINDS");
        if (kinds && *kinds) {
            sa.kinds = uint32_t(strtoul(kinds, nullptr, 0));
        }
        // the scene's depth with a stencil, for the shadows (KF4_SA_NO_STENCIL=1, tests: without, DATE as the GS)
        const char *no_stencil = getenv("KF4_SA_NO_STENCIL");
        sa.stencil = !(no_stencil && atoi(no_stencil) > 0) &&
                     gs->device->image_format_is_supported(VK_FORMAT_D32_SFLOAT_S8_UINT,
                                                           VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                               VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT);
        VkFormat format = sa.stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
        if (format != sa.depth_format) {
            sa.depth_format = format;
            sa.color.reset(); // (the targets made again at the next acquire)
            sa.depth.reset();
        }
        sa_table_reset();
        sat_reset(); // (T3)
    }
    sa.clear.valid = false;
    sa.fcopy.valid = false;
    sa.in_pass = false;
    sa_held_forget();
}

// (gsn_state's loading, gsn_restore) VRAM made anew: the scene's turn and the table forgotten. A
// state's or a capture's table comes after (sa_table_load). With --renderer modern asked for and not
// on yet (a --state-load before the first vblank, kf4-gsreplay's start) the scene starts now, so that
// the table loaded next is kept.
void sa_forget() {
    sa_group_draw();
    if (!sa.on) {
        if (sa_mode) {
            sa_switch(true);
        }
        return;
    }
    if (gs->in_pass && gs->pass_color == &SA_MARKER) {
        end_pass();
    }
    sat_reset(); // (T3)
    sa.bound = false;
    sa.pending = false;
    sa.clear.valid = false;
    sa.fcopy.valid = false;
    sa_table_reset();
    sa_held_forget();
    rgba_forget(); // (--native-sampler hardware)
}

// --stats: the table's line, and a line a map of what is still the GS's
void sa_print_table_stats() {
    std::vector<std::pair<int, SaStats>> maps;
    {
        std::lock_guard<std::mutex> hold(sat.per_map_lock);
        maps = sat.per_map;
        SaStats last = sat.at_vblank;
        sa_stats_per_map(maps, last); // (the counts since the last vblank)
    }
    const SaStats &s = sa_stats;
    auto tex_counts = [](const uint64_t *n) {
        for (int i = 0; i < SA_TEX_COUNT; i++) {
            fprintf(stderr, "%s%llu %s", i ? ", " : "", (unsigned long long)n[i], sa_tex_names[i]);
        }
    };
    fprintf(stderr, "[stats] standalone table: CLUT loads ");
    tex_counts(s.clut_loads);
    fprintf(stderr, " (%llu decoded); the scene's CLUTs: %llu the table's, %llu the GS's; a block's uploads at most %zu",
            (unsigned long long)s.clut_decodes, (unsigned long long)s.clut_table, (unsigned long long)s.clut_gs,
            sat.owners_most);
    fprintf(stderr, "; owners looked up %llu times (%llu walked)", (unsigned long long)(sa_owners_hits + sa_owners_walks),
            (unsigned long long)sa_owners_walks);
    if (sat.verify) {
        fprintf(stderr, "; verified: textures %llu same, %llu differ, CLUTs %llu same, %llu differ; not the table's: ",
                (unsigned long long)s.verify_same, (unsigned long long)s.verify_differ,
                (unsigned long long)s.verify_clut_same, (unsigned long long)s.verify_clut_differ);
        tex_counts(s.verify_not);
    }
    if (!sat.pack_dir.empty() || !sat.dump_dir.empty()) {
        fprintf(stderr, "; texture pack: %llu uploads replaced, %llu written", (unsigned long long)s.packed_chunks,
                (unsigned long long)s.dumped_chunks);
    }
    fprintf(stderr, "\n");
    // a map: the scene's textures and CLUTs not the table's, and its batches the GS drew (declined)
    std::sort(maps.begin(), maps.end(), [](const std::pair<int, SaStats> &a, const std::pair<int, SaStats> &b) {
        return a.first < b.first;
    });
    for (const auto &m : maps) {
        const SaStats &t = m.second;
        if (!t.batches && !t.textures[SA_TEX_TABLE] && !t.verify_same && !t.verify_differ) {
            continue;
        }
        uint64_t declined = 0;
        for (uint64_t d : t.declined) {
            declined += d;
        }
        fprintf(stderr, "[stats] standalone map %03d: %llu batches, %llu declined; textures ", m.first,
                (unsigned long long)t.batches, (unsigned long long)declined);
        tex_counts(t.textures);
        fprintf(stderr, "; CLUTs %llu the table's, %llu the GS's", (unsigned long long)t.clut_table,
                (unsigned long long)t.clut_gs);
        if (sat.verify) {
            fprintf(stderr, "; verified %llu same, %llu differ, CLUTs %llu same, %llu differ; not the table's: ",
                    (unsigned long long)t.verify_same, (unsigned long long)t.verify_differ,
                    (unsigned long long)t.verify_clut_same, (unsigned long long)t.verify_clut_differ);
            tex_counts(t.verify_not);
        }
        fprintf(stderr, "\n");
    }
}

// ---- The table in save states and captures ---------------------------------------------------------
// Without it, after a load every texture is the GS's ("not uploaded") until the game uploads it again,
// and a map's textures are uploaded once, when it loads. So a state (port/platform/gs.c, after the
// native GS's part) and a capture (record KF4GS_SA_TABLE, after the GS's) carry the table: the blocks
// that have owners (T4's: each owner with the nibbles of the block it still has), the pages' counts (low
// 24 bits and top byte), the CLUT mirror, and the uploads the owners name, bytes and all. Nothing of the
// program's layout: the uploads are the PS2's (BITBLTBUF, TRXPOS, TRXREG and the bytes the GS took).
// Little-endian, every field 4 or 8 bytes (u16 for the CLUT's halfwords):
//   u32 'SAT2', u32 chunks, u32 blocks, u32 pages (512); u64 seq;
//   u64 page_drawn_lo[pages]; u64 page_drawn_hi[pages];
//   u16 clut[512]; u32 clut_cbp[2]; u64 clut_gs[8]; u64 clut_packed[8];
//   chunks x {u32 id, u32 size, u64 seq, u64 bitbltbuf, u64 trxpos, u64 trxreg, size bytes padded to 4};
//   blocks x {u32 block, u32 n; n x {u32 id, u32 0, u64 seq, u64 cover[8]}}  (only blocks with owners)
// Loaded, an upload already kept (the same bytes, where they went) is the same chunk; any other gets a
// new id, so the decoded textures (found again by their uploads' ids) are never another's. An owner
// naming an upload the saver had let go names an id no chunk has (its texture the GS's, "upload let go").
const uint32_t SA_TABLE_MAGIC = 0x32544153u; // ("SAT2": T4's table; T6's first, "SAT1", had 4 owners a block)

struct SaTableHead {
    uint32_t magic, chunks, blocks, pages;
    uint64_t seq;
};
struct SaTableChunk {
    uint32_t id, size;
    uint64_t seq, bitbltbuf, trxpos, trxreg;
};
struct SaTableBlock {
    uint32_t block, n;
};
struct SaTableOwner {
    uint32_t id, pad;
    uint64_t seq;
    uint64_t cover[8];
};
struct SaTableClut {
    uint16_t clut[512];
    uint32_t cbp[2];
    uint64_t gs[8], packed[8];
};
static_assert(sizeof(SaTableHead) == 24 && sizeof(SaTableChunk) == 40 && sizeof(SaTableBlock) == 8 &&
                  sizeof(SaTableOwner) == 80 && sizeof(SaTableClut) == 1160,
              "the table's records are the file's");

// The table as bytes (none when the scene is off)
void sa_table_save(std::vector<uint8_t> &out) {
    out.clear();
    if (!sa.on || sat.blocks.size() != SA_BLOCKS) {
        return;
    }
    auto put = [&](const void *p, size_t n) {
        out.insert(out.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
    };
    std::vector<uint32_t> ids;
    uint32_t blocks = 0;
    for (const SaBlock &b : sat.blocks) {
        if (b.owners.empty()) {
            continue;
        }
        blocks++;
        for (const SaOwner &o : b.owners) {
            if (o.id && sa_chunk(o.id)) {
                ids.push_back(o.id);
            }
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    SaTableHead head = {SA_TABLE_MAGIC, uint32_t(ids.size()), blocks, 512, sa.seq};
    put(&head, sizeof(head));
    put(sat.page_drawn_lo, sizeof(sat.page_drawn_lo));
    put(sat.page_drawn_hi, sizeof(sat.page_drawn_hi));
    SaTableClut tcl;
    memcpy(tcl.clut, sat.clut, sizeof(tcl.clut));
    memcpy(tcl.cbp, sat.clut_cbp, sizeof(tcl.cbp));
    memcpy(tcl.gs, sat.clut_gs, sizeof(tcl.gs));
    memcpy(tcl.packed, sat.clut_packed, sizeof(tcl.packed));
    put(&tcl, sizeof(tcl));
    for (uint32_t id : ids) {
        const SaChunk &c = *sa_chunk(id);
        SaTableChunk tc = {id, uint32_t(c.bytes.size()), c.seq, c.bitbltbuf, c.trxpos, c.trxreg};
        put(&tc, sizeof(tc));
        put(c.bytes.data(), c.bytes.size());
        out.resize((out.size() + 3) & ~size_t(3), 0);
    }
    for (uint32_t k = 0; k < SA_BLOCKS; k++) {
        const SaBlock &b = sat.blocks[k];
        if (b.owners.empty()) {
            continue;
        }
        SaTableBlock tb = {k, uint32_t(b.owners.size())};
        put(&tb, sizeof(tb));
        for (const SaOwner &o : b.owners) {
            SaTableOwner to = {o.id, 0, o.seq, {}};
            memcpy(to.cover, o.cover.q, sizeof(to.cover));
            put(&to, sizeof(to));
        }
    }
}

// A state's or a capture's table, after sa_forget (VRAM loaded). False, and the table left empty (the
// GS's textures), when it does not read; true and nothing done when the scene is off.
bool sa_table_load(const uint8_t *data, size_t size) {
    if (!sa.on) {
        return true;
    }
    sa_table_reset();
    const uint8_t *at = data, *end = data + size;
    SaTableHead head;
    std::vector<uint64_t> drawn_lo(512), drawn_hi(512);
    SaTableClut tcl;
    if (size < sizeof(head) + 2 * 512 * sizeof(uint64_t) + sizeof(tcl)) {
        return false;
    }
    memcpy(&head, at, sizeof(head));
    at += sizeof(head);
    if (head.magic != SA_TABLE_MAGIC || head.pages != 512) {
        return false;
    }
    memcpy(drawn_lo.data(), at, 512 * sizeof(uint64_t));
    at += 512 * sizeof(uint64_t);
    memcpy(drawn_hi.data(), at, 512 * sizeof(uint64_t));
    at += 512 * sizeof(uint64_t);
    memcpy(&tcl, at, sizeof(tcl));
    at += sizeof(tcl);
    std::unordered_map<uint32_t, uint32_t> id_now; // (the saver's id: this table's)
    for (uint32_t i = 0; i < head.chunks; i++) {
        SaTableChunk tc;
        if (size_t(end - at) < sizeof(tc)) {
            return false;
        }
        memcpy(&tc, at, sizeof(tc));
        at += sizeof(tc);
        size_t padded = (size_t(tc.size) + 3) & ~size_t(3);
        if (size_t(end - at) < padded) {
            return false;
        }
        const uint8_t *bytes = at;
        at += padded;
        // (kept as sa_uploaded keeps an upload, without owning its blocks: they come below)
        PendingUpload p;
        p.bitbltbuf = tc.bitbltbuf;
        p.trxpos = tc.trxpos;
        p.trxreg = tc.trxreg;
        uint64_t hash = upload_hash(p, bytes, tc.size);
        uint32_t id = 0;
        auto found = sa.chunk_by_hash.find(hash);
        if (found != sa.chunk_by_hash.end()) {
            SaChunk *c = sa_chunk(found->second);
            if (c && c->bitbltbuf == tc.bitbltbuf && c->trxpos == tc.trxpos && c->trxreg == tc.trxreg &&
                c->bytes.size() == tc.size && memcmp(c->bytes.data(), bytes, tc.size) == 0) {
                id = found->second;
            }
        }
        if (!id) {
            id = sa.next_id++;
            if (!sa.next_id) {
                sa.next_id = 1;
            }
            SaChunk &c = sa.chunks[id];
            c.bitbltbuf = tc.bitbltbuf;
            c.trxpos = tc.trxpos;
            c.trxreg = tc.trxreg;
            c.bytes.assign(bytes, bytes + tc.size);
            c.hash = hash;
            if (!sat.pack_dir.empty() || !sat.dump_dir.empty()) {
                c.key = sa_pack_key(tc.bitbltbuf, tc.trxreg, bytes, tc.size);
                c.pack = sa_pack_bytes(c.key, tc.size);
                sa_stats.packed_chunks += c.pack ? 1 : 0;
            }
            sa.chunk_by_hash[hash] = id;
            sa.chunk_order.push_back(id);
            sa.chunk_bytes += tc.size;
            sa_stats.chunks_new++;
        }
        SaChunk &c = sa.chunks[id];
        c.seq = tc.seq;
        c.used = sa.frame;
        id_now[tc.id] = id;
    }
    std::vector<SaBlock> blocks(SA_BLOCKS);
    size_t most = 0;
    for (uint32_t i = 0; i < head.blocks; i++) {
        SaTableBlock tb;
        if (size_t(end - at) < sizeof(tb)) {
            return false;
        }
        memcpy(&tb, at, sizeof(tb));
        at += sizeof(tb);
        if (tb.block >= SA_BLOCKS || tb.n == 0 || tb.n > 64 * 8 || size_t(end - at) < size_t(tb.n) * sizeof(SaTableOwner)) {
            return false;
        }
        std::vector<SaOwner> &owners = blocks[tb.block].owners;
        owners.resize(tb.n);
        for (uint32_t k = 0; k < tb.n; k++) {
            SaTableOwner to;
            memcpy(&to, at, sizeof(to));
            at += sizeof(to);
            uint32_t id = 0;
            if (to.id) {
                auto it = id_now.find(to.id);
                if (it == id_now.end()) {
                    // (an upload the saver had let go: an id no chunk has, "upload let go")
                    id = sa.next_id++;
                    if (!sa.next_id) {
                        sa.next_id = 1;
                    }
                    id_now[to.id] = id;
                } else {
                    id = it->second;
                }
            }
            owners[k].id = id;
            owners[k].seq = to.seq;
            memcpy(owners[k].cover.q, to.cover, sizeof(to.cover));
        }
        most = std::max(most, owners.size());
    }
    sat.blocks.swap(blocks);
    sat.gen++;
    sat.owners_most = std::max(sat.owners_most, most);
    memcpy(sat.page_drawn_lo, drawn_lo.data(), sizeof(sat.page_drawn_lo));
    memcpy(sat.page_drawn_hi, drawn_hi.data(), sizeof(sat.page_drawn_hi));
    memcpy(sat.clut, tcl.clut, sizeof(tcl.clut));
    memcpy(sat.clut_cbp, tcl.cbp, sizeof(tcl.cbp));
    memcpy(sat.clut_gs, tcl.gs, sizeof(tcl.gs));
    memcpy(sat.clut_packed, tcl.packed, sizeof(tcl.packed));
    sa.seq = std::max(sa.seq, head.seq);
    sa_stats.tables_loaded++;
    return true;
}

// (gsn_capture) the table as a capture's record, when the scene is on
void sa_capture(void (*emit)(uint32_t type, const void *data, size_t size)) {
    std::vector<uint8_t> bytes;
    sa_table_save(bytes);
    if (!bytes.empty()) {
        emit(KF4GS_SA_TABLE, bytes.data(), bytes.size());
    }
}

// (port/platform/gs.c: gsn_table_state_size, gsn_table_state) a save state's table: its size first
// (the bytes kept until they are written), then the bytes
std::vector<uint8_t> sa_state_bytes;
size_t sa_state_size() {
    sa_table_save(sa_state_bytes);
    return sa_state_bytes.size();
}
void sa_state(PortStateIo *io, size_t size) {
    if (!io->loading) {
        size_t have = std::min(size, sa_state_bytes.size());
        port_state_bytes(io, sa_state_bytes.data(), have);
        if (size > have) {
            port_state_bytes(io, nullptr, size - have);
        }
        std::vector<uint8_t>().swap(sa_state_bytes);
        return;
    }
    std::vector<uint8_t> bytes(size);
    port_state_bytes(io, bytes.data(), size);
    if (!sa_table_load(bytes.data(), size)) {
        sa_table_reset();
        fprintf(stderr, "[gs] the state's page-ownership table does not read: its textures are the GS's\n");
    }
}

// --stats
void sa_print_stats() {
    if (!sa.on && !sa_stats.batches && !sa_stats.acquires) {
        return;
    }
    const SaStats &s = sa_stats;
    fprintf(stderr,
            "[stats] standalone: %llu batches (%llu triangles, %llu blended in the shader) in %llu draws; the scene took the frame %llu "
            "times, gave it back %llu (%llu drawn into); textures: ",
            (unsigned long long)s.batches, (unsigned long long)s.triangles, (unsigned long long)s.shader_blends,
            (unsigned long long)s.draws, (unsigned long long)s.acquires, (unsigned long long)s.releases,
            (unsigned long long)s.frames_touched);
    for (int i = 0; i < SA_TEX_COUNT; i++) {
        fprintf(stderr, "%s%llu %s", i ? ", " : "", (unsigned long long)s.textures[i], sa_tex_names[i]);
    }
    fprintf(stderr, "; %llu depth clears and %llu alpha sets drawn by the scene, %llu shadow batches by the stencil, %llu layers "
                    "exact over their own triangles, %llu Z-first draws by depth alone, %llu shines blended in the shader over their first pass's Z",
            (unsigned long long)s.depth_clears, (unsigned long long)s.alpha_sets, (unsigned long long)s.shadow_batches,
            (unsigned long long)s.own_layers, (unsigned long long)s.zfirst_alone, (unsigned long long)s.shine_z_left);
    fprintf(stderr, "; %.2f turns a picture", gsn_stats.pictures ? double(s.acquires) / double(gsn_stats.pictures) : 0.0);
    fprintf(stderr,
            " (T2: %llu taken from the GS's clear, %llu resampled copies; the GS's drawn by the scene: %llu 3D, %llu screen "
            "passes, %llu fills, %llu water; %llu times given while kept; %llu copies of the frame kept, %llu textures read from them; "
            "%llu pictures composited, %llu ops over the screen in them; %llu upscaled (%llu by FSR or DLSS), %llu motions)",
            (unsigned long long)s.cleared, (unsigned long long)s.resampled, (unsigned long long)s.gs_3d,
            (unsigned long long)s.gs_passes, (unsigned long long)s.gs_fills, (unsigned long long)s.gs_water,
            (unsigned long long)s.published,
            (unsigned long long)s.frame_copies, (unsigned long long)s.frame_copy_reads, (unsigned long long)s.composites,
            (unsigned long long)s.composite_ops, (unsigned long long)s.upscales, (unsigned long long)s.upscales_temporal,
            (unsigned long long)s.motions);
    fprintf(stderr, "; %llu decoded, %llu found again; uploads %llu (%.1f MB, %llu new), %llu asked for again; declined: ",
            (unsigned long long)s.decodes, (unsigned long long)s.decode_hits, (unsigned long long)s.uploads,
            s.upload_bytes / 1048576.0, (unsigned long long)s.chunks_new, (unsigned long long)s.replayed_uploads);
    for (int i = 0; i < SA_WHY_COUNT; i++) {
        fprintf(stderr, "%s%llu %s", i ? ", " : "", (unsigned long long)s.declined[i], sa_why_names[i]);
    }
    if (s.tables_loaded) {
        fprintf(stderr, "; %llu tables loaded (save states, captures)", (unsigned long long)s.tables_loaded);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "[stats] standalone skinned: %llu parts' passes kept and drawn together, %llu times, in %llu draws\n",
            (unsigned long long)sa_group.parts_kept, (unsigned long long)sa_group.groups, (unsigned long long)sa_group.draws);
#ifdef ND280_PLAN_FAST
    fprintf(stderr, "[stats] native draw 0x280 plans: %llu with the host's floats, %llu VU1's (a value at the limits)%s",
            (unsigned long long)nd280_plan_fast_n, (unsigned long long)nd280_plan_fast_slow,
            getenv("KF4_ND280_PLAN_CHECK") ? "" : "\n");
    if (getenv("KF4_ND280_PLAN_CHECK")) {
        fprintf(stderr, "; checked: %llu differ\n", (unsigned long long)nd280_plan_check_differ);
    }
#endif
    sa_memo_print_stats(); // (rc/own)
    sat_print_stats(); // (T3)
    sa_print_table_stats();
    if (pc_world_mode) {
        pc_world_print_stats(); // (gsn_pc_world.inc)
    }
}

#include "gsn_scene_targets.inc"
