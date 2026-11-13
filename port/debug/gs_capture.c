/* The GS frame capture (F9, the overlay's Debug page, --gs-capture-at): the GS's and VU1's state at
   the start of a frame, then everything that reaches them until it ends, in a file that any build
   reads (port/include/port/gs_capture_format.h), for tools/gsreplay (kf4-gsreplay) to draw again.

   When. A request is taken right after a scanout, on the DMA controller's thread: there the
   transfers started before the vblank are through, so VU1 is between programs and VIF1 between
   commands. Not while a GIF packet or an image transfer is half sent, or VIF1 is in the middle of a
   command: then it waits for the next scanout. It records the next two vblanks (both fields of the
   game's 30 Hz frame; --gs-capture-at VBLANK:N for N, KF4_GS_CAPTURE_VBLANKS=N for F9's) and writes the
   file after the last scanout.

   Cost. Unused, the hooks in port/platform/gs.c and vif1.c test a flag and nothing else. While it
   records, every GIF packet and VIF1 word is copied into memory; the file is compressed and written on
   the controller's thread when the capture ends (the game stops for that long, a fraction of a
   second). Nothing it does changes what the game or the GS do.

   Native draw. The chain's writes to PORT_GS_REG_NATIVE_DRAW name draws in the renderer core's side
   buffer (port/include/port/render.h): before each GIF or VIF1 record, what was added to it since is put
   in records of its own (rd_capture_poll: KF4GS_ND_MESH, KF4GS_ND_DRAWS), so the file holds every draw
   before the writes that name it. Without native draws there are none. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "port/debug.h"
#include "port/host.h"
#include "port/lz4_block.h"
#include "port/platform.h"
#include "port/render.h"
#include "port/savestate.h"

#if __has_include("build_rev.h")
#include "build_rev.h" /* KF4_BUILD_REV, git describe at the build (tools/build_rev.cmake) */
#endif
#ifndef KF4_BUILD_REV
#define KF4_BUILD_REV "unknown"
#endif

int port_gs_capture_recording;  /* the controller's thread only */
volatile int port_gs_capture_armed;

void port_debug_set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int port_debug_at_pending(void);
void port_debug_at_vblank(uint64_t vblank);

/* ---- Requests ---------------------------------------------------------------------------------- */
#define MAX_AT 64
typedef struct {
    uint64_t vblank;
    int vblanks;
} CaptureAt;

static CaptureAt capture_at[MAX_AT]; /* --gs-capture-at, sorted */
static int capture_at_count;
static int capture_at_next;
static volatile int request_pending; /* F9 or the overlay */
static int waiting_said;

/* the game's state line at recent vblanks (the game's thread writes, the controller's reads) */
#define NOTES 16
static char notes[NOTES][512];
static volatile uint64_t note_vblank[NOTES];

static int compare_at(const void *a, const void *b) {
    const CaptureAt *x = a, *y = b;

    return x->vblank < y->vblank ? -1 : x->vblank > y->vblank;
}

int port_gs_capture_init(const char *list) {
    const char *p = list;

    if (p == NULL) {
        return 0;
    }
    while (*p && capture_at_count < MAX_AT) {
        char *end;
        unsigned long long v = strtoull(p, &end, 0);
        int count = 2;

        if (end == p) {
            fprintf(stderr, "--gs-capture-at wants VBLANK[:VBLANKS] joined by commas, not %s\n", list);
            exit(2);
        }
        p = end;
        if (*p == ':') {
            count = (int)strtol(p + 1, &end, 0);
            p = end;
            if (count < 1 || count > 64) {
                fprintf(stderr, "--gs-capture-at: a capture holds 1 to 64 vblanks\n");
                exit(2);
            }
        }
        /* VBLANK is the first one recorded: it starts after the scanout of the one before */
        capture_at[capture_at_count].vblank = v ? v - 1 : 0;
        capture_at[capture_at_count].vblanks = count;
        capture_at_count++;
        if (*p == ',') {
            p++;
        }
    }
    qsort(capture_at, (size_t)capture_at_count, sizeof(capture_at[0]), compare_at);
    if (capture_at_count) {
        port_gs_capture_armed = 1;
    }
    return capture_at_count;
}

static char capture_folder[1100]; /* (port_debug_request_gs_capture_into) the next capture's folder */

