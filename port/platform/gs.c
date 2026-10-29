/* The GS: the GIF's three paths and the display, on the DMA controller's thread.

   Packets come from VU1's XGKICK (PATH1, port/platform/vif1.c), VIF1's DIRECT (PATH2) and DMA
   channel 2 (PATH3, port/platform/dmac.c), and go to the renderer --renderer chose: the backend in
   port/gs/gs_parallel.cpp (paraLLEl-GS, the default), the native GS (port/gs/native/, step 3's), or
   both, whose pictures are then compared (port_renderer_diff). At each vblank the kernel snapshots the display registers (PMODE..BGCOLOR in the
   register page) and queues them here; the controller's thread finishes the channels started before
   it, then has the backend scan the frame out, and hands the picture to the window
   (port/platform/video.c) and to --dump-frames.

   Without the backend (no Vulkan, or a --headless run that neither dumps nor captures) the packets are read all
   the same, so VIF1 and VU1 run, and then dropped. What the game does never depends on it: the
   picture is all the GS gives back, apart from the textures sceGsExecStoreImage reads, which the
   game only sends to the GS again. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/debug.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/present_vk.h"
#include "port/render.h"
#include "port/savestate.h"
#include "port/overlay.h"
#include "port/settings.h"

#ifdef KF4_HAVE_PARALLEL_GS
int port_gs_backend_open(void);
size_t port_gs_backend_state_size(void);
void port_gs_backend_state(PortStateIo *io);
void port_gs_backend_gif(int path, const void *data, size_t bytes);
void port_gs_backend_read_fifo(void *dst, uint32_t qwords);
int port_gs_backend_vsync(const uint64_t *privileged, int field, int want_pixels, PortGsFrame *frame);
void *port_gs_backend_context(void);
#include "gs_api.h"

/* --renderer pc: the PC renderer (port/gs/pc/) opened in place of the native GS, behind the same calls (gs_api.h) */
int gsr_pc_renderer;
#endif

#define VSYNC_QUEUE 16

typedef struct {
    uint64_t privileged[0xF0 / 8];
    uint64_t vblank;
    int field;
} VsyncRequest;

static int backend;     /* a renderer is up */
static int have_gs;     /* paraLLEl-GS is */
static int have_native; /* the native GS is */
PortRendererDiff port_renderer_diff;
static VsyncRequest queue[VSYNC_QUEUE];
static volatile uint64_t queue_head; /* written by the game thread */
static volatile uint64_t queue_tail; /* written by the controller's thread */
static PortGsFrame frame;
static PortGsFrame native_frame; /* --renderer both: the native GS's picture, beside paraLLEl-GS's */
static uint64_t interp_behind;   /* --frame-rate: pictures between left out, the renderer behind the game */
/* --frame-rate: the pictures between cut to fewer a vblank while the native GS's thread is behind
   (port_gs_run_vsync): interp_allow of them a vblank (INTERP_ALL: no cut), one fewer each time it falls
   behind, one more after interp_rest vblanks; vblanks drawn with fewer and with none; the cuts */
#define INTERP_ALL 16
#define INTERP_REST_MAX 120 /* the longest wait for one more picture: 2 s (it was 8 s, at 60 that long after a load) */
#define INTERP_LOAD_GRACE 120 /* vblanks after a quick load or teleport, and 30 after the overlay, not judged */
static uint64_t interp_vblanks_left_out, interp_vblanks_fewer, interp_cuts, interp_rest = 30;
static uint64_t interp_settle_until, interp_next_raise, interp_raised_at, interp_last_cut, interp_judge_from;
static int interp_behind_run, interp_allow = INTERP_ALL;
static volatile int interp_cut_now; /* (the counter: pictures between a vblank left out by the cut, now) */
static volatile int interp_cut_reset_asked; /* (port_gs_interp_cut_reset: the game's thread) */

/* After a quick load or teleport: no cut, and the load's falling behind not judged */
void port_gs_interp_cut_reset(void) {
    __atomic_store_n(&interp_cut_reset_asked, 1, __ATOMIC_RELEASE);
}

/* KF4_INTERP_CUT=0 (measure): no cut, every picture between drawn however far behind the native GS falls (the
   game's thread then waits for it): what a place can keep up */
static int interp_cut_on(void) {
    static int on = -1;

    if (on < 0) {
        on = getenv("KF4_INTERP_CUT") == NULL || atoi(getenv("KF4_INTERP_CUT")) != 0;
    }
    return on;
}

int port_interp_cut(void) {
    return __atomic_load_n(&interp_cut_now, __ATOMIC_RELAXED);
}

/* --dump-frames: the vblanks whose picture is written, sorted */
static uint64_t *dump_at;
static int dump_count;
static int dump_next;

static void parse_dumps(void) {
    const char *p = port_options.dump_frames;

    if (p == NULL) {
        return;
    }
    while (*p) {
        char *end;
        unsigned long long v = strtoull(p, &end, 0);

        if (end == p) {
            fprintf(stderr, "--dump-frames wants vblank numbers joined by commas, not %s\n", port_options.dump_frames);
            exit(2);
        }
        dump_at = realloc(dump_at, sizeof(*dump_at) * (size_t)(dump_count + 1));
        dump_at[dump_count++] = v;
        p = *end == ',' ? end + 1 : end;
    }
}

static void open_backend(void *unused) {
    (void)unused;
#ifdef KF4_HAVE_PARALLEL_GS
    if (port_options.renderer != PORT_RENDERER_NATIVE) {
        have_gs = port_gs_backend_open();
    }
    if (port_options.renderer != PORT_RENDERER_GS) {
        gsr_pc_renderer = port_options.renderer_pc;
        /* a Vulkan device of its own, beside paraLLEl-GS's (whose context has features on that the
           native GS does not want, and whose threads submit to the queue unlocked by the other device);
           with the window's swapchain on it for --presenter vulkan (port/platform/present_vk.cpp) */
        if (port_options.presenter == PORT_PRESENTER_VULKAN && !port_options.headless) {
            gsr_enable_present();
        }
        gsr_set_upscaler(port_options.upscaler); /* (before: --upscaler dlss's Vulkan extensions) */
        have_native = gsr_open(NULL);
        if (have_native) {
            char path[1100];

            gsr_set_scale(port_gs_native_scale_x(), port_gs_native_scale());
            if (port_options.native_msaa > 1) {
                port_options.native_msaa = gsr_set_msaa(port_options.native_msaa);
            }
            gsr_set_lod(port_options.native_lod);
            port_options.native_aniso = gsr_set_aniso(port_options.native_aniso);
            gsr_set_cutout_aa(port_options.native_cutout_aa);
            gsr_set_2d(port_options.native_2d);
            gsr_set_colour(port_options.native_colour);
            gsr_set_layer_z(port_options.native_layer_z);
            gsr_set_shine_exact(port_options.native_shine_exact);
            gsr_set_native_draw(port_options.native_draw_compare == PORT_ND_COMPARE_PS2 ? GSN_NATIVE_DRAW_PS2
                                                                                        : GSN_NATIVE_DRAW_NATIVE);
            gsr_set_standalone(port_options.renderer_modern); /* --renderer modern (P8) */
            gsr_set_hardware_sampler(port_options.native_sampler == PORT_SAMPLER_HARDWARE); /* --native-sampler (P1) */
            gsr_set_pc_world(port_options.renderer_fast);                                  /* --renderer fast (P2) */
            if (port_options.scene_scale || port_options.scene_msaa) {
                /* --scene-scale, --scene-msaa (P8 T2): across as port_gs_native_scale_x has it */
                int s = port_options.scene_scale;
                int sx = s && port_options.native_widescreen && port_options.widescreen ? (4 * s + 2) / 3 : s;
                gsr_set_scene(sx, s, port_options.scene_msaa);
            }
            gsr_set_presenter(port_video_submit, port_video_submit_image);
            snprintf(path, sizeof(path), "%s/native_gs_pipelines.bin", port_data_dir());
            gsr_pipeline_cache(path);
        }
    }
    if (port_options.renderer == PORT_RENDERER_NATIVE && !have_native) {
        /* the default renderer did not start (no Vulkan device it can use): paraLLEl-GS, if it can */
        have_gs = port_gs_backend_open();
        if (have_gs) {
            fprintf(stderr, "[gs] the native GS did not start: paraLLEl-GS draws instead\n");
        }
    }
    if (port_options.native_draw && !have_native) {
        /* (the native draws are the native GS's: without it, nothing would draw them) */
        fprintf(stderr, "[gs] --native-draw needs the native GS: off\n");
        port_options.native_draw = 0;
    }
    /* what did start, for the window (the Vulkan presenter needs the native GS), the overlay and --stats */
    if (have_gs || have_native) {
        port_options.renderer = have_gs && have_native ? PORT_RENDERER_BOTH
                                : have_native          ? PORT_RENDERER_NATIVE
                                                       : PORT_RENDERER_GS;
    }
    backend = have_gs || have_native;
#endif
}

