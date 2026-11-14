// The PC renderer's native draws (pcr_native.h)
#include "pcr_native.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../render/part_moves.hpp"
#include "../../render/pass_draw.hpp"
#include "../../render/render_internal.hpp"
#include "../../render/ripple.hpp"
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
        for (GpuList *l : {&arena_, &triangles_, &records_, &records_280_, &records_ripple_, &kicked_}) {
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
            parts_.frame(uint32_t(value) & 0xFFFFFF, walk_);
            passes_.frame(uint32_t(value) & 0xFFFFFF, walk_);
            if (walk_ != PartWalk::picture) {
                shown_kept_ = false; // (a new frame: its first picture between keeps the frame shown again, 0xF2)
            }
            break;
        default:
            break; // (READS_SHOWN: frame_interp.c's)
        }
    }

    void frame_mark(uint64_t value) {
        parts_.mark(value);
        if ((uint32_t(value >> 32) & 0xFF) == 0xF2) {
            // (frame_interp.c: the frame shown kept for the pictures between, read by the frame shown before's pass)
            shown_kept_ = true;
            shown_kept_fbp_ = uint32_t(value) & 0x1FF;
        }
    }

    void set_picture(const GsnInterpPicture *picture) {
        walk_ = picture ? PartWalk::picture : (frame_kept_ ? PartWalk::frame_kept : PartWalk::frame);
        picture_u_ = picture ? 1.0f - picture->t : 0.0f;
        picture_t_ = picture ? picture->t : 1.0f;
        moved_on_cpu_ = picture ? false : moved_on_cpu_; // (asked after the walk's end)
    }

    void begin_frame_kept() {
        frame_kept_ = true;
        trail_paired_ = false;
        trail_moves_.clear();
        if (walk_ != PartWalk::picture) {
            walk_ = PartWalk::frame_kept;
        }
    }

    void end_frame_kept() {
        frame_kept_ = false;
        if (walk_ != PartWalk::picture) {
            walk_ = PartWalk::frame;
        }
    }

    bool trail_paired() const {
        return trail_paired_;
    }

    std::vector<GsnInterpMove> take_trail_moves() {
        return std::move(trail_moves_);
    }

    bool moved_on_cpu() const {
        return moved_on_cpu_;
    }

    NativeCounts counts() const {
        NativeCounts c = counts_;
        c.moved_models = parts_.moved_models();
        c.paired_strips = parts_.paired_strips();
        return c;
    }

    NativeCounts counts_;

