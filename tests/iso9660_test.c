/* port/platform/iso9660.c and disc_image.c: an image made here (SYSTEM.CNF, DATA\KF4.DAT) listed as the disc
   reader and the launcher's disc check list it, broken images refused without reading past what was read, and
   the same image as CHDs (tests/data/, made from `iso9660_test write disc.iso` with chdman, below).

     iso9660_test
     iso9660_test write PATH   writes the image made here, for the CHDs:
       chdman createdvd -i disc.iso -o tests/data/disc_dvd.chd
       chdman createcd -i disc_mode1.cue -o tests/data/disc_cd_mode1.chd   (TRACK 01 MODE1/2048, the .iso)
       chdman createcd -i disc_mode2.cue -o tests/data/disc_cd_mode2.chd   (TRACK 01 MODE2/2352: a .bin of
         Mode 2 Form 1 frames, the .iso's sectors at byte 24 of each, written by tests/data/make_mode2_bin.py) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/disc_image.h"
#include "port/iso9660.h"

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

#define SECTORS 24
static unsigned char disc[SECTORS * ISO9660_SECTOR_SIZE];

static void put32(unsigned char *p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

/* a directory record at `at`; returns its length */
static unsigned record(unsigned char *at, const char *name, unsigned name_length, unsigned lsn, unsigned size, int is_dir) {
    unsigned length = 33 + name_length + ((33 + name_length) & 1);

    memset(at, 0, length);
    at[0] = (unsigned char)length;
    put32(at + 2, lsn);
    put32(at + 10, size);
    at[25] = is_dir ? 2 : 0;
    at[32] = (unsigned char)name_length;
    memcpy(at + 33, name, name_length);
    return length;
}

static void make_disc(void) {
    unsigned char *pvd = disc + 16 * ISO9660_SECTOR_SIZE;
    unsigned char *root = disc + 18 * ISO9660_SECTOR_SIZE;
    unsigned char *data = disc + 19 * ISO9660_SECTOR_SIZE;
    unsigned at = 0;

    memset(disc, 0, sizeof(disc));
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    put32(pvd + 80, SECTORS);
    record(pvd + 156, "\0", 1, 18, ISO9660_SECTOR_SIZE, 1);
    at += record(root + at, "\0", 1, 18, ISO9660_SECTOR_SIZE, 1);
    at += record(root + at, "\1", 1, 18, ISO9660_SECTOR_SIZE, 1);
    at += record(root + at, "DATA", 4, 19, ISO9660_SECTOR_SIZE, 1);
    at += record(root + at, "SYSTEM.CNF;1", 12, 20, 5, 0);
    at = 0;
    at += record(data + at, "\0", 1, 19, ISO9660_SECTOR_SIZE, 1);
    at += record(data + at, "\1", 1, 18, ISO9660_SECTOR_SIZE, 1);
    at += record(data + at, "KF4.DAT;1", 9, 21, 3000, 0);
    memcpy(disc + 20 * ISO9660_SECTOR_SIZE, "BOOT2", 5);
}

/* the bytes as an open disc image, or NULL */
static DiscImage *image_of(const unsigned char *bytes, size_t size) {
    FILE *f = tmpfile();
    DiscImage *image = NULL;

    if (f == NULL) {
        return NULL;
    }
    if (fwrite(bytes, 1, size, f) != size) {
        fclose(f);
        return NULL;
    }
    return disc_image_open(f, &image) == DISC_IMAGE_OK ? image : NULL;
}

typedef struct {
    char seen[8][64];
    unsigned lsn[8], size[8];
    int count;
} Listing;

static void visit(void *context, const char *path, uint32_t lsn, uint32_t size) {
    Listing *l = context;

    if (l->count < 8) {
        snprintf(l->seen[l->count], sizeof(l->seen[0]), "%s", path);
        l->lsn[l->count] = lsn;
        l->size[l->count] = size;
        l->count++;
    }
}

static Iso9660Result list_image(DiscImage *image, Listing *l, uint32_t *sectors) {
    memset(l, 0, sizeof(*l));
    return image != NULL ? iso9660_list(image, sectors, visit, l) : ISO9660_NOT_ISO;
}

static Iso9660Result list(const unsigned char *bytes, size_t size, Listing *l, uint32_t *sectors) {
    DiscImage *image = image_of(bytes, size);
    Iso9660Result r = list_image(image, l, sectors);

    disc_image_close(image);
    return r;
}

/* the listing of make_disc's image, read from `image` */
static void check_listing(const char *what, DiscImage *image) {
    Listing l;
    uint32_t sectors = 0;
    unsigned char buffer[ISO9660_SECTOR_SIZE];
    char name[96];

    snprintf(name, sizeof(name), "%s: lists", what);
    check(name, list_image(image, &l, &sectors) == ISO9660_OK);
    snprintf(name, sizeof(name), "%s: the disc's size", what);
    check(name, sectors == SECTORS);
    snprintf(name, sizeof(name), "%s: the files", what);
    check(name, l.count == 2 && strcmp(l.seen[0], "\\DATA\\KF4.DAT") == 0 && l.lsn[0] == 21 && l.size[0] == 3000 &&
                    strcmp(l.seen[1], "\\SYSTEM.CNF") == 0 && l.lsn[1] == 20 && l.size[1] == 5);
    snprintf(name, sizeof(name), "%s: reads a sector", what);
    check(name, image != NULL && disc_image_read(image, 20, 1, buffer) && memcmp(buffer, "BOOT2", 5) == 0);
    memset(buffer, 0xAA, sizeof(buffer));
    snprintf(name, sizeof(name), "%s: past the end: zeros, nothing read", what);
    check(name, image != NULL && !disc_image_read(image, 999, 1, buffer) && buffer[0] == 0 && buffer[2047] == 0);
}

