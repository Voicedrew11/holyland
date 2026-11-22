/* Mods (port/platform/mods.c, port/include/port/mods.h) with the disc (port/platform/disc.c): the text code, the
   rebuilt message table and name records, and a small extracted disc whose KF4.DAT members a mods folder replaces,
   read back as the game reads them (core/file.c: KF4.DAT's table, then a member's sectors from its offset). */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#include <unistd.h>
#define make_dir(path) mkdir(path, 0755)
#endif

#include "port/host.h"
#include "port/mods.h"
#include "port/platform.h"
#include "port/zones.h"

static int failures;

#define CHECK(cond)                                                                                               \
    do {                                                                                                          \
        if (!(cond)) {                                                                                            \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond);                                    \
            failures++;                                                                                           \
        }                                                                                                         \
    } while (0)

/* ---- What disc.c and mods.c take from the rest of the port ---- */
static char data_dir[512];

void port_fatal(const char *file, int line, const char *fmt, ...) {
    va_list args;

    fprintf(stderr, "%s:%d: ", file, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}
double port_wall_seconds(void) {
    return 0.0;
}
ZoneId port_zone_id(const char *name, ZoneKind kind) {
    (void)name, (void)kind;
    return 1;
}
int port_zone_begin(ZoneId id) {
    (void)id;
    return 0;
}
void port_zone_end(ZoneId id) {
    (void)id;
}
const char *port_data_dir(void) {
    return data_dir;
}

/* ---- Helpers ---- */
static void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8);
}
static void put32(unsigned char *p, uint32_t v) {
    put16(p, v & 0xFFFF), put16(p + 2, v >> 16);
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned get16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static void write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fwrite(data, 1, size, f);
    fclose(f);
}

/* A message table with "AB" at id 0, nothing at 1, "C\n" at 2 */
static unsigned char *small_messages(size_t *size) {
    unsigned char *t = calloc(1, 0x8000 + 64);
    size_t at = 0x8000;

    put32(t, 0x2000);
    put32(t + 4, (uint32_t)at);
    put16(t + at, 0x0821), put16(t + at + 2, 0x0822), put16(t + at + 4, 0xFFFF), at += 6;
    put32(t + 12, (uint32_t)at);
    put16(t + at, 0x0823), put16(t + at + 2, 0x8000), put16(t + at + 4, 0xFFFF), at += 6;
    *size = at;
    return t;
}

/* The game's own lookup: Message_GetText reads word id + 1 */
static int message_codes(const unsigned char *table, int id, unsigned *codes, int max) {
    uint32_t at = get32(table + (id + 1) * 4);
    int n = 0;

    if (at == 0) {
        return -1;
    }
    while (n < max && get16(table + at + n * 2) != 0xFFFF) {
        codes[n] = get16(table + at + n * 2);
        n++;
    }
    return n;
}

static void test_encode(void) {
    uint16_t codes[32] = { 0 };
    int unknown = 0;
    int n = mods_encode_text("Hi!\n{num3} gold\xE2\x80\xA6\xE6\x97\xA5", codes, 32, &unknown);

    CHECK(n == 14);
    CHECK(codes[0] == 0x0828 && codes[1] == 0x0849 && codes[2] == 0x0801); /* H i ! */
    CHECK(codes[3] == 0x8000);
    CHECK(codes[4] == 0x2300);
    CHECK(codes[5] == 0x0800); /* the space */
    CHECK(codes[10] == 0x080E && codes[11] == 0x080E && codes[12] == 0x080E); /* the ellipsis as three dots */
    CHECK(codes[13] == 0x081F && unknown == 1);                                /* a kanji the font lacks: '?' */
        CHECK(mods_encode_text("It's", codes, 32, NULL) == 4 && codes[2] == 0x0807);
    CHECK(mods_encode_text("abcdef", codes, 3, NULL) == 3); /* stops at max */
}

static void test_messages(void) {
    size_t size, out_size = 0;
    unsigned char *original = small_messages(&size), *built;
    char *texts[3] = { NULL, "New", "Two\nlines" };
    unsigned codes[16];

    built = mods_build_messages(original, size, texts, 3, &out_size);
    CHECK(built != NULL);
    if (built == NULL) {
        return;
    }
    CHECK(get32(built) == 0x2000);
    CHECK(message_codes(built, 0, codes, 16) == 2 && codes[0] == 0x0821 && codes[1] == 0x0822); /* kept */
    CHECK(message_codes(built, 1, codes, 16) == 3 && codes[0] == 0x082E);                       /* added */
    CHECK(message_codes(built, 2, codes, 16) == 9 && codes[3] == 0x8000);                       /* replaced */
    CHECK(message_codes(built, 3, codes, 16) == -1);                                            /* unused */
    CHECK(out_size == 0x8000 + 6 + 8 + 20);
    free(built);
    original[1] = 0;
    CHECK(mods_build_messages(original, size, texts, 3, &out_size) == NULL); /* not a message table */
    free(original);
}

