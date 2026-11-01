/* The window, and --dump-frames' pictures (port/include/port/platform.h).

   The GS's scanout (port/platform/gs.c) is read back on the DMA controller's thread (or, the native
   GS's, kept on the GPU) and queued here with its time; the window shows each when it is due (at each
   vblank the game's thread, waiting a little for the vblank's own; with --frame-rate above 60 the
   pictures between too, "The pictures between") in an SDL window, scaled to fit it at the TV's 4:3 (or stretched, by the settings), with
   the settings overlay (F1) over it. That is
   SDL_Renderer's work, or with --presenter vulkan a swapchain's on the native GS's own device
   (port/platform/present_vk.cpp), which takes the native GS's picture as the image it is on the GPU
   (port_video_submit_image) and the others' pixels as they are here. A dumped frame is
   written as a PNG, uncompressed, so no library is needed. It is the GS's own picture, 640x448 (or
   the scanout's size) whatever the window shows: with --widescreen it is the squeezed picture, to be
   shown at 16:9 (with --native-widescreen, the native GS's wider one, 4/3 or more as many pixels
   across as --native-scale gives). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define yield_thread() SwitchToThread()
#else
#include <pthread.h>
#include <sched.h>
#define yield_thread() sched_yield()
#endif

#ifdef KF4_HAVE_SDL
#include <SDL.h>
#endif

#include "port/controls.h"
#include "port/host.h"
#include "port/overlay.h"
#include "port/platform.h"
#include "port/present_vk.h"
#include "port/refresh_clock.h"
#include "port/screen_vblank.h"
#include "port/settings.h"

/* ---- The pictures, between the threads -------------------------------------------------------- */
#ifdef _WIN32
static SRWLOCK frame_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&frame_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&frame_lock)
/* the window's present: the game's thread, or the pictures between's (port_video_wait) */
static SRWLOCK present_lock = SRWLOCK_INIT;
#define PRESENT_LOCK() AcquireSRWLockExclusive(&present_lock)
#define PRESENT_UNLOCK() ReleaseSRWLockExclusive(&present_lock)
#else
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&frame_lock)
#define UNLOCK() pthread_mutex_unlock(&frame_lock)
static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;
#define PRESENT_LOCK() pthread_mutex_lock(&present_lock)
#define PRESENT_UNLOCK() pthread_mutex_unlock(&present_lock)
#endif

#define VBLANK_SECONDS (1001.0 / 60000.0)

/* The pictures handed over and not yet shown, oldest first, each with its tag (PORT_PICTURE_TAG: its
   vblank, and when in it). A vblank's own picture is due at that vblank (port_video_vblank's wall time);
   with --frame-rate above 60 the pictures between two vblanks are due at their time between, and
   port_video_wait shows them then. A present shows the newest picture due and leaves out those before
   it (a picture that came late is not shown after a newer one). The queue holds a few vblanks'
   pictures: more, and the oldest goes. Pixels (SDL_Renderer, or paraLLEl-GS's) are copied in; the
   native GS's pictures on the GPU are only named (gsn_scanout_take_tag: it keeps the last 16). */
#define PICTURES 24
typedef struct {
    uint64_t tag;
    int image;        /* the native GS's image, not pixels */
    uint32_t *pixels; /* (the queue's own buffer, kept for the next) */
    size_t room;      /* in pixels */
    uint32_t width, height;
} Picture;
static Picture queue[PICTURES];
static int queue_first, queue_count;
static uint64_t newest_in;    /* 1 + the newest vblank a picture was handed over for (0: none yet) */
static int last_fresh;        /* the window's last present at a vblank had a new picture */
static uint64_t presented_vblank; /* the last vblank port_video_present ran at */
static int presenter = PORT_PRESENTER_SDL;

/* --stats: how the pictures reached the window (port_video_print_stats) */
static uint64_t stat_submitted, stat_overwritten, stat_fresh, stat_stale, stat_gaps[4], stat_left_out, stat_late;
static double stat_last, stat_sum, stat_sum2, stat_max;
static uint64_t stat_intervals, stat_vblanks_since, stat_between;

/* (under the lock) the queue's next place, the oldest left out when it is full */
static FILE *sim_log;
static void note_ready(uint64_t tag);

static Picture *queue_add(uint64_t tag) {
    Picture *p;

    if (sim_log != NULL) {
        fprintf(sim_log, "S %.6f %llu %.4f\n", port_wall_seconds(), (unsigned long long)PORT_PICTURE_VBLANK(tag),
                PORT_PICTURE_PHASE(tag));
    }
    stat_submitted++;
    if (queue_count == PICTURES) {
        stat_overwritten++; /* the oldest is never shown */
        queue_first = (queue_first + 1) % PICTURES;
        queue_count--;
    }
    p = &queue[(queue_first + queue_count) % PICTURES];
    queue_count++;
    p->tag = tag;
    note_ready(tag);
    if (PORT_PICTURE_VBLANK(tag) + 1 > newest_in) {
        __atomic_store_n(&newest_in, PORT_PICTURE_VBLANK(tag) + 1, __ATOMIC_RELEASE);
    }
    return p;
}

void port_video_submit(const PortGsFrame *frame, uint64_t tag) {
    size_t n = (size_t)frame->width * frame->height;
    Picture *p;

    LOCK();
    p = queue_add(tag);
    if (p->room < n) {
        free(p->pixels);
        p->pixels = malloc(sizeof(uint32_t) * n);
        p->room = p->pixels != NULL ? n : 0;
    }
    if (p->pixels != NULL) {
        memcpy(p->pixels, frame->rgba, sizeof(uint32_t) * n);
        p->width = frame->width;
        p->height = frame->height;
    } else {
        p->width = p->height = 0;
    }
    p->image = 0;
    UNLOCK();
}

void port_video_submit_image(uint64_t tag) {
    LOCK();
    queue_add(tag)->image = 1;
    UNLOCK();
}

int port_video_takes_images(void) {
    return __atomic_load_n(&presenter, __ATOMIC_ACQUIRE) == PORT_PRESENTER_VULKAN;
}

int port_video_presenter(void) {
    return __atomic_load_n(&presenter, __ATOMIC_ACQUIRE);
}

