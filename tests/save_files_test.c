/* port/platform/save_files.c: the saves as files: written and read back, a write that fails or is cut
   short leaves the save it replaces whole, and what a crash leaves is mended when the folder is opened.

     save_files_test [FOLDER]   (a fresh folder of its own under the system's temporary folder by default) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "port/card_import.h"
#include "port/save_files.h"

static int failed, checked;
static char folder[512];

static void check(const char *what, int ok) {
    checked++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static void path_of(char *out, size_t size, int number, const char *suffix) {
    snprintf(out, size, "%s/save%d.kf4save%s", folder, number, suffix);
}

static int exists(int number, const char *suffix) {
    char path[600];
    struct stat st;

    path_of(path, sizeof(path), number, suffix);
    return stat(path, &st) == 0;
}

static void remove_all(void) {
    static const char *suffixes[] = {"", ".tmp", ".bak"};
    char path[600];
    int number;
    size_t i;

    for (number = 1; number <= PORT_SAVE_COUNT; number++) {
        for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            path_of(path, sizeof(path), number, suffixes[i]);
            remove(path);
        }
    }
}

/* a record and data that say which save they are */
static void make_save(uint8_t *record, uint8_t *data, size_t size, int mark) {
    size_t i;

    memset(record, mark, PORT_SAVE_RECORD_SIZE);
    for (i = 0; i < size; i++) {
        data[i] = (uint8_t)(i * 7 + (size_t)mark);
    }
}

static int reads_as(int index, int mark, size_t size) {
    static uint8_t want_record[PORT_SAVE_RECORD_SIZE], want_data[PORT_SAVE_DATA_MAX];
    static uint8_t record[PORT_SAVE_RECORD_SIZE], data[PORT_SAVE_DATA_MAX];
    size_t got = 0;

    make_save(want_record, want_data, size, mark);
    return port_save_read(index, record, data, sizeof(data), &got) == PORT_SAVE_OK && got == size &&
           memcmp(record, want_record, sizeof(record)) == 0 && memcmp(data, want_data, size) == 0;
}

static int write_save(int index, int mark, size_t size) {
    static uint8_t record[PORT_SAVE_RECORD_SIZE], data[PORT_SAVE_DATA_MAX + 1]; /* + 1: the too big write */

    make_save(record, data, size, mark);
    return port_save_write(index, record, data, size);
}

/* overwrites one byte of a file */
static void poke(int number, const char *suffix, long offset) {
    char path[600];
    FILE *file;

    path_of(path, sizeof(path), number, suffix);
    file = fopen(path, "r+b");
    if (file == NULL) {
        return;
    }
    fseek(file, offset, SEEK_SET);
    fputc(0x5A, file);
    fclose(file);
}

