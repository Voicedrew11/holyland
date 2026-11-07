/* The GS's local memory on the CPU (gsn_vram.h): the swizzled layout of each pixel format, and the
   GS's image transfers into, inside and out of it.

   Every address is worked out in nibbles (4 bits), the smallest unit a format owns, so that one
   function serves all of them: a page is 16384 nibbles, a block 512, a column 128. For each of the
   five layouts (32-bit, 16-bit, 16-bit "S", 8-bit, 4-bit) a table gives, for each pixel of a page,
   the nibble it starts at inside the page. The block part of that is then added to the base
   pointer, which counts blocks and need not be page aligned, so a buffer can start mid-page. */
#include "gsn_vram.h"

#include <string.h>

#define NIBBLE_MASK ((GSN_VRAM_BYTES * 2u) - 1u)

/* ---- The layouts ------------------------------------------------------------------------------ */
enum { LAYOUT_32, LAYOUT_16, LAYOUT_16S, LAYOUT_8, LAYOUT_4, LAYOUT_COUNT };

/* How a format reads and writes its pixel in the unit it lives in */
enum { KIND_32, KIND_24, KIND_16, KIND_8, KIND_8H, KIND_4, KIND_4HL, KIND_4HH };

/* The blocks of a page, as they lie in it: a block's number is its place in memory. A 32-bit (and
   PSMT8) page is 8 blocks across and 4 down; a 16-bit (and PSMT4) page 4 across and 8 down, in one
   of two orders (PSMCT16S and PSMZ16S keep the top and bottom halves of the page apart). */