static int picture_out_height; /* port_video_picture_height: written by port_video_present */

int port_video_picture_height(void) {
    return __atomic_load_n(&picture_out_height, __ATOMIC_ACQUIRE);
}

/* ---- When each picture is due (the game's thread) ---------------------------------------------- */
#define TIMES 64
static uint64_t time_vblank[TIMES];
static double time_wall[TIMES];
static uint64_t times_newest;
static int times_set;

#ifdef KF4_HAVE_SDL
static void sample_rates(void);
#endif

void port_video_vblank(uint64_t vblank, double wall) {
    LOCK();
    time_vblank[vblank % TIMES] = vblank;
    time_wall[vblank % TIMES] = wall;
    times_newest = vblank;
    times_set = 1;
    UNLOCK();
#ifdef KF4_HAVE_SDL
    sample_rates();
#endif
}

/* A picture's wall time: its vblank's, and its phase of a vblank after it */
static double picture_time(uint64_t tag) {
    uint64_t v = PORT_PICTURE_VBLANK(tag);
    double base;

    if (!times_set) {
        return 0;
    }
    if (time_vblank[v % TIMES] == v) {
        base = time_wall[v % TIMES];
    } else {
        base = time_wall[times_newest % TIMES] + ((double)v - (double)times_newest) * VBLANK_SECONDS;
    }
    return base + PORT_PICTURE_PHASE(tag) * VBLANK_SECONDS;
}

static int refresh_rate; /* the window's screen's (port_video_refresh_rate) */

/* The screen's refresh clock (port_video_refresh_seen), under the lock; started again with the rate the
   system names whenever that changes */
static RefreshClock refresh_clock;
static int refresh_clock_named = -1;

void port_video_refresh_seen(double seen_s) {
    int named = port_video_refresh_rate();

    LOCK();
    if (named != refresh_clock_named) {
        refresh_clock_init(&refresh_clock, named);
        refresh_clock_named = named;
    }
    refresh_clock_add(&refresh_clock, seen_s);
    UNLOCK();
}

/* The thread shows each picture a quarter of a vblank after its time (the vblank's own too): the
   renderer makes them one after another from the vblank on, and the first between is due a quarter of
   a vblank (at 240) after it; so each is there before it is due, and they are shown evenly. */
#define THREAD_DELAY (0.25 * VBLANK_SECONDS)

/* ---- Pictures timed by the screen's refreshes -------------------------------------------------------
   With --frame-rate monitor, once the refresh clock is sure of the screen's refreshes (by_refresh), each
   picture is the world ahead_s before the refresh it is shown at (port_video_refresh_schedule), and is
   handed to the screen from just after the refresh before that one (show_at): a mailbox swapchain shows
   at each refresh the last picture it was given, FIFO the next one. So a picture may come as late as the
   screen's last moment for its refresh and still be on it, every refresh one picture on from the last,
   as with vsync, whose queue holds back pictures that come early and gives those that come late a
   refresh more.
   ahead_s is how late after their time the pictures come from the renderer (the first after a new game
   frame waits for the frame: several milliseconds), the READY_PERCENTILE of the last READY_KEPT, and half a
   refresh more for the screen's own lead (a compositor takes its picture before the refresh) and the
   refresh clock's error: the latency the pictures need, and no more. It goes up at once and down slowly,
   so that a picture's refresh moves seldom. */
#define READY_KEPT 256
#define READY_PERCENTILE 0.98
#define AHEAD_MOST 0.05            /* seconds: a hitch does not hold every picture back after it */
#define AHEAD_DOWN 0.0002          /* seconds ahead_s comes down at most each READY_EVERY pictures */
#define READY_EVERY 32             /* pictures between two looks at ahead_s */
#define SHOW_AFTER_REFRESH 0.125   /* a picture handed over this much of a refresh after the refresh before its own */
static double ready_after[READY_KEPT]; /* the last pictures' times from their time to being handed over */
static int ready_count, ready_next;
static double ahead_s; /* 0: not measured yet */

/* (under the lock) the pictures are timed by the screen's refreshes now (monitor's rate is the screen's:
   port_frame_rate_hz, which kf4-gsreplay does not link) */
static int by_refresh(void) {
    return port_options.frame_rate == PORT_FRAME_RATE_MONITOR && port_video_refresh_rate() > 60 && !port_options.fast &&
           times_set && refresh_clock_locked(&refresh_clock);
}

static double refresh_ahead_s(void) {
    double half = 0.5 * refresh_clock_period_s(&refresh_clock);

    return ahead_s > 0 ? ahead_s : half + THREAD_DELAY;
}

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

/* (under the lock) a picture handed over now: how late after its time, and ahead_s from the last ones */
static void note_ready(uint64_t tag) {
    double sorted[READY_KEPT], target;

    if (!by_refresh()) {
        ready_count = ready_next = 0;
        return;
    }
    ready_after[ready_next] = port_wall_seconds() - picture_time(tag);
    ready_next = (ready_next + 1) % READY_KEPT;
    ready_count += ready_count < READY_KEPT;
    if (ready_count < 2 * READY_EVERY || ready_next % READY_EVERY != 0) {
        return;
    }
    memcpy(sorted, ready_after, sizeof(double) * (size_t)ready_count);
    qsort(sorted, (size_t)ready_count, sizeof(double), compare_doubles);
    target = sorted[(int)(READY_PERCENTILE * (ready_count - 1))] + 0.5 * refresh_clock_period_s(&refresh_clock);
    target = target < AHEAD_MOST ? target : AHEAD_MOST;
    if (ahead_s == 0 || target > ahead_s) {
        ahead_s = target;
    } else {
        ahead_s = target > ahead_s - AHEAD_DOWN ? target : ahead_s - AHEAD_DOWN;
    }
}

/* (under the lock) When a picture may be handed to the screen: by the refreshes, just after the refresh
   before its own (the one nearest its time and ahead_s); else at its time and delay */
static double show_at(uint64_t tag, double delay) {
    if (by_refresh()) {
        double period = refresh_clock_period_s(&refresh_clock);
        double refresh = refresh_clock_next_s(&refresh_clock, picture_time(tag) + refresh_ahead_s() - 0.5 * period);

        return refresh - (1.0 - SHOW_AFTER_REFRESH) * period;
    }
    return picture_time(tag) + delay;
}

