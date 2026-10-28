/* The profiler's counters and what they say (port/include/port/profile.h). */
#ifdef _WIN32
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#endif
#include <stdio.h>
#include <string.h>

#include "port/profile.h"

static int on;
static uint64_t stage_ns[PROFILE_STAGES];
static uint64_t pictures, vblanks;

/* the hitches kept: a ring, behind a spin lock (they are few: a stall each at most) */
static char hitch_text[PROFILE_HITCHES][PROFILE_HITCH_TEXT];
static int hitch_next, hitch_count;
static int hitch_lock;

int port_profile_on(void) {
    return __atomic_load_n(&on, __ATOMIC_RELAXED);
}

void port_profile_set_on(int value) {
    __atomic_store_n(&on, value != 0, __ATOMIC_RELAXED);
}

uint64_t port_profile_now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)frequency.QuadPart);
#else
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        return 0;
    }
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

void port_profile_add(ProfileStage stage, uint64_t ns) {
    if ((unsigned)stage >= PROFILE_STAGES) {
        return;
    }
    __atomic_fetch_add(&stage_ns[stage], ns, __ATOMIC_RELAXED);
}

void port_profile_picture(void) {
    __atomic_fetch_add(&pictures, 1, __ATOMIC_RELAXED);
}

void port_profile_vblank(void) {
    __atomic_fetch_add(&vblanks, 1, __ATOMIC_RELAXED);
}

static uint64_t compile_count, compile_ns, compile_longest_ns;

void port_profile_compile(uint64_t ns) {
    uint64_t longest = __atomic_load_n(&compile_longest_ns, __ATOMIC_RELAXED);

    __atomic_fetch_add(&compile_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&compile_ns, ns, __ATOMIC_RELAXED);
    while (ns > longest &&
           !__atomic_compare_exchange_n(&compile_longest_ns, &longest, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (port_profile_on()) {
        port_profile_add(PROFILE_COMPILE, ns);
    }
}

ProfileCompiles port_profile_compiles(void) {
    ProfileCompiles c;

    c.count = __atomic_load_n(&compile_count, __ATOMIC_RELAXED);
    c.ns = __atomic_load_n(&compile_ns, __ATOMIC_RELAXED);
    c.longest_ns = __atomic_load_n(&compile_longest_ns, __ATOMIC_RELAXED);
    return c;
}

ProfileCounters port_profile_read(void) {
    ProfileCounters c;
    int i;

    c.now_ns = port_profile_now_ns();
    for (i = 0; i < PROFILE_STAGES; i++) {
        c.ns[i] = __atomic_load_n(&stage_ns[i], __ATOMIC_RELAXED);
    }
    c.pictures = __atomic_load_n(&pictures, __ATOMIC_RELAXED);
    c.vblanks = __atomic_load_n(&vblanks, __ATOMIC_RELAXED);
    return c;
}

static void lock_hitches(void) {
    while (__atomic_exchange_n(&hitch_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}

static void unlock_hitches(void) {
    __atomic_store_n(&hitch_lock, 0, __ATOMIC_RELEASE);
}

void port_profile_hitch(const char *text) {
    lock_hitches();
    snprintf(hitch_text[hitch_next], PROFILE_HITCH_TEXT, "%s", text);
    hitch_next = (hitch_next + 1) % PROFILE_HITCHES;
    if (hitch_count < PROFILE_HITCHES) {
        hitch_count++;
    }
    unlock_hitches();
}

int port_profile_hitches(char texts[][PROFILE_HITCH_TEXT], int most) {
    int n, i;

    lock_hitches();
    n = hitch_count < most ? hitch_count : most;
    for (i = 0; i < n; i++) {
        int at = (hitch_next - 1 - i + PROFILE_HITCHES) % PROFILE_HITCHES;

        memcpy(texts[i], hitch_text[at], PROFILE_HITCH_TEXT);
    }
    unlock_hitches();
    return n;
}

const char *port_profile_bound_name(ProfileBound bound) {
    static const char *const names[PROFILE_BOUNDS] = {
        "nothing: keeping up", "the GPU", "the native GS's thread", "the back end's thread",
        "the game's thread", "the DMA controller's thread", "stalls now and then (no stage busy throughout)",
    };
    return (unsigned)bound < PROFILE_BOUNDS ? names[bound] : "?";
}

/* a thread busy this share of the time or more holds the rest back; the GPU's times leave out the
   gaps between command buffers, so it is named a little lower */
#define THREAD_BOUND_SHARE 0.85
#define GPU_BOUND_SHARE 0.80
/* keeping up: this share of the pictures the screen could show, and the game's 60 vblanks a second */
#define KEEPING_UP_SHARE 0.95
#define KEEPING_UP_VBLANKS 59.0

ProfileReport port_profile_report(const ProfileCounters *before, const ProfileCounters *after,
                                  double shown_per_second, double target_per_second) {
    ProfileReport r;
    double span_ns = after->now_ns > before->now_ns ? (double)(after->now_ns - before->now_ns) : 0.0;
    uint64_t pictures_drawn = after->pictures - before->pictures;
    struct {
        ProfileBound bound;
        double share;
        double needed;
    } busy[5];
    double most_over = 0.0;
    int i;

    memset(&r, 0, sizeof(r));
    r.bound = PROFILE_BOUND_NONE;
    if (span_ns <= 0.0) {
        return r;
    }
    r.seconds = span_ns / 1e9;
    for (i = 0; i < PROFILE_STAGES; i++) {
        r.share[i] = (double)(after->ns[i] - before->ns[i]) / span_ns;
    }
    r.pictures_per_second = (double)pictures_drawn / r.seconds;
    r.vblanks_per_second = (double)(after->vblanks - before->vblanks) / r.seconds;
    if (pictures_drawn > 0) {
        r.gpu_ms_per_picture = (double)(after->ns[PROFILE_GPU] - before->ns[PROFILE_GPU]) / 1e6 / (double)pictures_drawn;
    }
    if (shown_per_second >= KEEPING_UP_SHARE * target_per_second && r.vblanks_per_second >= KEEPING_UP_VBLANKS) {
        return r;
    }

    /* the busiest stage against the share that bounds it; a thread's waits are not its work */
    busy[0].bound = PROFILE_BOUND_GPU;
    busy[0].share = r.share[PROFILE_GPU];
    busy[0].needed = GPU_BOUND_SHARE;
    busy[1].bound = PROFILE_BOUND_GS;
    busy[1].share = r.share[PROFILE_GS] - r.share[PROFILE_GS_WAIT];
    busy[1].needed = THREAD_BOUND_SHARE;
    busy[2].bound = PROFILE_BOUND_BACKEND;
    busy[2].share = r.share[PROFILE_BACKEND];
    busy[2].needed = THREAD_BOUND_SHARE;
    busy[3].bound = PROFILE_BOUND_GAME;
    busy[3].share = r.share[PROFILE_GAME];
    busy[3].needed = THREAD_BOUND_SHARE;
    busy[4].bound = PROFILE_BOUND_DMA;
    busy[4].share = r.share[PROFILE_DMA] - r.share[PROFILE_RING_WAIT];
    busy[4].needed = THREAD_BOUND_SHARE;
    r.bound = PROFILE_BOUND_BURSTS;
    for (i = 0; i < 5; i++) {
        double over = busy[i].share / busy[i].needed;

        if (over >= 1.0 && over > most_over) {
            most_over = over;
            r.bound = busy[i].bound;
            r.bound_share = busy[i].share;
        }
    }
    return r;
}
