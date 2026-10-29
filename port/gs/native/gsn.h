/* The native GS (docs/port/STEP3.md): the GS drawn with the GPU's own rasteriser, through Vulkan
   (Granite's layer, the one paraLLEl-GS runs on), instead of emulated in compute by paraLLEl-GS.

   It takes what port/platform/gs.c hands the GS backend (GIF packets, the read-back FIFO, the vblank's
   display registers) and gives the same kind of picture back. port/gs/native/gsn.cpp says how. */
#ifndef PORT_GS_NATIVE_GSN_H
#define PORT_GS_NATIVE_GSN_H

#include <stddef.h>
#include <stdint.h>

#include "port/platform.h"
#include "port/savestate.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts it: on the Vulkan context paraLLEl-GS made (a Vulkan::Context *, to run beside it and compare)
   or, with NULL, on one of its own. 0 if there is no Vulkan device it can use. */
int gsn_open(void *shared_context);
/* GIF packets and vblanks (without a picture wanted) are worked on by the native GS's own thread; the
   functions that give something back wait for it to have done all that was sent before */
void gsn_gif(int path, const void *data, size_t bytes);
/* Waits until the native GS's thread has done all that was sent to it */
void gsn_wait(void);
/* The Vulkan pipeline cache kept in this file: read now (if it is there and this driver's), and
   written again from time to time when pipelines have been made */
void gsn_pipeline_cache(const char *path);
/* Pipelines made ahead (gsn_pipeline_warm.inc): begun by gsn_pipeline_cache, on threads of their own. Running: 1 until
   all are made; building: 1 while those the pipeline cache did not have are made, with how many of how many */
int gsn_pipelines_running(void);
int gsn_pipelines_building(int *done, int *total);
void gsn_print_pipeline_stats(void);
void gsn_read_fifo(void *dst, uint32_t qwords);
/* The vblank: the display registers (PMODE..BGCOLOR, a register every 16 bytes), and the picture
   into frame when want_pixels (1 if there is one). With GSN_WANT_IMAGE in want_pixels the picture
   stays on the GPU instead, as the image gsn_scanout_take hands out (GSN_WANT_IMAGE is returned when
   it is there; GSN_WANT_PIXELS as well asks for it in frame too, read back from that image).
   With GSN_PRESENT the picture goes to the presenter (gsn_set_presenter): its pixels, or with
   GSN_WANT_IMAGE word that the image is new, from the native GS's thread once it is there. Without
   GSN_WANT_PIXELS the caller then does not wait for it (and gets GSN_PRESENT, with GSN_WANT_IMAGE if
   asked); a vblank with no picture wanted does not wait either. */
#define GSN_WANT_PIXELS 1
#define GSN_WANT_IMAGE 2
#define GSN_PRESENT 4
int gsn_vsync(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame);
/* The same with the picture's tag (PORT_PICTURE_TAG: its vblank and when in it), which the presenter
   gets with it. With GSN_EXTRA it is not a vblank but one more picture between two (--frame-rate above
   60): the buffer shown read again, whole (none when the display reads it a field at a time), with
   nothing of the vblank's own done (the deinterlace's history, a new scale). */
#define GSN_EXTRA 16
/* With GSN_WANT_IMAGE and GSN_WANT_PIXELS: the pixels are the picture as the window shows it (the
   scaled image, gsn_set_present_scale) when there is one */
#define GSN_WANT_SCALED 32
/* The presenter's scaling: each picture made into an image width x height (the size the window shows
   it at) on the GPU, with a Catmull-Rom cubic (stretched by the ratio when it is made smaller, on linear
   light) and a contrast-adaptive sharpening of that strength (0: none); 0 x 0: none. From the next
   picture. gsn_present_scale_passes counts the pictures scaled. */
void gsn_set_present_scale(int width, int height, float sharpen);
uint64_t gsn_present_scale_passes(void);
int gsn_picture(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame, uint64_t tag);
/* The vblanks sent to the native GS's thread that it has not done yet */
int gsn_backlog(void);
void gsn_set_presenter(void (*pixels)(const PortGsFrame *frame, uint64_t tag), void (*image)(uint64_t tag));
/* The render targets' size, from the next vblank: x_scale times the PS2's across (1 to GSN_MAX_SCALE_X)
   and y_scale times down (1 to 4). --native-scale gives both the same; --native-widescreen a wider x. */