int port_video_scaling(void) {
#ifdef KF4_HAVE_SDL
    return port_options.present_scaling >= 0 ? port_options.present_scaling : port_settings.scaling;
#else
    return port_options.present_scaling >= 0 ? port_options.present_scaling : PORT_SCALING_BEST;
#endif
}

float port_video_sharpen(void) {
#ifdef KF4_HAVE_SDL
    return port_options.present_sharpen >= 0 ? port_options.present_sharpen : port_settings.sharpen;
#else
    return port_options.present_sharpen >= 0 ? port_options.present_sharpen : 0;
#endif
}

/* KF4_PRESENT_SIM=HZ, a test of the pacing without a window (--headless): pictures are handed over and
   presented at their times as for the Vulkan presenter, with nothing drawn, to a screen that refreshes
   HZ times a second (169.83: named 170, as a system names it; 0: none). Each picture presented is seen
   at the screen's next refresh, and the refresh clock learns the refreshes as from a screen's vblanks.
   --stats counts them, and the refreshes whose picture is not one refresh on from the last one's (a
   step of none, or two: what shows as a jump); KF4_PRESENT_LOG=FILE writes each present's wall time and
   the picture's vblank and phase (and "R" with the screen's first refresh and rate, "S" each handed over) */
static int sim;
static FILE *sim_log;
static double sim_hz, sim_start;  /* the simulated screen: refresh n at sim_start + n / sim_hz */
static int64_t sim_refresh = -1; /* the refresh the last picture presented is seen at */
static double sim_shown, sim_before; /* its picture's time, and the picture's at the refresh before */
static uint64_t stat_refreshes, stat_uneven;
#ifdef KF4_HAVE_SDL
static void between_start(void);
#endif

int port_video_simulated(void) {
    return sim;
}

int port_video_refresh_rate(void) {
    return __atomic_load_n(&refresh_rate, __ATOMIC_ACQUIRE);
}

#ifdef KF4_HAVE_SDL
/* The picture's place in a window of out_w x out_h pixels: the TV's 4:3, whatever the scanout's size;
   with --widescreen a 16:9 TV's (the game drew its picture squeezed for it, port/platform/gs.c); or
   the whole window, stretched */
void port_video_fit(int out_w, int out_h, int *x, int *y, int *width, int *height) {
    double aspect = port_options.widescreen ? 16.0 / 9.0 : 4.0 / 3.0;

    if (port_settings.aspect == ASPECT_STRETCH) {
        *width = out_w;
        *height = out_h;
    } else if ((double)out_w / out_h > aspect) {
        *height = out_h;
        *width = (int)(out_h * aspect + 0.5);
    } else {
        *width = out_w;
        *height = (int)(out_w / aspect + 0.5);
    }
    *x = (out_w - *width) / 2;
    *y = (out_h - *height) / 2;
}
#endif

/* ---- The window ------------------------------------------------------------------------------- */
#ifdef KF4_HAVE_SDL
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t texture_width, texture_height;
static int texture_smooth;
static uint32_t *shown;
static uint32_t shown_width, shown_height; /* its size */

/* the window's size in windowed mode: the height the settings say, and the width they say (a
   monitor's size, which the picture is fitted into) or as wide as the picture's shape (4:3, or 16:9
   with --widescreen) at that height */
static void window_size(int *width, int *height) {
    int h = port_settings.window_height < 448 ? 448 : port_settings.window_height;

    *width = port_settings.window_width > 0 ? port_settings.window_width
             : port_options.widescreen     ? (h * 16 + 8) / 9
                                            : (h * 4 + 2) / 3;
    *height = h;
}
#endif

#ifdef KF4_HAVE_SDL
/* The window's screen's refresh rate, for --frame-rate monitor; and its vblanks, for the refresh clock, when
   the screen is another */
static void read_refresh_rate(void) {
    static SDL_DisplayMode opened;
    SDL_DisplayMode mode;
    int display = SDL_GetWindowDisplayIndex(window);

    if (display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0 && mode.refresh_rate > 0) {
        __atomic_store_n(&refresh_rate, mode.refresh_rate, __ATOMIC_RELEASE);
        if (mode.w != opened.w || mode.h != opened.h || mode.refresh_rate != opened.refresh_rate) {
            opened = mode;
            port_screen_vblank_open(mode.w, mode.h, mode.refresh_rate);
        }
    }
}
#endif

