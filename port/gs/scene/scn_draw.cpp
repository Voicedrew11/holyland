// The scene renderer's drawing side (scn_draw.h)
#include "scn_draw.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "pcr_shaders.h"
#include "scn_shaders.h"

using namespace Vulkan;

namespace scn {

namespace {

// pcr_prim.h's and pcr_common.h's bindings, as the canvas binds them (pcr_canvas.cpp's bind_between and draw_batch)
constexpr uint32_t native_slots = 4;                         // (000, 280, 700, the ripple's: three buffers each)
constexpr uint32_t walk_clut_binding = 3 + 3 * native_slots; // (set 0's, 15: a kept walk's CLUTs, read by nothing here)
constexpr uint32_t walk_sampler_binding = walk_clut_binding + 1;
constexpr uint32_t walk_samplers = 16;
constexpr uint32_t trace_binding = 6, records_binding = 7, merged_binding = 8, planes_binding = 9;
constexpr uint32_t first_binding = 10; // (set 1's: pcr_prim.h's u_first, SCN_FIRST_DRAW)
constexpr uint32_t vertex_input_count = pcr::vertex_input_count;

uint32_t native_slot(pcr::NativeProgram program) {
    return program == pcr::NativeProgram::vu1_000   ? 0
           : program == pcr::NativeProgram::vu1_280 ? 1
           : program == pcr::NativeProgram::vu1_700 ? 2
                                                    : 3;
}

pcr::GpuWork work_of(const Draw &d) {
    if (!d.native) {
        return pcr::GpuWork::draws_2d;
    }
    switch (d.native_program) {
    case pcr::NativeProgram::vu1_280:
        return pcr::GpuWork::native_280;
    case pcr::NativeProgram::vu1_700:
        return pcr::GpuWork::native_700;
    default:
        return pcr::GpuWork::native_000;
    }
}

} // namespace

Drawer::Drawer(Device &device, pcr::GpuTimes &times) : device_(device), times_(times) {
    // (pcr_canvas.cpp's choose_dynamic: KF4_PC_DYNAMIC as there)
    const char *v = getenv("KF4_PC_DYNAMIC");
    int want = v ? atoi(v) : 2;
    const auto &f = device_.get_device_features();
    dynamic_ds_ = want >= 1 && f.device_api_core_version >= VK_API_VERSION_1_3;
    dynamic_blend_ = dynamic_ds_ && want >= 2 && f.eds3_features.extendedDynamicState3ColorBlendEnable &&
                     f.eds3_features.extendedDynamicState3ColorBlendEquation &&
                     f.eds3_features.extendedDynamicState3ColorWriteMask;
    first_draw_ok_ = device_.image_format_is_supported(first_format, VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
                                                                         VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT |
                                                                         VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT);
    if (!first_draw_ok_) {
        fprintf(stderr, "[scn] no R32F colour target that blends here: DATE by a copy of the target\n");
    }
    make_programs();
    ImageCreateInfo info = ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UINT);
    uint32_t zero = 0;
    ImageInitialData data = {&zero, 0, 0};
    dummy_uint_ = device_.create_image(info, &data);
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    dummy_float_ = device_.create_image(info, &data);
    BufferCreateInfo buffer = {};
    buffer.domain = BufferDomain::Device;
    buffer.size = 256;
    buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    std::array<uint8_t, 1024> zeros{};
    zeros_ = device_.create_buffer(buffer, zeros.data());
    buffer.size = 64;
    no_trace_ = device_.create_buffer(buffer, zeros.data());
    buffer.size = 1024;
    buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    no_clut_ = device_.create_buffer(buffer, zeros.data());
    // (set 2, pcr_prim.h's u_table: a kept walk's, never read here; one dummy)
    table_pool_ = device_.create_bindless_descriptor_pool(BindlessResourceType::Image, 1, 1);
    if (table_pool_ && table_pool_->allocate_descriptors(1)) {
        table_pool_->push_texture(dummy_float_->get_view());
        table_pool_->update();
        table_ = table_pool_->get_descriptor_set();
    }
}

