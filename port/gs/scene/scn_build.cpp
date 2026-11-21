// The scene renderer's builder (scn_build.h). Where it does what the PC renderer's canvas does (pcr_canvas.cpp: the
// targets' places in the GS's memory, the textures, a batch's state and its pixel arithmetic), it does it the same way,
// so that its pictures are the canvas's with KF4_PC_DEPTH=unorm (the depth the design took: D16_UNORM, the GPU's test);
// what differs is that nothing is drawn as it is met: the scene is built, then drawn whole (scn_draw.h).
#include "scn_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include "pcr_blend.h"
#include "pcr_gpu_time.h"
#include "pcr_hang_notes.h"
#include "pcr_hang_watch.h"
#include "pcr_jitter.h"
#include "pcr_prim_layout.h"
#include "pcr_textures.h"
#include "scn_draw.h"
#include "scn_scene.h"

extern "C" {
#include "gsn.h"
#include "gsn_vram.h"
#include "port/render.h"
}

using namespace Vulkan;
using namespace pcr;

namespace scn {

namespace {

uint32_t bits(uint64_t v, int at, int count) {
    return uint32_t((v >> at) & ((uint64_t(1) << count) - 1));
}

bool is_colour_16(uint32_t psm) {
    return psm == GSN_PSMCT16 || psm == GSN_PSMCT16S;
}

// A page of the format: pixels down (64 across for every colour and Z format)
uint32_t page_rows(uint32_t psm) {
    return is_colour_16(psm) || psm == 0x32 || psm == 0x3A ? 64 : 32;
}

uint32_t z_psm(uint64_t zbuf) {
    return 0x30 | bits(zbuf, 24, 4);
}

uint32_t zmax_of(uint32_t zpsm) {
    return zpsm == 0x30 ? 0xFFFFFFFFu : zpsm == 0x31 ? 0xFFFFFFu : 0xFFFFu;
}

// The depth a Z is in its Z buffer's image: Z / zmax (D16_UNORM's whole steps are the 16-bit Z buffers' whole Z)
float depth_of(uint32_t z, uint32_t zpsm) {
    uint32_t zmax = zmax_of(zpsm);
    return float(double(std::min(z, zmax)) / double(zmax));
}

uint32_t rounded_height(uint32_t height, uint32_t psm) {
    uint32_t rows = page_rows(psm);
    return std::clamp((height + rows - 1) / rows * rows, rows, 2048u);
}

// A buffer the game draws into (pcr_canvas.cpp's Surface): its pages from the base, fbw wide, as many rows of pages as
// height needs, and its image at the scene's scale
struct Target {
    uint32_t base_page = 0, fbw = 0, psm = 0;
    uint32_t height = 0; // GS pixels
    ImageHandle image;
    bool depth = false;
    uint64_t used = 0;       // (a depth target: the vblank it was last drawn with)
    bool cleared = false;    // (a depth target cleared whole to cleared_z, no Z written since: clear_depth_aliases)
    uint32_t cleared_z = 0;

    uint32_t width() const {
        return std::max(1u, fbw) * 64;
    }
    uint32_t end_page() const {
        return base_page + std::max(1u, fbw) * ((height + page_rows(psm) - 1) / page_rows(psm));
    }
    bool overlaps(uint32_t first, uint32_t end) const {
        return base_page < end && first < end_page();
    }
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

struct DrawState {
    uint64_t frame, zbuf, test, alpha, tex0, tex1, clamp, texa, fogcol, scissor, xyoffset, fba, colclamp, pabe, dthe;
    uint64_t miptbp1, miptbp2;
    uint32_t prim;
    uint64_t clut_serial;
    bool operator==(const DrawState &) const = default;
};

struct GpuTexture {
    TextureKey key;
    ImageHandle image;
    uint32_t levels = 1;
    uint64_t used = 0;
};

struct SampledTexture {
    TextureKey key;
    uint64_t texa = 0;
    std::array<uint32_t, 256> clut{};
    ImageHandle image;
    uint32_t levels = 1;
    uint64_t used = 0;
    uint8_t alpha_lo = 0, alpha_hi = 255;
    bool made_from(const TextureKey &k, uint64_t t, const std::array<uint32_t, 256> &c) const {
        return key == k && texa == t && clut == c;
    }
};

// A draw's texture as the store answered it
struct BoundTexture {
    ImageHandle image;
    const Sampler *sampler = nullptr;
    uint32_t src = SRC_CT32;
    uint32_t levels = 1;
    uint32_t cols = 0, rows = 0;
    uint32_t sampling = 0;
    bool ok = false;
    uint8_t alpha_lo = 0, alpha_hi = 255;
    std::array<uint32_t, 256> clut{}; // (SRC_INDEX)
};

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

// A batch: the GIF's primitives of one state (the canvas's batching), or one native batch
struct Batch {
    DrawState state{};
    BoundTexture texture;
    std::vector<Vertex> vertices;
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    bool native = false, native_alpha_is_one = false;
    bool shader_blend = false, sprites = true;
    std::vector<PixelBox> boxes;
    NativeBatch nb;
};

constexpr VkFormat colour_format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr float first_none = 3.0e38f; // (the first-draw image cleared: no triangle yet)

} // namespace

class Builder final : public Drawing {
public:
    Builder(Device &device, GpuTimes &times, HangNotes &notes)
        : device_(device), times_(times), notes_(notes), drawer_(device, times), store_(store_limits()) {
        const VkFormatFeatureFlags2 need = VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT |
                                           VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
        d16_ = device_.image_format_is_supported(VK_FORMAT_D16_UNORM, need);
        const char *date = getenv("KF4_SCN_DATE");
        first_draw_on_ = drawer_.first_draw_ok() && !(date && strcmp(date, "copy") == 0);
        fprintf(stderr, "[scn] the scene renderer: depth %s for the 16-bit Z buffers (D32F the others), DATE's shadows %s\n",
                d16_ ? "D16_UNORM" : "D32F (no D16_UNORM here)",
                first_draw_on_ ? "by the first-draw image" : "by a copy of the target");
    }

    ~Builder() override {
        flush_batch();
        close_scene(true);
    }

    // ---- Settings ----
    void set_scale(uint32_t x, uint32_t y) override {
        pending_scale_ = {std::clamp(x, 1u, 8u), std::clamp(y, 1u, 8u)};
    }
    void set_sampling(int lod, uint32_t aniso) override {
        lod_mode_ = lod;
        aniso_ = std::clamp(aniso, 1u, 16u);
    }
    void set_2d(int mode) override {
        mode_2d_ = mode;
    }

    // ---- The front end's events ----
    void primitive(const GsRegisterFile &file, const GifPrimitive &p) override {
        counts_.primitives++;
        if (p.shape == PrimitiveShape::point || p.shape == PrimitiveShape::line) {
            counts_.unsupported_shapes++;
            return;
        }
        DrawState s = state_of(file, p.prim);
        bool tme = bits(p.prim, 4, 1) != 0;
        if (!batch_open_ || !(s == batch_.state) || (tme && !texture_covers(p)) ||
            (batch_.shader_blend && overlaps_batch(p))) {
            flush_batch();
            open_batch(s, p);
        }
        add_primitive(p);
    }

    void native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &nb) override {
        flush_batch();
        counts_.native_batches++;
        Batch b;
        b.state = state_of(file, prim);
        b.native = true;
        b.native_alpha_is_one = nb.alpha_is_one;
        b.nb = nb;
        b.shader_blend = !choose_blend(b.state.alpha, bits(b.state.prim, 6, 1) != 0).supported;
        b.x0 = int32_t(bits(b.state.scissor, 0, 11));
        b.x1 = int32_t(bits(b.state.scissor, 16, 11));
        b.y0 = int32_t(bits(b.state.scissor, 32, 11));
        b.y1 = int32_t(bits(b.state.scissor, 48, 11));
        if (bits(b.state.prim, 4, 1)) {
            // (a native batch's texture read whole; the same state as the batch before, with nothing between that
            // changed what the store answers, the same texture. A target's copy too, while nothing is drawn over its
            // pages (drawn_over) and no op writes another image (add_op): as the canvas copies once for the batches it
            // puts together, and without it every batch's copy is an op that ends the pass)
            if (last_native_.valid && last_native_.state == b.state && last_native_.events == store_events_) {
                b.texture = last_native_.texture;
                counts_.textures_reused++;
            } else {
                b.texture = bind_texture(b.state, 1u << std::min(bits(b.state.tex0, 26, 4), 10u),
                                         1u << std::min(bits(b.state.tex0, 30, 4), 10u));
                last_native_ = {true, b.state, store_events_, b.texture, texture_pages(b.state)};
            }
        }
        build_draw(b);
    }

    void texture_written(uint64_t tex0, uint64_t texclut) override {
        std::optional<TextureSource> loaded = store_.load_clut(tex0, texclut);
        if (loaded) {
            clut_serial_++;
            store_events_++;
        }
    }

    void upload(const TransferRegisters &t, std::span<const uint8_t> bytes) override {
        flush_batch();
        store_events_++;
        TransferRegs regs = {t.bitbltbuf, t.trxpos, t.trxreg};
        store_.upload(regs, bytes);
        write_upload_into_targets(t, bytes);
    }

    void local_copy(const TransferRegisters &t) override {
        flush_batch();
        store_events_++;
        copy_between_targets(t);
        store_.copied({t.bitbltbuf, t.trxpos, t.trxreg});
    }

    void local_to_host(const TransferRegisters &t) override {
        flush_batch();
        TransferRegs regs = {t.bitbltbuf, t.trxpos, t.trxreg};
        fifo_at_ = 0;
        if (store_.read_back(regs, fifo_)) {
            counts_.read_backs++;
            return;
        }
        if (read_back_target(t)) {
            counts_.read_backs++;
            return;
        }
        counts_.unsupported_read_backs++;
        fifo_.clear();
    }

    void read_fifo(void *dst, uint32_t qwords) override {
        size_t want = size_t(qwords) * 16, have = fifo_.size() - std::min(fifo_at_, fifo_.size());
        size_t n = std::min(want, have);
        if (n) {
            memcpy(dst, fifo_.data() + fifo_at_, n);
        }
        memset(static_cast<uint8_t *>(dst) + n, 0, want - n);
        fifo_at_ += n;
    }

    // (frame_interp.c's replays and pictures between: not the scene renderer's yet; their marks pass)
    void frame_mark(uint64_t) override {}
    void upload_again(const GsRegisterFile &, uint64_t) override {
        counts_.uploads_again_passed++;
    }

    void forget() override {
        flush_batch();
        close_scene(true);
        store_.forget();
        colour_.clear();
        depth_.clear();
        textures_.clear();
        sampled_.clear();
        target_copies_.clear();
        clut_serial_++;
        store_events_++;
        last_native_ = {};
        alpha_clear_ = nullptr;
        alpha_watches_.clear();
    }

