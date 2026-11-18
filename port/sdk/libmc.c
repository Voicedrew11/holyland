/* libmc for the PC port (port/include/libmc.h): the memory card in slot 1 is a folder on the host, the one
   in slot 2 is never there.

   The folder is the card's root: the game's own folder /BASLUS-20318S/ is a subfolder of it holding real
   files (the save files, the list file, icon.sys and the icon), as PCSX2's folder memory cards keep them, so
   such a card can be used here (PCSX2's own _pcsx2_* files are not shown to the game). --memcard DIR names
   the folder (by default "memcard" in the data folder, beside settings.json and the save states), --memcard none leaves the slot empty, as the tests do.

   What the game sees is a formatted 8 MB PS2 card, answering as Sony's libmc and the IOP's mcman do:
   - sceMcGetInfo: type 2 (PS2), formatted, the free 1 KB clusters; -1 (sceMcResChangedCard) the first time
     the slot is asked about, 0 after;
   - the free clusters: the 8000 a freshly formatted card can allocate, less a cluster per started KB of
     each file and of each folder's entries (512 bytes each, "." and ".." included); a write past the end
     fails with -3 (sceMcResFullDevice), so the game's "card full" paths stay reachable;
   - sceMcGetDir: the entries matching a pattern ('*' and '?' in the last part), "." and ".." first when they
     match, then the rest by name; mcman's attributes (0x8427 a folder, 0x8497 a file); the dates are the
     host file's modification time in Japan's time, as the PS2's clock keeps it;
   - errors: -4 (no entry), -5 (denied), -6 (folder not empty), -7 (too many open files).
   Every call finishes at once: sceMcSync then hands back its result. sceMcFormat leaves the folder as it is
   (the card is always formatted, so the game never asks; it must not delete a player's saves). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "libmc.h"
#include "port/automap.h"
#include "port/hands.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/settings.h"
#include "port/savestate.h"

/* mcman's result codes and attributes (ps2sdk common/include/libmc-common.h) */
#define MC_OK 0
#define MC_CHANGED_CARD -1
#define MC_FULL -3
#define MC_NO_ENTRY -4
#define MC_DENIED -5
#define MC_NOT_EMPTY -6
#define MC_TOO_MANY_HANDLES -7
#define MC_NO_CARD -11
#define MC_TYPE_PS2 2
#define MC_ATTR_DIR 0x8427u  /* exists | 0x400 | sub-directory | read, write, execute */
#define MC_ATTR_FILE 0x8497u /* exists | 0x400 | closed | file | read, write, execute */
#define MC_RDONLY 0x0001
#define MC_WRONLY 0x0002
#define MC_RDWR 0x0003
#define MC_CREAT 0x0200

#define CARD_CLUSTERS 8000
#define CLUSTER 1024u
#define MAX_FILES 8
#define PATH_MAX_CARD 1024

/* sceMcSync's function numbers */
#define MC_FUNC_GET_INFO 1
#define MC_FUNC_OPEN 2
#define MC_FUNC_CLOSE 3
#define MC_FUNC_SEEK 4
#define MC_FUNC_READ 5
#define MC_FUNC_WRITE 6
#define MC_FUNC_MKDIR 11
#define MC_FUNC_CHDIR 12
#define MC_FUNC_GET_DIR 13
#define MC_FUNC_DELETE 15
#define MC_FUNC_FORMAT 16

static int pending_function; /* 0: no call in flight */
static int pending_result;

static char card_root[PATH_MAX_CARD]; /* "": no card */
static char card_cwd[PATH_MAX_CARD] = "/";
static int card_seen;                  /* sceMcGetInfo has answered for the card once */

typedef struct {
    FILE *file;
    int writable;
} McFile;

static McFile files[MAX_FILES];

static int start_call(int function, int result) {
    pending_function = function;
    pending_result = result;
    return 0;
}

/* ---- Paths ----------------------------------------------------------------------------------------- */
/* The card path `name` (absolute, or relative to the current directory) as a host path; 0 when it leaves
   the card (a ".." past the root) */