void port_gs_init(void) {
    parse_dumps();
    if (port_options.no_gs || (port_options.headless && dump_count == 0 && !port_debug_wants_gs())) {
        return;
    }
    port_dmac_call(open_backend, NULL);
    if (!backend) {
        fprintf(stderr, "[gs] no renderer: the game runs, but nothing is drawn\n");
    }
}

int port_gs_enabled(void) {
    return backend;
}

/* ---- The GIF ----------------------------------------------------------------------------------
   Each path's place in its GIF packet, read from the tags as the packets go by, with or without the
   backend: where the packets end (the GIF switches paths only there, port/platform/dmac.c), and the
   FINISH register, which sets CSR's FINISH once the GS has had it.

   The game clears FINISH as the hardware wants, by writing 1 to it (packet_buffer.c's GS_CSR |= 2),
   but nothing on the PC sees a store into the register page, so that sets the bit instead. It makes
   no difference to the game: it reads FINISH only after waiting for the channel whose chain ends with
   the FINISH register, and the GS has had it by then. */
#define GS_REG_FINISH 0x61
#define GS_CSR_FINISH 2
#define GS_REG_BITBLTBUF 0x50
#define GS_REG_TRXREG 0x52
#define GS_REG_TRXDIR 0x53
#define GS_REG_PRIM 0x00
#define GS_REG_XYZF2 0x04
#define GS_REG_XYZ2 0x05
#define GS_REG_XYZF3 0x0C
#define GS_REG_XYZ3 0x0D
#define GS_REG_XYOFFSET_1 0x18
#define GS_REG_XYOFFSET_2 0x19
#define GS_REG_TEX0_1 0x06
#define GS_REG_FRAME_1 0x4C
#define GS_REG_FRAME_2 0x4D
#define GS_REG_TEX0_2 0x07

typedef struct {
    uint32_t left;   /* qwords of the current tag's data still to come */
    uint32_t flg;    /* its FLG: 0 PACKED, 1 REGLIST, 2 and 3 IMAGE */
    uint32_t nreg;   /* its NREG (1..16) */
    uint64_t regs;   /* its REGS */
    uint32_t reg;    /* PACKED: the register of the next qword */
    int ad;          /* PACKED: one of the registers is A+D */
    int eop;         /* the current tag ends its packet */
    int in_packet;   /* between a packet's first tag and its end */
    /* --widescreen (below): read only while it is on */
    uint32_t prim;   /* PRIM as this path last set it */
    uint32_t tbp[2]; /* TEX0_1's and TEX0_2's TBP0, as this path last set them */
    uint32_t dwords; /* REGLIST: the register writes still to come */
    int squeezing;   /* the current tag is read register by register, to be squeezed */
    int widescreen;  /* PORT_WIDESCREEN_: what the path's drawing is, as the game last said */
} GifPath;

static GifPath gif_paths[4];

/* A host-to-local transfer (TRXDIR 0) takes its pixels from IMAGE packets, maybe across several:
   the bits still to come, so that a save state waits until none are (the GS's own count cannot be
   saved). BITBLTBUF and TRXREG as last written say how many a transfer takes. */
static uint64_t image_bitbltbuf;
static uint64_t image_trxreg;
static uint64_t image_bits_left;

static uint32_t psm_bits(uint32_t psm) {
    switch (psm) {
    case 0x01: /* PSMCT24 */
    case 0x31: /* PSMZ24 */
        return 24;
    case 0x02: /* PSMCT16 */
    case 0x0A: /* PSMCT16S */
    case 0x32: /* PSMZ16 */
    case 0x3A: /* PSMZ16S */
        return 16;
    case 0x13: /* PSMT8 */
        return 8;
    case 0x14: /* PSMT4 */
        return 4;
    default: /* the 32-bit formats, PSMT8H, PSMT4HL, PSMT4HH */
        return 32;
    }
}

/* --widescreen (below): the GS addresses that hold a picture of the frame, kept from the packets
   whether the switch is on or not: what was drawn into (FRAME), and where a local-to-local transfer
   or a 640-wide upload (the frame the game read back) went. Any other upload there makes it
   a texture again. Each is its TBP + 1 (0: none). */
#define FRAME_PICTURES 16
static uint32_t frame_pictures[FRAME_PICTURES];
static uint32_t frame_picture_next;

static void frame_picture(uint32_t tbp, int is_picture) {
    int i;

    for (i = 0; i < FRAME_PICTURES; i++) {
        if (frame_pictures[i] == tbp + 1) {
            if (!is_picture) {
                frame_pictures[i] = 0;
            }
            return;
        }
    }
    if (is_picture) {
        frame_pictures[frame_picture_next++ % FRAME_PICTURES] = tbp + 1;
    }
}

static void image_register(uint32_t reg, uint64_t value) {
    if (reg == GS_REG_BITBLTBUF) {
        image_bitbltbuf = value;
    } else if (reg == GS_REG_TRXREG) {
        image_trxreg = value;
    } else if (reg == GS_REG_TRXDIR) {
        if ((value & 3) != 1) {
            frame_picture((uint32_t)(image_bitbltbuf >> 32) & 0x3FFF, (value & 3) == 2 || (image_trxreg & 0xFFF) == 640);
        }
        if ((value & 3) == 0) {
            image_bits_left = (image_trxreg & 0xFFF) * ((image_trxreg >> 32) & 0xFFF) *
                              psm_bits((uint32_t)(image_bitbltbuf >> 56) & 0x3F);
        } else {
            image_bits_left = 0;
        }
    }
}

static void image_data(size_t qwords) {
    uint64_t bits = (uint64_t)qwords * 128;

    image_bits_left = bits < image_bits_left ? image_bits_left - bits : 0;
}

/* ---- Widescreen ------------------------------------------------------------------------------
   With port_widescreen() on, the game draws its 3D world with a 16:9 field of view into the same
   640x448 frame, and the window shows that frame at 16:9. Its 2D was laid out for 4:3, so it is
   squeezed here to three quarters of its width, as the vertices go by, which keeps its shape:
   - into the middle of the frame (PORT_WIDESCREEN_2D): the menus, the title, the message boxes;
   - or, for the HUD of the top left, against the frame's left edge, unless the player wants it
     centred (PORT_WIDESCREEN_HUD_LEFT, port_options.widescreen_hud_centred).
   What the packet chain (PATH1 and PATH2) draws is what the game last said with an A+D write to
   PORT_GS_REG_WIDESCREEN (game/widescreen.c; sent only while the switch is on); PATH3 carries nothing
   but 2D (the menus and the title), so it is squeezed unless told otherwise.
   Left as they are:
   - the 3D world (PATH1, and the triangles the game cuts up on the CPU, PATH2), projected wide;
   - anything drawn while the draw offset is not the 640x448 frame's (XYOFFSET_1 X 0x6C00): copies of
     the frame into a texture, scrolling textures, the 256x256 draw environment;
     (a primitive of context 2 drawn into the frame buffer of context 1 is the frame's too, at a draw
     offset of its own: the menus place their models and pictures so, the Equipment screen's weapon at
     XYOFFSET_2 X 0x7580 and the Journal's picture at 0x7660. It is squeezed about the frame's middle
     where its draw offset puts it, XYOFFSET_2 X plus 320 pixels: about X 0x8000, its own middle,
     each kept its 4:3 place, off to the left of its squeezed frame);
   - a vertex of an untextured primitive at or past the frame's left or right edge: full-screen fades
     and fills, and panels that reach the edge, stay reaching the 16:9 picture's edge;
   - a primitive textured with a picture of the frame (TBP0 0 or 0x1180, the two frame buffers, or
     0x2BC0, where the game copies the frame): the dimmed world behind the menus, the screen effects.
   A movie draws into a frame of its own, at a draw offset of 0: its pictures are squeezed about the
   middle of that frame, and its black clear, which reaches the edges, becomes the bars.
   With the switch off nothing here runs, and the packets go to the GS untouched. */
static uint32_t draw_offset_x = 0x6C00; /* XYOFFSET_1's X as last written, 12.4 */
static uint32_t draw_offset_x2 = 0x6C00; /* XYOFFSET_2's */
static uint32_t frame_fbp[2];            /* FRAME_1's and FRAME_2's FBP as last written (these two are
                                            not in save states: the game writes them again each frame) */

int port_widescreen(void) {
    return port_options.widescreen;
}

int port_whole_frame(void) {
    return port_options.whole_frame;
}

int port_seamless_scroll(void) {
    return port_options.seamless_scroll;
}

int port_flicker_filter(void) {
    return port_options.flicker_filter;
}

/* A mark from the game: PATH1 and PATH2 come in the order of the one packet chain (VU1's packets and
   VIF1's DIRECT ones), so a mark in either goes for both. */
