/* Save states (port/include/port/savestate.h): the whole running game in a file, and back.

   When. Only at a safe point: the kernel's vblank (port/platform/kernel.c), where every EE thread is
   in a kernel call, parked on its fiber. The thread that runs the vblank switches to the host's own
   fiber, which runs port_state_host, then switches back. The work itself runs on the DMA controller's
   thread (port_dmac_call), after the transfers the game started have gone through, so the controller,
   VIF1, VU1 and the GS stand still while it is done; the sound is locked (SDL's audio thread renders
   on the device's clock). A state is not taken while a GIF packet or an image transfer is half sent,
   or a host0: file is open: the request waits for the next vblank.

   What. Everything the game's run depends on, in this order (sync_all): game RAM, the scratchpad, VU0
   and the register pages; the program's image of the game's and ico-audio's globals (between the
   markers port/platform/state_begin.c and state_end.c); the EE threads' fibers and stacks; the kernel
   (threads, semaphores, handlers, timers, EE time); the DMA controller's counts; VIF1; VU1; the GIF's
   paths and the GS's VRAM and registers; the libraries (libgraph, libpad, libcdvd, SIF and the
   IOP heap, rand); the IOP's memory; libsd's sound data inputs; FSIOPSND, modmidi and modhsyn; the
   sound's clock. The parts list their own variables (port_*_state). Not in a state: what belongs to
   the host (the window, the sound device, the log, files, the options) and the counters of --stats,
   which count from the last state saved or loaded.

   Same build only. The stacks hold return addresses, and game RAM and the globals hold pointers (into
   game RAM, to the program's globals and code, into the stacks). So game RAM and the stacks are at
   fixed addresses (memory.c, fiber.c), the program is linked without ASLR (CMakeLists.txt), and a
   state carries a build id, a hash of the program's code: a state from another build is refused.

   The file: a header (StateHeader), then the state in blocks of up to 4 MB, each compressed in the
   LZ4 block format (or stored as it is when that is no smaller). */
#ifndef _WIN32
#define _GNU_SOURCE /* dl_iterate_phdr */
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <link.h>
#include <sys/stat.h>
#endif

#include "port/audio.h"
#include "port/lz4_block.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/sanitize.h"
#include "port/savestate.h"
#include "spu2.h"

/* The parts hand over whole regions: with ASan, the globals' image holds the redzones between them,
   and a stack the redzones of its frames (port/include/port/sanitize.h) */
#if PORT_ASAN
#define state_copy port_raw_copy
#else
#define state_copy memcpy
#endif

#define STATE_MAGIC "KF4STATE"
#define STATE_VERSION 1
#define BLOCK_SIZE 0x400000u
#define MAX_SAVE_AT 64

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t build_id;
    int64_t time;      /* when it was written, seconds since 1970 */
    uint64_t vblank;   /* the vblank it was taken at */
    uint64_t ee_cycle;
    uint64_t body_size;
    uint64_t body_hash;
} StateHeader;

/* the program's image of globals that a state holds (port/platform/state_begin.c) */
extern char port_state_data_begin[], port_state_data_end[];
extern char port_state_bss_begin[], port_state_bss_end[];

/* ---- Requests --------------------------------------------------------------------------------- */
enum { REQUEST_NONE, REQUEST_SAVE, REQUEST_LOAD };

static volatile int request;      /* from any thread: what the next safe point does */
static volatile int request_slot;

typedef struct {
    uint64_t vblank;
    int slot;
    const char *path; /* a file instead of a slot */
} SavePoint;

/* --state-save-at, sorted, and the next one due */
static SavePoint save_at[MAX_SAVE_AT];
static int save_at_count;
static int save_at_next;
/* --state-load */
static int load_pending;          /* not done yet */
static uint64_t load_vblank;      /* when (with load_at_vblank; else before the game starts) */
static int load_at_vblank;
static int load_slot;
static const char *load_path;

static int started;
static char message[512] = "no save state taken or loaded yet";

static void set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_message(const char *fmt, ...) {
    char text[sizeof(message)];
    va_list args;
    uint32_t mode = port_host_float_begin(); /* (printf rounds as MXCSR says) */

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    port_host_float_end(mode);
    /* (a request that waits says so once, not at every vblank) */
    if (strcmp(text, message) != 0) {
        memcpy(message, text, sizeof(message));
        fprintf(stderr, "[state] %s\n", message);
    }
}