static int host_path(const char *name, char *out, size_t size) {
    char card[PATH_MAX_CARD];
    char clean[PATH_MAX_CARD];
    char *part;
    char *next;
    size_t length = 0;

    if (name[0] == '/') {
        snprintf(card, sizeof(card), "%s", name);
    } else {
        if ((size_t)snprintf(card, sizeof(card), "%s/%s", card_cwd, name) >= sizeof(card)) {
            return 0;
        }
    }
    clean[0] = '\0';
    for (part = card; part != NULL; part = next) {
        next = strchr(part, '/');
        if (next != NULL) {
            *next++ = '\0';
        }
        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            char *slash = strrchr(clean, '/');

            if (slash == NULL) {
                return 0;
            }
            *slash = '\0';
            length = strlen(clean);
            continue;
        }
        if (strchr(part, '\\') != NULL || strchr(part, ':') != NULL) {
            return 0;
        }
        length += (size_t)snprintf(clean + length, sizeof(clean) - length, "/%s", part);
        if (length >= sizeof(clean)) {
            return 0;
        }
    }
    snprintf(out, size, "%s%s", card_root, clean);
    return 1;
}

static int is_dir(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int make_dir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

/* PCSX2's own bookkeeping in a folder memory card */
static int hidden_name(const char *name) {
    return strncmp(name, "_pcsx2_", 7) == 0;
}

/* The names in a host folder (not "." and ".."), sorted; the count, or -1 */
static int list_names(const char *path, char ***out) {
    char **names = NULL;
    int count = 0;
    int capacity = 0;

#ifdef _WIN32
    WIN32_FIND_DATAA found;
    char pattern[PATH_MAX_CARD + 4];
    HANDLE find;

    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        const char *name = found.cFileName;
#else
    DIR *dir = opendir(path);
    struct dirent *entry;

    if (dir == NULL) {
        return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
#endif
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && !hidden_name(name)) {
            if (count == capacity) {
                capacity = capacity ? capacity * 2 : 16;
                names = realloc(names, (size_t)capacity * sizeof(*names));
            }
            names[count++] = strdup(name);
        }
#ifdef _WIN32
    } while (FindNextFileA(find, &found));
    FindClose(find);
#else
    }
    closedir(dir);
#endif
    {
        int i;
        int j;

        for (i = 1; i < count; i++) {
            for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
                char *t = names[j];

                names[j] = names[j - 1];
                names[j - 1] = t;
            }
        }
    }
    *out = names;
    return count;
}