    std::vector<uint8_t> vram_image() override {
        flush_batch();
        close_scene(true);
        std::vector<uint8_t> image = store_.vram_image();
        for (auto &target : colour_) {
            if (target->psm != GSN_PSMCT32) {
                continue;
            }
            std::vector<uint32_t> pixels;
            if (!read_pixels(*target, 0, 0, target->width(), target->height, pixels)) {
                continue;
            }
            uint32_t w = target->width();
            for (uint32_t page = target->base_page; page < target->end_page(); page++) {
                uint32_t px, py;
                if (store_.page_source(page) != TextureSource::DrawnSurface || !target->page_place(page, px, py) ||
                    py + 32 > target->height) {
                    continue;
                }
                for (uint32_t y = 0; y < 32; y++) {
                    gsn_vram_write_row(image.data(), GSN_PSMCT32, target->base_page * 32, target->fbw, px, py + y, 64,
                                       &pixels[size_t(py + y) * w + px]);
                }
            }
        }
        return image;
    }

    TextureStore::ClutState clut_state() const override {
        return store_.clut_state();
    }

    void restore(std::span<const uint8_t> vram, const TextureStore::ClutState &clut) override {
        forget();
        store_.restore_vram(vram);
        store_.set_clut_state(clut);
    }

    bool scanout(CommandBuffer &out, const ShownRect &rect, const Image &dst) override {
        flush_batch();
        if (rect.w == 0 || rect.h == 0 || rect.psm != GSN_PSMCT32) {
            close_scene(false);
            return false;
        }
        Placed placed = colour_target(rect.bp / 32, rect.bw, GSN_PSMCT32, rect.y + rect.h);
        // (the scene so far submitted without a flush: the caller submits out right after, and both go to the GPU's
        // queue in one vkQueueSubmit)
        close_scene(false);
        times_.begin(out, GpuWork::picture);
        Target *s = placed.target;
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

    // (the temporal anti-aliasing's depth: not the scene renderer's yet)
    std::optional<ShownDepth> scanout_depth(CommandBuffer &, const ShownRect &, const Image &) override {
        return std::nullopt;
    }
    void begin_drawing(uint64_t, Jitter) override {}

    void submit() override {
        flush_batch();
        close_scene(true);
    }

    void vblank(uint64_t game_vblank) override {
        flush_batch();
        apply_scale();
        vblank_ = game_vblank;
        close_scene(true);
        {
            GpuWait wait(&notes_, "the scene renderer's next_frame_context", hang_clock_s());
            device_.next_frame_context();
        }
        frame_++;
        evict_textures();
        let_go_of_idle_depth();
    }

    bool settled() const override {
        return pending_scale_ == scale_;
    }

    void print_counts() const override {
        const Drawer::Counts &d = drawer_.counts();
        fprintf(stderr,
                "[stats] scene renderer: %llu scenes drawn; %llu passes, %llu draws (%llu native, %llu GIF, %llu clears in a "
                "pass), %llu ops, %llu load-op clears; %llu primitives, %llu native batches (%llu joined to the draw "
                "before, %llu more with its material, %llu draws with their state set)\n",
                (unsigned long long)counts_.scenes, (unsigned long long)d.passes, (unsigned long long)d.draws,
                (unsigned long long)d.native_draws, (unsigned long long)d.gif_draws, (unsigned long long)d.clears,
                (unsigned long long)d.ops, (unsigned long long)counts_.load_clears,
                (unsigned long long)counts_.primitives, (unsigned long long)counts_.native_batches,
                (unsigned long long)counts_.native_joined, (unsigned long long)counts_.native_same_material,
                (unsigned long long)d.state_set);
        fprintf(stderr,
                "[stats] scene renderer DATE: %llu shadow draws by the first-draw image (%llu darkened), %llu DATE draws by "
                "a copy (%llu of them native); alpha reset seen %llu times; the alpha read between the frame's clear and "
                "its reset in %llu of %llu frames checked\n",
                (unsigned long long)d.first_draws, (unsigned long long)d.darkened, (unsigned long long)counts_.date_copies,
                (unsigned long long)counts_.date_copies_native, (unsigned long long)counts_.alpha_resets,
                (unsigned long long)counts_.alpha_read_frames, (unsigned long long)counts_.alpha_checked_frames);
        fprintf(stderr,
                "[stats] scene renderer targets and textures: %llu targets made, %llu grown, %llu filled from uploads, "
                "%llu uploads written into them, %llu copies between them, %llu depth aliases cleared; textures %llu "
                "decoded, %llu sampled, %llu from targets (%llu into a kept image), %llu missing, %llu reused; "
                "%llu shader blends, %llu read-backs; "
                "not drawn: %llu shapes, %llu blends, %llu textures on targets, %llu copies, %llu uploads into targets, "
                "%llu read-backs; %llu uploads asked again passed\n",
                (unsigned long long)counts_.targets_made, (unsigned long long)counts_.targets_grown,
                (unsigned long long)counts_.filled_from_uploads, (unsigned long long)counts_.uploads_into_targets,
                (unsigned long long)counts_.copies_between_targets, (unsigned long long)counts_.depth_aliases_cleared,
                (unsigned long long)counts_.textures_uploaded, (unsigned long long)counts_.textures_sampled,
                (unsigned long long)counts_.textures_from_targets, (unsigned long long)counts_.target_copies_reused,
                (unsigned long long)counts_.textures_missing,
                (unsigned long long)counts_.textures_reused, (unsigned long long)counts_.shader_blends,
                (unsigned long long)counts_.read_backs, (unsigned long long)counts_.unsupported_shapes,
                (unsigned long long)counts_.unsupported_blends, (unsigned long long)counts_.unsupported_texture_on_target,
                (unsigned long long)counts_.unsupported_copies, (unsigned long long)counts_.unsupported_uploads,
                (unsigned long long)counts_.unsupported_read_backs, (unsigned long long)counts_.uploads_again_passed);
        TextureStoreCounts t = store_.counts();
        fprintf(stderr,
                "[stats] scene renderer texture store: %llu uploads (%llu bytes), %llu resolves (%llu kept answers), %llu "
                "decodes, %llu CLUT loads\n",
                (unsigned long long)t.uploads, (unsigned long long)t.upload_bytes, (unsigned long long)t.resolves,
                (unsigned long long)t.resolves_kept, (unsigned long long)t.decodes, (unsigned long long)t.clut_loads);
    }

private:
    // ---- The scene ----
    // The scene drawn and submitted (flush: put on the GPU's queue now, else with the next submit that flushes)
    void close_scene(bool flush) {
        end_shadow_run();
        pass_.open = false;
        last_clear_ = {};
        if (scene_.empty()) {
            return;
        }
        scene_.vblank = vblank_;
        if (dump_at_ != 0 && vblank_ >= dump_at_ && dumped_ < 3) {
            dump_scene();
            dumped_++;
        }
        CommandBufferHandle cmd = device_.request_command_buffer();
        cmd->set_dynamic_state(drawer_.dynamic_ds(), drawer_.dynamic_blend());
        drawer_.record(scene_, *cmd);
        times_.end(*cmd);
        char what[160];
        snprintf(what, sizeof(what), "scene, vblank %llu: %zu passes, %zu draws, %zu ops", (unsigned long long)vblank_,
                 scene_.passes.size(), scene_.draws.size(), scene_.ops.size());
        notes_.submitted("kf4-pc", what, hang_clock_s());
        device_.submit(cmd);
        if (flush) {
            device_.flush_frame();
        }
        counts_.scenes++;
        scene_.clear();
        clut_at_ = no_clut;
    }

    // (developers', KF4_SCN_DUMP_AT=VBLANK) The scene closed at or after VBLANK, and the two after it, step by step on
    // stderr: each pass's kind, images, load ops and draws, each op's kind, work and size
    void dump_scene() const {
        static const char *kinds[] = {"plain", "first_draw", "darken"};
        static const char *ops[] = {"clear", "copy", "blit"};
        fprintf(stderr, "[scn dump] vblank %llu: %zu steps, %zu passes, %zu draws, %zu ops\n",
                (unsigned long long)vblank_, scene_.steps.size(), scene_.passes.size(), scene_.draws.size(),
                scene_.ops.size());
        for (const Step &st : scene_.steps) {
            if (st.kind == Step::Kind::op) {
                const Op &o = scene_.ops[st.index];
                fprintf(stderr, "[scn dump]   op %-5s work %2d dst %p %ux%u extent %ux%u%s\n", ops[int(o.kind)],
                        int(o.work), (const void *)o.dst.get(), o.dst ? o.dst->get_width() : 0,
                        o.dst ? o.dst->get_height() : 0, o.extent.width, o.extent.height, o.depth ? " depth" : "");
                continue;
            }
            const Pass &ps = scene_.passes[st.index];
            uint32_t native = 0, gif = 0, clears = 0, shadows = 0;
            for (uint32_t i = 0; i < ps.draw_count; i++) {
                const Draw &d = scene_.draws[ps.first_draw + i];
                clears += d.clear ? 1 : 0;
                native += !d.clear && d.native ? 1 : 0;
                gif += !d.clear && !d.native ? 1 : 0;
                shadows += d.shadow != no_shadow ? 1 : 0;
            }
            fprintf(stderr,
                    "[scn dump]   pass %-10s colour %p %ux%u depth %p%s%s: %u draws (%u native, %u GIF, %u clears, "
                    "%u shadows)\n",
                    kinds[int(ps.kind)], (const void *)ps.colour.get(), ps.colour->get_width(),
                    ps.colour->get_height(), (const void *)ps.depth.get(), ps.clear_colour ? " clear-colour" : "",
                    ps.clear_depth ? " clear-depth" : "", ps.draw_count, native, gif, clears, shadows);
        }
    }
    const uint64_t dump_at_ = getenv("KF4_SCN_DUMP_AT") ? strtoull(getenv("KF4_SCN_DUMP_AT"), nullptr, 0) : 0;
    uint32_t dumped_ = 0;

    // The open pass: its colour and depth images and kind; a draw into others begins a new one
    struct OpenPass {
        bool open = false;
        uint32_t index = 0;
        const Image *colour = nullptr, *depth = nullptr;
        PassKind kind = PassKind::plain;
    } pass_;

    Pass &use_pass(const ImageHandle &colour, const ImageHandle &depth, PassKind kind, const ImageHandle &first = {}) {
        if (pass_.open && pass_.kind == kind && pass_.colour == colour.get() &&
            (!depth || pass_.depth == depth.get())) {
            return scene_.passes[pass_.index];
        }
        Pass p;
        p.kind = kind;
        p.colour = colour;
        p.depth = depth;
        p.first = first;
        p.first_draw = uint32_t(scene_.draws.size());
        pass_ = {true, uint32_t(scene_.passes.size()), colour.get(), depth.get(), kind};
        scene_.steps.push_back({Step::Kind::pass, uint32_t(scene_.passes.size())});
        scene_.passes.push_back(std::move(p));
        return scene_.passes.back();
    }

    void add_draw(Draw &&d) {
        scene_.draws.push_back(std::move(d));
        scene_.passes[pass_.index].draw_count++;
    }

    // Something outside the passes: the open one ended (the shadows' darkening drawn first)
    void add_op(Op &&op) {
        end_shadow_run();
        pass_.open = false;
        last_clear_ = {};
        if (op.dst && op.dst.get() == alpha_clear_) {
            alpha_clear_ = nullptr; // (written: its alpha no longer known)
        }
        if (last_native_.valid && last_native_.texture.src == SRC_SURFACE && op.dst) {
            // (a target written outside the passes, or the copy's kept image written again: no longer known the same)
            last_native_.valid = false;
        }
        scene_.steps.push_back({Step::Kind::op, uint32_t(scene_.ops.size())});
        scene_.ops.push_back(std::move(op));
    }

    void op_clear(const ImageHandle &image, bool depth, float value = 0.0f) {
        Op op;
        op.kind = Op::Kind::clear;
        op.dst = image;
        op.depth = depth;
        if (depth) {
            op.value.depthStencil.depth = value;
        }
        add_op(std::move(op));
    }

    void op_copy(const ImageHandle &dst, VkOffset3D dst_at, const ImageHandle &src, VkOffset3D src_at,
                 VkExtent3D extent, bool depth, GpuWork work) {
        Op op;
        op.kind = Op::Kind::copy;
        op.dst = dst;
        op.src = src;
        op.dst_at = dst_at;
        op.src_at = src_at;
        op.extent = extent;
        op.depth = depth;
        op.work = work;
        add_op(std::move(op));
    }

    void op_blit(const ImageHandle &dst, VkOffset3D dst_at, VkExtent3D extent, const ImageHandle &src,
                 VkOffset3D src_at, VkExtent3D src_extent, GpuWork work) {
        Op op;
        op.kind = Op::Kind::blit;
        op.dst = dst;
        op.src = src;
        op.dst_at = dst_at;
        op.extent = extent;
        op.src_at = src_at;
        op.src_extent = src_extent;
        op.work = work;
        add_op(std::move(op));
    }

    // ---- DATE's shadows by the first-draw image ----
    // The run of shadows drawn into the first-draw image since the last darkening, and the numbers given out since the
    // target's alpha was reset (one image a frame, cleared by the run after the reset)
    struct ShadowRun {
        ImageHandle colour, depth;
        std::vector<uint32_t> draws; // (scene_.draws' of the open run)
        uint32_t next = 0;           // the next triangle's number
        bool cleared = false;        // the first-draw image cleared since the reset
    } run_;
    ImageHandle first_image_;

    // The run's shadows drawn where their triangle came first, darkening the target
    void end_shadow_run() {
        if (run_.draws.empty()) {
            return;
        }
        std::vector<uint32_t> draws;
        draws.swap(run_.draws);
        pass_.open = false;
        use_pass(run_.colour, run_.depth, PassKind::darken, first_image_);
        for (uint32_t i : draws) {
            Draw d = scene_.draws[i];
            add_draw(std::move(d));
        }
        pass_.open = false; // (what follows is not the darkening's)
    }

    // ---- Targets ----
    ImageHandle make_target_image(uint32_t width, uint32_t height, bool depth, uint32_t psm) {
        VkFormat format = !depth ? colour_format
                          : d16_ && (psm == 0x32 || psm == 0x3A) ? VK_FORMAT_D16_UNORM
                                                                : VK_FORMAT_D32_SFLOAT;
        ImageCreateInfo info = ImageCreateInfo::render_target(width * scale_[0], height * scale_[1], format);
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                            : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageHandle image = device_.create_image(info, nullptr);
        if (!image) {
            fprintf(stderr, "[scn] no image for a %ux%u target\n", width, height);
        }
        return image;
    }

    struct Placed {
        Target *target = nullptr;
        uint32_t x = 0, y = 0;
    };

    // The colour target a frame at this base page and width draws into, height rows of it (pcr_canvas.cpp's
    // colour_surface: the same rules, as ops of the scene)
    Placed colour_target(uint32_t base_page, uint32_t fbw, uint32_t psm, uint32_t height, bool covered = false) {
        for (auto &s : colour_) {
            uint32_t x, y;
            if (s->fbw == fbw && s->page_place(base_page, x, y) && x == 0) {
                uint32_t needed = rounded_height(y + height, psm);
                if (s->height < needed) {
                    Target *grown = s.get();
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
        auto made = std::make_unique<Target>();
        made->base_page = base_page;
        made->fbw = fbw;
        made->psm = psm;
        made->height = rounded_height(height, psm);
        std::vector<std::pair<Target *, uint32_t>> taken;
        for (auto &o : colour_) {
            uint32_t rows = page_rows(psm), w = std::max(1u, fbw);
            if (o->fbw == fbw && o->psm == psm && o->base_page >= base_page && (o->base_page - base_page) % w == 0 &&
                o->overlaps(base_page, made->end_page())) {
                uint32_t y = (o->base_page - base_page) / w * rows;
                made->height = std::max(made->height, rounded_height(y + o->height, psm));
                taken.emplace_back(o.get(), y);
            }
        }
        made->image = make_target_image(made->width(), made->height, false, made->psm);
        if (!made->image) {
            return {};
        }
        counts_.targets_made++;
        op_clear(made->image, false);
        const bool whole = covered && taken.empty() && made->height == height;
        if (!whole) {
            fill_from_uploads(*made);
            for (auto [o, y] : taken) {
                uint32_t top = y * scale_[1];
                uint32_t rows = made->image->get_height() - std::min(top, made->image->get_height());
                rows = std::min(rows, o->image->get_height());
                if (rows != 0) {
                    op_copy(made->image, {0, int32_t(top), 0}, o->image, {0, 0, 0},
                            {o->image->get_width(), rows, 1}, false, GpuWork::surface);
                }
            }
        }
        let_go_overlapping(colour_, made->base_page, made->end_page());
        colour_.push_back(std::move(made));
        return {colour_.back().get(), 0, 0};
    }

    Target *depth_target(uint32_t base_page, uint32_t fbw, uint32_t zpsm, uint32_t height) {
        height = rounded_height(height, zpsm);
        for (auto &s : depth_) {
            if (s->base_page == base_page && s->fbw == fbw && s->psm == zpsm) {
                if (s->height < height) {
                    grow(*s, height);
                }
                s->used = frame_;
                return s.get();
            }
        }
        auto made = std::make_unique<Target>();
        made->base_page = base_page;
        made->fbw = fbw;
        made->psm = zpsm;
        made->height = height;
        made->depth = true;
        made->used = frame_;
        made->image = make_target_image(made->width(), height, true, made->psm);
        if (!made->image) {
            return nullptr;
        }
        counts_.targets_made++;
        op_clear(made->image, true, 0.0f);
        depth_.push_back(std::move(made));
        return depth_.back().get();
    }

    // (pcr_canvas.cpp's let_go_of_idle_depth)
    static constexpr uint64_t IDLE_DEPTH_VBLANKS = 600;
    void let_go_of_idle_depth() {
        for (auto it = depth_.begin(); it != depth_.end();) {
            Target *s = it->get();
            it = frame_ > s->used && frame_ - s->used > IDLE_DEPTH_VBLANKS ? depth_.erase(it) : std::next(it);
        }
    }

    void let_go_overlapping(std::vector<std::unique_ptr<Target>> &list, uint32_t first, uint32_t end) {
        for (auto it = list.begin(); it != list.end();) {
            if ((*it)->overlaps(first, end)) {
                if ((*it)->image.get() == alpha_clear_) {
                    alpha_clear_ = nullptr;
                }
                it = list.erase(it);
            } else {
                ++it;
            }
        }
    }

    void grow(Target &s, uint32_t height) {
        ImageHandle bigger = make_target_image(s.width(), height, s.depth, s.psm);
        if (!bigger) {
            return;
        }
        counts_.targets_grown++;
        op_clear(bigger, s.depth, 0.0f);
        op_copy(bigger, {0, 0, 0}, s.image, {0, 0, 0}, {s.image->get_width(), s.image->get_height(), 1}, s.depth,
                GpuWork::surface);
        if (s.image.get() == alpha_clear_) {
            alpha_clear_ = bigger.get();
        }
        s.image = bigger;
        s.cleared = false;
        s.height = height;
    }

    // (pcr_canvas.cpp's take_in_below)
    void take_in_below(Target &grown, uint32_t old_end) {
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
            if (height != 0) {
                op_copy(grown.image, {0, int32_t(top), 0}, o->image, {0, 0, 0}, {o->image->get_width(), height, 1},
                        false, GpuWork::surface);
            }
        }
        if (!any) {
            return;
        }
        for (auto it = colour_.begin(); it != colour_.end();) {
            Target *o = it->get();
            if (o != &grown && o->base_page >= old_end && o->base_page < end) {
                forget_alpha(o->image.get());
                it = colour_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // (pcr_canvas.cpp's fill_from_uploads)
    void fill_from_uploads(Target &s) {
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
        counts_.filled_from_uploads++;
        write_pixels(s, 0, 0, w, h, pixels.data());
    }

    // GS pixels (RGBA8, w x h) into a colour target at (x, y), each repeated at the scale: a staged image, blitted
    void write_pixels(Target &s, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *pixels) {
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(w, h, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageInitialData data = {pixels, 0, 0};
        ImageHandle staged = device_.create_image(info, &data);
        if (!staged) {
            return;
        }
        int32_t dx = int32_t(x * scale_[0]), dy = int32_t(y * scale_[1]);
        int32_t dw = int32_t(std::min(w, s.width() - std::min(x, s.width())) * scale_[0]);
        int32_t dh = int32_t(std::min(h, s.height - std::min(y, s.height)) * scale_[1]);
        if (dw <= 0 || dh <= 0) {
            return;
        }
        op_blit(s.image, {dx, dy, 0}, {uint32_t(dw), uint32_t(dh), 1}, staged, {0, 0, 0},
                {uint32_t(dw) / scale_[0], uint32_t(dh) / scale_[1], 1}, GpuWork::upload);
    }

    // (pcr_canvas.cpp's write_upload_into_surfaces)
    void write_upload_into_targets(const TransferRegisters &t, std::span<const uint8_t> bytes) {
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
                counts_.unsupported_uploads++;
                continue;
            }
            std::vector<uint32_t> pixels(size_t(w) * h);
            memcpy(pixels.data(), bytes.data(), pixels.size() * 4);
            uint32_t ux = px + x, uy = py + y;
            if (uy + h > s->height) {
                grow(*s, rounded_height(uy + h, s->psm));
            }
            write_pixels(*s, ux, uy, w, h, pixels.data());
            counts_.uploads_into_targets++;
        }
    }

    // (pcr_canvas.cpp's copy_between_surfaces)
    void copy_between_targets(const TransferRegisters &t) {
        uint32_t sbp = bits(t.bitbltbuf, 0, 14), sbw = bits(t.bitbltbuf, 16, 6), spsm = bits(t.bitbltbuf, 24, 6);
        uint32_t dbp = bits(t.bitbltbuf, 32, 14), dbw = bits(t.bitbltbuf, 48, 6), dpsm = bits(t.bitbltbuf, 56, 6);
        uint32_t sx = bits(t.trxpos, 0, 11), sy = bits(t.trxpos, 16, 11);
        uint32_t dx = bits(t.trxpos, 32, 11), dy = bits(t.trxpos, 48, 11);
        uint32_t w = bits(t.trxreg, 0, 12), h = bits(t.trxreg, 32, 12);
        Target *from = nullptr;
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
        alpha_read(from->image.get());
        bool covered = dx == 0 && dy == 0 && w >= std::max(1u, dbw) * 64 && fx + sx + w <= from->width() &&
                       fy + sy + h <= from->height;
        Placed placed = colour_target(dbp / 32, dbw, GSN_PSMCT32, dy + h, covered);
        Target *to = placed.target;
        find_from();
        if (!to || !from) {
            return;
        }
        uint32_t tx = placed.x, ty = placed.y;
        uint32_t cw = std::min({w, from->width() - std::min(fx + sx, from->width()),
                                to->width() - std::min(tx + dx, to->width())});
        uint32_t ch = std::min({h, from->height - std::min(fy + sy, from->height),
                                to->height - std::min(ty + dy, to->height)});
        if (cw == 0 || ch == 0) {
            return;
        }
        if (from == to) {
            counts_.unsupported_copies++;
            return;
        }
        op_copy(to->image, {int32_t((tx + dx) * scale_[0]), int32_t((ty + dy) * scale_[1]), 0}, from->image,
                {int32_t((fx + sx) * scale_[0]), int32_t((fy + sy) * scale_[1]), 0}, {cw * scale_[0], ch * scale_[1], 1},
                false, GpuWork::copy);
        counts_.copies_between_targets++;
    }

    // A rectangle of a colour target's GS pixels (the first of each at the scale) read from the GPU, as RGBA8. Draws the
    // scene so far and waits for the GPU.
    bool read_pixels(Target &from, uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<uint32_t> &out) {
        close_scene(false);
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
        CommandBufferHandle c = device_.request_command_buffer();
        times_.begin(*c, GpuWork::copy);
        c->barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        c->blit_image(*one, *from.image, {0, 0, 0}, {int32_t(w), int32_t(h), 1},
                      {int32_t(x * scale_[0]), int32_t(y * scale_[1]), 0},
                      {int32_t(w * scale_[0]), int32_t(h * scale_[1]), 1}, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
        c->barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        c->copy_image_to_buffer(*buffer, *one, 0, {0, 0, 0}, {w, h, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        c->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                   VK_ACCESS_2_HOST_READ_BIT);
        times_.end(*c);
        Fence fence;
        device_.submit(c, &fence);
        {
            GpuWait wait(&notes_, "the scene renderer's read-back", hang_clock_s());
            fence->wait();
        }
        const uint8_t *mapped = static_cast<const uint8_t *>(device_.map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT));
        if (!mapped) {
            return false;
        }
        out.resize(size_t(w) * h);
        memcpy(out.data(), mapped, out.size() * 4);
        device_.unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
        return true;
    }

    // (pcr_canvas.cpp's read_back_surface)
    bool read_back_target(const TransferRegisters &t) {
        uint32_t sbp = bits(t.bitbltbuf, 0, 14), sbw = bits(t.bitbltbuf, 16, 6), spsm = bits(t.bitbltbuf, 24, 6);
        uint32_t sx = bits(t.trxpos, 0, 11), sy = bits(t.trxpos, 16, 11);
        uint32_t w = bits(t.trxreg, 0, 12), h = bits(t.trxreg, 32, 12);
        if (spsm != GSN_PSMCT32 || sbp % 32 != 0 || w == 0 || h == 0) {
            return false;
        }
        Target *from = nullptr;
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
        alpha_read(from->image.get());
        std::vector<uint32_t> pixels;
        if (!read_pixels(*from, fx + sx, fy + sy, w, h, pixels)) {
            return false;
        }
        fifo_.assign((size_t(w) * h * 4 + 15) / 16 * 16, 0);
        memcpy(fifo_.data(), pixels.data(), pixels.size() * 4);
        return true;
    }

    // A new scale (from the vblank): every target made again at it, the colour's pixels scaled into it
    void apply_scale() {
        if (pending_scale_ == scale_) {
            return;
        }
        scale_ = pending_scale_;
        for (auto *list : {&colour_, &depth_}) {
            for (auto &s : *list) {
                ImageHandle image = make_target_image(s->width(), s->height, s->depth, s->psm);
                if (!image) {
                    continue;
                }
                if (s->depth) {
                    op_clear(image, true, 0.0f);
                } else {
                    op_blit(image, {0, 0, 0}, {image->get_width(), image->get_height(), 1}, s->image, {0, 0, 0},
                            {s->image->get_width(), s->image->get_height(), 1}, GpuWork::surface);
                }
                s->image = image;
            }
        }
        first_image_.reset();
        last_native_ = {};
    }

    // ---- Textures (pcr_canvas.cpp's) ----
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
                b.image = t->image;
                b.src = SRC_SAMPLED;
                b.levels = t->levels;
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
            b.image = t->image;
            b.levels = t->levels;
            b.sampling = sampling_bits(1);
            b.cols = std::max(cols, a.texture->read_cols);
            b.rows = std::max(rows, a.texture->read_rows);
            b.ok = true;
            if (b.src == SRC_INDEX) {
                b.clut = store_.clut_colours(s.tex0, s.texa).rgba;
            }
            return b;
        }
        if (a.source == TextureSource::DrawnSurface) {
            return target_texture(s.tex0, cols, rows);
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
        t.used = frame_;
        return &t;
    }

    bool sampler_reads(const DrawState &s, const DecodedTexture &d) {
        if (!sampler_on_) {
            return false;
        }
        uint32_t wms = bits(s.clamp, 0, 2), wmt = bits(s.clamp, 2, 2);
        if (wms >= 2 || wmt >= 2) {
            return false;
        }
        uint32_t tw = 1u << std::min(bits(s.tex0, 26, 4), 10u), th = 1u << std::min(bits(s.tex0, 30, 4), 10u);
        bool across = wms == 0 ? d.width_px == tw : d.width_px <= tw;
        bool down = wmt == 0 ? d.height_px == th : d.height_px <= th;
        return across && down;
    }

    // A texture as the GPU's sampler reads it: decoded once, the CLUT's colours and TEXA in it, kept by all it was made
    // from (the design's materials: an image kept by its key)
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
        SampledTexture &t = sampled_[h];
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

    uint32_t anisotropy_for(uint64_t tex1, uint32_t levels) const {
        uint32_t mmag = bits(tex1, 5, 1), mmin = bits(tex1, 6, 3);
        return aniso_ > 1 && levels > 1 && mmag == 1 && (mmin == 4 || mmin == 5) ? aniso_ : 1;
    }

    uint32_t sampling_bits(uint32_t aniso) const {
        float bias = lod_mode_ == GSN_LOD_FULL    ? -64.0f
                     : lod_mode_ == GSN_LOD_SCALE ? -std::log2(float(std::min(scale_[0], scale_[1])))
                                                  : 0.0f;
        int32_t b256 = int32_t(std::lround(bias * 256.0f));
        uint32_t a = 0;
        while (a < 4 && (2u << a) <= aniso) {
            a++;
        }
        return a << 10 | uint32_t(b256) << 16;
    }

    const Sampler &sampler_for(uint64_t tex1, uint64_t clamp, uint32_t levels, uint32_t aniso) {
        uint32_t mmag = bits(tex1, 5, 1), mmin = bits(tex1, 6, 3);
        bool mipmapped = mmin >= 2 && mmin <= 5 && levels > 1;
        uint32_t wrap = bits(clamp, 0, 4);
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
            return device_.get_stock_sampler(StockSampler::NearestClamp);
        }
        return *samplers_.emplace(key, made).first->second;
    }

    // A texture where a target was drawn: the target's part copied (an op), as the canvas's surface_texture
    BoundTexture target_texture(uint64_t tex0, uint32_t cols, uint32_t rows) {
        BoundTexture b;
        uint32_t tbp = bits(tex0, 0, 14), tbw = bits(tex0, 14, 6), psm = bits(tex0, 20, 6);
        Target *from = nullptr;
        uint32_t ox = 0, oy = 0;
        for (auto &s : colour_) {
            if (tbp % 32 == 0 && s->fbw == tbw && s->page_place(tbp / 32, ox, oy)) {
                from = s.get();
                break;
            }
        }
        if (!from || (psm != GSN_PSMCT32 && psm != GSN_PSMCT24 && !(is_colour_16(psm) && from->psm == psm))) {
            counts_.unsupported_texture_on_target++;
            return b;
        }
        uint32_t w = std::min(cols, from->width() - std::min(ox, from->width()));
        uint32_t h = std::min(rows, from->height - std::min(oy, from->height));
        if (w == 0 || h == 0) {
            counts_.unsupported_texture_on_target++;
            return b;
        }
        alpha_read(from->image.get());
        // One image kept a size and format, written again by each copy: the scene's steps are drawn in order with a
        // full barrier around each op, so a copy into it comes after every draw that read what it held before. It is
        // cleared only where what was copied into it before reaches past this copy (as pcr_copy_pool.h's).
        uint32_t pw = w * scale_[0], ph = h * scale_[1];
        uint64_t key = uint64_t(cols * scale_[0]) | uint64_t(rows * scale_[1]) << 16 |
                       uint64_t(from->image->get_format()) << 32;
        TargetCopy &kept = target_copies_[key];
        bool clear_first = false;
        if (!kept.image) {
            ImageCreateInfo info =
                ImageCreateInfo::render_target(cols * scale_[0], rows * scale_[1], from->image->get_format());
            info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            info.layout = ImageLayout::General;
            info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
            kept.image = device_.create_image(info, nullptr);
            if (!kept.image) {
                target_copies_.erase(key);
                return b;
            }
            clear_first = w < cols || h < rows;
        } else {
            clear_first = kept.w > pw || kept.h > ph;
            counts_.target_copies_reused++;
        }
        kept.w = clear_first ? pw : std::max(kept.w, pw);
        kept.h = clear_first ? ph : std::max(kept.h, ph);
        ImageHandle copy = kept.image;
        if (clear_first) {
            Op clear;
            clear.kind = Op::Kind::clear;
            clear.dst = copy;
            clear.work = GpuWork::texture_copy;
            add_op(std::move(clear));
        }
        op_copy(copy, {0, 0, 0}, from->image, {int32_t(ox * scale_[0]), int32_t(oy * scale_[1]), 0},
                {w * scale_[0], h * scale_[1], 1}, false, GpuWork::texture_copy);
        b.image = copy;
        b.src = SRC_SURFACE;
        b.cols = cols;
        b.rows = rows;
        b.ok = true;
        counts_.textures_from_targets++;
        return b;
    }

    struct TargetCopy {
        ImageHandle image;
        uint32_t w = 0, h = 0; // (pixels: what copies wrote into it since it was cleared)
    };
    std::unordered_map<uint64_t, TargetCopy> target_copies_;

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

    // ---- The GIF's batches (pcr_canvas.cpp's) ----
    void open_batch(const DrawState &s, const GifPrimitive &p) {
        batch_ = Batch();
        batch_.state = s;
        batch_.shader_blend = !choose_blend(s.alpha, bits(s.prim, 6, 1) != 0).supported;
        if (bits(s.prim, 4, 1)) {
            uint32_t cols = 0, rows = 0;
            texels_read(p, s.tex0, cols, rows);
            batch_.texture = bind_texture(s, cols, rows);
        }
        batch_open_ = true;
    }

    void flush_batch() {
        if (!batch_open_) {
            return;
        }
        batch_open_ = false;
        if (!batch_.vertices.empty()) {
            build_draw(batch_);
        }
        batch_ = Batch();
    }

    bool overlaps_batch(const GifPrimitive &p) const {
        PixelBox b = box_of(p, batch_.state.xyoffset);
        for (const PixelBox &o : batch_.boxes) {
            if (b.x0 <= o.x1 && o.x0 <= b.x1 && b.y0 <= o.y1 && o.y0 <= b.y1) {
                return true;
            }
        }
        return false;
    }

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
            }
        }
    }

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
        batch_.sprites = batch_.sprites && p.shape != PrimitiveShape::triangle;
        if (p.shape == PrimitiveShape::triangle) {
            const GifVertex &last = p.vertices[2];
            uint32_t zmax = zmax_of(z_psm(batch_.state.zbuf));
            if (p.vertices[0].z > zmax || p.vertices[1].z > zmax || p.vertices[2].z > zmax) {
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
                touch(batch_.vertices.back());
            }
            if (bits(batch_.state.prim, 4, 1)) {
                const Vertex *t = &batch_.vertices[first_vertex];
                bool flat = t[0].z == t[1].z && t[1].z == t[2].z && t[0].q == t[1].q && t[1].q == t[2].q;
                if (fst || flat) {
                    texel_rect(&batch_.vertices[first_vertex], 3, fst);
                }
            }
            return;
        }
        const GifVertex &a = p.vertices[0], &b = p.vertices[1];
        Vertex va = make(a, b), vb = make(b, b);
        va.z = vb.z;
        va.fog = vb.fog;
        memcpy(va.rgba, vb.rgba, 4);
        if (!fst) {
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
        c.s = vb.s;
        d.y = vb.y;
        d.t = vb.t;
        batch_.vertices.insert(batch_.vertices.end(), {va, c, d, c, vb, d});
        touch(va);
        touch(vb);
    }

    static bool blend_is_source(const Batch &b) {
        const DrawState &s = b.state;
        if (bits(s.alpha, 0, 2) != 0 || bits(s.alpha, 2, 2) != 1 || bits(s.alpha, 6, 2) != 1 || b.shader_blend) {
            return false;
        }
        uint32_t c = bits(s.alpha, 4, 2);
        if (c == 2) {
            return bits(s.alpha, 32, 8) == 0x80;
        }
        if (c != 0) {
            return false;
        }
        if (b.native) {
            if (b.nb.program != NativeProgram::vu1_000 || !b.native_alpha_is_one) {
                return false;
            }
        } else {
            if (b.vertices.empty()) {
                return false;
            }
            for (const Vertex &v : b.vertices) {
                if (v.rgba[3] != 0x80) {
                    return false;
                }
            }
        }
        if (!bits(s.prim, 4, 1) || !bits(s.tex0, 34, 1)) {
            return true;
        }
        uint32_t tfx = bits(s.tex0, 35, 2);
        return tfx <= 1 && b.texture.ok && b.texture.alpha_lo == 0x80 && b.texture.alpha_hi == 0x80;
    }

    // (pcr_canvas.cpp's clear_depth_aliases: a Z buffer cleared reaches the Z buffers of other widths over its pages)
    void clear_depth_aliases(const Batch &b, const Target &depth) {
        if (b.native || !b.sprites || b.vertices.empty()) {
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
        int32_t x1 = std::min(b.x1, int32_t(bits(scissor, 16, 11)) + 1);
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
            uint32_t zmax = zmax_of(o->psm);
            if (o->cleared && o->cleared_z == std::min(z, zmax)) {
                continue;
            }
            op_clear(o->image, true, depth_of(z, o->psm));
            o->cleared = true;
            o->cleared_z = std::min(z, zmax);
            counts_.depth_aliases_cleared++;
        }
    }

    // A batch's rectangle in its colour target's pixels (at the scale): its box within its scissor, within the target
    VkRect2D drawn_rect(uint64_t scissor, const PixelBox &box, const Target &colour, const Placed &placed) const {
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

    // A rectangle of a colour target copied for a shader to read while the target is drawn into (an op)
    ImageHandle copy_of_rect(const Target &s, const VkRect2D &rect, GpuWork work) {
        ImageCreateInfo info = ImageCreateInfo::render_target(rect.extent.width, rect.extent.height,
                                                              s.image->get_format());
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageHandle copy = device_.create_image(info, nullptr);
        if (!copy) {
            return copy;
        }
        op_copy(copy, {0, 0, 0}, s.image, {rect.offset.x, rect.offset.y, 0}, {rect.extent.width, rect.extent.height, 1},
                false, work);
        return copy;
    }

    // (pcr_canvas.cpp's as_clear) A batch that is a clear drawn as a rectangle: its pixels in the target
    bool as_clear(const Batch &b, const Placed &placed, const Target &colour, bool zte, bool zwrite, uint32_t ztst,
                  bool date, const Material::AlphaPass &pass, bool pass_rgb, bool pass_alpha, uint32_t pass_count,
                  const VkRect2D &scissor_px, VkClearRect &rect, bool &z_only) const {
        const DrawState &s = b.state;
        z_only = zwrite && !pass_rgb && !pass_alpha && pass.zwrite;
        if (b.native || b.shader_blend || !b.sprites || b.vertices.size() != 6 || bits(s.prim, 4, 3) != 0 || date ||
            pass_count != 1 || pass.mode != 0 || (!z_only && (!pass_rgb || !pass_alpha)) || (zte && ztst != 1) ||
            (zwrite && !zte)) {
            return false;
        }
        const Vertex &v0 = b.vertices[0];
        float x0 = v0.x, y0 = v0.y, x1 = v0.x, y1 = v0.y;
        for (const Vertex &v : b.vertices) {
            if ((!z_only && memcmp(v.rgba, v0.rgba, 4) != 0) || v.z != v0.z) {
                return false;
            }
            x0 = std::min(x0, v.x);
            y0 = std::min(y0, v.y);
            x1 = std::max(x1, v.x);
            y1 = std::max(y1, v.y);
        }
        float sx0 = float(scissor_px.offset.x) / float(scale_[0]), sy0 = float(scissor_px.offset.y) / float(scale_[1]);
        float sx1 = float(scissor_px.offset.x + int32_t(scissor_px.extent.width)) / float(scale_[0]);
        float sy1 = float(scissor_px.offset.y + int32_t(scissor_px.extent.height)) / float(scale_[1]);
        x0 += float(placed.x);
        x1 += float(placed.x);
        y0 += float(placed.y);
        y1 += float(placed.y);
        auto whole_or_out = [](float e, float lo, float hi, uint32_t scale) {
            float t = e * float(scale);
            return e <= lo || e >= hi || t == std::floor(t);
        };
        if (!whole_or_out(x0, sx0, sx1, scale_[0]) || !whole_or_out(x1, sx0, sx1, scale_[0]) ||
            !whole_or_out(y0, sy0, sy1, scale_[1]) || !whole_or_out(y1, sy0, sy1, scale_[1])) {
            return false;
        }
        int32_t px0 = std::max(scissor_px.offset.x, int32_t(std::max(x0, 0.0f) * float(scale_[0])));
        int32_t py0 = std::max(scissor_px.offset.y, int32_t(std::max(y0, 0.0f) * float(scale_[1])));
        int32_t px1 = std::min<int64_t>(scissor_px.offset.x + int32_t(scissor_px.extent.width),
                                        int64_t(std::max(x1, 0.0f) * float(scale_[0])));
        int32_t py1 = std::min<int64_t>(scissor_px.offset.y + int32_t(scissor_px.extent.height),
                                        int64_t(std::max(y1, 0.0f) * float(scale_[1])));
        px1 = std::min(px1, int32_t(colour.image->get_width()));
        py1 = std::min(py1, int32_t(colour.image->get_height()));
        if (px1 <= px0 || py1 <= py0) {
            return false;
        }
        rect = {{{px0, py0}, {uint32_t(px1 - px0), uint32_t(py1 - py0)}}, 0, 1};
        return true;
    }

    // (pcr_canvas.cpp's drawn_over) What a batch drew over told to the texture store
    void drawn_over(const Batch &b, const PixelBox &touched, bool wrote_z) {
        const DrawState &s = b.state;
        PixelBox box = touched;
        if (!b.native) {
            box.x1 -= 1;
            box.y1 -= 1;
        }
        uint32_t fbp = bits(s.frame, 0, 9), fbw = bits(s.frame, 16, 6), fpsm = bits(s.frame, 24, 6);
        uint32_t scax0 = bits(s.scissor, 0, 11), scax1 = bits(s.scissor, 16, 11);
        uint32_t scay0 = bits(s.scissor, 32, 11), scay1 = bits(s.scissor, 48, 11);
        int32_t x0 = std::max(box.x0, int32_t(scax0)), x1 = std::min(box.x1, int32_t(scax1));
        int32_t y0 = std::max(box.y0, int32_t(scay0)), y1 = std::min(box.y1, int32_t(scay1));
        if (x1 >= x0 && y1 >= y0) {
            VramRect drawn = {fbp * 32, fbw, fpsm, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1)};
            store_.drawn(drawn);
            std::bitset<512> pages = pages_of(drawn);
            if (wrote_z) {
                VramRect z = {bits(s.zbuf, 0, 9) * 32, fbw, z_psm(s.zbuf), uint32_t(x0), uint32_t(y0),
                              uint32_t(x1 - x0 + 1), uint32_t(y1 - y0 + 1)};
                store_.drawn(z);
                pages |= pages_of(z);
            }
            // (the last native batch's texture kept only while nothing is drawn over its pages)
            if (last_native_.valid && (pages & last_native_.pages).any()) {
                last_native_.valid = false;
            }
        }
    }

    // GsDraw_SetScreenAlpha(0) (game/gfx/gs_draw.c): sprites over the whole frame, untextured, one pass that writes
    // the alpha, every alpha written with bit 7 clear. The frame's alpha is then known to be clear everywhere. (The
    // frame is placed down its target where both frame buffers are one target, as at 2x with native widescreen: the
    // back buffer at row 448.)
    static bool is_alpha_reset(const Batch &b, const Target &colour, const Placed &placed, uint32_t pass_count,
                               const Material::AlphaPass &pass) {
        const DrawState &s = b.state;
        if (b.native || !b.sprites || b.vertices.empty() || pass_count != 1 || pass.mode != 0 || !(pass.mask & 0x8) ||
            bits(s.prim, 4, 1) || bits(s.fba, 0, 1) || placed.x != 0 || placed.y >= colour.height) {
            return false;
        }
        for (const Vertex &v : b.vertices) {
            if (v.rgba[3] & 0x80) {
                return false;
            }
        }
        uint32_t scax0 = bits(s.scissor, 0, 11), scax1 = bits(s.scissor, 16, 11);
        uint32_t scay0 = bits(s.scissor, 32, 11), scay1 = bits(s.scissor, 48, 11);
        int32_t bottom = int32_t(std::min(colour.height - placed.y, scay1 + 1));
        return scax0 == 0 && scay0 == 0 && scax1 + 1 >= colour.width() && b.x0 <= 0 && b.y0 <= 0 &&
               b.x1 >= int32_t(colour.width()) && b.y1 >= bottom;
    }

    // The frame's clear: a clear of the GS frame's own rectangle, its scissor from its top left over the target's
    // width and at least half a frame (224 rows) tall, all of it cleared. The frame can be placed down its target (both
    // frame buffers in one target), so it is found by its scissor and place, not the image's size.
    bool is_frame_clear(const VkRect2D &rect, const VkRect2D &scissor_px, const Target &colour, const Placed &placed,
                        uint32_t scax0, uint32_t scax1, uint32_t scay0, uint32_t scay1) const {
        if (placed.x != 0 || scax0 != 0 || scay0 != 0 || scax1 + 1 < colour.width() || scay1 + 1 < 224) {
            return false;
        }
        return rect.offset.x == 0 && rect.offset.y == int32_t(placed.y * scale_[1]) &&
               rect.extent.width >= colour.image->get_width() && rect.offset.x == scissor_px.offset.x &&
               rect.offset.y == scissor_px.offset.y && rect.extent.height >= scissor_px.extent.height;
    }

    // ---- A batch as the scene's draw (pcr_canvas.cpp's draw_batch, the depth KF4_PC_DEPTH=unorm's) ----
    void build_draw(Batch &b) {
        const DrawState &s = b.state;
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
        struct PassBits {
            uint32_t mode;
            bool rgb, alpha, z;
        };
        std::array<PassBits, 2> passes{};
        uint32_t pass_count = 0;
        if (!ate || atst == 1) {
            passes[pass_count++] = {0, true, true, zwrite};
        } else {
            PassBits fail = {2, afail == 1 || afail == 3, afail == 1, afail == 2 && zwrite};
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
        uint32_t height = uint32_t(std::clamp(std::min(b.y1, int32_t(scay1)) + 1, 1, 2048));
        // (the shadows' run ended before anything else is drawn, so that the targets it makes and copies follow it)
        bool date = bits(s.test, 14, 1) != 0;
        bool shadow_candidate = date && first_draw_on_ && b.native;
        if (!shadow_candidate) {
            end_shadow_run();
        }
        Placed placed = colour_target(fbp, fbw, fpsm == GSN_PSMCT24 ? GSN_PSMCT32 : fpsm, height);
        Target *colour = placed.target;
        if (!colour) {
            return;
        }
        Target *depth = nullptr;
        if (zte) {
            depth = depth_target(bits(s.zbuf, 0, 9), fbw, z_psm(s.zbuf), height);
            if (depth && depth->height < colour->height) {
                grow(*depth, colour->height);
            }
        }
        zte = zte && depth != nullptr;
        zwrite = zwrite && zte;
        if (zwrite && ztst == 1) {
            clear_depth_aliases(b, *depth);
        }
        if (zwrite) {
            depth->cleared = false;
        }
        PixelBox box = {b.x0, b.y0, b.x1, b.y1};
        BlendChoice blend = choose_blend(s.alpha, bits(s.prim, 6, 1) != 0);
        if (!blend.supported && !b.shader_blend) {
            counts_.unsupported_blends++;
        }
        if (blend.enable && unblend_on_ && blend_is_source(b)) {
            blend = BlendChoice();
        }
        if (b.shader_blend) {
            blend = BlendChoice();
            blend.flags = F_SHADER_BLEND;
            counts_.shader_blends++;
        }
        Material m;
        m.program = !b.native                                    ? Program::gif
                    : b.nb.program == NativeProgram::vu1_280      ? Program::nd280
                    : b.nb.program == NativeProgram::vu1_700      ? Program::nd700
                    : b.nb.program == NativeProgram::ripple       ? Program::ripple
                                                                  : Program::nd000;
        m.pass_count = pass_count;
        for (uint32_t i = 0; i < pass_count; i++) {
            m.passes[i].mode = passes[i].mode;
            m.passes[i].mask = (passes[i].rgb && blend.write_rgb ? 0x7u : 0u) | (passes[i].alpha ? 0x8u : 0u);
            m.passes[i].zwrite = passes[i].z && zwrite;
        }
        // DATE: the shadows by the first-draw image where the target's alpha is known clear but for the shadows' own
        // (each drawn once, the first draw passing its depth test, and every one of them sets bit 7), else a copy
        bool fba = bits(s.fba, 0, 1) != 0, datm = bits(s.test, 15, 1) != 0;
        bool shadow = shadow_candidate && !b.shader_blend && pass_count == 1 && passes[0].mode == 0 &&
                      passes[0].alpha && fba && !datm && alpha_clear_ == colour->image.get() &&
                      alpha_clear_y_ == placed.y &&
                      run_.next + b.nb.triangle_count <= (1u << 24);
        if (shadow_candidate && !shadow) {
            end_shadow_run();
        }
        // (the census: the frame's alpha read before its reset)
        // (a texture read from the target, a copy or a read-back of it: where they are made)
        if (date || (bits(s.prim, 6, 1) && bits(s.alpha, 4, 2) == 1)) {
            alpha_read(colour->image.get(), placed.y);
        }
        ImageHandle dest;
        VkOffset2D dest_offset = {0, 0};
        if ((date && !shadow) || b.shader_blend) {
            VkRect2D rect = drawn_rect(s.scissor, box, *colour, placed);
            if (rect.extent.width == 0 || rect.extent.height == 0) {
                return;
            }
            dest = copy_of_rect(*colour, rect, date ? GpuWork::date_copy : GpuWork::blend_copy);
            dest_offset = rect.offset;
            if (date) {
                counts_.date_copies++;
                counts_.date_copies_native += b.native ? 1 : 0;
            }
        }
        VkViewport vp = {0, 0, float(colour->image->get_width()), float(colour->image->get_height()), 0, 1};
        int32_t sx0 = int32_t((scax0 + placed.x) * scale_[0]), sy0 = int32_t((scay0 + placed.y) * scale_[1]);
        int32_t sx1 = std::min(int32_t((scax1 + 1 + placed.x) * scale_[0]), int32_t(colour->image->get_width()));
        int32_t sy1 = std::min(int32_t((scay1 + 1 + placed.y) * scale_[1]), int32_t(colour->image->get_height()));
        if (sx1 <= sx0 || sy1 <= sy0) {
            return;
        }
        VkRect2D scissor_px = {{sx0, sy0}, {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0)}};
        // the shader's constants (pcr_common.h)
        PrimPush &push = m.push;
        push.inv_size[0] = 1.0f / float(colour->width());
        push.inv_size[1] = 1.0f / float(colour->height);
        uint32_t zpsm = z_psm(s.zbuf);
        push.zmax = zmax_of(zpsm);
        push.zscale = zpsm == 0x30 ? 1.0f / 4294967296.0f : 1.0f / 16777216.0f;
        bool tme = bits(s.prim, 4, 1) != 0;
        uint32_t flags = 0;
        if (tme && b.texture.ok) {
            flags |= F_TME | (bits(s.prim, 8, 1) ? F_FST : 0) | bits(s.tex0, 35, 2) << F_TFX_SHIFT |
                     (bits(s.tex0, 34, 1) ? F_TCC : 0) | b.texture.src << F_SRC_SHIFT |
                     (b.texture.src == SRC_SURFACE && is_colour_16(bits(s.tex0, 20, 6)) ? F_SURFACE_CT16 : 0);
        }
        flags |= bits(s.prim, 5, 1) ? F_FGE : 0;
        flags |= fba ? F_FBA : 0;
        flags |= blend.flags & ~0x80000000u;
        flags |= date && !shadow ? F_DATE | (datm ? F_DATM : 0) : 0;
        flags |= mode_2d_ == GSN_2D_SMOOTH && (scale_[0] > 1 || scale_[1] > 1) && (flags & F_TME) && !b.native &&
                         b.texture.src != SRC_SURFACE
                     ? F_SMOOTH_2D
                     : 0;
        // (the depth: Z / zmax at each pixel, clamped by the depth clamp to the viewport's 0 to 1)
        flags |= F_PIXEL_CLAMP;
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
        int32_t k = int32_t(bits(s.tex1, 32, 12) << 20) >> 20;
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
        push.clut_at = 0xFFFFFFFFu;
        push.table = 0xFFFFu;
        push.trace_xy = no_trace;
        push.plane_at = no_plane;
        m.ztest = zte && !(ztst == 1 && !zwrite);
        m.zcompare = ztst == 0   ? VK_COMPARE_OP_NEVER
                     : ztst == 1 ? VK_COMPARE_OP_ALWAYS
                     : ztst == 2 ? VK_COMPARE_OP_GREATER_OR_EQUAL
                                 : VK_COMPARE_OP_GREATER;
        m.blend = blend;
        {
            pcr::ShaderState &st = m.state;
            st.program = !b.native                                 ? 0
                         : b.nb.program == NativeProgram::vu1_280 ? 2
                         : b.nb.program == NativeProgram::vu1_700 ? 3
                                                                  : 1;
            if (tme && b.texture.ok) {
                st.texture = b.texture.src == SRC_SAMPLED ? 3 : b.texture.src == SRC_SURFACE ? 2 : 1;
                st.tfx = uint8_t(bits(s.tex0, 35, 2));
                st.alpha_channel = bits(s.tex0, 34, 1) != 0;
            }
            st.fog = bits(s.prim, 5, 1) != 0;
            st.blending = bits(s.prim, 6, 1) != 0 || b.shader_blend;
            st.blend = uint8_t(bits(s.alpha, 0, 8));
            st.shader_blend = b.shader_blend;
            st.date = date && !shadow ? 2 : 0;
            st.depth = zte ? uint8_t(ztst) : 0;
            st.depth_write = zwrite;
            st.passes = uint8_t(pass_count);
        }
        float c = float(bits(s.alpha, 32, 8)) / 128.0f;
        m.constants[0] = m.constants[1] = m.constants[2] = c;
        m.constants[3] = 128.0f / 255.0f;
        // the textures
        const BoundTexture &bt = b.texture;
        if (bt.ok) {
            if (bt.src == SRC_SAMPLED) {
                m.sampled = bt.image;
                m.sampler = bt.sampler;
            } else if (bt.src == SRC_SURFACE) {
                m.surface = bt.image;
            } else {
                m.texels = bt.image;
                if (bt.src == SRC_INDEX) {
                    m.clut = clut_index(bt.clut);
                }
            }
        }
        m.dest = dest;
        Draw d;
        d.viewport = vp;
        d.scissor = scissor_px;
        if (b.native) {
            d.native = true;
            d.native_program = b.nb.program;
            d.arena = b.nb.arena;
            d.records = b.nb.records;
            d.triangles = b.nb.triangles;
            d.first_triangle = b.nb.first_triangle;
            d.triangle_count = b.nb.triangle_count;
            d.instance = b.nb.instance;
            d.kind = b.nb.kind;
            memcpy(d.id, b.nb.id, sizeof(d.id));
        }
        // a clear drawn as a rectangle: cleared (the pass's load op where it is the pass's first and covers it)
        VkClearRect clear_rect;
        bool z_only = false;
        bool clear = !b.native && as_clear(b, placed, *colour, zte, zwrite, ztst, date, m.passes[0], passes[0].rgb,
                                           passes[0].alpha, pass_count, scissor_px, clear_rect, z_only);
        if (clear) {
            const Vertex &v0 = b.vertices[0];
            uint32_t count = 0;
            if (!z_only) {
                VkClearAttachment &a = d.clear_att[count++];
                a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                a.colorAttachment = 0;
                uint8_t alpha = fba ? uint8_t(v0.rgba[3] | 0x80u) : v0.rgba[3];
                for (int i = 0; i < 3; i++) {
                    a.clearValue.color.float32[i] = float(v0.rgba[i]) / 255.0f;
                }
                a.clearValue.color.float32[3] = float(alpha) / 255.0f;
            }
            if (zwrite) {
                VkClearAttachment &a = d.clear_att[count++];
                a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                a.clearValue.depthStencil.depth = depth_of(v0.z, zpsm);
            }
            d.clear = true;
            d.clear_rect = clear_rect;
            d.clear_count = count;
            bool alpha_high = !z_only && ((fba ? v0.rgba[3] | 0x80u : v0.rgba[3]) & 0x80u) != 0;
            add_clear(*colour, zwrite ? depth : nullptr, std::move(d), std::move(m));
            if (!z_only && is_frame_clear(clear_rect.rect, scissor_px, *colour, placed, scax0, scax1, scay0, scay1)) {
                // (the frame's clear: the census watches its alpha until its reset)
                watch_alpha(colour->image.get(), placed.y);
            }
            if (alpha_high && colour->image.get() == alpha_clear_) {
                alpha_clear_ = nullptr;
            }
            drawn_over(b, box, zwrite);
            return;
        }
        // A native batch with the last draw's material, in its pass: that material again (the drawer sets nothing new
        // for it), and where its triangles go on from the last's in the same record, the last draw made longer (as the
        // canvas's batch of runs: pcr_canvas.cpp's native_batch). The shadows' numbers go on as their triangles do.
        Draw *last = b.native ? joinable_draw(*colour, depth, shadow, d) : nullptr;
        if (last && same_material(scene_.materials[last->material], m)) {
            d.material = last->material;
            const Material &mm = scene_.materials[d.material];
            bool longer = last->instance == d.instance &&
                          last->first_triangle + last->triangle_count == d.first_triangle &&
                          (!shadow || last->shadow + last->triangle_count == run_.next);
            counts_.native_joined += longer ? 1 : 0;
            counts_.native_same_material += longer ? 0 : 1;
            if (longer) {
                last->triangle_count += d.triangle_count;
                if (shadow) {
                    run_.next += d.triangle_count;
                }
                last_clear_ = {};
            } else if (shadow) {
                add_shadow(*colour, depth, std::move(d));
            } else {
                last_clear_ = {};
                add_draw(std::move(d));
            }
            if (!shadow && colour->image.get() == alpha_clear_) {
                for (uint32_t i = 0; i < mm.pass_count; i++) {
                    if (mm.passes[i].mask & 0x8) {
                        alpha_clear_ = nullptr;
                    }
                }
            }
            drawn_over(b, box, zte && zwrite);
            return;
        }
        uint32_t material = uint32_t(scene_.materials.size());
        scene_.materials.push_back(std::move(m));
        d.material = material;
        const Material &mm = scene_.materials.back();
        if (!b.native) {
            d.first_vertex = uint32_t(scene_.vertices.size());
            d.vertex_count = uint32_t(b.vertices.size());
            scene_.vertices.insert(scene_.vertices.end(), b.vertices.begin(), b.vertices.end());
        }
        if (shadow) {
            add_shadow(*colour, depth, std::move(d));
        } else {
            last_clear_ = {};
            use_pass(colour->image, depth ? depth->image : ImageHandle(), PassKind::plain);
            add_draw(std::move(d));
            bool writes_alpha = false;
            for (uint32_t i = 0; i < pass_count; i++) {
                writes_alpha |= (mm.passes[i].mask & 0x8) != 0;
            }
            if (is_alpha_reset(b, *colour, placed, pass_count, mm.passes[0])) {
                alpha_reset(colour->image.get(), placed.y);
            } else if (writes_alpha && colour->image.get() == alpha_clear_) {
                alpha_clear_ = nullptr;
            }
        }
        drawn_over(b, box, zte && zwrite);
    }

    // The open pass's last draw where a native batch into colour and depth (a shadow or not) may follow it with its
    // material: a native draw of the same program, buffers, viewport and scissor, last in the pass the batch goes into
    Draw *joinable_draw(const Target &colour, const Target *depth, bool shadow, const Draw &d) {
        if (!pass_.open || scene_.draws.empty()) {
            return nullptr;
        }
        const Pass &p = scene_.passes[pass_.index];
        const Image *depth_image = depth ? depth->image.get() : nullptr;
        if (p.draw_count == 0 || p.first_draw + p.draw_count != scene_.draws.size() || pass_.depth != depth_image) {
            return nullptr;
        }
        if (shadow ? pass_.kind != PassKind::first_draw || run_.draws.empty() ||
                         run_.colour.get() != colour.image.get() || run_.draws.back() != scene_.draws.size() - 1
                   : pass_.kind != PassKind::plain || pass_.colour != colour.image.get()) {
            return nullptr;
        }
        Draw &last = scene_.draws.back();
        if (!last.native || last.clear || last.native_program != d.native_program ||
            last.arena.get() != d.arena.get() || last.records.get() != d.records.get() ||
            last.triangles.get() != d.triangles.get() ||
            memcmp(&last.viewport, &d.viewport, sizeof(d.viewport)) != 0 ||
            memcmp(&last.scissor, &d.scissor, sizeof(d.scissor)) != 0 || (last.shadow != no_shadow) != shadow) {
            return nullptr;
        }
        return &last;
    }

    static bool same_material(const Material &a, const Material &b) {
        if (a.program != b.program || a.pass_count != b.pass_count || a.ztest != b.ztest || a.zcompare != b.zcompare ||
            a.texels.get() != b.texels.get() || a.surface.get() != b.surface.get() ||
            a.sampled.get() != b.sampled.get() || a.dest.get() != b.dest.get() || a.sampler != b.sampler ||
            a.clut != b.clut || memcmp(&a.push, &b.push, sizeof(a.push)) != 0 ||
            memcmp(a.constants, b.constants, sizeof(a.constants)) != 0) {
            return false;
        }
        const pcr::BlendChoice &x = a.blend, &y = b.blend;
        if (x.enable != y.enable || x.src != y.src || x.dst != y.dst || x.src_alpha != y.src_alpha || x.op != y.op ||
            x.flags != y.flags || x.write_rgb != y.write_rgb) {
            return false;
        }
        for (uint32_t i = 0; i < a.pass_count; i++) {
            if (a.passes[i].mode != b.passes[i].mode || a.passes[i].mask != b.passes[i].mask ||
                a.passes[i].zwrite != b.passes[i].zwrite) {
                return false;
            }
        }
        return true;
    }

    // A clear: the pass's load op where it is the pass's first and covers the whole of what it clears, else a clear
    // in the pass (vkCmdClearAttachments); left out where it writes what the clear before it wrote, nothing between
    // (pcr_canvas.cpp's same_as_last_clear: the game clears the frame twice, the second time with its other context's
    // registers)
    void add_clear(Target &colour, Target *depth, Draw &&d, Material &&m) {
        ImageHandle depth_image = depth ? depth->image : ImageHandle();
        LastClear now = {colour.image.get(), depth_image.get(), d.clear_rect.rect, {d.clear_att[0], d.clear_att[1]},
                         d.clear_count, true};
        if (last_clear_.valid && pass_.open && last_clear_.same(now)) {
            return;
        }
        bool fresh = !(pass_.open && pass_.colour == colour.image.get() && (!depth || pass_.depth == depth_image.get()) &&
                       pass_.kind == PassKind::plain);
        Pass &pass = use_pass(colour.image, depth_image, PassKind::plain);
        last_clear_ = now;
        const VkRect2D &r = d.clear_rect.rect;
        bool covers_colour = r.offset.x == 0 && r.offset.y == 0 && r.extent.width == colour.image->get_width() &&
                             r.extent.height == colour.image->get_height();
        bool covers_depth = !depth || (depth->image->get_width() == colour.image->get_width() &&
                                       depth->image->get_height() == colour.image->get_height());
        if (fresh && pass.draw_count == 0 && covers_colour && covers_depth) {
            for (uint32_t i = 0; i < d.clear_count; i++) {
                const VkClearAttachment &a = d.clear_att[i];
                if (a.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
                    pass.clear_colour = true;
                    pass.colour_value = a.clearValue.color;
                } else {
                    pass.clear_depth = true;
                    pass.depth_value = a.clearValue.depthStencil.depth;
                }
            }
            counts_.load_clears++;
            return;
        }
        d.material = uint32_t(scene_.materials.size());
        scene_.materials.push_back(std::move(m));
        add_draw(std::move(d));
    }

    struct LastClear {
        const Image *colour = nullptr, *depth = nullptr;
        VkRect2D rect{};
        VkClearAttachment att[2]{};
        uint32_t count = 0;
        bool valid = false;
        bool same(const LastClear &o) const {
            if (colour != o.colour || depth != o.depth || count != o.count || memcmp(&rect, &o.rect, sizeof(rect)) != 0) {
                return false;
            }
            for (uint32_t i = 0; i < count; i++) {
                if (att[i].aspectMask != o.att[i].aspectMask ||
                    memcmp(&att[i].clearValue, &o.att[i].clearValue, sizeof(VkClearValue)) != 0) {
                    return false;
                }
            }
            return true;
        }
    } last_clear_;

    // A shadow into the open run: the first-draw pass (its image cleared by the run's first pass since the reset)
    void add_shadow(Target &colour, Target *depth, Draw &&d) {
        last_clear_ = {};
        ImageHandle depth_image = depth ? depth->image : ImageHandle();
        if (!run_.draws.empty() && (run_.colour.get() != colour.image.get() || run_.depth.get() != depth_image.get())) {
            end_shadow_run();
        }
        if (!first_image_ || first_image_->get_width() != colour.image->get_width() ||
            first_image_->get_height() != colour.image->get_height()) {
            ImageCreateInfo info = ImageCreateInfo::render_target(colour.image->get_width(), colour.image->get_height(),
                                                                  Drawer::first_format);
            info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            info.layout = ImageLayout::General;
            info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
            first_image_ = device_.create_image(info, nullptr);
            run_.cleared = false;
        }
        run_.colour = colour.image;
        run_.depth = depth_image;
        Pass &pass = use_pass(first_image_, depth_image, PassKind::first_draw);
        if (!run_.cleared) {
            if (pass.draw_count != 0) {
                pass_.open = false; // (a pass of its own that clears it)
            }
            Pass &fresh = use_pass(first_image_, depth_image, PassKind::first_draw);
            fresh.clear_colour = true;
            fresh.colour_value.float32[0] = first_none;
            run_.cleared = true;
        }
        d.shadow = run_.next;
        run_.next += d.triangle_count;
        run_.draws.push_back(uint32_t(scene_.draws.size()));
        add_draw(std::move(d));
    }

    void alpha_reset(const Image *image, uint32_t y) {
        end_shadow_run();
        counts_.alpha_resets++;
        alpha_clear_ = image;
        alpha_clear_y_ = y;
        run_.next = 0;
        run_.cleared = false;
        auto w = std::find_if(alpha_watches_.begin(), alpha_watches_.end(),
                              [&](const AlphaWatch &x) { return x.image == image && x.y == y; });
        if (w != alpha_watches_.end()) {
            counts_.alpha_checked_frames++;
            counts_.alpha_read_frames += w->read ? 1 : 0;
            if (w->read && counts_.alpha_read_frames <= 5) {
                fprintf(stderr, "[scn] the frame's alpha read between its clear and its reset (vblank %llu)\n",
                        (unsigned long long)vblank_);
            }
            alpha_watches_.erase(w);
        }
    }

    // (each target watched on its own: a reflection's clear between the frame's clear and its reset is not the frame's)
    void watch_alpha(const Image *image, uint32_t y) {
        for (AlphaWatch &w : alpha_watches_) {
            if (w.image == image && w.y == y) {
                w.read = false;
                return;
            }
        }
        if (alpha_watches_.size() == 8) {
            alpha_watches_.erase(alpha_watches_.begin()); // (a target let go of, or grown into another image)
        }
        alpha_watches_.push_back({image, y, false});
    }

    void forget_alpha(const Image *image) {
        alpha_watches_.erase(std::remove_if(alpha_watches_.begin(), alpha_watches_.end(),
                                            [&](const AlphaWatch &w) { return w.image == image; }),
                             alpha_watches_.end());
    }

    // (a texture, copy or read-back of the target: every frame in it counted as read)
    void alpha_read(const Image *image) {
        for (AlphaWatch &w : alpha_watches_) {
            if (w.image == image) {
                w.read = true;
            }
        }
    }

    void alpha_read(const Image *image, uint32_t y) {
        for (AlphaWatch &w : alpha_watches_) {
            if (w.image == image && w.y == y) {
                w.read = true;
            }
        }
    }

    // The scene's CLUTs: the last one again where it is the same
    uint32_t clut_index(const std::array<uint32_t, 256> &clut) {
        if (clut_at_ != no_clut && clut_at_ < scene_.cluts.size() && scene_.cluts[clut_at_] == clut) {
            return clut_at_;
        }
        clut_at_ = uint32_t(scene_.cluts.size());
        scene_.cluts.push_back(clut);
        return clut_at_;
    }

    static TextureStoreLimits store_limits() {
        TextureStoreLimits limits;
        const char *kept = getenv("KF4_PC_RESOLVE_KEPT");
        if (kept && strcmp(kept, "0") == 0) {
            limits.answers_kept = 0;
        }
        return limits;
    }

    void scanout_lower_field(CommandBuffer &out, const Image &src, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                             const Image &dst) {
        int32_t height = int32_t(dst.get_height());
        int32_t down_rows = std::max<int32_t>(1, int32_t(scale_[1] + 0.5f));
        int32_t src_rows = (height - down_rows) / 2;
        VkClearValue black = {};
        out.clear_image(dst, black);
        out.barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (src_rows <= 0 || y0 + src_rows > y1) {
            return;
        }
        out.blit_image(dst, src, {0, down_rows, 0}, {int32_t(dst.get_width()), 2 * src_rows, 1}, {x0, y0, 0},
                       {x1 - x0, src_rows, 1}, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
    }

    Device &device_;
    GpuTimes &times_;
    HangNotes &notes_;
    Drawer drawer_;
    TextureStore store_;
    Scene scene_;
    bool d16_ = false, first_draw_on_ = false;
    std::array<uint32_t, 2> scale_ = {1, 1}, pending_scale_ = {1, 1};
    int lod_mode_ = 0, mode_2d_ = 0;
    uint32_t aniso_ = 1;
    uint64_t frame_ = 0, vblank_ = 0;
    uint64_t clut_serial_ = 0;
    uint64_t store_events_ = 0; // (anything that may change what the store answers: uploads, copies, CLUT loads; draws invalidate the last native batch's texture by its pages)
    std::vector<std::unique_ptr<Target>> colour_, depth_;
    std::unordered_map<uint64_t, GpuTexture> textures_;
    std::unordered_map<uint64_t, SampledTexture> sampled_;
    std::unordered_map<uint32_t, SamplerHandle> samplers_;
    Batch batch_;
    bool batch_open_ = false;
    struct LastNative {
        bool valid = false;
        DrawState state{};
        uint64_t events = 0;
        BoundTexture texture;
        std::bitset<512> pages; // (what its texture reads: every level's)
    } last_native_;

    // The pages a draw's texture may read: each level's (TEX0's, MIPTBP1's and MIPTBP2's), as the store reads them
    static std::bitset<512> texture_pages(const DrawState &s) {
        std::bitset<512> pages;
        uint32_t psm = bits(s.tex0, 20, 6), tw = bits(s.tex0, 26, 4), th = bits(s.tex0, 30, 4);
        uint32_t levels = std::min(bits(s.tex1, 2, 3), 6u) + 1;
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t tbp, tbw;
            if (l == 0) {
                tbp = bits(s.tex0, 0, 14);
                tbw = bits(s.tex0, 14, 6);
            } else {
                uint64_t m = l <= 3 ? s.miptbp1 : s.miptbp2;
                uint32_t k = (l - 1) % 3;
                tbp = bits(m, int(20 * k), 14);
                tbw = bits(m, int(20 * k + 14), 6);
            }
            uint32_t w = std::max(1u, (1u << std::min(tw, 10u)) >> l), h = std::max(1u, (1u << std::min(th, 10u)) >> l);
            pages |= pages_of({tbp, std::max(1u, tbw), psm, 0, 0, w, h});
        }
        return pages;
    }
    uint32_t clut_at_ = no_clut;
    std::vector<uint8_t> fifo_;
    size_t fifo_at_ = 0;
    // DATE: the colour target whose alpha is known clear (GsDraw_SetScreenAlpha(0)) but for the shadows'; the census's
    // targets watched from their clears to their resets, and whether their alpha was read meanwhile
    // (each frame by its image and its row in it: both frame buffers can be one target)
    const Image *alpha_clear_ = nullptr;
    uint32_t alpha_clear_y_ = 0;
    struct AlphaWatch {
        const Image *image;
        uint32_t y;
        bool read;
    };
    std::vector<AlphaWatch> alpha_watches_;
    const bool sampler_on_ = !getenv("KF4_PC_SAMPLER") || strcmp(getenv("KF4_PC_SAMPLER"), "0") != 0;
    const bool unblend_on_ = !(getenv("KF4_PC_UNBLEND") && strcmp(getenv("KF4_PC_UNBLEND"), "0") == 0);
    struct Counts {
        uint64_t scenes = 0, primitives = 0, native_batches = 0, load_clears = 0;
        uint64_t native_joined = 0, native_same_material = 0, target_copies_reused = 0;
        uint64_t date_copies = 0, date_copies_native = 0, alpha_resets = 0, alpha_read_frames = 0,
                 alpha_checked_frames = 0;
        uint64_t targets_made = 0, targets_grown = 0, filled_from_uploads = 0, uploads_into_targets = 0,
                 copies_between_targets = 0, depth_aliases_cleared = 0;
        uint64_t textures_uploaded = 0, textures_sampled = 0, textures_from_targets = 0, textures_missing = 0,
                 textures_reused = 0;
        uint64_t shader_blends = 0, read_backs = 0;
        uint64_t unsupported_shapes = 0, unsupported_blends = 0, unsupported_texture_on_target = 0,
                 unsupported_copies = 0, unsupported_uploads = 0, unsupported_read_backs = 0, uploads_again_passed = 0;
    } counts_;
};

std::unique_ptr<Drawing> make_builder(Device &device, GpuTimes &times, HangNotes &notes) {
    return std::make_unique<Builder>(device, times, notes);
}

} // namespace scn
