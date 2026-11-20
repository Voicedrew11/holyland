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
    if (const char *log = getenv("KF4_SCN_RUNS_LOG")) {
        runs_log_ = fopen(log, "w");
        if (runs_log_) {
            fprintf(runs_log_, "# vblank picture draws calls record_calls runs runs_280 end_pass end_state end_own "
                               "end_clear end_gif3d gif3d_splits records table cluts\n");
        }
    }
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
    // (the records' programs: the PC renderer's shaders and the scene's two DATE shaders made with PCR_RECORDS and
    // PCR_DRAWS_APART, each record a draw of its own: scn_shaders.h's)
#define SCN_REC_SHADER(name, layout) device_.request_shader(scn_spv_scn_##name, sizeof(scn_spv_scn_##name), layout)
    Shader *fs_rec = SCN_REC_SHADER(pcr_prim_frag_rec, &frag), *efs_rec = SCN_REC_SHADER(pcr_prim_early_frag_rec, &frag);
    Shader *idfs_rec =
        device_.request_shader(scn_spv_scn_first_draw_frag_rec, sizeof(scn_spv_scn_first_draw_frag_rec), &frag);
    Shader *firstfs_rec =
        device_.request_shader(scn_spv_scn_prim_first_frag_rec, sizeof(scn_spv_scn_prim_first_frag_rec), &first);
    Shader *vs_rec[size_t(Program::count)] = {
        SCN_REC_SHADER(pcr_prim_vert_rec, &vert),        SCN_REC_SHADER(pcr_nd000_vert_rec, &batch_vert),
        SCN_REC_SHADER(pcr_nd280_vert_rec, &batch_vert), SCN_REC_SHADER(pcr_nd700_vert_rec, &batch_vert),
        SCN_REC_SHADER(pcr_ndripple_vert_rec, &batch_vert),
    };
