/* The host side of the PC port: what port/platform provides to the rest of port/ (not to the game). */
#ifndef PORT_PLATFORM_H
#define PORT_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* ---- Start-up -------------------------------------------------------------------------------- */

/* Allocates game RAM and resets the scratchpad, register pages and VU0 (port/platform/memory.c). */
void port_memory_init(void);

/* Gives the program's sections their EE addresses (port/platform/ee_address.c). */
void port_address_init(void);

/* The game's main (src/game/main.c, renamed by the build). */
int kf4_game_main(void);

/* What the command line asked for (port/platform/main_host.c). */
typedef struct {
    const char *disc;        /* --disc: the disc image (.iso) or the extracted disc's folder */
    const char *input;       /* --input: a pad script (port/sdk/libpad.c) instead of SDL */
    const char *log;         /* --log: the per-frame state log ("-" for stderr) */
    const char *host_dir;    /* --host-dir: where host0: files go (default: the current directory) */
    long long frames;        /* --frames: stop after this many vblanks (0: run on) */
    int fast;                /* --fast: do not hold the game to 59.94 vblanks a second */
    int headless;            /* --headless: no SDL at all, and no window */
    int no_gs;               /* --no-gs: draw nothing (the packets are still read) */
    /* --renderer: who draws (port/platform/gs.c). PORT_RENDERER_NATIVE, step 3's (port/gs/native/),
       is the default (paraLLEl-GS instead if it cannot start: the renderers that did start are
       written back here); PORT_RENDERER_GS is paraLLEl-GS, the reference; PORT_RENDERER_BOTH runs
       the two side by side and compares their pictures, showing renderer_shown's. */
    int renderer;
    int renderer_shown;
    int renderer_forced;     /* --renderer: this run's, whatever settings.json says */
    int renderer_heatmap;    /* with both: the window shows how the two differ (--renderer-shown heatmap) */
    /* --renderer modern (P8, docs/port/NATIVE_DRAW_P8.md): the native GS, with the scene's native draws (map
       records and items: --native-draw map,sky,items unless it says otherwise) drawn into the scene's own
       targets (gsn_set_standalone); or settings.json's "renderer": "modern" */
    int renderer_modern;
    /* --renderer fast: modern, with the PC renderer's parts built so far (docs/port/PC_RENDERER.md) */
    int renderer_fast;
    /* --renderer pc (its development name; docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native
       GS"): the PC renderer (port/gs/pc/) opened in place of the native GS, renderer PORT_RENDERER_NATIVE */
    int renderer_pc;
    /* --scene-scale, --scene-msaa (P8 T2): with --renderer modern, the scene's own scale (1 to 4; with
       --native-widescreen and widescreen, 4/3 of it across) and samples (1, 2, 4, 8), apart from the GS's
       (--native-scale, --native-msaa), which then draws the 2D, the menus and the title alone; 0: the GS's */
    int scene_scale;
    int scene_msaa;
    /* --upscaler (docs/port/UPSCALING.md): with --renderer modern and a --scene-scale below the GS's, the scene
       brought up to the GS's scale at the composite, the 2D the GS's at its own scale (0: off, the composite at the
       scene's scale; 1: bilinear; 2: FSR 3.1; 3: DLSS) */
    int upscaler;
    /* --native-shine: with --renderer modern, a skinned model's sphere-mapped shine blended in the shader
       (the ice creatures on map 017: Cs + Cd As, As above 0x80) drawn whole, the default (0), or in runs of
       triangles that do not overlap (1, exact): where a part folds over its own edge the GS blends both
       triangles there, drawn whole one is (docs/port/NATIVE_DRAW_P8.md, "The icy corridor"). Live. */
    int native_shine_exact;
    int native_shine_exact_forced;
    /* --presenter: how the picture reaches the window (port/platform/video.c). PORT_PRESENTER_VULKAN,
       the default, a swapchain on the native GS's own device (port/platform/present_vk.cpp), with the
       native GS's picture taken from the GPU as it is; PORT_PRESENTER_SDL through SDL_Renderer (the
       window's when the native GS is not running or the Vulkan one does not start). */
    int presenter;
    int presenter_forced;    /* --presenter: this run's, whatever settings.json says */
    int native_scale;        /* --native-scale: the native GS's render targets, 1 to 4 times the PS2's size,
                                or 0, auto (port_gs_native_scale()) */
    int native_scale_forced;
    /* --native-widescreen: with widescreen on and the native GS, the render targets wider still
       (port_gs_native_scale_x(), port/platform/gs.c): the 16:9 picture drawn with square pixels
       instead of 640 columns stretched. Off by default; live from the overlay. */
    int native_widescreen;
    int native_widescreen_forced;
    /* --native-msaa: the native GS draws the 3D with this many samples a pixel (1, the default: none,
       the PS2's picture; 2, 4 or 8), its edges smoothed. Live from the overlay (but for a change from
       one count to another, which takes a restart). */
    int native_msaa;
    int native_msaa_forced;
    /* --native-lod: the mipmap level the native GS reads a texture at (GSN_LOD_*, port/gs/native/gsn.h:
       0 ps2, the GS's own, picked for 640x448; 1 scale, the default, finer by log2 of --native-scale,
       which at 1x is the GS's own; 2 full, level 0 at every distance). Live from the overlay. */
    int native_lod;
    int native_lod_forced;
    /* --native-aniso: anisotropic filtering of the native GS's mipmapped textures, the most reads along
       a pixel's footprint (1, off, the default; 2, 4, 8, 16; gsn_set_aniso) */
    int native_aniso;
    int native_aniso_forced;
    /* --native-sampler: PORT_SAMPLER_GS, the GS's texture unit in the shader, or PORT_SAMPLER_HARDWARE, the GPU's
       sampler on RGBA8 textures (docs/port/PC_RENDERER.md, P1) */
    int native_sampler;
    int native_sampler_forced; /* --native-sampler given */
#define PORT_SAMPLER_GS 0
#define PORT_SAMPLER_HARDWARE 1
    /* --native-cutout-aa: with MSAA, the native GS's cut-outs (netting, foliage, fences, grates: textures
       see-through in places) shaded at every sample (GSN_CUTOUT_AA_*, port/gs/native/gsn.h: 0 off, 1
       ssaa, the default). Nothing without MSAA. Live from the overlay. */
    int native_cutout_aa;
    int native_cutout_aa_forced;
    /* --native-2d: at a --native-scale above 1, the 2D's textures read nearest by the game (the font,
       the HUD, the menus' and title's pictures) read bilinear where magnified (GSN_2D_*,
       port/gs/native/gsn.h: 0 ps2, 1 smooth, the default). Nothing at 1x. Live from the overlay. */
    int native_2d;
    int native_2d_forced;
    /* --native-colour: how precisely the native GS keeps colours (GSN_COLOUR_*, port/gs/native/gsn.h: 0
       exact, the default, the GS's 8 bits a channel and its roundings; 1 high, 16 bits a channel and the
       colours' arithmetic in floats, dithered to 8 bits on the way out). Live from the overlay. */
    int native_colour;
    int native_colour_forced;
    /* --native-layer-z: how the native GS keeps and tests the 3D's Z (GSN_LAYER_Z_*, port/gs/native/gsn.h:
       0 ps2, the GS's whole numbers, the layers blended over a surface failing the test in places and
       surfaces meeting in a sawtooth; 1 tolerant, the default: Z's fraction kept and the layers tested
       within one Z, clean layers and seams). Live from the overlay. */
    int native_layer_z;
    int native_layer_z_forced;
    /* --vu1: who runs the VU1 programs (0x000, 0x280, 0x5A0, 0x700; 0x276 passes packets through;
       port/platform/vu1.c, vu1_hle*.c). PORT_VU1_HLE, the default, computes them directly, with
       the same results as the interpreter to the bit; PORT_VU1_INTERP is the cycle-accurate
       interpreter (and what any other program runs on); PORT_VU1_CHECK runs both and compares them,
       run by run. */
    int vu1;
    /* How the GS's output becomes a PC picture (port/gs/gs_parallel.cpp). Each is on by default and
       turned off by --no-<name>; --ps2-scanout turns them all off (the GS's raw output). */
    int deinterlace;         /* weave the two fields of an interlaced picture into one */
    int progressive;         /* a frame-mode interlaced picture is read whole, not a field at a time */
    int anti_blur;           /* undo the blur of a game that blends its two read circuits */
    int adapt_width;         /* the picture's width follows the game's, not the 640 of the TV */
    int whole_frame;         /* the game draws whole pictures, not fields for an interlaced TV
                                (port_whole_frame(): Game_ShiftScreenByViewTurn is left out, movies
                                are shown in frame mode) */
    int seamless_scroll;     /* the scrolling textures' sprites end on their targets' edges, so the
                                textures repeat without a seam (port_seamless_scroll(); on by default) */
    int flicker_filter;    /* the game's flicker filter is drawn (port_flicker_filter(): each row half
                                the row above, for an interlaced TV); off by default, on with
                                --flicker-filter or --ps2-scanout */
    int flicker_filter_forced;
    /* The opt-in 16:9 picture (port_widescreen(), port/platform/gs.c): off by default, turned on by
       --widescreen or settings.json, live from the overlay. */
    int widescreen;
    int widescreen_forced;   /* --widescreen: on for this run whatever settings.json says */
    int widescreen_hud_centred; /* the HUD stays in the 4:3 middle instead of the 16:9 corner */
    /* The opt-in higher frame rates (port_frame_interp(), port/platform/frame_interp.c): 30 (the
       PS2's: a picture every second vblank in play) by default; 60, 120, 144, 240 (pictures a second)
       or PORT_FRAME_RATE_MONITOR (the window's screen's refresh) with --frame-rate or settings.json,
       live from the overlay. PORT_FRAME_RATE_UNLOCKED: as many as the renderer keeps up with
       (port/include/port/unlocked_rate.h), no more than frame_rate_cap a second (0: no cap). */
#define PORT_FRAME_RATE_MONITOR 1000
#define PORT_FRAME_RATE_UNLOCKED 1001
    int frame_rate;
    int frame_rate_cap;
    int frame_rate_cap_forced; /* --frame-rate-cap: this run's, whatever settings.json says */
    /* --tearing or settings.json: unlocked's swapchain immediate (tearing, the least latency; a VRR screen's), not
       mailbox (port/platform/present_vk.cpp) */
    int tearing;
    int tearing_forced;
    /* --present-scaling, --present-sharpen: how the picture is scaled to the window (port_video_scaling,
       port/platform/video.c), this run's whatever settings.json says (-1: the settings'); --dump-present
       WxH: dumps write the picture as a window of that size shows it */
    int present_scaling;
    float present_sharpen;
    int dump_present_width, dump_present_height;
    int frame_rate_forced;   /* --frame-rate: this run's, whatever settings.json says */
    /* --draw-distance (port_draw_distance(), port/include/port/host.h): how much further the game
       draws, PORT_DRAW_DISTANCE_ (1x, the PS2's, by default); and whether the sky's fog goes out with
       it (draw_distance_sky 1) or stays the PS2's (0, the default). Live from the overlay. */
    int draw_distance;
    int draw_distance_forced; /* --draw-distance: this run's, whatever settings.json says */
    int draw_distance_sky;
    int draw_distance_sky_forced; /* --draw-distance-sky */
    /* The opt-in mods (port/include/port/host.h): off by default, turned on by their switch or
       settings.json, live from the overlay. */
    int myus_necklace;       /* the effect of Myu's Necklace, always (port_myus_necklace()) */
    int myus_necklace_forced; /* --myus-necklace: on for this run whatever settings.json says */
    int separate_run;        /* running on a key of its own, not the action button (port_controls_run_held()) */
    int separate_run_forced; /* --separate-run: on for this run whatever settings.json says */
    /* The frame-rate counter in the picture's top right corner (port_video_counter_draw): off by
       default, turned on by --show-fps or settings.json, F3 or the overlay's Interface page */
    int show_fps;
    int show_fps_forced;     /* --show-fps: on for this run whatever settings.json says */
    int invincible;          /* --invincible, or the Debug page: no HP lost (port_invincible()); never saved */
    /* --flash-recorder (port/platform/present_vk.cpp, docs/port/README.md "Debugging"): the Vulkan presenter
       keeps the last flash_seconds of presented pictures in a ring on the GPU, each flash_height rows high
       (its width the picture's shape), for the flash recorder's key (F8); 0 seconds: off. Live from the
       overlay's Debug page; never saved */
    float flash_seconds;
    int flash_height;
    /* --native-draw (port_native_draw(), port/include/port/host.h; docs/port/NATIVE_DRAW.md): which kinds
       of the game's draws go to the renderer core instead of VU1 and the GS's packets, PORT_ND_* bits (0,
       off, by default: the PS2 path, the game byte for byte as without it). --native-draw-compare: the PS2
       path's packets queued too, and which of the two the native GS draws (PORT_ND_COMPARE_*; 0 none).
       Both live from the overlay's Debug page. Of the kinds only the switch for the whole path is saved:
       settings.json's "native_draw", on (PORT_ND_ACCEPTED) or off, also the overlay's Renderer section. */
    int native_draw;
    int native_draw_forced; /* --native-draw: this run's, whatever settings.json says */
    int native_draw_compare;
    /* --native-wave-uvs (port_native_wave_uvs(), port/include/port/host.h): how a native map record's wave
       UVs (flag 0x100) are drawn, PORT_WAVE_UVS_*; never saved. --test-dma-wait (port_test_dma_wait()):
       tests only, the packet chain sent and waited for at once (no race with the next frame's update) */
    int native_wave_uvs;
    int test_dma_wait;
    /* --sky (port_native_sky(), port/include/port/host.h): how the native sky is drawn, PORT_SKY_*; settings.json
       display.native_sky, live from the overlay's Debug page */
    int native_sky;
    int native_sky_forced; /* --sky: this run's, whatever settings.json says */
    const char *dump_frames; /* --dump-frames: the vblanks whose picture is written, "N,N,..." */
    const char *dump_dir;    /* --dump-dir: where they go (default: the current directory) */
    const char *controls;    /* --controls: the keyboard and mouse layout, ps2 (default) or verdite */
    const char *memcard;     /* --memcard: the folder that is the memory card in slot 1, or "none" */
    const char *record_input; /* --record-input: write the live pad as a pad script */
    int clock_fixed;         /* --clock: the disc drive's clock is fixed (clock_fields), not the host's */
    const char *audio_dump;  /* --audio-dump: write the sound as a WAV file */
    int no_audio;            /* --no-audio: no sound device (the sound is still made) */
    int clock_fields[6];     /* year, month, day, hour, minute, second */
    int stats;               /* --stats: print where the controller's time went at exit */
    const char *state_save_at; /* --state-save-at: VBLANK[:SLOT|:FILE],... (port/platform/savestate.c) */
    const char *state_load;    /* --state-load: [VBLANK:]SLOT|FILE */
    const char *quick_save_at; /* --quick-save-at: VBLANK[:SLOT|:FILE],... (port/platform/quicksave.c) */
    const char *quick_load;    /* --quick-load: [VBLANK:]SLOT|FILE */
    const char *teleport;      /* --teleport: [VBLANK:]MAP[:PLACE] (port/platform/teleport.c) */
} PortOptions;
extern PortOptions port_options;

