/*
 * port/include/eeregs.h
 *
 * The EE's memory-mapped I/O registers for the PC port: the decomp's sce/libkernl/eeregs.h with each
 * address put through EE_HW (port/host.h), so `*D4_CHCR = 0x101;` is a store into the host's page of
 * register memory. The names and their sources are as in the decomp's header.
 */
#ifndef SCE_LIBKERNL_EEREGS_H
#define SCE_LIBKERNL_EEREGS_H

#include "port/host.h"

/* Timers. */ /* derived name */
#define T0_COUNT EE_HW(volatile int *, 0x10000000)
#define T0_MODE EE_HW(volatile int *, 0x10000010)
#define T1_COUNT EE_HW(volatile int *, 0x10000800)
#define T1_MODE EE_HW(volatile int *, 0x10000810)
#define T3_MODE EE_HW(volatile int *, 0x10001810)

/* IPU. */ /* derived name */
#define IPU_CMD EE_HW(volatile int *, 0x10002000)
#define IPU_CTRL EE_HW(volatile int *, 0x10002010)
#define IPU_BP EE_HW(volatile int *, 0x10002020)
#define IPU_TOP EE_HW(volatile int *, 0x10002030)
#define IPU_in_FIFO EE_HW(volatile void *, 0x10007010)

/* GIF. */
#define GIF_CTRL EE_HW(volatile int *, 0x10003000) /* derived name */
#define GIF_STAT EE_HW(volatile int *, 0x10003020)
#define GIF_STAT_APATH 0xC00 /* derived name; bits 10-11, the path in use (0 = idle) */

/* VIF0. */ /* derived name */
#define VIF0_FBRST EE_HW(volatile int *, 0x10003810)
#define VIF0_ERR EE_HW(volatile int *, 0x10003820)
#define VIF0_MARK EE_HW(volatile int *, 0x10003830)
#define VIF0_FIFO EE_HW(volatile void *, 0x10004000)

/* VIF1. */
#define VIF1_STAT EE_HW(volatile int *, 0x10003C00)
#define VIF1_FBRST EE_HW(volatile int *, 0x10003C10) /* derived name */
#define VIF1_FIFO EE_HW(volatile void *, 0x10005000)  /* derived name */

/* DMA channel 0, VIF0. */ /* derived name */
#define D0_CHCR EE_HW(volatile int *, 0x10008000)

/* DMA channel 1, VIF1. */
#define D1_CHCR EE_HW(volatile int *, 0x10009000)
#define D1_MADR EE_HW(volatile int *, 0x10009010)
#define D1_QWC EE_HW(volatile int *, 0x10009020)
#define D1_TADR EE_HW(volatile int *, 0x10009030)

/* DMA channel 2, GIF. */
#define D2_CHCR EE_HW(volatile int *, 0x1000A000)
#define D2_MADR EE_HW(volatile int *, 0x1000A010)
#define D2_QWC EE_HW(volatile int *, 0x1000A020)
#define D2_TADR EE_HW(volatile int *, 0x1000A030)

/* DMA channel 3, from IPU. */
#define D3_CHCR EE_HW(volatile int *, 0x1000B000)
#define D3_MADR EE_HW(volatile int *, 0x1000B010) /* derived name */
#define D3_QWC EE_HW(volatile int *, 0x1000B020)  /* derived name */

/* DMA channel 4, to IPU. */
#define D4_CHCR EE_HW(volatile int *, 0x1000B400)
#define D4_MADR EE_HW(volatile int *, 0x1000B410) /* derived name */
#define D4_QWC EE_HW(volatile int *, 0x1000B420)  /* derived name */
#define D4_TADR EE_HW(volatile int *, 0x1000B430) /* derived name */

/* DMA channels 5 to 7, SIF0 to SIF2. */ /* derived name */
#define D5_CHCR EE_HW(volatile int *, 0x1000C000)
#define D6_CHCR EE_HW(volatile int *, 0x1000C400)
#define D7_CHCR EE_HW(volatile int *, 0x1000C800)

/* DMA channel 8, from scratchpad. */ /* derived name */
#define D8_CHCR EE_HW(volatile int *, 0x1000D000)
#define D8_MADR EE_HW(volatile int *, 0x1000D010)
#define D8_QWC EE_HW(volatile int *, 0x1000D020)
#define D8_SADR EE_HW(volatile int *, 0x1000D080)

/* DMA channel 9, to scratchpad. */ /* derived name */
#define D9_CHCR EE_HW(volatile int *, 0x1000D400)
#define D9_MADR EE_HW(volatile int *, 0x1000D410)
#define D9_QWC EE_HW(volatile int *, 0x1000D420)
#define D9_TADR EE_HW(volatile int *, 0x1000D430)
#define D9_SADR EE_HW(volatile int *, 0x1000D480)

/* Dn_CHCR fields, shared by every channel's control register. */ /* derived name */
#define D_CHCR_DIR_FROM_MEMORY 0x1 /* bit0: DIR, the transfer reads memory */
#define D_CHCR_MOD_CHAIN 0x4       /* bits2-3: MOD, DMAtag chain */
#define D_CHCR_TTE 0x40            /* bit6: TTE, transfer the DMAtag too */
#define D_CHCR_STR 0x100           /* bit8: STR, start; set while the transfer runs */

/* DMA controller. */
#define D_CTRL EE_HW(volatile int *, 0x1000E000)
#define D_STAT EE_HW(volatile int *, 0x1000E010)    /* derived name */
#define D_PCR EE_HW(volatile int *, 0x1000E020)     /* derived name */
#define D_SQWC EE_HW(volatile int *, 0x1000E030)    /* derived name */
#define D_RBSR EE_HW(volatile int *, 0x1000E040)    /* derived name */
#define D_RBOR EE_HW(volatile int *, 0x1000E050)    /* derived name */
#define D_STADR EE_HW(volatile int *, 0x1000E060)   /* derived name */
#define D_ENABLER EE_HW(volatile int *, 0x1000F520) /* derived name */
#define D_ENABLEW EE_HW(volatile int *, 0x1000F590) /* derived name */

/* Interrupt controller. */ /* derived name */
#define INTC_STAT EE_HW(volatile int *, 0x1000F000)

/* GS privileged registers, 64 bits wide. */ /* derived name */
#define GS_PMODE EE_HW(volatile int64_t *, 0x12000000)
#define GS_SMODE2 EE_HW(volatile int64_t *, 0x12000020)
#define GS_DISPFB1 EE_HW(volatile int64_t *, 0x12000070)
#define GS_DISPLAY1 EE_HW(volatile int64_t *, 0x12000080)
#define GS_DISPFB2 EE_HW(volatile int64_t *, 0x12000090)
#define GS_DISPLAY2 EE_HW(volatile int64_t *, 0x120000A0)
#define GS_EXTDATA EE_HW(volatile int64_t *, 0x120000C0)
#define GS_BGCOLOR EE_HW(volatile int64_t *, 0x120000E0)
#define GS_CSR EE_HW(volatile uint64_t *, 0x12001000)
#define GS_BUSDIR EE_HW(volatile uint64_t *, 0x12001040)

#endif /* SCE_LIBKERNL_EEREGS_H */
