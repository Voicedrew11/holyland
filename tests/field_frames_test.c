/* port/platform/field_frames.c: which vblanks' packets are drawn again for the other field. A menu's field
   picture (224 lines, into the buffer not shown) is; the game's first 448-line frame, drawn while the display
   still shows the menu's last field, is not (it ran over the frame buffer below the field shown).

     field_frames_test */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/host.h"
#include "port/platform.h"

void port_fatal(const char *file, int line, const char *format, ...) {
    va_list args;

    fprintf(stderr, "%s:%d: ", file, line);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

enum { FRAME_1 = 0x4C, XYOFFSET_1 = 0x18, SCISSOR_1 = 0x40, PRIM = 0x00 };
enum { MENU_SHOWN_FBP = 70 };

static int failed, checked, sends;

static void count_send(int path, const void *data, size_t bytes) {
    (void)path;
    (void)data;
    (void)bytes;
    sends++;
}

/* The registers as the game leaves them in the menu: field mode, read circuit 2 showing page 70, 640 wide,
   DISPLAY's DH 447 (fields of 224 lines). */
static void menu_display(uint64_t privileged[0xF0 / 8]) {
    memset(privileged, 0, 0xF0);
    privileged[0x00 / 8] = 0x66;
    privileged[0x20 / 8] = 3;
    privileged[0x90 / 8] = (uint64_t)10 << 9 | MENU_SHOWN_FBP;
    privileged[0xA0 / 8] = 0x1bf9ff0183227cULL;
}

/* One vblank's packet: FRAME (640 wide) at fbp, a draw offset, a scissor to last_line when it is not
   negative, then a sprite's two vertices. */
static void keep_picture(uint32_t fbp, int last_line) {
    uint64_t q[2 * 8];
    int n = 0;

    port_field_frames_reset(1);
    q[2] = (uint64_t)10 << 16 | fbp;
    q[3] = FRAME_1;
    q[4] = (uint64_t)0x7200 << 32 | 0x6C00;
    q[5] = XYOFFSET_1;
    n = 2;
    if (last_line >= 0) {
        q[6] = (uint64_t)last_line << 48 | (uint64_t)639 << 16;
        q[7] = SCISSOR_1;
        n++;
    }
    q[2 * (n + 1)] = 6; /* a sprite */
    q[2 * (n + 1) + 1] = PRIM;
    n++;
    q[0] = (uint64_t)n | (uint64_t)1 << 60; /* NLOOP n, PACKED, NREG 1 */
    q[1] = 0xE;                              /* A+D */
    port_field_frames_keep(3, q, (size_t)(n + 1) * 16);
    q[0] = 1 | (uint64_t)1 << 15 | (uint64_t)2 << 60; /* NLOOP 1, EOP, PACKED, NREG 2 */
    q[1] = 0x55;                                     /* XYZ2, XYZ2 */
    q[2] = q[3] = q[4] = q[5] = 0;
    port_field_frames_keep(3, q, 3 * 16);
}

static void check(const char *name, uint32_t fbp, int last_line, int want_replayed) {
    uint64_t privileged[0xF0 / 8];
    uint32_t drawn = 0xFFFFFFFFu;
    int replayed;

    menu_display(privileged);
    keep_picture(fbp, last_line);
    sends = 0;
    replayed = port_field_frames_vsync(privileged, count_send, &drawn);
    checked++;
    if (replayed != want_replayed || (replayed && (drawn != fbp || sends == 0)) || (!replayed && sends != 0)) {
        failed++;
        printf("FAIL %s: replayed %d (wanted %d), drawn %u, %d sends\n", name, replayed, want_replayed, drawn, sends);
    } else {
        printf("ok   %s: replayed %d\n", name, replayed);
    }
}

int main(void) {
    check("a menu field (224 lines, page 0)", 0, 223, 1);
    check("no scissor written", 0, -1, 1);
    check("the game's first frame (448 lines, page 140)", 140, 447, 0);
    check("a field and a line (the Equipment screen)", 0, 224, 1);
    check("past half way to a frame", 0, 336, 0);
    printf("%d of %d failed\n", failed, checked);
    return failed != 0;
}
