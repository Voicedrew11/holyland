/* Mods: replacement game files and text (port/include/port/mods.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "cJSON.h"
#include "port/mods.h"

#ifndef MODS_TEST
#include "port/host.h"
#include "port/platform.h"
#include "port/settings.h"
#endif

#define MESSAGE_TABLE_WORDS 0x2000      /* font.txc: 0x2000 32-bit offsets, the first the table's own size */
#define MESSAGE_TEXT_ROOM 0x5FC00       /* the room font.txc has: from 0x1A0400 to the end of the 2 MB menu heap */
#define ITEM_RECORD 0x40                /* param/item_nam.prm */
#define ITEM_NAME_WORDS 23              /* the US records' name field: 0x2E bytes, then the item's numbers */
#define ITEM_COUNT 380
#define SPELL_RECORD 0x110              /* param/magic.prm */
#define SPELL_NAME_WORDS 24
#define SPELL_COUNT 256

#define CODE_LATIN 0x0800
#define CODE_NEW_LINE 0x8000
#define CODE_END 0xFFFF

/* ---- Text --------------------------------------------------------------------------------------- */

/* One UTF-8 character at *p (advancing p); 0 at the end. Bad bytes come back as themselves. */
static unsigned next_char(const unsigned char **p) {
    const unsigned char *s = *p;
    unsigned c = s[0];

    if (c == 0) {
        return 0;
    }
    if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *p += 3;
        return ((c & 0x0F) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    }
    if (c >= 0xC0 && c < 0xE0 && (s[1] & 0xC0) == 0x80) {
        *p += 2;
        return ((c & 0x1F) << 6) | (s[1] & 0x3Fu);
    }
    if (c >= 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *p += 4;
        return 0xFFFD;
    }
    *p += 1;
    return c;
}

int mods_encode_text(const char *text, uint16_t *out, int max, int *unknown) {
    const unsigned char *p = (const unsigned char *)text;
    int n = 0;

#define PUT(code)                                                                                                 \
    do {                                                                                                          \
        if (n < max) {                                                                                            \
            out[n++] = (uint16_t)(code);                                                                          \
        }                                                                                                         \
    } while (0)

    while (*p) {
        unsigned c;
        unsigned kind = 0, digit = 0;

        /* {numN} {nN} {cN}: the game's number and colour codes */
        if (*p == '{') {
            const char *s = (const char *)p + 1;
            int len = 0;

            if (strncmp(s, "num", 3) == 0) {
                kind = 0x2000, len = 3;
            } else if (*s == 'n') {
                kind = 0x1000, len = 1;
            } else if (*s == 'c') {
                kind = 0x4000, len = 1;
            }
            if (kind && s[len] >= '0' && s[len] <= '9' && s[len + 1] == '}') {
                digit = (unsigned)(s[len] - '0');
                PUT(kind | (digit << 8));
                p = (const unsigned char *)s + len + 2;
                continue;
            }
        }
        c = next_char(&p);
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            PUT(CODE_NEW_LINE);
            continue;
        }
        switch (c) {
        case 0x2026: /* ellipsis */
            PUT(CODE_LATIN + ('.' - 0x20));
            PUT(CODE_LATIN + ('.' - 0x20));
            c = '.';
            break;
        case 0x2018: case 0x2019: case 0x00B4: c = '\''; break;
        case 0x201C: case 0x201D: c = '"'; break;
        case 0x2013: case 0x2014: case 0x2212: c = '-'; break;
        case 0x00A0: case 0x3000: c = ' '; break;
        default: break;
        }
        if (c < 0x20 || c > 0x7E) {
            if (unknown) {
                (*unknown)++;
            }
            c = '?';
        }
        PUT(CODE_LATIN + (c - 0x20));
    }
#undef PUT
    return n;
}

/* ---- font.txc ------------------------------------------------------------------------------------ */

static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24);
}

