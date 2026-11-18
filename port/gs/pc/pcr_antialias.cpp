// The picture's anti-aliasing (pcr_antialias.h)
#include "pcr_antialias.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include "Textures/AreaTex.h"   // (third_party/smaa: SMAA's lookup textures)
#include "Textures/SearchTex.h"
#include "command_buffer.hpp"
#include "pcr_shaders.h"

using namespace Vulkan;

namespace pcr {

namespace {

// image kept at width x height in format (made again when the size changes; General layout, as the picture is), for
// what (a log line when it cannot be made); false when it cannot be
bool keep_image(Device &device, ImageHandle &image, uint32_t width, uint32_t height, VkFormat format,
                VkImageUsageFlags usage, const char *what) {
    if (image && image->get_width() == width && image->get_height() == height) {
        return true;
    }
    ImageCreateInfo info = ImageCreateInfo::render_target(width, height, format);
    info.usage = usage;
    info.layout = ImageLayout::General;
    info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
    image = device.create_image(info, nullptr);
    if (!image) {
        fprintf(stderr, "[pcr] no image for %s of a %ux%u picture: drawn without it\n", what, width, height);
    }
    return bool(image);
}

// picture copied into copy (the same size), after whatever wrote the picture and before whatever reads the copy
void copy_picture(CommandBuffer &cmd, const Image &picture, const Image &copy) {
    const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    cmd.copy_image(copy, picture, {0, 0, 0}, {0, 0, 0}, {picture.get_width(), picture.get_height(), 1}, colour, colour);
    cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

// A render pass over the whole of target with program, its textures bound by the caller between begin and draw
void begin_over(CommandBuffer &cmd, const Image &target, Program *program, bool cleared) {
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &target.get_view();
    rp.store_attachments = 1;
    if (cleared) {
        rp.clear_attachments = 1;
        rp.clear_color[0] = {};
    }
    cmd.begin_render_pass(rp);
    cmd.set_opaque_state();
    cmd.set_program(program);
    cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    cmd.set_cull_mode(VK_CULL_MODE_NONE);
    cmd.set_depth_test(false, false);
}

// The draw begun by begin_over (pcr_present.vert's triangle), its pass ended, and its writes made visible to what
// reads them next
void draw_over(CommandBuffer &cmd) {
    cmd.draw(3);
    cmd.end_render_pass();
    cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

// A program of pcr_present.vert and frag, which reads textures sampled float images (sets[0], bindings 0 up) and takes
// push_bytes of push constants
Program *over_program(Device &device, const uint32_t *frag, size_t frag_bytes, unsigned textures,
                      uint32_t push_bytes) {
    ResourceLayout vert = {};
    Shader *vs = device.request_shader(pcr_spv_pcr_present_vert, sizeof(pcr_spv_pcr_present_vert), &vert);
    ResourceLayout layout = {};
    layout.output_mask = 1;
    layout.sets[0].sampled_image_mask = (1u << textures) - 1;
    layout.sets[0].fp_mask = (1u << textures) - 1;
    for (unsigned t = 0; t < textures; t++) {
        layout.sets[0].meta[t].array_size = 1;
    }
    layout.push_constant_size = push_bytes;
    return device.request_program(vs, device.request_shader(frag, frag_bytes, &layout));
}

const VkImageUsageFlags TARGET_USAGE = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
const VkImageUsageFlags COPY_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

// FXAA (shaders/pcr_fxaa.frag): the picture copied, then drawn over whole from the copy
class Fxaa final : public AntiAliasMethod {
public:
    explicit Fxaa(Device &device)
        : device_(device), program_(over_program(device, pcr_spv_pcr_fxaa_frag, sizeof(pcr_spv_pcr_fxaa_frag), 1,
                                                 2 * sizeof(float))) {}

    bool apply(CommandBuffer &cmd, const AntiAliasFrame &frame) override {
        const Image &picture = frame.picture;
        uint32_t w = picture.get_width(), h = picture.get_height();
        if (!keep_image(device_, copy_, w, h, VK_FORMAT_R8G8B8A8_UNORM, COPY_USAGE, "FXAA's copy")) {
            return false;
        }
        copy_picture(cmd, picture, *copy_);
        begin_over(cmd, picture, program_, false);
        cmd.set_texture(0, 0, copy_->get_view(), StockSampler::LinearClamp);
        const float texel[2] = {1.0f / float(w), 1.0f / float(h)};
        cmd.push_constants(texel, 0, sizeof(texel));
        draw_over(cmd);
        return true;
    }

private:
    Device &device_;
    Program *program_;
    ImageHandle copy_; // (the picture's copy, kept while its size stays)
};

// SMAA's three passes (shaders/pcr_smaa.h, the reference implementation at its HIGH preset, luma edges), for SMAA 1x
// and T2x: the picture copied (only when the blend writes the picture itself: SMAA 1x; T2x blends into its history,
// so its passes read the picture as it is); its edges into edges_; their blending weights into weights_; the blend
// from the picture (or its copy) and the weights into a target. (SMAA's stencil, which runs the weights pass only where there are edges, is left out:
// SMAA 1x cost 0.17 ms a picture at 4x, 2026-11-16.)
class SmaaPasses {
public:
    explicit SmaaPasses(Device &device)
        : device_(device),
          edges_program_(over_program(device, pcr_spv_pcr_smaa_edges_frag, sizeof(pcr_spv_pcr_smaa_edges_frag), 1,
                                      sizeof(Push))),
          weights_program_(over_program(device, pcr_spv_pcr_smaa_weights_frag,
                                        sizeof(pcr_spv_pcr_smaa_weights_frag), 3, sizeof(Push))),
          blend_program_(over_program(device, pcr_spv_pcr_smaa_blend_frag, sizeof(pcr_spv_pcr_smaa_blend_frag), 2,
                                      sizeof(Push))),
          blend_moved_program_(over_program(device, pcr_spv_pcr_smaa_blend_t2x_frag,
                                            sizeof(pcr_spv_pcr_smaa_blend_t2x_frag), 3, sizeof(Push))),
          area_(lookup(device, AREATEX_WIDTH, AREATEX_HEIGHT, VK_FORMAT_R8G8_UNORM, areaTexBytes, "AreaTex")),
          search_(lookup(device, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_FORMAT_R8_UNORM, searchTexBytes,
                         "SearchTex")) {}

    // picture's SMAA into target (the picture itself, or another image its size), with subsample as the weights'
    // subsampleIndices (0 for SMAA 1x); with motion (not null), the motion blended too and its length in target's
    // alpha (pcr_smaa_blend_t2x.frag). False, nothing recorded, where an image cannot be made.
    bool run(CommandBuffer &cmd, const Image &picture, const std::array<float, 4> &subsample, const Image &target,
             const Image *motion) {
        uint32_t w = picture.get_width(), h = picture.get_height();
        // (the blend into another image than the picture: no copy, the passes read the picture. 0.3 ms a picture at
        // 2x on a Vega 7, 2026-11-18)
        const bool copied = &target == &picture;
        if (!area_ || !search_ ||
            (copied && !keep_image(device_, copy_, w, h, VK_FORMAT_R8G8B8A8_UNORM, COPY_USAGE, "SMAA's copy")) ||
            !keep_image(device_, edges_, w, h, VK_FORMAT_R8G8_UNORM, TARGET_USAGE, "SMAA's edges") ||
            !keep_image(device_, weights_, w, h, VK_FORMAT_R8G8B8A8_UNORM, TARGET_USAGE, "SMAA's weights")) {
            return false;
        }
        const Push push = {{1.0f / float(w), 1.0f / float(h), float(w), float(h)},
                           {subsample[0], subsample[1], subsample[2], subsample[3]}};
        const Image &source = copied ? *copy_ : picture;
        if (copied) {
            copy_picture(cmd, picture, *copy_);
        } else {
            // (after whatever wrote the picture: the scanout, the depth's copy)
            cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        }

        begin_over(cmd, *edges_, edges_program_, true);
        cmd.set_texture(0, 0, source.get_view(), StockSampler::LinearClamp);
        cmd.push_constants(&push, 0, sizeof(push));
        draw_over(cmd);

        // (not cleared: the weights pass writes every pixel, 0 where there is no edge, where the edges pass discards)
        begin_over(cmd, *weights_, weights_program_, false);
        cmd.set_texture(0, 0, edges_->get_view(), StockSampler::LinearClamp);
        cmd.set_texture(0, 1, area_->get_view(), StockSampler::LinearClamp);
        cmd.set_texture(0, 2, search_->get_view(), StockSampler::NearestClamp);
        cmd.push_constants(&push, 0, sizeof(push));
        draw_over(cmd);

        begin_over(cmd, target, motion ? blend_moved_program_ : blend_program_, false);
        cmd.set_texture(0, 0, source.get_view(), StockSampler::LinearClamp);
        cmd.set_texture(0, 1, weights_->get_view(), StockSampler::LinearClamp);
        if (motion) {
            cmd.set_texture(0, 2, motion->get_view(), StockSampler::LinearClamp);
        }
        cmd.push_constants(&push, 0, sizeof(push));
        draw_over(cmd);
        return true;
    }

private:
    struct Push { // (pcr_smaa.h's SmaaPush)
        float metrics[4];
        float subsample[4];
    };

    // One of SMAA's lookup textures (its header's bytes, rows of width texels of format), or null when it cannot be
    // made
    static ImageHandle lookup(Device &device, uint32_t width, uint32_t height, VkFormat format, const void *bytes,
                              const char *what) {
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(width, height, format);
        info.levels = 1;
        ImageInitialData data = {bytes, 0, 0};
        ImageHandle image = device.create_image(info, &data);
        if (!image) {
            fprintf(stderr, "[pcr] no image for SMAA's %s: pictures drawn without SMAA\n", what);
        }
        return image;
    }

    Device &device_;
    Program *edges_program_, *weights_program_, *blend_program_, *blend_moved_program_;
    ImageHandle area_, search_;          // (the lookup textures, made once)
    ImageHandle copy_, edges_, weights_; // (kept while the picture's size stays)
};

// SMAA 1x: SmaaPasses over the picture, in place
class Smaa final : public AntiAliasMethod {
public:
    explicit Smaa(Device &device) : passes_(device) {}

    bool apply(CommandBuffer &cmd, const AntiAliasFrame &frame) override {
        return passes_.run(cmd, frame.picture, {0.0f, 0.0f, 0.0f, 0.0f}, frame.picture, nullptr);
    }

private:
    SmaaPasses passes_;
};

// SMAA T2x: the 3D drawn with T2x's two jitters in turn; each picture's SMAA (its subsample indices the jitter's, the
// motion blended with it) kept as the history, and the picture the resolve of it and the previous one's
// (shaders/pcr_smaa_resolve.frag). With no history or no motion (a reset), the picture's SMAA alone.
class SmaaT2x final : public AntiAliasMethod {
public:
    explicit SmaaT2x(Device &device)
        : passes_(device), resolve_program_(over_program(device, pcr_spv_pcr_smaa_resolve_frag,
                                                         sizeof(pcr_spv_pcr_smaa_resolve_frag), 3, sizeof(Push))) {}

    bool apply(CommandBuffer &cmd, const AntiAliasFrame &frame) override {
        if (!frame.keep_into) {
            return passes_.run(cmd, frame.picture, {0.0f, 0.0f, 0.0f, 0.0f}, frame.picture, nullptr);
        }
        // (SMAA.hlsl's @SUBSAMPLE_INDICES: pcr_jitter.h's smaa_t2x has its jitters in its order)
        float s = frame.jitter_index % 2 == 0 ? 1.0f : 2.0f;
        if (!passes_.run(cmd, frame.picture, {s, s, s, 0.0f}, *frame.keep_into, frame.motion)) {
            return false;
        }
        bool has_then = frame.previous && frame.motion;
        begin_over(cmd, frame.picture, resolve_program_, false);
        cmd.set_texture(0, 0, frame.keep_into->get_view(), StockSampler::NearestClamp);
        cmd.set_texture(0, 1, (has_then ? frame.previous : frame.keep_into)->get_view(), StockSampler::LinearClamp);
        cmd.set_texture(0, 2, (frame.motion ? frame.motion : frame.keep_into)->get_view(), StockSampler::NearestClamp);
        const Push push = {{1.0f / float(frame.picture.get_width()), 1.0f / float(frame.picture.get_height())},
                           has_then ? 1u : 0u,
                           WEIGHT_SCALE};
        cmd.push_constants(&push, 0, sizeof(push));
        draw_over(cmd);
        return true;
    }

    JitterSequence jitter() const override {
        return JitterSequence::smaa_t2x;
    }

    bool keeps_own_history() const override {
        return true;
    }

private:
    struct Push { // (shaders/pcr_smaa_resolve.frag's)
        float texel[2];
        uint32_t has_then;
        float weight_scale;
    };
    static constexpr float WEIGHT_SCALE = 30.0f; // (SMAA_REPROJECTION_WEIGHT_SCALE's default)

    SmaaPasses passes_;
    Program *resolve_program_;
};

// KF4_AA_DEBUG: a debug view of the temporal inputs over every picture (shaders/pcr_aa_debug.frag's numbers), 0 none
uint32_t debug_view() {
    static const uint32_t view = [] {
        const char *e = getenv("KF4_AA_DEBUG");
        if (!e) {
            return 0u;
        }
        static const char *const names[] = {"depth", "motion", "reproject", "jitter"};
        for (uint32_t i = 0; i < 4; i++) {
            if (strcmp(e, names[i]) == 0) {
                return i + 1;
            }
        }
        fprintf(stderr, "[pcr] KF4_AA_DEBUG wants depth, motion, reproject or jitter, not %s: none shown\n", e);
        return 0u;
    }();
    return view;
}

// KF4_AA_JITTER=t2x|halton8: the 3D drawn with that jitter whatever the method (a developer's check of the jitter)
JitterSequence jitter_asked() {
    static const JitterSequence asked = [] {
        const char *e = getenv("KF4_AA_JITTER");
        if (e && strcmp(e, "t2x") == 0) {
            return JitterSequence::smaa_t2x;
        }
        if (e && strcmp(e, "halton8") == 0) {
            return JitterSequence::halton8;
        }
        return JitterSequence::none;
    }();
    return asked;
}

std::atomic<bool> cut_asked{false}; // (pcr_antialias_cut)

// KF4_AA_TRACE=1: a line a picture of what the temporal inputs had (a developer's)
bool aa_trace() {
    static const bool on = getenv("KF4_AA_TRACE") != nullptr;
    return on;
}

} // namespace

// ---- The history ----
AntiAliasHistory::Drawn AntiAliasHistory::next_drawing(JitterSequence sequence) {
    std::lock_guard<std::mutex> hold(lock_);
    uint32_t index = drawings_++ % jitter_length(sequence);
    return {jitter_at(sequence, index), index, ++numbered_};
}

AntiAliasHistory::Drawn AntiAliasHistory::drawing_in_step(JitterSequence sequence) {
    std::lock_guard<std::mutex> hold(lock_);
    uint32_t index = drawings_ % jitter_length(sequence);
    return {jitter_at(sequence, index), index, ++numbered_};
}

void AntiAliasHistory::drawing_not_made() {
    std::lock_guard<std::mutex> hold(lock_);
    drawings_--;
}

void AntiAliasHistory::cut() {
    std::lock_guard<std::mutex> hold(lock_);
    cut_ = true;
}

bool AntiAliasHistory::wanted() const {
    std::lock_guard<std::mutex> hold(lock_);
    return wanted_;
}

void AntiAliasHistory::set_wanted(bool wanted) {
    std::lock_guard<std::mutex> hold(lock_);
    if (wanted != wanted_) {
        cut_ = true;
    }
    wanted_ = wanted;
}

// ---- The temporal inputs a thread makes: the depth's image, the motion from the cameras, the debug views ----
class AntiAliasing::Inputs {
public:
    explicit Inputs(Device &device)
        : device_(device),
          motion_program_(motion_program(device)),
          debug_program_(over_program(device, pcr_spv_pcr_aa_debug_frag, sizeof(pcr_spv_pcr_aa_debug_frag), 4,
                                      sizeof(DebugPush))) {}

    const Image *depth(uint32_t width, uint32_t height) {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return keep_image(device_, depth_, width, height, VK_FORMAT_R32_SFLOAT, usage, "the depth") ? depth_.get()
                                                                                                     : nullptr;
    }

    // The motion from now's camera to then's, from the depth (made by depth(), holding now's), into the motion image;
    // null when it cannot be made
    const Image *motion(CommandBuffer &cmd, const PictureFacts &now, const PicturePlace &now_at,
                        const PictureFacts &then, const PicturePlace &then_at) {
        uint32_t w = depth_->get_width(), h = depth_->get_height();
        if (!keep_image(device_, motion_, w, h, VK_FORMAT_R16G16_SFLOAT, TARGET_USAGE, "the motion")) {
            return nullptr;
        }
        begin_over(cmd, *motion_, motion_program_, false);
        cmd.set_texture(0, 0, depth_->get_view(), StockSampler::NearestClamp);
        Cameras *u = cmd.allocate_typed_constant_data<Cameras>(0, 1, 1);
        *u = {};
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                u->now_view[r][c] = now.camera.world_to_view[size_t(r * 4 + c)];
                u->then_view[r][c] = then.camera.world_to_view[size_t(r * 4 + c)];
            }
        }
        const auto &ns = now.camera.screen, &ts = then.camera.screen;
        set4(u->now_screen, ns[0], ns[1], ns[2], ns[3]);
        set4(u->then_screen, ts[0], ts[1], ts[2], ts[3]);
        set4(u->now_z, ns[4], ns[5], now_at.zscale, 0.0f);
        set4(u->then_z, ts[4], ts[5], 0.0f, 0.0f);
        set4(u->now_place, now_at.origin[0], now_at.origin[1], now_at.scale[0], now_at.scale[1]);
        set4(u->then_place, then_at.origin[0], then_at.origin[1], then_at.scale[0], then_at.scale[1]);
        draw_over(cmd);
        return motion_.get();
    }