static void test_names(void) {
    unsigned char records[3 * 0x40];
    char *names[3] = { "Short", NULL, "A name far too long for the field" };
    int cut = 0, i;

    memset(records, 0x5A, sizeof(records));
    mods_put_names(records, sizeof(records), 0x40, 23, names, 3, &cut);
    CHECK(get16(records) == 0x0833 && get16(records + 8) == 0x0854 && get16(records + 10) == 0xFFFF);
    CHECK(get16(records + 44) == 0xFFFF && records[46] == 0x5A); /* the field ends at word 23; the numbers stay */
    CHECK(records[0x40] == 0x5A && records[0x40 + 63] == 0x5A); /* untouched */
    CHECK(cut == 1);
    for (i = 0; i < 22; i++) {
        CHECK(get16(records + 0x80 + i * 2) != 0xFFFF);
    }
    CHECK(get16(records + 0x80 + 44) == 0xFFFF); /* 22 characters and the end mark */
}

/* ---- A disc with KF4.DAT, and mods ---- */
#define MEMBERS 4
static const char *member_names[MEMBERS] = { "param/item_nam.prm", "font.txc", "param/magic.prm", "msg/m0001.tm2" };

static void make_disc(const char *root) {
    char path[1200];
    unsigned char *dat;
    size_t dat_size, at = 0x20000, msg_size;
    unsigned char *msgs = small_messages(&msg_size);
    int i;

    make_dir(root);
    snprintf(path, sizeof(path), "%s/SYSTEM.CNF", root);
    write_file(path, "BOOT2 = cdrom0:\\SLUS_203.18;1\n", 30);
    snprintf(path, sizeof(path), "%s/DATA", root);
    make_dir(path);
    dat_size = 0x20000 + 4 * 0x8000;
    dat = calloc(1, dat_size);
    put32(dat + 4, MEMBERS);
    for (i = 0; i < MEMBERS; i++) {
        unsigned char *row = dat + 0x40 + i * 0x40;
        size_t size = i == 1 ? msg_size : 0x1000;

        strcpy((char *)row, member_names[i]);
        put32(row + 0x34, (uint32_t)size);
        put32(row + 0x38, 0x8000);
        put32(row + 0x3C, (uint32_t)at);
        if (i == 1) {
            memcpy(dat + at, msgs, msg_size);
        } else {
            memset(dat + at, 0x40 + i, size);
        }
        at += 0x8000;
    }
    snprintf(path, sizeof(path), "%s/DATA/KF4.DAT", root);
    write_file(path, dat, dat_size);
    free(dat);
    free(msgs);
}

