/* The GS: the GIF's three paths and the display, on the DMA controller's thread.

   Packets come from VU1's XGKICK (PATH1, port/platform/vif1.c), VIF1's DIRECT (PATH2) and DMA
   channel 2 (PATH3, port/platform/dmac.c), and go to the renderer: the PC renderer (port/gs/pc/, the
   default), or with --renderer gs paraLLEl-GS (port/gs/gs_parallel.cpp), the accurate GS emulator, which is
   for comparing against. At each vblank the kernel snapshots the display registers (PMODE..BGCOLOR in the
   register page) and queues them here; the controller's thread finishes the channels started before
   it, then has the renderer scan the frame out, and hands the picture to the window
   (port/platform/video.c) and to --dump-frames.

   Without a renderer (none starts, or a --headless run that neither dumps nor captures) the packets are read all
   the same, so VIF1 and VU1 run, and then dropped. What the game does never depends on it: the
   picture is all the GS gives back, apart from the textures sceGsExecStoreImage reads, which the
   game only sends to the GS again. */
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
#endif

#define VSYNC_QUEUE 16

typedef struct {
    uint64_t privileged[0xF0 / 8];
    uint64_t vblank;
    int field;
} VsyncRequest;

static int backend; /* a renderer is up */
static int have_gs; /* paraLLEl-GS is (--renderer gs) */
static int have_pc; /* the PC renderer is (the default) */
static VsyncRequest queue[VSYNC_QUEUE];
static volatile uint64_t queue_head; /* written by the game thread */
static volatile uint64_t queue_tail; /* written by the controller's thread */
static PortGsFrame frame;
static PortGsFrame pc_frame;     /* the PC renderer's picture, read back as pixels when it is wanted */
static uint64_t interp_behind;   /* --frame-rate: pictures between left out, the renderer behind the game */
static uint64_t interp_kept_one; /* and vblanks that kept one picture between all the same (port_gs_run_vsync) */
/* --frame-rate: the pictures between cut to fewer a vblank while the PC renderer's thread is behind
   (port_gs_run_vsync): interp_allow of them a vblank (INTERP_ALL: no cut), one fewer each time it falls
   behind, one more after interp_rest vblanks; vblanks drawn with fewer and with none; the cuts */
#define INTERP_ALL 16
#define INTERP_REST_MAX 120 /* the longest wait for one more picture: 2 s (it was 8 s, at 60 that long after a load) */
#define INTERP_LOAD_GRACE 120 /* vblanks after a quick load or teleport, and 30 after the overlay, not judged */
static uint64_t interp_vblanks_left_out, interp_vblanks_fewer, interp_cuts, interp_rest = 30;
static uint64_t interp_settle_until, interp_next_raise, interp_raised_at, interp_last_cut, interp_judge_from;
static int interp_behind_run, interp_allow = INTERP_ALL;
static int interp_left_last; /* pictures between the vblank before left out as behind (the cut judges on them too) */
static volatile int interp_cut_now; /* (the counter: pictures between a vblank left out by the cut, now) */
static volatile int interp_cut_reset_asked; /* (port_gs_interp_cut_reset: the game's thread) */

/* After a quick load or teleport: no cut, and the load's falling behind not judged */
void port_gs_interp_cut_reset(void) {
    __atomic_store_n(&interp_cut_reset_asked, 1, __ATOMIC_RELEASE);
}

/* KF4_INTERP_CUT=0 (measure): no cut, every picture between drawn however far behind the PC renderer falls (the
   game's thread then waits for it): what a place can keep up */
static int interp_cut_on(void) {
    static int on = -1;

    if (on < 0) {
        on = getenv("KF4_INTERP_CUT") == NULL || atoi(getenv("KF4_INTERP_CUT")) != 0;
    }
    return on;
}

/* KF4_INTERP_CUT_LEFT=0 (measure): the cut judges on the backlog alone, as before 2026-11-12 (not on the pictures
   between left out as behind) */
static int cut_on_left_out(void) {
    static int on = -1;

    if (on < 0) {
        on = getenv("KF4_INTERP_CUT_LEFT") == NULL || atoi(getenv("KF4_INTERP_CUT_LEFT")) != 0;
    }
    return on;
}

/* KF4_INTERP_KEEP_ONE=0 (measure): a vblank behind leaves every picture between out, as before 2026-11-12 */
static int keep_one_on(void) {
    static int on = -1;

    if (on < 0) {
        on = getenv("KF4_INTERP_KEEP_ONE") == NULL || atoi(getenv("KF4_INTERP_KEEP_ONE")) != 0;
    }
    return on;
}