void port_state_request_save(int slot) {
    __atomic_store_n(&request_slot, slot, __ATOMIC_RELAXED);
    __atomic_store_n(&request, REQUEST_SAVE, __ATOMIC_RELEASE);
}

static char request_path[1100]; /* (a save to a file: port_state_request_save_file) */

void port_state_request_save_file(const char *path) {
    snprintf(request_path, sizeof(request_path), "%s", path);
    __atomic_store_n(&request_slot, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&request, REQUEST_SAVE, __ATOMIC_RELEASE);
}

void port_state_request_load(int slot) {
    __atomic_store_n(&request_slot, slot, __ATOMIC_RELAXED);
    __atomic_store_n(&request, REQUEST_LOAD, __ATOMIC_RELEASE);
}

const char *port_state_message(void) {
    return message;
}

int port_state_wanted(void) {
    uint64_t vblank = port_vblank_count();

    return __atomic_load_n(&request, __ATOMIC_ACQUIRE) != REQUEST_NONE ||
           (save_at_next < save_at_count && save_at[save_at_next].vblank <= vblank) ||
           (load_pending && load_at_vblank && load_vblank <= vblank);
}

/* ---- Where states go ------------------------------------------------------------------------------
   <the user's data folder>/<the port's folder>/states/slot<N>.kf4state: %APPDATA%\holyland on Windows,
   $XDG_DATA_HOME/holyland (or ~/.local/share/holyland) elsewhere: the folders of
   port/platform/data_dir.c, which also moves them from their old name (darker-reality). */
static void make_folder(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void state_folder(char *out, size_t size, int create) {
    const char *parts[] = {"holyland", "states"};
    const char *data_dir = getenv("KF4_DATA_DIR");
    size_t i;

    /* KF4_DATA_DIR moves the settings (port/overlay/settings.c) and the states together */
    if (data_dir != NULL && *data_dir) {
        snprintf(out, size, "%s", data_dir);
        if (create) {
            make_folder(out);
        }
        i = strlen(out);
        snprintf(out + i, size - i, "/states");
        if (create) {
            make_folder(out);
        }
        return;
    }
#ifdef _WIN32
    const char *base = getenv("APPDATA");

    snprintf(out, size, "%s", base != NULL && *base ? base : ".");
#else
    const char *base = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (base != NULL && *base) {
        snprintf(out, size, "%s", base);
    } else {
        snprintf(out, size, "%s/.local/share", home != NULL && *home ? home : ".");
        if (create) {
            char folder[1024];

            snprintf(folder, sizeof(folder), "%s/.local", home != NULL && *home ? home : ".");
            make_folder(folder);
            make_folder(out);
        }
    }
#endif
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        size_t length = strlen(out);

        snprintf(out + length, size - length, "/%s", parts[i]);
        if (create) {
            make_folder(out);
        }
    }
}

static void slot_path(char *out, size_t size, int slot, int create) {
    size_t length;

    state_folder(out, size, create);
    length = strlen(out);
    snprintf(out + length, size - length, "/slot%d.kf4state", slot);
}

static int read_header(const char *path, StateHeader *header) {
    FILE *file = fopen(path, "rb");
    int ok;

    if (file == NULL) {
        return 0;
    }
    ok = fread(header, sizeof(*header), 1, file) == 1 && memcmp(header->magic, STATE_MAGIC, 8) == 0;
    fclose(file);
    return ok;
}

int port_state_exists(int slot) {
    char path[1024];
    StateHeader header;

    slot_path(path, sizeof(path), slot, 0);
    return read_header(path, &header);
}

long long port_state_time(int slot) {
    char path[1024];
    StateHeader header;

    slot_path(path, sizeof(path), slot, 0);
    return read_header(path, &header) ? (long long)header.time : 0;
}

