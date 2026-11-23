// What a renderer under the PC renderer's shell draws with (port/gs/pc/pcr.cpp: its thread, inbox, scanout ring and
// presenter, shared): the GIF front end's events (pcr_gif.h), the native draws' batches (pcr_native.h), the
// pictures' scanout and the save states' memory. Two implement it:
// - pcr::Canvas (port/gs/pc/pcr_canvas.h), today's PC renderer: the GS's stream replayed as it comes;
// - scn::Builder (port/gs/scene/scn_build.h), the scene renderer (--renderer scene, developers' only): the stream
//   built into a scene, closed at the vblank and drawn from it (docs/plans/scene-renderer-design.md).
// One thread calls a Drawing (the shell's "kf4-pc"); read_fifo also from another while that thread is idle.
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <vector>

#include "device.hpp"
#include "gsn.h"
#include "pcr_gif.h"
#include "pcr_textures.h"

namespace pcr {

constexpr uint32_t no_instance = 0xFFFFFFFFu;

// The VU1 program whose maths draws a native batch (its vertex shader)
enum class NativeProgram : uint8_t {
    vu1_000, // shaders/pcr_nd000.vert: a triangle the arena's numbers of its corners, and of its kick with the kicks
             // before it; instance: the record (bits 0-29), Gouraud (30)
    vu1_280, // shaders/pcr_nd280.vert: a triangle the arena's slots of its corners and of its kick; instance: the record
             // (bits 0-27), the context whose XYOFFSET (28), the sphere pass (29), Gouraud (30)
    vu1_700, // shaders/pcr_nd700.vert: a triangle the arena's numbers of its corners and of its kick; instance: the
             // record (bits 0-29), Gouraud (30)
    ripple,  // shaders/pcr_ndripple.vert (the reflection's water, no VU1 program): a triangle the arena's numbers of its
             // corners; instance: the record (bits 0-29)
};

// A batch of a native draw (pcr_native.h): its triangles from first_triangle in triangles (a uvec4 each), drawn by the
// record of records the instance names, the meshes' vertices in arena
struct NativeBatch {
    NativeProgram program = NativeProgram::vu1_000;
    Vulkan::BufferHandle arena, records, triangles;
    uint32_t first_triangle = 0, triangle_count = 0, instance = 0;
    // (programs 0x000 and 0x280) the arena's vertices its triangles' corners are of: the first's slot and how many
    // (a vertex one slot for 0x000, two for 0x280); the scene renderer places each once a draw (scn_place.comp)
    uint32_t first_vertex = 0, vertex_count = 0;
    // (the frame's own walk kept: NativeDraws::begin_frame_kept) the instance its pictures between draw it with, the
    // record with its move; no_instance: none
    uint32_t picture_instance = no_instance;
    // Every corner's alpha is 0x80 exactly as the program leaves it in RGBAQ (program 0x000: its mesh's colours' alpha,
    // which the lights do not touch). False when not known. A blend by As that is then the source alone is drawn
    // without the blend (pcr_canvas.cpp's blend_is_source).
    bool alpha_is_one = false;
    // (developers' counters, KF4_PC_TIES and KF4_PC_OVERDRAW: pcr_canvas.cpp; the scene's DATE: scn_build.cpp) the draw it
    // is of: its RdDraw's kind (RD_KIND_*) and identity (port/include/port/render.h: the owner's EE address, the part, a
    // slot)
    uint32_t kind = 0, id[3] = {};
};

// A rectangle of the GS's memory as the display reads it: DISPFB's base (in blocks), width (in 64 pixels),
// format, and the pixels (x, y, w, h). lower_field 1: a field shown at twice its lines that an interlaced TV shows
// a line lower than the other (the odd lines): the game drew it half a field's line higher for that
// (sceGsSetHalfOffset), so it is moved down a line of the picture. woven 1: rect is an upper field shown at twice its
// lines, and the buffer at lower_bp (same width, format and place) holds the lower field of the same moment
// (whole frames: port/platform/field_frames.c), to be woven between its lines as an interlaced TV shows them.
struct ShownRect {
    uint32_t bp = 0, bw = 0, psm = 0, x = 0, y = 0, w = 0, h = 0;
    uint32_t lower_field = 0;
    uint32_t woven = 0, lower_bp = 0;
};

// After a scanout of rect into a picture dst's size: the depth of its 3D (Drawing::scanout_depth)
struct ShownDepth {
    float zscale = 0.0f;      // 1 / 2^24, or 1 / 2^32 for a 32-bit Z buffer
    float xyoffset[2] = {};   // GS pixels: a place on the GS's screen less it is the buffer's pixel
    uint64_t drawing = 0;     // the drawing (begin_drawing's) that last drew 3D into the buffer
};

// (pcr_jitter.h)
struct Jitter;

// What the native draws hand their batches and marks to (pcr_native.h)
class NativeSink {
public:
    virtual ~NativeSink() = default;
    // A native batch, with the registers as its writes left them and PRIM's 11 bits in effect
    virtual void native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &batch) = 0;
    // The port's marks for the pictures between (frame_interp.c's meaning: pcr_canvas.h's frame_mark)
    virtual void frame_mark(uint64_t value) = 0;
};

class Drawing : public NativeSink {
public:
    // Settings, from the next vblank: the scale (1 to 8 a GS pixel across and down), texture detail and anisotropy,
    // the 2D mode, the font, the controller's buttons (gsn.h's GSN_*)
    virtual void set_scale(uint32_t x_scale, uint32_t y_scale) = 0;
    virtual void set_sampling(int lod, uint32_t aniso) = 0;
    virtual void set_2d(int mode) = 0;
    virtual void set_font(int font) = 0;
    virtual void set_glyphs(const GsnGlyphs &glyphs) = 0;