    // view (debug_view's) drawn over picture, from now (the picture as made), then (the previous one's, or null), and
    // depth and motion (null: none)
    void debug(CommandBuffer &cmd, const Image &picture, uint32_t view, const Image &now, const Image *then,
               const Image *depth, float zscale, const Image *motion) {
        begin_over(cmd, picture, debug_program_, false);
        cmd.set_texture(0, 0, now.get_view(), StockSampler::NearestClamp);
        cmd.set_texture(0, 1, (then ? then : &now)->get_view(), StockSampler::LinearClamp);
        cmd.set_texture(0, 2, (depth ? depth : &now)->get_view(), StockSampler::NearestClamp);
        cmd.set_texture(0, 3, (motion ? motion : &now)->get_view(), StockSampler::NearestClamp);
        DebugPush push = {{1.0f / float(picture.get_width()), 1.0f / float(picture.get_height())},
                          zscale,
                          view,
                          (then ? 1u : 0u) | (depth ? 2u : 0u) | (motion ? 4u : 0u)};
        cmd.push_constants(&push, 0, sizeof(push));
        if (aa_trace()) {
            fprintf(stderr, "[aa] debug view %u, has %u\n", push.view, push.has);
        }
        draw_over(cmd);
    }

private:
    struct Cameras { // (shaders/pcr_aa_motion.frag's, std140)
        float now_view[3][4], then_view[3][4];
        float now_screen[4], then_screen[4], now_z[4], then_z[4], now_place[4], then_place[4];
    };
    struct DebugPush { // (shaders/pcr_aa_debug.frag's)
        float texel[2];
        float zscale;
        uint32_t view, has;
    };

    static void set4(float (&v)[4], float a, float b, float c, float d) {
        v[0] = a;
        v[1] = b;
        v[2] = c;
        v[3] = d;
    }

    static Program *motion_program(Device &device) {
        ResourceLayout vert = {};
        Shader *vs = device.request_shader(pcr_spv_pcr_present_vert, sizeof(pcr_spv_pcr_present_vert), &vert);
        ResourceLayout layout = {};
        layout.output_mask = 1;
        layout.sets[0].sampled_image_mask = 1;
        layout.sets[0].fp_mask = 1;
        layout.sets[0].uniform_buffer_mask = 2;
        layout.sets[0].meta[0].array_size = 1;
        layout.sets[0].meta[1].array_size = 1;
        return device.request_program(
            vs, device.request_shader(pcr_spv_pcr_aa_motion_frag, sizeof(pcr_spv_pcr_aa_motion_frag), &layout));
    }

    Device &device_;
    Program *motion_program_, *debug_program_;
    ImageHandle depth_, motion_; // (kept while the picture's size stays)
};

// ---- A thread's anti-aliasing ----
AntiAliasing::AntiAliasing(Device &device, AntiAliasHistory &history)
    : device_(device), history_(history), inputs_(std::make_unique<Inputs>(device)) {}
AntiAliasing::~AntiAliasing() = default;

const Image *AntiAliasing::depth_image(uint32_t width, uint32_t height) {
    return inputs_->depth(width, height);
}

JitterSequence AntiAliasing::jitter_of(int method_number) {
    AntiAliasMethod *m = method(method_number);
    JitterSequence own = m ? m->jitter() : JitterSequence::none;
    return own != JitterSequence::none ? own : jitter_asked();
}

bool AntiAliasing::apply(CommandBuffer &cmd, const Image &picture, int method_number, const PictureFacts &facts,
                         const PicturePlace &place) {
    AntiAliasMethod *m = method(method_number);
    uint32_t view = debug_view();
    bool temporal = (m && m->jitter() != JitterSequence::none) || view != 0;
    history_.set_wanted(temporal || jitter_asked() != JitterSequence::none);
    if (cut_asked.exchange(false)) {
        history_.cut();
    }
    AntiAliasFrame frame{picture};
    frame.jitter = facts.jitter;
    frame.jitter_index = facts.jitter_index;
    if (!temporal) {
        return m && m->apply(cmd, frame);
    }
    std::lock_guard<std::mutex> hold(history_.lock_);
    uint32_t w = picture.get_width(), h = picture.get_height();
    const VkImageUsageFlags kept_usage = COPY_USAGE | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!keep_image(device_, history_.next_, w, h, VK_FORMAT_R8G8B8A8_UNORM, kept_usage, "the history")) {
        return m && m->apply(cmd, frame);
    }
    const PictureFacts &then = history_.previous_facts_;
    const PicturePlace &then_at = history_.previous_place_;
    // (a frame of the game's after the next: its pictures left out, or the game paused; 0, not known, as no gap)
    bool frames_apart = facts.frame != 0 && then.frame != 0 && facts.frame > then.frame + 1;
    // (a picture whose camera or place is not known, a game frame's picture in a vblank scheduled apart: shown with no
    // history, and the history left as it was for the next)
    bool known = facts.camera.known && place.known;
    bool reset = history_.cut_ || facts.cut || !history_.previous_ || history_.previous_->get_width() != w ||
                 history_.previous_->get_height() != h || frames_apart || !known;
    bool cameras = known && then.camera.known && then_at.known;
    if (place.zscale > 0) {
        frame.depth = inputs_->depth(w, h);
    }
    if (!reset && cameras && frame.depth) {
        frame.motion = inputs_->motion(cmd, facts, place, then, then_at);
    }
    if (aa_trace()) {
        fprintf(stderr, "[aa] now: camera %d frame %llu place %d zscale %g; then: camera %d frame %llu place %d; "
                        "reset %d, motion %d\n",
                facts.camera.known, (unsigned long long)facts.frame, place.known, place.zscale, then.camera.known,
                (unsigned long long)then.frame, then_at.known, reset, frame.motion != nullptr);
    }
    if (aa_trace()) {
        const auto &c = facts.camera;
        fprintf(stderr, "[aa]   camera: screen %g %g %g %g %g %g; R0 %g %g %g t %g %g %g; origin %g %g scale %g %g\n",
                c.screen[0], c.screen[1], c.screen[2], c.screen[3], c.screen[4], c.screen[5], c.world_to_view[0],
                c.world_to_view[1], c.world_to_view[2], c.world_to_view[3], c.world_to_view[7], c.world_to_view[11],
                place.origin[0], place.origin[1], place.scale[0], place.scale[1]);
    }
    frame.previous = reset ? nullptr : history_.previous_.get();
    frame.keep_into = history_.next_.get();
    bool done = m && m->apply(cmd, frame);
    if (!m || !m->keeps_own_history()) {
        copy_picture(cmd, picture, *history_.next_);
    }
    if (view != 0) {
        inputs_->debug(cmd, picture, view, *history_.next_, frame.previous, frame.depth, place.zscale, frame.motion);
    }
    if (!known) {
        return done;
    }
    std::swap(history_.previous_, history_.next_);
    history_.previous_facts_ = facts;
    history_.previous_place_ = place;
    history_.cut_ = false;
    return done;
}

AntiAliasMethod *AntiAliasing::method(int method_number) {
    if (method_number <= PORT_AA_OFF || method_number >= PORT_AA_COUNT) {
        return nullptr;
    }
    std::unique_ptr<AntiAliasMethod> &m = made_[size_t(method_number)];
    if (!m) {
        m = method_for(method_number);
    }
    return m.get();
}

std::unique_ptr<AntiAliasMethod> AntiAliasing::method_for(int method_number) {
    switch (method_number) {
    case PORT_AA_FXAA:
        return std::make_unique<Fxaa>(device_);
    case PORT_AA_SMAA:
        return std::make_unique<Smaa>(device_);
    case PORT_AA_SMAA_T2X:
        return std::make_unique<SmaaT2x>(device_);
    default:
        return nullptr;
    }
}

} // namespace pcr

extern "C" void pcr_antialias_cut(void) {
    pcr::cut_asked.store(true);
}
