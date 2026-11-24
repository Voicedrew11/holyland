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
    check("env 0 turns it off", update_check_mode("0", 1) == UPDATE_CHECK_OFF);
    check("env force", update_check_mode("force", 0) == UPDATE_CHECK_FORCE);
    check("the player's choice", update_check_mode(NULL, 1) == UPDATE_CHECK_ON && update_check_mode("1", 0) == UPDATE_CHECK_OFF);
    check("env force overrides the player's off", update_check_mode("force", 0) == UPDATE_CHECK_FORCE);
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
    static const char *const linux_empty_xdg[] = {"HOME=/home/p", "XDG_DATA_HOME=", NULL};
    static const char *const given[] = {"HOME=/home/p", "KF4_DATA_DIR=/tmp/run", "XDG_DATA_HOME=/d", NULL};
    static const char *const given_builds[] = {"HOME=/home/p", "KF4_BUILDS_DIR=/b", NULL};
    static const char *const windows[] = {"APPDATA=C:\\U\\R", "LOCALAPPDATA=C:\\U\\L", NULL};
    static const char *const nothing[] = {NULL};
    char tiny[8];

    check("linux home", resolves(DATA_DIR_POSIX, linux_home, "/home/p/.local/share/holyland",
                                 "/home/p/.local/share/holyland/builds"));
    check("linux xdg", resolves(DATA_DIR_POSIX, linux_xdg, "/d/holyland", "/d/holyland/builds"));
    check("empty xdg is unset", resolves(DATA_DIR_POSIX, linux_empty_xdg, "/home/p/.local/share/holyland",
                                         "/home/p/.local/share/holyland/builds"));
    check("KF4_DATA_DIR keeps builds with it", resolves(DATA_DIR_POSIX, given, "/tmp/run", "/tmp/run/builds"));
    check("KF4_BUILDS_DIR", resolves(DATA_DIR_POSIX, given_builds, "/home/p/.local/share/holyland", "/b"));
    check("windows", resolves(DATA_DIR_WINDOWS, windows, "C:\\U\\R\\holyland", "C:\\U\\L\\holyland\\builds"));
    check("nothing set", resolves(DATA_DIR_POSIX, nothing, "holyland", "holyland/builds"));
    test_env = linux_home;
    check("too long", !data_dir_resolve(DATA_DIR_POSIX, lookup, tiny, sizeof(tiny)));
}

#ifndef _WIN32
static int is_folder(const char *path) {
    struct stat info;

    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int is_file(const char *path) {
    struct stat info;

    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static void write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "w");

    if (file != NULL) {
        fputs(text, file);
        fclose(file);
    }
}

static int file_says(const char *path, const char *text) {
    char got[64] = {0};
    FILE *file = fopen(path, "r");
    size_t n;

    if (file == NULL) {
        return 0;
    }
    n = fread(got, 1, sizeof(got) - 1, file);
    fclose(file);
    got[n] = 0;
    return strcmp(got, text) == 0;
}

static void in_home(char *out, size_t size, const char *home, const char *path) {
    snprintf(out, size, "%s%s", home, path);
}

/* in a scratch HOME: the darker-reality folders take their new names; the old config folder's files move into
   the data folder; a file already there is never replaced */
