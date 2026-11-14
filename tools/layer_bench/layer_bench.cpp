// kf4-layerbench: what a full-screen layer costs on this GPU, with no game in it.
//
// The PC renderer's pictures at 4x cost about 3.3 ms, and whether that is the card's limit or the renderer's doing
// was only inferred (docs/port/PC_OPTIMIZATION.md, "What a picture's layers are"). This draws plain layers over a
// target of the renderer's size and formats, one state changed at a time, and times them by the clock: pictures are
// submitted one after another and waited for, so the time a picture is the GPU's (it is the limit here).
//
//   kf4-layerbench [--size WxH] [--pictures N] [--layers N] [--only NAME]
//
// One thread. Prints a table: the case, ms a layer, and megabytes a layer the colour and depth targets hold.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "context.hpp"
#include "device.hpp"
#include "layer_bench_shaders.h"

using namespace Vulkan;

namespace {

enum class Shade { fill, textured, discards, own_depth };

struct Case {
    const char *name;
    Shade shade;
    bool depth;        // tested GEQUAL and written
    bool hidden;       // each layer behind the first: every fragment fails the test
    bool blend;        // source alpha over the target
    int grid;          // quads across and down
    bool pass_a_layer; // a render pass for each layer, not one for the picture
};

const Case cases[] = {
    {"fill (one colour, no depth, no blend)", Shade::fill, false, false, false, 1, false},
    {"textured", Shade::textured, false, false, false, 1, false},
    {"textured, blended", Shade::textured, false, false, true, 1, false},
    {"textured, depth", Shade::textured, true, false, false, 1, false},
    {"textured, depth, blended", Shade::textured, true, false, true, 1, false},
    {"... a shader that may discard", Shade::discards, true, false, true, 1, false},
    {"... and writes its own depth", Shade::own_depth, true, false, true, 1, false},
    {"... as 45000 triangles", Shade::discards, true, false, true, 150, false},
    {"45000 triangles: textured, depth", Shade::textured, true, false, false, 150, false},
    {"45000 triangles: ... blended", Shade::textured, true, false, true, 150, false},
    {"11000 triangles: ... blended", Shade::textured, true, false, true, 75, false},
    {"... a render pass a layer", Shade::discards, true, false, true, 1, true},
    {"hidden: textured, depth (early test)", Shade::textured, true, true, false, 1, false},
    {"hidden: a shader that may discard", Shade::discards, true, true, true, 1, false},
    {"hidden: a shader with its own depth", Shade::own_depth, true, true, true, 1, false},
};

struct Target {
    VkFormat colour;
    uint32_t samples;
    uint32_t width, height;        // the image's
    uint32_t drawn_w, drawn_h;     // the part drawn in (the viewport and scissor)
    const char *name;
};

struct Push {
    float z;
    int32_t grid;
    float tile_x, tile_y;
};

class Bench {
public:
    Bench(Device &device, int pictures, int layers) : device_(device), pictures_(pictures), layers_(layers) {}

    // False if a program or the noise could not be made
    bool prepare() {
        ResourceLayout vert = {};
        vert.push_constant_size = sizeof(Push);
        Shader *vs = device_.request_shader(layer_vert, sizeof(layer_vert), &vert);
        ResourceLayout frag = {};
        frag.output_mask = 1;
        frag.sets[0].sampled_image_mask = 1;
        frag.sets[0].fp_mask = 1;
        frag.sets[0].meta[0].array_size = 1;
        frag.push_constant_size = sizeof(Push);
        const uint32_t *code[4] = {layer_frag_fill, layer_frag_textured, layer_frag_discards, layer_frag_own_depth};
        const size_t sizes[4] = {sizeof(layer_frag_fill), sizeof(layer_frag_textured), sizeof(layer_frag_discards),
                                 sizeof(layer_frag_own_depth)};
        for (int i = 0; i < 4; i++) {
            programs_[i] = device_.request_program(vs, device_.request_shader(code[i], sizes[i], &frag));
            if (!programs_[i]) {
                return false;
            }
        }
        // noise: every texel its own, so the target's blocks cannot be compressed as a flat colour's can
        std::vector<uint32_t> texels(size_t(noise_size) * noise_size);
        uint32_t seed = 0x1234567u;
        for (uint32_t &t : texels) {
            seed = seed * 1664525u + 1013904223u;
            t = seed | 0xFF000000u;
        }
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(noise_size, noise_size, VK_FORMAT_R8G8B8A8_UNORM);
        ImageInitialData data = {texels.data(), 0, 0};
        noise_ = device_.create_image(info, &data);
        return bool(noise_);
    }

