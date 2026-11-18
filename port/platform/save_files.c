/* Save files (port/include/port/save_files.h): the game's saves as files in the save folder. Plain C on
   the C library and the file system only, so tests/save_files_test.c builds it on its own. Runs on the
   game's thread (game/saves.c) and at start. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "port/save_files.h"

#define SAVE_MAGIC "KF4SAVE"
#define SAVE_VERSION 1u
#define SAVE_HEADER_SIZE 40u
#define SAVE_DATA_MIN 4u /* the data's first word is its size */
#define PATH_SIZE 1024

static char folder_path[PATH_SIZE]; /* "": no folder */
static char message[512] = "no save read or written yet";

static void set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_message(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    fprintf(stderr, "[saves] %s\n", message);
}

const char *port_saves_message(void) {
    return message;
}

int port_saves_available(void) {
    return folder_path[0] != '\0';
}

const char *port_saves_folder(void) {
    return folder_path;
}

/* ---- Bytes ------------------------------------------------------------------------------------ */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static uint64_t hash_bytes(uint64_t hash, const uint8_t *p, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        hash ^= p[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static uint64_t save_hash(const uint8_t *record, const uint8_t *data, size_t size) {
    return hash_bytes(hash_bytes(0xCBF29CE484222325ull, record, PORT_SAVE_RECORD_SIZE), data, size);
}

/* ---- Paths ------------------------------------------------------------------------------------ */
/* save<index+1>.kf4save, with `suffix` (".tmp", ".bak" or "") after it; 0 when it does not fit */
static int save_path(char *out, size_t size, int index, const char *suffix) {
    int n = snprintf(out, size, "%s/save%d.kf4save%s", folder_path, index + 1, suffix);

    return n > 0 && (size_t)n < size;
}

static int file_exists(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

static int is_folder(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int make_one_folder(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

/* Makes the folder and any folders above it that are missing */
static int make_folder(const char *path) {
    char part[PATH_SIZE];
    size_t i;

    for (i = 1; path[i] != '\0' && i < sizeof(part); i++) {
        if ((path[i] == '/' || path[i] == '\\') && path[i - 1] != ':') {
            memcpy(part, path, i);
            part[i] = '\0';
            if (!is_folder(part) && !make_one_folder(part)) {
                return 0;
            }
        }
    }
    return make_one_folder(path);
}

static int index_ok(int index) {
    return index >= 0 && index < PORT_SAVE_COUNT;
}

/* ---- Reading ---------------------------------------------------------------------------------- */
/* Reads and checks the file at `path`. PORT_SAVE_OK with its record and data (data malloc'd: the caller
   frees it), or why not, with nothing to free. */
static PortSaveState read_whole(const char *path, uint8_t record[PORT_SAVE_RECORD_SIZE], uint8_t **data,
                                size_t *size) {
    FILE *file = fopen(path, "rb");
    uint8_t header[SAVE_HEADER_SIZE];
    uint32_t data_size;
    uint8_t *body = NULL;
    PortSaveState state = PORT_SAVE_DAMAGED;

    *data = NULL;
    *size = 0;
    if (file == NULL) {
        return errno == ENOENT ? PORT_SAVE_EMPTY : PORT_SAVE_DAMAGED;
    }
    if (fread(header, sizeof(header), 1, file) != 1 || memcmp(header, SAVE_MAGIC, 8) != 0 ||
        get32(header + 8) != SAVE_VERSION || get32(header + 12) != SAVE_HEADER_SIZE ||
        get32(header + 16) != PORT_SAVE_RECORD_SIZE) {
        goto done;
    }
    data_size = get32(header + 20);
    if (data_size < SAVE_DATA_MIN || data_size > PORT_SAVE_DATA_MAX) {
        goto done;
    }
    body = malloc(data_size);
    if (body == NULL || fread(record, PORT_SAVE_RECORD_SIZE, 1, file) != 1 || fread(body, data_size, 1, file) != 1 ||
        fgetc(file) != EOF || save_hash(record, body, data_size) != get64(header + 32)) {
        goto done;
    }
    *data = body;
    *size = data_size;
    body = NULL;
    state = PORT_SAVE_OK;
done:
    free(body);
    if (fclose(file) != 0) {
        state = PORT_SAVE_DAMAGED;
    }
    return state;
}

static int file_is_whole(const char *path) {
    uint8_t record[PORT_SAVE_RECORD_SIZE];
    uint8_t *data;
    size_t size;
    PortSaveState state = read_whole(path, record, &data, &size);

    free(data);
    return state == PORT_SAVE_OK;
}

/* Save `index` as read_whole reads it: from saveN, or, when that is damaged, from its backup saveN.bak (a
   deleted save stays deleted: a backup alone is not read). *from is the file it came from. */
static PortSaveState read_save(int index, uint8_t record[PORT_SAVE_RECORD_SIZE], uint8_t **data, size_t *size,
                               char from[PATH_SIZE]) {
    PortSaveState state;

    *data = NULL;
    *size = 0;
    if (!port_saves_available() || !index_ok(index) || !save_path(from, PATH_SIZE, index, "")) {
        return PORT_SAVE_EMPTY;
    }
    state = read_whole(from, record, data, size);
    if (state != PORT_SAVE_DAMAGED || !save_path(from, PATH_SIZE, index, ".bak") ||
        read_whole(from, record, data, size) != PORT_SAVE_OK) {
        save_path(from, PATH_SIZE, index, "");
        return state;
    }
    return PORT_SAVE_OK;
}

PortSaveState port_save_record(int index, uint8_t record[PORT_SAVE_RECORD_SIZE]) {
    uint8_t mine[PORT_SAVE_RECORD_SIZE];
    char from[PATH_SIZE];
    uint8_t *data;
    size_t size;
    PortSaveState state = read_save(index, record != NULL ? record : mine, &data, &size, from);

    free(data);
    return state;
}

PortSaveState port_save_read(int index, uint8_t record[PORT_SAVE_RECORD_SIZE], uint8_t *data, size_t capacity,
                             size_t *size) {
    char from[PATH_SIZE];
    uint8_t *whole;
    PortSaveState state = read_save(index, record, &whole, size, from);

    if (state == PORT_SAVE_OK) {
        memcpy(data, whole, *size < capacity ? *size : capacity);
        free(whole);
        set_message("save %d read from %s", index + 1, from);
    } else if (!port_saves_available()) {
        set_message("save %d: there is no save folder", index + 1);
    } else {
        set_message("save %d: %s %s", index + 1, from, state == PORT_SAVE_EMPTY ? "is not there" : "is damaged");
    }
    return state;
}

/* ---- Writing ---------------------------------------------------------------------------------- */
/* Flushes the file to the disk itself, not only to the system */
static int sync_file(FILE *file) {
    if (fflush(file) != 0) {
        return 0;
    }
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

/* The whole save written to `path` and on the disk: 1, or 0 with the file removed */
static int write_whole(const char *path, const uint8_t *record, const uint8_t *data, size_t size) {
    uint8_t header[SAVE_HEADER_SIZE] = {0};
    FILE *file = fopen(path, "wb");
    int ok;

    if (file == NULL) {
        return 0;
    }
    memcpy(header, SAVE_MAGIC, 8);
    put32(header + 8, SAVE_VERSION);
    put32(header + 12, SAVE_HEADER_SIZE);
    put32(header + 16, PORT_SAVE_RECORD_SIZE);
    put32(header + 20, (uint32_t)size);
    put64(header + 32, save_hash(record, data, size));
    ok = fwrite(header, sizeof(header), 1, file) == 1 && fwrite(record, PORT_SAVE_RECORD_SIZE, 1, file) == 1 &&
         fwrite(data, size, 1, file) == 1 && sync_file(file);
    if (fclose(file) != 0) {
        ok = 0;
    }
    if (!ok) {
        remove(path);
    }
    return ok;
}

int port_save_write(int index, const uint8_t record[PORT_SAVE_RECORD_SIZE], const uint8_t *data, size_t size) {
    char path[PATH_SIZE], temp[PATH_SIZE], backup[PATH_SIZE];

    if (!port_saves_available() || !index_ok(index)) {
        set_message("save %d: there is no save folder", index + 1);
        return 0;
    }
    if (size < SAVE_DATA_MIN || size > PORT_SAVE_DATA_MAX) {
        set_message("save %d: the game's save data is %zu bytes, not a save", index + 1, size);
        return 0;
    }
    if (!save_path(path, sizeof(path), index, "") || !save_path(temp, sizeof(temp), index, ".tmp") ||
        !save_path(backup, sizeof(backup), index, ".bak")) {
        set_message("save %d: the save folder's name is too long", index + 1);
        return 0;
    }
    if (!write_whole(temp, record, data, size)) {
        set_message("save %d: cannot write %s (%s)", index + 1, temp, strerror(errno));
        return 0;
    }
    /* the old save becomes the backup, then the new one takes its name (a plain rename onto a name that
       is free, which works the same on Windows). A damaged old save goes instead, so that the backup
       read in its place is kept. */
    if (file_exists(path) && !file_is_whole(path)) {
        if (remove(path) != 0) {
            set_message("save %d: cannot remove the damaged %s (%s)", index + 1, path, strerror(errno));
            remove(temp);
            return 0;
        }
    } else if (file_exists(path)) {
        if (file_exists(backup) && remove(backup) != 0) {
            set_message("save %d: cannot remove the old backup %s (%s)", index + 1, backup, strerror(errno));
            remove(temp);
            return 0;
        }
        if (rename(path, backup) != 0) {
            set_message("save %d: cannot keep %s as the backup (%s)", index + 1, path, strerror(errno));
            remove(temp);
            return 0;
        }
    }
    if (rename(temp, path) != 0) {
        set_message("save %d: cannot rename %s (%s); the previous save is %s", index + 1, temp, strerror(errno),
                    backup);
        return 0;
    }
    set_message("save %d written to %s", index + 1, path);
    return 1;
}

/* ---- Opening ---------------------------------------------------------------------------------- */
/* What a crash in port_save_write can leave, put right: with the save there, a .tmp is a write the game
   never saw finish, and goes; with the save missing, the crash came between the two renames, and the
   .tmp (written whole before the first) or else the .bak becomes the save. */
static void mend(int index) {
    char path[PATH_SIZE], temp[PATH_SIZE], backup[PATH_SIZE];
    uint8_t record[PORT_SAVE_RECORD_SIZE];
    uint8_t *data;
    size_t size;

    if (!save_path(path, sizeof(path), index, "") || !save_path(temp, sizeof(temp), index, ".tmp") ||
        !save_path(backup, sizeof(backup), index, ".bak")) {
        return;
    }
    if (!file_exists(temp)) {
        return;
    }
    if (!file_exists(path)) {
        if (read_whole(temp, record, &data, &size) == PORT_SAVE_OK) {
            free(data);
            if (rename(temp, path) == 0) {
                fprintf(stderr, "[saves] save %d: finished the write a crash cut short\n", index + 1);
                return;
            }
        } else if (file_exists(backup) && rename(backup, path) == 0) {
            fprintf(stderr, "[saves] save %d: brought back from %s after a crash\n", index + 1, backup);
        }
    }
    remove(temp);
}

int port_saves_open(const char *folder) {
    size_t n;
    int index;

    folder_path[0] = '\0';
    if (folder == NULL || strcmp(folder, "none") == 0) {
        fprintf(stderr, "[saves] no save folder: saving is off\n");
        return 0;
    }
    n = strlen(folder);
    if (n == 0 || n >= sizeof(folder_path) - 32) {
        fprintf(stderr, "[saves] the save folder's name is empty or too long; saving is off\n");
        return 0;
    }
    memcpy(folder_path, folder, n + 1);
    while (n > 1 && (folder_path[n - 1] == '/' || folder_path[n - 1] == '\\')) {
        folder_path[--n] = '\0';
    }
    if (!is_folder(folder_path) && !make_folder(folder_path)) {
        fprintf(stderr, "[saves] cannot make the save folder %s (%s); saving is off\n", folder_path, strerror(errno));
        folder_path[0] = '\0';
        return 0;
    }
    for (index = 0; index < PORT_SAVE_COUNT; index++) {
        mend(index);
    }
    fprintf(stderr, "[saves] the save folder is %s\n", folder_path);
    return 1;
}
