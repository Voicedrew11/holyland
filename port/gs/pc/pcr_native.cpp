// The PC renderer's native draws (pcr_native.h)
#include "pcr_native.h"

#include <string.h>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

#include "../../render/render_internal.hpp"
#include "../../render/vu1_record.hpp"

extern "C" {
#include "port/host.h"
}

using namespace Vulkan;

namespace pcr {

namespace {

// A buffer the CPU writes and the GPU reads, in units: filled from the front, replaced when full (what a draw reads
// is never written again)
struct GpuList {
    BufferHandle buffer;
    uint8_t *map = nullptr;
    uint32_t used = 0, room = 0, unit = 0;
};

constexpr uint32_t ARENA_FIRST = 1u << 19, ARENA_MOST = 1u << 22; // (vertices: 32 MB, at most 256 MB)
constexpr uint32_t TRIANGLES_FIRST = 1u << 16;
constexpr uint32_t RECORDS_ROOM = 4096;
constexpr uint32_t VERTEX_BYTES = 16 * 4; // (program 0x000's: four qwords, rd_vertex_words)

} // namespace

class NativeDraws::Impl {
public:
    explicit Impl(Device &device) : device_(device) {}

    ~Impl() {
        for (GpuList *l : {&arena_, &triangles_, &records_}) {
            if (l->buffer && l->map) {
                device_.unmap_host_buffer(*l->buffer, MEMORY_ACCESS_WRITE_BIT);
            }
        }
    }

    void met(uint64_t value, GifFrontEnd &front, Canvas &canvas) {
        switch (RD_ND_COMMAND(value)) {
        case RD_ND_DRAW:
            draw(value, front, canvas);
            break;
        case RD_ND_FRAME:
            rd_consumer_frame(value);
            break;
        default:
            break; // (ALT_BEGIN and _END: --native-draw-compare, off under the PC renderer; READS_SHOWN: frame_interp.c's)
        }
    }

    NativeCounts counts_;

private:
    void draw(uint64_t value, GifFrontEnd &front, Canvas &canvas) {
        RdDraw d;
        RdCamera cam;
        fetched_.clear();
        if (!rd_fetch(value, d, cam, fetched_)) {
            counts_.lost++;
            return;
        }
        if (d.program != 0) {
            counts_.unsupported++;
            unsupported_[d.program]++;
            return;
        }
        counts_.draws++;
        Vu1Record k = vu1_record_000(d, cam);
        // (the XYOFFSET of the context the draw's PRIM picks, before its batches write PRIM: the game's draws keep both
        // contexts' the same)
        const GsRegisterFile &file = front.registers();
        uint64_t offset = file.reg[gs_reg::xyoffset_1 + ((front.prim_in_effect() >> 9) & 1)];
        k.info[1] = int32_t(offset & 0xFFFF);
        k.info[2] = int32_t((offset >> 32) & 0xFFFF);
        uint32_t record = put_record(k);
        for (const auto &mesh : fetched_) {
            if (mesh->format != RD_VF_PACKED16) {
                counts_.unsupported++;
                continue;
            }
            uint32_t base = place(*mesh);
            for (const RdBatch &b : mesh->batches) {
                // what the VU1 run kicks before its strip, as the GS gets it
                if (b.start != RD_PROGRAM_CONTINUE) {
                    front.write(gs_reg::fogcol, d.fogcol); // (an MSCAL: the set-up kicks FOGCOL, VU1 0x3D)
                }
                for (size_t i = 0; i + 1 < b.ad.size(); i += 2) {
                    front.write(uint32_t(b.ad[i]), b.ad[i + 1]);
                }
                front.write(gs_reg::prim, (b.tag[0] >> 47) & 0x7FF);
                uint32_t n = uint32_t(b.triangles.size() / 3);
                counts_.batches++;
                if (n == 0) {
                    continue;
                }
                uint32_t prim = front.prim_in_effect();
                NativeBatch nb;
                nb.arena = arena_.buffer;
                nb.first_triangle = place_triangles(*mesh, b, base);
                nb.triangles = triangles_.buffer;
                nb.records = records_.buffer;
                nb.triangle_count = n;
                nb.instance = record | ((prim >> 3) & 1) << 30;
                canvas.native_batch(front.registers(), prim, nb);
                counts_.triangles += n;
            }
        }
        fetched_.clear();
    }

    void make(GpuList &l, uint32_t room, uint32_t unit) {
        if (l.buffer && l.map) {
            device_.unmap_host_buffer(*l.buffer, MEMORY_ACCESS_WRITE_BIT);
        }
        BufferCreateInfo info = {};
        info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
        info.size = VkDeviceSize(room) * unit;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        l.buffer = device_.create_buffer(info, nullptr);
        l.map = l.buffer ? static_cast<uint8_t *>(device_.map_host_buffer(*l.buffer, MEMORY_ACCESS_WRITE_BIT)) : nullptr;
        if (!l.map) {
            fprintf(stderr, "[pcr] native draws: no buffer of %llu bytes\n", (unsigned long long)info.size);
            abort();
        }
        l.used = 0;
        l.room = room;
        l.unit = unit;
    }