static void test_move_old_folders(void) {
    char home[] = "/tmp/data_dir_test_XXXXXX";
    char env_home[64], path[160];
    const char *env[] = {env_home, NULL};
    static const char *const made[] = {"/.config", "/.config/holyland", "/.config/holyland/saves", "/.local",
                                       "/.local/share", "/.local/share/darker-reality",
                                       "/.local/share/darker-reality/builds"};
    static const char *const files[] = {"/.config/holyland/settings.json", "/.config/holyland/saves/save0.kf4save",
                                        "/.config/holyland/launcher.json"};
    static const char *const left[] = {
        "/.local/share/holyland/settings.json", "/.local/share/holyland/saves/save0.kf4save",
        "/.local/share/holyland/launcher.json", "/.config/holyland/launcher.json", "/.local/share/holyland/saves",
        "/.local/share/holyland/builds", "/.config/holyland", "/.local/share/holyland", "/.local/share", "/.local",
        "/.config"};
    size_t i;

    if (mkdtemp(home) == NULL) {
        check("scratch folder", 0);
        return;
    }
    snprintf(env_home, sizeof(env_home), "HOME=%s", home);
    for (i = 0; i < sizeof(made) / sizeof(made[0]); i++) {
        in_home(path, sizeof(path), home, made[i]);
        mkdir(path, 0755);
    }
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        in_home(path, sizeof(path), home, files[i]);
        write_text(path, "old");
    }
    test_env = env;
    check("the old local folder and the config folder moved", data_dir_move_old_folders(DATA_DIR_POSIX, lookup) == 2);
    in_home(path, sizeof(path), home, "/.local/share/holyland/builds");
    check("builds came along", is_folder(path));
    in_home(path, sizeof(path), home, "/.local/share/holyland/saves/save0.kf4save");
    check("saves moved", is_file(path));
    in_home(path, sizeof(path), home, "/.local/share/holyland/settings.json");
    check("settings moved", file_says(path, "old"));
    in_home(path, sizeof(path), home, "/.config/holyland");
    check("the emptied config folder is gone", !is_folder(path));
    check("nothing more to move", data_dir_move_old_folders(DATA_DIR_POSIX, lookup) == 0);

    /* a config folder again (an older build run since), with a file the data folder has too */
    mkdir(path, 0755);
    in_home(path, sizeof(path), home, "/.local/share/holyland/launcher.json");
    write_text(path, "new");
    in_home(path, sizeof(path), home, "/.config/holyland/launcher.json");
    write_text(path, "older");
    check("a file of the same name is not moved", data_dir_move_old_folders(DATA_DIR_POSIX, lookup) == 0);
    in_home(path, sizeof(path), home, "/.local/share/holyland/launcher.json");
    check("the data folder's file is kept", file_says(path, "new"));
    in_home(path, sizeof(path), home, "/.config/holyland/launcher.json");
    check("the old one stays where it was", file_says(path, "older"));

    for (i = 0; i < sizeof(left) / sizeof(left[0]); i++) {
        in_home(path, sizeof(path), home, left[i]);
        if (remove(path) != 0) {
            rmdir(path);
        }
    }
    check("scratch folder emptied", rmdir(home) == 0);
}
#endif

static const char release_json[] =
    "{\"tag_name\":\"v0.3.0\",\"assets\":["
    "{\"name\":\"Holyland-0.3.0-x86_64.AppImage\",\"browser_download_url\":"
    "\"https://github.com/Voicedrew11/holyland/releases/download/v0.3.0/Holyland-0.3.0-x86_64.AppImage\"},"
    "{\"name\":\"Holyland-0.3.0-win-x64.zip\",\"browser_download_url\":"
    "\"https://github.com/Voicedrew11/holyland/releases/download/v0.3.0/Holyland-0.3.0-win-x64.zip\"},"
    "{\"name\":\"Holyland-0.3.0-win-x64-setup.exe\",\"browser_download_url\":"
    "\"https://github.com/Voicedrew11/holyland/releases/download/v0.3.0/Holyland-0.3.0-win-x64-setup.exe\"},"
    "{\"name\":\"SHA256SUMS\",\"browser_download_url\":"
    "\"https://github.com/Voicedrew11/holyland/releases/download/v0.3.0/SHA256SUMS\"},"
    "{\"name\":\"SHA256SUMS.sig\",\"browser_download_url\":"
    "\"https://github.com/Voicedrew11/holyland/releases/download/v0.3.0/SHA256SUMS.sig\"}]}";