#define GSN_MAX_SCALE_X 6
void gsn_set_scale(int x_scale, int y_scale);
/* The GPU the native GS is on: GSN_GPU_DISCRETE, GSN_GPU_INTEGRATED or GSN_GPU_OTHER (a CPU, virtual,
   or none open): --native-scale auto draws at most 4x, 2x and 1x on them */
#define GSN_GPU_OTHER 0
#define GSN_GPU_INTEGRATED 1
#define GSN_GPU_DISCRETE 2
int gsn_gpu_kind(void);
/* --native-lod: the mipmap level a texture is read at. The GS picks it from the distance (Q), for the
   PS2's 640x448: GSN_LOD_PS2 keeps that; GSN_LOD_SCALE (the default) takes it log2 of --native-scale
   finer, the detail the GS would give the larger picture (at 1x, the PS2's); GSN_LOD_FULL reads
   level 0 at every distance (at 1x too). From the next draw. */
#define GSN_LOD_PS2 0
#define GSN_LOD_SCALE 1
#define GSN_LOD_FULL 2
void gsn_set_lod(int mode);
/* --native-aniso: anisotropic filtering of mipmapped textures, from the next draw: up to `taps` reads
   (1, off, the default: the GS's picture; 2, 4, 8 or 16) along the long axis of a pixel's footprint on
   the texture, each at a level finer than the GS's by log2 of the reads (gs_frag.h, sample_texture).
   Returns the count used (a power of two, at most 16). */
int gsn_set_aniso(int taps);
/* --native-msaa: the samples a pixel of the targets the 3D is drawn into has, from the next vblank
   (1, the default, is the PS2's picture; 2, 4 or 8). Returns the count used: the most the GPU has up
   to `samples`. */
int gsn_set_msaa(int samples);
/* --native-cutout-aa: with MSAA (gsn_set_msaa), the edges inside cut-outs (netting, foliage, fences,
   grates, hair: textures see-through in places, by the alpha test or blending; flush_batch says which).
   GSN_CUTOUT_AA_OFF shades them once a pixel as the rest of the 3D, so each pixel sees the texture's
   alpha at one point; GSN_CUTOUT_AA_SSAA shades them at every sample (transparency supersampling):
   each sample is textured, tested and blended at its own place. Nothing without MSAA, or on a GPU
   without sample-rate shading. From the next draw. */
#define GSN_CUTOUT_AA_OFF 0
#define GSN_CUTOUT_AA_SSAA 1
void gsn_set_cutout_aa(int mode);
/* --native-2d: at a target scale above 1, the 2D (batches flat_2d takes whole) textured from VRAM and
   read nearest is read bilinear at each pixel's own place where its texture is magnified (smooth), or
   nearest at the GS pixel's integer point as at 1x (ps2). Nothing at 1x. */
#define GSN_2D_PS2 0
#define GSN_2D_SMOOTH 1
void gsn_set_2d(int mode);
/* --native-colour: how precisely colours are kept, from the next vblank. GSN_COLOUR_EXACT (the default)
   is the GS's: 8 bits a channel, every step of its arithmetic rounded as the GS rounds it, the PS2's
   picture. GSN_COLOUR_HIGH keeps the 32-bit targets at 16 bits a channel and the colour arithmetic in
   floats (the texture filter, texture function, fog, blending, and the frame's passes that read it
   again), alpha and 16-bit targets as the GS has them; the picture is dithered to 8 bits as it is
   scanned out, and anything read back (write-backs to VRAM, captures) is rounded to 8 bits. */
#define GSN_COLOUR_EXACT 0
#define GSN_COLOUR_HIGH 1
void gsn_set_colour(int mode);
/* --native-layer-z: how the 3D's Z is kept and tested, from the next draw. GSN_LAYER_Z_PS2 is the GS's:
   a whole number per pixel, so surfaces meeting at a shallow angle meet in a sawtooth, and the layers
   the game blends over its surfaces (a wall's second texture, the darkness strips in the arches: a
   mesh of their own, a vertex alpha below 0x80, GEQUAL) fail the test where the two meshes' Z round
   apart: dark hatching, teeth, a lace of light lines at --native-scale. GSN_LAYER_Z_TOLERANT keeps Z's
   fraction (straight seams) and lets a layer's colour pass within one Z of what is there, its Z
   written where the GS's test passes (flush_batch, "Z and layers"). */
