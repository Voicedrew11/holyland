// The native GS (docs/port/STEP3.md, port/gs/native/gsn.h): the GS drawn with the GPU's rasteriser.
//
// What the GS is asked for comes as GIF packets. They are read here as the GS reads them: the
// registers, the vertex queue and its primitives, the transfers into and out of VRAM, the CLUT. A run
// of primitives with no register written between them is a batch; each batch is drawn with Vulkan as
// soon as the next register write ends it.
//
// VRAM is kept twice:
// - on the CPU (gsn_vram.c), in the GS's own layout: what IMAGE uploads write, what local-to-local
//   copies move, what textures are decoded from;
// - on the GPU as render targets: an image for each frame buffer and Z buffer the game draws into,
//   keyed by its base pointer, width and format. A target can be newer than the CPU's copy of its
//   pages ("dirty"); whatever needs those pages on the CPU (a texture in another format, a read-back,
//   an upload in another shape) has the target written back first.
// A texture that is a target (the frame read back as a texture, the scrolling texture targets, the
// copy of the frame at 0x2BC0) is read from the target itself. Anything else is decoded from the CPU's
// VRAM into an integer image of the raw texels, cached by its pages' write counts, and looked up and
// filtered in the shader (port/gs/native/shaders/gs.frag), so a CLUT change needs no new texture.
//
// Blending is Vulkan's where the GS's equation maps onto it (most draws); otherwise, and for draws
// that read the target they draw into, the shader blends from a copy of the target taken just before,
// in the GS's integer arithmetic.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#ifdef __linux__
#include <pthread.h>
#endif
#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"

extern "C" {
#include "port/gs_capture_format.h"
#include "port/vu_float.h" // (native draw: gsn_native_draw.inc)
#include "port/platform.h"
#include "port/profile.h" // (Shift+P: the threads' and the GPU's times)
#include "port/host.h" // (native draw: the frame-rate marks, gsn_native_draw.inc)
#include "gsn.h"
#include "gsn_vram.h"
}
namespace Vulkan {
extern std::atomic<uint64_t> holyland_pipeline_binds; // (Granite's pipelines bound: cmake/granite_dynamic_state.inc)
extern void (*holyland_on_compile)(Device *device, const DeferredPipelineCompile &compile, bool compute, int64_t ns, bool sync,
                                   bool ok); // (cmake/granite_pipeline_warm.inc)
extern void (*holyland_on_render_pass)(Util::Hash hash, const RenderPassInfo &info);
}
#include "gsn_shaders.h"
#include "upscale_dlss.h" // (--upscaler dlss: gsn_upscale.inc, and the context's extensions)
#include "upscale_ffx.h"  // (--upscaler fsr3: gsn_upscale.inc)
#include "../../render/render_internal.hpp" // (native draw: gsn_native_draw.inc)

using namespace Vulkan;

GsnStats gsn_stats;

namespace {

// ---- GS registers --------------------------------------------------------------------------------
enum {
    R_PRIM = 0x00, R_RGBAQ = 0x01, R_ST = 0x02, R_UV = 0x03, R_XYZF2 = 0x04, R_XYZ2 = 0x05,
    R_TEX0_1 = 0x06, R_TEX0_2 = 0x07, R_CLAMP_1 = 0x08, R_CLAMP_2 = 0x09, R_FOG = 0x0A,
    R_XYZF3 = 0x0C, R_XYZ3 = 0x0D, R_TEX1_1 = 0x14, R_TEX1_2 = 0x15, R_TEX2_1 = 0x16, R_TEX2_2 = 0x17,
    R_XYOFFSET_1 = 0x18, R_XYOFFSET_2 = 0x19, R_PRMODECONT = 0x1A, R_PRMODE = 0x1B, R_TEXCLUT = 0x1C,
    R_SCANMSK = 0x22, R_MIPTBP1_1 = 0x34, R_MIPTBP1_2 = 0x35, R_MIPTBP2_1 = 0x36, R_MIPTBP2_2 = 0x37,
    R_TEXA = 0x3B, R_FOGCOL = 0x3D, R_TEXFLUSH = 0x3F, R_SCISSOR_1 = 0x40, R_SCISSOR_2 = 0x41,
    R_ALPHA_1 = 0x42, R_ALPHA_2 = 0x43, R_DIMX = 0x44, R_DTHE = 0x45, R_COLCLAMP = 0x46,
    R_TEST_1 = 0x47, R_TEST_2 = 0x48, R_PABE = 0x49, R_FBA_1 = 0x4A, R_FBA_2 = 0x4B,
    R_FRAME_1 = 0x4C, R_FRAME_2 = 0x4D, R_ZBUF_1 = 0x4E, R_ZBUF_2 = 0x4F, R_BITBLTBUF = 0x50,
    R_TRXPOS = 0x51, R_TRXREG = 0x52, R_TRXDIR = 0x53, R_HWREG = 0x54, R_SIGNAL = 0x60,
    R_FINISH = 0x61, R_LABEL = 0x62,
};

inline uint32_t bits(uint64_t v, unsigned shift, unsigned count) {
    return uint32_t((v >> shift) & ((uint64_t(1) << count) - 1));
}

inline float as_float(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

bool psm_is_palette(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT4 || psm == GSN_PSMT8H || psm == GSN_PSMT4HL || psm == GSN_PSMT4HH;
}

bool psm_is_16(uint32_t psm) {
    return psm == GSN_PSMCT16 || psm == GSN_PSMCT16S || psm == GSN_PSMZ16 || psm == GSN_PSMZ16S;
}

bool psm_is_depth(uint32_t psm) {
    return psm >= 0x30;
}

// Formats that share one layout of VRAM: a target in one can be read as another of the same
uint32_t psm_layout(uint32_t psm) {
    switch (psm) {
    case GSN_PSMCT24:
        return GSN_PSMCT32;
    case GSN_PSMZ24:
        return GSN_PSMZ32;
    default:
        return psm;
    }
}

// a page of a format: 8 KB, this many pixels across and down
uint32_t page_width(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT4 ? 128 : 64;
}

uint32_t page_height(uint32_t psm) {
    if (psm == GSN_PSMT4) {
        return 128;
    }
    if (psm == GSN_PSMT8 || psm_is_16(psm)) {
        return 64;
    }
    return 32;
}

const uint32_t vram_pages = GSN_VRAM_BYTES / 8192;

// ---- Vertices -----------------------------------------------------------------------------------
// One vertex as the GS queued it
struct QVertex {
    uint32_t x, y; // 12.4, before XYOFFSET
    uint32_t z;
    uint32_t rgba;
    float s, t, q;
    uint32_t u, v; // 10.4
    uint32_t fog;
};

// One corner of a triangle as the shaders take it (shaders/gs.vert)
struct Vtx {
    float x, y;
    uint32_t z;
    uint32_t rgba;
    float s, t, q;
    float fog;
};
static_assert(sizeof(Vtx) == 32, "Vtx is the vertex shader's input");

// The shaders' push constants (shaders/gs_common.h)
struct Push {
    float inv_size[2];
    float zscale;
    uint32_t zmax;
    uint32_t flags;
    uint32_t tex_size;
    uint32_t clamp_lo;
    uint32_t clamp_hi;
    uint32_t test;
    uint32_t fogcol;
    uint32_t alpha;
    uint32_t texa;
    uint32_t tex1;
    float lod_k;
    int32_t src_offset[2];
    int32_t dst_offset[2];
    uint32_t misc;
    uint32_t fbmsk;
    uint32_t tscale; // a texture that is a render target: how many of its pixels a GS pixel is, across
                     // (bits 0-15) and down (16-31)
    float lod_bias;  // --native-lod: added to the mipmap level the GS picks (only to which level)
    float zbias;     // --native-layer-z tolerant: added to the pixel's Z (a draw's colour pass)
    float zslope;    // ... and this times the Z's change across a GS pixel (a layer's colour pass)
};
static_assert(sizeof(Push) == 96, "Push is the shaders' push constant block");

enum : uint32_t {
    F_TME = 0x1, F_FST = 0x2, F_TFX_SHIFT = 2, F_TCC = 0x10, F_FGE = 0x20, F_SHADER_BLEND = 0x40,
    F_SRC_SHIFT = 8, F_ALPHA_FACTOR = 0x1000, F_PASS_SHIFT = 13, F_DATE = 0x8000, F_PREMUL = 0x10000,
    F_PREMUL_CEIL = 0x20000,
};
enum : uint32_t {
    SRC_CLUT = 0, SRC_CT32 = 1, SRC_CT24 = 2, SRC_CT16 = 3, SRC_TARGET_CT32 = 4, SRC_TARGET_CT24 = 5,
    SRC_TARGET_CT16 = 6,
};
enum : uint32_t {
    M_FBA = 0x1, M_COLCLAMP = 0x2, M_DST16 = 0x4, M_DST24 = 0x8, M_PABE = 0x10, M_SCANMSK_SHIFT = 5, M_SCALE_SHIFT = 7,
    M_ZCLAMP = 0x400, M_SCALE_X_SHIFT = 11, M_ANISO_SHIFT = 14, M_HIGH = 0x20000, M_ZFINE = 0x40000,
    M_SMOOTH_2D = 0x80000, M_RECT_2D = 0x100000, M_JITTER = 0x200000,
};

// ---- Render targets and textures ----------------------------------------------------------------
struct Target {
    uint32_t bp, bw, psm; // bp in 256-byte blocks
    uint32_t width, height; // in GS pixels: the image is `sx` times as wide and `sy` times as high
    uint32_t sx, sy;
    ImageHandle image;
    bool depth;
    bool dirty; // newer than the CPU's VRAM (somewhere: dirty_pages says where)
    bool stale; // a page of it is another target's (stale_pages: shares_pages below)
    // which of its pages (target_pages' order: rows of bw pages, when the target is `regular`) are
    // newer than the CPU's VRAM; the others hold what VRAM does
    std::vector<uint8_t> dirty_pages;
    std::vector<uint8_t> stale_pages;
    uint64_t used;
    // --native-msaa: the multisampled image the 3D is drawn into (made when first drawn so), and which
    // of the two has pixels the other does not (msaa_settle, msaa_ready)
    ImageHandle ms;
    bool ms_newer, image_newer;
    // made, and drawn into since, only by a picture between (replaying): what it has is the picture's,
    // not the game's, and need not go back to VRAM when dropped (prepare_write)
    bool replay_only = false;
};

// --frame-rate's pictures between (frame_interp.c): between their marks, and the frame shown before them
// (take_snapshot)
bool replaying;
struct Snapshot {
    uint32_t bp = 0;
    ImageHandle image;
    bool reads = false; // the draws between marks 0xF4 and 0xF5 read it in place of that buffer
} snapshot;

struct Texture {
    uint32_t tbp, tbw, psm, tw, th, levels;
    uint32_t cols, rows; // of level 0, those its draws read (1 << tw, 1 << th, or fewer without mipmaps)
    uint32_t mip_tbp[7], mip_tbw[7];
    ImageHandle image;
    uint32_t page_lo[7], page_count[7];
    uint64_t decoded; // the VRAM write count when it was decoded
    uint64_t hash;    // of the bytes it was decoded from (texture_hash)
    uint64_t used;
    // --native-cutout-aa: how many of its texels (all levels) have each value of the byte their alpha
    // comes from (a palette texture's index, a 32-bit texture's alpha): which alphas it has, how often
    uint32_t texels[256];
};

// The GIF's three paths, read as gs.c reads them
struct GifPath {
    uint32_t left, flg, nreg, reg, eop, in_packet, dwords;
    uint64_t regs;
};

struct Gs {
    // ---- the GS
    uint64_t reg[256];
    uint32_t internal_q;
    uint16_t clut[512];   // the CLUT buffer on the chip: 512 halfwords
    uint32_t clut_cbp[2]; // CBP0, CBP1: the last CBP loaded by CLD 2..5
    uint64_t clut_version;
    QVertex queue[3];
    Vtx queued[3]; // the same, as the shaders take them (made as each is queued)
    uint32_t queue_count;
    uint32_t fan_first; // a fan's first vertex is queue[0], kept
    GifPath paths[4];
    // transfers
    GsnUpload upload;
    uint32_t transfer_dir; // TRXDIR of the transfer under way (0 host to local, 1 local to host, 3 none)
    std::vector<uint8_t> readback;
    size_t readback_at;
    // the CPU's VRAM and how often each page was written
    std::vector<uint8_t> vram;
    uint64_t page_written[512];
    uint64_t vram_writes;

    // ---- Vulkan
    Context *context = nullptr;
    bool own_context = false;
    Device *device = nullptr;
    Program *program = nullptr;
    Program *program_zclamp = nullptr; // (shaders/gs_zclamp.frag: Z kept within the format per pixel)
    Program *program_zclamp_sample = nullptr; // (shaders/gs_zclamp_sample.frag: the same at every sample)
    // (shaders/gs_zonly.frag, gs_zonly_sample.frag: --native-layer-z tolerant's Z passes, the Z alone)
    Program *program_zonly = nullptr;
    Program *program_zonly_sample = nullptr;
    Program *program_ms_load = nullptr, *program_ms_load_depth = nullptr, *program_ms_depth = nullptr; // (msaa_*)
    Program *program_deinterlace = nullptr; // (the scanout's: shaders/deinterlace.frag)
    Program *program_narrow = nullptr;      // (--native-colour high's scanout: shaders/narrow.frag)
    Program *program_present_scale = nullptr;   // (the presenter's scaling: shaders/present_scale.frag)
    Program *program_present_sharpen = nullptr; // (and its sharpening: shaders/present_sharpen.frag)
    // --frame-rate's pictures between on the GPU: the kicks moved to the picture's time (shaders/gs_replay.comp),
    // and the vertices drawn made of them (shaders/gs_replay_corners.comp)
    Program *program_replay_kicks = nullptr, *program_replay_corners = nullptr;
    CommandBufferHandle cmd;
    ImageHandle dummy_raw, dummy_rgba, snapshot, scanout;
    ImageHandle resolved[2]; // a multisampled target's copies for shader blending, resolved by the passes (resolved_copy)
    ImageHandle load_stage[3]; // load_target_rect's rectangles at the PS2's size in the frame (8-bit, 16-bit, depth)
    // load_scaled_on_gpu's answers: each format, whether it blits both ways (here, not a static of the function: its
    // destructor would run at exit before ring_stop, with this thread still loading targets)
    std::vector<std::pair<VkFormat, bool>> blit_formats;
    BufferHandle readback_buffer;
    VkDeviceSize readback_size = 0;
    std::vector<uint8_t> narrowed; // read_image's pixels of a 16-bit image, rounded to 8 bits
    std::vector<std::unique_ptr<Target>> targets;
    std::vector<std::unique_ptr<Texture>> textures;
    uint64_t tick = 0;
    // the render pass under way
    Target *pass_color = nullptr;
    Target *pass_depth = nullptr;
    bool pass_ms = false; // drawing into the targets' multisampled images
    bool in_pass = false;
    // the pass has a second colour attachment, the copy of the target that shader blending reads (blend_scratch)
    bool pass_scratch = false;
    bool pass_area = false; // begun for a part of the targets only (a run of shader blending's)
    // shader blending's copies in the render pass ("Shader blending and the render pass"): its programs,
    // and the images it copies the target into (one for each size, format and sample count drawn so far)
    Program *program_in = nullptr, *program_zclamp_in = nullptr;
    Program *program_in_ms = nullptr, *program_zclamp_in_ms = nullptr, *program_zclamp_sample_in_ms = nullptr;
    Program *program_pass_copy = nullptr, *program_pass_resolve = nullptr;
    std::vector<ImageHandle> scratch;

    // the last batch drawn as a tie (flush_batch, "Ties"): when (tick), and its triangles
    uint64_t tie_tick = UINT64_MAX - 1;
    std::vector<Vtx> tie_verts;

    // ---- the batch being gathered: triangles since the last register write
    std::vector<Vtx> verts;
    std::vector<uint32_t> rects_2d; // (--native-2d smooth: each triangle's texels, flush_batch)
    uint32_t batch_prim = 0; // PRIM's attributes (PRMODE's when PRMODECONT says)
    bool batch_sprites = false; // the batch is sprites

    // the last field shown (field mode: the picture is woven from the last two)
    std::vector<uint32_t> fields[2];
    uint32_t field_width = 0, field_height = 0;
    // vblanks left that a frame-mode picture is read a field at a time: a primitive over more than 400
    // lines drawn with SCANMSK (into one field's lines only) sets it to 4 (scanmsk_seen, "The scanout")
    uint32_t field_scan_vblanks = 0;
    // the displayed buffer as it was at the last vblanks read a field at a time, the newest first (the
    // deinterlace's history; filled: how many are there)
    ImageHandle history[4];
    uint32_t history_filled = 0;
};

Gs *gs;

// --frame-rate's pictures between on the GPU ("Pictures between on the GPU" below): the frame they are
// drawn from, and what drawing its packets (the stream) records.
// A vertex kick as the GS queued it (shaders/gs_replay.vert's Kick): its registers, and for those a
// picture between can change (frame_interp.c changes the qwords written since the kick before), the
// kick in whose stretch they were last written (GSN_IM_NONE: before the stream)
struct InterpKick {
    uint32_t xy;     // X | Y << 16 (12.4, before XYOFFSET)
    uint32_t z;
    uint32_t rgba;
    float s, t, q;
    uint32_t uv;     // U | V << 16
    uint32_t fog;
    uint32_t offset; // XYOFFSET of its context, X | Y << 16
    uint32_t st_owner, rgba_owner, q_owner, uv_owner;
    uint32_t pad[3];
};
static_assert(sizeof(InterpKick) == 64, "InterpKick is gs_replay.vert's Kick");

// A corner of a triangle the pictures between draw (gs_replay.vert's input): the kicks it is made of and
// how (IC_*)
enum : uint32_t {
    IC_SPRITE = 0x8,      // a sprite's: w[0] and w[1] its two kicks; bits 0-1 its corner (0-3), bit 2 its triangle
    IC_FLAT_SHADE = 0x10, // a triangle's, flat: its colour is the third kick's (bits 0-1: which of the three it is)
    IC_FLAT_2D = 0x20,    // flat_2d's moves (--native-msaa, --native-scale above 1)
    IC_SNAP = 0x40,       // a sprite whose UV snap_sprite_uv can put on the texels' middles
    IC_FST = 0x80,        // UV, not STQ
};

struct InterpTargetState {
    Target *t;
    uint32_t bp, bw, psm;
    bool depth, has_ms;
    uint32_t width, height, sx, sy;
    bool dirty, stale, ms_newer, image_newer;
    std::vector<uint8_t> dirty_pages, stale_pages;
};
struct InterpState {
    std::vector<InterpTargetState> targets;
    bool in_pass, pass_ms;
    Target *pass_color, *pass_depth;
    uint64_t vram_writes, clut_version;
};

struct Interp {
    GsnInterpFrame *frame = nullptr;
    BufferHandle moves;            // frame->moves on the GPU
    BufferHandle kicks, corners;   // what drawing the stream recorded (mapped while it is drawn)
    BufferHandle resolved;         // the kicks at the picture's time (gs_replay.comp's)
    BufferHandle vertices;         // the vertices drawn, made of them (gs_replay_corners.comp's)
    BufferHandle picture;          // the picture's: its cameras and runs
    InterpKick *kick_map = nullptr;          // (mapped: written only)
    std::vector<InterpKick> kick_list;       // the same, to read
    uint32_t (*corner_map)[4] = nullptr;
    // while the stream is drawn
    bool active = false;
    bool broken = false;           // more kicks than the frame said, or more corners than there is room for
    uint32_t kick = 0, corner = 0;
    uint32_t st_src = GSN_IM_NONE, q_src = GSN_IM_NONE, rgba_src = GSN_IM_NONE, rgba_q_src = GSN_IM_NONE,
             uv_src = GSN_IM_NONE;
    uint32_t qkick[3] = {};                      // the queued vertices' kicks (gs->queue's)
    std::vector<std::array<uint32_t, 4>> vsrc;   // each of gs->verts as a corner
    // the recording, and the GS as it was before and after it
    bool recorded = false;
    bool tried = false; // (a recording of this frame was made: not recorded then, it could not be made again)
    bool still = false;          // the picture's camera is the frame's (GsnInterpFrame's still: gsn_scene_targets.inc's sat_kept)
    bool recorded_still = false; // (the recording's)
    InterpState start, end;
    std::vector<uint64_t> end_regs;
    uint64_t risky_runs = 0; // (stats: moving batches drawn in runs that do not overlap, cut where they are at t 1)
};
Interp interp;

// KF4_SA_REFLECTION_KEEP (gsn_scene_targets.inc, sat_kept): 0 the reflection drawn in every picture between, 1 (the
// default) kept from the frame while the camera is the frame's bit for bit, 2 always
int reflection_keep() {
    static int keep = -1;
    if (keep < 0) {
        const char *v = getenv("KF4_SA_REFLECTION_KEEP");
        keep = v && *v ? atoi(v) : 1;
    }
    return keep;
}

// --native-scale: the render targets' size, in pixels a GS pixel across (sx) and down (sy); 1 and 1
// are the PS2's. sx is larger than sy for --native-widescreen. Changed at a vblank (gsn_set_scale),
// when every target goes back to VRAM and comes again at the new size.
uint32_t target_sx = 1, target_sy = 1;
volatile uint32_t wanted_scale = 1 | 1 << 8; // sx | sy << 8
// --native-lod (gsn_set_lod): which mipmap level a texture is read at, at a larger --native-scale
volatile int lod_mode = GSN_LOD_SCALE;
// --native-aniso (gsn_set_aniso): log2 of the most taps along a mipmapped texture's footprint (0: off)
volatile int aniso_log2 = 0;
// --native-msaa: the samples a pixel of the 3D's targets has (1: none, the PS2's picture; 2, 4, 8).
// Changed at a vblank (gsn_set_msaa), as the scale is.
uint32_t target_samples = 1;
volatile uint32_t wanted_samples = 1;
// --native-cutout-aa (gsn_set_cutout_aa): with MSAA, the cut-outs (flush_batch) shaded at every
// sample; sample_shading, whether the GPU can (sampleRateShading)
volatile int cutout_aa_mode = GSN_CUTOUT_AA_OFF;
volatile int smooth_2d_mode = GSN_2D_PS2; // --native-2d (gsn_set_2d)
bool sample_shading = false;
// --native-layer-z (gsn_set_layer_z): Z as the GS has it (GSN_LAYER_Z_PS2), or kept fine and tested
// with a tolerance for the layers (GSN_LAYER_Z_TOLERANT: flush_batch, "Z and layers")
volatile int layer_z_mode = GSN_LAYER_Z_PS2;

// --native-shine (gsn_set_shine_exact): modern's shines blended in the shader drawn whole (0) or in runs (1)
volatile int shine_exact = 0;

// --native-colour (gsn_set_colour): the 32-bit colour targets at 8 bits a channel, as the GS has them
// (GSN_COLOUR_EXACT), or 16 (GSN_COLOUR_HIGH: "--native-colour high" below). Changed at a vblank, as the
// scale is.
uint32_t target_colour = GSN_COLOUR_EXACT;
volatile uint32_t wanted_colour = GSN_COLOUR_EXACT;

// The colour targets' format at a precision (GSN_COLOUR_*): 16-bit UNORM holds every 8-bit value exactly (v * 257),
// so what the GS's arithmetic writes there reads back as it is. colour_format(): the targets' now; the pipeline set
// takes the one wanted (the targets take it at a vblank, after the start)
VkFormat colour_format_of(uint32_t colour) {
    return colour == GSN_COLOUR_HIGH ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
}

VkFormat colour_format() {
    return colour_format_of(target_colour);
}

bool wide(const Image &image) {
    return image.get_format() == VK_FORMAT_R16G16B16A16_UNORM;
}

// An 8-bit RGBA pixel as a 16-bit one, each channel v * 257
uint64_t widen(uint32_t v) {
    uint64_t x = v;
    x = (x | x << 16) & 0x0000FFFF0000FFFFull;
    x = (x | x << 8) & 0x00FF00FF00FF00FFull; // each byte in a 16-bit lane
    return x | x << 8;
}

// A 16-bit RGBA pixel rounded to 8 bits a channel
uint32_t narrow(uint64_t v) {
    uint32_t out = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t x = uint32_t(v >> (c * 16)) & 0xFFFF;
        out |= ((x * 255u + 32767u) / 65535u) << (c * 8);
    }
    return out;
}

// n pixels into an image's update_image memory at pixel `at`, as the image holds them
void put_pixels(void *out, const Image &image, size_t at, const uint32_t *rgba, size_t n) {
    if (wide(image)) {
        uint64_t *o = static_cast<uint64_t *>(out) + at;
        for (size_t i = 0; i < n; i++) {
            o[i] = widen(rgba[i]);
        }
    } else {
        memcpy(static_cast<uint32_t *>(out) + at, rgba, n * 4);
    }
}

// (Shift+P's profiler, while open) the time in scope added to a stage
struct ProfileTimer {
    ProfileStage stage;
    uint64_t start;
    explicit ProfileTimer(ProfileStage s) : stage(s), start(port_profile_on() ? port_profile_now_ns() : 0) {}
    ~ProfileTimer() {
        if (start != 0) {
            port_profile_add(stage, port_profile_now_ns() - start);
        }
    }
};
// Time spent in a function, added to a counter of gsn_stats
struct Timer {
    uint64_t &total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit Timer(uint64_t &t) : total(t) {}
    ~Timer() {
        total += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }
};
int log_level; // KF4_GSN_LOG: 1 says when targets are made and dropped, 2 every batch too, 3 each vblank's display registers
bool hw_sampler; // --native-sampler hardware (gsn_set_hardware_sampler; P1: gsn_rgba.inc)

#define GSN_LOG(level, ...)                                                                                \
    do {                                                                                                   \
        if (log_level >= (level)) {                                                                        \
            fprintf(stderr, "[gsn] " __VA_ARGS__);                                                         \
        }                                                                                                  \
    } while (0)

void settle_upload(); // (the upload under way, written so far: below)

// --stats: why the render pass under way ends (GSN_WHY_*): what the innermost Why in scope says
int why_now = GSN_WHY_OTHER;
struct Why {
    int before;
    explicit Why(int why) : before(why_now) { why_now = why; }
    ~Why() { why_now = before; }
};

// KF4_GSN_GPU_TIME=1: timestamps between the GPU's pieces of work, each interval added to a kind
// (gpu_mark: the time since the mark before is the kind's), printed by --stats (gsn_print_gpu_times)
int gpu_time = -1;
uint64_t gpu_cmd_serial; // (command buffers begun: Cmd::out)
// KF4_GSN_GPU_TIME=2 also splits the render passes' time by what is drawn in them (gpu_enter: the time from there to the
// next mark is that kind's), in the pictures between's replays too (OP_GPU_MARK). Each mark waits for the GPU's work
// before it, so the times add up to more than without them: read them as shares.
const char *const gpu_kinds[] = {"other", "pass", "pass msaa", "resolve", "msaa load/depth", "copy", "scanout",
                                 "pass: untagged", "pass: end", "scene draws", "scene cut-outs per sample",
                                 "scene blend copies", "scene blend draws", "scene shadows", "scene skinned groups",
                                 "refl draws", "refl cut-outs per sample", "refl blend copies", "refl blend draws",
                                 "refl shadows", "refl skinned groups", "GS batches", "scene ops", "refl Z-first", "scene Z-first", "scene blend copies between", "refl blend copies between", "scene Z-first atest", "refl Z-first atest"};
enum {
    GPU_OTHER, GPU_PASS, GPU_PASS_MS, GPU_RESOLVE, GPU_MS_COPY, GPU_COPY, GPU_SCANOUT, GPU_UNTAGGED, GPU_PASS_END,
    GPU_SA_DRAW, GPU_SA_SAMPLE, GPU_SA_BLEND_COPY, GPU_SA_BLEND_DRAW, GPU_SA_SHADOW, GPU_SA_GROUP,
    GPU_SA_REFL, // (the same six into a named target: + GPU_SA_REFL - GPU_SA_DRAW)
    GPU_GS_BATCH = GPU_SA_REFL + 6, GPU_SA_OP, GPU_REFL_ZFIRST, GPU_SA_ZFIRST, GPU_SA_COPY_BETWEEN, GPU_REFL_COPY_BETWEEN, GPU_SA_ZFIRST_ATEST, GPU_REFL_ZFIRST_ATEST, GPU_KINDS
};
uint64_t gpu_marks[GPU_KINDS]; // (the back end's)
// (KF4_GSN_GPU_TIME=2: the draws made in each kind's time, and their triangles; counted by both threads, the replays' by
// the back end)
std::atomic<uint64_t> gpu_draws[GPU_KINDS], gpu_tris[GPU_KINDS];
int gpu_cur = -1; // (KF4_GSN_GPU_TIME=2: what is being drawn since the last mark)
void gpu_mark(int kind);
void gpu_enter(int kind);

// ---- Vulkan helpers ----------------------------------------------------------------------------
// The command buffer: Granite's, through Cmd, which hands each call on and, while a picture between is
// recorded (--frame-rate, "Pictures between on the GPU" below), also keeps it, so that the frame's next
// pictures between can make the same calls again (replay_ops) without the GS's work that led to them.
// Only the calls the native GS makes are here.
enum OpKind : uint32_t {
    OP_BARRIER, OP_BEGIN_PASS, OP_END_PASS, OP_OPAQUE, OP_PROGRAM, OP_TOPOLOGY, OP_CULL, OP_DEPTH_TEST,
    OP_DEPTH_COMPARE, OP_VIEWPORT, OP_SCISSOR, OP_TEXTURE, OP_CONSTANT, OP_BLEND_ENABLE, OP_BLEND_CONSTANTS,
    OP_BLEND_FACTORS, OP_BLEND_OP, OP_VERTEX_DATA, OP_VERTEX_BINDING, OP_VERTEX_ATTRIB, OP_WRITE_MASK, OP_PUSH,
    OP_DRAW, OP_COPY_IMAGE, OP_UPDATE_IMAGE, OP_RESOLVE, OP_CLEAR_IMAGE, OP_INTERP_KICKS, OP_MEMORY_BARRIER,
    OP_INPUT_ATTACHMENTS, OP_STORAGE, OP_DEPTH_BOUNDS, OP_STENCIL, OP_GPU_MARK, OP_CONSERVATIVE, OP_STORAGE_DATA,
    // (the back end's only, gsn_backend.inc: a stream's calls that are not a recording's)
    OP_NEW_CMD, OP_SUBMIT, OP_NEXT_FRAME, OP_PUBLISH, OP_PRESENT, OP_BLIT, OP_COPY_TO_BUFFER, OP_TIMESTAMP, OP_KICKS,
    OP_REPLAY, OP_RECORDING_BINDS, OP_UPSCALE,
};

struct Recording;
// A picture between made again from a recording (OP_REPLAY, gsn_backend.inc): the recording, the buffers its kicks'
// passes read for this picture, and the targets' images as they are now in place of those it names
struct ReplayJob {
    std::shared_ptr<const Recording> recording;
    BufferHandle picture, kicks, moves, resolved, corners, vertices;
    uint32_t kick_count = 0;
    std::vector<std::pair<const Image *, ImageHandle>> images;
};

// The calls kept: each an OpKind and its arguments (a struct of its own, then any bytes) in `arena`; the
// images and buffers they name, held so they live as long as the recording. The back end's streams
// (gsn_backend.inc) are kept the same way: every call the native GS makes, until its thread has made it.
struct Recording {
    bool on = false;
    bool ok = true; // nothing that cannot be made again (a read-back) was recorded
    struct Op {
        OpKind kind;
        uint32_t at; // its arguments in arena
    };
    std::vector<Op> ops;
    std::vector<uint8_t> arena;
    std::vector<ImageHandle> images;
    std::vector<BufferHandle> buffers;
    // bytes the caller writes after the call (constant and vertex data, an image's update): it writes them
    // into the recording, and at the next call they are copied into Granite's memory (settle). (Granite's
    // is write-combined: reading it back is slow.)
    struct Later {
        uint32_t at;
        void *to;
        uint32_t size;
    };
    std::vector<Later> later;
    uint64_t draws = 0;
    // The images of targets the calls name, with what the target is: the calls made again name that
    // target's images as they are then (a target drawn over after an upload is made anew each picture)
    struct TargetImage {
        const Image *image;
        uint32_t bp, bw, psm;
        bool ms;
    };
    std::vector<TargetImage> target_images;
    // (load_target_rect) Rectangles of VRAM at the PS2's size, each in a place of its own in these images, that
    // the recording's blits scale into targets: written once, as a decoded texture is, and kept with the recording
    struct Stage {
        ImageHandle image;
        uint32_t x = 0, y = 0, row = 0; // the next place: along the row, the row's height so far
    };
    std::vector<Stage> stages;
    const Buffer *held_chunk[2] = {}; // (the data chunks held last: hold_chunk)
    std::vector<ReplayJob> replays;   // (a stream's OP_REPLAY: its index here)

    void clear() {
        ops.clear();
        arena.clear();
        images.clear();
        buffers.clear();
        later.clear();
        target_images.clear();
        stages.clear();
        replays.clear();
        held_chunk[0] = held_chunk[1] = nullptr;
        kept_last = nullptr;
        insert_at = SIZE_MAX;
        ok = true;
        draws = 0;
    }
    void settle() {
        for (const Later &l : later) {
            memcpy(l.to, arena.data() + l.at, l.size);
        }
        later.clear();
    }
    // a call, its arguments, and room for `extra` bytes after them (returned: their place in arena)
    template <class T> uint32_t add(OpKind kind, const T &args, size_t extra = 0) {
        settle();
        uint32_t at = uint32_t((arena.size() + 7) & ~size_t(7));
        arena.resize(at + sizeof(T) + extra);
        memcpy(arena.data() + at, &args, sizeof(T));
        if (insert_at < ops.size()) {
            ops.insert(ops.begin() + ptrdiff_t(insert_at++), {kind, at});
        } else {
            ops.push_back({kind, at});
        }
        return at + uint32_t(sizeof(T));
    }
    // (Cmd::before_pass: the calls go in at this place, before the render pass under way began)
    size_t insert_at = SIZE_MAX;
    void hold(const Image &image) {
        Image &i = const_cast<Image &>(image);
        i.add_reference();
        images.push_back(ImageHandle(&i));
        for (const TargetImage &t : target_images) {
            if (t.image == &image) {
                return;
            }
        }
        for (auto &t : gs->targets) {
            if (t->image.get() == &image || t->ms.get() == &image) {
                target_images.push_back({&image, t->bp, t->bw, t->psm, t->ms.get() == &image});
                return;
            }
        }
    }
    // (a data chunk the calls name: held once, not for every call)
    void hold_chunk(const Buffer &buffer) {
        if (held_chunk[0] != &buffer && held_chunk[1] != &buffer) {
            held_chunk[1] = held_chunk[0];
            held_chunk[0] = &buffer;
            hold(buffer);
        }
    }
    void hold(const Buffer &buffer) {
        Buffer &b = const_cast<Buffer &>(buffer);
        b.add_reference();
        buffers.push_back(BufferHandle(&b));
    }
    // (a stream's: held, without a recording's note of which target an image is)
    void keep(const Image &image) {
        if (kept_last == &image) {
            return;
        }
        kept_last = &image;
        Image &i = const_cast<Image &>(image);
        i.add_reference();
        images.push_back(ImageHandle(&i));
    }
    const Image *kept_last = nullptr;
    template <class T> const T &args(const Op &op) const {
        return *reinterpret_cast<const T *>(arena.data() + op.at);
    }
};
// (the recording under way or made last: shared with the back end's replays of it, which may still be making its calls
// when the next is begun: interp_record makes a new one then)
std::shared_ptr<Recording> recording_ops = std::make_shared<Recording>();

struct ArgBarrier {
    VkPipelineStageFlags2 src_stage;
    VkAccessFlags2 src_access;
    VkPipelineStageFlags2 dst_stage;
    VkAccessFlags2 dst_access;
};
struct ArgPass {
    RenderPassInfo info;
    RenderPassInfo::Subpass subpasses[4]; // (a recording keeps 2: Cmd::begin_render_pass)
};
struct ArgTexture {
    uint32_t set, binding;
    const ImageView *view;
    StockSampler sampler;
};
struct ArgData { // push constants (offset)
    uint32_t a, b, c;
    uint32_t size;
};
// Constant data (set, binding; its bytes follow, kept to find the same again: finish_recording) and vertex data
// (binding, stride), where they are on the GPU (DataChunk)
struct ArgPlaced {
    uint32_t a, b;
    uint32_t size;
    const Buffer *buffer;
    VkDeviceSize offset;
};
struct ArgBinding {
    uint32_t binding;
    const Buffer *buffer;
    VkDeviceSize offset, stride;
    VkVertexInputRate rate;
};
struct ArgAttrib {
    uint32_t attrib, binding;
    VkFormat format;
    VkDeviceSize offset;
};
struct ArgDraw {
    uint32_t count, instances, first, first_instance;
};
struct ArgCopy {
    const Image *dst, *src;
    VkOffset3D dst_offset, src_offset;
    VkExtent3D extent;
    VkImageSubresourceLayers dst_sub, src_sub;
};
struct ArgUpdate {
    const Image *image;
    VkOffset3D offset;
    VkExtent3D extent;
    uint32_t row_length, image_height;
    VkImageSubresourceLayers sub;
    uint32_t size;
    const Buffer *from; // (finish: the bytes on the GPU, at from_offset, for a target's update)
    VkDeviceSize from_offset;
};
struct ArgResolve {
    const Image *dst, *src;
    VkImageResolve region;
};
struct ArgClear {
    const Image *image;
    VkClearValue value;
};
struct ArgValues {
    uint32_t v[4];
    float f[4];
};
// (the back end's, gsn_backend.inc)
// a submit the GS's thread waits for: done once the back end has submitted it, fence then the GPU's
struct SubmitWait {
    std::mutex lock;
    std::condition_variable cv;
    bool done = false;
    Fence fence;
};
struct ArgSubmit {
    SubmitWait *wait;
};
struct ArgPublish {
    int slot;
    uint64_t tag;
};
struct ArgBlit {
    const Image *dst, *src;
    VkOffset3D dst_offset, dst_extent, src_offset, src_extent;
    VkFilter filter;
};
// (--upscaler fsr3, dlss: gsn_upscale.inc) A picture upscaled, by which (sa_upscaler's 2 or 3), its inputs and output,
// with what upscale.h says of each
struct ArgUpscale {
    uint32_t mode;
    const Image *color, *depth, *motion, *out;
    float jitter[2];
    float frame_ms, near_plane, fov_y;
    uint32_t reset;
};
struct ArgCopyToBuffer {
    const Buffer *dst;
    const Image *src;
    VkDeviceSize buffer_offset;
    VkOffset3D offset;
    VkExtent3D extent;
    uint32_t row_length, slice_height;
    VkImageSubresourceLayers sub;
};
struct ArgKicks { // (the buffers interp_kicks_pass reads; none when the frame has no kicks)
    const Buffer *picture, *kicks, *moves, *resolved, *corners, *vertices;
    uint32_t count;
};

void interp_kicks_pass(CommandBuffer &c, const ArgKicks &a); // (the pictures between's kicks moved to their time: below)
ArgKicks interp_kicks_now(Recording &s); // (the buffers it reads for the picture under way, held by s)
Recording &backend_stream();               // (the stream the calls go into: gsn_backend.inc)
void backend_send();                       // (the calls written so far, to the back end's thread)
bool backend_stream_long();                // (a stream long enough to send before its submit)
uint64_t backend_sends();                  // (streams sent so far: which stream a place in one is in)
void backend_drain();                      // (everything sent, made)
void backend_stop();                       // (at exit)
void backend_start();                      // (gsn_open)
void backend_wait(SubmitWait &w);          // (until a submit sent is made, and the GPU is done with it)

// Bytes a texel of these formats takes (update_image's staging)
uint32_t texel_bytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UINT:
        return 1;
    case VK_FORMAT_R16_UINT:
        return 2;
    case VK_FORMAT_R16G16B16A16_UNORM:
        return 8;
    default:
        return 4; // RGBA8, R32_UINT, D32
    }
}

// Whether the last thing recorded is a full barrier (full_barrier): another one right after it would
// order nothing more. Anything recorded goes through cmd(), which clears it: whatever writes into the
// back end's stream some other way must clear it too.
bool barrier_fresh = false;
uint64_t pass_serial = 0; // (render passes begun: a draw's bindings last no longer than its pass, gsn_standalone.inc)

// The vertex and constant data the draws read, written by the CPU into buffers of our own, one after another
// and never written over (a full one let go: Granite keeps it until the GPU is done with it). Granite's own
// (allocate_vertex_data) has 4 KB blocks: a batch's vertices of more were a buffer made for them alone, a
// Vulkan buffer and its memory each batch. And a recording (--frame-rate's pictures between) keeps the chunks
// it wrote into, so its calls made again bind the data where it is, not copied anew for each picture.
struct DataChunk {
    BufferHandle buffer;
    uint8_t *map = nullptr;
    VkDeviceSize used = 0, flushed = 0, size = 0;
    BufferDomain domain = BufferDomain::LinkedDeviceHost;
};
DataChunk vertex_chunk, constant_chunk;
// and the bytes of the images' updates (Cmd::update_image: Granite's staging was its command buffer's, which only the
// back end's thread has), read once by a copy: in the host's memory
DataChunk upload_chunk = {{}, nullptr, 0, 0, 0, BufferDomain::Host};
struct DataPlace {
    const Buffer *buffer;
    VkDeviceSize offset;
    uint8_t *host;
};
DataPlace data_place(DataChunk &ch, VkDeviceSize size, VkDeviceSize align, VkBufferUsageFlags usage, VkDeviceSize chunk_size);
void data_flush(DataChunk &ch);
VkDeviceSize ubo_align();
VkDeviceSize ssbo_align();

// The CLUTs bound as storage data (Cmd::set_storage_data): those in the constant chunk now, by what they hold, so that
// a batch with the same CLUT as one before binds it where that one is (Granite makes a descriptor set for each
// storage buffer place: the same place, a set it has already)
struct StorageSeen {
    const Buffer *buffer = nullptr;
    struct Entry {
        uint64_t hash;
        VkDeviceSize offset;
        std::vector<uint8_t> bytes;
    };
    std::vector<Entry> entries; // (the latest last)
};
StorageSeen storage_seen;
std::vector<uint32_t> clut_scratch; // (flush_batch's CLUT and rectangles, before they are bound)

// Vulkan's dynamic state for the draws (cmake/granite_dynamic_state.cmake): depth, stencil, culling, topology and
// blend constants where the device has Vulkan 1.3; blending and the colour write mask too where it has
// VK_EXT_extended_dynamic_state3's. Draws that differ only there (the tie rule's Z-first and colour draws, a skinned
// group's marks and shines) then share a pipeline instead of each binding one of its own. The recording needs no
// calls of its own for it: its calls (OP_DEPTH_TEST, OP_WRITE_MASK ...) set Granite's state as before, and Granite
// sets Vulkan's before each draw it makes again. KF4_GSN_DYNAMIC=0: none (the pipelines as before, to compare);
// 1: not the blending and the write mask (as on a device without extended dynamic state 3).
struct DynamicState {
    bool ds = false, blend = false;
};
DynamicState dynamic_state_choice() {
    static DynamicState d;
    static bool chosen;
    if (!chosen) {
        chosen = true;
        const char *v = getenv("KF4_GSN_DYNAMIC");
        int want = v ? atoi(v) : 2;
        const auto &f = gs->device->get_device_features();
        d.ds = want >= 1 && f.device_api_core_version >= VK_API_VERSION_1_3;
        d.blend = d.ds && want >= 2 && f.eds3_features.extendedDynamicState3ColorBlendEnable &&
                  f.eds3_features.extendedDynamicState3ColorBlendEquation && f.eds3_features.extendedDynamicState3ColorWriteMask;
        GSN_LOG(1, "dynamic state: depth and stencil %s, blending %s\n", d.ds ? "on" : "off", d.blend ? "on" : "off");
    }
    return d;
}
uint64_t pipeline_binds() {
    return Vulkan::holyland_pipeline_binds.load(std::memory_order_relaxed);
}

// The calls into the command buffer go through Cmd, which writes each into the back end's stream (gsn_backend.inc:
// its thread makes them through Granite, in order) and, while a picture between is recorded, keeps it too.
class Cmd {
public:
    // The stream, with a command buffer begun on it: the first call after a submit begins one (OP_NEW_CMD), as
    // Granite's request_command_buffer was at the first call
    Recording &out() {
        if (!pass_open && backend_stream_long()) {
            backend_send(); // (between two calls, outside a render pass: before_pass needs its beginning here)
        }
        if (!cmd_open) {
            cmd_open = true;
            gpu_cmd_serial++;
            forget_bindings();
            backend_stream().add(OP_NEW_CMD, ArgValues{});
            gpu_mark(GPU_OTHER);
        }
        barrier_fresh = false;
        return backend_stream();
    }
    // whether a command buffer is begun (calls made since the last submit)
    bool begun() const {
        return cmd_open;
    }
    // (submit: the stream's command buffer sent; the next call begins another)
    void submitted() {
        cmd_open = false;
    }
    // The storage buffers Granite has bound (their cookies), so that the same bound again is not: Granite writes the
    // set's descriptors again for any buffer bound, the same or not (native draw's arena and records, bound for each
    // batch). Forgotten where Granite forgets them (a render pass's beginning and end, a new command buffer), where
    // they are bound past Cmd (the kicks' pass, replay_ops), and where a recording begins and ends (its calls must
    // bind what they draw with themselves).
    void forget_bindings() {
        memset(storage_bound, 0, sizeof(storage_bound));
    }
    // (a barrier of its own: one memory barrier)
    void memory_barrier(const VkMemoryBarrier2 &b, VkDependencyFlags flags) {
        struct {
            VkMemoryBarrier2 b;
            VkDependencyFlags flags;
        } a = {b, flags};
        a.b.pNext = nullptr;
        if (on()) {
            rec().add(OP_MEMORY_BARRIER, a);
        }
        out().add(OP_MEMORY_BARRIER, a);
    }
    static void memory_barrier_raw(CommandBuffer &c, VkMemoryBarrier2 b, VkDependencyFlags flags) {
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.dependencyFlags = flags;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        gs->device->get_device_table().vkCmdPipelineBarrier2(c.get_command_buffer(), &dep);
    }
    // (native draw's mesh, and its pictures between's Picture: gsn_native_draw.inc)
    void set_storage_buffer(unsigned set, unsigned binding, const Buffer &buffer) {
        if (set < 3 && binding < 8) {
            if (storage_bound[set][binding] == buffer.get_cookie()) {
                return;
            }
            storage_bound[set][binding] = buffer.get_cookie();
        }
        ArgBinding a = {set | binding << 8, &buffer, 0, 0, VK_VERTEX_INPUT_RATE_VERTEX};
        if (on()) {
            rec().hold(buffer);
            rec().add(OP_STORAGE, a);
        }
        Recording &s = out();
        s.hold(buffer);
        s.add(OP_STORAGE, a);
    }
    void set_input_attachments(unsigned set, unsigned first) {
        put(OP_INPUT_ATTACHMENTS, ArgValues{{set, first}, {}});
    }
    void barrier(VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
        put(OP_BARRIER, ArgBarrier{ss, sa, ds, da});
    }
    void begin_render_pass(const RenderPassInfo &info) {
        pass_serial++;
        ArgPass a;
        a.info = info;
        if (info.num_subpasses > 4) {
            fprintf(stderr, "[gsn] a render pass of %u subpasses: more than the back end keeps\n", info.num_subpasses);
            abort();
        }
        for (unsigned i = 0; i < info.num_subpasses; i++) {
            a.subpasses[i] = info.subpasses[i];
        }
        if (on()) {
            if (info.num_subpasses > 2) {
                rec().ok = false;
            }
            for (unsigned i = 0; i < info.num_color_attachments; i++) {
                rec().hold(info.color_attachments[i]->get_image());
            }
            if (info.depth_stencil) {
                rec().hold(info.depth_stencil->get_image());
            }
            rec().add(OP_BEGIN_PASS, a);
        }
        Recording &s = out();
        for (unsigned i = 0; i < info.num_color_attachments; i++) {
            s.keep(info.color_attachments[i]->get_image());
        }
        if (info.depth_stencil) {
            s.keep(info.depth_stencil->get_image());
        }
        s.add(OP_BEGIN_PASS, a);
        pass_open = true;
        pass_stream = backend_sends();
        pass_begin = s.ops.size() - 1;
        pass_begin_rec = on() ? rec().ops.size() - 1 : SIZE_MAX;
        pass_reads.clear();
        program_pending = false;
        forget_bindings();
    }
    void end_render_pass() {
        put(OP_END_PASS, ArgValues{});
        pass_open = false;
        program_pending = false;
        forget_bindings();
    }
    // Calls made before the render pass under way began, not after it ended: a transfer into an image no draw of the
    // pass has read (load_target_rect: a target loaded from VRAM), which would otherwise end the pass, and with it
    // store and load its attachments whole (at a scale, megabytes for a few texels). The calls between
    // before_pass_begin and before_pass_end go in before its OP_BEGIN_PASS, in the stream and in a recording made.
    bool before_pass_can(const Image *image, const Image *ms) const {
        if (!pass_open || pass_stream != backend_sends() || (on() && pass_begin_rec >= rec().ops.size())) {
            return false;
        }
        for (const Image *read : pass_reads) {
            if (read == image || (ms && read == ms)) {
                return false;
            }
        }
        return true;
    }
    void before_pass_begin() {
        before_fresh = barrier_fresh;
        barrier_fresh = false; // (what is before the pass's beginning is not known here)
        backend_stream().insert_at = pass_begin;
        if (on()) {
            rec().insert_at = pass_begin_rec;
        }
    }
    void before_pass_end() {
        Recording &s = backend_stream();
        pass_begin = s.insert_at;
        s.insert_at = SIZE_MAX;
        if (on()) {
            pass_begin_rec = rec().insert_at;
            rec().insert_at = SIZE_MAX;
        }
        barrier_fresh = before_fresh;
    }
    void set_opaque_state() {
        put(OP_OPAQUE, ArgValues{});
    }
    // A program is set when a draw first needs it: one set and set over again before any draw (each batch of the
    // scene's sets the map's program, then its kind's: sa_draw_range) never reaches Granite, which makes a program
    // set anew bind its pipeline again at the next draw, the same pipeline or not (a vkCmdBindPipeline a batch more
    // in the frame, the recording and each replay). A render pass's beginning and end forget the program (Granite's
    // too).
    void set_program(Program *program) {
        pending_program = program;
        program_pending = true;
    }
    void set_primitive_topology(VkPrimitiveTopology topology) {
        put(OP_TOPOLOGY, ArgValues{{uint32_t(topology)}, {}});
    }
    void set_cull_mode(VkCullModeFlags mode) {
        put(OP_CULL, ArgValues{{uint32_t(mode)}, {}});
    }
    void set_depth_test(bool test, bool write) {
        put(OP_DEPTH_TEST, ArgValues{{test, write}, {}});
    }
    // the depth bounds test (Granite as cmake/granite_depth_bounds.cmake makes it): on with its bounds, or off
    void set_depth_bounds(bool on_, float lo, float hi) {
        put(OP_DEPTH_BOUNDS, ArgValues{{uint32_t(on_)}, {lo, hi}});
    }
    static void depth_bounds_raw(CommandBuffer &c, bool on_, float lo, float hi) {
        c.set_depth_bounds_test(on_);
        if (on_) {
            gs->device->get_device_table().vkCmdSetDepthBounds(c.get_command_buffer(), lo, hi);
        }
    }
    // the stencil test (standalone's shadows, gsn_standalone.inc): on with its compare, the op where it passes
    // (KEEP where it or the depth test fails), the reference and the write mask; or off
    void set_stencil(bool on_, VkCompareOp op, VkStencilOp pass, uint32_t ref, uint32_t write_mask) {
        put(OP_STENCIL, ArgValues{{uint32_t(on_), uint32_t(op), uint32_t(pass), ref | write_mask << 8}, {}});
    }
    static void stencil_raw(CommandBuffer &c, bool on_, VkCompareOp op, VkStencilOp pass, uint32_t ref_mask) {
        c.set_stencil_test(on_);
        if (on_) {
            c.set_stencil_front_ops(op, pass, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
            c.set_stencil_back_ops(op, pass, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
            c.set_stencil_reference(0xFF, uint8_t(ref_mask >> 8), uint8_t(ref_mask));
        }
    }
    // conservative rasterization (overestimated: every pixel a triangle touches, all its samples covered); pipeline
    // state, cleared by set_opaque_state
    void set_conservative(bool on_) {
        put(OP_CONSERVATIVE, ArgValues{{uint32_t(on_)}, {}});
    }
    void set_depth_compare(VkCompareOp op) {
        put(OP_DEPTH_COMPARE, ArgValues{{uint32_t(op)}, {}});
    }
    void set_viewport(const VkViewport &vp) {
        put(OP_VIEWPORT, vp);
    }
    void set_scissor(const VkRect2D &rect) {
        put(OP_SCISSOR, rect);
    }
    void set_texture(unsigned set, unsigned binding, const ImageView &view, StockSampler sampler) {
        ArgTexture a = {set, binding, &view, sampler};
        if (on()) {
            rec().hold(view.get_image());
            rec().add(OP_TEXTURE, a);
        }
        Recording &s = out();
        s.keep(view.get_image());
        s.add(OP_TEXTURE, a);
        if (pass_open && (pass_reads.empty() || pass_reads.back() != &view.get_image())) {
            pass_reads.push_back(&view.get_image());
        }
    }
    template <class T> T *allocate_typed_constant_data(unsigned set, unsigned binding, unsigned count) {
        return static_cast<T *>(allocate_constant_data(set, binding, count * sizeof(T)));
    }
    void *allocate_constant_data(unsigned set, unsigned binding, VkDeviceSize size) {
        VkDeviceSize range = (size + 15) & ~VkDeviceSize(15);
        DataPlace d = data_place(constant_chunk, range, ubo_align(),
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 2u << 20);
        ArgPlaced a = {set, binding, uint32_t(size), d.buffer, d.offset};
        Recording &s = out();
        s.hold_chunk(*d.buffer);
        s.add(OP_CONSTANT, a);
        if (on()) {
            rec().hold_chunk(*d.buffer);
            uint32_t at = rec().add(OP_CONSTANT, a, size_t(size));
            rec().later.push_back({at, d.host, uint32_t(size)});
            return rec().arena.data() + at;
        }
        return d.host;
    }
    // Storage data (the CLUT, gs_frag.h's Clut): written now, into the constant chunk, and bound there; the same bytes
    // as a recent one in the chunk bound where they are. The chunk keeps them (a recording holds it): the calls made
    // again bind the same place.
    void set_storage_data(unsigned set, unsigned binding, const void *data, VkDeviceSize size) {
        uint64_t h = size;
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        for (VkDeviceSize k = 0; k + 8 <= size; k += 8) {
            uint64_t w;
            memcpy(&w, bytes + k, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        const Buffer *buffer = nullptr;
        VkDeviceSize offset = 0;
        if (storage_seen.buffer == constant_chunk.buffer.get() && constant_chunk.buffer) {
            for (size_t i = storage_seen.entries.size(); i-- > 0;) {
                const StorageSeen::Entry &e = storage_seen.entries[i];
                if (e.hash == h && e.bytes.size() == size && memcmp(e.bytes.data(), bytes, size_t(size)) == 0) {
                    buffer = storage_seen.buffer;
                    offset = e.offset;
                    break;
                }
            }
        }
        if (!buffer) {
            VkDeviceSize range = (size + 15) & ~VkDeviceSize(15);
            DataPlace d = data_place(constant_chunk, range, std::max(ubo_align(), ssbo_align()),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 2u << 20);
            memcpy(d.host, bytes, size_t(size));
            buffer = d.buffer;
            offset = d.offset;
            if (storage_seen.buffer != d.buffer) {
                storage_seen.buffer = d.buffer;
                storage_seen.entries.clear();
            }
            if (storage_seen.entries.size() >= 64) {
                storage_seen.entries.erase(storage_seen.entries.begin());
            }
            storage_seen.entries.push_back({h, offset, std::vector<uint8_t>(bytes, bytes + size)});
        }
        VkDeviceSize range = (size + 15) & ~VkDeviceSize(15);
        ArgPlaced a = {set, binding, uint32_t(range), buffer, offset};
        if (on()) {
            rec().hold_chunk(*buffer);
            rec().add(OP_STORAGE_DATA, a);
        }
        Recording &s = out();
        s.hold_chunk(*buffer);
        s.add(OP_STORAGE_DATA, a);
    }
    void set_blend_enable(bool enable) {
        put(OP_BLEND_ENABLE, ArgValues{{enable}, {}});
    }
    void set_blend_constants(const float constants[4]) {
        put(OP_BLEND_CONSTANTS, ArgValues{{}, {constants[0], constants[1], constants[2], constants[3]}});
    }
    void set_blend_factors(VkBlendFactor sc, VkBlendFactor sa, VkBlendFactor dc, VkBlendFactor da) {
        put(OP_BLEND_FACTORS, ArgValues{{uint32_t(sc), uint32_t(sa), uint32_t(dc), uint32_t(da)}, {}});
    }
    void set_blend_op(VkBlendOp colour, VkBlendOp alpha) {
        put(OP_BLEND_OP, ArgValues{{uint32_t(colour), uint32_t(alpha)}, {}});
    }
    void *allocate_vertex_data(unsigned binding, VkDeviceSize size, VkDeviceSize stride) {
        DataPlace d = data_place(vertex_chunk, size, 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 8u << 20);
        ArgPlaced a = {binding, uint32_t(stride), uint32_t(size), d.buffer, d.offset};
        Recording &s = out();
        s.hold_chunk(*d.buffer);
        s.add(OP_VERTEX_DATA, a);
        if (on()) {
            rec().hold_chunk(*d.buffer);
            rec().add(OP_VERTEX_DATA, a);
        }
        return d.host;
    }
    void set_vertex_binding(uint32_t binding, const Buffer &buffer, VkDeviceSize offset, VkDeviceSize stride) {
        ArgBinding a = {binding, &buffer, offset, stride, VK_VERTEX_INPUT_RATE_VERTEX};
        if (on()) {
            rec().hold(buffer);
            rec().add(OP_VERTEX_BINDING, a);
        }
        Recording &s = out();
        s.hold(buffer);
        s.add(OP_VERTEX_BINDING, a);
    }
    void set_vertex_attrib(uint32_t attrib, uint32_t binding, VkFormat format, VkDeviceSize offset) {
        put(OP_VERTEX_ATTRIB, ArgAttrib{attrib, binding, format, offset});
    }
    void set_color_write_mask(uint32_t mask) {
        put(OP_WRITE_MASK, ArgValues{{mask}, {}});
    }
    void push_constants(const void *data, VkDeviceSize offset, VkDeviceSize range) {
        ArgData a = {uint32_t(offset), 0, 0, uint32_t(range)};
        if (on()) {
            uint32_t at = rec().add(OP_PUSH, a, size_t(range));
            memcpy(rec().arena.data() + at, data, size_t(range));
        }
        Recording &s = out();
        uint32_t at = s.add(OP_PUSH, a, size_t(range));
        memcpy(s.arena.data() + at, data, size_t(range));
    }
    void draw(uint32_t count, uint32_t first = 0) {
        apply_program();
        if (on()) {
            rec().add(OP_DRAW, ArgDraw{count, 1, first, 0});
            rec().draws++;
        }
        if (gpu_time >= 2) {
            gpu_draws[gpu_cur >= 0 ? gpu_cur : GPU_UNTAGGED]++;
            gpu_tris[gpu_cur >= 0 ? gpu_cur : GPU_UNTAGGED] += count / 3;
        }
        out().add(OP_DRAW, ArgDraw{count, 1, first, 0});
    }
    void copy_image(const Image &dst, const Image &src, const VkOffset3D &dst_offset, const VkOffset3D &src_offset,
                    const VkExtent3D &extent, const VkImageSubresourceLayers &dst_sub, const VkImageSubresourceLayers &src_sub) {
        ArgCopy a = {&dst, &src, dst_offset, src_offset, extent, dst_sub, src_sub};
        if (on()) {
            rec().hold(dst);
            rec().hold(src);
            rec().add(OP_COPY_IMAGE, a);
        }
        Recording &s = out();
        s.keep(dst);
        s.keep(src);
        s.add(OP_COPY_IMAGE, a);
    }
    // Bytes for the image (Granite's update_image): the caller writes them at the place returned, in the upload chunk
    // (or, while a picture between is recorded, in the recording, copied there at the next call)
    void *update_image(const Image &image, const VkOffset3D &offset, const VkExtent3D &extent, uint32_t row_length,
                       uint32_t image_height, const VkImageSubresourceLayers &sub) {
        uint32_t rl = row_length ? row_length : image.get_width(sub.mipLevel);
        uint32_t ih = image_height ? image_height : image.get_height(sub.mipLevel);
        uint32_t size = rl * ih * sub.layerCount * texel_bytes(image.get_format());
        DataPlace d = data_place(upload_chunk, size, 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 8u << 20);
        Recording &s = out();
        s.keep(image);
        s.hold_chunk(*d.buffer);
        s.add(OP_UPDATE_IMAGE, ArgUpdate{&image, offset, extent, row_length, image_height, sub, size, d.buffer, d.offset});
        if (on()) {
            rec().hold(image);
            uint32_t at = rec().add(OP_UPDATE_IMAGE,
                                    ArgUpdate{&image, offset, extent, row_length, image_height, sub, size, nullptr, 0}, size);
            rec().later.push_back({at, d.host, size});
            return rec().arena.data() + at;
        }
        return d.host;
    }
    void resolve_image(const Image &dst, const Image &src, const VkImageResolve &region) {
        ArgResolve a = {&dst, &src, region};
        if (on()) {
            rec().hold(dst);
            rec().hold(src);
            rec().add(OP_RESOLVE, a);
        }
        Recording &s = out();
        s.keep(dst);
        s.keep(src);
        s.add(OP_RESOLVE, a);
    }
    static void resolve_raw(CommandBuffer &c, const Image &dst, const Image &src, const VkImageResolve &region) {
        gs->device->get_device_table().vkCmdResolveImage(c.get_command_buffer(), src.get_image(),
                                                         src.get_layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL), dst.get_image(),
                                                         dst.get_layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), 1, &region);
    }
    void clear_image(const Image &image, const VkClearValue &value) {
        if (on()) {
            rec().hold(image);
            rec().add(OP_CLEAR_IMAGE, ArgClear{&image, value});
        }
        Recording &s = out();
        s.keep(image);
        s.add(OP_CLEAR_IMAGE, ArgClear{&image, value});
    }
    void copy_image_to_buffer(const Buffer &dst, const Image &src, VkDeviceSize buffer_offset, const VkOffset3D &offset,
                              const VkExtent3D &extent, unsigned row_length, unsigned slice_height,
                              const VkImageSubresourceLayers &sub) {
        if (on()) {
            rec().ok = false; // (a read-back: what it reads is the picture's, and the CPU's work after it too)
        }
        Recording &s = out();
        s.hold(dst);
        s.keep(src);
        s.add(OP_COPY_TO_BUFFER, ArgCopyToBuffer{&dst, &src, buffer_offset, offset, extent, row_length, slice_height, sub});
    }
    // (not recorded: a picture's read-back of a target at a scale, read_image after it, is not one to make again)
    void blit_image(const Image &dst, const Image &src, const VkOffset3D &dst_offset, const VkOffset3D &dst_extent,
                    const VkOffset3D &src_offset, const VkOffset3D &src_extent, VkFilter filter) {
        Recording &s = out();
        s.keep(dst);
        s.keep(src);
        s.add(OP_BLIT, ArgBlit{&dst, &src, dst_offset, dst_extent, src_offset, src_extent, filter});
    }
    // (--upscaler fsr3, at the composite: never in a recording) A picture upscaled on the back end's thread
    void upscale(const ArgUpscale &a) {
        Recording &s = out();
        s.keep(*a.color);
        s.keep(*a.depth);
        s.keep(*a.motion);
        s.keep(*a.out);
        s.add(OP_UPSCALE, a);
    }
    // The same, recorded too (load_target_rect's scaling: a picture between's recording makes it again, into the
    // target's image as it is then, as it does its updates of targets)
    void blit_image_recorded(const Image &dst, const Image &src, const VkOffset3D &dst_offset, const VkOffset3D &dst_extent,
                             const VkOffset3D &src_offset, const VkOffset3D &src_extent, VkFilter filter) {
        ArgBlit a = {&dst, &src, dst_offset, dst_extent, src_offset, src_extent, filter};
        if (on()) {
            rec().hold(dst);
            rec().hold(src);
            rec().add(OP_BLIT, a);
        }
        Recording &s = out();
        s.keep(dst);
        s.keep(src);
        s.add(OP_BLIT, a);
    }
    // (and each picture's kicks moved to its time: gs_replay.comp, before its draws)
    void interp_kicks() {
        if (on()) {
            rec().add(OP_INTERP_KICKS, ArgValues{});
        }
        Recording &s = out();
        ArgKicks a = interp_kicks_now(s);
        s.add(OP_KICKS, a);
        if (a.count) {
            forget_bindings(); // (set 1 bound by the kicks' pass, past Cmd)
        }
    }
    // A picture between made again from the recording (gsn_backend.inc's replay_ops): what it binds is not known here
    void replay(ReplayJob &&job) {
        Recording &s = out();
        s.replays.push_back(std::move(job));
        s.add(OP_REPLAY, ArgValues{{uint32_t(s.replays.size() - 1)}, {}});
        forget_bindings();
    }

private:
    uint64_t storage_bound[3][8] = {};
    Program *pending_program = nullptr;
    bool program_pending = false;
    bool cmd_open = false;
    // the render pass under way: its OP_BEGIN_PASS in the stream and in a recording made, and the images its draws
    // have read (before_pass)
    bool pass_open = false;
    size_t pass_begin = SIZE_MAX, pass_begin_rec = SIZE_MAX;
    uint64_t pass_stream = 0; // (backend_sends when it began: its beginning is in the stream being written)
    std::vector<const Image *> pass_reads;
    bool before_fresh = false;
    void apply_program() {
        if (program_pending) {
            program_pending = false;
            if (on()) {
                rec().add(OP_PROGRAM, pending_program);
            }
            out().add(OP_PROGRAM, pending_program);
        }
    }
    // a call with no images or buffers to hold, into the recording too while one is made
    template <class T> void put(OpKind kind, const T &args) {
        if (on()) {
            rec().add(kind, args);
        }
        out().add(kind, args);
    }
    static bool on() {
        return recording_ops->on;
    }
    static Recording &rec() {
        return *recording_ops;
    }
};
Cmd cmd_layer;

Cmd &cmd() {
    return cmd_layer;
}

// The recording's constant data (the CLUTs: most batches' the same as another's, or none: zeros): each the same as
// one before it bound where that one is, so that the calls made again find it bound already (replay_ops) and do not
// bind it again (Granite writes the set's descriptors again for any buffer bound, the same or not)
void same_constants(Recording &r) {
    std::unordered_map<uint64_t, std::vector<uint32_t>> seen; // (a content hash: the calls whose bytes those are)
    for (size_t i = 0; i < r.ops.size(); i++) {
        const Recording::Op &op = r.ops[i];
        if (op.kind != OP_CONSTANT) {
            continue;
        }
        ArgPlaced &a = *reinterpret_cast<ArgPlaced *>(r.arena.data() + op.at);
        const uint8_t *data = r.arena.data() + op.at + sizeof(ArgPlaced);
        uint64_t h = a.size;
        for (uint32_t k = 0; k + 8 <= a.size; k += 8) {
            uint64_t w;
            memcpy(&w, data + k, 8);
            h = (h ^ w) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        std::vector<uint32_t> &same = seen[h];
        bool found = false;
        for (uint32_t j : same) {
            const ArgPlaced &b = r.args<ArgPlaced>(r.ops[j]);
            if (b.size == a.size && memcmp(r.arena.data() + r.ops[j].at + sizeof(ArgPlaced), data, a.size) == 0) {
                a.buffer = b.buffer;
                a.offset = b.offset;
                found = true;
                break;
            }
        }
        if (!found) {
            same.push_back(uint32_t(i));
        }
    }
}

// A recording made ready to be made again: barriers one after another made one (each is of all
// commands, before and after: one with all their stages and accesses orders as much), and the bytes of
// the updates of targets' images (their pixels from VRAM, the same each time) put in a buffer on the GPU
// once, to be copied from
void finish_recording(Recording &r) {
    std::vector<Recording::Op> ops;
    ops.reserve(r.ops.size());
    for (const Recording::Op &op : r.ops) {
        if (op.kind == OP_BARRIER && !ops.empty() && ops.back().kind == OP_BARRIER) {
            ArgBarrier &last = *reinterpret_cast<ArgBarrier *>(r.arena.data() + ops.back().at);
            const ArgBarrier &b = r.args<ArgBarrier>(op);
            last.src_stage |= b.src_stage;
            last.src_access |= b.src_access;
            last.dst_stage |= b.dst_stage;
            last.dst_access |= b.dst_access;
            continue;
        }
        ops.push_back(op);
    }
    r.ops.swap(ops);
    same_constants(r);
    VkDeviceSize bytes = 0;
    for (const Recording::Op &op : r.ops) {
        if (op.kind == OP_UPDATE_IMAGE) {
            const ArgUpdate &a = r.args<ArgUpdate>(op);
            for (const Recording::TargetImage &t : r.target_images) {
                if (t.image == a.image) {
                    bytes = (bytes + 15) & ~VkDeviceSize(15);
                    bytes += a.size;
                }
            }
        }
    }
    if (!bytes) {
        return;
    }
    BufferCreateInfo info = {};
    info.domain = BufferDomain::Device;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    std::vector<uint8_t> data(bytes);
    VkDeviceSize at = 0;
    std::vector<std::pair<ArgUpdate *, VkDeviceSize>> updates;
    for (const Recording::Op &op : r.ops) {
        if (op.kind == OP_UPDATE_IMAGE) {
            ArgUpdate &a = *reinterpret_cast<ArgUpdate *>(r.arena.data() + op.at);
            for (const Recording::TargetImage &t : r.target_images) {
                if (t.image == a.image) {
                    at = (at + 15) & ~VkDeviceSize(15);
                    memcpy(data.data() + at, r.arena.data() + op.at + sizeof(ArgUpdate), a.size);
                    updates.push_back({&a, at});
                    at += a.size;
                }
            }
        }
    }
    BufferHandle buffer = gs->device->create_buffer(info, data.data());
    for (auto &u : updates) {
        u.first->from = buffer.get();
        u.first->from_offset = u.second;
    }
    r.buffers.push_back(buffer);
}

// A picture between's recorded calls made again name the targets' images as the targets have them now (those that
// differ: interp_cannot_replay; the back end's replay_ops)
std::vector<std::pair<const Image *, const Image *>> replay_images;

// Everything recorded so far (its writes) before everything after it: one barrier between two pieces
// of work, however many of the steps between ask for one (the end of a render pass, a copy's before
// and after, the next render pass's beginning)
void full_barrier() {
    if (barrier_fresh) {
        return;
    }
    cmd().barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    barrier_fresh = true;
}

// (the GS's thread: the kind each interval ends with is worked out here, the timestamps written by the back end: OP_TIMESTAMP)
void gpu_mark(int kind) {
    if (gpu_time < 0) {
        gpu_time = getenv("KF4_GSN_GPU_TIME") ? std::max(atoi(getenv("KF4_GSN_GPU_TIME")), 0) : 0;
    }
    // (the profiler, Shift+P, while open: the same marks, for the GPU's time in all)
    if ((!gpu_time && !port_profile_on()) || !cmd().begun()) {
        return;
    }
    if (gpu_time >= 2 && recording_ops->on) {
        recording_ops->add(OP_GPU_MARK, ArgValues{{uint32_t(kind), 0}, {}});
    }
    if (gpu_time >= 2 && gpu_cur >= 0) {
        kind = gpu_cur;
        gpu_cur = -1;
    }
    backend_stream().add(OP_TIMESTAMP, ArgValues{{uint32_t(kind), uint32_t(gpu_time > 0)}, {}});
}

// (KF4_GSN_GPU_TIME=2) the GPU's time from here to the next mark is `kind`'s; the time before it, what was entered last
void gpu_enter(int kind) {
    if (gpu_time < 2 || !cmd().begun()) {
        return;
    }
    if (recording_ops->on) {
        recording_ops->add(OP_GPU_MARK, ArgValues{{uint32_t(kind), 1}, {}});
    }
    int before = gpu_cur >= 0 ? gpu_cur : GPU_UNTAGGED;
    backend_stream().add(OP_TIMESTAMP, ArgValues{{uint32_t(before), 1}, {}});
    gpu_cur = kind;
}

void end_pass() {
    if (gs->in_pass) {
        gsn_stats.pass_ends[why_now]++;
        gpu_enter(GPU_PASS_END); // (KF4_GSN_GPU_TIME=2: the stores)
        cmd().end_render_pass();
        gpu_mark(gs->pass_ms ? GPU_PASS_MS : GPU_PASS);
        gs->in_pass = false;
        gs->pass_color = nullptr;
        gs->pass_depth = nullptr;
        gs->pass_ms = false;
        gs->pass_scratch = false;
        gs->pass_area = false;
        barrier_fresh = false;
        full_barrier();
    }
}

// Room for size bytes of vertex or constant data in the chunk, a new chunk when it is full
DataPlace data_place(DataChunk &ch, VkDeviceSize size, VkDeviceSize align, VkBufferUsageFlags usage, VkDeviceSize chunk_size) {
    VkDeviceSize at = (ch.used + align - 1) & ~(align - 1);
    if (!ch.buffer || at + size > ch.size) {
        data_flush(ch);
        BufferCreateInfo info = {};
        info.domain = ch.domain;
        info.size = std::max(chunk_size, size);
        info.usage = usage;
        ch.buffer = gs->device->create_buffer(info, nullptr);
        ch.map = static_cast<uint8_t *>(gs->device->map_host_buffer(*ch.buffer, MEMORY_ACCESS_WRITE_BIT));
        ch.size = info.size;
        ch.used = ch.flushed = at = 0;
    }
    ch.used = at + size;
    return {ch.buffer.get(), at, ch.map + at};
}

// What the CPU wrote into the chunk since the last time, made the GPU's (before a submit)
void data_flush(DataChunk &ch) {
    recording_ops->settle(); // (a recording's constant data is copied in at its next call: before the flush)
    if (ch.buffer && ch.used > ch.flushed) {
        gs->device->unmap_host_buffer(*ch.buffer, MEMORY_ACCESS_WRITE_BIT, ch.flushed, ch.used - ch.flushed);
        ch.flushed = ch.used;
    }
}

VkDeviceSize ubo_align() {
    static VkDeviceSize align = 0;
    if (!align) {
        align = std::max<VkDeviceSize>(16, gs->device->get_gpu_properties().limits.minUniformBufferOffsetAlignment);
    }
    return align;
}

VkDeviceSize ssbo_align() {
    static VkDeviceSize align = 0;
    if (!align) {
        align = std::max<VkDeviceSize>(16, gs->device->get_gpu_properties().limits.minStorageBufferOffsetAlignment);
    }
    return align;
}

// Everything recorded so far, sent; with wait, finished too (the back end's thread submits it: OP_SUBMIT)
void submit(bool wait) {
    {
        Why why(GSN_WHY_SUBMIT);
        end_pass();
    }
    recording_ops->settle();
    if (!cmd().begun()) {
        return;
    }
    // A submit in the middle of a picture between drawn on the GPU (a write-back or read-back while it
    // is recorded: an upload asked for again over a target, say): the compute passes at its start
    // (interp_kicks_pass) move the kicks the stream has given so far, which the draws submitted now
    // use, so the corners' count so far goes in first and the buffers are flushed; after it, the passes
    // again, for the draws to come (recorded too, so a replay does the same). Without it the draws
    // already recorded ran with the corners' count of the picture before and the kicks not yet
    // written: pictures between with the HUD gone, the floor out of place, textures in wrong places
    // (a coin on the ground, map 000's HUD, October 2026).
    bool mid_picture = interp.active && recording_ops->on && interp.kick_map && interp.corner_map;
    if (mid_picture) {
        uint32_t count[4] = {interp.corner, 0, 0, 0};
        memcpy(interp.corner_map[0], count, sizeof(count));
        gs->device->unmap_host_buffer(*interp.kicks, MEMORY_ACCESS_WRITE_BIT);
        gs->device->unmap_host_buffer(*interp.corners, MEMORY_ACCESS_WRITE_BIT);
    }
    data_flush(vertex_chunk);
    data_flush(constant_chunk);
    data_flush(upload_chunk);
    SubmitWait done;
    backend_stream().add(OP_SUBMIT, ArgSubmit{wait ? &done : nullptr});
    cmd().submitted();
    backend_send();
    if (wait) {
        backend_wait(done);
    }
    if (mid_picture) {
        interp.kick_map = static_cast<InterpKick *>(gs->device->map_host_buffer(*interp.kicks, MEMORY_ACCESS_WRITE_BIT));
        interp.corner_map =
            static_cast<uint32_t(*)[4]>(gs->device->map_host_buffer(*interp.corners, MEMORY_ACCESS_WRITE_BIT));
        gsn_stats.interp_mid_submits++;
        cmd().interp_kicks();
    }
}

// A barrier between transfers and the draws or transfers after them
void transfer_barrier() {
    full_barrier();
}

ImageHandle make_image(uint32_t width, uint32_t height, VkFormat format, uint32_t levels, bool attachment) {
    ImageCreateInfo info = ImageCreateInfo::render_target(width, height, format);
    info.levels = levels;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (attachment) {
        // (a colour target is read in its own render pass by shader blending: an input attachment)
        info.usage |= format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    info.layout = ImageLayout::General;
    info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
    return gs->device->create_image(info, nullptr);
}

// Pixels from the GPU: an image's rectangle, as RGBA8 (or D32 floats), when the GPU is done
const uint8_t *read_image(const Image &image, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    VkDeviceSize size = VkDeviceSize(w) * h * (wide(image) ? 8 : 4);
    if (!gs->readback_buffer || gs->readback_size < size) {
        BufferCreateInfo info = {};
        info.domain = BufferDomain::CachedHost;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        gs->readback_buffer = gs->device->create_buffer(info, nullptr);
        gs->readback_size = size;
    }
    end_pass();
    VkImageAspectFlags aspect = image.get_format() == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                           : VK_IMAGE_ASPECT_COLOR_BIT;
    transfer_barrier();
    cmd().copy_image_to_buffer(*gs->readback_buffer, image, 0, {int32_t(x), int32_t(y), 0}, {w, h, 1}, 0, 0,
                               {aspect, 0, 0, 1});
    cmd().barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                  VK_ACCESS_HOST_READ_BIT);
    submit(true);
    const uint8_t *mapped =
        static_cast<const uint8_t *>(gs->device->map_host_buffer(*gs->readback_buffer, MEMORY_ACCESS_READ_BIT));
    if (!wide(image)) {
        return mapped;
    }
    // (--native-colour high: rounded to 8 bits)
    gs->narrowed.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; i++) {
        uint64_t v;
        memcpy(&v, mapped + i * 8, 8);
        uint32_t n = narrow(v);
        memcpy(gs->narrowed.data() + i * 4, &n, 4);
    }
    return gs->narrowed.data();
}

void done_reading() {
    gs->device->unmap_host_buffer(*gs->readback_buffer, MEMORY_ACCESS_READ_BIT);
}

// ---- --native-msaa ------------------------------------------------------------------------------
// With MSAA on (target_samples above 1), the 3D is drawn into multisampled images: a target drawn into
// with a Z buffer gets one (Target::ms, as large as its image), and so does its Z buffer. Every draw
// with a Z buffer goes to them; so does a draw without one into a target whose multisampled image is
// newer than its image, unless it reads that target as its texture (flush_batch). Everything else uses
// the image as before: draws without Z (the 2D, the passes over the whole frame), textures read from
// targets, write-backs, frame copies, uploads, the scanout. The two are kept in step lazily, at most
// one of them newer: before the image is read or written, a newer multisampled image is resolved
// into it (msaa_settle: colour, each pixel the mean of its samples; Z, its first sample); before the
// multisampled image is drawn into, a newer image is copied into every sample of it (msaa_ready).
// Draws into the multisampled images are shaded once a pixel, as without MSAA: only coverage and Z
// are per sample, so the pixels inside a primitive are what they are without MSAA, and its edges are
// smoothed when resolved. A draw that blends in the shader reads the target's resolved pixels
// (snapshot_of), which keeps a blended pixel's samples as one colour.

// Granite's cache of render passes does not tell them apart by their attachments' sample counts: a
// pass into multisampled images names its one subpass, where the passes into the images have the
// default (so the two kinds are not mixed up), and one run uses one sample count (msaa_samples_used).
// (Set on the GS's thread, read by gsn_set_msaa on the sender's or the overlay's; msaa_samples_have,
// the counts this GPU has, set by gsn_open, so gsn_set_msaa needs nothing of Granite's.)
std::atomic<uint32_t> msaa_samples_used{0};
uint32_t msaa_samples_have = 1;
// The GPU's kind (gsn_gpu_kind), set by gsn_open: --native-scale auto's most
int gpu_kind = GSN_GPU_OTHER;

void msaa_subpass(RenderPassInfo &rp, RenderPassInfo::Subpass &subpass) {
    subpass = {};
    subpass.num_color_attachments = rp.num_color_attachments;
    subpass.color_attachments[0] = 0;
    subpass.depth_stencil_mode = rp.depth_stencil ? RenderPassInfo::DepthStencil::ReadWrite : RenderPassInfo::DepthStencil::None;
    rp.subpasses = &subpass;
    rp.num_subpasses = 1;
}

ImageHandle make_ms_image(uint32_t width, uint32_t height, bool depth) {
    msaa_samples_used = target_samples;
    ImageCreateInfo info = ImageCreateInfo::render_target(width, height, depth ? VK_FORMAT_D32_SFLOAT : colour_format());
    info.samples = VkSampleCountFlagBits(target_samples);
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    info.layout = ImageLayout::General;
    info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
    return gs->device->create_image(info, nullptr);
}

// One of the copying passes (shaders/msaa.vert): src read at each pixel of dst, written to its colour
// or (depth) its Z
void msaa_copy_pass(const Image &dst, const Image &src, Program *program, bool depth) {
    end_pass();
    gpu_mark(GPU_OTHER);
    RenderPassInfo rp;
    RenderPassInfo::Subpass subpass;
    if (depth) {
        rp.depth_stencil = &dst.get_view();
        rp.op_flags = RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
    } else {
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &dst.get_view();
        rp.store_attachments = 1;
    }
    if (dst.get_create_info().samples != VK_SAMPLE_COUNT_1_BIT) {
        msaa_subpass(rp, subpass);
    }
    Cmd &c = cmd();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    c.begin_render_pass(rp);
    c.set_opaque_state();
    c.set_program(program);
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_test(depth, depth);
    c.set_depth_compare(VK_COMPARE_OP_ALWAYS);
    c.set_texture(0, 0, src.get_view(), StockSampler::NearestClamp);
    c.draw(3);
    c.end_render_pass();
    gpu_mark(GPU_MS_COPY);
    {
        uint64_t bytes = uint64_t(dst.get_width()) * dst.get_height() * (wide(dst) ? 8 : 4);
        gsn_stats.pass_store_bytes += bytes * dst.get_create_info().samples;
        gsn_stats.pass_load_bytes += bytes * src.get_create_info().samples;
    }
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

// A rectangle of a multisampled colour image resolved into a single-sampled one, at the same place
void msaa_resolve(const Image &dst, const Image &src, VkOffset3D at, VkExtent3D size) {
    end_pass();
    gpu_mark(GPU_OTHER);
    transfer_barrier();
    VkImageResolve region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = at;
    region.dstOffset = at;
    region.extent = size;
    cmd().resolve_image(dst, src, region);
    gpu_mark(GPU_RESOLVE);
    transfer_barrier();
    gsn_stats.ms_resolves++;
    gsn_stats.resolves_by[why_now]++;
    gsn_stats.resolve_bytes += uint64_t(size.width) * size.height * (wide(dst) ? 8 : 4) * (1 + src.get_create_info().samples);
}

// The target's image made current: what was drawn into the multisampled image, resolved into it.
// (Called before anything reads or writes the image; nothing when MSAA is off.)
void msaa_settle(Target &t) {
    if (!t.ms_newer) {
        return;
    }
    if (t.depth) {
        msaa_copy_pass(*t.image, *t.ms, gs->program_ms_depth, true);
        gsn_stats.ms_resolves++;
        gsn_stats.resolves_by[why_now]++;
    } else {
        msaa_resolve(*t.image, *t.ms, {0, 0, 0}, {t.image->get_width(), t.image->get_height(), 1});
    }
    t.ms_newer = false;
}

// The target's image has been written (a draw without MSAA, an upload, a copy): the multisampled
// image, if there is one, is behind it
void msaa_image_written(Target &t) {
    if (t.ms) {
        t.image_newer = true;
    }
}

// The target's multisampled image made, and current, to be drawn into
void msaa_ready(Target &t) {
    if (!t.ms) {
        t.ms = make_ms_image(t.image->get_width(), t.image->get_height(), t.depth);
        t.image_newer = true;
    }
    if (t.image_newer) {
        msaa_copy_pass(*t.ms, *t.image, t.depth ? gs->program_ms_load_depth : gs->program_ms_load, t.depth);
        t.image_newer = false;
        gsn_stats.ms_loads++;
    }
}

// The multisampled image let go (the target's image was made again at another size)
void msaa_forget(Target &t) {
    t.ms.reset();
    t.ms_newer = t.image_newer = false;
}

// The 2D covers the same GS pixels it does on the PS2, whole: drawn into a multisampled image
// (--native-msaa), or into a target larger than the PS2's (--native-scale, --native-widescreen). Its
// primitives are rectangles on the screen (sprites, and the menus' quads: two triangles of a strip
// with one Z), each triangle half of one: its corners are those of its bounding box. Such a
// triangle's corners are moved out to the edges of the GS pixels it covers (those whose integer point
// is inside: from ceil(x0) to ceil(x1) - 1 across, the same down), and its texture coordinates and fog
// carried along their slopes, so that each pixel's middle has what it had.
// (Left as they are, an edge GS pixel's samples, or its pixels of a larger target, on the far side
// of its integer point would not be covered: a rectangle over the whole frame would leave half of its
// first column and row as they were, and every box and letter of the menus would be blurred by half
// a pixel. At --native-scale 2, a picture drawn into a 128 x 128 target by a sprite from 0 to 127.5
// left the target's first column and row as they were: the sky's clouds on map 000 are such a
// picture, and showed it as bright lines where their texture repeats; the frame's passes over the
// whole picture left its first column and row: a line down the screen's left edge.) A triangle with a slant, or with its colour or Z not
// the same at its corners, is left as it is: the 3D. Returns whether it moved any.
bool flat_2d(std::vector<Vtx> &verts, bool *all = nullptr) {
    bool moved = false;
    size_t count = 0;
    for (size_t i = 0; i + 3 <= verts.size(); i += 3) {
        Vtx *v = &verts[i];
        if (v[0].z != v[1].z || v[0].z != v[2].z || v[0].rgba != v[1].rgba || v[0].rgba != v[2].rgba) {
            continue;
        }
        float x0 = std::min(v[0].x, std::min(v[1].x, v[2].x)), x1 = std::max(v[0].x, std::max(v[1].x, v[2].x));
        float y0 = std::min(v[0].y, std::min(v[1].y, v[2].y)), y1 = std::max(v[0].y, std::max(v[1].y, v[2].y));
        bool corners = x0 < x1 && y0 < y1;
        for (int k = 0; k < 3 && corners; k++) {
            corners = (v[k].x == x0 || v[k].x == x1) && (v[k].y == y0 || v[k].y == y1);
        }
        if (!corners) {
            continue;
        }
        // the attributes' slopes across and down (the plane through the three corners)
        double ax = double(v[1].x) - v[0].x, ay = double(v[1].y) - v[0].y;
        double bx = double(v[2].x) - v[0].x, by = double(v[2].y) - v[0].y;
        double det = ax * by - bx * ay;
        if (det == 0) {
            continue; // (two corners the same)
        }
        float *attr[3][4];
        for (int k = 0; k < 3; k++) {
            attr[k][0] = &v[k].s, attr[k][1] = &v[k].t, attr[k][2] = &v[k].q, attr[k][3] = &v[k].fog;
        }
        double dx[4], dy[4];
        for (int a = 0; a < 4; a++) {
            double da = double(*attr[1][a]) - *attr[0][a], db = double(*attr[2][a]) - *attr[0][a];
            dx[a] = (da * by - db * ay) / det;
            dy[a] = (db * ax - da * bx) / det;
        }
        for (int k = 0; k < 3; k++) {
            // the vertex shader adds half a GS pixel: ceil(x) - 0.5 puts the corner on the left edge of
            // GS pixel ceil(x) (at any scale, of its first pixel of the target)
            double nx = ceil(v[k].x) - 0.5, ny = ceil(v[k].y) - 0.5;
            for (int a = 0; a < 4; a++) {
                *attr[k][a] = float(*attr[k][a] + dx[a] * (nx - v[k].x) + dy[a] * (ny - v[k].y));
            }
            v[k].x = float(nx);
            v[k].y = float(ny);
        }
        moved = true;
        count++;
    }
    if (all) {
        *all = moved && count == verts.size() / 3;
    }
    return moved;
}

// --native-2d smooth: the texels a triangle of the 2D covers as the game gave its corners (before flat_2d
// moves them out to the edges of the GS pixels): the first and last across (out[0], bits 0-15 and 16-31)
// and down (out[1]), within the texture. The cubic's taps are kept inside them (gs_frag.h, sample_level).
// A letter of the font is a sprite over its own cell of a sheet of letters (10 x 20 texels, drawn 7 or 8
// GS pixels across); taps past the cell read the letters beside it and below it, and made faint marks at
// the letters' edges (a tick left of the I of "Idol", dots by f and S, specks under letters). (Not the
// texels the PS2 reads: a cell's last column, which the PS2 skips, can hold the letter's edge.)
void texel_rect_2d(const Vtx *v, bool fst, uint32_t tex_size, uint32_t out[2]) {
    double size[2] = {double(tex_size & 0xFFFF), double(tex_size >> 16)};
    for (int a = 0; a < 2; a++) {
        double lo = INFINITY, hi = -INFINITY;
        for (int k = 0; k < 3; k++) {
            double c = a ? v[k].t : v[k].s;
            c = fst ? c : c / v[k].q * size[a];
            lo = std::min(lo, c), hi = std::max(hi, c);
        }
        // (the GS's texel coordinates have 4 bits of fraction: within a sixteenth of a whole number, on it)
        double first = std::min(std::max(floor(lo + 1.0 / 32.0), 0.0), size[a] - 1.0);
        double last = std::min(std::max(ceil(hi - 1.0 / 32.0) - 1.0, first), size[a] - 1.0);
        out[a] = uint32_t(first) | uint32_t(last) << 16;
    }
}

// ---- The CPU's VRAM ----------------------------------------------------------------------------
void pages_written(uint32_t lo, uint32_t count) {
    gs->vram_writes++;
    for (uint32_t i = 0; i < count && i < vram_pages; i++) {
        gs->page_written[(lo + i) % vram_pages] = gs->vram_writes;
    }
}

// The pages a buffer of this format, base and width covers, `height` pixels down
void buffer_pages(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t width, uint32_t height, uint32_t *lo,
                  uint32_t *count) {
    uint32_t pw = page_width(psm), ph = page_height(psm);
    uint32_t across = std::max(std::max(bw, 1u) * 64 / pw, (width + pw - 1) / pw);
    uint32_t rows = (height + ph - 1) / ph;
    *lo = bp / 32;
    *count = across * rows;
    if (bp % 32) {
        // A buffer that starts mid-page reaches into the page after its last only where one of its
        // blocks is numbered past the page's 32 once its start is added. The blocks of a page it
        // covers are those of the part of a page its pixels reach (all of it past the first page across
        // or down). Counted as one page more whatever it covers, a CLUT uploaded to the last page of
        // VRAM (bp 0x3FE1, 8x2) reached round to page 0, the frame buffer: written back and dropped,
        // the frame shown from VRAM at 1x each time (the "N Gold" message, October 2026).
        uint32_t cw = (across > 1 || rows > 1) ? pw : std::min(width, pw);
        uint32_t ch = rows > 1 ? ph : std::min(height, ph);
        uint32_t most = 0;
        for (uint32_t y = 0; y < std::max(ch, 1u); y += 8) {
            for (uint32_t x = 0; x < std::max(cw, 1u); x += 8) {
                // (blocks are at least 8x8 in every layout: their first pixels found every 8)
                most = std::max(most, gsn_vram_offset(psm, 0, pw / 64, x, y, nullptr) / 256);
            }
        }
        if (most + bp % 32 >= 32) {
            *count += 1;
        }
    }
}

bool pages_overlap(uint32_t lo_a, uint32_t count_a, uint32_t lo_b, uint32_t count_b) {
    // pages wrap at 4 MB: compare on the circle
    uint32_t d = (lo_b + vram_pages - lo_a) % vram_pages;
    uint32_t e = (lo_a + vram_pages - lo_b) % vram_pages;
    return d < count_a || e < count_b;
}

void target_pages(const Target &t, uint32_t *lo, uint32_t *count) {
    buffer_pages(t.bp, t.bw, t.psm, t.width, t.height, lo, count);
}

// A target whose pages are rows of bw pages from bp (on a page, and as wide as its buffer): each of
// its pages is a known 64-pixel-wide rectangle of it, so what is newer than VRAM is kept by page.
// (Otherwise all of a dirty target counts as dirty.)
bool regular(const Target &t) {
    return t.bp % 32 == 0 && t.bw > 0 && t.width == t.bw * 64;
}

// The target's pages, none newer than VRAM
void clear_dirty(Target &t) {
    uint32_t lo, count;
    target_pages(t, &lo, &count);
    t.dirty_pages.assign(count, 0);
    t.dirty = false;
    t.stale_pages.assign(count, 0);
    t.stale = false;
}

// (standalone, P8: gsn_standalone.inc, included at the end of this namespace)
void sa_drawn(const Target &t, uint32_t y0, uint32_t y1);
bool sa_flush();
void sa_draw_begin(uint64_t value);
void sa_draw_end();
bool sa_waiting();
void sa_sync();
void sa_transfer(uint32_t dir);
void sa_forget();
void sa_switch(bool on);
void sa_copied(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);
void sa_upload_begins(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, bool recorded);
void sa_uploaded(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const uint8_t *data, size_t size, uint64_t hash);
void sa_uploaded_from_vram(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);
void sa_upload_replayed(uint32_t k);
void sa_vblank(bool extra);
void sa_clut_load(uint64_t tex0);
void sa_verify(uint64_t tex0, uint32_t levels, const uint32_t *mip_tbp, const uint32_t *mip_tbw, uint32_t cols,
               uint32_t rows);
void sa_set_map(int map);
extern volatile int sa_mode;

// Rows y0..y1-1 of the target drawn into (all of it, for one that is not regular)
void mark_dirty(Target &t, uint32_t y0, uint32_t y1) {
    sa_drawn(t, y0, y1); // (standalone: its pages not their uploads' any more)
    t.dirty = true;
    if (!regular(t)) {
        std::fill(t.dirty_pages.begin(), t.dirty_pages.end(), 1);
        return;
    }
    y1 = std::min(y1, t.height);
    uint32_t ph = page_height(t.psm);
    for (uint32_t r = y0 / ph; y0 < y1 && r <= (y1 - 1) / ph; r++) {
        for (uint32_t c = 0; c < t.bw && size_t(r) * t.bw + c < t.dirty_pages.size(); c++) {
            t.dirty_pages[size_t(r) * t.bw + c] = 1;
        }
    }
}

// Whether a page of these is one of the target's newer than VRAM
bool dirty_in(const Target &t, uint32_t lo, uint32_t count) {
    if (!t.dirty) {
        return false;
    }
    uint32_t tlo, tcount;
    target_pages(t, &tlo, &tcount);
    if (!regular(t)) {
        return pages_overlap(lo, count, tlo, tcount);
    }
    for (uint32_t k = 0; k < tcount && k < t.dirty_pages.size(); k++) {
        if (t.dirty_pages[k] && pages_overlap(lo, count, (tlo + k) % vram_pages, 1)) {
            return true;
        }
    }
    return false;
}

// ---- Targets ------------------------------------------------------------------------------------
// The GS's bytes of one pixel of a colour target's image (RGBA8), for its format
uint32_t target_pixel_to_vram(uint32_t rgba, uint32_t psm) {
    if (psm == GSN_PSMCT16 || psm == GSN_PSMCT16S) {
        uint32_t r = rgba & 0xFF, g = (rgba >> 8) & 0xFF, b = (rgba >> 16) & 0xFF, a = rgba >> 24;
        return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    }
    if (psm == GSN_PSMCT24) {
        return rgba & 0xFFFFFF;
    }
    return rgba;
}

uint32_t vram_to_target_pixel(uint32_t v, uint32_t psm) {
    if (psm == GSN_PSMCT16 || psm == GSN_PSMCT16S) {
        return ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) | ((v & 0x8000) ? 0x80000000u : 0);
    }
    return v;
}

double depth_scale(uint32_t psm) {
    return psm == GSN_PSMZ32 ? 1.0 / 4294967296.0 : 1.0 / 16777216.0;
}

uint32_t depth_max(uint32_t psm) {
    return psm == GSN_PSMZ32 ? 0xFFFFFFFFu : psm == GSN_PSMZ24 ? 0xFFFFFFu : 0xFFFFu;
}

// The target's pixels into the CPU's VRAM
// ---- Write-backs put off (shadows) -------------------------------------------------------------
// A target an upload (or a copy) writes part of in another shape had to go back to VRAM before the
// write: the rest of its pages are newer than VRAM. Reading it back waits for the GPU to finish all it
// was given (12 ms a time on map 060's shop, where the sky's 128 x 128 copy is drawn and then its pages
// take the shop's textures, a frame's uploads covering them all but one of them only in part). Now the
// target is kept aside instead, as a shadow of its pages, and what is written over them from then on
// is noted, bit by bit (shadow_mask: the transfer done again into it with every bit set). A page written
// over whole needs nothing of it. When the CPU's VRAM of a page still owed is wanted (claim_pages, a
// write-back over it, a save state), that page goes back as it would have, and then what was written
// since goes over it again: VRAM as it would have been.
struct Shadow {
    std::unique_ptr<Target> t;
    uint32_t lo, count; // its pages
};
std::vector<Shadow> shadows;
std::vector<uint8_t> shadow_mask; // (GSN_VRAM_BYTES: the bits written since a shadow was made)
bool shadow_busy;                  // (write_back for a shadow: no shadows looked at again)

void write_back(Target &t);
void shadow_resolve(uint32_t lo, uint32_t count);

void write_back(Target &t) {
    Timer timer(gsn_stats.ns_writeback);
    if (!t.dirty) {
        return;
    }
    if (!shadow_busy && !shadows.empty()) {
        // (a shadow over its pages is older: it goes back first)
        uint32_t tlo, tcount;
        target_pages(t, &tlo, &tcount);
        shadow_resolve(tlo, tcount);
    }
    Why why(GSN_WHY_WRITEBACK);
    msaa_settle(t);
    gsn_stats.writebacks++;
    GSN_LOG(1, "write back target bp %#x bw %u psm %#x %ux%u\n", t.bp, t.bw, t.psm, t.width, t.height);
    uint32_t lo, count;
    target_pages(t, &lo, &count);
    // the rows of pages with a page newer than VRAM, read; those pages written
    uint32_t ph = page_height(t.psm), row0 = 0, row1 = t.height;
    bool by_page = regular(t) && t.dirty_pages.size() == count;
    if (by_page) {
        uint32_t first = count, last = 0;
        for (uint32_t k = 0; k < count; k++) {
            if (t.dirty_pages[k]) {
                first = std::min(first, k);
                last = k;
            }
        }
        if (first == count) {
            t.dirty = false;
            return;
        }
        row0 = first / t.bw * ph;
        row1 = std::min(t.height, (last / t.bw + 1) * ph);
    }
    uint32_t sx = t.sx, sy = t.sy, pw = t.width * sx;
    const uint8_t *pixels;
    if (!t.depth && (sx > 1 || sy > 1)) {
        // A scaled colour target: the pixel in the middle of each GS pixel's, taken on the GPU (a blit,
        // nearest: GS pixel x reads x sx + sx / 2) into an image at the PS2's size, and only that read
        // back: at --native-scale 4 a 640 x 448 target read whole was 27 MB a write-back (the water's
        // wobble copies the frame into VRAM that the next frame's textures go through: one a frame)
        static ImageHandle small;
        uint32_t rows = row1 - row0;
        if (!small || small->get_width() < t.width || small->get_height() < rows ||
            small->get_format() != t.image->get_format()) {
            small = make_image(std::max(t.width, small ? small->get_width() : 0u),
                               std::max(rows, small ? small->get_height() : 0u), t.image->get_format(), 1, false);
        }
        end_pass();
        transfer_barrier();
        cmd().blit_image(*small, *t.image, {0, 0, 0}, {int32_t(t.width), int32_t(rows), 1}, {0, int32_t(row0 * sy), 0},
                         {int32_t(pw), int32_t(rows * sy), 1}, VK_FILTER_NEAREST);
        pixels = read_image(*small, 0, 0, t.width, rows);
        sx = sy = 1;
        pw = t.width;
    } else {
        pixels = read_image(*t.image, 0, row0 * sy, pw, (row1 - row0) * sy);
    }
    // (a CT24 target's alpha is VRAM's (CT24 draws do not write it) unless a CT32 draw into the same
    // buffer wrote it: the whole word goes back either way)
    uint32_t psm = t.depth ? t.psm : psm_layout(t.psm);
    std::vector<uint32_t> row(t.width);
    for (uint32_t y = row0; y < row1; y++) {
        // a run of pixels to go back: the row, or a page's part of it
        for (uint32_t x = 0; x < t.width;) {
            uint32_t n = t.width - x;
            if (by_page) {
                n = std::min(n, 64u);
                if (!t.dirty_pages[(y / ph) * t.bw + x / 64]) {
                    x += n;
                    continue;
                }
            }
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v;
                // a scaled target: the pixel in the middle of the GS pixel's
                memcpy(&v, pixels + (size_t((y - row0) * sy + sy / 2) * pw + (x + i) * sx + sx / 2) * 4, 4);
                if (t.depth) {
                    float f;
                    memcpy(&f, &v, 4);
                    double z = double(f) / depth_scale(t.psm) + 0.5;
                    row[i] = z >= double(depth_max(t.psm)) ? depth_max(t.psm) : uint32_t(z);
                } else {
                    row[i] = target_pixel_to_vram(v, psm);
                }
            }
            gsn_vram_write_row(gs->vram.data(), psm, t.bp, t.bw, x, y, n, row.data());
            x += n;
        }
    }
    done_reading();
    if (by_page) {
        for (uint32_t k = 0; k < count; k++) {
            if (t.dirty_pages[k]) {
                pages_written((lo + k) % vram_pages, 1);
            }
        }
    } else {
        pages_written(lo, count);
    }
    clear_dirty(t);
}

// (load_target_rect) A scaled target's loads are scaled on the GPU where its format can be blitted both ways, and
// not with KF4_GSN_LOAD_SCALE=0 (the rows made at the scale on the CPU, as before: to compare)
bool load_scaled_on_gpu(VkFormat format) {
    static int off = -1;
    std::vector<std::pair<VkFormat, bool>> &formats = gs->blit_formats;
    if (off < 0) {
        const char *v = getenv("KF4_GSN_LOAD_SCALE");
        off = v && *v == '0';
    }
    if (off) {
        return false;
    }
    for (const auto &f : formats) {
        if (f.first == format) {
            return f.second;
        }
    }
    bool ok = gs->device->image_format_is_supported(format, VK_FORMAT_FEATURE_2_BLIT_SRC_BIT | VK_FORMAT_FEATURE_2_BLIT_DST_BIT);
    formats.push_back({format, ok});
    return ok;
}

// The CPU's VRAM into a rectangle of the target
void load_target_rect(Target &t, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    if (x0 >= t.width || y0 >= t.height) {
        return;
    }
    w = std::min(w, t.width - x0);
    h = std::min(h, t.height - y0);
    if (w == 0 || h == 0) {
        return;
    }
    if (!shadows.empty()) {
        // (VRAM as it should be under the rectangle: a shadow's pages owed there first)
        uint32_t tlo, tcount;
        target_pages(t, &tlo, &tcount);
        shadow_resolve(tlo, tcount);
    }
    Why why(GSN_WHY_UPLOAD);
    // (a target that is not the render pass's, nor read by its draws so far: loaded before the pass began, which
    // goes on; Cmd::before_pass)
    bool before = gs->in_pass && &t != gs->pass_color && &t != gs->pass_depth && !t.ms_newer &&
                  cmd().before_pass_can(t.image.get(), t.ms.get());
    if (before) {
        cmd().before_pass_begin();
        gsn_stats.loads_before_pass++;
    } else {
        msaa_settle(t); // (the rest of the image stays)
        end_pass();
    }
    transfer_barrier();
    VkImageAspectFlags aspect = t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t sx = t.sx, sy = t.sy, pw = w * sx, ph = h * sy;
    uint32_t rows = h; // (made here at the target's scale, below)
    if ((sx > 1 || sy > 1) && load_scaled_on_gpu(t.image->get_format())) {
        // A scaled target: the rectangle sent at the PS2's size into a staging image, and a blit (nearest) makes each
        // GS pixel its sx x sy pixels in the target. Destination pixel x reads source (x + 0.5) / sx, which is never a
        // whole number, so each lands on its own GS pixel: the target's pixels as the rows repeated below made them,
        // with sx sy times fewer bytes made, copied and sent (at --native-scale 4 and native widescreen, 24: on map
        // 009 at 240 most of them the recording's, which also kept them, copied them to a buffer and copied them
        // again each picture). In a picture between's recording each rectangle has a place of its own in the
        // recording's staging images, written once as a decoded texture is; the recording makes the blit again.
        const Image *stage = nullptr;
        uint32_t at_x = 0, at_y = 0;
        if (recording_ops->on) {
            Recording &r = *recording_ops;
            for (Recording::Stage &s : r.stages) {
                if (s.image->get_format() != t.image->get_format()) {
                    continue;
                }
                if (s.x + w > s.image->get_width()) {
                    s.x = 0;
                    s.y += s.row;
                    s.row = 0;
                }
                if (s.x + w <= s.image->get_width() && s.y + h <= s.image->get_height()) {
                    stage = s.image.get();
                    at_x = s.x;
                    at_y = s.y;
                    s.x += w;
                    s.row = std::max(s.row, h);
                    break;
                }
            }
            if (!stage) {
                Recording::Stage s;
                s.image = make_image(std::max(w, 1024u), std::max(h, 512u), t.image->get_format(), 1, false);
                s.x = w;
                s.row = h;
                stage = s.image.get();
                r.stages.push_back(std::move(s));
            }
        } else {
            // (the frame's: one image, used again load after load; the barriers around each load order them)
            ImageHandle &s = gs->load_stage[t.depth ? 2 : wide(*t.image) ? 1 : 0];
            if (!s || s->get_format() != t.image->get_format() || s->get_width() < w || s->get_height() < h) {
                s = make_image(std::max(w, s ? s->get_width() : 256u), std::max(h, s ? s->get_height() : 256u),
                               t.image->get_format(), 1, false);
            }
            stage = s.get();
        }
        void *out = cmd().update_image(*stage, {int32_t(at_x), int32_t(at_y), 0}, {w, h, 1}, w, h, {aspect, 0, 0, 1});
        std::vector<uint32_t> row(w);
        size_t row_bytes = size_t(w) * (wide(*t.image) ? 8 : 4);
        for (uint32_t y = 0; y < h; y++) {
            gsn_vram_read_row(gs->vram.data(), t.depth ? t.psm : psm_layout(t.psm), t.bp, t.bw, x0, y0 + y, w, row.data());
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = row[x];
                if (t.depth) {
                    float f = float(double(v) * depth_scale(t.psm));
                    memcpy(&v, &f, 4);
                } else {
                    v = vram_to_target_pixel(v, t.psm);
                }
                row[x] = v;
            }
            put_pixels(static_cast<uint8_t *>(out) + size_t(y) * row_bytes, *t.image, 0, row.data(), w);
        }
        transfer_barrier();
        cmd().blit_image_recorded(*t.image, *stage, {int32_t(x0 * sx), int32_t(y0 * sy), 0}, {int32_t(pw), int32_t(ph), 1},
                                  {int32_t(at_x), int32_t(at_y), 0}, {int32_t(w), int32_t(h), 1}, VK_FILTER_NEAREST);
        gsn_stats.loads_scaled++;
        rows = 0;
    }
    void *out = rows ? cmd().update_image(*t.image, {int32_t(x0 * sx), int32_t(y0 * sy), 0}, {pw, ph, 1}, pw, ph,
                                          {aspect, 0, 0, 1})
                     : nullptr;
    std::vector<uint32_t> row(w), line(pw);
    std::vector<uint8_t> converted;
    for (uint32_t y = 0; y < rows; y++) {
        // (a CT24 target holds VRAM's alpha too: write_back writes the whole word)
        gsn_vram_read_row(gs->vram.data(), t.depth ? t.psm : psm_layout(t.psm), t.bp, t.bw, x0, y0 + y, w, row.data());
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = row[x];
            if (t.depth) {
                float f = float(double(v) * depth_scale(t.psm));
                memcpy(&v, &f, 4);
            } else {
                v = vram_to_target_pixel(v, t.psm);
            }
            for (uint32_t i = 0; i < sx; i++) {
                line[x * sx + i] = v;
            }
        }
        // (the row made here, then copied: the upload's memory is not for reading)
        size_t row_bytes = size_t(pw) * (wide(*t.image) ? 8 : 4);
        converted.resize(row_bytes);
        put_pixels(converted.data(), *t.image, 0, line.data(), pw);
        for (uint32_t j = 0; j < sy; j++) {
            memcpy(static_cast<uint8_t *>(out) + size_t(y * sy + j) * row_bytes, converted.data(), row_bytes);
        }
    }
    transfer_barrier();
    if (before) {
        cmd().before_pass_end();
    }
    msaa_image_written(t);
    // the pages wholly inside the rectangle hold what VRAM does now
    if (t.dirty && regular(t)) {
        uint32_t pgh = page_height(t.psm);
        // (the last row of pages may go past the target's height: it has only the rows above that)
        for (uint32_t r = (y0 + pgh - 1) / pgh; r * pgh < t.height && std::min((r + 1) * pgh, t.height) <= y0 + h; r++) {
            for (uint32_t c = (x0 + 63) / 64; (c + 1) * 64 <= x0 + w && c < t.bw; c++) {
                if (size_t(r) * t.bw + c < t.dirty_pages.size()) {
                    t.dirty_pages[size_t(r) * t.bw + c] = 0;
                }
            }
        }
        t.dirty = std::find(t.dirty_pages.begin(), t.dirty_pages.end(), 1) != t.dirty_pages.end();
    }
}

void drop_target(Target *t) {
    GSN_LOG(1, "drop target bp %#x bw %u psm %#x %ux%u%s\n", t->bp, t->bw, t->psm, t->width, t->height,
            t->dirty ? " (dirty)" : "");
    if (gs->pass_color == t || gs->pass_depth == t) {
        end_pass();
    }
    for (size_t i = 0; i < gs->targets.size(); i++) {
        if (gs->targets[i].get() == t) {
            gs->targets.erase(gs->targets.begin() + long(i));
            return;
        }
    }
}

// KF4_GSN_SHADOWS=0: write-backs not put off (as before)
bool shadows_on() {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("KF4_GSN_SHADOWS");
        on = !(v && atoi(v) == 0);
    }
    return on != 0;
}

// The target, gone from the targets, kept as a shadow (above) of the pages it has newer than VRAM
void shadow_make(Target *t) {
    if (gs->pass_color == t || gs->pass_depth == t) {
        end_pass();
    }
    msaa_settle(*t); // (its image as drawn)
    for (size_t i = 0; i < gs->targets.size(); i++) {
        if (gs->targets[i].get() == t) {
            Shadow sh;
            sh.t = std::move(gs->targets[i]);
            gs->targets.erase(gs->targets.begin() + long(i));
            target_pages(*sh.t, &sh.lo, &sh.count);
            if (shadow_mask.empty()) {
                shadow_mask.assign(GSN_VRAM_BYTES, 0);
            }
            for (uint32_t k = 0; k < sh.count; k++) {
                memset(shadow_mask.data() + size_t((sh.lo + k) % vram_pages) * 8192, 0, 8192);
            }
            GSN_LOG(1, "shadow of target bp %#x bw %u psm %#x %ux%u\n", t->bp, t->bw, t->psm, t->width, t->height);
            gsn_stats.shadows++;
            shadows.push_back(std::move(sh));
            return;
        }
    }
}

// A transfer is about to write its rectangle (DBP, DBW, DPSM; DSAX, DSAY; RRW, RRH): over a shadow, the
// bits it writes noted, and its pages written over whole let go. (An upload of whole pages sets theirs whole,
// and only the pages written are looked at: a shadow left over a frame's copy under the MP drain's wobble saw
// every upload of the pictures between swizzled again into the mask, 27 us each, about 4 ms a game frame.)
std::vector<uint32_t> whole_pages(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, bool *rgb_only);
void shadow_written(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg) {
    if (shadows.empty()) {
        return;
    }
    GsnUpload u;
    gsn_upload_begin(&u, bitbltbuf, trxpos, trxreg);
    uint32_t lo, count;
    buffer_pages(u.bp, u.bw, u.psm, u.x0 + u.w, u.y0 + u.h, &lo, &count);
    bool any = false;
    for (const Shadow &sh : shadows) {
        any = any || pages_overlap(lo, count, sh.lo, sh.count);
    }
    if (!any) {
        return;
    }
    // the transfer again, every bit of every pixel set, into the mask
    std::vector<uint32_t> whole = whole_pages(bitbltbuf, trxpos, trxreg, nullptr);
    if (!whole.empty()) {
        for (uint32_t page : whole) {
            memset(shadow_mask.data() + size_t(page) * 8192, 0xFF, 8192);
        }
    } else {
        size_t bytes = (size_t(u.w) * u.h * size_t(gsn_psm_bits(u.psm)) + 7) / 8;
        std::vector<uint8_t> ones(bytes + 16, 0xFF);
        gsn_upload_data(&u, shadow_mask.data(), ones.data(), ones.size(), nullptr, nullptr);
    }
    for (size_t i = 0; i < shadows.size();) {
        Shadow &sh = shadows[i];
        Target &t = *sh.t;
        bool left = false;
        for (uint32_t k = 0; k < sh.count && k < t.dirty_pages.size(); k++) {
            if (!t.dirty_pages[k]) {
                continue;
            }
            if (!pages_overlap(lo, count, (sh.lo + k) % vram_pages, 1)) {
                left = true; // (not written now: as it was)
                continue;
            }
            const uint8_t *m = shadow_mask.data() + size_t((sh.lo + k) % vram_pages) * 8192;
            bool whole = true;
            for (size_t b = 0; b < 8192 && whole; b += 8) {
                uint64_t v;
                memcpy(&v, m + b, 8);
                whole = v == ~uint64_t(0);
            }
            if (whole) {
                t.dirty_pages[k] = 0; // (written over: nothing of the shadow's is owed there)
            } else {
                left = true;
            }
        }
        if (!left) {
            gsn_stats.shadows_gone++;
            shadows.erase(shadows.begin() + long(i));
            continue;
        }
        i++;
    }
}

// The pages lo..count of the CPU's VRAM as they would be had the shadows over them gone back to VRAM
// when they were made
void shadow_resolve(uint32_t lo, uint32_t count) {
    if (shadow_busy) {
        return;
    }
    for (size_t i = 0; i < shadows.size();) {
        Shadow &sh = shadows[i];
        Target &t = *sh.t;
        if (!pages_overlap(lo, count, sh.lo, sh.count)) {
            i++;
            continue;
        }
        // its pages owed among these: written back alone, then what was written since put back over them
        std::vector<uint8_t> keep = t.dirty_pages, now(t.dirty_pages.size(), 0);
        std::vector<uint32_t> pages;
        for (uint32_t k = 0; k < sh.count && k < t.dirty_pages.size(); k++) {
            uint32_t page = (sh.lo + k) % vram_pages;
            if (t.dirty_pages[k] && pages_overlap(lo, count, page, 1)) {
                now[k] = 1;
                keep[k] = 0;
                pages.push_back(page);
            }
        }
        if (pages.empty()) {
            i++;
            continue;
        }
        std::vector<uint8_t> since(pages.size() * 8192);
        for (size_t p = 0; p < pages.size(); p++) {
            memcpy(since.data() + p * 8192, gs->vram.data() + size_t(pages[p]) * 8192, 8192);
        }
        t.dirty_pages = now;
        t.dirty = true;
        shadow_busy = true;
        write_back(t);
        shadow_busy = false;
        gsn_stats.shadows_resolved++;
        GSN_LOG(1, "shadow of target bp %#x: %zu pages written back\n", t.bp, pages.size());
        for (size_t p = 0; p < pages.size(); p++) {
            uint8_t *v = gs->vram.data() + size_t(pages[p]) * 8192;
            const uint8_t *m = shadow_mask.data() + size_t(pages[p]) * 8192, *w = since.data() + p * 8192;
            for (size_t b = 0; b < 8192; b++) {
                v[b] = uint8_t((v[b] & ~m[b]) | (w[b] & m[b]));
            }
        }
        t.dirty_pages = keep;
        t.dirty = std::find(keep.begin(), keep.end(), 1) != keep.end();
        if (!t.dirty) {
            shadows.erase(shadows.begin() + long(i));
            continue;
        }
        i++;
    }
}

void shadows_resolve_all() {
    shadow_resolve(0, vram_pages);
}

// Everything on the GPU that is newer than these pages of the CPU's VRAM, written back; and the
// targets over them that are not `keep`, gone (they are about to be written in another shape)
bool shares_pages(const Target &a, const Target &b);

// (sharer: targets that share pages with it (shares_pages) are left alone: take_pages moves pages
// between them)
// Pages a GPU copy of the frame is about to write whole (begin_transfer: every bit of each, from a target): a
// target lying wholly in them is dropped without going back to VRAM first, and a target made over them is not
// loaded from VRAM (the copy covers it). count 0: none.
uint32_t overwrite_lo = 0, overwrite_count = 0;

bool pages_within(uint32_t lo, uint32_t count, uint32_t outer_lo, uint32_t outer_count) {
    for (uint32_t k = 0; k < count; k++) {
        if (((lo + k - outer_lo) % vram_pages) >= outer_count) {
            return false;
        }
    }
    return true;
}

void claim_targets(uint32_t lo, uint32_t count, const Target *keep, bool drop, const Target *sharer);
void claim_pages(uint32_t lo, uint32_t count, const Target *keep, bool drop, const Target *sharer = nullptr) {
    shadow_resolve(lo, count);
    claim_targets(lo, count, keep, drop, sharer);
}

// (shadow_covers' marks, GSN_VRAM_BYTES: kept clear between calls)
std::vector<uint8_t> shadow_read_marks;

// A texture's level decoded from VRAM (bp, bw, psm: the w x h texels from 0, 0 its decode reads), its pages lo..count
// claimed: true when every bit of it on those pages that a shadow still owes was written since the shadow was made
// (shadow_mask). VRAM then holds those bits as they would be had the shadow gone back, and it need not go back. The MP
// drain's and the water's wobble copy the frame to 0x2BC0; the effects' textures go up over that copy after
// the wobble (Effect_UploadTextures), covering a page only in part, and one of them (a 64 x 64 PSMT8 at 0x3780,
// half a page) is read by a draw every frame: its page was written back for it, a read-back waiting for the
// GPU every frame (3-4 ms at 3x). KF4_GSN_SHADOW_READS=0: such a read takes the pages owed, as before.
bool shadow_covers(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h, uint32_t lo, uint32_t count) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("KF4_GSN_SHADOW_READS");
        on = !(v && atoi(v) == 0);
    }
    if (!on || shadows.empty() || shadow_mask.empty() || !gsn_psm_valid(psm)) {
        return false;
    }
    // the pages owed here
    std::vector<uint32_t> owed;
    for (const Shadow &sh : shadows) {
        for (uint32_t k = 0; k < sh.count && k < sh.t->dirty_pages.size(); k++) {
            uint32_t page = (sh.lo + k) % vram_pages;
            if (sh.t->dirty_pages[k] && pages_overlap(lo, count, page, 1)) {
                owed.push_back(page);
            }
        }
    }
    if (owed.empty()) {
        return true; // (nothing owed on these pages: shadow_resolve would do nothing)
    }
    if (shadow_read_marks.empty()) {
        shadow_read_marks.assign(GSN_VRAM_BYTES, 0);
    }
    // the texels marked as an upload of the rectangle would write them, every bit set
    GsnUpload u;
    uint64_t bitbltbuf = uint64_t(bp) << 32 | uint64_t(bw) << 48 | uint64_t(psm) << 56;
    uint64_t trxreg = uint64_t(w) | uint64_t(h) << 32;
    gsn_upload_begin(&u, bitbltbuf, 0, trxreg);
    size_t bytes = (size_t(w) * h * size_t(gsn_psm_bits(psm)) + 7) / 8;
    std::vector<uint8_t> ones(bytes + 16, 0xFF);
    uint32_t mlo = 1, mhi = 0;
    gsn_upload_data(&u, shadow_read_marks.data(), ones.data(), ones.size(), &mlo, &mhi);
    bool covered = true;
    for (uint32_t page : owed) {
        const uint8_t *r = shadow_read_marks.data() + size_t(page) * 8192, *m = shadow_mask.data() + size_t(page) * 8192;
        for (size_t b = 0; b < 8192 && covered; b += 8) {
            uint64_t rv, mv;
            memcpy(&rv, r + b, 8);
            memcpy(&mv, m + b, 8);
            covered = (rv & ~mv) == 0;
        }
    }
    if (mlo <= mhi) {
        memset(shadow_read_marks.data() + size_t(mlo) * 8192, 0, size_t(mhi - mlo + 1) * 8192);
    }
    return covered;
}

// (a texture's level read from VRAM) claim_pages, the shadows left owed where the read needs nothing of them
void claim_texture(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h, uint32_t lo, uint32_t count) {
    if (shadow_covers(bp, bw, psm, w, h, lo, count)) {
        gsn_stats.shadow_reads_covered++;
        claim_targets(lo, count, nullptr, false, nullptr);
        return;
    }
    claim_pages(lo, count, nullptr, false);
}

void claim_targets(uint32_t lo, uint32_t count, const Target *keep, bool drop, const Target *sharer) {
    for (size_t i = 0; i < gs->targets.size();) {
        Target *t = gs->targets[i].get();
        uint32_t tlo, tcount;
        target_pages(*t, &tlo, &tcount);
        if (t != keep && pages_overlap(lo, count, tlo, tcount) && !(sharer && shares_pages(*sharer, *t))) {
            if (drop) {
                // (a picture between's own: VRAM keeps the game's, prepare_write; one under a copy's whole pages:
                // all of it written over)
                if (!t->replay_only && !(overwrite_count && !t->depth && pages_within(tlo, tcount, overwrite_lo, overwrite_count))) {
                    write_back(*t);
                } else if (!t->replay_only) {
                    gsn_stats.writebacks_overwritten++;
                }
                drop_target(t);
                continue;
            }
            // (a target whose pages here hold what VRAM does need not go back)
            if (dirty_in(*t, lo, count)) {
                write_back(*t);
            }
        }
        i++;
    }
}

Target *find_target(uint32_t bp, uint32_t bw, uint32_t psm) {
    for (auto &t : gs->targets) {
        if (t->bp == bp && t->bw == bw && psm_layout(t->psm) == psm_layout(psm) && t->depth == psm_is_depth(psm)) {
            return t.get();
        }
    }
    return nullptr;
}

// A colour buffer that starts a whole number of page rows from a colour target's start, as wide and in
// the same layout, is the target's memory from that row on: its pixel (x, y) is the target's pixel
// (x, y + *rows). (*rows below 0: the buffer starts above the target.)
bool row_alias(const Target &t, uint32_t bp, uint32_t bw, uint32_t psm, int32_t *rows) {
    if (t.depth || psm_is_depth(psm) || t.bw != bw || bw == 0 || psm_layout(t.psm) != psm_layout(psm)) {
        return false;
    }
    // (a colour format's page is 64 pixels across: a row of pages is bw pages, 32 blocks each)
    int32_t d = int32_t(bp) - int32_t(t.bp), row = int32_t(bw) * 32;
    if (d % row != 0) {
        return false;
    }
    *rows = d / row * int32_t(page_height(t.psm));
    return true;
}

// A target made again in a new size, its old picture copied in `rows` rows down: what was newer than
// VRAM in the old one is in the new one
void carry_dirty(Target &t, const Target &old, uint32_t rows) {
    if (!old.dirty) {
        return;
    }
    t.dirty = true;
    if (regular(t) && regular(old) && old.bw == t.bw && rows % page_height(t.psm) == 0) {
        size_t shift = size_t(rows / page_height(t.psm)) * t.bw;
        for (size_t k = 0; k < old.dirty_pages.size() && k + shift < t.dirty_pages.size(); k++) {
            t.dirty_pages[k + shift] |= old.dirty_pages[k];
        }
    } else {
        std::fill(t.dirty_pages.begin(), t.dirty_pages.end(), 1);
    }
}

// Two Z buffers in the same format at the same address, at different widths (map 008 and 009's
// reflection draws with the main Z buffer's address at 256 wide, each frame): a page is the same 8 KB
// in both, its pixels in the same places, so a page moves from one to the other as a GPU copy of its
// 64-pixel-wide rectangle, where going back through VRAM cost two read-backs a frame. A page is the
// other's (stale) in all of them but one.
bool shares_pages(const Target &a, const Target &b) {
    return &a != &b && a.depth && b.depth && psm_layout(a.psm) == psm_layout(b.psm) && regular(a) && regular(b) &&
           a.sx == b.sx && a.sy == b.sy && a.bw != b.bw; // (scaled alike: a page is the same rectangle)
}

// Pages of t from the targets it shares pages with, where they are theirs: copied in on the GPU, and
// theirs no more (only_stale: just the pages t has as stale; any of those nobody has, from VRAM)
void take_pages(Target &t, bool only_stale) {
    uint32_t tlo, tcount;
    target_pages(t, &tlo, &tcount);
    uint32_t tph = page_height(t.psm), sx = t.sx, sy = t.sy;
    struct Copy {
        Target *from;
        uint32_t k_from, k_to;
    };
    std::vector<Copy> copies;
    for (auto &o : gs->targets) {
        if (!shares_pages(t, *o)) {
            continue;
        }
        uint32_t olo, ocount;
        target_pages(*o, &olo, &ocount);
        if (!pages_overlap(tlo, tcount, olo, ocount)) {
            continue;
        }
        for (uint32_t k = 0; k < tcount; k++) {
            if (only_stale && !t.stale_pages[k]) {
                continue;
            }
            uint32_t ko = ((tlo + k) % vram_pages + vram_pages - olo) % vram_pages;
            if (ko >= ocount || o->stale_pages[ko]) {
                continue;
            }
            copies.push_back({o.get(), ko, k});
            t.dirty_pages[k] = o->dirty_pages[ko];
            t.stale_pages[k] = 0;
            o->dirty_pages[ko] = 0;
            o->stale_pages[ko] = 1;
            o->stale = true;
        }
    }
    if (!copies.empty()) {
        Why why(GSN_WHY_COPY);
        // --native-msaa: the pages move between the targets' images, so each is made current first
        // (Z resolved: its first sample), and t's multisampled image is behind it after
        msaa_settle(t);
        for (const Copy &c : copies) {
            msaa_settle(*c.from);
        }
        end_pass();
        transfer_barrier();
        const VkImageSubresourceLayers z = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        for (const Copy &c : copies) {
            Target &o = *c.from;
            uint32_t oph = page_height(o.psm);
            uint32_t ox = (c.k_from % o.bw) * 64, oy = (c.k_from / o.bw) * oph;
            uint32_t tx = (c.k_to % t.bw) * 64, ty = (c.k_to / t.bw) * tph;
            uint32_t h = std::min(std::min(oph, o.height - oy), t.height - ty);
            cmd().copy_image(*t.image, *o.image, {int32_t(tx * sx), int32_t(ty * sy), 0}, {int32_t(ox * sx), int32_t(oy * sy), 0},
                             {64 * sx, h * sy, 1}, z, z);
        }
        transfer_barrier();
        msaa_image_written(t);
        gsn_stats.target_copies += copies.size();
        for (auto &o : gs->targets) {
            o->dirty = std::find(o->dirty_pages.begin(), o->dirty_pages.end(), 1) != o->dirty_pages.end();
        }
    }
    // pages nobody else has: VRAM's
    if (only_stale) {
        for (uint32_t k = 0; k < tcount; k++) {
            if (t.stale_pages[k]) {
                t.stale_pages[k] = 0;
                load_target_rect(t, (k % t.bw) * 64, (k / t.bw) * tph, 64, tph);
            }
        }
    }
    t.stale = false;
    t.dirty = std::find(t.dirty_pages.begin(), t.dirty_pages.end(), 1) != t.dirty_pages.end();
}

// The target that is the buffer at bp, at least width x height, made (from the CPU's VRAM) or grown
Target *get_target_made(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t width, uint32_t height);

Target *get_target(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t width, uint32_t height) {
    bool existed = find_target(bp, bw, psm) != nullptr;
    Target *t = get_target_made(bp, bw, psm, width, height);
    if (t) {
        // (a picture between's own target, until the game's frame draws into it)
        t->replay_only = replaying && (!existed || t->replay_only);
    }
    return t;
}

Target *get_target_made(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t width, uint32_t height) {
    Target *t = find_target(bp, bw, psm);
    width = std::max(width, std::max(bw, 1u) * 64);
    if (t && t->stale) {
        take_pages(*t, true);
    }
    if (t && t->width >= width && t->height >= height) {
        t->psm = psm;
        t->used = gs->tick;
        return t;
    }
    VkFormat format = psm_is_depth(psm) ? VK_FORMAT_D32_SFLOAT : colour_format();
    if (!t) {
        // A buffer that starts some rows above a target and runs into it (a scrolling texture's
        // target, drawn into from a page row higher the next time): the target grows upwards to start
        // where the buffer does, instead of going back to VRAM to be made again in the new shape
        Target *below = nullptr;
        int32_t rows = 0;
        for (auto &o : gs->targets) {
            if (row_alias(*o, bp, bw, psm, &rows) && rows < 0 && int32_t(height) > -rows) {
                below = o.get();
                break;
            }
        }
        if (below) {
            uint32_t down = uint32_t(-rows), sx = below->sx, sy = below->sy;
            uint32_t new_w = std::max(width, below->width), new_h = std::max(height, down + below->height);
            GSN_LOG(1, "target bp %#x grows up to bp %#x: %ux%u\n", below->bp, bp, new_w, new_h);
            end_pass();
            msaa_settle(*below);
            Target old = *below;
            below->bp = bp;
            below->psm = psm;
            below->image = make_image(new_w * sx, new_h * sy, format, 1, true);
            msaa_forget(*below);
            below->width = new_w;
            below->height = new_h;
            clear_dirty(*below);
            uint32_t lo, count;
            target_pages(*below, &lo, &count);
            claim_pages(lo, count, below, true);
            // VRAM where the old picture is not, then the old picture
            load_target_rect(*below, 0, 0, new_w, down);
            load_target_rect(*below, 0, down + old.height, new_w, new_h - (down + old.height));
            load_target_rect(*below, old.width, down, new_w - old.width, old.height);
            cmd().copy_image(*below->image, *old.image, {0, int32_t(down * sy), 0}, {0, 0, 0},
                             {old.width * sx, old.height * sy, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            transfer_barrier();
            carry_dirty(*below, old, down);
            below->used = gs->tick;
            return below;
        }
    }
    if (t) {
        // grown: the old picture kept, the new part from VRAM
        uint32_t new_w = std::max(width, t->width), new_h = std::max(height, t->height);
        ImageHandle image = make_image(new_w * t->sx, new_h * t->sy, format, 1, true);
        end_pass();
        msaa_settle(*t);
        Target old = *t;
        t->psm = psm;
        t->image = image;
        msaa_forget(*t);
        t->width = new_w;
        t->height = new_h;
        clear_dirty(*t);
        // what the old target did not cover comes from VRAM: the whole of it first, then the old on top
        uint32_t lo, count;
        target_pages(*t, &lo, &count);
        claim_pages(lo, count, t, true, t);
        load_target_rect(*t, 0, 0, new_w, new_h);
        VkImageAspectFlags aspect = t->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        cmd().copy_image(*t->image, *old.image, {0, 0, 0}, {0, 0, 0}, {old.width * t->sx, old.height * t->sy, 1},
                         {aspect, 0, 0, 1}, {aspect, 0, 0, 1});
        transfer_barrier();
        carry_dirty(*t, old, 0);
        take_pages(*t, false);
        t->used = gs->tick;
        return t;
    }
    std::unique_ptr<Target> made(new Target());
    made->bp = bp;
    made->bw = bw;
    made->psm = psm;
    made->width = width;
    made->height = height;
    made->depth = psm_is_depth(psm);
    clear_dirty(*made);
    made->used = gs->tick;
    made->sx = target_sx;
    made->sy = target_sy;
    made->image = make_image(width * made->sx, height * made->sy, format, 1, true);
    GSN_LOG(1, "new target bp %#x bw %u psm %#x %ux%u\n", bp, bw, psm, width, height);
    uint32_t lo, count;
    target_pages(*made, &lo, &count);
    // whatever else was drawn over these pages goes back to VRAM first, and stops being a target
    // (but for a Z buffer sharing its pages, which hands them over)
    claim_pages(lo, count, nullptr, true, made.get());
    if (!(overwrite_count && !made->depth && pages_within(lo, count, overwrite_lo, overwrite_count))) {
        load_target_rect(*made, 0, 0, width, height);
    }
    gs->targets.push_back(std::move(made));
    take_pages(*gs->targets.back(), false);
    return gs->targets.back().get();
}

// ---- Textures ----------------------------------------------------------------------------------
VkFormat raw_format(uint32_t psm) {
    if (psm_is_palette(psm)) {
        return VK_FORMAT_R8_UINT;
    }
    if (psm_is_16(psm)) {
        return VK_FORMAT_R16_UINT;
    }
    return VK_FORMAT_R32_UINT;
}

bool texture_current(const Texture &tex) {
    for (uint32_t l = 0; l < tex.levels; l++) {
        for (uint32_t i = 0; i < tex.page_count[l]; i++) {
            if (gs->page_written[(tex.page_lo[l] + i) % vram_pages] > tex.decoded) {
                return false;
            }
        }
    }
    return true;
}

// The bytes of a texture's pages: textures that take turns in the same VRAM (much of what the game
// uploads each frame) are decoded once each and found again by this
uint64_t texture_hash(const Texture &tex) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(tex.tbp) << 32 | tex.tbw << 24 | tex.psm << 16 | tex.tw << 8 | tex.th);
    h = (h ^ (uint64_t(tex.cols) << 16 | tex.rows)) * 0x100000001B3ull;
    for (uint32_t l = 0; l < tex.levels; l++) {
        h ^= uint64_t(tex.mip_tbp[l]) << 20 | tex.mip_tbw[l];
        for (uint32_t i = 0; i < tex.page_count[l]; i++) {
            const uint64_t *p = reinterpret_cast<const uint64_t *>(gs->vram.data()) + size_t((tex.page_lo[l] + i) % vram_pages) * 1024;
            for (int k = 0; k < 1024; k += 4) {
                h = (h ^ p[k]) * 0x100000001B3ull + (p[k + 1] ^ (p[k + 2] << 1) ^ (p[k + 3] << 2));
                h ^= h >> 29;
            }
        }
    }
    return h;
}

// Decoded textures by the hash of what they were decoded from
std::unordered_map<uint64_t, ImageHandle> decoded_pool;
std::unordered_map<uint64_t, std::array<uint32_t, 256>> decoded_texels; // (Texture::texels, by the same hash)
std::deque<uint64_t> decoded_order;
size_t decoded_pool_bytes;

size_t image_bytes(const Image &image) {
    size_t texel = image.get_format() == VK_FORMAT_R8_UINT ? 1 : image.get_format() == VK_FORMAT_R16_UINT ? 2 : 4;
    // levels after the first add a third at most
    return size_t(image.get_width()) * image.get_height() * texel * (image.get_create_info().levels > 1 ? 4 : 3) / 3;
}

void decode_texture(Texture &tex) {
    Timer timer(gsn_stats.ns_decode);
    VkFormat format = raw_format(tex.psm);
    uint64_t hash = texture_hash(tex);
    tex.decoded = gs->vram_writes;
    if (tex.image && hash == tex.hash) {
        return; // pages written, but not the bytes this texture has
    }
    tex.hash = hash;
    auto found = decoded_pool.find(hash);
    if (found != decoded_pool.end()) {
        tex.image = found->second;
        memcpy(tex.texels, decoded_texels[hash].data(), sizeof(tex.texels));
        gsn_stats.texture_reuses++;
        return;
    }
    tex.image = make_image(1u << tex.tw, 1u << tex.th, format, tex.levels, false);
    decoded_pool[hash] = tex.image;
    decoded_order.push_back(hash);
    decoded_pool_bytes += image_bytes(*tex.image);
    // the oldest let go beyond 2,048 of them or 256 MB (a texture in use keeps its image all the same)
    while (decoded_order.size() > 2048 || decoded_pool_bytes > (256u << 20)) {
        auto oldest = decoded_pool.find(decoded_order.front());
        if (oldest != decoded_pool.end()) {
            decoded_pool_bytes -= image_bytes(*oldest->second);
            decoded_pool.erase(oldest);
            decoded_texels.erase(decoded_order.front());
        }
        decoded_order.pop_front();
    }
    {
        Why why(GSN_WHY_DECODE);
        end_pass();
    }
    transfer_barrier();
    memset(tex.texels, 0, sizeof(tex.texels));
    for (uint32_t l = 0; l < tex.levels; l++) {
        uint32_t w = std::max(1u, (1u << tex.tw) >> l), h = std::max(1u, (1u << tex.th) >> l);
        void *out = cmd().update_image(*tex.image, {0, 0, 0}, {w, h, 1}, w, h, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1});
        uint32_t bp = tex.mip_tbp[l], bw = tex.mip_tbw[l];
        std::vector<uint32_t> row(w);
        for (uint32_t y = 0; y < h; y++) {
            size_t at = size_t(y) * w;
            if (format == VK_FORMAT_R32_UINT) {
                uint32_t *texel = static_cast<uint32_t *>(out) + at;
                gsn_vram_read_row(gs->vram.data(), tex.psm, bp, bw, 0, y, w, texel);
                for (uint32_t x = 0; x < w; x++) {
                    tex.texels[texel[x] >> 24]++;
                }
                continue;
            }
            gsn_vram_read_row(gs->vram.data(), tex.psm, bp, bw, 0, y, w, row.data());
            for (uint32_t x = 0; x < w; x++) {
                if (format == VK_FORMAT_R8_UINT) {
                    static_cast<uint8_t *>(out)[at + x] = uint8_t(row[x]);
                    tex.texels[row[x] & 0xFF]++;
                } else {
                    static_cast<uint16_t *>(out)[at + x] = uint16_t(row[x]);
                }
            }
        }
        gsn_stats.texture_bytes += size_t(w) * h;
    }
    memcpy(decoded_texels[hash].data(), tex.texels, sizeof(tex.texels));
    transfer_barrier();
    gsn_stats.texture_decodes++;
}

// The textures by what they are (texture_key), for the lookup each batch makes
std::unordered_map<uint64_t, Texture *> texture_index;

uint64_t texture_key(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t levels,
                     const uint32_t *mip_tbp, const uint32_t *mip_tbw, uint32_t cols, uint32_t rows) {
    uint64_t k = uint64_t(tbp) | uint64_t(tbw) << 14 | uint64_t(psm) << 20 | uint64_t(tw) << 26 | uint64_t(th) << 30 |
                 uint64_t(levels) << 34 | uint64_t(rows) << 37 | uint64_t(cols) << 48;
    for (uint32_t l = 1; l < levels; l++) {
        k = (k ^ (uint64_t(mip_tbp[l]) | uint64_t(mip_tbw[l]) << 14)) * 0x100000001B3ull;
    }
    return k;
}

bool same_texture(const Texture &t, uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t levels,
                  const uint32_t *mip_tbp, const uint32_t *mip_tbw, uint32_t cols, uint32_t rows) {
    return t.tbp == tbp && t.tbw == tbw && t.psm == psm && t.tw == tw && t.th == th && t.levels == levels &&
           t.cols == cols && t.rows == rows && memcmp(t.mip_tbp, mip_tbp, levels * 4) == 0 &&
           memcmp(t.mip_tbw, mip_tbw, levels * 4) == 0;
}

// cols, rows: of level 0, those the draw reads (1 << tw and 1 << th, or fewer without mipmaps): the
// pages it watches and hashes are theirs, so draws into the pages past them (where a texture larger
// than the part used runs on, round the end of VRAM, into the menus' field buffers) leave it as it is
Texture *get_texture(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t levels,
                     const uint32_t *mip_tbp, const uint32_t *mip_tbw, uint32_t cols, uint32_t rows) {
    uint64_t key = texture_key(tbp, tbw, psm, tw, th, levels, mip_tbp, mip_tbw, cols, rows);
    auto found = texture_index.find(key);
    if (found != texture_index.end() &&
        same_texture(*found->second, tbp, tbw, psm, tw, th, levels, mip_tbp, mip_tbw, cols, rows)) {
        Texture *t = found->second;
        t->used = gs->tick;
        if (t->decoded != gs->vram_writes && !texture_current(*t)) {
            decode_texture(*t);
        }
        return t;
    }
    std::unique_ptr<Texture> tex(new Texture());
    tex->tbp = tbp;
    tex->tbw = tbw;
    tex->psm = psm;
    tex->tw = tw;
    tex->th = th;
    tex->levels = levels;
    tex->cols = cols;
    tex->rows = rows;
    memcpy(tex->mip_tbp, mip_tbp, levels * 4);
    memcpy(tex->mip_tbw, mip_tbw, levels * 4);
    for (uint32_t l = 0; l < levels; l++) {
        buffer_pages(mip_tbp[l], mip_tbw[l], psm, std::max(1u, cols >> l), std::max(1u, rows >> l),
                     &tex->page_lo[l], &tex->page_count[l]);
    }
    tex->used = gs->tick;
    decode_texture(*tex);
    Texture *made = tex.get();
    if (found != texture_index.end()) {
        texture_index.erase(found); // (a key shared by two textures: the newer one is indexed)
    }
    texture_index[key] = made;
    gs->textures.push_back(std::move(tex));
    // a few hundred textures at most are in use at once: drop the oldest beyond that
    if (gs->textures.size() > 1024) {
        auto oldest = std::min_element(gs->textures.begin(), gs->textures.end(),
                                       [](const std::unique_ptr<Texture> &a, const std::unique_ptr<Texture> &b) {
                                           return a->used < b->used;
                                       });
        if (oldest->get() != made) {
            Texture *t = oldest->get();
            auto at = texture_index.find(
                texture_key(t->tbp, t->tbw, t->psm, t->tw, t->th, t->levels, t->mip_tbp, t->mip_tbw, t->cols, t->rows));
            if (at != texture_index.end() && at->second == t) {
                texture_index.erase(at);
            }
            gs->textures.erase(oldest);
        }
    }
    return made;
}

// ---- The CLUT ----------------------------------------------------------------------------------
// TEX0's CLD: whether the CLUT buffer is loaded now, and from where
// The entries a CLUT load reads from a VRAM, as its format has them (CSM1: the 16 x 16 or 8 x 2 rectangle,
// entries 8-15 and 16-23 of each 32 swapped; CSM2: a line of TEXCLUT's buffer): read a row at a time
void clut_read_entries(const uint8_t *vram, uint64_t tex0, uint64_t texclut, uint32_t *values) {
    uint32_t psm = bits(tex0, 20, 6), cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1);
    bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
    uint32_t entries = eight ? 256 : 16;
    uint32_t gpsm = cpsm == 0 ? GSN_PSMCT32 : cpsm == 2 ? GSN_PSMCT16 : GSN_PSMCT16S;
    if (csm != 0) {
        gsn_vram_read_row(vram, gpsm, cbp, bits(texclut, 0, 6), bits(texclut, 6, 6) * 16, bits(texclut, 12, 10), entries,
                          values);
        return;
    }
    uint32_t rect[256];
    uint32_t w = eight ? 16 : 8, h = eight ? 16 : 2;
    for (uint32_t y = 0; y < h; y++) {
        gsn_vram_read_row(vram, gpsm, cbp, 1, 0, y, w, rect + y * w);
    }
    for (uint32_t i = 0; i < entries; i++) {
        values[i] = rect[eight ? ((i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)) : i];
    }
}

void clut_load(uint64_t tex0) {
    uint32_t psm = bits(tex0, 20, 6);
    uint32_t cbp = bits(tex0, 37, 14);
    uint32_t cpsm = bits(tex0, 51, 4);
    uint32_t csm = bits(tex0, 55, 1);
    uint32_t csa = bits(tex0, 56, 5);
    uint32_t cld = bits(tex0, 61, 3);

    if (!psm_is_palette(psm)) {
        return;
    }
    switch (cld) {
    case 0:
        return;
    case 1:
        break;
    case 2:
        gs->clut_cbp[0] = cbp;
        break;
    case 3:
        gs->clut_cbp[1] = cbp;
        break;
    case 4:
        if (gs->clut_cbp[0] == cbp) {
            return;
        }
        gs->clut_cbp[0] = cbp;
        break;
    case 5:
        if (gs->clut_cbp[1] == cbp) {
            return;
        }
        gs->clut_cbp[1] = cbp;
        break;
    default:
        return;
    }

    // the CLUT's pages may be a target's (drawn into): VRAM first
    uint32_t lo, count;
    buffer_pages(cbp, 1, cpsm == 0 ? GSN_PSMCT32 : GSN_PSMCT16, 16, 16, &lo, &count);
    claim_pages(lo, count, nullptr, false);
    // The same load as the last, its pages not written since and the buffer as it left it, loads the same
    // (a model's batches each write the TEX0 they share)
    struct ClutMemo {
        const void *owner = nullptr;
        uint64_t key = 0, texclut = 0, stamp = 0;
        uint16_t clut[512];
    };
    static ClutMemo memo;
    const uint64_t key_bits = (uint64_t(0x3F) << 20) | (~uint64_t(0) << 37 & ~(uint64_t(7) << 61));
    uint64_t key = tex0 & key_bits, texclut = csm ? gs->reg[R_TEXCLUT] : 0;
    uint64_t newest = 0;
    for (uint32_t k = 0; k < count && k < vram_pages; k++) {
        newest = std::max(newest, gs->page_written[(lo + k) % vram_pages]);
    }
    if (memo.owner == gs && memo.key == key && memo.texclut == texclut && newest <= memo.stamp &&
        memcmp(memo.clut, gs->clut, sizeof(memo.clut)) == 0) {
        return;
    }
    bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
    // An 8-bit CLUT of 32-bit colours loads the whole buffer, whatever it held: the last few such loads, their pages
    // not written since, load the same (a skinned model's parts load two in turn: the first pass's, the shine's)
    struct Whole {
        const void *owner = nullptr;
        uint64_t key = 0, texclut = 0, stamp = 0;
        uint16_t clut[512];
    };
    static Whole whole[4];
    static uint32_t whole_next = 0;
    bool full = eight && cpsm == 0;
    if (full) {
        for (Whole &m : whole) {
            if (m.owner == gs && m.key == key && m.texclut == texclut && newest <= m.stamp) {
                if (memcmp(m.clut, gs->clut, sizeof(m.clut)) != 0) {
                    memcpy(gs->clut, m.clut, sizeof(m.clut));
                    gs->clut_version++;
                }
                memo.owner = gs;
                memo.key = key;
                memo.texclut = texclut;
                memo.stamp = m.stamp;
                memcpy(memo.clut, gs->clut, sizeof(memo.clut));
                return;
            }
        }
    }

    uint16_t loaded[512];
    memcpy(loaded, gs->clut, sizeof(loaded));
    uint32_t entries = eight ? 256 : 16;
    uint32_t values[256];
    clut_read_entries(gs->vram.data(), tex0, gs->reg[R_TEXCLUT], values);
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t v = values[i];
        if (cpsm == 0) { // PSMCT32
            uint32_t at = eight ? i : (csa * 16 + i) & 0xFF;
            loaded[at] = uint16_t(v);
            loaded[256 + at] = uint16_t(v >> 16);
        } else { // PSMCT16, PSMCT16S
            uint32_t at = eight ? i : (csa * 16 + i) & 0x1FF;
            loaded[at] = uint16_t(v);
        }
    }
    if (memcmp(loaded, gs->clut, sizeof(loaded)) != 0) {
        memcpy(gs->clut, loaded, sizeof(loaded));
        gs->clut_version++;
    }
    memo.owner = gs;
    memo.key = key;
    memo.texclut = texclut;
    memo.stamp = gs->vram_writes;
    memcpy(memo.clut, gs->clut, sizeof(memo.clut));
    if (full) {
        Whole &m = whole[whole_next++ % 4];
        m.owner = gs;
        m.key = key;
        m.texclut = texclut;
        m.stamp = gs->vram_writes;
        memcpy(m.clut, gs->clut, sizeof(m.clut));
    }
}

// The 256 colours (or 16) a palette texture's indices look up, as RGBA, for the CLUT's format
void clut_colours(uint64_t tex0, uint32_t *out) {
    uint32_t psm = bits(tex0, 20, 6);
    uint32_t cpsm = bits(tex0, 51, 4);
    uint32_t csa = bits(tex0, 56, 5);
    bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
    uint32_t count = eight ? 256 : 16;
    uint64_t texa = gs->reg[R_TEXA];

    for (uint32_t i = 0; i < count; i++) {
        if (cpsm == 0) {
            uint32_t at = eight ? i : ((csa & 15) * 16 + i) & 0xFF;
            out[i] = uint32_t(gs->clut[at]) | (uint32_t(gs->clut[256 + at]) << 16);
        } else {
            uint32_t at = eight ? i : (csa * 16 + i) & 0x1FF;
            uint32_t v = gs->clut[at];
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
}

// ---- Drawing ------------------------------------------------------------------------------------
// GS blend factors and equations that Vulkan's blending does as the GS does (but for rounding)
struct FixedBlend {
    VkBlendFactor src, dst;
    VkBlendOp op;
    uint32_t premul; // 0; or the shader gives Cs * C >> 7 (F_PREMUL), rounded up for a subtraction (F_PREMUL_CEIL)
};

bool fixed_blend(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t fix, FixedBlend *out) {
    // A, B, D: 0 Cs, 1 Cd, 2 zero. C: 0 As, 1 Ad, 2 FIX
    enum { CS = 0, CD = 1, ZERO = 2 };
    VkBlendFactor f, one_minus_f;

    if (a == b || (a == 3 || b == 3 || d == 3)) {
        if (a == 3 || b == 3 || d == 3) {
            return false; // reserved
        }
        out->op = VK_BLEND_OP_ADD;
        out->src = d == CS ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        out->dst = d == CD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        return true;
    }
    // Cs * C added to Cd or taken from it (glows, fire, shadows): the shader works out Cs * C >> 7 as
    // the GS does, Vulkan adds it or takes it away, so the result is the GS's exactly, whatever C is
    if (c != 1 && a == CS && b == ZERO && (d == CD || d == ZERO)) {
        out->op = VK_BLEND_OP_ADD;
        out->src = VK_BLEND_FACTOR_ONE;
        out->dst = d == CD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        out->premul = F_PREMUL;
        return true;
    }
    if (c != 1 && a == ZERO && b == CS && (d == CD || d == ZERO)) {
        // (0 - Cs) C >> 7 is -(Cs C / 128) rounded down: Cs C / 128 rounded up, taken away
        out->op = VK_BLEND_OP_REVERSE_SUBTRACT;
        out->src = VK_BLEND_FACTOR_ONE;
        out->dst = d == CD ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        out->premul = F_PREMUL | F_PREMUL_CEIL;
        return true;
    }
    if (c == 0) {
        f = VK_BLEND_FACTOR_SRC_ALPHA;
        one_minus_f = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    } else if (c == 2 && fix <= 0x80) {
        f = VK_BLEND_FACTOR_CONSTANT_COLOR;
        one_minus_f = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    } else {
        return false; // Ad (Vulkan's is Ad / 255, the GS's Ad / 128), a FIX above 1.0
    }
    out->op = VK_BLEND_OP_ADD;
    const VkBlendFactor ONE = VK_BLEND_FACTOR_ONE, NONE = VK_BLEND_FACTOR_ZERO;
#define CASE(A, B, D, SRC, DST, OP)                                                                       \
    if (a == A && b == B && d == D) {                                                                      \
        out->src = SRC;                                                                                    \
        out->dst = DST;                                                                                    \
        out->op = OP;                                                                                      \
        return true;                                                                                       \
    }
    CASE(CS, CD, CD, f, one_minus_f, VK_BLEND_OP_ADD)
    CASE(CS, CD, ZERO, f, f, VK_BLEND_OP_SUBTRACT)
    CASE(CS, ZERO, CD, f, ONE, VK_BLEND_OP_ADD)
    CASE(CS, ZERO, ZERO, f, NONE, VK_BLEND_OP_ADD)
    CASE(CD, CS, CS, one_minus_f, f, VK_BLEND_OP_ADD)
    CASE(CD, CS, ZERO, f, f, VK_BLEND_OP_REVERSE_SUBTRACT)
    CASE(CD, ZERO, CS, ONE, f, VK_BLEND_OP_ADD)
    CASE(CD, ZERO, ZERO, NONE, f, VK_BLEND_OP_ADD)
    CASE(ZERO, CS, CD, f, ONE, VK_BLEND_OP_REVERSE_SUBTRACT)
    CASE(ZERO, CS, ZERO, f, NONE, VK_BLEND_OP_REVERSE_SUBTRACT)
    CASE(ZERO, CS, CS, one_minus_f, NONE, VK_BLEND_OP_ADD)
    CASE(ZERO, CD, CS, ONE, f, VK_BLEND_OP_SUBTRACT)
    CASE(ZERO, CD, ZERO, NONE, NONE, VK_BLEND_OP_ADD)
    CASE(ZERO, CD, CD, NONE, one_minus_f, VK_BLEND_OP_ADD)
#undef CASE
    return false; // (A - B) C + D with D one of A and B: a factor of 1 + C
}

// ---- Shader blending and the render pass --------------------------------------------------------
// A draw that blends in the shader reads the target's pixels from before it. They used to come from a
// copy of the target made outside the render pass (a transfer, or with --native-msaa a resolve), so
// every such draw ended the pass and began it again; a batch whose primitives overlap is drawn in runs,
// each with its own copy: the shadows' silhouettes (DATE) are some 240 runs a picture on map 000, and
// the passes' ends and beginnings, the copies and the barriers between were most of the GPU's time at
// 240 pictures a second (STEP3.md, "Render passes"). Now:
// - A single-sampled target's copy is kept in the render pass, as its second colour attachment (the
//   scratch: an image as large as the target, of its format, whose contents do not outlive the pass).
//   Before each run a draw over the run's rectangle copies the target's pixels into it
//   (pass_copy.frag, the target read as an input attachment), and the run reads the scratch as an
//   input attachment where it read the copy (the gs_*_in.frag shaders). Barriers inside the pass (the
//   subpass's dependency on itself) order the copy after the draws before it and the run after the
//   copy. The pixels are copied exactly, so the run sees what it saw before.
// - A multisampled target's copy is its resolve, and that cannot be made in the shader exactly as
//   vkCmdResolveImage makes it (NVIDIA's rounding of a mean of two samples depends on more than the two
//   values: the same pair resolves to either of its neighbours in different places). So the copy is
//   still a resolve, but the render pass makes it: the pass that draws a run resolves the target into
//   the copy the next run reads, over both runs' rectangles (its render area), into one of two copies
//   in turn (the run reads the one the pass before resolved). One pass and one barrier a run, instead
//   of a pass, a resolve and four barriers. (The two resolves gave the same pixels at 2, 4 and 8
//   samples of 8 bits; of 16 bits, --native-colour high, one pixel in a picture came out 1 apart, so
//   those are still resolved outside the pass.) (KF4_GSN_MSAA_BLEND=fast copies them in the pass as for a
//   single-sampled target instead, rounding the mean itself: msaa_blend_in_pass.)
// A draw whose texture is its own target reads anywhere in it, not only its own pixels: that one still
// copies the whole target outside the pass. KF4_GSN_BLEND_COPY=1 makes every copy outside the pass, as
// before (for comparisons).
bool blend_copy_forced() {
    static int forced = -1;
    if (forced < 0) {
        const char *v = getenv("KF4_GSN_BLEND_COPY");
        forced = v && atoi(v) > 0;
    }
    return forced != 0;
}

// KF4_GSN_CS_TARGET=0: a draw into a 32-bit target whose texture is a render target, blended
// (Cs - Cd) As + Cd with As 0x80 everywhere, blended by the shader as before (flush_batch, blend_is_cs)
bool cs_over_target() {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("KF4_GSN_CS_TARGET");
        on = !v || atoi(v) != 0;
    }
    return on != 0;
}

// KF4_GSN_DRAW_NEVER=1: a draw whose alpha test never passes and keeps both buffers is drawn (it writes
// nothing) as before, not left out (flush_batch)
bool draw_never() {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("KF4_GSN_DRAW_NEVER");
        on = v && atoi(v) > 0;
    }
    return on != 0;
}

// KF4_GSN_SILHOUETTE_RUNS=1: the silhouettes drawn in runs as before (flush_batch, "A silhouette"):
// with MSAA a pixel half covered by one run's triangle and half by the next's came out a little darker
// where the next read the first's resolve; drawn whole, it is darkened once, as the GS's pixel is.
int silhouette_runs_mode() {
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("KF4_GSN_SILHOUETTE_RUNS");
        mode = v && atoi(v) > 0;
    }
    return mode;
}

// The depth bounds test (flush_batch, "The first phase"): the GPU has it (Granite turns it on) and
// KF4_GSN_DEPTH_BOUNDS is not 0
bool depth_bounds_ok() {
    static int ok = -1;
    if (ok < 0) {
        const char *v = getenv("KF4_GSN_DEPTH_BOUNDS");
        ok = gs->device->get_device_features().enabled_features.depthBounds && !(v && atoi(v) == 0);
    }
    return ok != 0;
}

// The Z a triangle's pixels can have: its corners', and a GS pixel's change of Z more each way (a pixel
// shaded once, at its middle, outside the triangle where only some of its samples are inside)
void z_reach(const Vtx *v, double *lo, double *hi) {
    double z0 = v[0].z, z1 = v[1].z, z2 = v[2].z;
    double ax = double(v[1].x) - v[0].x, ay = double(v[1].y) - v[0].y;
    double bx = double(v[2].x) - v[0].x, by = double(v[2].y) - v[0].y;
    double area = ax * by - ay * bx;
    double slope = 0.0;
    if (area != 0.0) {
        double dzdx = ((z1 - z0) * by - (z2 - z0) * ay) / area;
        double dzdy = ((z2 - z0) * ax - (z1 - z0) * bx) / area;
        slope = fabs(dzdx) + fabs(dzdy);
    }
    *lo = std::min({z0, z1, z2}) - slope;
    *hi = std::max({z0, z1, z2}) + slope;
}

// KF4_GSN_BLEND16_BY_PASS=1: a multisampled 16-bit target's copies resolved by the passes, as before
// (flush_batch, "a 16-bit target's samples"), for comparisons
bool blend_copy_16_by_pass() {
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("KF4_GSN_BLEND16_BY_PASS");
        mode = v && atoi(v) > 0;
    }
    return mode != 0;
}

// KF4_GSN_MSAA_BLEND=fast: a multisampled target's copies made in the render pass too, its samples'
// mean rounded here (pass_resolve.frag), which is not always the resolve's: a few pixels at edges come
// out 1 or 2 apart from the default's (about a hundred a picture on map 000). A third less GPU time a
// picture again at --native-scale 4 with MSAA 2 (STEP3.md, "Render passes").
bool msaa_blend_in_pass() {
    static int fast = -1;
    if (fast < 0) {
        const char *v = getenv("KF4_GSN_MSAA_BLEND");
        fast = v && strcmp(v, "fast") == 0;
    }
    return fast != 0 && !blend_copy_forced();
}

const Image &blend_scratch(const Image &like) {
    for (auto &im : gs->scratch) {
        if (im->get_width() == like.get_width() && im->get_height() == like.get_height() &&
            im->get_format() == like.get_format() && im->get_create_info().samples == like.get_create_info().samples) {
            return *im;
        }
    }
    ImageCreateInfo info = ImageCreateInfo::render_target(like.get_width(), like.get_height(), like.get_format());
    info.samples = like.get_create_info().samples;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    info.layout = ImageLayout::General;
    info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
    gs->scratch.push_back(gs->device->create_image(info, nullptr));
    return *gs->scratch.back();
}

// The render pass's subpass that resolves as it ends: colour attachment 0 (the multisampled target) resolved into 1
// (a copy of one sample)
void resolve_subpass(RenderPassInfo &rp, RenderPassInfo::Subpass &subpass) {
    subpass = {};
    subpass.num_color_attachments = 1;
    subpass.color_attachments[0] = 0;
    subpass.num_resolve_attachments = 1;
    subpass.resolve_attachments[0] = 1;
    subpass.depth_stencil_mode = rp.depth_stencil ? RenderPassInfo::DepthStencil::ReadWrite : RenderPassInfo::DepthStencil::None;
    rp.subpasses = &subpass;
    rp.num_subpasses = 1;
}

// The render pass's subpass with the scratch: colour attachments 0 (the target) and 1 (the scratch),
// both input attachments too. A multisampled pass lists its inputs the other way round, the scratch
// first: Granite's cache of render passes does not tell passes apart by their sample counts (msaa_subpass)
void scratch_subpass(RenderPassInfo &rp, RenderPassInfo::Subpass &subpass, bool ms) {
    subpass = {};
    subpass.num_color_attachments = 2;
    subpass.color_attachments[0] = 0;
    subpass.color_attachments[1] = 1;
    subpass.num_input_attachments = 2;
    subpass.input_attachments[0] = ms ? 1 : 0;
    subpass.input_attachments[1] = ms ? 0 : 1;
    subpass.depth_stencil_mode = rp.depth_stencil ? RenderPassInfo::DepthStencil::ReadWrite : RenderPassInfo::DepthStencil::None;
    rp.subpasses = &subpass;
    rp.num_subpasses = 1;
}

// A barrier inside the render pass: the colour attachments' writes so far, before input attachment
// reads after it (and those reads' stage after the writes: the scratch is not written while a run reads it)
void in_pass_barrier() {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
    cmd().memory_barrier(b, VK_DEPENDENCY_BY_REGION_BIT);
}

// The render pass into a colour target and its Z buffer (or none), made current unless it is already.
// ms: into the targets' multisampled images (--native-msaa). scratch: with the copy shader blending
// reads (above); a pass with it serves the draws without it too. area: the pass covers that part of the
// targets only, and resolve_to (with area) has the multisampled colour resolved into it at the pass's
// end (above); such a pass serves only the draws begun with it.
void begin_pass(Target *color, Target *depth, bool ms = false, bool scratch = false, const VkRect2D *area = nullptr,
                const Image *resolve_to = nullptr) {
    if (gs->in_pass && gs->pass_color == color && gs->pass_depth == depth && gs->pass_ms == ms &&
        (gs->pass_scratch || !scratch) && !area && !gs->pass_area) {
        return;
    }
    {
        Why why(gs->in_pass && gs->pass_color == color && gs->pass_depth == depth
                    ? (gs->pass_ms != ms ? GSN_WHY_MSAA : GSN_WHY_BLEND)
                    : GSN_WHY_TARGET);
        end_pass();
    }
    gpu_mark(GPU_OTHER);
    {
        const Image &ci = ms ? *color->ms : *color->image;
        uint64_t bytes = uint64_t(ci.get_width()) * ci.get_height() * (wide(ci) ? 8 : 4) * ci.get_create_info().samples;
        if (depth) {
            const Image &di = ms ? *depth->ms : *depth->image;
            bytes += uint64_t(di.get_width()) * di.get_height() * 4 * di.get_create_info().samples;
        }
        if (area) {
            bytes = bytes / (uint64_t(ci.get_width()) * ci.get_height()) * area->extent.width * area->extent.height;
        }
        gsn_stats.pass_load_bytes += bytes;
        gsn_stats.pass_store_bytes += bytes;
    }
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = ms ? &color->ms->get_view() : &color->image->get_view();
    rp.load_attachments = 1;
    rp.store_attachments = 1;
    if (depth) {
        rp.depth_stencil = ms ? &depth->ms->get_view() : &depth->image->get_view();
        rp.op_flags = RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT | RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
    }
    RenderPassInfo::Subpass subpass;
    if (resolve_to) {
        // (the copy is resolved over the render area; what it held there before is not wanted)
        rp.num_color_attachments = 2;
        rp.color_attachments[1] = &resolve_to->get_view();
        rp.store_attachments = 3;
        resolve_subpass(rp, subpass);
    } else if (scratch) {
        // (the scratch's contents are not wanted before the pass nor after it)
        rp.num_color_attachments = 2;
        rp.color_attachments[1] = &blend_scratch(ms ? *color->ms : *color->image).get_view();
        scratch_subpass(rp, subpass, ms);
    } else if (ms) {
        msaa_subpass(rp, subpass);
    }
    if (area) {
        rp.render_area = *area;
    }
    full_barrier();
    cmd().begin_render_pass(rp);
    gs->in_pass = true;
    gs->pass_color = color;
    gs->pass_depth = depth;
    gs->pass_ms = ms;
    gs->pass_scratch = scratch;
    gs->pass_area = area != nullptr;
    gsn_stats.passes++;
}

// The target's pixels in a rectangle (box: x0 y0 x1 y1 in GS pixels) copied into the scratch of the
// render pass under way (begun with it), for the next run of a draw that blends in the shader
void blend_copy_in_pass(Target &t, bool ms, const int32_t *box) {
    const Image &image = ms ? *t.ms : *t.image;
    int32_t sx = int32_t(t.sx), sy = int32_t(t.sy);
    int32_t x0 = std::max(box[0], 0) * sx, y0 = std::max(box[1], 0) * sy;
    int32_t x1 = std::min(box[2] * sx, int32_t(image.get_width())), y1 = std::min(box[3] * sy, int32_t(image.get_height()));
    if (x1 <= x0 || y1 <= y0) {
        return; // (nothing of the run on the target)
    }
    Cmd &c = cmd();
    in_pass_barrier();
    c.set_opaque_state();
    c.set_program(ms ? gs->program_pass_resolve : gs->program_pass_copy);
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_test(false, false);
    c.set_blend_enable(false);
    c.set_color_write_mask(0xF0); // (the scratch only)
    c.set_viewport({0.0f, 0.0f, float(image.get_width()), float(image.get_height()), 0.0f, 1.0f});
    c.set_scissor({{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}});
    c.set_input_attachments(0, 4);
    if (ms) {
        struct {
            int32_t samples;
            float scale;
        } push = {int32_t(image.get_create_info().samples), wide(image) ? 65535.0f : 255.0f};
        c.push_constants(&push, 0, sizeof(push));
    }
    c.draw(3);
    in_pass_barrier();
    gsn_stats.blend_copies_in_pass++;
}

void interp_box_at_0(uint32_t first, uint32_t count, int32_t *box);
bool nd_box_at_0(uint32_t first, uint32_t count, int32_t *box); // (native draw: gsn_native_draw.inc)
int nd_z_at_0(double *lo, double *hi);                          // (native draw: gsn_native_draw.inc)

// A run's rectangle: the GS pixels its primitives can cover, a pixel more each way (x0 y0 x1 y1)
void run_box(uint32_t first, uint32_t count, int32_t *box) {
    box[0] = box[1] = INT32_MAX;
    box[2] = box[3] = INT32_MIN;
    for (uint32_t i = first; i < first + count; i++) {
        const Vtx &v = gs->verts[i];
        box[0] = std::min(box[0], int32_t(floorf(v.x)) - 1);
        box[1] = std::min(box[1], int32_t(floorf(v.y)) - 1);
        box[2] = std::max(box[2], int32_t(ceilf(v.x)) + 2);
        box[3] = std::max(box[3], int32_t(ceilf(v.y)) + 2);
    }
    if (interp.active) {
        // (a picture between's moving vertices: where they are at t 0 too; native draw's, gsn_native_draw.inc)
        if (!nd_box_at_0(first, count, box)) {
            interp_box_at_0(first, count, box);
        }
    }
}

// The same in the target's own pixels, on its image (an empty rectangle when it is off the image)
VkRect2D box_area(const Target &t, const Image &image, const int32_t *box) {
    int32_t x0 = std::max(box[0], 0) * int32_t(t.sx), y0 = std::max(box[1], 0) * int32_t(t.sy);
    int32_t x1 = std::min(box[2] * int32_t(t.sx), int32_t(image.get_width()));
    int32_t y1 = std::min(box[3] * int32_t(t.sy), int32_t(image.get_height()));
    if (x1 <= x0 || y1 <= y0) {
        return {{0, 0}, {0, 0}};
    }
    return {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
}

VkRect2D rect_union(const VkRect2D &a, const VkRect2D &b) {
    if (!a.extent.width) {
        return b;
    }
    if (!b.extent.width) {
        return a;
    }
    int32_t x0 = std::min(a.offset.x, b.offset.x), y0 = std::min(a.offset.y, b.offset.y);
    int32_t x1 = std::max(a.offset.x + int32_t(a.extent.width), b.offset.x + int32_t(b.extent.width));
    int32_t y1 = std::max(a.offset.y + int32_t(a.extent.height), b.offset.y + int32_t(b.extent.height));
    return {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
}

// The two copies a multisampled target's runs read in turn (resolved by the render passes), as large as
// its image
const Image &resolved_copy(int which, const Image &like) {
    ImageHandle &im = gs->resolved[which];
    if (!im || im->get_width() != like.get_width() || im->get_height() != like.get_height() ||
        im->get_format() != like.get_format()) {
        im = make_image(like.get_width(), like.get_height(), like.get_format(), 1, true);
    }
    return *im;
}

// A copy of the target, for a draw that reads what it draws over
// (box: only the target's pixels a draw can cover, x0 y0 x1 y1 in GS pixels; NULL: all of it)
const Image &snapshot_of(Target &t, const int32_t *box = nullptr) {
    Why why(GSN_WHY_BLEND);
    uint32_t tw = t.width * t.sx, th = t.height * t.sy;
    if (!gs->snapshot || gs->snapshot->get_format() != t.image->get_format()) {
        gs->snapshot.reset();
    }
    if (!gs->snapshot || gs->snapshot->get_width() < tw || gs->snapshot->get_height() < th) {
        uint32_t w = std::max(tw, gs->snapshot ? gs->snapshot->get_width() : 0u);
        uint32_t h = std::max(th, gs->snapshot ? gs->snapshot->get_height() : 0u);
        gs->snapshot = make_image(w, h, t.image->get_format(), 1, false);
    }
    VkOffset3D at = {0, 0, 0};
    VkExtent3D size = {tw, th, 1};
    if (box) {
        int32_t sx = int32_t(t.sx), sy = int32_t(t.sy);
        int32_t x0 = std::max(box[0], 0) * sx, y0 = std::max(box[1], 0) * sy;
        int32_t x1 = std::min(box[2] * sx, int32_t(tw)), y1 = std::min(box[3] * sy, int32_t(th));
        if (x1 <= x0 || y1 <= y0) {
            return *gs->snapshot; // (nothing of it on the target: no copy, and the render pass goes on)
        }
        at = {x0, y0, 0};
        size = {uint32_t(x1 - x0), uint32_t(y1 - y0), 1};
    }
    if (t.ms_newer) {
        // --native-msaa: the target's resolved pixels
        msaa_resolve(*gs->snapshot, *t.ms, at, size);
        return *gs->snapshot;
    }
    end_pass();
    gpu_mark(GPU_OTHER);
    transfer_barrier();
    cmd().copy_image(*gs->snapshot, *t.image, at, at, size, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    gpu_mark(GPU_COPY);
    transfer_barrier();
    gsn_stats.target_copies++;
    gsn_stats.copy_bytes += uint64_t(size.width) * size.height * (wide(*t.image) ? 16 : 8);
    return *gs->snapshot;
}

// Whether two triangles' insides overlap, so that a pixel could be drawn by both: no edge of either
// has the other wholly on its far side (the separating axis test). Triangles that share an edge, or
// only touch, do not; nor does one with no area, which draws nothing. (In doubles: exact for 12.4.)
bool triangles_overlap(const Vtx *a, const Vtx *b) {
    float ax0 = std::min(a[0].x, std::min(a[1].x, a[2].x)), ax1 = std::max(a[0].x, std::max(a[1].x, a[2].x));
    float bx0 = std::min(b[0].x, std::min(b[1].x, b[2].x)), bx1 = std::max(b[0].x, std::max(b[1].x, b[2].x));
    float ay0 = std::min(a[0].y, std::min(a[1].y, a[2].y)), ay1 = std::max(a[0].y, std::max(a[1].y, a[2].y));
    float by0 = std::min(b[0].y, std::min(b[1].y, b[2].y)), by1 = std::max(b[0].y, std::max(b[1].y, b[2].y));
    if (ax1 <= bx0 || bx1 <= ax0 || ay1 <= by0 || by1 <= ay0) {
        return false;
    }
    for (int k = 0; k < 2; k++) {
        const Vtx *p = k ? b : a, *q = k ? a : b;
        double area = (double(p[1].x) - p[0].x) * (double(p[2].y) - p[0].y) -
                      (double(p[1].y) - p[0].y) * (double(p[2].x) - p[0].x);
        if (area == 0) {
            return false;
        }
        for (int i = 0; i < 3; i++) {
            const Vtx &e0 = p[i], &e1 = p[(i + 1) % 3];
            bool apart = true;
            for (int j = 0; j < 3 && apart; j++) {
                double side = (double(e1.x) - e0.x) * (double(q[j].y) - e0.y) - (double(e1.y) - e0.y) * (double(q[j].x) - e0.x);
                apart = (area > 0 ? side : -side) <= 0;
            }
            if (apart) {
                return false;
            }
        }
    }
    return true;
}

uint32_t effective_prim() {
    uint32_t prim = uint32_t(gs->reg[R_PRIM]) & 0x7FF;
    if (!(gs->reg[R_PRMODECONT] & 1)) {
        prim = (prim & 7) | (uint32_t(gs->reg[R_PRMODE]) & 0x7F8);
    }
    return prim;
}

// ---- Pictures between on the GPU: a batch of the frame's packets drawn for them ----------------------
// (gsn_interp_picture, below.) The batch's vertices are where the game drew them (t 1); each picture
// between draws them where they are at its time, in gs_replay.vert. What flush_batch decides from the
// vertices is decided here for every time a picture can have: a batch with vertices that move is
// neither cut to the scissor nor copied in part for its blending, its rows and the alphas it can blend
// by are those of either end, and its Z is not taken to stay at the format's largest.
bool interp_moving(uint32_t k, bool only_3d) {
    if (k >= interp.frame->kicks) {
        return false;
    }
    uint32_t w = interp.frame->moves[k].w[0];
    if ((w & 3) == GSN_IM_3D) {
        return (w & GSN_IM_VIEW) && (w & (GSN_IM_WORLD | GSN_IM_OBJ));
    }
    return !only_3d && (w & 3) == GSN_IM_FLAT && (w & GSN_IM_XY);
}

// The kick whose colour a corner has (the third's of a flat triangle; the second's of a sprite)
uint32_t interp_colour_kick(const std::array<uint32_t, 4> &c) {
    if (c[3] & IC_SPRITE) {
        return c[1];
    }
    return (c[3] & IC_FLAT_SHADE) ? c[2] : c[c[3] & 3];
}

// The kick whose T a corner has (its own: the sprites' corners 0 and 1 a's, 2 and 3 b's)
uint32_t interp_t_kick(const std::array<uint32_t, 4> &c) {
    if (c[3] & IC_SPRITE) {
        return (c[3] & 3) < 2 ? c[0] : c[1];
    }
    return c[c[3] & 3];
}

struct InterpBatch {
    bool moves = false, moves_3d = false;
};

InterpBatch interp_batch() {
    InterpBatch b;
    for (const auto &c : interp.vsrc) {
        int n = (c[3] & IC_SPRITE) ? 2 : 3;
        for (int i = 0; i < n; i++) {
            b.moves_3d = b.moves_3d || interp_moving(c[i], true);
            b.moves = b.moves || interp_moving(c[i], false);
        }
        if (b.moves_3d) {
            break;
        }
    }
    return b;
}

// The alphas the batch's colours can have at the pictures' times (the 2D's paired fades, the previous
// frame blended in by A^t)
void interp_alpha_range(uint32_t &lo, uint32_t &hi) {
    for (const auto &c : interp.vsrc) {
        uint32_t k = interp_colour_kick(c);
        if (k >= interp.frame->kicks) {
            continue;
        }
        k = interp.kick_list[k].rgba_owner;
        if (k >= interp.frame->kicks) {
            continue;
        }
        const GsnInterpMove &m = interp.frame->moves[k];
        if ((m.w[0] & 3) != GSN_IM_FLAT) {
            continue;
        }
        if (m.w[0] & GSN_IM_PREV) {
            uint32_t a = std::min(m.w[1] >> 24, m.w[2] >> 24);
            lo = std::min(lo, std::min(a, 128u));
            hi = std::max(hi, 128u);
        } else if (m.w[0] & GSN_IM_RGBA) {
            lo = std::min(lo, std::min(m.w[1] >> 24, m.w[2] >> 24));
            hi = std::max(hi, std::max(m.w[1] >> 24, m.w[2] >> 24));
        }
    }
}

// A corner's texture T at t 0 (a paired 2D item's, where it was in the frame before), as flush_batch's
// rows of a texture read from a target take it: false when it does not move
bool interp_t_at_0(const std::array<uint32_t, 4> &c, bool fst, float rows, float q, float *tv) {
    uint32_t k = interp_t_kick(c);
    if (k >= interp.frame->kicks) {
        return false;
    }
    k = fst ? interp.kick_list[k].uv_owner : interp.kick_list[k].st_owner;
    if (k >= interp.frame->kicks) {
        return false;
    }
    const GsnInterpMove &m = interp.frame->moves[k];
    if ((m.w[0] & 3) != GSN_IM_FLAT || !(m.w[0] & (fst ? GSN_IM_UV : GSN_IM_ST))) {
        return false;
    }
    float t, add;
    memcpy(&t, &m.w[6], 4);
    memcpy(&add, &m.w[8], 4);
    *tv = fst ? t + add : q > 0.0f ? (t + add) / q * rows : -1.0f;
    return true;
}

// A kick's place at t 0 (gs_replay.comp's moved at the frame's t 0 cameras, frame->move0 and mid0), in GS
// pixels less its XYOFFSET: with its model and with the camera when it may be either. Adds to box (x0 y0 x1
// y1); how far it is from where it is at t 1 (x, y) to most.
void interp_place_at_0(uint32_t k, float x, float y, int32_t *box, float *most, float z1 = 0.0f, float *zlo = nullptr,
                       float *zhi = nullptr) {
    if (k >= interp.frame->kicks) {
        return;
    }
    const GsnInterpMove &m = interp.frame->moves[k];
    const InterpKick &kick = interp.kick_list[k];
    float ox = float(kick.offset & 0xFFFF), oy = float(kick.offset >> 16);
    float places[2][2];
    int n = 0;
    auto f = [&](int i) {
        float v;
        memcpy(&v, &m.w[i], 4);
        return v;
    };
    if ((m.w[0] & 3) == GSN_IM_FLAT && (m.w[0] & GSN_IM_XY)) {
        places[n][0] = (float(m.w[3] & 0xFFFF) - ox) / 16.0f;
        places[n][1] = (float(m.w[3] >> 16) - oy) / 16.0f;
        n++;
    } else if ((m.w[0] & 3) == GSN_IM_3D && (m.w[0] & GSN_IM_VIEW)) {
        const float *screen = interp.frame->screen[(m.w[0] & GSN_IM_REFLECT) ? 1 : 0];
        for (int obj = 0; obj < 2; obj++) {
            // (gs_replay.comp's moved at t 0: v + dm)
            float p[3], v[3], e[3];
            if (obj ? !(m.w[0] & GSN_IM_OBJ) : !(m.w[0] & GSN_IM_WORLD)) {
                continue;
            }
            const float *r = interp.frame->move0, *mid = interp.frame->mid0;
            for (int i = 0; i < 3; i++) {
                p[i] = f(4 + i);
                e[i] = f(8 + i);
            }
            for (int i = 0; i < 3; i++) {
                if (obj && (m.w[0] & GSN_IM_EYE)) {
                    v[i] = p[i] + e[i];
                } else {
                    v[i] = p[i] + r[i * 4] * p[0] + r[i * 4 + 1] * p[1] + r[i * 4 + 2] * p[2] + r[i * 4 + 3];
                    if (obj) {
                        v[i] += mid[i * 4] * e[0] + mid[i * 4 + 1] * e[1] + mid[i * 4 + 2] * e[2];
                    }
                }
            }
            if (!(v[2] > 0.0f)) {
                continue;
            }
            if (zlo) {
                // (gs_replay.comp's Z: the kick's less (Z - cz) dm.z / m.z)
                float nz = z1 - f(14) * (v[2] - p[2]) / v[2];
                *zlo = std::min(*zlo, nz);
                *zhi = std::max(*zhi, nz);
            }
            float nx = screen[0] * v[0] / v[2] + screen[2], ny = screen[1] * v[1] / v[2] + screen[3];
            if (!(fabsf(nx - 2048.0f) <= 2047.0f && fabsf(ny - 2048.0f) <= 2047.0f)) {
                continue;
            }
            places[n][0] = nx - ox / 16.0f;
            places[n][1] = ny - oy / 16.0f;
            n++;
        }
    }
    for (int i = 0; i < n; i++) {
        box[0] = std::min(box[0], int32_t(floorf(places[i][0])) - 1);
        box[1] = std::min(box[1], int32_t(floorf(places[i][1])) - 1);
        box[2] = std::max(box[2], int32_t(ceilf(places[i][0])) + 2);
        box[3] = std::max(box[3], int32_t(ceilf(places[i][1])) + 2);
        *most = std::max(*most, std::max(fabsf(places[i][0] - x), fabsf(places[i][1] - y)));
    }
}

// A run of a batch with vertices that move: the pixels its blending reads at any time between the frame
// shown and this one. Those its triangles cover at t 1 (box), and at t 0; the camera turns on the way,
// so the corners are not on the line between: a margin of a quarter of the furthest a corner went, and
// four pixels.
void interp_box_at_0(uint32_t first, uint32_t count, int32_t *box) {
    float most = 0.0f;
    bool moved = false;
    for (uint32_t i = first; i < first + count && i < interp.vsrc.size(); i++) {
        const auto &c = interp.vsrc[i];
        const Vtx &v = gs->verts[i];
        if (c[3] & IC_SPRITE) {
            interp_place_at_0(c[0], v.x, v.y, box, &most);
            interp_place_at_0(c[1], v.x, v.y, box, &most);
            moved = moved || interp_moving(c[0], false) || interp_moving(c[1], false);
        } else {
            interp_place_at_0(c[c[3] & 3], v.x, v.y, box, &most);
            moved = moved || interp_moving(c[c[3] & 3], false);
        }
    }
    if (!moved) {
        return; // (nothing of it moves)
    }
    int32_t margin = 4 + int32_t(ceilf(most * 0.25f));
    box[0] -= margin;
    box[1] -= margin;
    box[2] += margin;
    box[3] += margin;
}

// A batch's Z at t 0 too (a picture between's moving vertices), as interp_box_at_0 its places: lo and hi
// widened to take them in, and a quarter of the furthest a Z went more each way (the camera's path is not a
// line between its ends)
void interp_z_at_0(double *lo, double *hi) {
    float zl = INFINITY, zh = -INFINITY;
    int32_t box[4] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    float most = 0.0f;
    for (size_t i = 0; i < interp.vsrc.size() && i < gs->verts.size(); i++) {
        const auto &c = interp.vsrc[i];
        const Vtx &v = gs->verts[i];
        if (!(c[3] & IC_SPRITE)) {
            interp_place_at_0(c[c[3] & 3], v.x, v.y, box, &most, float(v.z), &zl, &zh);
        }
    }
    if (zl > zh) {
        return;
    }
    double far = std::max({0.0, *lo - double(zl), double(zh) - *hi});
    *lo = std::min(*lo, double(zl)) - far * 0.25;
    *hi = std::max(*hi, double(zh)) + far * 0.25;
}

// The batch, drawn
// The batch is textured in perspective (ST, a Q not 1 at some vertex): the 3D, never a 2D rectangle,
// whatever its Z and colour. flat_2d takes a triangle of one Z and colour whose corners are those of
// its bounding box for half of a 2D rectangle; a billboard of program 0x700 (a brazier's glow) far
// enough away that its Z rounds the same at its corners, and upright on the screen, is one too, and
// moving each half's corners out to whole GS pixels made the halves overlap along their diagonal: a
// bright line across the glow at --native-scale above 1, on and off with the distance.
bool perspective_batch(const std::vector<Vtx> &verts) {
    uint32_t prim = gs->batch_prim;
    if (!((prim >> 4) & 1) || ((prim >> 8) & 1)) {
        return false;
    }
    for (const Vtx &v : verts) {
        if (v.q != 1.0f) {
            return true;
        }
    }
    return false;
}

#include "gsn_native_draw.inc"

void flush_batch() {
    Timer timer(gsn_stats.ns_flush);
    settle_upload();
    if (gs->verts.empty()) {
        return;
    }
    NdFlush nd_flush; // (native draw: whether the batch is native corners, gsn_native_draw.inc)
    // (a picture between drawn from the frame's packets: interp.vsrc is gs->verts as corners)
    struct ForgetCorners {
        ~ForgetCorners() {
            interp.vsrc.clear();
        }
    } forget_corners;
    InterpBatch moving;
    if (interp.active) {
        if (interp.vsrc.size() != gs->verts.size()) {
            interp.broken = true; // (never: every triangle added is added to both)
            interp.vsrc.resize(gs->verts.size(), {GSN_IM_NONE, GSN_IM_NONE, GSN_IM_NONE, 0});
        }
        moving = interp_batch();
    }
    if (nd.active && interp.active && !nd.pass_active) {
        moving.moves = moving.moves_3d = true; // (native draw: moved with the camera by nd_000.vert)
    }
    if (sa_flush()) {
        return; // (standalone: the scene's batch, drawn into the scene's targets: gsn_standalone.inc)
    }
    gpu_enter(GPU_GS_BATCH);
    gs->tick++;
    // what the decisions below need of the vertices, in one pass over them: their Z's range, the
    // largest alpha, the rows they reach
    uint32_t zmin = UINT32_MAX, zmax = 0, as_max = 0, as_min = 255;
    float vy0 = gs->verts[0].y, vy1 = vy0;
    for (const Vtx &v : gs->verts) {
        zmin = std::min(zmin, v.z);
        zmax = std::max(zmax, v.z);
        as_max = std::max(as_max, v.rgba >> 24);
        as_min = std::min(as_min, v.rgba >> 24);
        vy0 = std::min(vy0, v.y);
        vy1 = std::max(vy1, v.y);
    }
    if (interp.active) {
        interp_alpha_range(as_min, as_max);
    }
    uint32_t prim = gs->batch_prim;
    uint32_t ctxt = (prim >> 9) & 1;
    bool tme = (prim >> 4) & 1, fge = (prim >> 5) & 1, abe = (prim >> 6) & 1, fst = (prim >> 8) & 1;
    uint64_t frame = gs->reg[R_FRAME_1 + ctxt];
    uint64_t zbuf = gs->reg[R_ZBUF_1 + ctxt];
    uint64_t test = gs->reg[R_TEST_1 + ctxt];
    uint64_t alpha = gs->reg[R_ALPHA_1 + ctxt];
    uint64_t scissor = gs->reg[R_SCISSOR_1 + ctxt];
    uint64_t tex0 = gs->reg[R_TEX0_1 + ctxt];
    uint64_t tex1 = gs->reg[R_TEX1_1 + ctxt];
    uint64_t clamp = gs->reg[R_CLAMP_1 + ctxt];

    uint32_t fbp = bits(frame, 0, 9) * 32, fbw = bits(frame, 16, 6), fpsm = bits(frame, 24, 6);
    uint32_t fbmsk = uint32_t(frame >> 32);
    uint32_t zbp = bits(zbuf, 0, 9) * 32, zpsm = bits(zbuf, 24, 4) | 0x30;
    bool zmsk = bits(zbuf, 32, 1);
    uint32_t sx0 = bits(scissor, 0, 11), sx1 = bits(scissor, 16, 11);
    uint32_t sy0 = bits(scissor, 32, 11), sy1 = bits(scissor, 48, 11);
    bool ate = bits(test, 0, 1);
    uint32_t atst = bits(test, 1, 3), afail = bits(test, 12, 2);
    bool date = bits(test, 14, 1);
    uint32_t ztst = bits(test, 16, 1) ? bits(test, 17, 2) : 1;
    if (!bits(test, 16, 1)) {
        // ZTE 0 (undefined on the GS): the Z buffer is neither tested nor written, as paraLLEl-GS has
        // it. The movies draw each field with ZTE 0 and ZBUF at 0x1180, the movie's own picture:
        // writing Z there wrote zeros over the frame being shown (black and cyan bands at the top)
        zmsk = true;
    }

    if (sx1 < sx0 || sy1 < sy0 || (fpsm != GSN_PSMCT32 && fpsm != GSN_PSMCT24 && fpsm != GSN_PSMCT16 &&
                                   fpsm != GSN_PSMCT16S)) {
        gs->verts.clear();
        return;
    }
    if (fpsm == GSN_PSMCT24) {
        fbmsk |= 0xFF000000u;
    }
    bool dst16 = fpsm == GSN_PSMCT16 || fpsm == GSN_PSMCT16S;
    if (dst16) {
        // a 16-bit buffer keeps its colours' top five bits and alpha's top bit: what FBMSK masks of those
        fbmsk = (fbmsk & 0x80F8F8F8u) | 0x7F070707u;
    }

    // A batch wholly outside its scissor draws nothing: no target is made for it (the reflection's map strips
    // the scene does not take, below the 256 x 256 view: made from VRAM after the MP drain's or the water's copy
    // of the frame over 0x2BC0, that copy went back from the GPU first, a 640 x 448 read-back a frame)
    if (!moving.moves && (vy1 < float(sy0) - 1.0f || vy0 > float(sy1) + 1.0f)) {
        gs->verts.clear();
        return;
    }
    // ZTST NEVER draws nothing; ALWAYS without writing needs no Z buffer
    if (ztst == 0) {
        gs->verts.clear();
        return;
    }
    // Nor does an alpha test that never passes and keeps both buffers where it fails (ATST NEVER, AFAIL
    // KEEP): the Dark Succubus' body (map 038, Entity_PrepareScreenTexturedDraw) sets TEST_2 0x30001, so
    // the second pass of each of its parts writes nothing. Drawn, each was a render pass of its own:
    // into the frame without the Z buffer (ZTST ALWAYS, nothing written), between two of its parts'
    // first passes with it, and each such pass loads and stores the whole multisampled frame and Z
    // buffer (NATIVE_DRAW_P8.md, "The Dark Succubus' body").
    // KF4_GSN_DRAW_NEVER=1 draws them as before.
    if (ate && atst == 0 && afail == 0 && !draw_never()) {
        gs->verts.clear();
        return;
    }
    // A draw whose Z test always passes and that writes no Z needs no Z buffer: ZTST ALWAYS, or GEQUAL
    // with every vertex at the format's largest Z (the scrolling textures each frame, drawn with the
    // main Z buffer at their own width, which would otherwise make it a target of another shape)
    bool any_zwrite = !zmsk && (!ate || atst != 0 || afail == 2);
    bool always_passes = ztst == 1;
    if (ztst == 2) {
        always_passes = std::min(zmin, depth_max(zpsm)) == depth_max(zpsm) && !moving.moves_3d;
    }
    bool need_depth = !(always_passes && !any_zwrite);
    // The rows the targets need: to the scissor's bottom; for a draw with Z, only to the last row its
    // vertices reach when that is higher. The menus' scissor runs a row past their 224-line field
    // buffers (0-224, inclusive), and targets made 225 rows tall from it reached each other (the second
    // field buffer's last row is in the Z buffer's first page): as the two attachments are made as big
    // as each other, they could not be targets together, and the field's clear and backdrop were drawn
    // without Z, which the brightness sprite of the field before had left at its largest: the
    // backdrop failed the Z test, and every other line of the Equipment and Journal screens was black.
    // Those draws reach row 223 (a sprite to 223.5 covers row 223 last), as on the GS.
    uint32_t need_h = sy1 + 1;
    if (need_depth && !moving.moves_3d) {
        need_h = std::min(need_h, uint32_t(std::max(0.0f, floorf(vy1))) + 1);
    }
    Target *color = get_target(fbp, fbw, fpsm, fbw * 64, need_h);
    Target *depth = nullptr;
    if (need_depth && color->height > need_h) {
        // a colour target taller than the draw needs (one drawn over both frame buffers once, say) that
        // runs into the Z buffer: back to VRAM, and made again as tall as the draw needs
        uint32_t clo, ccount, zlo, zcount;
        target_pages(*color, &clo, &ccount);
        buffer_pages(zbp, fbw, zpsm, fbw * 64, need_h, &zlo, &zcount);
        if (pages_overlap(clo, ccount, zlo, zcount)) {
            write_back(*color);
            drop_target(color);
            color = get_target(fbp, fbw, fpsm, fbw * 64, need_h);
        }
    }
    if (need_depth) {
        // a Vulkan frame buffer's attachments are as big as each other. Making one may drop the other
        // (when their pages overlap, as the menus' Z buffer does the play's frame buffers), so both
        // are looked up again until they hold still.
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
        if (find_target(zbp, fbw, zpsm) != depth || find_target(fbp, fbw, fpsm) != color) {
            static int told;
            if (!told++) {
                fprintf(stderr, "[gsn] FRAME %#x and ZBUF %#x overlap: drawn without Z\n", fbp, zbp);
            }
            color = get_target(fbp, fbw, fpsm, fbw * 64, need_h);
            depth = nullptr;
        }
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
                (dst16 ? M_DST16 : 0) | (fpsm == GSN_PSMCT24 ? M_DST24 : 0) |
                (bits(gs->reg[R_PABE], 0, 1) ? M_PABE : 0) | (bits(gs->reg[R_SCANMSK], 0, 2) << M_SCANMSK_SHIFT);
    if (wide(*color->image) && !dst16) {
        push.misc |= M_HIGH; // --native-colour high: the colours' arithmetic in floats
    }
    push.flags = (fge ? F_FGE : 0);

    // the texture
    const ImageView *raw_view = &gs->dummy_raw->get_view();
    const ImageView *target_view = &gs->dummy_rgba->get_view();
    bool reads_own_target = false;
    bool reads_target = false; // a feedback pass: the texture is a render target
    uint32_t clut[256];
    bool want_clut = false;
    uint32_t tex_alpha_max = 255; // the largest alpha a texel can have, where it is known
    // --native-cutout-aa: the alphas the texture's texels have, from the least to the most (only of a
    // texture decoded from VRAM; cut_texels false where they are not known)
    bool cut_texels = false;
    uint32_t cut_lo = 255, cut_hi = 0;
    // the least and most alpha its texels have (a CLUT or 32-bit texture decoded from VRAM; else 0-255)
    uint32_t tex_lo = 0, tex_hi = 255;
    uint64_t cut_clear = 0, cut_all = 0;
    if (tme) {
        uint32_t tbp = bits(tex0, 0, 14), tbw = bits(tex0, 14, 6), tpsm = bits(tex0, 20, 6);
        uint32_t tw = std::min(bits(tex0, 26, 4), 10u), th = std::min(bits(tex0, 30, 4), 10u);
        push.flags |= F_TME | (fst ? F_FST : 0) | (bits(tex0, 35, 2) << F_TFX_SHIFT) | (bits(tex0, 34, 1) ? F_TCC : 0);
        push.tex_size = (1u << tw) | ((1u << th) << 16);
        push.clamp_lo = uint32_t(clamp & 0xFFFFFF);
        push.clamp_hi = bits(clamp, 24, 20);
        push.tex1 = bits(tex1, 5, 1) | (bits(tex1, 6, 3) << 1) | (bits(tex1, 2, 3) << 4) | (bits(tex1, 0, 1) << 7) |
                    (bits(tex1, 19, 2) << 8);
        int32_t k = int32_t(bits(tex1, 32, 12) << 20) >> 20; // signed 7.4
        push.lod_k = float(k) / 16.0f;
        // The GS picks a mipmap level from Q, the distance, not from the pixels a texel covers: at a
        // larger --native-scale a surface has more pixels at the same distance, and the level the PS2
        // picked for 640x448 is too coarse for them (further away, the picture blurs where the PS2's
        // was sharp). GSN_LOD_SCALE takes the level log2 of the scale finer (of the smaller of the two
        // scales: --native-widescreen's across is the larger), the detail the GS would show at that
        // resolution; GSN_LOD_FULL level 0 everywhere; GSN_LOD_PS2 the GS's own. At 1x GSN_LOD_SCALE is
        // the GS's own; GSN_LOD_FULL is level 0 at 1x too.
        uint32_t finer = std::min(target_sx, target_sy);
        int mode = lod_mode;
        push.lod_bias = mode == GSN_LOD_FULL ? -64.0f : mode == GSN_LOD_SCALE ? -log2f(float(finer)) : 0.0f;
        push.misc |= uint32_t(aniso_log2) << M_ANISO_SHIFT; // (--native-aniso: gs_frag.h, sample_texture)

        // The rows of the texture the batch reads, from the vertices' V, or T/Q (a frame read as a
        // 1024 x 512 texture reads only the frame's rows). A pixel samples above its V's largest, which
        // is at a vertex when Q stays above 0; bilinear filtering's row past that has no weight.
        auto rows_read = [&]() -> uint32_t {
            uint32_t rows = 1u << th;
            float vmin = 0.0f, vmax = 0.0f;
            for (const Vtx &v : gs->verts) {
                float tv = fst ? v.t : v.q > 0.0f ? v.t / v.q * float(rows) : -1.0f;
                vmin = std::min(vmin, tv);
                vmax = std::max(vmax, tv);
            }
            for (size_t i = 0; interp.active && moving.moves && i < interp.vsrc.size(); i++) {
                // (a picture between: also where a paired 2D item's texture was in the frame before)
                float tv;
                if (interp_t_at_0(interp.vsrc[i], fst, float(rows), gs->verts[i].q, &tv)) {
                    vmin = std::min(vmin, tv);
                    vmax = std::max(vmax, tv);
                }
            }
            // (a V less than a row above the top reads no row above its first: a pass over the frame moved up
            // half a line for the other field, as under water. Taken as reading above, the frame and its Z
            // buffer went back to VRAM and the pass read the frame decoded at 1x, every field picture)
            if (vmin > -1.0f && vmax < float(rows)) {
                rows = std::max(1u, uint32_t(ceilf(vmax)));
            }
            return rows;
        };
        // (the same across, from U or S/Q, for a texture decoded from VRAM; not in a picture between,
        // whose 2D items' S was elsewhere in the frame before: all of it)
        auto cols_read = [&]() -> uint32_t {
            uint32_t cols = 1u << tw;
            if (interp.active && moving.moves) {
                return cols;
            }
            float umin = 0.0f, umax = 0.0f;
            for (const Vtx &v : gs->verts) {
                float tu = fst ? v.s : v.q > 0.0f ? v.s / v.q * float(cols) : -1.0f;
                umin = std::min(umin, tu);
                umax = std::max(umax, tu);
            }
            if (umin >= 0.0f && umax < float(cols)) {
                cols = std::max(1u, uint32_t(ceilf(umax)));
            }
            return cols;
        };
        // a texture that is a target, or in one: read from the target
        Target *source = nullptr;
        for (auto &t : gs->targets) {
            if (t->depth || tbw != t->bw || tbp < t->bp) {
                continue;
            }
            uint32_t blocks = tbp - t->bp;
            uint32_t layout = psm_layout(tpsm);
            if (layout != psm_layout(t->psm) || blocks % 32 != 0) {
                continue;
            }
            uint32_t page = blocks / 32, across = std::max(t->bw, 1u);
            uint32_t x0 = (page % across) * page_width(t->psm), y0 = (page / across) * page_height(t->psm);
            if (y0 >= t->height) {
                continue;
            }
            // A texture whose rows the batch reads run past the target's is decoded from VRAM, the
            // targets' pages back there first: the rows past are VRAM's, or another target's
            uint32_t rows = rows_read();
            if (y0 + rows > t->height) {
                break;
            }
            source = t.get();
            push.src_offset[0] = int32_t(x0);
            push.src_offset[1] = int32_t(y0);
            push.tscale = t->sx | t->sy << 16;
            break;
        }
        if (source) {
            uint32_t src = tpsm == GSN_PSMCT24 ? SRC_TARGET_CT24 : psm_is_16(tpsm) ? SRC_TARGET_CT16 : SRC_TARGET_CT32;
            push.flags |= src << F_SRC_SHIFT;
            reads_target = true;
            if (replaying && snapshot.reads && snapshot.image && source->bp == snapshot.bp && source->image &&
                source->image->get_width() == snapshot.image->get_width() &&
                source->image->get_height() == snapshot.image->get_height()) {
                // (a picture between reading the frame shown before it: the copy kept, take_snapshot)
                target_view = &snapshot.image->get_view();
            } else if (source == color) {
                reads_own_target = true;
            } else {
                Why why(GSN_WHY_TEXTURE);
                msaa_settle(*source);
                target_view = &source->image->get_view();
            }
        } else {
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
            // Without mipmaps, only the rows read (to a whole page row) are the texture's: the menus'
            // backdrop, the play's frame kept at 0x3240 and read as 1024 x 512, runs on round the end of
            // VRAM into their two field buffers, and taken whole it sent both back to VRAM and was
            // decoded again at every field (October 2026: 30 ms a vblank)
            uint32_t cols = 1u << tw, rows = 1u << th;
            if (levels == 1 && bits(clamp, 0, 2) != 3) { // (REGION_REPEAT reads by its masks)
                uint32_t pw = page_width(tpsm);
                cols = std::min(cols, (cols_read() + pw - 1) / pw * pw);
            }
            if (levels == 1 && bits(clamp, 2, 2) != 3) {
                uint32_t ph = page_height(tpsm);
                rows = std::min(rows, (rows_read() + ph - 1) / ph * ph);
            }
            // the texture's pages may have been drawn into: back to VRAM first
            for (uint32_t l = 0; l < levels; l++) {
                uint32_t lo, count;
                buffer_pages(mip_tbp[l], mip_tbw[l], tpsm, std::max(1u, cols >> l), std::max(1u, rows >> l), &lo,
                             &count);
                claim_texture(mip_tbp[l], mip_tbw[l], tpsm, std::max(1u, (1u << tw) >> l), std::max(1u, (1u << th) >> l), lo,
                              count);
            }
            Texture *tex = get_texture(tbp, tbw, tpsm, tw, th, levels, mip_tbp, mip_tbw, cols, rows);
            sa_verify(tex0, levels, mip_tbp, mip_tbw, cols, rows); // (standalone: KF4_SA_VERIFY)
            raw_view = &tex->image->get_view();
            uint32_t src = psm_is_palette(tpsm)  ? SRC_CLUT
                           : tpsm == GSN_PSMCT24 ? SRC_CT24
                           : psm_is_16(tpsm)     ? SRC_CT16
                                                 : SRC_CT32;
            push.flags |= src << F_SRC_SHIFT;
            if (src == SRC_CLUT) {
                clut_colours(tex0, clut);
                want_clut = true;
                tex_alpha_max = 0;
                for (uint32_t i = 0; i < 256; i++) {
                    tex_alpha_max = std::max(tex_alpha_max, clut[i] >> 24);
                }
            }
            if (src == SRC_CLUT || src == SRC_CT32) {
                tex_lo = 255;
                tex_hi = 0;
                for (uint32_t i = 0; i < 256; i++) {
                    if (tex->texels[i]) {
                        uint32_t a = src == SRC_CLUT ? clut[i] >> 24 : i;
                        tex_lo = std::min(tex_lo, a);
                        tex_hi = std::max(tex_hi, a);
                    }
                }
            }
            if (cutout_aa_mode != GSN_CUTOUT_AA_OFF && target_samples > 1 && (src == SRC_CLUT || src == SRC_CT32)) {
                // the least and most alpha of the indices (CLUT) or alphas (32-bit) its texels have,
                // and how many texels have the least
                cut_texels = true;
                for (uint32_t i = 0; i < 256; i++) {
                    if (tex->texels[i]) {
                        uint32_t a = src == SRC_CLUT ? clut[i] >> 24 : i;
                        cut_lo = std::min(cut_lo, a);
                        cut_hi = std::max(cut_hi, a);
                    }
                }
                for (uint32_t i = 0; i < 256; i++) {
                    cut_all += tex->texels[i];
                    cut_clear += (src == SRC_CLUT ? clut[i] >> 24 : i) == cut_lo ? tex->texels[i] : 0;
                }
            }
        }
    }

    // blending: Vulkan's where it can, the shader's where it cannot or the draw reads its own target
    uint32_t ba = bits(alpha, 0, 2), bb = bits(alpha, 2, 2), bc = bits(alpha, 4, 2), bd = bits(alpha, 6, 2);
    uint32_t fix = bits(alpha, 32, 8);
    FixedBlend blend = {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0};
    bool shader_blend = false;
    bool partial_mask = false;
    for (int i = 0; i < 4; i++) {
        uint32_t m = (fbmsk >> (i * 8)) & 0xFF;
        if (m != 0 && m != 0xFF && !(dst16 && (m == 0x07 || m == 0x7F))) {
            partial_mask = true;
        }
    }
    // Vulkan's blending rounds where the GS truncates: exact for a layer added or taken away, and when
    // only one of Cs and Cd is kept, but not for (Cs - Cd) C + Cd. That is a difference of 1 a draw,
    // which the passes over the whole frame (the dimming each frame, the previous frame blended in, the
    // smear) carry on frame after frame: sprites, and draws whose texture is a render target, blend
    // here, exactly. (3D geometry is drawn again each frame, and keeps Vulkan's blending.)
    bool fixed = fixed_blend(ba, bb, bc, bd, fix, &blend);
    bool exact = fixed && (blend.premul != 0 || ba == bb);
    bool sprite = gs->batch_sprites;
    // --native-cutout-aa: a cut-out is a draw whose texture is see-through in places and solid in
    // others: a quarter or more of its texels have its least alpha (a mask, not a layer blended by a
    // smooth alpha as the walls' are, or one see-through all over as the mist's), which the alpha test
    // throws away where others pass (from the alphas its vertices and texels can make: a filtered
    // texel's alpha is between its four's, a primitive's between its vertices'), or which is 0 and
    // blended by. The netting, foliage, fences and grates are both: alpha > 0 drawn, blended by As.
    // Their edges inside the primitives are where the texture's alpha falls, which a pixel shaded once
    // sees at one point; each sample shaded on its own sees the texture, and is tested and blended, at
    // its own place (the AFAIL modes as exact as before, a sample at a time). Not the 2D, which covers
    // whole pixels as without MSAA, nor the frame's passes, whose textures are targets.
    bool cutout = false;
    if (cut_texels && bits(tex0, 34, 1) && cut_lo < cut_hi && cut_clear * 4 >= cut_all && !sprite && !fst &&
        cutout_aa_mode == GSN_CUTOUT_AA_SSAA && sample_shading) {
        uint32_t tfx = bits(tex0, 35, 2);
        uint32_t lo = tfx == 0 ? (as_min * cut_lo) >> 7 : tfx == 2 ? std::min(255u, as_min + cut_lo) : cut_lo;
        uint32_t hi = tfx == 0 ? std::min(255u, (as_max * cut_hi) >> 7) : tfx == 2 ? std::min(255u, as_max + cut_hi) : cut_hi;
        uint32_t ref = bits(test, 4, 8);
        bool splits = false;
        if (ate) {
            switch (atst) {
            case 2: // LESS
            case 5: // GEQUAL
                splits = lo < ref && hi >= ref;
                break;
            case 3: // LEQUAL
            case 6: // GREATER
                splits = lo <= ref && hi > ref;
                break;
            case 4: // EQUAL
            case 7: // NOTEQUAL
                splits = lo <= ref && ref <= hi && lo < hi;
                break;
            }
        }
        cutout = splits || (abe && bc == 0 && ba != bb && lo == 0 && hi > 0);
    }
    // Blending by an As above 0x80 (2.0): Vulkan's blend factor (and the alpha out that stands for it)
    // stops at 1.0. The largest As the batch can make, from its vertices and what is known of the
    // texture's alpha (a CLUT's entries, TEXA for 24 and 16-bit texels):
    if (tme && bits(tex0, 34, 1)) { // TCC
        uint32_t tpsm = bits(tex0, 20, 6);
        if (tpsm == GSN_PSMCT24) {
            tex_alpha_max = bits(gs->reg[R_TEXA], 0, 8);
        } else if (psm_is_16(tpsm)) {
            tex_alpha_max = std::max(bits(gs->reg[R_TEXA], 0, 8), bits(gs->reg[R_TEXA], 32, 8));
        }
        uint32_t tfx = bits(tex0, 35, 2);
        as_max = tfx == 0 ? std::min(255u, (as_max * tex_alpha_max) >> 7)
                 : tfx == 2 ? std::min(255u, as_max + tex_alpha_max)
                            : tex_alpha_max;
    }
    bool as_over = bc == 0 && !blend.premul && as_max > 0x80;
    // (Cs - Cd) As + Cd with As 0x80 at every pixel (or FIX 0x80) is Cs: no blending at all, the GS's
    // ((Cs - Cd) 0x80 >> 7) + Cd being Cs whatever Cd. Into a 16-bit target it is drawn unblended
    // rather than by the shader, which drew the batch in runs of triangles that do not overlap, each
    // its own render pass and MSAA resolve: the reflections' walls and models (map 009's poison pond,
    // 1,600 render passes a picture). The colour, its alpha (FBA, the alpha bit) and the masks come
    // out the same either way (gs_frag.h). (--native-colour high blends in floats: as before.)
    // The same into a 32-bit target (not 24-bit, whose alpha the shader keeps) for a draw whose texture
    // is a render target, which is blended by the shader otherwise (Vulkan's blending rounds a fraction
    // of As as the GS does not; with As 0x80 there is none): the Dark Succubus' body, map 038, reads the
    // screen copied to 0x3540 with As 0x80 and ALPHA 0x44, parts of 24-26 triangles folded over each
    // other; drawn by the shader each part was runs, with MSAA each a render pass and a resolve of the
    // whole frame (NATIVE_DRAW_P8.md, "The Dark Succubus' body"). (Other 32-bit draws with As 0x80 keep
    // Vulkan's blending, as before.)
    bool blend_is_cs = false, cs_target = false;
    if (abe && ba == 0 && bb == 1 && bd == 1 && (bc == 0 || bc == 2) && !blend.premul) {
        uint32_t c_lo = as_min, c_hi = as_max;
        if (bc == 2) {
            c_lo = c_hi = fix;
        } else if (tme && bits(tex0, 34, 1)) {
            uint32_t tfx = bits(tex0, 35, 2);
            c_lo = tfx == 0 ? (as_min * tex_lo) >> 7 : tfx == 2 ? std::min(255u, as_min + tex_lo) : tex_lo;
            c_hi = tfx == 0 ? std::min(255u, (as_max * tex_hi) >> 7) : tfx == 2 ? std::min(255u, as_max + tex_hi) : tex_hi;
        }
        cs_target = c_lo == 0x80 && c_hi == 0x80 && !wide(*color->image) && reads_target && !reads_own_target &&
                    fpsm == GSN_PSMCT32 && cs_over_target();
        blend_is_cs = (c_lo == 0x80 && c_hi == 0x80 && dst16 && !wide(*color->image)) || cs_target;
    }
    if (date || partial_mask || (abe && (reads_own_target || ((reads_target || sprite) && !exact && !cs_target) || (dst16 && !blend_is_cs) ||
                                         (push.misc & (M_PABE)) || !(push.misc & M_COLCLAMP) ||
                                         (bc == 0 && (push.misc & M_FBA)) || as_over || !fixed))) {
        shader_blend = true;
    }
    if (shader_blend) {
        push.flags |= F_SHADER_BLEND | (date ? F_DATE : 0);
        if (!abe) {
            push.alpha = (2 << 0) | (2 << 2) | (2 << 4) | (0 << 6); // (0 - 0) * FIX + Cs: no blending
        }
        gsn_stats.shader_blends++;
    } else if (blend_is_cs) {
    } else if (abe && blend.premul) {
        push.flags |= blend.premul;
    } else if (abe && bc == 0 && !(ba == bb)) {
        push.flags |= F_ALPHA_FACTOR;
    }
    gsn_stats.ns_draw_setup += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - timer.start).count());
    Timer record(gsn_stats.ns_draw_record);
    uint32_t scale_x = color->sx, scale_y = color->sy;
    uint32_t x1 = std::min(sx1 + 1, color->width), y1 = std::min(sy1 + 1, color->height);
    if (sx0 >= x1 || sy0 >= y1) {
        gs->verts.clear();
        return;
    }
    // Triangles wholly outside the scissor draw nothing: left out, before the runs below are cut (a
    // reflection drawn into its 256 x 256 target from the whole scene had most of its triangles off it,
    // each a run of its own). (A pixel's margin each way.) (A native draw's in a picture between: where
    // they are then is the GPU's, at t 0 nd_box_at_0's; not a screen pass's.)
    if (!(nd.pass_active && interp.active)) {
        float cx0 = float(sx0) - 1.0f, cy0 = float(sy0) - 1.0f, cx1 = float(x1) + 1.0f, cy1 = float(y1) + 1.0f;
        size_t kept = 0, n = gs->verts.size();
        Vtx *v = gs->verts.data();
        for (size_t i = 0; i + 2 < n; i += 3) {
            float tx0 = std::min(v[i].x, std::min(v[i + 1].x, v[i + 2].x));
            float tx1 = std::max(v[i].x, std::max(v[i + 1].x, v[i + 2].x));
            float ty0 = std::min(v[i].y, std::min(v[i + 1].y, v[i + 2].y));
            float ty1 = std::max(v[i].y, std::max(v[i + 1].y, v[i + 2].y));
            if (moving.moves && !(tx1 < cx0 || tx0 > cx1 || ty1 < cy0 || ty0 > cy1)) {
                // (inside at t 1)
            } else if (moving.moves) {
                // a picture between's moving vertices: left out only when outside at t 0 too, by a margin
                int32_t box[4] = {int32_t(floorf(tx0)) - 1, int32_t(floorf(ty0)) - 1, int32_t(ceilf(tx1)) + 2,
                                  int32_t(ceilf(ty1)) + 2};
                if (!nd_box_at_0(uint32_t(i), 3, box)) { // (native draw's corners: gsn_native_draw.inc)
                    interp_box_at_0(uint32_t(i), 3, box);
                }
                if (!(float(box[2]) < cx0 || float(box[0]) > cx1 || float(box[3]) < cy0 || float(box[1]) > cy1)) {
                    goto keep;
                }
                continue;
            } else if (tx1 < cx0 || tx0 > cx1 || ty1 < cy0 || ty0 > cy1) {
                continue;
            }
        keep:
            if (kept != i) {
                v[kept] = v[i];
                v[kept + 1] = v[i + 1];
                v[kept + 2] = v[i + 2];
                if (interp.active) {
                    interp.vsrc[kept] = interp.vsrc[i];
                    interp.vsrc[kept + 1] = interp.vsrc[i + 1];
                    interp.vsrc[kept + 2] = interp.vsrc[i + 2];
                }
            }
            kept += 3;
        }
        gsn_stats.primitives += (n - kept) / 3;
        gs->verts.resize(kept);
        if (interp.active) {
            interp.vsrc.resize(kept);
        }
        if (kept == 0) {
            gs->batch_sprites = false;
            return;
        }
    }
    push.misc |= scale_y << M_SCALE_SHIFT | scale_x << M_SCALE_X_SHIFT; // (SCANMSK's lines; nearest reads, gs_frag.h)
    // Each pixel's Z as the GS has it: the vertices' Z interpolated, rounded to a whole Z and kept
    // within the Z buffer's format (gs_zclamp.frag writes the depth). The GPU's own interpolation keeps
    // the fraction, so of two draws over the same plane (the lava's glow over the lava, ZTST GEQUAL)
    // the second could come out a hair behind the first where the GS has them equal; and clamping a
    // vertex beyond the format would tilt the plane.
    bool zclamp = depth != nullptr;
    if (zclamp) {
        push.misc |= M_ZCLAMP;
    }
    // --native-layer-z tolerant: a pixel's Z keeps its fraction ("Z and layers", below the passes)
    bool fine_z = zclamp && layer_z_mode == GSN_LAYER_Z_TOLERANT;
    if (fine_z) {
        push.misc |= M_ZFINE;
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

    // the alpha test: pixels that pass are written whole; those that fail, as AFAIL says
    struct Pass {
        uint32_t select; // 0 all, 1 passing, 2 failing
        uint32_t mask;
        bool zwrite;
        float zbias; // --native-layer-z tolerant: the Z test's tolerance (Z and layers, below)
        float zslope;
    } passes[4];
    int npasses = 0;
    bool zwrite = depth && !zmsk;
    if (!ate || atst == 1) {
        passes[npasses++] = {0, write_mask, zwrite, 0.0f};
    } else {
        if (atst != 0) {
            passes[npasses++] = {1, write_mask, zwrite, 0.0f};
        }
        uint32_t fail_mask = 0;
        bool fail_z = false;
        switch (afail) {
        case 0: // KEEP
            break;
        case 1: // FB_ONLY
            fail_mask = write_mask;
            break;
        case 2: // ZB_ONLY
            fail_z = zwrite;
            break;
        case 3: // RGB_ONLY
            // (a 16-bit frame buffer's alpha bit is written all the same, as paraLLEl-GS has it)
            fail_mask = dst16 ? write_mask : write_mask & 7;
            break;
        }
        if (fail_mask || fail_z) {
            passes[npasses++] = {atst == 0 ? 0u : 2u, fail_mask, fail_z, 0.0f};
        }
    }

    // Z and layers (--native-layer-z tolerant). The GS keeps Z as a whole number per pixel, and two
    // things the game draws come out of the rounding badly at a larger --native-scale:
    // - Where two surfaces meet at a shallow angle (a rock wall against a brick wall, the cave walls
    //   against the lava on map 000), where both round to the same Z the later one wins: a sawtooth of
    //   long teeth along the line. Here every pixel's Z keeps its fraction (M_ZFINE, gs_frag.h): they
    //   meet in a straight line.
    // - The game blends layers over its surfaces as meshes of their own (a wall's second texture; the
    //   darkness strips inside the arches of map 000's caves, the wall's texture again in dark vertex
    //   colours fading out by their alpha), Z tested GEQUAL. Their Z is the surface's to within a
    //   fraction, but the two meshes are cut into triangles differently (their vertices up to a
    //   quarter of a pixel and two Z apart), so the layer fails the test in a pattern: a comb of teeth
    //   and scattered pixels, at --native-scale fine hatching on the tower's stone, a swirl on its top
    //   and a lace of light lines in the arches, coming and going as the camera moves. A layer's
    //   colour passes within one Z of what is there, and half the Z's change across a GS pixel more,
    //   at most one Z more (its depth that much nearer, no Z written). The meshes are a fraction of a
    //   pixel apart, so at a grazing angle (a ledge's face, a tower's top seen from below) their Z are
    //   further apart, and one Z left much of the layer failing, flickering as the camera moved: 21% of
    //   the ledge's layer at 4x3, 10% with the slope (the rest where the layer adds nothing). Without
    //   the cap, edge-on layer triangles drew dark one-pixel lines past the creases they sit on. Then
    //   its Z is written where the test passes as the GS's (with the fraction), so the Z buffer is
    //   what it would be: a layer over a layer finds the surface's Z. (Half a Z, the GS's slack on
    //   average, left the far tower's top with a notch of the tower showing through its rock, 9.5% of
    //   the rock's pixels at 4x, where one Z leaves 3.6%; both clear the arches' lace.)
    // A layer: a triangle with a corner's alpha below 0x80 (where the layer fades out; the opaque
    // surfaces drawn the same way have 0x80 at every corner), blended over what is there by its alpha
    // ((Cs - Cd) As + Cd), Z tested GEQUAL, in a draw that writes Z. Triangles, not batches: a batch
    // can run several meshes together, and an opaque one drawn as a layer lets a thin pole's far side
    // through its near side at its edges. Not the glows and flames, added ((Cs - 0) As + Cd): quads of
    // their own through the things they light. A draw that writes no Z (fire, glows, particles) passes
    // within half a Z, the slack the GS's rounding gave two draws over the same plane (the lava's glow
    // over the lava), in its one pass. The batch is drawn in runs (below) so that a layer's triangles
    // see the triangles before them as on the GS though their colour writes no Z: drawn in one pass,
    // a layer's later triangles over its own earlier ones in front of them were drawn twice over each
    // other where the GS draws them once (flat olive blotches over the walls inside the tower on map
    // 000).
    bool any_zwrite_pass = false;
    for (int i = 0; i < npasses; i++) {
        any_zwrite_pass |= passes[i].zwrite;
    }
    // (a Z: the GS rounds to whole Z of the buffer's format, whatever its width. The tolerance used to
    // be a 16-bit Z's step, 65,536 Z of a 32-bit buffer: the menus draw the figure on the Equipment
    // screen into one, its whole depth within 4,000 Z, so every part of it tied with every other, and
    // parts behind showed through in patches. Where the Z are too large for a float to hold a whole Z,
    // a float's step at the batch's largest.)
    uint32_t zbig = std::min(zmax, depth_max(zpsm));
    float zstep = zbig < (1u << 24) ? 1.0f : float(zbig) / 8388608.0f;
    if (fine_z && !any_zwrite_pass && (ztst == 2 || ztst == 3)) {
        for (int i = 0; i < npasses; i++) {
            passes[i].zbias = 0.5f * zstep;
        }
    }
    // Ties (--native-layer-z tolerant). On the GS a draw over a surface of (nearly) the same Z wins where
    // the two round to the same Z: a sign on the wall it hangs on, a step's edge on the floor, the game's
    // data counts on it. With the fraction kept, such a draw comes out a hair behind and loses in
    // places that move with the camera (a sign's edges flickering behind its wall). So a draw that
    // writes Z and tests GEQUAL passes within half a Z of what is there, the slack the GS's rounding
    // gives on average: its Z first (the nearer of its own and what is there, as on the GS), then its
    // colour within half a Z, no Z written. Where two surfaces cross, the later one wins within half a
    // Z of the line: the line moves by that, and stays straight (the GS's rounding made it a sawtooth).
    // Only for draws whose colour comes out the same drawn twice over itself (not blended, or blended
    // (Cs - Cd) As + Cd with As 0x80 everywhere): within half a Z a batch's later triangles are drawn
    // over its earlier ones in front of them too. The layers (below) have their own passes.
    uint32_t a_lo = as_min, a_hi = as_max; // the least and most As the batch can make
    if (tme && bits(tex0, 34, 1)) {
        uint32_t tfx = bits(tex0, 35, 2);
        a_lo = tfx == 0 ? (as_min * tex_lo) >> 7 : tfx == 2 ? std::min(255u, as_min + tex_lo) : tex_lo;
        a_hi = tfx == 0 ? std::min(255u, (as_max * tex_hi) >> 7) : tfx == 2 ? std::min(255u, as_max + tex_hi) : tex_hi;
    }
    bool same_twice = !abe || (ba == 0 && bb == 1 && bd == 1 && (bc == 0 ? a_lo == 0x80 && a_hi == 0x80 : bc == 2 && fix == 0x80));
    // (the same for a layer batch's other triangles, whose corners' alphas are all 0x80: the As they make)
    uint32_t o_lo = 0x80, o_hi = 0x80;
    if (tme && bits(tex0, 34, 1)) {
        uint32_t tfx = bits(tex0, 35, 2);
        o_lo = tfx == 0 ? (0x80 * tex_lo) >> 7 : tfx == 2 ? std::min(255u, 0x80 + tex_lo) : tex_lo;
        o_hi = tfx == 0 ? std::min(255u, (0x80 * tex_hi) >> 7) : tfx == 2 ? std::min(255u, 0x80 + tex_hi) : tex_hi;
    }
    bool others_same_twice = o_lo == 0x80 && o_hi == 0x80;
    // the alpha test passes every pixel the batch can make: its Z pass needs no texture (gs_zonly.frag)
    uint32_t aref = bits(test, 4, 8);
    bool atest_always = !ate || atst == 1 ||
                        (atst == 2 && a_hi < aref) || (atst == 3 && a_hi <= aref) || (atst == 5 && a_lo >= aref) ||
                        (atst == 6 && a_lo > aref) || (atst == 4 && a_lo == aref && a_hi == aref) ||
                        (atst == 7 && (a_hi < aref || a_lo > aref));
    // A draw of the same triangles as the tie drawn just before it (VU1 program 0x280's second pass: the
    // menus' figure and the models drawn dark, then a sphere map added over them, Cd As + Cs) is tested as
    // that one was: else where the first passed within half a Z of a part in front, the second failed,
    // and the figure on the Equipment screen came out in light and dark patches, triangle by triangle.
    // Its colour is drawn over its own triangles where they lie within half a Z of each other, as the
    // tie's is (a second pass's do not: they are the first's).
    bool repeat = fine_z && any_zwrite_pass && ztst == 2 && !reads_own_target && !(push.flags & F_DATE) &&
                  gs->tick == gs->tie_tick + 1 && gs->tie_verts.size() == gs->verts.size();
    for (size_t i = 0; repeat && i < gs->tie_verts.size(); i++) {
        const Vtx &a = gs->tie_verts[i], &b = gs->verts[i];
        repeat = a.x == b.x && a.y == b.y && a.z == b.z;
    }
    bool tie = fine_z && any_zwrite_pass && ztst == 2 && (same_twice || repeat) && !reads_own_target &&
               !(push.flags & F_DATE);
    if (tie) {
        // (its triangles, for a second pass over them: repeat above)
        gs->tie_tick = gs->tick;
        gs->tie_verts.assign(gs->verts.begin(), gs->verts.end());
    }
    Pass layer_passes[4];
    int nlayer_passes = 0;
    if (fine_z && any_zwrite_pass && ztst == 2 && abe && ba == 0 && bb == 1 && bc == 0 && bd == 1 && as_min < 0x80 &&
        !reads_own_target) {
        for (int i = 0; i < npasses; i++) {
            if (passes[i].mask) {
                layer_passes[nlayer_passes++] = {passes[i].select, passes[i].mask, false, 1.0f * zstep, 0.5f};
            }
            if (passes[i].zwrite) {
                layer_passes[nlayer_passes++] = {passes[i].select, 0, true, 0.0f, 0.0f};
            }
        }
    }
    // A layer batch's other triangles (every corner's alpha 0x80: the wall's overlay where it does not
    // fade, the brick over the end wall of a corridor on map 1D) are ties too when their colour comes out
    // the same drawn twice: they lie on their surface as the layer does, and with no slack they lost to
    // it where its Z came out a hair nearer, the surface's own texture showing through the overlay in
    // flat patches that came and went as the camera moved.
    // A tie drawn in one pass (no alpha test's second pass, no layers) whose colour comes out the same
    // drawn twice: its colour within half a Z of what was there before the batch, no Z written, then the
    // batch again as the GS draws it, Z tested and written exactly (with the fraction). So it wins over
    // what was there within half a Z, and among its own triangles the nearer wins, as it should: with its
    // Z first and then its colour within half a Z of that, a batch's later triangles won over its earlier
    // ones within half a Z of them, and on map 3's rope bridge, far off, the planks' faces (a log of
    // strips a Z apart, one batch) came out as light and dark blocks that moved with the distance where
    // the GS hatches them. (Its second pass, repeat, keeps the Z first: drawn twice it would blend twice.)
    // (the passes the batch needs whatever the ties: a tie's passes need no runs, below)
    int own_passes = npasses;
    bool two_phase = tie && same_twice && npasses == 1 && !nlayer_passes;
    // The first phase matters only where what was there lies behind a triangle's Z by half a Z or less
    // (where it is not behind, the second phase draws over it): a pixel whose Z is outside the batch's Zs
    // and half a Z more has nothing of it, and the depth bounds test turns it away, before the shading
    // where the GPU can (the first phase is a second colour pass: the tower on map 000 24% more GPU time a
    // picture without it). The batch's Zs: its corners', a GS pixel's change of Z more (a pixel shaded at
    // its middle outside the triangle), and in a picture between its Zs at t 0 too (interp_z_at_0).
    // KF4_GSN_DEPTH_BOUNDS=0: without it.
    bool two_phase_bounds = false;
    float bounds_lo = 0.0f, bounds_hi = 1.0f;
    // (A native batch's in a picture between: nd_000.vert moves its corners on the GPU; nd_z_at_0 works out
    // where their Zs are at t 0, or that it cannot: no bounds.)
    if (two_phase && depth_bounds_ok()) {
        double lo = INFINITY, hi = -INFINITY;
        for (size_t t = 0; t + 2 < gs->verts.size(); t += 3) {
            double a, b;
            z_reach(&gs->verts[t], &a, &b);
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
        int nd_z = interp.active && moving.moves_3d ? nd_z_at_0(&lo, &hi) : 0; // (native draw: gsn_native_draw.inc)
        if (interp.active && moving.moves_3d && nd_z == 0) {
            interp_z_at_0(&lo, &hi);
        }
        double scale = depth_scale(zpsm);
        lo = (std::max(lo, 0.0) - 2.0 * zstep) * scale;
        hi = (std::min(hi, double(depth_max(zpsm))) + 2.5 * zstep) * scale;
        if ((lo > 0.0 || hi < 1.0) && nd_z >= 0) {
            two_phase_bounds = true;
            bounds_lo = float(std::max(lo * (1.0 - 1e-6), 0.0));
            bounds_hi = float(std::min(hi * (1.0 + 1e-6), 1.0));
        }
    }
    if (two_phase) {
        Pass p0 = passes[0];
        passes[0] = {p0.select, p0.mask, false, 0.5f * zstep, 0.0f};
        passes[1] = p0;
        npasses = 2;
    } else if (tie || (nlayer_passes && others_same_twice && fine_z && any_zwrite_pass && ztst == 2 &&
                       !reads_own_target && !(push.flags & F_DATE))) {
        Pass z_first[4];
        int n = 0;
        for (int i = 0; i < npasses; i++) {
            if (passes[i].zwrite) {
                z_first[n++] = {atest_always ? 0u : passes[i].select, 0, true, 0.0f, 0.0f};
            }
        }
        for (int i = 0; i < npasses; i++) {
            if (passes[i].mask) {
                z_first[n++] = {passes[i].select, passes[i].mask, false, 0.5f * zstep, 0.0f};
            }
        }
        npasses = n;
        for (int i = 0; i < n; i++) {
            passes[i] = z_first[i];
        }
    }
    auto is_layer = [&](uint32_t t) {
        const Vtx *v = &gs->verts[t * 3];
        return std::min({v[0].rgba >> 24, v[1].rgba >> 24, v[2].rgba >> 24}) < 0x80;
    };

    // A draw that reads the target it draws into (shader blending, DATE) reads a copy taken before it,
    // and one drawn in two passes (the alpha test's) draws every passing pixel before any failing one.
    // Either way, primitives of the batch that overlap would not see each other as the GS's do, one
    // after another: such a batch is drawn in runs of primitives that do not overlap, each with its
    // own copy and passes. (A draw whose texture is its own target reads one copy, taken before the
    // batch, for both.)
    // --native-msaa: the draws with Z to the multisampled images, and those without while they are newer
    // (unless they read their own target, which is resolved for it); the rest to the image
    bool ms = target_samples > 1 && (depth || (color->ms_newer && !reads_own_target));
    bool perspective = perspective_batch(gs->verts);
    bool all_flat = false; // (every triangle of the batch the 2D: flat_2d)
    // --native-2d smooth: each triangle's own texels (texel_rect_2d), of its corners as the game gave them,
    // before flat_2d moves them; after the CLUT for the shaders (below: gs_frag.h's Clut), gs.vert putting
    // the triangle's number in v_fog (so not with fog, nor with the native draw's vertex shaders)
    bool rect_2d = tme && !reads_target && smooth_2d_mode && !fge && !nd.active && !perspective && (scale_x > 1 || scale_y > 1);
    if (rect_2d) {
        gs->rects_2d.resize(gs->verts.size() / 3 * 2);
        for (size_t t = 0; t < gs->verts.size() / 3; t++) {
            texel_rect_2d(&gs->verts[t * 3], fst, push.tex_size, &gs->rects_2d[t * 2]);
        }
    }
    if (ms) {
        msaa_ready(*color);
        if (depth) {
            msaa_ready(*depth);
        }
        // (from here on: each run's copy of the target, below, is resolved from it)
        color->ms_newer = true;
        if (depth && zwrite) {
            depth->ms_newer = true;
        }
        // (the 2D, rectangles of one colour and Z, is shaded once a pixel: --native-cutout-aa)
        if (!perspective && !nd_batch_3d() && flat_2d(gs->verts, &all_flat)) { // (native draw: nd_batch_3d)
            cutout = false;
        }
    } else {
        Why why(GSN_WHY_MSAA);
        msaa_settle(*color);
        if ((scale_x > 1 || scale_y > 1) && !perspective && !nd_batch_3d()) { // (native draw: nd_batch_3d)
            flat_2d(gs->verts, &all_flat);
        }
    }
    if (all_flat && tme && !reads_target && smooth_2d_mode) {
        push.misc |= M_SMOOTH_2D; // (--native-2d smooth: gs_frag.h, sample_level)
    }
    rect_2d = rect_2d && (push.misc & M_SMOOTH_2D);

    // (Z and layers: within a run its other triangles are drawn first, then its layer's, layers_at
    // where they begin. So a run is cut where a triangle overlaps a layer's before it in the run. A
    // layer's over other triangles before it, as the layers over their surface in one batch, keeps its
    // order, as do the others among themselves, with their Z.)
    std::vector<uint32_t> runs(1, 0);
    std::vector<uint32_t> layers_at;
    // A tie's passes need no runs when its colour comes out the same drawn twice. Drawn in two phases
    // (above), each pass is the whole batch's in order. Its Z first, then its colour within half a Z (a
    // layer batch's other triangles): drawn whole, a pixel's colour is the last of the batch's triangles within half a
    // Z of the nearest of them all; in runs, the last within half a Z of the nearest of those before it
    // and itself. A triangle the batch leaves out that runs keep has a nearer one after it, which both
    // keep: the same last. (A repeat's colour is blended: in runs, as before.) Cut into runs, a tie's
    // triangle strips were drawn a few triangles a draw: ten times the draws, the native GS's thread the
    // limit at 240 where many models are in view.
    bool tie_runs = tie && !same_twice;
    // A silhouette (the shadows of VU1 program 0x5A0: DATE with DATM 0 and FBA, so the first triangle over
    // a pixel sets the alpha bit the test then turns the others away with) of one colour, untextured,
    // unfogged, writing no Z, makes the same pixel whichever of its triangles draws it: drawn whole, each
    // reads the target as it was before the batch, which is what the first over the pixel reads in runs,
    // and writes what it would. No runs (a zombie's or a statue's shadow: 1,000 runs a frame on map 009's
    // poison pond, each a render pass and an MSAA resolve).
    bool silhouette = shader_blend && date && !bits(test, 15, 1) && (push.misc & M_FBA) && !(push.misc & M_DST24) &&
                      !(fbmsk & 0x80000000u) && !tme && !fge && !any_zwrite_pass && own_passes == 1 &&
                      !reads_own_target && !gs->verts.empty() && silhouette_runs_mode() == 0;
    // (one colour: a native corner's RGB is its encoding, not its colour; nd_corner_colour gives the colour
    // its shader makes where the CPU knows it, the shadows' native corners' (0x5A0: RGB 0) among them, so
    // that native shadows are drawn whole exactly when the PS2 path's are)
    uint32_t colour0 = 0;
    silhouette = silhouette && nd_corner_colour(gs->verts[0], &colour0);
    for (size_t i = 1; silhouette && i < gs->verts.size(); i++) {
        uint32_t colour;
        silhouette = nd_corner_colour(gs->verts[i], &colour) && colour == colour0;
    }
    bool blend_runs = shader_blend && !silhouette;
    if (silhouette) {
        gsn_stats.silhouettes_whole++;
    }
    if (!reads_own_target && (blend_runs || own_passes > 1 || tie_runs || nlayer_passes)) {
        bool all = blend_runs || own_passes > 1 || tie_runs;
        uint32_t triangles = uint32_t(gs->verts.size() / 3), start = 0;
        std::vector<uint8_t> layer(triangles);
        for (uint32_t t = 0; t < triangles; t++) {
            layer[t] = nlayer_passes && is_layer(t);
        }
        bool run_has_layer = layer.size() && layer[0];
        for (uint32_t t = 1; t < triangles; t++) {
            // (a long run is cut anyway: the test is against each triangle of the run)
            bool cut = (all || run_has_layer) && t - start >= 256;
            for (uint32_t u = start; u < t && !cut; u++) {
                if (all || layer[u]) {
                    cut = triangles_overlap(&gs->verts[t * 3], &gs->verts[u * 3]);
                }
            }
            if (cut) {
                runs.push_back(t * 3);
                start = t;
                run_has_layer = false;
            }
            run_has_layer |= layer[t] != 0;
        }
        if (nlayer_passes) {
            std::vector<Vtx> verts;
            std::vector<std::array<uint32_t, 4>> vsrc;
            verts.reserve(gs->verts.size());
            vsrc.reserve(interp.vsrc.size());
            uint32_t layer_tris = 0;
            runs.push_back(triangles * 3);
            for (size_t r = 0; r + 1 < runs.size(); r++) {
                for (int l = 0; l < 2; l++) {
                    if (l) {
                        layers_at.push_back(uint32_t(verts.size()));
                    }
                    for (uint32_t t = runs[r] / 3; t < runs[r + 1] / 3; t++) {
                        if (layer[t] == l) {
                            verts.insert(verts.end(), gs->verts.begin() + t * 3, gs->verts.begin() + t * 3 + 3);
                            if (interp.active) {
                                vsrc.insert(vsrc.end(), interp.vsrc.begin() + t * 3, interp.vsrc.begin() + t * 3 + 3);
                            }
                            layer_tris += l;
                        }
                    }
                }
            }
            runs.pop_back();
            gs->verts.swap(verts);
            rect_2d = false; // (--native-2d smooth: rects_2d in the order before; layers are the 3D's)
            if (interp.active) {
                interp.vsrc.swap(vsrc);
            }
            if (layer_tris) {
                gsn_stats.layer_draws++;
            }
        }
    }
    runs.push_back(uint32_t(gs->verts.size()));
    // a picture between drawn from the frame's packets: the batch's corners, for gs_replay.vert
    uint32_t corners_at = 0;
    if (interp.active && !nd.active) {
        interp.risky_runs += moving.moves && runs.size() > 2;
        corners_at = interp.corner;
        if (size_t(corners_at) + interp.vsrc.size() > size_t(interp.frame->kicks) * 3 + 5) {
            interp.broken = true; // (never: a kick adds three corners at most)
            corners_at = 0;
        } else {
            uint32_t flags = (ms || scale_x > 1 || scale_y > 1) && !perspective ? IC_FLAT_2D : 0;
            for (size_t i = 0; i < interp.vsrc.size(); i++) {
                uint32_t c[4] = {interp.vsrc[i][0], interp.vsrc[i][1], interp.vsrc[i][2], interp.vsrc[i][3] | flags};
                memcpy(interp.corner_map[1 + corners_at + i], c, sizeof(c)); // (after the count)
            }
            interp.corner += uint32_t(interp.vsrc.size());
        }
    }

    // How each run gets the target's pixels from before it ("Shader blending and the render pass"): a
    // copy outside the render pass (a draw reading its own target; everything with KF4_GSN_BLEND_COPY),
    // a copy in the pass (a single-sampled target), or the multisampled target resolved by the passes
    enum { COPY_NONE, COPY_OUTSIDE, COPY_IN_PASS, COPY_BY_PASS } how = COPY_NONE;
    if (reads_own_target) {
        how = COPY_OUTSIDE;
    } else if (shader_blend) {
        // (a 16-bit target's samples are whole multiples of 8, its alpha 0 or 0x80: the mean of 2, 4 or 8
        // of them is a whole number, which pass_resolve.frag makes as the resolve does, with no half to
        // round either way. Its runs are copies in one pass, not a pass and a resolve each: map 009's
        // poison pond reflected, hundreds of runs a frame.)
        bool exact_mean = ms && dst16 && !wide(*color->image) && !blend_copy_16_by_pass();
        how = blend_copy_forced() ? COPY_OUTSIDE : !ms || msaa_blend_in_pass() || exact_mean ? COPY_IN_PASS : COPY_BY_PASS;
        if (how == COPY_BY_PASS && wide(*color->image)) {
            how = COPY_OUTSIDE; // (--native-colour high: a pass's resolve of 16-bit samples is not always vkCmdResolveImage's)
        }
    }
    // (COPY_BY_PASS: each run's rectangle on the multisampled image, and the first run's copy resolved by
    // a pass of its own that draws nothing)
    std::vector<VkRect2D> areas;
    if (how == COPY_BY_PASS) {
        for (size_t r = 0; r + 1 < runs.size(); r++) {
            int32_t box[4];
            run_box(runs[r], runs[r + 1] - runs[r], box);
            areas.push_back(box_area(*color, *color->ms, box));
        }
        if (areas[0].extent.width) {
            Why why(GSN_WHY_BLEND);
            begin_pass(color, nullptr, true, false, &areas[0], &resolved_copy(0, *color->ms));
            gsn_stats.ms_resolves++;
            gsn_stats.resolves_by[GSN_WHY_BLEND]++;
        }
    }
    for (size_t r = 0; r + 1 < runs.size(); r++) {
        uint32_t first = runs[r], count = runs[r + 1] - runs[r];
        const Image *dest_copy = nullptr;
        // (COPY_BY_PASS: the pass's part of the targets, and the copy it resolves for the next run)
        VkRect2D area = {};
        const Image *resolve_next = nullptr;
        if (how == COPY_OUTSIDE) {
            // a draw that reads its own target reads anywhere in it; one that blends, only its own pixels
            int32_t box[4];
            run_box(first, count, box);
            dest_copy = &snapshot_of(*color, reads_own_target ? nullptr : box);
            if (reads_own_target) {
                target_view = &dest_copy->get_view();
            }
        } else if (how == COPY_IN_PASS) {
            int32_t box[4];
            run_box(first, count, box);
            begin_pass(color, depth, ms, true);
            blend_copy_in_pass(*color, ms, box);
        } else if (how == COPY_BY_PASS) {
            dest_copy = &resolved_copy(int(r & 1), *color->ms);
            if (r + 2 < runs.size() && areas[r + 1].extent.width) {
                area = rect_union(areas[r], areas[r + 1]);
                resolve_next = &resolved_copy(int((r + 1) & 1), *color->ms);
                gsn_stats.ms_resolves++;
                gsn_stats.resolves_by[GSN_WHY_BLEND]++;
            }
        }

        begin_pass(color, depth, ms, false, resolve_next ? &area : nullptr, resolve_next);
        Cmd &c = cmd();
        c.set_opaque_state();
        // (the cut-outs at every sample: draws with Z into the multisampled images)
        bool per_sample = cutout && ms && zclamp;
        Program *program;
        if (how == COPY_IN_PASS) {
            program = per_sample ? gs->program_zclamp_sample_in_ms
                      : ms        ? (zclamp ? gs->program_zclamp_in_ms : gs->program_in_ms)
                                  : (zclamp ? gs->program_zclamp_in : gs->program_in);
            c.set_program(program);
            c.set_input_attachments(0, 4);
        } else {
            program = per_sample ? gs->program_zclamp_sample : zclamp ? gs->program_zclamp : gs->program;
            c.set_program(program);
        }
        if (per_sample && r == 0) {
            gsn_stats.cutout_draws++;
        }
        c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        c.set_cull_mode(VK_CULL_MODE_NONE);
        VkViewport vp = {0.0f, 0.0f, float(color->width * scale_x), float(color->height * scale_y), 0.0f, 1.0f};
        c.set_viewport(vp);
        if (resolve_next) {
            // (within the pass's part of the targets)
            int32_t ox0 = std::max(area.offset.x, int32_t(sx0 * scale_x)), oy0 = std::max(area.offset.y, int32_t(sy0 * scale_y));
            int32_t ox1 = std::min(area.offset.x + int32_t(area.extent.width), int32_t(x1 * scale_x));
            int32_t oy1 = std::min(area.offset.y + int32_t(area.extent.height), int32_t(y1 * scale_y));
            c.set_scissor({{ox0, oy0}, {uint32_t(std::max(ox1 - ox0, 0)), uint32_t(std::max(oy1 - oy0, 0))}});
        } else {
            c.set_scissor({{int32_t(sx0 * scale_x), int32_t(sy0 * scale_y)}, {(x1 - sx0) * scale_x, (y1 - sy0) * scale_y}});
        }
        c.set_texture(0, 0, *raw_view, StockSampler::NearestClamp);
        c.set_texture(0, 1, *target_view, StockSampler::NearestClamp);
        c.set_texture(0, 3, dest_copy ? dest_copy->get_view() : gs->dummy_rgba->get_view(), StockSampler::NearestClamp);
        // (--native-2d smooth: the run's triangles' texels after the CLUT, two a uvec4, as many as fit in
        // the 64 KiB of a uniform buffer: gs_frag.h's Clut)
        uint32_t tris = count / 3;
        bool rects = rect_2d && tris <= (VULKAN_MAX_UBO_SIZE - 1024) / 8;
        std::vector<uint32_t> &clut_data = clut_scratch; // (kept from batch to batch: its room)
        clut_data.assign(rects ? 256 + 4 * ((tris + 1) / 2) : 256, 0u);
        if (want_clut) {
            memcpy(clut_data.data(), clut, sizeof(clut));
        }
        if (rects) {
            memcpy(clut_data.data() + 256, &gs->rects_2d[first / 3 * 2], tris * 8);
        }
        c.set_storage_data(0, 2, clut_data.data(), clut_data.size() * sizeof(uint32_t));

        // depth
        if (depth) {
            c.set_depth_test(true, !zmsk);
            c.set_depth_compare(ztst == 2 ? VK_COMPARE_OP_GREATER_OR_EQUAL : ztst == 3 ? VK_COMPARE_OP_GREATER
                                                                                       : VK_COMPARE_OP_ALWAYS);
        } else {
            c.set_depth_test(false, false);
        }
        // blending
        if (abe && !shader_blend && !blend_is_cs) {
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

        // the vertices
        if (interp.active && !nd.active) {
            // (a picture between's: made of its kicks at its time by gs_replay_corners.comp)
            c.set_vertex_binding(0, *interp.vertices, VkDeviceSize(corners_at + first) * sizeof(Vtx), sizeof(Vtx));
        } else {
            size_t bytes = size_t(count) * sizeof(Vtx);
            void *vdata = c.allocate_vertex_data(0, bytes, sizeof(Vtx));
            memcpy(vdata, gs->verts.data() + first, bytes);
        }
        c.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vtx, x));
        c.set_vertex_attrib(1, 0, VK_FORMAT_R32_UINT, offsetof(Vtx, z));
        c.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vtx, rgba));
        c.set_vertex_attrib(3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, s));
        c.set_vertex_attrib(4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vtx, fog));
        if (nd.active) {
            nd_bind(c); // (native draw: the mesh and the draw's constants)
        }

        // (Z and layers: the run's other triangles with the passes, then its layer's with theirs: the
        // colour within the tolerance, no Z written, then the Z where the test passes, from
        // gs_zonly.frag when the pass is every pixel's and not DATE's: the Z without the texture)
        uint32_t others = layers_at.empty() ? count : layers_at[r] - first;
        for (int l = 0; l < 2; l++) {
            const Pass *rp = l ? layer_passes : passes;
            int nrp = l ? nlayer_passes : npasses;
            uint32_t from = l ? others : 0, n = l ? count - others : others;
            for (int i = 0; i < nrp && n; i++) {
                Push p = push;
                p.misc |= rects ? M_RECT_2D : 0;
                p.flags |= rp[i].select << F_PASS_SHIFT;
                p.zbias = rp[i].zbias;
                p.zslope = rp[i].zslope;
                bool zonly = fine_z && !rp[i].mask && rp[i].zwrite && rp[i].select == 0 && !(push.flags & F_DATE) &&
                             (per_sample ? gs->program_zonly_sample != nullptr : true);
                Program *base = zonly ? (per_sample ? gs->program_zonly_sample : gs->program_zonly) : program;
                if (zonly) {
                    c.set_program(base);
                }
                c.set_color_write_mask(rp[i].mask);
                if (depth) {
                    c.set_depth_test(true, rp[i].zwrite);
                }
                c.push_constants(&p, 0, sizeof(p));
                bool bounded = two_phase_bounds && l == 0 && i == 0;
                if (bounded) {
                    c.set_depth_bounds(true, bounds_lo, bounds_hi);
                }
                nd_draw_range(c, base, n, from, first); // (native draw: c.draw, a call a kind of corner)
                if (bounded) {
                    c.set_depth_bounds(false, 0.0f, 1.0f);
                }
                gsn_stats.draws++;
                if (zonly) {
                    c.set_program(program);
                }
            }
        }
    }
    // the rows the batch can have drawn into (its vertices' within the scissor, a row more each way)
    uint32_t dirty0 = uint32_t(std::max(float(sy0), std::max(0.0f, floorf(vy0) - 1.0f)));
    uint32_t dirty1 = uint32_t(std::min(float(y1), std::max(0.0f, ceilf(vy1) + 2.0f)));
    if (moving.moves) {
        dirty0 = sy0; // (a picture between's moving vertices: anywhere within the scissor)
        dirty1 = y1;
    }
    mark_dirty(*color, dirty0, dirty1);
    if (depth && zwrite) {
        mark_dirty(*depth, dirty0, dirty1);
    }
    if (!ms) {
        msaa_image_written(*color);
    }
    gsn_stats.primitives += gs->verts.size() / 3;
    gs->verts.clear();
    gs->batch_sprites = false;
}

// ---- Primitives ---------------------------------------------------------------------------------
Vtx make_vtx(const QVertex &q, uint32_t prim, const QVertex &colour_from) {
    uint32_t ctxt = (prim >> 9) & 1;
    uint64_t offset = gs->reg[R_XYOFFSET_1 + ctxt];
    Vtx v;
    v.x = (float(int32_t(q.x) - int32_t(bits(offset, 0, 16)))) / 16.0f;
    v.y = (float(int32_t(q.y) - int32_t(bits(offset, 32, 16)))) / 16.0f;
    v.z = q.z;
    v.rgba = colour_from.rgba;
    if ((prim >> 8) & 1) { // FST: UV in texels
        v.s = float(q.u) / 16.0f;
        v.t = float(q.v) / 16.0f;
        v.q = q.q;
    } else {
        v.s = q.s;
        v.t = q.t;
        v.q = q.q;
    }
    v.fog = float(q.fog);
    return v;
}

// A primitive drawn with SCANMSK into one field's lines and over more than 400 of them (Y within the
// scissor): the game draws its two fields as two pictures (Game_ShiftScreenByViewTurn puts the frame
// before, moved by the turn, on the even lines), so the next vblanks' pictures are read a field at a
// time, as the PS2 shows them ("The scanout"; paraLLEl-GS takes the same sign)
void scanmsk_seen(float y0, float y1) {
    if (!(gs->reg[R_SCANMSK] & 2)) {
        return;
    }
    uint64_t scissor = gs->reg[R_SCISSOR_1 + ((gs->batch_prim >> 9) & 1)];
    float top = std::max(y0, float(bits(scissor, 32, 11))), bottom = std::min(y1, float(bits(scissor, 48, 11)));
    if (bottom - top > 400.0f) {
        GSN_LOG(3, "SCANMSK %u primitive over lines %.1f-%.1f\n", uint32_t(gs->reg[R_SCANMSK] & 3), top, bottom);
        gs->field_scan_vblanks = 4;
    }
}

void add_triangle(const Vtx &a, const Vtx &b, const Vtx &c) {
    scanmsk_seen(std::min({a.y, b.y, c.y}), std::max({a.y, b.y, c.y}));
    bool gouraud = (gs->batch_prim >> 3) & 1;
    gs->verts.push_back(a);
    gs->verts.push_back(b);
    gs->verts.push_back(c);
    if (!gouraud) {
        // flat: the last vertex's colour
        Vtx *v = &gs->verts[gs->verts.size() - 3];
        v[0].rgba = v[1].rgba = c.rgba;
    }
    if (interp.active) {
        // (a picture between's: each corner as the three kicks)
        uint32_t info = (gouraud ? 0 : IC_FLAT_SHADE) | (((gs->batch_prim >> 8) & 1) ? IC_FST : 0);
        for (uint32_t i = 0; i < 3; i++) {
            interp.vsrc.push_back({interp.qkick[0], interp.qkick[1], interp.qkick[2], info | i});
        }
    }
}

// A UV sprite filtered bilinearly whose texture coordinates run with its position (a texel a pixel,
// within a sixteenth) and land on texels' middles, or a sixteenth past them on a size not a power of
// two: the GS's DDA steps a hair short and never leaves the middle, so the filter has nothing to
// blend (paraLLEl-GS's set-up turns such a sprite's filter to nearest). Its UV is put on the middles
// exactly, where bilinear filtering is nearest. (Signed: a corner's UV can come out under 0.)
bool snap_sprite_uv(const QVertex &a, const QVertex &b, uint32_t prim, int32_t *snapped_a, int32_t *snapped_b) {
    uint32_t ctxt = (prim >> 9) & 1;
    if (!((prim >> 8) & 1) || !bits(gs->reg[R_TEX1_1 + ctxt], 5, 1)) {
        return false; // STQ, or nearest already
    }
    uint64_t offset = gs->reg[R_XYOFFSET_1 + ctxt];
    int32_t pa[2] = {int32_t(a.x) - int32_t(bits(offset, 0, 16)), int32_t(a.y) - int32_t(bits(offset, 32, 16))};
    int32_t pb[2] = {int32_t(b.x) - int32_t(bits(offset, 0, 16)), int32_t(b.y) - int32_t(bits(offset, 32, 16))};
    int32_t ua[2] = {int32_t(a.u), int32_t(a.v)}, ub[2] = {int32_t(b.u), int32_t(b.v)};
    int32_t lo[2];
    for (int i = 0; i < 2; i++) {
        int32_t d0 = ua[i] - pa[i], d1 = ub[i] - pb[i];
        int32_t mn = std::min(d0, d1), mx = std::max(d0, d1);
        int32_t dist = std::abs(pb[i] - pa[i]);
        bool npot = (dist & (dist - 1)) != 0;
        bool centre = (mn & 15) == 8, next = (mx & 15) == 9 && npot;
        if (!(centre || next) || mx - mn > 1) {
            return false;
        }
        lo[i] = (mn & ~15) | 8; // the middle
    }
    for (int i = 0; i < 2; i++) {
        snapped_a[i] = pa[i] + lo[i];
        snapped_b[i] = pb[i] + lo[i];
    }
    return true;
}

void add_sprite(const QVertex &a, const QVertex &b) {
    uint32_t prim = gs->batch_prim;
    gs->batch_sprites = true;
    int32_t ua[2], ub[2];
    bool snapped = snap_sprite_uv(a, b, prim, ua, ub);
    // a sprite's colour, Z and fog are its second vertex's; its texture coordinates run corner to corner
    QVertex c[4] = {b, b, b, b};
    c[0].x = a.x, c[0].y = a.y, c[0].s = a.s, c[0].t = a.t, c[0].u = a.u, c[0].v = a.v;
    c[1].x = b.x, c[1].y = a.y, c[1].s = b.s, c[1].t = a.t, c[1].u = b.u, c[1].v = a.v;
    c[2].x = a.x, c[2].y = b.y, c[2].s = a.s, c[2].t = b.t, c[2].u = a.u, c[2].v = b.v;
    c[3] = b;
    Vtx v[4];
    for (int i = 0; i < 4; i++) {
        v[i] = make_vtx(c[i], prim, b);
    }
    if (snapped) {
        // corners 0 and 2 have a's U, 0 and 1 a's V
        for (int i = 0; i < 4; i++) {
            v[i].s = float(i == 0 || i == 2 ? ua[0] : ub[0]) / 16.0f;
            v[i].t = float(i == 0 || i == 1 ? ua[1] : ub[1]) / 16.0f;
        }
    }
    scanmsk_seen(std::min(v[0].y, v[3].y), std::max(v[0].y, v[3].y));
    gs->verts.push_back(v[0]);
    gs->verts.push_back(v[1]);
    gs->verts.push_back(v[2]);
    gs->verts.push_back(v[1]);
    gs->verts.push_back(v[3]);
    gs->verts.push_back(v[2]);
    if (interp.active) {
        // (a picture between's: the two kicks, which corner, which triangle)
        uint32_t ctxt = (prim >> 9) & 1;
        uint32_t info = IC_SPRITE | (((prim >> 8) & 1) ? IC_FST : 0) |
                        (((prim >> 8) & 1) && bits(gs->reg[R_TEX1_1 + ctxt], 5, 1) ? IC_SNAP : 0);
        static const uint32_t corners[6] = {0, 1, 2, 1 | 4, 3 | 4, 2 | 4};
        for (uint32_t i = 0; i < 6; i++) {
            interp.vsrc.push_back({interp.qkick[0], interp.qkick[1], 0, info | corners[i]});
        }
    }
}

// A picture between drawn from the frame's packets: the kick, as gs_replay.vert reads it; its number
uint32_t interp_kick(const QVertex &v, uint32_t prim) {
    uint32_t k = interp.kick++;
    if (k >= interp.frame->kicks) {
        interp.broken = true;
        return GSN_IM_NONE;
    }
    uint64_t offset = gs->reg[R_XYOFFSET_1 + ((prim >> 9) & 1)];
    InterpKick &out = interp.kick_map[k];
    InterpKick e;
    e.xy = v.x | v.y << 16;
    e.z = v.z;
    e.rgba = v.rgba;
    e.s = v.s;
    e.t = v.t;
    e.q = v.q;
    e.uv = v.u | v.v << 16;
    e.fog = v.fog;
    e.offset = bits(offset, 0, 16) | bits(offset, 32, 16) << 16;
    e.st_owner = interp.st_src;
    e.rgba_owner = interp.rgba_src;
    e.q_owner = interp.rgba_q_src;
    e.uv_owner = interp.uv_src;
    e.pad[0] = e.pad[1] = e.pad[2] = 0;
    memcpy(&out, &e, sizeof(e)); // (one write to the mapped buffer)
    interp.kick_list[k] = e;
    return k;
}

void vertex_kick(uint32_t address, uint64_t value) {
    nd_kick(); // (native draw: native corners pending are drawn first)
    bool fog = address == R_XYZF2 || address == R_XYZF3;
    bool draw = address == R_XYZF2 || address == R_XYZ2;
    QVertex v;
    v.x = bits(value, 0, 16);
    v.y = bits(value, 16, 16);
    v.z = fog ? bits(value, 32, 24) : uint32_t(value >> 32);
    v.fog = fog ? bits(value, 56, 8) : bits(gs->reg[R_FOG], 56, 8);
    v.rgba = uint32_t(gs->reg[R_RGBAQ]);
    v.q = as_float(uint32_t(gs->reg[R_RGBAQ] >> 32));
    v.s = as_float(uint32_t(gs->reg[R_ST]));
    v.t = as_float(uint32_t(gs->reg[R_ST] >> 32));
    v.u = bits(gs->reg[R_UV], 0, 14);
    v.v = bits(gs->reg[R_UV], 16, 14);

    uint32_t prim = effective_prim();
    if (gs->verts.empty()) {
        gs->batch_prim = prim;
        gs->batch_sprites = false;
    }
    // (a picture between drawn from the frame's packets: the kick recorded, its number kept with the queue's)
    // (native draw: a screen pass's kicks are not the frame's, gsn_nd_pass.inc)
    uint32_t kick_no = interp.active ? (nd.pass_kicking ? GSN_IM_NONE : interp_kick(v, prim)) : 0;
    uint32_t *qk = interp.qkick;
    uint32_t type = prim & 7;
    QVertex *q = gs->queue;
    Vtx *qv = gs->queued;
    switch (type) {
    case 3: // triangles
        qv[gs->queue_count] = make_vtx(v, prim, v);
        qk[gs->queue_count] = kick_no;
        q[gs->queue_count++] = v;
        if (gs->queue_count == 3) {
            if (draw) {
                add_triangle(qv[0], qv[1], qv[2]);
            }
            gs->queue_count = 0;
        }
        break;
    case 4: // a strip
        if (gs->queue_count == 3) {
            q[0] = q[1];
            q[1] = q[2];
            qv[0] = qv[1];
            qv[1] = qv[2];
            qk[0] = qk[1];
            qk[1] = qk[2];
            gs->queue_count = 2;
        }
        qv[gs->queue_count] = make_vtx(v, prim, v);
        qk[gs->queue_count] = kick_no;
        q[gs->queue_count++] = v;
        if (gs->queue_count == 3 && draw) {
            add_triangle(qv[0], qv[1], qv[2]);
        }
        break;
    case 5: // a fan
        if (gs->queue_count == 3) {
            q[1] = q[2];
            qv[1] = qv[2];
            qk[1] = qk[2];
            gs->queue_count = 2;
        }
        qv[gs->queue_count] = make_vtx(v, prim, v);
        qk[gs->queue_count] = kick_no;
        q[gs->queue_count++] = v;
        if (gs->queue_count == 3 && draw) {
            add_triangle(qv[0], qv[1], qv[2]);
        }
        break;
    case 6: // sprites
        qk[gs->queue_count] = kick_no;
        q[gs->queue_count++] = v;
        if (gs->queue_count == 2) {
            if (draw) {
                add_sprite(q[0], q[1]);
            }
            gs->queue_count = 0;
        }
        break;
    default: { // points and lines: not used by the game
        static bool told;
        if (!told) {
            fprintf(stderr, "[gsn] PRIM %u (points or lines) is not drawn\n", type);
            told = true;
        }
        gs->queue_count = 0;
        break;
    }
    }
}

// ---- Transfers ----------------------------------------------------------------------------------
struct TransferRect {
    uint32_t bp, bw, psm, x, y, w, h;
};

TransferRect destination_rect() {
    uint64_t bitbltbuf = gs->reg[R_BITBLTBUF], trxpos = gs->reg[R_TRXPOS], trxreg = gs->reg[R_TRXREG];
    return {bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), bits(bitbltbuf, 56, 6), bits(trxpos, 32, 11),
            bits(trxpos, 48, 11), bits(trxreg, 0, 12), bits(trxreg, 32, 12)};
}

void rect_pages(const TransferRect &r, uint32_t *lo, uint32_t *count) {
    buffer_pages(r.bp, r.bw, r.psm, r.x + r.w, r.y + r.h, lo, count);
}

std::vector<uint32_t> whole_pages(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, bool *rgb_only = nullptr);

// The rectangle has just been written in the CPU's VRAM: the targets over it, the same shape as it
// (row_alias: perhaps starting some rows apart), take it too; those in another shape that were kept
// (prepare_write: it writes whole pages of them) take those pages again; the others were written back
// and dropped before
void sync_targets(const TransferRect &r) {
    std::vector<uint32_t> whole = whole_pages(gs->reg[R_BITBLTBUF], gs->reg[R_TRXPOS], gs->reg[R_TRXREG]);
    for (auto &t : gs->targets) {
        int32_t rows;
        if (!row_alias(*t, r.bp, r.bw, r.psm, &rows)) {
            if (!regular(*t)) {
                continue;
            }
            uint32_t tlo, tcount;
            target_pages(*t, &tlo, &tcount);
            uint32_t ph = page_height(t->psm);
            if (t->replay_only && !t->depth) {
                // (prepare_write kept it: all its pages under the transfer as VRAM has them now)
                uint32_t lo, count;
                rect_pages(r, &lo, &count);
                for (uint32_t k = 0; k < tcount; k++) {
                    if (pages_overlap(lo, count, (tlo + k) % vram_pages, 1)) {
                        load_target_rect(*t, (k % t->bw) * 64, (k / t->bw) * ph, 64, ph);
                    }
                }
                continue;
            }
            for (uint32_t page : whole) {
                uint32_t k = (page + vram_pages - tlo) % vram_pages;
                if (k < tcount) {
                    load_target_rect(*t, (k % t->bw) * 64, (k / t->bw) * ph, 64, ph);
                }
            }
        } else {
            int32_t y = int32_t(r.y) + rows;
            uint32_t h = r.h;
            if (y < 0) {
                if (uint32_t(-y) >= h) {
                    continue;
                }
                h -= uint32_t(-y);
                y = 0;
            }
            load_target_rect(*t, r.x, uint32_t(y), r.w, h);
        }
    }
}


// Before the rectangle is written in the CPU's VRAM: the targets over it in another shape go back to
// VRAM and stop being targets (unless it writes whole pages of them, or all of them)
void prepare_write(const TransferRect &r) {
    uint32_t lo, count;
    rect_pages(r, &lo, &count);
    // the pages the transfer writes whole (when it is an upload of whole pages): a target entirely
    // under them loses every pixel, and needs no writing back first
    std::vector<uint32_t> whole = whole_pages(gs->reg[R_BITBLTBUF], gs->reg[R_TRXPOS], gs->reg[R_TRXREG]);
    std::sort(whole.begin(), whole.end());
    for (size_t i = 0; i < gs->targets.size();) {
        Target *t = gs->targets[i].get();
        uint32_t tlo, tcount;
        target_pages(*t, &tlo, &tcount);
        int32_t rows;
        bool same = row_alias(*t, r.bp, r.bw, r.psm, &rows);
        if (pages_overlap(lo, count, tlo, tcount)) {
            bool covered = !whole.empty() && t->bp % 32 == 0;
            for (uint32_t k = 0; covered && k < tcount; k++) {
                covered = std::binary_search(whole.begin(), whole.end(), (tlo + k) % vram_pages);
            }
            if (same && (rows == 0 || !covered)) {
                // the rest of the target stays newer than VRAM; the rectangle, after the write, both
                // (sync_targets). (A target some rows from the transfer that it covers whole is let go.)
                i++;
                continue;
            }
            // A target whose pages under the transfer are all written whole keeps the rest, and takes
            // those pages from VRAM after the write (sync_targets): it need not go back to VRAM first
            // (not a Z buffer: one may share its pages with another, take_pages)
            bool keep = !covered && !whole.empty() && regular(*t) && !t->depth;
            for (uint32_t k = 0; keep && k < tcount; k++) {
                uint32_t page = (tlo + k) % vram_pages;
                if (pages_overlap(lo, count, page, 1)) {
                    keep = std::binary_search(whole.begin(), whole.end(), page);
                }
            }
            if (keep) {
                i++;
                continue;
            }
            if (t->replay_only && replaying && regular(*t) && !t->depth) {
                // A picture between's own target (the water's wobble: its copy of the picture's frame)
                // stays, and takes the pages under the transfer from VRAM after it (sync_targets): what
                // the game's frame left there, the transfer's bytes added. Dropped, the picture's draws
                // after it that read the copy read it from VRAM at 1x, decoded again every picture. (Only
                // while a picture between is drawn: in the game's frame such a target is nobody's, and was
                // kept over every upload, each of its pages under it loaded again: about 540 pages a frame
                // under water at the pond, the MP drain's and the water's copy of the frame left there.)
                i++;
                continue;
            }
            // (a picture between's own target goes without: the game's frame left VRAM there as it
            // should stay)
            if (!covered && !t->replay_only) {
                if (t->dirty && regular(*t) && t->dirty_pages.size() && shadows_on()) {
                    shadow_make(t); // (its write-back put off: above)
                    continue;
                }
                write_back(*t);
            }
            drop_target(t);
            continue;
        }
        i++;
    }
}

// Uploads (host to local transfers). The game sends several MB a frame, much of it the same textures
// to the same places again (they take turns in the VRAM it shares between them), and the swizzle into
// the GS's layout is the native GS's largest cost. So an upload's bytes are kept until all have come,
// then written at once; one that fills whole pages leaves its pages' bytes behind, found again by a
// hash of the upload, and an upload found again is a copy of those pages (or nothing, when VRAM
// already holds them). Anything that needs VRAM before an upload is whole has it written so far
// (settle_upload), and the rest then goes in as it comes.
struct PendingUpload {
    bool active;
    bool incremental; // being written as it comes (it was settled before it was whole)
    uint64_t bitbltbuf, trxpos, trxreg;
    size_t expect; // its bytes, whole
    std::vector<uint8_t> data;
};
PendingUpload pending_upload;

struct PageImage {
    std::vector<uint32_t> pages;
    std::vector<uint8_t> bytes; // 8 KB a page, in pages' order
    bool rgb_only;              // a 24-bit upload: the pages' words' low three bytes only
    uint64_t serial;            // (page_holders: which image this is, never one let go before)
};
std::unordered_map<uint64_t, std::shared_ptr<PageImage>> page_images;
std::deque<uint64_t> page_image_order;
size_t page_image_bytes;
uint64_t page_image_serial;

// Each page as write_page_image last left it: the same as the image's index-th page (its serial; 0: none), when
// page_written had it at `written`. While nothing has written the page since, it still is, and the image asked
// for again need not compare it.
struct PageHolder {
    uint64_t serial, written;
    size_t index;
};
PageHolder page_holders[vram_pages];

// --frame-rate's pictures between (port/platform/frame_interp.c) draw the game's frame again from its
// packets, uploads and all: some 8 MB a frame, which went through the ring, were kept whole, hashed
// and compared with VRAM again for each picture. So the uploads of the packets between two vblanks are
// recorded as they come (their registers, and the pages they left, or their bytes), and a picture
// between, drawn from those packets, asks for the k-th of them again instead (PORT_GS_REG_REPLAY_UPLOAD
// in place of TRXDIR, and no data). Between the port's marks of a picture between nothing is recorded.
struct RecordedUpload {
    uint64_t bitbltbuf, trxpos, trxreg;
    std::shared_ptr<PageImage> image; // the pages it filled (complete_upload): written again from them
    std::vector<uint8_t> bytes;       // else its bytes, sent again
};
std::vector<RecordedUpload> uploads_now;   // since the last vblank
std::vector<RecordedUpload> uploads_frame; // the last vblank's that had any: what a picture between asks for
int recording = -1;                        // the upload under way in uploads_now (-1: none)
// (replaying: between the marks of a picture between, declared with the Target)

// The pages an upload fills entirely, in order; empty if it does not fill whole pages (a format that
// owns only part of each word, or a rectangle not on page bounds). With rgb_only, a 24-bit upload
// counts as filling its pages too (and *rgb_only says it is one).
std::vector<uint32_t> whole_pages(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, bool *rgb_only) {
    std::vector<uint32_t> pages;
    uint32_t dbp = bits(bitbltbuf, 32, 14), dbw = bits(bitbltbuf, 48, 6), psm = bits(bitbltbuf, 56, 6);
    uint32_t x = bits(trxpos, 32, 11), y = bits(trxpos, 48, 11), w = bits(trxreg, 0, 12), h = bits(trxreg, 32, 12);
    bool full = psm == GSN_PSMCT32 || psm == GSN_PSMCT16 || psm == GSN_PSMCT16S || psm == GSN_PSMT8 ||
                psm == GSN_PSMT4 || psm == GSN_PSMZ32 || psm == GSN_PSMZ16 || psm == GSN_PSMZ16S;
    if (rgb_only) {
        *rgb_only = psm == GSN_PSMCT24 || psm == GSN_PSMZ24;
        full = full || *rgb_only;
    }
    uint32_t pw = page_width(psm), ph = page_height(psm);
    if (psm == GSN_PSMZ16 || psm == GSN_PSMZ16S) {
        ph = 64;
    }
    uint32_t across = (psm == GSN_PSMT8 || psm == GSN_PSMT4) ? dbw / 2 : dbw;
    if (!full || dbp % 32 || x % pw || y % ph || w % pw || h % ph || w == 0 || h == 0 || across == 0 ||
        x + w > across * pw || x + w > 2048 || y + h > 2048) {
        return pages;
    }
    for (uint32_t r = y / ph; r < (y + h) / ph; r++) {
        for (uint32_t c = x / pw; c < (x + w) / pw; c++) {
            pages.push_back((dbp / 32 + r * across + c) % vram_pages);
        }
    }
    return pages;
}

inline uint64_t rotate_left(uint64_t v, int n) {
    return (v << n) | (v >> (64 - n));
}

// An upload's bytes, and where they go, as 64 bits. (Four lanes of 8 bytes each, so that the
// multiplies do not wait for each other: it runs at about the speed of reading the bytes.)
uint64_t upload_hash(const PendingUpload &p, const uint8_t *d, size_t size) {
    const uint64_t K1 = 0x9E3779B97F4A7C15ull, K2 = 0xC2B2AE3D27D4EB4Full, K3 = 0x165667B19E3779F9ull;
    uint64_t lane[4] = {p.bitbltbuf ^ K1, p.trxpos ^ K2, p.trxreg ^ K3, size};
    size_t i = 0;
    for (; i + 32 <= size; i += 32) {
        for (int k = 0; k < 4; k++) {
            uint64_t v;
            memcpy(&v, d + i + k * 8, 8);
            lane[k] = rotate_left(lane[k] + v * K2, 31) * K1;
        }
    }
    uint64_t h = rotate_left(lane[0], 1) + rotate_left(lane[1], 7) + rotate_left(lane[2], 12) + rotate_left(lane[3], 18);
    for (; i < size; i++) {
        h = (h ^ d[i]) * K3;
    }
    h ^= h >> 33;
    h *= K2;
    h ^= h >> 29;
    h *= K3;
    return h ^ (h >> 32);
}

// An upload's bytes, swizzled into VRAM as they are
void swizzle_upload(const uint8_t *data, size_t bytes) {
    Timer swizzle(gsn_stats.ns_swizzle);
    uint32_t lo = 0, hi = 0;
    if (gsn_upload_data(&gs->upload, gs->vram.data(), data, bytes, &lo, &hi) && lo <= hi) {
        pages_written(lo, hi - lo + 1);
    }
}

void upload_done() {
    if (pending_upload.active && pending_upload.incremental) {
        sa_uploaded_from_vram(pending_upload.bitbltbuf, pending_upload.trxpos, pending_upload.trxreg); // (standalone)
    }
    {
        Timer sync(gsn_stats.ns_sync);
        sync_targets(destination_rect());
    }
    gs->transfer_dir = 3;
    gs->upload.active = 0;
    pending_upload.active = false;
    pending_upload.data.clear();
}

void write_page_image(const PageImage &image);

// The whole upload: its pages copied in if it was seen before, else swizzled (and kept)
void complete_upload(const uint8_t *data, size_t size) {
    PendingUpload &p = pending_upload;
    uint64_t key = upload_hash(p, data, size);
    sa_uploaded(p.bitbltbuf, p.trxpos, p.trxreg, data, size, key); // (standalone: the page-ownership table)
    bool rgb_only = false;
    std::vector<uint32_t> pages = whole_pages(p.bitbltbuf, p.trxpos, p.trxreg, &rgb_only);
    auto found = pages.empty() ? page_images.end() : page_images.find(key);
    RecordedUpload *rec = recording >= 0 ? &uploads_now[size_t(recording)] : nullptr;
    if (found != page_images.end() && found->second->pages == pages && found->second->rgb_only == rgb_only) {
        if (rec) {
            rec->image = found->second;
        }
        write_page_image(*found->second);
        upload_done();
        return;
    }
    swizzle_upload(data, size);
    if (!pages.empty()) {
        auto image = std::make_shared<PageImage>();
        image->pages = pages;
        image->rgb_only = rgb_only;
        image->serial = ++page_image_serial;
        image->bytes.resize(pages.size() * 8192);
        for (size_t i = 0; i < pages.size(); i++) {
            memcpy(image->bytes.data() + i * 8192, gs->vram.data() + size_t(pages[i]) * 8192, 8192);
        }
        page_image_bytes += image->bytes.size();
        if (rec) {
            rec->image = image;
        }
        page_images[key] = std::move(image);
        page_image_order.push_back(key);
        // the oldest let go beyond 64 MB of them (one a picture between still asks for is kept by it)
        while (page_image_bytes > (64u << 20) && !page_image_order.empty()) {
            auto old = page_images.find(page_image_order.front());
            if (old != page_images.end()) {
                page_image_bytes -= old->second->bytes.size();
                page_images.erase(old);
            }
            page_image_order.pop_front();
        }
    } else if (rec) {
        rec->bytes.assign(data, data + size);
    }
    upload_done();
}

// An upload found again: its pages copied into VRAM (those that differ)
void write_page_image(const PageImage &image) {
    {
        const std::vector<uint32_t> &pages = image.pages;
        bool rgb_only = image.rgb_only;
        bool changed = false;
        for (size_t i = 0; i < pages.size(); i++) {
            PageHolder &holder = page_holders[pages[i]];
            if (holder.serial == image.serial && holder.index == i && holder.written == gs->page_written[pages[i]]) {
                gsn_stats.upload_pages_held++;
                continue;
            }
            uint8_t *dst = gs->vram.data() + size_t(pages[i]) * 8192;
            const uint8_t *src = image.bytes.data() + i * 8192;
            if (rgb_only) {
                // (the words' top bytes are not the upload's: they stay)
                bool page_changed = false;
                for (size_t k = 0; k < 8192; k += 4) {
                    uint32_t d, s;
                    memcpy(&d, dst + k, 4);
                    memcpy(&s, src + k, 4);
                    uint32_t v = (d & 0xFF000000u) | (s & 0xFFFFFFu);
                    page_changed |= v != d;
                    memcpy(dst + k, &v, 4);
                }
                if (page_changed) {
                    pages_written(pages[i], 1);
                    changed = true;
                }
            } else if (memcmp(dst, src, 8192) != 0) {
                memcpy(dst, src, 8192);
                pages_written(pages[i], 1);
                changed = true;
            }
            holder = {image.serial, gs->page_written[pages[i]], i};
        }
        gsn_stats.uploads_skipped += !changed;
        gsn_stats.uploads_copied += changed;
    }
}

// Something needs VRAM before the upload under way is whole: what has come goes in now
void settle_upload() {
    PendingUpload &p = pending_upload;
    if (!p.active || p.incremental) {
        return;
    }
    p.incremental = true;
    if (recording >= 0) {
        uploads_now[size_t(recording)].bytes.insert(uploads_now[size_t(recording)].bytes.end(), p.data.begin(), p.data.end());
    }
    // A target in another shape that prepare_write kept, as the upload writes whole pages of it: those
    // pages are VRAM's from now on (half written), and must not go back over it before sync_targets
    // takes them from it
    std::vector<uint32_t> whole = whole_pages(p.bitbltbuf, p.trxpos, p.trxreg);
    TransferRect r = destination_rect();
    for (auto &t : gs->targets) {
        int32_t rows;
        if (row_alias(*t, r.bp, r.bw, r.psm, &rows) || !regular(*t)) {
            continue;
        }
        uint32_t tlo, tcount;
        target_pages(*t, &tlo, &tcount);
        for (uint32_t page : whole) {
            uint32_t k = (page + vram_pages - tlo) % vram_pages;
            if (k < tcount && k < t->dirty_pages.size()) {
                t->dirty_pages[k] = 0;
            }
        }
    }
    swizzle_upload(p.data.data(), p.data.size());
    p.data.clear();
}

void interp_forget();

void forget_uploads() {
    interp_forget();
    nd_prep_forget(); // (rc/own: the frames' draws kept for their pictures between)
    pending_upload.active = false;
    pending_upload.data.clear();
    uploads_now.clear();
    uploads_frame.clear();
    recording = -1;
    replaying = false;
    snapshot = Snapshot();
    page_images.clear();
    page_image_order.clear();
    page_image_bytes = 0;
    memset(page_holders, 0, sizeof(page_holders));
}

void begin_transfer(uint32_t dir) {
    sa_transfer(dir); // (standalone: the scene's turn ends if the transfer touches its targets)
    settle_upload();
    uint64_t bitbltbuf = gs->reg[R_BITBLTBUF], trxpos = gs->reg[R_TRXPOS], trxreg = gs->reg[R_TRXREG];
    gs->transfer_dir = dir;
    if (dir == 0) {
        TransferRect r = destination_rect();
        prepare_write(r);
        shadow_written(bitbltbuf, trxpos, trxreg);
        gsn_upload_begin(&gs->upload, bitbltbuf, trxpos, trxreg);
        gsn_stats.uploads++;
        recording = -1;
        if (!replaying) {
            uploads_now.push_back({bitbltbuf, trxpos, trxreg, nullptr, {}});
            recording = int(uploads_now.size() - 1);
        }
        sa_upload_begins(bitbltbuf, trxpos, trxreg, !replaying); // (standalone)
        PendingUpload &u = pending_upload;
        u.active = gs->upload.active != 0;
        u.incremental = false;
        u.bitbltbuf = bitbltbuf;
        u.trxpos = trxpos;
        u.trxreg = trxreg;
        u.expect = (size_t(bits(trxreg, 0, 12)) * bits(trxreg, 32, 12) * gsn_psm_bits(bits(bitbltbuf, 56, 6)) + 127) / 128 * 16;
        u.data.clear();
    } else if (dir == 1) {
        // local to host: what the GPU drew there, back first
        uint32_t sbp = bits(bitbltbuf, 0, 14), sbw = bits(bitbltbuf, 16, 6), spsm = bits(bitbltbuf, 24, 6);
        uint32_t lo, count;
        buffer_pages(sbp, sbw, spsm, bits(trxpos, 0, 11) + bits(trxreg, 0, 12), bits(trxpos, 16, 11) + bits(trxreg, 32, 12),
                     &lo, &count);
        claim_pages(lo, count, nullptr, false);
        uint32_t w = bits(trxreg, 0, 12), h = bits(trxreg, 32, 12);
        size_t qwords = (size_t(w) * h * gsn_psm_bits(spsm) + 127) / 128;
        gs->readback.assign(qwords * 16, 0);
        gsn_vram_read_image(gs->vram.data(), bitbltbuf, trxpos, trxreg, gs->readback.data(), qwords);
        gs->readback_at = 0;
    } else if (dir == 2) {
        // local to local
        uint32_t sbp = bits(bitbltbuf, 0, 14), sbw = bits(bitbltbuf, 16, 6), spsm = bits(bitbltbuf, 24, 6);
        uint32_t sx = bits(trxpos, 0, 11), sy = bits(trxpos, 16, 11);
        TransferRect d = destination_rect();
        uint32_t w = d.w, h = d.h;
        // the frame copied by the GPU: from a target, into one, when the shapes allow
        Target *src = find_target(sbp, sbw, spsm);
        if (src && src->dirty && !src->depth && psm_layout(spsm) == psm_layout(d.psm) && sx + w <= src->width &&
            sy + h <= src->height && d.x == sx && d.y == sy && d.bw == sbw) {
            uint32_t lo, count;
            rect_pages(d, &lo, &count);
            // Whole pages of a 32-bit buffer from its start, which the source's pages do not share: every bit
            // of them is the copy's, so what was there need not go back to VRAM, nor come from it (under
            // water and in the MP drain, the wobble's copy of the frame each picture: three write-backs a
            // picture before)
            uint32_t slo, scount;
            target_pages(*src, &slo, &scount);
            if (d.psm == GSN_PSMCT32 && d.x == 0 && d.y == 0 && d.bp % 32 == 0 && d.bw > 0 && w == d.bw * 64 &&
                h % page_height(d.psm) == 0 && !pages_overlap(lo, count, slo, scount)) {
                shadow_written(bitbltbuf, trxpos, trxreg); // (the shadows' pages under it let go)
                overwrite_lo = lo;
                overwrite_count = count;
            }
            Target *dst = find_target(d.bp, d.bw, d.psm);
            if (!dst || dst->width < src->width || dst->height < d.y + h) {
                // the destination becomes a target of its own, its pixels from VRAM first
                dst = get_target(d.bp, d.bw, d.psm, src->width, std::max(d.y + h, dst ? dst->height : 0u));
            }
            claim_pages(lo, count, dst, true);
            overwrite_count = 0;
            // claim_pages may have dropped and re-made nothing of src: find it again
            src = find_target(sbp, sbw, spsm);
            dst = find_target(d.bp, d.bw, d.psm);
            if (src && dst) {
                Why why(GSN_WHY_COPY);
                msaa_settle(*src);
                msaa_settle(*dst);
                end_pass();
                transfer_barrier();
                uint32_t kx = src->sx, ky = src->sy;
                cmd().copy_image(*dst->image, *src->image, {int32_t(d.x * kx), int32_t(d.y * ky), 0},
                                 {int32_t(sx * kx), int32_t(sy * ky), 0}, {w * kx, h * ky, 1},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
                transfer_barrier();
                mark_dirty(*dst, d.y, d.y + h);
                msaa_image_written(*dst);
                gsn_stats.target_copies++;
                gs->transfer_dir = 3;
                return;
            }
        }
        sa_copied(bitbltbuf, trxpos, trxreg); // (standalone: not its uploads' any more)
        uint32_t lo, count;
        buffer_pages(sbp, sbw, spsm, sx + w, sy + h, &lo, &count);
        claim_pages(lo, count, nullptr, false);
        prepare_write(d);
        shadow_written(bitbltbuf, trxpos, trxreg);
        if (gsn_vram_copy(gs->vram.data(), bitbltbuf, trxpos, trxreg)) {
            uint32_t dlo, dcount;
            rect_pages(d, &dlo, &dcount);
            pages_written(dlo, dcount);
        }
        sync_targets(d);
        gs->transfer_dir = 3;
    } else {
        gs->transfer_dir = 3;
    }
}

void image_data(const uint8_t *data, size_t bytes) {
    Timer timer(gsn_stats.ns_upload);
    if (gs->transfer_dir != 0 || !gs->upload.active) {
        return;
    }
    gsn_stats.upload_bytes += bytes;
    PendingUpload &p = pending_upload;
    if (p.incremental) {
        if (recording >= 0) {
            uploads_now[size_t(recording)].bytes.insert(uploads_now[size_t(recording)].bytes.end(), data, data + bytes);
        }
        swizzle_upload(data, bytes);
        if (!gs->upload.active) {
            upload_done();
        }
        return;
    }
    if (p.data.empty() && bytes >= p.expect) {
        complete_upload(data, bytes); // (all of it at once: no need to keep it)
        return;
    }
    p.data.insert(p.data.end(), data, data + bytes);
    if (p.data.size() >= p.expect) {
        complete_upload(p.data.data(), p.data.size());
    }
}

// --frame-rate's pictures between: the frame shown before them, kept when they begin (frame_interp.c's
// mark 0xF2), and read in their place where they read that buffer as a texture (the previous frame
// blended in, Game_BlendPreviousFrame): the buffer itself is the one they are drawn into.
void take_snapshot(uint32_t fbp) {
    flush_batch();
    sa_sync();
    snapshot.image.reset();
    for (auto &t : gs->targets) {
        if (t->depth || t->bp != fbp * 32 || !t->image) {
            continue;
        }
        msaa_settle(*t);
        end_pass();
        uint32_t w = t->image->get_width(), h = t->image->get_height();
        ImageHandle image = make_image(w, h, t->image->get_format(), 1, true);
        transfer_barrier();
        cmd().copy_image(*image, *t->image, {0, 0, 0}, {0, 0, 0}, {w, h, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        transfer_barrier();
        snapshot.bp = t->bp;
        snapshot.image = image;
        break;
    }
}

// A picture between asks for the k-th upload of the frame again (PORT_GS_REG_REPLAY_UPLOAD: k, and the
// destination's half of BITBLTBUF to check it), with this upload's registers as the frame had them
void replay_upload(uint64_t value) {
    uint32_t k = uint32_t(value);
    if (k >= uploads_frame.size() || uint32_t(uploads_frame[k].bitbltbuf >> 32) != uint32_t(value >> 32) ||
        (uploads_frame[k].bitbltbuf >> 32) != (gs->reg[R_BITBLTBUF] >> 32) || uploads_frame[k].trxpos != gs->reg[R_TRXPOS] ||
        uploads_frame[k].trxreg != gs->reg[R_TRXREG]) {
        gsn_stats.replay_misses++;
        return;
    }
    // (kept alive here: image_data does not record, between the marks)
    std::shared_ptr<PageImage> image = uploads_frame[k].image;
    flush_batch();
    gs->reg[R_TRXDIR] = 0;
    begin_transfer(0);
    if (image) {
        Timer timer(gsn_stats.ns_upload);
        write_page_image(*image);
        sa_upload_replayed(k); // (standalone)
        upload_done();
    } else if (!uploads_frame[k].bytes.empty()) {
        image_data(uploads_frame[k].bytes.data(), uploads_frame[k].bytes.size());
    }
    gsn_stats.replay_uploads++;
}

// ---- Register writes ----------------------------------------------------------------------------
// TEX1's MTBA: a TEX0 written makes MIPTBP1 itself, levels 1 to 3 packed after level 0, each a quarter
// of the one before and half its buffer width
void mip_addresses(uint32_t ctxt) {
    if (!bits(gs->reg[R_TEX1_1 + ctxt], 9, 1)) {
        return;
    }
    uint64_t tex0 = gs->reg[R_TEX0_1 + ctxt];
    uint32_t psm = bits(tex0, 20, 6);
    uint32_t w = 1u << bits(tex0, 26, 4), h = 1u << bits(tex0, 30, 4);
    // a block of the format, in pixels
    uint32_t bw = psm == GSN_PSMT4 ? 32 : psm == GSN_PSMT8 || psm_is_16(psm) ? 16 : 8;
    uint32_t bh = psm == GSN_PSMT8 ? 16 : psm == GSN_PSMT4 ? 16 : 8;
    uint32_t base = bits(tex0, 0, 14);
    uint32_t blocks = (w / bw) * (h / bh);
    uint32_t row = w / 64;
    uint64_t miptbp1 = 0;
    for (int l = 0; l < 3; l++) {
        base += blocks;
        blocks /= 4;
        row /= 2;
        miptbp1 |= (uint64_t(base & 0x3FFF) | (uint64_t(row & 0x3F) << 14)) << (l * 20);
    }
    gs->reg[R_MIPTBP1_1 + ctxt] = miptbp1;
}

void gs_write(uint32_t address, uint64_t value) {
    if (nd.skipping && address != 0x7B) {
        nd_skip(address); // (--native-draw-compare: the PS2 path's packets left out)
        return;
    }
    switch (address) {
    case R_XYZF2:
    case R_XYZ2:
    case R_XYZF3:
    case R_XYZ3:
        vertex_kick(address, value);
        return;
    case R_RGBAQ:
    case R_ST:
    case R_UV:
    case R_FOG:
        gs->reg[address] = value;
        if (interp.active) {
            // (a picture between drawn from the frame's packets: in whose stretch it was written; packed()
            // has RGBAQ's Q from the last PACKED ST)
            if (address == R_RGBAQ) {
                interp.rgba_src = interp.kick;
                interp.rgba_q_src = GSN_IM_NONE;
            } else if (address == R_ST) {
                interp.st_src = interp.kick;
            } else if (address == R_UV) {
                interp.uv_src = interp.kick;
            }
        }
        return;
    case R_PRIM:
        gs->queue_count = 0;
        if ((value & 0x7FF) != (gs->reg[R_PRIM] & 0x7FF)) {
            uint64_t old = gs->reg[R_PRIM];
            gs->reg[R_PRIM] = value & 0x7FF;
            if ((effective_prim() & 0x7F8) != (gs->batch_prim & 0x7F8) && !gs->verts.empty()) {
                gs->reg[R_PRIM] = old;
                flush_batch();
                gs->reg[R_PRIM] = value & 0x7FF;
            }
        }
        return;
    case R_TEX0_1:
    case R_TEX0_2: {
        uint64_t old = gs->reg[address];
        uint64_t clut_before = gs->clut_version;
        // the CLUT is loaded as TEX0 is written; a batch drawn before keeps its own
        uint16_t saved[512];
        memcpy(saved, gs->clut, sizeof(saved));
        settle_upload(); // the CLUT comes from VRAM
        clut_load(value);
        bool changed = (old & ~(uint64_t(7) << 61)) != (value & ~(uint64_t(7) << 61)) || gs->clut_version != clut_before;
        if (changed && !gs->verts.empty()) {
            uint16_t now[512];
            memcpy(now, gs->clut, sizeof(now));
            memcpy(gs->clut, saved, sizeof(saved));
            flush_batch();
            memcpy(gs->clut, now, sizeof(now));
        }
        sa_clut_load(value); // (standalone: the table's CLUT)
        gs->reg[address] = value;
        mip_addresses(address - R_TEX0_1);
        return;
    }
    case R_TEX2_1:
    case R_TEX2_2: {
        // TEX2: TEX0's PSM and CLUT fields only
        uint32_t tex0 = address - R_TEX2_1 + R_TEX0_1;
        const uint64_t mask = (uint64_t(0x3F) << 20) | (~uint64_t(0) << 37);
        gs_write(tex0, (gs->reg[tex0] & ~mask) | (value & mask));
        return;
    }
    case R_TEXFLUSH:
    case 0x0F: // NOP
    case 0x7D: // the port's frame-rate mark (frame_interp.c): a picture between begins or ends
        if (address == 0x7D) {
            nd_mark(value); // (native draw: where the draws after it are, gsn_native_draw.inc)
        }
        if (bits(value, 32, 8) == 0xF0) {
            replaying = true;
            recording = -1;
        } else if (bits(value, 32, 8) == 0xF1) {
            replaying = false;
        } else if (bits(value, 32, 8) == 0xF2) {
            take_snapshot(bits(value, 0, 9));
        } else if (bits(value, 32, 8) == 0xF4 || bits(value, 32, 8) == 0xF5) {
            flush_batch();
            snapshot.reads = bits(value, 32, 8) == 0xF4;
        }
        return;
    case 0x7C: // the port's upload asked for again (frame_interp.c)
        replay_upload(value);
        return;
    case 0x7E: // the port's widescreen mark (gs.c)
        return;
    case 0x7B: // the port's native draw (PORT_GS_REG_NATIVE_DRAW: gsn_native_draw.inc)
        nd_register(value);
        return;
    case R_SIGNAL:
    case R_FINISH:
    case R_LABEL:
        return;
    case R_XYOFFSET_1:
    case R_XYOFFSET_2:
        // applied to each vertex as it is queued
        gs->reg[address] = value;
        return;
    case R_BITBLTBUF:
    case R_TRXPOS:
    case R_TRXREG:
        gs->reg[address] = value;
        return;
    case R_TRXDIR:
        flush_batch();
        gs->reg[address] = value;
        begin_transfer(uint32_t(value & 3));
        return;
    case R_HWREG:
        image_data(reinterpret_cast<const uint8_t *>(&value), 8);
        return;
    }
    if (gs->reg[address & 0xFF] != value) {
        flush_batch();
        gs->reg[address & 0xFF] = value;
    }
}

void packed(uint32_t desc, const uint64_t q[2]) {
    switch (desc) {
    case 0x0:
        gs_write(R_PRIM, q[0] & 0x7FF);
        break;
    case 0x1:
        gs_write(R_RGBAQ, (q[0] & 0xFF) | ((q[0] >> 24) & 0xFF00) | ((q[1] & 0xFF) << 16) |
                              ((q[1] >> 8) & 0xFF000000u) | (uint64_t(gs->internal_q) << 32));
        interp.rgba_q_src = interp.q_src;
        break;
    case 0x2:
        gs->internal_q = uint32_t(q[1]);
        interp.q_src = interp.kick;
        gs_write(R_ST, q[0]);
        break;
    case 0x3:
        gs_write(R_UV, (q[0] & 0x3FFF) | (((q[0] >> 32) & 0x3FFF) << 16));
        break;
    case 0x4: {
        uint64_t v = (q[0] & 0xFFFF) | (((q[0] >> 32) & 0xFFFF) << 16) | (((q[1] >> 4) & 0xFFFFFF) << 32) |
                     (((q[1] >> 36) & 0xFF) << 56);
        gs_write(((q[1] >> 47) & 1) ? R_XYZF3 : R_XYZF2, v);
        break;
    }
    case 0x5: {
        uint64_t v = (q[0] & 0xFFFF) | (((q[0] >> 32) & 0xFFFF) << 16) | ((q[1] & 0xFFFFFFFFu) << 32);
        gs_write(((q[1] >> 47) & 1) ? R_XYZ3 : R_XYZ2, v);
        break;
    }
    case 0xA:
        gs_write(R_FOG, ((q[1] >> 36) & 0xFF) << 56);
        break;
    case 0xC: {
        uint64_t v = (q[0] & 0xFFFF) | (((q[0] >> 32) & 0xFFFF) << 16) | (((q[1] >> 4) & 0xFFFFFF) << 32) |
                     (((q[1] >> 36) & 0xFF) << 56);
        gs_write(R_XYZF3, v);
        break;
    }
    case 0xD: {
        uint64_t v = (q[0] & 0xFFFF) | (((q[0] >> 32) & 0xFFFF) << 16) | ((q[1] & 0xFFFFFFFFu) << 32);
        gs_write(R_XYZ3, v);
        break;
    }
    case 0xE:
        gs_write(uint32_t(q[1] & 0xFF), q[0]);
        break;
    case 0xF:
        break;
    default: // TEX0, CLAMP: the register's own value
        gs_write(desc, q[0]);
        break;
    }
}

void gif(int path, const uint8_t *data, size_t bytes) {
    GifPath *p = &gs->paths[path];
    size_t count = bytes / 16;
    for (size_t i = 0; i < count; i++) {
        uint64_t q[2];
        memcpy(q, data + i * 16, 16);
        if (p->left == 0) {
            uint64_t tag = q[0];
            uint32_t nloop = uint32_t(tag & 0x7FFF);
            p->eop = uint32_t((tag >> 15) & 1);
            p->flg = uint32_t((tag >> 58) & 3);
            p->nreg = uint32_t(tag >> 60);
            if (p->nreg == 0) {
                p->nreg = 16;
            }
            p->regs = q[1];
            p->reg = 0;
            if (p->flg == 0) {
                p->left = nloop * p->nreg;
                if ((tag >> 46) & 1) {
                    gs_write(R_PRIM, (tag >> 47) & 0x7FF);
                }
            } else if (p->flg == 1) {
                p->left = (nloop * p->nreg + 1) / 2;
                p->dwords = nloop * p->nreg;
            } else {
                p->left = nloop;
            }
            p->in_packet = !(p->eop && p->left == 0);
            gs->internal_q = 0x3F800000u; // a tag sets Q to 1
            interp.q_src = GSN_IM_NONE;
            continue;
        }
        if (p->flg == 0) {
            packed(uint32_t(p->regs >> (p->reg * 4)) & 0xF, q);
            p->reg = p->reg + 1 == p->nreg ? 0 : p->reg + 1;
            p->left--;
        } else if (p->flg == 1) {
            for (int half = 0; half < 2 && p->dwords; half++, p->dwords--) {
                uint32_t desc = uint32_t(p->regs >> (p->reg * 4)) & 0xF;
                if (desc < 0xE) {
                    gs_write(desc, q[half]);
                }
                p->reg = p->reg + 1 == p->nreg ? 0 : p->reg + 1;
            }
            p->left--;
        } else {
            // IMAGE: as many qwords as there are, at once
            size_t n = std::min(size_t(p->left), count - i);
            image_data(data + i * 16, n * 16);
            p->left -= uint32_t(n);
            i += n - 1;
        }
        if (p->left == 0 && p->eop) {
            p->in_packet = 0;
        }
    }
}

// ---- Scanout ------------------------------------------------------------------------------------
// A rectangle of a buffer as RGBA: from its target if it is one, else from the CPU's VRAM
// (scaled: as many pixels as the target has, its scale across and down returned in got_scale[0] and
// [1]; else one a GS pixel)
void read_buffer(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 uint32_t *out, bool scaled = false, uint32_t *got_scale = nullptr) {
    Target *t = find_target(bp, bw, psm);
    if (got_scale) {
        got_scale[0] = got_scale[1] = 1;
    }
    if (t && x + w <= t->width && y + h <= t->height) {
        msaa_settle(*t);
        uint32_t sx = t->sx, sy = t->sy;
        const uint8_t *pixels = read_image(*t->image, x * sx, y * sy, w * sx, h * sy);
        if (scaled) {
            for (size_t i = 0; i < size_t(w) * h * sx * sy; i++) {
                uint32_t v;
                memcpy(&v, pixels + i * 4, 4);
                out[i] = v | 0xFF000000u;
            }
            if (got_scale) {
                got_scale[0] = sx;
                got_scale[1] = sy;
            }
        } else {
            for (uint32_t j = 0; j < h; j++) {
                for (uint32_t i = 0; i < w; i++) {
                    uint32_t v;
                    memcpy(&v, pixels + (size_t(j * sy + sy / 2) * w * sx + i * sx + sx / 2) * 4, 4);
                    out[size_t(j) * w + i] = v | 0xFF000000u;
                }
            }
        }
        done_reading();
        return;
    }
    uint32_t lo, count;
    buffer_pages(bp, bw, psm, x + w, y + h, &lo, &count);
    claim_pages(lo, count, nullptr, false);
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            uint32_t v = vram_to_target_pixel(gsn_vram_read(gs->vram.data(), psm, bp, bw, x + i, y + j), psm);
            out[size_t(j) * w + i] = v | 0xFF000000u;
        }
    }
}

bool display_rect(const uint64_t *privileged, GsnRect *r) {
    // a register every 16 bytes: PMODE, SMODE1, SMODE2, ..., DISPFB1, DISPLAY1, DISPFB2, DISPLAY2
    uint64_t pmode = privileged[0], smode2 = privileged[2 * 2];
    int circuit = (pmode & 2) ? 2 : (pmode & 1) ? 1 : 0;
    if (!circuit) {
        return false;
    }
    uint64_t dispfb = privileged[(circuit == 2 ? 9 : 7) * 2], display = privileged[(circuit == 2 ? 10 : 8) * 2];
    r->bp = bits(dispfb, 0, 9) * 32;
    r->bw = bits(dispfb, 9, 6);
    r->psm = bits(dispfb, 15, 5);
    r->x = bits(dispfb, 32, 11);
    r->y = bits(dispfb, 43, 11);
    r->w = (bits(display, 32, 12) + 1) / (bits(display, 23, 4) + 1);
    r->h = bits(display, 44, 11) + 1;
    if ((smode2 & 3) == 3) {
        r->h /= 2; // field mode: a field's picture
    }
    return r->w > 0 && r->h > 0 && r->w <= 1024 && r->h <= 1024;
}

void make_program() {
    ResourceLayout vert = {};
    vert.input_mask = 0x1F;
    vert.push_constant_size = sizeof(Push);
    ResourceLayout frag = {};
    frag.output_mask = 1;
    frag.push_constant_size = sizeof(Push);
    frag.sets[0].sampled_image_mask = (1u << 0) | (1u << 1) | (1u << 3);
    frag.sets[0].storage_buffer_mask = 1u << 2; // (the CLUT: gs_frag.h)
    frag.sets[0].fp_mask = (1u << 1) | (1u << 3);
    for (int b : {0, 1, 2, 3}) {
        frag.sets[0].meta[b].array_size = 1;
    }
    Shader *vs = gs->device->request_shader(gsn_spv_gs_vert, sizeof(gsn_spv_gs_vert), &vert);
    Shader *fs = gs->device->request_shader(gsn_spv_gs_frag, sizeof(gsn_spv_gs_frag), &frag);
    gs->program = gs->device->request_program(vs, fs);
    Shader *fs_zclamp = gs->device->request_shader(gsn_spv_gs_zclamp_frag, sizeof(gsn_spv_gs_zclamp_frag), &frag);
    gs->program_zclamp = gs->device->request_program(vs, fs_zclamp);
    if (sample_shading) {
        Shader *fs_sample = gs->device->request_shader(gsn_spv_gs_zclamp_sample_frag, sizeof(gsn_spv_gs_zclamp_sample_frag), &frag);
        gs->program_zclamp_sample = gs->device->request_program(vs, fs_sample);
    }
    ResourceLayout zonly = {};
    zonly.push_constant_size = sizeof(Push);
    gs->program_zonly = gs->device->request_program(
        vs, gs->device->request_shader(gsn_spv_gs_zonly_frag, sizeof(gsn_spv_gs_zonly_frag), &zonly));
    if (sample_shading) {
        gs->program_zonly_sample = gs->device->request_program(
            vs, gs->device->request_shader(gsn_spv_gs_zonly_sample_frag, sizeof(gsn_spv_gs_zonly_sample_frag), &zonly));
    }
    // --frame-rate's pictures between on the GPU: the picture's buffer, the frame's kicks, their moves, the
    // kicks moved, the corners, the vertices made of them (set 1)
    ResourceLayout replay = {};
    replay.sets[1].storage_buffer_mask = 0x3F;
    for (int b = 0; b < 6; b++) {
        replay.sets[1].meta[b].array_size = 1;
    }
    gs->program_replay_kicks = gs->device->request_program(
        gs->device->request_shader(gsn_spv_gs_replay_comp, sizeof(gsn_spv_gs_replay_comp), &replay));
    gs->program_replay_corners = gs->device->request_program(
        gs->device->request_shader(gsn_spv_gs_replay_corners_comp, sizeof(gsn_spv_gs_replay_corners_comp), &replay));

    // shader blending in the render pass: the destination from an input attachment (binding 5 in a
    // single-sampled target's pass, 4 in a multisampled one's: scratch_subpass) in place of binding 3
    for (int ms = 0; ms < 2; ms++) {
        ResourceLayout in = frag;
        in.sets[0].sampled_image_mask &= ~(1u << 3);
        in.sets[0].fp_mask &= ~(1u << 3);
        uint32_t b = ms ? 4 : 5;
        in.sets[0].input_attachment_mask = 1u << b;
        in.sets[0].fp_mask |= 1u << b;
        in.sets[0].meta[b].array_size = 1;
        if (!ms) {
            gs->program_in = gs->device->request_program(vs, gs->device->request_shader(gsn_spv_gs_in_frag, sizeof(gsn_spv_gs_in_frag), &in));
            gs->program_zclamp_in = gs->device->request_program(
                vs, gs->device->request_shader(gsn_spv_gs_zclamp_in_frag, sizeof(gsn_spv_gs_zclamp_in_frag), &in));
        } else {
            gs->program_in_ms = gs->device->request_program(
                vs, gs->device->request_shader(gsn_spv_gs_in_ms_frag, sizeof(gsn_spv_gs_in_ms_frag), &in));
            gs->program_zclamp_in_ms = gs->device->request_program(
                vs, gs->device->request_shader(gsn_spv_gs_zclamp_in_ms_frag, sizeof(gsn_spv_gs_zclamp_in_ms_frag), &in));
            if (sample_shading) {
                gs->program_zclamp_sample_in_ms = gs->device->request_program(
                    vs, gs->device->request_shader(gsn_spv_gs_zclamp_sample_in_ms_frag, sizeof(gsn_spv_gs_zclamp_sample_in_ms_frag), &in));
            }
        }
    }

    // --native-msaa's copying passes: a texture in, a colour or a depth out
    ResourceLayout copy_vert = {};
    ResourceLayout copy_frag = {};
    copy_frag.sets[0].sampled_image_mask = 1u << 0;
    copy_frag.sets[0].fp_mask = 1u << 0;
    copy_frag.sets[0].meta[0].array_size = 1;
    ResourceLayout copy_colour = copy_frag;
    copy_colour.output_mask = 1;
    Shader *copy_vs = gs->device->request_shader(gsn_spv_msaa_vert, sizeof(gsn_spv_msaa_vert), &copy_vert);
    gs->program_ms_load = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_msaa_load_frag, sizeof(gsn_spv_msaa_load_frag), &copy_colour));
    gs->program_ms_load_depth = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_msaa_load_depth_frag, sizeof(gsn_spv_msaa_load_depth_frag), &copy_frag));
    gs->program_ms_depth = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_msaa_depth_frag, sizeof(gsn_spv_msaa_depth_frag), &copy_frag));

    // shader blending in the render pass: the target into the scratch (an input attachment in, colour
    // attachment 1 out)
    ResourceLayout pass_copy = {};
    pass_copy.output_mask = 2;
    pass_copy.sets[0].input_attachment_mask = 1u << 4;
    pass_copy.sets[0].fp_mask = 1u << 4;
    pass_copy.sets[0].meta[4].array_size = 1;
    gs->program_pass_copy = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_pass_copy_frag, sizeof(gsn_spv_pass_copy_frag), &pass_copy));
    ResourceLayout pass_resolve = {};
    pass_resolve.output_mask = 2;
    pass_resolve.push_constant_size = 2 * sizeof(int32_t);
    pass_resolve.sets[0].input_attachment_mask = 1u << 5;
    pass_resolve.sets[0].fp_mask = 1u << 5;
    pass_resolve.sets[0].meta[5].array_size = 1;
    gs->program_pass_resolve = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_pass_resolve_frag, sizeof(gsn_spv_pass_resolve_frag), &pass_resolve));

    // the scanout's deinterlace: four images in, the picture out
    ResourceLayout deinterlace = {};
    deinterlace.output_mask = 1;
    deinterlace.push_constant_size = 3 * sizeof(int32_t);
    deinterlace.sets[0].sampled_image_mask = 0xF;
    deinterlace.sets[0].fp_mask = 0xF;
    for (int b = 0; b < 4; b++) {
        deinterlace.sets[0].meta[b].array_size = 1;
    }
    gs->program_deinterlace = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_deinterlace_frag, sizeof(gsn_spv_deinterlace_frag), &deinterlace));

    // --native-colour high's scanout: a 16-bit target in, an 8-bit picture out
    ResourceLayout narrowing = copy_colour;
    narrowing.push_constant_size = 8 * sizeof(int32_t);
    gs->program_narrow = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_narrow_frag, sizeof(gsn_spv_narrow_frag), &narrowing));

    // the presenter's scaling to the window, and its sharpening (present_scale)
    ResourceLayout scaling = copy_colour;
    scaling.push_constant_size = 5 * sizeof(int32_t);
    gs->program_present_scale = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_present_scale_frag, sizeof(gsn_spv_present_scale_frag), &scaling));
    ResourceLayout sharpening = copy_colour;
    sharpening.push_constant_size = 3 * sizeof(int32_t);
    gs->program_present_sharpen = gs->device->request_program(
        copy_vs, gs->device->request_shader(gsn_spv_present_sharpen_frag, sizeof(gsn_spv_present_sharpen_frag), &sharpening));
}

void reset_gs() {
    memset(gs->reg, 0, sizeof(gs->reg));
    gs->internal_q = 0x3F800000u;
    memset(gs->clut, 0, sizeof(gs->clut));
    gs->clut_cbp[0] = gs->clut_cbp[1] = UINT32_MAX;
    gs->queue_count = 0;
    memset(gs->paths, 0, sizeof(gs->paths));
    memset(&gs->upload, 0, sizeof(gs->upload));
    gs->transfer_dir = 3;
    gs->field_scan_vblanks = 0;
}

// ---- The scanout as an image (gsn_scanout_take) ---------------------------------------------------
// With a presenter on this device (port/platform/present_vk.cpp) the buffer the display shows is
// copied on the GPU into an image of its own, which the presenter draws from: no read-back. In field
// mode the image is the woven picture, each field's lines copied into its own rows (line l of the
// picture is field (l & 1)'s line l / 2), so the field before stays in the other rows.
// The images are a ring of slots: with --frame-rate above 60 a vblank has several pictures (gsn_picture),
// which the presenter shows at their own times, so the newest must not write over those still waiting.
// The slot the presenter holds (gsn_scanout_take) is passed over.
const int SCANOUT_SLOTS = 16;
struct ScanoutSlot {
    ImageHandle image;
    uint32_t width = 0, height = 0;
    ImageHandle scaled;              // the picture at the window's size (present_scale), or none
    uint32_t scaled_width = 0, scaled_height = 0;
    ImageHandle scaled_work;         // (the scaling's, before the sharpening)
    uint64_t serial = 0; // 0: none published in it (or it is being written)
    uint64_t tag = 0;    // the picture's (gsn_picture)
};
struct ScanoutImage {
    std::mutex lock; // (the presenter takes the image on the game's thread)
    ScanoutSlot slots[SCANOUT_SLOTS];
    std::vector<ImageHandle> retired; // images replaced while the presenter had one: dropped once it lets go
    int newest = -1;  // the slot published last (by the back end's thread, once the picture's calls are submitted)
    int writing = -1; // the slot written for the picture under way, not yet published (the GS's thread)
    int written = -1; // the slot written last (the GS's thread: published, or to be, in that order)
    int taken = -1;   // the slot the presenter holds
    uint64_t serial = 0;
};
ScanoutImage scanout_image;
bool present_wanted; // gsn_enable_present

// The surface extensions the Vulkan loader has (VK_KHR_surface and each platform's: SDL picks one;
// VK_EXT_headless_surface for SDL's offscreen driver)
void present_extensions(std::vector<const char *> &instance_ext, std::vector<const char *> &device_ext) {
    static const char *const surfaces[] = {"VK_KHR_win32_surface", "VK_KHR_xlib_surface", "VK_KHR_xcb_surface",
                                           "VK_KHR_wayland_surface", "VK_EXT_metal_surface",
                                           "VK_EXT_headless_surface"};
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return;
    }
    std::vector<VkExtensionProperties> have(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, have.data());
    auto has = [&](const char *name) {
        for (auto &e : have) {
            if (strcmp(e.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };
    if (!has(VK_KHR_SURFACE_EXTENSION_NAME)) {
        return;
    }
    for (const char *name : surfaces) {
        if (has(name)) {
            instance_ext.push_back(name);
        }
    }
    if (instance_ext.empty()) {
        return;
    }
    instance_ext.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    device_ext.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}

// gsn_vsync's want_pixels as vsync_now has it: the PC's switches (port_options) added
enum : int { VSYNC_PROGRESSIVE = 1 << 8, VSYNC_DEINTERLACE = 1 << 9, VSYNC_FINE_WEAVE = 1 << 10 };

// How the displayed buffer becomes the picture ("The scanout", at vsync_now)
enum ScanMode : uint32_t {
    SCAN_WHOLE,       // every line, as it is (progressive frame mode, a field picture alone)
    SCAN_WEAVE,       // field mode: this field's picture into its own lines of a picture twice as tall
    SCAN_FIELD,       // frame mode read a field at a time, without --deinterlace: this field's lines alone
    SCAN_DEINTERLACE, // the same with --deinterlace: deinterlace.frag over the buffer's last four vblanks
};

// The displayed buffer: where it is, and its target if it has one (and holds the whole of it)
struct Shown {
    uint32_t fbp, fbw, psm, dbx, dby, w, h;
    Target *t;
    uint32_t sx, sy; // its target's scale (or the targets' when it has none)
};

Shown shown_buffer(uint32_t fbp, uint32_t fbw, uint32_t psm, uint32_t dbx, uint32_t dby, uint32_t w, uint32_t h) {
    Shown s = {fbp, fbw, psm, dbx, dby, w, h, find_target(fbp, fbw, psm), target_sx, target_sy};
    if (s.t && (s.t->depth || dbx + w > s.t->width || dby + h > s.t->height)) {
        s.t = nullptr;
    }
    if (s.t) {
        s.sx = s.t->sx;
        s.sy = s.t->sy;
    }
    return s;
}

// shaders/narrow.frag over dst from s's target: the image's line dst_first + dst_step * m (sy rows of
// pixels each) is the target's line first + step * m, for m below count (sy 1: rows of pixels)
void narrow_pass(const Image &dst, const Shown &s, uint32_t first, uint32_t step, uint32_t count, uint32_t dst_first,
                 uint32_t dst_step, uint32_t sy) {
    {
        msaa_settle(*s.t);
        end_pass();
        RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &dst.get_view();
        rp.load_attachments = 1;
        rp.store_attachments = 1;
        Cmd &c = cmd();
        c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        c.begin_render_pass(rp);
        c.set_opaque_state();
        c.set_program(gs->program_narrow);
        c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        c.set_cull_mode(VK_CULL_MODE_NONE);
        c.set_depth_test(false, false);
        c.set_texture(0, 0, s.t->image->get_view(), StockSampler::NearestClamp);
        int32_t push[8] = {int32_t(s.dbx * s.sx), int32_t(s.dby * s.sy), int32_t(first), int32_t(step),
                           int32_t(dst_first), int32_t(dst_step), int32_t(count), int32_t(sy)};
        c.push_constants(push, 0, sizeof(push));
        c.draw(3);
        c.end_render_pass();
        c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
}

// count of the buffer's lines, from line `first` every `step`, into dst: the m-th into the band of sy
// rows of its line dst_first + dst_step * m. From the target on the GPU, or else from the CPU's VRAM
// (each GS pixel repeated sx times across and sy down).
void copy_lines(const Image &dst, const Shown &s, uint32_t first, uint32_t step, uint32_t count, uint32_t dst_first,
                uint32_t dst_step) {
    const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    uint32_t iw = s.w * s.sx, sy = s.sy;
    if (s.t && s.t->image->get_format() != dst.get_format()) {
        // --native-colour high: the 16-bit target dithered into the 8-bit image (shaders/narrow.frag)
        narrow_pass(dst, s, first, step, count, dst_first, dst_step, sy);
        return;
    }
    if (s.t) {
        msaa_settle(*s.t);
        end_pass();
        transfer_barrier();
        if (step == 1 && dst_step == 1) {
            cmd().copy_image(dst, *s.t->image, {0, int32_t(dst_first * sy), 0},
                             {int32_t(s.dbx * s.sx), int32_t((s.dby + first) * sy), 0}, {iw, count * sy, 1}, colour,
                             colour);
        } else {
            for (uint32_t m = 0; m < count; m++) {
                cmd().copy_image(dst, *s.t->image, {0, int32_t((dst_first + dst_step * m) * sy), 0},
                                 {int32_t(s.dbx * s.sx), int32_t((s.dby + first + step * m) * sy), 0}, {iw, sy, 1},
                                 colour, colour);
            }
        }
    } else {
        std::vector<uint32_t> small(size_t(s.w) * s.h);
        read_buffer(s.fbp, s.fbw, s.psm, s.dbx, s.dby, s.w, s.h, small.data());
        end_pass();
        transfer_barrier();
        for (uint32_t m = 0; m < count; m++) {
            uint32_t line = first + step * m;
            uint32_t *out = static_cast<uint32_t *>(cmd().update_image(
                dst, {0, int32_t((dst_first + dst_step * m) * sy), 0}, {iw, sy, 1}, iw, sy, colour));
            for (uint32_t y = 0; y < sy; y++) {
                for (uint32_t x = 0; x < iw; x++) {
                    out[size_t(y) * iw + x] = small[size_t(line) * s.w + x / s.sx];
                }
            }
        }
    }
    transfer_barrier();
}

// The picture of a field-mode screen at a scale (whole frames: the two fields are the same moment,
// port/platform/field_frames.c): field 0's rows, each twice (its rows are 2 sy a line of the picture),
// whichever field the vblank is (a field 1 vblank reads the other buffer, which has field 0 of the same
// moment). Woven a line (sy rows) at a time, each band of sy rows held a line drawn over twice the
// height, the other field's band half a line off: at 3x the menus' text in stripes of three rows. Woven
// a row at a time (field p's row r at the picture's row 2 r + p sy), whatever the game draws alike in
// both fields, not moved by the half line (Menu_DrawItemModel's models: field_frames.c), was combed half
// a line apart; and the vblank's own field alone made it jump half a line every vblank at 4x (the shop's
// goods, the Journal's pictures, October 2026). Only from a target in the picture's format; else 0 (a
// line at a time).
Shown field0_buffer; // the buffer the last field 0 was read from (whole frames: this vblank's, in the
bool field0_set;     // other buffer when the vblank is field 1)

int weave_fine(const Image &dst, const Shown &s, uint32_t p) {
    if (!s.t || s.sy < 2) {
        return 0;
    }
    Shown f = s;
    if (p == 1 && field0_set) {
        // (as it is now: the same buffer, still a target of this size)
        f = shown_buffer(field0_buffer.fbp, field0_buffer.fbw, field0_buffer.psm, field0_buffer.dbx, field0_buffer.dby,
                         field0_buffer.w, field0_buffer.h);
        if (!f.t || f.sx != s.sx || f.sy != s.sy) {
            f = s;
        }
    }
    uint32_t rows = s.h * s.sy;
    narrow_pass(dst, f, 0, 1, rows, 0, 2, 1);
    narrow_pass(dst, f, 0, 1, rows, 1, 2, 1);
    return 1;
}

// The deinterlace's history let go (a vblank not read a field at a time, or a new size)
void forget_history() {
    for (auto &h : gs->history) {
        h.reset();
    }
    gs->history_filled = 0;
}

// This vblank's field (the buffer's lines of parity `parity`, one under the other) into the
// deinterlace's history (SCAN_DEINTERLACE), the newest first: kept at every vblank read a field at a
// time, a picture wanted or not. (The history is fields as paraLLEl-GS keeps them, which a GS capture
// carries: KF4GS_GS_HISTORY.)
void record_history(const Shown &s, int parity) {
    uint32_t iw = s.w * s.sx, ih = s.h / 2 * s.sy;
    if (gs->history_filled && (gs->history[0]->get_width() != iw || gs->history[0]->get_height() != ih)) {
        forget_history();
    }
    ImageHandle oldest = gs->history_filled == 4 ? gs->history[3] : ImageHandle();
    for (int i = 0; i < 3 && oldest; i++) {
        if (gs->history[i] == oldest) {
            oldest.reset(); // (the same image twice, from a capture: not to be written over)
        }
    }
    for (int i = 3; i > 0; i--) {
        gs->history[i] = gs->history[i - 1];
    }
    gs->history[0] = oldest ? oldest : make_image(iw, ih, VK_FORMAT_R8G8B8A8_UNORM, 1, true);
    gs->history_filled = std::min(gs->history_filled + 1, 4u);
    copy_lines(*gs->history[0], s, uint32_t(parity), 2, s.h / 2, 0, 1);
}

// The picture of the deinterlace into dst (as tall as the buffer), from the history: a field before
// that is not there yet is this one (the vblank three before: the one before), so that with none
// before this field's lines are taken twice
void deinterlace_pass(const Image &dst, int parity, uint32_t sy, uint32_t lines) {
    const Image *before[4];
    for (uint32_t i = 0; i < 4; i++) {
        before[i] = i < gs->history_filled ? gs->history[i].get() : before[i == 3 ? 1 : 0];
    }
    end_pass();
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &dst.get_view();
    rp.store_attachments = 1;
    Cmd &c = cmd();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    c.begin_render_pass(rp);
    c.set_opaque_state();
    c.set_program(gs->program_deinterlace);
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_test(false, false);
    for (uint32_t i = 0; i < 4; i++) {
        c.set_texture(0, i, before[i]->get_view(), StockSampler::NearestClamp);
    }
    int32_t push[3] = {parity, int32_t(sy), int32_t(lines)};
    c.push_constants(push, 0, sizeof(push));
    c.draw(3);
    c.end_render_pass();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

// ---- The presenter's scaling ---------------------------------------------------------------------
// The picture scaled to the size the window shows it at (gsn_set_present_scale: the presenter's, or
// --dump-present's), on the GPU after its scanout, with shaders/present_scale.frag (a Catmull-Rom
// cubic, stretched by the ratio when the picture is made smaller, on linear light) and, with a strength,
// shaders/present_sharpen.frag after it; the presenter then copies it into the window a pixel a pixel.
// None when the presenter scales by itself (sharp or smooth, and no sharpening).
std::atomic<uint32_t> present_size{0};    // width << 16 | height, 0: none
std::atomic<uint32_t> present_sharpen{0}; // strength in thousandths
bool scanout_wants_scaled;                // (vsync_now: GSN_WANT_SCALED)
bool scanout_fine_weave;                  // (vsync_now: VSYNC_FINE_WEAVE)
uint64_t present_scale_passes;

void draw_pass(const Image &dst, Program *program, const ImageView &src, const void *push, size_t push_size) {
    end_pass();
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &dst.get_view();
    rp.store_attachments = 1;
    Cmd &c = cmd();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    c.begin_render_pass(rp);
    c.set_opaque_state();
    c.set_program(program);
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_test(false, false);
    c.set_texture(0, 0, src, StockSampler::NearestClamp);
    c.push_constants(push, 0, push_size);
    c.draw(3);
    c.end_render_pass();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

void present_scale(ScanoutSlot &sl, const Image &picture) {
    uint32_t size = present_size.load(std::memory_order_relaxed);
    uint32_t w = size >> 16, h = size & 0xFFFF;
    float sharpen = float(present_sharpen.load(std::memory_order_relaxed)) / 1000.0f;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        sl.scaled.reset();
        sl.scaled_width = sl.scaled_height = 0;
        return;
    }
    if (!sl.scaled || sl.scaled_width != w || sl.scaled_height != h) {
        sl.scaled = make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM, 1, true);
        sl.scaled_work.reset();
        sl.scaled_width = w;
        sl.scaled_height = h;
    }
    if (sharpen > 0 && !sl.scaled_work) {
        sl.scaled_work = make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM, 1, true);
    }
    uint32_t pw = picture.get_width(), ph = picture.get_height();
    struct {
        int32_t src[2];
        float ratio[2];
        int32_t linear;
    } scale = {{int32_t(pw), int32_t(ph)}, {float(pw) / float(w), float(ph) / float(h)}, 1};
    const Image &first = sharpen > 0 ? *sl.scaled_work : *sl.scaled;
    draw_pass(first, gs->program_present_scale, picture.get_view(), &scale, sizeof(scale));
    if (sharpen > 0) {
        struct {
            int32_t size[2];
            float strength;
        } sharp = {{int32_t(w), int32_t(h)}, sharpen};
        draw_pass(*sl.scaled, gs->program_present_sharpen, sl.scaled_work->get_view(), &sharp, sizeof(sharp));
    }
    present_scale_passes++;
    {
        // KF4_PRESENT_BENCH=N: the GPU's time for the scaling (and sharpening), N times over, once
        static int bench = -1;
        if (bench < 0) {
            bench = getenv("KF4_PRESENT_BENCH") ? atoi(getenv("KF4_PRESENT_BENCH")) : 0;
        }
        if (bench > 0 && present_scale_passes == 1) {
            submit(true);
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < bench; i++) {
                draw_pass(first, gs->program_present_scale, picture.get_view(), &scale, sizeof(scale));
            }
            submit(true);
            auto t1 = std::chrono::steady_clock::now();
            if (sharpen > 0) {
                struct {
                    int32_t size[2];
                    float strength;
                } sharp = {{int32_t(w), int32_t(h)}, sharpen};
                for (int i = 0; i < bench; i++) {
                    draw_pass(*sl.scaled, gs->program_present_sharpen, sl.scaled_work->get_view(), &sharp, sizeof(sharp));
                }
            }
            submit(true);
            auto t2 = std::chrono::steady_clock::now();
            fprintf(stderr, "[gsn] present scaling %ux%u -> %ux%u: %.3f ms a picture, sharpening %.3f ms\n", pw, ph, w, h,
                    std::chrono::duration<double, std::milli>(t1 - t0).count() / bench,
                    std::chrono::duration<double, std::milli>(t2 - t1).count() / bench);
        }
    }
}

bool sa_composite_size(const Shown &s, uint32_t *iw, uint32_t *ih); // (standalone T2: gsn_composite.inc)
void sa_composite(const Image &dst, const Shown &s);

// The displayed buffer into scanout_image as `mode` says (parity: the lines of this vblank's field; in
// field mode the buffer is the field's picture, and parity the lines of the woven picture it goes
// into); with frame, read back into it too. Returns GSN_WANT_IMAGE, and GSN_WANT_PIXELS when frame has
// the picture. (SCAN_DEINTERLACE draws from the history: record_history first.)
int scanout_to_image(const Shown &s, ScanMode mode, int parity, PortGsFrame *frame) {
    ScanoutImage &so = scanout_image;
    uint32_t lines = mode == SCAN_WEAVE ? s.h * 2 : mode == SCAN_FIELD ? s.h / 2 : s.h;
    uint32_t iw = s.w * s.sx, ih = lines * s.sy;
    bool composite = mode == SCAN_WHOLE && sa_composite_size(s, &iw, &ih); // (standalone T2: the scene's own scale)
    bool fresh = false;
    int slot = 0;
    ImageHandle before; // (the weave: the picture published last, whose other field's lines are kept)
    // (images are made and let go outside the lock: Granite's device lock, which the presenter holds
    // while it releases the image (gsn_queue_lock, then gsn_scanout_release), comes before it)
    {
        std::lock_guard<std::mutex> hold(so.lock);
        // (after the slot written last, not the one published last: the back end may not have published it yet)
        slot = (so.written + 1) % SCANOUT_SLOTS;
        if (slot == so.taken) {
            slot = (slot + 1) % SCANOUT_SLOTS;
        }
        so.writing = slot;
        so.slots[slot].serial = 0; // (not to be taken while it is written)
        fresh = !so.slots[slot].image || so.slots[slot].width != iw || so.slots[slot].height != ih;
        if (so.written >= 0 && so.slots[so.written].image && so.slots[so.written].width == iw &&
            so.slots[so.written].height == ih) {
            before = so.slots[so.written].image;
        }
        so.written = slot;
    }
    ImageHandle made = fresh ? make_image(iw, ih, VK_FORMAT_R8G8B8A8_UNORM, 1, true) : ImageHandle();
    std::vector<ImageHandle> let_go;
    {
        std::lock_guard<std::mutex> hold(so.lock);
        if (so.taken < 0) {
            let_go.swap(so.retired);
        }
        if (fresh) {
            ScanoutSlot &sl = so.slots[slot];
            if (sl.image) {
                let_go.push_back(sl.image); // (never the slot taken: passed over above)
            }
            sl.image = made;
            sl.width = iw;
            sl.height = ih;
        }
    }
    let_go.clear();
    const Image &dst = *so.slots[slot].image;
    switch (mode) {
    case SCAN_WHOLE:
        if (composite) {
            sa_composite(dst, s); // (standalone T2)
        } else {
            copy_lines(dst, s, 0, 1, s.h, 0, 1);
        }
        break;
    case SCAN_WEAVE:
        // the field into its own lines, the other field's from the picture before; into both when there
        // is none (as the read-back path starts the other field as this one)
        if (before && before.get() != &dst) {
            const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            end_pass();
            transfer_barrier();
            cmd().copy_image(dst, *before, {0, 0, 0}, {0, 0, 0}, {iw, ih, 1}, colour, colour);
            transfer_barrier();
        }
        for (uint32_t p = before ? uint32_t(parity) : 0; p <= (before ? uint32_t(parity) : 1u); p++) {
            if (!(scanout_fine_weave && weave_fine(dst, s, p))) {
                copy_lines(dst, s, 0, 1, s.h, p, 2);
            }
        }
        break;
    case SCAN_FIELD:
        copy_lines(dst, s, uint32_t(parity), 2, lines, 0, 1);
        break;
    case SCAN_DEINTERLACE:
        deinterlace_pass(dst, parity, s.sy, s.h);
        break;
    }
    int ok = GSN_WANT_IMAGE;
    present_scale(so.slots[slot], dst);
    const Image *readout = &dst;
    if (scanout_wants_scaled && so.slots[slot].scaled) {
        // (--dump-present: the picture as the window shows it)
        readout = so.slots[slot].scaled.get();
        iw = so.slots[slot].scaled_width;
        ih = so.slots[slot].scaled_height;
    }
    if (frame && size_t(iw) * ih <= PORT_GS_FRAME_PIXELS) {
        const uint8_t *pixels = read_image(*readout, 0, 0, iw, ih);
        for (size_t i = 0; i < size_t(iw) * ih; i++) {
            uint32_t v;
            memcpy(&v, pixels + i * 4, 4);
            frame->rgba[i] = v | 0xFF000000u;
        }
        done_reading();
        frame->width = iw;
        frame->height = ih;
        ok |= GSN_WANT_PIXELS;
    }
    return ok;
}

// After the picture's commands went to the queue: the presenter may show it (the back end's thread: OP_PUBLISH)
void scanout_published(int slot, uint64_t tag) {
    ScanoutImage &so = scanout_image;
    std::lock_guard<std::mutex> hold(so.lock);
    so.slots[slot].serial = ++so.serial;
    so.slots[slot].tag = tag;
    so.newest = slot;
}

// The picture written under way, to be published once its calls are submitted (the GS's thread)
void scanout_publish(uint64_t tag) {
    ScanoutImage &so = scanout_image;
    int slot;
    {
        std::lock_guard<std::mutex> hold(so.lock);
        slot = so.writing;
        so.writing = -1;
    }
    if (slot >= 0) {
        backend_stream().add(OP_PUBLISH, ArgPublish{slot, tag});
    }
}

// ---- Pictures between on the GPU ----------------------------------------------------------------
// --frame-rate above 30 draws pictures between the game's frames (port/platform/frame_interp.c): the
// frame's packets once more for each, every vertex moved to the picture's time. Walking the packets and
// moving the vertices on the CPU, then working through them here, cost about a game frame's CPU work a
// picture. Instead, for each frame:
// - frame_interp.c walks the packets once and hands them over (gsn_interp_frame) with, for each vertex
//   kick in them, how a picture between moves it (GsnInterpMove: with the camera, with its model, a 2D
//   item's place, colour and texture between the two frames', or not at all).
// - The first picture between (gsn_interp_picture) draws the packets as the pictures between always
//   were drawn, but each kick is recorded as the GS queued it (InterpKick), each corner of each triangle
//   as the kicks it is made of (interp.vsrc, flush_batch), and the triangles are drawn by
//   shaders/gs_replay.vert, which moves the kicks to the picture's time as frame_interp.c moved them on
//   the CPU, from a buffer of the picture's (its cameras, the model parts that move with their model at
//   that time). Every call into the command buffer is recorded too (Cmd, Recording).
// - The frame's next pictures between make the recorded calls again (replay_ops) with only the
//   picture's buffer new: no packets, no GS state worked out, no uploads asked for again.
// The recorded calls are only right if the GS is as it was before the recording (its targets, what of
// theirs is newer, the render pass, its registers, VRAM). When it is not, or something recorded cannot
// be made again (a read-back), the picture is drawn from the packets again and recorded anew.

// The picture's buffer (gs_replay.vert's Picture): its header, then the runs' bits
struct InterpPictureHeader {
    float move[12], mid[12];
    float screen[2][4]; // sx sy cx cy: the world's, the reflection's
    float zt[4];        // cz (the world's, the reflection's), t, 1 - t
    uint32_t kicks, runs;
    float jitter[2];    // (upscaling) the picture's sub-pixel jitter in NDC (sa_jitter_picture; sa_*_jitter.vert): 0 without
};
static_assert(sizeof(InterpPictureHeader) == 160, "InterpPictureHeader is gs_replay.vert's Picture");

// A picture as it comes through the ring: the frame's serial, the header, then the bits
struct InterpPictureMessage {
    uint64_t serial;
    uint64_t pad;
    InterpPictureHeader header;
};

// The recording emptied for the next, or a new one while the back end may still make the last one's calls again
void recording_fresh() {
    if (recording_ops.use_count() > 1) {
        recording_ops = std::make_shared<Recording>();
    } else {
        recording_ops->clear();
    }
}

void interp_free_frame() {
    if (interp.frame) {
        free(interp.frame->moves);
        free(interp.frame->stream);
        free(interp.frame);
        interp.frame = nullptr;
    }
}

// Nothing kept of the frame (a state loaded, the GS's VRAM replaced)
void interp_forget() {
    interp_free_frame();
    interp.moves.reset();
    interp.kicks.reset();
    interp.corners.reset();
    interp.resolved.reset();
    interp.vertices.reset();
    interp.picture.reset();
    interp.recorded = interp.tried = false;
    recording_fresh();
}

// The buffers the kicks' pass reads for the picture under way (none when the frame has no kicks), held by the stream
ArgKicks interp_kicks_now(Recording &s) {
    ArgKicks a = {};
    if (!interp.frame || !interp.frame->kicks) {
        return a;
    }
    a.count = interp.frame->kicks;
    if (interp.picture && interp.kicks && interp.moves && interp.resolved && interp.corners && interp.vertices) {
        for (const BufferHandle *h : {&interp.picture, &interp.kicks, &interp.moves, &interp.resolved, &interp.corners,
                                      &interp.vertices}) {
            s.hold(**h);
        }
        a.picture = interp.picture.get();
        a.kicks = interp.kicks.get();
        a.moves = interp.moves.get();
        a.resolved = interp.resolved.get();
        a.corners = interp.corners.get();
        a.vertices = interp.vertices.get();
    }
    return a;
}

// The frame's kicks at the picture's time (gs_replay.comp), for its draws after (the back end's thread)
void interp_kicks_pass(CommandBuffer &c, const ArgKicks &a) {
    if (!a.count) {
        return;
    }
    auto bind = [&] {
        if (a.picture) {
            c.set_storage_buffer(1, 0, *a.picture);
            c.set_storage_buffer(1, 1, *a.kicks);
            c.set_storage_buffer(1, 2, *a.moves);
            c.set_storage_buffer(1, 3, *a.resolved);
            c.set_storage_buffer(1, 4, *a.corners);
            c.set_storage_buffer(1, 5, *a.vertices);
        }
    };
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    c.set_program(gs->program_replay_kicks);
    bind();
    c.dispatch((a.count + 63) / 64, 1, 1);
    c.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    // (as many corners as the frame's kicks can make: the shader stops at the count the recording wrote)
    c.set_program(gs->program_replay_corners);
    bind();
    c.dispatch((a.count * 3 + 5 + 63) / 64, 1, 1);
    c.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
}

BufferHandle interp_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *initial) {
    BufferCreateInfo info = {};
    info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
    info.size = std::max<VkDeviceSize>(size, 16);
    info.usage = usage;
    return gs->device->create_buffer(info, initial);
}

void interp_capture(InterpState &s) {
    s.targets.clear();
    for (auto &t : gs->targets) {
        s.targets.push_back({t.get(), t->bp, t->bw, psm_layout(t->psm), t->depth, bool(t->ms), t->width, t->height, t->sx,
                             t->sy, t->dirty, t->stale, t->ms_newer, t->image_newer, t->dirty_pages, t->stale_pages});
    }
    // (by what the targets are, not which: gs->targets is reordered as targets come and go, and a target
    // drawn over after an upload is made anew each picture)
    std::sort(s.targets.begin(), s.targets.end(), [](const InterpTargetState &a, const InterpTargetState &b) {
        return std::make_tuple(a.bp, a.bw, a.psm, a.depth) < std::make_tuple(b.bp, b.bw, b.psm, b.depth);
    });
    s.in_pass = gs->in_pass;
    s.pass_ms = gs->pass_ms;
    s.pass_color = gs->pass_color;
    s.pass_depth = gs->pass_depth;
    s.vram_writes = gs->vram_writes;
    s.clut_version = gs->clut_version;
}

bool interp_same(const InterpState &a, const InterpState &b) {
    if (a.targets.size() != b.targets.size() || a.in_pass != b.in_pass || a.pass_ms != b.pass_ms ||
        a.pass_color != b.pass_color || a.pass_depth != b.pass_depth || a.vram_writes != b.vram_writes ||
        a.clut_version != b.clut_version) {
        GSN_LOG(1, "interp: GS differs: targets %zu/%zu pass %d/%d ms %d/%d vram %llu/%llu clut %llu/%llu\n", a.targets.size(),
                b.targets.size(), a.in_pass, b.in_pass, a.pass_ms, b.pass_ms, (unsigned long long)a.vram_writes,
                (unsigned long long)b.vram_writes, (unsigned long long)a.clut_version, (unsigned long long)b.clut_version);
        return false;
    }
    for (size_t i = 0; i < a.targets.size(); i++) {
        const InterpTargetState &x = a.targets[i], &y = b.targets[i];
        if (x.bp != y.bp || x.bw != y.bw || x.psm != y.psm || x.depth != y.depth || x.has_ms != y.has_ms ||
            x.width != y.width || x.height != y.height || x.sx != y.sx || x.sy != y.sy || x.dirty != y.dirty ||
            x.stale != y.stale || x.ms_newer != y.ms_newer || x.image_newer != y.image_newer ||
            x.dirty_pages != y.dirty_pages || x.stale_pages != y.stale_pages) {
            GSN_LOG(1, "interp: target %zu (bp %#x) differs: same %d image %d ms %d size %d dirty %d/%d stale %d/%d "
                       "ms_newer %d/%d image_newer %d/%d pages %d %d\n",
                    i, x.bp, x.bp == y.bp && x.bw == y.bw && x.psm == y.psm && x.depth == y.depth, 1, x.has_ms == y.has_ms,
                    x.width == y.width && x.height == y.height,
                    x.dirty, y.dirty, x.stale, y.stale, x.ms_newer, y.ms_newer, x.image_newer, y.image_newer,
                    x.dirty_pages == y.dirty_pages, x.stale_pages == y.stale_pages);
            if (log_level >= 2) {
                for (const InterpState *st : {&a, &b}) {
                    for (const InterpTargetState &z : st->targets) {
                        GSN_LOG(2, "  %s target %p bp %#x bw %u psm %#x %ux%u depth %d ms %d\n", st == &a ? "now" : "then",
                                (void *)z.t, z.bp, z.bw, z.psm, z.width, z.height, z.depth, z.has_ms);
                    }
                }
            }
            return false;
        }
    }
    return true;
}

// The targets' flags and the render pass as the recording left them (its calls made again: the targets
// are the same, interp_same)
void interp_restore(const InterpState &s) {
    for (const InterpTargetState &x : s.targets) {
        Target *found = nullptr;
        for (auto &t : gs->targets) {
            if (t->bp == x.bp && t->bw == x.bw && psm_layout(t->psm) == x.psm && t->depth == x.depth) {
                found = t.get();
            }
        }
        if (!found) {
            continue;
        }
        Target &t = *found;
        t.dirty = x.dirty;
        t.stale = x.stale;
        t.ms_newer = x.ms_newer;
        t.image_newer = x.image_newer;
        t.dirty_pages = x.dirty_pages;
        t.stale_pages = x.stale_pages;
        t.used = gs->tick;
    }
    gs->in_pass = s.in_pass;
    gs->pass_ms = s.pass_ms;
    gs->pass_color = s.pass_color;
    gs->pass_depth = s.pass_depth;
    if (s.in_pass) {
        // (the render pass the recording left open is the targets' now)
        for (const InterpTargetState &x : s.targets) {
            for (auto &t : gs->targets) {
                if (t->bp == x.bp && t->bw == x.bw && psm_layout(t->psm) == x.psm && t->depth == x.depth) {
                    if (x.t == s.pass_color) {
                        gs->pass_color = t.get();
                    }
                    if (x.t == s.pass_depth) {
                        gs->pass_depth = t.get();
                    }
                }
            }
        }
    }
}

// The frame (from frame_interp.c): kept, its moves on the GPU
void interp_frame_now(GsnInterpFrame *f) {
    interp_forget();
    interp.frame = f;
    interp.moves = interp_buffer(VkDeviceSize(f->kicks) * sizeof(GsnInterpMove), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 f->kicks ? f->moves : nullptr);
}

void gif(int path, const uint8_t *data, size_t bytes);

// The picture drawn from the frame's packets, and recorded
void interp_record() {
    Timer timer(gsn_stats.ns_interp_record);
    GsnInterpFrame *f = interp.frame;
    flush_batch(); // (nothing of before the picture in its recording)
    interp.kicks = interp_buffer(VkDeviceSize(f->kicks) * sizeof(InterpKick), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr);
    interp.corners = interp_buffer((VkDeviceSize(f->kicks) * 3 + 6) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr);
    if (!interp.resolved) {
        BufferCreateInfo info = {};
        info.domain = BufferDomain::Device;
        info.size = std::max<VkDeviceSize>(VkDeviceSize(f->kicks) * 32, 16);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        interp.resolved = gs->device->create_buffer(info, nullptr);
        info.size = std::max<VkDeviceSize>((VkDeviceSize(f->kicks) * 3 + 5) * sizeof(Vtx), 16);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        interp.vertices = gs->device->create_buffer(info, nullptr);
    }
    interp.kick_map = static_cast<InterpKick *>(gs->device->map_host_buffer(*interp.kicks, MEMORY_ACCESS_WRITE_BIT));
    interp.kick_list.resize(f->kicks);
    interp.corner_map = static_cast<uint32_t(*)[4]>(gs->device->map_host_buffer(*interp.corners, MEMORY_ACCESS_WRITE_BIT));
    end_pass(); // (the recording starts outside a render pass: its kicks moved first)
    interp.recorded_still = interp.still;
    interp_capture(interp.start);
    recording_fresh();
    recording_ops->on = true;
    backend_stream().add(OP_RECORDING_BINDS, ArgValues{{0}, {}}); // (--stats: the pipelines its calls bind)
    cmd().forget_bindings();
    cmd().interp_kicks();
    interp.active = true;
    interp.broken = false;
    interp.kick = interp.corner = 0;
    interp.st_src = interp.q_src = interp.rgba_src = interp.rgba_q_src = interp.uv_src = GSN_IM_NONE;
    interp.qkick[0] = interp.qkick[1] = interp.qkick[2] = GSN_IM_NONE;
    interp.vsrc.clear();
    uint32_t field_scan = gs->field_scan_vblanks;
    for (size_t at = 0; at + 8 <= f->stream_bytes;) {
        uint32_t head[2];
        memcpy(head, f->stream + at, 8);
        Timer gif_timer(gsn_stats.ns_gif);
        gif(int(head[0]), f->stream + at + 8, head[1]);
        at += 8 + head[1];
    }
    flush_batch(); // (the last batch is the picture's)
    backend_stream().add(OP_RECORDING_BINDS, ArgValues{{1}, {}});
    recording_ops->settle();
    recording_ops->on = false;
    cmd().forget_bindings();
    interp.active = false;
    finish_recording(*recording_ops);
    uint32_t count[4] = {interp.corner, 0, 0, 0}; // (the corners' count, before them)
    memcpy(interp.corner_map[0], count, sizeof(count));
    gs->device->unmap_host_buffer(*interp.kicks, MEMORY_ACCESS_WRITE_BIT);
    gs->device->unmap_host_buffer(*interp.corners, MEMORY_ACCESS_WRITE_BIT);
    interp.kick_map = nullptr;
    interp.corner_map = nullptr;
    if (interp.kick != f->kicks || interp.broken) {
        static int told;
        if (!told++) {
            fprintf(stderr, "[gsn] a picture between drawn on the GPU: %u vertex kicks where the frame has %u%s\n",
                    interp.kick, f->kicks, interp.broken ? " (broken)" : "");
        }
        interp.broken = true;
    }
    interp_capture(interp.end);
    interp.end_regs.assign(gs->reg, gs->reg + 256);
    interp.recorded = !interp.broken && recording_ops->ok && gs->field_scan_vblanks == field_scan;
    interp.tried = true;
    gsn_stats.interp_records++;
    GSN_LOG(2, "interp: %llu recorded, %llu batches moving drawn in runs, %zu calls\n",
            (unsigned long long)gsn_stats.interp_records, (unsigned long long)interp.risky_runs, recording_ops->ops.size());
}

// A multisampled target that now has its newer pixels in the other of its two images than before the recording (a
// frame's recording that begins with the frame buffer's multisampled image newer and ends with its 2D drawn into the
// image): both made the same picture (resolved, or loaded from the image), then named newer as before the recording,
// so that the recorded calls find what they found (the pictures the same: checked, every picture after one). Without
// it the frame was recorded again at its next picture, 367 of the first 1,400 frames of tests/newgame.pad (4 ms of
// this thread each). Only when nothing else differs (the calls cannot be made again otherwise).
void interp_settle_newer(InterpState &now) {
    std::vector<std::pair<InterpTargetState *, const InterpTargetState *>> differ;
    InterpState as_before = now;
    for (InterpTargetState &x : as_before.targets) {
        for (const InterpTargetState &s : interp.start.targets) {
            if (std::make_tuple(s.bp, s.bw, s.psm, s.depth) == std::make_tuple(x.bp, x.bw, x.psm, x.depth) && x.has_ms &&
                s.has_ms && (s.ms_newer != x.ms_newer || s.image_newer != x.image_newer)) {
                differ.push_back({&now.targets[size_t(&x - as_before.targets.data())], &s});
                x.ms_newer = s.ms_newer;
                x.image_newer = s.image_newer;
            }
        }
    }
    if (differ.empty() || !interp_same(as_before, interp.start)) {
        return;
    }
    Why why(GSN_WHY_MSAA);
    for (const auto &d : differ) {
        Target &t = *d.first->t;
        if (t.ms_newer) {
            msaa_settle(t);
        }
        if (t.image_newer) {
            msaa_ready(t);
        }
        t.ms_newer = d.first->ms_newer = d.second->ms_newer;
        t.image_newer = d.first->image_newer = d.second->image_newer;
    }
    gsn_stats.interp_newer_settled++;
}

// Why the recording cannot be made again for this picture (GsnStats' interp_rerecord): 0 it can; 1 the GS's targets are
// not as they were before it (their state, the render pass); 2 its registers differ; 3 nothing recorded yet (the frame's
// first picture between); 4 what was recorded cannot be made again (a read-back in it, its kicks miscounted, the field
// scan changed); 5 VRAM or the CLUT written since
int interp_cannot_replay() {
    if (!interp.recorded) {
        return interp.tried ? 4 : 3;
    }
    if (reflection_keep() == 1 && interp.recorded_still != interp.still) {
        return 6; // (the recording left the reflection out, or drew it: sat_kept)
    }
    flush_batch();
    // (the targets as before the recording; VRAM and the CLUT as after it: its uploads and CLUT loads leave
    // them as the frame left them, which is how they were before it too)
    InterpState now;
    interp_capture(now);
    if (now.vram_writes != interp.end.vram_writes || now.clut_version != interp.end.clut_version) {
        GSN_LOG(1, "interp: VRAM or the CLUT written since the recording\n");
        return 5;
    }
    now.vram_writes = interp.start.vram_writes;
    now.clut_version = interp.start.clut_version;
    // A target the recording made that is still there (the frame drawn by the game had none at that
    // buffer, the picture's own is kept: the sky's scrolling copy on map 063, since targets made only by
    // pictures between are dropped at the frame's end): its calls make it again, from VRAM, over the
    // same image. Without it the frame's second picture was drawn from the packets again each frame.
    auto key_of = [](const InterpTargetState &x) { return std::make_tuple(x.bp, x.bw, x.psm, x.depth); };
    auto shape_same = [](const InterpTargetState &x, const InterpTargetState &y) {
        return x.has_ms == y.has_ms && x.width == y.width && x.height == y.height && x.sx == y.sx && x.sy == y.sy;
    };
    size_t had = now.targets.size();
    now.targets.erase(std::remove_if(now.targets.begin(), now.targets.end(), [&](const InterpTargetState &x) {
        for (const InterpTargetState &s : interp.start.targets) {
            if (key_of(s) == key_of(x)) {
                return false;
            }
        }
        for (const InterpTargetState &e : interp.end.targets) {
            if (key_of(e) == key_of(x) && shape_same(e, x)) {
                return true;
            }
        }
        return false;
    }), now.targets.end());
    bool kept = now.targets.size() != had;
    interp_settle_newer(now);
    if (!interp_same(now, interp.start)) {
        return 1;
    }
    if (memcmp(gs->reg, interp.end_regs.data(), sizeof(gs->reg)) != 0) {
        return 2;
    }
    // the targets' images the recording names, as those targets have them now
    replay_images.clear();
    bool own = false;
    for (const Recording::TargetImage &ti : recording_ops->target_images) {
        Target *t = find_target(ti.bp, ti.bw, ti.psm);
        if (!t) {
            // A target the recording made and dropped (a scrolling texture's, made, grown up a page row
            // and dropped each picture: map 063's sky). The GS has the targets it had before the recording
            // (above), so none was this buffer then: the image is the recording's own, made and filled by
            // its calls, and they are made again on it.
            own = true;
            continue;
        }
        const Image *now = ti.ms ? t->ms.get() : t->image.get();
        if (!now || now->get_width() != ti.image->get_width() || now->get_height() != ti.image->get_height() ||
            now->get_format() != ti.image->get_format()) {
            GSN_LOG(1, "interp: target bp %#x%s is not as recorded\n", ti.bp, ti.ms ? " (multisampled)" : "");
            return 1;
        }
        if (now != ti.image) {
            replay_images.push_back({ti.image, now});
        }
    }
    gsn_stats.interp_kept_targets += kept;
    gsn_stats.interp_own_targets += own;
    return 0;
}

void sa_jitter_picture(float *jitter);
void sa_camera_picture(const float *move, const float *jitter_px);

void interp_picture_now(const uint8_t *data, size_t size) {
    InterpPictureMessage m;
    if (size < sizeof(m)) {
        return;
    }
    memcpy(&m, data, sizeof(m));
    if (!interp.frame || interp.frame->serial != m.serial) {
        gsn_stats.interp_misses++;
        return;
    }
    // the picture's buffer: the header (the frame's projections and kicks added), then the runs' bits
    std::vector<uint8_t> buffer(size - offsetof(InterpPictureMessage, header));
    memcpy(buffer.data(), data + offsetof(InterpPictureMessage, header), buffer.size());
    InterpPictureHeader *h = reinterpret_cast<InterpPictureHeader *>(buffer.data());
    for (int i = 0; i < 2; i++) {
        memcpy(h->screen[i], interp.frame->screen[i], 4 * sizeof(float));
        h->zt[i] = interp.frame->screen[i][4];
    }
    h->kicks = interp.frame->kicks;
    float jitter[4]; // (upscaling: this picture's own sub-pixel jitter, gsn_standalone.inc: in NDC, in the scene's pixels)
    sa_jitter_picture(jitter);
    h->jitter[0] = jitter[0];
    h->jitter[1] = jitter[1];
    sa_camera_picture(h->move, jitter + 2); // (upscaling: its camera, for the composite's motion)
    interp.still = interp.frame->still != 0; // (the camera of the frame shown is the frame's, bit for bit)
    interp.picture = interp_buffer(buffer.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, buffer.data());
    flush_batch();
    end_pass(); // (a picture between starts outside a render pass: its kicks moved first)
    int why = interp_cannot_replay();
    if (why == 0) {
        Timer timer(gsn_stats.ns_interp_replay);
        ReplayJob job;
        job.recording = recording_ops;
        job.picture = interp.picture;
        job.kicks = interp.kicks;
        job.moves = interp.moves;
        job.resolved = interp.resolved;
        job.corners = interp.corners;
        job.vertices = interp.vertices;
        job.kick_count = interp.frame->kicks;
        for (const auto &p : replay_images) {
            Image &now = const_cast<Image &>(*p.second);
            now.add_reference();
            job.images.push_back({p.first, ImageHandle(&now)});
        }
        cmd().replay(std::move(job));
        gsn_stats.interp_replay_draws += recording_ops->draws;
        interp_restore(interp.end);
        gsn_stats.draws += recording_ops->draws;
        gsn_stats.interp_replays++;
        return;
    }
    gsn_stats.interp_rerecord[why]++;
    interp_record();
}

// ---- The GS's own thread ------------------------------------------------------------------------
// The native GS works on a thread of its own, as the GS is a chip of its own: what is sent to it (GIF
// packets, vblanks) is copied into a ring, and its thread takes it from there while the thread that
// sent it (the DMA controller's) goes on with VU1's programs and the next packets. Whatever wants
// something back (a read-back, a picture to show, the GS's state) first waits until the ring is
// empty and the GS's thread idle, then does it on its own thread. (KF4_GSN_THREAD=0: no thread, all
// of it done as it is sent.)
enum : uint32_t { RING_GIF, RING_VSYNC, RING_SKIP, RING_INTERP_FRAME, RING_INTERP_PICTURE };

// A record in the ring: this header, then its bytes, padded to 16
struct RingRecord {
    uint32_t type;
    uint32_t size;
    int32_t arg;
    uint32_t want_pixels; // (a vblank's)
};
static_assert(sizeof(RingRecord) == 16, "a ring record's header is a qword");

const size_t ring_bytes = size_t(64) << 20; // (a power of two)
// the display registers gsn_vsync reads: PMODE to DISPLAY2, a register every 16 bytes
const size_t vsync_bytes = 11 * 16;

struct Ring {
    std::vector<uint8_t> bytes;
    std::atomic<uint64_t> head{0}; // bytes put in (the sender's)
    std::atomic<uint64_t> tail{0}; // bytes done (the GS thread's)
    std::atomic<bool> gs_sleeping{false}, sender_waiting{false};
    std::atomic<bool> stop{false}, stopped{false}; // (at exit: ring_stop)
    std::atomic<uint64_t> vsyncs_done{0};          // vblanks the GS's thread has done
    uint64_t vsyncs_sent = 0;                      // (the sender's)
    std::mutex lock;
    std::condition_variable wake; // the GS's thread, when there is something to do
    std::condition_variable done; // the sender, when the GS's thread has done something
};

Ring *ring; // (none: no thread)

int vsync_now(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame, uint64_t tag);

void ring_work(const RingRecord &r, const uint8_t *data) {
    if (r.type == RING_GIF) {
        Timer timer(gsn_stats.ns_gif);
        gif(r.arg, data, r.size);
    } else if (r.type == RING_VSYNC) {
        // (KF4_GSN_SLOW_US=N, debug: this thread N us late at each vblank, so that a window falls behind the game as a
        // busy GPU makes it: only the vblanks' own pictures shown, the way the lamp wall's patch was first caught)
        static int slow_us = getenv("KF4_GSN_SLOW_US") ? atoi(getenv("KF4_GSN_SLOW_US")) : 0;
        if (slow_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(slow_us));
        }
        uint64_t privileged[vsync_bytes / 8], tag;
        memcpy(privileged, data, vsync_bytes);
        memcpy(&tag, data + vsync_bytes, 8);
        vsync_now(privileged, r.arg, int(r.want_pixels), nullptr, tag);
    } else if (r.type == RING_INTERP_FRAME) {
        GsnInterpFrame *f;
        memcpy(&f, data, sizeof(f));
        interp_frame_now(f);
    } else if (r.type == RING_INTERP_PICTURE) {
        interp_picture_now(data, r.size);
    }
}

// The thread's name, for profilers and debuggers (Linux; elsewhere none)
void name_thread(const char *name) {
#ifdef __linux__
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}

// ---- KF4_HITCH_MS=N (measure): a vblank whose work on this thread took over N ms, and what it was ----
std::atomic<uint64_t> hitch_compiles{0}, hitch_compile_ns{0}; // (pipelines made by draws that waited: gsn_pipeline_warm.inc)
struct Hitch {
    int ms = -1;
    std::chrono::steady_clock::time_point t0;
    double type_ms[8] = {};
    uint64_t compile_ns0 = 0, compiles0 = 0;
    GsnStats s0 = {};
    uint64_t vblank = 0;
} hitch;
void hitch_begin() {
    if (hitch.ms < 0) {
        const char *e = getenv("KF4_HITCH_MS");
        hitch.ms = e ? atoi(e) : 0;
    }
    if (hitch.ms) {
        hitch.t0 = std::chrono::steady_clock::now();
    }
}
void hitch_end(uint32_t type, bool vblank) {
    if (!hitch.ms) {
        return;
    }
    hitch.type_ms[type & 7] +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hitch.t0).count();
    if (!vblank) {
        return;
    }
    hitch.vblank++;
    double total = 0;
    for (double v : hitch.type_ms) {
        total += v;
    }
    if (total > hitch.ms) {
        const GsnStats &s = gsn_stats, &o = hitch.s0;
        fprintf(stderr,
                "[hitch] gs vblank %llu: %.1f ms (gif %.1f vsync %.1f interp %.1f+%.1f) compiles %u %.1f ms; decodes %llu "
                "%.1f ms, uploads %llu %.1f ms, nd meshes %llu arenas %llu, targets %llu, flush %.1f ms, sync %.1f ms\n",
                (unsigned long long)hitch.vblank, total, hitch.type_ms[RING_GIF], hitch.type_ms[RING_VSYNC],
                hitch.type_ms[RING_INTERP_FRAME], hitch.type_ms[RING_INTERP_PICTURE], (unsigned)(hitch_compiles - hitch.compiles0),
                double(hitch_compile_ns - hitch.compile_ns0) / 1e6, (unsigned long long)(s.texture_decodes - o.texture_decodes),
                (s.ns_decode - o.ns_decode) / 1e6, (unsigned long long)(s.uploads - o.uploads),
                (s.ns_upload - o.ns_upload) / 1e6, (unsigned long long)(s.nd_mesh_uploads - o.nd_mesh_uploads),
                (unsigned long long)(s.nd_arenas - o.nd_arenas), (unsigned long long)s.targets,
                (s.ns_flush - o.ns_flush) / 1e6, (s.ns_sync - o.ns_sync) / 1e6);
    }
    memset(hitch.type_ms, 0, sizeof(hitch.type_ms));
    hitch.compile_ns0 = hitch_compile_ns;
    hitch.compiles0 = hitch_compiles;
    hitch.s0 = gsn_stats;
}

// ---- The profiler (Shift+P, port/include/port/profile.h): this thread's work by record, and a vblank's work over two
// vblanks' time named as a hitch, with what it did ----
struct ProfileVblank {
    uint64_t t0 = 0, work_ns = 0;
    bool begun = false; // (a vblank's counts below taken: the next vblank's work can be told apart)
    uint64_t compile0 = 0, wait0 = 0, decode0 = 0, upload0 = 0, interp0 = 0;
} profile_gs;
void profile_begin() {
    profile_gs.t0 = port_profile_on() ? port_profile_now_ns() : 0;
}
void profile_end(uint32_t type, bool vblank) {
    ProfileVblank &p = profile_gs;
    if (p.t0 == 0) {
        return;
    }
    uint64_t ns = port_profile_now_ns() - p.t0;
    port_profile_add(PROFILE_GS, ns);
    if (type == RING_INTERP_FRAME || type == RING_INTERP_PICTURE) {
        port_profile_add(PROFILE_GS_INTERP, ns);
    }
    p.work_ns += ns;
    if (!vblank) {
        return;
    }
    ProfileCounters c = port_profile_read();
    if (p.work_ns > 2 * 16683000u && p.begun) {
        char text[PROFILE_HITCH_TEXT];
        snprintf(text, sizeof(text),
                 "GS vblank: %.1f ms (pipelines made %.1f, waits for GPU %.1f, between %.1f, decodes %.1f, uploads %.1f)",
                 double(p.work_ns) / 1e6, double(c.ns[PROFILE_COMPILE] - p.compile0) / 1e6,
                 double(c.ns[PROFILE_GS_WAIT] - p.wait0) / 1e6, double(c.ns[PROFILE_GS_INTERP] - p.interp0) / 1e6,
                 double(gsn_stats.ns_decode - p.decode0) / 1e6, double(gsn_stats.ns_upload - p.upload0) / 1e6);
        port_profile_hitch(text);
    }
    p.work_ns = 0;
    p.begun = true;
    p.compile0 = c.ns[PROFILE_COMPILE];
    p.wait0 = c.ns[PROFILE_GS_WAIT];
    p.interp0 = c.ns[PROFILE_GS_INTERP];
    p.decode0 = gsn_stats.ns_decode;
    p.upload0 = gsn_stats.ns_upload;
}

void ring_thread() {
    Util::register_thread_index(0); // (Granite's command pools: the sender's when it does the work)
    name_thread("kf4-gs");
    Ring &q = *ring;
    for (;;) {
        if (q.stop.load()) {
            // the program is ending: nothing more is done, and nothing of the GS's is touched again
            q.stopped.store(true);
            {
                std::lock_guard<std::mutex> hold(q.lock);
                q.done.notify_all();
            }
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        uint64_t tail = q.tail.load(std::memory_order_relaxed);
        if (q.head.load(std::memory_order_acquire) == tail) {
            // nothing to do: a while spinning (packets come in bursts), then asleep
            auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
            while (q.head.load(std::memory_order_acquire) == tail && std::chrono::steady_clock::now() < until) {
                std::this_thread::yield();
            }
            if (q.head.load(std::memory_order_acquire) == tail) {
                std::unique_lock<std::mutex> hold(q.lock);
                q.gs_sleeping.store(true);
                while (q.head.load() == tail && !q.stop.load()) {
                    q.wake.wait(hold);
                }
                q.gs_sleeping.store(false);
            }
            continue;
        }
        const uint8_t *at = q.bytes.data() + (tail & (ring_bytes - 1));
        RingRecord r;
        memcpy(&r, at, sizeof(r));
        hitch_begin();
        profile_begin();
        ring_work(r, at + sizeof(r));
        profile_end(r.type, r.type == RING_VSYNC && !(r.want_pixels & GSN_EXTRA));
        hitch_end(r.type, r.type == RING_VSYNC && !(r.want_pixels & GSN_EXTRA));
        if (r.type == RING_VSYNC && !(r.want_pixels & GSN_EXTRA)) {
            q.vsyncs_done.fetch_add(1);
        }
        q.tail.store(tail + sizeof(r) + ((size_t(r.size) + 15) & ~size_t(15)));
        if (q.sender_waiting.load()) {
            std::lock_guard<std::mutex> hold(q.lock);
            q.done.notify_all();
        }
    }
}

// The sender: waits until the GS's thread has done all but `room` bytes of the ring's
template <class Done> void sender_wait(Done done) {
    Ring &q = *ring;
    if (done()) {
        return;
    }
    ProfileTimer waiting(PROFILE_RING_WAIT);
    // a short wait spinning, a long one asleep
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
    while (!done() && std::chrono::steady_clock::now() < until) {
        std::this_thread::yield();
    }
    if (!done()) {
        std::unique_lock<std::mutex> hold(q.lock);
        q.sender_waiting.store(true);
        while (!done()) {
            q.done.wait(hold);
        }
        q.sender_waiting.store(false);
    }
}

void ring_wait(size_t room) {
    Ring &q = *ring;
    uint64_t head = q.head.load(std::memory_order_relaxed);
    sender_wait([&] { return head - q.tail.load() <= ring_bytes - room || q.stopped.load(); });
}

// The sender: all that was sent, done (and the calls made by the back end: gsn_backend.inc)
void ring_idle() {
    if (ring) {
        ring_wait(ring_bytes);
    }
    backend_drain();
}

void ring_put(uint32_t type, int32_t arg, const void *data, size_t size, uint32_t want_pixels = 0) {
    Ring &q = *ring;
    size_t padded = (size + 15) & ~size_t(15);
    size_t need = sizeof(RingRecord) + padded;
    uint64_t head = q.head.load(std::memory_order_relaxed);
    size_t at = head & (ring_bytes - 1);
    if (at + need > ring_bytes) {
        // no room before the ring's end: the rest of it skipped
        ring_wait(ring_bytes - at);
        RingRecord skip = {RING_SKIP, uint32_t(ring_bytes - at - sizeof(RingRecord)), 0, 0};
        memcpy(q.bytes.data() + at, &skip, sizeof(skip));
        head += ring_bytes - at;
        q.head.store(head);
        at = 0;
    }
    ring_wait(need);
    RingRecord r = {type, uint32_t(size), arg, want_pixels};
    memcpy(q.bytes.data() + at, &r, sizeof(r));
    memcpy(q.bytes.data() + at + sizeof(r), data, size);
    q.head.store(head + need);
    if (q.gs_sleeping.load()) {
        std::lock_guard<std::mutex> hold(q.lock);
        q.wake.notify_one();
    }
}

// At exit: the GS's thread stops before the GS's objects are destroyed, once it has done what was sent
// (two seconds at most: what is left then stays undone)
void ring_stop() {
    Ring &q = *ring;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (q.tail.load() != q.head.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    q.stop.store(true);
    {
        std::lock_guard<std::mutex> hold(q.lock);
        q.wake.notify_one();
        q.done.notify_all();
    }
    until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!q.stopped.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    backend_stop(); // (then the back end's: what the GS's thread sent last is made)
}

void ring_start() {
    const char *on = getenv("KF4_GSN_THREAD");
    if (on && atoi(on) == 0) {
        return;
    }
    ring = new Ring();
    ring->bytes.resize(ring_bytes);
    std::thread(ring_thread).detach();
    atexit(ring_stop);
}

// ---- The pipeline cache -------------------------------------------------------------------------
// Vulkan pipelines are made as draws first need them (a second or two of them in the first minutes of
// play, when the driver has not kept them itself: a new build, or its own cache full). The pipeline
// cache is kept in the data folder (gsn_pipeline_cache), and written again a while after it grows.
//
// The files (the cache, 3 MB, and the data folder's pipeline list) are taken and written on a thread of their own,
// the cache writer's: taking the cache's bytes from Granite is about 3 ms and writing them 1-2 ms more, which the
// GS's thread once spent two seconds after any pipeline was made. Granite's cache may be read while pipelines are
// made on other threads (the driver's own cache is made for it; Granite's pipeline binaries hold their maps' read
// locks while they are walked: cmake/granite_pipeline_warm.cmake).
std::string cache_path;
size_t cache_saved_size; // (the cache writer's thread; at exit, the exiting one once that has stopped)
uint32_t cache_vsyncs;

// (gsn_pipeline_warm.inc) the data folder's list as text when it has more than was last taken; empty otherwise
std::string pw_list_text();

// the pipeline cache's bytes when it has grown since they were last taken; empty otherwise
std::vector<uint8_t> pipeline_cache_bytes() {
    size_t size = gs->device->get_pipeline_cache_size();
    if (size == 0 || size == cache_saved_size) {
        return {};
    }
    std::vector<uint8_t> data(size);
    if (!gs->device->get_pipeline_cache_data(data.data(), size)) {
        return {}; // (it grew meanwhile: the next look)
    }
    cache_saved_size = size;
    return data;
}

// a whole file written: into a file beside it, then in its place, so never half written
bool write_whole_file(const std::string &path, const void *data, size_t size) {
    std::string folder = path.substr(0, path.find_last_of("/\\"));
#ifdef _WIN32
    _mkdir(folder.c_str());
#else
    mkdir(folder.c_str(), 0755);
#endif
    std::string temp = path + ".new";
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f) {
        return false;
    }
    bool written = fwrite(data, 1, size, f) == size;
    written = fclose(f) == 0 && written;
    if (written) {
        remove(path.c_str());
        written = rename(temp.c_str(), path.c_str()) == 0;
    }
    return written;
}

struct PipelineFiles {
    std::string list_path; // the data folder's list
    std::mutex lock;
    std::condition_variable wake;
    bool wanted = false; // pipelines were made: the files are to be written again (those that changed)
    bool stop = false;
    std::thread thread;
} pipeline_files;

// the cache and the list written, those that changed since they were last
void write_pipeline_files() {
    std::vector<uint8_t> cache = pipeline_cache_bytes();
    if (!cache.empty()) {
        if (write_whole_file(cache_path, cache.data(), cache.size())) {
            GSN_LOG(1, "pipeline cache written: %zu bytes\n", cache.size());
        } else {
            fprintf(stderr, "[gsn] the pipeline cache could not be written to %s\n", cache_path.c_str());
        }
    }
    std::string list = pw_list_text();
    if (!list.empty() && !write_whole_file(pipeline_files.list_path, list.data(), list.size())) {
        fprintf(stderr, "[gsn] the pipeline list could not be written to %s\n", pipeline_files.list_path.c_str());
    }
}

// (the cache writer's thread) writes the files each time they are wanted, until stopped
void pipeline_files_thread() {
    name_thread("kf4-gs-files");
    PipelineFiles &pf = pipeline_files;
    std::unique_lock<std::mutex> hold(pf.lock);
    for (;;) {
        pf.wake.wait(hold, [&] { return pf.stop || pf.wanted; });
        if (pf.stop) {
            return;
        }
        pf.wanted = false;
        hold.unlock();
        write_pipeline_files();
        hold.lock();
    }
}

// (the GS's thread) pipelines were made: the cache writer's thread writes the files again
void pipeline_files_want() {
    std::lock_guard<std::mutex> hold(pipeline_files.lock);
    pipeline_files.wanted = true;
    pipeline_files.wake.notify_one();
}

void pipeline_files_start(const std::string &list_path) {
    pipeline_files.list_path = list_path;
    pipeline_files.thread = std::thread(pipeline_files_thread);
}

// (at exit) the cache writer's thread stopped once it has written what it was writing, then the files written a
// last time here (what the last seconds made is kept)
void pipeline_files_stop() {
    if (pipeline_files.thread.joinable()) {
        {
            std::lock_guard<std::mutex> hold(pipeline_files.lock);
            pipeline_files.stop = true;
            pipeline_files.wake.notify_one();
        }
        pipeline_files.thread.join();
    }
    write_pipeline_files();
}

#include "gsn_standalone.inc" // (P8: the scene drawn into its own targets)
#include "gsn_composite.inc"  // (P8 T2: the scene at a scale of its own, and the composite)
#include "gsn_pipeline_warm.inc" // (pipelines made ahead: the lists, and the start's work)
#include "gsn_pipeline_set.inc"  // (pipelines made ahead: every one the draws can ask for, from the code)

// (gsn_open, --upscaler dlss) A context with DLSS's Vulkan extensions besides the presenter's: the instance with those
// NGX names (that the loader has), then the GPU Granite would choose (GRANITE_VULKAN_DEVICE_INDEX, or the first of the
// most able kind, Granite's device_score), the device with those NGX names for it. Null where any of it cannot be had,
// and FSR instead (sa_upscaler 2). (The names are kept: Granite holds the pointers.)
Context *dlss_context(const std::vector<const char *> &instance_ext, const std::vector<const char *> &device_ext) {
    static std::vector<std::string> want_instance, want_device;
    auto fail = [](Context *c, const char *why) -> Context * {
        if (why) {
            fprintf(stderr, "[gsn] --upscaler dlss: %s: FSR instead\n", why);
        }
        delete c;
        sa_upscaler = 2;
        return nullptr;
    };
    if (!dlss_instance_extensions(want_instance)) {
        return fail(nullptr, nullptr); // (said why)
    }
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> have(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, have.data());
    std::vector<const char *> ie = instance_ext;
    for (const std::string &w : want_instance) {
        bool there = false;
        for (const auto &e : have) {
            there = there || w == e.extensionName;
        }
        if (!there) {
            return fail(nullptr, "the Vulkan loader has not the instance extensions NGX wants");
        }
        ie.push_back(w.c_str());
    }
    Context *c = new Context;
    c->set_num_thread_indices(2); // (the GS's thread's and the back end's)
    if (!c->init_instance(ie.data(), uint32_t(ie.size()), 0)) {
        return fail(c, "no Vulkan instance with NGX's extensions");
    }
    VkInstance instance = c->get_instance();
    uint32_t gpus_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpus_count, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpus_count);
    vkEnumeratePhysicalDevices(instance, &gpus_count, gpus.data());
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    const char *index = getenv("GRANITE_VULKAN_DEVICE_INDEX");
    if (index && atoi(index) >= 0 && atoi(index) < int(gpus_count)) {
        gpu = gpus[size_t(atoi(index))];
    }
    unsigned best = 0;
    bool chosen = gpu != VK_NULL_HANDLE;
    for (size_t i = gpus_count; i && !chosen; i--) {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(gpus[i - 1], &props);
        unsigned score = props.apiVersion < VK_API_VERSION_1_1                              ? 0
                         : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 3
                         : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                         : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 1
                                                                                      : 0;
        if (score >= best) {
            best = score;
            gpu = gpus[i - 1];
        }
    }
    if (gpu == VK_NULL_HANDLE || !dlss_device_extensions(instance, gpu, want_device)) {
        return fail(c, gpu == VK_NULL_HANDLE ? "no GPU" : nullptr);
    }
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    have.resize(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, have.data());
    std::vector<const char *> de = device_ext;
    for (const std::string &w : want_device) {
        bool there = false, asked = false;
        for (const auto &e : have) {
            there = there || w == e.extensionName;
        }
        for (const char *d : de) {
            asked = asked || w == d;
        }
        if (!there) {
            return fail(c, "the GPU has not the device extensions NGX wants");
        }
        if (!asked) {
            de.push_back(w.c_str());
        }
    }
    if (!c->init_device(gpu, VK_NULL_HANDLE, de.data(), uint32_t(de.size()), 0)) {
        return fail(c, "no Vulkan device with NGX's extensions");
    }
    return c;
}

} // namespace

// ---- The C interface ----------------------------------------------------------------------------
extern "C" int gsn_open(void *shared_context) {
    holyland_on_compile = pw_on_compile; // (gsn_pipeline_warm.inc: every pipeline Granite makes, written down)
    holyland_on_render_pass = pw_on_render_pass;
    gs = new Gs();
    gs->vram.assign(GSN_VRAM_BYTES, 0);
    memset(gs->page_written, 0, sizeof(gs->page_written));
    gs->vram_writes = 1;
    memset(page_holders, 0, sizeof(page_holders)); // (their counts are page_written's)
    reset_gs();

    if (shared_context) {
        gs->context = static_cast<Context *>(shared_context);
        present_wanted = false;
    } else {
        if (!Context::init_loader(nullptr)) {
            fprintf(stderr, "[gsn] no Vulkan loader\n");
            return 0;
        }
        // a presenter's surface and swapchain extensions when asked for; without them if they fail
        std::vector<const char *> instance_ext, device_ext;
        if (present_wanted) {
            present_extensions(instance_ext, device_ext);
        }
        // (--upscaler dlss: with NGX's extensions too, where they can be had)
        gs->context = sa_upscaler == 3 ? dlss_context(instance_ext, device_ext) : nullptr;
        bool made = gs->context != nullptr;
        if (!made) {
            gs->context = new Context;
            gs->context->set_num_thread_indices(2); // (the GS's thread's and the back end's)
            made = gs->context->init_instance_and_device(instance_ext.data(), uint32_t(instance_ext.size()),
                                                         device_ext.data(), uint32_t(device_ext.size()), 0);
        }
        if (!made) {
            delete gs->context;
            gs->context = nullptr;
            if (!instance_ext.empty()) {
                fprintf(stderr, "[gsn] no Vulkan device with a swapchain: the window is shown without one\n");
                gs->context = new Context;
                gs->context->set_num_thread_indices(2); // (the GS's thread's and the back end's)
                if (!gs->context->init_instance_and_device(nullptr, 0, nullptr, 0, 0)) {
                    delete gs->context;
                    gs->context = nullptr;
                }
            }
            present_wanted = false;
            if (!gs->context) {
                fprintf(stderr, "[gsn] no Vulkan device\n");
                return 0;
            }
        } else if (instance_ext.empty()) {
            present_wanted = false;
        }
        gs->own_context = true;
    }
    const char *level = getenv("KF4_GSN_LOG");
    log_level = level ? atoi(level) : 0;
    gs->device = new Device;
    gs->device->set_context(*gs->context);
    // a frame context a vblank: with four, the CPU waits at a vblank for the GPU's work of four vblanks
    // before (two left it waiting on the GPU with --fast, a third of the native GS's vsync time)
    gs->device->init_frame_contexts(4);
    // --native-cutout-aa shades at every sample (the SPIR-V's SampleRateShading)
    sample_shading = gs->device->get_device_features().enabled_features.sampleRateShading != VK_FALSE;
    make_program();

    gs->dummy_raw = make_image(1, 1, VK_FORMAT_R8_UINT, 1, false);
    gs->dummy_rgba = make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, 1, false);
    cmd().clear_image(*gs->dummy_raw, {});
    cmd().clear_image(*gs->dummy_rgba, {});
    transfer_barrier();
    submit(true);
    backend_start();
    fprintf(stderr, "[gsn] the native GS on %s\n", gs->device->get_gpu_properties().deviceName);
    // --native-msaa's sample counts: colour and depth attachments' both, and depth read in a shader
    const VkPhysicalDeviceLimits &limits = gs->device->get_gpu_properties().limits;
    msaa_samples_have = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts &
                        limits.sampledImageDepthSampleCounts;
    switch (gs->device->get_gpu_properties().deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        gpu_kind = GSN_GPU_DISCRETE;
        break;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        gpu_kind = GSN_GPU_INTEGRATED;
        break;
    default:
        gpu_kind = GSN_GPU_OTHER;
        break;
    }
    ring_start();
    return 1;
}

extern "C" void gsn_gif(int path, const void *data, size_t bytes) {
    if (ring) {
        // (a packet bigger than a quarter of the ring goes in pieces: the GIF reads it qword by qword)
        const uint8_t *p = static_cast<const uint8_t *>(data);
        while (bytes > ring_bytes / 4) {
            ring_put(RING_GIF, path, p, ring_bytes / 4);
            p += ring_bytes / 4;
            bytes -= ring_bytes / 4;
        }
        ring_put(RING_GIF, path, p, bytes);
        return;
    }
    Timer timer(gsn_stats.ns_gif);
    gif(path, static_cast<const uint8_t *>(data), bytes);
}

extern "C" void gsn_wait(void) {
    ring_idle();
}

extern "C" void gsn_interp_frame(GsnInterpFrame *frame) {
    if (ring) {
        ring_put(RING_INTERP_FRAME, 0, &frame, sizeof(frame));
        return;
    }
    interp_frame_now(frame);
}

extern "C" void gsn_interp_picture(const GsnInterpPicture *p) {
    std::vector<uint8_t> data(sizeof(InterpPictureMessage) + size_t((p->runs + 31) / 32) * 4);
    InterpPictureMessage m = {};
    m.serial = p->serial;
    memcpy(m.header.move, p->move, sizeof(m.header.move));
    memcpy(m.header.mid, p->mid, sizeof(m.header.mid));
    m.header.zt[2] = p->t;
    m.header.zt[3] = 1.0f - p->t;
    m.header.runs = p->runs;
    memcpy(data.data(), &m, sizeof(m));
    if (p->runs) {
        memcpy(data.data() + sizeof(m), p->run_bits, size_t((p->runs + 31) / 32) * 4);
    }
    if (ring) {
        ring_put(RING_INTERP_PICTURE, 0, data.data(), data.size());
        return;
    }
    interp_picture_now(data.data(), data.size());
}

extern "C" void gsn_pipeline_cache(const char *path) {
    ring_idle();
    std::vector<uint8_t> data;
    if (FILE *f = fopen(path, "rb")) {
        uint8_t buffer[65536];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
            data.insert(data.end(), buffer, buffer + n);
        }
        fclose(f);
    }
    // (made again empty if the file is not this driver's, or damaged)
    gs->device->init_pipeline_cache(data.empty() ? nullptr : data.data(), data.size());
    cache_path = path;
    cache_saved_size = gs->device->get_pipeline_cache_size();
    // the pipelines of the lists made ahead, on threads of their own (gsn_pipeline_warm.inc); the data folder's list
    // and the cache written at the end too (not only while it runs: what the last seconds made is kept)
    std::string folder = cache_path.substr(0, cache_path.find_last_of("/\\") + 1);
    pw_start(folder + "native_gs_pipeline_list.txt");
    pipeline_files_start(folder + "native_gs_pipeline_list.txt");
    atexit([] {
        pw.stop = true;
        pipeline_files_stop();
    });
}

extern "C" int gsn_pipelines_building(int *done, int *total) {
    *done = pw.done.load();
    *total = pw.total.load();
    return pw.building.load();
}

extern "C" int gsn_pipelines_running(void) {
    return pw.running.load();
}

extern "C" void gsn_print_pipeline_stats(void) {
    ProfileCompiles play = port_profile_compiles();
    fprintf(stderr,
            "[stats] pipelines made ahead: %zu on the lists (%zu not shipped), %d ready from the cache in %.2f s, %d built in "
            "%.1f s, %d stale; made by draws that waited: %llu (%.1f ms, the longest %.1f ms), %llu not on a list\n",
            pw.entries.size(), pw.unshipped, pw.ready.load(), pw.seconds_check, pw.done.load(), pw.seconds_build,
            pw.stale.load(), (unsigned long long)play.count, double(play.ns) / 1e6, double(play.longest_ns) / 1e6,
            (unsigned long long)pw.play_new.load());
    fprintf(stderr,
            "[stats] pipeline set: %zu from the code (%s; %zu on the lists too%s); made by draws that waited: %llu "
            "outside it, %llu in it not built yet\n",
            ps.hashes.size(), ps.by_site.c_str(), ps.on_lists, ps_only() ? "; the set alone made ahead" : "",
            (unsigned long long)ps.outside.load(), (unsigned long long)ps.inside_late.load());
}

extern "C" void gsn_read_fifo(void *dst, uint32_t qwords) {
    ring_idle();
    size_t bytes = size_t(qwords) * 16;
    size_t have = gs->readback.size() > gs->readback_at ? gs->readback.size() - gs->readback_at : 0;
    size_t n = std::min(bytes, have);
    memcpy(dst, gs->readback.data() + gs->readback_at, n);
    memset(static_cast<uint8_t *>(dst) + n, 0, bytes - n);
    gs->readback_at += n;
}

extern "C" const char *gsn_why_name(int why) {
    static const char *const names[GSN_WHY_COUNT] = {"other", "attachments", "MSAA on/off", "shader blend",
                                                      "texture from a target", "texture decode", "upload",
                                                      "target copy", "write-back", "scanout", "submit"};
    return why >= 0 && why < GSN_WHY_COUNT ? names[why] : "?";
}

extern "C" void gsn_print_gpu_times(void) {
    if (gpu_time <= 0 || !gs || !gs->device) {
        return;
    }
    gs->device->wait_idle();
    double total = 0;
    std::vector<std::pair<std::string, double>> kinds;
    gs->device->timestamp_log([&](const std::string &tag, const TimestampIntervalReport &r) {
        for (int k = 0; k < GPU_KINDS; k++) {
            if (tag == std::string("gsn ") + gpu_kinds[k]) {
                double ms = r.time_per_accumulation * double(gpu_marks[k]) * 1000.0;
                kinds.push_back({gpu_kinds[k], ms});
                total += ms;
            }
        }
    });
    double pictures = double(std::max<uint64_t>(gsn_stats.pictures, 1));
    fprintf(stderr, "[stats] native GS GPU time: %.1f ms in all, %.3f ms a picture (%llu pictures):", total,
            total / pictures, (unsigned long long)gsn_stats.pictures);
    for (auto &k : kinds) {
        fprintf(stderr, " %s %.3f", k.first.c_str(), k.second / pictures);
    }
    fprintf(stderr, "\n");
    if (gpu_time >= 2) {
        fprintf(stderr, "[stats] native GS draws a picture by kind (draws/triangles):");
        for (int k = 0; k < GPU_KINDS; k++) {
            if (gpu_draws[k]) {
                fprintf(stderr, " %s %.0f/%.0f", gpu_kinds[k], double(gpu_draws[k]) / pictures, double(gpu_tris[k]) / pictures);
            }
        }
        fprintf(stderr, "\n");
    }
}

namespace {
// gsn_set_presenter's (a picture's pixels, or a new image for gsn_scanout_take), and the pixels it
// is given
void (*presenter)(const PortGsFrame *frame, uint64_t tag);
void (*presenter_image)(uint64_t tag);
PortGsFrame presented;

// The window's picture (an image) handed over by the back end, once it has published it (OP_PRESENT)
void present_image(uint64_t tag) {
    backend_stream().add(OP_PRESENT, ArgPublish{-1, tag});
    backend_send();
}

// A vblank's picture, or with GSN_EXTRA one more picture between two vblanks (gsn_picture): the same
// scanout, without what belongs to the vblank (the field-at-a-time countdown, the deinterlace's history,
// a new scale or MSAA, the pipeline cache); a picture that would be read a field at a time is not made.
int vsync_now(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame, uint64_t tag) {
    Timer timer(gsn_stats.ns_vsync);
    bool extra = (want_pixels & GSN_EXTRA) != 0;
    if (!extra && !uploads_now.empty()) {
        // the uploads of the packets since the last vblank: what the pictures between ask for again
        uploads_frame.swap(uploads_now);
        uploads_now.clear();
        recording = -1;
    }
    // SDL_Renderer's picture at the window's size: the scaled image read back (GSN_WANT_SCALED with
    // GSN_PRESENT and GSN_WANT_IMAGE), for the presenter's pixels
    bool scaled_pixels = (want_pixels & GSN_PRESENT) && (want_pixels & GSN_WANT_SCALED) &&
                         (want_pixels & GSN_WANT_IMAGE) && !(want_pixels & GSN_WANT_PIXELS);
    if (((want_pixels & GSN_PRESENT) && !(want_pixels & (GSN_WANT_IMAGE | GSN_WANT_PIXELS))) || scaled_pixels) {
        if (!presented.rgba) {
            presented.rgba = static_cast<uint32_t *>(malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS));
        }
        frame = &presented;
    }
    flush_batch();
    sa_sync(); // (standalone: the picture is the GS's)
    sa_switch(sa_mode != 0);
    sa_vblank(extra);
    gsn_stats.pictures++;
    port_profile_picture();
    gsn_stats.pipeline_binds = pipeline_binds();
    Why why_scanout(GSN_WHY_SCANOUT);
    end_pass();
    gpu_mark(GPU_OTHER);
    // a register every 16 bytes: PMODE, SMODE1, SMODE2, ..., DISPFB1, DISPLAY1, DISPFB2, DISPLAY2
    auto preg = [&](int n) { return privileged[n * 2]; };
    uint64_t pmode = preg(0);
    uint64_t smode2 = preg(2);
    int circuit = (pmode & 2) ? 2 : (pmode & 1) ? 1 : 0;
    // the PC's switches (gsn_vsync)
    bool progressive = want_pixels & VSYNC_PROGRESSIVE, deinterlace = want_pixels & VSYNC_DEINTERLACE;
    scanout_wants_scaled = (want_pixels & GSN_WANT_SCALED) != 0;
    scanout_fine_weave = (want_pixels & VSYNC_FINE_WEAVE) != 0;
    want_pixels &= GSN_WANT_PIXELS | GSN_WANT_IMAGE | GSN_PRESENT;
    // a frame-mode picture read a field at a time: 4 vblanks after a tall SCANMSK primitive (scanmsk_seen)
    bool field_scan = gs->field_scan_vblanks > 0;
    if (gs->field_scan_vblanks && !extra) {
        gs->field_scan_vblanks--;
    }
    GSN_LOG(3, "vsync field %d%s PMODE %llx SMODE2 %llx DISPFB1 %llx DISPLAY1 %llx DISPFB2 %llx DISPLAY2 %llx\n", field,
            field_scan ? " (a field at a time)" : "", (unsigned long long)pmode, (unsigned long long)smode2,
            (unsigned long long)preg(7), (unsigned long long)preg(8), (unsigned long long)preg(9),
            (unsigned long long)preg(10));
    bool history_kept = false;
    int ok = 0;
    if (circuit) {
        uint64_t dispfb = preg(circuit == 2 ? 9 : 7), display = preg(circuit == 2 ? 10 : 8);
        uint32_t fbp = bits(dispfb, 0, 9) * 32, fbw = bits(dispfb, 9, 6), psm = bits(dispfb, 15, 5);
        uint32_t dbx = bits(dispfb, 32, 11), dby = bits(dispfb, 43, 11);
        uint32_t magh = bits(display, 23, 4) + 1;
        uint32_t w = (bits(display, 32, 12) + 1) / magh, h = bits(display, 44, 11) + 1;
        bool interlaced = smode2 & 1, field_mode = (smode2 >> 1) & 1;
        if (interlaced && field_mode) {
            h /= 2;
        }
        // The scanout ("The scanout"): a field picture woven with the one before (or alone); a frame
        // picture whole, or a field at a time (this field's lines, deinterlaced or alone)
        ScanMode mode = SCAN_WHOLE;
        int parity = 0;
        if (interlaced && field_mode) {
            mode = deinterlace ? SCAN_WEAVE : SCAN_WHOLE;
            parity = field & 1;
        } else if (interlaced && (!progressive || field_scan)) {
            mode = deinterlace ? SCAN_DEINTERLACE : SCAN_FIELD;
            parity = int((bits(display, 12, 11) ^ uint32_t(field)) & 1); // (DY's parity moves the fields)
        }
        uint32_t sx = target_sx, sy = target_sy;
        if (extra && mode != SCAN_WHOLE) {
            w = 0; // (a picture between is whole, or none)
        }
        if (w > 0 && h > 0 && w <= 1024 && h <= 1024 &&
            size_t(w) * h * sx * sy * (mode == SCAN_WEAVE ? 2 : 1) <= PORT_GS_FRAME_PIXELS) {
            Shown shown = shown_buffer(fbp, fbw, psm, dbx, dby, w, h);
            if (mode == SCAN_WEAVE && parity == 0) {
                field0_buffer = shown; // (weave_fine: field 1's picture is field 0's, of this vblank)
                field0_set = true;
            }
            if (mode == SCAN_DEINTERLACE) {
                record_history(shown, parity);
                history_kept = true;
            }
            if (want_pixels & GSN_WANT_IMAGE) {
                ok = scanout_to_image(shown, mode, parity, (want_pixels & GSN_WANT_PIXELS) || scaled_pixels ? frame : nullptr);
            } else if (mode == SCAN_WEAVE) {
                // each field its own picture, woven with the one before: kept every field, wanted or not
                int f = field & 1;
                uint32_t got[2];
                gs->fields[f].resize(size_t(w) * h * sx * sy);
                read_buffer(fbp, fbw, psm, dbx, dby, w, h, gs->fields[f].data(), true, got);
                if (got[0] != sx || got[1] != sy) { // not a target (yet): one pixel a GS pixel, repeated
                    std::vector<uint32_t> small(gs->fields[f].begin(), gs->fields[f].begin() + long(size_t(w) * h));
                    for (uint32_t y = 0; y < h * sy; y++) {
                        for (uint32_t x = 0; x < w * sx; x++) {
                            gs->fields[f][size_t(y) * w * sx + x] = small[size_t(y / sy) * w + x / sx];
                        }
                    }
                }
                if (log_level >= 2) {
                    uint64_t hash = 1469598103934665603ull;
                    for (uint32_t v : gs->fields[f]) {
                        hash = (hash ^ v) * 1099511628211ull;
                    }
                    GSN_LOG(2, "field %d (CSR %d) from fbp %#x: %016llx\n", f, field, fbp, (unsigned long long)hash);
                }
                if (gs->fields[f ^ 1].size() != gs->fields[f].size()) {
                    gs->fields[f ^ 1] = gs->fields[f];
                }
                if (want_pixels) {
                    // the woven picture's line l is field (l & 1)'s line l / 2, each line sy rows of pixels
                    for (uint32_t y = 0; y < h * 2 * sy; y++) {
                        uint32_t l = y / sy, row = (l / 2) * sy + y % sy, from = l & 1;
                        if (scanout_fine_weave && sy >= 2) {
                            // (as weave_fine: field 0's rows, each twice)
                            from = 0;
                            row = y / 2;
                        }
                        memcpy(frame->rgba + size_t(y) * w * sx, gs->fields[from].data() + size_t(row) * w * sx,
                               w * sx * 4);
                    }
                    frame->width = w * sx;
                    frame->height = h * 2 * sy;
                    ok = 1;
                }
            } else if (mode != SCAN_WHOLE || (shown.t && wide(*shown.t->image)) || sa_composite_size(shown, nullptr, nullptr)) {
                // (on the GPU, as for the presenter, and read back; --native-colour high's picture so
                // too, dithered as the window has it)
                if (want_pixels) {
                    ok = (scanout_to_image(shown, mode, parity, frame) & GSN_WANT_PIXELS) ? 1 : 0;
                }
            } else if (want_pixels) {
                uint32_t got[2];
                read_buffer(fbp, fbw, psm, dbx, dby, w, h, frame->rgba, true, got);
                frame->width = w * got[0];
                frame->height = h * got[1];
                ok = 1;
            }
        }
    }
    if (!history_kept && gs->history_filled && !extra) {
        forget_history();
    }
    gpu_mark(GPU_SCANOUT);
    submit(false);
    backend_stream().add(OP_NEXT_FRAME, ArgValues{});
    if (ok & GSN_WANT_IMAGE) {
        scanout_publish(tag);
    }
    backend_send();
    if (extra) {
        if (ok && (want_pixels & GSN_PRESENT)) {
            if (scaled_pixels && (ok & GSN_WANT_PIXELS) && presenter) {
                presenter(frame, tag);
            } else if ((ok & GSN_WANT_IMAGE) && presenter_image && !scaled_pixels) {
                present_image(tag);
            } else if (!(want_pixels & GSN_WANT_IMAGE) && presenter) {
                presenter(frame, tag);
            }
        }
        return ok;
    }
    // the pipeline cache and the data folder's pipeline list, written when pipelines have been made (looked at
    // every two seconds, by the cache writer's thread)
    if (!cache_path.empty() && ++cache_vsyncs % 120 == 0 && pw.made.load() != pw.made_saved) {
        pw.made_saved = pw.made.load();
        pipeline_files_want();
    }
    gsn_stats.targets = gs->targets.size();
    gsn_stats.textures = gs->textures.size();
    gsn_stats.pool_images = decoded_pool.size();
    gsn_stats.pool_mb = decoded_pool_bytes >> 20;
    gsn_stats.upload_copies = page_images.size();
    uint32_t wanted = wanted_scale;
    if ((wanted & 0xFF) != target_sx || (wanted >> 8) != target_sy || wanted_samples != target_samples ||
        wanted_colour != target_colour) {
        // every target back to VRAM; they come again at the new size as they are drawn into
        shadows_resolve_all();
        shadows.clear();
        for (auto &t : gs->targets) {
            write_back(*t);
        }
        end_pass();
        gs->targets.clear();
        gs->scratch.clear(); // (shader blending's images, as large as the targets were)
        gs->resolved[0].reset();
        gs->resolved[1].reset();
        target_sx = wanted & 0xFF;
        target_sy = wanted >> 8;
        target_samples = wanted_samples;
        target_colour = wanted_colour;
        gs->fields[0].clear();
        gs->fields[1].clear();
        forget_history();
    }
    if (ok && (want_pixels & GSN_PRESENT)) {
        // the window's picture, handed over now that it is there (an image: once its commands are on
        // the queue and it is published, by the back end: present_image)
        if (scaled_pixels && (ok & GSN_WANT_PIXELS) && presenter) {
            presenter(frame, tag);
        } else if ((ok & GSN_WANT_IMAGE) && presenter_image && !scaled_pixels) {
            present_image(tag);
        } else if (!(want_pixels & GSN_WANT_IMAGE) && presenter) {
            presenter(frame, tag);
        }
    }
    return ok;
}

#include "gsn_upscale.inc" // (--upscaler fsr3: the temporal upscaler's calls, on the back end's thread)
#include "gsn_backend.inc" // (the calls into Vulkan, made on a thread of their own)
} // namespace

extern "C" void gsn_set_presenter(void (*pixels)(const PortGsFrame *frame, uint64_t tag), void (*image)(uint64_t tag)) {
    presenter = pixels;
    presenter_image = image;
}

extern "C" int gsn_vsync(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame) {
    return gsn_picture(privileged, field, want_pixels, frame, 0);
}

extern "C" int gsn_picture(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame, uint64_t tag) {
    // the PC's switches for the scanout, as they are at this vblank
    // (whole frames: a field-mode screen's two fields are one moment, woven a row of pixels at a time)
    want_pixels |= (port_options.progressive ? VSYNC_PROGRESSIVE : 0) | (port_options.deinterlace ? VSYNC_DEINTERLACE : 0) |
                   (port_options.whole_frame ? VSYNC_FINE_WEAVE : 0);
    // On the GS's thread, with the sender going on: a vblank whose picture nobody waits for (none, or
    // one the GS's thread hands to the presenter). A picture wanted in frame, or an image the caller
    // goes on to show itself, waits for the thread to be idle and is made here.
    bool queued = !(want_pixels & GSN_WANT_PIXELS) && (!(want_pixels & GSN_WANT_IMAGE) || (want_pixels & GSN_PRESENT));
    if (ring && queued) {
        uint8_t data[vsync_bytes + 8];
        memcpy(data, privileged, vsync_bytes);
        memcpy(data + vsync_bytes, &tag, 8);
        ring_put(RING_VSYNC, field, data, sizeof(data), uint32_t(want_pixels));
        if (want_pixels & GSN_EXTRA) {
            return want_pixels & (GSN_PRESENT | GSN_WANT_IMAGE);
        }
        ring->vsyncs_sent++;
        if (want_pixels & GSN_PRESENT) {
            // A window's picture is at most two vblanks behind the game: the sender waits for the GS's
            // thread beyond that (with --fast it would run ahead by as many vblanks as the ring holds)
            Ring &q = *ring;
            sender_wait([&] { return q.vsyncs_sent - q.vsyncs_done.load() <= 2 || q.stopped.load(); });
        }
        return want_pixels & (GSN_PRESENT | GSN_WANT_IMAGE);
    }
    ring_idle();
    return vsync_now(privileged, field, want_pixels, frame, tag);
}

extern "C" int gsn_backlog(void) {
    return ring ? int(ring->vsyncs_sent - ring->vsyncs_done.load()) : 0;
}

extern "C" void gsn_set_scale(int x_scale, int y_scale) {
    uint32_t x = uint32_t(x_scale < 1 ? 1 : x_scale > GSN_MAX_SCALE_X ? GSN_MAX_SCALE_X : x_scale);
    uint32_t y = uint32_t(y_scale < 1 ? 1 : y_scale > 4 ? 4 : y_scale);
    wanted_scale = x | y << 8;
}

extern "C" int gsn_gpu_kind(void) {
    return gpu_kind;
}

extern "C" void gsn_set_lod(int mode) {
    lod_mode = mode;
}

extern "C" int gsn_set_aniso(int taps) {
    int l = 0;
    while (l < 4 && (2 << l) <= taps) {
        l++;
    }
    aniso_log2 = l;
    return 1 << l;
}

extern "C" void gsn_set_2d(int mode) {
    smooth_2d_mode = mode;
}

extern "C" void gsn_set_cutout_aa(int mode) {
    cutout_aa_mode = mode;
}

extern "C" void gsn_set_layer_z(int mode) {
    layer_z_mode = mode == GSN_LAYER_Z_TOLERANT ? GSN_LAYER_Z_TOLERANT : GSN_LAYER_Z_PS2;
}

extern "C" void gsn_set_shine_exact(int exact) {
    shine_exact = exact ? 1 : 0;
}

extern "C" void gsn_set_native_draw(int mode) {
    nd_mode = mode == GSN_NATIVE_DRAW_PS2 ? GSN_NATIVE_DRAW_PS2 : GSN_NATIVE_DRAW_NATIVE;
}

extern "C" void gsn_set_standalone(int on) {
    sa_mode = on; // (taken at the next vblank: gsn_standalone.inc)
}

extern "C" void gsn_set_hardware_sampler(int on) {
    hw_sampler = on != 0;
}

extern "C" void gsn_set_pc_world(int on) {
    pc_world_mode = on != 0; // (gsn_pc_world.inc: taken as each draw is met)
}

extern "C" void gsn_set_scene(int x_scale, int y_scale, int samples) {
    // (taken when the scene next takes a frame: gsn_standalone.inc, sa_shape)
    sa_wanted_scale = uint32_t(std::min(std::max(x_scale, 0), GSN_MAX_SCALE_X)) | uint32_t(std::min(std::max(y_scale, 0), 4)) << 8;
    sa_wanted_samples = uint32_t(std::min(std::max(samples, 0), 8));
}

extern "C" void gsn_set_upscaler(int mode) {
    sa_upscaler = mode;
}

extern "C" void gsn_set_scene_map(int map) {
    sa_set_map(map); // (standalone: --stats per map)
}

extern "C" void gsn_print_standalone_stats(void) {
    nd_prep_print_stats(); // (rc/own: native draw's work kept for the pictures between, hosted or modern)
    sa_print_stats();
    rgba_print_stats(); // (--native-sampler hardware)
}

extern "C" void gsn_set_colour(int mode) {
    wanted_colour = mode == GSN_COLOUR_HIGH ? GSN_COLOUR_HIGH : GSN_COLOUR_EXACT;
}

extern "C" int gsn_set_msaa(int samples) {
    // the most this GPU has up to `samples` (msaa_samples_have)
    uint32_t n = 1;
    while (n * 2 <= uint32_t(samples) && n < 8 && (msaa_samples_have & (n * 2))) {
        n *= 2;
    }
    // (off and on again is live; another count once one has been drawn with, from the next start)
    uint32_t used = msaa_samples_used.load();
    if (n > 1 && used) {
        n = used;
    }
    wanted_samples = n;
    return int(n);
}

extern "C" int gsn_display_rect(const uint64_t *privileged, GsnRect *rect) {
    return display_rect(privileged, rect);
}

extern "C" void gsn_read_rect(const GsnRect *r, uint32_t *rgba) {
    ring_idle();
    flush_batch();
    sa_sync(); // (standalone: the scene's turn ends)
    read_buffer(r->bp, r->bw, r->psm, r->x, r->y, r->w, r->h, rgba);
    submit(false);
}

extern "C" void gsn_read_rect_scaled(const GsnRect *r, uint32_t *rgba, uint32_t *scale) {
    ring_idle();
    flush_batch();
    sa_sync(); // (standalone)
    read_buffer(r->bp, r->bw, r->psm, r->x, r->y, r->w, r->h, rgba, true, scale);
    submit(false);
}

extern "C" void gsn_decode_rect(const uint8_t *vram, const GsnRect *r, uint32_t *rgba) {
    for (uint32_t j = 0; j < r->h; j++) {
        for (uint32_t i = 0; i < r->w; i++) {
            uint32_t v = gsn_vram_read(vram, r->psm, r->bp, r->bw, r->x + i, r->y + j);
            rgba[size_t(j) * r->w + i] = vram_to_target_pixel(v, r->psm);
        }
    }
}

extern "C" void gsn_write_rect(const GsnRect *r, const uint32_t *rgba) {
    ring_idle();
    flush_batch();
    sa_sync(); // (standalone)
    Target *t = find_target(r->bp, r->bw, r->psm);
    if (t && t->sx == 1 && t->sy == 1 && r->x + r->w <= t->width && r->y + r->h <= t->height) {
        msaa_settle(*t);
        msaa_image_written(*t);
        end_pass();
        transfer_barrier();
        void *out = cmd().update_image(*t->image, {int32_t(r->x), int32_t(r->y), 0}, {r->w, r->h, 1}, r->w, r->h,
                                       {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        put_pixels(out, *t->image, 0, rgba, size_t(r->w) * r->h);
        transfer_barrier();
        return;
    }
    uint32_t lo, count;
    buffer_pages(r->bp, r->bw, r->psm, r->x + r->w, r->y + r->h, &lo, &count);
    claim_pages(lo, count, nullptr, true);
    for (uint32_t j = 0; j < r->h; j++) {
        for (uint32_t i = 0; i < r->w; i++) {
            gsn_vram_write(gs->vram.data(), r->psm, r->bp, r->bw, r->x + i, r->y + j,
                           target_pixel_to_vram(rgba[size_t(j) * r->w + i], r->psm));
        }
    }
    pages_written(lo, count);
}

extern "C" const void *gsn_vram(void) {
    ring_idle();
    flush_batch();
    sa_sync(); // (standalone: captures, save states)
    shadows_resolve_all();
    for (auto &t : gs->targets) {
        write_back(*t);
    }
    return gs->vram.data();
}

extern "C" void gsn_capture(void (*emit)(uint32_t type, const void *data, size_t size)) {
    emit(KF4GS_VRAM, gsn_vram(), GSN_VRAM_BYTES);
    std::vector<Kf4GsReg> regs;
    static const uint32_t saved[] = {0x00, 0x01, 0x02, 0x03, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x14, 0x15, 0x18,
                                     0x19, 0x1A, 0x1B, 0x1C, 0x22, 0x34, 0x35, 0x36, 0x37, 0x3B, 0x3D, 0x40,
                                     0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C,
                                     0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53};
    for (uint32_t a : saved) {
        regs.push_back({a, 0, gs->reg[a]});
    }
    regs.push_back({KF4GS_REG_INTERNAL_Q, 0, gs->internal_q});
    regs.push_back({KF4GS_REG_CACHED_CBP0, 0, gs->clut_cbp[0]});
    regs.push_back({KF4GS_REG_CACHED_CBP1, 0, gs->clut_cbp[1]});
    emit(KF4GS_GS_REGS, regs.data(), regs.size() * sizeof(Kf4GsReg));

    // the scanout's history, as paraLLEl-GS's: the countdown, then the four fields (at the GS's
    // resolution only: a larger --native-scale's are left out, as paraLLEl-GS could not use them)
    std::vector<uint8_t> history;
    auto put = [&](const void *data, size_t size) {
        history.insert(history.end(), static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    };
    uint32_t head[2] = {gs->field_scan_vblanks, 4};
    put(head, sizeof(head));
    bool gs_size = target_sx == 1 && target_sy == 1;
    for (uint32_t i = 0; i < 4; i++) {
        Kf4GsField f = {-1, 0, 0, 0};
        const Image *image = gs_size && i < gs->history_filled ? gs->history[i].get() : nullptr;
        if (image) {
            f.present = 1;
            f.width = image->get_width();
            f.height = image->get_height();
            for (uint32_t j = 0; j < i; j++) {
                if (gs->history[j].get() == image) {
                    f.same_as = int32_t(j);
                }
            }
        }
        put(&f, sizeof(f));
        if (image && f.same_as < 0) {
            flush_batch();
            const uint8_t *pixels = read_image(*image, 0, 0, f.width, f.height);
            put(pixels, size_t(f.width) * f.height * 4);
            done_reading();
        }
    }
    emit(KF4GS_GS_HISTORY, history.data(), history.size());
    sa_capture(emit); // (standalone: the page-ownership table, when the scene is on)
}

extern "C" size_t gsn_state_size(void) {
    return GSN_VRAM_BYTES + sizeof(gs->reg) + sizeof(gs->internal_q) + sizeof(gs->clut) + sizeof(gs->clut_cbp) +
           sizeof(gs->queue) + sizeof(gs->queue_count) + sizeof(gs->paths) + sizeof(gs->upload) + sizeof(gs->transfer_dir);
}

extern "C" void gsn_state(PortStateIo *io) {
    ring_idle();
    if (!io->loading) {
        port_state_bytes(io, const_cast<void *>(gsn_vram()), GSN_VRAM_BYTES);
    } else {
        flush_batch();
        sa_forget();
        end_pass();
        gs->targets.clear();
        shadows.clear();
        gs->textures.clear();
        texture_index.clear();
        forget_uploads();
        port_state_bytes(io, gs->vram.data(), GSN_VRAM_BYTES);
        pages_written(0, vram_pages);
    }
    port_state_bytes(io, gs->reg, sizeof(gs->reg));
    port_state_bytes(io, &gs->internal_q, sizeof(gs->internal_q));
    port_state_bytes(io, gs->clut, sizeof(gs->clut));
    port_state_bytes(io, gs->clut_cbp, sizeof(gs->clut_cbp));
    port_state_bytes(io, gs->queue, sizeof(gs->queue));
    port_state_bytes(io, &gs->queue_count, sizeof(gs->queue_count));
    port_state_bytes(io, gs->paths, sizeof(gs->paths));
    port_state_bytes(io, &gs->upload, sizeof(gs->upload));
    port_state_bytes(io, &gs->transfer_dir, sizeof(gs->transfer_dir));
    if (io->loading) {
        for (uint32_t i = 0; i < 3; i++) {
            gs->queued[i] = make_vtx(gs->queue[i], effective_prim(), gs->queue[i]);
        }
        gs->verts.clear();
        gs->fields[0].clear();
        gs->fields[1].clear();
        forget_history();
    }
}

extern "C" int gsn_restore(uint32_t type, const void *data, size_t size) {
    ring_idle();
    switch (type) {
    case KF4GS_VRAM:
        if (size != GSN_VRAM_BYTES) {
            return 0;
        }
        flush_batch();
        sa_forget();
        end_pass();
        gs->targets.clear();
        shadows.clear();
        gs->textures.clear();
        texture_index.clear();
        forget_uploads();
        memcpy(gs->vram.data(), data, GSN_VRAM_BYTES);
        pages_written(0, vram_pages);
        reset_gs();
        forget_history(); // (a capture's comes after: KF4GS_GS_HISTORY)
        // (no targets: a capture is drawn with MSAA, and at the scale, from its start)
        target_samples = wanted_samples;
        target_colour = wanted_colour;
        target_sx = wanted_scale & 0xFF;
        target_sy = wanted_scale >> 8;
        return 1;
    case KF4GS_GS_REGS: {
        const Kf4GsReg *regs = static_cast<const Kf4GsReg *>(data);
        for (size_t n = 0; n < size / sizeof(Kf4GsReg); n++) {
            if (regs[n].address < 256) {
                gs->reg[regs[n].address] = regs[n].value;
            } else if (regs[n].address == KF4GS_REG_INTERNAL_Q) {
                gs->internal_q = uint32_t(regs[n].value);
            }
            // the CLUT buffer is not in a capture: the next TEX0 that compares loads it again
        }
        gs->clut_cbp[0] = gs->clut_cbp[1] = UINT32_MAX;
        return 1;
    }
    case KF4GS_GS_PRIV:
        return 1;
    case KF4GS_GS_HISTORY: {
        // the countdown and the fields, as gsn_capture writes them (or paraLLEl-GS)
        const uint8_t *at = static_cast<const uint8_t *>(data), *end = at + size;
        uint32_t head[2];
        if (size < sizeof(head)) {
            return 0;
        }
        memcpy(head, at, sizeof(head));
        at += sizeof(head);
        gs->field_scan_vblanks = head[0];
        forget_history();
        ImageHandle fields[4];
        for (uint32_t i = 0; i < head[1] && i < 4; i++) {
            Kf4GsField f;
            if (size_t(end - at) < sizeof(f)) {
                return 0;
            }
            memcpy(&f, at, sizeof(f));
            at += sizeof(f);
            if (!f.present) {
                continue;
            }
            if (f.same_as >= 0 && uint32_t(f.same_as) < i) {
                fields[i] = fields[f.same_as];
                continue;
            }
            size_t bytes = size_t(f.width) * f.height * 4;
            if (size_t(end - at) < bytes || !f.width || !f.height || f.width > 8192 || f.height > 8192) {
                return 0;
            }
            fields[i] = make_image(f.width, f.height, VK_FORMAT_R8G8B8A8_UNORM, 1, true);
            end_pass();
            transfer_barrier();
            void *out = cmd().update_image(*fields[i], {0, 0, 0}, {f.width, f.height, 1}, f.width, f.height,
                                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            memcpy(out, at, bytes);
            transfer_barrier();
            at += bytes;
        }
        // the fields there from the newest on (paraLLEl-GS's fills a missing one as its deinterlace does)
        while (gs->history_filled < 4 && fields[gs->history_filled]) {
            gs->history[gs->history_filled] = fields[gs->history_filled];
            gs->history_filled++;
        }
        return 1;
    }
    case KF4GS_SA_TABLE:
        return sa_table_load(static_cast<const uint8_t *>(data), size) ? 1 : 0; // (standalone)
    }
    return 0;
}

// (port/platform/gs.c) a save state's page-ownership table, after gsn_state (standalone: gsn_standalone.inc)
extern "C" size_t gsn_table_state_size(void) {
    ring_idle();
    return sa_state_size();
}
extern "C" void gsn_table_state(PortStateIo *io, size_t size) {
    ring_idle();
    sa_state(io, size);
}

// ---- The presenter's side (port/platform/present_vk.cpp) ------------------------------------------
extern "C" void gsn_enable_present(void) {
    present_wanted = true;
}

extern "C" int gsn_present_device(GsnPresentDevice *out) {
    if (!gs || !gs->context || !present_wanted) {
        return 0;
    }
    const QueueInfo &queues = gs->context->get_queue_info();
    out->instance = gs->context->get_instance();
    out->gpu = gs->context->get_gpu();
    out->device = gs->context->get_device();
    out->queue = queues.queues[QUEUE_INDEX_GRAPHICS];
    out->queue_family = queues.family_indices[QUEUE_INDEX_GRAPHICS];
    out->api_version = gs->context->get_enabled_device_features().instance_api_core_version;
    out->get_instance_proc_addr = reinterpret_cast<void *>(Context::get_instance_proc_addr());
    return out->queue != VK_NULL_HANDLE;
}

extern "C" void gsn_queue_lock(void) {
    gs->device->external_queue_lock();
}

extern "C" void gsn_queue_unlock(void) {
    gs->device->external_queue_unlock();
}

extern "C" int gsn_scanout_take_tag(GsnScanout *out, uint64_t tag) {
    ScanoutImage &so = scanout_image;
    std::lock_guard<std::mutex> hold(so.lock);
    int pick = so.newest;
    for (int i = 0; i < SCANOUT_SLOTS; i++) {
        const ScanoutSlot &sl = so.slots[i];
        if (sl.serial != 0 && sl.tag == tag && (pick < 0 || so.slots[pick].tag != tag || sl.serial > so.slots[pick].serial)) {
            pick = i;
        }
    }
    if (pick < 0 || !so.slots[pick].image || so.slots[pick].serial == 0) {
        return 0;
    }
    const ScanoutSlot &sl = so.slots[pick];
    out->image = uint64_t(sl.image->get_image());
    out->width = sl.width;
    out->height = sl.height;
    out->serial = sl.serial;
    out->tag = sl.tag;
    out->scaled = sl.scaled ? uint64_t(sl.scaled->get_image()) : 0;
    out->scaled_width = sl.scaled_width;
    out->scaled_height = sl.scaled_height;
    so.taken = pick;
    return 1;
}

extern "C" void gsn_set_present_scale(int width, int height, float sharpen) {
    present_size.store(width > 0 && height > 0 ? uint32_t(width) << 16 | uint32_t(height) : 0);
    present_sharpen.store(uint32_t(sharpen > 0 ? sharpen * 1000.0f + 0.5f : 0.0f));
}

extern "C" uint64_t gsn_present_scale_passes(void) {
    return present_scale_passes;
}

extern "C" int gsn_scanout_take(GsnScanout *out) {
    ScanoutImage &so = scanout_image;
    uint64_t tag;
    {
        std::lock_guard<std::mutex> hold(so.lock);
        if (so.newest < 0) {
            return 0;
        }
        tag = so.slots[so.newest].tag;
    }
    return gsn_scanout_take_tag(out, tag);
}

extern "C" void gsn_scanout_release(void) {
    std::lock_guard<std::mutex> hold(scanout_image.lock);
    scanout_image.taken = -1;
}
