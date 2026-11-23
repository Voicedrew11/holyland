/* The PC renderer (port/gs/pc/pcr.h, docs/port/PC_RENDERER.md) as the rest of the port calls it: gs.c and the
   presenter go through these, which pass each call on to pcr_. Its plain types come from gsn.h, which pcr.h
   includes. */
#ifndef PORT_PLATFORM_GS_API_H
#define PORT_PLATFORM_GS_API_H

#include "pcr.h"
#include "scn.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void gsr_find_vulkan_layers(void) {
    pcr_find_vulkan_layers();
}
static inline int gsr_open(void *shared_context) {
    return pcr_open(shared_context);
}
/* The scene renderer (port/gs/scene/scn.h, --renderer scene) in the PC renderer's place, under its shell: every other
   call as the PC renderer's */
static inline int gsr_open_scene(void *shared_context) {
    return scn_open(shared_context);
}
static inline void gsr_hang_reports_in(const char *folder) {
    pcr_hang_reports_in(folder);
}
static inline void gsr_gif(int path, const void *data, size_t bytes) {
    pcr_gif(path, data, bytes);
}
static inline void gsr_gif_vblank(int path, const void *data, size_t bytes) {
    pcr_gif_vblank(path, data, bytes);
}
static inline void gsr_settle(void) {
    pcr_settle();
}
static inline void gsr_pipeline_cache(const char *path) {
    pcr_pipeline_cache(path);
}
static inline void gsr_precompile(int scaling, int sharpen) {
    pcr_precompile(scaling, sharpen);
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
/* The renderer decides which pictures between it makes, from their cost (pcr_budgets_pictures): no cut, none left out
   as behind */
static inline int gsr_budgets_pictures(void) {
    return pcr_budgets_pictures();
}
static inline void gsr_present_sharpens(int on) {
    pcr_present_sharpens(on);
}
static inline const uint32_t *gsr_present_sharpen_spirv(size_t *vert_bytes, const uint32_t **frag, size_t *frag_bytes) {
    return pcr_present_sharpen_spirv(vert_bytes, frag, frag_bytes);
}
static inline int gsr_pictures_later(void) {
    return pcr_pictures_later();
}
static inline int gsr_picture_later(const uint64_t *privileged, int field, int want_pixels, uint64_t tag,
                                    const char *name) {
    return pcr_picture_later(privileged, field, want_pixels, tag, name);
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
static inline void gsr_set_antialias(int method) {
    pcr_set_antialias(method);
}
static inline void gsr_antialias_cut(void) {
    pcr_antialias_cut();
}
static inline void gsr_set_2d(int mode) {
    pcr_set_2d(mode);
}
static inline void gsr_set_font(int font) {
    pcr_set_font(font);
}
static inline void gsr_set_glyphs(const GsnGlyphs *glyphs) {
    pcr_set_glyphs(glyphs);
}
static inline float gsr_key_width_u(const char *key_name) {
    return pcr_key_width_u(key_name);
}
static inline int gsr_hint_parts(const GsnHintSprite *sprite, const float widths_u[4], GsnHintPart *parts) {
    return pcr_hint_parts(sprite, widths_u, parts);
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
static inline void gsr_enable_present(PcrMakeSurface make_surface) {
    pcr_enable_present(make_surface);
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