/* ---- The build id: a hash of the program's code and of where its sections are --------------- */
static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
    const uint8_t *p = data;
    size_t i = 0;

    for (; i + 8 <= size; i += 8) {
        uint64_t word;

        memcpy(&word, p + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    for (; i < size; i++) {
        hash = (hash ^ p[i]) * 0x100000001B3ull;
    }
    return hash;
}

#ifdef _WIN32
static uint64_t compute_build_id(void) {
    unsigned char *image = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(image + dos->e_lfanew);
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    uint64_t hash = 0xCBF29CE484222325ull;
    uintptr_t base = (uintptr_t)image;
    int i;

    hash = hash_bytes(hash, &base, sizeof(base));
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        hash = hash_bytes(hash, section->Name, sizeof(section->Name));
        hash = hash_bytes(hash, &section->VirtualAddress, sizeof(section->VirtualAddress));
        hash = hash_bytes(hash, &section->Misc.VirtualSize, sizeof(section->Misc.VirtualSize));
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            hash = hash_bytes(hash, image + section->VirtualAddress, section->Misc.VirtualSize);
        }
    }
    return hash;
}
#else
static int hash_program(struct dl_phdr_info *info, size_t size, void *data) {
    uint64_t *hash = data;
    int i;

    (void)size;
    /* the first object listed is the program itself */
    *hash = hash_bytes(*hash, &info->dlpi_addr, sizeof(info->dlpi_addr));
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];

        *hash = hash_bytes(*hash, ph, sizeof(*ph));
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) && !(ph->p_flags & PF_W)) {
            *hash = hash_bytes(*hash, (const void *)(info->dlpi_addr + ph->p_vaddr), ph->p_memsz);
        }
    }
    return 1;
}

static uint64_t compute_build_id(void) {
    uint64_t hash = 0xCBF29CE484222325ull;

    dl_iterate_phdr(hash_program, &hash);
    return hash;
}
#endif

static uint64_t build_id(void) {
    static uint64_t id;

    if (id == 0) {
        id = compute_build_id();
    }
    return id;
}

/* ---- Why there can be no state at all in this run (NULL: there can) ---------------------------- */
static const char *unavailable(void) {
    if (!port_memory_fixed()) {
        return "save states are off: game RAM is not at its fixed address";
    }
    if (!port_fiber_fixed()) {
        return "save states are off: the EE threads' stacks are not at their fixed address";
    }
    if ((uintptr_t)port_state_data_end <= (uintptr_t)port_state_data_begin ||
        (uintptr_t)port_state_bss_end <= (uintptr_t)port_state_bss_begin) {
        return "save states are off: the program's globals were not linked between their markers";
    }
    return NULL;
}

/* ---- The state, in memory --------------------------------------------------------------------- */
typedef struct {
    PortStateIo io;
    uint8_t *data;
    size_t size;     /* saving: bytes so far; loading: the whole state */
    size_t capacity;
    size_t at;       /* loading: the next byte */
} StateBuffer;

static void buffer_bytes(PortStateIo *io, void *data, size_t size) {
    StateBuffer *b = (StateBuffer *)io;

    if (io->loading) {
        if (size > b->size - b->at) {
            PORT_FATAL("the save state ends early");
        }
        if (data != NULL) {
            state_copy(data, b->data + b->at, size);
        }
        b->at += size;
        return;
    }
    if (size > b->capacity - b->size) {
        size_t capacity = b->capacity ? b->capacity : 0x6000000;

        while (size > capacity - b->size) {
            capacity *= 2;
        }
        b->data = realloc(b->data, capacity);
        if (b->data == NULL) {
            PORT_FATAL("out of memory for a save state of %zu MB", capacity >> 20);
        }
        b->capacity = capacity;
    }
    if (data != NULL) {
        state_copy(b->data + b->size, data, size);
    } else {
        memset(b->data + b->size, 0, size);
    }
    b->size += size;
}

/* A part's name before its state, checked on loading: a state that does not line up stops here */
static void section(PortStateIo *io, const char *name) {
    char tag[4];

    memcpy(tag, name, 4);
    PORT_STATE(io, tag);
    if (io->loading && memcmp(tag, name, 4) != 0) {
        PORT_FATAL("the save state is out of step at its part \"%.4s\"", name);
    }
}

/* The program's image of the game's and ico-audio's globals */
static void image_state(PortStateIo *io) {
    port_state_bytes(io, port_state_data_begin, (size_t)((uintptr_t)port_state_data_end - (uintptr_t)port_state_data_begin));
    port_state_bytes(io, port_state_bss_begin, (size_t)((uintptr_t)port_state_bss_end - (uintptr_t)port_state_bss_begin));
}

