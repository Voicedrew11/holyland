// The PC renderer's drawing (docs/port/PC_RENDERER.md, step 2):
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
// Two canvases, a thread each: the PC renderer's ("kf4-pc"), which takes the GS's input in, and the pump's
// ("kf4-pc-pump", pcr.cpp), which draws the pictures between again from kept walks (draw_again_alone) into surfaces of
// its own, copied from kf4-pc's (copy_surfaces, start_from). Each canvas is used by its thread only; what passes
// between them (a KeptWalk, SurfaceCopies) is not written once handed over.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "device.hpp"
#include "gs_drawing.h"
#include "pcr_between.h"
#include "pcr_frame_walk.h"
#include "pcr_gif.h"
#include "pcr_gpu_time.h"
#include "pcr_jitter.h"
#include "pcr_textures.h"
#include "pcr_trace.h"

namespace pcr {

struct CanvasCounts {
    uint64_t primitives = 0, triangles = 0, batches = 0, passes = 0;
    uint64_t textures_uploaded = 0, textures_from_surfaces = 0, textures_missing = 0;
    uint64_t textures_sampled = 0, exact_textures_region = 0, exact_textures_part = 0;
    uint64_t textures_mipmapped = 0, textures_anisotropic = 0; // (of textures_sampled)
    uint64_t surfaces_made = 0, surfaces_filled_from_uploads = 0, uploads_into_surfaces = 0, copies_between_surfaces = 0;
    uint64_t surface_images_reused = 0, texture_copies_reused = 0; // (KF4_PC_SURFACE_REUSE)
    uint64_t date_draws = 0, native_batches = 0, native_draw_calls = 0;
    uint64_t unsupported_blends = 0, unsupported_texture_on_surface = 0;
    uint64_t unsupported_copies = 0;
    uint64_t read_backs_from_uploads = 0, read_backs_from_surfaces = 0, unsupported_read_backs = 0;
    uint64_t uploads_made_again = 0, uploads_again_missed = 0, kept_frame_reads = 0;
    uint64_t surfaces_copied = 0; // (for the pump: copy_surfaces)
    uint64_t surface_copies_into_spares = 0; // (its images, a surface's and its multisampled one, not made new)
    // (a kept walk's textured batches drawn again: those with a table, those read from it, those with the walk's CLUT)
    uint64_t walk_texture_draws = 0, table_draws = 0, walk_clut_draws = 0;
    // (a kept walk's runs: the indirect draws, and the draws they made)
    uint64_t runs = 0, run_draws = 0;
    // (a kept walk's merged runs: the draw calls, and the native batches' triangles they drew)
    uint64_t merged_runs = 0, merged_triangles = 0;
    uint64_t clears = 0; // (batches cleared, vkCmdClearAttachments, not drawn: as_clear)
    uint64_t dead_batches = 0; // (a pictures' walk's batches not drawn: what they write is never read, pcr_dead_writes.h)
    uint64_t blends_left_out = 0; // (blended batches drawn without the blend: blend_is_source)
    uint64_t decals_settled = 0; // (decal batches given back their own depth where they won: settle_decal_depth)
};

// A picture between's walk as the canvas made it (pcr_canvas.cpp): its batches, uploads, copies, CLUT loads and marks
// in order, every image and buffer they use held, and what it read of the texture store from before it
class KeptWalk;

// (pcr_hang_notes.h)
class HangNotes;

// A canvas's surfaces as they were (Canvas::copy_surfaces): each colour and depth surface's image copied on the GPU, the
// frame shown kept (0xF2), and the scale; for a canvas on another thread to draw on from there (Canvas::start_from)
class SurfaceCopies;

// KF4_PC_SUBMIT_TOGETHER=0: the canvas's command buffer flushed on its own before a picture's (as before #110)
bool submit_together();

class Canvas final : public Drawing {
public:
    using ShownDepth = pcr::ShownDepth;
    // times: the GPU's time of what the canvas records (outlives the canvas)
    Canvas(Vulkan::Device &device, GpuTimes &times);
    ~Canvas();
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;

    // Each submit leaves a note of what it held, and each wait for the GPU says so (pcr_hang_notes.h). who: the
    // canvas's thread, a name that outlives the canvas. notes outlive the canvas.
    void watch_hangs(HangNotes &notes, const char *who);