static void free_names(char **names, int count) {
    int i;

    for (i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

/* The clusters a folder and everything in it take */
static uint32_t used_clusters(const char *path) {
    char **names;
    int count = list_names(path, &names);
    uint32_t used;
    int i;

    if (count < 0) {
        return 0;
    }
    used = (uint32_t)((count + 2) * 512 + CLUSTER - 1) / CLUSTER;
    for (i = 0; i < count; i++) {
        char child[PATH_MAX_CARD];
        struct stat st;

        snprintf(child, sizeof(child), "%s/%s", path, names[i]);
        if (stat(child, &st) != 0) {
            continue;
        }
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            used += used_clusters(child);
        } else {
            used += (uint32_t)(((uint64_t)st.st_size + CLUSTER - 1) / CLUSTER);
        }
    }
    free_names(names, count);
    return used;
}

static int free_clusters(void) {
    uint32_t used = used_clusters(card_root);

    return used >= CARD_CLUSTERS ? 0 : (int)(CARD_CLUSTERS - used);
}

/* ---- Set-up ----------------------------------------------------------------------------------------- */
void port_mc_init(void) {
    const char *dir = port_options.memcard;

    if (dir != NULL && strcmp(dir, "none") == 0) {
        port_event("memory card: none in slot 1 (--memcard none)");
        return;
    }
    if (dir == NULL) {
        /* in the data folder, beside settings.json and the save states (made if it is missing) */
        make_dir(port_data_dir());
        snprintf(card_root, sizeof(card_root), "%s/memcard", port_data_dir());
    } else {
        snprintf(card_root, sizeof(card_root), "%s", dir);
    }
    {
        size_t n = strlen(card_root);

        while (n > 1 && (card_root[n - 1] == '/' || card_root[n - 1] == '\\')) {
            card_root[--n] = '\0';
        }
    }
    if (!is_dir(card_root) && make_dir(card_root) != 0) {
        fprintf(stderr, "[port] memory card: cannot make the folder %s; slot 1 is empty\n", card_root);
        card_root[0] = '\0';
        return;
    }
    port_event("memory card: slot 1 is the folder %s", card_root);
}

/* ---- libmc ------------------------------------------------------------------------------------------ */
int sceMcInit(void) {
    return 0;
}

int sceMcGetInfo(int port, int slot, int *type, int *free, int *format) {
    int present = port == 0 && slot == 0 && card_root[0] != '\0';

    if (type) {
        *type = present ? MC_TYPE_PS2 : 0;
    }
    if (free) {
        *free = present ? free_clusters() : 0;
    }
    if (format) {
        *format = present ? 1 : 0;
    }
    if (!present) {
        return start_call(MC_FUNC_GET_INFO, MC_NO_CARD);
    }
    if (!card_seen) {
        card_seen = 1;
        return start_call(MC_FUNC_GET_INFO, MC_CHANGED_CARD);
    }
    return start_call(MC_FUNC_GET_INFO, MC_OK);
}

int sceMcOpen(int port, int slot, const char *name, int mode) {
    char path[PATH_MAX_CARD];
    const char *how;
    int fd;
    int exists;

    if (port != 0 || slot != 0 || card_root[0] == '\0') {
        return start_call(MC_FUNC_OPEN, MC_NO_CARD);
    }
    if (!host_path(name, path, sizeof(path))) {
        return start_call(MC_FUNC_OPEN, MC_NO_ENTRY);
    }
    for (fd = 0; fd < MAX_FILES && files[fd].file != NULL; fd++) {
    }
    if (fd == MAX_FILES) {
        return start_call(MC_FUNC_OPEN, MC_TOO_MANY_HANDLES);
    }
    if (is_dir(path)) {
        return start_call(MC_FUNC_OPEN, MC_DENIED);
    }
    {
        FILE *probe = fopen(path, "rb");

        exists = probe != NULL;
        if (probe) {
            fclose(probe);
        }
    }
    if (!exists && !(mode & MC_CREAT)) {
        return start_call(MC_FUNC_OPEN, MC_NO_ENTRY);
    }
    if ((mode & MC_RDWR) == MC_RDONLY) {
        how = "rb";
    } else if (!exists) {
        if (free_clusters() < 1) {
            return start_call(MC_FUNC_OPEN, MC_FULL);
        }
        how = "w+b";
    } else {
        how = "r+b";
    }
    files[fd].file = fopen(path, how);
    if (files[fd].file == NULL) {
        return start_call(MC_FUNC_OPEN, MC_NO_ENTRY);
    }
    files[fd].writable = (mode & MC_RDWR) != MC_RDONLY;
    port_automap_card_file(name, files[fd].writable); /* the automap mod: its state kept per save */
    port_hands_card_file(name, files[fd].writable); /* the hands mod: the hands kept per save */
    return start_call(MC_FUNC_OPEN, fd);
}

static McFile *open_file(int fd) {
    return fd >= 0 && fd < MAX_FILES && files[fd].file != NULL ? &files[fd] : NULL;
}

int sceMcClose(int fd) {
    McFile *f = open_file(fd);

    if (f == NULL) {
        return start_call(MC_FUNC_CLOSE, MC_NO_ENTRY);
    }
    fclose(f->file);
    f->file = NULL;
    return start_call(MC_FUNC_CLOSE, MC_OK);
}

int sceMcRead(int fd, void *buffer, int size) {
    McFile *f = open_file(fd);

    if (f == NULL || size < 0) {
        return start_call(MC_FUNC_READ, MC_NO_ENTRY);
    }
    return start_call(MC_FUNC_READ, (int)fread(buffer, 1, (size_t)size, f->file));
}

int sceMcWrite(int fd, const void *buffer, int size) {
    McFile *f = open_file(fd);
    long position;
    long end;
    int64_t grow;

    if (f == NULL || size < 0) {
        return start_call(MC_FUNC_WRITE, MC_NO_ENTRY);
    }
    if (!f->writable) {
        return start_call(MC_FUNC_WRITE, MC_DENIED);
    }
    /* the clusters the file gains must be free */
    position = ftell(f->file);
    fseek(f->file, 0, SEEK_END);
    end = ftell(f->file);
    fseek(f->file, position, SEEK_SET);
    grow = (int64_t)((position + size + CLUSTER - 1) / CLUSTER) - (int64_t)((end + CLUSTER - 1) / CLUSTER);
    if (grow > 0 && grow > free_clusters()) {
        return start_call(MC_FUNC_WRITE, MC_FULL);
    }
    {
        int written = (int)fwrite(buffer, 1, (size_t)size, f->file);

        fflush(f->file);
        return start_call(MC_FUNC_WRITE, written);
    }
}

int sceMcSeek(int fd, int offset, int origin) {
    McFile *f = open_file(fd);
    int whence = origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET;

    if (f == NULL || fseek(f->file, offset, whence) != 0) {
        return start_call(MC_FUNC_SEEK, MC_NO_ENTRY);
    }
    return start_call(MC_FUNC_SEEK, (int)ftell(f->file));
}

int sceMcMkdir(int port, int slot, const char *name) {
    char path[PATH_MAX_CARD];
    char *slash;

    if (port != 0 || slot != 0 || card_root[0] == '\0') {
        return start_call(MC_FUNC_MKDIR, MC_NO_CARD);
    }
    if (!host_path(name, path, sizeof(path))) {
        return start_call(MC_FUNC_MKDIR, MC_NO_ENTRY);
    }
    /* mcman: no entry when it exists already or its parent does not */
    slash = strrchr(path, '/');
    if (is_dir(path) || slash == NULL) {
        return start_call(MC_FUNC_MKDIR, MC_NO_ENTRY);
    }
    *slash = '\0';
    if (!is_dir(path)) {
        return start_call(MC_FUNC_MKDIR, MC_NO_ENTRY);
    }
    *slash = '/';
    if (free_clusters() < 1) {
        return start_call(MC_FUNC_MKDIR, MC_FULL);
    }
    return start_call(MC_FUNC_MKDIR, make_dir(path) == 0 ? MC_OK : MC_NO_ENTRY);
}

int sceMcChdir(int port, int slot, const char *new_dir, char *old_dir) {
    char path[PATH_MAX_CARD];
    char card[PATH_MAX_CARD];

    if (old_dir != NULL) {
        strcpy(old_dir, card_cwd);
    }
    if (port != 0 || slot != 0 || card_root[0] == '\0') {
        return start_call(MC_FUNC_CHDIR, MC_NO_CARD);
    }
    if (!host_path(new_dir, path, sizeof(path)) || !is_dir(path)) {
        return start_call(MC_FUNC_CHDIR, MC_NO_ENTRY);
    }
    snprintf(card, sizeof(card), "%s", path + strlen(card_root));
    snprintf(card_cwd, sizeof(card_cwd), "%s", card[0] ? card : "/");
    return start_call(MC_FUNC_CHDIR, MC_OK);
}

/* '*' any run of characters, '?' any one */
static int matches(const char *pattern, const char *name) {
    if (*pattern == '\0') {
        return *name == '\0';
    }
    if (*pattern == '*') {
        return matches(pattern + 1, name) || (*name != '\0' && matches(pattern, name + 1));
    }
    if (*name == '\0' || (*pattern != '?' && *pattern != *name)) {
        return 0;
    }
    return matches(pattern + 1, name + 1);
}

/* A card date (Japan's time, as the PS2's clock keeps it) from a host time */
static void card_date(sceMcStDateTime *date, time_t when) {
    time_t japan = when + 9 * 3600;
    struct tm *t = gmtime(&japan);

    memset(date, 0, sizeof(*date));
    if (t == NULL) {
        return;
    }
    date->Sec = (unsigned char)t->tm_sec;
    date->Min = (unsigned char)t->tm_min;
    date->Hour = (unsigned char)t->tm_hour;
    date->Day = (unsigned char)t->tm_mday;
    date->Month = (unsigned char)(t->tm_mon + 1);
    date->Year = (unsigned short)(t->tm_year + 1900);
}

static void fill_entry(sceMcTblGetDir *entry, const char *name, const char *path, int dot) {
    struct stat st;

    memset(entry, 0, sizeof(*entry));
    snprintf((char *)entry->EntryName, sizeof(entry->EntryName), "%s", name);
    if (stat(path, &st) != 0) {
        return;
    }
    card_date(&entry->_Create, st.st_mtime);
    card_date(&entry->_Modify, st.st_mtime);
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        char **names;
        int count = dot ? 0 : list_names(path, &names);

        entry->AttrFile = MC_ATTR_DIR;
        if (count > 0) {
            free_names(names, count);
        }
        entry->FileSizeByte = (unsigned int)(count < 0 ? 0 : count + 2);
    } else {
        entry->AttrFile = MC_ATTR_FILE;
        entry->FileSizeByte = (unsigned int)st.st_size;
    }
}