static const uint8_t block_order_32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t block_order_16[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t block_order_16s[8][4] = {
    {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};

/* Page and block sizes of each layout, as log2 */
static const struct {
    uint8_t page_w, page_h, block_w, block_h;
} layout_size[LAYOUT_COUNT] = {
    {6, 5, 3, 3}, /* 32-bit: 64x32 pages of 8x8 blocks */
    {6, 6, 4, 3}, /* 16-bit: 64x64 pages of 16x8 blocks */
    {6, 6, 4, 3},
    {7, 6, 4, 4}, /* PSMT8: 128x64 pages of 16x16 blocks */
    {7, 7, 5, 4}, /* PSMT4: 128x128 pages of 32x16 blocks */
};

static uint16_t page_table[LAYOUT_COUNT][16384];
static volatile int tables_ready;

static uint32_t bit(uint32_t v, int n) {
    return (v >> n) & 1u;
}

/* Where pixel (x, y) of a block lies in it, in nibbles. A block is four 64-byte columns, one under
   the other, each two pixel rows tall in the 32- and 16-bit formats and four in the 8- and 4-bit
   ones. Inside a column the pixels are interleaved:
   - 32-bit: the column's 16 words go x0, y0, x1, x2 (two pixels across, then the row below, ...);
   - 16-bit: its 32 halfwords go x3, x0, y0, x1, x2: pixel x and x+8 share a word;
   - 8-bit: its 64 bytes go y1, x3, x0, y0, x1, then x2 flipped by y1 and by the column's parity:
     the four rows are woven together, and every other column swaps its halves;
   - 4-bit: its 128 nibbles go y1, x3, x4, x0, y0, x1, then x2 the same way as the 8-bit. */
static uint32_t in_block(int layout, uint32_t x, uint32_t y) {
    uint32_t column, at;

    switch (layout) {
    case LAYOUT_32:
        column = y >> 1;
        at = bit(x, 0) | bit(y, 0) << 1 | bit(x, 1) << 2 | bit(x, 2) << 3;
        return column * 128 + at * 8;
    case LAYOUT_16:
    case LAYOUT_16S:
        column = y >> 1;
        at = bit(x, 3) | bit(x, 0) << 1 | bit(y, 0) << 2 | bit(x, 1) << 3 | bit(x, 2) << 4;
        return column * 128 + at * 4;
    case LAYOUT_8:
        column = y >> 2;
        at = bit(y, 1) | bit(x, 3) << 1 | bit(x, 0) << 2 | bit(y, 0) << 3 | bit(x, 1) << 4 |
             (bit(x, 2) ^ bit(y, 1) ^ (column & 1)) << 5;
        return column * 128 + at * 2;
    default:
        column = y >> 2;
        at = bit(y, 1) | bit(x, 3) << 1 | bit(x, 4) << 2 | bit(x, 0) << 3 | bit(y, 0) << 4 | bit(x, 1) << 5 |
             (bit(x, 2) ^ bit(y, 1) ^ (column & 1)) << 6;
        return column * 128 + at;
    }
}

static void build_tables(void) {
    for (int l = 0; l < LAYOUT_COUNT; l++) {
        uint32_t pw = 1u << layout_size[l].page_w, ph = 1u << layout_size[l].page_h;
        uint32_t bw = layout_size[l].block_w, bh = layout_size[l].block_h;

        for (uint32_t y = 0; y < ph; y++) {
            for (uint32_t x = 0; x < pw; x++) {
                uint32_t bx = x >> bw, by = y >> bh, block;

                if (l == LAYOUT_32 || l == LAYOUT_8) {
                    block = block_order_32[by][bx];
                } else if (l == LAYOUT_16S) {
                    block = block_order_16s[by][bx];
                } else {
                    block = block_order_16[by][bx];
                }
                page_table[l][(y << layout_size[l].page_w) | x] =
                    (uint16_t)(block * 512 + in_block(l, x & ((1u << bw) - 1), y & ((1u << bh) - 1)));
            }
        }
    }
    tables_ready = 1;
}

/* ---- Addresses -------------------------------------------------------------------------------- */
typedef struct {
    const uint16_t *table;
    uint32_t page_w, page_h; /* log2 */
    uint32_t stride;         /* pages from one row of pages to the next */
    uint32_t bp;
    uint32_t z_xor;          /* the Z formats' block arrangement */
    int kind;
} Layout;

static int psm_layout(uint32_t psm, int *kind, uint32_t *z_xor) {
    *z_xor = 0;
    switch (psm) {
    case GSN_PSMZ32: *z_xor = 0x18; /* fall through */
    case GSN_PSMCT32: *kind = KIND_32; return LAYOUT_32;
    case GSN_PSMZ24: *z_xor = 0x18; /* fall through */
    case GSN_PSMCT24: *kind = KIND_24; return LAYOUT_32;
    case GSN_PSMT8H: *kind = KIND_8H; return LAYOUT_32;
    case GSN_PSMT4HL: *kind = KIND_4HL; return LAYOUT_32;
    case GSN_PSMT4HH: *kind = KIND_4HH; return LAYOUT_32;
    case GSN_PSMZ16: *z_xor = 0x18; /* fall through */
    case GSN_PSMCT16: *kind = KIND_16; return LAYOUT_16;
    case GSN_PSMZ16S: *z_xor = 0x18; /* fall through */
    case GSN_PSMCT16S: *kind = KIND_16; return LAYOUT_16S;
    case GSN_PSMT8: *kind = KIND_8; return LAYOUT_8;
    case GSN_PSMT4: *kind = KIND_4; return LAYOUT_4;
    }
    return -1;
}

/* A buffer's layout. An unknown PSM is taken as PSMCT32. The Z formats' blocks are the colour
   formats' with the two top bits of the block number flipped, after the base pointer is added: a
   Z buffer that does not start on a page boundary is not the colour buffer's blocks moved. The 8-
   and 4-bit formats' pages are 128 pixels wide, so their row of pages is bw/2 pages long. */
static void layout_init(Layout *l, uint32_t psm, uint32_t bp, uint32_t bw) {
    int n;

    if (!tables_ready) {
        build_tables();
    }
    n = psm_layout(psm, &l->kind, &l->z_xor);
    if (n < 0) {
        n = psm_layout(GSN_PSMCT32, &l->kind, &l->z_xor);
    }
    l->table = page_table[n];
    l->page_w = layout_size[n].page_w;
    l->page_h = layout_size[n].page_h;
    l->stride = (n == LAYOUT_8 || n == LAYOUT_4) ? bw >> 1 : bw;
    l->bp = bp;
}

/* The nibble a pixel starts at. Past bw*64 a row runs on into the next pages, which belong to the
   next row of pages; and the whole wraps at 4 MB. */
static inline uint32_t pixel_nibble(const Layout *l, uint32_t x, uint32_t y) {
    uint32_t page = (y >> l->page_h) * l->stride + (x >> l->page_w);
    uint32_t in = l->table[((y & ((1u << l->page_h) - 1)) << l->page_w) | (x & ((1u << l->page_w) - 1))];
    uint32_t block = page * 32 + ((l->bp + (in >> 9)) ^ l->z_xor);

    return (block * 512 + (in & 511)) & NIBBLE_MASK;
}

static inline uint32_t load32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline uint32_t pixel_get(const uint8_t *vram, int kind, uint32_t nib) {
    const uint8_t *p = vram + (nib >> 1);

    switch (kind) {
    case KIND_32: return load32(p);
    case KIND_24: return load32(p) & 0xFFFFFFu;
    case KIND_16: return (uint32_t)p[0] | (uint32_t)p[1] << 8;
    case KIND_8: return p[0];
    case KIND_8H: return p[3];
    case KIND_4: return (p[0] >> ((nib & 1) * 4)) & 0xF;
    case KIND_4HL: return p[3] & 0xF;
    default: return p[3] >> 4;
    }
}

/* Writes a pixel; returns 1 if that changed VRAM */
static inline int pixel_put(uint8_t *vram, int kind, uint32_t nib, uint32_t v) {
    uint8_t *p = vram + (nib >> 1);
    uint8_t b;

    switch (kind) {
    case KIND_32:
        if (load32(p) == v) {
            return 0;
        }
        memcpy(p, &v, 4);
        return 1;
    case KIND_24:
        if (((load32(p) ^ v) & 0xFFFFFFu) == 0) {
            return 0;
        }
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        return 1;
    case KIND_16:
        if (p[0] == (uint8_t)v && p[1] == (uint8_t)(v >> 8)) {
            return 0;
        }
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        return 1;
    case KIND_8:
        b = (uint8_t)v;
        break;
    case KIND_8H:
        p += 3;
        b = (uint8_t)v;
        break;
    case KIND_4:
        b = (nib & 1) ? (uint8_t)((p[0] & 0x0F) | (v & 0xF) << 4) : (uint8_t)((p[0] & 0xF0) | (v & 0xF));
        break;
    case KIND_4HL:
        p += 3;
        b = (uint8_t)((p[0] & 0xF0) | (v & 0xF));
        break;
    default:
        p += 3;
        b = (uint8_t)((p[0] & 0x0F) | (v & 0xF) << 4);
        break;
    }
    if (*p == b) {
        return 0;
    }
    *p = b;
    return 1;
}

/* ---- Pixels ----------------------------------------------------------------------------------- */
int gsn_psm_bits(uint32_t psm) {
    switch (psm) {
    case GSN_PSMCT32: case GSN_PSMZ32: return 32;
    case GSN_PSMCT24: case GSN_PSMZ24: return 24;
    case GSN_PSMCT16: case GSN_PSMCT16S: case GSN_PSMZ16: case GSN_PSMZ16S: return 16;
    case GSN_PSMT8: case GSN_PSMT8H: return 8;
    case GSN_PSMT4: case GSN_PSMT4HL: case GSN_PSMT4HH: return 4;
    }
    return 0;
}

int gsn_psm_valid(uint32_t psm) {
    return gsn_psm_bits(psm) != 0;
}

uint32_t gsn_vram_read(const uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) {
    Layout l;

    layout_init(&l, psm, bp, bw);
    return pixel_get(vram, l.kind, pixel_nibble(&l, x, y));
}

void gsn_vram_write(uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) {
    Layout l;

    layout_init(&l, psm, bp, bw);
    pixel_put(vram, l.kind, pixel_nibble(&l, x, y), value);
}

static inline __attribute__((always_inline)) void read_row_as(const Layout *l, const uint8_t *vram, int kind, uint32_t x,
                                                               uint32_t y, uint32_t count, uint32_t *out) {
    for (uint32_t i = 0; i < count; i++) {
        out[i] = pixel_get(vram, kind, pixel_nibble(l, x + i, y));
    }
}

void gsn_vram_read_row(const uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                       uint32_t count, uint32_t *out) {
    Layout l;

    layout_init(&l, psm, bp, bw);
    switch (l.kind) {
    case KIND_32: read_row_as(&l, vram, KIND_32, x, y, count, out); break;
    case KIND_16: read_row_as(&l, vram, KIND_16, x, y, count, out); break;
    case KIND_8: read_row_as(&l, vram, KIND_8, x, y, count, out); break;
    case KIND_4: read_row_as(&l, vram, KIND_4, x, y, count, out); break;
    default: read_row_as(&l, vram, l.kind, x, y, count, out); break;
    }
}

void gsn_vram_write_row(uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                        uint32_t count, const uint32_t *in) {
    Layout l;

    layout_init(&l, psm, bp, bw);
    for (uint32_t i = 0; i < count; i++) {
        pixel_put(vram, l.kind, pixel_nibble(&l, x + i, y), in[i]);
    }
}

uint32_t gsn_vram_offset(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, int *nibble) {
    Layout l;
    uint32_t nib;
    int n = -1;

    layout_init(&l, psm, bp, bw);
    nib = pixel_nibble(&l, x, y);
    if (l.kind == KIND_4) {
        n = (int)(nib & 1);
    } else if (l.kind == KIND_4HL) {
        n = 6;
    } else if (l.kind == KIND_4HH) {
        n = 7;
    }
    if (nibble) {
        *nibble = n;
    }
    return nib >> 1;
}

/* ---- Transfers -------------------------------------------------------------------------------- */
/* The registers' fields */
#define FIELD(v, at, bits) ((uint32_t)((v) >> (at)) & ((1u << (bits)) - 1))

void gsn_upload_begin(GsnUpload *u, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg) {
    memset(u, 0, sizeof(*u));
    u->bp = FIELD(bitbltbuf, 32, 14);
    u->bw = FIELD(bitbltbuf, 48, 6);
    u->psm = FIELD(bitbltbuf, 56, 6);
    u->x0 = FIELD(trxpos, 32, 11);
    u->y0 = FIELD(trxpos, 48, 11);
    u->w = FIELD(trxreg, 0, 12);
    u->h = FIELD(trxreg, 32, 12);
    u->active = u->w != 0 && u->h != 0 && gsn_psm_valid(u->psm);
}

/* Writes `count` pixels of one row of the rectangle, from relative x `rx` on, taken from src: whole
   bytes for 8 bits and up, or nibbles from nibble `first` of src for 4 bits. Keeps the lowest and
   highest page written to. (write_run below has it made for each common format on its own, with
   `bits` and `kind` known.) */
static inline __attribute__((always_inline)) int write_run_as(const Layout *l, uint8_t *vram, uint32_t x0, uint32_t y,
                                                              uint32_t rx, uint32_t count, const uint8_t *src,
                                                              uint32_t first, int bits, int kind, uint32_t *page_lo,
                                                              uint32_t *page_hi) {
    uint32_t page_row = (y >> l->page_h) * l->stride;
    const uint16_t *row = l->table + ((y & ((1u << l->page_h) - 1)) << l->page_w);
    uint32_t xmask = (1u << l->page_w) - 1;
    uint32_t lo = *page_lo, hi = *page_hi;
    int changed = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t x = (x0 + rx + i) & 2047;
        uint32_t in = row[x & xmask];
        uint32_t block = (page_row + (x >> l->page_w)) * 32 + ((l->bp + (in >> 9)) ^ l->z_xor);
        uint32_t nib = (block * 512 + (in & 511)) & NIBBLE_MASK;
        uint32_t v, page = nib >> 14;

        switch (bits) {
        case 32: v = load32(src + i * 4); break;
        case 24: v = src[i * 3] | (uint32_t)src[i * 3 + 1] << 8 | (uint32_t)src[i * 3 + 2] << 16; break;
        case 16: v = src[i * 2] | (uint32_t)src[i * 2 + 1] << 8; break;
        case 8: v = src[i]; break;
        default: v = (src[(first + i) >> 1] >> (((first + i) & 1) * 4)) & 0xF; break;
        }
        changed |= pixel_put(vram, kind, nib, v);
        if (page < lo) {
            lo = page;
        }
        if (page > hi) {
            hi = page;
        }
    }
    *page_lo = lo;
    *page_hi = hi;
    return changed;
}

static int write_run(const Layout *l, uint8_t *vram, uint32_t x0, uint32_t y, uint32_t rx, uint32_t count,
                     const uint8_t *src, uint32_t first, int bits, uint32_t *page_lo, uint32_t *page_hi) {
    if (bits == 32 && l->kind == KIND_32) {
        return write_run_as(l, vram, x0, y, rx, count, src, first, 32, KIND_32, page_lo, page_hi);
    }
    if (bits == 24 && l->kind == KIND_24) {
        return write_run_as(l, vram, x0, y, rx, count, src, first, 24, KIND_24, page_lo, page_hi);
    }
    if (bits == 16 && l->kind == KIND_16) {
        return write_run_as(l, vram, x0, y, rx, count, src, first, 16, KIND_16, page_lo, page_hi);
    }
    if (bits == 8 && l->kind == KIND_8) {
        return write_run_as(l, vram, x0, y, rx, count, src, first, 8, KIND_8, page_lo, page_hi);
    }
    if (bits == 4 && l->kind == KIND_4) {
        return write_run_as(l, vram, x0, y, rx, count, src, first, 4, KIND_4, page_lo, page_hi);
    }
    return write_run_as(l, vram, x0, y, rx, count, src, first, bits, l->kind, page_lo, page_hi);
}

/* Moves the transfer on by n pixels along its row, to the next row at the row's end */
static void upload_advance(GsnUpload *u, uint32_t n) {
    u->x += n;
    if (u->x == u->w) {
        u->x = 0;
        if (++u->y == u->h) {
            u->active = 0;
        }
    }
}

int gsn_upload_data(GsnUpload *u, uint8_t *vram, const void *data, size_t bytes, uint32_t *page_lo,
                    uint32_t *page_hi) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t lo = 512, hi = 0;
    int changed = 0;

    if (u->active) {
        Layout l;
        int bits = gsn_psm_bits(u->psm);

        layout_init(&l, u->psm, u->bp, u->bw);
        if (bits == 4) {
            /* two pixels to a byte; a byte is never left half read except by the last pixel */
            uint64_t avail = (uint64_t)bytes * 2;
            uint32_t first = 0;

            while (u->active && avail != 0) {
                uint32_t n = u->w - u->x;

                if (n > avail) {
                    n = (uint32_t)avail;
                }
                changed |= write_run(&l, vram, u->x0, (u->y0 + u->y) & 2047, u->x, n, p, first, 4, &lo, &hi);
                p += (first + n) >> 1;
                first = (first + n) & 1;
                avail -= n;
                upload_advance(u, n);
            }
        } else {
            uint32_t size = (uint32_t)bits / 8;

            /* a pixel the last piece ended in the middle of */
            while (u->pending_bits != 0 && bytes != 0) {
                u->pending |= (uint64_t)*p++ << u->pending_bits;
                u->pending_bits += 8;
                bytes--;
                if (u->pending_bits == (uint32_t)bits) {
                    uint8_t one[4];

                    memcpy(one, &u->pending, 4);
                    changed |= write_run(&l, vram, u->x0, (u->y0 + u->y) & 2047, u->x, 1, one, 0, bits, &lo, &hi);
                    u->pending = 0;
                    u->pending_bits = 0;
                    upload_advance(u, 1);
                    break;
                }
            }
            while (u->active && bytes >= size) {
                uint32_t n = u->w - u->x;

                if (n > bytes / size) {
                    n = (uint32_t)(bytes / size);
                }
                changed |= write_run(&l, vram, u->x0, (u->y0 + u->y) & 2047, u->x, n, p, 0, bits, &lo, &hi);
                p += (size_t)n * size;
                bytes -= (size_t)n * size;
                upload_advance(u, n);
            }
            /* the start of a pixel the next piece ends */
            while (u->active && bytes != 0) {
                u->pending |= (uint64_t)*p++ << u->pending_bits;
                u->pending_bits += 8;
                bytes--;
            }
        }
    }
    if (page_lo) {
        *page_lo = lo;
    }
    if (page_hi) {
        *page_hi = hi;
    }
    return changed;
}