static void widescreen_marked(int path, int mode) {
    if (path == 3) {
        gif_paths[3].widescreen = mode;
    } else {
        gif_paths[1].widescreen = gif_paths[2].widescreen = mode;
    }
}

static int widescreen_mode(int path) {
    int mode = gif_paths[path].widescreen;

    if (mode == PORT_WIDESCREEN_3D && path == 3) {
        mode = PORT_WIDESCREEN_2D;
    }
    return mode;
}

static int is_frame_picture(uint32_t tbp) {
    int i;

    for (i = 0; i < FRAME_PICTURES; i++) {
        if (frame_pictures[i] == tbp + 1) {
            return 1;
        }
    }
    return 0;
}

/* the X of a vertex (12.4, in the low 16 bits of the register's data) for the path's mode */
static void squeeze_x(int path, uint8_t *data) {
    GifPath *p = &gif_paths[path];
    int mode = widescreen_mode(path);
    int x = data[0] | (data[1] << 8);
    int anchor = 0x8000;
    int shift = 0; /* the frame's place in this context's coordinates, from context 1's */

    if ((p->prim & 0x200) && frame_fbp[1] == frame_fbp[0]) {
        shift = (int)draw_offset_x2 - (int)draw_offset_x; /* context 2, into the frame (above) */
    }
    if (path == 3 && draw_offset_x == 0 && port_movie_playing()) {
        /* a movie (gfx/movie.c) draws its pictures into its own frame, with a draw offset of 0:
           squeezed about that frame's middle, between the bars of its black clear */
        anchor = 320 << 4;
    } else if (mode == PORT_WIDESCREEN_3D || (draw_offset_x >> 4) != 0x6C0) {
        return;
    }
    if (!(p->prim & 0x10) && (x <= 0x6C00 + shift || x >= 0x9400 + shift)) {
        return;
    }
    if ((p->prim & 0x10) && is_frame_picture(p->tbp[(p->prim >> 9) & 1])) {
        return;
    }
    if (mode == PORT_WIDESCREEN_HUD_LEFT && !port_options.widescreen_hud_centred) {
        anchor = 0x6C00;
    }
    anchor += shift;
    x = anchor + (x - anchor) * 3 / 4;
    data[0] = (uint8_t)x;
    data[1] = (uint8_t)(x >> 8);
}

/* A register write of a PACKED or REGLIST tag: PRIM and the vertices (only while squeezing). */
static void squeeze_register(int path, uint32_t reg, uint8_t *data) {
    switch (reg) {
    case GS_REG_PRIM:
        gif_paths[path].prim = (uint32_t)(data[0] | (data[1] << 8)) & 0x7FF;
        break;
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
        gif_paths[path].tbp[reg - GS_REG_TEX0_1] = (uint32_t)(data[0] | (data[1] << 8)) & 0x3FFF;
        break;
    case GS_REG_XYZF2:
    case GS_REG_XYZ2:
    case GS_REG_XYZF3:
    case GS_REG_XYZ3:
        squeeze_x(path, data);
        break;
    }
}

/* squeeze: the qwords may be changed (port_widescreen() is on, and they are a copy) */
static void read_gif(int path, uint64_t *qwords, size_t count, int squeeze) {
    GifPath *p = &gif_paths[path];
    size_t i = 0;

    while (i < count) {
        if (p->left == 0) {
            uint64_t tag = qwords[i * 2];
            uint32_t nloop = (uint32_t)(tag & 0x7FFF);
            uint32_t n;

            p->eop = (int)((tag >> 15) & 1);
            p->flg = (uint32_t)((tag >> 58) & 3);
            p->nreg = (uint32_t)(tag >> 60);
            if (p->nreg == 0) {
                p->nreg = 16;
            }
            p->regs = qwords[i * 2 + 1];
            p->reg = 0;
            p->ad = 0;
            p->squeezing = squeeze; /* not from the middle of a tag, when the switch has just been turned on */
            if (squeeze && ((tag >> 46) & 1)) {
                p->prim = (uint32_t)(tag >> 47) & 0x7FF; /* PRE */
            }
            if (p->flg == 0) {
                p->left = nloop * p->nreg;
                for (n = 0; n < p->nreg; n++) {
                    if (((p->regs >> (n * 4)) & 0xF) == 0xE) {
                        p->ad = 1;
                    }
                }
            } else if (p->flg == 1) {
                p->left = (nloop * p->nreg + 1) / 2;
                p->dwords = nloop * p->nreg;
            } else {
                p->left = nloop;
            }
            p->in_packet = !(p->eop && p->left == 0);
            i++;
            continue;
        }
        if (p->ad || (p->squeezing && squeeze && p->flg == 0)) {
            /* the A+D qwords: the one that writes FINISH */
            uint32_t descriptor = (uint32_t)(p->regs >> (p->reg * 4)) & 0xF;

            if (descriptor == 0xE) {
                uint32_t reg = (uint32_t)(qwords[i * 2 + 1] & 0xFF);

                if (reg == GS_REG_FINISH) {
                    __atomic_fetch_or(EE_HW(volatile uint64_t *, 0x12001000), (uint64_t)GS_CSR_FINISH, __ATOMIC_RELEASE);
                } else if (reg >= GS_REG_BITBLTBUF && reg <= GS_REG_TRXDIR) {
                    image_register(reg, qwords[i * 2]);
                } else if (reg == GS_REG_FRAME_1 || reg == GS_REG_FRAME_2) {
                    frame_picture((uint32_t)(qwords[i * 2] & 0x1FF) << 5, 1);
                    frame_fbp[reg - GS_REG_FRAME_1] = (uint32_t)(qwords[i * 2] & 0x1FF);
                } else if (reg == GS_REG_XYOFFSET_1) {
                    draw_offset_x = (uint32_t)(qwords[i * 2] & 0xFFFF);
                } else if (reg == GS_REG_XYOFFSET_2) {
                    draw_offset_x2 = (uint32_t)(qwords[i * 2] & 0xFFFF);
                } else if (p->squeezing && squeeze) {
                    if (reg == PORT_GS_REG_WIDESCREEN) {
                        widescreen_marked(path, (int)(qwords[i * 2] & 0xFF));
                    } else {
                        squeeze_register(path, reg, (uint8_t *)&qwords[i * 2]);
                    }
                }
            } else if (p->squeezing && squeeze) {
                /* PACKED: XYZF2 and XYZ2 have X in the low 16 bits as the registers do; the rest
                   (XYZF3 and XYZ3 among them) are the register's data as it is */
                squeeze_register(path, descriptor, (uint8_t *)&qwords[i * 2]);
            }
            p->reg = p->reg + 1 == p->nreg ? 0 : p->reg + 1;
            p->left--;
            i++;
        } else if (p->squeezing && squeeze && p->flg == 1) {
            /* REGLIST: two registers a qword (the last qword's second is padding when the count is odd) */
            int half;

            for (half = 0; half < 2 && p->dwords != 0; half++, p->dwords--) {
                squeeze_register(path, (uint32_t)(p->regs >> (p->reg * 4)) & 0xF, (uint8_t *)&qwords[i * 2 + half]);
                p->reg = p->reg + 1 == p->nreg ? 0 : p->reg + 1;
            }
            p->left--;
            i++;
        } else {
            size_t n = count - i < p->left ? count - i : p->left;

            if (p->flg >= 2) {
                image_data(n);
            }
            p->left -= (uint32_t)n;
            i += n;
        }
        if (p->left == 0 && p->eop) {
            p->in_packet = 0;
        }
    }
}

int port_gs_path_idle(int path) {
    return !gif_paths[path].in_packet;
}

void port_gs_gif(int path, const void *data, size_t bytes) {
    uint64_t start = port_options.stats ? port_ticks() : 0;

    if (port_options.widescreen) {
        /* a copy, squeezed (the game's packets are its own, and may be sent again) */
        static uint64_t *copy;
        static size_t copy_bytes;

        if (copy_bytes < bytes) {
            free(copy);
            copy_bytes = bytes * 2;
            copy = malloc(copy_bytes);
            if (copy == NULL) {
                PORT_FATAL("out of memory");
            }
        }
        memcpy(copy, data, bytes);
        data = copy;
        read_gif(path, copy, bytes / 16, 1);
    } else {
        read_gif(path, (uint64_t *)data, bytes / 16, 0);
    }
    if (port_gs_capture_recording) {
        port_gs_capture_gif(path, data, bytes);
    }
    if (port_frame_interp() && backend) {
        port_interp_keep(path, data, bytes); /* --frame-rate 60 (port/platform/frame_interp.c) */
    }
    if (port_whole_frame() && backend) {
        port_field_frames_keep(path, data, bytes); /* the fields' screens whole (port/platform/field_frames.c) */
    }
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_gs) {
        port_gs_backend_gif(path, data, bytes);
    }
    if (have_native) {
        gsr_gif(path, data, bytes);
    }