static void copy_file(int from, const char *from_suffix, int to, const char *to_suffix) {
    char a[600], b[600];
    char buffer[4096];
    size_t n;
    FILE *in, *out;

    path_of(a, sizeof(a), from, from_suffix);
    path_of(b, sizeof(b), to, to_suffix);
    in = fopen(a, "rb");
    out = fopen(b, "wb");
    while (in != NULL && out != NULL && (n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        fwrite(buffer, 1, n, out);
    }
    if (in != NULL) {
        fclose(in);
    }
    if (out != NULL) {
        fclose(out);
    }
}

static void round_trip(void) {
    uint8_t record[PORT_SAVE_RECORD_SIZE];
    uint8_t small[16];
    size_t got;

    check("an empty folder has no saves", port_save_record(0, NULL) == PORT_SAVE_EMPTY &&
                                              port_save_record(2, NULL) == PORT_SAVE_EMPTY);
    check("a save written", write_save(1, 0x11, 0x15400));
    check("and read back the same", reads_as(1, 0x11, 0x15400));
    check("its record listed", port_save_record(1, record) == PORT_SAVE_OK && record[0] == 0x11);
    check("the others still empty", port_save_record(0, NULL) == PORT_SAVE_EMPTY &&
                                        port_save_record(2, NULL) == PORT_SAVE_EMPTY);
    check("no .tmp left, no .bak for a first save", !exists(2, ".tmp") && !exists(2, ".bak"));
    check("a read into a smaller buffer takes what fits",
          port_save_read(1, record, small, sizeof(small), &got) == PORT_SAVE_OK && got == 0x15400);
    check("overwritten", write_save(1, 0x22, 0x15400) && reads_as(1, 0x22, 0x15400));
    check("the old save kept as the .bak", exists(2, ".bak"));
    check("no save 0 or 4", port_save_record(-1, NULL) == PORT_SAVE_EMPTY &&
                                port_save_record(PORT_SAVE_COUNT, NULL) == PORT_SAVE_EMPTY &&
                                !write_save(PORT_SAVE_COUNT, 1, 64));
    check("data of no size or too big refused", !write_save(0, 1, 0) && !write_save(0, 1, PORT_SAVE_DATA_MAX + 1) &&
                                                     port_save_record(0, NULL) == PORT_SAVE_EMPTY);
}

static void damage(void) {
    check("save 3 written", write_save(2, 0x33, 0x1000));
    poke(3, "", 40 + PORT_SAVE_RECORD_SIZE + 100);
    check("a changed byte, no backup: damaged", port_save_record(2, NULL) == PORT_SAVE_DAMAGED);
    check("written over, it is whole", write_save(2, 0x34, 0x1000) && reads_as(2, 0x34, 0x1000));
    check("the damaged file was not kept as the backup", !exists(3, ".bak"));
    poke(3, "", 0);
    check("not a save file, no backup: damaged", port_save_record(2, NULL) == PORT_SAVE_DAMAGED);

    /* with a whole backup */
    check("save 3 written twice", write_save(2, 0x35, 0x1000) && write_save(2, 0x36, 0x1000) && exists(3, ".bak"));
    poke(3, "", 40 + PORT_SAVE_RECORD_SIZE + 7);
    check("damaged: the backup is read in its place", port_save_record(2, NULL) == PORT_SAVE_OK &&
                                                          reads_as(2, 0x35, 0x1000));
    check("a write over it is whole", write_save(2, 0x37, 0x1000) && reads_as(2, 0x37, 0x1000));
    poke(3, "", 40 + PORT_SAVE_RECORD_SIZE + 9);
    check("and kept the whole backup, not the damaged file", reads_as(2, 0x35, 0x1000));
    {
        char path[600], backup[600];

        path_of(path, sizeof(path), 3, "");
        path_of(backup, sizeof(backup), 3, ".bak");
        remove(path);
        check("a deleted save is not brought back from its backup", exists(3, ".bak") &&
                                                                      port_save_record(2, NULL) == PORT_SAVE_EMPTY);
        remove(backup);
    }
}

/* the states a crash in the middle of port_save_write leaves, mended by port_saves_open */
static void crashes(void) {
    remove_all();
    check("save 1 written", write_save(0, 0x41, 0x2000));
    /* cut short while writing the .tmp: the save is there, the .tmp half written */
    copy_file(1, "", 1, ".tmp");
    poke(1, ".tmp", 50);
    check("a .tmp beside the save goes", port_saves_open(folder) && !exists(1, ".tmp") && reads_as(0, 0x41, 0x2000));

    /* between the two renames: the old save is the .bak, the new one whole in the .tmp */
    check("save 1 written again", write_save(0, 0x42, 0x2000));
    copy_file(1, "", 1, ".tmp");
    {
        char path[600], backup[600];

        path_of(path, sizeof(path), 1, "");
        path_of(backup, sizeof(backup), 1, ".bak");
        remove(backup);
        rename(path, backup);
    }
    check("the whole .tmp becomes the save", port_saves_open(folder) && reads_as(0, 0x42, 0x2000) && !exists(1, ".tmp"));

    /* the same, but the .tmp is not whole: the .bak comes back */
    {
        char path[600], backup[600];

        path_of(path, sizeof(path), 1, "");
        path_of(backup, sizeof(backup), 1, ".bak");
        copy_file(1, "", 1, ".tmp");
        poke(1, ".tmp", 60);
        remove(backup);
        rename(path, backup);
    }
    check("a broken .tmp: the .bak becomes the save", port_saves_open(folder) && reads_as(0, 0x42, 0x2000) &&
                                                          !exists(1, ".tmp"));
}

static void no_folder(void) {
    check("none: no folder", !port_saves_open("none") && !port_saves_available());
    check("none: nothing written", !write_save(0, 1, 64));
    check("none: nothing to list", port_save_record(0, NULL) == PORT_SAVE_EMPTY);
    check("back to the folder", port_saves_open(folder) && port_saves_available());
}

/* ---- Import from an emulator's memory card (port/platform/card_import.c) ---------------------------------------- */
#define CARD_CLUSTERS 1024u
#define CARD_ALLOC 8u /* the area's first cluster: 0 superblock, 1 the indirect FAT cluster, 2 the FAT */

static uint8_t card[CARD_CLUSTERS * 1024u];
static uint32_t card_next; /* the area's next free cluster */

static void put32_at(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Puts `size` bytes in a chain of the area's clusters; its first cluster */
static uint32_t card_file(const uint8_t *bytes, size_t size) {
    uint32_t first = card_next, count = (uint32_t)((size + 1023u) / 1024u), i;

    for (i = 0; i < count; i++) {
        put32_at(card + 2u * 1024u + (first + i) * 4u, 0x80000000u | (i + 1 < count ? first + i + 1 : 0x7FFFFFFFu));
    }
    memcpy(card + (CARD_ALLOC + first) * 1024u, bytes, size);
    card_next += count;
    return first;
}

static void card_entry(uint8_t *p, uint16_t mode, uint32_t length, uint32_t cluster, const char *name) {
    p[0] = (uint8_t)mode;
    p[1] = (uint8_t)(mode >> 8);
    put32_at(p + 4, length);
    put32_at(p + 0x10, cluster);
    snprintf((char *)p + 0x40, 32, "%s", name);
}

/* What mc.c's Mc_StartWriteChecked writes of `size` bytes; the file's length */
static size_t checked_file(uint8_t *out, const uint8_t *data, size_t size) {
    size_t at = 4, done = 0, n, full, i;
    uint8_t sum;

    put32_at(out, (uint32_t)size);
    for (full = 0x3FB;; full = 0x3FF) {
        n = size - done < full ? size - done : full;
        memcpy(out + at, data + done, n);
        for (sum = 0, i = 0; i < n; i++) {
            sum = (uint8_t)(sum + (uint8_t)~data[done + i]);
        }
        at += n;
        if (n != 0) {
            out[at++] = sum;
        }
        done += n;
        if (n < full) {
            out[at++] = 1;
            memset(out + at, 0, ((at + 0x1F) & ~(size_t)0x1F) - at);
            return (at + 0x1F) & ~(size_t)0x1F;
        }
    }
}

static int write_bytes(const char *path, const uint8_t *bytes, size_t size) {
    FILE *out = fopen(path, "wb");
    int ok = out != NULL && fwrite(bytes, size, 1, out) == 1;

    if (out != NULL && fclose(out) != 0) {
        ok = 0;
    }
    return ok;
}

/* The game's list naming save 2 (index 1) with `record`, and the save's checked file; their lengths */
static uint8_t card_list[PORT_SAVE_COUNT * PORT_SAVE_RECORD_SIZE + 0x200], card_save[PORT_SAVE_DATA_MAX + 0x400];
static size_t card_list_length, card_save_length;

static void game_files(const uint8_t *record, const uint8_t *data, size_t size, int damage) {
    static uint8_t list[PORT_SAVE_COUNT * PORT_SAVE_RECORD_SIZE];

    memset(list, 0, sizeof(list));
    memcpy(list + PORT_SAVE_RECORD_SIZE, record, PORT_SAVE_RECORD_SIZE);
    card_list_length = checked_file(card_list, list, sizeof(list));
    card_save_length = checked_file(card_save, data, size);
    card_save[0x500] ^= (uint8_t)(damage != 0);
}

/* A formatted card image holding the game's folder with those files, written with `page_stride` 512 or 528 (ECC
   bytes after each page) to `path` */
static int write_card(const char *path, size_t page_stride) {
    uint8_t game_folder[4 * 512] = {0}, root[3 * 512] = {0};
    static const uint8_t ecc[16];
    uint32_t list_at, save_at, folder_at, root_at;
    size_t page;
    FILE *out;
    int ok;

    memset(card, 0, sizeof(card));
    card_next = 0;
    memcpy(card, "Sony PS2 Memory Card Format 1.2.0.0", 35);
    put32_at(card + 0x28, 512u | 2u << 16);
    put32_at(card + 0x34, CARD_ALLOC);
    put32_at(card + 0x50, 1); /* ifc_list[0] */
    put32_at(card + 1024u, 2);
    root_at = card_file(root, sizeof(root)); /* first: the superblock gives cluster 0 of the area as the root */
    list_at = card_file(card_list, card_list_length);
    card_entry(game_folder + 2 * 512, 0x8497, (uint32_t)card_list_length, list_at, "BASLUS-20318S");
    save_at = card_file(card_save, card_save_length);
    card_entry(game_folder + 3 * 512, 0x8497, (uint32_t)card_save_length, save_at, "BASLUS-20318G01");
    card_entry(game_folder, 0x8427, 4, 0, ".");
    card_entry(game_folder + 512, 0x8427, 0, 0, "..");
    folder_at = card_file(game_folder, sizeof(game_folder));
    card_entry(root, 0x8427, 3, 0, ".");
    card_entry(root + 512, 0xA426, 0, 0, "..");
    card_entry(root + 1024, 0x8427, 4, folder_at, "BASLUS-20318S");
    memcpy(card + (CARD_ALLOC + root_at) * 1024u, root, sizeof(root));

    if ((out = fopen(path, "wb")) == NULL) {
        return 0;
    }
    ok = 1;
    for (page = 0; page < sizeof(card) / 512u && ok; page++) {
        ok = fwrite(card + page * 512u, 512, 1, out) == 1 && (page_stride != 528 || fwrite(ecc, sizeof(ecc), 1, out) == 1);
    }
    return fclose(out) == 0 && ok;
}

static int make_folder(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0700) == 0;
#endif
}

/* A PCSX2 folder card: the game's files as plain files */
static int write_folder_card(const char *path) {
    char game[700], file[760];

    snprintf(game, sizeof(game), "%s/BASLUS-20318S", path);
    snprintf(file, sizeof(file), "%s/BASLUS-20318S", game);
    if (!make_folder(path) || !make_folder(game) || !write_bytes(file, card_list, card_list_length)) {
        return 0;
    }
    snprintf(file, sizeof(file), "%s/BASLUS-20318G01", game);
    return write_bytes(file, card_save, card_save_length);
}

static void remove_folder_card(const char *path) {
    char game[700], file[760];

    snprintf(game, sizeof(game), "%s/BASLUS-20318S", path);
    snprintf(file, sizeof(file), "%s/BASLUS-20318S", game);
    remove(file);
    snprintf(file, sizeof(file), "%s/BASLUS-20318G01", game);
    remove(file);
    rmdir(game);
    rmdir(path);
}

static void card_import(void) {
    static uint8_t record[PORT_SAVE_RECORD_SIZE], data[0x15400], other_record[PORT_SAVE_RECORD_SIZE],
        other_data[0x2000], got_record[PORT_SAVE_RECORD_SIZE], got_data[PORT_SAVE_DATA_MAX];
    static PortCardSaves saves;
    char path[600], folder_card[600];
    size_t got = 0, stride;

    snprintf(path, sizeof(path), "%s/card.ps2", folder);
    snprintf(folder_card, sizeof(folder_card), "%s/Mcd002.ps2", folder);
    make_save(record, data, sizeof(data), 5);
    record[1] = 0;                    /* SaveInfo.damaged */
    put32_at(record + 0x0C, 24);      /* numbers[2]: the level */
    record[0x178 + 2] = 5;            /* the play time: 31 h 05 min */
    record[0x178 + 3] = 31;
    record[0x178 + 5] = 0;
    put32_at(data, sizeof(data));
    make_save(other_record, other_data, sizeof(other_data), 9); /* (its first word left as made: reads_as) */
    game_files(record, data, sizeof(data), 0);

    for (stride = 512; stride <= 528; stride += 16) {
        remove_all();
        check("a card image is written", write_card(path, stride));
        check("its save is read", port_card_read(path, &saves) == PORT_CARD_OK && saves.count == 1 &&
                                       saves.damaged == 0 && saves.saves[0].number == 1 &&
                                       saves.saves[0].size == sizeof(data) &&
                                       memcmp(saves.saves[0].data, data, sizeof(data)) == 0);
        check("its level and play time", port_card_save_level(&saves.saves[0]) == 24 &&
                                             port_card_save_minutes(&saves.saves[0]) == 31 * 60 + 5);
        check("a card's save is imported", port_saves_import_card(path) == 1);
        check("to the save of its number, record and data as on the card",
              port_save_read(1, got_record, got_data, sizeof(got_data), &got) == PORT_SAVE_OK && got == sizeof(data) &&
                  memcmp(got_record, record, sizeof(record)) == 0 && memcmp(got_data, data, sizeof(data)) == 0 &&
                  port_save_record(0, NULL) == PORT_SAVE_EMPTY);
        check("a save already imported is known", port_card_save_held_in(&saves.saves[0]) == 1);
        check("and not imported again", port_saves_import_card(path) == 0 && port_save_record(0, NULL) == PORT_SAVE_EMPTY);
    }

    /* a save of its number is there: the first free save, the one there untouched */
    remove_all();
    check("another save in slot 2", port_save_write(1, other_record, other_data, sizeof(other_data)));
    check("not held", port_card_save_held_in(&saves.saves[0]) == -1 && port_card_save_slot(&saves.saves[0]) == 0);
    check("goes to the first free save", port_card_save_import(&saves.saves[0]) == 0 && reads_as(1, 9, sizeof(other_data)));
    /* every save taken: nothing replaced */
    remove_all();
    check("three other saves", port_save_write(0, other_record, other_data, sizeof(other_data)) &&
                                   port_save_write(1, other_record, other_data, sizeof(other_data)) &&
                                   port_save_write(2, other_record, other_data, sizeof(other_data)));
    check("no free save", port_card_save_slot(&saves.saves[0]) == -1 && port_card_save_import(&saves.saves[0]) == -1 &&
                              port_saves_import_card(path) == 0);
    check("none replaced", reads_as(0, 9, sizeof(other_data)) && reads_as(1, 9, sizeof(other_data)) &&
                               reads_as(2, 9, sizeof(other_data)));

    /* a folder card */
    remove_all();
    check("a folder card is written", write_folder_card(folder_card));
    check("its save is read", port_card_read(folder_card, &saves) == PORT_CARD_OK && saves.count == 1 &&
                                  saves.saves[0].number == 1 && memcmp(saves.saves[0].data, data, sizeof(data)) == 0);
    check("and imported", port_saves_import_card(folder_card) == 1 && port_save_record(1, NULL) == PORT_SAVE_OK);
    remove_folder_card(folder_card);

    /* damaged and wrong cards */
    remove_all();
    game_files(record, data, sizeof(data), 1);
    check("a card with a damaged save is written", write_card(path, 512));
    check("a card save whose sum is wrong is left out",
          port_card_read(path, &saves) == PORT_CARD_OK && saves.count == 0 && saves.damaged == 1 &&
              port_saves_import_card(path) == 0 && port_save_record(1, NULL) == PORT_SAVE_EMPTY);
    memset(card, 0xFF, 4096);
    check("an unformatted card is written", write_bytes(path, card, 4096));
    check("an unformatted card is refused",
          port_card_read(path, &saves) == PORT_CARD_NOT_CARD && port_saves_import_card(path) == -1);
    remove(path);
    check("a missing card is refused",
          port_card_read(path, &saves) == PORT_CARD_UNREADABLE && port_saves_import_card(path) == -1);
    remove_all();
}

int main(int argc, char **argv) {
    if (argc > 1) {
        snprintf(folder, sizeof(folder), "%s", argv[1]);
    } else {
        const char *base = getenv("TMPDIR");

#ifdef _WIN32
        if (base == NULL) {
            base = getenv("TEMP");
        }
#endif
        snprintf(folder, sizeof(folder), "%s/kf4_save_files_test_%d", base != NULL ? base : "/tmp", (int)getpid());
    }
    check("the folder opened (and made)", port_saves_open(folder));
    remove_all();
    round_trip();
    damage();
    crashes();
    card_import();
    no_folder();
    remove_all();
    rmdir(folder);
    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