void port_video_apply_settings(void) {
#ifdef KF4_HAVE_SDL
    int width, height;

    if (window == NULL) {
        return;
    }
    read_refresh_rate();
    SDL_SetWindowFullscreen(window, port_settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!port_settings.fullscreen) {
        window_size(&width, &height);
        SDL_SetWindowSize(window, width, height);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (renderer != NULL) {
        SDL_RenderSetVSync(renderer, port_settings.vsync);
    }
#ifdef KF4_HAVE_VULKAN_PRESENTER
    if (presenter == PORT_PRESENTER_VULKAN) {
        port_vk_set_vsync(port_settings.vsync);
    }
#endif
#endif
}

#ifdef KF4_HAVE_SDL
static SDL_Window *make_window(Uint32 flags) {
    int width, height;

    window_size(&width, &height);
    if (port_settings.fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    /* pictures of the overlay for tests (KF4_OVERLAY_SHOT) need no window on the screen */
    if (getenv("KF4_OVERLAY_SHOT") != NULL) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    return SDL_CreateWindow("King's Field IV", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
}
#endif

void port_video_init(void) {
#ifdef KF4_HAVE_SDL
    if (port_options.headless && port_gs_enabled() && getenv("KF4_PRESENT_SIM") != NULL) {
        sim = 1;
        sim_hz = atof(getenv("KF4_PRESENT_SIM"));
        refresh_rate = (int)floor(sim_hz + 0.5);
        sim_start = port_wall_seconds() + 0.37 / (sim_hz > 0 ? sim_hz : 60);
        __atomic_store_n(&presenter, PORT_PRESENTER_VULKAN, __ATOMIC_RELEASE);
        if (getenv("KF4_PRESENT_LOG") != NULL) {
            sim_log = fopen(getenv("KF4_PRESENT_LOG"), "w");
        }
        if (sim_log != NULL && sim_hz > 0) {
            fprintf(sim_log, "R %.6f %.6f\n", sim_start, sim_hz); /* (the screen: its first refresh, its rate) */
        }
        between_start();
        return;
    }
    if (port_options.headless || !port_gs_enabled()) {
        return;
    }
    /* the window's sizes are the screen's own pixels: without this, Windows at 150% scaling shows a
       2560 x 1440 monitor as 1707 x 960 and stretches the window's picture up to it */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[video] no window: %s\n", SDL_GetError());
        return;
    }
#ifdef KF4_HAVE_VULKAN_PRESENTER
    /* the Vulkan presenter on the window, if it starts; else the window again, for SDL_Renderer. It
       needs the native GS's device: with paraLLEl-GS alone (--renderer gs, or the native GS did not
       start; port_options.renderer is what started) the window is SDL_Renderer's, quietly unless
       --presenter vulkan asked for it */
    if (port_options.presenter == PORT_PRESENTER_VULKAN && port_options.renderer == PORT_RENDERER_GS) {
        if (port_options.presenter_forced) {
            fprintf(stderr, "[video] --presenter vulkan needs the native GS: the window is SDL_Renderer's\n");
        }
    } else if (port_options.presenter == PORT_PRESENTER_VULKAN) {
        window = make_window(SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
        if (window != NULL && port_vk_open(window)) {
            shown = malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS);
            __atomic_store_n(&presenter, PORT_PRESENTER_VULKAN, __ATOMIC_RELEASE);
            port_overlay_init(window, NULL);
            read_refresh_rate();
            between_start();
            return;
        }
        if (window == NULL) {
            fprintf(stderr, "[video] no Vulkan window: %s\n", SDL_GetError());
        }
        fprintf(stderr, "[video] the Vulkan presenter did not start: the window is SDL_Renderer's\n");
        if (window != NULL) {
            SDL_DestroyWindow(window);
        }
    }
#else
    if (port_options.presenter == PORT_PRESENTER_VULKAN) {
        fprintf(stderr, "[video] built without the Vulkan presenter: the window is SDL_Renderer's\n");
    }
#endif
    window = make_window(SDL_WINDOW_RESIZABLE);
    if (window == NULL) {
        fprintf(stderr, "[video] no window: %s\n", SDL_GetError());
        return;
    }
    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED | (port_settings.vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (renderer == NULL) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (renderer == NULL) {
        fprintf(stderr, "[video] no renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        window = NULL;
        return;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    shown = malloc(sizeof(uint32_t) * PORT_GS_FRAME_PIXELS);
    port_overlay_init(window, renderer);
    read_refresh_rate();
#else
    if (!port_options.headless && port_gs_enabled()) {
        fprintf(stderr, "[video] built without SDL2: no window\n");
    }
#endif
}

#ifdef KF4_HAVE_SDL
/* SDL_Renderer's square for the mouse indicator (port_controls_draw) */
static void fill_rect(void *context, int x, int y, int width, int height, unsigned int rgba) {
    SDL_Rect rect;

    (void)context;
    rect.x = x;
    rect.y = y;
    rect.w = width;
    rect.h = height;
    SDL_SetRenderDrawColor(renderer, (Uint8)rgba, (Uint8)(rgba >> 8), (Uint8)(rgba >> 16), (Uint8)(rgba >> 24));
    SDL_RenderFillRect(renderer, &rect);
}
#endif

#ifdef KF4_HAVE_SDL
/* ---- The frame-rate counter (F3, --show-fps, the overlay's Interface page) --------------------- */
/* The new pictures presented to the window, by either thread (draw_window, under PRESENT_LOCK; the
   last one drawn again for the overlay is not counted), and the rates sampled from them and from the
   game's frame counter twice a second on the game's thread (port_video_vblank). The counter is drawn
   by the presenters with plain squares, as the mouse indicator is (port_controls_draw): not with
   ImGui, whose frame is the game thread's, so it never takes the pictures between from their own
   thread (thread_shows). */
static uint64_t pictures_presented;
static double rate_wall;
static uint64_t rate_pictures;
static unsigned rate_frames;
static double pictures_rate, frames_rate; /* (read by both threads: __atomic_load) */

static void sample_rates(void) {
    double now = port_wall_seconds(), span = now - rate_wall, rate;
    uint64_t pictures = __atomic_load_n(&pictures_presented, __ATOMIC_ACQUIRE);
    PortGameReadout game;

    if (rate_wall > 0 && span < 0.5) {
        return;
    }
    kf4_game_readout(&game);
    if (rate_wall > 0) {
        rate = (double)(pictures - rate_pictures) / span;
        __atomic_store(&pictures_rate, &rate, __ATOMIC_RELEASE);
        /* (the menus, the title and the movies have loops of their own, which the play's frame counter
           does not count: their pictures are the game's own, none between, so the counter says one rate,
           not "0 GAME") */
        rate = game.frame != rate_frames ? (double)(game.frame - rate_frames) / span : pictures_rate;
        __atomic_store(&frames_rate, &rate, __ATOMIC_RELEASE);
    }
    rate_wall = now;
    rate_pictures = pictures;
    rate_frames = game.frame;
}
#endif

double port_video_pictures_per_second(void) {
    double rate = 0;
#ifdef KF4_HAVE_SDL
    __atomic_load(&pictures_rate, &rate, __ATOMIC_ACQUIRE);
#endif
    return rate;
}

double port_video_frames_per_second(void) {
    double rate = 0;
#ifdef KF4_HAVE_SDL
    __atomic_load(&frames_rate, &rate, __ATOMIC_ACQUIRE);
#endif
    return rate;
}

int port_video_counter_shown(void) {
#ifdef KF4_HAVE_SDL
    return __atomic_load_n(&port_options.show_fps, __ATOMIC_RELAXED) && (window != NULL || sim);
#else
    return 0;
#endif
}

#ifdef KF4_HAVE_SDL
/* 5 x 7 glyphs, a row a byte (bit 4 the left column), for what the counter says */
static const char counter_chars[] = "0123456789FPSGAMECUT ";
static const uint8_t counter_glyphs[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x0F, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0F},
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0, 0, 0, 0, 0, 0, 0},
};

/* The counter's size in a picture `height` pixels high: a glyph's pixel, and the margin to the
   picture's edges (the mouse indicator's, port_controls_draw) */
static void counter_metrics(int height, int *cell, int *margin) {
    *cell = height / 360 > 2 ? height / 360 : 2;
    *margin = height * 3 / 100 > *cell * 2 ? height * 3 / 100 : *cell * 2;
}

/* A line of the counter's text, its right edge at `right`: each row's runs of cells as one square */
static void counter_line(void (*fill)(void *, int, int, int, int, unsigned int), void *context, const char *text,
                         int right, int top, int cell, unsigned int rgba) {
    int n = (int)strlen(text), i;
    int left = right - (n * 6 - 1) * cell;

    for (i = 0; i < n; i++) {
        const char *at = strchr(counter_chars, text[i]);
        const uint8_t *glyph = counter_glyphs[at != NULL && *at != 0 ? at - counter_chars : (int)sizeof(counter_chars) - 2];
        int row, column;

        for (row = 0; row < 7; row++) {
            for (column = 0; column < 5;) {
                int run = 0;

                while (column + run < 5 && (glyph[row] >> (4 - column - run) & 1)) {
                    run++;
                }
                if (run > 0) {
                    fill(context, left + (i * 6 + column) * cell, top + row * cell, run * cell, cell, rgba);
                    column += run;
                } else {
                    column++;
                }
            }
        }
    }
}

/* The counter's lines: the pictures shown a second; and the game's own frames a second when they
   differ (the pictures between of --frame-rate, or a vblank's picture shown twice), with "CUT N" while
   the renderer, behind, draws N pictures between a vblank fewer (port_gs_run_vsync's cut) */
static int counter_text(char lines[2][24]) {
    double pictures = port_video_pictures_per_second(), frames = port_video_frames_per_second();
    int cut = port_interp_cut();

    snprintf(lines[0], sizeof(lines[0]), "%.0f FPS", pictures);
    if (cut > 0) {
        snprintf(lines[1], sizeof(lines[1]), "%.0f GAME CUT %d", frames, cut);
        return 2;
    }
    if (fabs(pictures - frames) >= 1.5) {
        snprintf(lines[1], sizeof(lines[1]), "%.0f GAME", frames);
        return 2;
    }
    return 1;
}

int port_video_counter_height(int height) {
    char lines[2][24];
    int cell, margin;

    if (!port_video_counter_shown()) {
        return 0;
    }
    counter_metrics(height, &cell, &margin);
    return (counter_text(lines) * 9 + 2) * cell;
}

void port_video_counter_draw(void (*fill)(void *, int, int, int, int, unsigned int), void *context, int x, int y,
                             int width, int height) {
    char lines[2][24];
    int cell, margin, count, widest = 0, i, right, top;

    if (!port_video_counter_shown() || width < 64 || height < 64) {
        return;
    }
    counter_metrics(height, &cell, &margin);
    count = counter_text(lines);
    for (i = 0; i < count; i++) {
        int w = ((int)strlen(lines[i]) * 6 - 1) * cell;

        widest = w > widest ? w : widest;
    }
    /* the picture's top right corner (the widescreen HUD goes top left), on a dark square: opaque, as
       the Vulkan presenter draws squares by clearing them */
    right = x + width - margin;
    top = y + margin;
    fill(context, right - widest - cell * 4, top, widest + cell * 4, (count * 9 + 2) * cell, 0xFF100C0Cu);
    for (i = 0; i < count; i++) {
        counter_line(fill, context, lines[i], right - cell * 2, top + cell * 2 + i * 9 * cell, cell,
                     i == 0 ? 0xFF8CEBFFu : 0xFFB4B4B4u);
    }
}
#endif

#ifdef KF4_HAVE_SDL
/* Takes the picture to show: the newest that may be shown at `now` (show_at, with delay) and whose
   time is no later than `before` (with --fast, the newest), those before it left out. 1 if there is one:
   its pixels into shown (or image: the native GS's, by its tag). */
static int take_picture(double now, double delay, double before, int *image, uint64_t *tag) {
    int i, pick = -1;

    LOCK();
    for (i = 0; i < queue_count; i++) {
        const Picture *p = &queue[(queue_first + i) % PICTURES];

        if (port_options.fast || (show_at(p->tag, delay) <= now && picture_time(p->tag) <= before)) {
            pick = i;
        }
    }
    if (pick >= 0) {
        Picture *p = &queue[(queue_first + pick) % PICTURES];

        stat_left_out += (uint64_t)pick;
        *image = p->image;
        *tag = p->tag;
        if (!p->image && p->width != 0) {
            shown_width = p->width;
            shown_height = p->height;
            memcpy(shown, p->pixels, sizeof(uint32_t) * shown_width * shown_height);
        }
        queue_first = (queue_first + pick + 1) % PICTURES;
        queue_count -= pick + 1;
    }
    UNLOCK();
    return pick >= 0;
}

/* (KF4_PRESENT_SIM) the picture tag presented now, seen at the simulated screen's next refresh: the
   refreshes from the last one seen to that one counted, each even when its picture is one refresh after
   the one before it (to half a refresh) */
static void sim_screen(uint64_t tag) {
    double period = 1.0 / sim_hz, shown;
    int64_t refresh = (int64_t)ceil((port_wall_seconds() - sim_start) * sim_hz);

    LOCK();
    shown = picture_time(tag);
    UNLOCK();
    if (refresh != sim_refresh) {
        if (sim_refresh >= 0) {
            int64_t again = refresh - sim_refresh - 1; /* refreshes that showed sim_shown once more */

            if (sim_before > 0) {
                stat_refreshes++;
                stat_uneven += fabs(sim_shown - sim_before - period) > 0.5 * period;
            }
            stat_refreshes += (uint64_t)(again > 0 ? again : 0);
            stat_uneven += (uint64_t)(again > 0 ? again : 0);
            sim_before = sim_shown;
        }
        sim_refresh = refresh;
        port_video_refresh_seen(sim_start + (double)refresh * period);
    }
    sim_shown = shown;
}

/* The window drawn: a new picture (fresh: its pixels in shown, or image: the native GS's by its tag),
   or the last again for the overlay; with overlay, the overlay and the mouse indicator over it (the
   game's thread only). Under PRESENT_LOCK. */
static void draw_window(int fresh, int image, uint64_t tag, int overlay) {
    uint32_t width = shown_width, height = shown_height;

    if (fresh) {
        __atomic_add_fetch(&pictures_presented, 1, __ATOMIC_RELEASE);
    }
    if (port_options.stats && fresh) {
        /* the wall time between new pictures */
        double now = port_wall_seconds();

        stat_fresh++;
        if (stat_last > 0) {
            double d = now - stat_last;

            stat_intervals++;
            stat_sum += d;
            stat_sum2 += d * d;
            stat_max = d > stat_max ? d : stat_max;
            stat_late += times_set && now - picture_time(tag) > 0.5 * VBLANK_SECONDS;
        }
        stat_last = now;
    }
    if (fresh && !sim && port_options.frame_rate == PORT_FRAME_RATE_MONITOR) {
        /* the screen's last vblank, to the refresh clock (each once) */
        static double fed;
        double vblank;

        if (port_screen_vblank_last(&vblank) && vblank != fed) {
            fed = vblank;
            port_video_refresh_seen(vblank);
        }
    }
    if (sim) {
        if (fresh && sim_hz > 0) {
            sim_screen(tag);
        }
        if (sim_log != NULL && fresh) {
            fprintf(sim_log, "%.6f %llu %.4f\n", port_wall_seconds(), (unsigned long long)PORT_PICTURE_VBLANK(tag),
                    PORT_PICTURE_PHASE(tag));
        }
        return;
    }
#ifdef KF4_HAVE_VULKAN_PRESENTER
    if (presenter == PORT_PRESENTER_VULKAN) {
        /* (the presenter draws again with nothing new only for the overlay, as below) */
        port_vk_present(fresh && !image ? shown : NULL, width, height, fresh && image, tag, overlay);
        return;
    }
#endif
    /* with nothing new, the window is drawn again only for the overlay */
    if (!fresh && (texture == NULL || !port_overlay_visible())) {
        return;
    }
    if (fresh && (texture == NULL || texture_width != width || texture_height != height)) {
        if (texture != NULL) {
            SDL_DestroyTexture(texture);
        }
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, (int)width,
                                    (int)height);
        texture_width = width;
        texture_height = height;
        texture_smooth = -1;
        if (texture == NULL) {
            return;
        }
    }
    if (fresh) {
        SDL_UpdateTexture(texture, NULL, shown, (int)(width * 4));
    }
    {
        int out_w, out_h, smooth;
        SDL_Rect dst;
        SDL_BlendMode blend;

        SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
        port_video_fit(out_w, out_h, &dst.x, &dst.y, &dst.w, &dst.h);
        /* the native GS's picture scaled to the window already (the best scaling): a pixel a pixel;
           else SDL's bilinear (smooth, best) or nearest (sharp). The GS scales its next pictures to the
           picture's place now. */
        smooth = port_video_scaling() != PORT_SCALING_SHARP && !(texture_width == (uint32_t)dst.w && texture_height == (uint32_t)dst.h);
        if (texture_smooth != smooth) {
            texture_smooth = smooth;
            SDL_SetTextureScaleMode(texture, texture_smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        }
        if (port_video_scaling() == PORT_SCALING_BEST || port_video_sharpen() > 0) {
            port_gs_present_scale(dst.w, dst.h, port_video_sharpen());
        } else {
            port_gs_present_scale(0, 0, 0);
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, &dst);
        SDL_GetRenderDrawBlendMode(renderer, &blend);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        port_video_counter_draw(fill_rect, NULL, dst.x, dst.y, dst.w, dst.h);
        port_controls_draw(fill_rect, NULL, dst.x, dst.y, dst.w, dst.h);
        SDL_SetRenderDrawBlendMode(renderer, blend);
        port_overlay_draw(dst.x, dst.y, dst.w, dst.h);
        SDL_RenderPresent(renderer);
    }
}
#endif

#ifdef KF4_HAVE_SDL
static int between_on(void);
static int thread_shows(void);
#endif

void port_video_present(void) {
#ifdef KF4_HAVE_SDL
    int fresh, image = 0;
    uint64_t tag = 0;

    if (window == NULL && !sim) {
        return;
    }
    if (window != NULL) {
        /* the height the picture is shown at, for --native-scale auto (port_gs_native_scale) */
        int out_w, out_h, x, y, w, h;

#if SDL_VERSION_ATLEAST(2, 26, 0)
        SDL_GetWindowSizeInPixels(window, &out_w, &out_h);
#else
        SDL_GetWindowSize(window, &out_w, &out_h);
#endif
        if (out_w > 0 && out_h > 0) {
            port_video_fit(out_w, out_h, &x, &y, &w, &h);
            __atomic_store_n(&picture_out_height, h, __ATOMIC_RELEASE);
        }
    }
    if (between_on() && thread_shows()) {
        /* (the pictures' thread shows this vblank's too, a little after: port_video_wait) */
        LOCK();
        presented_vblank = times_newest;
        UNLOCK();
        return;
    }
    /* The picture of the vblank just begun comes from the renderer's thread about when this one wakes
       for it, sometimes a little after. Shown at the next vblank instead, the one before was shown
       twice. So while pictures are coming (the last present had one) and none of this vblank's has come,
       the window waits for it, up to half a vblank; not with --fast, which does not wait for the
       renderer (it is held at most two vblanks behind, gsn_vsync). */
    if (!port_options.fast && last_fresh && times_set) {
        double until = port_wall_seconds() + 0.5 * VBLANK_SECONDS;

        while (__atomic_load_n(&newest_in, __ATOMIC_ACQUIRE) <= times_newest && port_wall_seconds() < until) {
            port_sleep(0.00025);
        }
    }
    PRESENT_LOCK();
    fresh = take_picture(port_wall_seconds() + 0.001, 0, 1e30, &image, &tag);
    last_fresh = fresh;
    LOCK();
    presented_vblank = times_newest;
    UNLOCK();
    if (port_options.stats) {
        /* a new picture at this vblank, or the last one again */
        stat_vblanks_since++;
        if (fresh) {
            stat_gaps[stat_vblanks_since < 4 ? stat_vblanks_since : 3]++;
            stat_vblanks_since = 0;
        } else {
            stat_stale++;
        }
    }
    draw_window(fresh, image, tag, 1);
    PRESENT_UNLOCK();
#endif
}

#ifdef KF4_HAVE_SDL
/* --frame-rate above 60: the pictures between two vblanks, each shown at its time. With the Vulkan
   presenter a thread of their own shows them, so that the game's own work between two vblanks (a few
   milliseconds, often without a call into the kernel) does not hold them up; it leaves them to the
   game's thread while the overlay or the mouse indicator is drawn (their ImGui frame is the game
   thread's; the frame-rate counter is not ImGui's and stays on the thread), and with SDL_Renderer (which is the game thread's) they are the game thread's always:
   shown while it waits for the next vblank (port_video_wait) and whenever it is in the kernel meanwhile
   (port_video_poll). (At 60 and below every picture is a vblank's own, shown by port_video_present.) */
static int between_thread_up;
static volatile int between_stop;
#ifdef _WIN32
static HANDLE between_handle;
#else
static pthread_t between_handle;
#endif

static int between_on(void) {
    return (window != NULL || sim) && !port_options.fast && times_set && port_frame_interp() && port_frame_rate_hz() > 60;
}

/* the thread shows them now */
static int thread_shows(void) {
    return between_thread_up && !port_overlay_visible() && !port_controls_indicator_shown();
}

int port_video_refresh_schedule(uint64_t vblank, double *phases, int max) {
    int n = 0;

    LOCK();
    if (!by_refresh()) {
        UNLOCK();
        return -1;
    }
    {
        /* a picture at each refresh's time less ahead_s */
        double period = refresh_clock_period_s(&refresh_clock), ahead = refresh_ahead_s();
        double start = picture_time(PORT_PICTURE_TAG(vblank, 0)), end = start + VBLANK_SECONDS, t;

        for (t = refresh_clock_next_s(&refresh_clock, start + ahead) - ahead; t < end - 1e-6 && n < max; t += period) {
            double phase = (t - start) / VBLANK_SECONDS;

            phases[n++] = phase < 1e-6 ? 0 : phase;
        }
    }
    UNLOCK();
    return n;
}

/* Shows the picture that is due at `now`, if there is one: from the game's thread, a picture between
   (not the next vblank's own, which is port_video_present's); from the thread, any, THREAD_DELAY after
   its time (show_at: by the screen's refreshes when they are known). 1 if it did. *first: when the oldest
   picture waiting is due (-1: none). */
static int show_between(double now, double *first, int from_thread) {
    double before, delay = from_thread ? THREAD_DELAY : 0;
    int image = 0, i, pick = -1;
    uint64_t tag = 0;

    now += 0.0003;
    LOCK();
    before = from_thread ? 1e30 : picture_time(PORT_PICTURE_TAG(presented_vblank + 1, 0)) - 0.0005;
    *first = queue_count > 0 ? show_at(queue[queue_first].tag, delay) : -1;
    for (i = 0; i < queue_count; i++) {
        const Picture *p = &queue[(queue_first + i) % PICTURES];

        if (show_at(p->tag, delay) <= now && picture_time(p->tag) <= before) {
            pick = i;
        }
    }
    UNLOCK();
    if (pick < 0) {
        return 0;
    }
    PRESENT_LOCK();
    if (!take_picture(now, delay, before, &image, &tag)) {
        PRESENT_UNLOCK();
        return 0;
    }
    stat_between += PORT_PICTURE_PHASE(tag) > 0;
    draw_window(1, image, tag, !from_thread);
    PRESENT_UNLOCK();
    return 1;
}

/* Until `until` (the wall time), shows the pictures between as they are due */
static void show_until(double until, int from_thread) {
    for (;;) {
        double now = port_wall_seconds(), next = until, first;

        if (now >= until - 0.0002 || (from_thread && between_stop)) {
            break;
        }
        if (show_between(now, &first, from_thread)) {
            continue;
        }
        if (first > now && first < next) {
            next = first;
        }
        if (next > now + 0.002) {
            next = now + 0.002; /* (a picture may still come) */
        }
        /* asleep to half a millisecond before, then the rest in a spin: a sleep wakes up to half a
           millisecond late, an eighth of a picture's time at 240 */
        if (next - now > 0.0007) {
            port_sleep(next - now - 0.0005);
        } else {
            while (port_wall_seconds() < next) {
                yield_thread();
            }
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI between_main(LPVOID unused) {
#else
static void *between_main(void *unused) {
#endif
    (void)unused;
    while (!between_stop) {
        if (between_on() && thread_shows()) {
            show_until(port_wall_seconds() + 0.01, 1);
        } else {
            port_sleep(0.002);
        }
    }
    return 0;
}

static void between_end(void) {
    if (between_thread_up) {
        between_stop = 1;
#ifdef _WIN32
        WaitForSingleObject(between_handle, 1000);
#else
        pthread_join(between_handle, NULL);
#endif
        between_thread_up = 0;
    }
}

/* (port_video_init) the thread, for the Vulkan presenter */
static void between_start(void) {
#ifdef _WIN32
    between_handle = CreateThread(NULL, 0, between_main, NULL, 0, NULL);
    between_thread_up = between_handle != NULL;
#else
    between_thread_up = pthread_create(&between_handle, NULL, between_main, NULL) == 0;
#endif
    if (between_thread_up) {
        atexit(between_end);
    }
}
#else
int port_video_refresh_schedule(uint64_t vblank, double *phases, int max) {
    (void)vblank;
    (void)phases;
    (void)max;
    return -1;
}
#endif

void port_video_poll(void) {
#ifdef KF4_HAVE_SDL
    static double next_look;
    double now, first;

    if (!between_on() || thread_shows()) {
        return;
    }
    now = port_wall_seconds();
    if (now < next_look) {
        return;
    }
    next_look = now + 0.00025;
    show_between(now, &first, 0);
#endif
}

void port_video_wait(double until) {
#ifdef KF4_HAVE_SDL
    if (!between_on() || thread_shows()) {
        return;
    }
    show_until(until, 0);
#else
    (void)until;
#endif
}

/* ---- PNG -------------------------------------------------------------------------------------- */
static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    size_t i;

    if (crc_table[1] == 0) {
        uint32_t k;

        for (k = 0; k < 256; k++) {
            uint32_t c = k;
            int b;

            for (b = 0; b < 8; b++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            crc_table[k] = c;
        }
    }
    for (i = 0; i < n; i++) {
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, size_t size) {
    uint8_t head[8];
    uint8_t tail[4];
    uint32_t crc;

    put32(head, (uint32_t)size);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    fwrite(data, 1, size, f);
    crc = crc32_update(0xFFFFFFFFu, head + 4, 4);
    crc = crc32_update(crc, data, size) ^ 0xFFFFFFFFu;
    put32(tail, crc);
    fwrite(tail, 1, 4, f);
}

/* RGB rows, each after its filter byte (0), in a zlib stream of stored blocks */
int port_png_write(const char *path, const uint32_t *rgba, uint32_t width, uint32_t height) {
    size_t row = (size_t)width * 3 + 1;
    size_t raw_size = row * height;
    size_t blocks = (raw_size + 65534) / 65535;
    size_t z_size = 2 + raw_size + blocks * 5 + 4;
    uint8_t *raw = malloc(raw_size);
    uint8_t *z = malloc(z_size);
    uint8_t header[13];
    uint32_t a = 1, b = 0;
    size_t i, at, done;
    uint32_t x, y;
    FILE *f;
    int ok = 0;

    for (y = 0; y < height; y++) {
        uint8_t *out = raw + row * y;

        *out++ = 0;
        for (x = 0; x < width; x++) {
            uint32_t p = rgba[(size_t)y * width + x];

            *out++ = (uint8_t)p;
            *out++ = (uint8_t)(p >> 8);
            *out++ = (uint8_t)(p >> 16);
        }
    }
    z[0] = 0x78;
    z[1] = 0x01;
    at = 2;
    for (done = 0; done < raw_size;) {
        size_t n = raw_size - done > 65535 ? 65535 : raw_size - done;

        z[at++] = done + n == raw_size;
        z[at++] = (uint8_t)n;
        z[at++] = (uint8_t)(n >> 8);
        z[at++] = (uint8_t)~n;
        z[at++] = (uint8_t)(~n >> 8);
        memcpy(z + at, raw + done, n);
        at += n;
        done += n;
    }
    for (i = 0; i < raw_size; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put32(z + at, (b << 16) | a);
    at += 4;

    put32(header, width);
    put32(header + 4, height);
    header[8] = 8;  /* bits */
    header[9] = 2;  /* RGB */
    header[10] = 0; /* deflate */
    header[11] = 0; /* filters */
    header[12] = 0; /* not interlaced */

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "[video] cannot write %s\n", path);
    } else {
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        write_chunk(f, "IHDR", header, sizeof(header));
        write_chunk(f, "IDAT", z, at);
        write_chunk(f, "IEND", NULL, 0);
        fclose(f);
        ok = 1;
    }
    free(raw);
    free(z);
    return ok;
}

void port_video_print_stats(void) {
#ifdef KF4_HAVE_SDL
    double mean = stat_intervals ? stat_sum / (double)stat_intervals : 0;
    double sd = stat_intervals ? sqrt(stat_sum2 / (double)stat_intervals - mean * mean) : 0;

    if (window == NULL && !sim) {
        return;
    }
    fprintf(stderr,
            "[stats] window: %llu pictures handed over, %llu of them dropped (the queue full), %llu left out (a newer "
            "one due); %llu vblanks showed a new one (1, 2, 3+ vblanks after the last: %llu, %llu, %llu), %llu the "
            "last again; %llu pictures between vblanks shown; between new pictures %.2f ms (sd %.2f, most %.2f); %llu "
            "shown more than half a vblank after their time\n",
            (unsigned long long)stat_submitted, (unsigned long long)stat_overwritten, (unsigned long long)stat_left_out,
            (unsigned long long)(stat_gaps[1] + stat_gaps[2] + stat_gaps[3]), (unsigned long long)stat_gaps[1],
            (unsigned long long)stat_gaps[2], (unsigned long long)stat_gaps[3], (unsigned long long)stat_stale,
            (unsigned long long)stat_between, mean * 1e3, sd * 1e3, stat_max * 1e3, (unsigned long long)stat_late);
    fprintf(stderr, "[stats] window: the last half second, %.1f pictures a second, %.1f game frames a second\n",
            port_video_pictures_per_second(), port_video_frames_per_second());
    LOCK();
    if (refresh_clock_named < 0) {
        fprintf(stderr, "[stats] window: no vblanks of the screen known (the refresh clock not started)\n");
    } else {
        fprintf(stderr, "[stats] window: the screen's refresh clock %s, %.4f Hz (named %d); pictures %s, %.2f ms "
                        "before their refresh\n",
                refresh_clock_locked(&refresh_clock) ? "locked" : "not locked",
                1.0 / refresh_clock_period_s(&refresh_clock), port_video_refresh_rate(),
                by_refresh() ? "timed by it" : "not timed by it", refresh_ahead_s() * 1e3);
    }
    UNLOCK();
    if (sim && sim_hz > 0) {
        fprintf(stderr, "[stats] window: the screen (KF4_PRESENT_SIM) refreshed %llu times, %llu of them uneven (a "
                        "step of none or two refreshes)\n",
                (unsigned long long)stat_refreshes, (unsigned long long)stat_uneven);
    }
#endif
}

void port_video_dump(const PortGsFrame *frame, uint64_t vblank) {
    char name[64];

    snprintf(name, sizeof(name), "frame_%06llu.png", (unsigned long long)vblank);
    port_video_dump_named(frame, name);
}

void port_video_dump_named(const PortGsFrame *frame, const char *name) {
    char path[4096];

    snprintf(path, sizeof(path), "%s/%s", port_options.dump_dir ? port_options.dump_dir : ".", name);
    if (port_png_write(path, frame->rgba, frame->width, frame->height)) {
        fprintf(stderr, "[video] %ux%u, %s\n", frame->width, frame->height, path);
    }
}
