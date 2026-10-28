/* The profiler (Shift+P, port/overlay/profiler.cpp): where the time goes, thread by thread, to find
   what holds the frame rate down.

   Each thread adds the nanoseconds it spent in its stages while the panel is open (port_profile_on);
   the panel reads the counters twice a second and works out from two readings each thread's busy
   share of the time, the GPU's time a picture and what bounds the pictures a second
   (port_profile_report). Closed, a stage costs one relaxed atomic load.

   port/platform/profile.c, a library of its own (kf4_profile) that the native GS links as well as the
   port. Any thread may call any of these. */
#ifndef PORT_PROFILE_H
#define PORT_PROFILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROFILE_GAME,         /* the game's thread: its work between vblanks, not its wait for the next one */
    PROFILE_DISC,         /* (of PROFILE_GAME) disc reads */
    PROFILE_DMA,          /* the DMA controller's thread: VIF1, VU1 and the GIF's packets */
    PROFILE_RING_WAIT,    /* (of PROFILE_DMA) waiting for room in the native GS's ring: the GS behind */
    PROFILE_GS,           /* the native GS's thread: all its work, waits included */
    PROFILE_GS_INTERP,    /* (of PROFILE_GS) the pictures between, recorded and replayed */
    PROFILE_GS_WAIT,      /* (of PROFILE_GS) waiting for the back end or the GPU */
    PROFILE_COMPILE,      /* (of PROFILE_GS or PROFILE_BACKEND) pipelines made while a draw waited */
    PROFILE_BACKEND,      /* the back end's thread: Granite's calls into Vulkan */
    PROFILE_BACKEND_WAIT, /* the back end's thread: waiting for the GPU at a frame context */
    PROFILE_PRESENT_WAIT, /* the presenter: waiting for its frame's fence and a swapchain image */
    PROFILE_GPU,          /* the GPU's time on the native GS's work */
    PROFILE_STAGES
} ProfileStage;

/* The counters at one moment */
typedef struct {
    uint64_t now_ns;
    uint64_t ns[PROFILE_STAGES];
    uint64_t pictures; /* drawn by the native GS (vblanks' own and pictures between) */
    uint64_t vblanks;  /* the game's */
} ProfileCounters;

/* The panel is open: timers worth taking */
int port_profile_on(void);
void port_profile_set_on(int on);
/* A monotonic clock in nanoseconds */
uint64_t port_profile_now_ns(void);
void port_profile_add(ProfileStage stage, uint64_t ns);
void port_profile_picture(void);
void port_profile_vblank(void);
ProfileCounters port_profile_read(void);

/* The pipelines made while a draw waited (a compile in play), since the start: counted whether the panel is
   open or not, so that one line says how many there were and how long they took */
typedef struct {
    uint64_t count;
    uint64_t ns;         /* all of them */
    uint64_t longest_ns; /* the longest one */
} ProfileCompiles;
/* A pipeline made while a draw waited, ns long: counted, and while the panel is open its time is added to
   PROFILE_COMPILE */
void port_profile_compile(uint64_t ns);
ProfileCompiles port_profile_compiles(void);

/* A stall worth naming (a picture over twice its time): kept, the last PROFILE_HITCHES of them */
#define PROFILE_HITCHES 8
#define PROFILE_HITCH_TEXT 160
void port_profile_hitch(const char *text);
/* The hitches kept, newest first, into texts (at most PROFILE_HITCHES); their count */
int port_profile_hitches(char texts[][PROFILE_HITCH_TEXT], int most);

/* What bounds the pictures a second */
typedef enum {
    PROFILE_BOUND_NONE,    /* keeping up with the screen */
    PROFILE_BOUND_GPU,
    PROFILE_BOUND_GS,      /* the native GS's thread */
    PROFILE_BOUND_BACKEND, /* the back end's thread */
    PROFILE_BOUND_GAME,    /* the game's thread */
    PROFILE_BOUND_DMA,     /* the DMA controller's thread */
    PROFILE_BOUND_BURSTS,  /* no stage busy throughout: stalls now and then */
    PROFILE_BOUNDS
} ProfileBound;
const char *port_profile_bound_name(ProfileBound bound);

/* Between two readings */
typedef struct {
    double seconds;
    double share[PROFILE_STAGES]; /* each stage's time over the span's: a thread's busy share of a core */
    double gpu_ms_per_picture;    /* 0: no pictures, or no GPU times yet */
    double pictures_per_second;   /* drawn by the native GS */
    double vblanks_per_second;
    ProfileBound bound;
    double bound_share; /* the busy share that named it (0 for none and bursts) */
} ProfileReport;
/* shown_per_second: the pictures the window showed a second; target_per_second: as many as it could
   (the screen's refresh, or the frame-rate cap) */
ProfileReport port_profile_report(const ProfileCounters *before, const ProfileCounters *after,
                                  double shown_per_second, double target_per_second);

#ifdef __cplusplus
}
#endif

#endif