void Drawer::make_programs() {
    // The canvas's layouts (pcr_canvas.cpp's make_programs), so that its shaders' every binding has a resource
    ResourceLayout batch_vert = {};
    batch_vert.push_constant_size = sizeof(pcr::PrimPush);
    batch_vert.sets[0].storage_buffer_mask = (1u << (3 + 3 * native_slots)) - 1;
    for (uint32_t b = 0; b < 3 + 3 * native_slots; b++) {
        batch_vert.sets[0].meta[b].array_size = 1;
    }
    batch_vert.sets[1].storage_buffer_mask = 1u << records_binding | 1u << merged_binding | 1u << planes_binding;
    batch_vert.sets[1].meta[records_binding].array_size = 1;
    batch_vert.sets[1].meta[merged_binding].array_size = 1;
    batch_vert.sets[1].meta[planes_binding].array_size = 1;
    ResourceLayout vert = batch_vert;
    vert.input_mask = (1u << vertex_input_count) - 1;
    ResourceLayout frag = {};
    frag.output_mask = 1;
    frag.push_constant_size = sizeof(pcr::PrimPush);
    frag.sets[1].sampled_image_mask = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4);
    frag.sets[1].fp_mask = (1u << 1) | (1u << 3) | (1u << 4);
    frag.sets[1].uniform_buffer_mask = 1u << 2;
    frag.sets[1].storage_buffer_mask = 1u << trace_binding | 1u << records_binding;
    for (uint32_t b : {0u, 1u, 2u, 3u, 4u, trace_binding, records_binding}) {
        frag.sets[1].meta[b].array_size = 1;
    }
    frag.sets[0].storage_buffer_mask = 1u << walk_clut_binding;
    frag.sets[0].meta[walk_clut_binding].array_size = 1;
    frag.sets[0].sampler_mask = 1u << walk_sampler_binding;
    frag.sets[0].meta[walk_sampler_binding].array_size = walk_samplers;
    frag.sets[2].separate_image_mask = 1u;
    frag.sets[2].meta[0].array_size = DescriptorSetLayout::UNSIZED_ARRAY;
    frag.bindless_set_mask = 1u << 2;
    // (the darkening's: u_first besides)
    ResourceLayout first = frag;
    first.sets[1].sampled_image_mask |= 1u << first_binding;
    first.sets[1].fp_mask |= 1u << first_binding;
    first.sets[1].meta[first_binding].array_size = 1;
#define SCN_PCR_SHADER(name, layout) device_.request_shader(pcr_spv_##name, sizeof(pcr_spv_##name), layout)
    Shader *fs = SCN_PCR_SHADER(pcr_prim_frag, &frag), *efs = SCN_PCR_SHADER(pcr_prim_early_frag, &frag);
    Shader *idfs = device_.request_shader(scn_spv_scn_first_draw_frag, sizeof(scn_spv_scn_first_draw_frag), &frag);
    Shader *firstfs = device_.request_shader(scn_spv_scn_prim_first_frag, sizeof(scn_spv_scn_prim_first_frag), &first);
    Shader *vs[size_t(Program::count)] = {
        SCN_PCR_SHADER(pcr_prim_vert, &vert),         SCN_PCR_SHADER(pcr_nd000_vert, &batch_vert),
        SCN_PCR_SHADER(pcr_nd280_vert, &batch_vert),  SCN_PCR_SHADER(pcr_nd700_vert, &batch_vert),
        SCN_PCR_SHADER(pcr_ndripple_vert, &batch_vert),
    };
#undef SCN_PCR_SHADER
    for (size_t v = 0; v < size_t(Program::count); v++) {
        plain_[v] = device_.request_program(vs[v], fs);
        early_[v] = device_.request_program(vs[v], efs);
        first_id_[v] = device_.request_program(vs[v], idfs);
        first_[v] = device_.request_program(vs[v], firstfs);
    }
}

void Drawer::full_barrier(CommandBuffer &c) {
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

// A pass after a pass, nothing between: the attachment writes before made visible to the attachment reads and writes
// after (pcr_canvas.cpp's pass_to_pass_barrier), and to the fragment shader's reads (the darkening reads the first-draw
// image the pass before drew)
void Drawer::pass_to_pass_barrier(CommandBuffer &c) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    device_.get_device_table().vkCmdPipelineBarrier2(c.get_command_buffer(), &dep);
}

void Drawer::record(const Scene &scene, CommandBuffer &c) {
    bool after_pass = false;
    full_barrier(c); // (what was submitted before: the targets' last scene, uploads into images)
    for (const Step &step : scene.steps) {
        if (step.kind == Step::Kind::op) {
            if (after_pass) {
                full_barrier(c);
            }
            record_op(scene.ops[step.index], c);
            full_barrier(c);
            after_pass = false;
            continue;
        }
        const Pass &pass = scene.passes[step.index];
        if (after_pass) {
            pass_to_pass_barrier(c);
        }
        record_pass(scene, pass, c);
        after_pass = true;
    }
    if (after_pass) {
        full_barrier(c);
    }
}

