/* Whole frames of the screens the game draws as fields (port_whole_frame(), on by default;
   --no-whole-frame keeps the PS2's way).

   What the game does. The menus (Menu_Run: the pause menu's screens, the save screens, options and
   brightness screens) set the GS to field mode (sceGsResetGraph(0, 1, 2, 1)) with two 640x224 buffers:
   every vblank the game draws a new picture into the buffer not shown, with a draw offset half a line
   lower or not (sceGsSetHalfOffset, by the field it will be shown in), and the GS shows each buffer
   for one field. An interlaced TV shows the two fields one after the other; the PC weaves the last two
   into one 448-line picture, so two pictures of two different moments are woven together: whatever
   moves (the turning weapon of the Equipment screen, the model of a spell, a sliding panel, a fade)
   is combed every other line. The menus' VRAM has no room for a 448-line frame and its Z buffer (their
   textures are above the two fields and the Z buffer), so the game cannot draw them whole.

   What is done here. The GIF packets that reach the GS between two vblanks are kept. At the vblank,
   when the display is in field mode and they drew one picture, into the buffer not shown (every FRAME
   they set is that buffer, and they set the draw offset), they are sent again (to the renderers only)
   into the buffer shown, with the draw offsets (XYOFFSET_1 and _2) moved half a line the other way:
   that buffer then holds the same moment, drawn for the other field. The renderers are told of the
   new picture as the other field first (a vblank that shows nothing), then the vblank is scanned out
   as usual: the woven picture is one moment, whole. It is the picture the PS2 shows at the next
   vblank, shown a field early, as --frame-rate 60's pictures between are. As there:
   - textures and transfers that read the buffer drawn read the buffer shown instead (the brightness
     sprite draws the buffer over itself); local-to-host transfers, local-to-local ones into anything
     else, FINISH, SIGNAL and LABEL are not sent; the registers the replay changed (FRAME, TEX0,
     BITBLTBUF, XYOFFSET) are written back as the game left them;
   - nothing of the replay is left behind that the game sees: the buffer shown is cleared and drawn
     again by the game for the vblank after, and everything else is drawn again from the same packets.
   GS captures (F9) record the game's packets only: a capture replayed shows the fields as the PS2.

   Only the DMA controller's thread runs here (port/platform/gs.c). */
#include <stdlib.h>
#include <string.h>

#include "port/host.h"
#include "port/platform.h"

#define GS_PRIM 0x00
#define GS_TEX0_1 0x06
#define GS_TEX0_2 0x07
#define GS_XYOFFSET_1 0x18
#define GS_XYOFFSET_2 0x19
#define GS_SCISSOR_1 0x40
#define GS_SCISSOR_2 0x41
#define GS_FRAME_1 0x4C
#define GS_FRAME_2 0x4D
#define GS_BITBLTBUF 0x50
#define GS_TRXDIR 0x53
#define GS_SIGNAL 0x60
#define GS_FINISH 0x61
#define GS_LABEL 0x62

/* the packets of the last vblank: each its path and size, then its qwords */
typedef struct {
    uint32_t path;
    uint32_t bytes;
} Record;

static uint8_t *kept;
static size_t kept_size, kept_room;
static int kept_whole; /* the paths were between packets when keeping began */
static uint8_t *copy;
static size_t copy_room;
static uint64_t stat_replays;

void port_field_frames_reset(int whole) {
    kept_size = 0;
    kept_whole = whole;
}

void port_field_frames_keep(int path, const void *data, size_t bytes) {
    Record r;

    if (!kept_whole) {
        return;
    }
    if (kept_size + sizeof(r) + bytes > kept_room) {
        size_t room = (kept_size + sizeof(r) + bytes) * 2;
        uint8_t *grown = realloc(kept, room);

        if (grown == NULL) {
            kept_whole = 0;
            return;
        }
        kept = grown;
        kept_room = room;
    }
    r.path = (uint32_t)path;
    r.bytes = (uint32_t)bytes;
    memcpy(kept + kept_size, &r, sizeof(r));
    memcpy(kept + kept_size + sizeof(r), data, bytes);
    kept_size += sizeof(r) + bytes;
}

/* ---- A walk over the kept packets: the A+D writes (PACKED A+D, and REGLIST and PACKED with the
   register in the descriptor: PRIM, TEX0) seen, and in REPLAY changed ------------------------------ */
enum { SCAN, REPLAY };

typedef struct {
    uint32_t left, flg, nreg, reg, dwords;
    uint64_t regs;
} Path;

