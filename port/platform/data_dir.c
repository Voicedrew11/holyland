/* The data and builds folders (port/include/port/data_dir.h). No other part of the port is used here:
   the launcher links this file on its own. Each folder is the first rule of its table whose variable is set:
   a new place (a new system) is a new row. */
#include "port/data_dir.h"

#include <stdio.h>

typedef enum { ANY_SYSTEM, WINDOWS_ONLY, POSIX_ONLY } RuleSystem;

typedef struct {
    RuleSystem system;
    const char *variable; /* NULL: always applies */
    const char *format;   /* the path, from the variable's value */
} Rule;

/* KF4_DATA_DIR given for one run (tests, a second copy) keeps its builds with it: the builds rule then
   takes the data folder (data_dir_resolve_builds) */
static const Rule data_rules[] = {
    {ANY_SYSTEM, "KF4_DATA_DIR", "%s"},
    {WINDOWS_ONLY, "APPDATA", "%s\\holyland"},
    {POSIX_ONLY, "XDG_CONFIG_HOME", "%s/holyland"},
    {POSIX_ONLY, "HOME", "%s/.config/holyland"},
    {ANY_SYSTEM, NULL, "holyland"},
};

static const Rule builds_rules[] = {
    {ANY_SYSTEM, "KF4_BUILDS_DIR", "%s"},
    {WINDOWS_ONLY, "LOCALAPPDATA", "%s\\holyland\\builds"},
    {POSIX_ONLY, "XDG_DATA_HOME", "%s/holyland/builds"},
    {POSIX_ONLY, "HOME", "%s/.local/share/holyland/builds"},
};

static int applies(RuleSystem rule, DataDirSystem system) {
    return rule == ANY_SYSTEM || (rule == WINDOWS_ONLY) == (system == DATA_DIR_WINDOWS);
}

static const char *nonempty(DataDirEnv env, const char *name) {
    const char *value = env(name);

    return value != NULL && *value != 0 ? value : NULL;
}

/* the first rule that applies, written into `out`: 1, 0 if it did not fit, -1 if none applied */
static int resolve(const Rule *rules, size_t count, DataDirSystem system, DataDirEnv env, char *out, size_t out_size) {
    size_t i;

    for (i = 0; i < count; i++) {
        const char *value = rules[i].variable != NULL ? nonempty(env, rules[i].variable) : "";
        int n;

        if (!applies(rules[i].system, system) || value == NULL) {
            continue;
        }
        n = snprintf(out, out_size, rules[i].format, value);
        return n >= 0 && (size_t)n < out_size;
    }
    return -1;
}

int data_dir_resolve(DataDirSystem system, DataDirEnv env, char *out, size_t out_size) {
    return resolve(data_rules, sizeof(data_rules) / sizeof(data_rules[0]), system, env, out, out_size) == 1;
}

int data_dir_resolve_builds(DataDirSystem system, DataDirEnv env, char *out, size_t out_size) {
    char data[1024];
    int n;

    if (nonempty(env, "KF4_BUILDS_DIR") != NULL || nonempty(env, "KF4_DATA_DIR") == NULL) {
        int found = resolve(builds_rules, sizeof(builds_rules) / sizeof(builds_rules[0]), system, env, out, out_size);

        if (found >= 0) {
            return found;
        }
    }
    if (!data_dir_resolve(system, env, data, sizeof(data))) {
        return 0;
    }
    n = snprintf(out, out_size, system == DATA_DIR_WINDOWS ? "%s\\builds" : "%s/builds", data);
    return n >= 0 && (size_t)n < out_size;
}

DataDirSystem data_dir_this_system(void) {
#ifdef _WIN32
    return DATA_DIR_WINDOWS;
#else
    return DATA_DIR_POSIX;
#endif
}
