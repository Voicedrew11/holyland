/* The data and builds folders (port/include/port/data_dir.h). No other part of the port is used here:
   the launcher links this file on its own. Each folder is the first rule of its table whose variable is set:
   a new place (a new system) is a new row. */
#include "port/data_dir.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#endif

typedef enum { ANY_SYSTEM, WINDOWS_ONLY, POSIX_ONLY } RuleSystem;

typedef struct {
    RuleSystem system;
    const char *variable;   /* NULL: always applies */
    const char *format;     /* the path, from the variable's value */
    const char *old_format; /* the path while the port was named Darker Reality (2026-11-18 to 11-20); NULL: none */
} Rule;

typedef enum { NAME_NOW, NAME_BEFORE } FolderName;

/* KF4_DATA_DIR given for one run (tests, a second copy) keeps its builds with it: the builds rule then
   takes the data folder (data_dir_resolve_builds). On Linux it is the local folder below (XDG's data folder):
   the game's saves and settings live with its builds and states, in one place. (Its old name, darker-reality, is
   the local rules' to move: the same folder.) */
static const Rule data_rules[] = {
    {ANY_SYSTEM, "KF4_DATA_DIR", "%s", NULL},
    {WINDOWS_ONLY, "APPDATA", "%s\\holyland", "%s\\darker-reality"},
    {POSIX_ONLY, "XDG_DATA_HOME", "%s/holyland", NULL},
    {POSIX_ONLY, "HOME", "%s/.local/share/holyland", NULL},
    {ANY_SYSTEM, NULL, "holyland", "darker-reality"},
};

/* Linux's data folder until 2026-11-21: XDG's config folder, under either name. Its files are moved into the
   data folder (move_config_folders). */
static const Rule config_rules[] = {
    {POSIX_ONLY, "XDG_CONFIG_HOME", "%s/holyland", "%s/darker-reality"},
    {POSIX_ONLY, "HOME", "%s/.config/holyland", "%s/.config/darker-reality"},
};

/* the local folder: it holds the builds folder, and on Linux the save states (port/platform/savestate.c) */
static const Rule local_rules[] = {
    {WINDOWS_ONLY, "LOCALAPPDATA", "%s\\holyland", "%s\\darker-reality"},
    {POSIX_ONLY, "XDG_DATA_HOME", "%s/holyland", "%s/darker-reality"},
    {POSIX_ONLY, "HOME", "%s/.local/share/holyland", "%s/.local/share/darker-reality"},
};

static int applies(RuleSystem rule, DataDirSystem system) {
    return rule == ANY_SYSTEM || (rule == WINDOWS_ONLY) == (system == DATA_DIR_WINDOWS);
}

static const char *nonempty(DataDirEnv env, const char *name) {
    const char *value = env(name);

    return value != NULL && *value != 0 ? value : NULL;
}

/* the first rule that applies, written into `out`: 1, 0 if it did not fit (or, for NAME_BEFORE, the rule
   has no old name), -1 if none applied */
static int resolve(const Rule *rules, size_t count, FolderName name, DataDirSystem system, DataDirEnv env, char *out,
                   size_t out_size) {
    size_t i;

    for (i = 0; i < count; i++) {
        const char *value = rules[i].variable != NULL ? nonempty(env, rules[i].variable) : "";
        const char *format = name == NAME_NOW ? rules[i].format : rules[i].old_format;
        int n;

        if (!applies(rules[i].system, system) || value == NULL) {
            continue;
        }
        if (format == NULL) {
            return 0;
        }
        n = snprintf(out, out_size, format, value);
        return n >= 0 && (size_t)n < out_size;
    }
    return -1;
}

#define COUNT(rules) (sizeof(rules) / sizeof((rules)[0]))

static int exists(const char *path) {
    struct stat info;

    return stat(path, &info) == 0;
}