#define GSN_LAYER_Z_PS2 0
#define GSN_LAYER_Z_TOLERANT 1
void gsn_set_layer_z(int mode);
/* --native-shine (--renderer modern): a skinned model's sphere-mapped shine blended in the shader (the ice
   creatures on map 017) drawn whole (0, the default) or exact, in runs of triangles that do not overlap (1:
   the GS's picture where a part folds over its own edge; docs/port/NATIVE_DRAW_P8.md, "The icy corridor") */
void gsn_set_shine_exact(int exact);
/* --native-draw-compare (docs/port/NATIVE_DRAW.md, gsn_native_draw.inc): which of the two the frame has
   for the draws the game makes natively it draws: the native draws (GSN_NATIVE_DRAW_NATIVE, the
   default; the PS2 path's packets queued for comparison between their marks are left out) or the PS2
   path's packets (GSN_NATIVE_DRAW_PS2; the native draws left out). From the next draw. */
#define GSN_NATIVE_DRAW_NATIVE 0
#define GSN_NATIVE_DRAW_PS2 1
void gsn_set_native_draw(int mode);
/* --renderer modern (P8, docs/port/NATIVE_DRAW_P8.md; gsn_standalone.inc): the native draws of the scene's
   kinds drawn into the scene's own targets (D32F depth, its own colour image, MSAA), their textures from the
   uploads that own them; the GS draws the rest over it. From the next vblank. gsn_print_standalone_stats:
   its --stats line. */
void gsn_set_standalone(int on);
/* --native-sampler hardware (docs/port/PC_RENDERER.md, P1): the scene's textures read by the GPU's sampler from
   RGBA8 copies, not by the shader texel by texel */
void gsn_set_hardware_sampler(int on);
/* --renderer fast (docs/port/PC_RENDERER.md, P2, gsn_pc_world.inc): the map's records kept and drawn by the PC renderer
   by material, not by the scene's batch by batch */
void gsn_set_pc_world(int on);
void gsn_print_standalone_stats(void);
/* --scene-scale and --scene-msaa (P8 T2, gsn_composite.inc): the scene's own scale, across and down, and samples,
   apart from the GS's; 0: the GS's. From when the scene next takes a frame. */
void gsn_set_scene(int x_scale, int y_scale, int samples);
/* --upscaler (docs/port/UPSCALING.md, gsn_composite.inc's sa_upscale_held): a scene below the GS's scale (both
   single-sampled) brought up to it at the composite. 0 off, 1 bilinear, 2 FSR 3.1, 3 DLSS (before gsn_open: its
   Vulkan extensions) */
void gsn_set_upscaler(int mode);
/* (modern) The map the game is in (MapSlot.tag; -1 none), for --stats' counts a map: at each vblank */
void gsn_set_scene_map(int map);

/* A rectangle of a GS buffer: base (256-byte blocks), width (64-pixel units), format, place, size */
typedef struct {
    uint32_t bp, bw, psm, x, y, w, h;
} GsnRect;
/* The buffer the display shows at this vblank (circuit 2, else 1; a field's 224 lines in field mode):
   0 if none */
int gsn_display_rect(const uint64_t *privileged, GsnRect *rect);
/* A rectangle as RGBA (alpha 255): from the native GS's targets, or its VRAM */
void gsn_read_rect(const GsnRect *rect, uint32_t *rgba);
/* The same at the target's own pixels (--native-scale): rect->w * scale[0] by rect->h * scale[1] of
   them into rgba (room for GSN_MAX_SCALE_X * 4 times the rectangle), the scale across and down into
   scale (1 and 1 where it is not a target) */
void gsn_read_rect_scaled(const GsnRect *rect, uint32_t *rgba, uint32_t *scale);
/* The same from a VRAM image of the GS's (paraLLEl-GS's, to compare), alpha as the buffer has it */
void gsn_decode_rect(const uint8_t *vram, const GsnRect *rect, uint32_t *rgba);
/* A rectangle of a buffer made these pixels (as gsn_decode_rect gives them): kf4-gsreplay's
   --compare-renderers starts each draw from paraLLEl-GS's picture */
void gsn_write_rect(const GsnRect *rect, const uint32_t *rgba);

/* The GS's state in the GS capture's records (port/include/port/gs_capture_format.h): what a capture,
   a save state or kf4-gsreplay start from. gsn_vram is the 4 MB as the GS has it, render targets
   written back. */
const void *gsn_vram(void);
void gsn_capture(void (*emit)(uint32_t type, const void *data, size_t size));
int gsn_restore(uint32_t type, const void *data, size_t size);