unsigned char *mods_build_messages(const unsigned char *original, size_t size, char *const *messages, int count,
                                   size_t *out_size) {
    size_t room, used, table = MESSAGE_TABLE_WORDS * 4;
    unsigned char *out;
    int id;

    if (size < table || get32(original) != MESSAGE_TABLE_WORDS) {
        return NULL;
    }
    room = table;
    for (id = 0; id + 1 < MESSAGE_TABLE_WORDS; id++) {
        uint32_t at = get32(original + (id + 1) * 4);

        if (id < count && messages[id] != NULL) {
            room += (strlen(messages[id]) * 3 + 1) * 2; /* at most three codes a byte, and the end mark */
        } else if (at != 0 && at < size) {
            size_t end = at;

            while (end + 1 < size && (original[end] | (original[end + 1] << 8)) != CODE_END) {
                end += 2;
            }
            room += end - at + 2;
        }
    }
    out = calloc(1, room + 2);
    if (out == NULL) {
        return NULL;
    }
    put32(out, MESSAGE_TABLE_WORDS);
    used = table;
    for (id = 0; id + 1 < MESSAGE_TABLE_WORDS; id++) {
        uint32_t at = get32(original + (id + 1) * 4);
        uint16_t *codes = (uint16_t *)(out + used); /* (2-aligned: used stays even) */
        int n = 0;

        if (id < count && messages[id] != NULL) {
            n = mods_encode_text(messages[id], codes, (int)((room - used) / 2) - 1, NULL);
        } else if (at != 0 && at + 1 < size) {
            const unsigned char *s = original + at;

            while ((size_t)(s - original) + 1 < size && (s[0] | (s[1] << 8)) != CODE_END &&
                   used + (size_t)n * 2 + 4 <= room) {
                codes[n++] = (uint16_t)(s[0] | (s[1] << 8));
                s += 2;
            }
        } else {
            continue; /* unused: offset 0 */
        }
        if (used + (size_t)n * 2 + 2 > room) {
            free(out);
            return NULL;
        }
        codes[n] = CODE_END;
        put32(out + (id + 1) * 4, (uint32_t)used);
        used += (size_t)(n + 1) * 2;
    }
    *out_size = used;
    return out;
}

/* ---- Name records --------------------------------------------------------------------------------- */

void mods_put_names(unsigned char *records, size_t size, size_t record_size, int name_words, char *const *names,
                    int count, int *cut) {
    int n;

    for (n = 0; n < count && (size_t)(n + 1) * record_size <= size; n++) {
        uint16_t codes[64];
        unsigned char *r = records + (size_t)n * record_size;
        int len, i;

        if (names[n] == NULL) {
            continue;
        }
        len = mods_encode_text(names[n], codes, 64, NULL);
        if (len > name_words - 1) {
            len = name_words - 1;
            if (cut) {
                (*cut)++;
            }
        }
        for (i = 0; i < name_words; i++) {
            uint16_t code = i < len ? codes[i] : CODE_END;

            r[i * 2] = (unsigned char)code;
            r[i * 2 + 1] = (unsigned char)(code >> 8);
        }
    }
}

#ifndef MODS_TEST
/* ---- Reading the mods ------------------------------------------------------------------------------ */

static char *messages[MESSAGE_TABLE_WORDS];
static char *items[ITEM_COUNT];
static char *spells[SPELL_COUNT];
static int text_found;

static unsigned char *read_whole(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    unsigned char *data = NULL;
    long length;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (length = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc((size_t)length + 1);
        if (data != NULL && fread(data, 1, (size_t)length, f) != (size_t)length) {
            free(data);
            data = NULL;
        }
        if (data != NULL) {
            data[length] = 0;
            *size = (size_t)length;
        }
    }
    fclose(f);
    return data;
}

static int is_folder(const char *path) {
    struct stat info;

    return stat(path, &info) == 0 && (info.st_mode & S_IFMT) == S_IFDIR;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The names in a folder, sorted; free each and the list */
static char **list_folder(const char *folder, int *count) {
    char **names = NULL;
    int n = 0, capacity = 0;
#ifdef _WIN32
    char pattern[1024];
    WIN32_FIND_DATAA found;
    HANDLE find;

    snprintf(pattern, sizeof(pattern), "%s\\*", folder);
    find = FindFirstFileA(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const char *name = found.cFileName;
#else
    DIR *dir = opendir(folder);
    struct dirent *entry;

    if (dir != NULL) {
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
#endif
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
                continue;
            }
            if (n == capacity) {
                char **grown;

                capacity = capacity ? capacity * 2 : 16;
                grown = realloc(names, (size_t)capacity * sizeof(char *));
                if (grown == NULL) {
                    break;
                }
                names = grown;
            }
            names[n] = malloc(strlen(name) + 1);
            if (names[n] != NULL) {
                strcpy(names[n++], name);
            }
#ifdef _WIN32
        } while (FindNextFileA(find, &found));
        FindClose(find);
    }
#else
        }
        closedir(dir);
    }