/* --stats: what the DMA controller's thread spent its time on (port_ticks), counted only when
   port_options.stats is set; main_host.c prints it at exit. */
typedef struct {
    uint64_t vu1_ticks;   /* in VU1's run, the GIF hand-offs it made excluded */
    uint64_t gif_ticks;   /* handing GIF packets to the GS (all three paths) */
    uint64_t vsync_ticks; /* the GS's scanouts, with the picture to the window and --dump-frames */
    uint64_t vu1_runs;    /* MSCAL, MSCALF and MSCNT */
    uint64_t vu1_insns;   /* instruction pairs run (or that the HLE did the work of) */
    uint64_t vu1_hle_runs; /* of the runs, those --vu1 hle took */
    uint64_t vu1_hle_ticks; /* and their share of vu1_ticks */
    uint64_t vu1_cycles;  /* VU1's own clock (stalls included): two runs that timed the same agree */
    uint64_t path1_hash;  /* of every packet VU1's XGKICK sent: two runs that drew the same agree */
} PortStats;
extern PortStats port_stats;

/* ---- Fibers (port/platform/fiber.c) ---------------------------------------------------------
   Each EE thread is a fiber of the host's main thread: one runs at a time, and a switch happens
   only where the kernel makes one. */
typedef struct PortFiber PortFiber;

