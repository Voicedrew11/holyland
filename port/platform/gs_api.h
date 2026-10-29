/* The renderer behind gs.c and the presenter: the native GS (port/gs/native/gsn.h) or the PC renderer
   (port/gs/pc/pcr.h, --renderer pc; docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native GS").
   The two have the same C interface; gs.c picks one when it opens the renderer (gsr_pc_renderer), and every
   call made outside the renderers goes through these, which pass it on to the one open. With the native GS
   open they are its calls, as they were. */
#ifndef PORT_PLATFORM_GS_API_H
#define PORT_PLATFORM_GS_API_H

#include "gsn.h"
#include "pcr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1: the PC renderer is the one open (gs.c sets it before it opens one, on the DMA controller's thread,
   and never changes it after; every caller below comes after that open) */
extern int gsr_pc_renderer;

static inline int gsr_open(void *shared_context) {
    return gsr_pc_renderer ? pcr_open(shared_context) : gsn_open(shared_context);
}
static inline void gsr_gif(int path, const void *data, size_t bytes) {
    if (gsr_pc_renderer) {
        pcr_gif(path, data, bytes);
    } else {
        gsn_gif(path, data, bytes);
    }
}
static inline void gsr_wait(void) {
    if (gsr_pc_renderer) {
        pcr_wait();
    } else {
        gsn_wait();
    }
}
static inline void gsr_pipeline_cache(const char *path) {
    if (gsr_pc_renderer) {
        pcr_pipeline_cache(path);
    } else {
        gsn_pipeline_cache(path);
    }
}
static inline int gsr_pipelines_running(void) {
    return gsr_pc_renderer ? pcr_pipelines_running() : gsn_pipelines_running();
}
static inline int gsr_pipelines_building(int *done, int *total) {
    return gsr_pc_renderer ? pcr_pipelines_building(done, total) : gsn_pipelines_building(done, total);
}
static inline void gsr_read_fifo(void *dst, uint32_t qwords) {
    if (gsr_pc_renderer) {
        pcr_read_fifo(dst, qwords);
    } else {
        gsn_read_fifo(dst, qwords);
    }
}
static inline int gsr_vsync(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame) {
    return gsr_pc_renderer ? pcr_vsync(privileged, field, want_pixels, frame)
                           : gsn_vsync(privileged, field, want_pixels, frame);
}
static inline void gsr_set_present_scale(int width, int height, float sharpen) {
    if (gsr_pc_renderer) {
        pcr_set_present_scale(width, height, sharpen);
    } else {
        gsn_set_present_scale(width, height, sharpen);
    }
}
static inline int gsr_picture(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame,
                              uint64_t tag) {
    return gsr_pc_renderer ? pcr_picture(privileged, field, want_pixels, frame, tag)
                           : gsn_picture(privileged, field, want_pixels, frame, tag);
}
static inline int gsr_backlog(void) {
    return gsr_pc_renderer ? pcr_backlog() : gsn_backlog();
}
static inline void gsr_set_presenter(void (*pixels)(const PortGsFrame *frame, uint64_t tag),
                                     void (*image)(uint64_t tag)) {
    if (gsr_pc_renderer) {
        pcr_set_presenter(pixels, image);
    } else {
        gsn_set_presenter(pixels, image);
    }
}
static inline void gsr_set_scale(int x_scale, int y_scale) {
    if (gsr_pc_renderer) {
        pcr_set_scale(x_scale, y_scale);
    } else {
        gsn_set_scale(x_scale, y_scale);
    }
}
static inline int gsr_gpu_kind(void) {
    return gsr_pc_renderer ? pcr_gpu_kind() : gsn_gpu_kind();
}
static inline void gsr_set_lod(int mode) {
    if (gsr_pc_renderer) {
        pcr_set_lod(mode);
    } else {
        gsn_set_lod(mode);
    }
}
static inline int gsr_set_aniso(int taps) {
    return gsr_pc_renderer ? pcr_set_aniso(taps) : gsn_set_aniso(taps);
}
static inline int gsr_set_msaa(int samples) {
    return gsr_pc_renderer ? pcr_set_msaa(samples) : gsn_set_msaa(samples);
}
static inline void gsr_set_cutout_aa(int mode) {
    if (gsr_pc_renderer) {
        pcr_set_cutout_aa(mode);
    } else {
        gsn_set_cutout_aa(mode);
    }
}
static inline void gsr_set_2d(int mode) {
    if (gsr_pc_renderer) {
        pcr_set_2d(mode);
    } else {
        gsn_set_2d(mode);
    }
}
static inline void gsr_set_colour(int mode) {
    if (gsr_pc_renderer) {
        pcr_set_colour(mode);
    } else {
        gsn_set_colour(mode);
    }
}
static inline void gsr_set_layer_z(int mode) {
    if (gsr_pc_renderer) {
        pcr_set_layer_z(mode);
    } else {
        gsn_set_layer_z(mode);
    }
}
static inline void gsr_set_shine_exact(int exact) {
    if (gsr_pc_renderer) {
        pcr_set_shine_exact(exact);
    } else {
        gsn_set_shine_exact(exact);
    }
}
static inline void gsr_set_native_draw(int mode) {
    if (gsr_pc_renderer) {
        pcr_set_native_draw(mode);
    } else {
        gsn_set_native_draw(mode);
    }
}
static inline void gsr_set_standalone(int on) {
    if (gsr_pc_renderer) {
        pcr_set_standalone(on);
    } else {
        gsn_set_standalone(on);
    }
}
static inline void gsr_set_hardware_sampler(int on) {
    if (gsr_pc_renderer) {
        pcr_set_hardware_sampler(on);
    } else {
        gsn_set_hardware_sampler(on);
    }
}
static inline void gsr_set_pc_world(int on) {
    if (gsr_pc_renderer) {
        pcr_set_pc_world(on);
    } else {
        gsn_set_pc_world(on);
    }
}
static inline void gsr_set_scene(int x_scale, int y_scale, int samples) {
    if (gsr_pc_renderer) {
        pcr_set_scene(x_scale, y_scale, samples);
    } else {
        gsn_set_scene(x_scale, y_scale, samples);
    }
}
static inline void gsr_set_upscaler(int mode) {
    if (gsr_pc_renderer) {
        pcr_set_upscaler(mode);
    } else {
        gsn_set_upscaler(mode);
    }
}
static inline void gsr_set_scene_map(int map) {
    if (gsr_pc_renderer) {
        pcr_set_scene_map(map);
    } else {
        gsn_set_scene_map(map);
    }
}
static inline int gsr_display_rect(const uint64_t *privileged, GsnRect *rect) {
    return gsr_pc_renderer ? pcr_display_rect(privileged, rect) : gsn_display_rect(privileged, rect);
}
static inline void gsr_read_rect(const GsnRect *rect, uint32_t *rgba) {
    if (gsr_pc_renderer) {
        pcr_read_rect(rect, rgba);
    } else {
        gsn_read_rect(rect, rgba);
    }
}
static inline void gsr_capture(void (*emit)(uint32_t type, const void *data, size_t size)) {
    if (gsr_pc_renderer) {
        pcr_capture(emit);
    } else {
        gsn_capture(emit);
    }
}
static inline int gsr_restore(uint32_t type, const void *data, size_t size) {
    return gsr_pc_renderer ? pcr_restore(type, data, size) : gsn_restore(type, data, size);
}
static inline size_t gsr_state_size(void) {
    return gsr_pc_renderer ? pcr_state_size() : gsn_state_size();
}
static inline void gsr_state(PortStateIo *io) {
    if (gsr_pc_renderer) {
        pcr_state(io);
    } else {
        gsn_state(io);
    }
}
static inline size_t gsr_table_state_size(void) {
    return gsr_pc_renderer ? pcr_table_state_size() : gsn_table_state_size();
}
static inline void gsr_table_state(PortStateIo *io, size_t size) {
    if (gsr_pc_renderer) {
        pcr_table_state(io, size);
    } else {
        gsn_table_state(io, size);
    }
}
static inline void gsr_enable_present(void) {
    if (gsr_pc_renderer) {
        pcr_enable_present();
    } else {
        gsn_enable_present();
    }
}
static inline int gsr_present_device(GsnPresentDevice *out) {
    return gsr_pc_renderer ? pcr_present_device(out) : gsn_present_device(out);
}
static inline void gsr_queue_lock(void) {
    if (gsr_pc_renderer) {
        pcr_queue_lock();
    } else {
        gsn_queue_lock();
    }
}
static inline void gsr_queue_unlock(void) {
    if (gsr_pc_renderer) {
        pcr_queue_unlock();
    } else {
        gsn_queue_unlock();
    }
}
static inline int gsr_scanout_take_tag(GsnScanout *out, uint64_t tag) {
    return gsr_pc_renderer ? pcr_scanout_take_tag(out, tag) : gsn_scanout_take_tag(out, tag);
}
static inline void gsr_scanout_release(void) {
    if (gsr_pc_renderer) {
        pcr_scanout_release();
    } else {
        gsn_scanout_release();
    }
}
static inline void gsr_interp_frame(GsnInterpFrame *frame) {
    if (gsr_pc_renderer) {
        pcr_interp_frame(frame);
    } else {
        gsn_interp_frame(frame);
    }
}
static inline void gsr_interp_picture(const GsnInterpPicture *picture) {
    if (gsr_pc_renderer) {
        pcr_interp_picture(picture);
    } else {
        gsn_interp_picture(picture);
    }
}

#ifdef __cplusplus
}
#endif

#endif
