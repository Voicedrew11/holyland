/* libmpeg for the PC port: the sceMpeg calls the movie player (gfx/movie.c) makes, on the decoder in
   port/sdk/mpeg2.c instead of the IPU.

   What the game sees is libmpeg's (decomp: src/sce/libmpeg):
   - sceMpegDemuxPssRing splits the PSS file into its streams as libmpeg's demultiplexer does, and
     hands each packet to the stream's callback in the same record.
   - The video stream reaches the decoder the way it reaches the IPU, by DMA channel 4 from the
     player's ring of blocks (port/platform/ipu.c). When the channel has nothing more, the "no data"
     callback runs, which queues the blocks written since; IPU_BP says how far the decoder has read
     when the time stamp callback asks.
   - sceMpegGetPicture decodes until a picture is ready to show, in display order: a B picture at
     once, an I or P picture when the next reference comes, the last one at the sequence end code.
     frameCount, sceMpegIsEnd and sceMpegIsRefBuffEmpty change as libmpeg's do.
   - The picture is converted to RGB32 with the IPU's CSC. The movie player requests row-major
     output for its batched upload. The default still uses the IPU's 16x16 macroblock order.
     The background callback runs at the conversion, where libmpeg runs it during the CSC.

   libmpeg suspends the IPU's input around each conversion (the stop and restart DMA callbacks); the
   decoder here does not use the IPU, so it never calls them, and the stream's DMA only moves as the
   decoder reads. Decoding takes no EE time. The player shows a picture every two fields and can
   drop late pictures when the audio device clock runs ahead. */
#include <stdlib.h>
#include <string.h>

#include "eeregs.h"
#include "libmpeg.h"
#include "mpeg2.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/savestate.h"

/* the callback types (sceMpegCbType) */
#define CB_ERROR 0
#define CB_NODATA 1
#define CB_BACKGROUND 4
#define CB_TIMESTAMP 5
#define CB_STR 6
#define CB_COUNT 7
#define MAX_STREAM_CALLBACKS 0x40

typedef struct {
    sceMpegCallback func;
    void *data;
} Callback;

/* a stream callback: the 40-bit stream id (the PES stream id, and for a private stream its first four
   payload bytes) it takes, under a mask */
typedef struct {
    int64_t id;
    uint64_t mask;
    sceMpegCallback func;
    void *data;
} StreamCallback;

typedef struct {
    Callback callbacks[CB_COUNT];
    StreamCallback streams[MAX_STREAM_CALLBACKS];
    int stream_count;
    Mpeg2Decoder decoder;
    int is_end;
    int ref_count;  /* pictures decoded since the last flush (libmpeg's refCount) */
    int out_state;  /* a picture has been decoded since the reset */
    int total_frames;
    int pictures;   /* pictures shown since sceMpegCreate */
    int linear_output;
    int64_t pts[3], dts[3]; /* each picture buffer's time stamps */
} MpegState;

/* decoders made and not deleted */
static int decoders;

const char *port_mpeg_state_busy(void) {
    return port_movie_playing() ? "a movie is playing" : NULL;
}

int port_movie_playing(void) {
    return __atomic_load_n(&decoders, __ATOMIC_ACQUIRE) > 0;
}

static MpegState *state_of(sceMpeg *mp) {
    if (mp == NULL || mp->sys == NULL) {
        PORT_FATAL("sceMpeg: a handle sceMpegCreate did not make");
    }
    return mp->sys;
}

static int dispatch(sceMpeg *mp, void *cbdata) {
    MpegState *s = state_of(mp);
    Callback *cb = &s->callbacks[*(int *)cbdata];

    return cb->func != NULL ? cb->func(mp, cbdata, cb->data) : 0;
}

static void dispatch_simple(sceMpeg *mp, int type) {
    sceMpegCbData data;

    memset(&data, 0, sizeof(data));
    data.type = type;
    dispatch(mp, &data);
}

