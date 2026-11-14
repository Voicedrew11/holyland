/* The PC renderer (port/gs/pc/pcr.h, docs/port/PC_RENDERER.md) as the rest of the port calls it: gs.c and the
   presenter go through these, which pass each call on to pcr_. Its plain types come from gsn.h, which pcr.h
   includes. */
#ifndef PORT_PLATFORM_GS_API_H
#define PORT_PLATFORM_GS_API_H

#include "pcr.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline int gsr_open(void *shared_context) {
    return pcr_open(shared_context);
}
static inline void gsr_gif(int path, const void *data, size_t bytes) {
    pcr_gif(path, data, bytes);
}
static inline void gsr_pipeline_cache(const char *path) {
    pcr_pipeline_cache(path);
}
static inline void gsr_precompile(void) {
    pcr_precompile();
}
static inline int gsr_pipelines_running(void) {
    return pcr_pipelines_running();
}
static inline int gsr_pipelines_building(int *done, int *total) {
    return pcr_pipelines_building(done, total);
}
static inline void gsr_read_fifo(void *dst, uint32_t qwords) {
    pcr_read_fifo(dst, qwords);
}
static inline int gsr_vsync(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame) {
    return pcr_vsync(privileged, field, want_pixels, frame);
}
static inline void gsr_set_present_scale(int width, int height, float sharpen) {
    pcr_set_present_scale(width, height, sharpen);
}
static inline int gsr_picture(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame,
                              uint64_t tag) {
    return pcr_picture(privileged, field, want_pixels, frame, tag);
}
static inline int gsr_backlog(void) {
    return pcr_backlog();
}
/* At a vblank whose own picture is sent (gsr_picture): the renderer is behind the game, so its pictures between
   are left out. The game posts a vblank a vblank before its time, and the window shows its picture THREAD_DELAY
   after that time (port/platform/video.c). The PC renderer hands out a vblank's picture after the frame's packets
   sent before it, about when the next vblank is posted and still in time for the window: it is behind when the
   vblank before that one is not handed out either. */
static inline int gsr_behind(void) {
    return pcr_backlog() > 2;
}
static inline void gsr_set_presenter(void (*pixels)(const PortGsFrame *frame, uint64_t tag),
                                     void (*image)(uint64_t tag)) {
    pcr_set_presenter(pixels, image);
}
static inline void gsr_set_scale(int x_scale, int y_scale) {
    pcr_set_scale(x_scale, y_scale);
}
static inline int gsr_gpu_kind(void) {
    return pcr_gpu_kind();
}
static inline void gsr_set_lod(int mode) {
    pcr_set_lod(mode);
}
static inline int gsr_set_aniso(int taps) {
    return pcr_set_aniso(taps);
}
static inline int gsr_set_msaa(int samples) {
    return pcr_set_msaa(samples);
}
static inline void gsr_set_cutout_aa(int mode) {
    pcr_set_cutout_aa(mode);
}
static inline void gsr_set_2d(int mode) {
    pcr_set_2d(mode);
}
static inline void gsr_set_colour(int mode) {
    pcr_set_colour(mode);
}
static inline void gsr_set_layer_z(int mode) {
    pcr_set_layer_z(mode);
}
static inline void gsr_set_native_draw(int mode) {
    pcr_set_native_draw(mode);
}
static inline int gsr_display_rect(const uint64_t *privileged, GsnRect *rect) {
    return pcr_display_rect(privileged, rect);
}
static inline void gsr_read_rect(const GsnRect *rect, uint32_t *rgba) {
    pcr_read_rect(rect, rgba);
}
static inline void gsr_capture(void (*emit)(uint32_t type, const void *data, size_t size)) {
    pcr_capture(emit);
}
static inline int gsr_restore(uint32_t type, const void *data, size_t size) {
    return pcr_restore(type, data, size);
}
static inline size_t gsr_state_size(void) {
    return pcr_state_size();
}
static inline void gsr_state(PortStateIo *io) {
    pcr_state(io);
}
static inline size_t gsr_table_state_size(void) {
    return pcr_table_state_size();
}
static inline void gsr_table_state(PortStateIo *io, size_t size) {
    pcr_table_state(io, size);
}
static inline void gsr_enable_present(void) {
    pcr_enable_present();
}
static inline int gsr_present_device(GsnPresentDevice *out) {
    return pcr_present_device(out);
}
static inline void gsr_queue_lock(void) {
    pcr_queue_lock();
}
static inline void gsr_queue_unlock(void) {
    pcr_queue_unlock();
}
static inline int gsr_scanout_take_tag(GsnScanout *out, uint64_t tag) {
    return pcr_scanout_take_tag(out, tag);
}
static inline void gsr_scanout_release(void) {
    pcr_scanout_release();
}
static inline void gsr_interp_frame(GsnInterpFrame *frame) {
    pcr_interp_frame(frame);
}
static inline void gsr_interp_picture(const GsnInterpPicture *picture) {
    pcr_interp_picture(picture);
}

#ifdef __cplusplus
}
#endif

#endif