    // ms a layer of one case on one target; negative if the target's images could not be made
    double run(const Target &t, const Case &c) {
        ImageCreateInfo info = ImageCreateInfo::render_target(t.width, t.height, t.colour);
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.samples = VkSampleCountFlagBits(t.samples);
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        ImageHandle colour = device_.create_image(info, nullptr);
        info.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ImageHandle depth = device_.create_image(info, nullptr);
        if (!colour || !depth) {
            return -1;
        }
        for (int i = 0; i < warm_up; i++) {
            picture(t, c, *colour, *depth);
        }
        device_.wait_idle();
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < pictures_; i++) {
            picture(t, c, *colour, *depth);
        }
        device_.wait_idle();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return ms / pictures_ / layers_;
    }

private:
    static constexpr uint32_t noise_size = 1024;
    static constexpr int warm_up = 20;

    void begin(CommandBuffer &cmd, const Target &t, const Case &c, const Image &colour, const Image &depth, bool first) {
        RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &colour.get_view();
        rp.store_attachments = 1;
        rp.load_attachments = first ? 0 : 1;
        rp.clear_attachments = first ? 1 : 0;
        rp.depth_stencil = &depth.get_view();
        rp.op_flags = RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT |
                      (first ? RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT : RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT);
        rp.clear_depth_stencil.depth = c.hidden ? 0.0f : 0.0f;
        cmd.begin_render_pass(rp);
        cmd.set_viewport({0, 0, float(t.drawn_w), float(t.drawn_h), 0, 1});
        cmd.set_scissor({{0, 0}, {t.drawn_w, t.drawn_h}});
        cmd.set_opaque_state();
        cmd.set_program(programs_[int(c.shade)]);
        cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        cmd.set_cull_mode(VK_CULL_MODE_NONE);
        cmd.set_depth_test(c.depth, c.depth);
        cmd.set_depth_compare(VK_COMPARE_OP_GREATER_OR_EQUAL);
        cmd.set_blend_enable(c.blend);
        if (c.blend) {
            cmd.set_blend_factors(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                  VK_BLEND_FACTOR_ZERO);
            cmd.set_blend_op(VK_BLEND_OP_ADD, VK_BLEND_OP_ADD);
        }
        cmd.set_texture(0, 0, noise_->get_view(), StockSampler::NearestWrap);
    }

    // One picture: the layers, each nearer than the last (or, hidden, the first nearest), submitted
    void picture(const Target &t, const Case &c, const Image &colour, const Image &depth) {
        CommandBufferHandle cmd = device_.request_command_buffer();
        begin(*cmd, t, c, colour, depth, true);
        for (int i = 0; i < layers_; i++) {
            if (c.pass_a_layer && i > 0) {
                cmd->end_render_pass();
                cmd->barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
                begin(*cmd, t, c, colour, depth, false);
            }
            float step = 1.0f / float(layers_ + 1);
            Push push = {c.hidden ? 1.0f - step * float(i + 1) : step * float(i + 1), c.grid,
                         float(t.drawn_w) / float(noise_size), float(t.drawn_h) / float(noise_size)};
            cmd->push_constants(&push, 0, sizeof(push));
            cmd->draw(uint32_t(c.grid * c.grid * 6));
        }
        cmd->end_render_pass();
        device_.submit(cmd);
        device_.next_frame_context();
    }

    Device &device_;
    const int pictures_, layers_;
    Program *programs_[4] = {};
    ImageHandle noise_;
};

} // namespace