/* The decoder's stream: DMA channel 4, and the "no data" callback until it sends something. */
static void read_qword(void *ctx, uint8_t qword[16]) {
    while (!port_ipu_read_qword(qword)) {
        dispatch_simple(ctx, CB_NODATA);
    }
}

/* ---- The handle --------------------------------------------------------------------------------- */
void sceMpegInit(void) {
    *D3_QWC = 0;
    *D4_QWC = 0;
}

int sceMpegCreate(sceMpeg *mp, void *work_area, int size) {
    MpegState *s;

    (void)work_area;
    (void)size;
    /* the state lives on the host; the work area the game gives is left as it is */
    s = calloc(1, sizeof(MpegState));
    if (s == NULL) {
        PORT_FATAL("sceMpegCreate: no memory");
    }
    memset(mp, 0, sizeof(*mp));
    mp->sys = s;
    mp->pts = mp->dts = -1;
    mp->pts2nd = mp->dts2nd = -1;
    mpeg2_init(&s->decoder, read_qword, mp);
    __atomic_fetch_add(&decoders, 1, __ATOMIC_RELEASE);
    sceMpegReset(mp);
    port_event("movie: decoder created");
    return 1;
}

int sceMpegDelete(sceMpeg *mp) {
    MpegState *s = state_of(mp);

    port_event("movie: decoder deleted after %d pictures", s->pictures);
    mpeg2_free(&s->decoder);
    __atomic_fetch_sub(&decoders, 1, __ATOMIC_RELEASE);
    free(s);
    mp->sys = NULL;
    return 1;
}

void sceMpegReset(sceMpeg *mp) {
    MpegState *s = state_of(mp);

    s->is_end = 0;
    s->ref_count = 0;
    s->out_state = 0;
    s->total_frames = 0;
    mp->frameCount = 0;
    mpeg2_reset(&s->decoder);
}

sceMpegCallback sceMpegAddCallback(sceMpeg *mp, int type, sceMpegCallback func, void *data) {
    MpegState *s = state_of(mp);
    sceMpegCallback old;

    if (type < 0 || type >= CB_COUNT) {
        return NULL;
    }
    old = s->callbacks[type].func;
    s->callbacks[type].func = func;
    s->callbacks[type].data = data;
    return old;
}

void sceMpegSetDecodeMode(sceMpeg *mp, int ni, int np, int nb) {
    (void)mp;
    (void)ni;
    (void)np;
    (void)nb;
    PORT_TODO("sceMpegSetDecodeMode: every picture is decoded");
}

int sceMpegIsEnd(sceMpeg *mp) {
    return state_of(mp)->is_end;
}

int sceMpegIsRefBuffEmpty(sceMpeg *mp) {
    return state_of(mp)->ref_count == 0;
}

void port_mpeg_set_linear_output(sceMpeg *mp, int linear) {
    state_of(mp)->linear_output = linear;
}

/* ---- The demultiplexer (libmpeg's pack.o) ------------------------------------------------------- */
#define PSS_PCM_STREAM 0xBDFFA00000ULL

/* the PSS streams a callback can ask for, by type: the id and the mask of the bits that name it */
static const struct {
    uint64_t id;
    uint64_t mask;
} stream_types[10] = {
    {0xE000000000ULL, 0xFF00000000ULL}, /* 0 MPEG video, channel in the stream id */
    {0xBDFFC00000ULL, 0xFFFFFFFFFFULL}, /* 1 */
    {PSS_PCM_STREAM, 0xFFFFFFFFFFULL},  /* 2 PCM sound */
    {0xBDFFA10000ULL, 0xFFFFFFFFFFULL}, /* 3 */
    {0xBDFF900000ULL, 0xFFFFFFFFFFULL}, /* 4 */
    {0xC000000000ULL, 0xFF00000000ULL}, /* 5 MPEG audio */
    {0xBD80000000ULL, 0xFFFF000000ULL}, /* 6 */
    {0xBDA0000000ULL, 0xFFFF000000ULL}, /* 7 */
    {0xBD88000000ULL, 0xFFFF000000ULL}, /* 8 */
    {0xBD90000000ULL, 0xFFFF000000ULL}, /* 9 */
};