/* Save states (port/platform/savestate.c, through port/platform/gs.c): the native GS's state, VRAM
   first (4 MB, as the GS lays it out, render targets written back), then its registers and the CLUT
   buffer. Taken between GIF packets. */
size_t gsn_state_size(void);
void gsn_state(PortStateIo *io);
/* (--renderer modern) the page-ownership table (gsn_standalone.inc), after gsn_state: saving, its size
   (0 with the scene off) and then its bytes; loading, a state's `size` bytes (kept when the scene is on) */
size_t gsn_table_state_size(void);
void gsn_table_state(PortStateIo *io, size_t size);

/* A presenter on the native GS's own Vulkan device (port/platform/present_vk.cpp): the window's
   swapchain takes the picture straight from the GPU, with no read-back.
   gsn_enable_present, before gsn_open: the instance gets the surface extensions and the device the
   swapchain's, when the Vulkan loader and the GPU have them. gsn_present_device then hands out the
   device (0 if it has no swapchain); the presenter submits to the native GS's queue, between
   gsn_queue_lock and gsn_queue_unlock. Handles are Vulkan's, as void pointers and 64-bit integers. */
typedef struct {
    void *instance;           /* VkInstance */
    void *gpu;                /* VkPhysicalDevice */
    void *device;             /* VkDevice */
    void *queue;              /* VkQueue: the graphics queue the native GS draws on */
    uint32_t queue_family;
    uint32_t api_version;     /* the instance's */
    void *get_instance_proc_addr; /* PFN_vkGetInstanceProcAddr */
} GsnPresentDevice;
void gsn_enable_present(void);
int gsn_present_device(GsnPresentDevice *out);
void gsn_queue_lock(void);
void gsn_queue_unlock(void);
/* The picture of the last vblank asked for with GSN_WANT_IMAGE: an RGBA8 image (VkImage) in the
   GENERAL layout, width x height. The native GS writes it again at each vblank, with a barrier after
   whatever was submitted to the queue before, so a presenter's command buffer, submitted to the same
   queue with a barrier before it reads, sees one whole picture. Between take and release (once the
   presenter's command buffer is submitted) the image is not destroyed. serial counts the pictures.
   The native GS keeps the last few pictures (a ring of images): gsn_scanout_take_tag takes the newest
   with that tag (gsn_picture's), or the newest of all when it is no longer there. */
typedef struct {
    uint64_t image;
    uint32_t width, height;
    uint64_t serial;
    uint64_t tag;
    uint64_t scaled; /* the picture at the window's size (VkImage, GENERAL), 0: none */
    uint32_t scaled_width, scaled_height;
} GsnScanout;
int gsn_scanout_take(GsnScanout *out);
int gsn_scanout_take_tag(GsnScanout *out, uint64_t tag);
void gsn_scanout_release(void);

/* --frame-rate's pictures between drawn again on the GPU (port/platform/frame_interp.c, "On the GPU";
   gsn.cpp, "Pictures between on the GPU"). The DMA controller's thread walks the game's frame once and
   hands over:
   - gsn_interp_frame: the frame's packets as a picture between draws them (the buffer shown drawn into,
     the uploads asked for again ...), its vertices where the game put them, and for each vertex kick in
     them (XYZ2, XYZF2, XYZ3, XYZF3, whichever way written: PACKED, A+D, REGLIST) how a picture between
     moves it (GsnInterpMove). The native GS takes the struct and frees it, its moves and its stream
     (free()) when done with them.
   - gsn_interp_picture, for each picture between: its time, the cameras and the model parts that move
     with their model at that time. The first draws the frame from its packets, recording the draws
     (their state, and each corner as the kicks it is made of); the others draw the recording again,
     every vertex moved in the vertex shader (shaders/gs_replay.vert), or (when the GS is not as the
     recording found it) the packets once more.
   A GsnInterpMove, 16 words (the vertex shader's Move):
     w[0]: kind (GSN_IM_3D, GSN_IM_FLAT, or 0: as it is) and its flags
     3D: w[1] the run (model part), w[2] and w[3] the two kicks before with a place in the same path
         (GSN_IM_NONE: none: an ADC the picture adds covers the next two), w[4..6] the place in the
         frame's view (floats), w[7] its depth, w[8..10] its model's place at t 0 less its place in this
         frame (in the world or, before the eye, the camera's space), w[11..13] the model's move from t 0
         to t 1, w[14] Z - cz (float). (Moves from where the vertex is, so that the floats' precision is
         the move's, not the place's: no move, no change.)
     FLAT (the 2D): w[1] and w[2] RGBA of the frame before and of this one, w[3] and w[4] X | Y << 16
         (12.4) likewise, w[5] and w[6] S and T (or U and V in texels) of this one, w[7] and w[8] what is
         added to them at t 0 (floats) */