typedef struct {
    int mode;
    Path paths[4];
    /* SCAN */
    uint32_t draw_fbp; /* the FBP every FRAME write named (0xFFFFFFFF: none yet) */
    int other_fbp;     /* a FRAME write named another */
    int offset_set;    /* XYOFFSET_1 was written */
    uint32_t half;     /* its Y's half line (8: the half offset is on) */
    int draws;         /* a vertex was sent */
    uint32_t scissor_end; /* below the lowest line a SCISSOR write let be drawn (0: none written) */
    /* REPLAY */
    uint32_t shown_fbp;
    /* the registers as the game wrote them */
    uint64_t frame[2], tex0[2], xyoffset[2], bitbltbuf;
    int frame_set[2], tex0_set[2], xyoffset_set[2], bitbltbuf_set;
} Walk;

/* The draw offsets the game wrote in its last picture (and that picture's half line), and in the one
   before it: an offset the game wrote in the picture before, for the other field, and writes again
   unchanged is not moved by the half line. Menu_DrawItemModel draws the item models at a fixed
   XYOFFSET_2 in both fields; moved in the redraw, a model was half a line apart in the game's field and
   in the redrawn one, and at --native-scale 4 jumped every vblank (the shop's goods, the Equipment
   screen's weapon, October 2026). */
#define OFFSETS_KEPT 8
typedef struct {
    uint64_t value[2][OFFSETS_KEPT];
    int count[2];
    uint32_t half;
    int set;
} Offsets;
static Offsets offsets_now, offsets_before;

static void offsets_add(Offsets *o, int r, uint64_t value) {
    int i;

    for (i = 0; i < o->count[r]; i++) {
        if (o->value[r][i] == value) {
            return;
        }
    }
    if (o->count[r] < OFFSETS_KEPT) {
        o->value[r][o->count[r]++] = value;
    }
}

static int offset_fixed(int r, uint64_t value, uint32_t half) {
    int i;

    if (!offsets_before.set || offsets_before.half == half) {
        return 0;
    }
    for (i = 0; i < offsets_before.count[r]; i++) {
        if (offsets_before.value[r][i] == value) {
            return 1;
        }
    }
    return 0;
}

#define GS_DROPPED ((uint64_t)0x7D) /* a register the GS does not have (PORT_GS_REG_FRAME_INTERP) */

static void ad_write(Walk *w, uint32_t *reg, uint64_t *value) {
    uint32_t r = *reg;

    switch (r) {
    case GS_TEX0_1:
    case GS_TEX0_2:
        w->tex0[r - GS_TEX0_1] = *value;
        w->tex0_set[r - GS_TEX0_1] = 1;
        if (w->mode == REPLAY && (*value & 0x3FFF) == w->draw_fbp * 32) {
            *value = (*value & ~(uint64_t)0x3FFF) | (w->shown_fbp * 32);
        }
        break;
    case GS_FRAME_1:
    case GS_FRAME_2:
        w->frame[r - GS_FRAME_1] = *value;
        w->frame_set[r - GS_FRAME_1] = 1;
        if (w->mode == SCAN) {
            if (w->draw_fbp == 0xFFFFFFFFu) {
                w->draw_fbp = (uint32_t)(*value & 0x1FF);
            } else if ((uint32_t)(*value & 0x1FF) != w->draw_fbp) {
                w->other_fbp = 1;
            }
        } else if ((*value & 0x1FF) == w->draw_fbp) {
            *value = (*value & ~(uint64_t)0x1FF) | w->shown_fbp;
        }
        break;
    case GS_XYOFFSET_1:
    case GS_XYOFFSET_2:
        w->xyoffset[r - GS_XYOFFSET_1] = *value;
        w->xyoffset_set[r - GS_XYOFFSET_1] = 1;
        if (w->mode == SCAN) {
            offsets_add(&offsets_now, (int)(r - GS_XYOFFSET_1), *value);
            if (r == GS_XYOFFSET_1) {
                w->offset_set = 1;
                w->half = (uint32_t)(*value >> 32) & 8;
            }
        } else if (w->mode == REPLAY && !offset_fixed((int)(r - GS_XYOFFSET_1), *value, w->half)) {
            /* half a line the other way: the other field's lines (Y is 12.4, in bits 32-47) */
            uint64_t y = (*value >> 32) & 0xFFFF;

            y = w->half ? y - 8 : y + 8;
            *value = (*value & ~((uint64_t)0xFFFF << 32)) | ((y & 0xFFFF) << 32);
        }
        break;
    case GS_SCISSOR_1:
    case GS_SCISSOR_2:
        if (w->mode == SCAN) {
            uint32_t end = (uint32_t)(*value >> 48 & 0x7FF) + 1; /* SCAY1, the last line, + 1 */

            w->scissor_end = end > w->scissor_end ? end : w->scissor_end;
        }
        break;
    case GS_BITBLTBUF:
        w->bitbltbuf = *value;
        w->bitbltbuf_set = 1;
        if (w->mode == REPLAY) {
            if ((*value & 0x3FFF) == w->draw_fbp * 32) {
                *value = (*value & ~(uint64_t)0x3FFF) | (w->shown_fbp * 32);
            }
            if (((*value >> 32) & 0x3FFF) == w->draw_fbp * 32) {
                *value = (*value & ~((uint64_t)0x3FFF << 32)) | ((uint64_t)(w->shown_fbp * 32) << 32);
            }
        }
        break;
    case GS_TRXDIR:
        if (w->mode == REPLAY) {
            uint32_t dbp = (uint32_t)(w->bitbltbuf >> 32) & 0x3FFF;
            uint32_t dir = (uint32_t)(*value & 3);

            /* (bitbltbuf is the game's: a transfer into the buffer drawn goes into the one shown) */
            if (dir == 1 || (dir == 2 && dbp != w->draw_fbp * 32)) {
                *reg = (uint32_t)GS_DROPPED;
                *value = 0;
            }
        }
        break;
    case GS_SIGNAL:
    case GS_FINISH:
    case GS_LABEL:
        if (w->mode == REPLAY) {
            *reg = (uint32_t)GS_DROPPED;
            *value = 0;
        }
        break;
    }
}

