/* Finer sign pictures from the mods (port/include/port/signs.h): the pictures the game loaded, known by their
   indices' fingerprint, and each one's signs/mNNNN.png read once, the first time the renderer asks. PNG is read here
   (inflated by miniz, which libchdr brings): 8 bits a channel, grey, RGB, palette, grey and alpha or RGBA, not
   interlaced. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/mods.h"
#include "port/signs.h"

/* miniz (third_party/libchdr/deps/miniz-3.1.2), built into libchdr */
void *tinfl_decompress_mem_to_heap(const void *source, size_t source_length, size_t *out_length, int flags);
void mz_free(void *block);
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1

#define SIGN_W 512
#define SIGN_H 256
#define KNOWN 16 /* pictures kept by fingerprint, the last loaded first */

typedef struct {
    uint64_t fingerprint;
    int id;
    int tried;          /* its PNG looked for */
    uint32_t *rgba;     /* the finer picture, NULL none */
    uint32_t scale;
} Known;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Known known[KNOWN];
static int known_count;

uint64_t port_sign_fingerprint(const uint8_t *indices, size_t count) {
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;

    for (i = 0; i < count; i++) {
        h = (h ^ indices[i]) * 0x100000001B3ull;
    }
    return h;
}

void port_sign_loaded(int id, const void *file, size_t size) {
    const uint8_t *data = (const uint8_t *)file;
    static uint8_t indices[SIGN_W * SIGN_H];
    uint64_t tex0, fingerprint;
    uint32_t psm, width, height, i;
    int k;

    if (size < 0x20) {
        return;
    }
    memcpy(&tex0, data + 0x10, 8);
    psm = (uint32_t)(tex0 >> 20) & 0x3F;
    width = 1u << ((tex0 >> 26) & 15);
    height = 1u << ((tex0 >> 30) & 15);
    if (width != SIGN_W || height != SIGN_H) {
        return;
    }
    if (psm == 0x14) { /* PSMT4: two a byte, the first in the low four bits */
        if (size < 0x20 + SIGN_W * SIGN_H / 2) {
            return;
        }
        for (i = 0; i < SIGN_W * SIGN_H; i++) {
            uint8_t b = data[0x20 + i / 2];
            indices[i] = (i & 1) ? b >> 4 : b & 15;
        }
    } else if (psm == 0x13) { /* PSMT8 */
        if (size < 0x20 + SIGN_W * SIGN_H) {
            return;
        }
        memcpy(indices, data + 0x20, SIGN_W * SIGN_H);
    } else {
        return;
    }
    fingerprint = port_sign_fingerprint(indices, sizeof indices);
    pthread_mutex_lock(&lock);
    for (k = 0; k < known_count; k++) {
        if (known[k].fingerprint == fingerprint && known[k].id == id) {
            break;
        }
    }
    if (k == known_count) {
        if (known_count == KNOWN) { /* (the oldest let go: its picture kept, as a renderer may still hold it) */
            memmove(&known[0], &known[1], sizeof(Known) * (KNOWN - 1));
            known_count--;
        }
        k = known_count++;
        memset(&known[k], 0, sizeof(Known));
        known[k].fingerprint = fingerprint;
        known[k].id = id;
    }
    pthread_mutex_unlock(&lock);
}

