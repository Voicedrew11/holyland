/* The GS frame capture's file (.kf4gs): written by port/debug/gs_capture.c (F9, --gs-capture-at), read
   by tools/gsreplay (kf4-gsreplay). It is meant to be read by any build, so it holds nothing of the
   program's own layout: only the PS2's (GS registers by their GS address, VRAM as the GS lays it out,
   VU1's memories and registers, VIF1's registers, the GIF and VIF1 streams) and paraLLEl-GS's
   scanout history as plain pictures. Little-endian.

   The file: a header (Kf4GsHeader), then the body in blocks of up to 4 MB, each a pair of uint32
   (its size, its stored size) and the block, compressed in LZ4's block format, or as it is when that
   is no smaller (as save states, port/platform/savestate.c).

   The body: records, each a Kf4GsRecord and `size` bytes (a multiple of 4), in this order:
   META, VRAM, GS_REGS, GS_PRIV, GS_HISTORY, (SA_TABLE,) VU1, VIF1 (the state at the start, right after the
   scanout of the vblank in META's start_vblank), then the stream: GIF, VIFDATA, VU1_RUN, READ_FIFO
   and VSYNC records in the order they happened, and END. A reader skips record types it does not
   know.

   Native draw (--native-draw, port/include/port/render.h): the chain's PORT_GS_REG_NATIVE_DRAW writes
   (GS address 0x7B) name draws in the renderer core's side buffer, which the GIF stream does not hold.
   ND_MESH and ND_DRAWS records carry them, each before the first GIF or VIFDATA record that can name
   what it holds: the meshes as the PS2 data blocks they were parsed from (a reader parses them again),
   and per frame its cameras and draws. A run without native draws has none. */
#ifndef PORT_GS_CAPTURE_FORMAT_H
#define PORT_GS_CAPTURE_FORMAT_H

#include <stdint.h>

#define KF4GS_MAGIC "KF4GSCAP"
#define KF4GS_VERSION 1
#define KF4GS_BLOCK_SIZE 0x400000u

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t header_size; /* sizeof(Kf4GsHeader) */
    uint64_t body_size;   /* the body, uncompressed */
    uint64_t body_hash;   /* FNV-1a (64-bit) of the body */
} Kf4GsHeader;

typedef struct {
    uint32_t type;
    uint32_t size; /* bytes that follow */
} Kf4GsRecord;

enum {
    KF4GS_END = 0,
    /* text, "key=value" lines: start_vblank, vblanks, game (game/state_log.c's line), build, options */
    KF4GS_META = 1,
    KF4GS_VRAM = 2,       /* the GS's 4 MB, as the GS lays it out */
    KF4GS_GS_REGS = 3,    /* Kf4GsReg[]: the general registers by GS address (and KF4GS_REG_*) */
    KF4GS_GS_PRIV = 4,    /* uint64 lo[0x200], hi[0x200]: 0x12000000.., 0x12001000.. (CSR ...) */
    /* uint32 scanmsk_timeout, uint32 count, then count fields: Kf4GsField and, when present and new
       (same_as < 0), width * height RGBA pixels */
    KF4GS_GS_HISTORY = 5,
    /* micro memory (16 KB), data memory (16 KB), then Kf4GsVu1Regs */
    KF4GS_VU1 = 6,
    KF4GS_VIF1 = 7, /* Kf4GsVif1Regs */
    /* 8 was the page-ownership table of the removed native GS. No build writes it now; the number stays so that
       old captures still read as they were. */
    KF4GS_SA_TABLE = 8,

    KF4GS_GIF = 16,       /* uint32 path (1, 2, 3), then whole qwords, as handed to the GS */
    KF4GS_VIFDATA = 17,   /* uint32 words, as DMA channel 1 (or libgraph) fed VIF1 */
    KF4GS_VU1_RUN = 18,   /* uint32 address (instructions; KF4GS_MSCNT for MSCNT), top, itop */
    KF4GS_READ_FIFO = 19, /* uint32 qwords read back from the GS (BITBLTBUF/TRXDIR 1) */
    /* uint64 vblank, uint32 field, uint32 0, then the 0xF0 bytes of PMODE..BGCOLOR (the register
       page from 0x12000000, a register every 16 bytes) */
    KF4GS_VSYNC = 20,
    /* native draw: Kf4GsNdMesh, then qwords * 16 bytes: the data block (its first qword is the VIF
       stream's first) as it was when parsed. A mesh's id is given once per capture. */
    KF4GS_ND_MESH = 21,
    /* native draw: Kf4GsNdDraws, then cameras Kf4GsNdCamera (the frame's, all of them so far), then
       count draws, each a Kf4GsNdDraw and its mesh_count mesh ids (uint32, ND_MESH's), and a draw of
       program 0x280 then its turns (32 floats: RdDraw's turn; none before 0x280 was drawn). The draws are
       the frame's from index first on; a reader drops any it holds from first on (first 0: the frame
       begins again) */
    KF4GS_ND_DRAWS = 22,
};

#define KF4GS_MSCNT 0xFFFFFFFFu

/* Pseudo-addresses for what paraLLEl-GS keeps beside the registers */
#define KF4GS_REG_INTERNAL_Q 0x1000 /* the Q that ST last set (a float's bits) */
#define KF4GS_REG_CACHED_CBP0 0x1001 /* the CLUT's CBP last loaded, per context */
#define KF4GS_REG_CACHED_CBP1 0x1002

typedef struct {
    uint32_t address;
    uint32_t reserved;
    uint64_t value;
} Kf4GsReg;

typedef struct {
    int32_t same_as; /* an earlier field that is the same picture, or -1 */
    uint32_t width, height;
    uint32_t present;
} Kf4GsField;

/* VU1 between two programs (nothing in flight) */
typedef struct {
    uint32_t vf[32][4];
    uint32_t acc[4];
    uint32_t vi[16];
    uint32_t q, p, i, r;
    uint32_t mac, status, clip, clip_visible;
    int32_t mac_queued;
    uint32_t pc; /* where MSCNT goes on */
    uint64_t cycle;
} Kf4GsVu1Regs;

/* VIF1 between two commands */
typedef struct {
    uint32_t cl, wl, mode, mask;
    uint32_t row[4], col[4];
    uint32_t base, ofst, tops, itops, top, itop;
    uint32_t dbf, mskpath3, mark;
    uint32_t reserved;
} Kf4GsVif1Regs;

/* ---- Native draw's records (the renderer core's, render.h: RdDraw, RdCamera; float bits as they are) */
typedef struct {
    uint32_t id; /* what ND_DRAWS names it by (unique in the capture) */
    uint32_t key; /* rd_mesh_from_block's key (the block's EE address) */
    uint32_t qwords;
    uint32_t format; /* RdVertexFormat */
} Kf4GsNdMesh;

typedef struct {
    uint32_t serial; /* the frame's (RD_ND_FRAME's, bits 32-55 of a draw's handle) */
    uint32_t first;  /* the first draw's index in the frame (bits 0-31 of its handle) */
    uint32_t count;  /* draws that follow */
    uint32_t cameras; /* cameras that follow */
} Kf4GsNdDraws;

typedef struct {
    float world_screen[16], view[16], screen[16];
    uint32_t mode, projection;
} Kf4GsNdCamera;

typedef struct {
    uint32_t kind, id[3], program, flags, camera, mesh_count;
    float model[16], parent[16], light_dir[16], light_col[16];
    float fog[4];
    uint64_t fogcol;
    int32_t point_count;
    uint32_t reserved;
    float point_pos[16], point_col[16];
    float colour[12];
} Kf4GsNdDraw;

#endif
