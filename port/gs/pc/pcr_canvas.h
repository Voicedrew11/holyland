// The PC renderer's drawing (docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native GS", step 2):
// what the GS draws, without its VRAM. The GIF front end's events come in (pcr_gif.h); what they draw goes into
// surfaces, GPU images of their own for each buffer the game draws into, found by its base page, width and
// format, at the renderer's scale.
//
// - A primitive is drawn into its context's FRAME surface (and ZBUF's depth surface when TEST's ZTE is on), with
//   the GS's pixel arithmetic in shaders/pcr_prim.frag and Vulkan's blend for ALPHA. Primitives of the same state
//   go to the GPU together.
// - In a picture between (pcr_between.h: set_between), a primitive's corners name the kicks they were made of, and the
//   shaders make them again of those at the picture's time; a native batch's record says how it moves. The pictures
//   between's set (the picture, the kicks, the moves) is bound to every draw: zeros outside one.
// - Its texture is the texture store's (pcr_textures.h): decoded from its uploads, or, where a surface was drawn
//   since, read from that surface (a copy of it, so a surface can be read while drawn into).
// - An upload is given to the store, and written into the surfaces it falls on; a local copy is the store's when
//   it copies uploads, else made between surfaces.
// - The picture shown is a surface's rectangle (scanout), a surface made from the store's uploads when none was
//   drawn there.
//
// What is not done yet is counted (counts(): unsupported_*) and drawn as near as can be, never dropped silently.
//
// One thread: the PC renderer's ("kf4-pc"), which owns the device's command buffers while it works.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>

#include "device.hpp"
#include "pcr_between.h"
#include "pcr_gif.h"
#include "pcr_gpu_time.h"
#include "pcr_textures.h"

namespace pcr {

struct CanvasCounts {
    uint64_t primitives = 0, triangles = 0, batches = 0, passes = 0;
    uint64_t textures_uploaded = 0, textures_from_surfaces = 0, textures_missing = 0;
    uint64_t textures_sampled = 0, exact_textures_region = 0, exact_textures_part = 0;
    uint64_t surfaces_made = 0, surfaces_filled_from_uploads = 0, uploads_into_surfaces = 0, copies_between_surfaces = 0;
    uint64_t date_draws = 0, native_batches = 0, native_draw_calls = 0;
    uint64_t unsupported_blends = 0, unsupported_texture_on_surface = 0;
    uint64_t unsupported_copies = 0;
    uint64_t read_backs_from_uploads = 0, read_backs_from_surfaces = 0, unsupported_read_backs = 0;
    uint64_t uploads_made_again = 0, uploads_again_missed = 0, kept_frame_reads = 0;
};

// A rectangle of the GS's memory as the display reads it: DISPFB's base (in blocks), width (in 64 pixels),
// format, and the pixels (x, y, w, h)
struct ShownRect {
    uint32_t bp = 0, bw = 0, psm = 0, x = 0, y = 0, w = 0, h = 0;
};

// The VU1 program whose maths draws a native batch (its vertex shader)
enum class NativeProgram : uint8_t {
    vu1_000, // shaders/pcr_nd000.vert: a triangle the arena's numbers of its corners, and of its kick with the kicks
             // before it; instance: the record (bits 0-29), Gouraud (30)
    vu1_280, // shaders/pcr_nd280.vert: a triangle the arena's slots of its corners and of its kick; instance: the record
             // (bits 0-27), the context whose XYOFFSET (28), the sphere pass (29), Gouraud (30)
    vu1_700, // shaders/pcr_nd700.vert: a triangle the arena's numbers of its corners and of its kick; instance: the
             // record (bits 0-29), Gouraud (30)
};

// A batch of a native draw (pcr_native.h): its triangles from first_triangle in triangles (a uvec4 each), drawn by the
// record of records the instance names, the meshes' vertices in arena
struct NativeBatch {
    NativeProgram program = NativeProgram::vu1_000;
    Vulkan::BufferHandle arena, records, triangles;
    uint32_t first_triangle = 0, triangle_count = 0, instance = 0;
};

// A picture between's walk as the canvas made it (pcr_canvas.cpp): its batches, uploads, copies, CLUT loads and marks
// in order, every image and buffer they use held, and what it read of the texture store from before it
class KeptWalk;

class Canvas {
public:
    // times: the GPU's time of what the canvas records (outlives the canvas)
    Canvas(Vulkan::Device &device, GpuTimes &times);
    ~Canvas();
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;

