/* port/platform/file_replace.c: a file put in place of an old one, of none, a missing one refused with the old
   file kept, and on Windows the old file held open by another program for a moment (as a virus scanner does).

     file_replace_test [FOLDER]   (the files are made in FOLDER, else the working folder) */
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "port/file_replace.h"

static int failed, checked;

static void check(const char *what, int ok) {
    checked++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static int write_text(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    int ok = fputs(text, file) >= 0;
    return fclose(file) == 0 && ok;
}

/* the file's text is `text` */
static int holds(const char *path, const char *text) {
    char buffer[64] = {0};
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    return n == strlen(text) && memcmp(buffer, text, n) == 0;
}

static int exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

#ifdef _WIN32
/* another program's open of the old file, without FILE_SHARE_DELETE (so it cannot be replaced), closed after
   a moment on its own thread */
static DWORD WINAPI close_later(void *handle) {
    Sleep(150);
    CloseHandle((HANDLE)handle);
    return 0;
}

static void held_for_a_moment(const char *temp, const char *path) {
    check("held: the new file written", write_text(temp, "held"));
    HANDLE held = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    check("held: the old file opened by another", held != INVALID_HANDLE_VALUE);
    HANDLE closer = CreateThread(NULL, 0, close_later, held, 0, NULL);
    check("held: the closing thread started", closer != NULL);
    check("held: put in place once it is closed", port_replace_file(temp, path));
    check("held: the new text", holds(path, "held"));
    if (closer != NULL) {
        WaitForSingleObject(closer, INFINITE);
        CloseHandle(closer);
    }
}
#endif

int main(int argc, char **argv) {
    const char *folder = argc > 1 ? argv[1] : ".";
    char path[1024], temp[1024];

    snprintf(path, sizeof(path), "%s/file_replace_test.txt", folder);
    snprintf(temp, sizeof(temp), "%s/file_replace_test.txt.new", folder);
    remove(path);
    remove(temp);

    check("none: the new file written", write_text(temp, "first"));
    check("none: put in place", port_replace_file(temp, path));
    check("none: the new text", holds(path, "first"));
    check("none: the new file gone", !exists(temp));

    check("old: the new file written", write_text(temp, "second"));
    check("old: put in place of the old", port_replace_file(temp, path));
    check("old: the new text", holds(path, "second"));
    check("old: the new file gone", !exists(temp));

    check("missing: refused", !port_replace_file(temp, path));
    check("missing: the old file kept", holds(path, "second"));

#ifdef _WIN32
    held_for_a_moment(temp, path);
#endif

    remove(path);
    remove(temp);
    printf("%d of %d checks passed\n", checked - failed, checked);
    return failed != 0;
}