    // The surfaces' pixels a GS pixel is, across and down (1 to 8): from the next vblank
    void set_scale(uint32_t x_scale, uint32_t y_scale) override;
    // Texture detail (GSN_LOD_*) and anisotropy (1 to 16) for the batches from the next vblank
    void set_sampling(int lod, uint32_t aniso) override;
    // The 2D mode (GSN_2D_*) for the batches from the next draw: smooth reads the 2D's magnified textures with a cubic
    // at a scale above 1
    void set_2d(int mode) override;
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

    // The pictures between on a thread of their own (pcr.cpp's pump):
    // - (kf4-pc) Everything drawn so far, then every surface copied, submitted: a canvas on another thread whose
    //   commands are submitted after this returns may draw on the copies
    std::shared_ptr<SurfaceCopies> copy_surfaces();
    // - (the pump's canvas) Its surfaces from here on the copies' (its own let go; the copies' images taken), at their
    //   scale
    void start_from(SurfaceCopies &copies);
    // - (the pump's canvas) A kept walk's calls made again as draw_again makes them, but none of the texture store's
    //   changes (the store is kf4-pc's: what the walk's draws read of it is held in the walk): its uploads and copies
    //   only where they fall on this canvas's surfaces, its CLUT loads passed over. Submitted every so many batches
    //   and at the end, so that kf4-pc's next frame context (Granite's, which waits for every thread's open command
    //   buffer) never waits long, and never for a pump with nothing to do. picture_follows: the next thing this thread
    //   does is a picture whose scanout and submit follow at once; the end is then submitted without a flush, and goes
    //   to the GPU's queue in that picture's vkQueueSubmit (one a picture between, not two)
    void draw_again_alone(const KeptWalk &walk, const Between &between, bool picture_follows = false);

    // A frame walked once (pcr_frame_walk.h): keep_walk() outside a picture between, the frame's own packets, then
    // stop_keeping() (set_between(nullptr) also stops) and take_kept_walk(). A read-back in it is left out of what is
    // kept, as the stream leaves it out.
    void stop_keeping();
    // The frame's own walk as the frame's stream would make its pictures' walk: the draws into the buffer shown, the
    // uploads asked for again, the read-backs and copies left out, the draws reading the kept frame shown between
    // 0xF4 and 0xF5, the native batches with their pictures' records, 0xF0 before and 0xF1 after; the GIF corners
    // moved by their kicks (the walk's numbered: Between::adopt_live_kicks). Refused (walk nullptr) where the rules
    // cannot make the stream's.
    struct PictureWalk {
        std::shared_ptr<const KeptWalk> walk;
        FrameWalkRefused refused = FrameWalkRefused::none;
    };
    PictureWalk as_picture_walk(const KeptWalk &frame, const FrameTargets &targets) const;

    // The front end's events (pcr_gif.h), with its register file at each
    void primitive(const GsRegisterFile &file, const GifPrimitive &primitive) override;
    void texture_written(uint64_t tex0, uint64_t texclut) override;
    // A native batch, with the registers as its writes left them and PRIM's 11 bits in effect. Its texture is read
    // whole; what it draws over is taken as its scissor's rectangle.
    void native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &batch) override;
    void upload(const TransferRegisters &transfer, std::span<const uint8_t> bytes) override;
    void local_copy(const TransferRegisters &transfer) override;
    // A local-to-host transfer (TRXDIR 1): its pixels made ready for read_fifo, from the uploads or the surface
    void local_to_host(const TransferRegisters &transfer) override;
    // The read-back's next qwords (zeroes past its end). Any thread, while the canvas's thread is idle (after
    // everything sent before was done: pcr_read_fifo waits for that).
    void read_fifo(void *dst, uint32_t qwords) override;
    // The port's marks for the pictures between (frame_interp.c's meaning): 0x7D's 0xF0 and 0xF1 a
    // picture between begins and ends (its uploads are not the frame's), 0xF2 the frame shown kept, 0xF4 to 0xF5
    // the draws that read that kept frame in place of its buffer; 0x7C the frame's k-th upload made again (its
    // registers checked against those set now, as the GS checks them)
    void frame_mark(uint64_t value) override;
    void upload_again(const GsRegisterFile &file, uint64_t value) override;

