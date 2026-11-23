/* Card import (port/include/port/save_files.h, port_saves_import_card): the game's saves from a PS2 memory card
   image, as PCSX2 and RetroArch's PS2 cores keep a card (Mcd001.ps2), into the save folder. Plain C on the C
   library and save_files.c only, so tests/save_files_test.c builds it with that file.

   The card image. Pages of 512 bytes, each followed by 16 bytes of ECC in an image of 528-byte pages (the
   ECC is not read); two pages to a 1 KB cluster. The superblock (page 0) gives the first cluster of the
   allocatable area (alloc_offset), the root directory's cluster in that area and the indirect FAT clusters
   (ifc_list): each of those holds 256 cluster numbers of FAT clusters, each FAT cluster 256 entries, one per
   cluster of the area: bit 31 set when the cluster is in use, the next cluster of the file in bits 0-30
   (0x7FFFFFFF: the file's last). A directory is a file of 512-byte entries: mode (0x8000 the entry is in use,
   0x20 a directory, 0x10 a file), the length (a file's bytes, a directory's entries), the first cluster and,
   at 0x40, the name.

   The game's files (save/save_card.c, save/mc.c). In the folder /BASLUS-20318S: the list BASLUS-20318S
   (the three saves' 0x180-byte records, SaveInfo) and save N in BASLUS-20318G0N, each written by
   Mc_StartWriteChecked: a u32 of the data's size, then the data in 1 KB blocks (0x3FB bytes in the first,
   0x3FF in each after it), each block's bytes followed by the sum of their complements; after the last
   block's sum a 1 (the end mark), then zeros to a multiple of 0x20. A save's data is the save image as
   Game_WriteSaveImage made it, its first word its size: the same bytes save_files.c keeps.

   The card image is a file from outside: every number read from it is checked before it is used. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/save_files.h"

#define PAGE_SIZE 512u
#define CLUSTER_SIZE 1024u
#define ENTRY_SIZE 512u
#define LIST_SIZE (PORT_SAVE_COUNT * PORT_SAVE_RECORD_SIZE)
#define MODE_IN_USE 0x8000u
#define MODE_FOLDER 0x0020u
#define MODE_FILE 0x0010u

typedef struct {
    const uint8_t *image;
    size_t size;
    size_t page_stride;    /* 512, or 528 with ECC */
    uint32_t cluster_count; /* whole clusters in the image */
    uint32_t alloc_offset;
    uint32_t ifc[32];
} Card;

typedef struct {
    uint32_t mode, length, cluster;
    char name[33];
} Entry;

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Copies absolute cluster `n` to out (CLUSTER_SIZE bytes). 0 when it is past the image's end */
static int read_cluster(const Card *card, uint32_t n, uint8_t *out) {
    size_t page;

    if (n >= card->cluster_count) {
        return 0;
    }
    for (page = 0; page < CLUSTER_SIZE / PAGE_SIZE; page++) {
        memcpy(out + page * PAGE_SIZE, card->image + ((size_t)n * 2 + page) * card->page_stride, PAGE_SIZE);
    }
    return 1;
}

/* The FAT entry of cluster `n` of the area; 0 when it cannot be read */
static int fat_entry(const Card *card, uint32_t n, uint32_t *entry) {
    uint8_t cluster[CLUSTER_SIZE];
    uint32_t fat_index = n / 256u;

    if (fat_index / 256u >= 32u || !read_cluster(card, card->ifc[fat_index / 256u], cluster) ||
        !read_cluster(card, get32(cluster + (fat_index % 256u) * 4u), cluster)) {
        return 0;
    }
    *entry = get32(cluster + (n % 256u) * 4u);
    return 1;
}

/* The first `length` bytes of the file whose chain starts at cluster `first` of the area (malloc'd: the caller
   frees it), or NULL when the chain is broken or shorter */