#endif
    if (port_options.stats) {
        port_stats.gif_ticks += port_ticks() - start;
    }
    if (path != 3 && !gif_paths[path].in_packet) {
        port_gif_path3_poll();
    }
}

void port_gs_read_fifo(void *dst, uint32_t qwords) {
    if (port_gs_capture_recording) {
        port_gs_capture_read_fifo(qwords);
    }
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_gs && have_native) {
        /* both read the same transfer; paraLLEl-GS's pixels are the ones the game gets */
        void *scratch = malloc((size_t)qwords * 16);

        if (scratch != NULL) {
            gsr_read_fifo(scratch, qwords);
            free(scratch);
        }
    } else if (have_native) {
        gsr_read_fifo(dst, qwords);
        return;
    }
    if (have_gs) {
        port_gs_backend_read_fifo(dst, qwords);
        return;
    }
#endif
    memset(dst, 0, (size_t)qwords * 16);
}

/* ---- Vblanks ---------------------------------------------------------------------------------- */
#ifdef KF4_HAVE_PARALLEL_GS
/* --frame-rate 60: the packets port/platform/frame_interp.c sends again, to the renderers only */
static void replay_gif(int path, const void *data, size_t bytes) {
    if (have_gs) {
        port_gs_backend_gif(path, data, bytes);
    }
    if (have_native) {
        gsr_gif(path, data, bytes);
    }
}

/* --renderer both: the two pictures, compared channel by channel */
static void compare_frames(const PortGsFrame *a, const PortGsFrame *b, uint64_t vblank) {
    PortRendererDiff *d = &port_renderer_diff;
    uint64_t sum = 0;
    uint32_t max = 0, pixels = 0;
    size_t i, n;

    if (a->width != b->width || a->height != b->height) {
        max = 255;
        pixels = a->width * a->height;
        n = 0;
    } else {
        n = (size_t)a->width * a->height;
    }
    for (i = 0; i < n; i++) {
        uint32_t p = a->rgba[i], q = b->rgba[i];
        uint32_t worst = 0;
        int c;

        if (p == q) {
            continue;
        }
        for (c = 0; c < 24; c += 8) {
            int e = (int)((p >> c) & 0xFF) - (int)((q >> c) & 0xFF);
            uint32_t m = (uint32_t)(e < 0 ? -e : e);

            sum += m;
            worst = m > worst ? m : worst;
        }
        max = worst > max ? worst : max;
        pixels += worst > 1;
    }
    d->frames++;
    d->frames_differ += pixels != 0;
    d->last_max = max;
    d->last_pixels = pixels;
    d->last_mean = n ? (double)sum / (double)(n * 3) : 255.0;
    if (max > d->worst_max) {
        d->worst_max = max;
        d->worst_vblank = vblank;
    }
}

/* --renderer-shown heatmap: where the two buffers differ, over the native GS's picture dimmed to grey.
   A pixel whose channels are all within 1 stays grey; any more is coloured by its largest difference,
   from blue (2) through green and yellow (around 16) to red (64 and more). */
static void heat_map(const PortGsFrame *a, const PortGsFrame *b, PortGsFrame *out) {
    size_t i, n = (size_t)a->width * a->height;

    out->width = a->width;
    out->height = a->height;
    for (i = 0; i < n; i++) {
        uint32_t p = a->rgba[i], q = b->rgba[i];
        uint32_t worst = 0, grey;
        int c;

        for (c = 0; c < 24; c += 8) {
            int e = (int)((p >> c) & 0xFF) - (int)((q >> c) & 0xFF);
            uint32_t m = (uint32_t)(e < 0 ? -e : e);

            worst = m > worst ? m : worst;
        }
        if (worst <= 1) {
            grey = (((q & 0xFF) * 77 + ((q >> 8) & 0xFF) * 150 + ((q >> 16) & 0xFF) * 29) >> 8) / 3;
            out->rgba[i] = 0xFF000000u | grey << 16 | grey << 8 | grey;
        } else {
            /* t: 0 at 2, 1 at 64 and beyond, on a log scale */
            double t = worst >= 64 ? 1.0 : log2((double)worst / 2.0) / 5.0;
            double r = t < 0.5 ? 0.0 : t < 0.75 ? (t - 0.5) * 4.0 : 1.0;
            double g = t < 0.25 ? t * 4.0 : t < 0.75 ? 1.0 : (1.0 - t) * 4.0;
            double bl = t < 0.25 ? 1.0 : t < 0.5 ? (0.5 - t) * 4.0 : 0.0;

            out->rgba[i] = 0xFF000000u | (uint32_t)(bl * 255.0 + 0.5) << 16 | (uint32_t)(g * 255.0 + 0.5) << 8 |
                           (uint32_t)(r * 255.0 + 0.5);
        }
    }
}

/* --renderer both: the buffer the display shows, as each renderer drew it (the scanouts differ by
   their own conventions: paraLLEl-GS's crop of the TV picture and its weave of the fields). With the
   heat map on, its picture into heat (1 if there is one). */
static int compare_buffers(VsyncRequest *request, int dump, PortGsFrame *heat) {
    static PortGsFrame a, b;
    GsnRect rect;

    if (!gsr_display_rect(request->privileged, &rect)) {
        return 0;
    }
    if (a.rgba == NULL) {
        a.rgba = malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS);
        b.rgba = malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS);
    }
    a.width = b.width = rect.w;
    a.height = b.height = rect.h;
    port_gs_backend_read_rect(rect.bp, rect.bw, rect.psm, rect.x, rect.y, rect.w, rect.h, a.rgba);
    gsr_read_rect(&rect, b.rgba);
    compare_frames(&a, &b, request->vblank);
    if (dump) {
        char name[64];

        snprintf(name, sizeof(name), "frame_%06llu_gs_fb.png", (unsigned long long)request->vblank);
        port_video_dump_named(&a, name);
        snprintf(name, sizeof(name), "frame_%06llu_native_fb.png", (unsigned long long)request->vblank);
        port_video_dump_named(&b, name);
    }
    if (port_options.renderer_heatmap) {
        heat_map(&a, &b, heat);
        return 1;
    }
    return 0;
}

/* With SDL_Renderer and the native GS alone, the best scaling (or the sharpening): the native GS's
   picture at the window's size, read back (GSN_WANT_SCALED), for SDL_Renderer to draw a pixel a pixel */
static int sdl_scaled(void) {
    if (port_video_takes_images() || !have_native || have_gs || port_options.headless ||
        (port_video_scaling() != PORT_SCALING_BEST && port_video_sharpen() <= 0)) {
        return 0;
    }
    return GSN_WANT_IMAGE | GSN_WANT_SCALED;
}

/* The size the window shows the picture at, for the native GS's scaling (video.c's SDL_Renderer) */
void port_gs_present_scale(int width, int height, float sharpen) {
    if (have_native) {
        gsr_set_present_scale(width, height, sharpen);
    }
}

/* A picture of a renderer's (or each's, with both) for the window and --dump-frames: 1 if one is shown.
   With the Vulkan presenter the native GS's picture stays on the GPU (GSN_WANT_IMAGE), and is read back
   only for a dump. */