void port_debug_request_gs_capture_into(const char *folder) {
    if (port_debug_gs_capture_busy()) {
        return; /* (one is coming already: it goes where it was asked for) */
    }
    snprintf(capture_folder, sizeof(capture_folder), "%s", folder);
    port_debug_request_gs_capture();
}

void port_debug_request_gs_capture(void) {
    if (!port_gs_enabled()) {
        port_debug_set_message("no GS capture: nothing is drawn in this run (no renderer)");
        return;
    }
    port_gs_capture_note_vblank(port_vblank_count());
    __atomic_store_n(&request_pending, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&port_gs_capture_armed, 1, __ATOMIC_RELEASE);
    port_debug_set_message("GS capture asked for: it starts at the next scanout");
}

int port_debug_gs_capture_busy(void) {
    return __atomic_load_n(&request_pending, __ATOMIC_ACQUIRE) || port_gs_capture_recording;
}

void port_gs_capture_note_vblank(uint64_t vblank) {
    int slot = (int)(vblank % NOTES);

    if (port_debug_at_pending()) {
        port_debug_at_vblank(vblank);
    }

    kf4_game_state(notes[slot], sizeof(notes[slot]));
    __atomic_store_n(&note_vblank[slot], vblank, __ATOMIC_RELEASE);
}

/* ---- The record buffer (the controller's thread) ---------------------------------------------- */
static uint8_t *buffer;
static size_t buffer_size, buffer_room;
static int vsyncs_left;
static int capture_vblanks;
static uint64_t start_vblank;
static uint64_t records;

static void reserve(size_t more) {
    if (buffer_size + more <= buffer_room) {
        return;
    }
    while (buffer_size + more > buffer_room) {
        buffer_room = buffer_room ? buffer_room * 2 : 64u << 20;
    }
    buffer = realloc(buffer, buffer_room);
    if (buffer == NULL) {
        PORT_FATAL("out of memory for a GS capture");
    }
}

/* a record of a head and data, padded to 4 bytes */
static void put2(uint32_t type, const void *head, size_t head_size, const void *data, size_t size) {
    Kf4GsRecord record;
    size_t padded = (head_size + size + 3) & ~(size_t)3;

    reserve(sizeof(record) + padded);
    record.type = type;
    record.size = (uint32_t)padded;
    memcpy(buffer + buffer_size, &record, sizeof(record));
    buffer_size += sizeof(record);
    if (head_size) {
        memcpy(buffer + buffer_size, head, head_size);
    }
    if (size) {
        memcpy(buffer + buffer_size + head_size, data, size);
    }
    memset(buffer + buffer_size + head_size + size, 0, padded - head_size - size);
    buffer_size += padded;
    records++;
}

static void put(uint32_t type, const void *data, size_t size) {
    put2(type, NULL, 0, data, size);
}

/* ---- The hooks (the controller's thread) ------------------------------------------------------ */
void port_gs_capture_gif(int path, const void *data, size_t bytes) {
    uint32_t p = (uint32_t)path;

    rd_capture_poll(put2);
    put2(KF4GS_GIF, &p, 4, data, bytes);
}

void port_gs_capture_vif1(const uint32_t *words, size_t count) {
    rd_capture_poll(put2);
    put(KF4GS_VIFDATA, words, count * 4);
}

void port_gs_capture_vu1_run(uint32_t address, uint32_t top, uint32_t itop) {
    uint32_t v[3] = {address, top, itop};

    put(KF4GS_VU1_RUN, v, sizeof(v));
}

void port_gs_capture_read_fifo(uint32_t qwords) {
    put(KF4GS_READ_FIFO, &qwords, 4);
}

/* ---- Start and end ------------------------------------------------------------------------------ */
static __attribute__((unused)) void emit(uint32_t type, const void *data, size_t size) {
    put(type, data, size);
}

static const char *why_not_now(void) {
    Kf4GsVif1Regs vif;

    if (!port_gs_enabled()) {
        return "nothing is drawn in this run";
    }
    if (port_gs_state_busy() != NULL) {
        return port_gs_state_busy();
    }
    if (!port_vif1_debug_regs(&vif, 0)) {
        return "VIF1 is in the middle of a command";
    }
    if (!port_vu1_debug_idle()) {
        return "VU1 has results in flight";
    }
    return NULL;
}

