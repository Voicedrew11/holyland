/* Quick saves (port/include/port/quicksave.h): the host's side. The requests (keys, the overlay, the
   command line), the files, and the messages. The game's side is game/quicksave.c.

   The file, <data folder>/quicksaves/quick<N>.kf4quick, every number little-endian whatever the
   build (written and read byte by byte, never as a C struct):
     0   "KF4QUICK"
     8   u32 format version (QUICK_VERSION)
     12  u32 header size (QUICK_HEADER_SIZE): what follows it is the PC block
     16  u32 the save image's version word (0x15400, the game's own: its first word)
     20  u32 the save image's size in bytes (its blocks and their byte sums)
     24  u32 the PC block's size
     28  u32 0
     32  u64 FNV-1a hash of the PC block and the image
     40  the PC block: i64 time, i32 map, f32 x y z, f32 look heading roll, i32 hp hp_max mp mp_max
         level gold, u32 play seconds (64 bytes: the first files' whole block); then u16 the
         neighbouring map (0xFFFF: none), u8 the current map's origin that leads to it (0xFF: none),
         u8 flags: bit 0 it was set up, not only read, bit 1 the file was made by a tool
         (tools/map_tour.py), not by the game: the neighbour is worked out from the position, and
         the player put on the floor there (68 bytes); then u8 how many maps' live worlds follow (0 to 2),
         3 bytes 0, and per map (140 bytes): u8 its tag, u8 its spawn count, u8 its
         fixed item count, u8 0, u64 a bit per fixed item lying in it, u8[128] PORT_QUICK_SPAWN_ bits
         per spawn (PORT_QUICK_INFO_SIZE, 352 bytes, packed by port/platform/quick_info.c; a file without them loads with the world made afresh,
         as the game makes it when it sets a map up). The block's size
         is the format's minor version: a later one may append more, and a reader takes what it knows
         and skips the rest, so a file of any block size loads in every build of format version 1 (a
         64-byte one with no neighbour known: the game works it out from the position).
         then the save image, exactly as Game_WriteSaveImage made it (the bytes a save point's save holds).

   Beside a slot's file, quick<N>.kf4state: the save state taken at the vblank after it (port/platform/
   savestate.c), which a load uses when this build made it (port_quick_load_exact). A save removes the old
   state first, so a state is never older than its image. The history's copy of a slot's save gets a copy of
   that state beside it too (history-<n>.kf4state), and a load from any file uses the state beside it the same
   way. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "port/platform.h"
#include "port/file_replace.h"
#include "port/automap.h"
#include "port/hands.h"
#include "port/quick_history.h"
#include "port/quick_info.h"
#include "port/quicksave.h"
#include "port/savestate.h"
#include "port/settings.h"

#define QUICK_MAGIC "KF4QUICK"
#define QUICK_VERSION 1u
#define QUICK_HEADER_SIZE 40u
#define QUICK_INFO_SIZE PORT_QUICK_INFO_SIZE
#define QUICK_INFO_SIZE_MIN PORT_QUICK_INFO_SIZE_MIN
#define QUICK_IMAGE_MAGIC 0x15400u
#define QUICK_IMAGE_MAX 0x20000u
#define REQUEST_VBLANKS 60 /* a key's request that the game has not taken by then is dropped */
#define MAX_SAVE_AT 16
#define MAX_LOAD_AT 64

typedef struct {
    int kind;          /* PORT_QUICK_ */
    int slot;          /* or a file: */
    const char *path;
    int patient;       /* the command line's: waits until the game can, and is never dropped */
    uint64_t asked_at; /* the vblank it was asked at */
    /* a teleport's (port/include/port/teleport.h): where to */
    int map;
    float position[3];
    float heading;
    char label[32];
} Request;

static Request request;
static char message[512] = "no quick save taken or loaded yet";
static PortQuickInfo expected; /* what the last load should give */

typedef struct {
    uint64_t vblank;
    int slot;
    const char *path;
} SaveAt;
static SaveAt save_at[MAX_SAVE_AT];
static int save_at_count, save_at_next;
/* --quick-load: one or more, in vblank order */
static SaveAt load_at[MAX_LOAD_AT];
static int load_at_count, load_at_next;

