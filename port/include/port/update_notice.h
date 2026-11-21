/* Telling the player about a new release (docs/port/LAUNCHER.md, "Updates"): the rules the launcher
   (launcher/update_check.cpp, which asks GitHub) and the game's overlay (which shows the notice) share.
   No other part of the port is used here, and nothing here touches the network or the clock: callers
   pass the text and the time in, so every rule is tested on its own (tests/update_notice_test.c).

   update.json, in the data folder, is the launcher's answer cache. Only the launcher writes it:
     {"checked": 1795000000, "tag": "v0.2.0", "url": "https://github.com/.../releases/tag/v0.2.0"}
   "checked" is when GitHub was last asked (seconds since 1970). An empty tag means none was found.

   launcher.json, in the data folder, holds the player's choices:
     {"disc": "/path/to/kf4.iso", "update_check": true, "skipped": "v0.2.0", "setup_done": true}
   The launcher writes it while its window is open, the overlay only to set "skipped", and the game
   runs only after the launcher's window has closed: the two never write it at once. */
#ifndef PORT_UPDATE_NOTICE_H
#define PORT_UPDATE_NOTICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_REPOSITORY "Voicedrew11/darker-reality"
#define UPDATE_CHECK_INTERVAL_S 3600 /* at most once an hour, as Verdite's launcher */

typedef struct {
    int64_t checked_s; /* 0: never */
    char tag[64];      /* "" when none */
    char url[512];
} UpdateRelease;

typedef enum {
    UPDATE_CHECK_OFF,   /* KF4_UPDATE_CHECK=0, or the player turned it off */
    UPDATE_CHECK_ON,    /* at most once an hour */
    UPDATE_CHECK_FORCE, /* KF4_UPDATE_CHECK=force: now, whatever the cache says */
} UpdateCheckMode;

/* update.json's text into `out`. Returns 0 (and an empty `out`) when it is missing or broken. */
int update_cache_parse(const char *json, UpdateRelease *out);

/* `release` as update.json's text into `out`. Returns 0 if it did not fit. */
int update_cache_format(const UpdateRelease *release, char *out, size_t out_size);

/* GitHub's answer to GET /repos/<repo>/releases/latest into `out` (tag_name, html_url), checked at
   `now_s`. Returns 0 when the answer is not a release. */
int update_release_parse(const char *github_json, int64_t now_s, UpdateRelease *out);

/* KF4_UPDATE_CHECK's value (NULL when unset) and the player's "Check for updates at launch" */
UpdateCheckMode update_check_mode(const char *env_value, int player_wants_checks);

/* whether to ask GitHub now */
int update_check_due(UpdateCheckMode mode, const UpdateRelease *cache, int64_t now_s);

/* whether to tell the player about `release`: newer than the running version and not the one they
   skipped. A build with no version (a developer's, "") is never told. */
int update_notice_due(const UpdateRelease *release, const char *running_version, const char *skipped_tag);

/* 1 when version `tag` is newer than `current`. A leading 'v' is dropped and each is read as
   dot-separated numbers ("0.10.2"), anything after them ("-rc1", "+abc") ignored: the rule of Verdite's
   UpdateCheck.IsNewer. Either one not a version: 0. */
int update_version_is_newer(const char *tag, const char *current);

/* launcher.json's "skipped" set to `tag` in `json` (the file's text, or NULL for none), written into
   `out` with every other key kept. Returns 0 if it did not fit. */
int update_skip_in_launcher_json(const char *json, const char *tag, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