    uint32_t put_record(const Vu1Record &k) {
        if (!records_.buffer || records_.used == records_.room) {
            make(records_, RECORDS_ROOM, sizeof(Vu1Record));
        }
        memcpy(records_.map + size_t(records_.used) * sizeof(Vu1Record), &k, sizeof(k));
        return records_.used++;
    }

    // The mesh's first vertex in the arena (put there the first time it is drawn into this arena)
    uint32_t place(const RdMeshData &m) {
        auto it = placed_.find(m.id);
        if (it != placed_.end()) {
            return it->second;
        }
        uint32_t n = uint32_t(m.vertices.size() * sizeof(int32_t) / VERTEX_BYTES);
        if (!arena_.buffer || arena_.used + n > arena_.room) {
            // (full: a new arena, the meshes put in it again as they are drawn, their triangles too)
            uint32_t room = std::min(std::max(arena_.room ? arena_.room * 2 : ARENA_FIRST, n), std::max(ARENA_MOST, n));
            make(arena_, room, VERTEX_BYTES);
            placed_.clear();
            triangles_placed_.clear();
            counts_.arenas++;
        }
        uint32_t at = arena_.used;
        memcpy(arena_.map + size_t(at) * VERTEX_BYTES, m.vertices.data(), size_t(n) * VERTEX_BYTES);
        arena_.used += n;
        placed_[m.id] = at;
        counts_.meshes_placed++;
        return at;
    }

    // The batch's first triangle in the triangles' buffer (placed there the first time it is drawn into this arena): a
    // uvec4 each, the arena's numbers of its corners, and of its kick with how many of the two kicks before it are the
    // batch's (bits 28-29: VU1's clipping looks at those)
    uint32_t place_triangles(const RdMeshData &m, const RdBatch &b, uint32_t base) {
        uint64_t key = uint64_t(m.id) << 20 | uint64_t(&b - m.batches.data());
        auto it = triangles_placed_.find(key);
        if (it != triangles_placed_.end()) {
            return it->second;
        }
        uint32_t n = uint32_t(b.triangles.size() / 3);
        if (!triangles_.buffer || triangles_.used + n > triangles_.room) {
            make(triangles_, std::max(triangles_.room ? triangles_.room * 2 : TRIANGLES_FIRST, n), 16);
            triangles_placed_.clear();
        }
        uint32_t at = triangles_.used;
        uint32_t *out = reinterpret_cast<uint32_t *>(triangles_.map) + size_t(at) * 4;
        uint32_t first = base + b.first;
        for (uint32_t t = 0; t < n; t++) {
            uint32_t kick = b.triangles[t * 3 + 2];
            out[t * 4 + 0] = first + b.triangles[t * 3 + 0];
            out[t * 4 + 1] = first + b.triangles[t * 3 + 1];
            out[t * 4 + 2] = first + kick;
            out[t * 4 + 3] = (first + kick) | std::min(kick, 2u) << 28;
        }
        triangles_.used += n;
        triangles_placed_[key] = at;
        return at;
    }

    Device &device_;
    GpuList arena_, triangles_, records_;
    std::unordered_map<RdMesh, uint32_t> placed_;              // a mesh's first vertex in the arena
    std::unordered_map<uint64_t, uint32_t> triangles_placed_; // a batch's first triangle, by its mesh and number
    std::vector<std::shared_ptr<const RdMeshData>> fetched_;

public:
    std::map<uint32_t, uint64_t> unsupported_; // (the draws not drawn, by program)
};

NativeDraws::NativeDraws(Device &device) : impl_(std::make_unique<Impl>(device)) {}
NativeDraws::~NativeDraws() = default;

int NativeDraws::kinds() {
    // (program 0x000's kinds; the map's objects are 0x280's too, the skinned ones: not yet)
    return PORT_ND_MAP | PORT_ND_SKY | PORT_ND_ITEMS | PORT_ND_PICKED;
}

void NativeDraws::met(uint64_t value, GifFrontEnd &front, Canvas &canvas) {
    impl_->met(value, front, canvas);
}

NativeCounts NativeDraws::counts() const {
    return impl_->counts_;
}

void NativeDraws::print_counts(FILE *out) const {
    const NativeCounts &c = impl_->counts_;
    fprintf(out,
            "[stats] pc native draws: %llu draws, %llu batches (%llu triangles); %llu lost (their slot reused), %llu of a "
            "program not drawn yet; %llu meshes placed in %llu arenas\n",
            (unsigned long long)c.draws, (unsigned long long)c.batches, (unsigned long long)c.triangles,
            (unsigned long long)c.lost, (unsigned long long)c.unsupported, (unsigned long long)c.meshes_placed,
            (unsigned long long)c.arenas);
    for (const auto &[program, n] : impl_->unsupported_) {
        fprintf(out, "[stats] pc native draws not drawn yet: program 0x%x, %llu draws\n", program, (unsigned long long)n);
    }
}

} // namespace pcr
