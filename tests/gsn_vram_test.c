/* The CPU model of the GS's memory (port/gs/shared/gsn_vram.c) against paraLLEl-GS as the oracle.

   Both are fed the same random transfers in every pixel format: host to local (as a GIF packet on
   PATH 2 for paraLLEl, in random pieces for the model), local to local, and local to host; after
   each, the whole 4 MB must agree byte for byte (and what a local->host transfer sends must agree).
   Exits 77 (skipped) when there is no Vulkan device paraLLEl-GS can use.

   The transfers are kept to what the GS defines one way: paraLLEl does a transfer's pixels at once
   on the GPU, so a transfer two of whose pixels land on the same byte (a rectangle that runs past
   the buffer's width onto rows of pages it also covers below, or wraps all of VRAM) has no one
   answer there. Those are left out; a rectangle wider than the buffer is tried within one row of
   pages (paraLLEl drops the pixels past the width on every row of pages but the last). Overlapping
   copies are made with the DIR that makes them a move, which is what paraLLEl always does.

   paraLLEl's own view of VRAM (what port_gs_backend_vram maps) is a mirror of the GPU's, refreshed
   only for the pages its tracking says a transfer touched; that tracking misses some (a buffer not
   on a page boundary, a rectangle wrapping at 2048), so before each comparison all of VRAM is
   copied onto itself, which refreshes every page. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gsn_vram.h"
#include "port/platform.h"

int port_gs_backend_open(void);
void port_gs_backend_gif(int path, const void *data, size_t bytes);
void port_gs_backend_read_fifo(void *dst, uint32_t qwords);
const void *port_gs_backend_vram(void);

/* What gs_parallel.cpp needs of the program */
PortOptions port_options;

/* ---- Random numbers --------------------------------------------------------------------------- */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* 0..n-1 */
static uint32_t rnd(uint32_t n) {
    return n ? (uint32_t)(rng() % n) : 0;
}

static void fill_random(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(rng() >> 24);
    }
}

/* ---- GIF packets ------------------------------------------------------------------------------ */
static uint64_t *packet;
static size_t packet_qw, packet_cap;

static void put_qword(uint64_t lo, uint64_t hi) {
    if (packet_qw == packet_cap) {
        packet_cap = packet_cap ? packet_cap * 2 : 4096;
        packet = realloc(packet, packet_cap * 16);
    }
    packet[packet_qw * 2] = lo;
    packet[packet_qw * 2 + 1] = hi;
    packet_qw++;
}

static uint64_t gif_tag(uint32_t nloop, int eop, uint32_t flg, uint32_t nreg) {
    return (uint64_t)nloop | (uint64_t)(eop ? 1 : 0) << 15 | (uint64_t)flg << 58 | (uint64_t)nreg << 60;
}

/* The four registers of a transfer, as A+D, then (for TRXDIR 0) the pixels as IMAGE data */
static void send_transfer(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint32_t trxdir, const uint8_t *data,
                          size_t bytes) {
    size_t qwords = (bytes + 15) / 16;

    packet_qw = 0;
    put_qword(gif_tag(4, qwords == 0, 0, 1), 0xE);
    put_qword(bitbltbuf, 0x50);
    put_qword(trxpos, 0x51);
    put_qword(trxreg, 0x52);
    put_qword(trxdir, 0x53);
    for (size_t done = 0; done < qwords;) {
        size_t n = qwords - done > 32767 ? 32767 : qwords - done;

        put_qword(gif_tag((uint32_t)n, done + n == qwords, 2, 0), 0);
        for (size_t i = 0; i < n; i++) {
            uint64_t q[2] = {0, 0};
            size_t at = (done + i) * 16;

            memcpy(q, data + at, bytes - at < 16 ? bytes - at : 16);
            put_qword(q[0], q[1]);
        }
        done += n;
    }
    port_gs_backend_gif(2, packet, packet_qw * 16);
}

/* ---- The two memories ------------------------------------------------------------------------- */
static uint8_t *model;
static uint8_t *stamp; /* one bit per nibble of VRAM: written by the transfer being made up */
static uint8_t *before; /* the model's VRAM before the transfer */
static int failures;