int gsn_vram_copy(uint8_t *vram, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg) {
    Layout src, dst;
    uint32_t sx = FIELD(trxpos, 0, 11), sy = FIELD(trxpos, 16, 11);
    uint32_t dx = FIELD(trxpos, 32, 11), dy = FIELD(trxpos, 48, 11);
    uint32_t dir = FIELD(trxpos, 59, 2);
    uint32_t w = FIELD(trxreg, 0, 12), h = FIELD(trxreg, 32, 12);
    int changed = 0;

    layout_init(&src, FIELD(bitbltbuf, 24, 6), FIELD(bitbltbuf, 0, 14), FIELD(bitbltbuf, 16, 6));
    layout_init(&dst, FIELD(bitbltbuf, 56, 6), FIELD(bitbltbuf, 32, 14), FIELD(bitbltbuf, 48, 6));
    for (uint32_t j = 0; j < h; j++) {
        uint32_t ry = (dir & 1) ? h - 1 - j : j;

        for (uint32_t i = 0; i < w; i++) {
            uint32_t rx = (dir & 2) ? w - 1 - i : i;
            uint32_t v = pixel_get(vram, src.kind, pixel_nibble(&src, (sx + rx) & 2047, (sy + ry) & 2047));

            changed |= pixel_put(vram, dst.kind, pixel_nibble(&dst, (dx + rx) & 2047, (dy + ry) & 2047), v);
        }
    }
    return changed;
}