static int present_frames(VsyncRequest *request, int shown, int dump) {
    static PortGsFrame heat;
    uint64_t tag = PORT_PICTURE_TAG(request->vblank, 0);
    int want = shown || dump;
    int want_native = shown && port_video_takes_images() ? GSN_WANT_IMAGE | (dump ? GSN_WANT_PIXELS : 0)
                      : want                             ? GSN_WANT_PIXELS
                                                         : 0;
    if (dump && port_options.dump_present_width > 0) {
        want_native |= GSN_WANT_IMAGE | GSN_WANT_PIXELS | GSN_WANT_SCALED; /* (--dump-present) */
    }
    int got_gs = 0, got_native = 0, got_heat = 0;
    PortGsFrame *show;

    if (have_gs) {
        got_gs = port_gs_backend_vsync(request->privileged, request->field, want, &frame);
    }
    if (have_native) {
        if (!have_gs && shown && !dump) {
            /* the native GS's thread hands the window the picture (or with the Vulkan presenter word of
               the new image) when it has it, and this thread goes on */
            return gsr_picture(request->privileged, request->field,
                               GSN_PRESENT | (port_video_takes_images() ? GSN_WANT_IMAGE : 0) | sdl_scaled(), NULL,
                               tag) != 0;
        }
        got_native = gsr_picture(request->privileged, request->field, want_native, &native_frame, tag);
    }
    if (have_gs && have_native) {
        if (heat.rgba == NULL) {
            heat.rgba = malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS);
        }
        got_heat = compare_buffers(request, dump, &heat);
    }
    if (have_gs && have_native && got_heat) {
        /* the heat map, in place of either picture (each still dumped under its own name) */
        if (dump && got_gs && (got_native & GSN_WANT_PIXELS)) {
            char name[64];

            snprintf(name, sizeof(name), "frame_%06llu_gs.png", (unsigned long long)request->vblank);
            port_video_dump_named(&frame, name);
            snprintf(name, sizeof(name), "frame_%06llu_native.png", (unsigned long long)request->vblank);
            port_video_dump_named(&native_frame, name);
        }
        show = &heat;
    } else if (have_gs && have_native) {
        show = port_options.renderer_shown == PORT_RENDERER_NATIVE ? &native_frame : &frame;
        if (show == &native_frame && !(got_native & GSN_WANT_PIXELS) && (got_native & GSN_WANT_IMAGE)) {
            port_video_submit_image(tag);
            return 1;
        }
        if (dump && got_gs && (got_native & GSN_WANT_PIXELS)) {
            /* each picture, the one shown under the usual name */
            char name[64];

            port_video_dump(show, request->vblank);
            snprintf(name, sizeof(name), "frame_%06llu_gs.png", (unsigned long long)request->vblank);
            port_video_dump_named(&frame, name);
            snprintf(name, sizeof(name), "frame_%06llu_native.png", (unsigned long long)request->vblank);
            port_video_dump_named(&native_frame, name);
            dump = 0;
        }
        if (!(show == &frame ? got_gs : got_native & GSN_WANT_PIXELS)) {
            return 0;
        }
        if (show == &native_frame && (got_native & GSN_WANT_IMAGE) && shown) {
            /* (dumped above) the window takes the image */
            port_video_submit_image(tag);
            return 1;
        }
    } else if (have_native) {
        if (got_native & GSN_WANT_IMAGE) {
            if (dump && (got_native & GSN_WANT_PIXELS)) {
                port_video_dump(&native_frame, request->vblank);
            }
            port_video_submit_image(tag);
            return 1;
        }
        if (!got_native) {
            return 0;
        }
        show = &native_frame;
    } else {
        if (!got_gs) {
            return 0;
        }
        show = &frame;
    }
    if (dump) {
        port_video_dump(show, request->vblank);
    }
    if (shown) {
        port_video_submit(show, tag);
    }
    return 1;
}

/* --frame-rate above 60: a picture between two vblanks (phase of the way from this one to the next),
   drawn into the buffer shown: the native GS's, read again whole (gsn_picture with GSN_EXTRA), for the
   window (shown) and as frame_<vblank>_<phase in thousandths>.png (dump) */
static void present_extra(VsyncRequest *request, double phase, int shown, int dump) {
    uint64_t tag = PORT_PICTURE_TAG(request->vblank, phase);
    int images = shown && (port_video_takes_images() || port_options.headless);

    if (dump) {
        int got = gsr_picture(request->privileged, request->field,
                              GSN_EXTRA | GSN_WANT_PIXELS | (images || port_options.dump_present_width > 0 ? GSN_WANT_IMAGE : 0) |
                                  (port_options.dump_present_width > 0 ? GSN_WANT_SCALED : 0),
                              &native_frame, tag);
        char name[64];

        if (got & GSN_WANT_PIXELS) {
            snprintf(name, sizeof(name), "frame_%06llu_%03d.png", (unsigned long long)request->vblank,
                     (int)(phase * 1000.0 + 0.5));
            port_video_dump_named(&native_frame, name);
            if (shown) {
                if (images) {
                    port_video_submit_image(tag);
                } else {
                    port_video_submit(&native_frame, tag);
                }
            }
        }
    } else if (shown) {
        gsr_picture(request->privileged, request->field,
                    GSN_EXTRA | GSN_PRESENT | (images ? GSN_WANT_IMAGE : 0) | sdl_scaled(), NULL, tag);
    }
}

/* KF4_INTERP_DRAW_ALL=1 (a speed test): headless, every picture between drawn as for a window */
static int draw_all(void) {
    static int all = -1;

    if (all < 0) {
        all = getenv("KF4_INTERP_DRAW_ALL") != NULL;
    }
    return all;
}
#endif

void port_gs_post_vsync(void) {
    VsyncRequest *request;
    uint64_t head;

    if (!backend) {
        return;
    }
    /* The scanout goes between the packets started before the vblank and those after: the game
       (and --fast all the more) runs ahead of the controller, and would have cleared the buffer
       shown by the time it was read. So it goes into the controller's queue after the channels
       started so far (dmac.c). */
    head = __atomic_load_n(&queue_head, __ATOMIC_RELAXED);
    while (head - __atomic_load_n(&queue_tail, __ATOMIC_ACQUIRE) >= VSYNC_QUEUE) {
        port_dmac_wake(); /* the GS is a whole queue behind: let it catch up */
    }
    request = &queue[head % VSYNC_QUEUE];
    memcpy(request->privileged, ee_hw_ptr(0x12000000), sizeof(request->privileged));
    request->vblank = port_vblank_count();
    request->field = (int)((*(volatile uint64_t *)ee_hw_ptr(0x12001000) >> 13) & 1);
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native && port_options.renderer_modern) {
        PortGameReadout game; /* --renderer modern's --stats a map (P8) */

        kf4_game_readout(&game);
        gsr_set_scene_map(game.map_loaded ? game.map_tag : -1);
    }
#endif
    if (port_gs_capture_armed) {
        port_gs_capture_note_vblank(request->vblank);
    }
    __atomic_store_n(&queue_head, head + 1, __ATOMIC_RELEASE);
    port_dmac_post_vsync();
}

void port_gs_set_native_scale(int scale) {
    port_options.native_scale = scale;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        gsr_set_scale(port_gs_native_scale_x(), port_gs_native_scale());
    }
#endif
}

/* --native-scale auto: the picture drawn about as tall as the window shows it, so that the window
   scales it by about 1 (1440 lines: 3x, 1344, a little up; 2160: 4x, 1792; 1080: 2x, 896; 448 * 2.5 and
   up round up), not a 640x448 picture blown up by bilinear filtering. A larger scale than the window's
   is thrown away by the window's shrinking. At most 2x on an integrated GPU, and 1x on a GPU that is
   not a real one (lavapipe) or with no window. */
int port_gs_native_scale(void) {
    int h, s, most = 4;

    if (port_options.native_scale >= 1) {
        return port_options.native_scale > 4 ? 4 : port_options.native_scale;
    }
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        int kind = gsr_gpu_kind();

        most = kind == GSN_GPU_DISCRETE ? 4 : kind == GSN_GPU_INTEGRATED ? 2 : 1;
    }
#endif
    h = port_video_picture_height();
    if (h <= 0) {
        return 1;
    }
    s = (h + 224) / 448;
    return s < 1 ? 1 : s > most ? most : s;
}

void port_gs_set_native_aniso(int taps) {
    port_options.native_aniso = taps < 1 ? 1 : taps;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        port_options.native_aniso = gsr_set_aniso(taps);
    }
#endif
}

void port_gs_set_native_lod(int mode) {
    port_options.native_lod = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        gsr_set_lod(mode);
    }
#endif
}

void port_gs_set_native_2d(int mode) {
    port_options.native_2d = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        gsr_set_2d(mode);
    }
#endif
}

void port_gs_set_native_cutout_aa(int mode) {
    port_options.native_cutout_aa = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        gsr_set_cutout_aa(mode);
    }
#endif
}

void port_gs_set_native_colour(int mode) {
    port_options.native_colour = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        gsr_set_colour(mode);
    }
#endif
}

void port_gs_set_native_draw(int kinds, int compare) {
    port_options.native_draw = 0; /* (the native draws are the native GS's) */
    port_options.native_draw_compare = compare;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        port_options.native_draw = kinds;
        gsr_set_native_draw(compare == PORT_ND_COMPARE_PS2 ? GSN_NATIVE_DRAW_PS2 : GSN_NATIVE_DRAW_NATIVE);
    }
#endif
}

void port_gs_set_native_layer_z(int mode) {
    port_options.native_layer_z = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        gsr_set_layer_z(mode);
    }
#endif
}

void port_gs_set_native_shine_exact(int exact) {
    port_options.native_shine_exact = exact;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        gsr_set_shine_exact(exact);
    }
#endif
}

int port_gs_set_native_msaa(int samples) {
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        samples = gsr_set_msaa(samples);
    }
#endif
    port_options.native_msaa = samples;
    return samples;
}

/* --native-widescreen: the widescreen picture is the GS's 640x448 shown at 16:9, its 3D drawn with a
   16:9 field of view and its 2D squeezed (above). The native GS draws it as it is, only with more
   pixels across than down: 4/3 of --native-scale, rounded up (1x: 2, 2x: 3, 3x: 4, exactly 4/3; 4x: 6),
   so that a GS pixel's width on the 16:9 screen is never more than one of the target's pixels'
   height, and the window shrinks the picture across instead of stretching it. Nothing in the game or
   the GS's coordinates changes: the targets are only drawn finer across. */
