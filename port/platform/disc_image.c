/* A disc image read by sector (port/include/port/disc_image.h). Each format is a row of `formats`: the first
   that claims the file reads it. Everything read from the image is checked: the image is the player's file. */
#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif
#include "port/disc_image.h"

#include <stdlib.h>
#include <string.h>

#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

#define NO_HUNK UINT32_MAX

struct DiscImage {
    const struct Format *format;
    FILE *file;
    char description[48];
    /* a CHD's */
    chd_file *chd;
    uint8_t *hunk;          /* the hunk read last */
    uint32_t hunk_index;    /* its number, or NO_HUNK */
    uint32_t hunk_bytes;
    uint32_t unit_bytes;    /* a sector's bytes in a hunk: 2048 (DVD) or a CD frame (2448) */
    uint32_t data_offset;   /* where in its unit a sector's 2048 bytes start (16 in a raw Mode 1 frame) */
    uint32_t first_unit;    /* the data track's first sector, after a pregap kept in the image */
    uint32_t sectors;       /* the data track's sectors */
};

typedef struct Format {
    int (*claims)(FILE *file);
    DiscImageResult (*open)(DiscImage *image);
    int (*read)(DiscImage *image, uint32_t lsn, uint32_t sectors, unsigned char *out);
    void (*close)(DiscImage *image);
} Format;

static int seek(FILE *file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, SEEK_SET);
#else
    return fseeko(file, (off_t)offset, SEEK_SET);
#endif
}

/* ---- An ISO image ------------------------------------------------------------------------------ */
static int iso_claims(FILE *file) {
    (void)file;
    return 1; /* anything not claimed before it */
}

static DiscImageResult iso_open(DiscImage *image) {
    snprintf(image->description, sizeof(image->description), "ISO image");
    return DISC_IMAGE_OK;
}

static int iso_read(DiscImage *image, uint32_t lsn, uint32_t sectors, unsigned char *out) {
    size_t bytes = (size_t)sectors * DISC_IMAGE_SECTOR_SIZE;
    size_t got = 0;

    if (seek(image->file, (uint64_t)lsn * DISC_IMAGE_SECTOR_SIZE) == 0) {
        got = fread(out, 1, bytes, image->file);
    }
    if (got < bytes) {
        memset(out + got, 0, bytes - got);
    }
    return got > 0;
}

static void iso_close(DiscImage *image) {
    (void)image;
}

/* ---- A CHD ------------------------------------------------------------------------------------- */
static int chd_claims(FILE *file) {
    char magic[8];

    return seek(file, 0) == 0 && fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
           memcmp(magic, "MComprHD", sizeof(magic)) == 0;
}

/* where a CD track type's 2048 bytes are in its frame; -1 for a track with none (audio, Mode 2 Form 2) */
typedef struct {
    const char *type;
    int data_offset;
} CdTrackType;

static const CdTrackType cd_track_types[] = {
    {"MODE1", 0},       {"MODE1_RAW", 16}, {"MODE2_FORM1", 0}, {"MODE2_RAW", 24},
    {"MODE2", 8},       {"MODE2_FORM_MIX", 8},
};

static int cd_data_offset(const char *type) {
    size_t i;

    for (i = 0; i < sizeof(cd_track_types) / sizeof(cd_track_types[0]); i++) {
        if (strcmp(type, cd_track_types[i].type) == 0) {
            return cd_track_types[i].data_offset;
        }
    }
    return -1;
}