static void test_good(void) {
    DiscImage *image;

    make_disc();
    image = image_of(disc, sizeof(disc));
    check("an ISO image", image != NULL && strcmp(disc_image_format(image), "ISO image") == 0);
    check_listing("ISO", image);
    disc_image_close(image);
}

static DiscImage *open_data(const char *name) {
    char path[1024];
    FILE *f;
    DiscImage *image = NULL;

    snprintf(path, sizeof(path), "%s/%s", TEST_DATA_DIR, name);
    f = fopen(path, "rb");
    if (f == NULL || disc_image_open(f, &image) != DISC_IMAGE_OK) {
        printf("     (%s: cannot open)\n", path);
        return NULL;
    }
    return image;
}

static void test_chd(void) {
    static const struct {
        const char *file, *format;
    } chds[] = {
        {"disc_dvd.chd", "CHD (DVD)"},
        {"disc_cd_mode1.chd", "CHD (CD, MODE1)"},
        {"disc_cd_mode2.chd", "CHD (CD, MODE2_RAW)"},
    };
    unsigned char bytes[4096];
    size_t i, n = 0;
    FILE *f;
    DiscImage *image;

    make_disc();
    for (i = 0; i < sizeof(chds) / sizeof(chds[0]); i++) {
        image = open_data(chds[i].file);
        check(chds[i].format, image != NULL && strcmp(disc_image_format(image), chds[i].format) == 0);
        check_listing(chds[i].file, image);
        disc_image_close(image);
    }

    /* the DVD's CHD cut short: its header is there, its hunk map is not */
    {
        char path[1024];

        snprintf(path, sizeof(path), "%s/disc_dvd.chd", TEST_DATA_DIR);
        f = fopen(path, "rb");
        if (f != NULL) {
            n = fread(bytes, 1, 200, f);
            fclose(f);
        }
    }
    f = tmpfile();
    image = (void *)1;
    check("a CHD cut short is refused", f != NULL && fwrite(bytes, 1, n, f) == n && n == 200 &&
                                             disc_image_open(f, &image) == DISC_IMAGE_BROKEN && image == NULL);

    /* the magic, then nothing a CHD's header holds */
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, "MComprHD", 8);
    f = tmpfile();
    image = (void *)1;
    check("a CHD's magic alone is refused", f != NULL && fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes) &&
                                                 disc_image_open(f, &image) == DISC_IMAGE_BROKEN && image == NULL);
}

static int write_disc(const char *path) {
    FILE *f = fopen(path, "wb");
    int ok;

    make_disc();
    ok = f != NULL && fwrite(disc, 1, sizeof(disc), f) == sizeof(disc);
    if (f != NULL) {
        ok = fclose(f) == 0 && ok;
    }
    return ok ? 0 : 1;
}

static void test_broken(void) {
    Listing l;
    unsigned char *root = disc + 18 * ISO9660_SECTOR_SIZE;

    make_disc();
    disc[16 * ISO9660_SECTOR_SIZE + 1] = 'X';
    check("not ISO 9660", list(disc, sizeof(disc), &l, NULL) == ISO9660_NOT_ISO);
    check("too short a file", list(disc, 100, &l, NULL) == ISO9660_NOT_ISO);

    make_disc();
    root[34 * 2] = 200; /* the DATA record says it runs past the directory's end (168 bytes) */
    put32(disc + 16 * ISO9660_SECTOR_SIZE + 156 + 10, 34 * 2 + 100);
    check("a record past the directory's end", list(disc, sizeof(disc), &l, NULL) == ISO9660_BAD_DIRECTORY);

    make_disc();
    root[34 * 2 + 32] = 250; /* the DATA record's name runs past the record */
    check("a name past its record", list(disc, sizeof(disc), &l, NULL) == ISO9660_BAD_DIRECTORY);

    make_disc();
    root[34 * 2] = 20; /* shorter than a record */
    check("a record too short", list(disc, sizeof(disc), &l, NULL) == ISO9660_BAD_DIRECTORY);

    make_disc();
    put32(disc + 16 * ISO9660_SECTOR_SIZE + 156 + 10, 0x7FFFFFFF);
    check("a huge directory is refused, not allocated", list(disc, sizeof(disc), &l, NULL) == ISO9660_BAD_DIRECTORY);

    make_disc();
    put32(root + 34 * 2 + 2, 18); /* DATA is the root again: a loop, stopped by depth */
    check("a directory loop ends", list(disc, sizeof(disc), &l, NULL) == ISO9660_OK);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "write") == 0) {
        return write_disc(argv[2]);
    }
    test_good();
    test_broken();
    test_chd();
    printf("%d/%d passed\n", checked - failed, checked);
    return failed != 0;
}
