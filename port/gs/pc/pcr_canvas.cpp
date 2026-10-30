// The PC renderer's drawing without the GS's VRAM (pcr_canvas.h). The GS's registers at a primitive pick its
// surface, its texture and its pixel arithmetic; primitives of the same state are one batch, drawn when the state
// changes or something else needs the GPU (an upload, a copy, a picture).
#include "pcr_canvas.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <list>
#include <unordered_map>
#include <vector>

#include "pcr_shaders.h"
#include "pcr_textures.h"

extern "C" {
#include "gsn_vram.h"
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
};
static_assert(sizeof(PrimPush) <= 128, "push constants: 128 bytes at most");

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
};
enum : uint32_t { SRC_CT32 = 0, SRC_CT24 = 1, SRC_CT16 = 2, SRC_INDEX = 3, SRC_SURFACE = 4 };

struct Vertex {
    float x, y;
    uint32_t z;
    uint8_t rgba[4];
    float s, t, q;
    float fog;
};
static_assert(sizeof(Vertex) == 32, "Vertex: the vertex shader's inputs, packed");

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
    ImageHandle image;   // at the canvas's scale
    bool depth = false;

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

// What a batch draws with: the registers its primitives read, whole
struct DrawState {
    uint64_t frame, zbuf, test, alpha, tex0, tex1, clamp, texa, fogcol, scissor, xyoffset, fba, colclamp, pabe, dthe;
    uint64_t miptbp1, miptbp2;
    uint32_t prim; // (IIP, TME, FGE, ABE, AA1, FST, CTXT: the bits 3-10)
    uint64_t clut_serial;

    bool operator==(const DrawState &) const = default;
};

} // namespace

