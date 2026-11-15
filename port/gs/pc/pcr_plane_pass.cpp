// The PC renderer's plane pass: pcr_plane_pass.h
#include "pcr_plane_pass.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "pcr_gpu_time.h"
#include "pcr_shaders.h"

namespace pcr {

using namespace Vulkan;

namespace {

constexpr uint32_t programs = 4;
constexpr uint32_t chunk_entries = 1u << 15; // (a chunk's room unless a batch needs more: 512 KB entries, 1 MB planes)
constexpr uint32_t plane_bytes = 32;         // (an entry's two words, shaders/pcr_planes.h's u_planes_out)
constexpr uint32_t group_size = 64;          // (shaders/pcr_planes.h's local_size_x)

struct PlanesPush {
    uint32_t first, count;
};

// A run of a chunk's entries one dispatch works out: one program's, with the same buffers
struct Job {
    PlaneSources sources;
    uint32_t first = 0, count = 0;
};

struct Chunk {
    BufferHandle entries, planes;
    PlaneEntry *map = nullptr;
    uint32_t capacity = 0, used = 0;
    std::vector<Job> jobs;
};

bool same_sources(const PlaneSources &a, const PlaneSources &b) {
    return a.program == b.program && a.picture == b.picture && a.arena.get() == b.arena.get() &&
           a.records.get() == b.records.get() && a.triangles.get() == b.triangles.get();
}

} // namespace

class PlanePass::Impl {
public:
    explicit Impl(Device &device) : device_(device) {
        const std::array<std::pair<const uint32_t *, size_t>, programs> code = {{
            {pcr_spv_pcr_planes_000_comp, sizeof(pcr_spv_pcr_planes_000_comp)},
            {pcr_spv_pcr_planes_280_comp, sizeof(pcr_spv_pcr_planes_280_comp)},
            {pcr_spv_pcr_planes_700_comp, sizeof(pcr_spv_pcr_planes_700_comp)},
            {pcr_spv_pcr_planes_ripple_comp, sizeof(pcr_spv_pcr_planes_ripple_comp)},
        }};
        for (uint32_t p = 0; p < programs; p++) {
            // (set 0 as the program's vertex shader has it: the picture, and its arena, records and triangles at
            // 3 + 3 p up; set 1 the entries and the planes)
            ResourceLayout layout = {};
            layout.push_constant_size = sizeof(PlanesPush);
            layout.sets[0].storage_buffer_mask = 1u | 7u << (3 + 3 * p);
            for (uint32_t b : {0u, 3 + 3 * p, 4 + 3 * p, 5 + 3 * p}) {
                layout.sets[0].meta[b].array_size = 1;
            }
            layout.sets[1].storage_buffer_mask = 3u;
            layout.sets[1].meta[0].array_size = 1;
            layout.sets[1].meta[1].array_size = 1;
            Shader *shader = device_.request_shader(code[p].first, code[p].second, &layout);
            programs_[p] = shader ? device_.request_program(shader) : nullptr;
        }
    }

    PlaneRoom reserve(const PlaneSources &sources, uint32_t count) {
        PlaneRoom room;
        if (count == 0 || sources.program >= programs || !programs_[sources.program]) {
            return room;
        }
        bool fresh = false;
        if (chunks_.empty() || chunks_.back().used + count > chunks_.back().capacity) {
            if (!add_chunk(std::max(count, chunk_entries))) {
                return room;
            }
            fresh = true;
        }
        Chunk &c = chunks_.back();
        if (c.jobs.empty() || !same_sources(c.jobs.back().sources, sources)) {
            c.jobs.push_back({sources, c.used, 0});
        }
        c.jobs.back().count += count;
        room.entries = c.map + c.used;
        room.planes = c.planes.get();
        room.first = c.used;
        room.new_chunk = fresh;
        c.used += count;
        triangles_ += count;
        return room;
    }

    PlanePass::Submitted submit(GpuTimes &times) {
        PlanePass::Submitted done;
        if (chunks_.empty()) {
            return done;
        }
        CommandBufferHandle cmd = device_.request_command_buffer();
        times.begin(*cmd, GpuWork::planes);
        for (Chunk &c : chunks_) {
            device_.unmap_host_buffer(*c.entries, MEMORY_ACCESS_WRITE_BIT); // (the entries seen by the GPU)
            for (const Job &job : c.jobs) {
                const PlaneSources &s = job.sources;
                uint32_t at = 3 + 3 * s.program;
                cmd->set_program(programs_[s.program]);
                cmd->set_storage_buffer(0, 0, *s.picture);
                cmd->set_storage_buffer(0, at, *s.arena);
                cmd->set_storage_buffer(0, at + 1, *s.records);
                cmd->set_storage_buffer(0, at + 2, *s.triangles);
                cmd->set_storage_buffer(1, 0, *c.entries);
                cmd->set_storage_buffer(1, 1, *c.planes);
                PlanesPush push = {job.first, job.count};
                cmd->push_constants(&push, 0, sizeof(push));
                cmd->dispatch((job.count + group_size - 1) / group_size, 1, 1);
                dispatches_++;
                done.dispatches++;
                done.triangles += job.count;
            }
        }
        // (the planes before the canvas's vertex shaders read them, in the command buffer submitted next)
        cmd->barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        times.end(*cmd);
        device_.submit(cmd);
        chunks_.clear(); // (Granite deletes the buffers once the GPU is done with this frame context's work)
        return done;
    }

    uint64_t triangles() const {
        return triangles_;
    }

    uint64_t dispatches() const {
        return dispatches_;
    }

private:
    bool add_chunk(uint32_t capacity) {
        BufferCreateInfo info = {};
        info.domain = BufferDomain::Host;
        info.size = VkDeviceSize(capacity) * sizeof(PlaneEntry);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        Chunk c;
        c.entries = device_.create_buffer(info, nullptr);
        info.domain = BufferDomain::Device;
        info.size = VkDeviceSize(capacity) * plane_bytes;
        c.planes = device_.create_buffer(info, nullptr);
        if (!c.entries || !c.planes) {
            fprintf(stderr, "[pcr] plane pass: no buffers for %u triangles\n", capacity);
            return false;
        }
        c.map = static_cast<PlaneEntry *>(device_.map_host_buffer(*c.entries, MEMORY_ACCESS_WRITE_BIT));
        if (!c.map) {
            return false;
        }
        c.capacity = capacity;
        chunks_.push_back(std::move(c));
        return true;
    }

    Device &device_;
    std::array<Program *, programs> programs_{};
    std::vector<Chunk> chunks_; // (since the last submit)
    uint64_t triangles_ = 0, dispatches_ = 0;
};

PlanePass::PlanePass(Device &device) : impl_(std::make_unique<Impl>(device)) {}
PlanePass::~PlanePass() = default;

PlaneRoom PlanePass::reserve(const PlaneSources &sources, uint32_t count) {
    return impl_->reserve(sources, count);
}

PlanePass::Submitted PlanePass::submit(GpuTimes &times) {
    return impl_->submit(times);
}

uint64_t PlanePass::triangles() const {
    return impl_->triangles();
}

uint64_t PlanePass::dispatches() const {
    return impl_->dispatches();
}

} // namespace pcr
