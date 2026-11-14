/* The plain types and constants that the PC renderer (port/gs/pc/, pcr.h) and the port share: the rectangle of a GS
   buffer, the picture handed to the presenter and the device it lives on, the frame and the pictures between that
   port/platform/frame_interp.c makes, and the mode numbers the settings hold. The functions are the PC renderer's
   (pcr.h), reached through port/platform/gs_api.h. */
#ifndef PORT_GS_SHARED_GSN_H
#define PORT_GS_SHARED_GSN_H

#include <stddef.h>
#include <stdint.h>

/* The presenter's picture and the wait for it (pcr_picture's want): GSN_WANT_PIXELS asks for the picture in frame,
   GSN_WANT_IMAGE for the image the GPU holds, GSN_PRESENT for the presenter to be given it. With GSN_WANT_IMAGE and
   GSN_PRESENT the image is new, from the renderer's thread once it is there. Without GSN_WANT_PIXELS the caller does
   not wait for the picture (and gets GSN_PRESENT, with GSN_WANT_IMAGE if asked); a vblank with no picture wanted does
   not wait either. */
#define GSN_WANT_PIXELS 1
#define GSN_WANT_IMAGE 2
#define GSN_PRESENT 4
/* The same with the picture's tag (PORT_PICTURE_TAG: its vblank and when in it), which the presenter gets with it. With
   GSN_EXTRA it is not a vblank but one more picture between two (--frame-rate above 60): the buffer shown read again,
   whole (none when the display reads it a field at a time), with nothing of the vblank's own done (the deinterlace's
   history, a new scale). */
#define GSN_EXTRA 16
/* With GSN_WANT_IMAGE and GSN_WANT_PIXELS: the pixels are the picture as the window shows it (the scaled image,
   pcr_set_present_scale) when there is one */
#define GSN_WANT_SCALED 32
/* The most the render targets are scaled across (x_scale of pcr_set_scale: 1 to GSN_MAX_SCALE_X; y_scale is 1 to 4) */
#define GSN_MAX_SCALE_X 6
/* The GPU the PC renderer is on: GSN_GPU_DISCRETE, GSN_GPU_INTEGRATED or GSN_GPU_OTHER (a CPU, virtual, or none open).
   port/platform/gs.c picks the --native-scale auto's scale from it */
#define GSN_GPU_OTHER 0
#define GSN_GPU_INTEGRATED 1
#define GSN_GPU_DISCRETE 2
/* The mipmap level a texture is read at (pcr_set_lod): the PS2's picture is GSN_LOD_PS2 (not kept by the settings yet),
   GSN_LOD_SCALE (the default) takes it log2 of the scale finer, GSN_LOD_FULL level 0 at every distance */
#define GSN_LOD_PS2 0
#define GSN_LOD_SCALE 1
#define GSN_LOD_FULL 2
/* Cut-out edges (pcr_set_cutout_aa): shaded once a pixel (GSN_CUTOUT_AA_OFF), or at every sample (GSN_CUTOUT_AA_SSAA) */
#define GSN_CUTOUT_AA_OFF 0
#define GSN_CUTOUT_AA_SSAA 1
/* 2D textured from VRAM at a scale above 1 (pcr_set_2d): read nearest at the PS2's pixels, or bilinear (smooth) */
#define GSN_2D_PS2 0
#define GSN_2D_SMOOTH 1
/* How precisely colours are kept (pcr_set_colour): the GS's 8 bits and rounding, or 16 bits and floats in the
   colour arithmetic (the picture dithered to 8 bits as it is scanned out) */
#define GSN_COLOUR_EXACT 0
#define GSN_COLOUR_HIGH 1
/* How a layer blended over a surface is Z tested (pcr_set_layer_z): as the GS, or within one Z (tolerant layers) */
#define GSN_LAYER_Z_PS2 0
#define GSN_LAYER_Z_TOLERANT 1
/* Which of the two the frame has for the draws the game makes natively (pcr_set_native_draw): the native draws, or the
   PS2 path's packets */
#define GSN_NATIVE_DRAW_NATIVE 0
#define GSN_NATIVE_DRAW_PS2 1

/* A rectangle of a GS buffer: base (256-byte blocks), width (64-pixel units), format, place, size */
typedef struct {
    uint32_t bp, bw, psm, x, y, w, h;
} GsnRect;

/* A presenter on the PC renderer's own Vulkan device (port/platform/present_vk.cpp): the window's swapchain takes the
   picture straight from the GPU, with no read-back. pcr_enable_present, before pcr_open: the instance gets the surface
   extensions and the device the swapchain's, when the Vulkan loader and the GPU have them. pcr_present_device then
   hands out the device (0 if it has no swapchain); the presenter submits to the renderer's queue, between
   pcr_queue_lock and pcr_queue_unlock. Handles are Vulkan's, as void pointers and 64-bit integers. */
