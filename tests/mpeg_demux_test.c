/* The movie demultiplexer (port/sdk/libmpeg.c, sceMpegDemuxPssRing) over sectors the disc could not read: they are
   zeros (port/platform/disc.c), and the packets after them are still handed out and the bytes used, so the movie's
   reader is not given the same bytes again for ever. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libmpeg.h"
#include "port/platform.h"

/* ---- what libmpeg.c and mpeg2.c call in the port, not reached by demultiplexing ---- */
void *ee_hw_ptr(uint32_t ee_address) {
    static uint32_t registers[64];

    (void)ee_address;
    return registers;
}

int port_disc_read(uint32_t lsn, uint32_t sectors, void *buffer) {
    (void)lsn;
    (void)sectors;
    (void)buffer;
    return 0;
}

void port_event(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

void port_fatal(const char *file, int line, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "%s:%d: ", file, line);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    exit(2);
}

int port_ipu_read_qword(uint8_t qword[16]) {
    (void)qword;
    return 0;
}

void port_ipu_set_bp(int unused_bits) {
    (void)unused_bits;
}

void port_todo(const char *what, const char *file, int line) {
    fprintf(stderr, "%s:%d: todo: %s\n", file, line, what);
}

/* ---- the test ---- */
enum { SECTOR = 2048, PACK_HEADER = 14, PES_HEADER = 9, SLACK = 16 };

static int failures;

static void check(const char *name, int ok) {
    printf("%s: %s\n", ok ? "ok" : "FAILED", name);
    failures += !ok;
}

/* one sector: a pack, and in it one video packet (stream 0xE0) filling it, its payload `fill` */
static void write_pack(uint8_t *out, uint8_t fill) {
    static const uint8_t pack[PACK_HEADER] = {0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04,
                                              0x00, 0x04, 0x01, 0x00, 0x00, 0x03, 0xF8};
    unsigned length = SECTOR - PACK_HEADER - 6;

    memcpy(out, pack, PACK_HEADER);
    out += PACK_HEADER;
    out[0] = 0x00;
    out[1] = 0x00;
    out[2] = 0x01;
    out[3] = 0xE0;
    out[4] = (uint8_t)(length >> 8);
    out[5] = (uint8_t)length;
    out[6] = 0x80; /* '10', not scrambled */
    out[7] = 0x00; /* no time stamps, no flags */
    out[8] = 0x00; /* no more header */
    memset(out + PES_HEADER, fill, SECTOR - PACK_HEADER - PES_HEADER);
}

typedef struct {
    int packets;
    uint8_t fills[8];
} Seen;

static int on_video(sceMpeg *mp, void *cbdata, void *any) {
    const sceMpegCbDataStr *record = cbdata;
    Seen *seen = any;

    (void)mp;
    if (seen->packets < (int)sizeof(seen->fills)) {
        seen->fills[seen->packets] = record->data[0];
    }
    seen->packets++;
    return 1;
}

/* the sectors described by `layout` ('p': a pack, '0': zeros) and `tail` more zero bytes, demultiplexed at once */
static int demux(const char *layout, int tail, Seen *seen) {
    size_t sectors = strlen(layout);
    int size = (int)(sectors * SECTOR) + tail;
    uint8_t *buf = calloc(1, (size_t)size + SLACK); /* (read a little past the end, as the movie's ring allows) */
    sceMpeg mp;
    size_t i;
    int consumed;

    for (i = 0; i < sectors; i++) {
        if (layout[i] == 'p') {
            write_pack(buf + i * SECTOR, (uint8_t)(0xA0 + i));
        }
    }
    memset(seen, 0, sizeof(*seen));
    sceMpegCreate(&mp, NULL, 0);
    sceMpegAddStrCallback(&mp, 0, 0, on_video, seen);
    consumed = sceMpegDemuxPssRing(&mp, buf, size, buf, -1);
    sceMpegDelete(&mp);
    free(buf);
    return consumed;
}

int main(void) {
    Seen seen;
    int consumed;

    consumed = demux("pp", 0, &seen);
    check("whole packs: both handed out, all used", consumed == 2 * SECTOR && seen.packets == 2);

    consumed = demux("p0p", 0, &seen);
    check("a zero sector between packs: the pack after it handed out, all used",
          consumed == 3 * SECTOR && seen.packets == 2 && seen.fills[0] == 0xA0 && seen.fills[1] == 0xA2);

    consumed = demux("0p", 0, &seen);
    check("a zero sector first: used, and the pack after it handed out", consumed == 2 * SECTOR && seen.packets == 1);

    consumed = demux("00", 0, &seen);
    check("only zeros: used but the last three bytes (they may begin a start code)",
          consumed == 2 * SECTOR - 3 && seen.packets == 0);

    consumed = demux("p", 2, &seen);
    check("two zero bytes after a pack: not used (they may begin a start code)",
          consumed == SECTOR && seen.packets == 1);

    return failures ? 1 : 0;
}