/* ---- PNG ------------------------------------------------------------------------------------------------------ */
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* The PNG at path as RGBA (r in the low byte, the GS's alpha), *w x *h; NULL if it cannot be read */
static uint32_t *read_png(const char *path, uint32_t *w, uint32_t *h) {
    static const uint8_t magic[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    FILE *f = fopen(path, "rb");
    uint8_t *file = NULL, *idat = NULL, *raw = NULL, palette[256 * 4];
    size_t length, idat_length = 0, raw_length = 0, at;
    uint32_t width = 0, height = 0, depth = 0, colour = 0, interlace = 0, channels, stride, x, y;
    uint32_t *out = NULL;
    int i;

    for (i = 0; i < 256; i++) {
        palette[i * 4] = palette[i * 4 + 1] = palette[i * 4 + 2] = 0;
        palette[i * 4 + 3] = 255;
    }
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    length = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc(length);
    if (file == NULL || fread(file, 1, length, f) != length || length < 8 || memcmp(file, magic, 8) != 0) {
        goto done;
    }
    idat = malloc(length);
    for (at = 8; at + 12 <= length;) {
        uint32_t size = be32(file + at);
        const uint8_t *type = file + at + 4, *body = file + at + 8;

        if (at + 12 + (size_t)size > length) {
            goto done;
        }
        if (memcmp(type, "IHDR", 4) == 0 && size >= 13) {
            width = be32(body), height = be32(body + 4);
            depth = body[8], colour = body[9], interlace = body[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            for (i = 0; i < 256 && (uint32_t)i * 3 + 2 < size; i++) {
                memcpy(palette + i * 4, body + i * 3, 3);
            }
        } else if (memcmp(type, "tRNS", 4) == 0 && colour == 3) {
            for (i = 0; i < 256 && (uint32_t)i < size; i++) {
                palette[i * 4 + 3] = body[i];
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            memcpy(idat + idat_length, body, size);
            idat_length += size;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at += 12 + size;
    }
    channels = colour == 0 ? 1 : colour == 2 ? 3 : colour == 3 ? 1 : colour == 4 ? 2 : colour == 6 ? 4 : 0;
    if (depth != 8 || channels == 0 || interlace != 0 || width == 0 || height == 0 || width > 8192 || height > 8192) {
        fprintf(stderr, "[signs] %s: not an 8-bit, not interlaced PNG\n", path);
        goto done;
    }
    raw = tinfl_decompress_mem_to_heap(idat, idat_length, &raw_length, TINFL_FLAG_PARSE_ZLIB_HEADER);
    stride = width * channels;
    if (raw == NULL || raw_length < (size_t)(stride + 1) * height) {
        fprintf(stderr, "[signs] %s: its picture data cannot be read\n", path);
        goto done;
    }
    /* the rows' filters undone in place (each row: its filter byte, then the row) */
    for (y = 0; y < height; y++) {
        uint8_t *row = raw + (size_t)y * (stride + 1), filter = row[0], *p = row + 1;
        const uint8_t *up = y > 0 ? raw + (size_t)(y - 1) * (stride + 1) + 1 : NULL;

        for (x = 0; x < stride; x++) {
            int a = x >= channels ? p[x - channels] : 0, b = up ? up[x] : 0, c = up && x >= channels ? up[x - channels] : 0;

            switch (filter) {
            case 1: p[x] = (uint8_t)(p[x] + a); break;
            case 2: p[x] = (uint8_t)(p[x] + b); break;
            case 3: p[x] = (uint8_t)(p[x] + (a + b) / 2); break;
            case 4: p[x] = (uint8_t)(p[x] + paeth(a, b, c)); break;
            default: break;
            }
        }
    }
    out = malloc((size_t)width * height * 4);
    for (y = 0; y < height && out != NULL; y++) {
        const uint8_t *p = raw + (size_t)y * (stride + 1) + 1;

        for (x = 0; x < width; x++, p += channels) {
            uint32_t r, g, b, a;

            switch (colour) {
            case 0: r = g = b = p[0], a = 255; break;
            case 2: r = p[0], g = p[1], b = p[2], a = 255; break;
            case 3: r = palette[p[0] * 4], g = palette[p[0] * 4 + 1], b = palette[p[0] * 4 + 2], a = palette[p[0] * 4 + 3]; break;
            case 4: r = g = b = p[0], a = p[1]; break;
            default: r = p[0], g = p[1], b = p[2], a = p[3]; break;
            }
            a = (a * 128 + 127) / 255; /* (the GS's alpha: 0x80 opaque) */
            out[(size_t)y * width + x] = r | g << 8 | b << 16 | a << 24;
        }
    }
    *w = width;
    *h = height;
done:
    if (raw != NULL) {
        mz_free(raw);
    }
    free(idat);
    free(file);
    fclose(f);
    return out;
}

int port_sign_finer(uint64_t fingerprint, const uint32_t **rgba, uint32_t *scale) {
    int k, found = 0;

    pthread_mutex_lock(&lock);
    for (k = known_count - 1; k >= 0; k--) {
        if (known[k].fingerprint == fingerprint) {
            break;
        }
    }
    if (k >= 0 && !known[k].tried) {
        char name[32], member[32];
        const char *path;

        known[k].tried = 1;
        snprintf(name, sizeof name, "signs/m%04d.png", known[k].id);
        snprintf(member, sizeof member, "msg/m%04d.tm2", known[k].id);
        path = port_mods_find_with(name, member); /* (the one made with the picture in use) */
        if (path != NULL) {
            uint32_t w = 0, h = 0, *picture = read_png(path, &w, &h);

            if (picture != NULL && w % SIGN_W == 0 && h == w / 2 && w / SIGN_W >= 1 && w / SIGN_W <= 8) {
                known[k].rgba = picture;
                known[k].scale = w / SIGN_W;
                fprintf(stderr, "[signs] %s: %ux%u, %u times as fine\n", path, w, h, known[k].scale);
            } else {
                if (picture != NULL) {
                    fprintf(stderr, "[signs] %s: %ux%u is not 512 x 256 times 1 to 8\n", path, w, h);
                }
                free(picture);
            }
        }
    }
    if (k >= 0 && known[k].rgba != NULL) {
        *rgba = known[k].rgba;
        *scale = known[k].scale;
        found = 1;
    }
    pthread_mutex_unlock(&lock);
    return found;
}