    // The front end's events (pcr_gif.h), with its register file at each
    virtual void primitive(const GsRegisterFile &file, const GifPrimitive &primitive) = 0;
    virtual void texture_written(uint64_t tex0, uint64_t texclut) = 0;
    virtual void upload(const TransferRegisters &transfer, std::span<const uint8_t> bytes) = 0;
    virtual void local_copy(const TransferRegisters &transfer) = 0;
    // A local-to-host transfer (TRXDIR 1): its pixels made ready for read_fifo
    virtual void local_to_host(const TransferRegisters &transfer) = 0;
    // The read-back's next qwords (zeroes past its end). Any thread, while the drawing's thread is idle.
    virtual void read_fifo(void *dst, uint32_t qwords) = 0;
    // 0x7C: the frame's k-th upload made again (pcr_canvas.h's)
    virtual void upload_again(const GsRegisterFile &file, uint64_t value) = 0;

    // A state loaded: nothing of the GS's memory is known any more
    virtual void forget() = 0;
    // Save states: the GS's 4 MB as drawn (the uploads', the colour targets' pixels over the pages drawn last) and the
    // CLUT buffer; taken back as uploads'
    virtual std::vector<uint8_t> vram_image() = 0;
    virtual TextureStore::ClutState clut_state() const = 0;
    virtual void restore(std::span<const uint8_t> vram, const TextureStore::ClutState &clut) = 0;

    // Everything drawn so far recorded, the shown rectangle copied (scaled to dst's size) into dst, which the commands
    // that follow in cmd may read. False when there is nothing to show (dst left as it was).
    virtual bool scanout(Vulkan::CommandBuffer &cmd, const ShownRect &rect, const Vulkan::Image &dst) = 0;
    // (a vblank shown again: docs/plans/scene-slice5-results.md) The last scanout was of rect, as the buffer is, and
    // nothing drawn, cleared or copied since meets what it read: the same picture. False when not known (the canvas).
    virtual bool scanout_unchanged(const ShownRect &rect) {
        (void)rect;
        return false;
    }
    // After scanout: the depth of the picture's 3D copied into dst (R32F), where it can be given (pcr_canvas.h's)
    virtual std::optional<ShownDepth> scanout_depth(Vulkan::CommandBuffer &cmd, const ShownRect &rect,
                                                    const Vulkan::Image &dst) = 0;
    // (the temporal anti-aliasing) What is drawn from here on is drawing number drawing, its 3D moved by jitter
    virtual void begin_drawing(uint64_t drawing, Jitter jitter) = 0;
    // What is recorded, submitted (before a picture's own commands are)
    virtual void submit() = 0;
    // A vblank (each, whether a picture is made or not). game_vblank: the game's number for it where known.
    virtual void vblank(uint64_t game_vblank) = 0;

    // --stats: its lines
    virtual void print_counts() const = 0;
    // Whether the scale last set is drawn with (it is taken at a vblank: kf4-gsreplay waits for it)
    virtual bool settled() const = 0;

    // Pictures between (a renderer with its own way of drawing them: the scene renderer's; the canvas's are the shell's,
    // pcr_between.h). frame_taken: frame_interp.c hands over a game frame (its GsnInterpFrame), whose packets are those
    // taken in since the last vblank (open), those between the two vblanks before (last: the frame is mostly taken at
    // its first picture between, a vblank after its packets), or neither (none: no picture between of it is drawn).
    // While a frame's packets may be (frames taken lately) their kicks are numbered and the native draws' records are
    // put twice, the frame's and its pictures' (NativeBatch::picture_instance); the trail's and the passes' kicks
    // numbered from native_kick_base are the frame's kicks' count on.
    enum class FrameStretch : uint8_t { none, open, last };
    struct FrameShape {
        uint32_t draw_fbp = 0, shown_fbp = 0; // (GsnInterpFrame's: the buffer drawn, the one shown, FBP in pages)
        uint32_t kicks = 0;                    // the frame's stream's kicks (native_kick_base's are numbered on from it)
    };
    virtual void frame_taken(FrameStretch stretch, const FrameShape &shape) {
        (void)stretch;
        (void)shape;
    }
    // A picture between of the frame taken: its buffers (pcr_between.h's picture, kicks and moves: the shaders' set 0
    // bindings 0 to 2) while it is drawn. True when drawn (the next picture shows it).
    struct PictureBuffers {
        const Vulkan::Buffer *picture = nullptr, *kicks = nullptr, *moves = nullptr;
    };
    // A picture made (a vblank's or an extra one), whether it was drawn (scanout) or not: a picture between drawn
    // before it was its to show, and is not the next one's
    virtual void picture_made() {}
    virtual bool picture_between(const GsnInterpPicture &picture, const PictureBuffers &buffers) {
        (void)picture;
        (void)buffers;
        return false;
    }
    // (the scene renderer's slice 5, docs/plans/scene-slice5-results.md) What the stream adds from here comes before this
    // vblank's pictures between still to be drawn and the extra pictures that show them, which are drawn as if it came
    // after them (as it was sent). False when that cannot be (nothing is to be taken ahead). Open until the drawing must
    // draw what was added (packets_ahead_open false): the vblank, a frame taken, a read-back.
    virtual bool packets_ahead() {
        return false;
    }
    virtual bool packets_ahead_open() const {
        return false;
    }
    // A picture sent after the packets taken ahead: what was added drawn first, as the picture would in its turn
    virtual void packets_ahead_end() {}
};

} // namespace pcr