/* a CD's first track, from its metadata (CHT2, or the older CHTR) */
static DiscImageResult chd_cd_track(DiscImage *image) {
    char text[256], type[32], subtype[32], pregap_type[32] = "-", pregap_sub[32];
    uint32_t length = 0;
    int track = 0, frames = 0, pregap = 0, postgap = 0, offset;

    memset(text, 0, sizeof(text));
    if (chd_get_metadata(image->chd, CDROM_TRACK_METADATA2_TAG, 0, text, sizeof(text) - 1, &length, NULL, NULL) ==
        CHDERR_NONE) {
        if (sscanf(text, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                   &track, type, subtype, &frames, &pregap, pregap_type, pregap_sub, &postgap) != 8) {
            return DISC_IMAGE_BROKEN;
        }
    } else if (chd_get_metadata(image->chd, CDROM_TRACK_METADATA_TAG, 0, text, sizeof(text) - 1, &length, NULL,
                                NULL) == CHDERR_NONE) {
        if (sscanf(text, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", &track, type, subtype, &frames) != 4) {
            return DISC_IMAGE_BROKEN;
        }
    } else {
        return DISC_IMAGE_NOT_A_DISC;
    }
    offset = cd_data_offset(type);
    if (offset < 0 || frames <= 0 || pregap < 0) {
        return DISC_IMAGE_NOT_A_DISC;
    }
    /* a pregap kept in the image ("V...") comes before the track's sector 0 */
    image->first_unit = pregap_type[0] == 'V' ? (uint32_t)pregap : 0;
    if ((uint32_t)frames <= image->first_unit) {
        return DISC_IMAGE_NOT_A_DISC;
    }
    image->sectors = (uint32_t)frames - image->first_unit;
    image->unit_bytes = image->unit_bytes ? image->unit_bytes : CD_FRAME_SIZE;
    image->data_offset = (uint32_t)offset;
    snprintf(image->description, sizeof(image->description), "CHD (CD, %s)", type);
    return DISC_IMAGE_OK;
}

static DiscImageResult chd_open_image(DiscImage *image) {
    const chd_header *header;
    uint32_t length = 0;
    char dvd[16];
    DiscImageResult result;

    if (chd_open_file(image->file, CHD_OPEN_READ, NULL, &image->chd) != CHDERR_NONE) {
        image->chd = NULL;
        return DISC_IMAGE_BROKEN;
    }
    header = chd_get_header(image->chd);
    image->hunk_bytes = header->hunkbytes;
    image->unit_bytes = header->unitbytes;
    image->hunk_index = NO_HUNK;
    if (chd_get_metadata(image->chd, DVD_METADATA_TAG, 0, dvd, sizeof(dvd), &length, NULL, NULL) == CHDERR_NONE) {
        image->unit_bytes = DISC_IMAGE_SECTOR_SIZE;
        image->sectors = (uint32_t)(header->logicalbytes / DISC_IMAGE_SECTOR_SIZE);
        snprintf(image->description, sizeof(image->description), "CHD (DVD)");
        result = DISC_IMAGE_OK;
    } else {
        result = chd_cd_track(image);
    }
    if (result != DISC_IMAGE_OK) {
        return result;
    }
    /* a unit never crosses a hunk, and a sector's 2048 bytes fit in its unit */
    if (image->hunk_bytes == 0 || image->unit_bytes < image->data_offset + DISC_IMAGE_SECTOR_SIZE ||
        image->hunk_bytes % image->unit_bytes != 0) {
        return DISC_IMAGE_BROKEN;
    }
    image->hunk = malloc(image->hunk_bytes);
    return image->hunk != NULL ? DISC_IMAGE_OK : DISC_IMAGE_NO_MEMORY;
}

/* one sector's 2048 bytes; 0 (and zeros) where the image cannot be read */
static int chd_read_sector(DiscImage *image, uint32_t lsn, unsigned char *out) {
    uint64_t byte = ((uint64_t)image->first_unit + lsn) * image->unit_bytes;
    uint64_t hunk = byte / image->hunk_bytes;

    if (lsn >= image->sectors || hunk >= NO_HUNK) {
        memset(out, 0, DISC_IMAGE_SECTOR_SIZE);
        return 0;
    }
    if (image->hunk_index != (uint32_t)hunk) {
        image->hunk_index = NO_HUNK;
        if (chd_read(image->chd, (uint32_t)hunk, image->hunk) != CHDERR_NONE) {
            memset(out, 0, DISC_IMAGE_SECTOR_SIZE);
            return 0;
        }
        image->hunk_index = (uint32_t)hunk;
    }
    memcpy(out, image->hunk + byte % image->hunk_bytes + image->data_offset, DISC_IMAGE_SECTOR_SIZE);
    return 1;
}

static int chd_read_image(DiscImage *image, uint32_t lsn, uint32_t sectors, unsigned char *out) {
    int any = 0;
    uint32_t i;

    for (i = 0; i < sectors; i++) {
        any |= chd_read_sector(image, lsn + i, out + (size_t)i * DISC_IMAGE_SECTOR_SIZE);
    }
    return any;
}

static void chd_close_image(DiscImage *image) {
    if (image->chd != NULL) {
        chd_close(image->chd);
    }
    free(image->hunk);
}

/* ---- The formats, the first that claims a file reads it ------------------------------------------ */
static const Format formats[] = {
    {chd_claims, chd_open_image, chd_read_image, chd_close_image},
    {iso_claims, iso_open, iso_read, iso_close},
};

DiscImageResult disc_image_open(FILE *file, DiscImage **out) {
    DiscImage *image = calloc(1, sizeof(DiscImage));
    DiscImageResult result;
    size_t i;

    *out = NULL;
    if (image == NULL) {
        fclose(file);
        return DISC_IMAGE_NO_MEMORY;
    }
    image->file = file;
    for (i = 0; image->format == NULL; i++) {
        if (formats[i].claims(file)) {
            image->format = &formats[i];
        }
    }
    result = image->format->open(image);
    if (result != DISC_IMAGE_OK) {
        disc_image_close(image);
        return result;
    }
    *out = image;
    return DISC_IMAGE_OK;
}

const char *disc_image_result_text(DiscImageResult result) {
    switch (result) {
    case DISC_IMAGE_OK:
        return "the image can be read";
    case DISC_IMAGE_BROKEN:
        return "the CHD cannot be read: it is damaged, or it is a child of another CHD (make it again with "
               "chdman from the disc, or from an ISO image)";
    case DISC_IMAGE_NOT_A_DISC:
        return "the CHD holds no disc data (a hard disk's, a laserdisc's, or a CD with no data track)";
    case DISC_IMAGE_NO_MEMORY:
        return "out of memory";
    }
    return "unknown";
}

int disc_image_read(DiscImage *image, uint32_t lsn, uint32_t sectors, void *buffer) {
    return image->format->read(image, lsn, sectors, buffer);
}

const char *disc_image_format(const DiscImage *image) {
    return image->description;
}

void disc_image_close(DiscImage *image) {
    if (image == NULL) {
        return;
    }
    image->format->close(image);
    fclose(image->file);
    free(image);
}