static void sync_all(PortStateIo *io) {
    section(io, "MEM ");
    port_memory_state(io);
    section(io, "IMG ");
    image_state(io);
    section(io, "FIBR");
    port_fiber_state(io);
    section(io, "KERN");
    port_kernel_state(io);
    section(io, "DMAC");
    port_dmac_state(io);
    section(io, "VIF1");
    port_vif1_state(io);
    section(io, "VU1 ");
    port_vu1_state(io);
    section(io, "GS  ");
    port_gs_state(io);
    section(io, "GRPH");
    port_libgraph_state(io);
    section(io, "PAD ");
    port_pad_state(io);
    section(io, "CDVD");
    port_cdvd_state(io);
    section(io, "SIF ");
    port_sif_state(io);
    section(io, "LIBC");
    port_libc_state(io);
    section(io, "IOP ");
    port_state_bytes(io, port_iop_ram(), IOP_RAM_SIZE);
    section(io, "SD  ");
    port_sd_state(io);
    section(io, "FSND");
    port_fsiopsnd_state(io);
    section(io, "MIDI");
    port_modmidi_state(io);
    section(io, "HSYN");
    port_modhsyn_state(io);
    section(io, "AUDO");
    port_audio_state(io);
    section(io, "END ");
}

/* ---- On the DMA controller's thread ------------------------------------------------------------- */
typedef struct {
    StateBuffer buffer;
    const char *busy;     /* save: why not now */
    uint64_t spu2_frame;  /* the SPU2's time in the state (the WAV dump's frame, on the game's clock) */
} Job;

static void save_on_controller(void *arg) {
    Job *job = arg;

    job->busy = port_gs_state_busy();
    if (job->busy != NULL) {
        return;
    }
    port_audio_lock();
    sync_all(&job->buffer.io);
    job->spu2_frame = spu2_time();
    port_audio_unlock();
    memset(&port_stats, 0, sizeof(port_stats));
}

static void load_on_controller(void *arg) {
    Job *job = arg;

    port_audio_lock();
    sync_all(&job->buffer.io);
    job->spu2_frame = spu2_time();
    port_audio_unlock();
    if (job->buffer.at != job->buffer.size) {
        PORT_FATAL("the save state has %zu bytes more than this program reads", job->buffer.size - job->buffer.at);
    }
    memset(&port_stats, 0, sizeof(port_stats));
}

/* LZ4's block format: port/platform/lz4_block.c */