static const uint32_t all_psms[] = {GSN_PSMCT32, GSN_PSMCT24, GSN_PSMCT16, GSN_PSMCT16S, GSN_PSMT8,
                                    GSN_PSMT4,   GSN_PSMT8H,  GSN_PSMT4HL, GSN_PSMT4HH, GSN_PSMZ32,
                                    GSN_PSMZ24,  GSN_PSMZ16,  GSN_PSMZ16S};
#define PSM_COUNT (sizeof(all_psms) / sizeof(all_psms[0]))

static const char *psm_name(uint32_t psm) {
    switch (psm) {
    case GSN_PSMCT32: return "PSMCT32";
    case GSN_PSMCT24: return "PSMCT24";
    case GSN_PSMCT16: return "PSMCT16";
    case GSN_PSMCT16S: return "PSMCT16S";
    case GSN_PSMT8: return "PSMT8";
    case GSN_PSMT4: return "PSMT4";
    case GSN_PSMT8H: return "PSMT8H";
    case GSN_PSMT4HL: return "PSMT4HL";
    case GSN_PSMT4HH: return "PSMT4HH";
    case GSN_PSMZ32: return "PSMZ32";
    case GSN_PSMZ24: return "PSMZ24";
    case GSN_PSMZ16: return "PSMZ16";
    case GSN_PSMZ16S: return "PSMZ16S";
    }
    return "?";
}

static void (*debug_hook)(const uint8_t *gs);

/* Compares all of VRAM; prints the first differences */
static void sync_all(void);

static int compare_vram(const char *what) {
    const uint8_t *gs;

    sync_all();
    gs = port_gs_backend_vram();
    int shown = 0;

    if (memcmp(gs, model, GSN_VRAM_BYTES) == 0) {
        return 1;
    }
    failures++;
    printf("FAIL %s\n", what);
    if (debug_hook) {
        debug_hook(gs);
    }
    for (uint32_t i = 0; i < GSN_VRAM_BYTES && shown < 8; i++) {
        if (gs[i] != model[i]) {
            printf("  0x%06X (page %u block %u): paraLLEl %02X model %02X\n", i, i >> 13, (i >> 8) & 31, gs[i],
                   model[i]);
            shown++;
        }
    }
    /* take paraLLEl's VRAM, so one difference is not reported again by every later transfer */
    memcpy(model, gs, GSN_VRAM_BYTES);
    return 0;
}

/* ---- Making up transfers ---------------------------------------------------------------------- */
typedef struct {
    uint32_t bp, bw, x, y, w, h;
} Rect;

static int page_w(uint32_t psm) {
    return (psm == GSN_PSMT8 || psm == GSN_PSMT4) ? 128 : 64;
}

static int page_h(uint32_t psm) {
    int bits = gsn_psm_bits(psm);

    if (psm == GSN_PSMT4) {
        return 128;
    }
    return (psm == GSN_PSMT8 || (bits == 16)) ? 64 : 32;
}

/* paraLLEl's page tracking (compute_page_rect) works out a rectangle's blocks in its first page
   from the block coordinates of its far corner taken modulo 32; when that comes out below the near
   corner's (a buffer not on a page boundary, and a rectangle some pages wide), the count of blocks
   underflows and paraLLEl loops for a very long time. Such rectangles are not tried. */
static int parallel_tracks(uint32_t psm, const Rect *r) {
    uint32_t pw = (uint32_t)page_w(psm), ph = (uint32_t)page_h(psm);
    uint32_t bw = psm == GSN_PSMT4 ? 32 : (gsn_psm_bits(psm) == 16 || psm == GSN_PSMT8) ? 16 : 8;
    uint32_t bh = (psm == GSN_PSMT8 || psm == GSN_PSMT4) ? 16 : 8;
    uint32_t x = r->x % pw, y = r->y % ph, w = r->w + x, h = r->h + y;
    int single = (w - 1) / pw == 0 && (h - 1) / ph == 0;

    if (r->bp % 32 == 0 && !single) {
        return 1;
    }
    return ((w - 1) / bw) % 32 >= x / bw && ((h - 1) / bh) % 32 >= y / bh;
}

/* Marks the units a rectangle's pixels are in; 0 if two of them share one (or one is marked
   already, when keep is set) */