int port_interp_cut(void) {
    return __atomic_load_n(&interp_cut_now, __ATOMIC_RELAXED);
}

/* The last vblank whose pictures between were cut or left out as behind (port_interp_behind_lately) */
static uint64_t interp_behind_at;

/* Any thread: the renderer behind the game lately (the cut lowering the count now, or a picture between left out as
   behind in the last half second): the PC renderer's latency levers engage only then (pcr.cpp), so that a rate it
   keeps up with (240 on the machines measured) draws as before them. KF4_PC_LATENCY_GATE=0: always. */
int port_interp_behind_lately(void) {
    static int gate = -1;
    uint64_t at;

    if (gate < 0) {
        const char *e = getenv("KF4_PC_LATENCY_GATE");
        gate = e == NULL || strcmp(e, "0") != 0;
    }
    if (!gate || port_interp_cut() > 0) {
        return 1;
    }
    at = __atomic_load_n(&interp_behind_at, __ATOMIC_RELAXED);
    return at != 0 && port_vblank_count() < at + 30;
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

#ifdef KF4_HAVE_PARALLEL_GS
/* The PC renderer on a Vulkan device of its own, with the window's swapchain on it for --presenter vulkan
   (port/platform/present_vk.cpp). 1 when it started; without a device it can use, it does not, and nothing
   is drawn. */
static int open_pc_renderer(void) {
    char path[1100];

    if (port_options.presenter == PORT_PRESENTER_VULKAN && !port_options.headless) {
        gsr_enable_present();
    }
    /* (before it opens: its pipelines are made through the kept cache from the first draw on) */
    snprintf(path, sizeof(path), "%s/pc_pipelines.bin", port_data_dir());
    gsr_pipeline_cache(path);
    if (!gsr_open(NULL)) {
        fprintf(stderr, "[gs] the PC renderer did not start (no Vulkan device it can use)\n");
        return 0;
    }
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
    gsr_set_native_draw(GSN_NATIVE_DRAW_NATIVE);
    gsr_set_presenter(port_video_submit, port_video_submit_image);
    return 1;
}
#endif

static void open_backend(void *unused) {
    (void)unused;
#ifdef KF4_HAVE_PARALLEL_GS
    if (port_options.renderer == PORT_RENDERER_GS) {
        have_gs = port_gs_backend_open();
    } else {
        have_pc = open_pc_renderer();
    }
    if (!have_pc) {
        port_options.native_draw = 0; /* (the native draws are the PC renderer's: nothing else draws them) */
    } else if (port_options.native_draw) {
        /* the PC renderer draws the kinds it has: the others go the PS2 path's way, as packets */
        if (port_options.native_draw & ~pcr_native_kinds()) {
            fprintf(stderr, "[gs] --native-draw: the PC renderer draws kinds 0x%x of 0x%x natively, the rest as packets\n",
                    port_options.native_draw & pcr_native_kinds(), port_options.native_draw);
        }
        port_options.native_draw &= pcr_native_kinds();
    }
    backend = have_gs || have_pc;
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
    if (have_pc) {
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
    if (have_gs) {
        port_gs_backend_read_fifo(dst, qwords);
        return;
    }
    if (have_pc) {
        gsr_read_fifo(dst, qwords);
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
    if (have_pc) {
        gsr_gif(path, data, bytes);
    }
}

/* With SDL_Renderer and the PC renderer alone, the best scaling (or the sharpening): the PC renderer's
   picture at the window's size, read back (GSN_WANT_SCALED), for SDL_Renderer to draw a pixel a pixel */
static int sdl_scaled(void) {
    if (port_video_takes_images() || !have_pc || port_options.headless ||
        (port_video_scaling() != PORT_SCALING_BEST && port_video_sharpen() <= 0)) {
        return 0;
    }
    return GSN_WANT_IMAGE | GSN_WANT_SCALED;
}

/* The size the window shows the picture at, for the PC renderer's scaling (video.c's SDL_Renderer) */
void port_gs_present_scale(int width, int height, float sharpen) {
    if (have_pc) {
        gsr_set_present_scale(width, height, sharpen);
    }
}

/* The window's and --dump-frames' picture of this vblank: 1 if one is shown. With the Vulkan presenter the PC
   renderer's picture stays on the GPU (GSN_WANT_IMAGE), and is read back only for a dump. */
static int present_frames(VsyncRequest *request, int shown, int dump) {
    uint64_t tag = PORT_PICTURE_TAG(request->vblank, 0);
    int want = shown || dump;
    int want_pc = shown && port_video_takes_images() ? GSN_WANT_IMAGE | (dump ? GSN_WANT_PIXELS : 0)
                  : want                             ? GSN_WANT_PIXELS
                                                     : 0;
    int got;

    if (dump && port_options.dump_present_width > 0) {
        want_pc |= GSN_WANT_IMAGE | GSN_WANT_PIXELS | GSN_WANT_SCALED; /* (--dump-present) */
    }
    if (have_gs) {
        if (!port_gs_backend_vsync(request->privileged, request->field, want, &frame)) {
            return 0;
        }
        if (dump) {
            port_video_dump(&frame, request->vblank);
        }
        if (shown) {
            port_video_submit(&frame, tag);
        }
        return 1;
    }
    if (!have_pc) {
        return 0;
    }
    if (shown && !dump) {
        /* the PC renderer's thread hands the window the picture (or with the Vulkan presenter word of the new
           image) when it has it, and this thread goes on */
        return gsr_picture(request->privileged, request->field,
                           GSN_PRESENT | (port_video_takes_images() ? GSN_WANT_IMAGE : 0) | sdl_scaled(), NULL,
                           tag) != 0;
    }
    got = gsr_picture(request->privileged, request->field, want_pc, &pc_frame, tag);
    if (got & GSN_WANT_IMAGE) {
        if (dump && (got & GSN_WANT_PIXELS)) {
            port_video_dump(&pc_frame, request->vblank);
        }
        port_video_submit_image(tag);
        return 1;
    }
    if (!got) {
        return 0;
    }
    if (dump) {
        port_video_dump(&pc_frame, request->vblank);
    }
    if (shown) {
        port_video_submit(&pc_frame, tag);
    }
    return 1;
}

/* --frame-rate above 60: a picture between two vblanks (phase of the way from this one to the next),
   drawn into the buffer shown: the PC renderer's, read again whole (pcr_picture with GSN_EXTRA), for the
   window (shown) and as frame_<vblank>_<phase in thousandths>.png (dump) */
static void present_extra(VsyncRequest *request, double phase, int shown, int dump) {
    uint64_t tag = PORT_PICTURE_TAG(request->vblank, phase);
    int images = shown && (port_video_takes_images() || port_options.headless);

    if (dump) {
        int got = gsr_picture(request->privileged, request->field,
                              GSN_EXTRA | GSN_WANT_PIXELS | (images || port_options.dump_present_width > 0 ? GSN_WANT_IMAGE : 0) |
                                  (port_options.dump_present_width > 0 ? GSN_WANT_SCALED : 0),
                              &pc_frame, tag);
        char name[64];

        if (got & GSN_WANT_PIXELS) {
            snprintf(name, sizeof(name), "frame_%06llu_%03d.png", (unsigned long long)request->vblank,
                     (int)(phase * 1000.0 + 0.5));
            port_video_dump_named(&pc_frame, name);
            if (shown) {
                if (images) {
                    port_video_submit_image(tag);
                } else {
                    port_video_submit(&pc_frame, tag);
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
    if (port_gs_capture_armed) {
        port_gs_capture_note_vblank(request->vblank);
    }
    __atomic_store_n(&queue_head, head + 1, __ATOMIC_RELEASE);
    port_dmac_post_vsync();
}

void port_gs_set_native_scale(int scale) {
    port_options.native_scale = scale;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
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
    if (have_pc) {
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
    if (have_pc) {
        port_options.native_aniso = gsr_set_aniso(taps);
    }
#endif
}

void port_gs_set_native_lod(int mode) {
    port_options.native_lod = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        gsr_set_lod(mode);
    }
#endif
}

void port_gs_set_native_2d(int mode) {
    port_options.native_2d = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        gsr_set_2d(mode);
    }
#endif
}

void port_gs_set_native_cutout_aa(int mode) {
    port_options.native_cutout_aa = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        gsr_set_cutout_aa(mode);
    }
#endif
}

void port_gs_set_native_colour(int mode) {
    port_options.native_colour = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        gsr_set_colour(mode);
    }
#endif
}

int port_gs_native_draws_now(void) {
    /* (the PC renderer moves them only in its own pictures between: frame_interp.c's CPU way cannot) */
    return port_frame_rate_hz() <= 30 || port_interp_on_gpu();
}

void port_gs_set_native_draw(int kinds) {
    port_options.native_draw = 0;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        port_options.native_draw = kinds & pcr_native_kinds();
    }
#endif
}

int port_gs_native_kinds(void) {
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        return pcr_native_kinds();
    }
#endif
    return 0;
}

void port_gs_set_native_layer_z(int mode) {
    port_options.native_layer_z = mode;
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        gsr_set_layer_z(mode);
    }
#endif
}

int port_gs_set_native_msaa(int samples) {
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        samples = gsr_set_msaa(samples);
    }
#endif
    port_options.native_msaa = samples;
    return samples;
}