int port_gs_native_scale_x(void) {
    int s = port_gs_native_scale();

    if (port_options.native_widescreen && port_options.widescreen) {
        return (4 * s + 2) / 3;
    }
    return s;
}

/* --stats: what the native GS did */
void port_gs_print_stats(void) {
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native && gsr_pc_renderer) {
        pcr_print_stats(); /* (--renderer pc: its own; the lines below are the native GS's) */
        return;
    }
    if (have_native) {
        const GsnStats *s = &gsn_stats;

        gsn_wait();
        fprintf(stderr,
                "[stats] native GS: %llu draws, %llu triangles, %llu render passes, %llu shader blends, %llu target "
                "copies, %llu write-backs; %llu textures decoded (%llu texels), %llu found again; %llu uploads (%.1f MB, %llu found in VRAM already, %llu copied in whole; %llu pages known unchanged)\n",
                (unsigned long long)s->draws, (unsigned long long)s->primitives, (unsigned long long)s->passes,
                (unsigned long long)s->shader_blends, (unsigned long long)s->target_copies,
                (unsigned long long)s->writebacks, (unsigned long long)s->texture_decodes,
                (unsigned long long)s->texture_bytes, (unsigned long long)s->texture_reuses, (unsigned long long)s->uploads, (double)s->upload_bytes / 1048576.0,
                (unsigned long long)s->uploads_skipped,
                (unsigned long long)s->uploads_copied, (unsigned long long)s->upload_pages_held);
        fprintf(stderr,
                "[stats] native GS time: GIF %.3f s (draws %.3f, uploads %.3f: swizzle %.3f, targets %.3f; "
                "write-backs %.3f, decodes %.3f), vsync %.3f s\n",
                s->ns_gif / 1e9, s->ns_flush / 1e9, s->ns_upload / 1e9, s->ns_swizzle / 1e9, s->ns_sync / 1e9,
                s->ns_writeback / 1e9, s->ns_decode / 1e9, s->ns_vsync / 1e9);
        fprintf(stderr, "[stats] native GS draws: %.3f s working out their state, %.3f s recording them\n",
                s->ns_draw_setup / 1e9, s->ns_draw_record / 1e9);
        fprintf(stderr, "[stats] native GS back end: %.3f s playing the calls into Vulkan; %llu targets loaded before "
                "the render pass under way (not ending it); %llu target loads scaled on the GPU\n", s->ns_backend / 1e9,
                (unsigned long long)s->loads_before_pass, (unsigned long long)s->loads_scaled);
        if (s->images_let_go || s->images_untransitioned) {
            fprintf(stderr, "[stats] native GS images: %llu handed to the back end's stream to let go (the back end's "
                    "thread drops their last reference); %llu made without Granite's layout change (UNDEFINED, the change "
                    "in the stream)\n",
                    (unsigned long long)s->images_let_go, (unsigned long long)s->images_untransitioned);
        }
        if (interp_behind || interp_vblanks_fewer) {
            fprintf(stderr, "[stats] pictures between left out as the renderer was behind the game: %llu; cut to fewer "
                    "a vblank %llu times: %llu vblanks drawn with fewer (%llu with none)\n", (unsigned long long)interp_behind,
                    (unsigned long long)interp_cuts, (unsigned long long)interp_vblanks_fewer,
                    (unsigned long long)interp_vblanks_left_out);
        }
        if (s->interp_records || s->interp_replays || s->interp_misses) {
            fprintf(stderr,
                    "[stats] native GS pictures between on the GPU: %llu drawn from the packets (%.3f s, %.1f us each), "
                    "%llu from the recording (%.3f s, %.1f us each), %llu of no frame; drawn from the packets again as "
                    "the GS was not as recorded: %llu (targets), %llu (registers), %llu (VRAM or the CLUT written), %llu (the "
                    "recording could not be made again); %llu submits while recording; replays with a target the recording "
                    "made: %llu kept, %llu its own; %llu with a multisampled target's images made the same first\n",
                    (unsigned long long)s->interp_records, s->ns_interp_record / 1e9,
                    s->interp_records ? s->ns_interp_record / 1e3 / (double)s->interp_records : 0.0,
                    (unsigned long long)s->interp_replays, s->ns_interp_replay / 1e9,
                    s->interp_replays ? s->ns_interp_replay / 1e3 / (double)s->interp_replays : 0.0,
                    (unsigned long long)s->interp_misses, (unsigned long long)s->interp_rerecord[1],
                    (unsigned long long)s->interp_rerecord[2], (unsigned long long)s->interp_rerecord[5],
                    (unsigned long long)s->interp_rerecord[4], (unsigned long long)s->interp_mid_submits,
                    (unsigned long long)s->interp_kept_targets, (unsigned long long)s->interp_own_targets,
                    (unsigned long long)s->interp_newer_settled);
        }
        if (s->pipeline_binds) {
            fprintf(stderr, "[stats] native GS pipelines bound: %llu for %llu draws; pictures between: %.1f a recording, "
                    "%.1f a replay (for %.1f draws)\n", (unsigned long long)s->pipeline_binds, (unsigned long long)s->draws,
                    s->interp_records ? s->interp_record_binds / (double)s->interp_records : 0.0,
                    s->interp_replays ? s->interp_replay_binds / (double)s->interp_replays : 0.0,
                    s->interp_replays ? s->interp_replay_draws / (double)s->interp_replays : 0.0);
        }
        if (s->replay_uploads || s->replay_misses) {
            fprintf(stderr, "[stats] native GS: %llu uploads asked for again by the pictures between, %llu not found\n",
                    (unsigned long long)s->replay_uploads, (unsigned long long)s->replay_misses);
        }
        if (s->ms_resolves || s->ms_loads) {
            fprintf(stderr,
                    "[stats] native GS MSAA: %llu resolves, %llu loads into the multisampled images, %llu cut-out "
                    "draws shaded at every sample\n",
                    (unsigned long long)s->ms_resolves, (unsigned long long)s->ms_loads,
                    (unsigned long long)s->cutout_draws);
        }
        if (s->shadows) {
            fprintf(stderr, "[stats] native GS: %llu write-backs put off: %llu never owed, %llu pages' made later; %llu textures read "
                            "over pages owed without them (all they read written since)\n",
                    (unsigned long long)s->shadows, (unsigned long long)s->shadows_gone, (unsigned long long)s->shadows_resolved,
                    (unsigned long long)s->shadow_reads_covered);
        }
        if (s->writebacks_overwritten) {
            fprintf(stderr, "[stats] native GS: %llu write-backs not made: the targets under a copy of the frame\n",
                    (unsigned long long)s->writebacks_overwritten);
        }
        if (s->silhouettes_whole) {
            fprintf(stderr, "[stats] native GS: %llu silhouettes drawn whole\n", (unsigned long long)s->silhouettes_whole);
        }
        if (s->layer_draws) {
            fprintf(stderr, "[stats] native GS: %llu batches with layers (--native-layer-z tolerant)\n",
                    (unsigned long long)s->layer_draws);
        }
        if (port_options.native_draw) {
            fprintf(stderr,
                    "[stats] native draw: %llu draws queued (%llu screen passes), %llu drawn (%llu batches), %llu lost, %llu not drawn yet; "
                    "%llu meshes parsed (%llu refused), %llu found again, %.1f MB hashed; %llu meshes put on the GPU; "
                    "%llu PS2-path writes left out; %llu models declined (drawn the PS2 way); the plan %.3f s, "
                    "%llu vertices (%llu found planned); pictures between: %llu model parts and %llu strips moved "
                    "between their frames\n",
                    (unsigned long long)rd_stats.draws, (unsigned long long)rd_stats.passes, (unsigned long long)s->nd_draws,
                    (unsigned long long)s->nd_batches, (unsigned long long)s->nd_lost,
                    (unsigned long long)s->nd_unsupported, (unsigned long long)rd_stats.meshes_parsed,
                    (unsigned long long)rd_stats.mesh_unsupported, (unsigned long long)rd_stats.mesh_hits,
                    rd_stats.hash_bytes / 1048576.0, (unsigned long long)s->nd_mesh_uploads,
                    (unsigned long long)s->nd_skipped, (unsigned long long)rd_stats.declined, s->ns_nd_plan / 1e9,
                    (unsigned long long)s->nd_plan_vertices, (unsigned long long)s->nd_plan_reused,
                    (unsigned long long)s->nd_moved_models, (unsigned long long)s->nd_moved_strips);
        }
        if (port_options.renderer_modern || port_options.native_draw) {
            gsn_print_standalone_stats(); /* (P8; and native draw's for the pictures between) */
        }
        gsn_print_pipeline_stats();
        {
            int w;
            double pictures = s->pictures ? (double)s->pictures : 1.0;

            fprintf(stderr, "[stats] native GS: %llu pictures; a picture: %.1f render passes, %.1f resolves, "
                            "%.1f MB loaded and %.1f MB stored by passes, %.1f MB resolved, %.1f MB copied for blending\n",
                    (unsigned long long)s->pictures, (double)s->passes / pictures, (double)s->ms_resolves / pictures,
                    (double)s->pass_load_bytes / pictures / 1048576.0, (double)s->pass_store_bytes / pictures / 1048576.0,
                    (double)s->resolve_bytes / pictures / 1048576.0, (double)s->copy_bytes / pictures / 1048576.0);
            if (s->blend_copies_in_pass) {
                fprintf(stderr, "[stats] native GS: %llu copies for shader blending made in the render pass\n",
                        (unsigned long long)s->blend_copies_in_pass);
            }
            fprintf(stderr, "[stats] native GS render passes ended by:");
            for (w = 0; w < GSN_WHY_COUNT; w++) {
                if (s->pass_ends[w]) {
                    fprintf(stderr, " %s %llu;", gsn_why_name(w), (unsigned long long)s->pass_ends[w]);
                }
            }
            fprintf(stderr, "\n[stats] native GS resolves for:");
            for (w = 0; w < GSN_WHY_COUNT; w++) {
                if (s->resolves_by[w]) {
                    fprintf(stderr, " %s %llu;", gsn_why_name(w), (unsigned long long)s->resolves_by[w]);
                }
            }
            fprintf(stderr, "\n");
            gsn_print_gpu_times();
        }
        fprintf(stderr, "[stats] native GS holds: %llu targets, %llu textures, %llu decoded images (%llu MB), %llu uploads\n",
                (unsigned long long)s->targets, (unsigned long long)s->textures, (unsigned long long)s->pool_images,
                (unsigned long long)s->pool_mb, (unsigned long long)s->upload_copies);
    }
