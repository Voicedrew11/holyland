/* The disc (port/include/port/platform.h): the user's own copy, read by sector as the drive reads it.

   An ISO 9660 image (.iso) is read as it is: its sector numbers are the disc's. An extracted disc (a
   folder holding SYSTEM.CNF, DATA\ and so on) has no sector numbers, so each of its files is given a
   run of sectors of its own, in path order; the game only ever reads a file through the sector
   sceCdSearchFile gave it (KF4.DAT's members are found by their offset from its first sector), so any
   layout works.

   Every file, and every member of KF4.DAT, is listed with its first sector, so that the log can say
   what the game reads. */
#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
/* the 32-bit stat fails on files over 2 GB, such as the disc image */
#define stat _stat64
#else
#include <dirent.h>
#include <pthread.h>
#endif

#include "port/host.h"
#include "port/platform.h"
#include "port/profile.h"

typedef struct {
    char *path;       /* ISO 9660 path without the version: "\DATA\KF4.DAT" (or KF4.DAT's "\DATA\KF4.DAT:name") */
    char *host_path;  /* an extracted disc's file on the host, else NULL */
    uint32_t lsn;
    uint32_t size;
    int is_member;    /* a KF4.DAT member: in the names, never searched or read on its own */
} DiscFile;

static FILE *image;      /* an ISO image, or NULL for a folder */
static DiscFile *files;
static int file_count;
static int file_capacity;
static uint32_t disc_sectors; /* the sectors the disc has */

/* the extracted disc's file open last, kept open for the reads that follow */
static FILE *open_host_file;
static const DiscFile *open_host_entry;

static char *copy_string(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);

    if (copy == NULL) {
        PORT_FATAL("out of memory");
    }
    memcpy(copy, s, n);
    return copy;
}