#endif
    if (n > 1) {
        qsort(names, (size_t)n, sizeof(char *), compare_names);
    }
    *count = n;
    return names;
}

/* files/: every file, by its path under files/ as an archive member name */
static void apply_files(const char *folder, const char *member_prefix, int depth) { /* folder: under 2 KB */
    int count, i;
    char **names = list_folder(folder, &count);

    for (i = 0; i < count; i++) {
        char path[2048], member[600];

        snprintf(path, sizeof(path), "%s/%s", folder, names[i]);
        snprintf(member, sizeof(member), "%s%s", member_prefix, names[i]);
        if (is_folder(path)) {
            if (depth < 4) {
                strncat(member, "/", sizeof(member) - strlen(member) - 1);
                apply_files(path, member, depth + 1);
            }
        } else {
            size_t size = 0;
            unsigned char *data = read_whole(path, &size);

            if (data == NULL || size == 0) {
                fprintf(stderr, "[mods]   cannot read %s\n", path);
            } else if (port_disc_replace_member(member, data, (uint32_t)size)) {
                fprintf(stderr, "[mods]   %s (%zu bytes)\n", member, size);
            } else {
                fprintf(stderr, "[mods]   %s is not a file of KF4.DAT; left out\n", member);
            }
            free(data);
        }
        free(names[i]);
    }
    free(names);
}

/* text.json's "messages", "items" or "spells" into table (later mods win) */
static int take_texts(const cJSON *object, char **table, int count) {
    const cJSON *item;
    int taken = 0;

    cJSON_ArrayForEach(item, object) {
        char *end = NULL;
        long id = item->string ? strtol(item->string, &end, 10) : -1;

        if (!cJSON_IsString(item) || end == NULL || *end != 0 || id < 0 || id >= count) {
            continue;
        }
        free(table[id]);
        table[id] = malloc(strlen(item->valuestring) + 1);
        if (table[id] != NULL) {
            strcpy(table[id], item->valuestring);
            taken++;
        }
    }
    return taken;
}

static void read_text(const char *path) {
    size_t size = 0;
    unsigned char *data = read_whole(path, &size);
    cJSON *root;

    if (data == NULL) {
        return;
    }
    root = cJSON_Parse((const char *)data);
    free(data);
    if (root == NULL) {
        fprintf(stderr, "[mods]   %s is not valid JSON; left out\n", path);
        return;
    }
    fprintf(stderr, "[mods]   text.json: %d messages, %d item names, %d spell names\n",
            take_texts(cJSON_GetObjectItemCaseSensitive(root, "messages"), messages, MESSAGE_TABLE_WORDS),
            take_texts(cJSON_GetObjectItemCaseSensitive(root, "items"), items, ITEM_COUNT),
            take_texts(cJSON_GetObjectItemCaseSensitive(root, "spells"), spells, SPELL_COUNT));
    text_found = 1;
    cJSON_Delete(root);
}

