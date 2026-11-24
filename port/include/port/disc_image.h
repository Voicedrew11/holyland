/* A disc image, read by its 2048-byte sectors whatever the file holds: the game's disc reader
   (port/platform/disc.c) and the launcher's disc check (launcher/disc_check.cpp) read images through this,
   and list their files with port/iso9660.h. No other part of the port is used here.

   The formats (disc_image.c's table; a new format is a new row):
     - an ISO image (.iso): the sectors as they are;
     - a CHD (.chd, MAME's compressed hunks; third_party/libchdr): a DVD's (chdman createdvd), or a CD's
       first track (chdman createcd: raw or cooked, Mode 1 or Mode 2 Form 1).
   The format is told by the file's first bytes, not its name.

   One reader at a time: an image is not safe to read from two threads at once (disc.c locks). */
#ifndef PORT_DISC_IMAGE_H
#define PORT_DISC_IMAGE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISC_IMAGE_SECTOR_SIZE 2048

typedef struct DiscImage DiscImage;

typedef enum {
    DISC_IMAGE_OK,
    DISC_IMAGE_BROKEN,      /* a CHD that cannot be read: damaged, or a child of another CHD (not supported) */
    DISC_IMAGE_NOT_A_DISC,  /* a CHD of something else (a hard disk, a laserdisc), or a CD with no data track */
    DISC_IMAGE_NO_MEMORY,
} DiscImageResult;

/* Opens the image in `file` (opened "rb"), which it owns from here: closed by disc_image_close, or here when
   the result is not DISC_IMAGE_OK. A file that is not a CHD is taken as an ISO image. */
DiscImageResult disc_image_open(FILE *file, DiscImage **image);

/* the player's words for a result: "the CHD is damaged ..." */
const char *disc_image_result_text(DiscImageResult result);

/* Reads `sectors` sectors from `lsn` into `buffer` (sectors * DISC_IMAGE_SECTOR_SIZE bytes). Past the end of
   the image, or where the image cannot be read, reads zeros. Returns the sectors read from the image (0 when
   none was; fewer than `sectors` when some are zeros because they could not be read). */
uint32_t disc_image_read(DiscImage *image, uint32_t lsn, uint32_t sectors, void *buffer);

/* what the image is, for the log: "ISO image", "CHD (DVD)", "CHD (CD, MODE2_RAW)" */
const char *disc_image_format(const DiscImage *image);

/* closes the image and its file; NULL does nothing */
void disc_image_close(DiscImage *image);

#ifdef __cplusplus
}
#endif

#endif