void gsn_vram_read_image(const uint8_t *vram, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, void *dst,
                         size_t qwords) {
    Layout l;
    uint8_t *out = (uint8_t *)dst;
    size_t size = qwords * 16, at = 0; /* at: in nibbles */
    uint32_t psm = FIELD(bitbltbuf, 24, 6);
    uint32_t sx = FIELD(trxpos, 0, 11), sy = FIELD(trxpos, 16, 11);
    uint32_t w = FIELD(trxreg, 0, 12), h = FIELD(trxreg, 32, 12);
    int bits = gsn_psm_bits(psm);

    memset(out, 0, size);
    if (bits == 0) {
        return;
    }
    layout_init(&l, psm, FIELD(bitbltbuf, 0, 14), FIELD(bitbltbuf, 16, 6));
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            uint32_t v = pixel_get(vram, l.kind, pixel_nibble(&l, (sx + i) & 2047, (sy + j) & 2047));

            if (at >= size * 2) {
                return;
            }
            if (bits == 4) {
                out[at >> 1] |= (uint8_t)(v << ((at & 1) * 4));
            } else {
                /* a 24-bit pixel can straddle the end of the last qword asked for */
                for (int b = 0; b < bits / 8 && (at >> 1) + (size_t)b < size; b++) {
                    out[(at >> 1) + (size_t)b] = (uint8_t)(v >> (b * 8));
                }
            }
            at += (size_t)bits / 4;
        }
    }
}
