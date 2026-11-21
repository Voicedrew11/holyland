/* The data and builds folders (port/include/port/data_dir.h). No other part of the port is used here:
   the launcher links this file on its own. Each folder is the first rule of its table whose variable is set:
   a new place (a new system) is a new row. */
#include "port/data_dir.h"

#include <stdio.h>
#include <sys/stat.h>

typedef enum { ANY_SYSTEM, WINDOWS_ONLY, POSIX_ONLY } RuleSystem;

typedef struct {
    RuleSystem system;
    const char *variable;   /* NULL: always applies */
    const char *format;     /* the path, from the variable's value */
    const char *old_format; /* the path while the port was named Darker Reality (2026-11-18 to 11-20); NULL: none */
} Rule;

typedef enum { NAME_NOW, NAME_BEFORE } FolderName;

/* KF4_DATA_DIR given for one run (tests, a second copy) keeps its builds with it: the builds rule then
   takes the data folder (data_dir_resolve_builds) */
static const Rule data_rules[] = {
    {ANY_SYSTEM, "KF4_DATA_DIR", "%s", NULL},
    {WINDOWS_ONLY, "APPDATA", "%s\\holyland", "%s\\darker-reality"},
    {POSIX_ONLY, "XDG_CONFIG_HOME", "%s/holyland", "%s/darker-reality"},
    {POSIX_ONLY, "HOME", "%s/.config/holyland", "%s/.config/darker-reality"},
    {ANY_SYSTEM, NULL, "holyland", "darker-reality"},
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

int data_dir_resolve(DataDirSystem system, DataDirEnv env, char *out, size_t out_size) {
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
    char parent[1024];
    int n;

    if (given != NULL) {
        n = snprintf(out, out_size, "%s", given);
    } else if (builds_parent(system, env, parent, sizeof(parent))) {
        n = snprintf(out, out_size, system == DATA_DIR_WINDOWS ? "%s\\builds" : "%s/builds", parent);
    } else {
        return 0;
    }
    return n >= 0 && (size_t)n < out_size;
}

static int exists(const char *path) {
    struct stat info;

    return stat(path, &info) == 0;
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

int data_dir_move_old_folders(DataDirSystem system, DataDirEnv env) {
    if (nonempty(env, "KF4_DATA_DIR") != NULL || nonempty(env, "KF4_BUILDS_DIR") != NULL) {
        return 0;
    }
    return move_old(data_rules, COUNT(data_rules), system, env) + move_old(local_rules, COUNT(local_rules), system, env);
}

DataDirSystem data_dir_this_system(void) {
#ifdef _WIN32
    return DATA_DIR_WINDOWS;
#else
    return DATA_DIR_POSIX;
#endif
}