#undef SCN_REC_SHADER
#undef SCN_PCR_SHADER
    Shader *layerfs = device_.request_shader(scn_spv_scn_picture_layer_frag, sizeof(scn_spv_scn_picture_layer_frag),
                                             &frag);
    picture_layer_ = device_.request_program(vs[size_t(Program::gif)], layerfs);
    for (size_t v = 0; v < size_t(Program::count); v++) {
        plain_[v] = device_.request_program(vs[v], fs);
        early_[v] = device_.request_program(vs[v], efs);
        first_id_[v] = device_.request_program(vs[v], idfs);
        first_[v] = device_.request_program(vs[v], firstfs);
        plain_rec_[v] = device_.request_program(vs_rec[v], fs_rec);
        early_rec_[v] = device_.request_program(vs_rec[v], efs_rec);
        first_id_rec_[v] = device_.request_program(vs_rec[v], idfs_rec);
        first_rec_[v] = device_.request_program(vs_rec[v], firstfs_rec);
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

// The scene's records, table and CLUTs. A material reads the table where its texture is one the store decoded or
// sampled (u_texels, u_sampled), with its sampler among the scene's sixteen; one that reads a target or its copy, or
// a picture's layer, keeps its own set 1 and push constants.
void Drawer::prepare(const Scene &scene) {
    prepared_ = Prepared();
    Prepared &p = prepared_;
    p.first_record.assign(scene.draws.size(), no_record);
    if (!records_on_) {
        return;
    }
    p.samplers[0] = &device_.get_stock_sampler(StockSampler::NearestClamp);
    uint32_t samplers = 1;
    std::vector<const ImageView *> views;
    std::vector<uint32_t> table_of(scene.materials.size(), no_record);
    std::vector<uint8_t> eligible(scene.materials.size(), 0);
    for (size_t i = 0; i < scene.materials.size(); i++) {
        const Material &m = scene.materials[i];
        if (m.picture_layer || m.surface || m.dest) {
            continue;
        }
        const ImageView *view = nullptr;
        uint32_t s = 0;
        if (m.sampled && m.sampler) {
            while (s < samplers && p.samplers[s] != m.sampler) {
                s++;
            }
            if (s == samplers) {
                if (samplers == p.samplers.size()) {
                    counts_.own_sampler++;
                    continue;
                }
                p.samplers[samplers++] = m.sampler;
            }
            view = &m.sampled->get_view();
        } else if (m.texels) {
            view = &m.texels->get_view();
        }
        if (view) {
            uint32_t at = 0;
            while (at < views.size() && views[at] != view) {
                at++;
            }
            if (at == views.size()) {
                if (views.size() >= 0xFFFFu) {
                    continue;
                }
                views.push_back(view);
            }
            table_of[i] = at | s << 16;
        }
        eligible[i] = 1;
    }
    for (uint32_t i = samplers; i < p.samplers.size(); i++) {
        p.samplers[i] = p.samplers[0];
    }
    if (!views.empty()) {
        p.table_pool = device_.create_bindless_descriptor_pool(BindlessResourceType::Image, 1, unsigned(views.size()));
        if (p.table_pool && p.table_pool->allocate_descriptors(unsigned(views.size()))) {
            for (const ImageView *v : views) {
                p.table_pool->push_texture(*v);
            }
            p.table_pool->update();
            p.table = p.table_pool->get_descriptor_set();
        } else {
            p.table_pool.reset();
            return; // (no table: every draw with its own set 1)
        }
    }
    // the CLUTs, 256 colours each, in the scene's order (Scene::cluts holds each once a run of draws)
    if (!scene.cluts.empty()) {
        BufferCreateInfo info = {};
        info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
        info.size = scene.cluts.size() * sizeof(scene.cluts[0]);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        p.cluts = device_.create_buffer(info, scene.cluts.data());
        if (!p.cluts) {
            return;
        }
    }
    // a record a draw's pass, in the draws' order
    std::vector<pcr::PrimPush> records;
    records.reserve(scene.draws.size() + scene.draws.size() / 4);
    for (size_t di = 0; di < scene.draws.size(); di++) {
        const Draw &d = scene.draws[di];
        if (d.clear || !eligible[d.material]) {
            continue;
        }
        const Material &m = scene.materials[d.material];
        p.first_record[di] = uint32_t(records.size());
        pcr::PrimPush r = m.push;
        if (d.shadow != no_shadow) {
            r.record = d.shadow;
        }
        r.table = table_of[d.material] == no_record ? 0xFFFFu : table_of[d.material];
        if (m.sampled && table_of[d.material] != no_record) {
            // (a sampled texture's size from its record: pcr_prim.h's sampled_size)
            r.src_offset[0] = int32_t(m.sampled->get_width());
            r.src_offset[1] = int32_t(m.sampled->get_height());
        }
        r.clut_at = m.clut != no_clut ? m.clut * 256u : 0xFFFFFFFFu;
        r.merged = 0;
        r.factors = 0;
        for (uint32_t i = 0; i < m.pass_count; i++) {
            r.flags = (m.push.flags & ~(3u << pcr::F_PASS_SHIFT)) | m.passes[i].mode << pcr::F_PASS_SHIFT;
            records.push_back(r);
        }
    }
    if (records.empty()) {
        std::fill(p.first_record.begin(), p.first_record.end(), no_record);
        return;
    }
    BufferCreateInfo info = {};
    info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
    info.size = records.size() * sizeof(pcr::PrimPush);
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    p.records = device_.create_buffer(info, records.data());
    if (!p.records) {
        std::fill(p.first_record.begin(), p.first_record.end(), no_record);
        return;
    }
    counts_.records += records.size();
    counts_.table_entries += views.size();
    counts_.table_max = std::max<uint64_t>(counts_.table_max, views.size());
    counts_.cluts += scene.cluts.size();
}

// A call into the runs: a run goes on while the key is the same, else a new one starts and what ended the last is
// counted
void Drawer::count_call(const RunKey &key) {
    counts_.calls++;
    RunState &r = run_;
    auto same = [](const RunKey &a, const RunKey &b) {
        return a.program == b.program && a.ztest == b.ztest && a.zwrite == b.zwrite && a.zcompare == b.zcompare &&
               a.blend == b.blend &&
               (!a.blend || (a.src == b.src && a.src_alpha == b.src_alpha && a.dst == b.dst && a.op == b.op)) &&
               a.mask == b.mask && memcmp(a.constants, b.constants, sizeof(a.constants)) == 0 &&
               memcmp(&a.viewport, &b.viewport, sizeof(a.viewport)) == 0 &&
               memcmp(&a.scissor, &b.scissor, sizeof(a.scissor)) == 0 &&
               memcmp(a.native, b.native, sizeof(a.native)) == 0 && a.gif_3d == b.gif_3d;
    };
    // (a stretch of the GIF's 3D between two calls that would have been one run)
    if (key.gif_3d && !r.in_gif3d) {
        r.in_gif3d = true;
        r.before_gif3d_valid = r.open && !r.after_clear && !r.last.own && !r.last.gif_3d;
        r.before_gif3d = r.last;
    } else if (!key.gif_3d && r.in_gif3d) {
        r.in_gif3d = false;
        if (r.before_gif3d_valid && !r.after_clear && !key.own && same(r.before_gif3d, key)) {
            counts_.gif3d_splits++;
        }
    }
    End end = end_count;
    if (!r.open) {
        end = end_pass;
    } else if (r.after_clear) {
        end = end_clear;
    } else if (key.own || r.last.own) {
        end = end_own;
    } else if (!same(r.last, key)) {
        end = key.gif_3d || r.last.gif_3d ? end_gif3d : end_state;
    }
    if (end != end_count) {
        counts_.runs++;
        counts_.runs_280 += key.n280 ? 1 : 0;
        counts_.ends[end]++;
    }
    r.open = true;
    r.after_clear = false;
    r.last = key;
}

void Drawer::log_scene(const Scene &scene, const Counts &b) {
    const Counts &a = counts_;
    bool picture = std::any_of(scene.passes.begin(), scene.passes.end(), [](const Pass &p) { return p.picture; });
    fprintf(runs_log_, "%llu %d %llu %llu %llu %llu %llu", (unsigned long long)scene.vblank, picture ? 1 : 0,
            (unsigned long long)(a.draws - b.draws), (unsigned long long)(a.calls - b.calls),
            (unsigned long long)(a.record_calls - b.record_calls), (unsigned long long)(a.runs - b.runs),
            (unsigned long long)(a.runs_280 - b.runs_280));
    for (uint32_t e = 0; e < end_count; e++) {
        fprintf(runs_log_, " %llu", (unsigned long long)(a.ends[e] - b.ends[e]));
    }
    fprintf(runs_log_, " %llu %llu %llu %llu\n", (unsigned long long)(a.gif3d_splits - b.gif3d_splits),
            (unsigned long long)(a.records - b.records), (unsigned long long)(a.table_entries - b.table_entries),
            (unsigned long long)(a.cluts - b.cluts));
}

void Drawer::record(const Scene &scene, CommandBuffer &c) {
    const Counts before = counts_;
    counts_.scenes++;
    prepare(scene);
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
    if (runs_log_) {
        log_scene(scene, before);
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
    if (pass.colour) {
        rp.num_color_attachments = 1;
        rp.color_attachments[0] = &pass.colour->get_view();
        rp.store_attachments = 1;
        if (pass.clear_colour) {
            rp.clear_attachments = 1;
            rp.clear_color[0] = pass.colour_value;
        } else {
            rp.load_attachments = 1;
        }
    }
    if (pass.depth) {
        rp.depth_stencil = &pass.depth->get_view();
        rp.op_flags = RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT |
                      (pass.clear_depth ? RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT : RENDER_PASS_OP_LOAD_DEPTH_STENCIL_BIT);
        rp.clear_depth_stencil = {pass.depth_value, 0};
    }
    rp.render_area = pass.area;
    if (time_clears_ && (pass.clear_colour || pass.clear_depth)) {
        // (developers', KF4_SCN_TIME_CLEARS=1: its load ops' clears timed as clears, else they are in the interval of
        // the draw before; a timestamp more, which waits for the GPU: the frame takes longer timed)
        times_.begin(c, pcr::GpuWork::surface);
    }
    c.begin_render_pass(rp);
    run_ = RunState();
    uint32_t set = no_material; // (the material whose state the command buffer has: a draw of it again sets nothing)
    for (uint32_t i = 0; i < pass.draw_count; i++) {
        const Draw &d = scene.draws[pass.first_draw + i];
        record_draw(scene, pass, pass.first_draw + i, c, set);
        set = d.clear || scene.materials[d.material].pass_count != 1 ? no_material : d.material;
    }
    c.end_render_pass();
}

// The records' bindings (a draw from the scene's records): set 0's native buffers for its program and the scene's
// CLUTs and samplers, set 1's dummies but for the records and the pass's first-draw image, set 2 the scene's table.
// The same resources again bind nothing (Granite compares them).
void Drawer::bind_records(const Pass &pass, const Draw &d, CommandBuffer &c) {
    const Prepared &p = prepared_;
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
    c.set_storage_buffer(0, walk_clut_binding, p.cluts ? *p.cluts : *zeros_);
    for (uint32_t i = 0; i < walk_samplers; i++) {
        c.set_sampler(0, walk_sampler_binding + i, *p.samplers[i]);
    }
    c.set_storage_buffer(1, trace_binding, *no_trace_);
    c.set_storage_buffer(1, records_binding, *p.records);
    c.set_storage_buffer(1, merged_binding, *zeros_);
    c.set_storage_buffer(1, planes_binding, *zeros_);
    c.set_texture(1, 0, dummy_uint_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, 1, dummy_float_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, 4, dummy_float_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, 3, dummy_float_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, first_binding, pass.first ? pass.first->get_view() : dummy_float_->get_view(),
                  StockSampler::NearestClamp);
    c.set_uniform_buffer(1, 2, *no_clut_);
    if (p.table) {
        c.set_bindless(2, p.table);
    } else if (table_.handle.set != VK_NULL_HANDLE) {
        c.set_bindless(2, table_);
    }
}

// A draw's own bindings (slice 2's way: its textures and CLUT in set 1, its fields as push constants)
void Drawer::bind_own(const Scene &scene, const Pass &pass, const Draw &d, const Material &m, CommandBuffer &c) {
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
}

// (a draw from the records: the push constants are its record's place, pcr_common.h's pcr_push.record; merged 0)
void Drawer::push_record(CommandBuffer &c, uint32_t record) {
    pcr::PrimPush push = {};
    push.record = record;
    push.trace_xy = pcr::no_trace;
    c.push_constants(&push, 0, sizeof(push));
}

void Drawer::record_draw(const Scene &scene, const Pass &pass, uint32_t index, CommandBuffer &c, uint32_t set) {
    counts_.draws++;
    const Draw &d = scene.draws[index];
    const Material &m = scene.materials[d.material];
    const uint32_t rec = prepared_.first_record[index];
    if (!d.clear && d.native && set == d.material) {
        // (the draw before's material: its state, bindings and constants are the command buffer's; its record, or a
        // shadow's number)
        counts_.native_draws++;
        counts_.first_draws += pass.kind == PassKind::first_draw ? 1 : 0;
        counts_.darkened += pass.kind == PassKind::darken ? 1 : 0;
        if (rec != no_record) {
            push_record(c, rec);
            counts_.record_calls++;
        } else if (d.shadow != no_shadow) {
            pcr::PrimPush push = m.push;
            push.flags = (m.push.flags & ~(3u << pcr::F_PASS_SHIFT)) | m.passes[0].mode << pcr::F_PASS_SHIFT;
            push.record = d.shadow;
            c.push_constants(&push, 0, sizeof(push));
        }
        count_call(run_.last);
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
        run_.after_clear = true;
        c.clear_quad(d.clear_rect, d.clear_att, d.clear_count);
        return;
    }
    pcr::ShaderState st = m.state;
    st.first_draw = pass.kind == PassKind::first_draw ? 1 : pass.kind == PassKind::darken ? 2 : 0;
    times_.begin_draw(c, pass.picture ? pcr::GpuWork::picture : work_of(d), pcr::shader_state_key(st));
    counts_.native_draws += d.native ? 1 : 0;
    counts_.gif_draws += d.native ? 0 : 1;
    counts_.first_draws += pass.kind == PassKind::first_draw ? 1 : 0;
    counts_.darkened += pass.kind == PassKind::darken ? 1 : 0;
    if (rec != no_record) {
        bind_records(pass, d, c);
    } else {
        bind_own(scene, pass, d, m, c);
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
    const bool records = rec != no_record;
    pcr::PrimPush push = m.push;
    if (d.shadow != no_shadow) {
        push.record = d.shadow;
    }
    RunKey key;
    key.viewport = d.viewport;
    key.scissor = d.scissor;
    key.zcompare = m.zcompare;
    memcpy(key.constants, m.constants, sizeof(key.constants));
    if (d.native) {
        key.native[0] = d.arena.get();
        key.native[1] = d.records.get();
        key.native[2] = d.triangles.get();
    }
    key.own = !records;
    key.gif_3d = d.gif_3d;
    key.n280 = d.native && d.native_program == pcr::NativeProgram::vu1_280;
    for (uint32_t i = 0; i < m.pass_count; i++) {
        const Material::AlphaPass &ap = m.passes[i];
        if (pass.kind == PassKind::first_draw) {
            // (each triangle's number, the least kept: no colour of the GS's, nothing else written)
            key.program = records ? first_id_rec_[v] : first_id_[v];
            key.ztest = m.ztest;
            key.zwrite = false;
            key.blend = true;
            key.src = key.src_alpha = key.dst = VK_BLEND_FACTOR_ONE;
            key.op = VK_BLEND_OP_MIN;
            key.mask = 0x1;
            c.set_program(key.program);
            c.set_depth_test(m.ztest, false);
            c.set_blend_enable(true);
            c.set_blend_factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);
            c.set_blend_op(VK_BLEND_OP_MIN, VK_BLEND_OP_MIN);
            c.set_color_write_mask(0x1);
        } else {
            bool early = ap.mode == 0 && !(m.push.flags & pcr::F_DATE);
            key.program = m.picture_layer               ? picture_layer_
                          : pass.kind == PassKind::darken ? (records ? first_rec_[v] : first_[v])
                          : early                         ? (records ? early_rec_[v] : early_[v])
                                                          : (records ? plain_rec_[v] : plain_[v]);
            key.ztest = m.ztest;
            key.zwrite = m.ztest && ap.zwrite;
            key.blend = m.blend.enable;
            key.src = m.blend.src;
            key.src_alpha = m.blend.src_alpha;
            key.dst = m.blend.dst;
            key.op = m.blend.op;
            key.mask = ap.mask;
            c.set_program(key.program);
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
        if (records) {
            push_record(c, rec + i);
            counts_.record_calls++;
        } else {
            push.flags = (m.push.flags & ~(3u << pcr::F_PASS_SHIFT)) | ap.mode << pcr::F_PASS_SHIFT;
            c.push_constants(&push, 0, sizeof(push));
        }
        count_call(key);
        if (d.native) {
            c.draw(d.triangle_count * 3, 1, d.first_triangle * 3, d.instance);
        } else {
            c.draw(d.vertex_count);
        }
    }
}

} // namespace scn
