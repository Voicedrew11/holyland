// The PC renderer's GIF front end (docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native GS"):
// the GIF packets of PATH1, 2 and 3 taken as the GS takes them, and turned into events for the renderer. It keeps
// the GS's register file and the GIF's tag state, and nothing else: no VRAM, no drawing.
//
// It is the native GS's front end (port/gs/native/gsn.cpp: gif(), packed(), gs_write(), vertex_kick(),
// begin_transfer(), image_data()) again, without what that does with the packets.
//
// The events, in the order the packets make them (GifEvents below). At each, registers() is the register file as
// the write left it (the write's own value stored first), so the renderer reads FRAME, ZBUF, TEST, ALPHA, TEX0 ...
// there when it needs them.
// - register_written: a register of the file changed (old and new value). Not for the vertex registers (RGBAQ, ST,
//   UV, FOG: they come in the primitives' vertices), nor TEX0 (its own event), nor the kicks (XYZ*), which the file
//   does not keep, as gsn's does not.
// - texture_written: TEX0_1 or TEX0_2 written, or TEX2_1 or TEX2_2 (which write TEX0's PSM and CLUT fields only),
//   each time, changed or not: the renderer loads the CLUT then. With the TEX0 that results and TEXCLUT then.
//   MIPTBP1 is made from it first when TEX1's MTBA says so (as the GS does).
// - primitive: a primitive finished by a drawing kick (XYZ2 or XYZF2, or PACKED XYZ with ADC clear). Strips and
//   fans come as their triangles, one event each, the vertices in the GS's queue order; a line strip as its lines.
//   A kick by XYZ3 or XYZF3 (PACKED: ADC set) moves the queue on as the GS does but draws nothing. A PRIM write
//   empties the queue. PRIM type 7 draws nothing.
// - image_upload: a host-to-local transfer (TRXDIR 0) whose bytes have all come (IMAGE data or HWREG writes, over
//   any number of packets and feeds): the pixels' bytes, w * h * bits rounded up to a byte, and no more. A new
//   TRXDIR before then gives what had come, UploadEnd::cut_short. A transfer of no pixels (TRXREG's width or
//   height 0) or of a format that is not one gives nothing, and its data is dropped (as gsn's).
// - local_copy (TRXDIR 2), local_to_host (TRXDIR 1): with the transfer's registers as TRXDIR found them.
// - native_draw (0x7B), upload_again (0x7C), frame_mark (0x7D), widescreen (0x7E): the port's own registers. A
//   native draw's handler may write the registers its batches kick (write()).
// - gs_sync: SIGNAL, FINISH, LABEL (not kept in the file, as gsn's does not keep them).
// TEXFLUSH and NOP (0x0F) make no event.
//
// Packets may come split, at qword boundaries, over any number of feeds; each path keeps its own tag. Bytes past
// the last whole qword of a feed are not read. A tag whose NLOOP runs past the data waits for the next feed.
// Not thread-safe: one thread feeds it (the one the renderer's GS work runs on), and an event must not feed it
// again.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace pcr {

// The GS's register addresses the front end treats on their own, and those the renderer reads at a draw
namespace gs_reg {
constexpr uint32_t prim = 0x00, rgbaq = 0x01, st = 0x02, uv = 0x03, xyzf2 = 0x04, xyz2 = 0x05, tex0_1 = 0x06,
                   tex0_2 = 0x07, clamp_1 = 0x08, clamp_2 = 0x09, fog = 0x0A, xyzf3 = 0x0C, xyz3 = 0x0D,
                   tex1_1 = 0x14, tex1_2 = 0x15, tex2_1 = 0x16, tex2_2 = 0x17, xyoffset_1 = 0x18,
                   xyoffset_2 = 0x19, prmodecont = 0x1A, prmode = 0x1B, texclut = 0x1C, scanmsk = 0x22,
                   miptbp1_1 = 0x34, miptbp1_2 = 0x35, miptbp2_1 = 0x36, miptbp2_2 = 0x37, texa = 0x3B,
                   fogcol = 0x3D, texflush = 0x3F, scissor_1 = 0x40, scissor_2 = 0x41, alpha_1 = 0x42,
                   alpha_2 = 0x43, dimx = 0x44, dthe = 0x45, colclamp = 0x46, test_1 = 0x47, test_2 = 0x48,
                   pabe = 0x49, fba_1 = 0x4A, fba_2 = 0x4B, frame_1 = 0x4C, frame_2 = 0x4D, zbuf_1 = 0x4E,
                   zbuf_2 = 0x4F, bitbltbuf = 0x50, trxpos = 0x51, trxreg = 0x52, trxdir = 0x53, hwreg = 0x54,
                   signal = 0x60, finish = 0x61, label = 0x62;
// the port's own (port/include/port/host.h's PORT_GS_REG_*)
constexpr uint32_t native_draw = 0x7B, upload_again = 0x7C, frame_mark = 0x7D, widescreen = 0x7E;
} // namespace gs_reg

// The GS's registers: all 256 addresses as written (PRIM's 11 bits only), and the GIF's Q, which a PACKED ST
// sets, a PACKED RGBAQ takes into RGBAQ, and every tag sets back to 1.0
struct GsRegisterFile {
    std::array<uint64_t, 256> reg{};
    uint32_t internal_q = 0x3F800000u; // (a float's bits)
};

