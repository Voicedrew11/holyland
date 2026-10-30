// The PC renderer's drawing (docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native GS", step 2):
// what the GS draws, without its VRAM. The GIF front end's events come in (pcr_gif.h); what they draw goes into
// surfaces, GPU images of their own for each buffer the game draws into, found by its base page, width and
// format, at the renderer's scale.
//
// - A primitive is drawn into its context's FRAME surface (and ZBUF's depth surface when TEST's ZTE is on), with
//   the GS's pixel arithmetic in shaders/pcr_prim.frag and Vulkan's blend for ALPHA. Primitives of the same state
//   go to the GPU together.
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
#include "pcr_gif.h"

namespace pcr {

struct CanvasCounts {
    uint64_t primitives = 0, triangles = 0, batches = 0, passes = 0;
    uint64_t textures_uploaded = 0, textures_from_surfaces = 0, textures_missing = 0;
    uint64_t surfaces_made = 0, surfaces_filled_from_uploads = 0, uploads_into_surfaces = 0, copies_between_surfaces = 0;
    uint64_t date_draws = 0;
    uint64_t unsupported_blends = 0, unsupported_texture_on_surface = 0;
    uint64_t unsupported_copies = 0;
    uint64_t read_backs_from_uploads = 0, read_backs_from_surfaces = 0, unsupported_read_backs = 0;
};

// A rectangle of the GS's memory as the display reads it: DISPFB's base (in blocks), width (in 64 pixels),
// format, and the pixels (x, y, w, h)
struct ShownRect {
    uint32_t bp = 0, bw = 0, psm = 0, x = 0, y = 0, w = 0, h = 0;
};

class Canvas {
public:
    explicit Canvas(Vulkan::Device &device);
    ~Canvas();
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;

    // The surfaces' pixels a GS pixel is, across and down (1 to 8): from the next vblank
    void set_scale(uint32_t x_scale, uint32_t y_scale);

    // The front end's events (pcr_gif.h), with its register file at each
    void primitive(const GsRegisterFile &file, const GifPrimitive &primitive);
    void texture_written(uint64_t tex0, uint64_t texclut);
    void upload(const TransferRegisters &transfer, std::span<const uint8_t> bytes);
    void local_copy(const TransferRegisters &transfer);
    // A local-to-host transfer (TRXDIR 1): its pixels made ready for read_fifo, from the uploads or the surface
    void local_to_host(const TransferRegisters &transfer);
    // The read-back's next qwords (zeroes past its end). Any thread, while the canvas's thread is idle (after
    // everything sent before was done: pcr_read_fifo waits for that).
    void read_fifo(void *dst, uint32_t qwords);
    // A state loaded: nothing of the GS's memory is known any more
    void forget();

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
    std::unique_ptr<Impl> impl_;
};

} // namespace pcr