static void make_mods(const char *root) {
    char path[1200];
    const char *text = "{ \"messages\": { \"0\": \"Hello\", \"2\": \"Bye\" },\n"
                       "  \"items\": { \"1\": \"Knife\" }, \"spells\": { \"0\": \"Fireball\" } }";
    unsigned char picture[3000];

    make_dir(root);
    snprintf(path, sizeof(path), "%s/a-text", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/a-text/text.json", root);
    write_file(path, text, strlen(text));
    snprintf(path, sizeof(path), "%s/b-files", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/b-files/files", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/b-files/files/msg", root);
    make_dir(path);
    memset(picture, 0x77, sizeof(picture));
    snprintf(path, sizeof(path), "%s/b-files/files/msg/m0001.tm2", root);
    write_file(path, picture, sizeof(picture));
    snprintf(path, sizeof(path), "%s/b-files/files/nonsense.bin", root);
    write_file(path, picture, 10);
    /* finer sign pictures: b-files's goes with its own m0001.tm2; c-signs's m0003 with the disc's picture */
    snprintf(path, sizeof(path), "%s/b-files/signs", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/b-files/signs/m0001.png", root);
    write_file(path, "b", 1);
    snprintf(path, sizeof(path), "%s/c-signs", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/c-signs/signs", root);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/c-signs/signs/m0001.png", root);
    write_file(path, "c", 1);
    snprintf(path, sizeof(path), "%s/c-signs/signs/m0003.png", root);
    write_file(path, "c", 1);
    snprintf(path, sizeof(path), "%s/_off", root); /* skipped */
    make_dir(path);
    snprintf(path, sizeof(path), "%s/_off/text.json", root);
    write_file(path, "{ \"messages\": { \"0\": \"Off\" } }", 30);
}

/* core/file.c: KF4.DAT's table read whole, then a member found by name and read from its offset */
static unsigned char *game_reads(const char *name, uint32_t *size) {
    uint32_t dat_lsn, dat_size, count, i;
    unsigned char *toc = malloc(0x20000), *data;

    if (!port_disc_find("\\DATA\\KF4.DAT;1", &dat_lsn, &dat_size) || !port_disc_read(dat_lsn, 64, toc)) {
        free(toc);
        return NULL;
    }
    count = get32(toc + 4);
    for (i = 0; i < count; i++) {
        const unsigned char *row = toc + 0x40 + i * 0x40;

        if (strcmp((const char *)row, name) == 0) {
            uint32_t lsn = dat_lsn + (get32(row + 0x3C) >> 11);

            *size = get32(row + 0x34);
            data = malloc((*size + 2047) / 2048 * 2048);
            if (!port_disc_read(lsn, (*size + 2047) / 2048, data)) {
                free(data);
                data = NULL;
            }
            free(toc);
            return data;
        }
    }
    free(toc);
    return NULL;
}

static void test_disc(const char *base) {
    char disc[600], mods[600];
    uint32_t size = 0, lsn = 0, found_size = 0;
    unsigned char *data;
    unsigned codes[16];

    snprintf(disc, sizeof(disc), "%s/disc", base);
    snprintf(mods, sizeof(mods), "%s/mods", base);
    make_disc(disc);
    make_mods(mods);
    port_disc_open(disc);
    port_mods_open(mods);

    data = game_reads("font.txc", &size);
    CHECK(data != NULL);
    if (data != NULL) {
        CHECK(message_codes(data, 0, codes, 16) == 5 && codes[0] == 0x0828); /* "Hello", not the skipped "Off" */
        CHECK(message_codes(data, 2, codes, 16) == 3 && codes[0] == 0x0822); /* "Bye" */
        free(data);
    }
    data = game_reads("param/item_nam.prm", &size);
    CHECK(data != NULL && size == 0x1000);
    if (data != NULL) {
        CHECK(data[0] == 0x40 && get16(data + 0x40) == 0x082B && get16(data + 0x4A) == 0xFFFF); /* item 1: Knife */
        CHECK(data[0x40 + 46] == 0x40);
        free(data);
    }
    data = game_reads("param/magic.prm", &size);
    CHECK(data != NULL && get16(data) == 0x0826 && get16(data + 16) == 0xFFFF && data[48] == 0x42);
    free(data);
    data = game_reads("msg/m0001.tm2", &size);
    CHECK(data != NULL && size == 3000 && data[0] == 0x77 && data[2999] == 0x77 && data[3000] == 0);
    free(data);
    /* the member listing follows, and the disc's own files still read */
    CHECK(port_disc_find_member("msg/m0001.tm2", &lsn, &found_size) && found_size == 3000);
    CHECK(port_disc_find("\\SYSTEM.CNF", &lsn, &found_size) && found_size == 30);
    /* replacing again keeps one row */
    CHECK(port_disc_replace_member("MSG\\M0001.TM2", "xyz", 3));
    data = game_reads("msg/m0001.tm2", &size);
    CHECK(data != NULL && size == 3 && data[0] == 'x');
    free(data);
    CHECK(!port_disc_replace_member("no/such.file", "x", 1));
    /* a finer picture goes with the picture in use, whatever the mods' order */
    {
        const char *path = port_mods_find_with("signs/m0001.png", "msg/m0001.tm2");

        CHECK(path != NULL && strstr(path, "b-files") != NULL);
        path = port_mods_find_with("signs/m0003.png", "msg/m0003.tm2");
        CHECK(path != NULL && strstr(path, "c-signs") != NULL);
        CHECK(port_mods_find_with("signs/m0009.png", "msg/m0009.tm2") == NULL);
    }
}

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "mods_test_tmp";

    make_dir(base);
    snprintf(data_dir, sizeof(data_dir), "%s", base);
    test_encode();
    test_messages();
    test_names();
    test_disc(base);
    if (failures) {
        fprintf(stderr, "mods_test: %d failed\n", failures);
        return 1;
    }
    printf("mods_test: ok\n");
    return 0;
}