static void add_file(const char *path, const char *host_path, uint32_t lsn, uint32_t size, int is_member) {
    DiscFile *f;

    if (file_count == file_capacity) {
        file_capacity = file_capacity ? file_capacity * 2 : 256;
        files = realloc(files, (size_t)file_capacity * sizeof(DiscFile));
        if (files == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    f = &files[file_count++];
    f->path = copy_string(path);
    f->host_path = host_path ? copy_string(host_path) : NULL;
    f->lsn = lsn;
    f->size = size;
    f->is_member = is_member;
}

static int seek_image(uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(image, (__int64)offset, SEEK_SET);
#else
    return fseeko(image, (off_t)offset, SEEK_SET);
#endif
}

static int read_image(uint32_t lsn, uint32_t sectors, void *buffer) {
    size_t bytes = (size_t)sectors * DISC_SECTOR_SIZE;
    size_t got;

    if (seek_image((uint64_t)lsn * DISC_SECTOR_SIZE) != 0) {
        return 0;
    }
    got = fread(buffer, 1, bytes, image);
    if (got < bytes) {
        memset((unsigned char *)buffer + got, 0, bytes - got);
    }
    return got > 0;
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- An ISO 9660 image ------------------------------------------------------------------------ */
static void walk_iso_directory(const char *dir_path, uint32_t lsn, uint32_t size, int depth) {
    unsigned char *data;
    uint32_t sectors = (size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE;
    uint32_t offset = 0;

    if (depth > 8 || sectors == 0) {
        return;
    }
    data = malloc((size_t)sectors * DISC_SECTOR_SIZE);
    if (data == NULL || !read_image(lsn, sectors, data)) {
        PORT_FATAL("cannot read the disc's directory at sector %u", lsn);
    }
    while (offset < size) {
        const unsigned char *record = data + offset;
        unsigned length = record[0];
        char name[256];
        char path[512];
        unsigned name_length;
        unsigned i;

        if (length == 0) {
            /* records do not cross a sector: the rest of this one is empty */
            offset = (offset / DISC_SECTOR_SIZE + 1) * DISC_SECTOR_SIZE;
            continue;
        }
        name_length = record[32];
        if (!(name_length == 1 && (record[33] == 0 || record[33] == 1))) {
            for (i = 0; i < name_length && i < sizeof(name) - 1; i++) {
                name[i] = (char)record[33 + i];
            }
            name[i] = 0;
            /* the version (";1") is not part of the name kept */
            if (strchr(name, ';')) {
                *strchr(name, ';') = 0;
            }
            snprintf(path, sizeof(path), "%s\\%s", dir_path, name);
            if (record[25] & 2) {
                walk_iso_directory(path, le32(record + 2), le32(record + 10), depth + 1);
            } else {
                add_file(path, NULL, le32(record + 2), le32(record + 10), 0);
            }
        }
        offset += length;
    }
    free(data);
}

static void open_iso(const char *path) {
    unsigned char pvd[DISC_SECTOR_SIZE];
    const unsigned char *root;

    image = fopen(path, "rb");
    if (image == NULL) {
        PORT_FATAL("cannot open the disc image %s", path);
    }
    if (!read_image(16, 1, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        PORT_FATAL("%s is not an ISO 9660 image", path);
    }
    disc_sectors = le32(pvd + 80);
    root = pvd + 156;
    walk_iso_directory("", le32(root + 2), le32(root + 10), 0);
}

/* ---- An extracted disc ------------------------------------------------------------------------ */
static uint32_t next_folder_lsn = 0x200;

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void upper(char *s) {
    for (; *s; s++) {
        *s = (char)toupper((unsigned char)*s);
    }
}

static void add_name(char ***names, int *n, int *capacity, const char *name) {
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return;
    }
    if (*n == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 32;
        *names = realloc(*names, (size_t)*capacity * sizeof(char *));
        if (*names == NULL) {
            PORT_FATAL("out of memory");
        }
    }
    (*names)[(*n)++] = copy_string(name);
}

/* the names in a host folder, sorted (so that the layout is the same on every host) */
static char **list_folder(const char *host_dir, int *count) {
    char **names = NULL;
    int n = 0, capacity = 0;
#ifdef _WIN32
    char pattern[1024];
    WIN32_FIND_DATAA found;
    HANDLE find;

    snprintf(pattern, sizeof(pattern), "%s\\*", host_dir);
    find = FindFirstFileA(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            add_name(&names, &n, &capacity, found.cFileName);
        } while (FindNextFileA(find, &found));
        FindClose(find);
    }
#else
    DIR *dir = opendir(host_dir);
    struct dirent *entry;

    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            add_name(&names, &n, &capacity, entry->d_name);
        }
        closedir(dir);
    }
#endif
    if (n > 0) {
        qsort(names, (size_t)n, sizeof(char *), compare_strings);
    }
    *count = n;
    return names;
}

static void walk_folder(const char *dir_path, const char *host_dir, int depth) {
    int count, i;
    char **names = list_folder(host_dir, &count);

    for (i = 0; i < count; i++) {
        char host_path[1024];
        char path[512];
        struct stat info;

        snprintf(host_path, sizeof(host_path), "%s/%s", host_dir, names[i]);
        snprintf(path, sizeof(path), "%s\\%s", dir_path, names[i]);
        upper(path);
        if (stat(host_path, &info) != 0) {
            /* gone since it was listed */
        } else if (S_ISDIR(info.st_mode)) {
            if (depth < 8) {
                walk_folder(path, host_path, depth + 1);
            }
        } else if ((uint64_t)info.st_size < 0xFFFFFFFFu) {
            uint32_t size = (uint32_t)info.st_size;

            add_file(path, host_path, next_folder_lsn, size, 0);
            next_folder_lsn += (size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE + 1;
        }
        free(names[i]);
    }
    free(names);
}

/* ---- KF4.DAT's members, for the log ----------------------------------------------------------- */
static void list_archive(void) {
    uint32_t lsn, size, count, i;
    unsigned char *toc;

    if (!port_disc_find("\\DATA\\KF4.DAT", &lsn, &size)) {
        return;
    }
    toc = malloc(0x20000);
    if (toc == NULL || !port_disc_read(lsn, 0x20000 / DISC_SECTOR_SIZE, toc)) {
        free(toc);
        return;
    }
    count = le32(toc + 4);
    for (i = 0; i < count && 0x40 + (i + 1) * 0x40 <= 0x20000; i++) {
        const unsigned char *entry = toc + 0x40 + i * 0x40;
        char name[0x35];
        char path[0x60];

        memcpy(name, entry, 0x34);
        name[0x34] = 0;
        snprintf(path, sizeof(path), "KF4.DAT:%s", name);
        add_file(path, NULL, lsn + le32(entry + 0x3C) / DISC_SECTOR_SIZE, le32(entry + 0x34), 1);
    }
    free(toc);
}

static int compare_lsn(const void *a, const void *b) {
    const DiscFile *x = a, *y = b;

    if (x->lsn != y->lsn) {
        return x->lsn < y->lsn ? -1 : 1;
    }
    /* a member that starts KF4.DAT comes after the file */
    return x->is_member - y->is_member;
}

void port_disc_open(const char *path) {
    struct stat info;

    if (path == NULL) {
        PORT_FATAL("no disc: give the game's disc image or extracted disc with --disc (or KF4_DISC)");
    }
    if (stat(path, &info) != 0) {
        PORT_FATAL("the disc %s does not exist", path);
    }
    if (S_ISDIR(info.st_mode)) {
        walk_folder("", path, 0);
        disc_sectors = next_folder_lsn;
    } else {
        open_iso(path);
    }
    if (!port_disc_find("\\SYSTEM.CNF", NULL, NULL) || !port_disc_find("\\DATA\\KF4.DAT", NULL, NULL)) {
        PORT_FATAL("%s is not King's Field IV's disc (no SYSTEM.CNF or DATA\\KF4.DAT)", path);
    }
    list_archive();
    qsort(files, (size_t)file_count, sizeof(DiscFile), compare_lsn);
    fprintf(stderr, "[disc] %s: %s, %d files and archive members\n", path, image ? "ISO image" : "folder",
            file_count);
}

/* Compares an ISO 9660 path with a listed one: any case, '/' or '\', the version (";1") left out. */
static int same_path(const char *wanted, const char *listed) {
    if (*wanted != '\\' && *wanted != '/') {
        listed++; /* the listed paths start with '\' */
    }
    for (;; wanted++, listed++) {
        char a = *wanted == '/' ? '\\' : (char)toupper((unsigned char)*wanted);
        char b = (char)toupper((unsigned char)*listed);

        if (a == ';' || a == 0) {
            return b == 0;
        }
        if (a != b) {
            return 0;
        }
    }
}

int port_disc_find(const char *path, uint32_t *lsn, uint32_t *size) {
    int i;

    for (i = 0; i < file_count; i++) {
        if (!files[i].is_member && same_path(path, files[i].path)) {
            if (lsn) {
                *lsn = files[i].lsn;
            }
            if (size) {
                *size = files[i].size;
            }
            return 1;
        }
    }
    return 0;
}

int port_disc_find_member(const char *name, uint32_t *lsn, uint32_t *size) {
    int i;

    for (i = 0; i < file_count; i++) {
        const char *listed = files[i].is_member ? strchr(files[i].path, ':') : NULL;
        const char *a = name, *b = listed ? listed + 1 : NULL;

        if (b == NULL) {
            continue;
        }
        while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*a == 0 && *b == 0) {
            if (lsn) {
                *lsn = files[i].lsn;
            }
            if (size) {
                *size = files[i].size;
            }
            return 1;
        }
    }
    return 0;
}

/* The listed file (not a member) holding a sector, for an extracted disc. */
static const DiscFile *host_file_at(uint32_t lsn) {
    int i;

    for (i = 0; i < file_count; i++) {
        const DiscFile *f = &files[i];
        uint32_t sectors = (f->size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE;

        if (!f->is_member && lsn >= f->lsn && lsn < f->lsn + (sectors ? sectors : 1)) {
            return f;
        }
    }
    return NULL;
}

/* One reader at a time: the image is one FILE (a seek, then a read), and the sound driver reads its
   streams from the audio thread (port/audio) while the game reads from its own */
#ifdef _WIN32
static SRWLOCK read_lock = SRWLOCK_INIT;
#define READ_LOCK() AcquireSRWLockExclusive(&read_lock)
#define READ_UNLOCK() ReleaseSRWLockExclusive(&read_lock)
#else
static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
#define READ_LOCK() pthread_mutex_lock(&read_lock)
#define READ_UNLOCK() pthread_mutex_unlock(&read_lock)
#endif

static int disc_read(uint32_t lsn, uint32_t sectors, void *buffer);

double port_hitch_disc_ms; /* (KF4_HITCH_MS, measure) */
uint64_t port_hitch_disc_bytes;

int port_disc_read(uint32_t lsn, uint32_t sectors, void *buffer) {
    int result;
    double t0 = port_wall_seconds();

    READ_LOCK();
    result = disc_read(lsn, sectors, buffer);
    READ_UNLOCK();
    port_hitch_disc_ms += (port_wall_seconds() - t0) * 1e3;
    port_hitch_disc_bytes += (uint64_t)sectors * 2048;
    if (port_profile_on()) {
        port_profile_add(PROFILE_DISC, (uint64_t)((port_wall_seconds() - t0) * 1e9));
    }
    return result;
}

static int disc_read(uint32_t lsn, uint32_t sectors, void *buffer) {
    unsigned char *out = buffer;

    if (lsn >= disc_sectors) {
        return 0;
    }
    if (image != NULL) {
        return read_image(lsn, sectors, buffer);
    }
    /* an extracted disc: sector by run of one file */
    while (sectors > 0) {
        const DiscFile *f = host_file_at(lsn);
        uint32_t run;
        size_t want, got = 0;

        if (f == NULL) {
            memset(out, 0, DISC_SECTOR_SIZE);
            out += DISC_SECTOR_SIZE;
            lsn++;
            sectors--;
            continue;
        }
        run = (f->size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE - (lsn - f->lsn);
        if (run > sectors) {
            run = sectors;
        }
        if (run == 0) {
            run = 1;
        }
        want = (size_t)run * DISC_SECTOR_SIZE;
        if (open_host_entry != f) {
            if (open_host_file != NULL) {
                fclose(open_host_file);
            }
            open_host_file = fopen(f->host_path, "rb");
            open_host_entry = open_host_file ? f : NULL;
        }
        if (open_host_file != NULL) {
#ifdef _WIN32
            _fseeki64(open_host_file, (__int64)(lsn - f->lsn) * DISC_SECTOR_SIZE, SEEK_SET);
#else
            fseeko(open_host_file, (off_t)(lsn - f->lsn) * DISC_SECTOR_SIZE, SEEK_SET);
#endif
            got = fread(out, 1, want, open_host_file);
        }
        if (got < want) {
            memset(out + got, 0, want - got);
        }
        out += want;
        lsn += run;
        sectors -= run;
    }
    return 1;
}

const char *port_disc_name_at(uint32_t lsn, uint32_t *first_lsn) {
    int low = 0, high = file_count - 1, found = -1;

    /* the last listed entry that starts at or before lsn */
    while (low <= high) {
        int mid = (low + high) / 2;

        if (files[mid].lsn <= lsn) {
            found = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    if (found < 0) {
        return NULL;
    }
    if (first_lsn) {
        *first_lsn = files[found].lsn;
    }
    return files[found].path;
}
