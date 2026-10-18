/* The Sony libraries that step 1 leaves as stand-ins: libgraph and libdev until graphics (step 2),
   libmpeg until movies, libsdr until sound (step 4). Each does nothing, or the least that keeps the
   game going, and logs once that it was reached. libgraph's two waits are real: sceGsSyncV waits for
   the vblank (port/platform/kernel.c) and sceGsSyncPath for the DMA controller (port/platform/dmac.c).
   The rest of the SDK is in port/sdk: libcdvd.c, libdma.c, libmc.c, libpad.c and sif.c, and the
   kernel in port/platform/kernel.c. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eeregs.h"
#include "libdev.h"
#include "libgraph.h"
#include "libmpeg.h"
#include "libsdr.h"
#include "port/host.h"
#include "port/platform.h"

#define STUB(name) PORT_TODO(name " (stub)")

/* ---- libgraph ------------------------------------------------------------------------------- */
static sceGsGParam gs_param = {1, 2, 0, 0, 0, 0};
sceGsGParam *sceGsGetGParam(void) { return &gs_param; }
void sceGsResetGraph(short mode, short inter, short omode, short ffmd) { (void)mode; gs_param.inter = inter; gs_param.omode = omode; gs_param.ffmd = ffmd; }
void sceGsResetPath(void) {}
void sceGsPutDispEnv(void *disp) { (void)disp; STUB("sceGsPutDispEnv"); }
int sceGsPutDrawEnv(void *pkt) { (void)pkt; STUB("sceGsPutDrawEnv"); return 0; }
void sceGsSetDefDispEnv(sceGsDispEnv *disp, short psm, short w, short h, short dx, short dy) { (void)psm; (void)w; (void)h; (void)dx; (void)dy; memset(disp, 0, sizeof(*disp)); STUB("sceGsSetDefDispEnv"); }
int sceGsSetDefDrawEnv(void *env, short psm, short w, short h, short ztst, short zpsm) { (void)env; (void)psm; (void)w; (void)h; (void)ztst; (void)zpsm; STUB("sceGsSetDefDrawEnv"); return 8; }
int sceGsSetDefClear(sceGsClear *cl, short ztst, short x, short y, short w, short h, unsigned char r, unsigned char g, unsigned char b, unsigned char a, unsigned int z) { (void)cl; (void)ztst; (void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b; (void)a; (void)z; STUB("sceGsSetDefClear"); return 6; }
void sceGsSetDefDBuff(void *db, short psm, short w, short h, short ztst, short zpsm, short flag) { (void)db; (void)psm; (void)w; (void)h; (void)ztst; (void)zpsm; (void)flag; STUB("sceGsSetDefDBuff"); }
void sceGsSetDefDBuffDc(void *db, short psm, short w, short h, short ztst, short zpsm, short flag) { (void)db; (void)psm; (void)w; (void)h; (void)ztst; (void)zpsm; (void)flag; STUB("sceGsSetDefDBuffDc"); }
int sceGsSetDefLoadImage(void *li, short dbp, short dbw, short dpsm, short dsax, short dsay, short rrw, short rrh) { (void)li; (void)dbp; (void)dbw; (void)dpsm; (void)dsax; (void)dsay; (void)rrw; (void)rrh; STUB("sceGsSetDefLoadImage"); return 0; }
void sceGsSetDefStoreImage(void *si, short sbp, short sbw, short spsm, short ssax, short ssay, short rrw, short rrh) { (void)si; (void)sbp; (void)sbw; (void)spsm; (void)ssax; (void)ssay; (void)rrw; (void)rrh; STUB("sceGsSetDefStoreImage"); }
int sceGsExecLoadImage(void *li, void *image) { (void)li; (void)image; STUB("sceGsExecLoadImage"); return 0; }
int sceGsExecStoreImage(void *si, void *image) { (void)si; (void)image; STUB("sceGsExecStoreImage"); return 0; }
void sceGsSetHalfOffset(void *draw, short x, short y, short half) { (void)draw; (void)x; (void)y; (void)half; }
void sceGsSetHalfOffset2(void *draw, short x, short y, short half) { (void)draw; (void)x; (void)y; (void)half; }
int sceGsSwapDBuff(void *db, int id) { (void)db; (void)id; STUB("sceGsSwapDBuff"); return 0; }
int sceGsSwapDBuffDc(void *db, int id) { (void)db; (void)id; STUB("sceGsSwapDBuffDc"); return 0; }
/* Mode 0 waits until paths 1 to 3 are idle (the VIF1 and GIF channels done) and returns 0; mode 1
   returns nonzero while one is busy. */
int sceGsSyncPath(int mode, unsigned short timeout) {
    (void)timeout;
    if (mode != 0) {
        port_dmac_wake();
        return (*D1_CHCR & 0x100) | (*D2_CHCR & 0x100);
    }
    port_dmac_wait(1);
    port_dmac_wait(2);
    return 0;
}

/* Waits for the next vblank and returns the field it starts (GS_CSR's FIELD bit: 1 for the odd one). */
int sceGsSyncV(int mode) {
    (void)mode;
    port_vsync();
    return (*GS_CSR >> 13) & 1;
}

/* ---- libmpeg: every movie is over at once --------------------------------------------------- */
void sceMpegInit(void) {}
int sceMpegCreate(sceMpeg *mp, void *work_area, int size) { (void)work_area; (void)size; memset(mp, 0, sizeof(*mp)); port_event("movie: none played yet, so it is over at once"); return 1; }
int sceMpegDelete(sceMpeg *mp) { (void)mp; return 0; }
void sceMpegReset(sceMpeg *mp) { (void)mp; }
sceMpegCallback sceMpegAddCallback(sceMpeg *mp, int type, sceMpegCallback func, void *data) { (void)mp; (void)type; (void)data; return func; }
int sceMpegAddStrCallback(sceMpeg *mp, int type, int ch, sceMpegCallback func, void *data) { (void)mp; (void)type; (void)ch; (void)func; (void)data; return 0; }
int sceMpegDemuxPssRing(sceMpeg *mp, void *buf, int size, void *ring, int ring_size) { (void)mp; (void)buf; (void)ring; (void)ring_size; return size; }
int sceMpegGetPicture(sceMpeg *mp, void *rgb32, int mbcount) { (void)mp; (void)rgb32; (void)mbcount; return -1; }
int sceMpegIsEnd(sceMpeg *mp) { (void)mp; return 1; }
int sceMpegIsRefBuffEmpty(sceMpeg *mp) { (void)mp; return 1; }
void sceMpegSetDecodeMode(sceMpeg *mp, int ni, int np, int nb) { (void)mp; (void)ni; (void)np; (void)nb; }

/* ---- libsdr --------------------------------------------------------------------------------- */
int sceSdRemoteInit(void) { return 0; }
int sceSdRemote(int wait, int command, ...) { (void)wait; (void)command; STUB("sceSdRemote (no sound yet)"); return 0; }

/* ---- libdev --------------------------------------------------------------------------------- */
int sceDevVu0Pause(void) { return 0; }
int sceDevVu0SetTBit(void) { return 0; }
int sceDevVu0IsRunning(void) { return 0; }
int sceDevVif0Reset(void) { return 0; }
int sceDevVif1Reset(void) { return 0; }
int sceDevVu0Reset(void) { return 0; }
int sceDevVu1Reset(void) { return 0; }
