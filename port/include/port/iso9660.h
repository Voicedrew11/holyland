/* An ISO 9660 disc's files: the game's disc reader (port/platform/disc.c) and the launcher's disc check
   (launcher/disc_check.cpp) list a disc image (port/disc_image.h: an .iso or a .chd) with this. No other part of
   the port is used here.

   Paths are the disc's, from the root, '\'-separated and without the version: "\DATA\KF4.DAT". */
#ifndef PORT_ISO9660_H
#define PORT_ISO9660_H

#include <stdint.h>

#include "port/disc_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISO9660_SECTOR_SIZE DISC_IMAGE_SECTOR_SIZE

typedef enum {
    ISO9660_OK,
    ISO9660_NOT_ISO,       /* no primary volume descriptor at sector 16 */
    ISO9660_BAD_DIRECTORY, /* a directory that cannot be read or does not hold together */
} Iso9660Result;

typedef void (*Iso9660Visit)(void *context, const char *path, uint32_t lsn, uint32_t size);

/* Calls `visit` for every file (not directory) of the image, in the order the directories list them,
   and gives the disc's size in sectors. */
Iso9660Result iso9660_list(DiscImage *image, uint32_t *disc_sectors, Iso9660Visit visit, void *context);

#ifdef __cplusplus
}
#endif

#endif