static void walk_record(Walk *w, int path, uint8_t *data, size_t bytes) {
    Path *p = &w->paths[path];
    size_t count = bytes / 16, i = 0;

    while (i < count) {
        uint8_t *q = data + i * 16;

        if (p->left == 0) {
            uint64_t tag, regs;
            uint32_t nloop;

            memcpy(&tag, q, 8);
            memcpy(&regs, q + 8, 8);
            nloop = (uint32_t)(tag & 0x7FFF);
            p->flg = (uint32_t)((tag >> 58) & 3);
            p->nreg = (uint32_t)(tag >> 60);
            if (p->nreg == 0) {
                p->nreg = 16;
            }
            p->regs = regs;
            p->reg = 0;
            if (p->flg == 0) {
                p->left = nloop * p->nreg;
            } else if (p->flg == 1) {
                p->left = (nloop * p->nreg + 1) / 2;
                p->dwords = nloop * p->nreg;
            } else {
                p->left = nloop;
            }
            i++;
            continue;
        }
        if (p->flg == 0) {
            uint32_t d = (uint32_t)(p->regs >> (p->reg * 4)) & 0xF;
            uint64_t v[2];

            memcpy(v, q, 16);
            if (d == 0x4 || d == 0x5 || d == 0xC || d == 0xD) {
                w->draws = 1;
            } else if (d == 0x6 || d == 0x7) {
                uint32_t reg = d;

                ad_write(w, &reg, &v[0]);
                memcpy(q, &v[0], 8);
            } else if (d == 0xE) {
                uint32_t reg = (uint32_t)(v[1] & 0xFF);

                ad_write(w, &reg, &v[0]);
                v[1] = (v[1] & ~(uint64_t)0xFF) | reg;
                memcpy(q, v, 16);
            }
            p->reg = p->reg + 1 == p->nreg ? 0 : p->reg + 1;
            p->left--;
            i++;
        } else if (p->flg == 1) {
            int half;

            for (half = 0; half < 2 && p->dwords != 0; half++, p->dwords--) {
                uint32_t d = (uint32_t)(p->regs >> (p->reg * 4)) & 0xF;
                uint64_t v;

                memcpy(&v, q + half * 8, 8);
                if (d == 0x6 || d == 0x7) {
                    uint32_t reg = d;

                    ad_write(w, &reg, &v);
                    memcpy(q + half * 8, &v, 8);
                } else if (d == 0x5 || d == 0xD || d == 0x4 || d == 0xC) {
                    w->draws = 1;
                }
                p->reg = p->reg + 1 == p->nreg ? 0 : p->reg + 1;
            }
            p->left--;
            i++;
        } else {
            size_t n = count - i < p->left ? count - i : p->left;

            p->left -= (uint32_t)n;
            i += n;
        }
    }
}

static void walk_init(Walk *w, int mode) {
    memset(w, 0, sizeof(*w));
    w->mode = mode;
    w->draw_fbp = 0xFFFFFFFFu;
}

/* The lines of a field: half of DISPLAY's DH + 1 (read circuit 2's, or 1's when only it is on) */
static uint32_t field_lines(const uint64_t *privileged, int circuit2) {
    uint64_t display = circuit2 ? privileged[0xA0 / 8] : privileged[0x80 / 8];

    return ((uint32_t)(display >> 44 & 0x7FF) + 1) / 2;
}