/* The directory listing of the last mode-0 call, for mode-1 calls that go on with it */
static char **listing;
static int listing_count;
static int listing_next;
static char listing_dir[PATH_MAX_CARD];

int sceMcGetDir(int port, int slot, const char *name, unsigned int mode, int max_entries,
                sceMcTblGetDir *table) {
    char card[PATH_MAX_CARD];
    char dir[PATH_MAX_CARD];
    char *slash;
    const char *pattern;
    int found = 0;

    if (port != 0 || slot != 0 || card_root[0] == '\0') {
        return start_call(MC_FUNC_GET_DIR, MC_NO_CARD);
    }
    if (mode == 0) {
        if (listing != NULL) {
            free_names(listing, listing_count);
            listing = NULL;
        }
        /* the folder part (the current folder when there is none) and the pattern for the names */
        snprintf(card, sizeof(card), "%s", name);
        slash = strrchr(card, '/');
        if (slash == NULL) {
            snprintf(card, sizeof(card), ".");
            pattern = name;
        } else {
            pattern = name + (slash - card) + 1;
            if (slash == card) {
                slash[1] = '\0';
            } else {
                *slash = '\0';
            }
        }
        if (!host_path(card, dir, sizeof(dir)) || !is_dir(dir)) {
            return start_call(MC_FUNC_GET_DIR, MC_NO_ENTRY);
        }
        snprintf(listing_dir, sizeof(listing_dir), "%s", dir);
        {
            char **names;
            int count = list_names(dir, &names);
            int i;

            if (count < 0) {
                return start_call(MC_FUNC_GET_DIR, MC_NO_ENTRY);
            }
            listing = malloc((size_t)(count + 2) * sizeof(*listing));
            listing_count = 0;
            if (matches(pattern, ".")) {
                listing[listing_count++] = strdup(".");
            }
            if (matches(pattern, "..")) {
                listing[listing_count++] = strdup("..");
            }
            for (i = 0; i < count; i++) {
                if (matches(pattern, names[i])) {
                    listing[listing_count++] = strdup(names[i]);
                }
            }
            free_names(names, count);
        }
        listing_next = 0;
    }
    while (listing != NULL && listing_next < listing_count && found < max_entries) {
        const char *entry = listing[listing_next++];
        char path[PATH_MAX_CARD];
        int dot = strcmp(entry, ".") == 0 || strcmp(entry, "..") == 0;

        if ((size_t)snprintf(path, sizeof(path), "%s/%s", listing_dir, dot ? "" : entry) >= sizeof(path)) {
            continue;
        }
        if (table != NULL) {
            fill_entry(&table[found], entry, path, dot);
        }
        found++;
    }
    return start_call(MC_FUNC_GET_DIR, found);
}

