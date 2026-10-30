/* The PC renderer (docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native GS"): the GS's input
   (GIF packets, the vblank's display registers, the read-back FIFO, the pictures between) drawn by code of its
   own on the GPU, with none of the native GS's. --renderer pc (its development name) opens it in place of the
   native GS (port/platform/gs.c, through port/platform/gs_api.h).

   Its C interface is the native GS's (port/gs/native/gsn.h, whose plain types it shares): every call has the
   native GS's shape and contract, and gsn.h says what each does. What it does so far (step 1, the skeleton):
   - its own Vulkan device (with the presenter's swapchain when pcr_enable_present was called first) and its
     own thread ("kf4-pc"), which takes the packets, vblanks and pictures between in the order sent;
   - at each vblank, the picture the display registers name, at their size times pcr_set_scale's: an image
     cleared to black (KF4_PC_CLEAR=RRGGBB: that colour), handed out as the native GS hands out its pictures
     (the scanout ring of pcr_scanout_take_tag, the presenter's callbacks, pixels read back into frame);
     a picture between (GSN_EXTRA) is the last picture again;
   - the packets are counted, the read-back FIFO reads zeroes, save states and captures hold nothing of it
     (size 0), and the settings are kept for the steps to come.
   Since step 5: pcr_interp_frame's frame is kept, and each pcr_interp_picture draws its packets again, moved on
   the GPU to the picture's time (pcr_between.h). */
#ifndef PORT_GS_PC_PCR_H
#define PORT_GS_PC_PCR_H

#include <stddef.h>
#include <stdint.h>

#include "gsn.h" /* (the shared plain types only: GsnRect, GsnScanout, GsnPresentDevice, GsnInterp*) */
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

/* The settings (gsn.h's): kept for the steps that draw; the counts returned are those the PC renderer will use */
void pcr_set_scale(int x_scale, int y_scale);
void pcr_set_lod(int mode);
int pcr_set_aniso(int taps);
int pcr_set_msaa(int samples);
void pcr_set_cutout_aa(int mode);
void pcr_set_2d(int mode);
void pcr_set_colour(int mode);
void pcr_set_layer_z(int mode);
void pcr_set_shine_exact(int exact);
/* The --native-draw kinds the PC renderer draws (PORT_ND_* bits): gs.c turns on only those under it */
int pcr_native_kinds(void);
void pcr_set_native_draw(int mode);
void pcr_set_standalone(int on);
void pcr_set_hardware_sampler(int on);
void pcr_set_pc_world(int on);
void pcr_set_scene(int x_scale, int y_scale, int samples);
void pcr_set_upscaler(int mode);
void pcr_set_scene_map(int map);

/* Pipelines: none made ahead yet */
void pcr_pipeline_cache(const char *path);
int pcr_pipelines_running(void);
int pcr_pipelines_building(int *done, int *total);

/* Captures and save states: nothing of the PC renderer's yet (sizes 0; a record or state given is not taken) */
void pcr_capture(void (*emit)(uint32_t type, const void *data, size_t size));
int pcr_restore(uint32_t type, const void *data, size_t size);
size_t pcr_state_size(void);
void pcr_state(PortStateIo *io);
size_t pcr_table_state_size(void);
void pcr_table_state(PortStateIo *io, size_t size);

/* --stats: the "[stats] pc renderer:" line (packets, bytes, vblanks, pictures) */
void pcr_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif
