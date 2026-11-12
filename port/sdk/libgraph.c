/* libgraph on the PC: Sony's library as the decomp has it (decomp/src/sce/libgraph/graphNNN.c), with
   the R5900 left out. The GS records are built bit for bit as on the PS2, and go to the GS the same
   way: by DMA channels 1 and 2 (port/platform/dmac.c) and the GS's privileged registers, which
   port/platform/gs.c scans out at each vblank.

   Where the PC differs:
   - the GS's CSR is plain memory that the kernel keeps FIELD in (port/platform/kernel.c), so the
     writes that reset or flush the GS leave it alone, and clearing FINISH clears the bit; the GS
     sets FINISH when it has had the register (port/platform/gs.c);
   - a DMA address is an EE address (port/platform/ee_address.c), not a physical one masked from a
     pointer; a packet on the stack, which has none, is sent from a copy;
   - the VIF1 FIFO reads and writes go to VIF1 and the GS on the DMA controller's thread;
   - SetGsCrt (a kernel call) and the INTC masks are done here. */
#include <stdio.h>
#include <string.h>

#include "eeregs.h"
#include "libgraph.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/savestate.h"

#define GS_SMODE1 EE_HW(volatile int64_t *, 0x12000010)
#define CHCR_STR 0x100
#define GS_CSR_FINISH 2

static sceGsGParam gsGParam = {1, 2, 1, 3, 0, 0};

/* ---- The PC's helpers ------------------------------------------------------------------------- */
/* A DMA channel's MADR for a packet: its EE address, with the scratchpad's under bit 31 as Sony
   writes it. A packet with no EE address is copied into bounce first (bytes of it). */
static uint32_t dma_address(const void *packet, void *bounce, size_t bounce_size, size_t bytes) {
    EEWord word;

    if (!ee_word_try(packet, &word)) {
        if (bytes > bounce_size) {
            PORT_FATAL("libgraph: a packet of 0x%zx bytes off the stack, more than the copy holds", bytes);
        }
        memcpy(bounce, packet, bytes);
        word = EE_WORD(bounce);
    }
    if (word - EE_SCRATCHPAD_ADDRESS < 0x4000u) {
        return (word & 0x3FFF) | 0x80000000u;
    }
    return word;
}

/* (the channels the game started before are queued first, port/platform/dmac.c) */
static void start_channel(EeChcr *chcr, int value) {
    port_dmac_wake();
    *chcr = value;
    port_dmac_wake();
}

/* CSR = FINISH: the write clears the flag */
static void clear_finish(void) {
    __atomic_fetch_and(GS_CSR, ~(uint64_t)GS_CSR_FINISH, __ATOMIC_RELAXED);
}

/* Sony's waits give up after 0x1000000 polls; on the PC the controller always finishes */
static void wait_channel(int channel) {
    port_dmac_wait(channel);
}

/* The kernel's SetGsCrt: the video mode in SMODE1 and SMODE2. */
static void set_gs_crt(short inter, short omode, short ffmd) {
    switch (omode) {
    case 2: /* NTSC */
        *GS_SMODE1 = 0x740834504LL;
        break;
    case 3: /* PAL */
        *GS_SMODE1 = 0x740814504LL;
        break;
    default:
        PORT_TODO("SetGsCrt: a video mode other than NTSC or PAL");
        break;
    }
    *GS_SMODE2 = (int64_t)(inter & 1) | ((int64_t)(ffmd & 1) << 1);
}

static void feed_vif1(void *words) {
    port_vif1_feed(words, 4);
}

/* sceGsResetPath's FIFO writes */
static void reset_path(void *packet) {
    port_vif1_reset();
    port_vu1_reset();
    port_vif1_feed(packet, 8);
}

typedef struct {
    void *dst;
    uint32_t qwords;
} FifoRead;

static void read_fifo(void *arg) {
    FifoRead *read = arg;

    port_gs_read_fifo(read->dst, read->qwords);
}

/* ---- graph001 --------------------------------------------------------------------------------- */
void sceGsResetGraph(short mode, short inter, short omode, short ffmd) {
    sceGsGParam *g;

    switch (mode) {
    case 0:
        g = sceGsGetGParam();
        /* (the GS reset, CSR 0x200) */
        g->inter = inter;
        g->omode = omode;
        g->version = (short)((*GS_CSR >> 16) & 0xFF);
        /* (GsPutIMR(0xFF00)) */
        g->ffmd = ffmd != 0;
        if (g->intcUsed != 0) {
            PORT_TODO("sceGsResetGraph: libgraph's own vblank handler");
            g->intcUsed = 0;
            g->handler = 0;
        }
        set_gs_crt(inter & 1, omode & 0xFF, ffmd & 1);
        break;
    case 1:
        /* (the GS flush, CSR 0x100) */
        break;
    }
}

sceGsGParam *sceGsGetGParam(void) {
    return &gsGParam;
}

/* ---- graph002 --------------------------------------------------------------------------------- */
/* STCYCL 4 4, STMASK 0, NOP, STMOD 0, MSKPATH3 0, BASE 0, OFFSET 0, ITOP 0 */
static unsigned int sceGsResetPathPacket[8] __attribute__((aligned(16))) = {
    0x01000404, 0x20000000, 0x00000000, 0x05000000, 0x06000000, 0x03000000, 0x02000000, 0x04000000};

void sceGsResetPath(void) {
    /* VIF1_FBRST 1, VIF1_ERR 2, VU1 reset (VPU_STAT), GIF_CTRL 1, then the packet into the FIFO */
    *VIF1_FBRST = 1;
    *EE_HW(volatile int *, 0x10003C20) = 2;
    port_dmac_call(reset_path, sceGsResetPathPacket);
    *GIF_CTRL = 1;
}

/* ---- graph003 --------------------------------------------------------------------------------- */
void sceGsSetDefDispEnv(sceGsDispEnv *disp, short psm, short w, short h, short dx, short dy) {
    sceGsGParam *gp = sceGsGetGParam();

    disp->pmode = 0x66;
    if (gp->inter != 0) {
        if (gp->ffmd != 0) {
            disp->smode2 = 3;
        } else {
            disp->smode2 = 1;
        }
    } else {
        disp->smode2 = 2;
    }
    disp->dispfb = ((int64_t)(psm & 0xF) << 15) | ((int64_t)(((w + 0x3F) >> 6) & 0x3F) << 9);

    if (gp->omode == 2 || gp->omode == 3) {
        int ntsc = gp->omode == 2;
        int x0 = ntsc ? 0x27C : 0x290;
        int q = (w + 0x9FF) / w;
        int64_t magh = (int64_t)(q - 1) << 23;
        int64_t dw = (int64_t)(q * w - 1) << 32;
        int64_t ddx = ((int64_t)(dx * q) + x0) & 0xFFF;

        if (gp->inter == 1) {
            int64_t ddy = (int64_t)((dy + (ntsc ? 0x32 : 0x48)) & 0xFFF) << 12;

            if (gp->ffmd) {
                disp->display = magh | dw | (ddx | (int64_t)(h * 2 - 1) << 44) | ddy;
            } else {
                disp->display = magh | dw | (ddx | (int64_t)(h - 1) << 44) | ddy;
            }
        } else {
            disp->display = magh | dw | (ddx | (int64_t)(h - 1) << 44) |
                            (int64_t)((dy + (ntsc ? 0x19 : 0x24)) & 0xFFF) << 12;
        }
    } else {
        printf("sceGsDefDispEnv:Not support displaymode for %d!!\n", gp->omode);
    }
    disp->bgcolor = 0;
}

/* ---- graph004 --------------------------------------------------------------------------------- */
void sceGsPutDispEnv(void *disp) {
    int64_t *s = (int64_t *)disp;

    if (sceGsGetGParam()->version == 1) {
        *GS_PMODE = s[0];
        *GS_DISPFB1 = s[2];
        *GS_DISPLAY1 = s[3];
        *GS_EXTDATA = s[4];
    } else {
        *GS_PMODE = s[0];
        *GS_SMODE2 = s[1];
        *GS_DISPFB2 = s[2];
        *GS_DISPLAY2 = s[3];
        *GS_BGCOLOR = s[4];
    }
}

/* ---- graph005 --------------------------------------------------------------------------------- */
short sceGszbufaddr(short psm, short width, short height) {
    sceGsGParam *gp = sceGsGetGParam();
    int h;
    int w;

    h = (width + 0x3F) / 0x40;
    if (psm & 2) {
        w = (height + 0x3F) / 0x40;
    } else {
        w = (height + 0x1F) / 0x20;
    }
    /* (the member reads the modes unsigned) */
    if ((unsigned short)gp->inter == 1 && (unsigned short)gp->ffmd == 0) {
        return (short)(h * w);
    }
    return (short)(h * w * 2);
}

/* ---- graph006, graph025 ----------------------------------------------------------------------- */
static int set_def_draw_env(sceGsDrawEnv *env, short psm, short w, short h, short ztst, short zpsm, int context2) {
    if (context2) {
        env->frame_addr = 0x4D;
        env->frame = ((int64_t)((w >> 6) & 0x3F) << 16) | ((int64_t)(psm & 0xF) << 24);
        env->zbuf_addr = 0x4F;
    } else {
        env->frame_addr = 0x4C;
        env->frame = ((int64_t)(((w + 0x3F) >> 6) & 0x3F) << 16) | ((int64_t)(psm & 0xF) << 24);
        env->zbuf_addr = 0x4E;
    }
    if (ztst == 0) {
        env->zbuf = (int64_t)(short)sceGszbufaddr(psm, w, h) | ((int64_t)(zpsm & 0xF) << 24) | ((int64_t)1 << 32);
    } else {
        env->zbuf = (int64_t)(short)sceGszbufaddr(psm, w, h) | ((int64_t)(zpsm & 0xF) << 24);
    }
    env->xyoffset_addr = context2 ? 0x19 : 0x18;
    env->xyoffset = (((int64_t)0x800 - (short)(w >> 1)) << 4) | (((int64_t)0x800 - (short)(h >> 1)) << 36);
    env->scissor_addr = context2 ? 0x41 : 0x40;
    env->scissor = ((int64_t)(w - 1) << 16) | ((int64_t)(h - 1) << 48);
    env->prmodecont_addr = 0x1A;
    env->prmodecont |= 1;
    env->colclamp_addr = 0x46;
    env->colclamp |= 1;
    env->dthe_addr = 0x45;
    if (psm & 2) {
        env->dthe |= 1;
    } else {
        env->dthe &= ~1;
    }
    env->test_addr = context2 ? 0x48 : 0x47;
    if (ztst) {
        env->test = ((int64_t)(ztst & 3) << 17) | 0x00010000;
    } else {
        env->test = 0x00030000;
    }
    return 8;
}

int sceGsSetDefDrawEnv(void *env, short psm, short w, short h, short ztst, short zpsm) {
    return set_def_draw_env(env, psm, w, h, ztst, zpsm, 0);
}

int sceGsSetDefDrawEnv2(void *env, short psm, short w, short h, short ztst, short zpsm) {
    return set_def_draw_env(env, psm, w, h, ztst, zpsm, 1);
}

/* ---- graph007 --------------------------------------------------------------------------------- */
int sceGsSetDefClear(sceGsClear *cl, short ztst, short x, short y, short w, short h, unsigned char r,
                     unsigned char g, unsigned char b, unsigned char a, unsigned int z) {
    cl->test_1_addr = 0x47;
    cl->test_1 = 0x00030000;
    cl->prim_addr = 0;
    cl->prim = 6;
    cl->rgbaq_addr = 1;
    cl->rgbaq = (uint64_t)r | ((uint64_t)g << 8) | ((uint64_t)b << 16) | ((uint64_t)a << 24) |
                ((uint64_t)0x3F800000 << 32);
    cl->xyz2_0_addr = 5;
    cl->xyz2_0 = (uint64_t)(int64_t)(x << 4) | ((uint64_t)(int64_t)(y << 4) << 16) | ((uint64_t)z << 32);
    cl->xyz2_1_addr = 5;
    cl->xyz2_1 = (uint64_t)(int64_t)((x + w) << 4) | ((uint64_t)(int64_t)((y + h) << 4) << 16) | ((uint64_t)z << 32);
    cl->test_1r_addr = 0x47;
    if (ztst) {
        cl->test_1r = ((uint64_t)(ztst & 3) << 17) | 0x00010000;
    } else {
        cl->test_1r = 0x00030000;
    }
    return 6;
}

/* ---- graph008 --------------------------------------------------------------------------------- */
static uint8_t put_draw_env_copy[0x800] __attribute__((aligned(16)));

int sceGsPutDrawEnv(void *pkt) {
    int qwc;
    size_t bytes;

    wait_channel(2);
    qwc = (int)(*(int64_t *)pkt & 0x7FFF);
    bytes = (size_t)(qwc + 1) * 16;
    if (bytes > sizeof(put_draw_env_copy)) {
        PORT_FATAL("libgraph: a draw environment of 0x%zx bytes, more than the copy holds", bytes);
    }
    /* Always sent from a copy, as it is at the start (a transfer is done at its start, port/platform/dmac.c): the
       game rewrites its draw environments without waiting for the channel (Map_DrawRecords sets the clear colour in
       both every frame), and the controller's thread may not have taken the last one yet (ThreadSanitizer). */
    memcpy(put_draw_env_copy, pkt, bytes);
    *D2_QWC = qwc + 1;
    *D2_MADR = (int)dma_address(put_draw_env_copy, NULL, 0, bytes);
    start_channel(D2_CHCR, 0x101);
    return 0;
}

/* ---- graph009, graph023 ----------------------------------------------------------------------- */
/* a GIF tag for NLOOP A+D register writes, ending the packet */
static void set_ad_tag(int64_t *tag, int nloop) {
    tag[0] = (int64_t)nloop | ((int64_t)1 << 15) | ((int64_t)1 << 60);
    tag[1] = 0xE;
}

/* DISPFB's FBP (bits 0-8) and FRAME's FBP (bits 0-8), rewritten in place */
static void set_fbp(int64_t *reg, int fbp) {
    *reg = (*reg & ~(int64_t)0x1FF) | (fbp & 0x1FF);
}

void sceGsSetDefDBuff(void *buffer, short psm, short w, short h, short ztst, short zpsm, short flag) {
    sceGsDBuff *db = buffer;
    sceGsGParam *gp = sceGsGetGParam();
    int zb;
    short fbp;

    sceGsSetDefDispEnv(&db->disp[0], psm, w, h, 0, 0);
    sceGsSetDefDispEnv(&db->disp[1], psm, w, h, 0, 0);
    sceGsSetDefDrawEnv(&db->draw0, psm, w, h, ztst, zpsm);
    sceGsSetDefDrawEnv(&db->draw1, psm, w, h, ztst, zpsm);
    if (flag) {
        sceGsSetDefClear(&db->clear0, ztst, 0x800 - (w >> 1), 0x800 - (h >> 1), w, h, 0, 0, 0, 0, 0);
        sceGsSetDefClear(&db->clear1, ztst, 0x800 - (w >> 1), 0x800 - (h >> 1), w, h, 0, 0, 0, 0, 0);
    }
    set_ad_tag(db->giftag0, flag ? 0xE : 8);
    set_ad_tag(db->giftag1, flag ? 0xE : 8);

    zb = sceGszbufaddr(psm, w, h);
    if ((gp->inter == 1 && gp->ffmd == 1) || gp->inter == 0) {
        fbp = (short)(zb >> 1);
        set_fbp(&db->disp[1].dispfb, fbp);
        set_fbp(&db->draw0.frame, zb >> 1);
    }
}

/* sceGsSetDefDBuff with a context-2 draw environment after each frame's own */
typedef struct {
    sceGsDispEnv disp[2]; /* 0x000 */
    int64_t giftag0[2];   /* 0x050 */
    sceGsDrawEnv draw0;   /* 0x060 */
    sceGsDrawEnv draw0c2; /* 0x0E0 */
    sceGsClear clear0;    /* 0x160 */
    int64_t giftag1[2];   /* 0x1C0 */
    sceGsDrawEnv draw1;   /* 0x1D0 */
    sceGsDrawEnv draw1c2; /* 0x250 */
    sceGsClear clear1;    /* 0x2D0 */
} sceGsDBuffDc;
LAYOUT_SIZE(sceGsDBuffDc, 0x330);
LAYOUT_SIZE(sceGsDBuff, 0x230);

void sceGsSetDefDBuffDc(void *buffer, short psm, short w, short h, short ztst, short zpsm, short flag) {
    sceGsDBuffDc *db = buffer;
    sceGsGParam *gp = sceGsGetGParam();
    int zb;
    short fbp;

    sceGsSetDefDispEnv(&db->disp[0], psm, w, h, 0, 0);
    sceGsSetDefDispEnv(&db->disp[1], psm, w, h, 0, 0);
    sceGsSetDefDrawEnv(&db->draw0, psm, w, h, ztst, zpsm);
    sceGsSetDefDrawEnv2(&db->draw0c2, psm, w, h, ztst, zpsm);
    sceGsSetDefDrawEnv(&db->draw1, psm, w, h, ztst, zpsm);
    sceGsSetDefDrawEnv2(&db->draw1c2, psm, w, h, ztst, zpsm);
    if (flag) {
        sceGsSetDefClear(&db->clear0, ztst, 0x800 - (w >> 1), 0x800 - (h >> 1), w, h, 0, 0, 0, 0, 0);
        sceGsSetDefClear(&db->clear1, ztst, 0x800 - (w >> 1), 0x800 - (h >> 1), w, h, 0, 0, 0, 0, 0);
    }
    set_ad_tag(db->giftag0, flag ? 0x16 : 0x10);
    set_ad_tag(db->giftag1, flag ? 0x16 : 0x10);

    zb = sceGszbufaddr(psm, w, h);
    if ((gp->inter == 1 && gp->ffmd == 1) || gp->inter == 0) {
        fbp = (short)(zb >> 1);
        set_fbp(&db->disp[1].dispfb, fbp);
        set_fbp(&db->draw0.frame, zb >> 1);
        set_fbp(&db->draw0c2.frame, zb >> 1);
    }
}

/* ---- graph010, graph024 ----------------------------------------------------------------------- */
int sceGsSwapDBuff(void *db, int id) {
    int s0 = id & 1;

    sceGsPutDispEnv((char *)db + s0 * 0x28);
    return sceGsPutDrawEnv((char *)db + (s0 ? 0x140 : 0x50));
}

int sceGsSwapDBuffDc(void *db, int id) {
    int s0 = id & 1;

    sceGsPutDispEnv((char *)db + s0 * 0x28);
    return sceGsPutDrawEnv((char *)db + (s0 ? 0x1C0 : 0x50));
}

/* ---- graph011 --------------------------------------------------------------------------------- */
/* Waits for the next vblank and returns the field it starts (GS_CSR's FIELD bit: 1 for the odd one),
   or 1 when the display is not interlaced. libgraph's own handler (intcUsed) is never installed. */
int sceGsSyncV(int mode) {
    sceGsGParam *gp = sceGsGetGParam();

    (void)mode;
    port_vsync();
    if (gp->inter != 1) {
        return 1;
    }
    return (int)((*GS_CSR >> 13) & 1);
}

/* ---- graph012 --------------------------------------------------------------------------------- */
/* Mode 0 waits until paths 1 to 3 are idle and returns 0; otherwise the bits of the ones that are
   busy (1 channel 1, 2 channel 2; VIF1, VU1 and the GIF are idle once their channel is done). */
int sceGsSyncPath(int mode, unsigned short timeout) {
    int r;

    (void)timeout;
    if (mode == 0) {
        port_dmac_wait(1);
        port_dmac_wait(2);
        return 0;
    }
    port_dmac_wake();
    r = 0;
    if (*D1_CHCR & CHCR_STR) {
        r |= 1;
    }
    if (*D2_CHCR & CHCR_STR) {
        r |= 2;
    }
    return r;
}

/* ---- graph015 --------------------------------------------------------------------------------- */
int sceGsSetDefLoadImage(void *image, short dbp, short dbw, short dpsm, short dsax, short dsay, short rrw,
                         short rrh) {
    sceGsLoadImage *di = image;
    int size = 0;

    switch (dpsm) {
    case 0:
    case 48:
        size = (rrw * rrh) >> 2;
        break;
    case 1:
    case 49:
        size = (rrw * rrh * 3) >> 4;
        break;
    case 2:
    case 10:
    case 50:
    case 58:
        size = (rrw * rrh) >> 3;
        break;
    case 19:
    case 27:
        size = (rrw * rrh) >> 4;
        break;
    case 20:
    case 36:
    case 44:
        size = (rrw * rrh) >> 5;
        break;
    }
    if (size > 0x7FFF) {
        printf("sceGsSetDefLoadImage: too big size\r\n");
        return 0;
    }
    /* NLOOP 4, NREG 1, A+D */
    di->giftag0[0] = 4 | ((int64_t)1 << 60);
    di->giftag0[1] = 0xE;
    di->bitbltbuf = ((int64_t)dbp << 32) | ((int64_t)dbw << 48) | ((int64_t)dpsm << 56);
    di->abitbltbuf = 0x50;
    di->trxpos = ((int64_t)dsax << 32) | ((int64_t)dsay << 48);
    di->atrxpos = 0x51;
    di->trxreg = (int64_t)rrw | ((int64_t)rrh << 32);
    di->atrxreg = 0x52;
    di->trxdir = 0;
    di->atrxdir = 0x53;
    /* NLOOP size, EOP, IMAGE */
    di->giftag1[0] = (int64_t)(size & 0x7FFF) | ((int64_t)1 << 15) | ((int64_t)2 << 58);
    di->giftag1[1] = 0;
    return 6;
}

/* ---- graph016 --------------------------------------------------------------------------------- */
void sceGsSetDefStoreImage(void *image, short sbp, short sbw, short spsm, short ssax, short ssay, short rrw,
                           short rrh) {
    sceGsStoreImage *di = image;

    /* NOP, MSKPATH3 1, FLUSHA, DIRECT 6 */
    di->vifcode[0] = 0;
    di->vifcode[1] = 0x06008000;
    di->vifcode[2] = 0x13000000;
    di->vifcode[3] = 0x50000006;
    /* NLOOP 5, EOP, NREG 1, A+D */
    di->giftag[0] = 5 | ((int64_t)1 << 15) | ((int64_t)1 << 60);
    di->giftag[1] = 0xE;
    di->bitbltbuf = (int64_t)sbp | ((int64_t)sbw << 16) | ((int64_t)spsm << 24);
    di->abitbltbuf = 0x50;
    di->trxpos = (int64_t)ssax | ((int64_t)ssay << 16);
    di->atrxpos = 0x51;
    di->trxreg = (int64_t)rrw | ((int64_t)rrh << 32);
    di->atrxreg = 0x52;
    di->finish = 0;
    di->afinish = 0x61;
    di->trxdir = 1;
    di->atrxdir = 0x53;
}

/* ---- graph017 --------------------------------------------------------------------------------- */
static uint8_t load_image_copy[0x60] __attribute__((aligned(16)));
/* an image's largest transfer, 0x7FFF qwords */
static uint8_t image_copy[0x7FFF * 16] __attribute__((aligned(16)));

int sceGsExecLoadImage(void *pkt, void *img) {
    int qwc;

    wait_channel(2);
    *D2_QWC = 6;
    *D2_MADR = (int)dma_address(pkt, load_image_copy, sizeof(load_image_copy), 0x60);
    start_channel(D2_CHCR, 0x101);
    wait_channel(2);
    qwc = (int)(*(int64_t *)((char *)pkt + 0x50) & 0x7FFF);
    *D2_QWC = qwc;
    /* (Vram_Clear's image is on the stack) */
    *D2_MADR = (int)dma_address(img, image_copy, sizeof(image_copy), (size_t)qwc * 16);
    start_channel(D2_CHCR, 0x101);
    return 0;
}

/* ---- graph018 --------------------------------------------------------------------------------- */
/* MSKPATH3 0 then three NOPs, into the VIF1 FIFO to unmask PATH3 again */
static unsigned int sceGsUnmaskPath3Packet[4] __attribute__((aligned(16))) = {0x06000000, 0, 0, 0};
static uint8_t store_image_copy[0x70] __attribute__((aligned(16)));
static uint8_t store_image_tail[16 * 16] __attribute__((aligned(16)));

int sceGsExecStoreImage(void *pkt, void *img) {
    int64_t *words = pkt;
    int rem = 0;
    int n1;
    int n8;
    int psm;
    int w;
    int h;
    int nh;
    int len;
    int rest;
    int j;
    int extra = 0;
    int64_t trxreg;
    FifoRead read;

    n8 = 0;
    n1 = 0;
    nh = 0;
    trxreg = words[8];
    w = (int)(trxreg & 0xFFF);
    h = (int)((trxreg >> 32) & 0xFFF);
    psm = (int)((words[4] >> 24) & 0x3F);

    switch (psm) {
    case 0x00:
    case 0x30:
        len = (w * h) << 2;
        rem = len & 0xF;
        n1 = (len >> 4) & 7;
        n8 = (len >> 4) & 0xFFFFFFF8;
        if (rem == 0) {
            goto same_height;
        }
        nh = (h + 3) & 0x1FFC;
        rest = (w * nh) >> 2;
        rest = rest - n8 - n1;
        goto pad;
    case 0x01:
    case 0x31:
        len = (w * h) * 3;
        rem = len & 0xF;
        n1 = (len >> 4) & 7;
        n8 = (len >> 4) & 0xFFFFFFF8;
        if (rem == 0) {
            goto same_height;
        }
        nh = (h + 15) & 0x1FF0;
        rest = ((w * nh) * 3) >> 4;
        extra = rest - n8 - n1 - 1;
        break;
    case 0x02:
    case 0x0A:
    case 0x32:
    case 0x3A:
        len = (w * h) << 1;
        rem = len & 0xF;
        n1 = (len >> 4) & 7;
        n8 = (len >> 4) & 0xFFFFFFF8;
        if (rem == 0) {
            goto same_height;
        }
        nh = (h + 7) & 0xFFFFFFF8;
        rest = (w * nh) >> 3;
        rest = rest - n8 - n1;
        goto pad;
    case 0x13:
    case 0x1B:
        len = w * h;
        rem = len & 0xF;
        n1 = (len >> 4) & 7;
        n8 = (len >> 4) & 0xFFFFFFF8;
        if (rem == 0) {
            goto same_height;
        }
        nh = (h + 7) & 0xFFFFFFF8;
        rest = (w * nh) >> 4;
        rest = rest - n8 - n1;
        goto pad;
    case 0x14:
    case 0x24:
    case 0x2C:
        len = (w * h) >> 1;
        rem = len & 0xF;
        n1 = (len >> 4) & 7;
        n8 = (len >> 4) & 0xFFFFFFF8;
        if (rem == 0) {
        same_height:
            nh = h;
            extra = 0;
            break;
        }
        nh = (h + 7) & 0xFFFFFFF8;
        rest = (w * nh) >> 5;
        rest = rest - n8 - n1;
    pad:
        extra = rest - 1;
        break;
    default:
        break;
    }

    if (rem != 0) {
        /* TRXREG with the height rounded up, so the GS sends whole qwords */
        words[8] = (int64_t)(int)(words[8] & 0xFFF) | ((int64_t)nh << 32);
    }

    wait_channel(1);
    /* (GsPutIMR: the FINISH interrupt masked) */
    clear_finish();
    *D1_QWC = 7;
    *D1_MADR = (int)dma_address(pkt, store_image_copy, sizeof(store_image_copy), 0x70);
    start_channel(D1_CHCR, 0x101);
    wait_channel(1);
    /* the packet's FINISH has been through the GS */
    while (!(*GS_CSR & GS_CSR_FINISH)) {
    }

    *VIF1_STAT = 0x800000;
    *GS_BUSDIR = 1;

    if (n8 != 0) {
        EEWord word;

        /* into an image off the stack by way of a copy */
        if (ee_word_try(img, &word) || (size_t)n8 * 16 > sizeof(image_copy)) {
            *D1_MADR = (int)dma_address(img, NULL, 0, (size_t)n8 * 16);
        } else {
            wait_channel(2); /* (the copy may still be going to the GS) */
            *D1_MADR = (int)dma_address(image_copy, NULL, 0, 0);
        }
        *D1_QWC = n8;
        start_channel(D1_CHCR, 0x100);
        wait_channel(1);
        if (!ee_word_try(img, &word)) {
            memcpy(img, image_copy, (size_t)n8 * 16);
        }
    }

    /* the rest through the VIF1 FIFO, a qword at a time */
    for (j = 0; j < n1; j++) {
        read.dst = (char *)img + (size_t)(n8 + j) * 16;
        read.qwords = 1;
        port_dmac_call(read_fifo, &read);
    }
    if (rem != 0) {
        read.dst = store_image_tail;
        read.qwords = 1;
        port_dmac_call(read_fifo, &read);
        for (j = 0; j < rem; j++) {
            *((char *)img + (n8 + n1) * 16 + j) = (char)store_image_tail[j];
        }
        for (j = 0; j < extra; j++) {
            port_dmac_call(read_fifo, &read);
        }
    }

    *VIF1_STAT = 0;
    *GS_BUSDIR = 0;
    /* (GsPutIMR(imr)) */
    clear_finish();
    port_dmac_call(feed_vif1, sceGsUnmaskPath3Packet);
    return 0;
}

/* ---- graph021, graph022 ----------------------------------------------------------------------- */
static void set_half_offset(void *draw, short x, short y, short half) {
    uint64_t v = *(uint64_t *)((char *)draw + 0x30);
    int64_t a, b, ta, t, hi;

    b = (short)y;
    b -= (uint64_t)((int)((v >> 48) & 0x7FF) + 1) >> 1;
    t = b << 4;
    a = (short)x;
    a -= (uint64_t)((int)((v >> 16) & 0x7FF) + 1) >> 1;
    ta = a << 4;
    if (half != 0) {
        hi = (t + 8) << 32;
    } else {
        hi = b << 36;
    }
    *(int64_t *)((char *)draw + 0x20) = ta | hi;
}

void sceGsSetHalfOffset(void *draw, short x, short y, short half) {
    set_half_offset(draw, x, y, half);
}

void sceGsSetHalfOffset2(void *draw, short x, short y, short half) {
    set_half_offset(draw, x, y, half);
}

/* ---- Save states: the mode, and the copies of packets that DMA channel 2 may still have to send (one
   that waits for PATH3) ---------------------------------------------------------------------------- */
void port_libgraph_state(PortStateIo *io) {
    PORT_STATE(io, gsGParam);
    PORT_STATE(io, put_draw_env_copy);
    PORT_STATE(io, load_image_copy);
    PORT_STATE(io, image_copy);
    PORT_STATE(io, store_image_copy);
    PORT_STATE(io, store_image_tail);
}