private:
    // A move on a draw's record (the frame's walk's is none: its place kept for the next frame's pictures)
    static void apply_move(Vu1Record &k, const PartMove &m) {
        k.info[3] |= m.flags;
        memcpy(k.obj, m.obj, sizeof(k.obj));
        k.bt = m.model;
    }

    static void apply_move(Vu1Record280 &k, const PartMove &m) {
        k.info[1] = m.flags;
        k.bt[0] = m.model;
        k.bt[1] = m.parent;
    }

    // A draw's record with its move in a list. In the frame's walk kept, twice: the frame's (no move) and then the
    // picture's, as put_pair; k is the frame's, the one the kick plans read
    uint32_t put_moved(GpuList &list, Vu1Record &k, const PartMove &m) {
        if (walk_ != PartWalk::frame_kept) {
            apply_move(k, m);
            return put(list, RECORDS_ROOM, &k, sizeof(k));
        }
        Vu1Record picture = k;
        apply_move(k, PartMove());
        apply_move(picture, m);
        return put_pair(list, RECORDS_ROOM, &k, &picture, sizeof(k));
    }

    uint32_t put_moved(GpuList &list, Vu1Record280 &k, const PartMove &m) {
        if (walk_ != PartWalk::frame_kept) {
            apply_move(k, m);
            return put(list, RECORDS_ROOM, &k, sizeof(k));
        }
        Vu1Record280 picture = k;
        apply_move(k, PartMove());
        apply_move(picture, m);
        return put_pair(list, RECORDS_ROOM, &k, &picture, sizeof(k));
    }

    // A batch of a draw whose record is put twice in the frame's walk kept: the instance its picture between draws
    void name_picture(NativeBatch &nb) const {
        if (walk_ == PartWalk::frame_kept) {
            nb.picture_instance = nb.instance + 1;
        }
    }

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
        if (d.program == 0x700) {
            draw_700(d, cam, front, canvas);
            return;
        }
        if (d.program == RD_PROGRAM_GS_STRIP) {
            draw_gs_strip(d, front);
            return;
        }
        if (d.program == RD_PROGRAM_RIPPLE) {
            draw_ripple(d, cam, front, canvas);
            return;
        }
        if (d.program == RD_PROGRAM_PASS) {
            draw_pass(d, front, canvas);
            return;
        }
        if (d.program != 0 && d.program != 0x5A0) {
            counts_.unsupported++;
            unsupported_[d.program]++;
            return;
        }
        counts_.draws++;
        // (program 0x5A0, the shadows: 0x000's maths from the silhouette meshes, no lights and no fog)
        bool shadow = d.program == 0x5A0;
        Vu1Record k = shadow ? vu1_record_5a0(d, cam) : vu1_record_000(d, cam);
        // (the XYOFFSET of the context the draw's PRIM picks, before its batches write PRIM: the game's draws keep both
        // contexts' the same)
        const GsRegisterFile &file = front.registers();
        uint64_t offset = file.reg[gs_reg::xyoffset_1 + ((front.prim_in_effect() >> 9) & 1)];
        k.info[1] = int32_t(offset & 0xFFFF);
        k.info[2] = int32_t((offset >> 32) & 0xFFFF);
        uint32_t record = put_moved(records_, k, parts_.draw(d, cam, walk_));
        for (const auto &mesh : fetched_) {
            if (mesh->format != (shadow ? RD_VF_SILHOUETTE : RD_VF_PACKED16)) {
                counts_.unsupported++;
                continue;
            }
            uint32_t base = place(*mesh);
            for (const RdBatch &b : mesh->batches) {
                // what the VU1 run kicks before its strip, as the GS gets it (0x5A0's batches have no A+D packet)
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
                // (tolerant layers: KF4_PC_LAYERS=all, every batch that fades a layer, its twins too; none: none)
                static const char *const layers = getenv("KF4_PC_LAYERS");
                bool every = layers && strcmp(layers, "all") == 0, none = layers && strcmp(layers, "none") == 0;
                nb.layer = !shadow && !none && fades(b) && (every || !twin(*mesh, b));
                name_picture(nb);
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

    // A draw of program 0x700 (the effects): as VU1's run of each batch, its A+D packet (the flipbook's TEX0 perhaps put
    // in), then the strip unless VU1's count of clipped vertices says it is not kicked, its kicked triangles listed
    void draw_700(const RdDraw &d, const RdCamera &cam, GifFrontEnd &front, Canvas &canvas) {
        counts_.draws++;
        Vu1Record k = vu1_record_700(d, cam);
        const GsRegisterFile &file = front.registers();
        uint64_t offset = file.reg[gs_reg::xyoffset_1 + ((front.prim_in_effect() >> 9) & 1)];
        k.info[1] = int32_t(offset & 0xFFFF);
        k.info[2] = int32_t((offset >> 32) & 0xFFFF);
        uint32_t record = put_moved(records_, k, parts_.draw(d, cam, walk_));
        for (const auto &mesh : fetched_) {
            if (mesh->format != RD_VF_PACKED16) {
                counts_.unsupported++;
                continue;
            }
            uint32_t base = place(*mesh);
            for (const RdBatch &b : mesh->batches) {
                if (b.start != RD_PROGRAM_CONTINUE && b.start != 0x700) {
                    counts_.unsupported++; // (the adapter sends only meshes of 0x700's: rd_mesh_runs_program)
                    continue;
                }
                if (b.start != RD_PROGRAM_CONTINUE) {
                    front.write(gs_reg::fogcol, d.fogcol);
                }
                std::vector<uint64_t> ad = vu1_ad_700(b, d);
                for (size_t i = 0; i + 1 < ad.size(); i += 2) {
                    front.write(uint32_t(ad[i]), ad[i + 1]);
                }
                counts_.batches++;
                Vu1Kicks700 plan;
                if (!vu1_kicks_700(&mesh->vertices[size_t(b.first) * 16], b, k, plan)) {
                    counts_.unsupported++;
                    continue;
                }
                if (!plan.kick) {
                    continue;
                }
                front.write(gs_reg::prim, (b.tag[0] >> 47) & 0x7FF);
                uint32_t prim = front.prim_in_effect();
                NativeBatch nb;
                nb.program = NativeProgram::vu1_700;
                nb.first_triangle = list_kicked(b, plan.adc, base, 1, nb.triangle_count);
                if (nb.triangle_count == 0) {
                    continue;
                }
                nb.arena = arena_.buffer;
                nb.records = records_.buffer;
                nb.triangles = kicked_.buffer;
                nb.instance = record | ((prim >> 3) & 1) << 30;
                name_picture(nb);
                canvas.native_batch(front.registers(), prim, nb);
                counts_.triangles += nb.triangle_count;
            }
        }
        fetched_.clear();
    }

    // A draw of a GIF packet the game built on the CPU (the weapon's trail: render.cpp, parse_gs_strip): written as the
    // GS takes the packet, PRIM from the tag's PRE, Q 1, then each vertex's RGBAQ and XYZ2 (XYZ3 with ADC); its
    // triangles come to the canvas as the front end's primitives. In a picture between, each point t of the way from
    // its partner in the frame before (an untextured strip's), X and Y in 12.4 units rounded, each colour rounded, as
    // the strip's shader
    void draw_gs_strip(const RdDraw &d, GifFrontEnd &front) {
        std::vector<uint32_t> xy, rgba, from;
        for (const auto &mesh : fetched_) {
            if (mesh->format != RD_VF_GS_STRIP) {
                continue;
            }
            for (const RdBatch &b : mesh->batches) {
                for (uint32_t k = 0; k < b.count; k++) {
                    const int32_t *v = &mesh->vertices[size_t(b.first + k) * 8];
                    xy.push_back((uint32_t(v[4]) & 0xFFFF) | (uint32_t(v[5]) & 0xFFFF) << 16);
                    rgba.push_back((uint32_t(v[0]) & 0xFF) | (uint32_t(v[1]) & 0xFF) << 8 | (uint32_t(v[2]) & 0xFF) << 16 |
                                   (uint32_t(v[3]) & 0xFF) << 24);
                }
            }
        }
        const PartMoves::StripPoints *before = parts_.strip(d, xy, rgba, walk_, from);
        size_t at = 0;
        for (const auto &mesh : fetched_) {
            if (mesh->format != RD_VF_GS_STRIP) {
                counts_.unsupported++;
                continue;
            }
            counts_.draws++;
            for (const RdBatch &b : mesh->batches) {
                front.write_packed_tag((b.tag[0] >> 47) & 0x7FF);
                bool untextured = !((front.prim_in_effect() >> 4) & 1);
                bool lerp = before && walk_ == PartWalk::picture && untextured;
                moved_on_cpu_ |= lerp;
                // (the frame's own walk kept: its pictures move the trail by its kicks, numbered here, each with the
                // move the CPU's lerp makes; KF4_PC_TRAIL_KICKS=0, or a kick count off: the frame's pictures walk)
                bool by_kicks = before && untextured && walk_ == PartWalk::frame_kept && trail_kicks_ && !trail_paired_;
                uint32_t first_kick = native_kick_base + uint32_t(trail_moves_.size());
                if (by_kicks) {
                    front.number_native_kicks(first_kick);
                } else if (before && untextured && walk_ == PartWalk::frame_kept) {
                    trail_paired_ = true; // (its picture between would move it on the CPU)
                }
                for (uint32_t k = 0; k < b.count; k++, at++) {
                    const int32_t *v = &mesh->vertices[size_t(b.first + k) * 8];
                    int32_t q[8];
                    memcpy(q, v, sizeof(q));
                    if (by_kicks) {
                        GsnInterpMove m = {};
                        m.w[0] = GSN_IM_FLAT | GSN_IM_XY | GSN_IM_RGBA;
                        m.w[1] = before->rgba[from[at]];
                        m.w[2] = (uint32_t(v[0]) & 0xFF) | (uint32_t(v[1]) & 0xFF) << 8 |
                                 (uint32_t(v[2]) & 0xFF) << 16 | (uint32_t(v[3]) & 0xFF) << 24;
                        m.w[3] = before->xy[from[at]];
                        m.w[4] = (uint32_t(v[4]) & 0xFFFF) | (uint32_t(v[5]) & 0xFFFF) << 16;
                        trail_moves_.push_back(m);
                    }
                    if (lerp) {
                        uint32_t was_xy = before->xy[from[at]], was_rgba = before->rgba[from[at]];
                        q[4] = int32_t(lerped(uint32_t(v[4]) & 0xFFFF, was_xy & 0xFFFF));
                        q[5] = int32_t(lerped(uint32_t(v[5]) & 0xFFFF, was_xy >> 16));
                        for (int c = 0; c < 4; c++) {
                            q[c] = int32_t(lerped(uint32_t(v[c]) & 0xFF, (was_rgba >> (8 * c)) & 0xFF));
                        }
                    }
                    front.write_packed_qword(0x1, uint32_t(q[0]) | uint64_t(uint32_t(q[1])) << 32,
                                             uint32_t(q[2]) | uint64_t(uint32_t(q[3])) << 32); // RGBAQ
                    front.write_packed_qword(0x5, uint32_t(q[4]) | uint64_t(uint32_t(q[5])) << 32,
                                             uint32_t(q[6]) | uint64_t(uint32_t(q[7])) << 32); // XYZ2 (XYZ3: ADC)
                }
                if (by_kicks) {
                    if (front.native_kick_next() != native_kick_base + uint32_t(trail_moves_.size())) {
                        trail_paired_ = true; // (a kick count off: never)
                    }
                    front.stop_native_kicks();
                }
                counts_.batches++;
            }
        }
        fetched_.clear();
    }

    // A draw of the reflection's rippled water (RD_PROGRAM_RIPPLE: ripple.hpp): each block of the part a strip, PRIM
    // written as the strip's tag's PRE writes it (a strip, textured, blended); its triangles those the EE does not give
    // ADC, drawn by shaders/pcr_ndripple.vert with RGBAQ's colour and FOG's fog as the chain left them (the TEX0, ALPHA
    // and CLAMP of the strips are the chain's own writes, before and after the draws)
    void draw_ripple(const RdDraw &d, const RdCamera &cam, GifFrontEnd &front, Canvas &canvas) {
        counts_.draws++;
        RippleRecord k = ripple_record(d, cam);
        const GsRegisterFile &file = front.registers();
        k.info[3] = int32_t(uint32_t(file.reg[gs_reg::rgbaq]));
        k.fog[0] = float((file.reg[gs_reg::fog] >> 56) & 0xFF);
        // (the world's and the reflection's move with the picture's camera)
        const PartMove move = parts_.draw(d, cam, walk_);
        uint32_t record = 0;
        bool put_record = false;
        for (const auto &mesh : fetched_) {
            uint32_t n = uint32_t(mesh->vertices.size() / 16), total = 0;
            for (const RdBatch &b : mesh->batches) {
                total += b.count;
            }
            // (the block's vertices in order, its batches' together: Wobble_DrawRippledModel reads them so)
            if (mesh->format != RD_VF_PACKED16 || total != n || n == 0) {
                counts_.unsupported++;
                continue;
            }
            front.write(gs_reg::prim, 0x54);
            uint32_t prim = front.prim_in_effect();
            if (!put_record) {
                uint64_t offset = file.reg[gs_reg::xyoffset_1 + ((prim >> 9) & 1)];
                k.info[0] = int32_t(offset & 0xFFFF);
                k.info[1] = int32_t((offset >> 32) & 0xFFFF);
                record = put_ripple(k, move);
                put_record = true;
            }
            uint32_t base = place(*mesh);
            ripple_adc(mesh->vertices.data(), n, k, adc_);
            counts_.batches++;
            NativeBatch nb;
            nb.program = NativeProgram::ripple;
            nb.first_triangle = list_ripple(adc_, base, nb.triangle_count);
            if (nb.triangle_count == 0) {
                continue;
            }
            nb.arena = arena_.buffer;
            nb.records = records_ripple_.buffer;
            nb.triangles = kicked_.buffer;
            nb.instance = record;
            name_picture(nb);
            canvas.native_batch(front.registers(), prim, nb);
            counts_.triangles += nb.triangle_count;
        }
        fetched_.clear();
    }

    // A draw of a screen pass (RD_PROGRAM_PASS: pass_draw.hpp): its packets as the PS2 path's, written as the GS takes
    // them; their primitives come to the canvas as the front end's. In a picture between, t of the way from the same pass
    // of the frame before (moved on the CPU: the walk is not kept to be drawn again), and the frame drawn read where the
    // picture is drawn, as frame_interp.c writes the PS2 path's TEX0 (ad_write); the frame shown before's pass reads the
    // frame shown kept (0xF2), between the marks that have the canvas read it (0xF4, 0xF5), as frame_interp.c puts them
    // around the PS2 path's packet
    void draw_pass(const RdDraw &d, GifFrontEnd &front, Canvas &canvas) {
        counts_.draws++;
        RdPass p;
        rd_pass_of(d, p);
        const GsRegisterFile &file = front.registers();
        // (frame_interp.c keeps the frame shown when a pass reads it: so for the frame's own walk kept, the pictures')
        bool kept = walk_ == PartWalk::frame_kept || (shown_kept_ && shown_kept_fbp_ * 32 == p.frame_tbp);
        PassMove move = passes_.pass(p, file.reg[gs_reg::tex0_1], kept, walk_, picture_t_);
        if (move.mode != 0 && walk_ == PartWalk::frame_kept) {
            trail_paired_ = true; // (its picture between would move it on the CPU)
        }
        if (walk_ != PartWalk::picture) {
            move = PassMove();
        }
        moved_on_cpu_ |= move.mode != 0;
        uint32_t from = 0xFFFFFFFFu, to = 0;
        if (walk_ == PartWalk::picture && p.pass != RD_PASS_PREVIOUS) {
            from = p.frame_tbp;
            to = uint32_t(file.reg[gs_reg::frame_1] & 0x1FF) * 32;
        }
        bool reads_kept = walk_ == PartWalk::picture && p.pass == RD_PASS_PREVIOUS && kept;
        if (reads_kept) {
            canvas.frame_mark(uint64_t(0xF4) << 32);
        }
        stream_.q.clear();
        pass_stream(p, move, stream_);
        if (stream_.q.empty()) {
            counts_.unsupported++;
            unsupported_[RD_PROGRAM_PASS]++;
        }
        walk_packets(stream_, front, from, to);
        if (reads_kept) {
            canvas.frame_mark(uint64_t(0xF5) << 32);
        }
        counts_.batches++;
    }

    // GIF packets built on the CPU (PACKED tags and their qwords), written as the GS takes them; a TEX0 write whose TBP0
    // is from made to, to
    void walk_packets(const PassStream &s, GifFrontEnd &front, uint32_t from, uint32_t to) {
        size_t n = s.q.size() / 2;
        for (size_t i = 0; i < n;) {
            uint64_t tag = s.q[i * 2], regs = s.q[i * 2 + 1];
            i++;
            uint32_t nloop = uint32_t(tag & 0x7FFF), nreg = uint32_t(tag >> 60);
            nreg = nreg ? nreg : 16;
            if ((tag >> 46) & 1) {
                front.write_packed_tag((tag >> 47) & 0x7FF);
            } else {
                front.write_packed_tag_no_pre();
            }
            for (uint32_t k = 0; k < nloop * nreg && i < n; k++, i++) {
                uint32_t desc = uint32_t(regs >> ((k % nreg) * 4)) & 0xF;
                uint64_t lo = s.q[i * 2], hi = s.q[i * 2 + 1];
                if (desc == 0xE && ((hi & 0xFF) == gs_reg::tex0_1 || (hi & 0xFF) == gs_reg::tex0_2) &&
                    (lo & 0x3FFF) == from) {
                    lo = (lo & ~uint64_t(0x3FFF)) | to;
                }
                front.write_packed_qword(desc, lo, hi);
            }
        }
    }

    // A water draw's record with its move (in the frame's walk kept, the frame's and then its picture's, as put_pair)
    uint32_t put_ripple(RippleRecord &k, const PartMove &m) {
        int32_t moving = (m.flags & VU1_MOVE_CAMERA) ? RIPPLE_MOVE_CAMERA : 0;
        if (walk_ != PartWalk::frame_kept) {
            k.info[2] |= moving;
            return put(records_ripple_, RECORDS_ROOM, &k, sizeof(k));
        }
        RippleRecord picture = k;
        picture.info[2] |= moving;
        return put_pair(records_ripple_, RECORDS_ROOM, &k, &picture, sizeof(k));
    }

    // A water strip's triangles (kicks 2 to n - 1 without ADC), listed: each the arena's numbers of its three corners
    // (and the kick's again). Their first's place; n their count.
    uint32_t list_ripple(const std::vector<uint8_t> &adc, uint32_t base, uint32_t &n) {
        n = 0;
        for (size_t kick = 2; kick < adc.size(); kick++) {
            n += !adc[kick];
        }
        if (n == 0) {
            return 0;
        }
        if (!kicked_.buffer || kicked_.used + n > kicked_.room) {
            make(kicked_, std::max(kicked_.room ? kicked_.room : TRIANGLES_FIRST, n), 16);
        }
        uint32_t first = kicked_.used;
        uint32_t *out = reinterpret_cast<uint32_t *>(kicked_.map) + size_t(first) * 4;
        for (uint32_t kick = 2; kick < uint32_t(adc.size()); kick++) {
            if (adc[kick]) {
                continue;
            }
            out[0] = base + kick - 2;
            out[1] = base + kick - 1;
            out[2] = base + kick;
            out[3] = base + kick;
            out += 4;
        }
        kicked_.used += n;
        return first;
    }

    // now, moved t of the way back to was (the strip shader's rounding: u = 1 - t)
    uint32_t lerped(uint32_t now, uint32_t was) const {
        return uint32_t(int32_t(now) + int32_t(std::floor(float(int32_t(was) - int32_t(now)) * picture_u_ + 0.5f)));
    }

    // A draw's record into a list of records (a new list when full): its number there
    uint32_t put(GpuList &list, uint32_t room, const void *record, uint32_t bytes) {
        if (!list.buffer || list.used == list.room) {
            make(list, room, bytes);
        }
        memcpy(list.map + size_t(list.used) * bytes, record, bytes);
        return list.used++;
    }

    // A draw's two records side by side in one list (the frame's, then its picture's): the first's number, the
    // second's the next (an instance plus one: the record is its low bits)
    uint32_t put_pair(GpuList &list, uint32_t room, const void *first, const void *second, uint32_t bytes) {
        if (!list.buffer || list.used + 2 > list.room) {
            make(list, room, bytes);
        }
        uint32_t at = list.used;
        memcpy(list.map + size_t(at) * bytes, first, bytes);
        memcpy(list.map + size_t(at + 1) * bytes, second, bytes);
        list.used += 2;
        return at;
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
        uint32_t record = put_moved(records_280_, k, parts_.draw(d, cam, walk_));
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
                    nb.first_triangle = list_kicked(b, plan.adc, base, 2, nb.triangle_count);
                    if (nb.triangle_count == 0) {
                        continue;
                    }
                    nb.arena = arena_.buffer;
                    nb.records = records_280_.buffer;
                    nb.triangles = kicked_.buffer;
                    nb.instance = record | ((prim >> 9) & 1) << 28 | pass << 29 | ((prim >> 3) & 1) << 30;
                    name_picture(nb);
                    canvas.native_batch(front.registers(), prim, nb);
                    counts_.triangles += nb.triangle_count;
                }
            }
        }
        fetched_.clear();
    }

    // The triangles VU1 kicks of a batch (those whose kick has no ADC), listed for one pass: each the arena's slots of its
    // corners and of its kick (slots a vertex takes: 1 for 0x700's, 2 for 0x280's). Their first's place; n their count.
    uint32_t list_kicked(const RdBatch &b, const bool *adc, uint32_t base, uint32_t slots, uint32_t &n) {
        n = 0;
        for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
            n += !adc[b.triangles[i + 2]];
        }
        if (n == 0) {
            return 0;
        }
        if (!kicked_.buffer || kicked_.used + n > kicked_.room) {
            make(kicked_, std::max(kicked_.room ? kicked_.room : TRIANGLES_FIRST, n), 16);
        }
        uint32_t first = kicked_.used;
        uint32_t *out = reinterpret_cast<uint32_t *>(kicked_.map) + size_t(first) * 4;
        uint32_t slot0 = base + slots * b.first;
        for (size_t i = 0; i + 2 < b.triangles.size(); i += 3) {
            uint32_t kick = b.triangles[i + 2];
            if (adc[kick]) {
                continue;
            }
            out[0] = slot0 + slots * b.triangles[i];
            out[1] = slot0 + slots * b.triangles[i + 1];
            out[2] = slot0 + slots * kick;
            out[3] = slot0 + slots * kick;
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
            solid_.clear();
            twins_.clear();
            counts_.arenas++;
        }
        uint32_t at = arena_.used;
        memcpy(arena_.map + size_t(at) * VERTEX_BYTES, m.vertices.data(), size_t(n) * VERTEX_BYTES);
        arena_.used += n;
        placed_[m.id] = at;
        note_solid(m);
        counts_.meshes_placed++;
        return at;
    }

    // (tolerant layers) A batch that fades: blended (PRIM's ABE, in its tag) with a corner's alpha below 0x80
    static bool fades(const RdBatch &b) {
        return ((b.tag[0] >> 53) & 1) != 0 && b.alpha_lo < 0x80;
    }
    // A triangle by the places of its three corners in its mesh (before its draw's matrix), whatever their order; 0: two
    // of them at one place (nothing drawn)
    static uint64_t triangle_of(const RdMeshData &m, const RdBatch &b, size_t t) {
        uint64_t c[3];
        for (int i = 0; i < 3; i++) {
            const int32_t *p = &m.vertices[size_t(b.first + b.triangles[t * 3 + i]) * rd_vertex_words(m.format)];
            c[i] = uint64_t(uint16_t(p[0])) | uint64_t(uint16_t(p[1])) << 16 | uint64_t(uint16_t(p[2])) << 32;
        }
        std::sort(c, c + 3);
        if (c[0] == c[1] || c[1] == c[2]) {
            return 0;
        }
        return ((c[0] * 0x9E3779B97F4A7C15ull ^ c[1]) * 0x9E3779B97F4A7C15ull ^ c[2]) * 0x9E3779B97F4A7C15ull | 1;
    }
    // (a mesh placed: the triangles of its batches that do not fade)
    void note_solid(const RdMeshData &m) {
        if (m.format != RD_VF_PACKED16) {
            return;
        }
        for (const RdBatch &b : m.batches) {
            for (size_t t = 0; !fades(b) && t < b.triangles.size() / 3; t++) {
                solid_.insert(triangle_of(m, b, t));
            }
        }
        solid_generation_++;
    }
    // Whether a batch that fades is a twin: each of its triangles has the three places of a triangle of a batch that
    // does not fade, of any mesh placed. The game draws a surface's second texture as the surface's own strips again,
    // in a draw of its own by the same matrix, the vertices' alphas saying how much of it shows. Drawn corner for corner
    // alike (place_triangles' order) the two have the same Z at every pixel, to the bit: a tie, which the later wins as
    // on the GS, with no slack and no second pass (pcr_canvas.cpp's tolerant passes). (Two meshes that share three
    // places by chance, drawn by other matrices, are no twins: the batch then goes without a slack only.) Asked again
    // when more meshes have been placed since it was not one: its surface may be met after it.
    bool twin(const RdMeshData &m, const RdBatch &b) {
        Twin &known = twins_[uint64_t(m.id) << 20 | uint64_t(&b - m.batches.data())];
        if (!known.yes && known.generation != solid_generation_) {
            known.generation = solid_generation_;
            known.yes = !b.triangles.empty();
            for (size_t t = 0; known.yes && t < b.triangles.size() / 3; t++) {
                uint64_t triangle = triangle_of(m, b, t);
                known.yes = triangle == 0 || solid_.count(triangle) != 0;
            }
        }
        return known.yes;
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
        // (tolerant layers: the order of its corners by their places in the mesh, in the first corner's top bits, so
        // that two triangles of the same three places, an overlay's and its surface's, are drawn corner for corner
        // alike and their Z comes out the same to the bit: pcr_nd000.vert)
        uint32_t words = rd_vertex_words(m.format);
        auto before = [&](uint32_t a, uint32_t c) {
            const int32_t *p = &m.vertices[size_t(b.first + a) * words], *q = &m.vertices[size_t(b.first + c) * words];
            return std::lexicographical_compare(p, p + 3, q, q + 3);
        };
        for (uint32_t t = 0; t < n; t++) {
            uint32_t kick = b.triangles[t * 3 + 2];
            uint32_t c[3] = {b.triangles[t * 3 + 0], b.triangles[t * 3 + 1], kick};
            uint32_t o[3] = {0, 1, 2};
            std::stable_sort(o, o + 3, [&](uint32_t i, uint32_t j) { return before(c[i], c[j]); });
            uint32_t order = o[0] * 2 + (o[1] > o[2] ? 1 : 0); // (0-5: which corner first, then whether the others swap)
            out[t * 4 + 0] = (first + c[0]) | order << 29;
            out[t * 4 + 1] = first + c[1];
            out[t * 4 + 2] = first + kick;
            out[t * 4 + 3] = (first + kick) | std::min(kick, 2u) << 28;
            // (tolerant layers: bit 30, a triangle of a batch that fades that is a surface's own again, among the
            // meshes placed so far: its Z is the surface's to the bit, and a layer's Z pass leaves it out,
            // pcr_nd000.vert. KF4_PC_LAYER_TWINS=0: none marked.)
            static const bool mark = !getenv("KF4_PC_LAYER_TWINS") || strcmp(getenv("KF4_PC_LAYER_TWINS"), "0") != 0;
            uint64_t triangle = mark && fades(b) ? triangle_of(m, b, t) : 0;
            bool twin_triangle = triangle != 0 && solid_.count(triangle) != 0;
            out[t * 4 + 3] |= twin_triangle ? 1u << 30 : 0;
            counts_.fading_triangles += fades(b) ? 1 : 0;
            counts_.twin_triangles += twin_triangle ? 1 : 0;
        }
        triangles_.used += n;
        triangles_placed_[key] = at;
        return at;
    }

    Device &device_;
    GpuList arena_, triangles_, records_, records_280_, records_ripple_, kicked_;
    std::vector<uint8_t> adc_; // (a water strip's ADC)
    PassMoves passes_;
    PassStream stream_;
    bool shown_kept_ = false;    // (since the frame's first picture between: the frame shown kept, 0xF2)
    uint32_t shown_kept_fbp_ = 0;
    float picture_t_ = 1.0f;
    std::unordered_map<RdMesh, uint32_t> placed_;              // a mesh's first vertex in the arena
    std::unordered_map<uint64_t, uint32_t> triangles_placed_; // a batch's first triangle, by its mesh and number
    // (tolerant layers: twin) the triangles of the placed meshes' batches that do not fade, by their three places; how
    // many times meshes were noted; and each fading batch's answer, with the count it was worked out at
    std::unordered_set<uint64_t> solid_;
    uint32_t solid_generation_ = 1;
    struct Twin {
        bool yes = false;
        uint32_t generation = 0;
    };
    std::unordered_map<uint64_t, Twin> twins_;
    std::vector<std::shared_ptr<const RdMeshData>> fetched_;
    PartMoves parts_;
    PartWalk walk_ = PartWalk::frame;
    bool frame_kept_ = false, trail_paired_ = false, moved_on_cpu_ = false;
    // (the frame's own walk kept) the trails' kicks' moves (take_trail_moves), and KF4_PC_TRAIL_KICKS (0: a trail
    // paired refuses the frame's own walk, its pictures walked, as before)
    std::vector<GsnInterpMove> trail_moves_;
    bool trail_kicks_ = trail_kicks_wanted();
    static bool trail_kicks_wanted() {
        const char *v = getenv("KF4_PC_TRAIL_KICKS");
        return !(v && strcmp(v, "0") == 0);
    }
    float picture_u_ = 0.0f; // (1 - the picture's t)