static void set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_message(const char *fmt, ...) {
    char text[sizeof(message)];
    va_list args;
    uint32_t mode = port_host_float_begin(); /* (printf rounds as MXCSR says) */

    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    port_host_float_end(mode);
    if (strcmp(text, message) != 0) {
        memcpy(message, text, sizeof(message));
        fprintf(stderr, "[quick] %s\n", message);
    }
}

const char *port_quick_message(void) {
    return message;
}

int port_quick_slot(void) {
    return port_settings.quick_slot >= 0 && port_settings.quick_slot < PORT_QUICK_SLOTS ? port_settings.quick_slot : 0;
}

/* ---- Where they go ---------------------------------------------------------------------------- */
static void make_folder(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void slot_path(char *out, size_t size, int slot, int create) {
    if (create) {
        make_folder(port_data_dir());
    }
    snprintf(out, size, "%s/quicksaves", port_data_dir());
    if (create) {
        make_folder(out);
    }
    snprintf(out + strlen(out), size - strlen(out), "/quick%d.kf4quick", slot);
}

/* a quick save's exact state, beside its file: name.kf4quick's is name.kf4state */
static void state_beside(char *out, size_t size, const char *quick_path) {
    static const char suffix[] = ".kf4quick";
    size_t length = strlen(quick_path), stem = length;

    if (length >= sizeof(suffix) - 1 && strcmp(quick_path + length - (sizeof(suffix) - 1), suffix) == 0) {
        stem = length - (sizeof(suffix) - 1);
    }
    snprintf(out, size, "%.*s.kf4state", (int)stem, quick_path);
}

static void target_path(char *out, size_t size, const Request *r, int create) {
    if (r->path != NULL) {
        snprintf(out, size, "%s", r->path);
    } else {
        slot_path(out, size, r->slot, create);
    }
}

static void request_name(char *out, size_t size, const Request *r) {
    if (r->kind == PORT_QUICK_TELEPORT) {
        snprintf(out, size, "to map %03d (%s)", r->map, r->label);
    } else if (r->path != NULL) {
        snprintf(out, size, "%s", r->path);
    } else {
        snprintf(out, size, "quick save %d", r->slot);
    }
}

/* ---- Little-endian bytes ---------------------------------------------------------------------- */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static uint64_t hash_bytes(uint64_t h, const uint8_t *data, size_t size) {
    size_t i;

    for (i = 0; i < size; i++) {
        h = (h ^ data[i]) * 0x100000001B3ull;
    }
    return h;
}

/* Checks a header: NULL, or why the file is not one this program reads */
static const char *check_header(const uint8_t *h, uint32_t *image_size, uint32_t *info_size) {
    if (memcmp(h, QUICK_MAGIC, 8) != 0) {
        return "it is not a quick save";
    }
    if (get32(h + 8) != QUICK_VERSION || get32(h + 12) != QUICK_HEADER_SIZE) {
        return "it is a quick save of another version of the format";
    }
    if (get32(h + 16) != QUICK_IMAGE_MAGIC) {
        return "its save image is of another version of the game";
    }
    *image_size = get32(h + 20);
    *info_size = get32(h + 24);
    if (*image_size < 0x100 || *image_size > QUICK_IMAGE_MAX || *info_size < QUICK_INFO_SIZE_MIN || *info_size > 0x10000) {
        return "the file is damaged";
    }
    return NULL;
}

/* ---- Reading ---------------------------------------------------------------------------------- */
/* The whole file checked: the image (malloc'd) and the info, or NULL and why */
static uint8_t *read_file(const char *path, size_t *size, PortQuickInfo *info, const char **why) {
    FILE *file = fopen(path, "rb");
    uint8_t header[QUICK_HEADER_SIZE];
    uint32_t image_size, info_size;
    uint8_t *body = NULL, *image;

    *why = NULL;
    if (file == NULL) {
        *why = "there is no such quick save";
        return NULL;
    }
    if (fread(header, sizeof(header), 1, file) != 1) {
        *why = "it is not a quick save";
    } else {
        *why = check_header(header, &image_size, &info_size);
    }
    if (*why == NULL) {
        body = malloc((size_t)info_size + image_size);
        if (body == NULL) {
            *why = "out of memory";
        } else if (fread(body, 1, (size_t)info_size + image_size, file) != (size_t)info_size + image_size) {
            *why = "the file is cut short";
        } else if (hash_bytes(0xCBF29CE484222325ull, body, (size_t)info_size + image_size) != get64(header + 32)) {
            *why = "the file is damaged (its hash does not match)";
        }
    }
    fclose(file);
    if (*why != NULL) {
        free(body);
        return NULL;
    }
    port_quick_info_unpack(body, info_size, info);
    image = malloc(image_size);
    if (image == NULL) {
        free(body);
        *why = "out of memory";
        return NULL;
    }
    memcpy(image, body + info_size, image_size);
    free(body);
    *size = image_size;
    return image;
}

int port_quick_info(int slot, PortQuickInfo *info) {
    char path[1024];

    slot_path(path, sizeof(path), slot, 0);
    return port_quick_file_info(path, info);
}

int port_quick_file_info(const char *path, PortQuickInfo *info) {
    uint8_t head[QUICK_HEADER_SIZE + QUICK_INFO_SIZE_MIN]; /* what the menu lists */
    uint32_t image_size, info_size;
    FILE *file;
    int ok;

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    ok = fread(head, sizeof(head), 1, file) == 1 && check_header(head, &image_size, &info_size) == NULL;
    fclose(file);
    if (ok) {
        port_quick_info_unpack(head + QUICK_HEADER_SIZE, QUICK_INFO_SIZE_MIN, info);
    }
    return ok;
}

/* ---- Requests --------------------------------------------------------------------------------- */
static const char *verb(int kind) {
    return kind == PORT_QUICK_SAVE ? "save" : kind == PORT_QUICK_LOAD ? "load" : "teleport";
}

static void ask(int kind, int slot, const char *path, int patient) {
    char name[1100];

    request.kind = kind;
    request.slot = slot;
    request.path = path;
    request.patient = patient;
    request.asked_at = port_vblank_count();
    request_name(name, sizeof(name), &request);
    set_message("%s %s asked for", kind == PORT_QUICK_SAVE ? "saving" : kind == PORT_QUICK_LOAD ? "loading" : "teleporting",
                name);
}

void port_quick_request_save(int slot) {
    ask(PORT_QUICK_SAVE, slot, NULL, 0);
}

void port_quick_request_load(int slot) {
    ask(PORT_QUICK_LOAD, slot, NULL, 0);
}

void port_quick_request_save_file(const char *path) {
    static char kept[1100];

    snprintf(kept, sizeof(kept), "%s", path);
    ask(PORT_QUICK_SAVE, -1, kept, 0);
}

void port_quick_request_load_file(const char *path) {
    static char kept[1100];

    snprintf(kept, sizeof(kept), "%s", path);
    ask(PORT_QUICK_LOAD, -1, kept, 0);
}

void port_quick_request_teleport(int map, const float position[3], float heading, const char *label, int patient) {
    request.map = map;
    memcpy(request.position, position, sizeof(request.position));
    request.heading = heading;
    snprintf(request.label, sizeof(request.label), "%s", label);
    ask(PORT_QUICK_TELEPORT, -1, NULL, patient);
}

void port_quick_teleport_target(int *map, float position[3], float *heading) {
    *map = request.map;
    memcpy(position, request.position, sizeof(request.position));
    *heading = request.heading;
}

int port_quick_wanted(void) {
    return request.kind;
}

void port_quick_refuse(const char *why) {
    char name[1100];

    request_name(name, sizeof(name), &request);
    if (request.patient) {
        set_message("waiting to %s %s: %s", verb(request.kind), name, why);
        return;
    }
    set_message("cannot %s %s now: %s", verb(request.kind), name, why);
    request.kind = PORT_QUICK_NONE;
}

void port_quick_vblank(void) {
    uint64_t vblank = port_vblank_count();

    kf4_quick_vblank();

    if (request.kind != PORT_QUICK_NONE && !request.patient && vblank - request.asked_at > REQUEST_VBLANKS) {
        port_quick_refuse(request.kind == PORT_QUICK_SAVE
                              ? "not in play (a menu, the map or a loading screen; quick saves are taken in play)"
                              : "the game cannot load from here (quick loads are taken in play and on the title "
                                "screen)");
    }
    if (request.kind == PORT_QUICK_NONE && save_at_next < save_at_count && save_at[save_at_next].vblank <= vblank) {
        ask(PORT_QUICK_SAVE, save_at[save_at_next].slot, save_at[save_at_next].path, 1);
        save_at_next++;
    }
    if (request.kind == PORT_QUICK_NONE && load_at_next < load_at_count && load_at[load_at_next].vblank <= vblank) {
        ask(PORT_QUICK_LOAD, load_at[load_at_next].slot, load_at[load_at_next].path, 1);
        load_at_next++;
    }
}

/* The file written whole beside its place, then put in place (the old one is kept if anything fails): 1 when written */
static int write_quick_file(const char *path, const uint8_t *header, const uint8_t *info, const uint8_t *image,
                            size_t size) {
    char temp[1100];
    FILE *file;
    int ok;

    snprintf(temp, sizeof(temp), "%s.part", path);
    file = fopen(temp, "wb");
    ok = file != NULL;
    if (ok) {
        ok = fwrite(header, QUICK_HEADER_SIZE, 1, file) == 1 && fwrite(info, QUICK_INFO_SIZE, 1, file) == 1 &&
             fwrite(image, 1, size, file) == size;
        ok = fclose(file) == 0 && ok;
    }
    if (ok) {
        ok = port_replace_file(temp, path);
    }
    if (!ok) {
        remove(temp);
    }
    return ok;
}

/* A slot's save kept in the history too (port/include/port/quick_history.h), with the mods' files. state_out: where
   the copy of the slot's exact state goes, beside the history's file ("" when it was not written). */
static void keep_in_history(const uint8_t *header, const uint8_t *info, const uint8_t *image, size_t size,
                            char *state_out, size_t state_size) {
    char path[1024];

    state_out[0] = 0;
    port_quick_history_next_path(path, sizeof(path));
    state_beside(state_out, state_size, path);
    remove(state_out); /* (a stale one left by a file removed by hand) */
    if (!write_quick_file(path, header, info, image, size)) {
        port_event("quick history: cannot write %s", path);
        state_out[0] = 0;
        return;
    }
    port_automap_quick_saved(path);
    port_hands_quick_saved(path);
    port_quick_history_added();
}

int port_quick_write(const uint8_t *image, size_t size, const PortQuickInfo *game_info) {
    char path[1024], name[1100], state[1100], history_state[1100];
    uint8_t header[QUICK_HEADER_SIZE];
    uint8_t info[QUICK_INFO_SIZE];
    PortQuickInfo stamped = *game_info;
    int slot = request.path == NULL ? request.slot : -1;

    request_name(name, sizeof(name), &request);
    request.kind = PORT_QUICK_NONE;
    if (size < 0x100 || size > QUICK_IMAGE_MAX || get32(image) != QUICK_IMAGE_MAGIC) {
        set_message("cannot save %s: the game's save image is not as expected", name);
        return 0;
    }
    stamped.time = (int64_t)time(NULL);
    port_quick_info_pack(info, &stamped);
    memset(header, 0, sizeof(header));
    memcpy(header, QUICK_MAGIC, 8);
    put32(header + 8, QUICK_VERSION);
    put32(header + 12, QUICK_HEADER_SIZE);
    put32(header + 16, QUICK_IMAGE_MAGIC);
    put32(header + 20, (uint32_t)size);
    put32(header + 24, QUICK_INFO_SIZE);
    put64(header + 32, hash_bytes(hash_bytes(0xCBF29CE484222325ull, info, sizeof(info)), image, size));

    target_path(path, sizeof(path), &request, 1);
    state_beside(state, sizeof(state), path);
    remove(state); /* (none yet is fine) never left older than the image */
    if (!write_quick_file(path, header, info, image, size)) {
        set_message("cannot write %s", path);
        return 0;
    }
    port_automap_quick_saved(path); /* the automap mod: the explored maps kept with the save */
    port_hands_quick_saved(path); /* the hands mod: the hands kept with the save */
    if (slot >= 0) {
        keep_in_history(header, info, image, size, history_state, sizeof(history_state));
        /* at the next vblank: the exact state, and its copy in the history */
        port_state_request_save_file_and_copy(state, history_state);
    }
    port_event("quick saved to %s: map %02X pos %.2f,%.2f,%.2f heading %.3f look %.3f hp %d mp %d neighbour %02X "
               "(origin %d, %s)", name, stamped.map_tag, stamped.position[0], stamped.position[1], stamped.position[2],
               stamped.rotation[1], stamped.rotation[0], stamped.hp, stamped.mp, stamped.neighbour_map & 0xFFFF,
               stamped.neighbour_origin, stamped.neighbour_set_up ? "set up" : "read");
    set_message("saved %s: map %02X, level %d, HP %d/%d", name, stamped.map_tag, stamped.level, stamped.hp,
                stamped.hp_max);
    return 1;
}

uint8_t *port_quick_read(size_t *size, PortQuickInfo *info) {
    char path[1024], name[1100];
    const char *why;
    uint8_t *image;

    request_name(name, sizeof(name), &request);
    target_path(path, sizeof(path), &request, 0);
    image = read_file(path, size, info, &why);
    if (image == NULL) {
        set_message("cannot load %s: %s", name, why);
        request.kind = PORT_QUICK_NONE;
        return NULL;
    }
    expected = *info;
    port_automap_quick_loading(path); /* the automap mod: the save's explored maps brought back */
    port_hands_quick_loading(path); /* the hands mod: the save's hands brought back */
    return image;
}

int port_quick_load_exact(void) {
    char path[1024], state[1100], name[1100];
    PortQuickInfo info;
    long long state_time;
    const char *why;
    uint8_t *image;
    size_t size;

    if (request.kind != PORT_QUICK_LOAD) {
        return 0;
    }
    request_name(name, sizeof(name), &request);
    target_path(path, sizeof(path), &request, 0);
    state_beside(state, sizeof(state), path);
    why = port_state_file_usable(state, &state_time);
    if (why != NULL) {
        port_event("quick load of %s: no exact state (%s): the game loads its save image", name, why);
        return 0;
    }
    image = read_file(path, &size, &info, &why); /* the quick save must be whole too, and not newer */
    free(image);
    if (image == NULL || state_time < info.time) {
        port_event("quick load of %s: its exact state %s: the game loads its save image", name,
                   image == NULL ? "has no good quick save beside it" : "is older than the quick save");
        return 0;
    }
    request.kind = PORT_QUICK_NONE;
    expected = info;
    /* the mods' files live outside the state, as for an image load (port_quick_read) */
    port_automap_quick_loading(path); /* the automap mod: the save's explored maps brought back */
    port_hands_quick_loading(path); /* the hands mod: the save's hands brought back */
    port_state_request_load_file(state);
    port_event("quick load of %s: its exact state", name);
    set_message("loaded %s: map %02X, level %d, HP %d/%d", name, info.map_tag, info.level, info.hp, info.hp_max);
    return 1;
}

void port_quick_loaded(int ok) {
    char name[1100];

    request_name(name, sizeof(name), &request);
    request.kind = PORT_QUICK_NONE;
    if (!ok) {
        set_message("cannot load %s: the game's check of its save image failed", name);
        return;
    }
    port_event("quick load of %s: the game restarts from its save image", name);
    set_message("loading %s: map %02X, level %d", name, expected.map_tag, expected.level);
}

void port_quick_teleported(const PortQuickInfo *info) {
    char name[1100];

    request_name(name, sizeof(name), &request);
    request.kind = PORT_QUICK_NONE;
    expected = *info;
    port_event("teleport %s: pos %.2f,%.2f,%.2f heading %.3f: the game restarts on that map", name, info->position[0],
               info->position[1], info->position[2], info->rotation[1]);
    set_message("teleporting %s", name);
}

void port_quick_moved(const float position[3]) {
    memcpy(expected.position, position, sizeof(expected.position));
}

void port_quick_placed(const PortQuickInfo *now) {
    int i, exact = now->map_tag == expected.map_tag, near = exact;

    /* the check comes at the first vblank of play, which may be after the first frame's update: a
       held stick may have moved the player a step, and the ground may have hurt him */
    for (i = 0; i < 3; i++) {
        exact = exact && now->position[i] == expected.position[i] && now->rotation[i] == expected.rotation[i];
        near = near && fabsf(now->position[i] - expected.position[i]) < 64.0f &&
               fabsf(now->rotation[i] - expected.rotation[i]) < 0.2f;
    }
    port_event("quick loaded: map %02X pos %.2f,%.2f,%.2f heading %.3f look %.3f hp %d mp %d%s", now->map_tag,
               now->position[0], now->position[1], now->position[2], now->rotation[1], now->rotation[0], now->hp,
               now->mp, exact ? " (as saved)" : near ? " (a step from where it was saved)" : " (not where it was saved)");
    port_gs_interp_cut_reset();
    if (near) {
        set_message("loaded: map %02X, level %d, HP %d/%d", now->map_tag, now->level, now->hp, now->hp_max);
    } else {
        set_message("loaded, but the player is at map %02X %.1f,%.1f,%.1f, not map %02X %.1f,%.1f,%.1f", now->map_tag,
                    now->position[0], now->position[1], now->position[2], expected.map_tag, expected.position[0],
                    expected.position[1], expected.position[2]);
    }
}

/* ---- The command line ------------------------------------------------------------------------- */
/* SLOT or FILE: a number is a slot */
static void parse_target(const char *text, int *slot, const char **path) {
    char *end;
    long value = strtol(text, &end, 10);

    if (*text != 0 && *end == 0 && value >= 0 && value < PORT_QUICK_SLOTS) {
        *slot = (int)value;
        *path = NULL;
    } else {
        *slot = -1;
        *path = text;
    }
}

void port_quick_init(void) {
    if (port_options.quick_save_at != NULL) {
        char *list = malloc(strlen(port_options.quick_save_at) + 1);
        char *item;

        strcpy(list, port_options.quick_save_at);
        for (item = strtok(list, ","); item != NULL; item = strtok(NULL, ",")) {
            SaveAt *point = &save_at[save_at_count];
            char *end;

            if (save_at_count == MAX_SAVE_AT) {
                fprintf(stderr, "--quick-save-at: at most %d\n", MAX_SAVE_AT);
                exit(2);
            }
            point->vblank = strtoull(item, &end, 10);
            if (end == item || (*end != 0 && *end != ':') || (save_at_count > 0 && point->vblank < point[-1].vblank)) {
                fprintf(stderr, "--quick-save-at wants VBLANK[:SLOT|:FILE],... in order, not %s\n", item);
                exit(2);
            }
            point->slot = 0;
            point->path = NULL;
            if (*end == ':') {
                parse_target(end + 1, &point->slot, &point->path);
            }
            save_at_count++;
        }
    }
    if (port_options.quick_load != NULL) {
        /* [VBLANK:]SLOT|FILE,...: each waits for the one before it to be taken (a map tour loads
           one after another in one run) */
        char *list = malloc(strlen(port_options.quick_load) + 1);
        char *item;

        strcpy(list, port_options.quick_load);
        for (item = strtok(list, ","); item != NULL; item = strtok(NULL, ",")) {
            SaveAt *point = &load_at[load_at_count];
            const char *text = item;
            char *end;
            unsigned long long vblank = strtoull(text, &end, 10);

            if (load_at_count == MAX_LOAD_AT) {
                fprintf(stderr, "--quick-load: at most %d\n", MAX_LOAD_AT);
                exit(2);
            }
            point->vblank = 0;
            if (end != text && *end == ':') {
                point->vblank = vblank;
                text = end + 1;
            }
            if (load_at_count > 0 && point->vblank < point[-1].vblank) {
                fprintf(stderr, "--quick-load wants [VBLANK:]SLOT|FILE,... in order, not %s\n", item);
                exit(2);
            }
            parse_target(text, &point->slot, &point->path);
            load_at_count++;
        }
    }
}
