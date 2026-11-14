// The PC renderer's drawing without the GS's VRAM (pcr_canvas.h). The GS's registers at a primitive pick its
// surface, its texture and its pixel arithmetic; primitives of the same state are one batch, drawn when the state
// changes or something else needs the GPU (an upload, a copy, a picture).
#include "pcr_canvas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <list>
#include <unordered_map>
#include <variant>
#include <vector>

#include "pcr_shaders.h"
#include "pcr_gpu_time.h"
#include "pcr_pipelines.h"
#include "pcr_textures.h"

extern "C" {
#include "gsn_vram.h"
#include "port/profile.h"
}

using namespace Vulkan;

namespace pcr {

namespace {

uint32_t bits(uint64_t v, int at, int count) {
    return uint32_t((v >> at) & ((uint64_t(1) << count) - 1));
}

// ---- What the shaders are given (shaders/pcr_common.h: the two must agree) -----------------------------------
struct PrimPush {
    float inv_size[2];
    float zscale;
    uint32_t zmax;
    uint32_t flags;
    uint32_t tex_size;
    uint32_t clamp_lo, clamp_hi;
    uint32_t test;
    uint32_t fogcol;
    uint32_t texa;
    uint32_t tex1;
    float lod_k;
    uint32_t levels;
    int32_t src_offset[2];
    int32_t src_scale[2];
    int32_t place[2];
    int32_t dest_offset[2];
    uint32_t alpha;
    uint32_t clut_at;
    uint32_t table;
    uint32_t record; // (a run's programs, PCR_RECORDS: its first record in the picture's records, Impl::Runs)
    uint32_t factors; // (a run's draw, PCR_RECORDS: its blend's factors from the shader, dual-source: F_DUAL | ...)
    uint32_t trace_xy;   // (developers' pixel trace) the target's pixel traced: x | y << 16, or no_trace
    uint32_t trace_draw; // ... the draw its fragments are recorded as
    uint32_t unused;     // (a record's size a multiple of 8 bytes)
};
static_assert(sizeof(PrimPush) <= 128, "push constants: 128 bytes at most");
static_assert(sizeof(PrimPush) % 8 == 0, "a record: std430's stride of pcr_common.h's PcrRecord");
constexpr uint32_t no_trace = 0xFFFFFFFFu; // (pcr_common.h's PCR_NO_TRACE)

// The developers' pixel trace's buffer (pcr_prim.h's Trace, pcr_probe.h's): a head of 16 bytes (the fragments
// recorded), the fragments' records (pcr.h's PcrTraceFragment), then the probes' (PcrTraceProbe)
constexpr size_t trace_fragments = 4096, trace_probes = 1024;
constexpr size_t trace_probe_offset = 16 + trace_fragments * sizeof(PcrTraceFragment);
constexpr size_t trace_bytes = trace_probe_offset + trace_probes * sizeof(PcrTraceProbe);
static_assert(sizeof(PcrTraceFragment) == 48 && sizeof(PcrTraceProbe) == 80, "pcr_prim.h's and pcr_probe.h's records");
static_assert(trace_probe_offset == (1 + 4096 * 3) * 16, "pcr_probe.h's PCR_PROBE_FIRST");

enum : uint32_t {
    F_TME = 0x1,
    F_FST = 0x2,
    F_TFX_SHIFT = 2,
    F_TCC = 0x10,
    F_FGE = 0x20,
    F_SRC_SHIFT = 6,
    F_PASS_SHIFT = 9,
    F_FBA = 0x800,
    F_PREMUL = 0x1000,
    F_C_FIX = 0x2000,
    F_ALPHA_FACTOR = 0x4000,
    F_DATE = 0x8000,
    F_DATM = 0x10000,
    F_SHADER_BLEND = 0x20000,
    F_SURFACE_CT16 = 0x40000,
    F_MSAA = 0x80000,
    F_SMOOTH_2D = 0x100000,
    F_HIGH = 0x200000,
    F_DECAL = 0x400000,
    F_DECAL_LOSS = 0x800000, // (developers' decal losses: pcr_common.h)
    F_DECAL_LOSS_FAR = 0x1000000,
    F_EXACT_Z = 0x2000000, // (pcr_common.h's exact Z)
    F_LAYERS = 0x4000000,  // (pcr_common.h) tolerant layers: Z's fraction kept
    F_LAYER_TEST = 0x8000000, // ... a draw's colour tested half a Z nearer
    F_LAYER_FINE = 0x10000000, // ... a sixteenth of a Z
    F_LAYER_WIDE = 0x20000000, // ... a Z and half its slope (a decal's)
};
// PrimPush::factors (pcr_prim.h's o_factor): the blend's colour factors, source (bits 0-2) and destination (4-6), as
// the shader gives them (FACTOR_*), the alpha written as the blend would have made it
constexpr uint32_t F_DUAL = 0x80000000u;
enum : uint32_t { FACTOR_ZERO, FACTOR_ONE, FACTOR_AS, FACTOR_ONE_MINUS_AS, FACTOR_FIX, FACTOR_ONE_MINUS_FIX, FACTOR_NONE };
uint32_t factor_code(VkBlendFactor f) {
    switch (f) {
    case VK_BLEND_FACTOR_ZERO: return FACTOR_ZERO;
    case VK_BLEND_FACTOR_ONE: return FACTOR_ONE;
    case VK_BLEND_FACTOR_SRC_ALPHA: return FACTOR_AS;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return FACTOR_ONE_MINUS_AS;
    case VK_BLEND_FACTOR_CONSTANT_COLOR: return FACTOR_FIX;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return FACTOR_ONE_MINUS_FIX;
    default: return FACTOR_NONE;
    }
}
enum : uint32_t { SRC_CT32 = 0, SRC_CT24 = 1, SRC_CT16 = 2, SRC_INDEX = 3, SRC_SURFACE = 4, SRC_SAMPLED = 5 };

// (PrimPush::clut_at, Batch::clut_at: the CLUT is set 1's, not the kept walk's)
constexpr uint32_t no_walk_clut = 0xFFFFFFFFu;
// (PrimPush::table, Batch::table_at: the texture is set 1's, not the kept walk's table's)
constexpr uint32_t no_table = 0xFFFFu;
// (a kept walk's samplers, u_samplers: the stock nearest one and 15 of the walk's)
constexpr uint32_t walk_samplers = 16;

// (Vertex::rect: no texels known, the 2D mode's smooth not for its primitive)
constexpr uint32_t no_rect = 0xFFFFFFFFu;

struct Vertex {
    float x, y;
    uint32_t z;
    uint8_t rgba[4];
    float s, t, q;
    float fog;
    // (a picture between's corner, pcr_prim.vert's a_kicks: the kicks it is made of and how, K_* below; no_kick: as
    // it is)
    uint32_t kicks[4] = {no_kick, no_kick, no_kick, 0};
    // (the 2D mode's smooth: the texels its primitive covers across, rect[0], and down, rect[1], each first | last << 16,
    // of a sprite's or a 2D triangle's corners (FST, or one Z and Q); no_rect for the rest: texel_rect)
    uint32_t rect[2] = {no_rect, no_rect};
    // (exact Z, pcr_common.h) a whole Z its triangle's three corners share: the first corner's (link_corners)
    uint32_t zref = 0;
};
static_assert(sizeof(Vertex) == 60, "Vertex: the vertex shader's inputs, packed");

// Each triangle's corners given its first corner's Z as the Z they share (Vertex::zref; vertices three a triangle)
void link_corners(std::span<Vertex> triangles) {
    for (size_t i = 0; i + 3 <= triangles.size(); i += 3) {
        for (size_t k = 0; k < 3; k++) {
            triangles[i + k].zref = triangles[i].z;
        }
    }
}
// Vertex::kicks[3] (pcr_prim.vert's PCR_K_*): a sprite's corner (0-3), a sprite's, UV for S and T
enum : uint32_t { K_CORNER = 0x3, K_SPRITE = 0x4, K_FST = 0x8 };

// The GS pixels a primitive may draw (those whose integer points are in its box), less XYOFFSET, inclusive
struct PixelBox {
    int32_t x0, y0, x1, y1;
};

PixelBox box_of(const GifPrimitive &p, uint64_t xyoffset) {
    float ox = float(bits(xyoffset, 0, 16)) / 16.0f, oy = float(bits(xyoffset, 32, 16)) / 16.0f;
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    uint32_t n = p.shape == PrimitiveShape::triangle ? 3 : 2;
    for (uint32_t i = 0; i < n; i++) {
        float x = float(p.vertices[i].x) / 16.0f - ox, y = float(p.vertices[i].y) / 16.0f - oy;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    return {int32_t(std::ceil(x0)), int32_t(std::ceil(y0)), int32_t(std::ceil(x1)) - 1, int32_t(std::ceil(y1)) - 1};
}

// ---- The GS's formats ----------------------------------------------------------------------------------------
bool is_colour_16(uint32_t psm) {
    return psm == GSN_PSMCT16 || psm == GSN_PSMCT16S;
}

// A page of the format: pixels down (64 across for every colour and Z format)
uint32_t page_rows(uint32_t psm) {
    return is_colour_16(psm) || psm == 0x32 || psm == 0x3A ? 64 : 32;
}

// ZBUF's PSM (4 bits) as the Z format's own
uint32_t z_psm(uint64_t zbuf) {
    return 0x30 | bits(zbuf, 24, 4);
}

// The blend: Vulkan's factors for ALPHA's (A - B) * C + D, with the shader's part (pcr_prim.frag's flags)
struct BlendChoice {
    bool enable = false;
    VkBlendFactor src = VK_BLEND_FACTOR_ONE, dst = VK_BLEND_FACTOR_ZERO, src_alpha = VK_BLEND_FACTOR_ONE;
    VkBlendOp op = VK_BLEND_OP_ADD;
    uint32_t flags = 0;
    bool write_rgb = true;
    bool supported = true;
};

BlendChoice choose_blend(uint64_t alpha, bool abe) {
    BlendChoice b;
    if (!abe) {
        return b;
    }
    uint32_t a = bits(alpha, 0, 2), bb = bits(alpha, 2, 2), c = bits(alpha, 4, 2), d = bits(alpha, 6, 2);
    bool fix = c == 2;
    if (c == 1) {
        b.supported = false; // (C = Ad: by As)
    }
    uint32_t c_flag = fix ? F_C_FIX : 0;
    // the factor C as Vulkan reads it: the alpha out (As / 128, put back by the constant) or the constant (FIX / 128)
    VkBlendFactor by_c = fix ? VK_BLEND_FACTOR_CONSTANT_COLOR : VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor by_one_minus_c = fix ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    uint32_t c_out = fix ? 0 : F_ALPHA_FACTOR;
    VkBlendFactor alpha_src = fix ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_CONSTANT_ALPHA;
    if (a == bb) {
        // (A - B) is 0: D alone
        if (d == 0) {
            return b; // Cs
        }
        if (d == 1) {
            b.write_rgb = false; // Cd: the colour kept, the alpha written
            return b;
        }
        b.flags = F_PREMUL | F_C_FIX; // 0: Cs * 0 (FIX read as 0 by the caller's push)
        b.flags |= 0x80000000u;      // (the caller gives FIX 0)
        return b;
    }
    if (a == 0 && bb == 1 && d == 1) { // Cs * C + Cd * (1 - C)
        b.enable = true;
        b.src = by_c;
        b.dst = by_one_minus_c;
        b.src_alpha = alpha_src;
        b.flags = c_out;
        return b;
    }
    if (a == 1 && bb == 0 && d == 0) { // Cd * C + Cs * (1 - C)
        b.enable = true;
        b.src = by_one_minus_c;
        b.dst = by_c;
        b.src_alpha = alpha_src;
        b.flags = c_out;
        return b;
    }
    if (a == 0 && bb == 2) { // Cs * C + D
        b.flags = F_PREMUL | c_flag;
        if (d == 1) {
            b.enable = true;
            b.dst = VK_BLEND_FACTOR_ONE;
        } else if (d == 0) {
            b.enable = true; // Cs * C + Cs: as Cs * (C + 1), not had: Cs * C (counted)
            b.supported = false;
        }
        return b;
    }
    if (a == 2 && bb == 0) { // D - Cs * C
        b.flags = F_PREMUL | c_flag;
        if (d == 1) {
            b.enable = true;
            b.dst = VK_BLEND_FACTOR_ONE;
            b.op = VK_BLEND_OP_REVERSE_SUBTRACT;
        } else {
            b.supported = false; // (0 or Cs less Cs * C: drawn as 0 - nothing)
            b.flags = F_PREMUL | F_C_FIX | 0x80000000u;
        }
        return b;
    }
    if (a == 1 && bb == 2) { // Cd * C + D
        b.enable = true;
        b.src = d == 0 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
        b.dst = by_c;
        b.src_alpha = alpha_src;
        b.flags = c_out;
        b.supported = d != 1; // (Cd * (1 + C): drawn as Cd * C)
        return b;
    }
    // the rest ((Cs - Cd) * C + 0 or + Cs, (Cd - Cs) * C + Cd or + 0, 0 - Cd ...): drawn as Cs * C + Cd * (1 - C)
    b.supported = false;
    b.enable = true;
    b.src = by_c;
    b.dst = by_one_minus_c;
    b.src_alpha = alpha_src;
    b.flags = c_out;
    return b;
}

// ---- Surfaces --------------------------------------------------------------------------------------------------
// A buffer the game draws into: its pages from the base, fbw wide, as many rows of pages as height needs
struct Surface {
    uint32_t base_page = 0, fbw = 0, psm = 0;
    uint32_t height = 0; // GS pixels
    ImageHandle image;   // at the canvas's scale (a depth surface's: with the canvas's samples)
    bool depth = false;
    // MSAA (a colour surface at the canvas's samples above 1): the image drawn into, beside image, which every copy,
    // read and upload uses. ms_ahead: drawn since image was (image made again from it, resolved, before it is read);
    // ms_behind: image written since ms was (ms made again from it, expanded, before it is drawn into). Never both.
    ImageHandle ms;
    bool ms_ahead = false, ms_behind = false;

    uint32_t width() const {
        return std::max(1u, fbw) * 64;
    }
    uint32_t end_page() const {
        return base_page + std::max(1u, fbw) * ((height + page_rows(psm) - 1) / page_rows(psm));
    }
    bool overlaps(uint32_t first, uint32_t end) const {
        return base_page < end && first < end_page();
    }
    // a page's place in the surface, in GS pixels (false when the page is not in it)
    bool page_place(uint32_t page, uint32_t &x, uint32_t &y) const {
        if (page < base_page || page >= end_page()) {
            return false;
        }
        uint32_t p = page - base_page, w = std::max(1u, fbw);
        x = (p % w) * 64;
        y = (p / w) * page_rows(psm);
        return true;
    }
};

// ---- A texture on the GPU ----------------------------------------------------------------------------------------
struct GpuTexture {
    TextureKey key;
    ImageHandle image;
    uint32_t levels = 1;
    bool index = false;
    uint64_t used = 0; // (the frame last drawn with)
};

// A texture as the GPU's sampler reads it: its colours (CLUT and TEXA applied: colour_texels) in RGBA8, found again by
// all it was made from, compared whole
struct SampledTexture {
    TextureKey key;
    uint64_t texa = 0;                   // (TEXA's bits the colours read: TA0, AEM, TA1)
    std::array<uint32_t, 256> clut{};    // (palette textures: the colours; else zeros)
    ImageHandle image;
    uint32_t levels = 1;
    uint64_t used = 0; // (the frame last drawn with)
    uint8_t alpha_lo = 0, alpha_hi = 255; // (its colours' least and most alpha, every level: cut-out AA's)

    bool made_from(const TextureKey &k, uint64_t t, const std::array<uint32_t, 256> &c) const {
        return key == k && texa == t && clut == c;
    }
};

// What a batch draws with: the registers its primitives read, whole
struct DrawState {
    uint64_t frame, zbuf, test, alpha, tex0, tex1, clamp, texa, fogcol, scissor, xyoffset, fba, colclamp, pabe, dthe;
    uint64_t miptbp1, miptbp2;
    uint32_t prim; // (IIP, TME, FGE, ABE, AA1, FST, CTXT: the bits 3-10)
    uint64_t clut_serial;

    bool operator==(const DrawState &) const = default;
};

// The texture store's limits: KF4_PC_RESOLVE_KEPT=0 keeps no resolve's answer (every resolve walks: to compare with)
TextureStoreLimits store_limits() {
    TextureStoreLimits limits;
    const char *kept = getenv("KF4_PC_RESOLVE_KEPT");
    if (kept && strcmp(kept, "0") == 0) {
        limits.answers_kept = 0;
    }
    return limits;
}

} // namespace

class Canvas::Impl {
public:
    Impl(Device &device, GpuTimes &times) : device_(device), times_(times) {
        choose_depth_format();
        make_programs();
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UINT);
        uint32_t zero = 0;
        ImageInitialData data = {&zero, 0, 0};
        dummy_uint_ = device_.create_image(info, &data);
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        dummy_float_ = device_.create_image(info, &data);
        no_table_ = make_table({&dummy_float_->get_view()});
        table_on_ = bool(no_table_.set);
        no_samplers_.fill(nullptr);
        no_samplers_[0] = &device_.get_stock_sampler(StockSampler::NearestClamp);
        const auto &f = device_.get_device_features();
        runs_ok_ = runs_asked_ && table_on_ && f.enabled_features.multiDrawIndirect && f.vk11_features.shaderDrawParameters;
        native_runs_ok_ = runs_ok_ && f.enabled_features.drawIndirectFirstInstance;
        // (the dual-source blend's pictures are the same as the blend's only where it holds a factor as the shader
        // assumes, pcr_prim.h: checked on NVIDIA; elsewhere the runs end where the blend changes, KF4_PC_INDIRECT=plain)
        dual_ok_ = runs_ok_ && f.enabled_features.dualSrcBlend && !runs_plain_ &&
                   device_.get_gpu_properties().vendorID == 0x10DE;
        max_run_draws_ = std::max(device_.get_gpu_properties().limits.maxDrawIndirectCount, 1u);
        if (!exact_z_) {
            fprintf(stderr, "[pcr] exact Z off (%s): decals may lose to the surface under them here and there\n",
                    getenv("KF4_PC_EXACT_Z") ? "KF4_PC_EXACT_Z=0" : "this GPU has no depth clamp");
        }
    }

    ~Impl() {
        warm_stop(); // (its thread reads warm_images_)
        end_pass();
        if (cmd_) {
            times_.end(*cmd_);
            device_.submit(cmd_);
        }
    }

    void set_scale(uint32_t x, uint32_t y) {
        pending_scale_ = {std::clamp(x, 1u, 8u), std::clamp(y, 1u, 8u)};
    }

    void set_msaa(uint32_t samples, bool cutout_aa) {
        pending_samples_ = samples == 2 || samples == 4 || samples == 8 ? samples : 1;
        cutout_aa_ = cutout_aa && sample_shading_;
    }

    void set_between(const Between *between) {
        flush_batch();
        between_ = between;
        if (!between) {
            stop_keeping();
        }
    }

    void stop_keeping() {
        flush_batch();
        if (keeping_) {
            keeping_ = false;
            kept_->read = store_.reads_watched();
            kept_sizes_ = {kept_->steps.size(), kept_->batches.size()};
        }
    }

    // ---- The events ----
    void primitive(const GsRegisterFile &file, const GifPrimitive &p) {
        counts_.primitives++;
        if (p.shape == PrimitiveShape::point || p.shape == PrimitiveShape::line) {
            counts_unsupported_shape_++;
            return;
        }
        DrawState s = state_of(file, p.prim);
        bool tme = bits(p.prim, 4, 1) != 0;
        if (!batch_open_ || batch_.native || !(s == batch_.state) || (tme && !texture_covers(p)) ||
            (batch_.shader_blend && overlaps_batch(p))) {
            flush_batch();
            open_batch(s, p);
        }
        add_primitive(p);
    }

    void native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &nb) {
        counts_.native_batches++;
        counts_.triangles += nb.triangle_count;
        DrawState s = state_of(file, prim);
        bool same = batch_open_ && batch_.native && batch_.program == nb.program && s == batch_.state &&
                    batch_.arena == nb.arena &&
                    batch_.records == nb.records && batch_.triangles == nb.triangles;
        if (!same) {
            flush_batch();
            open_native_batch(s, nb);
        }
        // (a run of triangles by one record: the next batch of the same draw's mesh goes on from where it ends)
        if (!batch_.runs.empty() && batch_.runs.back().instance == nb.instance &&
            batch_.runs.back().first + batch_.runs.back().count == nb.first_triangle) {
            batch_.runs.back().count += nb.triangle_count;
        } else {
            batch_.runs.push_back({nb.first_triangle, nb.triangle_count, nb.instance, nb.picture_instance});
        }
    }

    void texture_written(uint64_t tex0, uint64_t texclut) {
        if (keeping_) {
            kept_->steps.emplace_back(KeptClutLoad{tex0, texclut});
        }
        std::optional<TextureSource> loaded = store_.load_clut(tex0, texclut);
        if (loaded) {
            clut_serial_++;
            if (*loaded != TextureSource::FromUploads) {
                counts_clut_not_uploads_++;
            }
        }
    }

    void upload(const TransferRegisters &t, std::span<const uint8_t> bytes) {
        flush_batch();
        TransferRegs regs = {t.bitbltbuf, t.trxpos, t.trxreg};
        bool taken = store_.upload(regs, bytes);
        write_upload_into_surfaces(t, bytes);
        uint64_t chunk = taken ? store_.last_chunk() : 0;
        std::shared_ptr<const std::vector<uint8_t>> kept_bytes;
        if (keeping_ || !replaying_) {
            // (the store's chunk when it is these bytes, as it is unless the upload had bytes past its pixels)
            kept_bytes = taken ? store_.chunk_bytes(chunk) : nullptr;
            if (!kept_bytes || kept_bytes->size() != bytes.size()) {
                auto copy = std::make_shared<std::vector<uint8_t>>(bytes.size());
                memcpy(copy->data(), bytes.data(), bytes.size()); // (libc++ copies a range a byte at a time)
                kept_bytes = std::move(copy);
            }
        }
        if (keeping_) {
            kept_->steps.emplace_back(KeptUpload{{t, kept_bytes, chunk}});
        }
        if (!replaying_) {
            // (the frame's uploads: what its pictures between ask for again)
            uploads_now_.push_back({t, kept_bytes, chunk});
        }
    }

    // ---- Developers' views (pcr.h's pcr_debug_*) ----
    bool trace_pixel(const PcrTraceSpot &spot) {
        if (spot.x < 0 || spot.y < 0 || spot.sub_x < 0 || spot.sub_y < 0) {
            return false;
        }
        if (!trace_.on) {
            BufferCreateInfo info = {};
            info.domain = BufferDomain::CachedHost;
            info.size = trace_bytes;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            std::vector<uint8_t> zeros(trace_bytes);
            BufferHandle buffer = device_.create_buffer(info, zeros.data());
            if (!buffer) {
                return false;
            }
            trace_buf_ = buffer;
        }
        flush_batch();
        trace_ = Trace();
        trace_.on = true;
        trace_.spot = spot;
        return true;
    }

    void trace_draw(uint32_t draw) {
        flush_batch();
        trace_.draw = draw;
    }

    // In a pass of its own: the traced pixel's samples in the surfaces its last draw drew into
    void trace_probe() {
        flush_batch();
        if (!trace_.on || !trace_.colour || trace_.probes >= trace_probes || !holds(colour_, trace_.colour)) {
            return;
        }
        Surface &colour = *trace_.colour;
        Surface *depth = trace_.depth && holds(depth_, trace_.depth) ? trace_.depth : nullptr;
        const Image &cimage = colour.ms && !colour.ms_behind ? *colour.ms : *colour.image;
        uint32_t samples = uint32_t(depth ? depth->image->get_create_info().samples : cimage.get_create_info().samples);
        bool colour_read = uint32_t(cimage.get_create_info().samples) == samples;
        end_pass();
        CommandBuffer &c = cmd(GpuWork::surface);
        full_barrier(c);
        c.set_program(samples > 1 ? probe_ms_program_ : probe_program_);
        // (a depth surface's view: Granite samples its depth alone, the float view)
        c.set_texture(0, 0, depth ? depth->image->get_view() : cimage.get_view(), StockSampler::NearestClamp);
        c.set_texture(0, 1, colour_read ? cimage.get_view() : depth->image->get_view(),
                      StockSampler::NearestClamp);
        c.set_storage_buffer(0, 2, *trace_buf_);
        struct {
            int32_t xy[2];
            uint32_t draw, slot, samples, has_depth; // (has_depth: 1 a depth surface read, 2 the colour not read)
            float zscale;
        } push = {};
        push.xy[0] = trace_.x;
        push.xy[1] = trace_.y;
        push.draw = trace_.draw;
        push.slot = trace_.probes;
        push.samples = samples;
        push.has_depth = (depth ? 1u : 0u) | (colour_read ? 0u : 2u);
        push.zscale = trace_.zscale;
        c.push_constants(&push, 0, sizeof(push));
        c.dispatch(1, 1, 1);
        full_barrier(c);
        trace_.probes++;
        set0_fresh_ = false; // (a compute program's set 0 bound over the draws')
    }

    // Everything recorded so far done on the GPU, and the trace's records since the last take moved out
    size_t take_trace(std::vector<PcrTraceFragment> &fragments, std::vector<PcrTraceProbe> &probes) {
        fragments.clear();
        probes.clear();
        if (!trace_.on) {
            return 0;
        }
        flush_batch();
        submit();
        device_.wait_idle();
        auto *bytes = static_cast<uint8_t *>(
            device_.map_host_buffer(*trace_buf_, MEMORY_ACCESS_READ_BIT | MEMORY_ACCESS_WRITE_BIT));
        if (!bytes) {
            return 0;
        }
        uint32_t count = 0;
        memcpy(&count, bytes, sizeof(count));
        size_t kept = std::min<size_t>(count, trace_fragments);
        fragments.resize(kept);
        memcpy(fragments.data(), bytes + 16, kept * sizeof(PcrTraceFragment));
        probes.resize(trace_.probes);
        memcpy(probes.data(), bytes + trace_probe_offset, probes.size() * sizeof(PcrTraceProbe));
        memset(bytes, 0, 16);
        device_.unmap_host_buffer(*trace_buf_, MEMORY_ACCESS_READ_BIT | MEMORY_ACCESS_WRITE_BIT);
        trace_.probes = 0;
        return count;
    }

    void show_decal_losses(bool on) {
        flush_batch();
        decal_losses_ = on;
    }

    bool settled() const {
        return pending_samples_ == samples_ && pending_scale_ == scale_;
    }

    void frame_mark(uint64_t value) {
        if (keeping_) {
            kept_->steps.emplace_back(KeptMark{value});
        }
        switch (bits(value, 32, 8)) {
        case 0xF0:
            replaying_ = true;
            break;
        case 0xF1:
            replaying_ = false;
            break;
        case 0xF2:
            keep_frame(bits(value, 0, 9));
            break;
        case 0xF4:
        case 0xF5:
            flush_batch();
            kept_reads_ = bits(value, 32, 8) == 0xF4;
            break;
        }
    }

    void upload_again(const GsRegisterFile &file, uint64_t value) {
        uint32_t k = uint32_t(value);
        const TransferRegisters now = {file.reg[gs_reg::bitbltbuf], file.reg[gs_reg::trxpos], file.reg[gs_reg::trxreg]};
        if (k >= uploads_frame_.size() || uint32_t(uploads_frame_[k].transfer.bitbltbuf >> 32) != uint32_t(value >> 32) ||
            (uploads_frame_[k].transfer.bitbltbuf >> 32) != (now.bitbltbuf >> 32) ||
            uploads_frame_[k].transfer.trxpos != now.trxpos || uploads_frame_[k].transfer.trxreg != now.trxreg) {
            counts_.uploads_again_missed++;
            return;
        }
        upload_made_again(uploads_frame_[k]);
    }

