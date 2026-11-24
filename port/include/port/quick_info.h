/* A quick save's PC block (port/platform/quick_info.c): what the file says about itself, packed into
   bytes and back. The layout is described with the file's in port/platform/quicksave.c. */
#ifndef PORT_QUICK_INFO_H
#define PORT_QUICK_INFO_H

#include <stdint.h>

#include "port/quicksave.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_QUICK_INFO_SIZE 352u    /* the block written now: with the neighbour and the live world */
#define PORT_QUICK_INFO_SIZE_MIN 64u /* the first files' block (no neighbour, no live world) */

/* Packs info into PORT_QUICK_INFO_SIZE bytes at p */
void port_quick_info_pack(uint8_t *p, const PortQuickInfo *info);
/* Unpacks a block of `size` bytes (at least PORT_QUICK_INFO_SIZE_MIN): what an older, shorter block
   lacks is left as for a file without it (neighbour_known 0, map_count 0); a longer one's rest is skipped */
void port_quick_info_unpack(const uint8_t *p, uint32_t size, PortQuickInfo *info);

#ifdef __cplusplus
}
#endif

#endif