static int stamp_rect(uint32_t psm, const Rect *r, int keep) {
    if (!parallel_tracks(psm, r)) {
        return 0;
    }
    if (!keep) {
        memset(stamp, 0, GSN_VRAM_BYTES / 4);
    }
    for (uint32_t j = 0; j < r->h; j++) {
        for (uint32_t i = 0; i < r->w; i++) {
            int nibble;
            uint32_t at = gsn_vram_offset(psm, r->bp, r->bw, (r->x + i) & 2047, (r->y + j) & 2047, &nibble) * 2;

            at += nibble > 0 ? (uint32_t)nibble : 0;
            if (stamp[at >> 3] & (1u << (at & 7))) {
                return 0;
            }
            stamp[at >> 3] |= (uint8_t)(1u << (at & 7));
        }
    }
    return 1;
}

/* A random rectangle of a buffer in format psm, at most max_pixels big. Mostly inside the buffer's
   width; sometimes running past it, within one row of pages. */
static void random_rect(uint32_t psm, Rect *r, uint32_t max_pixels) {
    int small = psm == GSN_PSMT8 || psm == GSN_PSMT4;
    uint32_t ph = (uint32_t)page_h(psm);

    r->bp = rnd(16384);
    r->bw = small ? 2 + rnd(rnd(4) ? 15 : 31) : 1 + rnd(rnd(4) ? 16 : 32); /* PSMT8/T4: bw 1 is 0 pages */
    if (rnd(5) != 0) {
        uint32_t width = small ? (r->bw >> 1) * 128 : r->bw * 64;

        r->x = rnd(width);
        r->w = 1 + (rnd(3) == 0 ? rnd(8) : rnd(width - r->x));
        if (r->w > width - r->x) {
            r->w = width - r->x;
        }
        r->y = rnd(2048);
        r->h = 1 + (rnd(3) == 0 ? rnd(8) : rnd(max_pixels / r->w > 600 ? 600 : max_pixels / r->w));
    } else {
        r->x = rnd(2048);
        r->w = 1 + rnd(rnd(2) ? 2048 - r->x : 300);
        r->y = rnd(2048 / ph) * ph + rnd(ph);
        r->h = 1 + rnd(ph - (r->y % ph));
    }
}

static uint64_t make_bitbltbuf(uint32_t sbp, uint32_t sbw, uint32_t spsm, uint32_t dbp, uint32_t dbw, uint32_t dpsm) {
    return (uint64_t)sbp | (uint64_t)sbw << 16 | (uint64_t)spsm << 24 | (uint64_t)dbp << 32 | (uint64_t)dbw << 48 |
           (uint64_t)dpsm << 56;
}

static uint64_t make_trxpos(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t dir) {
    return (uint64_t)sx | (uint64_t)sy << 16 | (uint64_t)dx << 32 | (uint64_t)dy << 48 | (uint64_t)dir << 59;
}

static uint64_t make_trxreg(uint32_t w, uint32_t h) {
    return (uint64_t)w | (uint64_t)h << 32;
}

/* All of VRAM copied onto itself: changes nothing, but has paraLLEl take every page as written */
static void sync_all(void) {
    send_transfer(make_bitbltbuf(0, 16, 0, 0, 16, 0), 0, make_trxreg(1024, 1024), 2, NULL, 0);
}

static uint8_t *data_buf;
#define MAX_DATA (2u << 20)

/* One host->local transfer to both: the model is fed in random pieces */
static void upload_both(uint32_t psm, const Rect *r, const uint8_t *data, size_t bytes) {
    uint64_t bitbltbuf = make_bitbltbuf(0, 0, 0, r->bp, r->bw, psm);
    uint64_t trxpos = make_trxpos(0, 0, r->x, r->y, 0);
    uint64_t trxreg = make_trxreg(r->w, r->h);
    GsnUpload u;
    size_t at = 0;
    uint32_t lo = 512, hi = 0;
    int changed = 0;

    send_transfer(bitbltbuf, trxpos, trxreg, 0, data, bytes);
    memcpy(before, model, GSN_VRAM_BYTES);
    gsn_upload_begin(&u, bitbltbuf, trxpos, trxreg);
    while (at < bytes) {
        size_t n = rnd(4) == 0 ? bytes - at : 1 + rnd(rnd(2) ? 40 : 4000);
        uint32_t piece_lo, piece_hi;

        if (n > bytes - at) {
            n = bytes - at;
        }
        changed |= gsn_upload_data(&u, model, data + at, n, &piece_lo, &piece_hi);
        if (piece_lo <= piece_hi) {
            lo = piece_lo < lo ? piece_lo : lo;
            hi = piece_hi > hi ? piece_hi : hi;
        }
        at += n;
    }
    if (u.active) {
        printf("FAIL %s upload still waits for pixels after all of them\n", psm_name(psm));
        failures++;
    }
    /* what gsn_upload_data says it changed, and where */
    if (changed != (memcmp(before, model, GSN_VRAM_BYTES) != 0)) {
        printf("FAIL %s upload says it %s VRAM\n", psm_name(psm), changed ? "changed" : "did not change");
        failures++;
    }
    for (uint32_t i = 0; i < GSN_VRAM_BYTES; i += 8192) {
        if ((i >> 13 < lo || i >> 13 > hi) && memcmp(before + i, model + i, 8192) != 0) {
            printf("FAIL %s upload changed page %u outside the pages it gave, %u..%u\n", psm_name(psm), i >> 13, lo,
                   hi);
            failures++;
            break;
        }
    }
}