int sceMpegAddStrCallback(sceMpeg *mp, int type, int ch, sceMpegCallback func, void *data) {
    MpegState *s = state_of(mp);
    int64_t id;
    int shift = 0;
    int i;

    if (type < 0 || type >= 10) {
        return 0;
    }
    if (stream_types[type].mask == 0xFFFF000000ULL) {
        shift = 24;
    } else if (stream_types[type].mask == 0xFF00000000ULL) {
        shift = 32;
    }
    id = (int64_t)(stream_types[type].id | ((uint64_t)ch << shift));
    for (i = 0; i < s->stream_count && s->streams[i].id != id; i++) {
    }
    if (i == MAX_STREAM_CALLBACKS) {
        return 0;
    }
    if (i == s->stream_count) {
        s->stream_count++;
    }
    s->streams[i].id = id;
    s->streams[i].mask = stream_types[type].mask;
    s->streams[i].func = func;
    s->streams[i].data = data;
    return 0;
}

/* The bits of the data being demultiplexed, which may wrap around the end of the read ring. */
typedef struct {
    const uint8_t *buf;
    const uint8_t *ring;
    int ring_size; /* -1: no ring */
    uint64_t pos;  /* in bits, from buf */
} PssBits;

static const uint8_t *pss_byte_ptr(const PssBits *b, uint64_t byte) {
    const uint8_t *p = b->buf + byte;

    if (b->ring_size > 0 && p >= b->ring + b->ring_size) {
        p -= b->ring_size;
    }
    return p;
}

static uint32_t pss_next(const PssBits *b, int n) {
    uint64_t value = 0;
    uint64_t byte = b->pos >> 3;
    int have = 0;
    int skip = (int)(b->pos & 7);

    while (have < skip + n) {
        value = value << 8 | *pss_byte_ptr(b, byte++);
        have += 8;
    }
    return (uint32_t)((value >> (have - skip - n)) & ((1ull << n) - 1));
}

/* reads n bits, any number of them, and returns the last 32 at most */
static uint32_t pss_get(PssBits *b, int n) {
    uint32_t value = 0;

    while (n > 0) {
        int chunk = n > 24 ? 24 : n;

        value = (uint32_t)((uint64_t)value << chunk) | pss_next(b, chunk);
        b->pos += (uint64_t)chunk;
        n -= chunk;
    }
    return value;
}

static void pss_pack_header(PssBits *b) {
    uint32_t stuffing;
    uint32_t i;

    pss_get(b, 0x22 + 3 + 1 + 15 + 1 + 15 + 1 + 9 + 0x1E); /* to pack_stuffing_length */
    stuffing = pss_get(b, 3);
    for (i = 0; i < stuffing; i++) {
        pss_get(b, 8);
    }
    if (pss_next(b, 32) == 0x1BB) {
        /* the system header */
        pss_get(b, 0x38);
        pss_get(b, 0x28);
        while (pss_next(b, 1) == 1) {
            pss_get(b, 0x18);
        }
    }
}

typedef struct {
    int64_t id;
    int64_t pts, dts;
    uint64_t header_pos, data_pos; /* in bits */
    int data_len;
} PesPacket;

static int64_t pss_time_stamp(PssBits *b) {
    uint32_t high, mid, low;

    pss_get(b, 4);
    high = pss_get(b, 3);
    pss_get(b, 1);
    mid = pss_get(b, 15);
    pss_get(b, 1);
    low = pss_get(b, 15);
    pss_get(b, 1);
    return (int64_t)((uint64_t)((high >> 2) & 1) << 32 | (uint32_t)(high << 30 | mid << 15 | low));
}

