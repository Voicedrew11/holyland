/* port/platform/iso9660.c: an image made here (SYSTEM.CNF, DATA\KF4.DAT) listed as the disc reader and the
   launcher's disc check list it, and broken images refused without reading past what was read.

     iso9660_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static FILE *image_of(const unsigned char *bytes, size_t size) {
    FILE *f = tmpfile();

    if (f != NULL && fwrite(bytes, 1, size, f) != size) {
        fclose(f);
        return NULL;
    }
    return f;
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

static Iso9660Result list(const unsigned char *bytes, size_t size, Listing *l, uint32_t *sectors) {
    FILE *f = image_of(bytes, size);
    Iso9660Result r;

    memset(l, 0, sizeof(*l));
    if (f == NULL) {
        return ISO9660_NOT_ISO;
    }
    r = iso9660_list(f, sectors, visit, l);
    fclose(f);
    return r;
}

static void test_good(void) {
    Listing l;
    uint32_t sectors = 0;
    unsigned char buffer[ISO9660_SECTOR_SIZE];
    FILE *f;

    make_disc();
    check("lists", list(disc, sizeof(disc), &l, &sectors) == ISO9660_OK);
    check("the disc's size", sectors == SECTORS);
    check("two files", l.count == 2);
    check("into folders, no version", strcmp(l.seen[0], "\\DATA\\KF4.DAT") == 0 && l.lsn[0] == 21 && l.size[0] == 3000);
    check("root file", strcmp(l.seen[1], "\\SYSTEM.CNF") == 0 && l.lsn[1] == 20 && l.size[1] == 5);
    f = image_of(disc, sizeof(disc));
    check("reads a sector", f != NULL && iso9660_read(f, 20, 1, buffer) && memcmp(buffer, "BOOT2", 5) == 0);
    check("past the end: zeros, nothing read", f != NULL && !iso9660_read(f, 999, 1, buffer) && buffer[0] == 0);
    if (f != NULL) {
        fclose(f);
    }
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

int main(void) {
    test_good();
    test_broken();
    printf("%d/%d passed\n", checked - failed, checked);
    return failed != 0;
}