static uint32_t dbg_psm;
static Rect dbg_rect;

static void upload_debug(const uint8_t *gs) {
    uint32_t diffs = 0, first = 0xFFFFFFFF, inside = 0;
    const uint8_t *again;

    for (uint32_t i = 0; i < GSN_VRAM_BYTES; i++) {
        if (gs[i] != model[i]) {
            if (!diffs) {
                first = i;
            }
            diffs++;
        }
    }
    for (uint32_t j = 0; j < dbg_rect.h; j++) {
        for (uint32_t i = 0; i < dbg_rect.w; i++) {
            uint32_t at = gsn_vram_offset(dbg_psm, dbg_rect.bp, dbg_rect.bw, (dbg_rect.x + i) & 2047,
                                          (dbg_rect.y + j) & 2047, NULL);
            if (at <= first && first < at + 4) {
                printf("  first difference is pixel (%u,%u) of the rectangle\n", i, j);
                inside = 1;
            }
        }
    }
    again = port_gs_backend_vram();
    printf("  %u bytes differ; first %s the rectangle; a second read %s\n", diffs, inside ? "in" : "NOT in",
           memcmp(again, gs, GSN_VRAM_BYTES) ? "differs" : "is the same");
}

static int test_uploads(uint32_t psm, int count) {
    int bits = gsn_psm_bits(psm);
    char what[256];

    for (int t = 0; t < count; t++) {
        Rect r;
        size_t bytes;

        do {
            random_rect(psm, &r, (MAX_DATA * 8 / (uint32_t)bits) / 4);
            /* paraLLEl waits for (w*h*bits)/64 doublewords; with none, it writes nothing */
        } while ((uint64_t)r.w * r.h * (uint32_t)bits < 64 || !stamp_rect(psm, &r, 0));
        bytes = ((size_t)r.w * r.h * (size_t)bits + 7) / 8;
        fill_random(data_buf, bytes);
        dbg_psm = psm;
        dbg_rect = r;
        debug_hook = upload_debug;
        upload_both(psm, &r, data_buf, bytes);
        snprintf(what, sizeof(what), "upload %s DBP 0x%X DBW %u DSAX %u DSAY %u RRW %u RRH %u", psm_name(psm), r.bp,
                 r.bw, r.x, r.y, r.w, r.h);
        if (!compare_vram(what) && failures > 20) {
            return 0;
        }
    }
    return 1;
}

static Rect dbg_src;