    void local_copy(const TransferRegisters &t) {
        flush_batch();
        if (keeping_) {
            kept_->steps.emplace_back(KeptCopy{t});
        }
        TransferRegs regs = {t.bitbltbuf, t.trxpos, t.trxreg};
        copy_between_surfaces(t);
        store_.copied(regs);
    }

    void local_to_host(const TransferRegisters &t) {
        flush_batch();
        // (what is read back goes to the game: not kept, as the frame's stream leaves it out; a picture between's walk
        // has none)
        TransferRegs regs = {t.bitbltbuf, t.trxpos, t.trxreg};
        fifo_at_ = 0;
        if (store_.read_back(regs, fifo_)) {
            counts_.read_backs_from_uploads++;
            return;
        }
        if (read_back_surface(t)) {
            counts_.read_backs_from_surfaces++;
            return;
        }
        counts_.unsupported_read_backs++;
        fifo_.clear();
    }

    void read_fifo(void *dst, uint32_t qwords) {
        size_t want = size_t(qwords) * 16, have = fifo_.size() - std::min(fifo_at_, fifo_.size());
        size_t n = std::min(want, have);
        if (n) {
            memcpy(dst, fifo_.data() + fifo_at_, n);
        }
        memset(static_cast<uint8_t *>(dst) + n, 0, want - n);
        fifo_at_ += n;
    }

    void forget() {
        flush_batch();
        kept_spoilt_ |= keeping_;
        end_pass();
        uploads_now_.clear();
        uploads_frame_.clear();
        kept_frame_.reset();
        store_.forget();
        colour_.clear();
        depth_.clear();
        textures_.clear();
        sampled_.clear();
        clut_serial_++;
    }

    void vblank() {
        flush_batch();
        if (!uploads_now_.empty()) {
            uploads_frame_.swap(uploads_now_);
            uploads_now_.clear();
        }
        // (the surfaces made again at a new scale before the submit: next_frame_context waits for every command buffer
        // asked for to be submitted, this thread's too, so one left open after it hangs kf4-pc at its next; 2026-11-07,
        // native widescreen switched off with the pump drawing)
        apply_scale();
        submit();
        // (Granite lets go of a frame context's allocations, images and buffers only when frames move on: each
        // vblank, pictures or none, or a run that shows nothing holds all it ever drew)
        device_.next_frame_context();
        frame_++;
        evict_textures();
    }

    std::vector<uint8_t> vram_image() {
        flush_batch();
        std::vector<uint8_t> image = store_.vram_image();
        for (auto &surface : colour_) {
            if (surface->psm != GSN_PSMCT32) {
                continue;
            }
            std::vector<uint32_t> pixels;
            if (!read_surface(*surface, pixels)) {
                continue;
            }
            uint32_t w = surface->width();
            for (uint32_t page = surface->base_page; page < surface->end_page(); page++) {
                uint32_t px, py;
                if (store_.page_source(page) != TextureSource::DrawnSurface || !surface->page_place(page, px, py) ||
                    py + 32 > surface->height) {
                    continue;
                }
                for (uint32_t y = 0; y < 32; y++) {
                    gsn_vram_write_row(image.data(), GSN_PSMCT32, surface->base_page * 32, surface->fbw, px, py + y, 64,
                                       &pixels[size_t(py + y) * w + px]);
                }
            }
        }
        return image;
    }

    TextureStore::ClutState clut_state() const {
        return store_.clut_state();
    }

    void restore(std::span<const uint8_t> vram, const TextureStore::ClutState &clut) {
        forget();
        store_.restore_vram(vram);
        store_.set_clut_state(clut);
    }

    bool scanout(CommandBuffer &out, const ShownRect &rect, const Image &dst) {
        flush_batch();
        // (submitted without a flush: the caller submits out right after, and that puts both on the GPU's queue in
        // one vkQueueSubmit)
        if (rect.w == 0 || rect.h == 0 || rect.psm != GSN_PSMCT32) {
            submit(!submit_together());
            return false;
        }
        Placed placed = colour_surface(rect.bp / 32, rect.bw, GSN_PSMCT32, rect.y + rect.h);
        if (placed.surface) {
            resolve(*placed.surface);
        }
        submit(!submit_together());
        times_.begin(out, GpuWork::picture); // (the drawing submitted: what follows in out is the picture's)
        Surface *s = placed.surface;
        if (!s) {
            return false;
        }
        out.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT);
        uint32_t rx = rect.x + placed.x, ry = rect.y + placed.y;
        int32_t x0 = int32_t(rx * scale_[0]), y0 = int32_t(ry * scale_[1]);
        int32_t x1 = int32_t(std::min(rx + rect.w, s->width()) * scale_[0]);
        int32_t y1 = int32_t(std::min(ry + rect.h, s->height) * scale_[1]);
        if (x1 <= x0 || y1 <= y0) {
            return false;
        }
        if (rect.lower_field && dst.get_height() == 2 * uint32_t(y1 - y0)) {
            scanout_lower_field(out, *s->image, x0, y0, x1, y1, dst);
            return true;
        }
        if (s->image->get_format() != dst.get_format()) {
            scanout_narrow(out, *s->image, {x0, y0, x1 - x0, y1 - y0},
                           {0, 0, int32_t(dst.get_width()), int32_t(dst.get_height())}, dst);
            return true;
        }
        if (uint32_t(x1 - x0) == dst.get_width() && uint32_t(y1 - y0) == dst.get_height()) {
            const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            out.copy_image(dst, *s->image, {0, 0, 0}, {x0, y0, 0}, {dst.get_width(), dst.get_height(), 1}, colour,
                           colour);
        } else {
            out.blit_image(dst, *s->image, {0, 0, 0}, {int32_t(dst.get_width()), int32_t(dst.get_height()), 1},
                           {x0, y0, 0}, {x1 - x0, y1 - y0, 1}, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
        }
        return true;
    }

