/* The update notice's rules (port/include/port/update_notice.h). Uses cJSON and nothing else. */
#include "port/update_notice.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static void copy_text(char *out, size_t out_size, const char *text) {
    snprintf(out, out_size, "%s", text != NULL ? text : "");
}

static const char *string_item(const cJSON *object, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

int update_cache_parse(const char *json, UpdateRelease *out) {
    cJSON *root;
    const cJSON *checked;

    memset(out, 0, sizeof(*out));
    if (json == NULL) {
        return 0;
    }
    root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return 0;
    }
    checked = cJSON_GetObjectItemCaseSensitive(root, "checked");
    if (cJSON_IsNumber(checked) && checked->valuedouble > 0) {
        out->checked_s = (int64_t)checked->valuedouble;
    }
    copy_text(out->tag, sizeof(out->tag), string_item(root, "tag"));
    copy_text(out->url, sizeof(out->url), string_item(root, "url"));
    cJSON_Delete(root);
    return 1;
}

static int print_into(cJSON *root, char *out, size_t out_size) {
    char *text = cJSON_Print(root);
    int fits = text != NULL && strlen(text) < out_size;

    if (fits) {
        memcpy(out, text, strlen(text) + 1);
    }
    cJSON_free(text);
    return fits;
}

int update_cache_format(const UpdateRelease *release, char *out, size_t out_size) {
    cJSON *root = cJSON_CreateObject();
    int fits;

    if (root == NULL) {
        return 0;
    }
    cJSON_AddNumberToObject(root, "checked", (double)release->checked_s);
    cJSON_AddStringToObject(root, "tag", release->tag);
    cJSON_AddStringToObject(root, "url", release->url);
    fits = print_into(root, out, out_size);
    cJSON_Delete(root);
    return fits;
}

int update_release_parse(const char *github_json, int64_t now_s, UpdateRelease *out) {
    cJSON *root;
    const char *tag, *url;
    int ok;

    memset(out, 0, sizeof(*out));
    if (github_json == NULL) {
        return 0;
    }
    root = cJSON_Parse(github_json);
    tag = string_item(root, "tag_name");
    url = string_item(root, "html_url");
    /* a URL that is not GitHub's page is never opened: the answer could come from anywhere on the way */
    ok = tag != NULL && url != NULL && strlen(tag) < sizeof(out->tag) && strlen(url) < sizeof(out->url) &&
         strncmp(url, "https://github.com/", 19) == 0;
    if (ok) {
        out->checked_s = now_s;
        copy_text(out->tag, sizeof(out->tag), tag);
        copy_text(out->url, sizeof(out->url), url);
    }
    cJSON_Delete(root);
    return ok;
}

UpdateCheckMode update_check_mode(const char *env_value, int player_wants_checks) {
    if (env_value != NULL && strcmp(env_value, "0") == 0) {
        return UPDATE_CHECK_OFF;
    }
    if (env_value != NULL && strcmp(env_value, "force") == 0) {
        return UPDATE_CHECK_FORCE;
    }
    return player_wants_checks ? UPDATE_CHECK_ON : UPDATE_CHECK_OFF;
}

int update_check_due(UpdateCheckMode mode, const UpdateRelease *cache, int64_t now_s) {
    switch (mode) {
    case UPDATE_CHECK_OFF:
        return 0;
    case UPDATE_CHECK_FORCE:
        return 1;
    case UPDATE_CHECK_ON:
        /* a clock set back counts as due */
        return cache->checked_s <= 0 || now_s < cache->checked_s ||
               now_s - cache->checked_s >= UPDATE_CHECK_INTERVAL_S;
    }
    return 0;
}

int update_notice_due(const UpdateRelease *release, const char *running_version, const char *skipped_tag) {
    if (running_version == NULL || running_version[0] == 0 || release->tag[0] == 0 || release->url[0] == 0) {
        return 0;
    }
    if (skipped_tag != NULL && strcmp(skipped_tag, release->tag) == 0) {
        return 0;
    }
    return update_version_is_newer(release->tag, running_version);
}

/* up to 4 dot-separated numbers; returns how many were read, 0 if `text` is not a version */
static int read_version(const char *text, long parts[4]) {
    int count = 0;

    memset(parts, 0, 4 * sizeof(long));
    if (text == NULL) {
        return 0;
    }
    if (*text == 'v' || *text == 'V') {
        text++;
    }
    while (count < 4 && isdigit((unsigned char)*text)) {
        long value = 0;

        while (isdigit((unsigned char)*text)) {
            if (value < 100000000L) {
                value = value * 10 + (*text - '0');
            }
            text++;
        }
        parts[count++] = value;
        if (*text != '.') {
            break;
        }
        text++;
    }
    return count;
}

int update_version_is_newer(const char *tag, const char *current) {
    long a[4], b[4];
    int i;

    if (read_version(tag, a) == 0 || read_version(current, b) == 0) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return 0;
}

int update_skip_in_launcher_json(const char *json, const char *tag, char *out, size_t out_size) {
    cJSON *root = json != NULL ? cJSON_Parse(json) : NULL;
    int fits;

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (root == NULL) {
        return 0;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, "skipped");
    cJSON_AddStringToObject(root, "skipped", tag != NULL ? tag : "");
    fits = print_into(root, out, out_size);
    cJSON_Delete(root);
    return fits;
}