#define GSN_IM_NONE 0xFFFFFFFFu
#define GSN_IM_3D 1u
#define GSN_IM_FLAT 2u
/* 3D */
#define GSN_IM_VIEW 0x4u     /* it has a place (vertex_view) */
#define GSN_IM_OBJ 0x8u      /* it can move with its model: P0, D (when its run's bit is set) */
#define GSN_IM_WORLD 0x10u   /* the world or the reflection: else with the camera */
#define GSN_IM_EYE 0x20u     /* before the eye: P0 and D in the camera's space */
#define GSN_IM_REFLECT 0x40u /* by the reflection's projection */
#define GSN_IM_SCALE_ST 0x80u /* its own ST (Q 1/w): S and T scaled, Q made 1/w again */
#define GSN_IM_XYZF 0x100u   /* 24-bit Z */
#define GSN_IM_SCREEN_ST 0x200u /* its S and T made from the screen, with the place (frame_interp.c, StPlane):
                                   H's h0..h5 in w[8..13], h6 in w[15], h7 in w[1] (floats) */
/* FLAT */
#define GSN_IM_XY 0x4u
#define GSN_IM_RGBA 0x8u
#define GSN_IM_ST 0x10u
#define GSN_IM_UV 0x20u
#define GSN_IM_PREV 0x40u   /* the frame shown blended in (Game_BlendPreviousFrame): alpha (A^t) */
#define GSN_IM_PAIRED 0x80u /* (PREV) its alpha is the frame before's to this one's */
typedef struct {
    uint32_t w[16];
} GsnInterpMove;
typedef struct {
    uint64_t serial;       /* the frame's: the pictures say which they are of */
    float screen[2][5];    /* the world's projection and the reflection's: sx, sy, cx, cy, cz */
    float move0[12], mid0[12]; /* GsnInterpPicture's move and mid at t 0 (the camera of the frame shown) */
    uint32_t kicks;        /* the vertex kicks in the stream: moves has one for each */
    uint32_t runs;         /* the runs (model parts) the moves name */
    uint32_t still;        /* 1: the camera of the frame shown is this frame's, bit for bit (every picture between is
                              then seen from the frame's camera: gsn_scene_targets.inc's sat_kept) */
    GsnInterpMove *moves;  /* (malloc'd) */
    uint8_t *stream;       /* (malloc'd) GIF packets: a uint32_t path, a uint32_t size, then the bytes */
    size_t stream_bytes;
} GsnInterpFrame;
typedef struct {
    uint64_t serial;   /* the frame's */
    float move[12];    /* the frame's view to the picture's: rows of the rotation less the identity, each with its
                          translation */
    float mid[12];     /* the world to the picture's view: rows of the rotation (and the translation) */
    float t;           /* 0 the frame shown, 1 the frame drawn */
    uint32_t runs;     /* bits */
    const uint32_t *run_bits; /* (runs + 31) / 32 words: the runs drawn with their model's move */
} GsnInterpPicture;
void gsn_interp_frame(GsnInterpFrame *frame);
void gsn_interp_picture(const GsnInterpPicture *picture);

