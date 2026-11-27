/* The quick save history's files (port/include/port/quick_history.h): listed from the folder, named in order, thinned
   by the rule (quick_history_rules.c) after each one is added. On the game's thread. */
#include "port/quick_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "port/automap.h"
#include "port/hands.h"
#include "port/platform.h"
#include "port/settings.h"

#define PREFIX "history-"
#define SUFFIX ".kf4quick"
#define PATH_SIZE 1024
/* files read from the folder at most (more is never there unless put there by hand: the rule keeps 13) */
#define LISTED_MAX 256

typedef struct {
    uint32_t number;
    PortQuickInfo info;
} Listed;

/* the folder's whole saves, newest first, read when first asked for and after each change */
static Listed listed[LISTED_MAX];
static int listed_count;
static int listed_known;
static uint32_t highest_number; /* of any history file, whole or not */

static void make_folder(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void folder_path(char *out, size_t size) {
    snprintf(out, size, "%s/quicksaves/history", port_data_dir());
}

static void file_path(char *out, size_t size, uint32_t number) {
    char folder[PATH_SIZE];

    folder_path(folder, sizeof(folder));
    snprintf(out, size, "%s/" PREFIX "%06u" SUFFIX, folder, (unsigned)number);
}

/* history-<n>.kf4quick's n, or 0 when the name is not one */
static uint32_t number_of(const char *name) {
    size_t prefix = strlen(PREFIX), length = strlen(name), suffix = strlen(SUFFIX);
    unsigned long value = 0;

    if (length <= prefix + suffix || strncmp(name, PREFIX, prefix) != 0 ||
        strcmp(name + length - suffix, SUFFIX) != 0) {
        return 0;
    }
    for (size_t i = prefix; i < length - suffix; i++) {
        if (name[i] < '0' || name[i] > '9' || value > 0xFFFFFFFul) {
            return 0;
        }
        value = value * 10 + (unsigned long)(name[i] - '0');
    }
    return (uint32_t)value;
}

static int newest_first(const void *a, const void *b) {
    uint32_t x = ((const Listed *)a)->number, y = ((const Listed *)b)->number;

    return x < y ? 1 : x > y ? -1 : 0;
}

static void take_name(const char *name) {
    char path[PATH_SIZE];
    uint32_t number = number_of(name);

    if (number == 0) {
        return;
    }
    if (number > highest_number) {
        highest_number = number;
    }
    if (listed_count == LISTED_MAX) {
        return;
    }
    file_path(path, sizeof(path), number);
    if (port_quick_file_info(path, &listed[listed_count].info)) {
        listed[listed_count].number = number;
        listed_count++;
    }
}

static void list_folder(void) {
    char folder[PATH_SIZE];

    listed_count = 0;
    highest_number = 0;
    listed_known = 1;
    folder_path(folder, sizeof(folder));
#ifdef _WIN32
    char pattern[PATH_SIZE + 4];
    WIN32_FIND_DATAA found;
    HANDLE find;

    snprintf(pattern, sizeof(pattern), "%s\\*", folder);
    find = FindFirstFileA(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            take_name(found.cFileName);
        } while (FindNextFileA(find, &found));
        FindClose(find);
    }
#else
    DIR *dir = opendir(folder);
    struct dirent *entry;

    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            take_name(entry->d_name);
        }
        closedir(dir);
    }
#endif
    if (listed_count > 1) {
        qsort(listed, (size_t)listed_count, sizeof(listed[0]), newest_first);
    }
}

static void know_folder(void) {
    if (!listed_known) {
        list_folder();
    }
}

void port_quick_history_next_path(char *out, size_t size) {
    char folder[PATH_SIZE];

    list_folder(); /* (a file put there since, by another copy of the game, still gets a number of its own) */
    make_folder(port_data_dir());
    snprintf(folder, sizeof(folder), "%s/quicksaves", port_data_dir());
    make_folder(folder);
    folder_path(folder, sizeof(folder));
    make_folder(folder);
    file_path(out, size, highest_number + 1);
}

/* The file and what the mods kept with it */
static void remove_save(uint32_t number) {
    char path[PATH_SIZE];

    file_path(path, sizeof(path), number);
    if (remove(path) != 0) {
        port_event("quick history: cannot remove %s", path);
        return;
    }
    port_automap_quick_removed(path);
    port_hands_quick_removed(path);
}

void port_quick_history_added(void) {
    PortQuickHistoryEntry entries[LISTED_MAX];
    uint8_t keep[LISTED_MAX];
    int dropped = 0;

    list_folder();
    for (int i = 0; i < listed_count; i++) {
        entries[i].play_seconds = listed[i].info.play_seconds;
        entries[i].map_tag = listed[i].info.map_tag;
    }
    port_quick_history_choose(entries, listed_count, keep);
    for (int i = 0; i < listed_count; i++) {
        if (!keep[i]) {
            remove_save(listed[i].number);
            dropped++;
        }
    }
    if (dropped > 0) {
        list_folder();
    }
    port_event("quick history: %d saves kept, %d dropped", listed_count, dropped);
}

int port_quick_history_count(void) {
    know_folder();
    return listed_count;
}

int port_quick_history_info(int index, PortQuickInfo *info) {
    know_folder();
    if (index < 0 || index >= listed_count) {
        return 0;
    }
    *info = listed[index].info;
    return 1;
}

void port_quick_history_request_load(int index) {
    char path[PATH_SIZE];

    know_folder();
    if (index < 0 || index >= listed_count) {
        return;
    }
    file_path(path, sizeof(path), listed[index].number);
    port_quick_request_load_file(path);
}