/* The calling host thread as a fiber (the EE's main thread). */
PortFiber *port_fiber_main(void);
/* A fiber that will run entry(arg) when first switched to. entry must not return. */
PortFiber *port_fiber_create(size_t stack_size, void (*entry)(void *), void *arg);
/* Frees a fiber that is not running. */
void port_fiber_delete(PortFiber *fiber);
/* Saves the running fiber in from and runs to. */
void port_fiber_switch(PortFiber *from, PortFiber *to);

/* ---- The EE's integer division (port/platform/int_divide.c) --------------------------------
   INT_MIN / -1 gives INT_MIN and remainder 0, as the EE's DIV does, where x86's IDIV traps; a division
   by zero says where it was before the program ends (the PS2 stops there too: ee-gcc's break 7). */
void port_int_divide_init(void);

/* ---- The EE's float unit -------------------------------------------------------------------
   The EE's FPU (COP1) rounds toward zero, reads denormals as zero and flushes results that would be
   one to zero. The EE threads' fibers run with the SSE unit set the same way (MXCSR, which each fiber
   keeps as its own), so the game's float C computes as on the PS2 (docs/port/RULES.md, "PS2 numbers").
   --fpu-round nearest leaves them in the host's IEEE mode (round to nearest, denormals), to compare.
   What is left different: the EE has no infinities or NaNs (an exponent of 255 is a number, a
   result too large is +-0x7FFFFFFF and a division by zero +-0x7FFFFFFF, where SSE gives +-FLT_MAX,
   +-inf or a NaN).
   Host code on the game's fibers that formats or parses numbers runs in the host's mode: the C
   library's printf and strtod round as MXCSR says (UCRT's do). */