/* the bits each combination of the four flags ahead of the extension flag adds to a PES header */
static const uint8_t pes_flag_bits[16] = {0, 16, 8, 24, 8, 24, 16, 32, 24, 40, 32, 48, 32, 48, 40, 56};

static void pss_pes_packet(PssBits *b, PesPacket *pkt) {
    int64_t sid;
    int length;

    pkt->header_pos = b->pos;
    pss_get(b, 24);
    sid = pss_get(b, 8);
    pkt->id = sid << 32;
    length = (int)pss_get(b, 16);
    pkt->pts = pkt->dts = -1;
    pkt->data_pos = b->pos;
    pkt->data_len = 0;
    if (sid == 0xBE) {
        b->pos += (uint64_t)length * 8;
    } else if (sid == 0xBC || sid == 0xBF || sid == 0xF0 || sid == 0xF1 || sid == 0xFF || sid == 0xF2 ||
               sid == 0xF8) {
        if (sid == 0xBF) {
            length -= 4;
            pkt->id |= pss_get(b, 32);
        }
        b->pos += (uint64_t)length * 8;
    } else {
        int pts_dts, escr, flags, extension, header_length;
        uint64_t base;
        int rest;

        pss_get(b, 2);
        pss_get(b, 2); /* PES_scrambling_control */
        pss_get(b, 4);
        pts_dts = (int)pss_get(b, 2);
        escr = (int)pss_get(b, 1);
        flags = (int)pss_get(b, 4);
        extension = (int)pss_get(b, 1);
        header_length = (int)pss_get(b, 8);
        base = b->pos;
        if (pts_dts & 2) {
            pkt->pts = pss_time_stamp(b);
        }
        if (pts_dts == 3) {
            pkt->dts = pss_time_stamp(b);
        }
        if (escr) {
            pss_get(b, 0x30);
        }
        if (flags) {
            pss_get(b, pes_flag_bits[flags]);
        }
        if (extension) {
            int private_data = (int)pss_get(b, 1);
            int pack_field = (int)pss_get(b, 1);
            int sequence_counter = (int)pss_get(b, 1);
            int pstd = (int)pss_get(b, 1);
            int extension2;

            pss_get(b, 3);
            extension2 = (int)pss_get(b, 1);
            if (private_data) {
                pss_get(b, 0x80);
            }
            if (pack_field) {
                port_event("movie: a PES pack_header_field_flag, which PSS does not have");
                return;
            }
            if (sequence_counter) {
                pss_get(b, 16);
            }
            if (pstd) {
                pss_get(b, 16);
            }
            if (extension2) {
                uint32_t count, i;

                pss_get(b, 1);
                count = pss_get(b, 7);
                for (i = 0; i < count; i++) {
                    pss_get(b, 8);
                }
            }
        }
        rest = header_length - (int)((b->pos - base) >> 3);
        b->pos += (uint64_t)(int64_t)rest * 8;
        pkt->data_len = length - header_length - 3;
        pkt->data_pos = b->pos;
        rest = pkt->data_len;
        if (pkt->id == 0xBD00000000LL) {
            /* a private stream: its first four bytes say which */
            pkt->id |= pss_get(b, 32);
            rest -= 4;
        }
        b->pos += (uint64_t)(int64_t)rest * 8;
    }
}

/* 1 if the bytes at b->pos, all four of them in the `size` bytes there are, start no packet or pack: a sector the
   disc could not read, given as zeros (port_disc_read). PSS packs are whole sectors, so the next one starts one. */
static int pss_starts_nothing(const PssBits *b, int size) {
    return b->pos + 32 <= (uint64_t)size * 8 && pss_next(b, 24) != 1;
}

/* Bytes that start nothing skipped, to the next start code (or the last three bytes, which may begin one). Without
   it the demultiplexing stopped at them having used nothing, and the movie's reader (gfx/movie.c) gave the same
   bytes again for ever: a black screen with the movie's sound going on. Returns 1 if any were skipped. */
