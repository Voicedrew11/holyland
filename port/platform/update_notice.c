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

/* ---- Updating --------------------------------------------------------------------------------------------- */

static int ends_with(const char *text, const char *suffix) {
    size_t n = strlen(text), m = strlen(suffix);

    return n >= m && strcmp(text + n - m, suffix) == 0;
}

int update_asset_pick(const char *github_json, const char *tag, const char *name_suffix, const char *url_prefix,
                      char *name, size_t name_size, char *url, size_t url_size) {
    cJSON *root;
    const cJSON *assets, *asset;
    const char *found_name = NULL, *found_url = NULL;
    int matches = 0, ok;

    if (name_size > 0) {
        name[0] = 0;
    }
    if (url_size > 0) {
        url[0] = 0;
    }
    if (github_json == NULL || tag == NULL || name_suffix == NULL || name_suffix[0] == 0 || url_prefix == NULL) {
        return 0;
    }
    root = cJSON_Parse(github_json);
    {
        const char *release_tag = string_item(root, "tag_name");

        if (release_tag == NULL || strcmp(release_tag, tag) != 0) {
            cJSON_Delete(root);
            return 0;
        }
    }
    assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (!cJSON_IsArray(assets)) {
        assets = NULL;
    }
    cJSON_ArrayForEach(asset, assets) {
        const char *asset_name = string_item(asset, "name");
        const char *asset_url = string_item(asset, "browser_download_url");

        if (asset_name == NULL || asset_url == NULL) {
            continue;
        }
        if (name_suffix[0] == '-' ? !ends_with(asset_name, name_suffix) : strcmp(asset_name, name_suffix) != 0) {
            continue;
        }
        matches++;
        found_name = asset_name;
        found_url = asset_url;
    }
    /* a name with a path in it or a URL elsewhere is never taken: the answer could come from anywhere on the way */
    ok = matches == 1 && strncmp(found_url, url_prefix, strlen(url_prefix)) == 0 && strchr(found_name, '/') == NULL &&
         strchr(found_name, '\\') == NULL && strlen(found_name) < name_size && strlen(found_url) < url_size;
    if (ok) {
        copy_text(name, name_size, found_name);
        copy_text(url, url_size, found_url);
    }
    cJSON_Delete(root);
    return ok;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int update_sums_lookup(const char *sums_text, const char *file_name, char hex[65]) {
    const char *line = sums_text;
    size_t name_length;

    hex[0] = 0;
    if (sums_text == NULL || file_name == NULL || file_name[0] == 0) {
        return 0;
    }
    name_length = strlen(file_name);
    while (*line != 0) {
        const char *end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        const char *name;
        size_t i;

        if (length > 0 && line[length - 1] == '\r') {
            length--;
        }
        /* "<64 hex><space><space or *><name>" */
        name = line + 66;
        if (length == 66 + name_length && line[64] == ' ' && (line[65] == ' ' || line[65] == '*') &&
            memcmp(name, file_name, name_length) == 0) {
            for (i = 0; i < 64; i++) {
                int digit = hex_digit((unsigned char)line[i]);

                if (digit < 0) {
                    hex[0] = 0;
                    return 0;
                }
                hex[i] = "0123456789abcdef"[digit];
            }
            hex[64] = 0;
            return 1;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return 0;
}

int update_hex_bytes(const char *text, unsigned char *out, size_t size) {
    size_t i;

    if (text == NULL) {
        return 0;
    }
    while (isspace((unsigned char)*text)) {
        text++;
    }
    for (i = 0; i < size; i++) {
        int high = hex_digit((unsigned char)text[2 * i]);
        int low = high < 0 ? -1 : hex_digit((unsigned char)text[2 * i + 1]);

        if (high < 0 || low < 0) {
            return 0;
        }
        out[i] = (unsigned char)(high << 4 | low);
    }
    text += 2 * size;
    while (isspace((unsigned char)*text)) {
        text++;
    }
    return *text == 0;
}

int update_request_parse(const char *json, char *tag, size_t tag_size) {
    cJSON *root;
    const char *value;
    long parts[4];
    int ok;

    if (tag_size > 0) {
        tag[0] = 0;
    }
    if (json == NULL) {
        return 0;
    }
    root = cJSON_Parse(json);
    value = string_item(root, "tag");
    ok = value != NULL && strlen(value) < tag_size && read_version(value, parts) > 0;
    if (ok) {
        copy_text(tag, tag_size, value);
    }
    cJSON_Delete(root);
    return ok;
}

int update_request_format(const char *tag, char *out, size_t out_size) {
    cJSON *root = cJSON_CreateObject();
    int fits;

    if (root == NULL) {
        return 0;
    }
    cJSON_AddStringToObject(root, "tag", tag != NULL ? tag : "");
    fits = print_into(root, out, out_size);
    cJSON_Delete(root);
    return fits;
}
