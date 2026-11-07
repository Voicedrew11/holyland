/* The GS's local memory (its 4 MB of VRAM) on the CPU, laid out as the GS lays it out.

   The GS does not store a picture row after row. VRAM is 512 pages of 8 KB; a page is 32 blocks of
   256 bytes; a block is 4 columns of 64 bytes. Each pixel format (PSM) cuts a page into a rectangle
   of its own size (64x32 for the 32-bit formats, 64x64 for the 16-bit ones, 128x64 for PSMT8,
   128x128 for PSMT4), orders the blocks inside it in its own way, and the pixels inside a column in
   its own way. A buffer is a run of pages from a base pointer, `bw` 64-pixel units wide. The Z
   formats use the same pixels-in-a-column order as the colour formats of their size, but their
   blocks in a different arrangement. PSMT8H, PSMT4HL and PSMT4HH are textures kept in the unused
   top byte of a PSMCT24 buffer: they have the PSMCT32 layout and own only that byte or nibble.

   This file is the memory model and the GS's image transfers (BITBLTBUF, TRXPOS, TRXREG, TRXDIR):
   host to local (IMAGE data from a GIF path), local to local, and local to host. The host is taken
   to be little-endian, as the GS is.

   All functions are thread-compatible: they keep no state of their own beyond read-only layout
   tables, which are built on the first call. */
#ifndef GSN_VRAM_H
#define GSN_VRAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GSN_VRAM_BYTES (4u << 20)

/* PSM codes as the GS has them */
enum {
    GSN_PSMCT32 = 0x00,
    GSN_PSMCT24 = 0x01,
    GSN_PSMCT16 = 0x02,
    GSN_PSMCT16S = 0x0A,
    GSN_PSMT8 = 0x13,
    GSN_PSMT4 = 0x14,
    GSN_PSMT8H = 0x1B,
    GSN_PSMT4HL = 0x24,
    GSN_PSMT4HH = 0x2C,
    GSN_PSMZ32 = 0x30,
    GSN_PSMZ24 = 0x31,
    GSN_PSMZ16 = 0x32,
    GSN_PSMZ16S = 0x3A
};

/* Bits per pixel as a transfer carries them: 32, 24, 16, 8 or 4 (PSMT8H 8, PSMT4HL/HH 4); 0 for a
   PSM the GS does not have. */
int gsn_psm_bits(uint32_t psm);
int gsn_psm_valid(uint32_t psm);

/* One pixel. bp is the base pointer in 256-byte blocks (TBP0, FBP*32, ZBP*32, DBP, SBP), bw the
   buffer width in 64-pixel units. x and y are not limited to the buffer: a pixel past bw*64 is
   where the GS's page arithmetic puts it (in a later page), and addresses wrap at 4 MB.
   The value is the pixel as a transfer carries it: PSMCT24/Z24 the low 24 bits of the word, the
   16-bit formats a halfword, PSMT8/T8H a byte, the 4-bit formats a nibble. */
uint32_t gsn_vram_read(const uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
void gsn_vram_write(uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
/* The same for `count` pixels of a row, from (x, y) rightwards: what gsn_vram_read would give for
   each, into out; each of in written as gsn_vram_write would */
void gsn_vram_read_row(const uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                       uint32_t count, uint32_t *out);
void gsn_vram_write_row(uint8_t *vram, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                        uint32_t count, const uint32_t *in);

/* The byte offset in VRAM of the unit that holds the pixel: the 32-bit word for PSMCT32/24, Z32/24,
   PSMT8H and PSMT4HL/HH (these own only part of it), the halfword for the 16-bit formats, the byte
   for PSMT8 and PSMT4. *nibble (if not NULL) is, for PSMT4, 0 for the low nibble of that byte and
   1 for the high one; for PSMT4HL and PSMT4HH, 6 or 7 (the nibble of the word, counting from bit
   0); for the other formats -1. */
uint32_t gsn_vram_offset(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, int *nibble);

/* A host->local transfer (TRXDIR 0), fed in pieces as IMAGE data arrives. The pixels come in rows
   of the rectangle, packed with no padding: 24-bit pixels are 3 bytes and straddle qwords, 4-bit
   pixels are two to a byte, low nibble first, and a row of odd width shares a byte with the next.
   The rectangle's coordinates wrap at 2048, as the GS's transmission area does. */
typedef struct {
    uint32_t psm, bp, bw;      /* destination (DPSM, DBP, DBW) */
    uint32_t x0, y0, w, h;     /* DSAX, DSAY, RRW, RRH */
    uint32_t x, y;             /* the next pixel, relative to x0, y0 */
    uint64_t pending; uint32_t pending_bits; /* the bytes of a pixel a piece ended in the middle of */
    int active;                /* pixels are still awaited */
} GsnUpload;

void gsn_upload_begin(GsnUpload *u, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);

/* Writes the pixels in `data` into VRAM; data past the rectangle's last pixel is ignored. Returns 1
   if any byte of VRAM changed. *page_lo and *page_hi (if not NULL) get the first and last 8 KB page
   (0..511) a pixel was written to, inclusive, for the caller to invalidate; *page_lo > *page_hi
   when no pixel was written. (A transfer that wraps at 4 MB reports 0..511.) */
int gsn_upload_data(GsnUpload *u, uint8_t *vram, const void *data, size_t bytes, uint32_t *page_lo,
                    uint32_t *page_hi);

/* A local->local transfer (TRXDIR 2), pixel by pixel in the order TRXPOS's DIR asks for (bit 0:
   rows from the bottom up; bit 1: each row from right to left), which is what makes an overlapping
   copy come out right when the program chooses the order for it. The source pixel is read as SPSM
   and written as DPSM. Returns 1 if any byte of VRAM changed. */
int gsn_vram_copy(uint8_t *vram, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);

/* A local->host transfer (TRXDIR 1): fills `qwords` qwords at dst with the source rectangle's
   pixels as the GS sends them (packed as for host->local); past the last pixel, zeroes. */
void gsn_vram_read_image(const uint8_t *vram, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, void *dst,
                         size_t qwords);

#ifdef __cplusplus
}
#endif

#endif