static int pss_skip_unreadable(PssBits *b, int size) {
    uint64_t from = b->pos;

    while (pss_starts_nothing(b, size)) {
        b->pos += 8;
    }
    if (b->pos == from) {
        return 0;
    }
    port_event("movie: %llu bytes that start no packet skipped (a sector that could not be read?)",
               (unsigned long long)((b->pos - from) >> 3));
    return 1;
}

int sceMpegDemuxPssRing(sceMpeg *mp, void *buf, int size, void *ring, int ring_size) {
    MpegState *s = state_of(mp);
    PssBits b;
    PesPacket pkt;
    sceMpegCallback other = NULL;
    void *other_data = NULL;
    int consumed = 0;
    int go_on = 1;
    int i;

    b.buf = buf;
    b.ring = ring;
    b.ring_size = ring_size;
    b.pos = 0;
    /* a callback for "every other stream", if one was given */
    for (i = 0; i < s->stream_count; i++) {
        if (s->streams[i].id == 0xBDFF000000LL) {
            other = s->streams[i].func;
            other_data = s->streams[i].data;
            break;
        }
    }
    do {
        if (go_on && pss_skip_unreadable(&b, size)) {
            consumed = (int)(b.pos >> 3);
        }
        if (pss_next(&b, 32) == 0x1BA) {
            pss_pack_header(&b);
        }
        while (pss_next(&b, 24) == 1 && pss_next(&b, 32) != 0x1BA && pss_next(&b, 32) != 0x1B9 &&
               b.pos < (uint64_t)size * 8 && go_on) {
            sceMpegCbDataStr record;

            pss_pes_packet(&b, &pkt);
            if (b.pos > (uint64_t)size * 8) {
                continue; /* the packet is not all there yet */
            }
            record.type = CB_STR;
            record.header = (unsigned char *)pss_byte_ptr(&b, pkt.header_pos >> 3);
            record.data = (unsigned char *)pss_byte_ptr(&b, pkt.data_pos >> 3);
            record.len = (unsigned int)pkt.data_len;
            record.pts = pkt.pts;
            record.dts = pkt.dts;
            for (i = 0; i < s->stream_count; i++) {
                if (s->streams[i].id == (int64_t)((uint64_t)pkt.id & s->streams[i].mask)) {
                    go_on = s->streams[i].func(mp, &record, s->streams[i].data);
                    break;
                }
            }
            if (i == s->stream_count && other != NULL) {
                go_on = other(mp, &record, other_data);
            }
            if (go_on) {
                consumed = (int)(b.pos >> 3);
            }
        }
        if (b.pos > (uint64_t)size * 8) {
            break;
        }
    } while (pss_next(&b, 32) == 0x1BA || (go_on && pss_starts_nothing(&b, size)));
    return consumed;
}

/* ---- PCM read-ahead for disc movies -------------------------------------------------------------- */
typedef struct {
    uint8_t header[40]; /* SShd (including its 24-byte body), then SSbd and the PCM byte count */
    uint32_t header_size;
    uint8_t *data;
    uint32_t size;
    uint32_t used;
} PssPcm;