#endif
}

/* The GS's state as capture records (port/debug/gs_capture.c): paraLLEl-GS's, or the native GS's */
void port_gs_state_records(void (*emit)(uint32_t type, const void *data, size_t size)) {
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_gs) {
        port_gs_backend_capture(emit);
    } else if (have_native) {
        gsr_capture(emit);
    }
#else
    (void)emit;
#endif
}

void port_gs_finish(void) {
    while (backend && port_gs_vsync_pending()) {
        port_dmac_wake();
    }
}

int port_gs_replay_uploads(void) {
    return have_native && !have_gs && getenv("KF4_INTERP_SEND_UPLOADS") == NULL;
}

int port_gs_interp_gpu(void) {
    return port_gs_replay_uploads();
}

void port_gs_interp_frame(void *frame) {
#ifdef KF4_HAVE_PARALLEL_GS
    gsr_interp_frame((GsnInterpFrame *)frame);
#else
    (void)frame;
#endif
}

void port_gs_interp_picture(const void *picture) {
#ifdef KF4_HAVE_PARALLEL_GS
    gsr_interp_picture((const GsnInterpPicture *)picture);
#else
    (void)picture;
#endif
}

int port_gs_shaders_building(int *done, int *total) {
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native) {
        return gsr_pipelines_building(done, total);
    }
#endif
    *done = *total = 0;
    return 0;
}

int port_gs_shaders_running(void) {
#ifdef KF4_HAVE_PARALLEL_GS
    return have_native && gsr_pipelines_running();
#else
    return 0;
#endif
}

int port_gs_vsync_pending(void) {
    return __atomic_load_n(&queue_head, __ATOMIC_ACQUIRE) != __atomic_load_n(&queue_tail, __ATOMIC_RELAXED);
}

#ifdef KF4_HAVE_PARALLEL_GS
static int interp_on;           /* --frame-rate above 30 was on at the last vblank */
#endif

void port_gs_run_vsync(void) {
    uint64_t tail = __atomic_load_n(&queue_tail, __ATOMIC_RELAXED);
    uint64_t start = port_options.stats ? port_ticks() : 0;
#ifdef KF4_HAVE_PARALLEL_GS
    VsyncRequest *request = &queue[tail % VSYNC_QUEUE];
    int dump = 0;
    int shown = !port_options.headless || port_video_simulated();
    /* --frame-rate: the times in this vblank of the pictures shown (port_interp_schedule), whether a game
       frame is being drawn between (interp_live), and whether the vblank's own picture is one */
    double interp_phases[16];
    int interp_count = 0, interp_live = 0, own_shown = 1, interp_allow_now = INTERP_ALL, interp_between = 0;

    while (dump_next < dump_count && dump_at[dump_next] < request->vblank) {
        dump_next++;
    }
    if (dump_next < dump_count && dump_at[dump_next] == request->vblank) {
        dump = 1;
    }
    if (frame.rgba == NULL) {
        frame.rgba = malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS);
        native_frame.rgba = malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS);
    }
#ifdef KF4_HAVE_SDL
    if (port_options.dump_present_width > 0 && have_native) {
        /* --dump-present WxH: the pictures scaled as a window of that size shows them (the best scaling;
           the others are the presenter's blit) */
        int x, y, w, h;

        port_video_fit(port_options.dump_present_width, port_options.dump_present_height, &x, &y, &w, &h);
        gsr_set_present_scale(w, h, port_video_sharpen());
    }
