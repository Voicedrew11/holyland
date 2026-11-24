/* A file put in place of another in one step (port/include/port/file_replace.h). */
#ifdef _WIN32
#include <windows.h>
#else
#include <stdio.h>
#endif

#include "port/file_replace.h"

#ifdef _WIN32
/* another program has one of the files open for a moment (a virus scanner, a cloud folder's sync) */
static int held_open(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED || error == ERROR_LOCK_VIOLATION;
}

int port_replace_file(const char *temp, const char *path) {
    DWORD waited_ms = 0;
    DWORD pause_ms = 10;

    for (;;) {
        if (MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return 1;
        }
        if (!held_open(GetLastError()) || waited_ms >= FILE_REPLACE_WAIT_MS) {
            return 0;
        }
        Sleep(pause_ms);
        waited_ms += pause_ms;
        if (pause_ms < 200) {
            pause_ms *= 2;
        }
    }
}
#else
int port_replace_file(const char *temp, const char *path) {
    return rename(temp, path) == 0;
}
#endif