    // A state loaded: nothing of the GS's memory is known any more
    void forget() override;

    // Save states: the GS's 4 MB (the uploads', with the colour surfaces' pixels over the pages drawn last) and the
    // CLUT buffer; taken back as the uploads' (the surfaces made again from them as they are drawn into or shown).
    // Z buffers are not kept (the game draws its Z anew each frame).
    std::vector<uint8_t> vram_image() override;
    TextureStore::ClutState clut_state() const override;
    void restore(std::span<const uint8_t> vram, const TextureStore::ClutState &clut) override;

    // Everything drawn so far recorded, the shown rectangle copied (scaled to dst's size) into dst, which the
    // commands that follow in cmd may read. False when there is nothing to show (dst left as it was).
    bool scanout(Vulkan::CommandBuffer &cmd, const ShownRect &rect, const Vulkan::Image &dst) override;
    // After scanout of rect into a picture dst's size: the depth of its 3D (the Z buffer last drawn with into the
    // buffer shown) copied into dst (R32F, as the picture is placed): the GS's Z times zscale, larger nearer, 0 where
    // nothing was drawn; and the XYOFFSET that 3D was drawn with. zscale 0, dst left as it was, when there is no depth
    // to give (a depth format without its float (D24), the Z buffer holding the next frame's, a picture scaled); none
    // when there is no 3D in the buffer, or the picture is of one field. (gs_drawing.h's ShownDepth)
    std::optional<ShownDepth> scanout_depth(Vulkan::CommandBuffer &cmd, const ShownRect &rect,
                                            const Vulkan::Image &dst) override;
    // (the temporal anti-aliasing) What is drawn from here on is drawing number drawing (the caller's: scanout_depth
    // gives it back for the buffer its 3D was last drawn into), with jitter (pcr_jitter.h): the 3D (draws with a depth
    // test that are native, or GIF triangles and lines not read by UV) into the frame's buffers (those the last two
    // pictures showed: the one shown and the one drawn) moved by it; the 2D (the HUD tests Z too, but draws sprites and
    // reads UV), the full-screen effects and the game's own targets are not. Jitter{} (the default) moves nothing.
    void begin_drawing(uint64_t drawing, Jitter jitter) override;
    // The command buffer the canvas records into, submitted (before a picture's own commands are)
    void submit() override;
    // A vblank (each, whether a picture is made or not): what was drawn submitted, a new scale taken. game_vblank: the
    // game's number for it, where known (the developers' counters name the frames by it; else the canvas's own count)
    void vblank(uint64_t game_vblank = UINT64_MAX) override;
    // A frame taken for its pictures between, with its packets the last taken in: the uploads since the last vblank
    // are the frame's, what its pictures ask for again (upload_again), as the vblank after them would have made them
    void frame_taken();

    // Developers' views (pcr.h's pcr_debug_*, which say what each does; kf4-gsreplay --renderer pc), never a player's:
    // the pixel trace (trace_pixel to start it: false when the spot is not one or there is no memory for it; take_trace
    // submits, waits for the GPU and returns the fragments counted, kept or not) and the decal losses
    bool trace_pixel(const PcrTraceSpot &spot);
    void trace_draw(uint32_t draw);
    void gpu_draw(uint32_t draw); // (KF4_GPU_DRAWS: pcr.h's pcr_debug_gpu_draw)
    void trace_probe();
    size_t take_trace(std::vector<PcrTraceFragment> &fragments, std::vector<PcrTraceProbe> &probes);
    void show_decal_losses(bool on);
    // whether the scale last set is drawn with (it is taken at a vblank)
    bool settled() const override;

    // (developers' KF4_PC_FRAME_CENSUS, pcr_frame_census.h) each native batch shown to observer first, with the registers
    // it is drawn with
    void observe_native_batches(std::function<void(const GsRegisterFile &, uint32_t, const NativeBatch &)> observer);

    CanvasCounts counts() const;
    // --stats: the "[stats] pc canvas" lines, and the texture store's
    void print_counts() const override;

private:
    class Impl;
    friend class KeptWalk;
    friend class SurfaceCopies;
    std::unique_ptr<Impl> impl_;
};

} // namespace pcr