#define PORT_MXCSR_EE 0xFFC0u   /* exceptions masked, round toward zero, FTZ, DAZ */
#define PORT_MXCSR_HOST 0x1F80u /* exceptions masked, round to nearest */
#define PORT_MXCSR_FLAGS 0x003Fu
extern int port_fpu_round_nearest; /* --fpu-round nearest */
/* The MXCSR an EE thread's fiber starts with. */
uint32_t port_ee_mxcsr(void);
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
/* Host code that must compute or format in the host's mode: begin returns what end puts back. */
static inline uint32_t port_host_float_begin(void) {
    uint32_t saved = _mm_getcsr();

    _mm_setcsr(PORT_MXCSR_HOST);
    return saved;
}
static inline void port_host_float_end(uint32_t saved) {
    _mm_setcsr(saved);
}
#else
static inline uint32_t port_host_float_begin(void) {
    return 0;
}
static inline void port_host_float_end(uint32_t saved) {
    (void)saved;
}
#endif

/* ---- The EE kernel and its clock (port/platform/kernel.c) -----------------------------------
   Time is the EE's bus clock (147.456 MHz), counted by the host: it moves only at vblank waits and
   at the calls a game polls in a loop, so the same input gives the same run. */
#define EE_BUS_HZ 147456000u
/* The kernel's first steps, before the game's main runs. */
void port_kernel_init(void);
/* Runs the game's main as the EE's main thread, on a fiber; does not return. */
int port_kernel_run(int (*game_main)(void));
/* The current EE time in bus cycles, and the vblanks so far. */
uint64_t port_ee_cycles(void);
uint64_t port_vblank_count(void);
/* Waits for the next vblank start (the EE's VSync): other threads run meanwhile. */
void port_vsync(void);
/* Lets time pass for a call the game polls in a loop (cycles of EE time). */
void port_kernel_spend(uint32_t cycles);
/* Runs the interrupt handlers that came due (the DMA controller's ends). Every kernel call does. */
void port_kernel_poll(void);

