// The PC renderer's native draws (docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native GS", step 3):
// the game's draws handed over at their own level (port/include/port/render.h) instead of VU1's packets, met in the
// chain as PORT_GS_REG_NATIVE_DRAW writes and drawn there, in order with everything else.
//
// A draw of program 0x000 (the map's records, the sky, the objects, the items, the picked item) is drawn as VU1 and the
// GS would have drawn its packets: for each batch (one VU1 run) the registers the run kicks are written to the GIF front
// end as the GS gets them (FOGCOL at the set-up, the A+D packet, PRIM from the strip's tag), so the GS's state after the
// draw is the PS2 path's; then the canvas draws the batch's triangles from its mesh on the GPU, with VU1's maths in the
// vertex shader (shaders/pcr_nd000.vert, port/render/shaders/vu1_000_*.h) and the GS's pixel as for any primitive.
// On the GPU: the meshes' vertices in one buffer (the arena: a mesh put there when first drawn), the batches' triangles
// in another (placed once a mesh), the draws' records (port/render/vu1_record.hpp) in a third; all written by the CPU
// before the GPU reads them, and never written again where a draw reads (a full buffer is replaced).
//
// The kinds it draws (kinds()) are the only ones gs.c turns on under the PC renderer; a draw of another program met all
// the same is counted and left out.
//
// One thread: the PC renderer's ("kf4-pc").
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>

#include "device.hpp"
#include "pcr_canvas.h"
#include "pcr_gif.h"

namespace pcr {

struct NativeCounts {
    uint64_t draws = 0, batches = 0, triangles = 0, lost = 0, unsupported = 0;
    uint64_t meshes_placed = 0, arenas = 0;
};

class NativeDraws {
public:
    explicit NativeDraws(Vulkan::Device &device);
    ~NativeDraws();
    NativeDraws(const NativeDraws &) = delete;
    NativeDraws &operator=(const NativeDraws &) = delete;

    // The --native-draw kinds the PC renderer draws (PORT_ND_* bits)
    static int kinds();

    // PORT_GS_REG_NATIVE_DRAW's value, met in the chain (the front end's native_draw event): a draw drawn, its batches'
    // registers written through front; a frame's mark said to the renderer core
    void met(uint64_t value, GifFrontEnd &front, Canvas &canvas);

    NativeCounts counts() const;
    // --stats: the "[stats] pc native draws" line
    void print_counts(FILE *out) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pcr