#endif
    if (port_frame_interp()) {
        /* the game's frame drawn again into the buffer shown, for the pictures between: at this vblank's
           start when the vblank's own picture is one (the second vblank of a game frame), and after it
           (frame rates above 60) */
        int idle = port_gs_path_idle(1) && port_gs_path_idle(2) && port_gs_path_idle(3);
        /* the pictures between are the native GS's (paraLLEl-GS is the reference, at the vblanks only),
           whole frames (not read a field at a time) */
        int extras = have_native && (!have_gs || port_options.renderer_shown == PORT_RENDERER_NATIVE) &&
                     port_whole_frame();

        interp_count = extras ? port_interp_schedule(request->vblank, interp_phases, 16) : 1;
        if (!extras) {
            interp_phases[0] = 0;
        }
        if (idle) {
            port_interp_vsync(request->privileged);
        }
        port_interp_reset(idle);
        interp_live = idle && port_interp_active();
        interp_behind_run = interp_live && !dump && !port_options.fast && shown && have_native && interp_cut_on() &&
                                    gsr_backlog() > 1
                                ? interp_behind_run + 1
                                : 0;
        {
            /* The native GS's thread a vblank behind as this vblank and the one before began: one picture
               between fewer a vblank (down to none: the game's own pictures, as at 30), so that a place whose
               pictures between cost more than the thread has keeps the game's rate and as many pictures as fit,
               not a cliff to 60. Up to half a second to catch up after a cut; one more picture after interp_rest
               vblanks without one, and when that falls behind again within half a second, after twice as long
               (up to 2 s). A quick load or teleport ends any cut, and falling behind is not judged for 2 s after
               one, nor while the F1 overlay is open (its pictures are shown from the game's thread) and half a
               second after. Only F1: the minimap and the stats mod's lines keep the overlay on screen through
               play, and judging nothing then would leave no cut at all. Seen in --stats and on the F3 counter ("CUT"). */
            int between = 0, i;

            for (i = 0; i < interp_count; i++) {
                between += interp_phases[i] > 0;
            }
            if (__atomic_exchange_n(&interp_cut_reset_asked, 0, __ATOMIC_ACQ_REL)) {
                interp_allow = INTERP_ALL;
                interp_rest = 30;
                interp_raised_at = 0;
                interp_judge_from = request->vblank + INTERP_LOAD_GRACE;
            }
            if (port_overlay_open() && interp_judge_from < request->vblank + 30) {
                interp_judge_from = request->vblank + 30;
            }
            if (interp_behind_run == 0) {
                interp_settle_until = 0; /* (caught up since the last cut: the next fall behind is judged at once) */
            }
            /* --frame-rate unlocked: the rate follows the renderer (frame_interp.c), judged as the cut is, and behind
               too when pictures between of the vblank before were left out for it (below); the cut only once the
               rate is as low as it goes */
            static uint64_t unlocked_left_seen;
            int unlocked = port_options.frame_rate == PORT_FRAME_RATE_UNLOCKED;
            if (unlocked && interp_live && !dump && request->vblank >= interp_judge_from) {
                port_interp_unlocked_vblank(request->vblank, interp_behind_run > 0 || interp_behind != unlocked_left_seen);
            }
            unlocked_left_seen = interp_behind;
            if (interp_behind_run >= 2 && request->vblank >= interp_settle_until && request->vblank >= interp_judge_from &&
                interp_allow > 0 && (!unlocked || port_interp_unlocked_at_floor())) {
                interp_allow = (interp_allow < between ? interp_allow : between) - 1;
                if (interp_allow < 0) {
                    interp_allow = 0;
                }
                if (interp_raised_at && request->vblank < interp_raised_at + 30 && interp_rest < INTERP_REST_MAX) {
                    interp_rest *= 2;
                }
                interp_settle_until = request->vblank + 30; /* (to catch up before judging again: at most half a second) */
                interp_next_raise = request->vblank + interp_rest;
                interp_last_cut = request->vblank;
                interp_cuts++;
            } else if (interp_allow < INTERP_ALL && request->vblank >= interp_next_raise && interp_behind_run == 0) {
                if (interp_last_cut < interp_raised_at) {
                    interp_rest = 30; /* (the raise before held) */
                }
                interp_allow = interp_allow + 1 >= between ? INTERP_ALL : interp_allow + 1;
                interp_raised_at = request->vblank;
                interp_next_raise = request->vblank + interp_rest;
            }
            interp_allow_now = interp_allow < between ? interp_allow : between;
            __atomic_store_n(&interp_cut_now, interp_live ? between - interp_allow_now : 0, __ATOMIC_RELAXED);
            if (interp_live && !dump && interp_allow_now < between) {
                interp_vblanks_fewer++;
            }
            if (interp_live && !dump && interp_allow_now == 0 && between > 0) {
                interp_live = 0;
                interp_vblanks_left_out++;
            }
            interp_between = between;
        }
        /* the vblank's own picture: shown when it is one of the pictures (or there is no picture between) */
        own_shown = interp_phases[0] == 0 || !interp_live;
        if (interp_live && (own_shown || dump)) {
            port_interp_picture(0, replay_gif);
        }
        interp_on = 1;
    } else {
        __atomic_store_n(&interp_cut_now, 0, __ATOMIC_RELAXED);
        if (interp_on) {
            port_interp_forget(); /* turned off (the overlay): nothing kept for when it is on again */
            interp_on = 0;
        }
    }
    if (port_whole_frame()) {
        /* a screen drawn as fields (the menus): the picture just drawn drawn again into the buffer shown,
           for the other field, and given to the renderers as that field first, so that the vblank's
           woven picture is one moment (port/platform/field_frames.c) */
        int idle = port_gs_path_idle(1) && port_gs_path_idle(2) && port_gs_path_idle(3);
        uint32_t drawn;

        if (idle && port_field_frames_vsync(request->privileged, replay_gif, &drawn)) {
            uint64_t other[0xF0 / 8];
            int i;

            memcpy(other, request->privileged, sizeof(other));
            for (i = 0x70 / 8; i <= 0x90 / 8; i += 0x20 / 8) {
                other[i] = (other[i] & ~(uint64_t)0x1FF) | drawn; /* DISPFB1 and DISPFB2 */
            }
            if (have_gs) {
                port_gs_backend_vsync(other, request->field ^ 1, 0, &frame);
            }
            if (have_native) {
                gsr_vsync(other, request->field ^ 1, 0, NULL);
            }
        }
        port_field_frames_reset(idle);
    } else {
        port_field_frames_reset(0);
    }
    if (have_native) {
        /* (widescreen and --native-widescreen change live: the scale follows, from this vblank's end) */
        gsr_set_scale(port_gs_native_scale_x(), port_gs_native_scale());
    }
    if (present_frames(request, shown && own_shown, dump)) {
    } else if (dump) {
        fprintf(stderr, "[gs] vblank %llu: nothing is shown, no picture to dump\n", (unsigned long long)request->vblank);
    }
    if (interp_live && interp_count > 0) {
        /* the pictures between this vblank and the next: for the window, only while this thread and the
           native GS's keep up with the game (a picture between left out rather than the game held up) */
        int i, j = 0, more = shown || draw_all();

        for (i = 0; i < interp_count; i++) {
            if (interp_phases[i] <= 0 || !(more || dump)) {
                continue;
            }
            if (!dump && interp_allow_now < interp_between) {
                /* the cut: interp_allow_now of the vblank's pictures between, spread over it (the k-th at
                   (k + 0.5) / allowed of the way) */
                int k = j++, keep = 0, a;

                for (a = 0; a < interp_allow_now; a++) {
                    keep |= (int)((a + 0.5) * interp_between / interp_allow_now) == k;
                }
                if (!keep) {
                    continue;
                }
            }
            if (!dump && !port_options.fast && shown &&
                (__atomic_load_n(&queue_head, __ATOMIC_ACQUIRE) - tail > 1 || gsr_backlog() > 1)) {
                interp_behind++;
                continue;
            }
            if (port_interp_picture(interp_phases[i], replay_gif)) {
                present_extra(request, interp_phases[i], more, dump);
            }
        }
        port_interp_end_vblank(replay_gif);
    }
    if (port_gs_capture_armed) {
        port_gs_capture_vsync(request->privileged, request->field, request->vblank);
    }
#endif
    if (port_options.stats) {
        port_stats.vsync_ticks += port_ticks() - start;
    }
    __atomic_store_n(&queue_tail, tail + 1, __ATOMIC_RELEASE);
}

/* ---- Save states -------------------------------------------------------------------------------
   On the controller's thread, between GIF packets: the paths' places, and the GS's VRAM, registers
   and recent fields when there is a renderer, after their size (a state taken without one has none,
   and a run without one skips them). */
const char *port_gs_state_busy(void) {
    int path;

    for (path = 1; path < 4; path++) {
        if (gif_paths[path].in_packet) {
            return "a GIF packet is half sent";
        }
    }
    if (image_bits_left != 0) {
        return "an image transfer to the GS is half sent";
    }
    return NULL;
}

/* A renderer's part of a state saved by another (the state has the other's and not this one's): its
   VRAM, which both put first, so that the textures are there; the registers come back as the game
   next writes them */
static void take_other_vram(PortStateIo *io, uint64_t bytes, int into_native) {
    void *vram = bytes >= 4u << 20 ? malloc(4u << 20) : NULL;

    if (vram == NULL) {
        port_state_bytes(io, NULL, (size_t)bytes);
        return;
    }
    port_state_bytes(io, vram, 4u << 20);
    port_state_bytes(io, NULL, (size_t)(bytes - (4u << 20)));
#ifdef KF4_HAVE_PARALLEL_GS
    if (into_native) {
        gsr_restore(KF4GS_VRAM, vram, 4u << 20);
    } else {
        port_gs_backend_restore(KF4GS_VRAM, vram, 4u << 20);
    }
#else
    (void)into_native;
#endif
    free(vram);
}

void port_gs_state(PortStateIo *io) {
    uint64_t backend_bytes = 0;
    uint64_t native_bytes = 0;
    uint64_t table_bytes = 0;
    int native_loaded = 0;

    (void)native_loaded;

    if (io->loading) {
        port_interp_forget(); /* (--frame-rate 60) the packets kept are not this state's */
        port_field_frames_reset(0); /* (whole frames) nor are these */
    }
    PORT_STATE(io, gif_paths);
    PORT_STATE(io, draw_offset_x);
    PORT_STATE(io, frame_pictures);
    PORT_STATE(io, frame_picture_next);
    PORT_STATE(io, image_bitbltbuf);
    PORT_STATE(io, image_trxreg);
    PORT_STATE(io, image_bits_left);
    /* paraLLEl-GS's state, then the native GS's: each only when it runs; a state loaded into the other
       renderer gives it the VRAM (take_other_vram) */
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_gs) {
        backend_bytes = port_gs_backend_state_size();
    }
    if (have_native) {
        native_bytes = gsr_state_size();
    }
#endif
    PORT_STATE(io, backend_bytes);
    if (backend_bytes != 0) {
#ifdef KF4_HAVE_PARALLEL_GS
        if (have_gs) {
            port_gs_backend_state(io);
        } else if (have_native && io->loading) {
            take_other_vram(io, backend_bytes, 1);
            backend_bytes = 0; /* (the native GS's own, if any, is read below) */
        } else
#endif
        {
            port_state_bytes(io, NULL, (size_t)backend_bytes);
        }
    }
    PORT_STATE(io, native_bytes);
    if (native_bytes != 0) {
#ifdef KF4_HAVE_PARALLEL_GS
        if (have_native && native_bytes == gsr_state_size()) {
            gsr_state(io);
            native_loaded = 1;
        } else if (have_gs && io->loading && !backend_bytes) {
            take_other_vram(io, native_bytes, 0);
        } else
#endif
        {
            port_state_bytes(io, NULL, (size_t)native_bytes);
        }
    }
    /* (--renderer modern) the native GS's page-ownership table, when the scene was on: kept when it is
       on again, so that its textures come from their uploads at once */
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_native && !io->loading) {
        table_bytes = gsr_table_state_size();
    }
#endif
    PORT_STATE(io, table_bytes);
    if (table_bytes != 0) {
#ifdef KF4_HAVE_PARALLEL_GS
        if (have_native && (!io->loading || native_loaded)) {
            gsr_table_state(io, (size_t)table_bytes);
        } else
#endif
        {
            port_state_bytes(io, NULL, (size_t)table_bytes);
        }
    }
}
