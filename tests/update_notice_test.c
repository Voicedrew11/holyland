/* port/platform/update_notice.c: the update notice's rules, which the launcher and the overlay share; and
   port/platform/data_dir.c, where both look for the player's files.

     update_notice_test */
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "port/data_dir.h"
#include "port/update_notice.h"

static int failed, checked;

static void check(const char *what, int ok) {
    checked++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static void test_versions(void) {
    check("newer patch", update_version_is_newer("v0.1.1", "0.1.0"));
    check("newer minor beats bigger patch", update_version_is_newer("0.2.0", "0.1.9"));
    check("numbers, not text", update_version_is_newer("v0.10.0", "0.9.0"));
    check("same is not newer", !update_version_is_newer("v1.0.0", "1.0.0"));
    check("older is not newer", !update_version_is_newer("v0.9", "1.0"));
    check("missing parts are 0", !update_version_is_newer("v1.0", "1.0.0") && update_version_is_newer("1.0.1", "1"));
    check("suffix ignored", !update_version_is_newer("v1.0.0-rc1", "1.0.0") && update_version_is_newer("V2.0+abc", "1.5"));
    check("not a version", !update_version_is_newer("preview", "0.1.0") && !update_version_is_newer("v1", ""));
    check("NULL", !update_version_is_newer(NULL, "1") && !update_version_is_newer("2", NULL));
}

static void test_github_answer(void) {
    UpdateRelease r;

    check("a release", update_release_parse("{\"tag_name\":\"v0.2.0\",\"html_url\":"
                                            "\"https://github.com/Voicedrew11/darker-reality/releases/tag/v0.2.0\",\"draft\":false}",
                                            1000, &r) &&
                           strcmp(r.tag, "v0.2.0") == 0 && r.checked_s == 1000 &&
                           strcmp(r.url, "https://github.com/Voicedrew11/darker-reality/releases/tag/v0.2.0") == 0);
    check("not GitHub's page is refused",
          !update_release_parse("{\"tag_name\":\"v9\",\"html_url\":\"https://example.com/x\"}", 1, &r) && r.tag[0] == 0);
    check("no tag", !update_release_parse("{\"message\":\"Not Found\"}", 1, &r));
    check("broken text", !update_release_parse("{\"tag_name\":", 1, &r));
    check("an array", !update_release_parse("[1,2]", 1, &r));
    check("NULL", !update_release_parse(NULL, 1, &r));
}

static void test_cache(void) {
    UpdateRelease a = {1795000000, "v0.3.1", "https://github.com/Voicedrew11/darker-reality/releases/tag/v0.3.1"}, b;
    char text[1024];

    check("written", update_cache_format(&a, text, sizeof(text)));
    check("read back", update_cache_parse(text, &b) && b.checked_s == a.checked_s && strcmp(b.tag, a.tag) == 0 &&
                           strcmp(b.url, a.url) == 0);
    check("too small a buffer", !update_cache_format(&a, text, 10));
    check("missing file", !update_cache_parse(NULL, &b) && b.checked_s == 0 && b.tag[0] == 0);
    check("broken file", !update_cache_parse("{oops", &b) && b.tag[0] == 0);
    check("a long tag is cut, not overrun",
          update_cache_parse("{\"tag\":\"v1234567890123456789012345678901234567890123456789012345678901234567890\"}", &b) &&
              strlen(b.tag) == sizeof(b.tag) - 1);
}

static void test_when_to_ask(void) {
    UpdateRelease never = {0}, recent = {10000, "v1", "https://github.com/x"};

    check("env 0 turns it off", update_check_mode("0", 1) == UPDATE_CHECK_OFF);
    check("env force", update_check_mode("force", 0) == UPDATE_CHECK_FORCE);
    check("the player's choice", update_check_mode(NULL, 1) == UPDATE_CHECK_ON && update_check_mode("1", 0) == UPDATE_CHECK_OFF);
    check("never asked: due", update_check_due(UPDATE_CHECK_ON, &never, 5));
    check("within the hour: not due", !update_check_due(UPDATE_CHECK_ON, &recent, 10000 + UPDATE_CHECK_INTERVAL_S - 1));
    check("an hour on: due", update_check_due(UPDATE_CHECK_ON, &recent, 10000 + UPDATE_CHECK_INTERVAL_S));
    check("clock set back: due", update_check_due(UPDATE_CHECK_ON, &recent, 9000));
    check("force: due", update_check_due(UPDATE_CHECK_FORCE, &recent, 10001));
    check("off: never", !update_check_due(UPDATE_CHECK_OFF, &never, 5));
}

static void test_when_to_tell(void) {
    UpdateRelease r = {1, "v0.2.0", "https://github.com/Voicedrew11/darker-reality/releases/tag/v0.2.0"};
    UpdateRelease none = {1, "", ""};

    check("newer: told", update_notice_due(&r, "0.1.0", NULL));
    check("same: not told", !update_notice_due(&r, "0.2.0", ""));
    check("skipped: not told", !update_notice_due(&r, "0.1.0", "v0.2.0"));
    check("skipped an older one: told", update_notice_due(&r, "0.1.0", "v0.1.5"));
    check("developer's build: never", !update_notice_due(&r, "", NULL) && !update_notice_due(&r, NULL, NULL));
    check("no release: not told", !update_notice_due(&none, "0.1.0", NULL));
}

static void test_skip(void) {
    char text[512];
    UpdateRelease dummy;

    (void)dummy;
    check("kept keys", update_skip_in_launcher_json("{\"disc\":\"/d/kf4.iso\",\"update_check\":false,\"skipped\":\"v1\"}",
                                                    "v2", text, sizeof(text)) &&
                           strstr(text, "\"disc\":\t\"/d/kf4.iso\"") != NULL && strstr(text, "\"update_check\":\tfalse") != NULL &&
                           strstr(text, "\"skipped\":\t\"v2\"") != NULL && strstr(text, "\"v1\"") == NULL);
    check("no file yet", update_skip_in_launcher_json(NULL, "v3", text, sizeof(text)) && strstr(text, "\"v3\"") != NULL);
    check("broken file replaced", update_skip_in_launcher_json("[", "v3", text, sizeof(text)) && strstr(text, "\"v3\"") != NULL);
}

/* the environment of a test: name=value pairs */
static const char *const *test_env;

static const char *lookup(const char *name) {
    size_t n = strlen(name);
    int i;

    for (i = 0; test_env[i] != NULL; i++) {
        if (strncmp(test_env[i], name, n) == 0 && test_env[i][n] == '=') {
            return test_env[i] + n + 1;
        }
    }
    return NULL;
}

static int resolves(DataDirSystem system, const char *const *env, const char *data, const char *builds) {
    char a[256], b[256];

    test_env = env;
    return data_dir_resolve(system, lookup, a, sizeof(a)) && data_dir_resolve_builds(system, lookup, b, sizeof(b)) &&
           strcmp(a, data) == 0 && strcmp(b, builds) == 0;
}

static void test_data_dir(void) {
    static const char *const linux_home[] = {"HOME=/home/p", NULL};
    static const char *const linux_xdg[] = {"HOME=/home/p", "XDG_CONFIG_HOME=/c", "XDG_DATA_HOME=/d", NULL};
    static const char *const linux_empty_xdg[] = {"HOME=/home/p", "XDG_CONFIG_HOME=", NULL};
    static const char *const given[] = {"HOME=/home/p", "KF4_DATA_DIR=/tmp/run", "XDG_DATA_HOME=/d", NULL};
    static const char *const given_builds[] = {"HOME=/home/p", "KF4_BUILDS_DIR=/b", NULL};
    static const char *const windows[] = {"APPDATA=C:\\U\\R", "LOCALAPPDATA=C:\\U\\L", NULL};
    static const char *const nothing[] = {NULL};
    char tiny[8];

    check("linux home", resolves(DATA_DIR_POSIX, linux_home, "/home/p/.config/darker-reality", "/home/p/.local/share/darker-reality/builds"));
    check("linux xdg", resolves(DATA_DIR_POSIX, linux_xdg, "/c/darker-reality", "/d/darker-reality/builds"));
    check("empty xdg is unset", resolves(DATA_DIR_POSIX, linux_empty_xdg, "/home/p/.config/darker-reality",
                                         "/home/p/.local/share/darker-reality/builds"));
    check("KF4_DATA_DIR keeps builds with it", resolves(DATA_DIR_POSIX, given, "/tmp/run", "/tmp/run/builds"));
    check("KF4_BUILDS_DIR", resolves(DATA_DIR_POSIX, given_builds, "/home/p/.config/darker-reality", "/b"));
    check("windows", resolves(DATA_DIR_WINDOWS, windows, "C:\\U\\R\\darker-reality", "C:\\U\\L\\darker-reality\\builds"));
    check("nothing set", resolves(DATA_DIR_POSIX, nothing, "darker-reality", "darker-reality/builds"));
    test_env = linux_home;
    check("too long", !data_dir_resolve(DATA_DIR_POSIX, lookup, tiny, sizeof(tiny)));
}

#ifndef _WIN32
static int is_folder(const char *path) {
    struct stat info;

    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* in a scratch HOME: the old folders move once; a folder already under the new name is never replaced */
static void test_move_old_folders(void) {
    char home[] = "/tmp/data_dir_test_XXXXXX";
    char env_home[64], path[128];
    const char *env[] = {env_home, NULL};
    static const char *const made[] = {"/.config", "/.config/holyland", "/.local", "/.local/share",
                                       "/.local/share/holyland", "/.local/share/holyland/builds"};
    static const char *const left[] = {"/.config/holyland", "/.config/darker-reality", "/.config",
                                       "/.local/share/darker-reality/builds", "/.local/share/darker-reality",
                                       "/.local/share", "/.local"};
    size_t i;

    if (mkdtemp(home) == NULL) {
        check("scratch folder", 0);
        return;
    }
    snprintf(env_home, sizeof(env_home), "HOME=%s", home);
    for (i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", home, made[i]);
        mkdir(path, 0755);
    }
    test_env = env;
    check("both moved", data_dir_move_old_folders(DATA_DIR_POSIX, lookup) == 2);
    snprintf(path, sizeof(path), "%s/.config/darker-reality", home);
    check("data folder has its new name", is_folder(path));
    snprintf(path, sizeof(path), "%s/.local/share/darker-reality/builds", home);
    check("builds came along", is_folder(path));
    snprintf(path, sizeof(path), "%s/.config/holyland", home);
    mkdir(path, 0755);
    check("a new folder is never replaced", data_dir_move_old_folders(DATA_DIR_POSIX, lookup) == 0 && is_folder(path));
    for (i = 0; i < sizeof(left) / sizeof(left[0]); i++) {
        snprintf(path, sizeof(path), "%s%s", home, left[i]);
        rmdir(path);
    }
    rmdir(home);
}
#endif

int main(void) {
    test_versions();
    test_github_answer();
    test_cache();
    test_when_to_ask();
    test_when_to_tell();
    test_skip();
    test_data_dir();
#ifndef _WIN32
    test_move_old_folders();
#endif
    printf("%d/%d passed\n", checked - failed, checked);
    return failed != 0;
}
