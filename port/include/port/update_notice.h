/* Telling the player about a new release (docs/port/LAUNCHER.md, "Updates"): the rules the launcher
   (launcher/update_check.cpp, which asks GitHub) and the game's overlay (which shows the notice) share.
   No other part of the port is used here, and nothing here touches the network or the clock: callers
   pass the text and the time in, so every rule is tested on its own (tests/update_notice_test.c).

   update.json, in the data folder, is the launcher's answer cache. Only the launcher writes it:
     {"checked": 1795000000, "tag": "v0.2.0", "url": "https://github.com/.../releases/tag/v0.2.0"}
   "checked" is when GitHub last answered (seconds since 1970). An empty tag means none was found.

   launcher.json, in the data folder, holds the player's choices:
     {"disc": "/path/to/kf4.iso", "update_check": true, "skipped": "v0.2.0", "setup_done": true}
   The launcher writes it while its window is open, the overlay only to set "skipped", and the game
   runs only after the launcher's window has closed: the two never write it at once.

   update-request.json, in the data folder, is the game asking the launcher to update once it has quit (the player
   chose Update at the title or in the F1 menu): {"tag": "v0.3.0"}. The game writes it, the launcher reads and
   removes it. */
#ifndef PORT_UPDATE_NOTICE_H
#define PORT_UPDATE_NOTICE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_REPOSITORY "Voicedrew11/holyland"

typedef struct {
    int64_t checked_s; /* 0: never */
    char tag[64];      /* "" when none */
    char url[512];
} UpdateRelease;

typedef enum {
    UPDATE_CHECK_OFF,   /* KF4_UPDATE_CHECK=0, or the player turned it off */
    UPDATE_CHECK_ON,    /* once each time the launcher starts */
    UPDATE_CHECK_FORCE, /* KF4_UPDATE_CHECK=force: each start, even when the player turned it off */
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

/* RECOMPLAUNCHER's value (NULL when unset). 1 when RecompLauncher started this copy (it sets RECOMPLAUNCHER=1 for every
   game it starts) and so installs Holyland's releases itself: this copy then neither asks GitHub nor offers an update,
   or the two would replace the same files. */
int update_left_to_recomplauncher(const char *recomplauncher_env);

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

/* ---- Updating (launcher/self_update.cpp) -------------------------------------------------------------------- */

#define UPDATE_SUMS_ASSET "SHA256SUMS"         /* every asset's SHA-256, as sha256sum writes it */
#define UPDATE_SIGNATURE_ASSET "SHA256SUMS.sig" /* SHA256SUMS's Ed25519 signature, 128 hex digits */

/* The asset of GitHub's answer about one release (GET /repos/<repo>/releases/tags/<tag> or .../latest) whose name
   ends with `name_suffix` when that starts with '-' ("-x86_64.AppImage"), else is it exactly ("SHA256SUMS"). Its name and its
   browser_download_url into `name` and `url`. The URL must start with `url_prefix` ("https://github.com/"; a test's
   answer: "file://"). Returns 0 when the answer is not a release of tag `tag`, when no asset or more than one
   matches, or when it does not fit. */
int update_asset_pick(const char *github_json, const char *tag, const char *name_suffix, const char *url_prefix,
                      char *name, size_t name_size, char *url, size_t url_size);

/* The SHA-256 SHA256SUMS gives for `file_name` (lines "<64 hex>  <name>" or "<64 hex> *<name>") into `hex`
   (65 bytes: 64 lowercase digits and the end). Returns 0 when it gives none or the line is not a SHA-256. */
int update_sums_lookup(const char *sums_text, const char *file_name, char hex[65]);

/* Hex text (whitespace around it ignored) into exactly `size` bytes. Returns 0 when it is anything else. */
int update_hex_bytes(const char *text, unsigned char *out, size_t size);

/* update-request.json's text into `tag`. Returns 0 when it is missing, broken or names no version. */
int update_request_parse(const char *json, char *tag, size_t tag_size);

/* update-request.json's text for `tag`. Returns 0 if it did not fit. */
int update_request_format(const char *tag, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