/* ---- The DMA controller (port/platform/dmac.c) ----------------------------------------------
   A host thread that plays the EE's DMA controller: a channel the game starts (STR set in its
   CHCR) is finished, and its STR cleared, as the hardware would. The transfers reach the GS in the
   order the game started them. */
void port_dmac_start(void);
/* (the game's thread, at every call into the host) Queues the channels the game has started since the
   last call, and wakes the controller: the game may be about to start one. */
void port_dmac_wake(void);
/* Waits until a channel's STR is clear. */
void port_dmac_wait(int channel);
/* (the kernel) The game's thread sleeps until a vblank is due (1), or runs again (0): the controller naps
   while it sleeps, as nothing can start a channel then. */
void port_dmac_game_sleeps(int sleeps);
/* (the kernel) The channels among `wanted` started since the last call, once their transfers are done:
   their end interrupts are due. */
uint32_t port_dmac_ends(uint32_t wanted);
/* (port_gs_post_vsync) Queues a vblank's scanout after the transfers started before it. */
void port_dmac_post_vsync(void);
/* (the controller) PATH3 may go on: it was unmasked, or a PATH1 or PATH2 packet ended. */
void port_gif_path3_poll(void);
/* The channel's CHCR as the game sees it, and transfers finished so far. */
volatile int *port_dmac_chcr(int channel);
uint64_t port_dmac_transfers(int channel);
/* Runs fn(arg) on the controller's thread, which owns the GS, and returns when it has. */
void port_dmac_call(void (*fn)(void *), void *arg);

/* ---- The IPU's input (port/platform/ipu.c) --------------------------------------------------
   DMA channel 4, which the controller's thread leaves alone: it moves on the game's thread, as the
   movie decoder (port/sdk/libmpeg.c) takes the stream. */
/* Sees what the game wrote into D4_CHCR: a start, or a stop (the kernel calls it at EIntr, which
   ends the movie player's writes). */
void port_ipu_dma_sync(void);
/* The next quadword channel 4 sends; 0 while it is stopped or its chain has ended. */
int port_ipu_read_qword(uint8_t qword[16]);
/* Sets IPU_BP for a decoder that holds `unused_bits` bits it has read and not used. */
void port_ipu_set_bp(int unused_bits);
/* 1 while a movie plays (port/sdk/libmpeg.c: a decoder exists), from any thread */
int port_movie_playing(void);

/* ---- VIF1 and VU1 (port/platform/vif1.c, port/platform/vu1.c) -------------------------------
   Run on the controller's thread as DMA channel 1 feeds them. */
void port_vif1_reset(void);
/* Takes VIF codes and their data, in 32-bit words (any split: a command may span two calls). */
void port_vif1_feed(const uint32_t *words, size_t count);
/* VIF1's MSKPATH3: 1 while PATH3 is masked. */
int port_vif1_path3_masked(void);
/* XGKICK: sends the GIF packet at a VU1 data address (in qwords) by PATH1. */
void port_gif_path1_kick(uint32_t address);
/* The bytes of the GIF packet XGKICK would send from address, into out (32 KB); returns its size. */
size_t port_gif_path1_read(uint32_t address, uint8_t *out);

void port_vu1_reset(void);
/* MPG wrote micro memory: decode it again. */
void port_vu1_micro_written(void);
uint8_t *port_vu1_micro(void);
uint8_t *port_vu1_data(void);
/* Runs a microprogram from an address (in instructions) to its end, with VIF1's TOP and ITOP. */
void port_vu1_start(uint32_t address, uint32_t top, uint32_t itop);
/* MSCNT: runs on from where the last one ended. */
void port_vu1_continue(uint32_t top, uint32_t itop);
/* The address (in instructions) the running or last run started at: which program XGKICKs */
extern uint32_t port_vu1_run_pc;
/* --frame-rate 60: the running or last run's batch, a hash of what was unpacked for it (vif1.c), and
   the runs' count */
extern uint64_t port_vif1_run_key;
extern uint32_t port_vif1_run_serial;
/* --vu1 */
enum { PORT_VU1_INTERP, PORT_VU1_HLE, PORT_VU1_CHECK };
/* --vu1 check: the runs compared, and those where the HLE and the interpreter differed (each
   difference is told on stderr, the first few) */
extern uint64_t port_vu1_check_runs, port_vu1_check_differ;

/* ---- The GS (port/platform/gs.c, port/gs/gs_parallel.cpp) -----------------------------------
   Everything but port_gs_init and port_gs_post_vsync runs on the controller's thread. */
/* MODERN and FAST are the settings' (port_settings.renderer, settings.json's "renderer") only: port_options.renderer is
   then NATIVE, with renderer_modern (and renderer_fast) set */
enum { PORT_RENDERER_GS = 0, PORT_RENDERER_NATIVE = 1, PORT_RENDERER_BOTH = 2, PORT_RENDERER_MODERN = 3, PORT_RENDERER_FAST = 4 };
/* port_options.draw_distance: --draw-distance 1, 1.5, 2, 4 or max */
enum { PORT_DRAW_DISTANCE_1 = 0, PORT_DRAW_DISTANCE_1_5, PORT_DRAW_DISTANCE_2, PORT_DRAW_DISTANCE_4, PORT_DRAW_DISTANCE_MAX,
       PORT_DRAW_DISTANCE_COUNT };