/* `folder` + the system's separator + `name` into `out`; 1, or 0 if it did not fit */
static int join(DataDirSystem system, const char *folder, const char *name, char *out, size_t out_size) {
    int n = snprintf(out, out_size, system == DATA_DIR_WINDOWS ? "%s\\%s" : "%s/%s", folder, name);

    return n >= 0 && (size_t)n < out_size;
}

/* whether `folder` holds a saves folder */
static int has_saves(DataDirSystem system, const char *folder) {
    char saves[1100];

    return join(system, folder, "saves", saves, sizeof(saves)) && exists(saves);
}

/* Started by RecompLauncher (RECOMPLAUNCHER_DATA: its folder for the game, <install>/recompdata): its folder, so it
   moves, backs up and removes the game's files with the game; unless only the usual folder (the rules after
   KF4_DATA_DIR's) has saves, a player's from before: those stay where they are, nothing moved or copied. Saves in
   RecompLauncher's folder keep it there, whatever appears in the usual one later. */
static int resolve_for_recomplauncher(const char *given, DataDirSystem system, DataDirEnv env, char *out,
                                      size_t out_size) {
    char usual[1024];
    int n;

    if (!has_saves(system, given) &&
        resolve(data_rules + 1, COUNT(data_rules) - 1, NAME_NOW, system, env, usual, sizeof(usual)) == 1 &&
        has_saves(system, usual)) {
        n = snprintf(out, out_size, "%s", usual);
    } else {
        n = snprintf(out, out_size, "%s", given);
    }
    return n >= 0 && (size_t)n < out_size;
}

int data_dir_resolve(DataDirSystem system, DataDirEnv env, char *out, size_t out_size) {
    const char *recomplauncher = nonempty(env, "RECOMPLAUNCHER_DATA");

    if (nonempty(env, "KF4_DATA_DIR") == NULL && recomplauncher != NULL) {
        return resolve_for_recomplauncher(recomplauncher, system, env, out, out_size);
    }
    return resolve(data_rules, COUNT(data_rules), NAME_NOW, system, env, out, out_size) == 1;
}

/* the folder the builds folder goes in: the local folder, else (KF4_DATA_DIR given, or no local rule
   applied) the data folder; 1, or 0 if it did not fit */
static int builds_parent(DataDirSystem system, DataDirEnv env, char *out, size_t out_size) {
    if (nonempty(env, "KF4_DATA_DIR") == NULL) {
        int found = resolve(local_rules, COUNT(local_rules), NAME_NOW, system, env, out, out_size);

        if (found >= 0) {
            return found;
        }
    }
    return data_dir_resolve(system, env, out, out_size);
}

int data_dir_resolve_builds(DataDirSystem system, DataDirEnv env, char *out, size_t out_size) {
    const char *given = nonempty(env, "KF4_BUILDS_DIR");
    const char *recomplauncher = nonempty(env, "RECOMPLAUNCHER_DATA");
    char parent[1024];
    int n;

    if (given != NULL) {
        n = snprintf(out, out_size, "%s", given);
    } else if (recomplauncher != NULL && nonempty(env, "KF4_DATA_DIR") == NULL) {
        /* always RecompLauncher's, even for a player whose saves stay in the usual folder: removed with the game, and
           in its cache folder, which its backups leave out */
        n = snprintf(out, out_size, system == DATA_DIR_WINDOWS ? "%s\\cache\\builds" : "%s/cache/builds",
                     recomplauncher);
    } else if (builds_parent(system, env, parent, sizeof(parent))) {
        n = snprintf(out, out_size, system == DATA_DIR_WINDOWS ? "%s\\builds" : "%s/builds", parent);
    } else {
        return 0;
    }
    return n >= 0 && (size_t)n < out_size;
}

