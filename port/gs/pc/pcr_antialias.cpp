// The picture's anti-aliasing (pcr_antialias.h)
#include "pcr_antialias.h"

#include <stdio.h>

#include "command_buffer.hpp"
#include "pcr_shaders.h"

using namespace Vulkan;

namespace pcr {

namespace {

// FXAA (shaders/pcr_fxaa.frag): the picture copied, then drawn over whole from the copy
class Fxaa final : public AntiAliasMethod {
public:
    explicit Fxaa(Device &device) : device_(device) {
        ResourceLayout vert = {};
        Shader *vs = device_.request_shader(pcr_spv_pcr_present_vert, sizeof(pcr_spv_pcr_present_vert), &vert);
        ResourceLayout frag = {};
        frag.output_mask = 1;
        frag.sets[0].sampled_image_mask = 1;
        frag.sets[0].fp_mask = 1;
        frag.sets[0].meta[0].array_size = 1;
        frag.push_constant_size = 2 * sizeof(float);
        program_ = device_.request_program(
            vs, device_.request_shader(pcr_spv_pcr_fxaa_frag, sizeof(pcr_spv_pcr_fxaa_frag), &frag));
    }

    bool apply(CommandBuffer &cmd, const Image &picture) override {
        uint32_t w = picture.get_width(), h = picture.get_height();
        if (!copy_image(w, h)) {
            return false;
        }
        const VkImageSubresourceLayers colour = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        cmd.copy_image(*copy_, picture, {0, 0, 0}, {0, 0, 0}, {w, h, 1}, colour, colour);
        cmd.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        RenderPassInfo rp;
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &picture.get_view();
        rp.store_attachments = 1;
        cmd.begin_render_pass(rp);
        cmd.set_opaque_state();
        cmd.set_program(program_);
        cmd.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        cmd.set_cull_mode(VK_CULL_MODE_NONE);
        cmd.set_depth_test(false, false);
        cmd.set_texture(0, 0, copy_->get_view(), StockSampler::LinearClamp);
        const float texel[2] = {1.0f / float(w), 1.0f / float(h)};
        cmd.push_constants(texel, 0, sizeof(texel));
        cmd.draw(3);
        cmd.end_render_pass();
        cmd.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        return true;
    }

private:
    // The picture's copy, kept while its size stays; false when it cannot be made
    bool copy_image(uint32_t width, uint32_t height) {
        if (copy_ && copy_->get_width() == width && copy_->get_height() == height) {
            return true;
        }
        ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R8G8B8A8_UNORM);
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.levels = 1;
        info.layout = ImageLayout::General;
        info.initial_layout = VK_IMAGE_LAYOUT_GENERAL;
        copy_ = device_.create_image(info, nullptr);
        if (!copy_) {
            fprintf(stderr, "[pcr] no image for FXAA's copy of a %ux%u picture: drawn without it\n", width, height);
        }
        return bool(copy_);
    }

    Device &device_;
    Program *program_ = nullptr;
    ImageHandle copy_;
};

} // namespace

AntiAliasing::AntiAliasing(Device &device) : device_(device) {}
AntiAliasing::~AntiAliasing() = default;

bool AntiAliasing::apply(CommandBuffer &cmd, const Image &picture, int method) {
    if (method <= PORT_AA_OFF || method >= PORT_AA_COUNT) {
        return false;
    }
    std::unique_ptr<AntiAliasMethod> &m = made_[size_t(method)];
    if (!m) {
        m = method_for(method);
    }
    return m && m->apply(cmd, picture);
}

std::unique_ptr<AntiAliasMethod> AntiAliasing::method_for(int method) {
    switch (method) {
    case PORT_AA_FXAA:
        return std::make_unique<Fxaa>(device_);
    default:
        return nullptr;
    }
}

} // namespace pcr