static uint8_t *read_file(const Card *card, uint32_t first, uint32_t length) {
    uint32_t clusters = (length + CLUSTER_SIZE - 1u) / CLUSTER_SIZE;
    uint8_t *out = malloc((size_t)clusters * CLUSTER_SIZE + 1u);
    uint32_t n = first, i, entry;

    if (out == NULL || clusters > card->cluster_count) {
        free(out);
        return NULL;
    }
    for (i = 0; i < clusters; i++) {
        if (n >= card->cluster_count - card->alloc_offset ||
            !read_cluster(card, card->alloc_offset + n, out + (size_t)i * CLUSTER_SIZE) || !fat_entry(card, n, &entry) ||
            !(entry & 0x80000000u)) {
            free(out);
            return NULL;
        }
        n = entry & 0x7FFFFFFFu;
    }
    return out;
}

static void read_entry(const uint8_t *p, Entry *entry) {
    entry->mode = (uint32_t)p[0] | (uint32_t)p[1] << 8;
    entry->length = get32(p + 4);
    entry->cluster = get32(p + 0x10);
    memcpy(entry->name, p + 0x40, 32);
    entry->name[32] = '\0';
}

/* Finds `name` in the directory `folder`; 1 with its entry */
static int find(const Card *card, const Entry *folder, const char *name, uint32_t mode, Entry *found) {
    uint8_t *entries;
    uint32_t i;
    int ok = 0;

    if (folder->length > card->cluster_count * (CLUSTER_SIZE / ENTRY_SIZE) ||
        (entries = read_file(card, folder->cluster, folder->length * ENTRY_SIZE)) == NULL) {
        return 0;
    }
    for (i = 0; i < folder->length && !ok; i++) {
        read_entry(entries + (size_t)i * ENTRY_SIZE, found);
        ok = (found->mode & (MODE_IN_USE | mode)) == (MODE_IN_USE | mode) && strcmp(found->name, name) == 0;
    }
    free(entries);
    return ok;
}

/* A checked file's data (mc.c's Mc_StartWriteChecked): `data` gets at most `capacity` bytes. Returns the data's
   size, or 0 when the file is not whole (cut short, a block's sum wrong, no end mark) or does not fit */
static uint32_t unpack_checked(const uint8_t *file, uint32_t length, uint8_t *data, uint32_t capacity) {
    uint32_t size, done = 0, at = 4, block, n, i;
    uint8_t sum;

    if (length < 4 || (size = get32(file)) == 0 || size > capacity) {
        return 0;
    }
    for (block = 0;; block++) {
        uint32_t full = block == 0 ? 0x3FBu : 0x3FFu;

        n = size - done < full ? size - done : full;
        if (at + n + 2u > length) {
            return 0;
        }
        for (sum = 0, i = 0; i < n; i++) {
            sum = (uint8_t)(sum + (uint8_t)~file[at + i]);
        }
        if (n != 0 && file[at + n] != sum) {
            return 0;
        }
        memcpy(data + done, file + at, n);
        done += n;
        at += n + (n != 0);
        if (n < full) {
            return file[at] == 1 ? size : 0;
        }
    }
}

/* Reads checked file `name` from the game's folder into data; its size, or 0 */
static uint32_t read_checked(const Card *card, const Entry *folder, const char *name, uint8_t *data,
                             uint32_t capacity) {
    Entry entry;
    uint8_t *file;
    uint32_t size;

    if (!find(card, folder, name, MODE_FILE, &entry) || entry.length > capacity + capacity / 0x3FFu + 0x40u ||
        (file = read_file(card, entry.cluster, entry.length)) == NULL) {
        return 0;
    }
    size = unpack_checked(file, entry.length, data, capacity);
    free(file);
    return size;
}