class Canvas::Impl {
public:
    explicit Impl(Device &device) : device_(device) {
        make_programs();
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UINT);
        uint32_t zero = 0;
        ImageInitialData data = {&zero, 0, 0};
        dummy_uint_ = device_.create_image(info, &data);
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        dummy_float_ = device_.create_image(info, &data);
    }

    ~Impl() {
        end_pass();
        if (cmd_) {
            device_.submit(cmd_);
        }
    }

    void set_scale(uint32_t x, uint32_t y) {
        pending_scale_ = {std::clamp(x, 1u, 8u), std::clamp(y, 1u, 8u)};
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
        if (!batch_open_ || batch_.native || !(s == batch_.state) || (tme && !texture_covers(p))) {
            flush_batch();
            open_batch(s, p);
        }
        add_primitive(p);
    }

    void native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &nb) {
        counts_.native_batches++;
        counts_.triangles += nb.triangle_count;
        DrawState s = state_of(file, prim);
        bool same = batch_open_ && batch_.native && s == batch_.state && batch_.arena == nb.arena &&
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
            batch_.runs.push_back({nb.first_triangle, nb.triangle_count, nb.instance});
        }
    }

    void texture_written(uint64_t tex0, uint64_t texclut) {
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
        if (!replaying_) {
            // (the frame's uploads: what its pictures between ask for again)
            uploads_now_.push_back({t, std::make_shared<std::vector<uint8_t>>(bytes.begin(), bytes.end()),
                                    taken ? store_.last_chunk() : 0});
        }
    }

    void frame_mark(uint64_t value) {
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
        const RecordedUpload &r = uploads_frame_[k];
        flush_batch();
        if (r.chunk == 0 || !store_.upload_again(r.chunk)) {
            store_.upload({r.transfer.bitbltbuf, r.transfer.trxpos, r.transfer.trxreg}, *r.bytes);
        }
        write_upload_into_surfaces(r.transfer, *r.bytes);
        counts_.uploads_made_again++;
    }

    void local_copy(const TransferRegisters &t) {
        flush_batch();
        TransferRegs regs = {t.bitbltbuf, t.trxpos, t.trxreg};
        copy_between_surfaces(t);
        store_.copied(regs);
    }

    void local_to_host(const TransferRegisters &t) {
        flush_batch();
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
        end_pass();
        uploads_now_.clear();
        uploads_frame_.clear();
        kept_frame_.reset();
        store_.forget();
        colour_.clear();
        depth_.clear();
        textures_.clear();
        clut_serial_++;
    }

    void vblank() {
        flush_batch();
        if (!uploads_now_.empty()) {
            uploads_frame_.swap(uploads_now_);
            uploads_now_.clear();
        }
        submit();
        // (Granite lets go of a frame context's allocations, images and buffers only when frames move on: each
        // vblank, pictures or none, or a run that shows nothing holds all it ever drew)
        device_.next_frame_context();
        apply_scale();
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
        if (rect.w == 0 || rect.h == 0 || rect.psm != GSN_PSMCT32) {
            submit();
            return false;
        }
        Placed placed = colour_surface(rect.bp / 32, rect.bw, GSN_PSMCT32, rect.y + rect.h);
        submit();
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

    void submit() {
        end_pass();
        if (cmd_) {
            device_.submit(cmd_);
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
        if (!cmd_) {
            cmd_ = device_.request_command_buffer();
        }
        return *cmd_;
    }

    void full_barrier(CommandBuffer &c) {
        c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    void end_pass() {
        if (pass_colour_) {
            cmd().end_render_pass();
            full_barrier(cmd());
            pass_colour_ = nullptr;
            pass_depth_ = nullptr;
        }
    }

    // The pass drawing into colour (and depth): the open one when it is that, else a new one
    void begin_pass(Surface &colour, Surface *depth) {
        if (pass_colour_ == &colour && (depth == nullptr || pass_depth_ == depth)) {
            return;
        }
        end_pass();
        RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &colour.image->get_view();
        rp.load_attachments = 1;
        rp.store_attachments = 1;
        if (depth) {
            rp.depth_stencil = &depth->image->get_view();
            rp.op_flags = RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT | RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
        }
        CommandBuffer &c = cmd();
        full_barrier(c);
        c.begin_render_pass(rp);
        pass_colour_ = &colour;
        pass_depth_ = depth;
        counts_.passes++;
    }

    // ---- Surfaces ----
    ImageHandle make_surface_image(uint32_t width, uint32_t height, bool depth) {
        ImageCreateInfo info = ImageCreateInfo::render_target(width * scale_[0], height * scale_[1],
                                                              depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                            : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageHandle image = device_.create_image(info, nullptr);
        if (!image) {
            fprintf(stderr, "[pcr] no image for a %ux%u surface\n", width, height);
        }
        return image;
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
                    grow(*s, needed);
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
        CommandBuffer &c = cmd();
        VkClearValue zero = {};
        c.clear_image(*made->image, zero);
        full_barrier(c);
        fill_from_uploads(*made);
        const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        for (auto [o, y] : taken) {
            c.copy_image(*made->image, *o->image, {0, int32_t(y * scale_[1]), 0}, {0, 0, 0},
                         {o->image->get_width(), o->image->get_height(), 1}, colour, colour);
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
        cmd().clear_image(*made->image, zero, VK_IMAGE_ASPECT_DEPTH_BIT);
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

    // A surface made taller: a new image, the old one's pixels copied in
    void grow(Surface &s, uint32_t height) {
        if (pass_colour_ == &s || pass_depth_ == &s) {
            end_pass();
        }
        ImageHandle bigger = make_surface_image(s.width(), height, s.depth);
        if (!bigger) {
            return;
        }
        CommandBuffer &c = cmd();
        VkClearValue zero = {};
        if (s.depth) {
            c.clear_image(*bigger, zero, VK_IMAGE_ASPECT_DEPTH_BIT);
        } else {
            c.clear_image(*bigger, zero);
        }
        full_barrier(c);
        VkImageSubresourceLayers layers = {VkImageAspectFlags(s.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0,
                                           0, 1};
        c.copy_image(*bigger, *s.image, {0, 0, 0}, {0, 0, 0}, {s.image->get_width(), s.image->get_height(), 1}, layers,
                     layers);
        full_barrier(c);
        s.image = bigger;
        s.height = height;
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

    // GS pixels (RGBA8, w x h) into a colour surface at (x, y), each repeated at the canvas's scale
    void write_pixels(Surface &s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels) {
        if (pass_colour_ == &s) {
            end_pass();
        }
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(w, h, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        ImageInitialData data = {pixels, 0, 0};
        ImageHandle staged = device_.create_image(info, &data);
        if (!staged) {
            return;
        }
        CommandBuffer &c = cmd();
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
        for (auto &s : colour_) {
            if (s->fbw == sbw && sbp % 32 == 0 && s->page_place(sbp / 32, fx, fy)) {
                from = s.get();
                break;
            }
        }
        if (!from || w == 0 || h == 0) {
            return; // (uploads': the texture store's)
        }
        if (spsm != GSN_PSMCT32 || dpsm != GSN_PSMCT32 || dbp % 32 != 0) {
            counts_.unsupported_copies++;
            return;
        }
        Placed placed = colour_surface(dbp / 32, dbw, GSN_PSMCT32, dy + h);
        Surface *to = placed.surface;
        if (!to) {
            return;
        }
        uint32_t tx = placed.x, ty = placed.y;
        end_pass();
        CommandBuffer &c = cmd();
        full_barrier(c);
        const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        uint32_t cw = std::min({w, from->width() - std::min(fx + sx, from->width()), to->width() - std::min(tx + dx, to->width())});
        uint32_t ch = std::min({h, from->height - std::min(fy + sy, from->height), to->height - std::min(ty + dy, to->height)});
        if (cw == 0 || ch == 0) {
            return;
        }
        if (from == to) {
            counts_.unsupported_copies++; // (within one surface: overlapping, as the GS orders it; not made)
            return;
        }
        c.copy_image(*to->image, *from->image, {int32_t((tx + dx) * scale_[0]), int32_t((ty + dy) * scale_[1]), 0},
                     {int32_t((fx + sx) * scale_[0]), int32_t((fy + sy) * scale_[1]), 0},
                     {cw * scale_[0], ch * scale_[1], 1}, colour, colour);
        full_barrier(c);
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
        end_pass();
        CommandBuffer &c = cmd();
        full_barrier(c);
        int32_t x0 = int32_t(x * scale_[0]), y0 = int32_t(y * scale_[1]);
        c.blit_image(*one, *from.image, {0, 0, 0}, {int32_t(w), int32_t(h), 1}, {x0, y0, 0},
                     {int32_t(w * scale_[0]), int32_t(h * scale_[1]), 1}, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
        full_barrier(c);
        c.copy_image_to_buffer(*buffer, *one, 0, {0, 0, 0}, {w, h, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        c.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_HOST_READ_BIT);
        Fence fence;
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
        const ImageView *view = nullptr; // (u_texels, or u_surface for SRC_SURFACE)
        uint32_t src = SRC_CT32;
        uint32_t levels = 1;
        uint32_t cols = 0, rows = 0; // the part of level 0 the batch may read
        ImageHandle surface_copy;     // (a surface's copy: kept until the batch is drawn)
        bool ok = false;
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
            GpuTexture *t = gpu_texture(a);
            if (!t) {
                return b;
            }
            b.view = &t->image->get_view();
            b.levels = t->levels;
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
        if (!from || (psm != GSN_PSMCT32 && psm != GSN_PSMCT24)) {
            counts_.unsupported_texture_on_surface++;
            return b;
        }
        uint32_t w = std::min(cols, from->width() - std::min(ox, from->width()));
        uint32_t h = std::min(rows, from->height - std::min(oy, from->height));
        if (w == 0 || h == 0) {
            counts_.unsupported_texture_on_surface++;
            return b;
        }
        ImageCreateInfo info = ImageCreateInfo::render_target(cols * scale_[0], rows * scale_[1], VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        b.surface_copy = device_.create_image(info, nullptr);
        if (!b.surface_copy) {
            return b;
        }
        end_pass();
        CommandBuffer &c = cmd();
        full_barrier(c);
        VkClearValue zero = {};
        c.clear_image(*b.surface_copy, zero);
        full_barrier(c);
        const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        // (between 0xF4 and 0xF5 of a picture between, the frame shown kept at 0xF2 in place of its buffer)
        bool kept = replaying_ && kept_reads_ && kept_frame_ && from->base_page == kept_page_ &&
                    kept_frame_->get_width() == from->image->get_width() &&
                    kept_frame_->get_height() == from->image->get_height();
        counts_.kept_frame_reads += kept ? 1 : 0;
        c.copy_image(*b.surface_copy, kept ? *kept_frame_ : *from->image, {0, 0, 0},
                     {int32_t(ox * scale_[0]), int32_t(oy * scale_[1]), 0},
                     {w * scale_[0], h * scale_[1], 1}, colour, colour);
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
                kept_frame_ = copy_of(*s->image);
                kept_page_ = base_page;
                return;
            }
        }
    }

    // An image's copy, for a shader to read while the image is drawn into
    ImageHandle copy_of(const Image &image) {
        ImageCreateInfo info = ImageCreateInfo::render_target(image.get_width(), image.get_height(), image.get_format());
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageHandle copy = device_.create_image(info, nullptr);
        if (!copy) {
            return copy;
        }
        end_pass();
        CommandBuffer &c = cmd();
        full_barrier(c);
        const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.copy_image(*copy, image, {0, 0, 0}, {0, 0, 0}, {image.get_width(), image.get_height(), 1}, colour, colour);
        full_barrier(c);
        return copy;
    }

    void evict_textures() {
        if (textures_.size() < 4096) {
            return;
        }
        for (auto it = textures_.begin(); it != textures_.end();) {
            it = frame_ - it->second.used > 120 ? textures_.erase(it) : std::next(it);
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
        };
        bool native = false;
        BufferHandle arena, records, triangles;
        std::vector<Run> runs;
    };

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
        batch_.arena.reset();
        batch_.records.reset();
        batch_.triangles.reset();
        batch_.runs.clear();
        if (bits(s.prim, 4, 1)) {
            batch_.texture = bind_texture(s, cols, rows);
            if (batch_.texture.src == SRC_INDEX) {
                ClutColours colours = store_.clut_colours(s.tex0, s.texa);
                batch_.clut = colours.rgba;
            }
        }
        batch_open_ = true;
    }

    void add_primitive(const GifPrimitive &p) {
        uint64_t xy = batch_.state.xyoffset;
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
        if (p.shape == PrimitiveShape::triangle) {
            const GifVertex &last = p.vertices[2];
            for (uint32_t i = 0; i < 3; i++) {
                batch_.vertices.push_back(make(p.vertices[i], last));
                touch(batch_.vertices.back());
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
        Vertex c = va, d = va;
        c.x = vb.x;
        c.s = vb.s; // (x1, y0)
        d.y = vb.y;
        d.t = vb.t; // (x0, y1)
        batch_.vertices.insert(batch_.vertices.end(), {va, c, d, c, vb, d});
        touch(va);
        touch(vb);
        counts_.triangles += 2;
    }

    void flush_batch() {
        if (!batch_open_) {
            return;
        }
        batch_open_ = false;
        if (!batch_.vertices.empty() || !batch_.runs.empty()) {
            draw_batch();
        }
        batch_.texture = BoundTexture(); // (a surface's copy let go once drawn)
        batch_.arena.reset();
        batch_.records.reset();
        batch_.triangles.reset();
    }

    void draw_batch() {
        const DrawState &s = batch_.state;
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
        };
        std::array<Pass, 2> passes{};
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
        // (a surface as tall as what is drawn in it, not its scissor: the game's scissors run past small targets, onto
        // the targets below them)
        uint32_t height = uint32_t(std::clamp(std::min(batch_.y1, int32_t(scay1)) + 1, 1, 2048));
        Placed placed = colour_surface(fbp, fbw, fpsm == GSN_PSMCT24 ? GSN_PSMCT32 : fpsm, height);
        Surface *colour = placed.surface;
        if (!colour) {
            return;
        }
        if (!batch_.native && (placed.x != 0 || placed.y != 0)) {
            // (a frame inside a surface: its pixels at their place there)
            for (Vertex &v : batch_.vertices) {
                v.x += float(placed.x);
                v.y += float(placed.y);
            }
        }
        Surface *depth = nullptr;
        if (zte) {
            depth = depth_surface(bits(s.zbuf, 0, 9), fbw, z_psm(s.zbuf), height);
            if (depth && (depth->image->get_width() != colour->image->get_width() ||
                          depth->image->get_height() != colour->image->get_height())) {
                // (the Vulkan pass wants them alike: the taller made taller still)
                uint32_t h = std::max(colour->height, depth->height);
                if (colour->height < h) {
                    grow(*colour, h);
                }
                if (depth->height < h) {
                    grow(*depth, h);
                }
            }
        }
        zte = zte && depth != nullptr;
        zwrite = zwrite && zte;
        // DATE: the destination as it is before the draw, which the shader reads (a copy: the draw writes it)
        ImageHandle dest_copy;
        bool date = bits(s.test, 14, 1) != 0;
        if (date) {
            dest_copy = copy_of(*colour->image);
            counts_.date_draws++;
        }
        begin_pass(*colour, depth);
        CommandBuffer &c = cmd();
        c.set_program(batch_.native ? native_program_ : program_);
        c.set_opaque_state();
        c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        c.set_cull_mode(VK_CULL_MODE_NONE);
        VkViewport vp = {0, 0, float(colour->image->get_width()), float(colour->image->get_height()), 0, 1};
        c.set_viewport(vp);
        int32_t sx0 = int32_t((scax0 + placed.x) * scale_[0]), sy0 = int32_t((scay0 + placed.y) * scale_[1]);
        int32_t sx1 = std::min(int32_t((scax1 + 1 + placed.x) * scale_[0]), int32_t(colour->image->get_width()));
        int32_t sy1 = std::min(int32_t((scay1 + 1 + placed.y) * scale_[1]), int32_t(colour->image->get_height()));
        if (sx1 <= sx0 || sy1 <= sy0) {
            return;
        }
        c.set_scissor({{sx0, sy0}, {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0)}});
        c.set_depth_compare(ztst == 0 ? VK_COMPARE_OP_NEVER
                            : ztst == 1 ? VK_COMPARE_OP_ALWAYS
                            : ztst == 2 ? VK_COMPARE_OP_GREATER_OR_EQUAL
                                        : VK_COMPARE_OP_GREATER);
        BlendChoice blend = choose_blend(s.alpha, bits(s.prim, 6, 1) != 0);
        if (!blend.supported) {
            counts_.unsupported_blends++;
        }
        PrimPush push = {};
        push.inv_size[0] = 1.0f / float(colour->width());
        push.inv_size[1] = 1.0f / float(colour->height);
        uint32_t zpsm = z_psm(s.zbuf);
        push.zmax = zpsm == 0x30 ? 0xFFFFFFFFu : zpsm == 0x31 ? 0xFFFFFFu : 0xFFFFu;
        push.zscale = zpsm == 0x30 ? 1.0f / 4294967296.0f : 1.0f / 16777216.0f;
        bool tme = bits(s.prim, 4, 1) != 0;
        uint32_t flags = 0;
        if (tme && batch_.texture.ok) {
            flags |= F_TME | (bits(s.prim, 8, 1) ? F_FST : 0) | bits(s.tex0, 35, 2) << F_TFX_SHIFT |
                     (bits(s.tex0, 34, 1) ? F_TCC : 0) | batch_.texture.src << F_SRC_SHIFT;
        } else if (tme) {
            counts_untextured_++;
        }
        flags |= bits(s.prim, 5, 1) ? F_FGE : 0;
        flags |= bits(s.fba, 0, 1) ? F_FBA : 0;
        flags |= blend.flags & ~0x80000000u;
        flags |= date ? F_DATE | (bits(s.test, 15, 1) ? F_DATM : 0) : 0;
        push.flags = flags;
        push.tex_size = (1u << std::min(bits(s.tex0, 26, 4), 10u)) | (1u << std::min(bits(s.tex0, 30, 4), 10u)) << 16;
        push.clamp_lo = uint32_t(s.clamp & 0xFFFFFFu);
        uint32_t fix = (blend.flags & 0x80000000u) ? 0 : bits(s.alpha, 32, 8);
        push.clamp_hi = uint32_t((s.clamp >> 24) & 0xFFFFFu) | fix << 24;
        push.test = atst | bits(s.test, 4, 8) << 3;
        push.fogcol = uint32_t(s.fogcol & 0xFFFFFF);
        push.texa = bits(s.texa, 0, 8) | bits(s.texa, 15, 1) << 15 | bits(s.texa, 32, 8) << 16;
        push.tex1 = bits(s.tex1, 5, 1) | bits(s.tex1, 6, 3) << 1 | bits(s.tex1, 2, 3) << 4 | bits(s.tex1, 0, 1) << 7 |
                    bits(s.tex1, 19, 2) << 8;
        int32_t k = int32_t(bits(s.tex1, 32, 12) << 20) >> 20; // (signed 7.4)
        push.lod_k = float(k) / 16.0f;
        push.levels = batch_.texture.levels;
        push.src_scale[0] = int32_t(scale_[0]);
        push.src_scale[1] = int32_t(scale_[1]);
        push.place[0] = int32_t(placed.x);
        push.place[1] = int32_t(placed.y);
        // textures: the texels (or the dummy), the surface's copy (or the dummy), the CLUT
        if (batch_.texture.ok && batch_.texture.src == SRC_SURFACE) {
            c.set_texture(0, 0, dummy_uint_->get_view(), StockSampler::NearestClamp);
            c.set_texture(0, 1, *batch_.texture.view, StockSampler::NearestClamp);
        } else {
            c.set_texture(0, 0, batch_.texture.ok ? *batch_.texture.view : dummy_uint_->get_view(),
                          StockSampler::NearestClamp);
            c.set_texture(0, 1, dummy_float_->get_view(), StockSampler::NearestClamp);
        }
        c.set_texture(0, 3, dest_copy ? dest_copy->get_view() : dummy_float_->get_view(), StockSampler::NearestClamp);
        void *clut = c.allocate_constant_data(0, 2, sizeof(batch_.clut));
        memcpy(clut, batch_.clut.data(), sizeof(batch_.clut));
        float constants[4] = {float(bits(s.alpha, 32, 8)) / 128.0f, float(bits(s.alpha, 32, 8)) / 128.0f,
                              float(bits(s.alpha, 32, 8)) / 128.0f, 128.0f / 255.0f};
        c.set_blend_constants(constants);
        if (batch_.native) {
            c.set_storage_buffer(1, 0, *picture_);
            c.set_storage_buffer(2, 0, *batch_.arena);
            c.set_storage_buffer(2, 1, *batch_.records);
            c.set_storage_buffer(2, 2, *batch_.triangles);
        }
        size_t bytes = batch_.vertices.size() * sizeof(Vertex);
        for (int i = 0; i < pass_count; i++) {
            const Pass &pass = passes[i];
            push.flags = (flags & ~(3u << F_PASS_SHIFT)) | pass.mode << F_PASS_SHIFT;
            c.push_constants(&push, 0, sizeof(push));
            c.set_depth_test(zte, pass.z);
            if (blend.enable) {
                c.set_blend_enable(true);
                c.set_blend_factors(blend.src, blend.src_alpha, blend.dst, VK_BLEND_FACTOR_ZERO);
                c.set_blend_op(blend.op, VK_BLEND_OP_ADD);
            } else {
                c.set_blend_enable(false);
            }
            uint32_t mask = (pass.rgb && blend.write_rgb ? 0x7u : 0u) | (pass.alpha ? 0x8u : 0u);
            c.set_color_write_mask(mask);
            if (batch_.native) {
                for (const Batch::Run &r : batch_.runs) {
                    c.draw(r.count * 3, 1, r.first * 3, r.instance);
                    counts_.native_draw_calls++;
                }
                continue;
            }
            void *v = c.allocate_vertex_data(0, bytes, sizeof(Vertex));
            memcpy(v, batch_.vertices.data(), bytes);
            c.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x));
            c.set_vertex_attrib(1, 0, VK_FORMAT_R32_UINT, offsetof(Vertex, z));
            c.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, rgba));
            c.set_vertex_attrib(3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, s));
            c.set_vertex_attrib(4, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, fog));
            c.draw(uint32_t(batch_.vertices.size()));
        }
        // what was drawn over, for the texture store (its pages are a surface's now, not the uploads')
        int32_t x0 = std::max(batch_.x0, int32_t(scax0)), x1 = std::min(batch_.x1, int32_t(scax1));
        int32_t y0 = std::max(batch_.y0, int32_t(scay0)), y1 = std::min(batch_.y1, int32_t(scay1));
        if (x1 >= x0 && y1 >= y0) {
            store_.drawn({fbp * 32, fbw, fpsm, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1)});
            if (zte && zwrite) {
                store_.drawn({bits(s.zbuf, 0, 9) * 32, fbw, z_psm(s.zbuf), uint32_t(x0), uint32_t(y0),
                              uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1)});
            }
        }
    }

    // ---- Scale ----
    void apply_scale() {
        if (pending_scale_ == scale_) {
            return;
        }
        end_pass();
        std::array<uint32_t, 2> old = scale_;
        scale_ = pending_scale_;
        for (auto *list : {&colour_, &depth_}) {
            for (auto &s : *list) {
                ImageHandle image = make_surface_image(s->width(), s->height, s->depth);
                if (!image) {
                    continue;
                }
                CommandBuffer &c = cmd();
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

    void make_programs() {
        ResourceLayout vert = {};
        vert.input_mask = 0x1F;
        vert.push_constant_size = sizeof(PrimPush);
        ResourceLayout frag = {};
        frag.output_mask = 1;
        frag.push_constant_size = sizeof(PrimPush);
        frag.sets[0].sampled_image_mask = (1u << 0) | (1u << 1) | (1u << 3);
        frag.sets[0].fp_mask = (1u << 1) | (1u << 3);
        frag.sets[0].uniform_buffer_mask = 1u << 2;
        for (int b : {0, 1, 2, 3}) {
            frag.sets[0].meta[b].array_size = 1;
        }
        Shader *vs = device_.request_shader(pcr_spv_pcr_prim_vert, sizeof(pcr_spv_pcr_prim_vert), &vert);
        Shader *fs = device_.request_shader(pcr_spv_pcr_prim_frag, sizeof(pcr_spv_pcr_prim_frag), &frag);
        program_ = device_.request_program(vs, fs);
        ResourceLayout nd = {};
        nd.push_constant_size = sizeof(PrimPush);
        nd.sets[1].storage_buffer_mask = 1u;
        nd.sets[2].storage_buffer_mask = 7u;
        nd.sets[1].meta[0].array_size = 1;
        for (int b : {0, 1, 2}) {
            nd.sets[2].meta[b].array_size = 1;
        }
        Shader *nvs = device_.request_shader(pcr_spv_pcr_nd000_vert, sizeof(pcr_spv_pcr_nd000_vert), &nd);
        native_program_ = device_.request_program(nvs, fs);
        // (the pictures between's camera for native draws: none yet, zeros)
        BufferCreateInfo info = {};
        info.domain = BufferDomain::Device;
        info.size = 256;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        std::array<uint8_t, 256> zeros{};
        picture_ = device_.create_buffer(info, zeros.data());
    }

    Device &device_;
    Program *program_ = nullptr, *native_program_ = nullptr;
    BufferHandle picture_;
    ImageHandle dummy_uint_, dummy_float_;
    CommandBufferHandle cmd_;
    Surface *pass_colour_ = nullptr, *pass_depth_ = nullptr;
    std::array<uint32_t, 2> scale_ = {1, 1}, pending_scale_ = {1, 1};
    std::vector<std::unique_ptr<Surface>> colour_, depth_;
    TextureStore store_;
    std::unordered_map<uint64_t, GpuTexture> textures_;
    uint64_t clut_serial_ = 0, frame_ = 0;
    Batch batch_;
    bool batch_open_ = false;
    CanvasCounts counts_;
    std::vector<uint8_t> fifo_; // (the last read-back's bytes, read_fifo's)
    // the pictures between: the uploads since the vblank and the last vblank's, a picture between under way, the
    // frame shown kept (0xF2) and whether draws read it now
    struct RecordedUpload {
        TransferRegisters transfer;
        std::shared_ptr<const std::vector<uint8_t>> bytes;
        uint64_t chunk; // (the texture store's: 0 if it refused it)
    };
    std::vector<RecordedUpload> uploads_now_, uploads_frame_;
    bool replaying_ = false, kept_reads_ = false;
    ImageHandle kept_frame_;
    uint32_t kept_page_ = 0;
    size_t fifo_at_ = 0;
    uint64_t counts_unsupported_shape_ = 0, counts_unsupported_upload_ = 0, counts_untextured_ = 0,
             counts_clut_not_uploads_ = 0;

public:
    void print(FILE *out) const {
        TextureStoreCounts t = store_.counts();
        const CanvasCounts &c = counts_;
        fprintf(out,
                "[stats] pc canvas: %llu primitives (%llu triangles) in %llu batches, %llu passes; textures %llu "
                "uploaded, %llu from surfaces, %llu missing (%llu drawn untextured); surfaces %llu made, %llu filled "
                "from uploads; %llu uploads into surfaces, %llu copies between them\n",
                (unsigned long long)c.primitives, (unsigned long long)c.triangles, (unsigned long long)c.batches,
                (unsigned long long)c.passes, (unsigned long long)c.textures_uploaded,
                (unsigned long long)c.textures_from_surfaces, (unsigned long long)c.textures_missing,
                (unsigned long long)counts_untextured_, (unsigned long long)c.surfaces_made,
                (unsigned long long)c.surfaces_filled_from_uploads, (unsigned long long)c.uploads_into_surfaces,
                (unsigned long long)c.copies_between_surfaces);
        fprintf(out,
                "[stats] pc canvas, not done yet: %llu blends, (%llu DATE draws done), %llu textures on a surface, %llu "
                "copies, %llu uploads into a surface, %llu points and lines, %llu CLUT loads not from uploads\n",
                (unsigned long long)c.unsupported_blends, (unsigned long long)c.date_draws,
                (unsigned long long)c.unsupported_texture_on_surface, (unsigned long long)c.unsupported_copies,
                (unsigned long long)counts_unsupported_upload_, (unsigned long long)counts_unsupported_shape_,
                (unsigned long long)counts_clut_not_uploads_);
        fprintf(out,
                "[stats] pc texture store: %llu uploads (%.1f MB, %llu refused), %llu chunks kept; %llu resolves, %llu "
                "decodes, %llu found again; %llu CLUT loads (%llu decodes); %llu copies made as uploads; read-backs %llu "
                "from uploads, %llu from surfaces, %llu not had; pictures between: %llu uploads made again (%llu "
                "missed, %llu from their chunk), %llu reads of the frame kept\n",
                (unsigned long long)t.uploads, double(t.upload_bytes) / 1048576.0, (unsigned long long)t.uploads_refused,
                (unsigned long long)t.chunks_kept, (unsigned long long)t.resolves, (unsigned long long)t.decodes,
                (unsigned long long)t.decode_hits, (unsigned long long)t.clut_loads, (unsigned long long)t.clut_decodes,
                (unsigned long long)t.copies_as_uploads, (unsigned long long)c.read_backs_from_uploads,
                (unsigned long long)c.read_backs_from_surfaces, (unsigned long long)c.unsupported_read_backs,
                (unsigned long long)c.uploads_made_again, (unsigned long long)c.uploads_again_missed,
                (unsigned long long)t.uploads_again, (unsigned long long)c.kept_frame_reads);
    }
};

Canvas::Canvas(Device &device) : impl_(std::make_unique<Impl>(device)) {}
Canvas::~Canvas() = default;

void Canvas::set_scale(uint32_t x_scale, uint32_t y_scale) {
    impl_->set_scale(x_scale, y_scale);
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