/* the level a name ("1", "1.5", "2", "4", "max") stands for, -1 for none; and a level's name */
int port_draw_distance_parse(const char *name);
const char *port_draw_distance_name(int level);
enum { PORT_PRESENTER_SDL = 0, PORT_PRESENTER_VULKAN = 1 };
/* --renderer both: how the last pictures of the two compared (port/platform/gs.c) */
typedef struct {
    uint64_t frames;          /* pictures compared */
    uint64_t frames_differ;   /* of which some pixel differs by more than 1 in a channel */
    uint32_t last_max;        /* the last picture's largest difference in a channel */
    uint32_t last_pixels;     /* its pixels that differ by more than 1 */
    double last_mean;         /* its mean difference per channel */
    uint32_t worst_max;       /* the largest difference seen */
    uint64_t worst_vblank;    /* where */
} PortRendererDiff;
extern PortRendererDiff port_renderer_diff;
void port_gs_print_stats(void);
/* --frame-rate: a quick load or teleport ends any cut of the pictures between (any thread) */
void port_gs_interp_cut_reset(void);
/* The native GS's scale (port_options.native_scale: 1 to 4, or 0 for auto), from the next vblank */
void port_gs_set_native_scale(int scale);
/* The scale the native GS draws at: port_options.native_scale, or for auto (0) the multiple of 448 lines
   nearest the height the window shows the picture at (port_video_picture_height()), 1 to 4; at most 2
   on an integrated GPU, 1 on any other that is not a discrete one (lavapipe) and with no window
   (headless) */
int port_gs_native_scale(void);
/* The height in pixels the window shows the picture at (port_video_fit), 0 with no window: written
   at each present, read by the GS's thread */
int port_video_picture_height(void);
/* The native GS's scale across: native_scale, or with --native-widescreen (and widescreen on) the
   smallest whole number at least 4/3 of it */
int port_gs_native_scale_x(void);
/* The native GS's MSAA (port_options.native_msaa), from the next vblank; returns the count the GPU
   gives (see gsn_set_msaa) */
int port_gs_set_native_msaa(int samples);
/* The native GS's mipmap levels (port_options.native_lod), from the next draw */
void port_gs_set_native_lod(int mode);
/* The native GS's anisotropic filtering (port_options.native_aniso: 1 off, 2 to 16), from the next draw */
void port_gs_set_native_aniso(int taps);
/* The native GS's cut-outs with MSAA (port_options.native_cutout_aa), from the next draw */
void port_gs_set_native_cutout_aa(int mode);
/* The native GS's 2D at a larger scale (port_options.native_2d), from the next draw */
void port_gs_set_native_2d(int mode);
/* The native GS's colour precision (port_options.native_colour), from the next vblank */
void port_gs_set_native_colour(int mode);
/* The native GS's Z (port_options.native_layer_z), from the next draw */
void port_gs_set_native_layer_z(int mode);
/* --renderer modern's shines blended in the shader: whole (0) or exact, in runs (1)
   (port_options.native_shine_exact), from the next draw */
void port_gs_set_native_shine_exact(int exact);
/* --native-draw and --native-draw-compare (port_options.native_draw, native_draw_compare), from the next
   frame */
void port_gs_set_native_draw(int kinds, int compare);

#define PORT_GS_FRAME_PIXELS (3840u * 2048u) /* the native GS at 4 times the PS2's 448 down, 6 across (--native-widescreen) */
typedef struct {
    uint32_t width, height;
    uint32_t *rgba; /* width * height pixels, R in the low byte */
} PortGsFrame;
/* Starts the renderer (after port_dmac_start), unless the run draws nothing. */
void port_gs_init(void);
/* 1 when there is a renderer. */
int port_gs_enabled(void);
/* 1 when the native GS draws alone: --frame-rate's pictures between ask it for the frame's uploads
   again instead of sending them (port/platform/frame_interp.c) */
int port_gs_replay_uploads(void);
/* --frame-rate's pictures between drawn on the GPU (port/platform/frame_interp.c, "On the GPU"): 1 when
   the native GS draws them (alone, and not KF4_INTERP_CPU); the frame (a GsnInterpFrame, handed over:
   the native GS frees it) and each picture (a GsnInterpPicture) of port/gs/native/gsn.h */
int port_gs_interp_gpu(void);
void port_gs_interp_frame(void *frame);
void port_gs_interp_picture(const void *picture);
/* A GIF packet by PATH1, 2 or 3, in whole qwords. */
void port_gs_gif(int path, const void *data, size_t bytes);
/* 1 when a path is between GIF packets (the GIF may hand the GS to another path). */
int port_gs_path_idle(int path);
/* Reads qwords of a local-to-host transfer (BITBLTBUF/TRXDIR 1) from the GS. */
void port_gs_read_fifo(void *dst, uint32_t qwords);
/* At vblank start (the game thread): queues the display registers for a scanout. */
void port_gs_post_vsync(void);
/* Waits until every queued vblank is scanned out (before the program stops). */
void port_gs_finish(void);
/* The controller's side: a vblank is queued; scan it out. */
int port_gs_vsync_pending(void);
/* The native GS's pipelines made ahead at the start (docs/port/RENDERER.md, "Pipelines made ahead"): 1 while they
   are made (running), and while those not in the pipeline cache are (building: how many of how many); any thread */