/* ---- The file ----------------------------------------------------------------------------------- */
static int write_state(const char *path, const StateBuffer *buffer) {
    char temp[1100];
    StateHeader header;
    FILE *file;
    uint8_t *packed;
    uint32_t *table;
    size_t at;
    int ok = 1;

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, STATE_MAGIC, 8);
    header.version = STATE_VERSION;
    header.header_size = sizeof(header);
    header.build_id = build_id();
    header.time = (int64_t)time(NULL);
    header.vblank = port_vblank_count();
    header.ee_cycle = port_ee_cycles();
    header.body_size = buffer->size;
    header.body_hash = hash_bytes(0xCBF29CE484222325ull, buffer->data, buffer->size);

    snprintf(temp, sizeof(temp), "%s.part", path);
    file = fopen(temp, "wb");
    if (file == NULL) {
        return 0;
    }
    packed = malloc(BLOCK_SIZE + BLOCK_SIZE / 255 + 16);
    table = malloc(sizeof(uint32_t) << LZ_HASH_BITS);
    if (packed == NULL || table == NULL) {
        PORT_FATAL("out of memory for a save state");
    }
    ok = fwrite(&header, sizeof(header), 1, file) == 1;
    for (at = 0; ok && at < buffer->size; at += BLOCK_SIZE) {
        uint32_t sizes[2];
        size_t n = buffer->size - at < BLOCK_SIZE ? buffer->size - at : BLOCK_SIZE;
        size_t packed_size = lz_compress(buffer->data + at, n, packed, table);
        const uint8_t *data = packed;

        if (packed_size >= n) {
            packed_size = n;
            data = buffer->data + at;
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
    if (ok) {
        remove(path);
        ok = rename(temp, path) == 0;
    }
    if (!ok) {
        remove(temp);
    }
    return ok;
}

/* Reads a state into buffer, checked; NULL, or why it cannot be loaded */
static const char *read_state(const char *path, StateHeader *header, StateBuffer *buffer) {
    FILE *file = fopen(path, "rb");
    uint8_t *packed = NULL;
    size_t at;
    const char *error = NULL;

    if (file == NULL) {
        return "there is no such state";
    }
    if (fread(header, sizeof(*header), 1, file) != 1 || memcmp(header->magic, STATE_MAGIC, 8) != 0) {
        error = "it is not a save state";
    } else if (header->version != STATE_VERSION || header->header_size != sizeof(*header)) {
        error = "it is a save state of another version";
    } else if (header->build_id != build_id()) {
        error = "it was saved by another build of the program (a state loads only in the build that saved it)";
    } else if ((buffer->data = malloc(header->body_size ? header->body_size : 1)) == NULL ||
               (packed = malloc(BLOCK_SIZE)) == NULL) {
        error = "out of memory";
    }
    for (at = 0; error == NULL && at < header->body_size; at += BLOCK_SIZE) {
        uint32_t sizes[2];
        size_t n = header->body_size - at < BLOCK_SIZE ? header->body_size - at : BLOCK_SIZE;

        if (fread(sizes, sizeof(sizes), 1, file) != 1 || sizes[0] != n || sizes[1] > n ||
            fread(packed, 1, sizes[1], file) != sizes[1]) {
            error = "the file is cut short or damaged";
        } else if (sizes[1] == n) {
            memcpy(buffer->data + at, packed, n);
        } else if (!lz_decompress(packed, sizes[1], buffer->data + at, n)) {
            error = "the file is damaged";
        }
    }
    if (error == NULL && hash_bytes(0xCBF29CE484222325ull, buffer->data, header->body_size) != header->body_hash) {
        error = "the file is damaged";
    }
    free(packed);
    fclose(file);
    if (error != NULL) {
        free(buffer->data);
        buffer->data = NULL;
        return error;
    }
    buffer->size = header->body_size;
    buffer->at = 0;
    return NULL;
}

/* ---- Saving and loading, on the host's fiber ------------------------------------------------------ */
static void describe(char *out, size_t size, int slot, const char *path) {
    if (path != NULL) {
        snprintf(out, size, "%s", path);
    } else {
        snprintf(out, size, "slot %d", slot);
    }
}

/* 1 when done (or given up), 0 when it has to wait for a later vblank */
static int save(int slot, const char *file) {
    char path[1024];
    char name[1100];
    const char *why = unavailable();
    double start = port_wall_seconds();
    Job job;

    describe(name, sizeof(name), slot, file);
    if (why != NULL) {
        set_message("%s", why);
        return 1;
    }
    why = port_sif_state_busy();
    if (why == NULL) {
        why = port_mpeg_state_busy();
    }
    if (why != NULL) {
        set_message("waiting to save to %s: %s", name, why);
        return 0;
    }
    memset(&job, 0, sizeof(job));
    job.buffer.io.bytes = buffer_bytes;
    port_dmac_call(save_on_controller, &job);
    if (job.busy != NULL) {
        set_message("waiting to save to %s: %s", name, job.busy);
        return 0;
    }
    if (file != NULL) {
        snprintf(path, sizeof(path), "%s", file);
    } else {
        slot_path(path, sizeof(path), slot, 1);
    }
    if (!write_state(path, &job.buffer)) {
        set_message("cannot write the state to %s", path);
    } else {
        port_event("state saved to %s", name);
        set_message("saved to %s at vblank %llu (%.1f MB, SPU2 frame %llu, %.0f ms)", name,
                    (unsigned long long)port_vblank_count(), (double)job.buffer.size / 1048576.0,
                    (unsigned long long)job.spu2_frame, (port_wall_seconds() - start) * 1000.0);
    }
    free(job.buffer.data);
    return 1;
}

/* 1 when the state was loaded */
static int load(int slot, const char *file) {
    char path[1024];
    char name[1100];
    const char *why = unavailable();
    double start = port_wall_seconds();
    StateHeader header;
    Job job;

    describe(name, sizeof(name), slot, file);
    if (why != NULL) {
        set_message("%s", why);
        return 0;
    }
    if (file != NULL) {
        snprintf(path, sizeof(path), "%s", file);
    } else {
        slot_path(path, sizeof(path), slot, 0);
    }
    memset(&job, 0, sizeof(job));
    job.buffer.io.bytes = buffer_bytes;
    job.buffer.io.loading = 1;
    why = read_state(path, &header, &job.buffer);
    if (why != NULL) {
        set_message("cannot load %s: %s", name, why);
        return 0;
    }
    port_dmac_call(load_on_controller, &job);
    free(job.buffer.data);
    /* the save points before the state's vblank have passed */
    while (save_at_next < save_at_count && save_at[save_at_next].vblank < port_vblank_count()) {
        save_at_next++;
    }
    port_event("state loaded from %s", name);
    set_message("loaded %s, saved at vblank %llu (SPU2 frame %llu, %.0f ms)", name,
                (unsigned long long)header.vblank, (unsigned long long)job.spu2_frame,
                (port_wall_seconds() - start) * 1000.0);
    return 1;
}

void port_state_host(void) {
    int kind;

    if (!started) {
        started = 1;
        if (load_pending && !load_at_vblank) {
            load_pending = 0;
            if (!load(load_slot, load_path)) {
                exit(2);
            }
        }
    }
    if (load_pending && load_at_vblank && load_vblank <= port_vblank_count()) {
        load_pending = 0;
        load(load_slot, load_path);
    }
    kind = __atomic_load_n(&request, __ATOMIC_ACQUIRE);
    if (kind == REQUEST_LOAD) {
        __atomic_store_n(&request, REQUEST_NONE, __ATOMIC_RELEASE);
        load(__atomic_load_n(&request_slot, __ATOMIC_RELAXED), NULL);
    } else if (kind == REQUEST_SAVE) {
        int slot = __atomic_load_n(&request_slot, __ATOMIC_RELAXED);

        if (save(slot, slot < 0 ? request_path : NULL)) {
            __atomic_compare_exchange_n(&request, &kind, REQUEST_NONE, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        }
    }
    while (save_at_next < save_at_count && save_at[save_at_next].vblank <= port_vblank_count()) {
        if (!save(save_at[save_at_next].slot, save_at[save_at_next].path)) {
            break;
        }
        save_at_next++;
    }
}

/* ---- The command line ------------------------------------------------------------------------- */
/* SLOT or FILE: a number is a slot */
static void parse_target(const char *text, int *slot, const char **path) {
    char *end;
    long value = strtol(text, &end, 10);

    if (*text != 0 && *end == 0 && value >= 0) {
        *slot = (int)value;
        *path = NULL;
    } else {
        *slot = -1;
        *path = text;
    }
}

static int compare_save_points(const void *a, const void *b) {
    const SavePoint *x = a, *y = b;

    return x->vblank < y->vblank ? -1 : x->vblank > y->vblank;
}

void port_state_init(void) {
    const char *why;

    if (port_options.state_save_at != NULL) {
        /* VBLANK[:SLOT|:FILE], comma separated */
        char *list = malloc(strlen(port_options.state_save_at) + 1);
        char *item;

        strcpy(list, port_options.state_save_at);
        for (item = strtok(list, ","); item != NULL; item = strtok(NULL, ",")) {
            SavePoint *point = &save_at[save_at_count];
            char *end;

            if (save_at_count == MAX_SAVE_AT) {
                fprintf(stderr, "--state-save-at: at most %d save points\n", MAX_SAVE_AT);
                exit(2);
            }
            point->vblank = strtoull(item, &end, 10);
            if (end == item || (*end != 0 && *end != ':')) {
                fprintf(stderr, "--state-save-at wants VBLANK[:SLOT|:FILE], not %s\n", item);
                exit(2);
            }
            point->slot = 0;
            point->path = NULL;
            if (*end == ':') {
                parse_target(end + 1, &point->slot, &point->path);
            }
            save_at_count++;
        }
        qsort(save_at, (size_t)save_at_count, sizeof(save_at[0]), compare_save_points);
    }
    if (port_options.state_load != NULL) {
        const char *text = port_options.state_load;
        char *end;
        unsigned long long vblank = strtoull(text, &end, 10);

        /* [VBLANK:]SLOT|FILE */
        if (end != text && *end == ':') {
            load_at_vblank = 1;
            load_vblank = vblank;
            text = end + 1;
        }
        parse_target(text, &load_slot, &load_path);
        load_pending = 1;
    }
    why = unavailable();
    if (why != NULL && (save_at_count > 0 || load_pending)) {
        fprintf(stderr, "[state] %s\n", why);
        exit(2);
    }
}
