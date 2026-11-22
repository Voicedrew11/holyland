// The scene renderer's drawing side (scn_draw.h)
#include "scn_draw.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "pcr_shaders.h"
#include "scn_shaders.h"

extern "C" {
#include "port/platform.h"
}
#include "port/scene_depth.h"

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
constexpr uint32_t jobs_binding = 11, placed_binding = 12; // (set 1's: scn_placed.h's, SCN_PLACED)
constexpr uint32_t vertex_input_count = pcr::vertex_input_count;
constexpr uint32_t scn_alpha_one = 0x1; // (pcr_common.h's SCN_ALPHA_ONE)

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
    // (the records: gl_DrawID in the vertex stage, and the fragment stage's subgroup's first record, pcr_common.h's
    // PCR_EACH_RECORD; the runs: indirect draws of several commands, each with its own first instance)
    const bool draw_ids = f.vk11_features.shaderDrawParameters;
    const bool ballot = (f.vk11_props.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0 &&
                        (f.vk11_props.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0;
    if (records_on_ && (!draw_ids || !ballot)) {
        fprintf(stderr, "[scn] no draw parameters or subgroup ballot in the fragment stage here: no records\n");
        records_on_ = false;
    }
    indirect_on_ = indirect_on_ && records_on_ && f.enabled_features.multiDrawIndirect &&
                   f.enabled_features.drawIndirectFirstInstance;
    max_run_ = indirect_on_ ? std::max(device_.get_gpu_properties().limits.maxDrawIndirectCount, 1u) : 1u;
    // (the player menu's Depth, --scene-depth, or developers' KF4_SCN_DEPTH=gpu and KF4_SCN_SETTLE=0|nearer:
    // port/scene_depth.h) exact Z needs the GPU's depth clamp and 64-bit integers (pcr_exact_z.h)
    const int mode = port_scene_depth_with_env(port_options.scene_depth, getenv("KF4_SCN_DEPTH"), getenv("KF4_SCN_SETTLE"));
    exact_z_ = f.enabled_features.depthClamp && f.enabled_features.shaderInt64 && mode != PORT_SCENE_DEPTH_GPU;
    settle_decals_ = exact_z_ && mode != PORT_SCENE_DEPTH_UNSETTLED;
    settle_nearer_ = exact_z_ && mode == PORT_SCENE_DEPTH_NEARER;
    fprintf(stderr, "[scn] depth: %s%s\n", port_scene_depth(mode)->name,
            !exact_z_ && mode != PORT_SCENE_DEPTH_GPU ? " asked, the GPU's here (no depth clamp or 64-bit integers)" : "");
    make_programs();
    if (const char *log = getenv("KF4_SCN_RUNS_LOG")) {
        runs_log_ = fopen(log, "w");
        if (runs_log_) {
            fprintf(runs_log_, "# vblank picture draws calls record_calls runs runs_280 end_pass end_state end_own "
                               "end_clear end_gif3d gif3d_splits records table cluts draw_calls indirect_calls "
                               "indirect_draws calls_gif calls_000 calls_280 calls_700 calls_ripple\n");
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
    // (0x000's and 0x280's placed vertices: SCN_PLACED)
    ResourceLayout placed_vert = batch_vert;
    placed_vert.sets[1].storage_buffer_mask |= 1u << jobs_binding | 1u << placed_binding;
    placed_vert.sets[1].meta[jobs_binding].array_size = 1;
    placed_vert.sets[1].meta[placed_binding].array_size = 1;
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
// (exact Z: the shader made with PCR_EXACT_Z, <name>_xz, where it is on)
#define SCN_PCR_SHADER_XZ(name, layout)                                                                                \
    (exact_z_ ? SCN_PCR_SHADER(name##_xz, layout) : SCN_PCR_SHADER(name, layout))
    Shader *fs = SCN_PCR_SHADER_XZ(pcr_prim_frag, &frag), *efs = SCN_PCR_SHADER_XZ(pcr_prim_early_frag, &frag);
    Shader *idfs = device_.request_shader(scn_spv_scn_first_draw_frag, sizeof(scn_spv_scn_first_draw_frag), &frag);
    Shader *firstfs = device_.request_shader(scn_spv_scn_prim_first_frag, sizeof(scn_spv_scn_prim_first_frag), &first);
    Shader *vs[size_t(Program::count)] = {
        SCN_PCR_SHADER_XZ(pcr_prim_vert, &vert),      SCN_PCR_SHADER(pcr_nd000_vert, &batch_vert),
        SCN_PCR_SHADER(pcr_nd280_vert, &batch_vert),  SCN_PCR_SHADER(pcr_nd700_vert, &batch_vert),
        SCN_PCR_SHADER(pcr_ndripple_vert, &batch_vert),
    };
    // (the records' programs: the PC renderer's shaders and the scene's two DATE shaders made with PCR_RECORDS and
    // PCR_DRAWS_APART, each record a draw of its own: scn_shaders.h's)
#define SCN_REC_SHADER_ONE(name, layout) device_.request_shader(scn_spv_scn_##name, sizeof(scn_spv_scn_##name), layout)
#define SCN_REC_SHADER(name, layout)                                                                                   \
    (exact_z_ ? SCN_REC_SHADER_ONE(name##_xz, layout) : SCN_REC_SHADER_ONE(name, layout))
    Shader *fs_rec = SCN_REC_SHADER(pcr_prim_frag_rec, &frag), *efs_rec = SCN_REC_SHADER(pcr_prim_early_frag_rec, &frag);
    Shader *idfs_rec = SCN_REC_SHADER(first_draw_frag_rec, &frag);
    Shader *firstfs_rec = SCN_REC_SHADER(prim_first_frag_rec, &first);
    Shader *vs_rec[size_t(Program::count)] = {
        SCN_REC_SHADER(pcr_prim_vert_rec, &vert),        SCN_REC_SHADER(pcr_nd000_vert_rec, &batch_vert),
        SCN_REC_SHADER(pcr_nd280_vert_rec, &batch_vert), SCN_REC_SHADER(pcr_nd700_vert_rec, &batch_vert),
        SCN_REC_SHADER(pcr_ndripple_vert_rec, &batch_vert),
    };
    if (placed_on_) {
        vs_rec[size_t(Program::nd000)] = SCN_REC_SHADER(pcr_nd000_vert_placed, &placed_vert);
        vs_rec[size_t(Program::nd280)] = SCN_REC_SHADER(pcr_nd280_vert_placed, &placed_vert);
        // (the placing: set 0 as the vertex shader's, the picture and the program's arena and records; set 1 the jobs
        // and the placed vertices)
        const std::array<std::pair<const uint32_t *, size_t>, 2> code = {{
            {scn_spv_scn_place_000_comp, sizeof(scn_spv_scn_place_000_comp)},
            {scn_spv_scn_place_280_comp, sizeof(scn_spv_scn_place_280_comp)},
        }};
        for (uint32_t p = 0; p < 2; p++) {
            ResourceLayout layout = {};
            layout.push_constant_size = sizeof(uint32_t);
            layout.sets[0].storage_buffer_mask = 1u | 3u << (3 + 3 * p);
            for (uint32_t b : {0u, 3 + 3 * p, 4 + 3 * p}) {
                layout.sets[0].meta[b].array_size = 1;
            }
            layout.sets[1].storage_buffer_mask = 3u;
            layout.sets[1].meta[0].array_size = 1;
            layout.sets[1].meta[1].array_size = 1;
            Shader *shader = device_.request_shader(code[p].first, code[p].second, &layout);
            place_[p] = shader ? device_.request_program(shader) : nullptr;
        }
    }
    if (getenv("KF4_SCN_ND_EMPTY") && atoi(getenv("KF4_SCN_ND_EMPTY")) != 0) {
        // (developers': the bound of the native draws' maths, nothing drawn by them)
        vs_rec[size_t(Program::nd000)] = SCN_REC_SHADER_ONE(pcr_nd000_vert_empty, &batch_vert);
        vs_rec[size_t(Program::nd280)] = SCN_REC_SHADER_ONE(pcr_nd280_vert_empty, &batch_vert);
    }
    // (slice 3d: program 0x280's two passes as one draw, pcr_prim.h's SCN_FUSE_280; the early tests: nothing discarded)
    fused_280_ = device_.request_program(SCN_REC_SHADER(pcr_nd280_vert_fuse, &batch_vert),
                                         SCN_REC_SHADER(pcr_prim_early_frag_fuse, &frag));
#undef SCN_REC_SHADER
#undef SCN_REC_SHADER_ONE
#undef SCN_PCR_SHADER_XZ
#undef SCN_PCR_SHADER
    Shader *layerfs = device_.request_shader(scn_spv_scn_picture_layer_frag, sizeof(scn_spv_scn_picture_layer_frag),
                                             &frag);
    picture_layer_ = device_.request_program(vs[size_t(Program::gif)], layerfs);
    if (exact_z_) {
        Shader *resetfs = device_.request_shader(pcr_spv_pcr_depth_reset_frag, sizeof(pcr_spv_pcr_depth_reset_frag), &frag);
        for (size_t v = 0; v < size_t(Program::count); v++) {
            depth_reset_rec_[v] = device_.request_program(vs_rec[v], resetfs);
        }
    }
    Shader *flatfs = device_.request_shader(scn_spv_scn_flat_frag, sizeof(scn_spv_scn_flat_frag), &frag);
    for (size_t v = 0; v < size_t(Program::count); v++) {
        flat_[v] = device_.request_program(vs[v], flatfs);
        flat_rec_[v] = device_.request_program(vs_rec[v], flatfs);
    }
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
    // (slice 3d) a first pass fused with its sphere pass is drawn from the records, the sphere's with it, or not at all
    for (size_t i = 0; i < scene.materials.size(); i++) {
        uint32_t f = scene.materials[i].fuse;
        if (f != no_material && eligible[i] && !eligible[f]) {
            eligible[i] = 0;
            counts_.fused_unrecorded++;
        }
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
    // the GIF's primitives' corners, for the draws from the records (one buffer: a run's draws from it)
    if (!scene.vertices.empty()) {
        BufferCreateInfo info = {};
        info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
        info.size = scene.vertices.size() * sizeof(scene.vertices[0]);
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        p.vertices = device_.create_buffer(info, scene.vertices.data());
        if (!p.vertices) {
            return;
        }
    }
    // a record a draw's pass, in the draws' order
    std::vector<pcr::PrimPush> records;
    records.reserve(scene.draws.size() + scene.draws.size() / 4);
    auto record_of = [&](const Material &m, uint32_t mi) {
        pcr::PrimPush r = m.push;
        r.table = table_of[mi] == no_record ? 0xFFFFu : table_of[mi];
        if (m.sampled && table_of[mi] != no_record && r.src_offset[0] == 0) {
            // (a sampled texture's size from its record: pcr_prim.h's sampled_size; one already there is the texture's
            // own, its image finer: scn_build.cpp's font sheets)
            r.src_offset[0] = int32_t(m.sampled->get_width());
            r.src_offset[1] = int32_t(m.sampled->get_height());
        }
        r.clut_at = m.clut != no_clut ? m.clut * 256u : 0xFFFFFFFFu;
        r.merged = 0;
        r.factors = 0;
        return r;
    };
    // (slice 3d) the fused first passes' records, to point at their sphere pass's (after the draws' records: merged)
    std::vector<std::pair<uint32_t, uint32_t>> fused; // (the first's record, the sphere's material)
    for (size_t di = 0; di < scene.draws.size(); di++) {
        const Draw &d = scene.draws[di];
        if (d.clear || !eligible[d.material]) {
            continue;
        }
        const Material &m = scene.materials[d.material];
        p.first_record[di] = uint32_t(records.size());
        pcr::PrimPush r = record_of(m, d.material);
        if (d.shadow != no_shadow) {
            r.record = d.shadow;
        }
        if (m.fuse != no_material) {
            fused.push_back({uint32_t(records.size()), m.fuse});
        }
        for (uint32_t i = 0; i < m.pass_count; i++) {
            r.flags = (m.push.flags & ~(3u << pcr::F_PASS_SHIFT)) | m.passes[i].mode << pcr::F_PASS_SHIFT;
            r.factors = alpha_one(m, i) ? scn_alpha_one : 0;
            records.push_back(r);
        }
    }
    // the sphere passes' records, one a material, in pcr::PrimPush::merged of their first's (pcr_prim.h's SCN_FUSE_280)
    if (!fused.empty()) {
        std::vector<uint32_t> sphere_record(scene.materials.size(), no_record);
        for (const auto &[at, sm] : fused) {
            if (sphere_record[sm] == no_record) {
                const Material &m = scene.materials[sm];
                sphere_record[sm] = uint32_t(records.size());
                pcr::PrimPush r = record_of(m, sm);
                r.flags = m.push.flags; // (its alpha test's pass mode its own: fuse_280)
                records.push_back(r);
            }
            records[at].merged = sphere_record[sm];
        }
    }
    // (exact Z's settled decals: every record again without PCR_F_DECAL, record r's at r + own_offset, so that a run's
    // own depth is drawn from consecutive records as its colour is: settle_run)
    p.own_offset = 0;
    if (std::any_of(scene.materials.begin(), scene.materials.end(), [](const Material &m) { return m.settle; })) {
        p.own_offset = uint32_t(records.size());
        records.reserve(2 * records.size());
        for (uint32_t i = 0; i < p.own_offset; i++) {
            pcr::PrimPush own = records[i];
            // (no plane worked out: the GPU's own depth)
            own.flags = (own.flags & ~(pcr::F_DECAL | pcr::F_EXACT_Z)) | pcr::F_SCN_OWN_DEPTH;
            records.push_back(own);
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
    // the runs' commands: a call from the records each at most
    if (indirect_on_) {
        info.size = records.size() * sizeof(VkDrawIndirectCommand);
        info.domain = BufferDomain::LinkedDeviceHost;
        info.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        p.commands = device_.create_buffer(info, nullptr);
        p.command_map = p.commands ? static_cast<VkDrawIndirectCommand *>(
                                         device_.map_host_buffer(*p.commands, MEMORY_ACCESS_WRITE_BIT))
                                   : nullptr;
        if (p.command_map) {
            p.command_capacity = uint32_t(records.size());
        } else {
            p.commands.reset();
        }
    }
    counts_.records += records.size();
    counts_.table_entries += views.size();
    counts_.table_max = std::max<uint64_t>(counts_.table_max, views.size());
    counts_.cluts += scene.cluts.size();
}

// (an unblended pass that writes no alpha: its alpha out 1.0 from its record, pcr_common.h's SCN_ALPHA_ONE)
bool Drawer::alpha_one(const Material &m, uint32_t pass) {
    return !m.blend.enable && (m.passes[pass].mask & 0x8u) == 0;
}

bool Drawer::same_state(const RunKey &a, const RunKey &b) {
    return a.program == b.program && a.ztest == b.ztest && a.zwrite == b.zwrite && a.zcompare == b.zcompare &&
           a.blend == b.blend &&
           (!a.blend || (a.src == b.src && a.src_alpha == b.src_alpha && a.dst == b.dst && a.dst_alpha == b.dst_alpha &&
                         a.op == b.op && a.op_alpha == b.op_alpha)) &&
           a.mask == b.mask && memcmp(a.constants, b.constants, sizeof(a.constants)) == 0 &&
           memcmp(&a.viewport, &b.viewport, sizeof(a.viewport)) == 0 &&
           memcmp(&a.scissor, &b.scissor, sizeof(a.scissor)) == 0 &&
           memcmp(a.native, b.native, sizeof(a.native)) == 0 && a.gif_3d == b.gif_3d && a.stencil == b.stencil;
}

// A key in one form for the same pixels: the alpha's blend where the alpha is not written, and the blend constants where
// no factor reads them, as unblended's (the call's state is set from the key in this form, set_call_state; the blend
// constants are the material's, set_draw_state, read by no factor where the key has them 0)
void Drawer::canonical(RunKey &k) {
    const RunKey none;
    if (!k.blend) {
        k.src = none.src;
        k.src_alpha = none.src_alpha;
        k.dst = none.dst;
        k.dst_alpha = none.dst_alpha;
        k.op = none.op;
        k.op_alpha = none.op_alpha;
    } else if ((k.mask & 0x8u) == 0) {
        k.src_alpha = VK_BLEND_FACTOR_ONE;
        k.dst_alpha = VK_BLEND_FACTOR_ZERO;
        k.op_alpha = VK_BLEND_OP_ADD;
    }
    auto constant = [](VkBlendFactor f) {
        return f >= VK_BLEND_FACTOR_CONSTANT_COLOR && f <= VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    };
    if (!k.blend || !(constant(k.src) || constant(k.dst) || constant(k.src_alpha) || constant(k.dst_alpha))) {
        memset(k.constants, 0, sizeof(k.constants));
    }
}

// Whether a call of key draws the same pixels in a run of state run, run then the state for both: the same state, or
// a run blended by the source's alpha (Cs As + Cd (1 - As)) and an unblended call whose alpha out is 1.0 and is not
// written (alpha_one, the record's SCN_ALPHA_ONE: Cs 1 + Cd 0 is Cs), either way round
bool Drawer::merge(RunKey &run, const RunKey &key) {
    if (same_state(run, key)) {
        run.alpha_one = run.alpha_one && key.alpha_one;
        return true;
    }
    auto by_alpha = [](const RunKey &k) {
        return k.blend && k.src == VK_BLEND_FACTOR_SRC_ALPHA && k.dst == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA &&
               k.op == VK_BLEND_OP_ADD && (k.mask & 0x8u) == 0;
    };
    const RunKey *blended = nullptr, *plain = nullptr;
    if (by_alpha(run) && key.alpha_one) {
        blended = &run;
        plain = &key;
    } else if (run.alpha_one && by_alpha(key)) {
        blended = &key;
        plain = &run;
    } else {
        return false;
    }
    RunKey as = *plain; // (the unblended call as blended)
    as.blend = true;
    as.src = blended->src;
    as.src_alpha = blended->src_alpha;
    as.dst = blended->dst;
    as.dst_alpha = blended->dst_alpha;
    as.op = blended->op;
    as.op_alpha = blended->op_alpha;
    memcpy(as.constants, blended->constants, sizeof(as.constants));
    if (!same_state(as, *blended)) {
        return false;
    }
    RunKey merged = *blended;
    merged.alpha_one = false;
    run = merged;
    return true;
}

// A call into the runs: a run goes on while the call merges with its state (merge), else a new one starts and what
// ended the last is counted
void Drawer::count_call(const RunKey &key) {
    counts_.calls++;
    RunState &r = run_;
    auto same = [](const RunKey &a, const RunKey &b) {
        RunKey m = a;
        return merge(m, b);
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
    RunKey run = r.last;
    if (!r.open) {
        end = end_pass;
    } else if (r.after_clear) {
        end = end_clear;
    } else if (key.own || r.last.own) {
        end = end_own;
    } else if (!merge(run, key)) {
        end = key.gif_3d || r.last.gif_3d ? end_gif3d : end_state;
    }
    if (end != end_count) {
        counts_.runs++;
        counts_.runs_280 += key.n280 ? 1 : 0;
        counts_.ends[end]++;
    }
    r.open = true;
    r.after_clear = false;
    r.last = end != end_count ? key : run;
    r.call = key;
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
    fprintf(runs_log_, " %llu %llu %llu %llu %llu %llu %llu", (unsigned long long)(a.gif3d_splits - b.gif3d_splits),
            (unsigned long long)(a.records - b.records), (unsigned long long)(a.table_entries - b.table_entries),
            (unsigned long long)(a.cluts - b.cluts), (unsigned long long)(a.draw_calls - b.draw_calls),
            (unsigned long long)(a.indirect_calls - b.indirect_calls),
            (unsigned long long)(a.indirect_draws - b.indirect_draws));
    for (size_t i = 0; i < a.draw_calls_by.size(); i++) {
        fprintf(runs_log_, " %llu", (unsigned long long)(a.draw_calls_by[i] - b.draw_calls_by[i]));
    }
    fprintf(runs_log_, "\n");
}

void Drawer::record(const Scene &scene, CommandBuffer &c, const PictureDraw *picture,
                    const std::vector<uint8_t> *left_out, const Substitutes *substitutes) {
    picture_ = picture;
    left_out_ = left_out;
    substitutes_ = substitutes;
    const Counts before = counts_;
    counts_.scenes++;
    counts_.picture_scenes += picture ? 1 : 0;
    prepare(scene);
    prepare_placing(scene);
    bool after_pass = false;
    full_barrier(c); // (what was submitted before: the targets' last scene, uploads into images)
    place_vertices(c);
    for (size_t si = 0; si < scene.steps.size(); si++) {
        const Step &step = scene.steps[si];
        if (left_out && si < left_out->size() && (*left_out)[si]) {
            continue;
        }
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
    if (prepared_.command_map) {
        device_.unmap_host_buffer(*prepared_.commands, MEMORY_ACCESS_WRITE_BIT);
        prepared_.command_map = nullptr;
    }
    if (runs_log_ && !picture) {
        log_scene(scene, before);
    }
    picture_ = nullptr;
    left_out_ = nullptr;
    substitutes_ = nullptr;
}

const Image &Drawer::image(const ImageHandle &h) const {
    if (picture_ && picture_->stand_ins) {
        auto it = picture_->stand_ins->find(h.get());
        if (it != picture_->stand_ins->end()) {
            return *it->second;
        }
    }
    if (substitutes_) {
        auto it = substitutes_->find(h.get());
        if (it != substitutes_->end()) {
            return *it->second;
        }
    }
    return *h;
}

const Buffer *Drawer::picture_buffer(uint32_t binding) const {
    const pcr::Drawing::PictureBuffers &b = picture_->buffers;
    const Buffer *buffer = binding == 0 ? b.picture : binding == 1 ? b.kicks : b.moves;
    return buffer ? buffer : zeros_.get();
}

uint32_t Drawer::instance_of(const Draw &d) const {
    return picture_ && d.picture_instance != 0xFFFFFFFFu ? d.picture_instance : d.instance;
}

uint32_t Drawer::first_instance(uint32_t index, const Draw &d) const {
    const uint32_t job = index < prepared_.job_of.size() ? prepared_.job_of[index] : no_record;
    return job != no_record ? job : instance_of(d);
}

// The scene's jobs: each native draw of 0x000 or 0x280 drawn from the records (their programs the placed vertices'),
// but a fused first pass (its own program), its run of vertices with the instance it is drawn with; a run met again
// (the same draw in another pass: DATE's shadows, the first-draw image) the same job. Laid out by the program's
// buffers, a dispatch each.
void Drawer::prepare_placing(const Scene &scene) {
    Prepared &p = prepared_;
    p.job_of.assign(scene.draws.size(), no_record);
    if (!placed_on_ || !place_[0] || !place_[1]) {
        return;
    }
    struct Key {
        const Buffer *arena, *records;
        uint32_t first, count, instance;
        bool operator==(const Key &o) const {
            return arena == o.arena && records == o.records && first == o.first && count == o.count &&
                   instance == o.instance;
        }
    };
    struct KeyHash {
        size_t operator()(const Key &k) const {
            uint64_t h = uint64_t(k.first) * 0x9E3779B97F4A7C15ull ^ uint64_t(k.instance) * 0xC2B2AE3D27D4EB4Full ^
                         uint64_t(k.count) << 40 ^ reinterpret_cast<uintptr_t>(k.records) ^
                         reinterpret_cast<uintptr_t>(k.arena) >> 4;
            return size_t(h ^ h >> 29);
        }
    };
    struct Pending {
        uint32_t slot;
        Key key;
    };
    std::vector<Pending> pending;
    std::vector<uint32_t> pending_of(scene.draws.size(), no_record);
    std::unordered_map<Key, uint32_t, KeyHash> seen;
    // (the draws of the steps left out: not placed)
    std::vector<uint8_t> drawn;
    if (left_out_) {
        drawn.assign(scene.draws.size(), 1);
        for (size_t si = 0; si < scene.steps.size() && si < left_out_->size(); si++) {
            const Step &st = scene.steps[si];
            if ((*left_out_)[si] && st.kind == Step::Kind::pass) {
                const Pass &pass = scene.passes[st.index];
                for (uint32_t i = 0; i < pass.draw_count; i++) {
                    drawn[pass.first_draw + i] = 0;
                }
            }
        }
    }
    for (size_t di = 0; di < scene.draws.size(); di++) {
        const Draw &d = scene.draws[di];
        if ((!drawn.empty() && !drawn[di]) || d.clear || !d.native || p.first_record[di] == no_record ||
            d.native_vertex_count == 0 ||
            scene.materials[d.material].fuse != no_material) {
            continue;
        }
        uint32_t slot = native_slot(d.native_program);
        if (slot > 1) {
            continue;
        }
        Key key = {d.arena.get(), d.records.get(), d.native_first_vertex, d.native_vertex_count, instance_of(d)};
        auto [it, fresh] = seen.try_emplace(key, uint32_t(pending.size()));
        if (fresh) {
            pending.push_back({slot, key});
        } else {
            counts_.place_shared++;
        }
        pending_of[di] = it->second;
    }
    if (pending.empty()) {
        return;
    }
    // (in order of their buffers: a dispatch a program's arena and records)
    std::vector<uint32_t> order(pending.size());
    for (uint32_t i = 0; i < order.size(); i++) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const Pending &x = pending[a], &y = pending[b];
        if (x.slot != y.slot) {
            return x.slot < y.slot;
        }
        if (x.key.arena != y.key.arena) {
            return x.key.arena < y.key.arena;
        }
        return x.key.records < y.key.records;
    });
    std::vector<uint32_t> job_at(pending.size());
    std::vector<uint32_t> jobs;
    jobs.reserve(pending.size() * 4);
    uint32_t vertices = 0;
    for (uint32_t i = 0; i < order.size(); i++) {
        const Pending &e = pending[order[i]];
        job_at[order[i]] = i;
        if (p.placings.empty() || p.placings.back().slot != e.slot || p.placings.back().arena != e.key.arena ||
            p.placings.back().records != e.key.records) {
            p.placings.push_back({e.slot, e.key.arena, e.key.records, i, 0});
        }
        p.placings.back().count++;
        jobs.insert(jobs.end(), {e.key.instance, e.key.first, vertices, e.key.count});
        vertices += e.key.count;
    }
    BufferCreateInfo info = {};
    info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
    info.size = jobs.size() * sizeof(uint32_t);
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    p.jobs = device_.create_buffer(info, jobs.data());
    info.domain = BufferDomain::Device;
    info.size = VkDeviceSize(vertices) * 80; // (scn_placed.h's Placed)
    p.placed = p.jobs ? device_.create_buffer(info, nullptr) : BufferHandle();
    if (!p.jobs || !p.placed) {
        p.jobs.reset();
        p.placed.reset();
        p.placings.clear();
        return;
    }
    for (size_t di = 0; di < scene.draws.size(); di++) {
        if (pending_of[di] != no_record) {
            p.job_of[di] = job_at[pending_of[di]];
        }
    }
    counts_.place_jobs += pending.size();
    counts_.placed_vertices += vertices;
}

// The jobs' vertices placed (scn_place_*.comp), before the passes' vertex shaders read them
void Drawer::place_vertices(CommandBuffer &c) {
    const Prepared &p = prepared_;
    if (p.placings.empty()) {
        return;
    }
    for (const Prepared::Placing &pl : p.placings) {
        times_.begin(c, pl.slot == 0 ? pcr::GpuWork::native_000 : pcr::GpuWork::native_280);
        uint32_t at = 3 + 3 * pl.slot;
        c.set_program(place_[pl.slot]);
        c.set_storage_buffer(0, 0, picture_ ? *picture_buffer(0) : *zeros_);
        c.set_storage_buffer(0, at, *pl.arena);
        c.set_storage_buffer(0, at + 1, *pl.records);
        c.set_storage_buffer(1, 0, *p.jobs);
        c.set_storage_buffer(1, 1, *p.placed);
        for (uint32_t first = 0; first < pl.count; first += 65535) {
            uint32_t job = pl.first_job + first;
            c.push_constants(&job, 0, sizeof(job));
            c.dispatch(std::min(pl.count - first, 65535u), 1, 1);
            counts_.place_dispatches++;
        }
    }
    c.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
              VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void Drawer::record_op(const Op &op, CommandBuffer &c) {
    counts_.ops++;
    times_.begin(c, op.work);
    const VkImageSubresourceLayers layers = {
        VkImageAspectFlags(op.depth ? depth_aspects(image(op.dst).get_format()) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT)), 0, 0, 1};
    switch (op.kind) {
    case Op::Kind::clear:
        if (op.depth) {
            c.clear_image(image(op.dst), op.value, depth_aspects(image(op.dst).get_format()));
        } else {
            c.clear_image(image(op.dst), op.value);
        }
        break;
    case Op::Kind::copy:
        c.copy_image(image(op.dst), image(op.src), op.dst_at, op.src_at, op.extent, layers, layers);
        break;
    case Op::Kind::blit:
        c.blit_image(image(op.dst), image(op.src), op.dst_at,
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
        rp.color_attachments[0] = &image(pass.colour).get_view();
        rp.store_attachments = 1;
        if (pass.clear_colour) {
            rp.clear_attachments = 1;
            rp.clear_color[0] = pass.colour_value;
        } else {
            rp.load_attachments = 1;
        }
    }
    if (pass.depth) {
        rp.depth_stencil = &image(pass.depth).get_view();
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
    pending_ = Pending();
    uint32_t set = no_material; // (the material whose state the command buffer has: a draw of it again sets nothing)
    for (uint32_t i = 0; i < pass.draw_count; i++) {
        const Draw &d = scene.draws[pass.first_draw + i];
        record_draw(scene, pass, pass.first_draw + i, c, set);
        set = d.clear || scene.materials[d.material].pass_count != 1 ? no_material : d.material;
    }
    flush_run(c);
    c.end_render_pass();
}

// The records' bindings (a draw from the scene's records): set 0's native buffers for its program and the scene's
// CLUTs and samplers, set 1's dummies but for the records and the pass's first-draw image, set 2 the scene's table.
// The same resources again bind nothing (Granite compares them).
void Drawer::bind_records(const Pass &pass, const Draw &d, CommandBuffer &c) {
    const Prepared &p = prepared_;
    for (uint32_t b = 0; b < 3; b++) {
        c.set_storage_buffer(0, b, picture_ ? *picture_buffer(b) : *zeros_);
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
    c.set_storage_buffer(1, jobs_binding, p.jobs ? *p.jobs : *zeros_);
    c.set_storage_buffer(1, placed_binding, p.placed ? *p.placed : *zeros_);
    c.set_storage_buffer(1, merged_binding, *zeros_);
    c.set_storage_buffer(1, planes_binding, *zeros_);
    c.set_texture(1, 0, dummy_uint_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, 1, dummy_float_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, 4, dummy_float_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, 3, dummy_float_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, first_binding, pass.first ? image(pass.first).get_view() : dummy_float_->get_view(),
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
        c.set_storage_buffer(0, b, picture_ ? *picture_buffer(b) : *zeros_);
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
    c.set_texture(1, 1, m.surface ? image(m.surface).get_view() : dummy_float_->get_view(), StockSampler::NearestClamp);
    if (m.sampled && m.sampler) {
        c.set_texture(1, 4, m.sampled->get_view(), *m.sampler);
    } else {
        c.set_texture(1, 4, dummy_float_->get_view(), StockSampler::NearestClamp);
    }
    c.set_texture(1, 3, m.dest ? image(m.dest).get_view() : dummy_float_->get_view(), StockSampler::NearestClamp);
    c.set_texture(1, first_binding, pass.first ? image(pass.first).get_view() : dummy_float_->get_view(),
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

// (the draw calls by program, Counts::draw_calls_by: the GIF's, then the native programs' as their slots)
static uint32_t call_slot(const Draw &d) {
    return d.native ? 1 + native_slot(d.native_program) : 0;
}

// A call joins the open run where the run counting would have it one (count_call: the same state, no clear or draw of
// its own between, the same pass) and its record is the next: the run's records are the push constants' first plus
// gl_DrawID
bool Drawer::joins(const RunKey &key, uint32_t record) const {
    const Pending &r = pending_;
    return r.open && !run_.after_clear && prepared_.command_map && prepared_.commands_used < prepared_.command_capacity &&
           r.count < max_run_ && record == r.first_record + r.count && [&] {
               RunKey m = r.key;
               return merge(m, key);
           }();
}

// (the call's state and push constants are the command buffer's: set by the caller where it opens a run)
void Drawer::add_to_run(const RunKey &key, uint32_t record, const VkDrawIndirectCommand &command, uint32_t slot,
                        CommandBuffer &c) {
    Prepared &p = prepared_;
    Pending &r = pending_;
    if (r.open) {
        // (the run's state, the blend by the source's alpha where the call brings it: set now, the run not yet drawn)
        RunKey m = r.key;
        merge(m, key);
        if (m.blend != r.key.blend) {
            set_call_state(m, c);
        }
        r.key = m;
    } else {
        r.open = true;
        r.key = key;
        r.first_record = record;
        r.first_command = p.commands_used;
        r.count = 0;
        r.slot = slot;
        r.first = command;
    }
    if (p.command_map && p.commands_used < p.command_capacity) {
        p.command_map[p.commands_used++] = command;
    }
    r.count++;
}

void Drawer::flush_run(CommandBuffer &c) {
    Pending &r = pending_;
    if (!r.open) {
        return;
    }
    r.open = false;
    if (r.key.stencil == RunKey::Stencil::own_first) {
        own_depth_first(r, c);
    }
    draw_run(r, c);
    if (r.key.stencil == RunKey::Stencil::mark_won) {
        settle_run(r, c);
    } else if (r.key.stencil == RunKey::Stencil::own_after) {
        own_depth_after(r, c);
    }
}

void Drawer::draw_run(const Pending &r, CommandBuffer &c) {
    if (r.count == 1) {
        c.draw(r.first.vertexCount, r.first.instanceCount, r.first.firstVertex, r.first.firstInstance);
    } else {
        c.draw_indirect(*prepared_.commands, VkDeviceSize(r.first_command) * sizeof(VkDrawIndirectCommand), r.count,
                        sizeof(VkDrawIndirectCommand));
        counts_.indirect_calls++;
        counts_.indirect_draws += r.count;
    }
    counts_.draw_calls++;
    counts_.draw_calls_by[r.slot]++;
}

void Drawer::set_draw_state(const Scene &scene, const Pass &pass, const Draw &d, const Material &m, bool records,
                            CommandBuffer &c) {
    counts_.state_set++;
    c.set_viewport(d.viewport);
    c.set_scissor(tiny_on_ && pass.kind == PassKind::plain && !m.picture_layer ? VkRect2D{d.scissor.offset, {1, 1}}
                                                                             : d.scissor);
    pcr::ShaderState st = m.state;
    st.first_draw = pass.kind == PassKind::first_draw ? 1 : pass.kind == PassKind::darken ? 2 : 0;
    times_.begin_draw(c, pass.picture ? pcr::GpuWork::picture_layers : work_of(d), pcr::shader_state_key(st));
    if (records) {
        bind_records(pass, d, c);
    } else {
        bind_own(scene, pass, d, m, c);
    }
    set_shared_pipeline_state(m.zcompare, m.constants, c);
    if (!d.native) {
        if (records) {
            // (the scene's corners: the draw's first is its command's first vertex)
            set_vertex_input(*prepared_.vertices, c);
        } else {
            size_t bytes = size_t(d.vertex_count) * sizeof(pcr::Vertex);
            memcpy(c.allocate_vertex_data(0, bytes, sizeof(pcr::Vertex)), scene.vertices.data() + d.first_vertex,
                   bytes);
            for (uint32_t i = 0; i < vertex_input_count; i++) {
                c.set_vertex_attrib(i, 0, pcr::vertex_inputs[i].format, pcr::vertex_inputs[i].offset);
            }
        }
    }
}

void Drawer::set_shared_pipeline_state(VkCompareOp zcompare, const float constants[4], CommandBuffer &c) {
    c.set_opaque_state();
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_compare(zcompare);
    c.set_blend_constants(constants);
}

void Drawer::set_vertex_input(const Buffer &vertices, CommandBuffer &c) {
    c.set_vertex_binding(0, vertices, 0, sizeof(pcr::Vertex));
    for (uint32_t i = 0; i < vertex_input_count; i++) {
        c.set_vertex_attrib(i, 0, pcr::vertex_inputs[i].format, pcr::vertex_inputs[i].offset);
    }
}

void Drawer::set_call_state(const RunKey &key, CommandBuffer &c) {
    c.set_program(key.program);
    c.set_depth_test(key.ztest, key.zwrite);
    c.set_blend_enable(key.blend);
    // (an unblended call's factors set too without dynamic blend, canonical's: they are in the pipeline's state then,
    // and a call's pipeline is its key's alone, not the call before's too, as pipeline_set lists them)
    if (key.blend || !dynamic_blend_) {
        c.set_blend_factors(key.src, key.src_alpha, key.dst, key.dst_alpha);
        c.set_blend_op(key.op, key.op_alpha);
    }
    c.set_color_write_mask(key.mask);
    // (exact Z's settle: the stencil's decal_won bit, pcr_canvas.cpp's mark_decal_won and settle_decal_depth)
    c.set_stencil_test(key.stencil == RunKey::Stencil::mark_won || key.stencil == RunKey::Stencil::reset_won);
    if (key.stencil == RunKey::Stencil::mark_won) {
        c.set_stencil_ops(VK_COMPARE_OP_ALWAYS, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
        c.set_stencil_reference(decal_won, decal_won, decal_won);
    } else if (key.stencil == RunKey::Stencil::reset_won) {
        c.set_stencil_ops(VK_COMPARE_OP_EQUAL, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP);
        c.set_stencil_reference(decal_won, decal_won, decal_won);
    }
}

void Drawer::record_draw(const Scene &scene, const Pass &pass, uint32_t index, CommandBuffer &c, uint32_t set) {
    counts_.draws++;
    const Draw &d = scene.draws[index];
    const Material &m = scene.materials[d.material];
    const uint32_t rec = prepared_.first_record[index];
    const uint32_t slot = call_slot(d);
    if (!d.clear && d.native && set == d.material) {
        // (the draw before's material: its state, bindings and constants are the command buffer's; its record, or a
        // shadow's number)
        counts_.native_draws++;
        counts_.first_draws += pass.kind == PassKind::first_draw ? 1 : 0;
        counts_.darkened += pass.kind == PassKind::darken ? 1 : 0;
        const RunKey key = run_.call;
        const VkDrawIndirectCommand command = {native_corners(d), 1, d.first_triangle * 3,
                                               rec != no_record ? first_instance(index, d) : instance_of(d)};
        if (rec != no_record) {
            if (!joins(key, rec)) {
                flush_run(c);
                set_call_state(key, c); // (the run before may have been blended by its calls: merge)
                push_record(c, rec);
            }
            counts_.record_calls++;
            count_call(key);
            add_to_run(key, rec, command, slot, c);
            return;
        }
        flush_run(c);
        if (d.shadow != no_shadow) {
            pcr::PrimPush push = m.push;
            push.flags = (m.push.flags & ~(3u << pcr::F_PASS_SHIFT)) | m.passes[0].mode << pcr::F_PASS_SHIFT;
            push.record = d.shadow;
            c.push_constants(&push, 0, sizeof(push));
        }
        count_call(key);
        c.draw(command.vertexCount, 1, command.firstVertex, command.firstInstance);
        counts_.draw_calls++;
        counts_.draw_calls_by[slot]++;
        return;
    }
    if (d.clear) {
        flush_run(c);
        counts_.state_set++;
        c.set_viewport(d.viewport);
        c.set_scissor(d.scissor);
        pcr::ShaderState clear = {};
        clear.first_draw = 3;
        times_.begin_draw(c, work_of(d), pcr::shader_state_key(clear));
        counts_.clears++;
        run_.after_clear = true;
        c.clear_quad(d.clear_rect, d.clear_att, d.clear_count);
        return;
    }
    counts_.native_draws += d.native ? 1 : 0;
    counts_.gif_draws += d.native ? 0 : 1;
    counts_.first_draws += pass.kind == PassKind::first_draw ? 1 : 0;
    counts_.darkened += pass.kind == PassKind::darken ? 1 : 0;
    size_t v = size_t(m.program);
    const bool records = rec != no_record;
    const VkDrawIndirectCommand command =
        d.native ? VkDrawIndirectCommand{native_corners(d), 1, d.first_triangle * 3,
                                         records ? first_instance(index, d) : instance_of(d)}
                 : VkDrawIndirectCommand{d.vertex_count, 1, records ? d.first_vertex : 0, 0};
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
    bool state_set = false; // (the state the draw's calls share: set once, by the first call that opens a run)
    for (uint32_t i = 0; i < m.pass_count; i++) {
        const Material::AlphaPass &ap = m.passes[i];
        if (pass.kind == PassKind::first_draw) {
            // (each triangle's number, the least kept: no colour of the GS's, nothing else written)
            key.program = records ? first_id_rec_[v] : first_id_[v];
            key.ztest = m.ztest;
            key.zwrite = false;
            key.blend = true;
            key.src = key.src_alpha = key.dst = key.dst_alpha = VK_BLEND_FACTOR_ONE;
            key.op = key.op_alpha = VK_BLEND_OP_MIN;
            key.mask = 0x1;
        } else {
            bool early = ap.mode == 0 && !(m.push.flags & pcr::F_DATE);
            key.program = m.fuse != no_material && records ? fused_280_
                          : m.picture_layer               ? picture_layer_
                          : pass.kind == PassKind::darken ? (records ? first_rec_[v] : first_[v])
                          : early                         ? (records ? early_rec_[v] : early_[v])
                                                          : (records ? plain_rec_[v] : plain_[v]);
            key.ztest = m.ztest;
            key.zwrite = m.ztest && ap.zwrite;
            key.blend = m.blend.enable;
            key.src = m.blend.src;
            key.src_alpha = m.blend.src_alpha;
            key.dst = m.blend.dst;
            key.dst_alpha = VK_BLEND_FACTOR_ZERO;
            key.op = m.blend.op;
            key.op_alpha = VK_BLEND_OP_ADD;
            key.mask = ap.mask;
        }
        if (flat_on_ && pass.kind == PassKind::plain && !m.picture_layer) {
            key.program = records ? flat_rec_[v] : flat_[v]; // (developers': KF4_SCN_FLAT, the shading's bound)
        }
        key.alpha_one = records && blend_runs_ && alpha_one(m, i) && pass.kind != PassKind::first_draw &&
                        !m.picture_layer && m.fuse == no_material;
        key.stencil = key.zwrite && settles(pass, m, rec) ? RunKey::Stencil::mark_won : RunKey::Stencil::off;
        if (key.stencil == RunKey::Stencil::mark_won && m.program == Program::nd280) {
            // (program 0x280's decals, about 1,900 a picture at 009's fire: their own depth first, then their colour
            // writing none, own_depth_first; pcr_canvas.cpp's decal_depth_first)
            key.stencil = RunKey::Stencil::own_first;
            key.zwrite = false;
        } else if (key.stencil == RunKey::Stencil::mark_won && settle_nearer_) {
            // (PORT_SCENE_DEPTH_NEARER: the colour writing no depth, then the own depth where nearer)
            key.stencil = RunKey::Stencil::own_after;
            key.zwrite = false;
        }
        canonical(key);
        if (records && joins(key, rec + i)) {
            // (the open run's state is this call's: nothing set)
            counts_.record_calls++;
            count_call(key);
            add_to_run(key, rec + i, command, slot, c);
            continue;
        }
        flush_run(c);
        if (!state_set) {
            set_draw_state(scene, pass, d, m, records, c);
            state_set = true;
        }
        set_call_state(key, c);
        count_call(key);
        if (records) {
            push_record(c, rec + i);
            counts_.record_calls++;
            add_to_run(key, rec + i, command, slot, c);
            continue;
        }
        push.flags = (m.push.flags & ~(3u << pcr::F_PASS_SHIFT)) | ap.mode << pcr::F_PASS_SHIFT;
        c.push_constants(&push, 0, sizeof(push));
        if (d.native) {
            c.draw(command.vertexCount, 1, command.firstVertex, command.firstInstance);
        } else {
            c.draw(d.vertex_count);
        }
        counts_.draw_calls++;
        counts_.draw_calls_by[slot]++;
    }
}

bool Drawer::settles(const Pass &pass, const Material &m, uint32_t record) const {
    return m.settle && m.fuse == no_material && !m.picture_layer && pass.kind == PassKind::plain && record != no_record &&
           depth_reset_rec_[size_t(m.program)] != nullptr && !flat_on_;
}

// A run of settled decals (pcr_canvas.cpp's settle_decal_depth, a run at a time). Each is tested with the top fine
// part of its whole Z, so that it wins against the surface of its whole Z under it as on the GS; written as its depth,
// that top part made every later draw of the same whole Z lose to it where the GS lets the later draw win (map 063's
// rock hole showing the sky). So their passes that write Z set the stencil's decal_won bit where they pass (zero
// before: this clears every bit it set, and a depth surface's stencil is cleared with it), and after the run its calls
// are drawn twice more with no colour: where the bit is set the depth reset to the farthest (the viewport's depth 0 to
// 0) and the bit cleared; then the decal's own depth, tested GEQUAL, so that each of those pixels ends with the nearest
// of the run's fragments' there (elsewhere it lost, and loses again). Its own depth is the GPU's interpolated one, not
// the shader's
// (PCR_F_SCN_OWN_DEPTH, the records at own_offset: pcr_common.h), so that both passes are tested before their shader,
// which does nothing (pcr_depth_reset.frag); the viewport's depth range 0 to the largest Z's depth, so that its clamp is
// the exact depth's. (Settled as the PC renderer does it, the own depth the shader's, map 063 took 3.53 ms of GPU a
// picture against 2.64 unsettled; the own depth written by the first fragment alone, no reset, let a rock's far side
// through to the sky; 2026-11-21.)
void Drawer::settle_run(const Pending &r, CommandBuffer &c) {
    size_t v = 0;
    while (v < size_t(Program::count) && r.key.program != early_rec_[v] && r.key.program != plain_rec_[v]) {
        v++;
    }
    if (v == size_t(Program::count) || !depth_reset_rec_[v] || prepared_.own_offset == 0) {
        return; // (not a program of the records: not settled)
    }
    RunKey reset = r.key;
    reset.program = depth_reset_rec_[v];
    reset.ztest = reset.zwrite = true;
    reset.blend = false;
    reset.mask = 0;
    reset.stencil = RunKey::Stencil::reset_won;
    reset.zcompare = VK_COMPARE_OP_ALWAYS;
    reset.viewport.minDepth = reset.viewport.maxDepth = 0.0f; // (the farthest)
    set_call_state(reset, c);
    c.set_viewport(reset.viewport);
    c.set_depth_compare(VK_COMPARE_OP_ALWAYS);
    push_record(c, r.first_record);
    draw_run(r, c);
    RunKey own = reset;
    own.stencil = RunKey::Stencil::off;
    own.zcompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
    own.viewport.maxDepth = own_depth_most_;
    set_call_state(own, c);
    c.set_viewport(own.viewport);
    c.set_depth_compare(VK_COMPARE_OP_GREATER_OR_EQUAL);
    push_record(c, r.first_record + prepared_.own_offset);
    draw_run(r, c);
    set_call_state(r.key, c);
    c.set_viewport(r.key.viewport);
    c.set_depth_compare(r.key.zcompare);
    push_record(c, r.first_record);
    counts_.settled_runs++;
}

// A run of program 0x280's decals with its depth given first (pcr_canvas.cpp's decal_depth_first): its calls drawn with
// no colour and their own depth (settle_run's: the GPU's, the records at own_offset), GEQUAL, so that each pixel holds
// the nearer of what was there and the run's own depth; then the run itself (after this), tested with the top fine part
// (a tie with the surface under it passes, as on the GS) and writing no depth. No stencil, one draw more a run, not
// two. Unlike the GS (and the settle), where the decals overlap themselves their farther part is not drawn under the
// nearer: the PC renderer's choice for this program alone (009's fire, 2026-11-19: no pixel more than 4 apart from the
// settle's in 32 frames at 4x; for every program, map 013's blended tree came out thinner)
void Drawer::own_depth_first(const Pending &r, CommandBuffer &c) {
    own_depth(r, VK_COMPARE_OP_GREATER_OR_EQUAL, c);
}

// (PORT_SCENE_DEPTH_NEARER) A run of settled decals drawn writing no depth (tested with the top fine part, so
// that each ties with the surface of its whole Z under it), then its calls again with no colour and their own depth
// (the GPU's, as settle_run's), GREATER: written only where the decal is nearer than what is there. A decal lying on
// its surface leaves the surface's depth, which a later draw on that plane ties with by the fine part as it would
// without the decal; one in front of what is under it hides what comes after behind it. One draw more a run and no
// stencil, against settle_run's two and the stencil's marks. Overlapping decals within the run are drawn as with the
// settle: each tested against the depth from before the run.
void Drawer::own_depth_after(const Pending &r, CommandBuffer &c) {
    own_depth(r, VK_COMPARE_OP_GREATER, c);
}

void Drawer::own_depth(const Pending &r, VkCompareOp compare, CommandBuffer &c) {
    size_t v = 0;
    while (v < size_t(Program::count) && r.key.program != early_rec_[v] && r.key.program != plain_rec_[v]) {
        v++;
    }
    if (v == size_t(Program::count) || !depth_reset_rec_[v] || prepared_.own_offset == 0) {
        return; // (not a program of the records)
    }
    RunKey own = r.key;
    own.program = depth_reset_rec_[v];
    own.ztest = own.zwrite = true;
    own.blend = false;
    own.mask = 0;
    own.stencil = RunKey::Stencil::off;
    own.zcompare = compare;
    own.viewport.minDepth = 0.0f;
    own.viewport.maxDepth = own_depth_most_;
    set_call_state(own, c);
    c.set_viewport(own.viewport);
    c.set_depth_compare(compare);
    push_record(c, r.first_record + prepared_.own_offset);
    draw_run(r, c);
    set_call_state(r.key, c);
    c.set_viewport(r.key.viewport);
    c.set_depth_compare(r.key.zcompare);
    push_record(c, r.first_record);
    counts_.settled_runs++;
}

// ---- The pipelines drawn with, listed (pipeline_set) ----

std::vector<Drawer::RunKey> Drawer::call_keys(bool first_draw) const {
    // The depth's states (record_draw's: ztest, zwrite only where ztest; the compare a material's, from ZTST), in a
    // pipeline only without dynamic depth state
    struct Depth {
        bool test, write;
        VkCompareOp compare;
    };
    std::vector<Depth> depths;
    if (dynamic_ds_) {
        depths.push_back({false, false, VK_COMPARE_OP_ALWAYS});
    } else {
        for (VkCompareOp compare : {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_GREATER_OR_EQUAL,
                                    VK_COMPARE_OP_GREATER}) {
            depths.push_back({false, false, compare});
            depths.push_back({true, false, compare});
            if (!first_draw) {
                depths.push_back({true, true, compare}); // (the first-draw passes write no Z)
            }
        }
    }
    // The blends and write masks: the first-draw passes' one (record_draw's), else every blend choose_blend makes
    // (ALPHA's A, B, C and D, FIX 0 or not, ABE on or off: scn_build.cpp's material; the blends left out, unblended)
    // with every write mask a pass can have (scn_build.cpp: 0x7 the colour, 0x8 the alpha), in canonical's form; one
    // where the blend is dynamic state. (Without dynamic depth state the blend constants are in the pipeline too: a
    // blend by FIX has a pipeline for each FIX, not listed, made at the draw as before; Vulkan 1.3 has it.)
    std::vector<RunKey> blends;
    auto add_blend = [&](const RunKey &k) {
        for (const RunKey &b : blends) {
            if (b.blend == k.blend && b.src == k.src && b.src_alpha == k.src_alpha && b.dst == k.dst &&
                b.dst_alpha == k.dst_alpha && b.op == k.op && b.op_alpha == k.op_alpha && b.mask == k.mask) {
                return;
            }
        }
        blends.push_back(k);
    };
    if (first_draw) {
        RunKey k;
        k.blend = true;
        k.src = k.src_alpha = k.dst = k.dst_alpha = VK_BLEND_FACTOR_ONE;
        k.op = k.op_alpha = VK_BLEND_OP_MIN;
        k.mask = 0x1;
        canonical(k);
        add_blend(k);
    } else if (dynamic_blend_) {
        RunKey k;
        k.mask = 0xF;
        canonical(k);
        add_blend(k);
    } else {
        for (bool abe : {false, true}) {
            for (uint64_t alpha = 0; alpha < 256; alpha++) {
                for (uint64_t fix : {0u, 0x80u}) {
                    const pcr::BlendChoice b = pcr::choose_blend(alpha | fix << 32, abe);
                    for (uint32_t mask : {0x0u, 0x7u, 0x8u, 0xFu}) {
                        RunKey k;
                        k.blend = b.enable;
                        k.src = b.src;
                        k.src_alpha = b.src_alpha;
                        k.dst = b.dst;
                        k.dst_alpha = VK_BLEND_FACTOR_ZERO;
                        k.op = b.op;
                        k.op_alpha = VK_BLEND_OP_ADD;
                        k.mask = mask;
                        canonical(k);
                        add_blend(k);
                    }
                }
            }
        }
    }
    std::vector<RunKey> keys;
    keys.reserve(depths.size() * blends.size());
    for (const Depth &d : depths) {
        for (RunKey k : blends) {
            k.ztest = d.test;
            k.zwrite = d.write;
            k.zcompare = d.compare;
            keys.push_back(k);
        }
    }
    return keys;
}

std::vector<DeferredPipelineCompile> Drawer::pipeline_set(std::span<const VkFormat> colour_formats,
                                                          std::span<const VkFormat> depth_formats) {
    // The programs record_draw draws with: the first-draw passes' (first_id_: in them, and only there), the others'
    std::vector<Vulkan::Program *> firsts, others;
    for (size_t v = 0; v < size_t(Program::count); v++) {
        firsts.insert(firsts.end(), {first_id_[v], first_id_rec_[v]});
        others.insert(others.end(), {plain_[v], early_[v], first_[v], plain_rec_[v], early_rec_[v], first_rec_[v]});
        if (flat_on_) {
            others.insert(others.end(), {flat_[v], flat_rec_[v]});
        }
    }
    others.insert(others.end(), {fused_280_, picture_layer_});
    std::erase(firsts, nullptr);
    std::erase(others, nullptr);
    const std::vector<RunKey> first_keys = call_keys(true), other_keys = call_keys(false);

    // A pass of every pair of formats (none of either, but not both): attachments of one pixel (a compatible render
    // pass is named by its attachments' formats and samples, not their sizes or layouts)
    std::vector<VkFormat> colours = {VK_FORMAT_UNDEFINED}, depths = {VK_FORMAT_UNDEFINED};
    colours.insert(colours.end(), colour_formats.begin(), colour_formats.end());
    depths.insert(depths.end(), depth_formats.begin(), depth_formats.end());
    auto attachment = [&](VkFormat format, bool depth) {
        ImageCreateInfo info = ImageCreateInfo::render_target(1, 1, format);
        info.usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        return device_.create_image(info, nullptr);
    };
    std::vector<DeferredPipelineCompile> out;
    std::unordered_set<Util::Hash> seen;
    CommandBufferHandle cmd = device_.request_command_buffer();
    cmd->set_dynamic_state(dynamic_ds_, dynamic_blend_);
    for (VkFormat colour : colours) {
        for (VkFormat depth : depths) {
            if (colour == VK_FORMAT_UNDEFINED && depth == VK_FORMAT_UNDEFINED) {
                continue;
            }
            ImageHandle colour_image = colour != VK_FORMAT_UNDEFINED ? attachment(colour, false) : ImageHandle();
            ImageHandle depth_image = depth != VK_FORMAT_UNDEFINED ? attachment(depth, true) : ImageHandle();
            if ((colour != VK_FORMAT_UNDEFINED && !colour_image) || (depth != VK_FORMAT_UNDEFINED && !depth_image)) {
                continue; // (not a format this device draws into: no pass of it)
            }
            RenderPassInfo rp;
            if (colour_image) {
                rp.num_color_attachments = 1;
                rp.color_attachments[0] = &colour_image->get_view();
            }
            rp.depth_stencil = depth_image ? &depth_image->get_view() : nullptr;
            rp.render_area = {{0, 0}, {1, 1}};
            cmd->begin_render_pass(rp);
            const bool first = colour == first_format;
            auto list = [&](const RunKey &key) {
                set_shared_pipeline_state(key.zcompare, key.constants, *cmd);
                set_vertex_input(*zeros_, *cmd); // (read only by the GIF's programs: in the others' no input)
                set_call_state(key, *cmd);
                DeferredPipelineCompile compile;
                cmd->extract_pipeline_state(compile);
                if (seen.insert(compile.hash).second) {
                    out.push_back(compile);
                }
            };
            // (exact Z's settled decals, record_settled: in a pass whose depth has a stencil)
            const bool settle = !first && depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
            for (Vulkan::Program *program : first ? firsts : others) {
                for (RunKey key : first ? first_keys : other_keys) {
                    key.program = program;
                    list(key);
                    if (settle && (key.zwrite || dynamic_ds_)) {
                        key.stencil = RunKey::Stencil::mark_won;
                        list(key);
                    }
                }
            }
            for (Vulkan::Program *program : depth_reset_rec_) {
                if (!settle || !program) {
                    continue;
                }
                RunKey key;
                key.program = program;
                key.ztest = key.zwrite = true;
                key.zcompare = VK_COMPARE_OP_ALWAYS;
                key.mask = 0;
                canonical(key);
                key.stencil = RunKey::Stencil::reset_won; // (the reset)
                list(key);
                key.stencil = RunKey::Stencil::off; // (the own depth)
                key.zcompare = VK_COMPARE_OP_GREATER_OR_EQUAL;
                list(key);
                key.zcompare = VK_COMPARE_OP_GREATER; // (PORT_SCENE_DEPTH_NEARER's own depth, own_depth_after)
                list(key);
            }
            cmd->end_render_pass();
        }
    }
    device_.submit(cmd);
    return out;
}

std::vector<Vulkan::Program *> Drawer::compute_programs() const {
    std::vector<Vulkan::Program *> out;
    for (Vulkan::Program *p : place_) {
        if (p) {
            out.push_back(p);
        }
    }
    return out;
}

} // namespace scn