static void copy_debug(const uint8_t *gs) {
    uint32_t diffs = 0, first = 0xFFFFFFFF;

    for (uint32_t i = 0; i < GSN_VRAM_BYTES; i++) {
        if (gs[i] != model[i]) {
            if (!diffs) {
                first = i;
            }
            diffs++;
        }
    }
    printf("  %u bytes differ\n", diffs);
    for (uint32_t j = 0; j < dbg_rect.h; j++) {
        for (uint32_t i = 0; i < dbg_rect.w; i++) {
            uint32_t at = gsn_vram_offset(dbg_psm, dbg_rect.bp, dbg_rect.bw, dbg_rect.x + i, dbg_rect.y + j, NULL);
            if (at <= first && first < at + 4) {
                uint32_t sx = dbg_src.x + i, sy = dbg_src.y + j;
                uint32_t want = gsn_vram_read(before, dbg_psm, dbg_src.bp, dbg_src.bw, sx, sy);
                uint32_t got = gsn_vram_read(gs, dbg_psm, dbg_rect.bp, dbg_rect.bw, dbg_rect.x + i, dbg_rect.y + j);
                uint32_t old = gsn_vram_read(before, dbg_psm, dbg_rect.bp, dbg_rect.bw, dbg_rect.x + i, dbg_rect.y + j);

                printf("  first difference: destination pixel (%u,%u): source %08X, paraLLEl %08X, old %08X\n", i, j,
                       want, got, old);
                for (uint32_t b = 0; b < dbg_src.h; b++) {
                    for (uint32_t a = 0; a < dbg_src.w; a++) {
                        if (gsn_vram_read(before, dbg_psm, dbg_src.bp, dbg_src.bw, dbg_src.x + a, dbg_src.y + b) == got) {
                            printf("  paraLLEl's value is source pixel (%u,%u)\n", a, b);
                        }
                    }
                }
                if (gsn_psm_bits(dbg_psm) == 32) {
                    uint32_t fifo[4];

                    send_transfer(make_bitbltbuf(dbg_rect.bp, dbg_rect.bw, dbg_psm, 0, 0, 0),
                                  make_trxpos(dbg_rect.x + i, dbg_rect.y + j, 0, 0, 0), make_trxreg(4, 1), 1, NULL, 0);
                    port_gs_backend_read_fifo(fifo, 1);
                    printf("  paraLLEl's FIFO read of it: %08X\n", fifo[0]);
                }
                return;
            }
        }
    }
    printf("  first difference 0x%X is not in the destination\n", first);
}

static int test_copies(uint32_t psm, int count) {
    char what[256];

    for (int t = 0; t < count; t++) {
        Rect s, d;
        uint32_t dir = 0;
        uint64_t bitbltbuf, trxpos, trxreg;

        for (;;) {
            random_rect(psm, &s, 64 * 1024);
            if (rnd(3) == 0 && s.w > 2 && s.h > 2) {
                /* an overlapping copy within one buffer, in the order that makes it a move */
                int dx = (int)rnd(17) - 8, dy = (int)rnd(17) - 8;
                uint32_t width = (psm == GSN_PSMT8 || psm == GSN_PSMT4) ? (s.bw >> 1) * 128 : s.bw * 64;

                d = s;
                if ((int)s.x + dx < 0 || s.x + s.w + (uint32_t)(dx > 0 ? dx : 0) > width || (int)s.y + dy < 0 ||
                    s.y + s.h + 8 > 2048) {
                    continue;
                }
                d.x = (uint32_t)((int)s.x + dx);
                d.y = (uint32_t)((int)s.y + dy);
                dir = (dy > 0 ? 1u : 0u) | (dx > 0 ? 2u : 0u);
                if (!stamp_rect(psm, &s, 0) || !stamp_rect(psm, &d, 0)) {
                    continue;
                }
            } else {
                random_rect(psm, &d, 64 * 1024);
                d.w = s.w;
                d.h = s.h;
                if (d.x + d.w > 2048) {
                    d.x = 2048 - d.w;
                }
                if (d.y + d.h > 2048) {
                    d.y = 2048 - d.h;
                }
                /* source and destination apart: then the order does not matter */
                if (!stamp_rect(psm, &s, 0) || !stamp_rect(psm, &d, 0) || !stamp_rect(psm, &s, 1)) {
                    continue;
                }
                /* the destination must keep to the buffer as a host->local one would */
                {
                    uint32_t width = (psm == GSN_PSMT8 || psm == GSN_PSMT4) ? (d.bw >> 1) * 128 : d.bw * 64;
                    uint32_t ph = (uint32_t)page_h(psm);

                    if (d.x + d.w > width && (d.y / ph != (d.y + d.h - 1) / ph)) {
                        continue;
                    }
                }
                dir = rnd(4);
            }
            break;
        }
        bitbltbuf = make_bitbltbuf(s.bp, s.bw, psm, d.bp, d.bw, psm);
        trxpos = make_trxpos(s.x, s.y, d.x, d.y, dir);
        trxreg = make_trxreg(s.w, s.h);
        if (getenv("GSN_VERBOSE")) {
            printf("\n copy SBP 0x%X SBW %u (%u,%u) -> DBP 0x%X DBW %u (%u,%u) %ux%u DIR %u", s.bp, s.bw, s.x, s.y,
                   d.bp, d.bw, d.x, d.y, s.w, s.h, dir);
        }
        memcpy(before, model, GSN_VRAM_BYTES);
        dbg_psm = psm;
        dbg_rect = d;
        dbg_src = s;
        debug_hook = copy_debug;
        send_transfer(bitbltbuf, trxpos, trxreg, 2, NULL, 0);
        if (gsn_vram_copy(model, bitbltbuf, trxpos, trxreg) != (memcmp(before, model, GSN_VRAM_BYTES) != 0)) {
            printf("FAIL copy %s: what gsn_vram_copy says it changed\n", psm_name(psm));
            failures++;
        }
        snprintf(what, sizeof(what), "copy %s SBP 0x%X SBW %u (%u,%u) -> DBP 0x%X DBW %u (%u,%u) %ux%u DIR %u",
                 psm_name(psm), s.bp, s.bw, s.x, s.y, d.bp, d.bw, d.x, d.y, s.w, s.h, dir);
        if (!compare_vram(what) && failures > 20) {
            return 0;
        }
    }
    return 1;
}