int port_gs_shaders_running(void);
int port_gs_shaders_building(int *done, int *total);
void port_gs_run_vsync(void);

/* --frame-rate (port/platform/frame_interp.c), on the controller's thread: each GIF packet sent to the
   GS is kept (port_interp_keep) until the vblank; port_interp_vsync, before the vblank's scanout, keeps
   them on as the pending frame when they drew the game's world into the buffer not shown (for this
   vblank and the next); port_interp_reset starts the next vblank's (whole: every path is between
   packets).
   port_interp_forget drops what was kept and the last frame's camera (a state was loaded). */
void port_interp_keep(int path, const void *data, size_t bytes);
void port_interp_vsync(const uint64_t *privileged);
/* A picture between at this point of the vblank (phase 0 to 1): the frame kept drawn again into the
   buffer shown, everything moved to where it is at that time (1 when it was drawn; 0: the buffer shown
   is right as it is, or nothing to draw). port_interp_end_vblank after the vblank's last. */
int port_interp_picture(double phase, void (*send)(int path, const void *data, size_t bytes));
void port_interp_end_vblank(void (*send)(int path, const void *data, size_t bytes));
/* 1 while a frame kept is being drawn between (the vblank's pictures come from it) */
int port_interp_active(void);
/* The pictures a second (port_options.frame_rate, the screen's for PORT_FRAME_RATE_MONITOR), and the
   times in vblank (0 to 1) of the pictures shown in it: up to max, their count returned (the
   vblank's own, at 0, among them when it is shown) */
int port_frame_rate_hz(void);
int port_interp_schedule(uint64_t vblank, double *phases, int max);
/* (the controller's thread) --frame-rate unlocked: a game vblank judged, behind 1 when the native GS was behind
   as it began (port/include/port/unlocked_rate.h); port_interp_unlocked_at_floor: 1 when the rate is as low as it
   goes (gs.c's cut of the pictures between then judges); port_interp_print_unlocked: its --stats line */
void port_interp_unlocked_vblank(uint64_t vblank, int behind);
int port_interp_unlocked_at_floor(void);
/* (port/platform/gs.c) The pictures between a vblank the renderer leaves out now because it fell behind
   (0: none; the F3 counter's "CUT") */
int port_interp_cut(void);
void port_interp_reset(int whole);
void port_interp_forget(void);
void port_interp_print_stats(void);

/* Whole frames of the screens drawn as fields (port_whole_frame(); port/platform/field_frames.c), on
   the controller's thread: each GIF packet sent to the GS is kept (port_field_frames_keep) until the
   vblank; port_field_frames_vsync, before the vblank's scanout, when the display is in field mode and
   they drew one picture into the buffer not shown, sends them again through send into the buffer
   shown, half a line the other way, and gives 1 and the buffer they drew (*drawn), to be scanned out
   as the other field first; port_field_frames_reset starts the next vblank's (whole: every path is
   between packets; 0 also drops what was kept). */
void port_field_frames_keep(int path, const void *data, size_t bytes);
int port_field_frames_vsync(const uint64_t *privileged, void (*send)(int path, const void *data, size_t bytes),
                            uint32_t *drawn);
void port_field_frames_reset(int whole);
uint64_t port_field_frames_replays(void);

/* ---- The window (port/platform/video.c) -------------------------------------------------- */
void port_video_init(void);
/* A picture's tag: the vblank it belongs to, and when in it (phase, 0 to 1: 0 is the vblank's own
   picture; --frame-rate above 60 has more between two vblanks). The window shows each at its time. */
#define PORT_PICTURE_TAG(vblank, phase) (((uint64_t)(vblank) << 16) | (uint64_t)((phase) * 65536.0) & 0xFFFF)
#define PORT_PICTURE_VBLANK(tag) ((tag) >> 16)
#define PORT_PICTURE_PHASE(tag) ((double)((tag) & 0xFFFF) / 65536.0)
/* (the controller's or the GS's thread) a picture for the window, with its tag */
void port_video_submit(const PortGsFrame *frame, uint64_t tag);
/* (the controller's or the GS's thread) a picture that is the native GS's image on the GPU
   (gsn_scanout_take_tag) */
void port_video_submit_image(uint64_t tag);
/* (the game thread) the wall time (port_wall_seconds) a vblank is shown at: its pictures' times */
void port_video_vblank(uint64_t vblank, double wall);
/* (the game thread) until the wall time `until` (or a little before it), shows the pictures between
   vblanks (frame rates above 60) as their times come; the caller sleeps the rest. port_video_poll, from
   the kernel's calls between vblanks: shows one that is due. */
void port_video_wait(double until);
void port_video_poll(void);
/* How the picture is scaled to the window: PORT_SCALING_ (the settings', or --present-scaling), and the
   sharpening's strength (0 to 1) */