typedef struct {
    void *instance;           /* VkInstance */
    void *gpu;                /* VkPhysicalDevice */
    void *device;             /* VkDevice */
    void *queue;              /* VkQueue: the graphics queue the renderer draws on */
    uint32_t queue_family;
    uint32_t api_version;     /* the instance's */
    void *get_instance_proc_addr; /* PFN_vkGetInstanceProcAddr */
} GsnPresentDevice;

/* The picture of the last vblank asked for with GSN_WANT_IMAGE: an RGBA8 image (VkImage) in the GENERAL layout,
   width x height. The renderer writes it again at each vblank, with a barrier after whatever was submitted to the queue
   before, so a presenter's command buffer, submitted to the same queue with a barrier before it reads, sees one whole
   picture. Between take and release (once the presenter's command buffer is submitted) the image is not destroyed.
   serial counts the pictures. The renderer keeps the last few pictures (a ring of images): pcr_scanout_take_tag takes
   the newest with that tag (pcr_picture's), or the newest of all when it is no longer there. */
typedef struct {
    uint64_t image;
    uint32_t width, height;
    uint64_t serial;
    uint64_t tag;
    uint64_t scaled; /* the picture at the window's size (VkImage, GENERAL), 0: none */
    uint32_t scaled_width, scaled_height;
} GsnScanout;

/* --frame-rate's pictures between drawn on the GPU (port/platform/frame_interp.c, "On the GPU"). The DMA controller's
   thread walks the game's frame once and hands over:
   - GsnInterpFrame: the frame's packets as a picture between draws them (the buffer shown drawn into, the uploads asked
     for again ...), its vertices where the game put them, and for each vertex kick in them (XYZ2, XYZF2, XYZ3, XYZF3,
     whichever way written: PACKED, A+D, REGLIST) how a picture between moves it (GsnInterpMove). The renderer takes the
     struct and frees it, its moves and its stream (free()) when done with them.
   - GsnInterpPicture: for each picture between: its time, the cameras and the model parts that move with their model
     at that time. The first draws the frame from its packets, recording the draws (their state, and each corner as
     the kicks it is made of); the others draw the recording again, every vertex moved in the vertex shader
     (port/gs/pc/shaders/pcr_prim.vert), or (when the GS is not as the recording found it) the packets once more.
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
                              then seen from the frame's camera) */
    GsnInterpMove *moves;  /* (malloc'd) */
    uint8_t *stream;       /* (malloc'd) GIF packets: a uint32_t path, a uint32_t size, then the bytes */
    size_t stream_bytes;
    /* What the stream did to the frame's packets, for the renderer that keeps the frame's own walk and makes the
       stream's of it (pcr.cpp's Intake) */
    uint64_t packet_bytes;          /* the frame's packets as the game sent them (the pending frame's), in bytes */
    uint32_t draw_fbp, shown_fbp;   /* the buffer the frame drew into (FBP), and the one shown, which the stream draws
                                       into instead (FRAME, TEX0 and BITBLTBUF naming draw_fbp name shown_fbp) */
    uint32_t uploads_asked_again;   /* 1: its uploads into anything but draw_fbp are asked for again (0x7C) */
    uint32_t shown_kept;            /* 1: the frame shown is kept (0xF2), and the stream's records that set TEX0 to
                                       shown_fbp read that copy (between 0xF4 and 0xF5) */
} GsnInterpFrame;
typedef struct {
    uint64_t serial;   /* the frame's */
    float move[12];    /* the frame's view to the picture's: rows of the rotation less the identity, each with its
                          translation */
    float mid[12];     /* the world to the picture's view: rows of the rotation (and the translation) */
    float t;           /* 0 the frame shown, 1 the frame drawn */
    double hand_yaw_rad, hand_pitch_rad; /* the mouse's hand its camera was turned to (port/include/port/mouse_lead.h): a
                          renderer turns it on by what the hand has moved since, as it draws it */
    double shown_s;    /* when the window shows it (port_wall_seconds; 0: not known): the mouse's hand is taken a steady
                          time before it */
    uint32_t leave_out_late; /* 1: the renderer leaves it out when it would be late, and begins it no sooner than it
                          needs (--frame-rate unlocked: port/gs/pc/pcr_late.h) */
    uint32_t runs;     /* bits */
    const uint32_t *run_bits; /* (runs + 31) / 32 words: the runs drawn with their model's move */
} GsnInterpPicture;

#endif