static int test_reads(uint32_t psm, int count) {
    int bits = gsn_psm_bits(psm);
    static uint8_t gs_out[1 << 20], model_out[1 << 20];

    for (int t = 0; t < count; t++) {
        Rect r;
        uint64_t bitbltbuf, trxpos, trxreg;
        size_t qwords;

        do {
            random_rect(psm, &r, (sizeof(gs_out) * 8 / (uint32_t)bits) / 2);
            qwords = (size_t)r.w * r.h * (size_t)bits / 128; /* paraLLEl sends whole qwords only */
        } while (qwords == 0 || !parallel_tracks(psm, &r));
        bitbltbuf = make_bitbltbuf(r.bp, r.bw, psm, 0, 0, 0);
        trxpos = make_trxpos(r.x, r.y, 0, 0, 0);
        trxreg = make_trxreg(r.w, r.h);
        send_transfer(bitbltbuf, trxpos, trxreg, 1, NULL, 0);
        port_gs_backend_read_fifo(gs_out, (uint32_t)qwords);
        gsn_vram_read_image(model, bitbltbuf, trxpos, trxreg, model_out, qwords);
        if (memcmp(gs_out, model_out, qwords * 16) != 0) {
            size_t i = 0;

            while (gs_out[i] == model_out[i]) {
                i++;
            }
            printf("FAIL read %s SBP 0x%X SBW %u SSAX %u SSAY %u RRW %u RRH %u: byte %u paraLLEl %02X model %02X\n",
                   psm_name(psm), r.bp, r.bw, r.x, r.y, r.w, r.h, (unsigned)i, gs_out[i], model_out[i]);
            if (++failures > 20) {
                return 0;
            }
        }
    }
    return 1;
}

/* gsn_vram_read gives back what gsn_vram_write put, as many bits as the format has */
static void test_round_trip(void) {
    for (size_t p = 0; p < PSM_COUNT; p++) {
        uint32_t psm = all_psms[p];
        int bits = gsn_psm_bits(psm);
        uint32_t mask = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;

        for (int t = 0; t < 20000; t++) {
            uint32_t bp = rnd(16384), bw = rnd(64), x = rnd(2048), y = rnd(2048), v = (uint32_t)rng();

            gsn_vram_write(model, psm, bp, bw, x, y, v);
            if (gsn_vram_read(model, psm, bp, bw, x, y) != (v & mask)) {
                printf("FAIL round trip %s bp 0x%X bw %u (%u,%u)\n", psm_name(psm), bp, bw, x, y);
                failures++;
                break;
            }
        }
    }
}