// A vertex: what the GS latched at its kick
struct GifVertex {
    uint16_t x = 0, y = 0; // XYZ's X and Y: 12.4 fixed, as written (XYOFFSET not taken off)
    uint32_t z = 0;        // 24 bits from XYZF, 32 from XYZ
    uint32_t rgba = 0;     // RGBAQ's R, G, B, A (R in the low byte)
    float q = 1.0f;        // RGBAQ's Q
    float s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0; // UV: 10.4 fixed (14 bits)
    uint8_t fog = 0;       // XYZF's F, or FOG's when the kick was XYZ
};

enum class PrimitiveShape : uint8_t { point, line, triangle, sprite };

struct GifPrimitive {
    PrimitiveShape shape = PrimitiveShape::point;
    // PRIM's 11 bits in effect: PRIM's type, and its attributes (IIP, TME, FGE, ABE, AA1, FST, CTXT, FIX) or, when
    // PRMODECONT's AC is 0, PRMODE's. The type stays the one written (4 for a strip's triangle, 5 for a fan's).
    uint32_t prim = 0;
    uint32_t vertex_count = 0; // 1, 2, 3 or 2 (a sprite's two corners)
    std::array<GifVertex, 3> vertices{};
};

// BITBLTBUF, TRXPOS and TRXREG as a transfer began with them
struct TransferRegisters {
    uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
};

enum class UploadEnd : uint8_t { complete, cut_short };

// What the renderer is told (the order and meaning: the top of this file)
class GifEvents {
public:
    virtual ~GifEvents() = default;
    virtual void register_written(uint32_t address, uint64_t old_value, uint64_t new_value) = 0;
    virtual void texture_written(uint32_t context, uint64_t tex0, uint64_t texclut) = 0; // context 0 or 1
    virtual void primitive(const GifPrimitive &primitive) = 0;
    // (bytes are valid during the call only)
    virtual void image_upload(const TransferRegisters &transfer, std::span<const uint8_t> bytes, UploadEnd end) = 0;
    virtual void local_copy(const TransferRegisters &transfer) = 0;
    virtual void local_to_host(const TransferRegisters &transfer) = 0;
    virtual void native_draw(uint64_t value) = 0;
    virtual void upload_again(uint64_t value) = 0;
    virtual void frame_mark(uint64_t value) = 0;
    virtual void widescreen(uint64_t value) = 0;
    virtual void gs_sync(uint32_t address, uint64_t value) = 0; // SIGNAL, FINISH or LABEL
};

class GifFrontEnd {
public:
    static constexpr int path_count = 4; // PATH1, 2 and 3 by their numbers (0 is taken too, as gsn takes it)

    explicit GifFrontEnd(GifEvents &events);

    // A path's bytes, as many whole qwords as there are; a path out of 0..3 is ignored
    void feed(int path, std::span<const uint8_t> bytes);

    const GsRegisterFile &registers() const { return file_; }

    // A register written as an A+D write is, with its events: what a native draw's batch kicks (pcr_native.h). Not
    // during a feed's event but native_draw's.
    void write(uint32_t address, uint64_t value) { write_register(address, value); }

    // PRIM's 11 bits in effect (GifPrimitive::prim's: PRMODE's attributes when PRMODECONT's AC is 0)
    uint32_t prim_in_effect() const;

    // A state loaded: its register file taken, every path between packets, no vertex queued, no transfer begun
    void restore(const GsRegisterFile &file);

private:
    enum class TagMode : uint8_t { packed, reglist, image };
    struct PathTag {
        TagMode mode = TagMode::packed;
        uint32_t qwords_left = 0;  // of the tag's data; 0: the next qword is a tag
        uint32_t dwords_left = 0;  // REGLIST: of the tag's registers (the last qword may be half used)
        uint32_t register_count = 16;
        uint32_t next_register = 0;
        uint64_t registers = 0;    // the tag's REGS: 4 bits a register
    };
    struct PendingUpload {
        TransferRegisters transfer;
        size_t expected_bytes = 0;
        std::vector<uint8_t> bytes;
    };

    void read_tag(PathTag &tag, uint64_t low, uint64_t high);
    void write_packed(uint32_t descriptor, uint64_t low, uint64_t high);
    void write_reglist(PathTag &tag, uint64_t low, uint64_t high);
    void write_register(uint32_t address, uint64_t value);
    void store(uint32_t address, uint64_t value);
    void write_prim(uint64_t value);
    void write_tex0(uint32_t context, uint64_t value);
    void make_mip_addresses(uint32_t context);
    void kick(uint32_t address, uint64_t value);
    GifVertex latch_vertex(uint32_t address, uint64_t value) const;
    void emit(PrimitiveShape shape, uint32_t prim, uint32_t vertex_count);
    void begin_transfer(uint32_t direction);
    void cut_upload_short();
    void image_data(std::span<const uint8_t> bytes);

    GifEvents &events_;
    GsRegisterFile file_;
    std::array<PathTag, path_count> paths_{};
    std::array<GifVertex, 3> queue_{}; // the GS's vertex queue
    uint32_t queued_ = 0;
    std::optional<PendingUpload> upload_;
};

} // namespace pcr