int main(int argc, char **argv) {
    uint32_t width = 3840, height = 1792;
    int pictures = 200, layers = 8;
    const char *only = nullptr;
    for (int i = 1; i < argc; i++) {
        const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (strcmp(argv[i], "--size") == 0 && value && sscanf(value, "%ux%u", &width, &height) == 2) {
            i++;
        } else if (strcmp(argv[i], "--pictures") == 0 && value) {
            pictures = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--layers") == 0 && value) {
            layers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--only") == 0 && value) {
            only = argv[++i];
        } else {
            fprintf(stderr, "usage: kf4-layerbench [--size WxH] [--pictures N] [--layers N] [--only NAME]\n");
            return 2;
        }
    }
    if (width == 0 || height == 0 || width > 8192 || height > 8192 || pictures < 1 || layers < 1 || layers > 64) {
        fprintf(stderr, "kf4-layerbench: a size up to 8192x8192, pictures from 1, layers 1 to 64\n");
        return 2;
    }
    if (!Context::init_loader(nullptr)) {
        fprintf(stderr, "kf4-layerbench: no Vulkan loader\n");
        return 1;
    }
    Context context;
    context.set_num_thread_indices(1);
    if (!context.init_instance_and_device(nullptr, 0, nullptr, 0, 0)) {
        fprintf(stderr, "kf4-layerbench: no Vulkan device\n");
        return 1;
    }
    Device device;
    device.set_context(context);
    device.init_frame_contexts(4);
    Bench bench(device, pictures, layers);
    if (!bench.prepare()) {
        fprintf(stderr, "kf4-layerbench: the programs or the noise could not be made\n");
        return 1;
    }
    const Target targets[] = {
        {VK_FORMAT_R16G16B16A16_UNORM, 2, width, height, width, height, "16 bits a channel, 2 samples (the owner's)"},
        {VK_FORMAT_R16G16B16A16_UNORM, 2, width, height * 2, width, height,
         "... in an image twice as tall (the renderer's: both buffers in one)"},
        {VK_FORMAT_R16G16B16A16_UNORM, 1, width, height, width, height, "16 bits a channel, 1 sample"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, 2, width, height, width, height, "16-bit floats, 2 samples"},
        {VK_FORMAT_R16G16B16A16_SFLOAT, 1, width, height, width, height, "16-bit floats, 1 sample"},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 2, width, height, width, height, "10 bits a channel (2 of alpha), 2 samples"},
        {VK_FORMAT_R8G8B8A8_UNORM, 2, width, height, width, height, "8 bits a channel, 2 samples"},
        {VK_FORMAT_R8G8B8A8_UNORM, 1, width, height, width, height, "8 bits a channel, 1 sample"},
        {VK_FORMAT_R8G8B8A8_UNORM, 4, width, height, width, height, "8 bits a channel, 4 samples"},
        {VK_FORMAT_R8G8B8A8_UNORM, 8, width, height, width, height, "8 bits a channel, 8 samples"},
        {VK_FORMAT_R16G16B16A16_UNORM, 4, width, height, width, height, "16 bits a channel, 4 samples"},
    };
    printf("%s, %ux%u drawn, %d layers a picture, %d pictures a case\n", device.get_gpu_properties().deviceName, width,
           height, layers, pictures);
    for (const Target &t : targets) {
        double mb = double(t.drawn_w) * t.drawn_h * t.samples * (t.colour == VK_FORMAT_R8G8B8A8_UNORM || t.colour == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? 4 : 8) / 1e6;
        printf("\n%s: a layer's colour is %.0f MB, its depth %.0f MB\n", t.name, mb,
               double(t.drawn_w) * t.drawn_h * t.samples * 4 / 1e6);
        for (const Case &c : cases) {
            if (only && !strstr(c.name, only)) {
                continue;
            }
            double ms = bench.run(t, c);
            if (ms < 0) {
                printf("  %-40s  (no memory for the target)\n", c.name);
                continue;
            }
            printf("  %-40s %7.3f ms a layer\n", c.name, ms);
            fflush(stdout);
        }
    }
    return 0;
}