/* --native-widescreen: the widescreen picture is the GS's 640x448 shown at 16:9, its 3D drawn with a
   16:9 field of view and its 2D squeezed (above). The PC renderer draws it as it is, only with more
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

#ifdef KF4_HAVE_PARALLEL_GS
/* --stats: the pictures between this thread left out */
static void print_interp_left_out(void) {
    if (interp_behind || interp_vblanks_fewer) {
        fprintf(stderr, "[stats] pictures between left out as the renderer was behind the game: %llu; cut to fewer "
                "a vblank %llu times: %llu vblanks drawn with fewer (%llu with none); %llu vblanks behind kept one\n",
                (unsigned long long)interp_behind, (unsigned long long)interp_cuts,
                (unsigned long long)interp_vblanks_fewer, (unsigned long long)interp_vblanks_left_out,
                (unsigned long long)interp_kept_one);
    }
}
#endif

/* --stats: the PC renderer's, and the pictures between this thread left out */
void port_gs_print_stats(void) {
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc) {
        pcr_print_stats();
        print_interp_left_out();
    }
#endif
}

/* The GS's state as capture records (port/debug/gs_capture.c): paraLLEl-GS's, or the PC renderer's */
void port_gs_state_records(void (*emit)(uint32_t type, const void *data, size_t size)) {
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_gs) {
        port_gs_backend_capture(emit);
    } else if (have_pc) {
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
    return have_pc && getenv("KF4_INTERP_SEND_UPLOADS") == NULL;
}

