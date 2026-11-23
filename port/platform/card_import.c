/* Card import (port/include/port/card_import.h): the game's saves from a PS2 emulator's memory card into the save
   folder. First written by Solaire (PR #238).

   The card image. Pages of 512 bytes, each followed by 16 bytes of ECC in an image of 528-byte pages (the
   ECC is not read); two pages to a 1 KB cluster. The superblock (page 0) gives the first cluster of the
   allocatable area (alloc_offset), the root directory's cluster in that area and the indirect FAT clusters
   (ifc_list): each of those holds 256 cluster numbers of FAT clusters, each FAT cluster 256 entries, one per
   cluster of the area: bit 31 set when the cluster is in use, the next cluster of the file in bits 0-30
   (0x7FFFFFFF: the file's last). A directory is a file of 512-byte entries: mode (0x8000 the entry is in use,
   0x20 a directory, 0x10 a file), the length (a file's bytes, a directory's entries), the first cluster and,
   at 0x40, the name.

   A folder card (PCSX2's "folder" memory card): the card's directories are folders and its files plain files, so
   the game's files are read as they are.

   The game's files (save/save_card.c, save/mc.c). In the folder /BASLUS-20318S: the list BASLUS-20318S
   (the three saves' 0x180-byte records, SaveInfo) and save N in BASLUS-20318G0N, each written by
   Mc_StartWriteChecked: a u32 of the data's size, then the data in 1 KB blocks (0x3FB bytes in the first,
   0x3FF in each after it), each block's bytes followed by the sum of their complements; after the last
   block's sum a 1 (the end mark), then zeros to a multiple of 0x20. A save's data is the save image as
   Game_WriteSaveImage made it, its first word its size: the same bytes save_files.c keeps. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/card_import.h"

#define PAGE_SIZE 512u
#define CLUSTER_SIZE 1024u
#define ENTRY_SIZE 512u
#define LIST_SIZE (PORT_SAVE_COUNT * PORT_SAVE_RECORD_SIZE)
#define MODE_IN_USE 0x8000u
#define MODE_FOLDER 0x0020u
#define MODE_FILE 0x0010u
#define GAME_FOLDER "BASLUS-20318S"
#define LIST_FILE "BASLUS-20318S"
/* the most a checked file of `capacity` bytes of data can take: the size word, a sum per block, the end mark and
   the zeros after it */
#define CHECKED_MAX(capacity) ((capacity) + (capacity) / 0x3FBu + 0x40u)

/* SaveInfo (save/save_card.h): the level is numbers[2], the play time's bytes at 0x178 (PlayTime_ComputeCurrent:
   1 seconds, 2 minutes, 3 hours, 5 hundreds of 99 hours) */
#define RECORD_LEVEL 0x0C
#define RECORD_PLAY_TIME 0x178