/* A member as the disc now holds it (a mod's files/ copy if one replaced it); NULL if missing */
static unsigned char *read_member(const char *name, uint32_t *size) {
    uint32_t lsn;
    unsigned char *data;

    if (!port_disc_find_member(name, &lsn, size) || *size == 0) {
        return NULL;
    }
    data = malloc(((size_t)*size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE * DISC_SECTOR_SIZE);
    if (data != NULL && !port_disc_read(lsn, (*size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE, data)) {
        free(data);
        data = NULL;
    }
    return data;
}

static int any(char *const *table, int count) {
    int i;

    for (i = 0; i < count; i++) {
        if (table[i] != NULL) {
            return 1;
        }
    }
    return 0;
}

static void apply_names(const char *member, size_t record, int words, char *const *names, int count) {
    uint32_t size;
    unsigned char *data;
    int cut = 0;

    if (!any(names, count) || (data = read_member(member, &size)) == NULL) {
        return;
    }
    mods_put_names(data, size, record, words, names, count, &cut);
    if (cut) {
        fprintf(stderr, "[mods] %d names in %s were cut to %d characters\n", cut, member, words - 1);
    }
    port_disc_replace_member(member, data, size);
    free(data);
}

static void apply_text(void) {
    uint32_t size;
    unsigned char *original, *built;
    size_t built_size = 0;
    int id, unknown = 0;

    if (any(messages, MESSAGE_TABLE_WORDS) && (original = read_member("font.txc", &size)) != NULL) {
        for (id = 0; id < MESSAGE_TABLE_WORDS; id++) {
            uint16_t scratch[4096];

            if (messages[id] != NULL) {
                mods_encode_text(messages[id], scratch, 4096, &unknown);
            }
        }
        if (unknown) {
            fprintf(stderr, "[mods] %d characters the US font lacks were shown as '?'\n", unknown);
        }
        built = mods_build_messages(original, size, messages, MESSAGE_TABLE_WORDS, &built_size);
        if (built == NULL) {
            fprintf(stderr, "[mods] font.txc is not a message table; the messages are left out\n");
        } else if (built_size > MESSAGE_TEXT_ROOM) {
            fprintf(stderr, "[mods] the messages take %zu bytes, more than the game's %u; left out\n", built_size,
                    (unsigned)MESSAGE_TEXT_ROOM);
        } else {
            port_disc_replace_member("font.txc", built, (uint32_t)built_size);
            fprintf(stderr, "[mods] font.txc rebuilt: %zu bytes (was %u)\n", built_size, (unsigned)size);
        }
        free(built);
        free(original);
    }
    apply_names("param/item_nam.prm", ITEM_RECORD, ITEM_NAME_WORDS, items, ITEM_COUNT);
    apply_names("param/magic.prm", SPELL_RECORD, SPELL_NAME_WORDS, spells, SPELL_COUNT);
}

static char *mod_folders[64]; /* the mods applied, in order (port_mods_find) */
static int mod_folder_count;

const char *port_mods_find(const char *file) {
    static char found[2200];
    int i;

    for (i = mod_folder_count - 1; i >= 0; i--) {
        FILE *f;

        snprintf(found, sizeof(found), "%s/%s", mod_folders[i], file);
        if ((f = fopen(found, "rb")) != NULL) {
            fclose(f);
            return found;
        }
    }
    return NULL;
}

const char *port_mods_find_with(const char *file, const char *member) {
    static char found[2200];
    int i;

    for (i = mod_folder_count - 1; i >= 0; i--) {
        FILE *f;

        snprintf(found, sizeof(found), "%s/files/%s", mod_folders[i], member);
        if ((f = fopen(found, "rb")) != NULL) { /* (this mod's picture is the one in use) */
            fclose(f);
            snprintf(found, sizeof(found), "%s/%s", mod_folders[i], file);
            if ((f = fopen(found, "rb")) != NULL) {
                fclose(f);
                return found;
            }
            return NULL;
        }
    }
    return port_mods_find(file);
}

int port_mods_active(const char *name) {
    int i;

    for (i = 0; i < mod_folder_count; i++) {
        const char *base = mod_folders[i], *p;

        for (p = mod_folders[i]; *p; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        if (strcmp(base, name) == 0) {
            return 1;
        }
    }
    return 0;
}

void port_mods_open(const char *folder) {
    char path[1024];
    char **names;
    int count, i;

    if (folder != NULL && strcmp(folder, "none") == 0) {
        return;
    }
    if (folder == NULL) {
        snprintf(path, sizeof(path), "%s/mods", port_data_dir());
    } else {
        snprintf(path, sizeof(path), "%s", folder);
    }
    if (!is_folder(path)) {
        return;
    }
    names = list_folder(path, &count);
    for (i = 0; i < count; i++) {
        char mod[2048], part[2100];

        snprintf(mod, sizeof(mod), "%s/%s", path, names[i]);
        if (names[i][0] != '.' && names[i][0] != '_' && is_folder(mod)) {
            fprintf(stderr, "[mods] %s\n", names[i]);
            if (mod_folder_count < 64) {
                mod_folders[mod_folder_count++] = strdup(mod);
            }
            snprintf(part, sizeof(part), "%s/files", mod);
            if (is_folder(part)) {
                apply_files(part, "", 0);
            }
            snprintf(part, sizeof(part), "%s/text.json", mod);
            read_text(part);
        }
        free(names[i]);
    }
    free(names);
    if (text_found) {
        apply_text();
    }
}
#endif