static uint32_t pcm_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int pss_append_pcm(PssPcm *pcm, const uint8_t *data, uint32_t bytes, uint32_t file_size) {
    if (pcm->data == NULL) {
        uint32_t needed = sizeof(pcm->header) - pcm->header_size;
        uint32_t take = bytes < needed ? bytes : needed;
        const uint8_t *h = pcm->header;

        memcpy(pcm->header + pcm->header_size, data, take);
        pcm->header_size += take;
        if (pcm->header_size < sizeof(pcm->header)) {
            return 1;
        }
        if (memcmp(h, "SShd", 4) != 0 || pcm_le32(h + 4) != 24 || pcm_le32(h + 8) != 1 ||
            pcm_le32(h + 12) != 48000 || pcm_le32(h + 16) != 2 || pcm_le32(h + 20) != 512 ||
            memcmp(h + 32, "SSbd", 4) != 0) {
            return 0;
        }
        pcm->size = pcm_le32(h + 36);
        if (pcm->size == 0 || pcm->size > file_size || pcm->size % 1024 != 0) {
            return 0;
        }
        pcm->data = malloc(pcm->size);
        if (pcm->data == NULL) {
            return 0;
        }
        data += take;
        bytes -= take;
    }
    if (bytes > pcm->size - pcm->used) {
        bytes = pcm->size - pcm->used;
    }
    memcpy(pcm->data + pcm->used, data, bytes);
    pcm->used += bytes;
    return 1;
}

/* Read the packetized stream in disc order, retaining only PCM. A complete packet and the next disc
   read fit in the buffer even when a packet crosses a read boundary. */