int port_field_frames_vsync(const uint64_t *privileged, void (*send)(int path, const void *data, size_t bytes),
                            uint32_t *drawn) {
    static Walk scan, w;
    uint64_t smode2 = privileged[0x20 / 8];
    /* the buffer shown: DISPFB2's, or DISPFB1's when only read circuit 1 is on */
    int circuit2 = (privileged[0] & 2) != 0;
    uint64_t dispfb = circuit2 ? privileged[0x90 / 8] : privileged[0x70 / 8];
    uint32_t shown = (uint32_t)(dispfb & 0x1FF);
    uint64_t fix[2 * 10];
    size_t at;
    int n = 0, i;

    /* interlaced, field mode, and a whole vblank's packets */
    if ((smode2 & 3) != 3 || !kept_whole || kept_size == 0) {
        return 0;
    }
    walk_init(&scan, SCAN);
    memset(&offsets_now, 0, sizeof(offsets_now));
    for (at = 0; at < kept_size;) {
        Record r;

        memcpy(&r, kept + at, sizeof(r));
        walk_record(&scan, (int)r.path, kept + at + sizeof(r), r.bytes);
        at += sizeof(r) + r.bytes;
    }
    /* one picture, into the buffer not shown, at a draw offset of its own */
    if (!scan.draws || scan.other_fbp || scan.draw_fbp == 0xFFFFFFFFu || scan.draw_fbp == shown ||
        !scan.offset_set || ((uint32_t)(dispfb >> 9) & 0x3F) != ((uint32_t)(scan.frame[0] >> 16) & 0x3F)) {
        return 0;
    }
    /* a field's picture: its scissor nearer a field's height than the whole frame's (the Equipment screen's
       is a field and a line). As the menu closes, the display still shows the menu's field (page 70) while
       the game draws its first 448-line frame (into page 140). Replayed into page 70, that frame ran over
       the buffer at 140: one vblank showed its top half stretched, the two after its bottom half twice
       (November 2026) */
    if (scan.scissor_end * 2 > field_lines(privileged, circuit2) * 3) {
        return 0;
    }
    offsets_now.half = scan.half;
    offsets_now.set = 1;
    walk_init(&w, REPLAY);
    w.draw_fbp = scan.draw_fbp;
    w.shown_fbp = shown;
    w.half = scan.half;
    for (at = 0; at < kept_size;) {
        Record r;

        memcpy(&r, kept + at, sizeof(r));
        if (copy_room < r.bytes) {
            free(copy);
            copy_room = r.bytes * 2;
            copy = malloc(copy_room);
            if (copy == NULL) {
                PORT_FATAL("out of memory");
            }
        }
        memcpy(copy, kept + at + sizeof(r), r.bytes);
        walk_record(&w, (int)r.path, copy, r.bytes);
        send((int)r.path, copy, r.bytes);
        at += sizeof(r) + r.bytes;
    }
    /* the registers the replay changed, back to what the game left in them (by PATH3: every path is
       between packets) */
    for (i = 0; i < 2; i++) {
        if (scan.frame_set[i]) {
            n++;
            fix[2 * n] = scan.frame[i];
            fix[2 * n + 1] = (uint64_t)(GS_FRAME_1 + i);
        }
        if (scan.tex0_set[i]) {
            n++;
            fix[2 * n] = scan.tex0[i] & ~((uint64_t)7 << 61); /* CLD 0: no CLUT load */
            fix[2 * n + 1] = (uint64_t)(GS_TEX0_1 + i);
        }
        if (scan.xyoffset_set[i]) {
            n++;
            fix[2 * n] = scan.xyoffset[i];
            fix[2 * n + 1] = (uint64_t)(GS_XYOFFSET_1 + i);
        }
    }
    if (scan.bitbltbuf_set) {
        n++;
        fix[2 * n] = scan.bitbltbuf;
        fix[2 * n + 1] = GS_BITBLTBUF;
    }
    if (n > 0) {
        fix[0] = (uint64_t)n | ((uint64_t)1 << 15) | ((uint64_t)1 << 60); /* NLOOP n, EOP, PACKED, NREG 1 */
        fix[1] = 0xE;                                                       /* A+D */
        send(3, fix, (size_t)(n + 1) * 16);
    }
    stat_replays++;
    offsets_before = offsets_now;
    *drawn = scan.draw_fbp;
    return 1;
}

uint64_t port_field_frames_replays(void) {
    return stat_replays;
}