int port_gs_interp_gpu(void) {
    /* (unless KF4_PC_BETWEEN=0: then the PC renderer takes the frame's packets again, moved, frame_interp.c's own
       way, and the game's packets in place of its native draws) */
    static int pc_between = -1;

    if (pc_between < 0) {
        const char *e = getenv("KF4_PC_BETWEEN");
        pc_between = e == NULL || strcmp(e, "0") != 0;
    }
    return port_gs_replay_uploads() && pc_between;
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
    if (have_pc) {
        return gsr_pipelines_building(done, total);
    }
#endif
    *done = *total = 0;
    return 0;
}

int port_gs_shaders_running(void) {
#ifdef KF4_HAVE_PARALLEL_GS
    return have_pc && gsr_pipelines_running();
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
        pc_frame.rgba = malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS);
    }
#ifdef KF4_HAVE_SDL
    if (port_options.dump_present_width > 0 && have_pc) {
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
        /* the pictures between are the PC renderer's (paraLLEl-GS draws the vblanks only), whole frames (not read a
           field at a time) */
        int extras = have_pc && port_whole_frame();

        interp_count = extras ? port_interp_schedule(request->vblank, interp_phases, 16) : 1;
        if (!extras) {
            interp_phases[0] = 0;
        }
        if (idle) {
            port_interp_vsync(request->privileged);
        }
        port_interp_reset(idle);
        interp_live = idle && port_interp_active();
        /* behind: the backlog over 1 as this vblank begins, or pictures between of the vblank before left out as
           behind. Left-outs keep the backlog low (a vblank's pictures left out are not drawn), so on the backlog
           alone the cut seldom lowered the count to what the renderer makes, and the pictures came in bursts and
           gaps (PERFORMANCE.md, 2026-11-12) */
        interp_behind_run = interp_live && !dump && !port_options.fast && shown && have_pc && interp_cut_on() &&
                                    (gsr_backlog() > 1 || (interp_left_last > 0 && cut_on_left_out()))
                                ? interp_behind_run + 1
                                : 0;
        interp_left_last = 0;
        {
            /* The PC renderer's thread a vblank behind as this vblank and the one before began: one picture
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
            if (interp_behind_run >= 2 && request->vblank >= interp_settle_until && request->vblank >= interp_judge_from &&
                interp_allow > 0) {
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
            port_interp_picture(request->vblank, 0, replay_gif);
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
            if (have_pc) {
                gsr_vsync(other, request->field ^ 1, 0, NULL);
            }
        }
        port_field_frames_reset(idle);
    } else {
        port_field_frames_reset(0);
    }
    if (have_pc) {
        /* (widescreen and --native-widescreen change live: the scale follows, from this vblank's end) */
        gsr_set_scale(port_gs_native_scale_x(), port_gs_native_scale());
    }
    if (present_frames(request, shown && own_shown, dump)) {
    } else if (dump) {
        fprintf(stderr, "[gs] vblank %llu: nothing is shown, no picture to dump\n", (unsigned long long)request->vblank);
    }
    if (interp_live && interp_count > 0) {
        /* the pictures between this vblank and the next: for the window, only while this thread and the
           PC renderer's keeps up with the game (a picture between left out rather than the game held up). A
           vblank whose own picture is not shown (above 60 its phases need not include 0) keeps at least one: when
           every picture between would be left out as behind, the last of them is drawn anyway. Leaving all out
           took the vblank off the screen (the game's speed seemed to drop to 57-58 vblanks a second uncapped,
           docs/port/PERFORMANCE.md), and it cleared the backlog before the cut above saw two vblanks behind. */
        int i, j = 0, more = shown || draw_all(), asked = shown && own_shown, last_left_out = -1;

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
                (__atomic_load_n(&queue_head, __ATOMIC_ACQUIRE) - tail > 1 || gsr_behind())) {
                interp_behind++;
                __atomic_store_n(&interp_behind_at, request->vblank, __ATOMIC_RELAXED);
                interp_left_last++;
                last_left_out = i;
                continue;
            }
            asked = 1;
            if (port_interp_picture(request->vblank, interp_phases[i], replay_gif)) {
                present_extra(request, interp_phases[i], more, dump);
            }
        }
        if (!asked && last_left_out >= 0 && keep_one_on()) {
            /* (no picture of this vblank after it: still in order) */
            interp_behind--;
            interp_kept_one++;
            if (port_interp_picture(request->vblank, interp_phases[last_left_out], replay_gif)) {
                present_extra(request, interp_phases[last_left_out], more, dump);
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
    /* paraLLEl-GS's state, then the PC renderer's: each only when it runs; a state loaded into the other
       renderer gives it the VRAM (take_other_vram) */
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_gs) {
        backend_bytes = port_gs_backend_state_size();
    }
    if (have_pc) {
        native_bytes = gsr_state_size();
    }
#endif
    PORT_STATE(io, backend_bytes);
    if (backend_bytes != 0) {
#ifdef KF4_HAVE_PARALLEL_GS
        if (have_gs) {
            port_gs_backend_state(io);
        } else if (have_pc && io->loading) {
            take_other_vram(io, backend_bytes, 1);
            backend_bytes = 0; /* (the PC renderer's own, if any, is read below) */
        } else
#endif
        {
            port_state_bytes(io, NULL, (size_t)backend_bytes);
        }
    }
    PORT_STATE(io, native_bytes);
    if (native_bytes != 0) {
#ifdef KF4_HAVE_PARALLEL_GS
        if (have_pc && native_bytes == gsr_state_size()) {
            gsr_state(io);
            native_loaded = 1;
        } else if (have_gs && io->loading && !backend_bytes) {
            take_other_vram(io, native_bytes, 0);
        } else if (have_pc && io->loading && native_bytes >= (4u << 20)) {
            /* a state from the native GS (since removed: an older build) in the PC renderer, or the other way: both begin with
               the GS's 4 MB */
            take_other_vram(io, native_bytes, 1);
        } else
#endif
        {
            port_state_bytes(io, NULL, (size_t)native_bytes);
        }
    }
    /* the renderer's page-ownership table, when it has one: kept when it is
       on again, so that its textures come from their uploads at once */
#ifdef KF4_HAVE_PARALLEL_GS
    if (have_pc && !io->loading) {
        table_bytes = gsr_table_state_size();
    }
#endif
    PORT_STATE(io, table_bytes);
    if (table_bytes != 0) {
#ifdef KF4_HAVE_PARALLEL_GS
        if (have_pc && (!io->loading || native_loaded)) {
            gsr_table_state(io, (size_t)table_bytes);
        } else
#endif
        {
            port_state_bytes(io, NULL, (size_t)table_bytes);
        }
    }
}
