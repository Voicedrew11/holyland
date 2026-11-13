/* The PC renderer (docs/port/PC_RENDERER.md): the GS's input (GIF packets, the vblank's display registers, the
   read-back FIFO, the pictures between) drawn by code of its own on the GPU. It is the renderer a player gets: it
   opens through port/platform/gs_api.h, which port/platform/gs.c calls.

   Its C interface takes and gives the plain types of port/gs/shared/gsn.h, which says what the constants and the
   structs mean. Each call's comment says what it does. What it does so far (step 1, the skeleton):
   - its own Vulkan device (with the presenter's swapchain when pcr_enable_present was called first) and its
     own thread ("kf4-pc"), which takes the packets, vblanks and pictures between in the order sent;
   - at each vblank, the picture the display registers name, at their size times pcr_set_scale's: an image
     cleared to black (KF4_PC_CLEAR=RRGGBB: that colour), handed out through the scanout ring of
     pcr_scanout_take_tag, the presenter's callbacks, and pixels read back into frame;
     a picture between (GSN_EXTRA) is the last picture again;
   - the packets are counted, the read-back FIFO reads zeroes, save states and captures hold nothing of it
     (size 0), and the settings are kept for the steps to come.
   Since step 5: pcr_interp_frame's frame is kept, and each pcr_interp_picture draws its packets again, moved on
   the GPU to the picture's time (pcr_between.h).
   Since docs/port/PERFORMANCE.md's item 1: a second thread, the pump ("kf4-pc-pump"), draws the pictures between
   again from the frame's kept walk on surfaces of its own (copied from kf4-pc's at the frame's first), and makes the
   pictures that show them, while kf4-pc takes the next frame's packets in. Every picture is handed out in the order
   asked for. KF4_PC_PUMP=0: no pump (kf4-pc draws them all, as before). */
#ifndef PORT_GS_PC_PCR_H
#define PORT_GS_PC_PCR_H

#include <stddef.h>
#include <stdint.h>

#include "gsn.h" /* (port/gs/shared: the plain types and constants only) */
#include "pcr_trace.h"
#include "port/platform.h"
#include "port/savestate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opening: shared_context must be NULL (the PC renderer runs on a device of its own). 0 without a Vulkan
   device it can use. pcr_enable_present, before: the presenter's surface and swapchain extensions. */
int pcr_open(void *shared_context);
void pcr_enable_present(void);
int pcr_gpu_kind(void);

/* The GS's input, worked on by the PC renderer's thread in the order sent; pcr_wait returns once it has
   done everything sent before */
void pcr_gif(int path, const void *data, size_t bytes);
void pcr_wait(void);
void pcr_read_fifo(void *dst, uint32_t qwords);
int pcr_vsync(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame);
int pcr_picture(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame, uint64_t tag);
int pcr_backlog(void);
void pcr_interp_frame(GsnInterpFrame *frame);
void pcr_interp_picture(const GsnInterpPicture *picture);

/* The picture handed out: the presenter's callbacks, its device and queue, the scanout ring */
void pcr_set_presenter(void (*pixels)(const PortGsFrame *frame, uint64_t tag), void (*image)(uint64_t tag));
int pcr_present_device(GsnPresentDevice *out);
void pcr_queue_lock(void);
void pcr_queue_unlock(void);
int pcr_scanout_take(GsnScanout *out);
int pcr_scanout_take_tag(GsnScanout *out, uint64_t tag);
void pcr_scanout_release(void);
void pcr_set_present_scale(int width, int height, float sharpen);
int pcr_display_rect(const uint64_t *privileged, GsnRect *rect);
void pcr_read_rect(const GsnRect *rect, uint32_t *rgba);

/* The settings: kept for the steps that draw; the counts returned are those the PC renderer will use */
void pcr_set_scale(int x_scale, int y_scale);
void pcr_set_lod(int mode);
int pcr_set_aniso(int taps);
int pcr_set_msaa(int samples);
void pcr_set_cutout_aa(int mode);
void pcr_set_2d(int mode);
void pcr_set_colour(int mode);
void pcr_set_layer_z(int mode);
/* The --native-draw kinds the PC renderer draws (PORT_ND_* bits): gs.c turns on only those under it */
int pcr_native_kinds(void);
void pcr_set_native_draw(int mode);

/* Pipelines: none made ahead yet */
void pcr_pipeline_cache(const char *path);
int pcr_pipelines_running(void);
int pcr_pipelines_building(int *done, int *total);

/* GS captures (F9): pcr_capture emits the GS's memory (KF4GS_VRAM) and registers (KF4GS_GS_REGS) of
   port/include/port/gs_capture_format.h; pcr_restore takes those two back (kf4-gsreplay --renderer pc), 0 for any
   other record. Save states: pcr_state (the GS's memory, registers and CLUT buffer). */
void pcr_capture(void (*emit)(uint32_t type, const void *data, size_t size));
int pcr_restore(uint32_t type, const void *data, size_t size);
size_t pcr_state_size(void);
void pcr_state(PortStateIo *io);
size_t pcr_table_state_size(void);
void pcr_table_state(PortStateIo *io, size_t size);

/* --stats: the "[stats] pc renderer:" line (packets, bytes, vblanks, pictures) */
void pcr_print_stats(void);

/* ---- Developers' views (kf4-gsreplay --renderer pc), never a player's ----
   Each waits until the PC renderer has done everything sent before it, and acts there.

   The pixel trace: every fragment the draws shade at one pixel of the surfaces, and the pixel's samples where asked.
   The pixel: GS pixel (x, y) of the frame buffer at base page fbp (FRAME's FBP), and of the scale's block of surface
   pixels that GS pixel is, the one sub_x across and sub_y down. Its draws are drawn with their depth test after their
   shaders (as the draws that may discard are), so that the trace sees the fragments that fail it too; the picture is
   the same. pcr_debug_trace_draw numbers the draws from there (the batch open is drawn first: a batch is one draw's).
   pcr_debug_trace_probe records the pixel's samples now, as the draws so far left them: in a pass of its own, which
   ends the render pass open. pcr_debug_trace_take: what was recorded since the last take (fragments past room are
   counted in the return, not kept). */
/* (the records: pcr_trace.h) */
int pcr_debug_trace(const PcrTraceSpot *spot);
void pcr_debug_trace_draw(uint32_t draw);
void pcr_debug_trace_probe(void);
size_t pcr_debug_trace_take(PcrTraceFragment *fragments, size_t room, PcrTraceProbe *probes, size_t probe_room,
                            size_t *probes_taken);

/* Decal losses: each decal (pcr_canvas.cpp's is_decal) drawn again where it lost its depth test by less than
   pcr_common.h's PCR_DECAL_LOSS_MARGIN (16 Z), 16 (of 255) of red and blue added to the picture there for each decal
   that lost (its alpha test as it has it, its Z not written): the picture less the same one drawn without them shows
   where. A decal behind a nearer surface by more is left as it is. */
void pcr_debug_decal_losses(int on);

/* 1 once the scale and MSAA's samples last set are what the PC renderer draws with: each is taken at a vblank, and
   MSAA's only once its pipelines are made (pcr_pipelines.h), which takes some vblanks after it is set */
int pcr_debug_settled(void);

#ifdef __cplusplus
}
#endif

#endif
