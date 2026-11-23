/* The frame profiler's record (port/include/port/zones.h): the zones' names, the lanes' rings and the pictures.

   An event is one 64-bit word: the zone's id in bits 0-15, an end in bit 16, its time in ns since the record's
   base (set when recording first turns on) in bits 17-63 (39 hours). A lane's ring is written by its one thread
   and read by zone_record.cpp's: each word is stored and loaded atomically (relaxed), and the count written is
   stored with release after it, so a reader sees whole words and knows which were written over while it read. */
#ifdef _WIN32
#include <windows.h>
#else
#define _GNU_SOURCE
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/profile.h"
#include "port/thread_name.h"
#include "port/zones.h"

#define RING_BITS 20
#define RING_SIZE ((uint64_t)1 << RING_BITS) /* events a lane keeps unread: 8 MB */
#define PICTURES_SIZE 4096
#define END_BIT ((uint64_t)1 << 16)
#define TIME_SHIFT 17

struct ZoneLane {
    char name[ZONE_LANE_NAME];
    int gpu;
    uint64_t *ring;
    uint64_t written; /* (atomic: the lane's thread stores, readers load) */
};

/* ---- the spin lock around the tables' growth (names and lanes are made rarely) ---- */
static int table_lock;

static void lock_tables(void) {
    while (__atomic_exchange_n(&table_lock, 1, __ATOMIC_ACQUIRE)) {
    }
}

static void unlock_tables(void) {
    __atomic_store_n(&table_lock, 0, __ATOMIC_RELEASE);
}

/* ---- recording ---- */
static int wanted;
static uint64_t base_ns; /* (0: never on yet) */

int port_zones_recording(void) {
    return __atomic_load_n(&wanted, __ATOMIC_RELAXED) > 0;
}