static int import(const Card *card) {
    static uint8_t data[PORT_SAVE_DATA_MAX];
    uint8_t list[LIST_SIZE];
    uint8_t cluster[CLUSTER_SIZE];
    const uint8_t *sb = card->image;
    Entry root, folder;
    char name[32];
    uint32_t size;
    int index, imported = 0;

    if (!read_cluster(card, card->alloc_offset + get32(sb + 0x3C), cluster)) {
        fprintf(stderr, "[saves] import: the card's root directory is past the image's end\n");
        return -1;
    }
    read_entry(cluster, &root); /* the root's "." entry gives the root's length */
    root.cluster = get32(sb + 0x3C);
    if (!find(card, &root, "BASLUS-20318S", MODE_FOLDER, &folder)) {
        fprintf(stderr, "[saves] import: no King's Field IV (BASLUS-20318S) folder on the card\n");
        return -1;
    }
    if (read_checked(card, &folder, "BASLUS-20318S", list, LIST_SIZE) != LIST_SIZE) {
        fprintf(stderr, "[saves] import: the card's save list (BASLUS-20318S) is missing or damaged\n");
        return -1;
    }
    for (index = 0; index < PORT_SAVE_COUNT; index++) {
        const uint8_t *record = list + index * PORT_SAVE_RECORD_SIZE;

        if (record[0] == 0) { /* SaveInfo.exists */
            continue;
        }
        snprintf(name, sizeof(name), "BASLUS-20318G%02d", index);
        size = read_checked(card, &folder, name, data, sizeof(data));
        if (record[1] != 0 || size < 8 || get32(data) != size) { /* SaveInfo.damaged; the image's first word */
            fprintf(stderr, "[saves] import: save %d on the card is damaged; not imported\n", index + 1);
            continue;
        }
        if (port_save_record(index, NULL) != PORT_SAVE_EMPTY) {
            fprintf(stderr, "[saves] import: save %d is already in %s; not replaced (move it away to import)\n",
                    index + 1, port_saves_folder());
            continue;
        }
        if (!port_save_write(index, record, data, size)) {
            fprintf(stderr, "[saves] import: save %d: %s\n", index + 1, port_saves_message());
            continue;
        }
        fprintf(stderr, "[saves] import: save %d imported\n", index + 1);
        imported++;
    }
    return imported;
}

int port_saves_import_card(const char *path) {
    FILE *file;
    long length;
    uint8_t *image = NULL;
    Card card;
    uint32_t i;
    int result = -1;

    if (!port_saves_available()) {
        fprintf(stderr, "[saves] import: there is no save folder (--saves none)\n");
        return -1;
    }
    if ((file = fopen(path, "rb")) == NULL || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[saves] import: cannot read %s\n", path);
        goto done;
    }
    memset(&card, 0, sizeof(card));
    card.size = (size_t)length;
    card.page_stride = card.size % 528u == 0 && card.size % (528u * 1024u) == 0 ? 528u : PAGE_SIZE;
    card.cluster_count = (uint32_t)(card.size / (card.page_stride * 2u));
    if (card.size < PAGE_SIZE || (image = malloc(card.size)) == NULL || fread(image, card.size, 1, file) != 1) {
        fprintf(stderr, "[saves] import: cannot read %s\n", path);
        goto done;
    }
    card.image = image;
    if (memcmp(image, "Sony PS2 Memory Card Format ", 28) != 0) {
        fprintf(stderr, "[saves] import: %s is not a PS2 memory card image (or the card was never formatted)\n", path);
        goto done;
    }
    if (get32(image + 0x28) != (PAGE_SIZE | 2u << 16)) { /* page_len 512, pages_per_cluster 2 */
        fprintf(stderr, "[saves] import: a card of pages other than 512 bytes, two to a cluster\n");
        goto done;
    }
    card.alloc_offset = get32(image + 0x34);
    for (i = 0; i < 32; i++) {
        card.ifc[i] = get32(image + 0x50 + i * 4);
    }
    result = card.alloc_offset < card.cluster_count ? import(&card) : -1;
done:
    free(image);
    if (file != NULL) {
        fclose(file);
    }
    return result;
}