uint8_t *port_mpeg_load_pcm(uint32_t lsn, uint32_t file_size, uint32_t *pcm_size) {
    enum { READ_BYTES = 64 * 1024, MAX_PACKET_BYTES = 6 + 65535 };
    uint8_t *buffer = malloc(READ_BYTES + MAX_PACKET_BYTES);
    uint32_t buffered = 0;
    PssPcm pcm = {0};

    if (buffer == NULL) {
        return NULL;
    }
    for (uint32_t offset = 0; offset < file_size;) {
        uint32_t take = file_size - offset < READ_BYTES ? file_size - offset : READ_BYTES;
        uint32_t sectors = (take + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE;
        uint32_t pos = 0;

        if (!port_disc_read(lsn + offset / DISC_SECTOR_SIZE, sectors, buffer + buffered)) {
            break;
        }
        offset += take;
        buffered += take;
        while (buffered - pos >= 6) {
            const uint8_t *p = buffer + pos;
            uint32_t available = buffered - pos;
            uint32_t packet_size;

            if (p[0] != 0 || p[1] != 0 || p[2] != 1 || p[3] == 0xb9) {
                goto done;
            }
            if (p[3] == 0xba) {
                if (available < 14) {
                    break;
                }
                packet_size = 14 + (p[13] & 7);
            } else {
                packet_size = 6 + ((uint32_t)p[4] << 8 | p[5]);
            }
            if (available < packet_size) {
                break;
            }
            if (p[3] == (PSS_PCM_STREAM >> 32) && packet_size >= 13) {
                uint32_t payload = 9 + p[8];

                if (payload + 4 <= packet_size && pcm_le32(p + payload) == 0x0000a0ff) {
                    if (!pss_append_pcm(&pcm, p + payload + 4, packet_size - payload - 4, file_size)) {
                        goto done;
                    }
                    if (pcm.data != NULL && pcm.used == pcm.size) {
                        *pcm_size = pcm.size;
                        free(buffer);
                        return pcm.data;
                    }
                }
            }
            pos += packet_size;
        }
        buffered -= pos;
        memmove(buffer, buffer + pos, buffered);
    }

done:
    free(buffer);
    free(pcm.data);
    return NULL;
}

/* ---- Pictures ----------------------------------------------------------------------------------- */
static uint8_t clamp_color(int value) {
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

/* The IPU's CSC: ITU-R BT.601 with its 8-bit coefficients (in 128ths: 1.164 0x95, 1.596 0xCC,
   0.813 0x68, 0.391 0x32, 2.016 0x102), alpha 0x80. Two adjacent pixels share chroma. */
static void convert_line(uint8_t *restrict out, const uint8_t *luma, const uint8_t *cb, const uint8_t *cr,
                         int width) {
    for (int x = 0; x < width; x += 2) {
        int blue = cb[x / 2] - 128;
        int red = cr[x / 2] - 128;
        int r = 0xCC * red + 64;
        int g = -0x68 * red - 0x32 * blue + 64;
        int b = 0x102 * blue + 64;

        for (int i = 0; i < 2; i++) {
            int y = luma[x + i];
            int lum = 0x95 * (y > 16 ? y - 16 : 0);

            out[0] = clamp_color((lum + r) >> 7);
            out[1] = clamp_color((lum + g) >> 7);
            out[2] = clamp_color((lum + b) >> 7);
            out[3] = 0x80;
            out += 4;
        }
    }
}

static void convert_picture(MpegState *s, const Mpeg2Frame *frame, uint8_t *rgb32, int mb_limit) {
    const Mpeg2Decoder *d = &s->decoder;
    int width = d->mb_width * 16;
    int n = 0;

    if (s->linear_output && mb_limit >= d->mb_width * d->mb_height) {
        for (int y = 0; y < d->mb_height * 16; y++) {
            size_t chroma = (size_t)(y / 2) * (width / 2);
            size_t luma = (size_t)y * width;

            convert_line(rgb32 + luma * 4, frame->y + luma, frame->cb + chroma, frame->cr + chroma, width);
        }
        return;
    }

    /* The IPU's macroblocks go down each column, then across. */
    for (int mb_x = 0; mb_x < d->mb_width; mb_x++) {
        for (int mb_y = 0; mb_y < d->mb_height; mb_y++) {
            uint8_t *out = rgb32 + (size_t)n * 0x400;

            if (n++ >= mb_limit) {
                return;
            }
            for (int y = 0; y < 16; y++) {
                int py = mb_y * 16 + y;
                const uint8_t *luma = frame->y + py * width + mb_x * 16;
                const uint8_t *cb = frame->cb + (py >> 1) * (width / 2) + mb_x * 8;
                const uint8_t *cr = frame->cr + (py >> 1) * (width / 2) + mb_x * 8;

                convert_line(out + y * 64, luma, cb, cr, 16);
            }
        }
    }
}

static void output_picture(sceMpeg *mp, int index, void *rgb32, int mb_limit) {
    MpegState *s = state_of(mp);

    mp->width = s->decoder.width;
    mp->height = s->decoder.height;
    mp->pts = s->pts[index];
    mp->dts = s->dts[index];
    s->pictures++;
    dispatch_simple(mp, CB_BACKGROUND);
    convert_picture(s, s->decoder.frames[index], rgb32, mb_limit);
}

int sceMpegGetPicture(sceMpeg *mp, void *rgb32, int mbcount) {
    MpegState *s = state_of(mp);
    Mpeg2Decoder *d = &s->decoder;

    s->is_end = 0;
    for (;;) {
        int type = mpeg2_next_picture(d);
        int shown;

        if (type == 0) {
            /* the sequence end code: the reference picture still held is the last one shown */
            s->is_end = 1;
            if (s->ref_count != 0 && s->out_state && d->backward >= 0) {
                mp->frameCount = s->total_frames;
                s->ref_count = 0;
                output_picture(mp, d->backward, rgb32, mbcount);
            }
            return 1;
        }
        s->out_state = 1;
        shown = type == MPEG2_B ? -1 : d->backward;
        {
            sceMpegCbDataTimeStamp stamp;

            memset(&stamp, 0, sizeof(stamp));
            stamp.type = CB_TIMESTAMP;
            stamp.pts = stamp.dts = -1;
            port_ipu_set_bp(mpeg2_unused_bits(d));
            dispatch(mp, &stamp);
            mpeg2_decode_picture(d);
            s->pts[d->current] = stamp.pts;
            s->dts[d->current] = stamp.dts;
        }
        if (type == MPEG2_B) {
            shown = d->current;
        }
        mp->frameCount = s->total_frames;
        s->total_frames++;
        s->ref_count++;
        if (shown >= 0) {
            output_picture(mp, shown, rgb32, mbcount);
            return 1;
        }
    }
}