public:
    std::map<uint32_t, uint64_t> unsupported_; // (the draws not drawn, by program)
};

NativeDraws::NativeDraws(Device &device) : impl_(std::make_unique<Impl>(device)) {}
NativeDraws::~NativeDraws() = default;

int NativeDraws::kinds() {
    // (programs 0x000, 0x280, 0x5A0 and 0x700; the reflection's water, RD_PROGRAM_RIPPLE; the screen passes,
    // RD_PROGRAM_PASS)
    return PORT_ND_MAP | PORT_ND_SKY | PORT_ND_ITEMS | PORT_ND_PICKED | PORT_ND_OBJECTS | PORT_ND_SKINNED | PORT_ND_ARM |
           PORT_ND_GOLD | PORT_ND_SHADOWS | PORT_ND_EFFECTS | PORT_ND_WATER | PORT_ND_PASSES;
}

void NativeDraws::met(uint64_t value, GifFrontEnd &front, Canvas &canvas) {
    impl_->met(value, front, canvas);
}

void NativeDraws::frame_mark(uint64_t value) {
    impl_->frame_mark(value);
}

void NativeDraws::set_picture(const GsnInterpPicture *picture) {
    impl_->set_picture(picture);
}
bool NativeDraws::moved_on_cpu() const {
    return impl_->moved_on_cpu();
}

