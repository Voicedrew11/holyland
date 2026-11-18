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
    no_folder();
    remove_all();
    rmdir(folder);
    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
