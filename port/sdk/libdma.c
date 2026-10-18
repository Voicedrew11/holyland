/* libdma for the PC port (port/include/libdma.h), as Sony's (decomp/src/sce/libdma): the channels
   are the register blocks in eeregs.h, the ones the game also starts by hand, and a send sets them up
   and starts the channel for the DMA controller (port/platform/dmac.c) to finish. */
#include "eeregs.h"
#include "libdma.h"
#include "port/host.h"
#include "port/platform.h"

#define CHCR_STR 0x100

static const uint32_t channel_base[10] = {
    0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
    0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400,
};

/* Which channels sceDmaReset clears: all but the three SIF channels. */
static const int reset_channel[10] = {1, 1, 1, 1, 1, 0, 0, 0, 1, 1};

static int channel_number(DmaChan *channel) {
    int i;

    for (i = 0; i < 10; i++) {
        if (channel == EE_HW(DmaChan *, channel_base[i])) {
            return i;
        }
    }
    PORT_FATAL("libdma: %p is not a DMA channel", (void *)channel);
}

DmaChan *sceDmaGetChan(unsigned int id) {
    return id < 10 ? EE_HW(DmaChan *, channel_base[id]) : NULL;
}

/* Clears the channels and the controller's status, and with mode 1 turns the controller on. Returns
   whether it was on. */
int sceDmaReset(int mode) {
    int old = *D_CTRL & 1;
    int i;

    for (i = 0; i < 10; i++) {
        if (reset_channel[i]) {
            volatile int *registers = EE_HW(volatile int *, channel_base[i]);

            registers[0x00 / 4] = 0;
            registers[0x30 / 4] = 0;
            registers[0x10 / 4] = 0;
            registers[0x50 / 4] = 0;
            registers[0x40 / 4] = 0;
            registers[0x80 / 4] = 0;
        }
    }
    *D_STAT = *D_STAT & 0xFF1F0000;
    if (mode == 1) {
        *D_CTRL = *D_CTRL | 1;
    }
    return old;
}

/* Starts the chain at addr (source chain mode, from memory). */
void sceDmaSend(DmaChan *channel, void *addr) {
    int number = channel_number(channel);

    port_dmac_wait(number);
    channel->tadr = (int)EE_WORD(addr);
    channel->qwc = 0;
    __atomic_store_n(&channel->chcr, (channel->chcr & ~0xC) | 0x105, __ATOMIC_RELEASE);
    port_dmac_wake();
}

/* Starts qwc quadwords at addr (normal mode, from memory). */
void sceDmaSendN(DmaChan *channel, void *addr, int qwc) {
    int number = channel_number(channel);

    port_dmac_wait(number);
    channel->madr = (int)EE_WORD(addr);
    channel->qwc = qwc;
    __atomic_store_n(&channel->chcr, (channel->chcr & ~0xC) | 0x101, __ATOMIC_RELEASE);
    port_dmac_wake();
}

/* Mode 1: whether the channel is still running; else waits until it is done and returns 0. */
int sceDmaSync(DmaChan *channel, int mode, int timeout) {
    (void)timeout;
    if (mode == 1) {
        port_dmac_wake();
        return (channel->chcr >> 8) & 1;
    }
    port_dmac_wait(channel_number(channel));
    return 0;
}
