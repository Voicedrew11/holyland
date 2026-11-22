/* The profiler's counters and what they say (port/include/port/profile.h). */
#ifdef _WIN32
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/profile.h"

static uint64_t pictures;

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

void port_profile_picture(void) {
    __atomic_fetch_add(&pictures, 1, __ATOMIC_RELAXED);
}

static uint64_t compile_count, compile_ns, compile_longest_ns;

void port_profile_compile(uint64_t ns) {
    uint64_t longest = __atomic_load_n(&compile_longest_ns, __ATOMIC_RELAXED);

    __atomic_fetch_add(&compile_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&compile_ns, ns, __ATOMIC_RELAXED);
    while (ns > longest &&
           !__atomic_compare_exchange_n(&compile_longest_ns, &longest, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

ProfileCompiles port_profile_compiles(void) {
    ProfileCompiles c;

    c.count = __atomic_load_n(&compile_count, __ATOMIC_RELAXED);
    c.ns = __atomic_load_n(&compile_ns, __ATOMIC_RELAXED);
    c.longest_ns = __atomic_load_n(&compile_longest_ns, __ATOMIC_RELAXED);
    return c;
}

/* ---- The GPU's time by what it was doing ------------------------------------------------------
   An open-addressed table of keys to rows, behind a spin lock: the renderers add as their timestamps come in
   (a few thousand a vblank with detail on), the panel reads twice a second. */
#define GPU_SLOTS (2 * PROFILE_GPU_ROWS) /* a power of two */
#define GPU_OTHERS_KEY UINT64_MAX

static int gpu_recording, gpu_detail;
static int gpu_lock;
static uint64_t gpu_keys[GPU_SLOTS];
static int gpu_slot_row[GPU_SLOTS]; /* the row + 1; 0: empty */
static ProfileGpuRow gpu_rows[PROFILE_GPU_ROWS];
static int gpu_row_count;
static uint64_t gpu_pictures_at_reset;

int port_profile_gpu_recording(void) {
    return __atomic_load_n(&gpu_recording, __ATOMIC_RELAXED);
}

void port_profile_gpu_set_recording(int value) {
    __atomic_store_n(&gpu_recording, value != 0, __ATOMIC_RELAXED);
}

int port_profile_gpu_detail(void) {
    return __atomic_load_n(&gpu_detail, __ATOMIC_RELAXED);
}

void port_profile_gpu_set_detail(int value) {
    __atomic_store_n(&gpu_detail, value < 0 ? 0 : value > 2 ? 2 : value, __ATOMIC_RELAXED);
}

static void lock_gpu(void) {
    while (__atomic_exchange_n(&gpu_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}

static void unlock_gpu(void) {
    __atomic_store_n(&gpu_lock, 0, __ATOMIC_RELEASE);
}

void port_profile_gpu_reset(void) {
    lock_gpu();
    memset(gpu_slot_row, 0, sizeof(gpu_slot_row));
    gpu_row_count = 0;
    gpu_pictures_at_reset = __atomic_load_n(&pictures, __ATOMIC_RELAXED);
    unlock_gpu();
}

/* key's row, made when new (lock held); -1 when the table is full */
static int gpu_row_of(uint64_t key, const char *name, ProfileGpuLevel level) {
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    unsigned slot = (unsigned)(h >> 40) & (GPU_SLOTS - 1);
    ProfileGpuRow *row;

    for (;;) {
        int at = gpu_slot_row[slot];

        if (at == 0) {
            break;
        }
        if (gpu_keys[slot] == key) {
            return at - 1;
        }
        slot = (slot + 1) & (GPU_SLOTS - 1);
    }
    if (gpu_row_count >= PROFILE_GPU_ROWS - (key == GPU_OTHERS_KEY ? 0 : 1)) {
        return -1; /* (the last row kept for "(others)") */
    }
    row = &gpu_rows[gpu_row_count];
    snprintf(row->name, sizeof(row->name), "%s", name);
    row->level = level;
    row->ns = 0;
    row->intervals = 0;
    gpu_keys[slot] = key;
    gpu_slot_row[slot] = ++gpu_row_count;
    return gpu_row_count - 1;
}

void port_profile_gpu_add(uint64_t key, const char *name, ProfileGpuLevel level, uint64_t ns, uint64_t intervals) {
    int row;

    lock_gpu();
    row = gpu_row_of(key, name, level);
    if (row < 0) {
        row = gpu_row_of(GPU_OTHERS_KEY, "(others)", level);
    }
    if (row >= 0) {
        gpu_rows[row].ns += ns;
        gpu_rows[row].intervals += intervals;
    }
    unlock_gpu();
}

static int gpu_row_order(const void *a, const void *b) {
    const ProfileGpuRow *x = a, *y = b;

    if (x->ns != y->ns) {
        return x->ns > y->ns ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

int port_profile_gpu_rows(ProfileGpuRow *rows, int most) {
    int n;

    if (most <= 0) {
        return 0;
    }
    lock_gpu();
    n = gpu_row_count < most ? gpu_row_count : most;
    if (n == gpu_row_count) {
        memcpy(rows, gpu_rows, (size_t)n * sizeof(*rows));
        unlock_gpu();
        qsort(rows, (size_t)n, sizeof(*rows), gpu_row_order);
        return n;
    }
    /* fewer wanted than there are: the most time of all of them */
    {
        ProfileGpuRow *all = malloc(sizeof(gpu_rows));

        if (all == NULL) {
            unlock_gpu();
            return 0;
        }
        n = gpu_row_count < PROFILE_GPU_ROWS ? gpu_row_count : PROFILE_GPU_ROWS; /* (never more: gpu_row_of) */
        memcpy(all, gpu_rows, (size_t)n * sizeof(*all));
        unlock_gpu();
        qsort(all, (size_t)n, sizeof(*all), gpu_row_order);
        n = n < most ? n : most;
        memcpy(rows, all, (size_t)n * sizeof(*rows));
        free(all);
    }
    return n;
}

uint64_t port_profile_gpu_pictures(void) {
    uint64_t at;

    lock_gpu();
    at = gpu_pictures_at_reset;
    unlock_gpu();
    return __atomic_load_n(&pictures, __ATOMIC_RELAXED) - at;
}