void Drawer::record_op(const Op &op, CommandBuffer &c) {
    counts_.ops++;
    times_.begin(c, op.work);
    const VkImageSubresourceLayers layers = {
        VkImageAspectFlags(op.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
    switch (op.kind) {
    case Op::Kind::clear:
        if (op.depth) {
            c.clear_image(*op.dst, op.value, VK_IMAGE_ASPECT_DEPTH_BIT);
        } else {
            c.clear_image(*op.dst, op.value);
        }
        break;
    case Op::Kind::copy:
        c.copy_image(*op.dst, *op.src, op.dst_at, op.src_at, op.extent, layers, layers);
        break;
    case Op::Kind::blit:
        c.blit_image(*op.dst, *op.src, op.dst_at,
                     {int32_t(op.extent.width), int32_t(op.extent.height), int32_t(op.extent.depth)}, op.src_at,
                     {int32_t(op.src_extent.width), int32_t(op.src_extent.height), int32_t(op.src_extent.depth)}, 0, 0,
                     0, 0, 1, VK_FILTER_NEAREST);
        break;
    }
}

void Drawer::record_pass(const Scene &scene, const Pass &pass, CommandBuffer &c) {
    counts_.passes++;
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &pass.colour->get_view();
    rp.store_attachments = 1;
    if (pass.clear_colour) {
        rp.clear_attachments = 1;
        rp.clear_color[0] = pass.colour_value;
    } else {
        rp.load_attachments = 1;
    }
    if (pass.depth) {
        rp.depth_stencil = &pass.depth->get_view();
        rp.op_flags = RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT |
                      (pass.clear_depth ? RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT : RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT);
        rp.clear_depth_stencil = {pass.depth_value, 0};
    }
    c.begin_render_pass(rp);
    uint32_t set = no_material; // (the material whose state the command buffer has: a draw of it again sets nothing)
    for (uint32_t i = 0; i < pass.draw_count; i++) {
        const Draw &d = scene.draws[pass.first_draw + i];
        record_draw(scene, pass, d, c, set);
        set = d.clear || scene.materials[d.material].pass_count != 1 ? no_material : d.material;
    }
    c.end_render_pass();
}

void Drawer::record_draw(const Scene &scene, const Pass &pass, const Draw &d, CommandBuffer &c, uint32_t set) {
    counts_.draws++;
    const Material &m = scene.materials[d.material];
    if (!d.clear && d.native && set == d.material) {
        // (the draw before's material: its state, bindings and constants are the command buffer's; a shadow's number)
        counts_.native_draws++;
        counts_.first_draws += pass.kind == PassKind::first_draw ? 1 : 0;
        counts_.darkened += pass.kind == PassKind::darken ? 1 : 0;
        if (d.shadow != no_shadow) {
            pcr::PrimPush push = m.push;
            push.flags = (m.push.flags & ~(3u << pcr::F_PASS_SHIFT)) | m.passes[0].mode << pcr::F_PASS_SHIFT;
            push.record = d.shadow;
            c.push_constants(&push, 0, sizeof(push));
        }
        c.draw(d.triangle_count * 3, 1, d.first_triangle * 3, d.instance);
        return;
    }
    counts_.state_set++;
    c.set_viewport(d.viewport);
    c.set_scissor(d.scissor);
    if (d.clear) {
        pcr::ShaderState clear = {};
        clear.first_draw = 3;
        times_.begin_draw(c, work_of(d), pcr::shader_state_key(clear));
        counts_.clears++;
        c.clear_quad(d.clear_rect, d.clear_att, d.clear_count);
        return;
    }
    pcr::ShaderState st = m.state;
    st.first_draw = pass.kind == PassKind::first_draw ? 1 : pass.kind == PassKind::darken ? 2 : 0;
    times_.begin_draw(c, work_of(d), pcr::shader_state_key(st));
    counts_.native_draws += d.native ? 1 : 0;
    counts_.gif_draws += d.native ? 0 : 1;
    counts_.first_draws += pass.kind == PassKind::first_draw ? 1 : 0;
    counts_.darkened += pass.kind == PassKind::darken ? 1 : 0;
    // Set 0: the pictures between's buffers (zeros: none), the native programs' buffers, a kept walk's CLUTs and
    // samplers (never read here)
    for (uint32_t b = 0; b < 3; b++) {
        c.set_storage_buffer(0, b, *zeros_);
    }
    uint32_t mine = d.native ? native_slot(d.native_program) : native_slots;
    for (uint32_t slot = 0; slot < native_slots; slot++) {
        const Buffer *buffers[3] = {zeros_.get(), zeros_.get(), zeros_.get()};
        if (slot == mine) {
            buffers[0] = d.arena.get();
            buffers[1] = d.records.get();
            buffers[2] = d.triangles.get();
        }
        for (uint32_t i = 0; i < 3; i++) {
            c.set_storage_buffer(0, 3 + 3 * slot + i, *buffers[i]);
        }
    }
    c.set_storage_buffer(0, walk_clut_binding, *zeros_);
    const Sampler &nearest = device_.get_stock_sampler(StockSampler::NearestClamp);
    for (uint32_t i = 0; i < walk_samplers; i++) {
        c.set_sampler(0, walk_sampler_binding + i, nearest);
    }
    // Set 1: the material's textures and CLUT, the trace (none), a kept walk's records (none)
    c.set_storage_buffer(1, trace_binding, *no_trace_);
    c.set_storage_buffer(1, records_binding, *zeros_);
    c.set_storage_buffer(1, merged_binding, *zeros_);
    c.set_storage_buffer(1, planes_binding, *zeros_);
    c.set_texture(1, 0, m.texels ? m.texels->get_view() : dummy_uint_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, 1, m.surface ? m.surface->get_view() : dummy_float_->get_view(), StockSampler::NearestClamp);
    if (m.sampled && m.sampler) {
        c.set_texture(1, 4, m.sampled->get_view(), *m.sampler);
    } else {
        c.set_texture(1, 4, dummy_float_->get_view(), StockSampler::NearestClamp);
    }
    c.set_texture(1, 3, m.dest ? m.dest->get_view() : dummy_float_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, first_binding, pass.first ? pass.first->get_view() : dummy_float_->get_view(),
                  StockSampler::NearestClamp);
    if (m.clut != no_clut) {
        void *clut = c.allocate_constant_data(1, 2, sizeof(scene.cluts[m.clut]));
        memcpy(clut, scene.cluts[m.clut].data(), sizeof(scene.cluts[m.clut]));
    } else {
        c.set_uniform_buffer(1, 2, *no_clut_);
    }
    if (table_.handle.set != VK_NULL_HANDLE) {
        c.set_bindless(2, table_);
    }
    c.set_opaque_state();
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_compare(m.zcompare);
    c.set_blend_constants(m.constants);
    if (!d.native) {
        size_t bytes = size_t(d.vertex_count) * sizeof(pcr::Vertex);
        memcpy(c.allocate_vertex_data(0, bytes, sizeof(pcr::Vertex)), scene.vertices.data() + d.first_vertex, bytes);
        for (uint32_t i = 0; i < vertex_input_count; i++) {
            c.set_vertex_attrib(i, 0, pcr::vertex_inputs[i].format, pcr::vertex_inputs[i].offset);
        }
    }
    size_t v = size_t(m.program);
    pcr::PrimPush push = m.push;
    if (d.shadow != no_shadow) {
        push.record = d.shadow;
    }
    for (uint32_t i = 0; i < m.pass_count; i++) {
        const Material::AlphaPass &ap = m.passes[i];
        push.flags = (m.push.flags & ~(3u << pcr::F_PASS_SHIFT)) | ap.mode << pcr::F_PASS_SHIFT;
        if (pass.kind == PassKind::first_draw) {
            // (each triangle's number, the least kept: no colour of the GS's, nothing else written)
            c.set_program(first_id_[v]);
            c.set_depth_test(m.ztest, false);
            c.set_blend_enable(true);
            c.set_blend_factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
            c.set_blend_op(VK_BLEND_OP_MIN, VK_BLEND_OP_MIN);
            c.set_color_write_mask(0x1);
        } else {
            bool early = ap.mode == 0 && !(m.push.flags & pcr::F_DATE);
            c.set_program(pass.kind == PassKind::darken ? first_[v] : early ? early_[v] : plain_[v]);
            c.set_depth_test(m.ztest, m.ztest && ap.zwrite);
            if (m.blend.enable) {
                c.set_blend_enable(true);
                c.set_blend_factors(m.blend.src, m.blend.src_alpha, m.blend.dst, VK_BLEND_FACTOR_ZERO);
                c.set_blend_op(m.blend.op, VK_BLEND_OP_ADD);
            } else {
                c.set_blend_enable(false);
            }
            c.set_color_write_mask(ap.mask);
        }
        c.push_constants(&push, 0, sizeof(push));
        if (d.native) {
            c.draw(d.triangle_count * 3, 1, d.first_triangle * 3, d.instance);
        } else {
            c.draw(d.vertex_count);
        }
    }
}

} // namespace scn