/* gsn_vram_read_row and gsn_vram_write_row do what gsn_vram_read and gsn_vram_write do, pixel by pixel */
static void test_rows(void) {
    static uint32_t row[2048], one[2048];

    for (size_t p = 0; p < PSM_COUNT; p++) {
        uint32_t psm = all_psms[p];

        for (int t = 0; t < 200; t++) {
            uint32_t bp = rnd(16384), bw = rnd(64), x = rnd(2048), y = rnd(2048), count = 1 + rnd(2048);

            for (uint32_t i = 0; i < count; i++) {
                row[i] = (uint32_t)rng();
            }
            gsn_vram_write_row(model, psm, bp, bw, x, y, count, row);
            memcpy(before, model, GSN_VRAM_BYTES);
            for (uint32_t i = 0; i < count; i++) {
                gsn_vram_write(before, psm, bp, bw, x + i, y, row[i]);
            }
            gsn_vram_read_row(model, psm, bp, bw, x, y, count, row);
            for (uint32_t i = 0; i < count; i++) {
                one[i] = gsn_vram_read(model, psm, bp, bw, x + i, y);
            }
            if (memcmp(before, model, GSN_VRAM_BYTES) != 0 || memcmp(row, one, count * 4) != 0) {
                printf("FAIL rows %s bp 0x%X bw %u (%u,%u) x %u\n", psm_name(psm), bp, bw, x, y, count);
                failures++;
                break;
            }
        }
    }
}

/* gsn_upload_data's speed, for a 1024x1024 PSMCT32 picture */
static void measure_upload(void) {
    uint8_t *vram = calloc(GSN_VRAM_BYTES, 1);
    uint64_t bitbltbuf = make_bitbltbuf(0, 0, 0, 0, 16, GSN_PSMCT32);
    uint64_t trxreg = make_trxreg(1024, 1024);
    clock_t start = clock();
    int rounds = 0;
    double seconds;

    fill_random(data_buf, GSN_VRAM_BYTES / 2);
    do {
        for (int half = 0; half < 2; half++) {
            GsnUpload u;

            gsn_upload_begin(&u, bitbltbuf, 0, trxreg);
            gsn_upload_data(&u, vram, data_buf, GSN_VRAM_BYTES / 2, NULL, NULL);
            gsn_upload_data(&u, vram, data_buf + (half ? 1 : 0), GSN_VRAM_BYTES / 2, NULL, NULL);
        }
        rounds++;
        seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    } while (seconds < 0.5);
    printf("gsn_upload_data PSMCT32: %.0f MB/s\n", rounds * 2 * 4.0 / seconds);
    free(vram);
}

/* gsn_vram_test [rounds [seed]]: rounds scales the number of transfers (1 by default) */
int main(int argc, char **argv) {
    clock_t start = clock();
    int rounds = argc > 1 ? atoi(argv[1]) : 1;
    int uploads = 300 * rounds, copies = 120 * rounds, reads = 60 * rounds;

    if (argc > 2) {
        rng_state = strtoull(argv[2], NULL, 0) | 1;
    }

    model = calloc(GSN_VRAM_BYTES, 1);
    stamp = calloc(GSN_VRAM_BYTES / 4, 1);
    before = calloc(GSN_VRAM_BYTES, 1);
    data_buf = malloc(MAX_DATA + 16);

    test_round_trip();
    test_rows();
    measure_upload();

    if (!port_gs_backend_open()) {
        printf("no Vulkan device for paraLLEl-GS: skipped\n");
        return 77;
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    /* all of VRAM random, in both: 1024x1024 PSMCT32 pixels, in two halves */
    {
        Rect half = {0, 16, 0, 0, 1024, 512};

        memcpy(model, port_gs_backend_vram(), GSN_VRAM_BYTES);
        fill_random(data_buf, GSN_VRAM_BYTES / 2);
        upload_both(GSN_PSMCT32, &half, data_buf, GSN_VRAM_BYTES / 2);
        half.y = 512;
        fill_random(data_buf, GSN_VRAM_BYTES / 2);
        upload_both(GSN_PSMCT32, &half, data_buf, GSN_VRAM_BYTES / 2);
        compare_vram("filling VRAM");
    }

    for (size_t p = 0; p < PSM_COUNT && failures <= 20; p++) {
        uint32_t psm = all_psms[p];

        printf("%s: uploads", psm_name(psm));
        if (!test_uploads(psm, uploads)) {
            break;
        }
        printf(", copies");
        if (!test_copies(psm, copies)) {
            break;
        }
        /* paraLLEl does not send 4-bit formats to the host */
        if (gsn_psm_bits(psm) != 4) {
            printf(", reads");
            if (!test_reads(psm, reads)) {
                break;
            }
        }
        printf("\n");
    }
    printf("%s, %.1f s\n", failures ? "FAILED" : "all agree", (double)(clock() - start) / CLOCKS_PER_SEC);
    return failures ? 1 : 0;
}