/* moves the folder of the first rule that applies from its old name to its name now; 1 if it moved */
static int move_old(const Rule *rules, size_t count, DataDirSystem system, DataDirEnv env) {
    char now[1024], before[1024];

    if (resolve(rules, count, NAME_NOW, system, env, now, sizeof(now)) != 1 ||
        resolve(rules, count, NAME_BEFORE, system, env, before, sizeof(before)) != 1 || !exists(before) ||
        exists(now)) {
        return 0;
    }
    if (rename(before, now) != 0) {
        fprintf(stderr, "data folder: could not move %s to %s; it is left where it is\n", before, now);
        return 0;
    }
    fprintf(stderr, "data folder: moved %s to %s (the port's new name)\n", before, now);
    return 1;
}

#ifndef _WIN32
/* every folder of `path` made, as mkdir -p; 1, or 0 if one could not be */
static int make_folders(const char *path) {
    char partial[1024];
    size_t i, length = strlen(path);

    if (length == 0 || length >= sizeof(partial)) {
        return 0;
    }
    for (i = 1; i <= length; i++) {
        if (path[i] == '/' || path[i] == 0) {
            memcpy(partial, path, i);
            partial[i] = 0;
            if (mkdir(partial, 0755) != 0 && errno != EEXIST) {
                return 0;
            }
        }
    }
    return 1;
}

/* each file and folder in `from` moved into `into`, unless `into` already has one of that name (that one is
   left in `from`); `from` removed once empty. 1 if anything moved. */
static int move_contents(const char *from, const char *into) {
    DIR *folder;
    struct dirent *entry;
    char source[1024], target[1024];
    int moved = 0, left = 0;

    if (!make_folders(into)) {
        fprintf(stderr, "data folder: could not make %s; %s is left where it is\n", into, from);
        return 0;
    }
    folder = opendir(from);
    if (folder == NULL) {
        fprintf(stderr, "data folder: could not read %s; it is left where it is\n", from);
        return 0;
    }
    while ((entry = readdir(folder)) != NULL) {
        int a, b;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        a = snprintf(source, sizeof(source), "%s/%s", from, entry->d_name);
        b = snprintf(target, sizeof(target), "%s/%s", into, entry->d_name);
        if (a < 0 || (size_t)a >= sizeof(source) || b < 0 || (size_t)b >= sizeof(target) || exists(target) ||
            rename(source, target) != 0) {
            left++;
            continue;
        }
        moved = 1;
    }
    closedir(folder);
    if (left > 0) {
        fprintf(stderr, "data folder: %d of %s's files were left there (%s has files of the same names)\n", left, from, into);
    } else if (rmdir(from) != 0) {
        fprintf(stderr, "data folder: could not remove the emptied %s\n", from);
    }
    if (moved) {
        fprintf(stderr, "data folder: moved %s's files to %s\n", from, into);
    }
    return moved;
}

/* Linux's old data folders (config_rules: holyland, then darker-reality) moved into the data folder */
static int move_config_folders(DataDirSystem system, DataDirEnv env) {
    char data[1024], old[1024];
    int moved = 0;

    if (system != DATA_DIR_POSIX || !data_dir_resolve(system, env, data, sizeof(data))) {
        return 0;
    }
    if (resolve(config_rules, COUNT(config_rules), NAME_NOW, system, env, old, sizeof(old)) == 1 && exists(old)) {
        moved += move_contents(old, data);
    }
    if (resolve(config_rules, COUNT(config_rules), NAME_BEFORE, system, env, old, sizeof(old)) == 1 && exists(old)) {
        moved += move_contents(old, data);
    }
    return moved;
}
#endif

int data_dir_move_old_folders(DataDirSystem system, DataDirEnv env) {
    int moved;

    if (nonempty(env, "KF4_DATA_DIR") != NULL || nonempty(env, "KF4_BUILDS_DIR") != NULL) {
        return 0;
    }
    moved = move_old(data_rules, COUNT(data_rules), system, env) + move_old(local_rules, COUNT(local_rules), system, env);
#ifndef _WIN32
    moved += move_config_folders(system, env);
#endif
    return moved;
}

DataDirSystem data_dir_this_system(void) {
#ifdef _WIN32
    return DATA_DIR_WINDOWS;
#else
    return DATA_DIR_POSIX;
#endif
}