void NativeDraws::begin_frame_kept() {
    impl_->begin_frame_kept();
}

void NativeDraws::end_frame_kept() {
    impl_->end_frame_kept();
}

bool NativeDraws::trail_paired() const {
    return impl_->trail_paired();
}

std::vector<GsnInterpMove> NativeDraws::take_trail_moves() {
    return impl_->take_trail_moves();
}

NativeCounts NativeDraws::counts() const {
    return impl_->counts();
}

void NativeDraws::print_counts(FILE *out) const {
    const NativeCounts c = impl_->counts();
    fprintf(out,
            "[stats] pc native draws: %llu draws, %llu batches (%llu triangles); %llu lost (their slot reused), %llu of a "
            "program not drawn yet; %llu meshes placed in %llu arenas\n",
            (unsigned long long)c.draws, (unsigned long long)c.batches, (unsigned long long)c.triangles,
            (unsigned long long)c.lost, (unsigned long long)c.unsupported, (unsigned long long)c.meshes_placed,
            (unsigned long long)c.arenas);
    if (c.fading_triangles) {
        fprintf(out, "[stats] pc native draws, tolerant layers: %llu triangles placed of batches that fade, %llu of them "
                     "a surface's own again (left out of a layer's Z pass)\n",
                (unsigned long long)c.fading_triangles, (unsigned long long)c.twin_triangles);
    }
    fprintf(out, "[stats] pc native draws in pictures between: %llu moved by their own matrices, %llu trails paired\n",
            (unsigned long long)c.moved_models, (unsigned long long)c.paired_strips);
    for (const auto &[program, n] : impl_->unsupported_) {
        fprintf(out, "[stats] pc native draws not drawn yet: program 0x%x, %llu draws\n", program, (unsigned long long)n);
    }
}

} // namespace pcr
