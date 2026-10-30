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
        for (GpuList *l : {&arena_, &triangles_, &records_, &records_280_, &kicked_}) {
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
        if (d.program == 0x280) {
            draw_280(d, cam, front, canvas);
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
        uint32_t record = put(records_, RECORDS_ROOM, &k, sizeof(k));
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
                nb.program = NativeProgram::vu1_000;
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

    // A draw's record into a list of records (a new list when full): its number there
    uint32_t put(GpuList &list, uint32_t room, const void *record, uint32_t bytes) {
        if (!list.buffer || list.used == list.room) {
            make(list, room, bytes);
        }
        memcpy(list.map + size_t(list.used) * bytes, record, bytes);
        return list.used++;
    }

    // A draw of program 0x280 (the skinned models): as VU1's run of each batch, the plan says whether the run draws
    // (its strip and second pass, and so their registers) and which triangles VU1 kicks; those are listed each time
    void draw_280(const RdDraw &d, const RdCamera &cam, GifFrontEnd &front, Canvas &canvas) {
        counts_.draws++;
        Vu1Record280 k = vu1_record_280(d, cam);
        const GsRegisterFile &file = front.registers();
        for (int c = 0; c < 2; c++) {
            uint64_t offset = file.reg[gs_reg::xyoffset_1 + c];
            k.offset[2 * c] = int32_t(offset & 0xFFFF);
            k.offset[2 * c + 1] = int32_t((offset >> 32) & 0xFFFF);
        }
        uint32_t record = put(records_280_, RECORDS_ROOM, &k, sizeof(k));
        for (const auto &mesh : fetched_) {
            if (mesh->format != RD_VF_FLOAT_SKIN) {
                counts_.unsupported++;
                continue;
            }
            uint32_t base = place(*mesh);
            for (const RdBatch &b : mesh->batches) {
                // what the run kicks: FOGCOL at an MSCAL (the set-up), the A+D packet; then, unless every vertex is
                // clipped, the strip (PRIM from its tag), and the second pass's A+D packet and strip
                if (b.start != RD_PROGRAM_CONTINUE) {
                    front.write(gs_reg::fogcol, d.fogcol);
                }
                for (size_t i = 0; i + 1 < b.ad.size(); i += 2) {
                    front.write(uint32_t(b.ad[i]), b.ad[i + 1]);
                }
                counts_.batches++;
                Vu1Kicks280 plan;
                if (!vu1_kicks_280(&mesh->vertices[size_t(b.first) * 32], b, k, plan)) {
                    counts_.unsupported++;
                    continue;
                }
                if (plan.all_clipped) {
                    continue;
                }
                for (uint32_t pass = 0; pass < 1u + b.sphere; pass++) {
                    if (pass) {
                        for (size_t i = 0; i + 1 < b.ad2.size(); i += 2) {
                            front.write(uint32_t(b.ad2[i]), b.ad2[i + 1]);
                        }
                    }
                    front.write(gs_reg::prim, ((pass ? b.tag2[0] : b.tag[0]) >> 47) & 0x7FF);
                    uint32_t prim = front.prim_in_effect();
                    NativeBatch nb;
                    nb.program = NativeProgram::vu1_280;
                    nb.first_triangle = list_kicked_280(b, plan, base, nb.triangle_count);
                    if (nb.triangle_count == 0) {
                        continue;
                    }
                    nb.arena = arena_.buffer;
                    nb.records = records_280_.buffer;
                    nb.triangles = kicked_.buffer;
                    nb.instance = record | ((prim >> 9) & 1) << 28 | pass << 29 | ((prim >> 3) & 1) << 30;
                    canvas.native_batch(front.registers(), prim, nb);
                    counts_.triangles += nb.triangle_count;
                }
            }
        }
        fetched_.clear();
    }

    // The triangles VU1 kicks of a batch of program 0x280 (those whose kick has no ADC), listed for one pass: each the
    // arena's slots (two a vertex) of its corners and of its kick. Their first's place in the list; n their count.
    uint32_t list_kicked_280(const RdBatch &b, const Vu1Kicks280 &plan, uint32_t base, uint32_t &n) {
        n = 0;
        for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
            n += !plan.adc[b.triangles[i + 2]];
        }
        if (n == 0) {
            return 0;
        }
        if (!kicked_.buffer || kicked_.used + n > kicked_.room) {
            make(kicked_, std::max(kicked_.room ? kicked_.room : TRIANGLES_FIRST, n), 16);
        }
        uint32_t first = kicked_.used;
        uint32_t *out = reinterpret_cast<uint32_t *>(kicked_.map) + size_t(first) * 4;
        uint32_t slot0 = base + 2 * b.first;
        for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
            uint32_t kick = b.triangles[i + 2];
            if (plan.adc[kick]) {
                continue;
            }
            out[0] = slot0 + 2 * b.triangles[i];
            out[1] = slot0 + 2 * b.triangles[i + 1];
            out[2] = slot0 + 2 * kick;
            out[3] = slot0 + 2 * kick;
            out += 4;
        }
        kicked_.used += n;
        return first;
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
    GpuList arena_, triangles_, records_, records_280_, kicked_;
    std::unordered_map<RdMesh, uint32_t> placed_;              // a mesh's first vertex in the arena
    std::unordered_map<uint64_t, uint32_t> triangles_placed_; // a batch's first triangle, by its mesh and number
    std::vector<std::shared_ptr<const RdMeshData>> fetched_;

public:
    std::map<uint32_t, uint64_t> unsupported_; // (the draws not drawn, by program)
};

NativeDraws::NativeDraws(Device &device) : impl_(std::make_unique<Impl>(device)) {}
NativeDraws::~NativeDraws() = default;

int NativeDraws::kinds() {
    // (programs 0x000 and 0x280)
    return PORT_ND_MAP | PORT_ND_SKY | PORT_ND_ITEMS | PORT_ND_PICKED | PORT_ND_OBJECTS | PORT_ND_SKINNED | PORT_ND_ARM |
           PORT_ND_GOLD;
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