enum { PORT_SCALING_SHARP = 0, PORT_SCALING_SMOOTH = 1, PORT_SCALING_BEST = 2 };
int port_video_scaling(void);
/* (port/platform/gs.c) the size the window shows the picture at, which the native GS scales it to with
   the best scaling (0 x 0: none) */
void port_gs_present_scale(int width, int height, float sharpen);
float port_video_sharpen(void);
/* the refresh rate of the window's screen (Hz), 0 when not known (no window) */
int port_video_refresh_rate(void);
/* (any thread) the window's screen refreshed at seen_s (port_wall_seconds): its vblank as the system knows it
   (port/include/port/screen_vblank.h), or the KF4_PRESENT_SIM screen's. The screen's refresh clock learns from
   them (port/include/port/refresh_clock.h). */
void port_video_refresh_seen(double seen_s);
/* (the controller's thread) --frame-rate monitor: the times in the vblank (0 to 1) of its pictures, one a
   refresh of the screen, each the world a little before the refresh it is shown at, by as much as the
   renderer's pictures need (port/platform/video.c, "Pictures timed by the screen's refreshes";
   port_interp_schedule). Up to max, their count returned; -1 while the refresh clock is not sure of the
   screen's refreshes. */
int port_video_refresh_schedule(uint64_t vblank, double *phases, int max);
/* 1 in a --headless run with KF4_PRESENT_SIM (a test of the window's pacing, with nothing drawn) */
int port_video_simulated(void);
/* 1 while the window is the Vulkan presenter's, which takes the native GS's picture as an image */
int port_video_takes_images(void);
/* the presenter running: PORT_PRESENTER_ (SDL when the Vulkan one was asked for and did not start) */
int port_video_presenter(void);
/* (the controller's thread) writes the picture as <dump-dir>/frame_<vblank>.png */
void port_video_dump(const PortGsFrame *frame, uint64_t vblank);
/* the same, under its own name in --dump-dir */
void port_video_dump_named(const PortGsFrame *frame, const char *name);
/* writes RGBA pixels (R in the low byte) as an RGB PNG: 1 when it was written */
int port_png_write(const char *path, const uint32_t *rgba, uint32_t width, uint32_t height);
/* (the game thread, at vblank) shows the newest picture due and keeps the window alive */
void port_video_present(void);
/* --stats: how the pictures reached the window */
void port_video_print_stats(void);
/* The new pictures presented to the window a second (by the game's thread or the pictures' own), and
   the game's own frames a second, sampled twice a second; 0 with no window */
double port_video_pictures_per_second(void);
double port_video_frames_per_second(void);
/* The frame-rate counter (port_options.show_fps), drawn by the presenters over the picture at x, y,
   width x height in the window's pixels, as squares (fill: as port_controls_draw's PortFillRect);
   port_video_counter_height: how far down from the picture's top margin it reaches (0: not shown) */
int port_video_counter_shown(void);
int port_video_counter_height(int height);
void port_video_counter_draw(void (*fill)(void *context, int x, int y, int width, int height, unsigned int rgba),
                             void *context, int x, int y, int width, int height);

/* ---- The disc (port/platform/disc.c) --------------------------------------------------------
   The user's own disc, as an ISO 9660 image or as the folder it was extracted to, read by sector
   number as the drive reads it. */
#define DISC_SECTOR_SIZE 2048u
void port_disc_open(const char *path);
/* Finds a file by its ISO 9660 path ("\DATA\KF4.DAT;1", any case, the version optional):
   1 and its first sector and size, or 0. */
int port_disc_find(const char *path, uint32_t *lsn, uint32_t *size);
/* Finds a member of KF4.DAT by its name ("097.map", any case): 1 and its first sector and size, or 0. */
int port_disc_find_member(const char *name, uint32_t *lsn, uint32_t *size);
/* Reads sectors into buffer; 0 when they are past the end of the disc. */
int port_disc_read(uint32_t lsn, uint32_t sectors, void *buffer);
/* The name of the file a sector belongs to, for the log (NULL: none). */
const char *port_disc_name_at(uint32_t lsn, uint32_t *first_lsn);

/* ---- The log --------------------------------------------------------------------------------
   port_log writes one line to the state log (--log), port_event the same with the vblank count,
   for the things that happen between frames (files opened, movies, the memory card). */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int port_log_enabled(void);

/* The game's state in one line (game/state_log.c), written to the log once a frame. */
void kf4_game_state(char *line, size_t size);

/* ---- The memory card (port/sdk/libmc.c) ----------------------------------------------------- */
/* Opens the card folder of --memcard (by default <host-dir>/memcard), making it if it is missing */
void port_mc_init(void);

/* ---- The pad (port/sdk/libpad.c) ------------------------------------------------------------ */
void port_pad_init(void);
/* Once a vblank: SDL's events. */
void port_pad_poll(void);

/* ---- Small host helpers (port/platform/host_time.c) --------------------------------------- */
/* Seconds since the program started, from the host's monotonic clock. */
double port_wall_seconds(void);
/* Sleeps for about the given seconds. */
void port_sleep(double seconds);
/* The host's fastest monotonic counter (for --stats), and its ticks a second. */
uint64_t port_ticks(void);
double port_ticks_per_second(void);

#endif /* PORT_PLATFORM_H */
