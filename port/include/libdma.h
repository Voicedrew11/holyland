/*
 * port/include/libdma.h
 *
 * libdma for the PC port (port/sdk/libdma.c). sceDmaSend is where the game hands a finished packet
 * chain to the hardware; on the PC it is the hook the renderer takes the chain from.
 */
#ifndef SCE_LIBDMA_LIBDMA_H
#define SCE_LIBDMA_LIBDMA_H

#include "eeregs.h"

/* A channel's registers (CHCR, MADR, QWC, TADR), as the game reads them. CHCR is atomic (eeregs.h's EeChcr). */
typedef struct DmaChan {
    EeChcr chcr;
    int pad0[3];
    int madr;
    int pad1[3];
    int qwc;
    int pad2[3];
    int tadr;
} DmaChan;

/* The channels: 0 VIF0, 1 VIF1, 2 GIF, 3 from IPU, 4 to IPU, 5 to 7 SIF, 8 and 9 scratchpad. */
DmaChan *sceDmaGetChan(unsigned int id);
int sceDmaReset(int mode);
/* Sends the chain or block at addr (a pointer on the PC; the EE took the address as a number). */
void sceDmaSend(DmaChan *channel, void *addr);
void sceDmaSendN(DmaChan *channel, void *addr, int qwc);
int sceDmaSync(DmaChan *channel, int mode, int timeout);

#endif /* SCE_LIBDMA_LIBDMA_H */