static void test_assets(void) {
    char name[128], url[512];
    const char *gh = "https://github.com/";

    check("the AppImage", update_asset_pick(release_json, "v0.3.0", "-x86_64.AppImage", gh, name, sizeof(name), url,
                                            sizeof(url)) &&
                              strcmp(name, "Holyland-0.3.0-x86_64.AppImage") == 0 &&
                              strstr(url, "/v0.3.0/Holyland-0.3.0-x86_64.AppImage") != NULL);
    check("the zip is not the installer",
          update_asset_pick(release_json, "v0.3.0", "-win-x64.zip", gh, name, sizeof(name), url, sizeof(url)) &&
              strcmp(name, "Holyland-0.3.0-win-x64.zip") == 0);
    check("the installer",
          update_asset_pick(release_json, "v0.3.0", "-win-x64-setup.exe", gh, name, sizeof(name), url, sizeof(url)) &&
              strcmp(name, "Holyland-0.3.0-win-x64-setup.exe") == 0);
    check("SHA256SUMS exactly, not its .sig",
          update_asset_pick(release_json, "v0.3.0", "SHA256SUMS", gh, name, sizeof(name), url, sizeof(url)) &&
              strcmp(name, "SHA256SUMS") == 0);
    check("the .sig", update_asset_pick(release_json, "v0.3.0", "SHA256SUMS.sig", gh, name, sizeof(name), url,
                                        sizeof(url)) &&
                          strcmp(name, "SHA256SUMS.sig") == 0);
    check("another release's answer is refused",
          !update_asset_pick(release_json, "v0.4.0", "-x86_64.AppImage", gh, name, sizeof(name), url, sizeof(url)) &&
              name[0] == 0 && url[0] == 0);
    check("no such asset", !update_asset_pick(release_json, "v0.3.0", "-macos.dmg", gh, name, sizeof(name), url,
                                              sizeof(url)));
    check("a URL too long for the room is refused",
          !update_asset_pick(release_json, "v0.3.0", "-win-x64-setup.exe", gh, name, sizeof(name), url, 10));
    check("two that match are refused",
          !update_asset_pick("{\"tag_name\":\"v1\",\"assets\":["
                             "{\"name\":\"a-x86_64.AppImage\",\"browser_download_url\":\"https://github.com/a\"},"
                             "{\"name\":\"b-x86_64.AppImage\",\"browser_download_url\":\"https://github.com/b\"}]}",
                             "v1", "-x86_64.AppImage", gh, name, sizeof(name), url, sizeof(url)));
    check("a URL elsewhere is refused",
          !update_asset_pick("{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"a-x86_64.AppImage\","
                             "\"browser_download_url\":\"https://example.com/a-x86_64.AppImage\"}]}",
                             "v1", "-x86_64.AppImage", gh, name, sizeof(name), url, sizeof(url)));
    check("a test's prefix",
          update_asset_pick("{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"a-x86_64.AppImage\","
                            "\"browser_download_url\":\"file:///tmp/a-x86_64.AppImage\"}]}",
                            "v1", "-x86_64.AppImage", "file://", name, sizeof(name), url, sizeof(url)));
    check("a name with a path is refused",
          !update_asset_pick("{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"../a-x86_64.AppImage\","
                             "\"browser_download_url\":\"https://github.com/a\"}]}",
                             "v1", "-x86_64.AppImage", gh, name, sizeof(name), url, sizeof(url)));
    check("not JSON", !update_asset_pick("<html>", "v1", "-x", gh, name, sizeof(name), url, sizeof(url)) &&
                          !update_asset_pick(NULL, "v1", "-x", gh, name, sizeof(name), url, sizeof(url)));
}

static void test_sums(void) {
    char hex[65];
    const char *sums = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08  a.AppImage\n"
                       "60303AE22B998861BCE3B28F33EEC1BE758A213C86C93C076DBE9F558C11C752 *b.zip\r\n"
                       "zz303ae22b998861bce3b28f33eec1be758a213c86c93c076dbe9f558c11c752  bad.exe\n"
                       "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08  a.AppImage.old";

    check("a sum", update_sums_lookup(sums, "a.AppImage", hex) &&
                       strcmp(hex, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08") == 0);
    check("binary mark, CRLF, upper case read as lower",
          update_sums_lookup(sums, "b.zip", hex) &&
              strcmp(hex, "60303ae22b998861bce3b28f33eec1be758a213c86c93c076dbe9f558c11c752") == 0);
    check("not hex", !update_sums_lookup(sums, "bad.exe", hex) && hex[0] == 0);
    check("a name only a prefix of another's", update_sums_lookup(sums, "a.AppImage.old", hex) &&
                                                   !update_sums_lookup(sums, "a.AppIm", hex));
    check("none", !update_sums_lookup(sums, "c", hex) && !update_sums_lookup(NULL, "c", hex) &&
                      !update_sums_lookup("", "c", hex));
}

static void test_hex(void) {
    unsigned char bytes[4];

    check("hex bytes", update_hex_bytes(" 0aFf10e3\n", bytes, 4) && bytes[0] == 0x0a && bytes[1] == 0xff &&
                           bytes[2] == 0x10 && bytes[3] == 0xe3);
    check("too short", !update_hex_bytes("0aff10", bytes, 4));
    check("too long", !update_hex_bytes("0aff10e300", bytes, 4));
    check("not hex", !update_hex_bytes("0aff10eg", bytes, 4) && !update_hex_bytes(NULL, bytes, 4));
}

static void test_request(void) {
    char text[256], tag[64];

    check("a request round trip", update_request_format("v0.3.0", text, sizeof(text)) &&
                                      update_request_parse(text, tag, sizeof(tag)) && strcmp(tag, "v0.3.0") == 0);
    check("no version is no request", !update_request_parse("{\"tag\":\"preview\"}", tag, sizeof(tag)) && tag[0] == 0);
    check("broken", !update_request_parse("{", tag, sizeof(tag)) && !update_request_parse(NULL, tag, sizeof(tag)));
    check("too small", !update_request_format("v0.3.0", text, 4));
}

int main(void) {
    test_versions();
    test_github_answer();
    test_cache();
    test_when_to_ask();
    test_when_to_tell();
    test_skip();
    test_assets();
    test_sums();
    test_hex();
    test_request();
    test_data_dir();
#ifndef _WIN32
    test_move_old_folders();
#endif
    printf("%d/%d passed\n", checked - failed, checked);
    return failed != 0;
}