/* What the last frames cost and did (--stats, the overlay) */
typedef struct {
    uint64_t draws, primitives, passes, texture_decodes, texture_bytes, target_copies, writebacks,
        uploads, upload_bytes, shader_blends, texture_reuses, uploads_skipped, uploads_copied;
    uint64_t upload_pages_held; /* pages of an upload found again not compared: as it left them, not written since */
    /* nanoseconds spent (nested: a flush inside the GIF's time counts in both) */
    uint64_t ns_gif, ns_flush, ns_upload, ns_writeback, ns_decode, ns_vsync, ns_swizzle, ns_sync;
    /* what it holds now: render targets, textures, decoded images (and their MB), uploads kept */
    uint64_t targets, textures, pool_images, pool_mb, upload_copies;
    uint64_t replay_uploads, replay_misses; /* uploads asked for again by --frame-rate's pictures between */
    uint64_t ns_draw_setup, ns_draw_record; /* a draw's state worked out; its Vulkan commands */
    uint64_t ms_resolves, ms_loads; /* --native-msaa: multisampled images resolved, and loaded from their image */
    uint64_t cutout_draws;          /* --native-cutout-aa: draws shaded at every sample */
    uint64_t shadows, shadows_gone, shadows_resolved; /* write-backs put off, never owed, made later (gsn.cpp, shadows) */
    uint64_t shadow_reads_covered;  /* textures read over pages owed, all of what they read written since (shadow_covers) */
    uint64_t writebacks_overwritten; /* write-backs not made: the target under a GPU copy's whole pages (begin_transfer) */
    uint64_t silhouettes_whole;     /* silhouettes drawn whole, not in runs (flush_batch, "A silhouette") */
    uint64_t layer_draws;           /* --native-layer-z tolerant: batches with layers (flush_batch, "Z and layers") */
    /* --frame-rate's pictures between on the GPU (gsn_interp_picture): drawn from the packets (the first
       of a frame, or again: interp_records), from the recording (interp_replays), and the time each took */
    uint64_t interp_records, interp_replays, interp_misses, ns_interp_record, ns_interp_replay;
    uint64_t interp_mid_submits; /* submits while a picture between is recorded (write-backs in it) */
    /* why drawn from the packets (gsn.cpp, interp_cannot_replay): [1] the GS's targets changed, [2] its registers, [3] the
       frame's first picture between, [4] the recording could not be made again (a read-back in it), [5] VRAM or the
       CLUT written since; replays that named a target the recording made and kept, or made and dropped (its own) */
    uint64_t interp_rerecord[8];
    uint64_t interp_kept_targets, interp_own_targets;
    uint64_t interp_newer_settled; /* replays that first made a multisampled target's two images the same (interp_settle_newer) */
    /* graphics pipelines bound (Granite's vkCmdBindPipeline, all of the GS's thread's and the presenter's), and those of
       the pictures between drawn from the packets and from the recording; the draws of those from the recording */
    uint64_t pipeline_binds, interp_record_binds, interp_replay_binds, interp_replay_draws;
    /* Why render passes ended, and why multisampled images were resolved (GSN_WHY_*); the bytes the
       passes loaded and stored (their attachments, whole), resolves and copies moved; pictures made */
    uint64_t pass_ends[16], resolves_by[16];
    uint64_t pass_load_bytes, pass_store_bytes, resolve_bytes, copy_bytes, pictures;
    uint64_t blend_copies_in_pass; /* shader blending's copies of the target made in the render pass */
    /* native draw (gsn_native_draw.inc): draws and batches drawn, draws whose frame was gone or whose
       program is not drawn yet, meshes put on the GPU, register writes left out (--native-draw-compare) */
    uint64_t nd_draws, nd_batches, nd_lost, nd_unsupported, nd_mesh_uploads, nd_skipped, nd_arenas;
    /* (native draw) the CPU plan (nd_plan): its time, the vertices it worked out, and those it found planned
       already (the same batch by the same matrix: a camera standing still, a picture between) */
    uint64_t ns_nd_plan, nd_plan_vertices, nd_plan_reused;
    /* (native draw) pictures between: model parts drawn between their two places (P2-5), strips (the
       trail) drawn between their two frames' */
    uint64_t nd_moved_models, nd_moved_strips;
    /* the back end (gsn_backend.inc): its thread's time making the calls into Vulkan */
    uint64_t ns_backend;
    uint64_t loads_before_pass; /* targets loaded from VRAM before the render pass under way, not ending it */
    uint64_t loads_scaled;      /* target loads sent at the PS2's size and scaled on the GPU (load_target_rect) */
    /* images let go by the back end's thread (let_go: the last reference dropped there), and images made without
       Granite's own layout change (general_image: UNDEFINED, the change in the GS's stream) */
    uint64_t images_let_go, images_untransitioned;
} GsnStats;
enum {
    GSN_WHY_OTHER, GSN_WHY_TARGET, GSN_WHY_MSAA, GSN_WHY_BLEND, GSN_WHY_TEXTURE, GSN_WHY_DECODE, GSN_WHY_UPLOAD,
    GSN_WHY_COPY, GSN_WHY_WRITEBACK, GSN_WHY_SCANOUT, GSN_WHY_SUBMIT, GSN_WHY_COUNT
};
const char *gsn_why_name(int why);
/* KF4_GSN_GPU_TIME=1: the GPU's time by kind of work (timestamps around each), printed by --stats */
void gsn_print_gpu_times(void);
extern GsnStats gsn_stats;

#ifdef __cplusplus
}
#endif

#endif