typedef struct {
    const uint8_t *image;
    size_t size;
    size_t page_stride;     /* 512, or 528 with ECC */
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

/* ---- A checked file (mc.c's Mc_StartWriteChecked) ------------------------------------------------------------- */
/* `data` gets at most `capacity` bytes. Returns the data's size, or 0 when the file is not whole (cut short, a
   block's sum wrong, no end mark) or does not fit */
static uint32_t unpack_checked(const uint8_t *file, size_t length, uint8_t *data, uint32_t capacity) {
    uint32_t size, done = 0, block, n, i;
    size_t at = 4;
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

/* ---- A card image -------------------------------------------------------------------------------------------- */
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
static uint8_t *read_chain(const Card *card, uint32_t first, uint32_t length) {
    uint32_t clusters = (length + CLUSTER_SIZE - 1u) / CLUSTER_SIZE;
    uint32_t n = first, i, entry;
    uint8_t *out;

    if (clusters > card->cluster_count - card->alloc_offset ||
        (out = malloc((size_t)clusters * CLUSTER_SIZE + 1u)) == NULL) {
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
        (entries = read_chain(card, folder->cluster, folder->length * ENTRY_SIZE)) == NULL) {
        return 0;
    }
    for (i = 0; i < folder->length && !ok; i++) {
        read_entry(entries + (size_t)i * ENTRY_SIZE, found);
        ok = (found->mode & (MODE_IN_USE | mode)) == (MODE_IN_USE | mode) && strcmp(found->name, name) == 0;
    }
    free(entries);
    return ok;
}

/* Reads checked file `name` from the game's folder on the image into data; its size, or 0 */
static uint32_t image_checked(const Card *card, const Entry *folder, const char *name, uint8_t *data,
                              uint32_t capacity) {
    Entry entry;
    uint8_t *file;
    uint32_t size;

    if (!find(card, folder, name, MODE_FILE, &entry) || entry.length > CHECKED_MAX(capacity) ||
        (file = read_chain(card, entry.cluster, entry.length)) == NULL) {
        return 0;
    }
    size = unpack_checked(file, entry.length, data, capacity);
    free(file);
    return size;
}

/* ---- Where the game's files are: on an image, or in a folder card ---------------------------------------------- */
typedef struct {
    const Card *card;   /* an image: its game folder's entry */
    Entry folder;
    const char *path;   /* else the folder card's folder */
} GameFiles;

/* Reads checked file `name` of the game into data; its size, or 0 */
static uint32_t read_checked(const GameFiles *files, const char *name, uint8_t *data, uint32_t capacity) {
    char path[4096];
    FILE *file;
    uint8_t *bytes;
    size_t length;
    uint32_t size = 0;

    if (files->card != NULL) {
        return image_checked(files->card, &files->folder, name, data, capacity);
    }
    if ((size_t)snprintf(path, sizeof(path), "%s/" GAME_FOLDER "/%s", files->path, name) >= sizeof(path) ||
        (file = fopen(path, "rb")) == NULL) {
        return 0;
    }
    if ((bytes = malloc(CHECKED_MAX(capacity) + 1u)) != NULL) {
        length = fread(bytes, 1, CHECKED_MAX(capacity) + 1u, file);
        size = length <= CHECKED_MAX(capacity) ? unpack_checked(bytes, length, data, capacity) : 0;
        free(bytes);
    }
    fclose(file);
    return size;
}

/* The saves the list names, from the game's files */
static PortCardResult read_saves(const GameFiles *files, PortCardSaves *saves) {
    uint8_t list[LIST_SIZE];
    char name[32];
    int index;

    if (read_checked(files, LIST_FILE, list, LIST_SIZE) != LIST_SIZE) {
        return PORT_CARD_DAMAGED;
    }
    for (index = 0; index < PORT_SAVE_COUNT; index++) {
        const uint8_t *record = list + index * PORT_SAVE_RECORD_SIZE;
        PortCardSave *save = &saves->saves[saves->count];
        uint32_t size;

        if (record[0] == 0) { /* SaveInfo.exists */
            continue;
        }
        snprintf(name, sizeof(name), "BASLUS-20318G%02d", index);
        size = read_checked(files, name, save->data, sizeof(save->data));
        if (record[1] != 0 || size < 8 || get32(save->data) != size) { /* SaveInfo.damaged; the image's first word */
            saves->damaged++;
            continue;
        }
        save->number = index;
        memcpy(save->record, record, PORT_SAVE_RECORD_SIZE);
        save->size = size;
        saves->count++;
    }
    return PORT_CARD_OK;
}

/* A folder card: 1 when `path` holds the game's folder with its list */
static int is_folder_card(const char *path) {
    char list[4096];
    FILE *file;

    if ((size_t)snprintf(list, sizeof(list), "%s/" GAME_FOLDER "/" LIST_FILE, path) >= sizeof(list) ||
        (file = fopen(list, "rb")) == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

/* The whole of a file (malloc'd), or NULL; *size its length */
static uint8_t *read_whole(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    uint8_t *bytes = NULL;
    long length;

    if (file == NULL) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0 &&
        (bytes = malloc((size_t)length)) != NULL && fread(bytes, (size_t)length, 1, file) != 1) {
        free(bytes);
        bytes = NULL;
    }
    if (bytes != NULL) {
        *size = (size_t)length;
    }
    fclose(file);
    return bytes;
}

static PortCardResult read_image(const uint8_t *image, size_t size, PortCardSaves *saves) {
    uint8_t cluster[CLUSTER_SIZE];
    Card card;
    Entry root;
    GameFiles files;
    uint32_t i;

    if (size < PAGE_SIZE || memcmp(image, "Sony PS2 Memory Card Format ", 28) != 0 ||
        get32(image + 0x28) != (PAGE_SIZE | 2u << 16)) { /* page_len 512, pages_per_cluster 2 */
        return PORT_CARD_NOT_CARD;
    }
    memset(&card, 0, sizeof(card));
    card.image = image;
    card.size = size;
    card.page_stride = size % (528u * 1024u) == 0 ? 528u : PAGE_SIZE;
    card.cluster_count = (uint32_t)(size / (card.page_stride * 2u));
    card.alloc_offset = get32(image + 0x34);
    for (i = 0; i < 32; i++) {
        card.ifc[i] = get32(image + 0x50 + i * 4);
    }
    if (card.alloc_offset >= card.cluster_count ||
        !read_cluster(&card, card.alloc_offset + get32(image + 0x3C), cluster)) {
        return PORT_CARD_NOT_CARD;
    }
    read_entry(cluster, &root); /* the root's "." entry gives the root's length */
    root.cluster = get32(image + 0x3C);
    memset(&files, 0, sizeof(files));
    files.card = &card;
    if (!find(&card, &root, GAME_FOLDER, MODE_FOLDER, &files.folder)) {
        return PORT_CARD_NO_GAME;
    }
    return read_saves(&files, saves);
}

PortCardResult port_card_read(const char *path, PortCardSaves *saves) {
    PortCardResult result;
    uint8_t *image;
    size_t size = 0;

    memset(saves, 0, sizeof(*saves));
    if (is_folder_card(path)) {
        GameFiles files;

        memset(&files, 0, sizeof(files));
        files.path = path;
        return read_saves(&files, saves);
    }
    if ((image = read_whole(path, &size)) == NULL) {
        return PORT_CARD_UNREADABLE;
    }
    result = read_image(image, size, saves);
    free(image);
    return result;
}

/* ---- Into the save folder ------------------------------------------------------------------------------------- */
int port_card_save_level(const PortCardSave *save) {
    return (int)get32(save->record + RECORD_LEVEL);
}

int port_card_save_minutes(const PortCardSave *save) {
    const uint8_t *time = save->record + RECORD_PLAY_TIME;

    return time[2] + (time[3] + time[5] * 99) * 60;
}

int port_card_save_held_in(const PortCardSave *save) {
    uint8_t record[PORT_SAVE_RECORD_SIZE];
    uint8_t *data = malloc(PORT_SAVE_DATA_MAX);
    size_t size;
    int index, held = -1;

    if (data == NULL) {
        return -1;
    }
    for (index = 0; index < PORT_SAVE_COUNT && held < 0; index++) {
        if (port_save_read(index, record, data, PORT_SAVE_DATA_MAX, &size) == PORT_SAVE_OK && size == save->size &&
            memcmp(data, save->data, size) == 0) {
            held = index;
        }
    }
    free(data);
    return held;
}

int port_card_save_slot(const PortCardSave *save) {
    int index;

    if (!port_saves_available()) {
        return -1;
    }
    if (port_save_record(save->number, NULL) == PORT_SAVE_EMPTY) {
        return save->number;
    }
    for (index = 0; index < PORT_SAVE_COUNT; index++) {
        if (port_save_record(index, NULL) == PORT_SAVE_EMPTY) {
            return index;
        }
    }
    return -1;
}

int port_card_save_import(const PortCardSave *save) {
    int slot = port_card_save_slot(save);

    if (slot < 0 || !port_save_write(slot, save->record, save->data, save->size)) {
        return -1;
    }
    return slot;
}

static const char *result_text(PortCardResult result) {
    switch (result) {
    case PORT_CARD_UNREADABLE:
        return "cannot be read";
    case PORT_CARD_NOT_CARD:
        return "is not a PS2 memory card (or the card was never formatted)";
    case PORT_CARD_NO_GAME:
        return "has no King's Field IV saves (no BASLUS-20318S folder)";
    case PORT_CARD_DAMAGED:
        return "has King's Field IV's folder, but its save list is missing or damaged";
    case PORT_CARD_OK:
        break;
    }
    return "was read";
}

int port_saves_import_card(const char *path) {
    PortCardSaves *saves;
    PortCardResult result;
    int i, imported = 0;

    if (!port_saves_available()) {
        fprintf(stderr, "[saves] import: there is no save folder (--saves none)\n");
        return -1;
    }
    if ((saves = malloc(sizeof(*saves))) == NULL) {
        return -1;
    }
    result = port_card_read(path, saves);
    if (result != PORT_CARD_OK) {
        fprintf(stderr, "[saves] import: %s %s\n", path, result_text(result));
        free(saves);
        return -1;
    }
    if (saves->damaged != 0) {
        fprintf(stderr, "[saves] import: %d save(s) on the card are damaged; not imported\n", saves->damaged);
    }
    for (i = 0; i < saves->count; i++) {
        const PortCardSave *save = &saves->saves[i];
        int held = port_card_save_held_in(save), slot;

        if (held >= 0) {
            fprintf(stderr, "[saves] import: card save %d is already save %d\n", save->number + 1, held + 1);
            continue;
        }
        if ((slot = port_card_save_import(save)) < 0) {
            fprintf(stderr, "[saves] import: card save %d not imported: %s\n", save->number + 1,
                    port_card_save_slot(save) < 0 ? "no free save in the folder (saves are never replaced)"
                                                  : port_saves_message());
            continue;
        }
        fprintf(stderr, "[saves] import: card save %d imported as save %d\n", save->number + 1, slot + 1);
        imported++;
    }
    free(saves);
    return imported;
}