void port_zones_want(int more) {
    uint64_t zero = 0;

    if (more > 0) {
        uint64_t now = port_profile_now_ns();

        /* (the base first: a thread that sees recording on has it) */
        __atomic_compare_exchange_n(&base_ns, &zero, now ? now : 1, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    if (__atomic_add_fetch(&wanted, more, __ATOMIC_RELEASE) < 0) {
        __atomic_store_n(&wanted, 0, __ATOMIC_RELAXED); /* (more ends than wants: a caller's bug, not a lasting one) */
    }
}

/* ---- names ---- */
static char names[ZONE_IDS_MOST][ZONE_NAME_MOST];
static ZoneKind kinds[ZONE_IDS_MOST];
static int name_count; /* (atomic: the ids 1..name_count are whole) */

ZoneId port_zone_id(const char *name, ZoneKind kind) {
    int i, n;
    ZoneId id = ZONE_NONE;

    if (name == NULL || (unsigned)kind >= ZONE_KINDS) {
        return ZONE_NONE;
    }
    lock_tables();
    n = __atomic_load_n(&name_count, __ATOMIC_RELAXED);
    for (i = 1; i <= n; i++) {
        if (strncmp(names[i], name, ZONE_NAME_MOST - 1) == 0) {
            id = (ZoneId)i;
            break;
        }
    }
    if (id == ZONE_NONE && n + 1 < ZONE_IDS_MOST) {
        id = (ZoneId)(n + 1);
        snprintf(names[id], ZONE_NAME_MOST, "%s", name);
        kinds[id] = kind;
        __atomic_store_n(&name_count, n + 1, __ATOMIC_RELEASE);
    }
    unlock_tables();
    return id;
}

const char *port_zone_name(ZoneId id) {
    return id >= 1 && id <= port_zone_count() ? names[id] : "";
}

ZoneKind port_zone_kind(ZoneId id) {
    return id >= 1 && id <= port_zone_count() ? kinds[id] : ZONE_WORK;
}

int port_zone_count(void) {
    return __atomic_load_n(&name_count, __ATOMIC_ACQUIRE);
}

/* ---- lanes ---- */
static struct ZoneLane lanes[ZONE_LANES_MOST];
static int lane_count; /* (atomic: lanes 0..lane_count-1 are whole) */
static __thread ZoneLane *thread_lane;
static __thread int thread_lane_refused;

ZoneLane *port_zone_lane_make(const char *name, int gpu) {
    ZoneLane *lane = NULL;
    uint64_t *ring = (uint64_t *)calloc(RING_SIZE, sizeof(uint64_t));
    int n;

    if (ring == NULL) {
        return NULL;
    }
    lock_tables();
    n = __atomic_load_n(&lane_count, __ATOMIC_RELAXED);
    if (n < ZONE_LANES_MOST) {
        lane = &lanes[n];
        snprintf(lane->name, sizeof(lane->name), "%s", name ? name : "?");
        lane->gpu = gpu != 0;
        lane->ring = ring;
        lane->written = 0;
        __atomic_store_n(&lane_count, n + 1, __ATOMIC_RELEASE);
    }
    unlock_tables();
    if (lane == NULL) {
        free(ring);
    }
    return lane;
}

/* The calling thread's name, for its lane */
static void thread_name(char *name, size_t size) {
    const char *given = port_thread_name();

    if (given[0] != '\0') {
        snprintf(name, size, "%s", given);
        return;
    }
#ifdef _WIN32
    snprintf(name, size, "thread %lu", (unsigned long)GetCurrentThreadId());
#else
    snprintf(name, size, "thread %p", (void *)pthread_self());
#endif
}

static ZoneLane *my_lane(void) {
    char name[ZONE_LANE_NAME];

    if (thread_lane != NULL || thread_lane_refused) {
        return thread_lane;
    }
    thread_name(name, sizeof(name));
    thread_lane = port_zone_lane_make(name, 0);
    thread_lane_refused = thread_lane == NULL;
    return thread_lane;
}

static void put(ZoneLane *lane, ZoneId id, int end, uint64_t ns) {
    uint64_t base = __atomic_load_n(&base_ns, __ATOMIC_ACQUIRE);
    uint64_t at, word;

    if (lane == NULL || base == 0 || id == ZONE_NONE) {
        return;
    }
    at = ns > base ? ns - base : 0;
    word = (uint64_t)id | (end ? END_BIT : 0) | at << TIME_SHIFT;
    /* (only this thread writes written: a plain read of it is its own) */
    __atomic_store_n(&lane->ring[lane->written & (RING_SIZE - 1)], word, __ATOMIC_RELAXED);
    __atomic_store_n(&lane->written, lane->written + 1, __ATOMIC_RELEASE);
}

int port_zone_begin(ZoneId id) {
    ZoneLane *lane;

    if (!port_zones_recording() || id == ZONE_NONE) {
        return 0;
    }
    lane = my_lane();
    if (lane == NULL) {
        return 0;
    }
    put(lane, id, 0, port_profile_now_ns());
    return 1;
}

void port_zone_end(ZoneId id) {
    /* (written even when recording just turned off: its begin was) */
    put(thread_lane, id, 1, port_profile_now_ns());
}

void port_zone_after(ZoneId id, uint64_t start_ns) {
    ZoneLane *lane;

    if (!port_zones_recording() || id == ZONE_NONE) {
        return;
    }
    lane = my_lane();
    put(lane, id, 0, start_ns);
    put(lane, id, 1, port_profile_now_ns());
}

void port_zone_lane_begin_at(ZoneLane *lane, ZoneId id, uint64_t ns) {
    put(lane, id, 0, ns);
}

void port_zone_lane_end_at(ZoneLane *lane, ZoneId id, uint64_t ns) {
    put(lane, id, 1, ns);
}

/* ---- pictures: a ring of times written by whichever thread shows them (one at a time: the presenter) ---- */
static uint64_t picture_ns[PICTURES_SIZE];
static uint64_t pictures_written;
static int picture_lock;

void port_zone_picture(void) {
    uint64_t n;

    if (!port_zones_recording()) {
        return;
    }
    while (__atomic_exchange_n(&picture_lock, 1, __ATOMIC_ACQUIRE)) {
    }
    n = __atomic_load_n(&pictures_written, __ATOMIC_RELAXED);
    __atomic_store_n(&picture_ns[n % PICTURES_SIZE], port_profile_now_ns(), __ATOMIC_RELAXED);
    __atomic_store_n(&pictures_written, n + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&picture_lock, 0, __ATOMIC_RELEASE);
}

/* ---- reading ---- */
int port_zone_lanes(void) {
    return __atomic_load_n(&lane_count, __ATOMIC_ACQUIRE);
}

const char *port_zone_lane_name(int lane) {
    return lane >= 0 && lane < port_zone_lanes() ? lanes[lane].name : "";
}

int port_zone_lane_is_gpu(int lane) {
    return lane >= 0 && lane < port_zone_lanes() ? lanes[lane].gpu : 0;
}

size_t port_zone_lane_read(int lane, uint64_t *cursor, ZoneEvent *events, size_t most, uint64_t *lost) {
    ZoneLane *l;
    uint64_t written, from, to, after, i, base;
    size_t n = 0;

    if (lost != NULL) {
        *lost = 0;
    }
    if (lane < 0 || lane >= port_zone_lanes() || cursor == NULL) {
        return 0;
    }
    l = &lanes[lane];
    base = __atomic_load_n(&base_ns, __ATOMIC_ACQUIRE);
    written = __atomic_load_n(&l->written, __ATOMIC_ACQUIRE);
    from = *cursor;
    if (written - from > RING_SIZE) {
        if (lost != NULL) {
            *lost += written - RING_SIZE - from;
        }
        from = written - RING_SIZE;
    }
    to = written - from > most ? from + most : written;
    for (i = from; i < to; i++) {
        uint64_t word = __atomic_load_n(&l->ring[i & (RING_SIZE - 1)], __ATOMIC_RELAXED);

        events[n].id = (ZoneId)(word & 0xFFFF);
        events[n].end = (word & END_BIT) != 0;
        events[n].ns = base + (word >> TIME_SHIFT);
        n++;
    }
    /* (those the writer came round to while they were copied are not what was there: dropped) */
    after = __atomic_load_n(&l->written, __ATOMIC_ACQUIRE);
    if (after - from > RING_SIZE) {
        uint64_t bad = after - RING_SIZE - from;

        if (bad > n) {
            bad = n;
        }
        memmove(events, events + bad, (n - bad) * sizeof(ZoneEvent));
        n -= (size_t)bad;
        if (lost != NULL) {
            *lost += bad;
        }
    }
    *cursor = to;
    return n;
}

size_t port_zone_pictures_read(uint64_t *cursor, uint64_t *ns, size_t most) {
    uint64_t written = __atomic_load_n(&pictures_written, __ATOMIC_ACQUIRE);
    uint64_t from = *cursor, i;
    size_t n = 0;

    if (written - from > PICTURES_SIZE) {
        from = written - PICTURES_SIZE;
    }
    for (i = from; i < written && n < most; i++) {
        ns[n++] = __atomic_load_n(&picture_ns[i % PICTURES_SIZE], __ATOMIC_RELAXED);
    }
    *cursor = i;
    return n;
}
