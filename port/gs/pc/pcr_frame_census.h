// KF4_PC_FRAME_CENSUS=FILE (developers' only, --renderer pc): what each game frame hands the renderer, counted frame by
// frame, for the scene renderer's design (docs/plans/scene-renderer-design.md, "The frame as a scene"). The frames are
// the stretches of packets between two vblanks (a game frame's chain comes within one); KF4_PC_FRAME_CENSUS_AT="A-B,C"
// (the game's vblanks, as --dump-frames numbers them) writes only the stretches closed at those vblanks, else every one
// with packets. One JSON object a line (tools/scene_census.py reads them):
// - the bytes by path; the GIF primitives by path (1, 2, 3, or "native": made by a native draw's handler, the screen
//   passes and the weapon's trail) and by class (shape, textured, blended, depth-tested, DATE), with their pixels (each
//   primitive's rectangle in the scissor, GS pixels: an upper bound for a triangle);
// - the native draws by kind and program, their batches, triangles, sphere passes (program 0x280), meshes and cameras;
// - the draws in the stream's order (a run of GIF primitives of one state is one draw, a native batch one): how many
//   runs of one pipeline (the GS state less the texture), of one pipeline and texture, of one target; how many
//   pipelines, textures and targets in all; and between consecutive batches of program 0x280, what changed;
// - the transfers (uploads and their bytes, by where they go; uploads asked again; local copies; read-backs) and marks;
// - the canvas's own counts over the stretch (its batches, draw calls, passes, clears, DATE draws, textures read from
//   surfaces).
// One thread: the PC renderer's (kf4-pc); the caller leaves out pictures between's walks.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>

#include "pcr_canvas.h"
#include "pcr_gif.h"

namespace pcr {

class FrameCensus {
public:
    // From KF4_PC_FRAME_CENSUS (nullptr when unset or the file cannot be written)
    static std::unique_ptr<FrameCensus> from_env();
    ~FrameCensus();

    void bytes(int path, size_t n);
    // native: made inside a native draw's handler (path then means nothing)
    void primitive(int path, bool native, const GsRegisterFile &file, const GifPrimitive &primitive);
    void native_draw(uint64_t value);
    void native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &batch);
    void upload(const TransferRegisters &transfer, size_t bytes);
    void upload_again();
    void local_copy(const TransferRegisters &transfer);
    void read_back(const TransferRegisters &transfer);
    void frame_mark(uint64_t value);
    // A vblank: the stretch since the last closed and written when wanted; canvas: the canvas's counts now
    void vblank(uint64_t game_vblank, const CanvasCounts &canvas);

private:
    FrameCensus();
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pcr