int sceMcFormat(int port, int slot) {
    if (port != 0 || slot != 0 || card_root[0] == '\0') {
        return start_call(MC_FUNC_FORMAT, MC_NO_CARD);
    }
    port_event("memory card: the game asked to format the card; the folder is kept as it is");
    return start_call(MC_FUNC_FORMAT, MC_OK);
}

int sceMcDelete(int port, int slot, const char *name) {
    char path[PATH_MAX_CARD];

    if (port != 0 || slot != 0 || card_root[0] == '\0') {
        return start_call(MC_FUNC_DELETE, MC_NO_CARD);
    }
    if (!host_path(name, path, sizeof(path)) || strcmp(path, card_root) == 0) {
        return start_call(MC_FUNC_DELETE, MC_NO_ENTRY);
    }
    if (is_dir(path)) {
        char **names;
        int count = list_names(path, &names);

        if (count > 0) {
            free_names(names, count);
            return start_call(MC_FUNC_DELETE, MC_NOT_EMPTY);
        }
#ifdef _WIN32
        return start_call(MC_FUNC_DELETE, _rmdir(path) == 0 ? MC_OK : MC_DENIED);
#else
        return start_call(MC_FUNC_DELETE, rmdir(path) == 0 ? MC_OK : MC_DENIED);
#endif
    }
    if (remove(path) != 0) {
        return start_call(MC_FUNC_DELETE, errno == ENOENT ? MC_NO_ENTRY : MC_DENIED);
    }
    return start_call(MC_FUNC_DELETE, MC_OK);
}

int sceMcSync(int mode, int *cmd, int *result) {
    (void)mode;
    if (pending_function == 0) {
        return -1;
    }
    if (cmd) {
        *cmd = pending_function;
    }
    if (result) {
        *result = pending_result;
    }
    pending_function = 0;
    return 1;
}

/* ---- Save states ------------------------------------------------------------------------------ */
/* The card's files are the host's: no state is taken while one is open, or while a listing the game
   reads in parts is under way. The card's contents are not in a state (as on an emulator, loading a
   state leaves the saves on the card as they are). */
const char *port_mc_state_busy(void) {
    int fd;

    for (fd = 0; fd < MAX_FILES; fd++) {
        if (files[fd].file != NULL) {
            return "a memory card file is open";
        }
    }
    if (listing != NULL && listing_next < listing_count) {
        return "the game is reading the memory card's folder";
    }
    return NULL;
}

void port_mc_state(PortStateIo *io) {
    PORT_STATE(io, pending_function);
    PORT_STATE(io, pending_result);
    PORT_STATE(io, card_cwd);
    PORT_STATE(io, card_seen);
}