static void start(uint64_t vblank, int vblanks) {
    char meta[2048];
    const char *game = notes[vblank % NOTES];
    Kf4GsVu1Regs vu;
    Kf4GsVif1Regs vif;
    int n;
    uint64_t best = 0;

    /* the game's state at this vblank, or the newest noted before it */
    for (n = 0; n < NOTES; n++) {
        uint64_t v = __atomic_load_n(&note_vblank[n], __ATOMIC_ACQUIRE);

        if (v <= vblank && v >= best && notes[n][0]) {
            best = v;
            game = notes[n];
        }
    }
    buffer_size = 0;
    records = 0;
    snprintf(meta, sizeof(meta),
             "format=kf4gs %d\nstart_vblank=%llu\nvblanks=%d\ngame_vblank=%llu\ngame=%.511s\nbuild=%s (%s %s)\n"
             "deinterlace=%d\nprogressive=%d\nanti_blur=%d\nadapt_width=%d\nwidescreen=%d\n"
             "widescreen_hud_centred=%d\nwhole_frame=%d\nflicker_filter=%d\n",
             KF4GS_VERSION, (unsigned long long)vblank, vblanks, (unsigned long long)best, game, KF4_BUILD_REV,
             __DATE__, __TIME__, port_options.deinterlace, port_options.progressive, port_options.anti_blur,
             port_options.adapt_width, port_options.widescreen, port_options.widescreen_hud_centred,
             port_options.whole_frame, port_options.flicker_filter);
    put(KF4GS_META, meta, strlen(meta));
#ifdef KF4_HAVE_PARALLEL_GS
    port_gs_state_records(emit); /* (without a renderer no capture starts: why_not_now) */
#endif
    port_vu1_debug_regs(&vu, 0);
    port_vif1_debug_regs(&vif, 0);
    reserve(32768 + sizeof(vu) + 16);
    {
        /* one record: micro memory, data memory, the registers */
        uint8_t *block = malloc(32768 + sizeof(vu));

        if (block == NULL) {
            PORT_FATAL("out of memory for a GS capture");
        }
        memcpy(block, port_vu1_micro(), 16384);
        memcpy(block + 16384, port_vu1_data(), 16384);
        memcpy(block + 32768, &vu, sizeof(vu));
        put(KF4GS_VU1, block, 32768 + sizeof(vu));
        free(block);
    }
    put(KF4GS_VIF1, &vif, sizeof(vif));
    rd_capture_begin();
    start_vblank = vblank;
    vsyncs_left = capture_vblanks = vblanks;
    port_gs_capture_recording = 1;
    port_debug_set_message("GS capture: recording vblanks %llu to %llu", (unsigned long long)vblank + 1,
                           (unsigned long long)vblank + (unsigned long long)vblanks);
}

static uint64_t fnv(uint64_t hash, const uint8_t *p, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        hash = (hash ^ p[i]) * 0x100000001B3ull;
    }
    return hash;
}

static int write_file(const char *path) {
    Kf4GsHeader header;
    FILE *file = fopen(path, "wb");
    uint8_t *packed;
    uint32_t *table;
    size_t at;
    int ok;

    if (file == NULL) {
        return 0;
    }
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, KF4GS_MAGIC, 8);
    header.version = KF4GS_VERSION;
    header.header_size = sizeof(header);
    header.body_size = buffer_size;
    header.body_hash = fnv(0xCBF29CE484222325ull, buffer, buffer_size);
    packed = malloc(KF4GS_BLOCK_SIZE + KF4GS_BLOCK_SIZE / 255 + 16);
    table = malloc(sizeof(uint32_t) << LZ_HASH_BITS);
    if (packed == NULL || table == NULL) {
        PORT_FATAL("out of memory for a GS capture");
    }
    ok = fwrite(&header, sizeof(header), 1, file) == 1;
    for (at = 0; ok && at < buffer_size; at += KF4GS_BLOCK_SIZE) {
        uint32_t sizes[2];
        size_t n = buffer_size - at < KF4GS_BLOCK_SIZE ? buffer_size - at : KF4GS_BLOCK_SIZE;
        size_t packed_size = lz_compress(buffer + at, n, packed, table);
        const uint8_t *data = packed;

        if (packed_size >= n) {
            packed_size = n;
            data = buffer + at;
        }
        sizes[0] = (uint32_t)n;
        sizes[1] = (uint32_t)packed_size;
        ok = fwrite(sizes, sizeof(sizes), 1, file) == 1 && fwrite(data, 1, packed_size, file) == packed_size;
    }
    free(packed);
    free(table);
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

