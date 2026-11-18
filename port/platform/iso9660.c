/* An ISO 9660 image's files (port/include/port/iso9660.h). Everything read from the image is checked
   against the buffer it is in: the image is the player's file. */
#include "port/iso9660.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 8
#define MAX_DIRECTORY_BYTES (1u << 20) /* KF4's largest is a few sectors */

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    DiscImage *image;
    Iso9660Visit visit;
    void *context;
} Walk;

/* the record's name, without the version (";1"); 0 for the "." and ".." records */
static int record_name(const unsigned char *record, unsigned length, char *name, size_t name_size) {
    unsigned name_length = record[32];
    unsigned i;
    char *version;

    if (33u + name_length > length) {
        return -1;
    }
    if (name_length == 1 && (record[33] == 0 || record[33] == 1)) {
        return 0;
    }
    for (i = 0; i < name_length && i < name_size - 1; i++) {
        name[i] = (char)record[33 + i];
    }
    name[i] = 0;
    version = strchr(name, ';');
    if (version != NULL) {
        *version = 0;
    }
    return 1;
}

static Iso9660Result walk_directory(const Walk *walk, const char *dir_path, uint32_t lsn, uint32_t size, int depth) {
    unsigned char *data;
    uint32_t sectors = (size + ISO9660_SECTOR_SIZE - 1) / ISO9660_SECTOR_SIZE;
    uint32_t offset = 0;
    Iso9660Result result = ISO9660_OK;

    if (depth > MAX_DEPTH || sectors == 0) {
        return ISO9660_OK;
    }
    if (size > MAX_DIRECTORY_BYTES) {
        return ISO9660_BAD_DIRECTORY;
    }
    data = malloc((size_t)sectors * ISO9660_SECTOR_SIZE);
    if (data == NULL) {
        return ISO9660_BAD_DIRECTORY;
    }
    if (!disc_image_read(walk->image, lsn, sectors, data)) {
        free(data);
        return ISO9660_BAD_DIRECTORY;
    }
    while (offset < size && result == ISO9660_OK) {
        const unsigned char *record = data + offset;
        unsigned length = record[0];
        uint32_t room = size - offset;
        char name[256];
        char path[512];
        int named;

        if (length == 0) {
            /* records do not cross a sector: the rest of this one is empty */
            offset = (offset / ISO9660_SECTOR_SIZE + 1) * ISO9660_SECTOR_SIZE;
            continue;
        }
        if (length < 34 || length > room) {
            result = ISO9660_BAD_DIRECTORY;
            break;
        }
        named = record_name(record, length, name, sizeof(name));
        if (named < 0) {
            result = ISO9660_BAD_DIRECTORY;
            break;
        }
        if (named > 0) {
            snprintf(path, sizeof(path), "%s\\%s", dir_path, name);
            if (record[25] & 2) {
                result = walk_directory(walk, path, le32(record + 2), le32(record + 10), depth + 1);
            } else {
                walk->visit(walk->context, path, le32(record + 2), le32(record + 10));
            }
        }
        offset += length;
    }
    free(data);
    return result;
}

Iso9660Result iso9660_list(DiscImage *image, uint32_t *disc_sectors, Iso9660Visit visit, void *context) {
    unsigned char pvd[ISO9660_SECTOR_SIZE];
    const unsigned char *root = pvd + 156;
    Walk walk = {image, visit, context};

    if (!disc_image_read(image, 16, 1, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        return ISO9660_NOT_ISO;
    }
    if (disc_sectors != NULL) {
        *disc_sectors = le32(pvd + 80);
    }
    return walk_directory(&walk, "", le32(root + 2), le32(root + 10), 0);
}