    // Colour high: a rectangle of a 16-bit surface (x, y, width, height) stretched over a rectangle of the 8-bit picture
    // dst, dithered (pcr_narrow.frag); what dst has outside that rectangle is kept
    void scanout_narrow(CommandBuffer &out, const Image &src, std::array<int32_t, 4> from, std::array<int32_t, 4> to,
                        const Image &dst) {
        RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &dst.get_view();
        rp.store_attachments = 1;
        rp.load_attachments = 1;
        rp.render_area = {{to[0], to[1]}, {uint32_t(to[2]), uint32_t(to[3])}};
        out.begin_render_pass(rp);
        out.set_program(narrow_program_);
        out.set_opaque_state();
        out.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        out.set_cull_mode(VK_CULL_MODE_NONE);
        out.set_depth_test(false, false);
        out.set_stencil_test(false);
        out.set_blend_enable(false);
        out.set_color_write_mask(0xF);
        VkViewport vp = {0, 0, float(dst.get_width()), float(dst.get_height()), 0, 1};
        out.set_viewport(vp);
        out.set_scissor(rp.render_area);
        out.set_texture(0, 0, src.get_view(), StockSampler::NearestClamp);
        int32_t push[8] = {from[0], from[1], from[2], from[3], to[2], to[3], to[0], to[1]};
        out.push_constants(push, 0, sizeof(push));
        out.draw(3);
        out.end_render_pass();
        out.barrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    // A lower field's rows x0..x1, y0..y1 of src stretched to twice their height into dst, a line of the picture lower
    // (its rows each twice, from the picture's row of the line, dst's height over the rect's lines; the line above
    // them black, and what falls below dst left out)
    void scanout_lower_field(CommandBuffer &out, const Image &src, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             const Image &dst) {
        int32_t height = int32_t(dst.get_height());
        int32_t down_rows = std::max<int32_t>(1, int32_t(scale_[1] + 0.5f)); // (a line of the picture: a field's half)
        int32_t src_rows = (height - down_rows) / 2;
        VkClearValue black = {};
        out.clear_image(dst, black);
        out.barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (src_rows <= 0 || y0 + src_rows > y1) {
            return;
        }
        if (src.get_format() != dst.get_format()) {
            out.barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            scanout_narrow(out, src, {x0, y0, x1 - x0, src_rows}, {0, down_rows, int32_t(dst.get_width()), 2 * src_rows},
                           dst); // (colour high)
            return;
        }
        out.blit_image(dst, src, {0, down_rows, 0}, {int32_t(dst.get_width()), 2 * src_rows, 1}, {x0, y0, 0},
                       {x1 - x0, src_rows, 1}, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
    }

    // What is recorded, on the GPU's queue now: Granite holds a submit without a fence until the next frame context,
    // so the GPU would start this thread's work only then (the pump's: up to a vblank later, kf4-pc's next picture).
    // flush false: handed to Granite but left for the next submit that flushes (one the caller makes at once).
    void submit(bool flush = true) {
        flush_run();
        end_pass();
        if (runs_.buffer) {
            device_.unmap_host_buffer(*runs_.buffer, MEMORY_ACCESS_WRITE_BIT); // (the records so far seen by the GPU)
        }
        if (cmd_) {
            times_.end(*cmd_);
            device_.submit(cmd_);
            if (flush) {
                device_.flush_frame();
            }
            cmd_.reset();
        }
    }

    CanvasCounts counts() const {
        CanvasCounts c = counts_;
        return c;
    }

    uint64_t unsupported_shapes() const {
        return counts_unsupported_shape_;
    }

private:
    // ---- The command buffer and its render pass ----
    CommandBuffer &cmd() {
        flush_run(); // (what the caller records follows the run's draws)
        if (!cmd_) {
            cmd_ = device_.request_command_buffer();
            set0_fresh_ = false;
            table_bound_ = VK_NULL_HANDLE;
            cmd_->set_dynamic_state(dynamic_.ds, dynamic_.blend);
            pass_ended_ = false; // (what was submitted before: begin_pass's full barrier)
        }
        return *cmd_;
    }

    // Vulkan's dynamic state for the draws (cmake/granite_dynamic_state.cmake), as the GS has it: depth,
    // stencil, culling and blend constants where the device has Vulkan 1.3; blending and the colour write mask too
    // where it has VK_EXT_extended_dynamic_state3's. A batch that differs from the one before only there (native
    // 280's two blends, a DATE batch's stencil passes) then binds no pipeline of its own. KF4_PC_DYNAMIC=0: none
    // (the pipelines as before, to compare); 1: not the blending and the write mask.
    struct DynamicChoice {
        bool ds = false, blend = false;
    };
    static DynamicChoice choose_dynamic(const Device &device) {
        const char *v = getenv("KF4_PC_DYNAMIC");
        int want = v ? atoi(v) : 2;
        const auto &f = device.get_device_features();
        DynamicChoice d;
        d.ds = want >= 1 && f.device_api_core_version >= VK_API_VERSION_1_3;
        d.blend = d.ds && want >= 2 && f.eds3_features.extendedDynamicState3ColorBlendEnable &&
                  f.eds3_features.extendedDynamicState3ColorBlendEquation &&
                  f.eds3_features.extendedDynamicState3ColorWriteMask;
        return d;
    }

    // The command buffer, what is recorded into it from here on timed as work's
    CommandBuffer &cmd(GpuWork work) {
        times_.begin(cmd(), work);
        return *cmd_;
    }

    void full_barrier(CommandBuffer &c) {
        c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        pass_ended_ = false;
    }

    // The barriers around a render pass. Everything recorded outside a pass (copies, clears, uploads, resolves, the
    // picture's scanout) is between full barriers of its own, so a pass ended needs none after it, and one begun needs
    // a full barrier only after such work (or at the start of a command buffer: what was submitted before). A pass
    // that follows a pass, nothing recorded between, waits only for the attachment writes before it: its vertex work
    // then runs while the pass before shades its last pixels, where a full barrier emptied the GPU between them (at
    // 009 a picture has about ten pass changes: the reflection's target, DATE's set-up, the pump's submits).
    // KF4_PC_PASS_BARRIER=full: the full barrier after every pass and before every pass, as before (to compare).
    void end_pass() {
        flush_run();
        if (pass_colour_) {
            cmd().end_render_pass();
            if (full_pass_barriers_) {
                full_barrier(cmd());
            } else {
                pass_ended_ = true;
            }
            pass_colour_ = nullptr;
            pass_depth_ = nullptr;
            pass_reads_ = PassReads::nothing;
        }
    }

    // A pass after a pass: the attachment writes before (colour, depth and stencil, and vkCmdClearAttachments') made
    // visible to the attachment reads and writes after, and to DATE's input attachment read
    void pass_to_pass_barrier(CommandBuffer &c) {
        VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        device_.get_device_table().vkCmdPipelineBarrier2(c.get_command_buffer(), &dep);
        pass_ended_ = false;
    }

    // What the draws of a pass read of its colour target in the pass (an input attachment of itself: DATE's set-up)
    enum class PassReads { nothing, target };

    // The pass drawing into colour (and depth): the open one when it is that, else a new one
    void begin_pass(Surface &colour, Surface *depth, PassReads reads) {
        if (pass_colour_ == &colour && (depth == nullptr || pass_depth_ == depth) &&
            (reads == PassReads::nothing || pass_reads_ == reads)) {
            return;
        }
        end_pass();
        RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = colour.ms ? &colour.ms->get_view() : &colour.image->get_view();
        rp.load_attachments = 1;
        rp.store_attachments = 1;
        if (depth) {
            rp.depth_stencil = &depth->image->get_view();
            rp.op_flags = RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT | RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
        }
        RenderPassInfo::Subpass subpass = {};
        if (reads == PassReads::target) {
            subpass.num_color_attachments = 1;
            subpass.color_attachments[0] = 0;
            subpass.num_input_attachments = 1;
            subpass.input_attachments[0] = 0;
            subpass.depth_stencil_mode =
                depth ? RenderPassInfo::DepthStencil::ReadWrite : RenderPassInfo::DepthStencil::None;
            rp.subpasses = &subpass;
            rp.num_subpasses = 1;
        }
        CommandBuffer &c = cmd();
        if (pass_ended_) {
            pass_to_pass_barrier(c);
        } else {
            full_barrier(c);
        }
        c.begin_render_pass(rp);
        set0_fresh_ = false; // (Granite forgets the bindings at a render pass)
        pass_colour_ = &colour;
        pass_depth_ = depth;
        pass_reads_ = reads;
        counts_.passes++;
    }

    // In the pass: the colour target's writes so far before the input attachment reads after (the subpass's own
    // dependency on itself)
    void in_pass_barrier(CommandBuffer &c) {
        VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        device_.get_device_table().vkCmdPipelineBarrier2(c.get_command_buffer(), &dep);
    }

    // ---- Surfaces ----
    ImageHandle make_surface_image(uint32_t width, uint32_t height, bool depth) {
        ImageCreateInfo info = ImageCreateInfo::render_target(width * scale_[0], height * scale_[1],
                                                              depth ? depth_format_ : colour_format());
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                            : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT); // (DATE's set-up reads it: begin_pass)
        if (trace_.on) {
            info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT; // (the trace's probe reads the depth surfaces)
        }
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        if (depth && samples_ > 1) {
            info.samples = VkSampleCountFlagBits(samples_); // (drawn only: never read, so never resolved)
        }
        ImageHandle image = device_.create_image(info, nullptr);
        if (!image) {
            fprintf(stderr, "[pcr] no image for a %ux%u surface\n", width, height);
        }
        return image;
    }

    // ---- MSAA: a colour surface's multisampled image (Surface::ms) ----
    ImageHandle make_ms_image(uint32_t width, uint32_t height) {
        ImageCreateInfo info = ImageCreateInfo::render_target(width, height, colour_format());
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                     (trace_.on ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u); // (the trace's probe reads it)
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        info.samples = VkSampleCountFlagBits(samples_);
        ImageHandle image = device_.create_image(info, nullptr);
        if (!image) {
            fprintf(stderr, "[pcr] no multisampled image for a %ux%u surface\n", width, height);
        }
        return image;
    }

    // Before a colour surface is drawn into: its multisampled image there and as image is (made, or expanded where
    // image was written since). False without MSAA (or without memory for it: drawn into image as at 1x).
    bool ensure_ms(Surface &s) {
        if (samples_ <= 1 || s.depth) {
            return false;
        }
        if (s.ms && (s.ms->get_width() != s.image->get_width() || s.ms->get_height() != s.image->get_height())) {
            resolve(s);
            s.ms.reset();
        }
        if (!s.ms) {
            s.ms = make_ms_image(s.image->get_width(), s.image->get_height());
            if (!s.ms) {
                return false;
            }
            s.ms_ahead = false;
            s.ms_behind = true;
        }
        if (s.ms_behind) {
            expand(s, {{0, 0}, {s.image->get_width(), s.image->get_height()}});
            s.ms_behind = false;
            counts_msaa_.expands++;
        }
        return true;
    }

    // Before a colour surface's image is read whole (or written where it was drawn): what was drawn into its
    // multisampled image resolved into it
    void resolve(Surface &s) {
        if (!s.ms || !s.ms_ahead) {
            return;
        }
        end_pass();
        CommandBuffer &c = cmd(GpuWork::copy);
        full_barrier(c);
        resolve_rect(c, *s.image, {0, 0, 0}, *s.ms, {0, 0, 0}, {s.image->get_width(), s.image->get_height(), 1});
        full_barrier(c);
        s.ms_ahead = false;
        counts_msaa_.resolves++;
    }

    void resolve_rect(CommandBuffer &c, const Image &dst, VkOffset3D dst_at, const Image &ms, VkOffset3D src_at,
                      VkExtent3D extent) {
        VkImageResolve r = {};
        r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.dstSubresource = r.srcSubresource;
        r.srcOffset = src_at;
        r.dstOffset = dst_at;
        r.extent = extent;
        device_.get_device_table().vkCmdResolveImage(c.get_command_buffer(), ms.get_image(), VK_IMAGE_LAYOUT_GENERAL,
                                                     dst.get_image(), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }

    // A rectangle of a colour surface's pixels (at the canvas's scale) copied into dst at dst_at: from its image, or
    // resolved from its multisampled image when that was drawn since (outside a render pass, barriers the caller's)
    void copy_out(CommandBuffer &c, const Image &dst, VkOffset3D dst_at, const Surface &s, VkOffset3D src_at,
                  VkExtent3D extent) {
        if (s.ms && s.ms_ahead) {
            resolve_rect(c, dst, dst_at, *s.ms, src_at, extent);
            counts_msaa_.rect_resolves++;
            return;
        }
        const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.copy_image(dst, *s.image, dst_at, src_at, extent, colour, colour);
    }

    // After a colour surface's image was written over a rectangle (at the canvas's scale): the same pixels into every
    // sample of its multisampled image, unless that is to be expanded whole anyway (ms_behind)
    void wrote_image(Surface &s, const VkRect2D &rect) {
        if (!s.ms || s.ms_behind || rect.extent.width == 0 || rect.extent.height == 0) {
            return;
        }
        expand(s, rect);
        counts_msaa_.rect_expands++;
    }

    // image's pixels over rect into every sample of ms (a pass of its own, image read by pcr_expand.frag)
    void expand(Surface &s, const VkRect2D &rect) {
        end_pass();
        CommandBuffer &c = cmd(GpuWork::surface);
        full_barrier(c);
        RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &s.ms->get_view();
        rp.load_attachments = 1;
        rp.store_attachments = 1;
        rp.render_area = rect;
        c.begin_render_pass(rp);
        set0_fresh_ = false;
        c.set_program(expand_program_);
        c.set_opaque_state();
        c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        c.set_cull_mode(VK_CULL_MODE_NONE);
        c.set_depth_test(false, false);
        c.set_stencil_test(false);
        c.set_blend_enable(false);
        c.set_color_write_mask(0xF);
        VkViewport vp = {0, 0, float(s.ms->get_width()), float(s.ms->get_height()), 0, 1};
        c.set_viewport(vp);
        c.set_scissor(rect);
        c.set_texture(0, 0, s.image->get_view(), StockSampler::NearestClamp);
        c.draw(3);
        c.end_render_pass();
        full_barrier(c);
    }

    static uint32_t rounded_height(uint32_t height, uint32_t psm) {
        uint32_t rows = page_rows(psm);
        return std::clamp((height + rows - 1) / rows * rows, rows, 2048u);
    }

    // A frame's place: the surface it draws into and where in it its base page is (GS pixels)
    struct Placed {
        Surface *surface = nullptr;
        uint32_t x = 0, y = 0;
    };

    // The colour surface a frame at this base page and width draws into, height rows of it (grown as needed). The
    // game draws into buffers that overlap (two 128-wide targets a page row apart): a base inside a surface of the
    // same width, at the start of a row of its pages, is that surface at an offset. A surface made takes in those of
    // its width it overlaps from its base on (their pixels copied to their place in it); every other colour surface
    // it overlaps is let go (the GS's memory there is this one's now). It starts as the uploads left its pages.
    Placed colour_surface(uint32_t base_page, uint32_t fbw, uint32_t psm, uint32_t height) {
        for (auto &s : colour_) {
            uint32_t x, y;
            if (s->fbw == fbw && s->page_place(base_page, x, y) && x == 0) {
                uint32_t needed = rounded_height(y + height, psm);
                if (s->height < needed) {
                    Surface *grown = s.get(); // (the list's elements move when take_in_below lets others go)
                    uint32_t old_end = grown->end_page();
                    grow(*grown, needed);
                    take_in_below(*grown, old_end);
                    grown->psm = psm;
                    return {grown, 0, y};
                }
                s->psm = psm;
                return {s.get(), 0, y};
            }
        }
        auto made = std::make_unique<Surface>();
        made->base_page = base_page;
        made->fbw = fbw;
        made->psm = psm;
        made->height = rounded_height(height, psm);
        // (those it takes in: as tall as their place and height)
        std::vector<std::pair<Surface *, uint32_t>> taken;
        for (auto &o : colour_) {
            uint32_t rows = page_rows(psm), w = std::max(1u, fbw);
            if (o->fbw == fbw && o->psm == psm && o->base_page >= base_page && (o->base_page - base_page) % w == 0 &&
                o->overlaps(base_page, made->end_page())) {
                uint32_t y = (o->base_page - base_page) / w * rows;
                made->height = std::max(made->height, rounded_height(y + o->height, psm));
                taken.emplace_back(o.get(), y);
            }
        }
        made->image = make_surface_image(made->width(), made->height, false);
        if (!made->image) {
            return {};
        }
        counts_.surfaces_made++;
        end_pass();
        CommandBuffer &c = cmd(GpuWork::surface);
        VkClearValue zero = {};
        c.clear_image(*made->image, zero);
        full_barrier(c);
        fill_from_uploads(*made);
        // (as much of each as fits: a surface is at most 2048 rows, and one taken in a few rows down may be that tall
        // already; copied whole, the copy ran past the image's end and NVIDIA lost the device: the under-water screen,
        // 2026-11-11)
        for (auto [o, y] : taken) {
            uint32_t top = y * scale_[1], rows = made->image->get_height() - std::min(top, made->image->get_height());
            rows = std::min(rows, o->image->get_height());
            if (rows != 0) {
                copy_out(c, *made->image, {0, int32_t(top), 0}, *o, {0, 0, 0}, {o->image->get_width(), rows, 1});
            }
        }
        full_barrier(c);
        let_go_overlapping(colour_, made->base_page, made->end_page());
        colour_.push_back(std::move(made));
        return {colour_.back().get(), 0, 0};
    }

    Surface *depth_surface(uint32_t base_page, uint32_t fbw, uint32_t zpsm, uint32_t height) {
        height = rounded_height(height, zpsm);
        for (auto &s : depth_) {
            if (s->base_page == base_page && s->fbw == fbw && s->psm == zpsm) {
                if (s->height < height) {
                    grow(*s, height);
                }
                return s.get();
            }
        }
        auto made = std::make_unique<Surface>();
        made->base_page = base_page;
        made->fbw = fbw;
        made->psm = zpsm;
        made->height = height;
        made->depth = true;
        made->image = make_surface_image(made->width(), height, true);
        if (!made->image) {
            return nullptr;
        }
        counts_.surfaces_made++;
        end_pass();
        VkClearValue zero = {};
        cmd(GpuWork::surface).clear_image(*made->image, zero, VK_IMAGE_ASPECT_DEPTH_BIT);
        full_barrier(cmd());
        depth_.push_back(std::move(made));
        return depth_.back().get();
    }

    void let_go_overlapping(std::vector<std::unique_ptr<Surface>> &list, uint32_t first, uint32_t end) {
        for (auto it = list.begin(); it != list.end();) {
            if ((*it)->overlaps(first, end)) {
                if (pass_colour_ == it->get() || pass_depth_ == it->get()) {
                    end_pass();
                }
                it = list.erase(it);
            } else {
                ++it;
            }
        }
    }

    // A surface made taller: a new image, the old one's pixels copied in. (The open pass ended whatever it draws
    // into: copies and clears are not allowed in a render pass, and recorded in one, RADV lost the pass's draws.)
    void grow(Surface &s, uint32_t height) {
        end_pass();
        ImageHandle bigger = make_surface_image(s.width(), height, s.depth);
        if (!bigger) {
            return;
        }
        // (the multisampled image grown with it, its samples copied; without memory for it, resolved and let go: drawn
        // at 1x until ensure_ms makes one again)
        ImageHandle bigger_ms;
        if (s.ms) {
            bigger_ms = make_ms_image(bigger->get_width(), bigger->get_height());
            if (!bigger_ms) {
                resolve(s);
            }
        }
        CommandBuffer &c = cmd(GpuWork::surface);
        VkClearValue zero = {};
        if (s.depth) {
            c.clear_image(*bigger, zero, VK_IMAGE_ASPECT_DEPTH_BIT);
        } else {
            c.clear_image(*bigger, zero);
        }
        if (bigger_ms) {
            c.clear_image(*bigger_ms, zero);
        }
        full_barrier(c);
        VkImageSubresourceLayers layers = {VkImageAspectFlags(s.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                                           0, 1};
        c.copy_image(*bigger, *s.image, {0, 0, 0}, {0, 0, 0}, {s.image->get_width(), s.image->get_height(), 1}, layers,
                     layers);
        if (bigger_ms) {
            c.copy_image(*bigger_ms, *s.ms, {0, 0, 0}, {0, 0, 0}, {s.ms->get_width(), s.ms->get_height(), 1}, layers,
                         layers);
        }
        full_barrier(c);
        s.image = bigger;
        s.ms = bigger_ms;
        if (!s.ms) {
            s.ms_ahead = s.ms_behind = false;
        }
        s.height = height;
    }

    // A colour surface grown over pages from old_end on: the colour surfaces that begin in those pages are its now, as
    // a surface made takes in those it overlaps. Those of its width and format, at the start of a row of its pages,
    // copied to their place in it (as much as fits); every one let go. Kept, they held a second, older picture of the
    // same memory, found first by the base pages inside them: play grows the frame's surface (base 0) over the title's
    // second field buffer (page 70), the menu draws into page 70 (that old surface, grown), and the first picture of
    // the menu, showing page 140 (the frame's other buffer, in both), came from the old surface: black (2026-11-11).
    void take_in_below(Surface &grown, uint32_t old_end) {
        uint32_t end = grown.end_page(), w = std::max(1u, grown.fbw), rows = page_rows(grown.psm);
        bool any = false;
        for (auto &o : colour_) {
            if (o.get() == &grown || o->base_page < old_end || o->base_page >= end) {
                continue;
            }
            any = true;
            if (o->fbw != grown.fbw || o->psm != grown.psm || (o->base_page - grown.base_page) % w != 0) {
                continue;
            }
            uint32_t top = (o->base_page - grown.base_page) / w * rows * scale_[1];
            uint32_t height = grown.image->get_height() - std::min(top, grown.image->get_height());
            height = std::min(height, o->image->get_height());
            if (height == 0) {
                continue;
            }
            end_pass();
            CommandBuffer &c = cmd(GpuWork::surface);
            full_barrier(c);
            copy_out(c, *grown.image, {0, int32_t(top), 0}, *o, {0, 0, 0}, {o->image->get_width(), height, 1});
            full_barrier(c);
            wrote_image(grown, {{0, int32_t(top)}, {o->image->get_width(), height}}); // (and into its samples)
        }
        if (!any) {
            return;
        }
        for (auto it = colour_.begin(); it != colour_.end();) {
            Surface *o = it->get();
            if (o != &grown && o->base_page >= old_end && o->base_page < end) {
                if (pass_colour_ == o) {
                    end_pass();
                }
                it = colour_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The surface's pages as the uploads left them (those wholly an upload's): read from the texture store a page
    // at a time, written in at the canvas's scale
    void fill_from_uploads(Surface &s) {
        if (s.psm != GSN_PSMCT32) {
            return;
        }
        uint32_t w = s.width(), h = s.height;
        std::vector<uint32_t> pixels;
        bool any = false;
        for (uint32_t page = s.base_page; page < s.end_page(); page++) {
            TextureRegs regs;
            regs.tex0 = uint64_t(page * 32) | uint64_t(1) << 14 | uint64_t(GSN_PSMCT32) << 20 | uint64_t(6) << 26 |
                        uint64_t(5) << 30;
            TextureAnswer a = store_.resolve(regs, 64, 32);
            if (a.source != TextureSource::FromUploads || !a.texture) {
                continue;
            }
            uint32_t px, py;
            if (!s.page_place(page, px, py) || py + 32 > h) {
                continue;
            }
            if (!any) {
                pixels.assign(size_t(w) * h, 0);
                any = true;
            }
            for (uint32_t y = 0; y < 32; y++) {
                memcpy(&pixels[size_t(py + y) * w + px], &a.texture->rgba[size_t(y) * 64], 64 * 4);
            }
        }
        if (!any) {
            return;
        }
        counts_.surfaces_filled_from_uploads++;
        write_pixels(s, 0, 0, w, h, pixels.data());
    }

    // GS pixels (RGBA8, w x h) into a colour surface at (x, y), each repeated at the canvas's scale. (The open pass
    // ended whatever it draws into, as grow's: an upload into a small target while the frame's pass was open lost
    // the frame's draws around it, 2026-11-07: objects, doors and the HUD's gauge missing.)
    void write_pixels(Surface &s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels) {
        end_pass();
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(w, h, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL; // (the blit reads it GENERAL: left READ_ONLY_OPTIMAL, it was not)
        ImageInitialData data = {pixels, 0, 0};
        ImageHandle staged = device_.create_image(info, &data);
        if (!staged) {
            return;
        }
        CommandBuffer &c = cmd(GpuWork::upload);
        full_barrier(c);
        int32_t dx = int32_t(x * scale_[0]), dy = int32_t(y * scale_[1]);
        int32_t dw = int32_t(std::min(w, s.width() - std::min(x, s.width())) * scale_[0]);
        int32_t dh = int32_t(std::min(h, s.height - std::min(y, s.height)) * scale_[1]);
        if (dw <= 0 || dh <= 0) {
            return;
        }
        c.blit_image(*s.image, *staged, {dx, dy, 0}, {dw, dh, 1}, {0, 0, 0},
                     {int32_t(dw / scale_[0]), int32_t(dh / scale_[1]), 1}, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
        full_barrier(c);
        wrote_image(s, {{dx, dy}, {uint32_t(dw), uint32_t(dh)}});
    }

    // An upload into the GS's memory where a colour surface is: its pixels written into the surface too
    void write_upload_into_surfaces(const TransferRegisters &t, std::span<const uint8_t> bytes) {
        uint32_t dbp = bits(t.bitbltbuf, 32, 14), dbw = bits(t.bitbltbuf, 48, 6), dpsm = bits(t.bitbltbuf, 56, 6);
        uint32_t x = bits(t.trxpos, 32, 11), y = bits(t.trxpos, 48, 11);
        uint32_t w = bits(t.trxreg, 0, 12), h = bits(t.trxreg, 32, 12);
        if (w == 0 || h == 0) {
            return;
        }
        uint32_t first = dbp / 32, end = first + std::max(1u, dbw) * ((y + h + 31) / 32 + 1);
        for (auto &s : colour_) {
            if (!s->overlaps(first, end)) {
                continue;
            }
            uint32_t px, py;
            if (dpsm != GSN_PSMCT32 || s->psm != GSN_PSMCT32 || dbp % 32 != 0 || dbw != s->fbw ||
                !s->page_place(first, px, py) || bytes.size() < size_t(w) * h * 4) {
                counts_unsupported_upload_++;
                continue;
            }
            std::vector<uint32_t> pixels(size_t(w) * h);
            memcpy(pixels.data(), bytes.data(), pixels.size() * 4);
            uint32_t ux = px + x, uy = py + y;
            if (uy + h > s->height) {
                grow(*s, rounded_height(uy + h, s->psm));
            }
            write_pixels(*s, ux, uy, w, h, pixels.data());
            counts_.uploads_into_surfaces++;
        }
    }

    // A local copy from a colour surface: into the colour surface at the destination (made when there is none),
    // pixel for pixel at the canvas's scale. Formats other than CT32 to CT32 are counted, not made.
    void copy_between_surfaces(const TransferRegisters &t) {
        uint32_t sbp = bits(t.bitbltbuf, 0, 14), sbw = bits(t.bitbltbuf, 16, 6), spsm = bits(t.bitbltbuf, 24, 6);
        uint32_t dbp = bits(t.bitbltbuf, 32, 14), dbw = bits(t.bitbltbuf, 48, 6), dpsm = bits(t.bitbltbuf, 56, 6);
        uint32_t sx = bits(t.trxpos, 0, 11), sy = bits(t.trxpos, 16, 11);
        uint32_t dx = bits(t.trxpos, 32, 11), dy = bits(t.trxpos, 48, 11);
        uint32_t w = bits(t.trxreg, 0, 12), h = bits(t.trxreg, 32, 12);
        Surface *from = nullptr;
        uint32_t fx = 0, fy = 0;
        auto find_from = [&] {
            from = nullptr;
            for (auto &s : colour_) {
                if (s->fbw == sbw && sbp % 32 == 0 && s->page_place(sbp / 32, fx, fy)) {
                    from = s.get();
                    break;
                }
            }
        };
        find_from();
        if (!from || w == 0 || h == 0) {
            return; // (uploads': the texture store's)
        }
        if (spsm != GSN_PSMCT32 || dpsm != GSN_PSMCT32 || dbp % 32 != 0) {
            counts_.unsupported_copies++;
            return;
        }
        Placed placed = colour_surface(dbp / 32, dbw, GSN_PSMCT32, dy + h);
        Surface *to = placed.surface;
        find_from(); // (the destination made or grown may have taken the source in and let it go)
        if (!to || !from) {
            return;
        }
        uint32_t tx = placed.x, ty = placed.y;
        end_pass();
        CommandBuffer &c = cmd(GpuWork::copy);
        full_barrier(c);
        uint32_t cw = std::min({w, from->width() - std::min(fx + sx, from->width()), to->width() - std::min(tx + dx, to->width())});
        uint32_t ch = std::min({h, from->height - std::min(fy + sy, from->height), to->height - std::min(ty + dy, to->height)});
        if (cw == 0 || ch == 0) {
            return;
        }
        if (from == to) {
            counts_.unsupported_copies++; // (within one surface: overlapping, as the GS orders it; not made)
            return;
        }
        VkOffset3D to_at = {int32_t((tx + dx) * scale_[0]), int32_t((ty + dy) * scale_[1]), 0};
        copy_out(c, *to->image, to_at, *from, {int32_t((fx + sx) * scale_[0]), int32_t((fy + sy) * scale_[1]), 0},
                 {cw * scale_[0], ch * scale_[1], 1});
        full_barrier(c);
        wrote_image(*to, {{to_at.x, to_at.y}, {cw * scale_[0], ch * scale_[1]}});
        counts_.copies_between_surfaces++;
    }

    // A rectangle of a colour surface's GS pixels (the middle of each at the canvas's scale) read from the GPU, as
    // RGBA8 (w x h). Waits for the GPU.
    bool read_pixels(Surface &from, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint32_t> &out) {
        ImageCreateInfo info = ImageCreateInfo::render_target(w, h, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageHandle one = device_.create_image(info, nullptr);
        BufferCreateInfo binfo = {};
        binfo.domain = BufferDomain::CachedHost;
        binfo.size = VkDeviceSize(w) * h * 4;
        binfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        BufferHandle buffer = device_.create_buffer(binfo, nullptr);
        if (!one || !buffer) {
            return false;
        }
        resolve(from);
        end_pass();
        CommandBuffer &c = cmd(GpuWork::copy);
        full_barrier(c);
        int32_t x0 = int32_t(x * scale_[0]), y0 = int32_t(y * scale_[1]);
        c.blit_image(*one, *from.image, {0, 0, 0}, {int32_t(w), int32_t(h), 1}, {x0, y0, 0},
                     {int32_t(w * scale_[0]), int32_t(h * scale_[1]), 1}, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
        full_barrier(c);
        c.copy_image_to_buffer(*buffer, *one, 0, {0, 0, 0}, {w, h, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        c.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_HOST_READ_BIT);
        Fence fence;
        times_.end(*cmd_);
        device_.submit(cmd_, &fence);
        cmd_.reset();
        fence->wait();
        const uint8_t *mapped = static_cast<const uint8_t *>(device_.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
        if (!mapped) {
            return false;
        }
        out.resize(size_t(w) * h);
        memcpy(out.data(), mapped, out.size() * 4);
        device_.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
        return true;
    }

    bool read_surface(Surface &s, std::vector<uint32_t> &out) {
        return read_pixels(s, 0, 0, s.width(), s.height, out);
    }

    // A read-back of a drawn CT32 surface: its GS pixels (the middle of each at the canvas's scale) read from the GPU
    bool read_back_surface(const TransferRegisters &t) {
        uint32_t sbp = bits(t.bitbltbuf, 0, 14), sbw = bits(t.bitbltbuf, 16, 6), spsm = bits(t.bitbltbuf, 24, 6);
        uint32_t sx = bits(t.trxpos, 0, 11), sy = bits(t.trxpos, 16, 11);
        uint32_t w = bits(t.trxreg, 0, 12), h = bits(t.trxreg, 32, 12);
        if (spsm != GSN_PSMCT32 || sbp % 32 != 0 || w == 0 || h == 0) {
            return false;
        }
        Surface *from = nullptr;
        uint32_t fx = 0, fy = 0;
        for (auto &s : colour_) {
            if (s->fbw == sbw && s->page_place(sbp / 32, fx, fy)) {
                from = s.get();
                break;
            }
        }
        if (!from || fx + sx + w > from->width() || fy + sy + h > from->height) {
            return false;
        }
        std::vector<uint32_t> pixels;
        if (!read_pixels(*from, fx + sx, fy + sy, w, h, pixels)) {
            return false;
        }
        fifo_.assign((size_t(w) * h * 4 + 15) / 16 * 16, 0);
        memcpy(fifo_.data(), pixels.data(), pixels.size() * 4);
        return true;
    }

    // What a primitive of PRIM's bits prim draws with
    DrawState state_of(const GsRegisterFile &file, uint32_t prim) const {
        const auto &r = file.reg;
        uint32_t ctxt = bits(prim, 9, 1);
        return {r[gs_reg::frame_1 + ctxt], r[gs_reg::zbuf_1 + ctxt], r[gs_reg::test_1 + ctxt],
                r[gs_reg::alpha_1 + ctxt], r[gs_reg::tex0_1 + ctxt] & ~(uint64_t(7) << 61),
                r[gs_reg::tex1_1 + ctxt], r[gs_reg::clamp_1 + ctxt], r[gs_reg::texa], r[gs_reg::fogcol],
                r[gs_reg::scissor_1 + ctxt], r[gs_reg::xyoffset_1 + ctxt], r[gs_reg::fba_1 + ctxt],
                r[gs_reg::colclamp], r[gs_reg::pabe], r[gs_reg::dthe], r[gs_reg::miptbp1_1 + ctxt],
                r[gs_reg::miptbp2_1 + ctxt], prim & 0x7F8u, clut_serial_};
    }

    // ---- Textures ----
    struct BoundTexture {
        const ImageView *view = nullptr; // (u_texels; u_surface for SRC_SURFACE; u_sampled for SRC_SAMPLED)
        const Sampler *sampler = nullptr; // (SRC_SAMPLED: the GS's filters and wrap modes as the sampler's)
        uint32_t src = SRC_CT32;
        uint32_t levels = 1;
        uint32_t cols = 0, rows = 0; // the part of level 0 the batch may read
        ImageHandle surface_copy;     // (a surface's copy: kept until the batch is drawn)
        ImageHandle held;             // (a decoded texture's image: held while a kept walk may draw with it)
        uint32_t sampling = 0;        // the pushes' tex1 bits 10-31 (pcr_common.h): texture detail and anisotropy
        bool ok = false;
        uint8_t alpha_lo = 0, alpha_hi = 255; // (the texels' least and most alpha where known: cut-out AA's)
    };

    static uint32_t source_of(uint32_t psm) {
        switch (psm) {
        case GSN_PSMCT24:
        case 0x31:
            return SRC_CT24;
        case GSN_PSMCT16:
        case GSN_PSMCT16S:
        case 0x32:
        case 0x3A:
            return SRC_CT16;
        case GSN_PSMCT32:
        case 0x30:
            return SRC_CT32;
        default:
            return SRC_INDEX;
        }
    }

    // The texels of level 0 a primitive reads across and down (its texture coordinates' largest, whole texels)
    static void texels_read(const GifPrimitive &p, uint64_t tex0, uint32_t &cols, uint32_t &rows) {
        uint32_t tw = 1u << std::min(bits(tex0, 26, 4), 10u), th = 1u << std::min(bits(tex0, 30, 4), 10u);
        bool fst = bits(p.prim, 8, 1) != 0;
        float most_u = 0, most_v = 0;
        for (uint32_t i = 0; i < p.vertex_count; i++) {
            const GifVertex &v = p.vertices[i];
            float u = fst ? v.u / 16.0f : (v.q != 0 ? v.s / v.q * float(tw) : float(tw));
            float t = fst ? v.v / 16.0f : (v.q != 0 ? v.t / v.q * float(th) : float(th));
            if (!std::isfinite(u) || !std::isfinite(t)) {
                u = float(tw);
                t = float(th);
            }
            most_u = std::max(most_u, u);
            most_v = std::max(most_v, t);
        }
        cols = uint32_t(std::clamp(std::ceil(most_u) + 1.0f, 1.0f, float(tw)));
        rows = uint32_t(std::clamp(std::ceil(most_v) + 1.0f, 1.0f, float(th)));
    }

    bool texture_covers(const GifPrimitive &p) const {
        uint32_t cols, rows;
        texels_read(p, batch_.state.tex0, cols, rows);
        return cols <= batch_.texture.cols && rows <= batch_.texture.rows;
    }

    BoundTexture bind_texture(const DrawState &s, uint32_t cols, uint32_t rows) {
        BoundTexture b;
        TextureRegs regs = {s.tex0, s.tex1, s.miptbp1, s.miptbp2, s.clamp};
        uint32_t psm = bits(s.tex0, 20, 6);
        uint32_t tw = 1u << std::min(bits(s.tex0, 26, 4), 10u), th = 1u << std::min(bits(s.tex0, 30, 4), 10u);
        // (with REPEAT or REGION_REPEAT every texel may be read)
        if (bits(s.clamp, 0, 2) == 0 || bits(s.clamp, 0, 2) == 3) {
            cols = tw;
        }
        if (bits(s.clamp, 2, 2) == 0 || bits(s.clamp, 2, 2) == 3) {
            rows = th;
        }
        TextureAnswer a = store_.resolve(regs, cols, rows);
        b.cols = cols;
        b.rows = rows;
        b.src = source_of(psm);
        if (a.source == TextureSource::FromUploads && a.texture) {
            if (sampler_reads(s, *a.texture)) {
                SampledTexture *t = sampled_texture(a, s);
                if (!t) {
                    return b;
                }
                b.view = &t->image->get_view();
                b.held = t->image;
                b.src = SRC_SAMPLED;
                b.levels = t->levels;
                uint32_t mmin = bits(s.tex1, 6, 3);
                counts_.textures_mipmapped += t->levels > 1 && mmin >= 2 && mmin <= 5 ? 1 : 0;
                uint32_t aniso = anisotropy_for(s.tex1, t->levels);
                b.sampler = &sampler_for(s.tex1, s.clamp, t->levels, aniso);
                b.sampling = sampling_bits(aniso);
                b.alpha_lo = t->alpha_lo;
                b.alpha_hi = t->alpha_hi;
                b.cols = std::max(cols, a.texture->read_cols);
                b.rows = std::max(rows, a.texture->read_rows);
                b.ok = true;
                counts_.textures_sampled++;
                return b;
            }
            GpuTexture *t = gpu_texture(a);
            if (!t) {
                return b;
            }
            b.view = &t->image->get_view();
            b.held = t->image;
            b.levels = t->levels;
            b.sampling = sampling_bits(1);
            b.cols = std::max(cols, a.texture->read_cols);
            b.rows = std::max(rows, a.texture->read_rows);
            b.ok = true;
            return b;
        }
        if (a.source == TextureSource::DrawnSurface) {
            return surface_texture(s.tex0, cols, rows);
        }
        counts_.textures_missing++;
        return b;
    }

    GpuTexture *gpu_texture(const TextureAnswer &a) {
        uint64_t h = a.key.hash();
        auto found = textures_.find(h);
        if (found != textures_.end() && found->second.key == a.key) {
            found->second.used = frame_;
            return &found->second;
        }
        const DecodedTexture &d = *a.texture;
        bool index = d.kind == TexelKind::Index8;
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(d.width_px, d.height_px,
                                                                    index ? VK_FORMAT_R8_UINT : VK_FORMAT_R8G8B8A8_UINT);
        info.levels = d.levels;
        info.misc = 0;
        std::array<ImageInitialData, 7> data{};
        for (uint32_t l = 0; l < d.levels; l++) {
            data[l].data = index ? static_cast<const void *>(d.indices.data() + d.level_start[l])
                                 : static_cast<const void *>(d.rgba.data() + d.level_start[l]);
            data[l].row_length = d.level_width(l);
            data[l].image_height = d.level_height(l);
        }
        ImageHandle image = device_.create_image(info, data.data());
        if (!image) {
            return nullptr;
        }
        counts_.textures_uploaded++;
        GpuTexture &t = textures_[h];
        t.key = a.key;
        t.image = image;
        t.levels = d.levels;
        t.index = index;
        t.used = frame_;
        return &t;
    }

    // ---- The GPU's sampler (PC_RENDERER.md, "How it draws": the GS's texel read only where it cannot stand in) ----
    // Whether the sampler reads this texture as the GS would: REPEAT and CLAMP are its address modes (the region
    // modes are not: counted), over the texture's own size (the store decodes only the part a draw reads, so a
    // repeated texture must be whole, and a clamped one no wider than the GS's: counted)
    bool sampler_reads(const DrawState &s, const DecodedTexture &d) {
        if (!sampler_on_) {
            return false;
        }
        uint32_t wms = bits(s.clamp, 0, 2), wmt = bits(s.clamp, 2, 2);
        if (wms >= 2 || wmt >= 2) {
            counts_.exact_textures_region++;
            return false;
        }
        uint32_t tw = 1u << std::min(bits(s.tex0, 26, 4), 10u), th = 1u << std::min(bits(s.tex0, 30, 4), 10u);
        bool across = wms == 0 ? d.width_px == tw : d.width_px <= tw;
        bool down = wmt == 0 ? d.height_px == th : d.height_px <= th;
        if (!across || !down) {
            counts_.exact_textures_part++;
            return false;
        }
        return true;
    }

    SampledTexture *sampled_texture(const TextureAnswer &a, const DrawState &s) {
        const DecodedTexture &d = *a.texture;
        uint32_t psm = bits(s.tex0, 20, 6);
        uint64_t texa = s.texa & (0xFFu | 1u << 15 | uint64_t(0xFF) << 32);
        ClutColours clut;
        if (d.kind == TexelKind::Index8) {
            clut = store_.clut_colours(s.tex0, s.texa);
        }
        uint64_t h = a.key.hash() ^ (texa * 0x9E3779B97F4A7C15ull);
        for (uint32_t c : clut.rgba) {
            h = (h ^ c) * 0x100000001B3ull;
        }
        auto found = sampled_.find(h);
        if (found != sampled_.end() && found->second.made_from(a.key, texa, clut.rgba)) {
            found->second.used = frame_;
            return &found->second;
        }
        std::vector<uint32_t> colours = colour_texels(d, psm, clut, texa);
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(d.width_px, d.height_px, VK_FORMAT_R8G8B8A8_UNORM);
        info.levels = d.levels;
        info.misc = 0;
        std::array<ImageInitialData, 7> data{};
        for (uint32_t l = 0; l < d.levels; l++) {
            data[l].data = colours.data() + d.level_start[l];
            data[l].row_length = d.level_width(l);
            data[l].image_height = d.level_height(l);
        }
        ImageHandle image = device_.create_image(info, data.data());
        if (!image) {
            return nullptr;
        }
        counts_.textures_uploaded++;
        SampledTexture &t = sampled_[h]; // (a different texture of the same hash: put in its place)
        t.key = a.key;
        t.texa = texa;
        t.clut = clut.rgba;
        t.image = image;
        t.levels = d.levels;
        t.used = frame_;
        t.alpha_lo = 255;
        t.alpha_hi = 0;
        for (uint32_t c : colours) {
            t.alpha_lo = std::min(t.alpha_lo, uint8_t(c >> 24));
            t.alpha_hi = std::max(t.alpha_hi, uint8_t(c >> 24));
        }
        return &t;
    }

    // Texture detail and anisotropy (pcr_set_lod, pcr_set_aniso; PC_RENDERER.md, "Texture detail and anisotropy"),
    // for the batches from the next vblank
    // The 2D mode (GSN_2D_*): with smooth, at a scale above 1, the 2D's magnified textures read with a cubic
    void set_2d(int mode) {
        mode_2d_ = mode;
    }

    // Layer Z (GSN_LAYER_Z_*): with tolerant, Z's fraction kept and the GS's ties kept on average (draw_batch's
    // tolerant passes, pcr_prim.h's pcr_exact_depth), for the batches from the next vblank
    void set_layer_z(int mode) {
        layers_ = mode == GSN_LAYER_Z_TOLERANT;
    }

    void set_sampling(int lod, uint32_t aniso) {
        lod_mode_ = lod;
        aniso_ = std::clamp(aniso, 1u, 16u);
    }

    // The anisotropy a sampled texture is read with: aniso_ for a mipmapped texture filtered bilinear within its
    // levels (MMAG linear, MMIN 4 or 5: the 3D's floors and walls), else 1. The 2D (no mipmaps, read nearest) and the
    // GS's own reads (region modes, surfaces) never.
    uint32_t anisotropy_for(uint64_t tex1, uint32_t levels) const {
        uint32_t mmag = bits(tex1, 5, 1), mmin = bits(tex1, 6, 3);
        return aniso_ > 1 && levels > 1 && mmag == 1 && (mmin == 4 || mmin == 5) ? aniso_ : 1;
    }

    // tex1's bits 10-31 in the pushes: log2 of the anisotropy (10-12) and the bias the texture detail adds to the GS's
    // level (16-31, signed, in 256ths). The GS picks the level from Q, the distance, for 640 x 448: at a larger scale
    // a surface has more pixels at the same distance and the PS2's level is too coarse for them. scale: -log2 of the
    // smaller of the two scales (native widescreen's across is the larger; 0 at 1x); full: -64 (level 0); ps2: 0.
    uint32_t sampling_bits(uint32_t aniso) {
        float bias = lod_mode_ == GSN_LOD_FULL    ? -64.0f
                     : lod_mode_ == GSN_LOD_SCALE ? -std::log2(float(std::min(scale_[0], scale_[1])))
                                                  : 0.0f;
        int32_t b256 = int32_t(std::lround(bias * 256.0f));
        uint32_t a = 0;
        while (a < 4 && (2u << a) <= aniso) {
            a++;
        }
        if (aniso > 1) {
            counts_.textures_anisotropic++;
        }
        return a << 10 | uint32_t(b256) << 16;
    }

    // The sampler for TEX1's filters (MMAG, MMIN) over a texture of levels (TEX1's MXL is in the decode's levels).
    // The level is the GS's, from Q (the shader's textureLod): at 0 or below MMAG's filter, above it MMIN's. Without
    // mipmaps (MMIN 0 or 1, or one level) the lod is held at 0.25, which reads level 0 by MMIN's filter. With aniso
    // above 1 (anisotropy_for), the sampler's anisotropy, which the shader's textureGrad reads with.
    const Sampler &sampler_for(uint64_t tex1, uint64_t clamp, uint32_t levels, uint32_t aniso) {
        uint32_t mmag = bits(tex1, 5, 1), mmin = bits(tex1, 6, 3);
        bool mipmapped = mmin >= 2 && mmin <= 5 && levels > 1;
        uint32_t wrap = bits(clamp, 0, 4); // (WMS, WMT: REPEAT or CLAMP here)
        if (!device_.get_device_features().enabled_features.samplerAnisotropy) {
            aniso = 1;
        }
        uint32_t key = mmag | mmin << 1 | (mipmapped ? levels : 0) << 4 | wrap << 8 | aniso << 12;
        auto found = samplers_.find(key);
        if (found != samplers_.end()) {
            return *found->second;
        }
        SamplerCreateInfo info = {};
        info.mag_filter = mmag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.min_filter = mmin == 1 || mmin == 4 || mmin == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        info.mipmap_mode = mipmapped && (mmin == 3 || mmin == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                  : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.address_mode_u = (wrap & 3) == 0 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.address_mode_v = (wrap >> 2) == 0 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.address_mode_w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.min_lod = 0.0f;
        info.max_lod = mipmapped ? float(levels - 1) : 0.25f;
        if (aniso > 1) {
            info.anisotropy_enable = VK_TRUE;
            info.max_anisotropy = std::min(float(aniso), device_.get_gpu_properties().limits.maxSamplerAnisotropy);
        }
        info.border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        SamplerHandle made = device_.create_sampler(info);
        if (!made) {
            fprintf(stderr, "[pcr] a sampler could not be made: nearest and clamped instead\n");
            return device_.get_stock_sampler(StockSampler::NearestClamp);
        }
        return *samplers_.emplace(key, made).first->second;
    }

    // A texture where a surface was drawn: the surface's part copied (so the surface can be drawn into while read)
    BoundTexture surface_texture(uint64_t tex0, uint32_t cols, uint32_t rows) {
        BoundTexture b;
        uint32_t tbp = bits(tex0, 0, 14), tbw = bits(tex0, 14, 6), psm = bits(tex0, 20, 6);
        Surface *from = nullptr;
        uint32_t ox = 0, oy = 0;
        for (auto &s : colour_) {
            if (tbp % 32 == 0 && s->fbw == tbw && s->page_place(tbp / 32, ox, oy)) {
                from = s.get();
                break;
            }
        }
        // (a 16-bit texture only from a surface of its own format: PSMCT16 and PSMCT16S place their pixels
        // differently in a page; the shader reads it as the GS would, F_SURFACE_CT16)
        if (!from || (psm != GSN_PSMCT32 && psm != GSN_PSMCT24 && !(is_colour_16(psm) && from->psm == psm))) {
            counts_.unsupported_texture_on_surface++;
            return b;
        }
        uint32_t w = std::min(cols, from->width() - std::min(ox, from->width()));
        uint32_t h = std::min(rows, from->height - std::min(oy, from->height));
        if (w == 0 || h == 0) {
            counts_.unsupported_texture_on_surface++;
            return b;
        }
        ImageCreateInfo info = ImageCreateInfo::render_target(cols * scale_[0], rows * scale_[1], from->image->get_format());
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        b.surface_copy = device_.create_image(info, nullptr);
        if (!b.surface_copy) {
            return b;
        }
        end_pass();
        CommandBuffer &c = cmd(GpuWork::texture_copy);
        full_barrier(c);
        VkClearValue zero = {};
        c.clear_image(*b.surface_copy, zero);
        full_barrier(c);
        // (between 0xF4 and 0xF5 of a picture between, the frame shown kept at 0xF2 in place of its buffer)
        bool kept = replaying_ && kept_reads_ && kept_frame_ && from->base_page == kept_page_ &&
                    kept_frame_->get_width() == from->image->get_width() &&
                    kept_frame_->get_height() == from->image->get_height() &&
                    kept_frame_->get_format() == from->image->get_format();
        counts_.kept_frame_reads += kept ? 1 : 0;
        VkOffset3D at = {int32_t(ox * scale_[0]), int32_t(oy * scale_[1]), 0};
        if (kept) {
            const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.copy_image(*b.surface_copy, *kept_frame_, {0, 0, 0}, at, {w * scale_[0], h * scale_[1], 1}, colour, colour);
        } else {
            copy_out(c, *b.surface_copy, {0, 0, 0}, *from, at, {w * scale_[0], h * scale_[1], 1});
        }
        full_barrier(c);
        b.view = &b.surface_copy->get_view();
        b.src = SRC_SURFACE;
        b.cols = cols;
        b.rows = rows;
        b.ok = true;
        counts_.textures_from_surfaces++;
        return b;
    }

    // 0xF2: the frame shown (the colour surface at that base page), kept before a picture between draws over it
    void keep_frame(uint32_t base_page) {
        flush_batch();
        kept_frame_.reset();
        for (auto &s : colour_) {
            if (s->base_page == base_page) {
                kept_frame_ = copy_of(*s, GpuWork::copy);
                kept_page_ = base_page;
                return;
            }
        }
    }

    // A colour surface's copy, for a shader to read while the surface is drawn into
    ImageHandle copy_of(const Surface &s, GpuWork work) {
        const Image &image = *s.image;
        ImageCreateInfo info = ImageCreateInfo::render_target(image.get_width(), image.get_height(), image.get_format());
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageHandle copy = device_.create_image(info, nullptr);
        if (!copy) {
            return copy;
        }
        end_pass();
        CommandBuffer &c = cmd(work);
        full_barrier(c);
        copy_out(c, *copy, {0, 0, 0}, s, {0, 0, 0}, {image.get_width(), image.get_height(), 1});
        full_barrier(c);
        return copy;
    }

    // A rectangle of a colour surface (its pixels), copied for a shader to read while the surface is drawn into
    ImageHandle copy_of_rect(const Surface &s, const VkRect2D &rect) {
        const Image &image = *s.image;
        ImageCreateInfo info = ImageCreateInfo::render_target(rect.extent.width, rect.extent.height, image.get_format());
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageHandle copy = device_.create_image(info, nullptr);
        if (!copy) {
            return copy;
        }
        end_pass();
        CommandBuffer &c = cmd(GpuWork::blend_copy);
        full_barrier(c);
        copy_out(c, *copy, {0, 0, 0}, s, {rect.offset.x, rect.offset.y, 0}, {rect.extent.width, rect.extent.height, 1});
        full_barrier(c);
        return copy;
    }

    void evict_textures() {
        if (textures_.size() >= 4096) {
            for (auto it = textures_.begin(); it != textures_.end();) {
                it = frame_ - it->second.used > 120 ? textures_.erase(it) : std::next(it);
            }
        }
        if (sampled_.size() >= 4096) {
            for (auto it = sampled_.begin(); it != sampled_.end();) {
                it = frame_ - it->second.used > 120 ? sampled_.erase(it) : std::next(it);
            }
        }
    }

    // ---- Batches ----
    struct Batch {
        DrawState state;
        BoundTexture texture;
        std::array<uint32_t, 256> clut{};
        std::vector<Vertex> vertices;
        int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN; // (GS pixels touched)
        // a native batch's (pcr_nd000.vert): its buffers, and its runs of triangles, a draw call each
        struct Run {
            uint32_t first, count, instance;
            uint32_t picture_instance; // (NativeBatch's: no_instance, or the record its pictures between draw it with)
        };
        bool native = false;
        bool moved = false;           // (a picture between's: corners the shader moves, anywhere in its scissor)
        bool numbered = false;        // (corners naming their kicks: moved in a picture between's walk)
        bool native_kicks = false;    // (numbered, some of them a native draw's: native_kick_base on, as_picture_walk)
        bool shader_blend = false;    // (a blend with no Vulkan form: the GS's in the shader, from a copy)
        bool sprites = true;          // (every primitive a sprite: clear_depth_aliases)
        std::vector<PixelBox> boxes;  // (shader_blend: its primitives' pixels, none of them overlapping)
        NativeProgram program = NativeProgram::vu1_000;
        BufferHandle arena, records, triangles;
        std::vector<Run> runs;
        // (a kept walk's GIF batch: where its vertices start in the walk's buffer, Kept::vertices; none: copied to
        // the GPU at each draw)
        static constexpr uint32_t no_gpu_vertices = UINT32_MAX;
        uint32_t gpu_first = no_gpu_vertices;
        // (a kept walk's batch through the CLUT: its CLUT's first colour in the walk's buffer, Kept::cluts; none: set 1's)
        uint32_t clut_at = no_walk_clut;
        // (a kept walk's batch: its texture's place in the walk's table, Kept::table, and its sampler's in Kept::samplers;
        // no_table: set 1's)
        uint32_t table_at = no_table, sampler_at = 0;
    };

    // The batch's shader state, for the profiler's GPU time by state (port_profile_gpu_detail; 0 when off)
    uint32_t batch_state_key(const Batch &b, bool zte, bool zwrite, uint32_t ztst, bool date, bool date_stencil,
                             int passes) const {
        if (!port_profile_gpu_detail()) {
            return 0;
        }
        const DrawState &s = b.state;
        const BoundTexture &bt = b.texture;
        ShaderState st = {};
        st.program = !b.native                              ? 0
                     : b.program == NativeProgram::vu1_280 ? 2
                     : b.program == NativeProgram::vu1_700 ? 3
                                                                : 1;
        if (bits(s.prim, 4, 1) && bt.ok) {
            st.texture = bt.src == SRC_SAMPLED ? 3 : bt.src == SRC_SURFACE ? 2 : 1;
            st.tfx = uint8_t(bits(s.tex0, 35, 2));
            st.alpha_channel = bits(s.tex0, 34, 1) != 0;
        }
        st.fog = bits(s.prim, 5, 1) != 0;
        st.blending = bits(s.prim, 6, 1) != 0 || b.shader_blend;
        st.blend = uint8_t(bits(s.alpha, 0, 8));
        st.shader_blend = b.shader_blend;
        st.date = !date ? 0 : date_stencil ? 1 : 2;
        st.depth = zte ? uint8_t(ztst) : 0;
        st.depth_write = zwrite;
        st.passes = uint8_t(passes);
        return shader_state_key(st);
    }

    static GpuWork batch_work(const Batch &b) {
        if (!b.native) {
            return GpuWork::draws_2d;
        }
        switch (b.program) {
        case NativeProgram::vu1_280:
            return GpuWork::native_280;
        case NativeProgram::vu1_700:
            return GpuWork::native_700;
        default:
            return GpuWork::native_000;
        }
    }

    // An upload of the frame's (what its pictures between ask for again), or a kept walk's
    struct RecordedUpload {
        TransferRegisters transfer;
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        uint64_t chunk; // (the texture store's: 0 if it refused it)
    };

    // A descriptor table (Granite's bindless set: images alone, pcr_prim.h's u_table), its pool its own
    struct Table {
        BindlessDescriptorPoolHandle pool;
        BindlessDescriptorSet set;
    };
    Table make_table(const std::vector<const ImageView *> &views) const {
        Table t;
        if (views.empty()) {
            return t;
        }
        t.pool = device_.create_bindless_descriptor_pool(BindlessResourceType::Image, 1, unsigned(views.size()));
        if (!t.pool || !t.pool->allocate_descriptors(unsigned(views.size()))) {
            t.pool.reset();
            return t;
        }
        for (const ImageView *v : views) {
            t.pool->push_texture(*v);
        }
        t.pool->update();
        t.set = t.pool->get_descriptor_set();
        return t;
    }

    // ---- A kept walk's steps (keep_walk), in the order the walk made them ----
    // (a batch: Kept::batches' index; batches are kept out of the step so a step is small and the list grows cheaply)
    struct KeptBatch {
        uint32_t index;
    };
    struct KeptUpload {
        RecordedUpload upload; // (made again from its bytes; its chunk what an upload asked again finds)
    };
    struct KeptUploadAgain {
        RecordedUpload upload;
    };
    struct KeptCopy {
        TransferRegisters transfer;
    };
    struct KeptClutLoad {
        uint64_t tex0, texclut;
    };
    struct KeptMark {
        uint64_t value;
    };
    using KeptStep = std::variant<KeptBatch, KeptUpload, KeptUploadAgain, KeptCopy, KeptClutLoad, KeptMark>;
    // (moved, not copied, when the lists grow)
    static_assert(std::is_nothrow_move_constructible_v<Batch>);
    static_assert(std::is_nothrow_move_constructible_v<KeptStep>);
    struct Kept {
        std::vector<KeptStep> steps;
        std::vector<Batch> batches;
        PagesRead read;
        BufferHandle vertices; // (its GIF batches' vertices, put on the GPU once: put_vertices_on_gpu)
        BufferHandle cluts;    // (its batches' CLUTs, put on the GPU once: put_cluts_on_gpu)
        Table table;           // (its batches' textures, put_table_on_gpu) and their samplers
        std::array<const Sampler *, walk_samplers> samplers{};

        void keep(Batch &&batch) {
            steps.emplace_back(KeptBatch{uint32_t(batches.size())});
            batches.push_back(std::move(batch));
        }
    };
    // (the last kept walk's sizes, reserved for the next: a frame's walk is about as long as the frame before's)
    struct KeptSizes {
        size_t steps = 0, batches = 0;
    };
    friend class pcr::KeptWalk;
    friend class pcr::SurfaceCopies;
    friend class pcr::Canvas;

    // ---- Step 5b: a walk's calls kept, and made again ----
    void keep_walk() {
        kept_ = std::make_unique<Kept>();
        kept_->steps.reserve(kept_sizes_.steps);
        kept_->batches.reserve(kept_sizes_.batches);
        kept_spoilt_ = false;
        keeping_ = true;
        store_.watch_reads();
    }

    std::unique_ptr<Kept> take_kept_walk() {
        std::unique_ptr<Kept> walk = std::move(kept_);
        if (keeping_) {
            return nullptr; // (taken before the walk's end)
        }
        if (kept_spoilt_ || !walk || walk->steps.empty()) {
            return nullptr;
        }
        return walk;
    }

    bool fits(const Kept &walk) const {
        return store_.unchanged(walk.read);
    }

    void draw_again(const Kept &walk, const Between &between) {
        flush_batch();
        between_ = &between;
        walk_vertices_ = walk.vertices.get();
        walk_cluts_ = walk.cluts.get();
        walk_table_ = walk.table.set ? &walk : nullptr;
        start_runs(walk);
        for (const KeptStep &step : walk.steps) {
            std::visit([&](const auto &kept) { make_again(walk, kept); }, step);
        }
        flush_batch();
        end_runs();
        between_ = nullptr;
        walk_vertices_ = nullptr;
        walk_cluts_ = nullptr;
        walk_table_ = nullptr;
    }

    // A walk's GIF batches' vertices put in one buffer of its own, which its pictures between draw from: drawn again,
    // a batch then binds its place there instead of copying its vertices to the GPU (the pump's cost a batch,
    // PERFORMANCE.md). Called once, before the walk is handed out (it is not changed after); without memory the
    // GPU can map, the batches are copied at each draw as before. KF4_PC_WALK_VERTICES=0: never (to compare).
    void put_vertices_on_gpu(Kept &walk) const {
        // (a batch copied from another walk, drawn again while this one was kept, names its place in that one's)
        size_t count = 0;
        for (Batch &b : walk.batches) {
            b.gpu_first = Batch::no_gpu_vertices;
            count += b.native ? 0 : b.vertices.size();
        }
        walk.vertices.reset();
        if (!walk_vertices_on_ || count == 0 || count > Batch::no_gpu_vertices) {
            return;
        }
        BufferCreateInfo info = {};
        info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
        info.size = VkDeviceSize(count) * sizeof(Vertex);
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        BufferHandle buffer = device_.create_buffer(info, nullptr);
        auto *v = buffer ? static_cast<Vertex *>(device_.map_host_buffer(*buffer, MEMORY_ACCESS_WRITE_BIT)) : nullptr;
        if (!v) {
            return;
        }
        uint32_t first = 0;
        for (Batch &b : walk.batches) {
            if (b.native || b.vertices.empty()) {
                continue;
            }
            memcpy(v + first, b.vertices.data(), b.vertices.size() * sizeof(Vertex));
            b.gpu_first = first;
            first += uint32_t(b.vertices.size());
        }
        device_.unmap_host_buffer(*buffer, MEMORY_ACCESS_WRITE_BIT);
        walk.vertices = std::move(buffer);
    }

    // A walk's CLUTs (its batches through one: 256 colours each, the same CLUT once) put in one buffer of its own, which
    // its pictures between read (u_walk_cluts, from PrimPush::clut_at) instead of a CLUT copied to the GPU at each draw
    // into a set 1 of its own. Called once, before the walk is handed out, as put_vertices_on_gpu.
    // KF4_PC_BINDLESS=0: never (to compare).
    void put_cluts_on_gpu(Kept &walk) const {
        walk.cluts.reset();
        std::vector<uint32_t> colours;
        std::unordered_map<uint64_t, uint32_t> found; // (a CLUT's hash: its first colour)
        for (Batch &b : walk.batches) {
            b.clut_at = no_walk_clut;
            if (!bindless_on_ || !b.texture.ok || b.texture.src != SRC_INDEX) {
                continue;
            }
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t c : b.clut) {
                h = (h ^ c) * 0x100000001B3ull;
            }
            auto at = found.find(h);
            if (at != found.end() && memcmp(colours.data() + at->second, b.clut.data(), sizeof(b.clut)) == 0) {
                b.clut_at = at->second;
                continue;
            }
            b.clut_at = uint32_t(colours.size());
            found[h] = b.clut_at;
            colours.insert(colours.end(), b.clut.begin(), b.clut.end());
        }
        if (colours.empty()) {
            return;
        }
        BufferCreateInfo info = {};
        info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
        info.size = colours.size() * sizeof(uint32_t);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        walk.cluts = device_.create_buffer(info, colours.data());
        if (!walk.cluts) {
            for (Batch &b : walk.batches) {
                b.clut_at = no_walk_clut;
            }
        }
    }

    // A walk's textures (each image its batches read that the walk holds: decoded and sampled textures; not a surface's,
    // copied at the draw) in one descriptor table of its own, and the samplers they are read with in a small array:
    // its pictures between then bind no texture a batch (set 1 stays as it is: the dummies), only the table once.
    // Called once, before the walk is handed out, as put_vertices_on_gpu. A batch whose sampler finds no room (more
    // than 15 in a walk) keeps set 1's. KF4_PC_BINDLESS=0: never (to compare).
    void put_table_on_gpu(Kept &walk) const {
        walk.table = {};
        walk.samplers.fill(nullptr);
        walk.samplers[0] = &device_.get_stock_sampler(StockSampler::NearestClamp);
        uint32_t samplers = 1;
        std::vector<const ImageView *> views;
        std::unordered_map<const ImageView *, uint32_t> found;
        for (Batch &b : walk.batches) {
            b.table_at = no_table;
            b.sampler_at = 0;
            const BoundTexture &bt = b.texture;
            if (!bindless_on_ || !table_on_ || !bt.ok || !bt.view || bt.src == SRC_SURFACE) {
                continue;
            }
            uint32_t s = 0;
            if (bt.src == SRC_SAMPLED) {
                while (s < samplers && walk.samplers[s] != bt.sampler) {
                    s++;
                }
                if (s == samplers) {
                    if (samplers == walk_samplers) {
                        continue;
                    }
                    walk.samplers[samplers++] = bt.sampler;
                }
            }
            auto at = found.find(bt.view);
            if (at == found.end()) {
                if (views.size() >= no_table) {
                    continue;
                }
                at = found.emplace(bt.view, uint32_t(views.size())).first;
                views.push_back(bt.view);
            }
            b.table_at = at->second;
            b.sampler_at = s;
        }
        walk.table = make_table(views);
        if (!walk.table.set) {
            for (Batch &b : walk.batches) {
                b.table_at = no_table;
            }
        }
    }

    // ---- The pictures between on a thread of their own (the pump's canvas) ----
    struct Copies {
        std::vector<std::unique_ptr<Surface>> colour, depth;
        ImageHandle kept_frame;
        uint32_t kept_page = 0;
        std::array<uint32_t, 2> scale = {1, 1};
        uint32_t samples = 1;
        bool cutout_aa = false;
        int precision = GSN_COLOUR_EXACT; // (colour_mode_)
        int mode_2d = GSN_2D_PS2;         // (read as each batch is drawn: the pump's canvas has no set_2d of its own)
        bool layers = false;              // (layers_, the same: set_layer_z's)
    };

    std::unique_ptr<Copies> copy_surfaces() {
        flush_batch();
        end_pass();
        auto copies = std::make_unique<Copies>();
        copies->kept_frame = kept_frame_; // (never written: a 0xF2 makes a new one)
        copies->kept_page = kept_page_;
        copies->scale = scale_;
        copies->samples = samples_;
        copies->cutout_aa = cutout_aa_;
        copies->precision = colour_mode_;
        copies->mode_2d = mode_2d_;
        copies->layers = layers_;
        CommandBuffer &c = cmd(GpuWork::copy);
        full_barrier(c);
        for (auto *list : {&colour_, &depth_}) {
            for (const auto &s : *list) {
                ImageHandle image = make_surface_image(s->width(), s->height, s->depth);
                if (!image) {
                    continue; // (the pump makes it again, as a surface it never had)
                }
                VkImageAspectFlags aspect = !s->depth ? VK_IMAGE_ASPECT_COLOR_BIT
                                            : stencil_on_ ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                                          : VK_IMAGE_ASPECT_DEPTH_BIT;
                const VkImageSubresourceLayers layers = {aspect, 0, 0, 1};
                VkExtent3D extent = {s->image->get_width(), s->image->get_height(), 1};
                // (MSAA: the image and the multisampled one each copied only where it is not to be made again from the
                // other: an image behind its samples is resolved before it is read, samples behind expanded)
                ImageHandle ms;
                if (s->ms) {
                    ms = make_ms_image(s->ms->get_width(), s->ms->get_height());
                    if (!ms) {
                        continue;
                    }
                    if (!s->ms_behind) {
                        c.copy_image(*ms, *s->ms, {0, 0, 0}, {0, 0, 0}, extent, layers, layers);
                    }
                }
                if (!s->ms || !s->ms_ahead) {
                    c.copy_image(*image, *s->image, {0, 0, 0}, {0, 0, 0}, extent, layers, layers);
                }
                auto copy = std::make_unique<Surface>(*s);
                copy->image = std::move(image);
                copy->ms = std::move(ms);
                (s->depth ? copies->depth : copies->colour).push_back(std::move(copy));
                counts_.surfaces_copied++;
            }
        }
        full_barrier(c);
        submit();
        return copies;
    }

    void start_from(Copies &copies) {
        flush_batch();
        end_pass();
        colour_ = std::move(copies.colour);
        depth_ = std::move(copies.depth);
        kept_frame_ = std::move(copies.kept_frame);
        kept_page_ = copies.kept_page;
        scale_ = pending_scale_ = copies.scale;
        samples_ = pending_samples_ = copies.samples;
        cutout_aa_ = copies.cutout_aa;
        colour_mode_ = pending_colour_ = copies.precision;
        mode_2d_ = copies.mode_2d;
        layers_ = copies.layers;
        replaying_ = kept_reads_ = false;
        // (the surfaces are kf4-pc's, its store's uploads not here: this canvas only draws kept walks again, whose
        // textures are bound already, and its store is never read; so what it draws over is not marked in it)
        store_follows_draws_ = false;
    }

    // (draw_again_alone) Batches drawn between two submits of the pump's command buffer: kf4-pc's next frame context
    // waits for at most that many
    static constexpr uint32_t batches_a_submit = 128;

    void draw_again_alone(const Kept &walk, const Between &between, bool picture_follows) {
        flush_batch();
        between_ = &between;
        walk_vertices_ = walk.vertices.get();
        walk_cluts_ = walk.cluts.get();
        walk_table_ = walk.table.set ? &walk : nullptr;
        start_runs(walk);
        uint32_t batches = 0;
        for (const KeptStep &step : walk.steps) {
            std::visit([&](const auto &kept) { make_again_alone(walk, kept); }, step);
            if (std::holds_alternative<KeptBatch>(step) && ++batches % batches_a_submit == 0) {
                submit();
            }
        }
        flush_batch();
        end_runs();
        between_ = nullptr;
        walk_vertices_ = nullptr;
        walk_cluts_ = nullptr;
        walk_table_ = nullptr;
        submit(!picture_follows); // (a picture next: its submit puts this on the GPU's queue with it)
    }

    void make_again_alone(const Kept &walk, const KeptBatch &kept) {
        make_again(walk, kept);
    }
    void make_again_alone(const Kept &, const KeptUpload &kept) {
        flush_batch();
        write_upload_into_surfaces(kept.upload.transfer, *kept.upload.bytes);
    }
    void make_again_alone(const Kept &, const KeptUploadAgain &kept) {
        flush_batch();
        write_upload_into_surfaces(kept.upload.transfer, *kept.upload.bytes);
        counts_.uploads_made_again++;
    }
    void make_again_alone(const Kept &, const KeptCopy &kept) {
        flush_batch();
        copy_between_surfaces(kept.transfer);
    }
    void make_again_alone(const Kept &, const KeptClutLoad &) {}
    void make_again_alone(const Kept &, const KeptMark &kept) {
        frame_mark(kept.value);
    }

    // ---- A frame walked once (pcr_frame_walk.h): the frame's own walk as its stream's ----
    struct PictureKept {
        std::unique_ptr<Kept> walk;
        FrameWalkRefused refused = FrameWalkRefused::none;
    };
    PictureKept as_picture_walk(const Kept &frame, const FrameTargets &t) const {
        PictureKept out;
        out.walk = std::make_unique<Kept>();
        out.walk->read = frame.read;
        Kept &picture = *out.walk;
        picture.steps.reserve(frame.steps.size() + 2);
        picture.batches.reserve(frame.batches.size());
        picture.steps.emplace_back(KeptMark{uint64_t(0xF0) << 32});
        for (const KeptStep &step : frame.steps) {
            out.refused = std::visit([&](const auto &kept) { return in_picture(frame, kept, t, picture); }, step);
            if (out.refused != FrameWalkRefused::none) {
                out.walk.reset();
                return out;
            }
        }
        picture.steps.emplace_back(KeptMark{uint64_t(0xF1) << 32});
        return out;
    }

    // A draw into the buffer shown where it drew into the buffer drawn, and reading it there where it read the buffer
    // drawn's pixels (a surface's, looked up again as it is drawn); textured from the frame shown, between 0xF4 and
    // 0xF5; a native batch by its pictures' records, a GIF batch's numbered corners moved
    static FrameWalkRefused in_picture(const Kept &frame, const KeptBatch &step, const FrameTargets &t, Kept &picture) {
        const Batch &kept = frame.batches[step.index];
        bool textured = bits(kept.state.prim, 4, 1) != 0;
        if (textured && tex0_reads_drawn(kept.state.tex0, t) && kept.texture.src != SRC_SURFACE) {
            return FrameWalkRefused::reads_drawn;
        }
        Batch b = kept;
        b.state.frame = frame_in_picture(kept.state.frame, t);
        b.state.tex0 = tex0_in_picture(kept.state.tex0, t);
        if (b.native) {
            for (Batch::Run &run : b.runs) {
                if (run.picture_instance == no_instance) {
                    return FrameWalkRefused::native_unpaired;
                }
                run.instance = run.picture_instance;
                run.picture_instance = no_instance;
            }
        } else {
            b.moved = b.numbered;
            if (b.native_kicks) {
                // (a native draw's kicks, the trail's: numbered on from the stream's, Between::adopt_live_kicks)
                for (Vertex &v : b.vertices) {
                    for (int i = 0; i < 3; i++) {
                        if (v.kicks[i] != no_kick && v.kicks[i] >= native_kick_base) {
                            v.kicks[i] = v.kicks[i] - native_kick_base + t.kicks;
                        }
                    }
                }
            }
        }
        bool kept_read = textured && tex0_reads_kept(kept.state.tex0, t);
        if (kept_read) {
            picture.steps.emplace_back(KeptMark{uint64_t(0xF4) << 32});
        }
        picture.keep(std::move(b));
        if (kept_read) {
            picture.steps.emplace_back(KeptMark{uint64_t(0xF5) << 32});
        }
        return FrameWalkRefused::none;
    }
    static FrameWalkRefused in_picture(const Kept &, const KeptUpload &kept, const FrameTargets &t, Kept &picture) {
        switch (transfer_in_picture(0, kept.upload.transfer, t)) {
        case TransferInPicture::asked_again:
            picture.steps.emplace_back(KeptUploadAgain{kept.upload});
            return FrameWalkRefused::none;
        case TransferInPicture::as_is:
            picture.steps.emplace_back(kept);
            return FrameWalkRefused::none;
        default:
            return FrameWalkRefused::uploads_drawn;
        }
    }
    static FrameWalkRefused in_picture(const Kept &, const KeptUploadAgain &, const FrameTargets &, Kept &) {
        return FrameWalkRefused::marks; // (0x7C: not the game's own packets)
    }
    static FrameWalkRefused in_picture(const Kept &, const KeptCopy &kept, const FrameTargets &t, Kept &) {
        return transfer_in_picture(2, kept.transfer, t) == TransferInPicture::left_out ? FrameWalkRefused::none
                                                                                       : FrameWalkRefused::copies_drawn;
    }
    static FrameWalkRefused in_picture(const Kept &, const KeptClutLoad &kept, const FrameTargets &t, Kept &picture) {
        picture.steps.emplace_back(KeptClutLoad{tex0_in_picture(kept.tex0, t), kept.texclut});
        return FrameWalkRefused::none;
    }
    static FrameWalkRefused in_picture(const Kept &, const KeptMark &kept, const FrameTargets &, Kept &picture) {
        uint32_t index = bits(kept.value, 32, 8);
        if (index >= 0xF0 && index <= 0xF5) {
            return FrameWalkRefused::marks;
        }
        picture.steps.emplace_back(kept);
        return FrameWalkRefused::none;
    }

    void upload_made_again(const RecordedUpload &r) {
        flush_batch();
        if (keeping_) {
            kept_->steps.emplace_back(KeptUploadAgain{r});
        }
        if (r.chunk == 0 || !store_.upload_again(r.chunk)) {
            store_.upload({r.transfer.bitbltbuf, r.transfer.trxpos, r.transfer.trxreg}, *r.bytes);
        }
        write_upload_into_surfaces(r.transfer, *r.bytes);
        counts_.uploads_made_again++;
    }

    // The open batch, drawn, as a kept walk keeps it (its lists moved out: begin_batch empties them anyway); a surface's
    // copy is made again when it is drawn again
    Batch take_kept_batch() {
        size_t vertices = batch_.vertices.size();
        Batch b = std::move(batch_);
        batch_.vertices.reserve(vertices); // (the next batch's room, as the copy this replaces made)
        let_go_of_surface_copy(b);
        return b;
    }
    static void let_go_of_surface_copy(Batch &b) {
        b.texture.surface_copy.reset();
        if (b.texture.src == SRC_SURFACE) {
            b.texture.view = nullptr;
        }
    }

    // (drawn from where the walk keeps it: a copy only for a surface's texture, made again)
    void make_again(const Kept &walk, const KeptBatch &step) {
        flush_batch();
        const Batch &kept = walk.batches[step.index];
        if (kept.texture.src != SRC_SURFACE) {
            draw_made_again(kept);
            return;
        }
        Batch b = kept;
        b.texture = surface_texture(kept.state.tex0, kept.texture.cols, kept.texture.rows);
        draw_made_again(b);
    }
    void draw_made_again(const Batch &b) {
        if (b.vertices.empty() && b.runs.empty()) {
            return;
        }
        draw_batch(b);
        if (keeping_) {
            Batch kept = b;
            let_go_of_surface_copy(kept);
            kept_->keep(std::move(kept));
        }
    }
    void make_again(const Kept &, const KeptUpload &kept) {
        upload(kept.upload.transfer, *kept.upload.bytes);
    }
    void make_again(const Kept &, const KeptUploadAgain &kept) {
        upload_made_again(kept.upload);
    }
    void make_again(const Kept &, const KeptCopy &kept) {
        local_copy(kept.transfer);
    }
    void make_again(const Kept &, const KeptClutLoad &kept) {
        texture_written(kept.tex0, kept.texclut);
    }
    void make_again(const Kept &, const KeptMark &kept) {
        frame_mark(kept.value);
    }

    void open_batch(const DrawState &s, const GifPrimitive &p) {
        uint32_t cols = 0, rows = 0;
        if (bits(s.prim, 4, 1)) {
            texels_read(p, s.tex0, cols, rows);
        }
        begin_batch(s, cols, rows);
        batch_.x0 = batch_.y0 = INT32_MAX;
        batch_.x1 = batch_.y1 = INT32_MIN;
    }

    // (a native batch's texture is read whole, and what it draws over taken as its scissor: its corners are the GPU's)
    void open_native_batch(const DrawState &s, const NativeBatch &nb) {
        begin_batch(s, 1u << std::min(bits(s.tex0, 26, 4), 10u), 1u << std::min(bits(s.tex0, 30, 4), 10u));
        batch_.native = true;
        batch_.program = nb.program;
        batch_.arena = nb.arena;
        batch_.records = nb.records;
        batch_.triangles = nb.triangles;
        batch_.x0 = int32_t(bits(s.scissor, 0, 11));
        batch_.x1 = int32_t(bits(s.scissor, 16, 11));
        batch_.y0 = int32_t(bits(s.scissor, 32, 11));
        batch_.y1 = int32_t(bits(s.scissor, 48, 11));
    }

    void begin_batch(const DrawState &s, uint32_t cols, uint32_t rows) {
        batch_.state = s;
        batch_.texture = BoundTexture();
        batch_.vertices.clear();
        batch_.native = false;
        batch_.moved = false;
        batch_.numbered = false;
        batch_.native_kicks = false;
        batch_.arena.reset();
        batch_.records.reset();
        batch_.triangles.reset();
        batch_.runs.clear();
        batch_.boxes.clear();
        batch_.sprites = true;
        batch_.shader_blend = shader_blend_on_ && !choose_blend(s.alpha, bits(s.prim, 6, 1) != 0).supported;
        if (bits(s.prim, 4, 1)) {
            batch_.texture = bind_texture(s, cols, rows);
            if (batch_.texture.src == SRC_INDEX) {
                ClutColours colours = store_.clut_colours(s.tex0, s.texa);
                batch_.clut = colours.rgba;
            }
        }
        batch_open_ = true;
    }

    // A triangle with a corner's Z past its Z buffer's largest (16 or 24 bits), cut where its Z reaches it. The GS
    // clamps each pixel's Z, interpolated from the corners as they are; the vertex shader can only clamp the corners,
    // which lowered every pixel's Z across the triangle, so a rock's face near the camera (map 112, views 10-11) failed
    // its Z test where the GS's passed. Z, like every attribute, goes linearly across the screen, so the part where Z
    // is under the largest is drawn as it is, and the part beyond it with every corner at or past it: there the vertex
    // shader's clamp is the GS's.
    void add_cut_at_zmax(const std::array<Vertex, 3> &v, uint32_t zmax) {
        auto cut = [&](const Vertex &a, const Vertex &b) {
            double t = (double(zmax) - double(a.z)) / (double(b.z) - double(a.z));
            auto lerp = [&](float x, float y) { return float(double(x) + (double(y) - double(x)) * t); };
            Vertex c = a;
            c.x = lerp(a.x, b.x);
            c.y = lerp(a.y, b.y);
            c.z = zmax;
            for (int i = 0; i < 4; i++) {
                c.rgba[i] = uint8_t(std::lround(lerp(float(a.rgba[i]), float(b.rgba[i]))));
            }
            c.s = lerp(a.s, b.s);
            c.t = lerp(a.t, b.t);
            c.q = lerp(a.q, b.q);
            c.fog = lerp(a.fog, b.fog);
            return c;
        };
        // (each side's polygon, the triangle's corners in order with the cuts between: 3 or 4 corners)
        std::array<Vertex, 4> under, over;
        uint32_t nu = 0, no = 0;
        for (uint32_t i = 0; i < 3; i++) {
            const Vertex &a = v[i], &b = v[(i + 1) % 3];
            bool a_under = a.z < zmax, b_under = b.z < zmax;
            if (a_under) {
                under[nu++] = a;
            } else {
                over[no++] = a;
            }
            if (a_under != b_under) {
                Vertex c = cut(a, b);
                under[nu++] = c;
                over[no++] = c;
            }
        }
        for (const auto &[polygon, n] : {std::pair<const std::array<Vertex, 4> &, uint32_t>{under, nu}, {over, no}}) {
            for (uint32_t i = 2; i < n; i++) {
                batch_.vertices.insert(batch_.vertices.end(), {polygon[0], polygon[i - 1], polygon[i]});
                link_corners(std::span(batch_.vertices).last(3));
                counts_.triangles++;
            }
        }
    }

    // The 2D mode's smooth (pcr_prim.h, smooth_2d): the texels a sprite or a 2D triangle covers as the game gave its
    // corners, the first and last across and down within the texture, into each corner's rect. The cubic's taps are
    // kept inside them: a letter of the font is a sprite over its own cell of a sheet of letters (10 x 20 texels drawn
    // 7 or 8 GS pixels across), and taps past the cell read the letters beside and below it (faint ticks, dots and
    // specks at the letters' edges: the native GS's text bleed, 2026-10-24). (Not only the texels the GS reads: a
    // cell's last column, which the GS skips, can hold the letter's edge.)
    void texel_rect(Vertex *v, size_t count, bool fst) const {
        float size[2] = {float(1u << std::min(bits(batch_.state.tex0, 26, 4), 10u)),
                         float(1u << std::min(bits(batch_.state.tex0, 30, 4), 10u))};
        uint32_t rect[2];
        for (int a = 0; a < 2; a++) {
            float lo = INFINITY, hi = -INFINITY;
            for (size_t k = 0; k < count; k++) {
                float c = a ? v[k].t : v[k].s;
                c = fst ? c : (v[k].q != 0 ? c / v[k].q * size[a] : 0.0f);
                lo = std::min(lo, c);
                hi = std::max(hi, c);
            }
            if (!std::isfinite(lo) || !std::isfinite(hi)) {
                return;
            }
            // (the GS's texel coordinates have 4 bits of fraction: within a sixteenth of a whole number, on it)
            float first = std::clamp(std::floor(lo + 1.0f / 32.0f), 0.0f, size[a] - 1.0f);
            float last = std::clamp(std::ceil(hi - 1.0f / 32.0f) - 1.0f, first, size[a] - 1.0f);
            rect[a] = uint32_t(first) | uint32_t(last) << 16;
        }
        for (size_t k = 0; k < count; k++) {
            v[k].rect[0] = rect[0];
            v[k].rect[1] = rect[1];
        }
    }

    void add_primitive(const GifPrimitive &p) {
        uint64_t xy = batch_.state.xyoffset;
        if (batch_.shader_blend) {
            batch_.boxes.push_back(box_of(p, xy));
        }
        float ox = float(bits(xy, 0, 16)) / 16.0f, oy = float(bits(xy, 32, 16)) / 16.0f;
        bool gouraud = bits(p.prim, 3, 1) != 0, fst = bits(p.prim, 8, 1) != 0;
        auto make = [&](const GifVertex &v, const GifVertex &colour_from) {
            Vertex out;
            out.x = float(v.x) / 16.0f - ox;
            out.y = float(v.y) / 16.0f - oy;
            out.z = v.z;
            const GifVertex &c = gouraud ? v : colour_from;
            out.rgba[0] = uint8_t(c.rgba);
            out.rgba[1] = uint8_t(c.rgba >> 8);
            out.rgba[2] = uint8_t(c.rgba >> 16);
            out.rgba[3] = uint8_t(c.rgba >> 24);
            if (fst) {
                out.s = float(v.u) / 16.0f;
                out.t = float(v.v) / 16.0f;
                out.q = 1.0f;
            } else {
                out.s = v.s;
                out.t = v.t;
                out.q = v.q;
            }
            out.fog = float(v.fog);
            return out;
        };
        auto touch = [&](const Vertex &v) {
            batch_.x0 = std::min(batch_.x0, int32_t(std::floor(v.x)));
            batch_.y0 = std::min(batch_.y0, int32_t(std::floor(v.y)));
            batch_.x1 = std::max(batch_.x1, int32_t(std::ceil(v.x)));
            batch_.y1 = std::max(batch_.y1, int32_t(std::ceil(v.y)));
        };
        // (a picture between's: every corner of the walk's kicks, or none moved; numbered outside one, the frame's own
        // walk kept: the corners name their kicks, which nothing moves until the walk is made the pictures')
        uint32_t corners = p.shape == PrimitiveShape::triangle ? 3 : 2;
        bool moved = true;
        for (uint32_t i = 0; i < corners; i++) {
            moved = moved && p.vertices[i].kick != no_kick;
        }
        batch_.numbered |= moved;
        for (uint32_t i = 0; moved && i < corners; i++) {
            batch_.native_kicks |= p.vertices[i].kick >= native_kick_base;
        }
        batch_.moved |= moved && between_ && between_->active();
        uint32_t fst_bit = fst ? K_FST : 0;
        batch_.sprites = batch_.sprites && p.shape != PrimitiveShape::triangle;
        if (p.shape == PrimitiveShape::triangle) {
            const GifVertex &last = p.vertices[2];
            uint32_t zpsm = z_psm(batch_.state.zbuf);
            uint32_t zmax = zpsm == 0x31 ? 0xFFFFFFu : zpsm == 0x30 ? 0xFFFFFFFFu : 0xFFFFu;
            if (!moved && (p.vertices[0].z > zmax || p.vertices[1].z > zmax || p.vertices[2].z > zmax)) {
                std::array<Vertex, 3> v = {make(p.vertices[0], last), make(p.vertices[1], last), make(p.vertices[2], last)};
                add_cut_at_zmax(v, zmax);
                for (const Vertex &corner : v) {
                    touch(corner);
                }
                return;
            }
            size_t first_vertex = batch_.vertices.size();
            for (uint32_t i = 0; i < 3; i++) {
                batch_.vertices.push_back(make(p.vertices[i], last));
                if (moved) {
                    const GifVertex &colour = gouraud ? p.vertices[i] : last;
                    Vertex &v = batch_.vertices.back();
                    v.kicks[0] = p.vertices[i].kick;
                    v.kicks[1] = colour.kick;
                    v.kicks[2] = last.kick;
                    v.kicks[3] = fst_bit;
                }
                touch(batch_.vertices.back());
            }
            link_corners(std::span(batch_.vertices).subspan(first_vertex, 3));
            if (bits(batch_.state.prim, 4, 1)) {
                // (an FST triangle, or one of the 2D the menus draw with S, T and Q: one Z and one Q at its corners)
                const Vertex *t = &batch_.vertices[first_vertex];
                bool flat = t[0].z == t[1].z && t[1].z == t[2].z && t[0].q == t[1].q && t[1].q == t[2].q;
                if (fst || flat) {
                    texel_rect(&batch_.vertices[first_vertex], 3, fst);
                }
            }
            counts_.triangles++;
            return;
        }
        // a sprite: its two corners, Z, colour and fog the second's; its texture coordinates across and down
        const GifVertex &a = p.vertices[0], &b = p.vertices[1];
        Vertex va = make(a, b), vb = make(b, b);
        va.z = vb.z;
        va.fog = vb.fog;
        std::array<uint8_t, 4> rgba = {vb.rgba[0], vb.rgba[1], vb.rgba[2], vb.rgba[3]};
        memcpy(va.rgba, rgba.data(), 4);
        if (!fst) {
            // (each corner's S / Q and T / Q kept, over the second's Q)
            float q = vb.q != 0 ? vb.q : 1.0f;
            float ua = va.q != 0 ? va.s / va.q : 0, ta = va.q != 0 ? va.t / va.q : 0;
            float ub = vb.s / q, tb = vb.t / q;
            va.s = ua * q;
            va.t = ta * q;
            va.q = q;
            vb.s = ub * q;
            vb.t = tb * q;
        }
        if (bits(batch_.state.prim, 4, 1)) {
            Vertex pair[2] = {va, vb};
            texel_rect(pair, 2, fst);
            memcpy(va.rect, pair[0].rect, sizeof(va.rect));
            memcpy(vb.rect, pair[1].rect, sizeof(vb.rect));
        }
        Vertex c = va, d = va;
        c.x = vb.x;
        c.s = vb.s; // (x1, y0)
        d.y = vb.y;
        d.t = vb.t; // (x0, y1)
        if (moved) {
            // (corner 0 a's X and Y, 1 b's X a's Y, 2 a's X b's Y, 3 b's)
            Vertex *corner[4] = {&va, &c, &d, &vb};
            for (uint32_t i = 0; i < 4; i++) {
                corner[i]->kicks[0] = a.kick;
                corner[i]->kicks[1] = b.kick;
                corner[i]->kicks[2] = no_kick;
                corner[i]->kicks[3] = i | K_SPRITE | fst_bit;
            }
        }
        batch_.vertices.insert(batch_.vertices.end(), {va, c, d, c, vb, d});
        link_corners(std::span(batch_.vertices).last(6));
        touch(va);
        touch(vb);
        counts_.triangles += 2;
    }

    // Whether a primitive may draw over a pixel the open batch's primitives draw (their boxes overlap): a blend the
    // shader makes reads the destination as it was before the batch
    bool overlaps_batch(const GifPrimitive &p) const {
        PixelBox b = box_of(p, batch_.state.xyoffset);
        for (const PixelBox &o : batch_.boxes) {
            if (b.x0 <= o.x1 && o.x0 <= b.x1 && b.y0 <= o.y1 && o.y0 <= b.y1) {
                return true;
            }
        }
        return false;
    }

    void flush_batch() {
        if (!batch_open_) {
            return;
        }
        batch_open_ = false;
        if (!batch_.vertices.empty() || !batch_.runs.empty()) {
            draw_batch(batch_);
            if (keeping_) {
                kept_->keep(take_kept_batch());
            }
        }
        batch_.texture = BoundTexture(); // (a surface's copy let go once drawn)
        batch_.arena.reset();
        batch_.records.reset();
        batch_.triangles.reset();
    }

    // Cut-out AA: whether a batch is a cut-out, shaded at every sample of a multisampled target. A cut-out is textured
    // with the texture's alpha (TCC) and alpha-tested, and the alphas it can make fall on both sides of the test: the
    // netting, foliage, fences and grates, whose edges inside a primitive are where the texture's alpha crosses AREF.
    // Shaded once a pixel, a pixel is in or out whole; shaded at each sample, each sample is textured and tested at its
    // own place. The alphas it can make: the texture's least and most (known for a texture the GPU's sampler reads,
    // else any), by the texture function with the vertices' alphas (a GIF batch's; a native batch's taken as 0x80).
    // Not the 2D (FST: sprites, the HUD, the menus), which covers whole pixels.
    bool is_cutout(const Batch &b) const {
        return cutout_aa_ && cuts_out(b);
    }

    // (tolerant layers) a layer, as the native GS took it: a decal (is_decal) with a corner's alpha below 0x80, a rock's
    // face or a wall's shading fading out
    static bool is_layer(const Batch &b) {
        if (b.native || !is_decal(b)) {
            return false;
        }
        for (const Vertex &v : b.vertices) {
            if (v.rgba[3] < 0x80) {
                return true;
            }
        }
        return false;
    }

    // (is_cutout's, whatever the setting) the batch's alpha test keeps some of its pixels and drops others
    static bool cuts_out(const Batch &b) {
        const DrawState &s = b.state;
        if (!bits(s.prim, 4, 1) || !b.texture.ok || !bits(s.tex0, 34, 1) || bits(s.prim, 8, 1) ||
            !bits(s.test, 0, 1)) {
            return false;
        }
        auto [lo, hi] = alpha_range(b);
        uint32_t atst = bits(s.test, 1, 3), ref = bits(s.test, 4, 8);
        switch (atst) {
        case 2: // LESS
        case 5: // GEQUAL
            return lo < ref && hi >= ref;
        case 3: // LEQUAL
        case 6: // GREATER
            return lo <= ref && hi > ref;
        case 4: // EQUAL
        case 7: // NOTEQUAL
            return lo <= ref && ref <= hi && lo < hi;
        default: // NEVER, ALWAYS
            return false;
        }
    }

    // The least and most alpha a batch's pixels can have, before the alpha test: its vertices' (a native batch's taken as
    // 0x80), through the texture function with the texture's where it is read with TCC (any alpha, 0 to 255, where the
    // texture's are not known)
    struct AlphaRange {
        uint32_t lo, hi;
    };
    static AlphaRange alpha_range(const Batch &b) {
        const DrawState &s = b.state;
        uint32_t vlo = 0x80, vhi = 0x80;
        if (!b.native && !b.vertices.empty()) {
            vlo = 255;
            vhi = 0;
            for (const Vertex &v : b.vertices) {
                vlo = std::min<uint32_t>(vlo, v.rgba[3]);
                vhi = std::max<uint32_t>(vhi, v.rgba[3]);
            }
        }
        if (!bits(s.prim, 4, 1) || !bits(s.tex0, 34, 1)) {
            return {vlo, vhi}; // (untextured, or the texture's alpha not taken: the vertices')
        }
        uint32_t tlo = b.texture.ok ? b.texture.alpha_lo : 0, thi = b.texture.ok ? b.texture.alpha_hi : 255;
        uint32_t tfx = bits(s.tex0, 35, 2);
        uint32_t lo = tfx == 0 ? (vlo * tlo) >> 7 : tfx == 2 ? std::min(255u, vlo + tlo) : tlo;
        uint32_t hi = tfx == 0 ? std::min(255u, (vhi * thi) >> 7) : tfx == 2 ? std::min(255u, vhi + thi) : thi;
        return {lo, hi};
    }

    // A Z buffer cleared reaches the Z buffers of other widths over the same pages. A depth surface is one width's view
    // of the GS's memory (depth_surface's key: base, width, format), so a Z buffer the game reads at another width
    // than it clears it at kept, in its own image, whatever was last written at that width: the Heart of the Forest
    // (014) draws its water's reflection into a 256-wide target with ZTST GEQUAL and ZMSK over the 640-wide frame's Z
    // buffer, which the frame's clear has just zeroed; the reflection's own image held Z from frames before, most of
    // the reflection failed against it, and the water showed the black where it was not drawn (2026-11-11). A clear
    // here: a batch of sprites writing one Z with ZTST ALWAYS over whole rows of its surface's pages. Every other depth
    // surface of the format wholly in those pages is cleared to the same Z (any format for Z 0: no bits in any).
    void clear_depth_aliases(const Batch &b, const Surface &depth) {
        if (b.native || b.moved || !b.sprites || b.vertices.empty()) {
            return;
        }
        uint32_t z = b.vertices[0].z;
        for (const Vertex &v : b.vertices) {
            if (v.z != z) {
                return;
            }
        }
        uint64_t scissor = b.state.scissor;
        int32_t x0 = std::max(b.x0, int32_t(bits(scissor, 0, 11))), y0 = std::max(b.y0, int32_t(bits(scissor, 32, 11)));
        int32_t x1 = std::min(b.x1, int32_t(bits(scissor, 16, 11)) + 1); // (exclusive: a sprite's right edge)
        int32_t y1 = std::min(b.y1, int32_t(bits(scissor, 48, 11)) + 1);
        if (x0 > 0 || y0 > 0 || x1 < int32_t(depth.width()) || y1 <= 0) {
            return;
        }
        uint32_t rows = std::min(uint32_t(y1), depth.height) / page_rows(depth.psm);
        uint32_t end = depth.base_page + std::max(1u, depth.fbw) * rows;
        for (auto &o : depth_) {
            if (o.get() == &depth || o->base_page < depth.base_page || o->end_page() > end ||
                (o->psm != depth.psm && z != 0)) {
                continue;
            }
            uint32_t zmax = o->psm == 0x30 ? 0xFFFFFFFFu : o->psm == 0x31 ? 0xFFFFFFu : 0xFFFFu;
            float zscale = o->psm == 0x30 ? 1.0f / 4294967296.0f : 1.0f / 16777216.0f; // (pcr_prim.vert's depth)
            VkClearValue value = {};
            value.depthStencil.depth = float(std::min(z, zmax)) * zscale;
            end_pass();
            CommandBuffer &c = cmd(GpuWork::surface);
            full_barrier(c);
            c.clear_image(*o->image, value, VK_IMAGE_ASPECT_DEPTH_BIT);
            full_barrier(c);
            counts_depth_aliases_cleared_++;
        }
    }

    // ---- A kept walk's runs (#110, PERFORMANCE.md: merged draws) ----
    // A picture between draws its kept walk's batches again with the same state, batch after batch, as the frame did:
    // the driver's draw, Granite's state flush and the push constants a batch were most of the pump's time. Drawn
    // again, consecutive batches whose set 1 is the dummies (texture from the walk's table, CLUT the walk's) and whose
    // state is the same (the pass, scissor, program, depth, blend, write mask, blend constant, a native program's
    // buffers) are one run: their fields go in records of the picture's (Runs::buffer, set 1's u_records), their draws
    // in an indirect draw's commands after them, and the run is one vkCmdDrawIndirect with the runs' programs
    // (PCR_RECORDS: a draw's record is the push constants' first + gl_DrawID). Vulkan keeps the order of the
    // primitives across an indirect draw's draws, as across draws, so the pictures are the same. A run ends before
    // anything else is recorded (cmd(), end_pass, submit) and before a batch of other state sets its state.
    // KF4_PC_INDIRECT=0: a draw a batch, as before (to compare).
    struct RunKey {
        const void *colour = nullptr, *depth = nullptr;
        uint32_t width = 0, height = 0;
        int32_t scissor[4] = {};
        uint32_t fix = 0;
        int v = 0;
        const void *native[3] = {};
        // (a pass's)
        const Program *program = nullptr;
        uint32_t depth_state = 0, blend = 0, mask = 0;
        bool operator==(const RunKey &) const = default;
    };
    struct Runs {
        BufferHandle buffer;     // (the picture's records, then its commands)
        uint8_t *map = nullptr;  // (written as the batches are drawn)
        uint32_t capacity = 0, used = 0;
        VkDeviceSize commands_at = 0;
        bool open = false;       // (a run whose draws are not yet recorded: the command buffer's state is its)
        RunKey key;
        uint32_t first = 0, count = 0;
    };

    // A kept walk drawn again: room for its batches' draws, a pass at a time (two at most), as records and commands
    void start_runs(const Kept &walk) {
        end_runs();
        if (!runs_ok_ || !walk_table_ || !between_ || !between_->active()) {
            return;
        }
        uint64_t n = 0;
        for (const Batch &b : walk.batches) {
            n += b.native ? b.runs.size() : 1;
        }
        n *= 2;
        if (n == 0 || n > (1u << 20)) {
            return;
        }
        BufferCreateInfo info = {};
        info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
        info.size = n * (sizeof(PrimPush) + sizeof(VkDrawIndirectCommand));
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        runs_.buffer = device_.create_buffer(info, nullptr);
        runs_.map = runs_.buffer ? static_cast<uint8_t *>(device_.map_host_buffer(*runs_.buffer, MEMORY_ACCESS_WRITE_BIT))
                                 : nullptr;
        if (!runs_.map) {
            runs_.buffer.reset();
            return;
        }
        runs_.capacity = uint32_t(n);
        runs_.used = 0;
        runs_.commands_at = n * sizeof(PrimPush);
    }

    void end_runs() {
        flush_run();
        if (runs_.buffer) {
            device_.unmap_host_buffer(*runs_.buffer, MEMORY_ACCESS_WRITE_BIT);
        }
        runs_ = Runs();
    }

    // A batch's pass in the open run (one is opened, with this state, if none is): a record and a command a draw
    void add_to_run(CommandBuffer &c, const Batch &b, PrimPush &push, const RunKey &key) {
        uint32_t draws = b.native ? uint32_t(b.runs.size()) : 1;
        if (runs_.open && runs_.count + draws > max_run_draws_) {
            flush_run(); // (the device's most an indirect draw; the state is the run's, the same key)
        }
        uint32_t at = runs_.used;
        if (!runs_.open) {
            runs_.open = true;
            runs_.key = key;
            runs_.first = at;
            runs_.count = 0;
            push.record = at;
            c.push_constants(&push, 0, sizeof(push)); // (the record field only is read: the run's first)
            push.record = 0;
        }
        auto *records = reinterpret_cast<PrimPush *>(runs_.map);
        auto *commands = reinterpret_cast<VkDrawIndirectCommand *>(runs_.map + runs_.commands_at);
        if (b.native) {
            for (const Batch::Run &r : b.runs) {
                memcpy(&records[at], &push, sizeof(push));
                commands[at] = {r.count * 3, 1, r.first * 3, r.instance};
                at++;
            }
            counts_.native_draw_calls += b.runs.size();
        } else {
            memcpy(&records[at], &push, sizeof(push));
            commands[at] = {uint32_t(b.vertices.size()), 1, b.gpu_first, 0};
            at++;
        }
        runs_.count += at - runs_.used;
        runs_.used = at;
    }

    // The open run's draws recorded: the command buffer's state is still the run's
    void flush_run() {
        if (!runs_.open) {
            return;
        }
        runs_.open = false;
        cmd_->draw_indirect(*runs_.buffer, runs_.commands_at + VkDeviceSize(runs_.first) * sizeof(VkDrawIndirectCommand),
                            runs_.count, sizeof(VkDrawIndirectCommand));
        counts_.runs++;
        counts_.run_draws += runs_.count;
    }

    // The command buffer, to set state or record what does not end a run (draw_batch's)
    CommandBuffer &cmd_keeping_run() {
        if (!cmd_) {
            return cmd();
        }
        return *cmd_;
    }

    // A decal (developers' views: the trace marks its fragments, --decal-losses draws where it lost): a blended draw Z
    // tested GEQUAL whose blend shows what is under it somewhere (not blend_covers)
    static bool is_decal(const Batch &b) {
        const DrawState &s = b.state;
        return bits(s.test, 16, 1) != 0 && bits(s.test, 17, 2) == 2 && bits(s.prim, 6, 1) != 0 && !blend_covers(b);
    }

    // Whether a blended batch's blend writes its source colour alone at every pixel: ALPHA's (Cs - Cd) * C + Cd with C
    // 0x80 or more there (FIX, or As over its whole alpha_range), so that nothing under it shows
    static bool blend_covers(const Batch &b) {
        const DrawState &s = b.state;
        uint32_t a = bits(s.alpha, 0, 2), bb = bits(s.alpha, 2, 2), c = bits(s.alpha, 4, 2), d = bits(s.alpha, 6, 2);
        if (a != 0 || bb != 1 || d != 1) {
            return false;
        }
        if (c == 2) {
            return bits(s.alpha, 32, 8) >= 0x80;
        }
        return c == 0 && alpha_range(b).lo >= 0x80;
    }

    // A batch drawn (the open one, or a kept walk's again); the batch is not changed
    void draw_batch(const Batch &b) {
        const DrawState &s = b.state;
        counts_.batches++;
        uint32_t fbp = bits(s.frame, 0, 9), fbw = bits(s.frame, 16, 6), fpsm = bits(s.frame, 24, 6);
        uint64_t scissor = s.scissor;
        uint32_t scax0 = bits(scissor, 0, 11), scax1 = bits(scissor, 16, 11);
        uint32_t scay0 = bits(scissor, 32, 11), scay1 = bits(scissor, 48, 11);
        if (scax1 < scax0 || scay1 < scay0) {
            return;
        }
        bool zte = bits(s.test, 16, 1) != 0;
        uint32_t ztst = bits(s.test, 17, 2);
        bool ate = bits(s.test, 0, 1) != 0;
        uint32_t atst = bits(s.test, 1, 3), afail = bits(s.test, 12, 2);
        bool zwrite = zte && bits(s.zbuf, 32, 1) == 0;
        // the alpha test's passes: (pass mode, writes rgb, writes alpha, writes Z)
        struct Pass {
            uint32_t mode;
            bool rgb, alpha, z;
            bool layers = false; // (tolerant layers: the pass's colour tested half a Z nearer; PCR_F_LAYER_TEST)
            bool fine = false;   // (... a sixteenth of a Z: a cut-out's, PCR_F_LAYER_FINE)
            bool wide = false;   // (... a Z and half its slope: a decal's, PCR_F_LAYER_WIDE)
        };
        std::array<Pass, 4> passes{};
        int pass_count = 0;
        if (!ate || atst == 1) {
            passes[pass_count++] = {0, true, true, zwrite};
        } else {
            Pass fail = {2, afail == 1 || afail == 3, afail == 1, afail == 2 && zwrite};
            if (atst == 0) {
                fail.mode = 0;
                if (fail.rgb || fail.z) {
                    passes[pass_count++] = fail;
                }
            } else {
                passes[pass_count++] = {1, true, true, zwrite};
                if (afail != 0) {
                    passes[pass_count++] = fail;
                }
            }
        }
        if (pass_count == 0) {
            return;
        }
        // Tolerant layers (set_layer_z; pcr_prim.h's pcr_exact_depth): Z's fraction kept, so that where surfaces cross
        // the line is straight and among a batch's own triangles the nearer is kept. A layer (is_layer: a decal fading
        // out, a rock's face or a wall's shading) is drawn in two phases, the GS's tie kept on average: its Z written
        // exactly first, without colour (pcr_prim_zonly.frag, the GPU's own depth, where nothing is discarded; else the
        // pass with its alpha test), then its colour tested a Z and half its change across a GS pixel nearer (up to a
        // Z more), no Z written. A draw that writes no Z is tested within half a Z (a cut-out's a sixteenth: foliage
        // cards near each other). Everything else is drawn once, as the GS draws it with the fraction kept. (The native
        // GS's tolerant layers, 2026-10, 4af1dbd6 and 669bee01, drew every surface twice as well, for the GS's ties
        // between surfaces: here that was a second pass of every batch, +35-45% GPU time a picture and Andrew's profile
        // under 240 at 009, the vertex work of the native draws twice and the runs cut at every pass.)
        bool writes_z = false;
        for (int i = 0; i < pass_count; i++) {
            writes_z |= passes[i].z;
        }
        if (exact_z_ && layers_ && z_psm(s.zbuf) != 0x30 && zte && ztst == 2 && (!writes_z || is_layer(b))) {
            // (tolerant layers: a layer in two phases, a draw that writes no Z tested within its slack)
            bool wide = is_layer(b), fine = !wide && cuts_out(b);
            std::array<Pass, 4> zs{}, colours{};
            int nz = 0, nc = 0;
            for (int i = 0; i < pass_count; i++) {
                if (passes[i].z) {
                    zs[nz++] = {passes[i].mode, false, false, true, false};
                }
                if (passes[i].rgb || passes[i].alpha) {
                    colours[nc++] = {passes[i].mode, passes[i].rgb, passes[i].alpha, false, true, fine, wide};
                }
            }
            pass_count = 0;
            for (int i = 0; i < nz; i++) {
                passes[pass_count++] = zs[i];
            }
            for (int i = 0; i < nc; i++) {
                passes[pass_count++] = colours[i];
            }
        }
        // (a surface as tall as what is drawn in it, not its scissor: the game's scissors run past small targets, onto
        // the targets below them)
        uint32_t height = uint32_t(std::clamp(std::min(b.y1, int32_t(scay1)) + 1, 1, 2048));
        Placed placed = colour_surface(fbp, fbw, fpsm == GSN_PSMCT24 ? GSN_PSMCT32 : fpsm, height);
        Surface *colour = placed.surface;
        if (!colour) {
            return;
        }
        // DATE: by the stencil, which a DATE batch's pass has with its Z buffer's surface (ZBUF's, ZTE on or off)
        bool date = bits(s.test, 14, 1) != 0;
        bool date_stencil = date && stencil_on_;
        Surface *depth = nullptr;
        if (zte || date_stencil) {
            depth = depth_surface(bits(s.zbuf, 0, 9), fbw, z_psm(s.zbuf), height);
            // (the Vulkan pass is as large as its smallest attachment: a depth surface shorter than the colour one made
            // as tall. Not the other way: two targets a page row apart that share a Z buffer each grew the other by
            // that row whenever one was made again, up to 2048 rows: the under-water screen's copy over the water's
            // targets makes them again every frame)
            if (depth && depth->height < colour->height) {
                grow(*depth, colour->height);
            }
        }
        // MSAA: drawn into the colour surface's multisampled image (and its depth surface's, made at the samples); DATE
        // by the stencil only where each sample can be read (pcr_date_ms.frag), else by a copy
        bool ms = ensure_ms(*colour);
        if (depth && uint32_t(depth->image->get_create_info().samples) != (ms ? samples_ : 1u)) {
            depth = nullptr; // (no multisampled image for the colour: no memory)
        }
        date_stencil = date_stencil && (!ms || sample_shading_);
        zte = zte && depth != nullptr;
        zwrite = zwrite && zte;
        if (zwrite && ztst == 1) {
            clear_depth_aliases(b, *depth);
        }
        PixelBox box = {b.x0, b.y0, b.x1, b.y1}; // (GS pixels touched)
        if (b.moved) {
            // (a picture between's corners are where the shader moves them: anywhere in the scissor, within the surface
            // as tall as the frame drew it)
            box = {int32_t(scax0), int32_t(scay0),
                   std::min(int32_t(scax1), int32_t(colour->width()) - 1 - int32_t(placed.x)),
                   std::min(int32_t(scay1), int32_t(colour->height) - 1 - int32_t(placed.y))};
        }
        date_stencil = date_stencil && depth != nullptr;
        // (KF4_PC_STENCIL=0: the destination as it is before the draw, which the shader reads: a copy, as the draw
        // writes it)
        ImageHandle dest_copy;
        VkOffset2D dest_offset = {0, 0};
        if (date) {
            counts_.date_draws++;
            if (!date_stencil) {
                dest_copy = copy_of(*colour, GpuWork::date_copy);
            }
        }
        // a blend with no Vulkan form: the GS's in the shader, from a copy of the rectangle drawn in
        if (b.shader_blend) {
            counts_blends_by_shader_++;
            counts_native_shader_blends_ += b.native ? 1 : 0;
            if (!dest_copy) {
                VkRect2D rect = drawn_rect(b.state.scissor, box, *colour, placed);
                if (rect.extent.width == 0 || rect.extent.height == 0) {
                    return;
                }
                dest_copy = copy_of_rect(*colour, rect);
                dest_offset = rect.offset;
            }
        }
        // (a run open, Runs: these calls record nothing, so it is left open; the GPU's timing of it may land in this
        // batch's)
        times_.begin_draw(cmd_keeping_run(), batch_work(b),
                          batch_state_key(b, zte, zwrite, ztst, date, date_stencil, pass_count));
        begin_pass(*colour, depth, date_stencil ? PassReads::target : PassReads::nothing);
        CommandBuffer &c = cmd_keeping_run(); // (begin_pass made it; a pass begun ended any run)
        VkViewport vp = {0, 0, float(colour->image->get_width()), float(colour->image->get_height()), 0, 1};
        int32_t sx0 = int32_t((scax0 + placed.x) * scale_[0]), sy0 = int32_t((scay0 + placed.y) * scale_[1]);
        int32_t sx1 = std::min(int32_t((scax1 + 1 + placed.x) * scale_[0]), int32_t(colour->image->get_width()));
        int32_t sy1 = std::min(int32_t((scay1 + 1 + placed.y) * scale_[1]), int32_t(colour->image->get_height()));
        if (sx1 <= sx0 || sy1 <= sy0) {
            return;
        }
        VkRect2D scissor_px = {{sx0, sy0}, {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0)}};
        // (a clear drawn as a rectangle: cleared, vkCmdClearAttachments, the same pixels without the draw's shader)
        VkClearRect clear_rect;
        bool clear = as_clear(b, *colour, placed, zte, zwrite, ztst, date, passes[0], pass_count, scissor_px, clear_rect);
        // (cut-out AA: a cut-out's fragment shader at every sample, pcr_prim_sample.frag)
        bool per_sample = ms && is_cutout(b);
        int v = !b.native                              ? 0
                : b.program == NativeProgram::vu1_280 ? 2
                : b.program == NativeProgram::vu1_700 ? 3
                : b.program == NativeProgram::ripple  ? 4
                                                       : 1;
        BlendChoice blend = choose_blend(s.alpha, bits(s.prim, 6, 1) != 0);
        bool blend_supported = blend.supported;
        if (b.shader_blend) {
            blend = BlendChoice();
            blend.flags = F_SHADER_BLEND;
        }
        PrimPush push = {};
        push.inv_size[0] = 1.0f / float(colour->width());
        push.inv_size[1] = 1.0f / float(colour->height);
        uint32_t zpsm = z_psm(s.zbuf);
        push.zmax = zpsm == 0x30 ? 0xFFFFFFFFu : zpsm == 0x31 ? 0xFFFFFFu : 0xFFFFu;
        push.zscale = zpsm == 0x30 ? 1.0f / 4294967296.0f : 1.0f / 16777216.0f;
        bool tme = bits(s.prim, 4, 1) != 0;
        uint32_t flags = 0;
        if (tme && b.texture.ok) {
            flags |= F_TME | (bits(s.prim, 8, 1) ? F_FST : 0) | bits(s.tex0, 35, 2) << F_TFX_SHIFT |
                     (bits(s.tex0, 34, 1) ? F_TCC : 0) | b.texture.src << F_SRC_SHIFT |
                     (b.texture.src == SRC_SURFACE && is_colour_16(bits(s.tex0, 20, 6)) ? F_SURFACE_CT16 : 0);
        }
        flags |= bits(s.prim, 5, 1) ? F_FGE : 0;
        flags |= bits(s.fba, 0, 1) ? F_FBA : 0;
        flags |= blend.flags & ~0x80000000u;
        flags |= date && !date_stencil ? F_DATE | (bits(s.test, 15, 1) ? F_DATM : 0) : 0;
        flags |= ms ? F_MSAA : 0;
        // (the 2D mode's smooth: only the primitives with texels known read with it, sprites and 2D triangles, and
        // only where magnified: pcr_prim.h's smooth_2d)
        flags |= mode_2d_ == GSN_2D_SMOOTH && (scale_[0] > 1 || scale_[1] > 1) && (flags & F_TME) && !b.native &&
                         b.texture.src != SRC_SURFACE
                     ? F_SMOOTH_2D
                     : 0;
        flags |= colour->image->get_format() == VK_FORMAT_R16G16B16A16_UNORM ? F_HIGH : 0;
        flags |= is_decal(b) ? F_DECAL : 0;
        flags |= exact_z_ ? F_EXACT_Z : 0;
        // (tolerant layers: not in a Z32 buffer, the menus', whose Zs a float does not keep whole)
        flags |= exact_z_ && layers_ && z_psm(s.zbuf) != 0x30 ? F_LAYERS : 0;
        push.flags = flags;
        push.tex_size = (1u << std::min(bits(s.tex0, 26, 4), 10u)) | (1u << std::min(bits(s.tex0, 30, 4), 10u)) << 16;
        push.clamp_lo = uint32_t(s.clamp & 0xFFFFFFu);
        uint32_t fix = (blend.flags & 0x80000000u) ? 0 : bits(s.alpha, 32, 8);
        push.clamp_hi = uint32_t((s.clamp >> 24) & 0xFFFFFu) | fix << 24;
        push.test = atst | bits(s.test, 4, 8) << 3;
        push.fogcol = uint32_t(s.fogcol & 0xFFFFFF);
        push.texa = bits(s.texa, 0, 8) | bits(s.texa, 15, 1) << 15 | bits(s.texa, 32, 8) << 16;
        push.tex1 = bits(s.tex1, 5, 1) | bits(s.tex1, 6, 3) << 1 | bits(s.tex1, 2, 3) << 4 | bits(s.tex1, 0, 1) << 7 |
                    bits(s.tex1, 19, 2) << 8 | b.texture.sampling;
        int32_t k = int32_t(bits(s.tex1, 32, 12) << 20) >> 20; // (signed 7.4)
        push.lod_k = float(k) / 16.0f;
        push.levels = b.texture.levels;
        push.src_scale[0] = int32_t(scale_[0]);
        push.src_scale[1] = int32_t(scale_[1]);
        push.place[0] = int32_t(placed.x);
        push.place[1] = int32_t(placed.y);
        push.dest_offset[0] = dest_offset.x;
        push.dest_offset[1] = dest_offset.y;
        push.alpha = bits(s.alpha, 0, 8) | bits(s.alpha, 32, 8) << 8 | bits(s.colclamp, 0, 1) << 16 |
                     bits(s.pabe, 0, 1) << 17 | (fpsm == GSN_PSMCT24 ? 1u : 0u) << 18;
        // textures: the texels, the surface's copy, the sampled texture (each the dummy when not this batch's kind)
        const BoundTexture &bt = b.texture;
        bool sampled = bt.ok && bt.src == SRC_SAMPLED, from_surface = bt.ok && bt.src == SRC_SURFACE;
        bool texels = bt.ok && !sampled && !from_surface;
        // (a kept walk's batch: its texture from the walk's table, set 2, with its sampler from set 0's: set 1 stays the
        // dummies)
        bool from_table = walk_table_ && b.table_at != no_table && (texels || sampled);
        push.table = from_table ? b.table_at | b.sampler_at << 16 : no_table;
        bool walk_texture = walk_table_ && (texels || sampled);
        texels = texels && !from_table;
        sampled = sampled && !from_table;
        // (developers' pixel trace: the traced pixel in this batch's target, where its frame buffer is the spot's)
        bool traced = trace_.on && fbp == trace_.spot.fbp;
        push.trace_xy = no_trace;
        push.trace_draw = trace_.draw;
        if (traced) {
            trace_.x = (trace_.spot.x + int32_t(placed.x)) * int32_t(scale_[0]) + trace_.spot.sub_x;
            trace_.y = (trace_.spot.y + int32_t(placed.y)) * int32_t(scale_[1]) + trace_.spot.sub_y;
            push.trace_xy = uint32_t(trace_.x) | uint32_t(trace_.y) << 16;
            trace_.colour = colour;
            if (depth) {
                trace_.depth = depth;
                trace_.zscale = push.zscale;
            }
        }
        // (a kept walk's batch: its CLUT in the walk's buffer, set 0's, bind_between)
        bool indexed = (texels || from_table) && bt.src == SRC_INDEX;
        bool walk_clut = indexed && walk_cluts_ && b.clut_at != no_walk_clut;
        push.clut_at = walk_clut ? b.clut_at : no_walk_clut;
        float constants[4] = {float(bits(s.alpha, 32, 8)) / 128.0f, float(bits(s.alpha, 32, 8)) / 128.0f,
                              float(bits(s.alpha, 32, 8)) / 128.0f, 128.0f / 255.0f};
        // A kept walk's batch whose set 1 is the dummies (its texture from the table, its CLUT the walk's, no copy at
        // the draw) drawn in its picture's run (Runs): its fields a record, its draws an indirect draw's, with the
        // batches before it of the same state. Anything else ends the run first, before the state is set.
        uint32_t draws = b.native ? uint32_t(b.runs.size()) : 1;
        // (a traced batch drawn alone: its fragments that fail the test are recorded, so it is not tested early)
        bool in_run = runs_.map && between_ && between_->active() && !traced && !clear && !date && !b.shader_blend && !texels &&
                      !sampled && !from_surface && !dest_copy && !(indexed && !walk_clut) &&
                      (b.native ? native_runs_ok_ : walk_vertices_ && b.gpu_first != Batch::no_gpu_vertices) &&
                      runs_.used + draws * uint32_t(pass_count) <= runs_.capacity;
        // (a run's blend: its factors from the shader, dual-source, where they are ones it can give: Vulkan's blend of
        // the same values, so the same pixels; a batch unblended is one with factors one and zero. Its runs then do
        // not end where the blend changes: native 280 changes it about every batch)
        uint32_t cs = factor_code(blend.src), cd = factor_code(blend.dst);
        bool dual = in_run && dual_ok_ && blend.op == VK_BLEND_OP_ADD && cs != FACTOR_NONE && cd != FACTOR_NONE &&
                    ((!blend.enable && dual_unblended_) || (blend.enable && blend.src_alpha == VK_BLEND_FACTOR_ONE) ||
                     (blend.src_alpha == VK_BLEND_FACTOR_CONSTANT_ALPHA && (blend.flags & F_ALPHA_FACTOR)));
        push.factors = dual ? F_DUAL | cs | cd << 4 : 0;
        RunKey key = {};
        if (in_run) {
            key.colour = colour;
            key.depth = depth;
            key.width = colour->image->get_width();
            key.height = colour->image->get_height();
            key.scissor[0] = scissor_px.offset.x;
            key.scissor[1] = scissor_px.offset.y;
            key.scissor[2] = int32_t(scissor_px.extent.width);
            key.scissor[3] = int32_t(scissor_px.extent.height);
            key.fix = dual ? 0 : bits(s.alpha, 32, 8); // (the blend constant: read by no dual-source blend)
            key.v = v;
            if (b.native) {
                key.native[0] = b.arena.get();
                key.native[1] = b.records.get();
                key.native[2] = b.triangles.get();
            }
        }
        // (a pass's program, write mask and, in a run, its part of the run's state)
        auto pass_state = [&](int i, uint32_t &mask) {
            const Pass &pass = passes[i];
            // (a pass that discards nothing, no alpha test's pass and no DATE from a copy: tested before its shader)
            // (not where traced, whose fragments that fail the test are recorded too)
            bool early = pass.mode == 0 && !(flags & F_DATE) && !traced;
            Program *program = in_run ? (early ? (per_sample ? run_early_sample_programs_[v] : run_early_programs_[v])
                                               : (per_sample ? run_sample_programs_[v] : run_programs_[v]))
                                      : (early ? (per_sample ? early_sample_programs_[v] : early_programs_[v])
                                               : (per_sample ? sample_programs_[v] : programs_[v]));
            // (tolerant layers: a pass writing Z alone where nothing is discarded, the Z alone)
            if (early && pass.z && !pass.rgb && !pass.alpha && exact_z_) {
                program = in_run ? run_zonly_programs_[v] : zonly_programs_[v];
            }
            mask = (pass.rgb && blend.write_rgb ? 0x7u : 0u) | (pass.alpha ? 0x8u : 0u);
            if (in_run) {
                key.program = program;
                key.depth_state = (zte ? 1u : 0u) | (pass.z ? 2u : 0u) | ztst << 2;
                key.blend = dual         ? F_DUAL
                            : blend.enable ? 1u | uint32_t(blend.src) << 1 | uint32_t(blend.src_alpha) << 6 |
                                                 uint32_t(blend.dst) << 11 | uint32_t(blend.op) << 16
                                           : 0u;
                key.mask = mask;
            }
            return program;
        };
        uint32_t first_mask = 0;
        pass_state(0, first_mask);
        // (the run open ended here unless this batch's first pass continues it: the state set below is this batch's)
        if (runs_.open && (!in_run || !(runs_.key == key))) {
            flush_run();
        }
        if (!blend_supported && !b.shader_blend) {
            counts_.unsupported_blends++;
        }
        if (tme && !b.texture.ok) {
            counts_untextured_++;
        }
        counts_.table_draws += from_table ? 1 : 0;
        counts_.walk_texture_draws += walk_texture ? 1 : 0;
        c.set_viewport(vp);
        if (date_stencil) {
            set_up_date(c, *colour, drawn_rect(s.scissor, box, *colour, placed), bits(s.test, 15, 1) != 0, ms);
        }
        c.set_scissor(scissor_px);
        if (clear) {
            const Vertex &v0 = b.vertices[0];
            VkClearAttachment att[2] = {};
            att[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            att[0].colorAttachment = 0;
            uint8_t a = bits(s.fba, 0, 1) ? uint8_t(v0.rgba[3] | 0x80u) : v0.rgba[3];
            for (int i = 0; i < 3; i++) {
                att[0].clearValue.color.float32[i] = float(v0.rgba[i]) / 255.0f;
            }
            att[0].clearValue.color.float32[3] = float(a) / 255.0f;
            uint32_t count = 1;
            if (zwrite) {
                // (pcr_prim.vert's depth: Z within the format's, by its scale)
                uint32_t zp = z_psm(s.zbuf);
                uint32_t zmax = zp == 0x30 ? 0xFFFFFFFFu : zp == 0x31 ? 0xFFFFFFu : 0xFFFFu;
                float zscale = zp == 0x30 ? 1.0f / 4294967296.0f : 1.0f / 16777216.0f;
                att[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                att[1].clearValue.depthStencil.depth = float(std::min(v0.z, zmax)) * zscale;
                count = 2;
            }
            c.clear_quad(clear_rect, att, count);
            counts_clears_++;
            drawn_over(b, *colour, ms, box, zte && zwrite);
            return;
        }
        counts_msaa_.cutout_batches += per_sample ? 1 : 0;
        counts_.walk_clut_draws += walk_clut ? 1 : 0;
        c.set_opaque_state();
        c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        c.set_cull_mode(VK_CULL_MODE_NONE);
        c.set_depth_compare(ztst == 0 ? VK_COMPARE_OP_NEVER
                            : ztst == 1 ? VK_COMPARE_OP_ALWAYS
                            : ztst == 2 ? VK_COMPARE_OP_GREATER_OR_EQUAL
                                        : VK_COMPARE_OP_GREATER);
        c.set_storage_buffer(1, 6, *trace_buf_); // (pcr_prim.h's u_trace)
        // (set 1, the batch's textures and CLUT: the last set, so Granite pushes it, vkCmdPushDescriptorSet, rather
        // than allocating and writing a set each batch)
        c.set_texture(1, 0, texels ? *bt.view : dummy_uint_->get_view(), StockSampler::NearestClamp);
        c.set_texture(1, 1, from_surface ? *bt.view : dummy_float_->get_view(), StockSampler::NearestClamp);
        if (sampled) {
            c.set_texture(1, 4, *bt.view, *bt.sampler);
        } else {
            c.set_texture(1, 4, dummy_float_->get_view(), StockSampler::NearestClamp);
        }
        c.set_texture(1, 3, dest_copy ? dest_copy->get_view() : dummy_float_->get_view(), StockSampler::NearestClamp);
        if (indexed && !walk_clut) {
            void *clut = c.allocate_constant_data(1, 2, sizeof(b.clut));
            memcpy(clut, b.clut.data(), sizeof(b.clut));
        } else {
            c.set_uniform_buffer(1, 2, *no_clut_); // (read only for SRC_INDEX: one buffer, not a 1 KB copy a batch)
        }
        c.set_blend_constants(constants);
        bind_between(c, b.native ? &b : nullptr);
        if (in_run && !b.native) {
            // (a run's GIF batches: the walk's vertices from its start, each draw's first vertex its batch's place)
            c.set_vertex_binding(0, *walk_vertices_, 0, sizeof(Vertex));
        } else if (!b.native) {
            bind_vertices(c, b);
        }
        for (int i = 0; i < pass_count; i++) {
            const Pass &pass = passes[i];
            push.flags = (flags & ~(3u << F_PASS_SHIFT)) | pass.mode << F_PASS_SHIFT | (pass.layers ? F_LAYER_TEST : 0) |
                         (pass.fine ? F_LAYER_FINE : 0) | (pass.wide ? F_LAYER_WIDE : 0);
            uint32_t mask = 0;
            Program *program = pass_state(i, mask);
            // (a later pass: the run ends unless it continues it, before its state is set)
            if (in_run && i > 0 && runs_.open && !(runs_.key == key)) {
                flush_run();
            }
            c.set_program(program);
            c.set_depth_test(zte, pass.z);
            if (date_stencil) {
                draw_where_date_passes(c, pass.alpha, bits(s.fba, 0, 1) != 0, bits(s.test, 15, 1) != 0);
            }
            if (dual) {
                // (the colour's factors the shader's second output, the alpha its first as written: every such batch
                // of a run with one blend state)
                c.set_blend_enable(true);
                c.set_blend_factors(VK_BLEND_FACTOR_SRC1_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
                                    VK_BLEND_FACTOR_ZERO);
                c.set_blend_op(VK_BLEND_OP_ADD, VK_BLEND_OP_ADD);
            } else if (blend.enable) {
                c.set_blend_enable(true);
                c.set_blend_factors(blend.src, blend.src_alpha, blend.dst, VK_BLEND_FACTOR_ZERO);
                c.set_blend_op(blend.op, VK_BLEND_OP_ADD);
            } else {
                c.set_blend_enable(false);
            }
            c.set_color_write_mask(mask);
            if (in_run) {
                if (!b.native) {
                    set_vertex_attribs(c);
                }
                add_to_run(c, b, push, key);
                continue;
            }
            c.push_constants(&push, 0, sizeof(push));
            if (b.native) {
                for (const Batch::Run &r : b.runs) {
                    c.draw(r.count * 3, 1, r.first * 3, r.instance);
                    counts_.native_draw_calls++;
                }
                continue;
            }
            set_vertex_attribs(c);
            c.draw(uint32_t(b.vertices.size()));
        }
        if (decal_losses_ && zte && (flags & F_DECAL)) {
            draw_decal_losses(c, b, push, passes, pass_count, ztst, per_sample, v);
        }
        drawn_over(b, *colour, ms, box, zte && zwrite);
    }

    // (developers' decal losses) The decal batch b drawn again where it lost its depth test by less than
    // PCR_DECAL_LOSS_MARGIN: the test turned round, no Z written, each pass that writes colour with its alpha test. A
    // little magenta added where it lost, and taken away again where it lost by more than the margin (behind a nearer
    // surface): small, so that the decals over each other add up short of white (else the sum is clamped there, and
    // taking it away again leaves the colour short)
    template <typename Passes>
    void draw_decal_losses(CommandBuffer &c, const Batch &b, PrimPush push, const Passes &passes, int pass_count,
                           uint32_t ztst, bool per_sample, int v) {
        c.set_depth_compare(ztst == 2 ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_LESS_OR_EQUAL);
        c.set_blend_enable(true);
        c.set_blend_factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
        c.set_color_write_mask(0x7u);
        uint32_t flags = push.flags & ~(3u << F_PASS_SHIFT);
        push.trace_xy = no_trace;
        for (int i = 0; i < pass_count; i++) {
            if (!passes[i].rgb) {
                continue;
            }
            for (uint32_t far : {0u, uint32_t(F_DECAL_LOSS_FAR)}) {
                push.flags = flags | passes[i].mode << F_PASS_SHIFT | F_DECAL_LOSS | far;
                c.set_blend_op(far ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD, VK_BLEND_OP_ADD);
                c.set_program(per_sample ? sample_programs_[v] : programs_[v]);
                c.push_constants(&push, 0, sizeof(push));
                c.set_depth_test(true, false);
                if (b.native) {
                    for (const Batch::Run &r : b.runs) {
                        c.draw(r.count * 3, 1, r.first * 3, r.instance);
                    }
                    continue;
                }
                set_vertex_attribs(c);
                c.draw(uint32_t(b.vertices.size()));
            }
        }
    }

    static bool holds(const std::vector<std::unique_ptr<Surface>> &list, const Surface *s) {
        for (const auto &o : list) {
            if (o.get() == s) {
                return true;
            }
        }
        return false;
    }

    // After a batch: its multisampled image ahead of its image, and what was drawn over told to the texture store (its
    // pages are a surface's now, not the uploads')
    void drawn_over(const Batch &b, Surface &colour, bool ms, const PixelBox &box, bool wrote_z) {
        const DrawState &s = b.state;
        uint32_t fbp = bits(s.frame, 0, 9), fbw = bits(s.frame, 16, 6), fpsm = bits(s.frame, 24, 6);
        uint32_t scax0 = bits(s.scissor, 0, 11), scax1 = bits(s.scissor, 16, 11);
        uint32_t scay0 = bits(s.scissor, 32, 11), scay1 = bits(s.scissor, 48, 11);
        colour.ms_ahead |= ms;
        int32_t x0 = std::max(box.x0, int32_t(scax0)), x1 = std::min(box.x1, int32_t(scax1));
        int32_t y0 = std::max(box.y0, int32_t(scay0)), y1 = std::min(box.y1, int32_t(scay1));
        if (x1 >= x0 && y1 >= y0 && store_follows_draws_) {
            store_.drawn({fbp * 32, fbw, fpsm, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1)});
            if (wrote_z) {
                store_.drawn({bits(s.zbuf, 0, 9) * 32, fbw, z_psm(s.zbuf), uint32_t(x0), uint32_t(y0),
                              uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1)});
            }
        }
    }

    // A batch that is a clear drawn as a rectangle: one sprite of one colour and one Z, untextured, unblended, without
    // fog, every pixel written (no alpha test's pass, no DATE), Z always or no Z test, as the frame has it (not moved
    // in a picture between). Its pixels: the sprite within the scissor, at the canvas's scale (rect), when each edge of
    // the sprite inside the scissor is on a whole GS pixel (a sprite covers the pixels from its first edge to before its
    // last at any scale and with MSAA: gs_to_clip). The colour the shader would write (FBA's bit 7 with it), the depth
    // pcr_prim.vert's. KF4_PC_CLEARS=0: drawn (to compare).
    bool as_clear(const Batch &b, const Surface &colour, const Placed &placed, bool zte, bool zwrite, uint32_t ztst,
                  bool date, const auto &pass, int pass_count, const VkRect2D &scissor_px, VkClearRect &rect) const {
        const DrawState &s = b.state;
        if (!clears_on_ || b.native || b.shader_blend || b.moved || (between_ && between_->active()) || !b.sprites ||
            b.vertices.size() != 6 || bits(s.prim, 4, 3) != 0 || date || pass_count != 1 || pass.mode != 0 ||
            !pass.rgb || !pass.alpha || (zte && ztst != 1) || (zwrite && !zte)) {
            return false;
        }
        const Vertex &v0 = b.vertices[0];
        float x0 = v0.x, y0 = v0.y, x1 = v0.x, y1 = v0.y;
        for (const Vertex &v : b.vertices) {
            if (memcmp(v.rgba, v0.rgba, 4) != 0 || v.z != v0.z) {
                return false;
            }
            x0 = std::min(x0, v.x);
            y0 = std::min(y0, v.y);
            x1 = std::max(x1, v.x);
            y1 = std::max(y1, v.y);
        }
        // (the scissor in GS pixels of the surface, as scissor_px is)
        float sx0 = float(scissor_px.offset.x) / float(scale_[0]), sy0 = float(scissor_px.offset.y) / float(scale_[1]);
        float sx1 = float(scissor_px.offset.x + int32_t(scissor_px.extent.width)) / float(scale_[0]);
        float sy1 = float(scissor_px.offset.y + int32_t(scissor_px.extent.height)) / float(scale_[1]);
        x0 += float(placed.x);
        x1 += float(placed.x);
        y0 += float(placed.y);
        y1 += float(placed.y);
        auto whole_or_out = [](float e, float lo, float hi) { return e <= lo || e >= hi || e == std::floor(e); };
        if (!whole_or_out(x0, sx0, sx1) || !whole_or_out(x1, sx0, sx1) || !whole_or_out(y0, sy0, sy1) ||
            !whole_or_out(y1, sy0, sy1)) {
            return false;
        }
        int32_t px0 = std::max(scissor_px.offset.x, int32_t(std::max(x0, 0.0f)) * int32_t(scale_[0]));
        int32_t py0 = std::max(scissor_px.offset.y, int32_t(std::max(y0, 0.0f)) * int32_t(scale_[1]));
        int32_t px1 = std::min<int64_t>(scissor_px.offset.x + int32_t(scissor_px.extent.width),
                                        int64_t(std::max(x1, 0.0f)) * scale_[0]);
        int32_t py1 = std::min<int64_t>(scissor_px.offset.y + int32_t(scissor_px.extent.height),
                                        int64_t(std::max(y1, 0.0f)) * scale_[1]);
        px1 = std::min(px1, int32_t(colour.image->get_width()));
        py1 = std::min(py1, int32_t(colour.image->get_height()));
        if (px1 <= px0 || py1 <= py0) {
            return false; // (draws nothing: left to the draw, which draws nothing either)
        }
        rect = {{{px0, py0}, {uint32_t(px1 - px0), uint32_t(py1 - py0)}}, 0, 1};
        return true;
    }

    // A GIF batch's vertices bound for its passes: its place in the kept walk's buffer when drawn again from one, else
    // copied to the GPU. (A frame inside a surface: the vertex shader adds its place there, PrimPush::place.)
    void bind_vertices(CommandBuffer &c, const Batch &b) {
        if (walk_vertices_ && b.gpu_first != Batch::no_gpu_vertices) {
            c.set_vertex_binding(0, *walk_vertices_, VkDeviceSize(b.gpu_first) * sizeof(Vertex), sizeof(Vertex));
            return;
        }
        size_t bytes = b.vertices.size() * sizeof(Vertex);
        memcpy(c.allocate_vertex_data(0, bytes, sizeof(Vertex)), b.vertices.data(), bytes);
    }

    // A batch's rectangle in its colour surface's pixels (at the canvas's scale): its primitives' box within its
    // scissor (a native batch's: its scissor), within the surface; empty when it draws nothing
    VkRect2D drawn_rect(uint64_t scissor, const PixelBox &box, const Surface &colour, const Placed &placed) const {
        int32_t x0 = std::max(box.x0, int32_t(bits(scissor, 0, 11)));
        int32_t x1 = std::min(box.x1, int32_t(bits(scissor, 16, 11)));
        int32_t y0 = std::max(box.y0, int32_t(bits(scissor, 32, 11)));
        int32_t y1 = std::min(box.y1, int32_t(bits(scissor, 48, 11)));
        int32_t w = int32_t(colour.image->get_width()), h = int32_t(colour.image->get_height());
        int32_t rx0 = std::clamp((x0 + int32_t(placed.x)) * int32_t(scale_[0]), 0, w);
        int32_t ry0 = std::clamp((y0 + int32_t(placed.y)) * int32_t(scale_[1]), 0, h);
        int32_t rx1 = std::clamp((x1 + 1 + int32_t(placed.x)) * int32_t(scale_[0]), 0, w);
        int32_t ry1 = std::clamp((y1 + 1 + int32_t(placed.y)) * int32_t(scale_[1]), 0, h);
        if (rx1 <= rx0 || ry1 <= ry0) {
            return {{0, 0}, {0, 0}};
        }
        return {{rx0, ry0}, {uint32_t(rx1 - rx0), uint32_t(ry1 - ry0)}};
    }

    // ---- DATE by the stencil (PC_RENDERER.md, "How it draws") ----
    // The GS draws a DATE pixel only where the target's alpha bit 7 is DATM as it is then. Over the rectangle the batch
    // draws in (its primitives' box within its scissor; a native batch's scissor), the stencil is cleared and set
    // where the target's bit is DATM, read in the pass (pcr_date.frag); the batch then draws where it is set. The
    // scissor is the rectangle's after (the caller sets the batch's again). rect: drawn_rect's.
    void set_up_date(CommandBuffer &c, const Surface &colour, const VkRect2D &rect, bool datm, bool ms) {
        counts_date_by_stencil_++;
        if (rect.extent.width == 0 || rect.extent.height == 0) {
            return; // (the batch draws nothing within its scissor)
        }
        VkClearValue zero = {};
        c.clear_quad(0, {rect, 0, 1}, zero, VK_IMAGE_ASPECT_STENCIL_BIT);
        in_pass_barrier(c);
        c.set_program(ms ? date_ms_program_ : date_program_); // (MSAA: each sample's stencil from its own alpha)
        c.set_opaque_state();
        c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        c.set_cull_mode(VK_CULL_MODE_NONE);
        c.set_depth_test(false, false);
        c.set_color_write_mask(0);
        c.set_stencil_test(true);
        c.set_stencil_ops(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
        c.set_stencil_reference(1, 1, 1);
        c.set_scissor(rect);
        PrimPush push = {};
        push.inv_size[0] = 1.0f / float(colour.width());
        push.inv_size[1] = 1.0f / float(colour.height);
        push.flags = (datm ? F_DATM : 0) | (ms ? F_MSAA : 0);
        push.src_scale[0] = int32_t(scale_[0]); // (gs_to_clip's)
        push.src_scale[1] = int32_t(scale_[1]);
        c.push_constants(&push, 0, sizeof(push));
        c.set_input_attachments(1, 5);
        bind_between(c, nullptr);
        // (a triangle over the whole target: the scissor is the rectangle)
        float fw = float(colour.width()), fh = float(colour.height);
        Vertex corners[3] = {};
        corners[0].x = -1.0f;
        corners[0].y = -1.0f;
        corners[1].x = 2.0f * fw + 1.0f;
        corners[1].y = -1.0f;
        corners[2].x = -1.0f;
        corners[2].y = 2.0f * fh + 1.0f;
        void *v = c.allocate_vertex_data(0, sizeof(corners), sizeof(Vertex));
        memcpy(v, corners, sizeof(corners));
        set_vertex_attribs(c);
        c.draw(3);
    }

    // A pass of a DATE batch drawn where the stencil is set. A pixel it writes with FBA has alpha bit 7 set, so with
    // DATM 0 the primitives after it in the batch may no longer draw there: its stencil is cleared. A pixel whose
    // alpha it writes without FBA has the bit the fragment's (not known to the stencil): kept, and counted.
    void draw_where_date_passes(CommandBuffer &c, bool writes_alpha, bool fba, bool datm) {
        c.set_stencil_test(true);
        VkStencilOp written = writes_alpha && fba && !datm ? VK_STENCIL_OP_ZERO : VK_STENCIL_OP_KEEP;
        c.set_stencil_ops(VK_COMPARE_OP_EQUAL, written, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
        c.set_stencil_reference(1, 1, 1);
        if (writes_alpha && !fba) {
            counts_date_alpha_unknown_++;
        }
    }

    // Set 0, the same in every batch program (so that a change of program keeps it bound, and the textures' set 1,
    // the last, is the one Granite pushes): the pictures between's picture, kicks and moves (zeros outside a picture
    // between), and each native program's arena, records and triangles in bindings of its own (3-5 native 000, 6-8
    // 280, 9-11 700, 12-14 the ripple's: a frame's batches of one program share their buffers, so the set stays the
    // same across a walk). The buffers a batch does not read are the last bound there, or a placeholder; Granite
    // writes a set again only when a binding changes, and forgets them at each render pass (set0_fresh_).
    static constexpr uint32_t native_slots = 4;
    static constexpr uint32_t walk_clut_binding = 3 + 3 * native_slots; // (the fragment stage's, 15)
    static constexpr uint32_t walk_sampler_binding = walk_clut_binding + 1; // (16-31)
    static constexpr uint32_t records_binding = 7; // (set 1's: the picture's records, Runs)
    static uint32_t native_slot(NativeProgram program) {
        return program == NativeProgram::vu1_000   ? 0
               : program == NativeProgram::vu1_280 ? 1
               : program == NativeProgram::vu1_700 ? 2
                                                    : 3;
    }
    void bind_between(CommandBuffer &c, const Batch *native) {
        bool on = between_ && between_->active();
        c.set_storage_buffer(0, 0, on ? between_->picture() : *picture_);
        c.set_storage_buffer(0, 1, on ? between_->kicks() : *picture_);
        c.set_storage_buffer(0, 2, on ? between_->moves() : *picture_);
        c.set_storage_buffer(0, walk_clut_binding, walk_cluts_ ? *walk_cluts_ : *picture_); // (pcr_prim.h's u_walk_cluts)
        c.set_storage_buffer(1, records_binding, runs_.buffer ? *runs_.buffer : *picture_); // (pcr_common.h's u_records)
        if (native) {
            uint32_t at = 3 * native_slot(native->program);
            const BufferHandle *mine[3] = {&native->arena, &native->records, &native->triangles};
            for (uint32_t i = 0; i < 3; i++) {
                if (last_native_[at + i].get() != mine[i]->get()) {
                    last_native_[at + i] = *mine[i];
                }
                c.set_storage_buffer(0, 3 + at + i, *last_native_[at + i]);
            }
        }
        if (!set0_fresh_) {
            for (uint32_t i = 0; i < 3 * native_slots; i++) {
                c.set_storage_buffer(0, 3 + i, last_native_[i] ? *last_native_[i] : *picture_);
            }
            set0_fresh_ = true;
            samplers_fresh_ = false;
        }
        // (a kept walk's samplers, u_samplers: set again only when they change, or at a render pass)
        const auto &samplers = walk_table_ ? walk_table_->samplers : no_samplers_;
        if (!samplers_fresh_ || samplers != samplers_bound_) {
            for (uint32_t i = 0; i < walk_samplers; i++) {
                c.set_sampler(0, walk_sampler_binding + i, samplers[i] ? *samplers[i] : *samplers[0]);
            }
            samplers_bound_ = samplers;
            samplers_fresh_ = true;
        }
        bind_table(c);
    }

    // Set 2, the table: the kept walk's while one is drawn again, else the canvas's own (one dummy, read by nothing)
    void bind_table(CommandBuffer &c) {
        const BindlessDescriptorSet &want = walk_table_ ? walk_table_->table.set : no_table_.set;
        if (!want || want.handle.set == table_bound_) {
            return;
        }
        c.set_bindless(2, want);
        table_bound_ = want.handle.set;
    }

    void set_vertex_attribs(CommandBuffer &c) {
        c.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x));
        c.set_vertex_attrib(1, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, z));
        c.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, rgba));
        c.set_vertex_attrib(3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, s));
        c.set_vertex_attrib(4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, fog));
        c.set_vertex_attrib(5, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(Vertex, kicks));
        c.set_vertex_attrib(6, 0, VK_FORMAT_R32G32_UINT, offsetof(Vertex, rect));
        c.set_vertex_attrib(7, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, zref));
    }

    // The depth surfaces' format: with a stencil (DATE's) where the device has one, else D32F alone and DATE by a copy
    void choose_depth_format() {
        const VkFormatFeatureFlags2 need = VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
        bool asked = !getenv("KF4_PC_STENCIL") || strcmp(getenv("KF4_PC_STENCIL"), "0") != 0;
        for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
            if (asked && device_.image_format_is_supported(f, need)) {
                depth_format_ = f;
                stencil_on_ = true;
                return;
            }
        }
        if (asked) {
            fprintf(stderr, "[pcr] no depth format with a stencil: DATE by a copy of the target\n");
        }
    }

    // ---- Colour precision ----
    // Colour high (GSN_COLOUR_HIGH): the colour surfaces 16 bits a channel (16-bit UNORM holds every 8-bit value exactly,
    // v * 257, so what the GS's arithmetic writes reads back as it is), the colours' arithmetic in floats (pcr_prim.h,
    // F_HIGH), the picture dithered to 8 bits as it is scanned out (pcr_narrow.frag). Where the GPU cannot draw into the
    // format, exact.
    VkFormat colour_format() const {
        return colour_mode_ == GSN_COLOUR_HIGH ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    }

    bool high_colour_ok() {
        const VkFormatFeatureFlags2 need = VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT |
                                           VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_BLIT_SRC_BIT |
                                           VK_FORMAT_FEATURE_2_BLIT_DST_BIT | VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
                                           VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
        if (high_ok_ < 0) {
            high_ok_ = device_.image_format_is_supported(VK_FORMAT_R16G16B16A16_UNORM, need) ? 1 : 0;
        }
        return high_ok_ == 1;
    }

    void set_colour(int mode) {
        if (mode == GSN_COLOUR_HIGH && !high_colour_ok()) {
            if (!colour_refused_) {
                fprintf(stderr, "[pcr] colour high: this GPU cannot draw 16-bit colour surfaces, so exact\n");
                colour_refused_ = true;
            }
            mode = GSN_COLOUR_EXACT;
        }
        pending_colour_ = mode == GSN_COLOUR_HIGH ? GSN_COLOUR_HIGH : GSN_COLOUR_EXACT;
    }

    // The precision changed (from a vblank, as the scale): every colour surface made again in the new format from its
    // pixels (a blit converts), its samples made again from it (ensure_ms). (The pipelines made ahead for a change of
    // MSAA are made against the format of the run's first change of samples, so the other precision's are made as
    // drawn: warm_images_ is read by the warming thread and not made again here.)
    void apply_colour() {
        if (pending_colour_ == colour_mode_) {
            return;
        }
        end_pass();
        for (auto &s : colour_) {
            resolve(*s);
            s->ms.reset();
            s->ms_ahead = s->ms_behind = false;
        }
        colour_mode_ = pending_colour_;
        for (auto &s : colour_) {
            ImageHandle image = make_surface_image(s->width(), s->height, false);
            if (!image) {
                continue;
            }
            CommandBuffer &c = cmd(GpuWork::surface);
            full_barrier(c);
            c.blit_image(*image, *s->image, {0, 0, 0}, {int32_t(image->get_width()), int32_t(image->get_height()), 1},
                         {0, 0, 0}, {int32_t(s->image->get_width()), int32_t(s->image->get_height()), 1}, 0, 0, 0, 0, 1,
                         VK_FILTER_NEAREST);
            full_barrier(c);
            s->image = image;
        }
        kept_frame_.reset(); // (of the old format: a picture between reads its buffer until the next 0xF2)
    }

    // ---- Scale ----
    void apply_scale() {
        apply_samples();
        apply_colour();
        if (pending_scale_ == scale_) {
            return;
        }
        end_pass();
        for (auto &s : colour_) {
            resolve(*s); // (the new images made from the old ones' pixels; their samples made again: ensure_ms)
            s->ms.reset();
            s->ms_ahead = s->ms_behind = false;
        }
        std::array<uint32_t, 2> old = scale_;
        scale_ = pending_scale_;
        for (auto *list : {&colour_, &depth_}) {
            for (auto &s : *list) {
                ImageHandle image = make_surface_image(s->width(), s->height, s->depth);
                if (!image) {
                    continue;
                }
                CommandBuffer &c = cmd(GpuWork::surface);
                full_barrier(c);
                if (s->depth) {
                    VkClearValue zero = {};
                    c.clear_image(*image, zero, VK_IMAGE_ASPECT_DEPTH_BIT);
                } else {
                    c.blit_image(*image, *s->image, {0, 0, 0},
                                 {int32_t(image->get_width()), int32_t(image->get_height()), 1}, {0, 0, 0},
                                 {int32_t(s->image->get_width()), int32_t(s->image->get_height()), 1}, 0, 0, 0, 0, 1,
                                 VK_FILTER_NEAREST);
                }
                full_barrier(c);
                s->image = image;
            }
        }
        (void)old;
    }

    // Small images of the surfaces' colour and depth formats at samples: the attachments the pipelines made ahead are
    // made against (pcr_pipelines.h)
    struct WarmImages {
        ImageHandle colour, depth;
    };
    WarmImages make_warm_images(uint32_t samples) {
        WarmImages w;
        ImageCreateInfo info = ImageCreateInfo::render_target(16, 16, colour_format());
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        info.samples = VkSampleCountFlagBits(samples);
        w.colour = device_.create_image(info, nullptr);
        info = ImageCreateInfo::render_target(16, 16, depth_format_);
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        info.samples = VkSampleCountFlagBits(samples);
        w.depth = device_.create_image(info, nullptr);
        return w;
    }

    // MSAA's samples changed (from a vblank, as the scale): the colour surfaces' multisampled images resolved and let go
    // (made again at the new count as each is drawn into: ensure_ms), the depth surfaces made again at it, cleared (the
    // game draws its Z anew each frame, as apply_scale has it)
    void apply_samples() {
        if (pending_samples_ == samples_) {
            return;
        }
        // (drawn at the old count until the pipelines are made at the new one: pcr_pipelines.h)
        WarmImages &w = warm_images_[pending_samples_];
        if (!w.colour) {
            w = make_warm_images(pending_samples_);
        }
        if (w.colour && !warm_ready(pending_samples_, w.colour->get_view(), w.depth ? &w.depth->get_view() : nullptr)) {
            counts_msaa_.vblanks_waited++;
            return;
        }
        end_pass();
        for (auto &s : colour_) {
            resolve(*s);
            s->ms.reset();
            s->ms_ahead = s->ms_behind = false;
        }
        samples_ = pending_samples_;
        for (auto &s : depth_) {
            ImageHandle image = make_surface_image(s->width(), s->height, true);
            if (!image) {
                continue;
            }
            CommandBuffer &c = cmd(GpuWork::surface);
            full_barrier(c);
            VkClearValue zero = {};
            c.clear_image(*image, zero, VK_IMAGE_ASPECT_DEPTH_BIT);
            full_barrier(c);
            s->image = image;
        }
    }

    void make_programs() {
        // Every batch program's vertex stage has the same set 0 (bind_between: the pictures between's three buffers,
        // a native batch's three), so a change of program keeps it; the fragment stage's set 1 (the textures and the
        // CLUT) is the last set, which Granite pushes (vkCmdPushDescriptorSet) instead of allocating a set a batch
        ResourceLayout batch_vert = {};
        batch_vert.push_constant_size = sizeof(PrimPush);
        batch_vert.sets[0].storage_buffer_mask = (1u << (3 + 3 * native_slots)) - 1; // (bind_between's)
        for (uint32_t b = 0; b < 3 + 3 * native_slots; b++) {
            batch_vert.sets[0].meta[b].array_size = 1;
        }
        // (set 1's records of the picture's runs, u_records: read by the runs' programs, PCR_RECORDS, in both stages;
        // declared in every batch program, so that one and a run's have the same layout and a change binds nothing)
        batch_vert.sets[1].storage_buffer_mask = 1u << records_binding;
        batch_vert.sets[1].meta[records_binding].array_size = 1;
        ResourceLayout vert = batch_vert;
        vert.input_mask = 0x7F;
        ResourceLayout frag = {};
        frag.output_mask = 1;
        frag.push_constant_size = sizeof(PrimPush);
        frag.sets[1].sampled_image_mask = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4);
        frag.sets[1].fp_mask = (1u << 1) | (1u << 3) | (1u << 4);
        frag.sets[1].uniform_buffer_mask = 1u << 2;
        frag.sets[1].storage_buffer_mask = 1u << 6; // (the developers' pixel trace)
        for (int b : {0, 1, 2, 3, 4, 6}) {
            frag.sets[1].meta[b].array_size = 1;
        }
        frag.sets[1].storage_buffer_mask |= 1u << records_binding;
        frag.sets[1].meta[records_binding].array_size = 1;
        // (set 0's one binding of the fragment stage: a kept walk's CLUTs, u_walk_cluts; the DATE programs declare it
        // too, so that set 0's layout is the same in every program a batch draws with)
        frag.sets[0].storage_buffer_mask = 1u << walk_clut_binding;
        frag.sets[0].meta[walk_clut_binding].array_size = 1;
        // (and a kept walk's samplers, u_samplers; set 2 its table, u_table: Granite's bindless set, one binding)
        frag.sets[0].sampler_mask = 1u << walk_sampler_binding;
        frag.sets[0].meta[walk_sampler_binding].array_size = walk_samplers;
        frag.sets[2].separate_image_mask = 1u;
        frag.sets[2].meta[0].array_size = DescriptorSetLayout::UNSIZED_ARRAY;
        frag.bindless_set_mask = 1u << 2;
        Shader *vs = device_.request_shader(pcr_spv_pcr_prim_vert, sizeof(pcr_spv_pcr_prim_vert), &vert);
        // (the fragment shaders with exact Z, _xz, where it is on: pcr_common.h)
        auto shader = [&](const uint32_t *code, size_t size, const uint32_t *exact, size_t exact_size,
                          const ResourceLayout *layout) {
            return exact_z_ ? device_.request_shader(exact, exact_size, layout) : device_.request_shader(code, size, layout);
        };
#define PCR_SHADER(name, layout) \
    shader(pcr_spv_##name, sizeof(pcr_spv_##name), pcr_spv_##name##_xz, sizeof(pcr_spv_##name##_xz), layout)
        Shader *fs = PCR_SHADER(pcr_prim_frag, &frag), *sfs = PCR_SHADER(pcr_prim_sample_frag, &frag);
        Shader *efs = PCR_SHADER(pcr_prim_early_frag, &frag), *esfs = PCR_SHADER(pcr_prim_sample_early_frag, &frag);
        Shader *zfs = PCR_SHADER(pcr_prim_zonly_frag, &frag);
#define PCR_VERTEX_SHADER(name, layout) device_.request_shader(pcr_spv_##name, sizeof(pcr_spv_##name), layout)
        // (each vertex shader with the four fragment shaders: draw_batch's choice)
        auto programs_for = [&](int v, Shader *vertex) {
            programs_[v] = device_.request_program(vertex, fs);
            sample_programs_[v] = device_.request_program(vertex, sfs);
            early_programs_[v] = device_.request_program(vertex, efs);
            early_sample_programs_[v] = device_.request_program(vertex, esfs);
            zonly_programs_[v] = device_.request_program(vertex, zfs);
        };
        programs_for(0, vs);
        // (the same programs reading their fields from the picture's records: a kept walk's runs, draw_batch)
        Shader *rfs = PCR_SHADER(pcr_prim_frag_rec, &frag), *rsfs = PCR_SHADER(pcr_prim_sample_frag_rec, &frag);
        Shader *refs = PCR_SHADER(pcr_prim_early_frag_rec, &frag);
        Shader *resfs = PCR_SHADER(pcr_prim_sample_early_frag_rec, &frag);
        Shader *rzfs = PCR_SHADER(pcr_prim_zonly_frag_rec, &frag);
        auto run_programs_for = [&](int v, Shader *vertex) {
            run_programs_[v] = device_.request_program(vertex, rfs);
            run_sample_programs_[v] = device_.request_program(vertex, rsfs);
            run_early_programs_[v] = device_.request_program(vertex, refs);
            run_early_sample_programs_[v] = device_.request_program(vertex, resfs);
            run_zonly_programs_[v] = device_.request_program(vertex, rzfs);
        };
        run_programs_for(0, PCR_VERTEX_SHADER(pcr_prim_vert_rec, &vert));
        ResourceLayout date = {};
        date.push_constant_size = sizeof(PrimPush);
        date.sets[0] = frag.sets[0];
        date.sets[2] = frag.sets[2];
        date.bindless_set_mask = frag.bindless_set_mask;
        date.sets[1].input_attachment_mask = 1u << 5;
        date.sets[1].fp_mask = 1u << 5;
        date.sets[1].meta[5].array_size = 1;
        Shader *dfs = device_.request_shader(pcr_spv_pcr_date_frag, sizeof(pcr_spv_pcr_date_frag), &date);
        date_program_ = device_.request_program(vs, dfs);
        Shader *dmfs = device_.request_shader(pcr_spv_pcr_date_ms_frag, sizeof(pcr_spv_pcr_date_ms_frag), &date);
        date_ms_program_ = device_.request_program(vs, dmfs);
        ResourceLayout cover = {};
        ResourceLayout expand = {};
        expand.output_mask = 1;
        expand.sets[0].sampled_image_mask = 1u;
        expand.sets[0].fp_mask = 1u;
        expand.sets[0].meta[0].array_size = 1;
        Shader *cvs = device_.request_shader(pcr_spv_pcr_cover_vert, sizeof(pcr_spv_pcr_cover_vert), &cover);
        Shader *xfs = device_.request_shader(pcr_spv_pcr_expand_frag, sizeof(pcr_spv_pcr_expand_frag), &expand);
        expand_program_ = device_.request_program(cvs, xfs);
        ResourceLayout narrow = expand;
        narrow.push_constant_size = 8 * sizeof(int32_t);
        Shader *nfs = device_.request_shader(pcr_spv_pcr_narrow_frag, sizeof(pcr_spv_pcr_narrow_frag), &narrow);
        narrow_program_ = device_.request_program(cvs, nfs);
        ResourceLayout nd = batch_vert;
        Shader *nvs = device_.request_shader(pcr_spv_pcr_nd000_vert, sizeof(pcr_spv_pcr_nd000_vert), &nd);
        programs_for(1, nvs);
        Shader *nvs280 = device_.request_shader(pcr_spv_pcr_nd280_vert, sizeof(pcr_spv_pcr_nd280_vert), &nd);
        programs_for(2, nvs280);
        Shader *nvs700 = device_.request_shader(pcr_spv_pcr_nd700_vert, sizeof(pcr_spv_pcr_nd700_vert), &nd);
        programs_for(3, nvs700);
        Shader *nvsripple = device_.request_shader(pcr_spv_pcr_ndripple_vert, sizeof(pcr_spv_pcr_ndripple_vert), &nd);
        programs_for(4, nvsripple);
        run_programs_for(1, PCR_VERTEX_SHADER(pcr_nd000_vert_rec, &nd));
        run_programs_for(2, PCR_VERTEX_SHADER(pcr_nd280_vert_rec, &nd));
        run_programs_for(3, PCR_VERTEX_SHADER(pcr_nd700_vert_rec, &nd));
        run_programs_for(4, PCR_VERTEX_SHADER(pcr_ndripple_vert_rec, &nd));
#undef PCR_SHADER
#undef PCR_VERTEX_SHADER
        // (outside a picture between: its picture zeros, and no kicks or moves, read by nothing)
        BufferCreateInfo info = {};
        info.domain = BufferDomain::Device;
        info.size = 256;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        std::array<uint8_t, 256> zeros{};
        picture_ = device_.create_buffer(info, zeros.data());
        BufferCreateInfo clut = {};
        clut.domain = BufferDomain::Device;
        clut.size = sizeof(Batch::clut);
        clut.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        std::array<uint8_t, sizeof(Batch::clut)> no_colours{};
        no_clut_ = device_.create_buffer(clut, no_colours.data());
        // (the developers' pixel trace: bound to every draw, written by none until trace_pixel makes its own)
        BufferCreateInfo trace = {};
        trace.domain = BufferDomain::Device;
        trace.size = 64;
        trace.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        std::array<uint8_t, 64> no_trace_bytes{};
        trace_buf_ = device_.create_buffer(trace, no_trace_bytes.data());
        ResourceLayout probe = {};
        probe.push_constant_size = 7 * sizeof(uint32_t);
        probe.sets[0].sampled_image_mask = 3u;
        probe.sets[0].fp_mask = 3u;
        probe.sets[0].storage_buffer_mask = 4u;
        for (int b : {0, 1, 2}) {
            probe.sets[0].meta[b].array_size = 1;
        }
        Shader *pcs = device_.request_shader(pcr_spv_pcr_probe_comp, sizeof(pcr_spv_pcr_probe_comp), &probe);
        probe_program_ = device_.request_program(pcs);
        Shader *pmcs = device_.request_shader(pcr_spv_pcr_probe_ms_comp, sizeof(pcr_spv_pcr_probe_ms_comp), &probe);
        probe_ms_program_ = device_.request_program(pmcs);
        // (their pipelines made again ahead of a change of MSAA's samples: pcr_pipelines.h)
        WarmProgram warmed[23];
        for (int v = 0; v < 5; v++) {
            warmed[v] = {programs_[v], sample_programs_[v]};
            warmed[5 + v] = {early_programs_[v], early_sample_programs_[v]};
        }
        warmed[10] = {date_program_, date_ms_program_};
        for (int v = 0; v < 5; v++) {
            warmed[11 + v] = {run_programs_[v], run_sample_programs_[v]};
            warmed[16 + v] = {run_early_programs_[v], run_early_sample_programs_[v]};
        }
        // (the Z alone: the GIF batches' and a kept walk's, the native draws' made as first drawn)
        warmed[21] = {zonly_programs_[0], zonly_programs_[0]};
        warmed[22] = {run_zonly_programs_[0], run_zonly_programs_[0]};
        warm_watch(device_, warmed, 23, expand_program_);
    }

    Device &device_;
    const DynamicChoice dynamic_ = choose_dynamic(device_);
    GpuTimes &times_;
    // the batches' programs: the GIF's (pcr_prim.vert), native 000, 280, 700, the ripple's; each shaded once a pixel,
    // and once a sample (cut-out AA: pcr_prim_sample.frag)
    std::array<Program *, 5> programs_{}, sample_programs_{};
    // ... and the same for the draws that discard nothing, the depth test before the shader (pcr_prim_early.frag)
    std::array<Program *, 5> early_programs_{}, early_sample_programs_{};
    // ... and each of them reading a draw's fields from the picture's records (PCR_RECORDS): a kept walk's runs
    std::array<Program *, 5> run_programs_{}, run_sample_programs_{}, run_early_programs_{}, run_early_sample_programs_{};
    // (tolerant layers: a pass writing Z alone that discards nothing, pcr_prim_zonly.frag; a kept walk's runs')
    std::array<Program *, 5> zonly_programs_{}, run_zonly_programs_{};
    // (end_pass) a pass ended with nothing recorded since; KF4_PC_PASS_BARRIER=full: full barriers around every pass
    bool pass_ended_ = false;
    const bool full_pass_barriers_ = getenv("KF4_PC_PASS_BARRIER") && strcmp(getenv("KF4_PC_PASS_BARRIER"), "full") == 0;
    // (as_clear) KF4_PC_CLEARS=0: clears drawn as the rectangles they are; and the batches cleared
    const bool clears_on_ = !getenv("KF4_PC_CLEARS") || strcmp(getenv("KF4_PC_CLEARS"), "0") != 0;
    uint64_t counts_clears_ = 0;
    Program *date_program_ = nullptr, *date_ms_program_ = nullptr, *expand_program_ = nullptr;
    // MSAA: the samples a colour surface's pixel has when drawn into (1: none) and from the next vblank (set_msaa), and
    // whether cut-outs are shaded at every sample (only where the device shades a sample at a time: sampleRateShading)
    uint32_t samples_ = 1, pending_samples_ = 1;
    bool cutout_aa_ = false;
    const bool sample_shading_ = device_.get_device_features().enabled_features.sampleRateShading != VK_FALSE;
    // Exact Z (pcr_common.h): the GS's whole Z at each pixel, so that a decal ties with the surface under it as on the GS
    // and the later draw wins. Needs depth clamp (the corners' bound past 1), which every desktop GPU has. Without it, or
    // with KF4_PC_EXACT_Z=0, the GPU's interpolated depth: decals drawn over a surface cut into other triangles lose to
    // it here and there.
    const bool exact_z_ = device_.get_device_features().enabled_features.depthClamp &&
                          !(getenv("KF4_PC_EXACT_Z") && strcmp(getenv("KF4_PC_EXACT_Z"), "0") == 0);
    struct MsaaCounts {
        uint64_t resolves = 0, rect_resolves = 0, expands = 0, rect_expands = 0, cutout_batches = 0, vblanks_waited = 0;
    } counts_msaa_;
    // the depth surfaces' format, and whether DATE is by its stencil (KF4_PC_STENCIL=0: by a copy, as before)
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT;
    bool stencil_on_ = false;
    PassReads pass_reads_ = PassReads::nothing;
    BufferHandle picture_;
    BufferHandle no_clut_; // (bound as the CLUT of a batch that reads none)
    // ---- Developers' views (pcr.h's pcr_debug_*) ----
    struct Trace {
        bool on = false;
        PcrTraceSpot spot = {};
        uint32_t draw = 0;
        uint32_t probes = 0; // probe records since the last take
        // the traced pixel's surfaces as its last draw had them (the last depth surface of its draws), and its pixel
        Surface *colour = nullptr, *depth = nullptr;
        int32_t x = 0, y = 0;
        float zscale = 1.0f / 16777216.0f;
    } trace_;
    BufferHandle trace_buf_;
    Program *probe_program_ = nullptr, *probe_ms_program_ = nullptr;
    bool decal_losses_ = false;
    const Between *between_ = nullptr;
    ImageHandle dummy_uint_, dummy_float_;
    CommandBufferHandle cmd_;
    Surface *pass_colour_ = nullptr, *pass_depth_ = nullptr;
    std::array<uint32_t, 2> scale_ = {1, 1}, pending_scale_ = {1, 1};
    int lod_mode_ = GSN_LOD_SCALE; // (set_sampling)
    int mode_2d_ = GSN_2D_PS2;     // (set_2d)
    bool layers_ = false;          // (set_layer_z) tolerant layers
    int colour_mode_ = GSN_COLOUR_EXACT, pending_colour_ = GSN_COLOUR_EXACT; // (set_colour, from the next vblank)
    bool colour_refused_ = false;
    int high_ok_ = -1; // (high_colour_ok's answer, asked once)
    std::array<BufferHandle, 12> last_native_; // (bind_between) each native program's last arena, records, triangles
    bool set0_fresh_ = false; // (bind_between) set 0's native bindings made since the command buffer's last render pass
    Program *narrow_program_ = nullptr; // (colour high's scanout: pcr_narrow.frag)
    uint32_t aniso_ = 1;
    std::vector<std::unique_ptr<Surface>> colour_, depth_;
    TextureStore store_{store_limits()};
    std::array<WarmImages, 9> warm_images_; // (by samples: make_warm_images')
    std::unordered_map<uint64_t, GpuTexture> textures_;
    std::unordered_map<uint64_t, SampledTexture> sampled_;
    // (by sampler_for's key; never let go: a kept walk's batches name them, drawn again on the pump's thread too)
    std::unordered_map<uint32_t, SamplerHandle> samplers_;
    // KF4_PC_SAMPLER=0: every texture by the GS's read (to compare with)
    const bool sampler_on_ = !getenv("KF4_PC_SAMPLER") || strcmp(getenv("KF4_PC_SAMPLER"), "0") != 0;
    // KF4_PC_SHADER_BLEND=0: a blend with no Vulkan form drawn as the nearest Vulkan blend, as before (counted)
    const bool shader_blend_on_ = !getenv("KF4_PC_SHADER_BLEND") || strcmp(getenv("KF4_PC_SHADER_BLEND"), "0") != 0;
    uint64_t clut_serial_ = 0, frame_ = 0;
    Batch batch_;
    bool batch_open_ = false;
    CanvasCounts counts_;
    std::vector<uint8_t> fifo_; // (the last read-back's bytes, read_fifo's)
    // the pictures between: the uploads since the vblank and the last vblank's, a picture between under way, the
    // frame shown kept (0xF2) and whether draws read it now
    std::vector<RecordedUpload> uploads_now_, uploads_frame_;
    bool replaying_ = false, kept_reads_ = false;
    // (step 5b) the walk being kept, and whether it read back from the GS's memory or forgot it
    std::unique_ptr<Kept> kept_;
    bool store_follows_draws_ = true; // (false on the pump's canvas: start_from)
    const Buffer *walk_vertices_ = nullptr; // (the kept walk's vertices while one is drawn again: draw_again)
    const Buffer *walk_cluts_ = nullptr;    // (... and its CLUTs)
    const Kept *walk_table_ = nullptr;      // (... and the walk, when it has a table: its table and samplers)
    Runs runs_;                             // (... and its picture's runs)
    // KF4_PC_INDIRECT=0: a kept walk's batches a draw each, as before (to compare); off where the device has no
    // multi-draw indirect or draw parameters; a native batch's in runs only where an indirect draw has a first instance
    const bool runs_asked_ = !getenv("KF4_PC_INDIRECT") || strcmp(getenv("KF4_PC_INDIRECT"), "0") != 0;
    // KF4_PC_INDIRECT=plain: runs without the dual-source blend (a blend change ends a run: to compare)
    const bool runs_plain_ = getenv("KF4_PC_INDIRECT") && strcmp(getenv("KF4_PC_INDIRECT"), "plain") == 0;
    bool runs_ok_ = false, native_runs_ok_ = false, dual_ok_ = false;
    uint32_t max_run_draws_ = 1;
    // (KF4_PC_INDIRECT=unblended: an unblended batch in a dual-source run too, blended with one and zero: it then reads
    // its target, which costs the GPU more than the runs it joins save)
    const bool dual_unblended_ = getenv("KF4_PC_INDIRECT") && strcmp(getenv("KF4_PC_INDIRECT"), "unblended") == 0;
    Table no_table_;                        // (bound outside a kept walk: one dummy)
    bool table_on_ = false;                 // (no table where the device has none: set 1's textures, as before)
    VkDescriptorSet table_bound_ = VK_NULL_HANDLE; // (bind_table: set 2 as bound in the command buffer)
    std::array<const Sampler *, walk_samplers> no_samplers_{};
    std::array<const Sampler *, walk_samplers> samplers_bound_{}; // (bind_between: u_samplers as bound, since a render
    bool samplers_fresh_ = false;                                 // pass)
    // KF4_PC_BINDLESS=0: a kept walk's CLUTs copied to the GPU at each draw, as before (to compare)
    const bool bindless_on_ = !getenv("KF4_PC_BINDLESS") || strcmp(getenv("KF4_PC_BINDLESS"), "0") != 0;
    // KF4_PC_WALK_VERTICES=0: a kept walk's vertices copied to the GPU at each draw, as before (to compare)
    const bool walk_vertices_on_ = !getenv("KF4_PC_WALK_VERTICES") || strcmp(getenv("KF4_PC_WALK_VERTICES"), "0") != 0;
    KeptSizes kept_sizes_;
    bool keeping_ = false, kept_spoilt_ = false;
    ImageHandle kept_frame_;
    uint32_t kept_page_ = 0;
    size_t fifo_at_ = 0;
    uint64_t counts_unsupported_shape_ = 0, counts_unsupported_upload_ = 0, counts_untextured_ = 0,
             counts_depth_aliases_cleared_ = 0, counts_clut_not_uploads_ = 0, counts_date_by_stencil_ = 0, counts_date_alpha_unknown_ = 0,
             counts_blends_by_shader_ = 0, counts_native_shader_blends_ = 0;

public:
    void print(FILE *out) const {
        TextureStoreCounts t = store_.counts();
        const CanvasCounts &c = counts_;
        fprintf(out,
                "[stats] pc canvas: %llu primitives (%llu triangles) in %llu batches, %llu passes; %llu native batches in %llu "
                "draw calls; textures %llu "
                "uploaded, %llu from surfaces, %llu missing (%llu drawn untextured); surfaces %llu made, %llu filled "
                "from uploads; %llu uploads into surfaces, %llu copies between them; %llu Z buffers cleared by another "
                "width's clear; %llu clears drawn as rectangles cleared\n",
                (unsigned long long)c.primitives, (unsigned long long)c.triangles, (unsigned long long)c.batches,
                (unsigned long long)c.passes, (unsigned long long)c.native_batches,
                (unsigned long long)c.native_draw_calls, (unsigned long long)c.textures_uploaded,
                (unsigned long long)c.textures_from_surfaces, (unsigned long long)c.textures_missing,
                (unsigned long long)counts_untextured_, (unsigned long long)c.surfaces_made,
                (unsigned long long)c.surfaces_filled_from_uploads, (unsigned long long)c.uploads_into_surfaces,
                (unsigned long long)c.copies_between_surfaces, (unsigned long long)counts_depth_aliases_cleared_,
                (unsigned long long)counts_clears_);
        fprintf(out,
                "[stats] pc canvas textures: %llu batches read by the GPU's sampler (%llu mipmapped, %llu anisotropic); "
                "by the GS's read: %llu (a region mode), %llu (decoded in part); kept walks' textured batches: %llu, %llu "
                "of them from the walk's table, %llu with the walk's CLUT\n",
                (unsigned long long)c.textures_sampled, (unsigned long long)c.textures_mipmapped,
                (unsigned long long)c.textures_anisotropic, (unsigned long long)c.exact_textures_region,
                (unsigned long long)c.exact_textures_part, (unsigned long long)c.walk_texture_draws,
                (unsigned long long)c.table_draws, (unsigned long long)c.walk_clut_draws);
        fprintf(out,
                "[stats] pc canvas, not done yet: %llu blends (%llu batches blended by the shader, %llu of them native: their "
                "triangles may overlap), (%llu DATE draws done, %llu by the stencil, %llu writing an "
                "alpha the stencil does not know), %llu textures on a surface, %llu copies, %llu uploads into a surface, "
                "%llu points and lines, %llu CLUT loads not from uploads\n",
                (unsigned long long)c.unsupported_blends, (unsigned long long)counts_blends_by_shader_,
                (unsigned long long)counts_native_shader_blends_, (unsigned long long)c.date_draws,
                (unsigned long long)counts_date_by_stencil_, (unsigned long long)counts_date_alpha_unknown_,
                (unsigned long long)c.unsupported_texture_on_surface, (unsigned long long)c.unsupported_copies,
                (unsigned long long)counts_unsupported_upload_, (unsigned long long)counts_unsupported_shape_,
                (unsigned long long)counts_clut_not_uploads_);
        if (samples_ > 1 || counts_msaa_.resolves || counts_msaa_.cutout_batches) {
            fprintf(out,
                    "[stats] pc canvas MSAA: %u samples, cut-out AA %s; %llu batches shaded at every sample; %llu "
                    "surfaces resolved whole, %llu rectangles resolved (copies, textures, reads); %llu surfaces "
                    "expanded into their samples whole, %llu rectangles (uploads, copies into them); %llu vblanks drawn at the old "
                    "count while the pipelines were made\n",
                    samples_, cutout_aa_ ? "on" : "off", (unsigned long long)counts_msaa_.cutout_batches,
                    (unsigned long long)counts_msaa_.resolves, (unsigned long long)counts_msaa_.rect_resolves,
                    (unsigned long long)counts_msaa_.expands, (unsigned long long)counts_msaa_.rect_expands,
                    (unsigned long long)counts_msaa_.vblanks_waited);
        }
        warm_print_counts();
        fprintf(out,
                "[stats] pc texture store: %llu uploads (%.1f MB, %llu refused), %llu chunks kept; %llu resolves, %llu "
                "decodes, %llu found again, %llu answered kept; %llu CLUT loads (%llu decodes); %llu copies made as uploads; read-backs %llu "
                "from uploads, %llu from surfaces, %llu not had; pictures between: %llu uploads made again (%llu "
                "missed, %llu from their chunk), %llu reads of the frame kept\n",
                (unsigned long long)t.uploads, double(t.upload_bytes) / 1048576.0, (unsigned long long)t.uploads_refused,
                (unsigned long long)t.chunks_kept, (unsigned long long)t.resolves, (unsigned long long)t.decodes,
                (unsigned long long)t.decode_hits, (unsigned long long)t.resolves_kept, (unsigned long long)t.clut_loads, (unsigned long long)t.clut_decodes,
                (unsigned long long)t.copies_as_uploads, (unsigned long long)c.read_backs_from_uploads,
                (unsigned long long)c.read_backs_from_surfaces, (unsigned long long)c.unsupported_read_backs,
                (unsigned long long)c.uploads_made_again, (unsigned long long)c.uploads_again_missed,
                (unsigned long long)t.uploads_again, (unsigned long long)c.kept_frame_reads);
    }
};

Canvas::Canvas(Device &device, GpuTimes &times) : impl_(std::make_unique<Impl>(device, times)) {}
Canvas::~Canvas() = default;

void Canvas::set_msaa(uint32_t samples, bool cutout_aa) {
    impl_->set_msaa(samples, cutout_aa);
}
void Canvas::set_scale(uint32_t x_scale, uint32_t y_scale) {
    impl_->set_scale(x_scale, y_scale);
}
void Canvas::set_colour(int mode) {
    impl_->set_colour(mode);
}
void Canvas::set_2d(int mode) {
    impl_->set_2d(mode);
}
void Canvas::set_layer_z(int mode) {
    impl_->set_layer_z(mode);
}
void Canvas::set_sampling(int lod, uint32_t aniso) {
    impl_->set_sampling(lod, aniso);
}
void Canvas::set_between(const Between *between) {
    impl_->set_between(between);
}

// (Canvas::Impl's Kept, under the name the header gives it)
class KeptWalk : public Canvas::Impl::Kept {
public:
    explicit KeptWalk(Canvas::Impl::Kept &&kept) : Canvas::Impl::Kept(std::move(kept)) {}
};

// (Canvas::Impl's Copies, under the name the header gives it)
class SurfaceCopies : public Canvas::Impl::Copies {
public:
    explicit SurfaceCopies(Canvas::Impl::Copies &&copies) : Canvas::Impl::Copies(std::move(copies)) {}
};

std::shared_ptr<SurfaceCopies> Canvas::copy_surfaces() {
    return std::make_shared<SurfaceCopies>(std::move(*impl_->copy_surfaces()));
}
void Canvas::start_from(SurfaceCopies &copies) {
    impl_->start_from(copies);
}
bool submit_together() {
    static const bool on = [] {
        const char *v = getenv("KF4_PC_SUBMIT_TOGETHER");
        return !(v && strcmp(v, "0") == 0);
    }();
    return on;
}
void Canvas::draw_again_alone(const KeptWalk &walk, const Between &between, bool picture_follows) {
    impl_->draw_again_alone(walk, between, picture_follows);
}

void Canvas::keep_walk() {
    impl_->keep_walk();
}
std::shared_ptr<const KeptWalk> Canvas::take_kept_walk() {
    std::unique_ptr<Impl::Kept> kept = impl_->take_kept_walk();
    if (!kept) {
        return nullptr;
    }
    impl_->put_vertices_on_gpu(*kept);
    impl_->put_cluts_on_gpu(*kept);
    impl_->put_table_on_gpu(*kept);
    return std::make_shared<const KeptWalk>(std::move(*kept));
}
bool Canvas::fits(const KeptWalk &walk) const {
    return impl_->fits(walk);
}
void Canvas::draw_again(const KeptWalk &walk, const Between &between) {
    impl_->draw_again(walk, between);
}
void Canvas::stop_keeping() {
    impl_->stop_keeping();
}
Canvas::PictureWalk Canvas::as_picture_walk(const KeptWalk &frame, const FrameTargets &targets) const {
    Impl::PictureKept kept = impl_->as_picture_walk(frame, targets);
    if (!kept.walk) {
        return {nullptr, kept.refused};
    }
    impl_->put_vertices_on_gpu(*kept.walk);
    impl_->put_cluts_on_gpu(*kept.walk);
    impl_->put_table_on_gpu(*kept.walk);
    return {std::make_shared<const KeptWalk>(std::move(*kept.walk)), FrameWalkRefused::none};
}
void Canvas::primitive(const GsRegisterFile &file, const GifPrimitive &primitive) {
    impl_->primitive(file, primitive);
}
void Canvas::texture_written(uint64_t tex0, uint64_t texclut) {
    impl_->texture_written(tex0, texclut);
}
void Canvas::upload(const TransferRegisters &transfer, std::span<const uint8_t> bytes) {
    impl_->upload(transfer, bytes);
}
void Canvas::local_copy(const TransferRegisters &transfer) {
    impl_->local_copy(transfer);
}
void Canvas::local_to_host(const TransferRegisters &transfer) {
    impl_->local_to_host(transfer);
}
void Canvas::read_fifo(void *dst, uint32_t qwords) {
    impl_->read_fifo(dst, qwords);
}
void Canvas::forget() {
    impl_->forget();
}
void Canvas::native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &batch) {
    impl_->native_batch(file, prim, batch);
}
void Canvas::frame_mark(uint64_t value) {
    impl_->frame_mark(value);
}
bool Canvas::trace_pixel(const PcrTraceSpot &spot) {
    return impl_->trace_pixel(spot);
}
void Canvas::trace_draw(uint32_t draw) {
    impl_->trace_draw(draw);
}
void Canvas::trace_probe() {
    impl_->trace_probe();
}
size_t Canvas::take_trace(std::vector<PcrTraceFragment> &fragments, std::vector<PcrTraceProbe> &probes) {
    return impl_->take_trace(fragments, probes);
}
void Canvas::show_decal_losses(bool on) {
    impl_->show_decal_losses(on);
}
bool Canvas::settled() const {
    return impl_->settled();
}
void Canvas::upload_again(const GsRegisterFile &file, uint64_t value) {
    impl_->upload_again(file, value);
}
std::vector<uint8_t> Canvas::vram_image() {
    return impl_->vram_image();
}
TextureStore::ClutState Canvas::clut_state() const {
    return impl_->clut_state();
}
void Canvas::restore(std::span<const uint8_t> vram, const TextureStore::ClutState &clut) {
    impl_->restore(vram, clut);
}
bool Canvas::scanout(CommandBuffer &cmd, const ShownRect &rect, const Image &dst) {
    return impl_->scanout(cmd, rect, dst);
}
void Canvas::submit() {
    impl_->submit();
}
void Canvas::vblank() {
    impl_->vblank();
}
CanvasCounts Canvas::counts() const {
    return impl_->counts();
}
void Canvas::print_counts() const {
    impl_->print(stderr);
}

} // namespace pcr