static void finish(void) {
    char path[1200];

    put(KF4GS_END, NULL, 0);
    port_gs_capture_recording = 0;
    if (capture_folder[0]) {
        snprintf(path, sizeof(path), "%s/gs_%06llu.kf4gs", capture_folder, (unsigned long long)start_vblank + 1);
        capture_folder[0] = 0;
    } else {
        port_debug_capture_path("gs", start_vblank + 1, "kf4gs", path, sizeof(path));
    }
    if (write_file(path)) {
        port_debug_set_message("GS capture of vblanks %llu to %llu written: %s (%.1f MB in memory, %llu records)",
                               (unsigned long long)start_vblank + 1,
                               (unsigned long long)(start_vblank + (uint64_t)capture_vblanks), path,
                               (double)buffer_size / 1048576.0, (unsigned long long)records);
    } else {
        port_debug_set_message("GS capture: cannot write %s", path);
    }
    /* keep the memory for the next one, but not a large one's */
    if (buffer_room > (256u << 20)) {
        free(buffer);
        buffer = NULL;
        buffer_room = 0;
    }
    buffer_size = 0;
    if (capture_at_next >= capture_at_count && !__atomic_load_n(&request_pending, __ATOMIC_ACQUIRE) &&
        !port_debug_at_pending()) {
        __atomic_store_n(&port_gs_capture_armed, 0, __ATOMIC_RELEASE);
    }
}

/* F9's vblanks: 2 (a frame of the game's), or KF4_GS_CAPTURE_VBLANKS (1 to 600: ten seconds; about 5 MB a vblank in
   memory while it records, 3 GB at 600) to catch something that shows in some frames only */
static int f9_vblanks(void) {
    static int vblanks = 0;

    if (vblanks == 0) {
        const char *v = getenv("KF4_GS_CAPTURE_VBLANKS");
        long n = v ? strtol(v, NULL, 0) : 2;

        vblanks = n < 1 ? 1 : n > 600 ? 600 : (int)n;
    }
    return vblanks;
}

void port_gs_capture_vsync(const uint64_t *privileged, int field, uint64_t vblank) {
    int vblanks = f9_vblanks();
    const char *why;

    if (port_gs_capture_recording) {
        uint8_t head[16];
        uint32_t f = (uint32_t)field, zero = 0;

        memcpy(head, &vblank, 8);
        memcpy(head + 8, &f, 4);
        memcpy(head + 12, &zero, 4);
        put2(KF4GS_VSYNC, head, 16, privileged, 0xF0);
        if (--vsyncs_left == 0) {
            finish();
        }
        return;
    }
    /* a capture due: --gs-capture-at's (those passed while one recorded are dropped), or F9's */
    while (capture_at_next < capture_at_count && capture_at[capture_at_next].vblank < vblank) {
        capture_at_next++;
    }
    if (capture_at_next < capture_at_count && capture_at[capture_at_next].vblank == vblank) {
        vblanks = capture_at[capture_at_next].vblanks;
    } else if (!__atomic_load_n(&request_pending, __ATOMIC_ACQUIRE)) {
        if (capture_at_next >= capture_at_count && !port_debug_at_pending()) {
            __atomic_store_n(&port_gs_capture_armed, 0, __ATOMIC_RELEASE);
        }
        return;
    }
    why = why_not_now();
    if (why != NULL) {
        if (!waiting_said) {
            port_debug_set_message("GS capture waits for the next scanout: %s", why);
            waiting_said = 1;
        }
        if (capture_at_next < capture_at_count && capture_at[capture_at_next].vblank == vblank) {
            capture_at[capture_at_next].vblank++; /* try again at the next one */
        }
        return;
    }
    waiting_said = 0;
    if (capture_at_next < capture_at_count && capture_at[capture_at_next].vblank == vblank) {
        capture_at_next++;
    } else {
        __atomic_store_n(&request_pending, 0, __ATOMIC_RELEASE);
    }
    start(vblank, vblanks);
}