    // The surfaces' pixels a GS pixel is, across and down (1 to 8): from the next vblank
    void set_scale(uint32_t x_scale, uint32_t y_scale);
    // A picture between's walk begins (its buffers: the canvas reads them at each draw until set_between(nullptr)) or
    // ends; the open batch is drawn first either way (its corners are the walk's, or not)
    void set_between(const Between *between);

    // Step 5b: a walk's calls kept. keep_walk() before set_between(between): every call until set_between(nullptr)
    // is kept as well as made (its images and buffers held), with what it read of the texture store from before it,
    // then taken by take_kept_walk (nullptr when nothing was kept, or the walk read back from the GS's memory)
    void keep_walk();
    std::shared_ptr<const KeptWalk> take_kept_walk();
    // Whether a kept walk can be made again now: every page of the store it read from before it as it was then (the
    // surfaces are looked up again as its draws are made)
    bool fits(const KeptWalk &walk) const;
    // A kept walk's calls made again, between's picture bound as set_between's (its kicks: the walk's), without the
    // walk: its draws, copies of surfaces, uploads and copies, CLUT loads and marks in their order, so that the store
    // and the surfaces are left as the walk leaves them
    void draw_again(const KeptWalk &walk, const Between &between);

    // The front end's events (pcr_gif.h), with its register file at each
    void primitive(const GsRegisterFile &file, const GifPrimitive &primitive);
    void texture_written(uint64_t tex0, uint64_t texclut);
    // A native batch, with the registers as its writes left them and PRIM's 11 bits in effect. Its texture is read
    // whole; what it draws over is taken as its scissor's rectangle.
    void native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &batch);
    void upload(const TransferRegisters &transfer, std::span<const uint8_t> bytes);
    void local_copy(const TransferRegisters &transfer);
    // A local-to-host transfer (TRXDIR 1): its pixels made ready for read_fifo, from the uploads or the surface
    void local_to_host(const TransferRegisters &transfer);
    // The read-back's next qwords (zeroes past its end). Any thread, while the canvas's thread is idle (after
    // everything sent before was done: pcr_read_fifo waits for that).
    void read_fifo(void *dst, uint32_t qwords);
    // The port's marks for the pictures between (frame_interp.c; the native GS's meaning): 0x7D's 0xF0 and 0xF1 a
    // picture between begins and ends (its uploads are not the frame's), 0xF2 the frame shown kept, 0xF4 to 0xF5
    // the draws that read that kept frame in place of its buffer; 0x7C the frame's k-th upload made again (its
    // registers checked against those set now, as the native GS checks them)
    void frame_mark(uint64_t value);
    void upload_again(const GsRegisterFile &file, uint64_t value);

    // A state loaded: nothing of the GS's memory is known any more
    void forget();

    // Save states: the GS's 4 MB (the uploads', with the colour surfaces' pixels over the pages drawn last) and the
    // CLUT buffer; taken back as the uploads' (the surfaces made again from them as they are drawn into or shown).
    // Z buffers are not kept (the game draws its Z anew each frame).
    std::vector<uint8_t> vram_image();
    TextureStore::ClutState clut_state() const;
    void restore(std::span<const uint8_t> vram, const TextureStore::ClutState &clut);

    // Everything drawn so far recorded, the shown rectangle copied (scaled to dst's size) into dst, which the
    // commands that follow in cmd may read. False when there is nothing to show (dst left as it was).
    bool scanout(Vulkan::CommandBuffer &cmd, const ShownRect &rect, const Vulkan::Image &dst);
    // The command buffer the canvas records into, submitted (before a picture's own commands are)
    void submit();
    // A vblank (each, whether a picture is made or not): what was drawn submitted, a new scale taken
    void vblank();

    CanvasCounts counts() const;
    // --stats: the "[stats] pc canvas" lines, and the texture store's
    void print_counts() const;

private:
    class Impl;
    friend class KeptWalk;
    std::unique_ptr<Impl> impl_;
};

} // namespace pcr
