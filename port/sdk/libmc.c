/* libmc for the PC port (port/include/libmc.h): two empty memory card slots, until saves (step 5) keep
   cards as folders.

   Every call is accepted (returns 0) and finishes at once; sceMcSync then hands back its result. A
   card check answers -11, which the game takes for "no card" (it reads any answer of -11 or less that
   way, and -10 as a card error); every other call fails as it would on an empty slot. */

#include "libmc.h"
#include "port/host.h"
#include "port/platform.h"

#define MC_NO_CARD -11

/* sceMcSync's function numbers */
#define MC_FUNC_GET_INFO 1
#define MC_FUNC_OPEN 2
#define MC_FUNC_CLOSE 3
#define MC_FUNC_SEEK 4
#define MC_FUNC_READ 5
#define MC_FUNC_WRITE 6
#define MC_FUNC_MKDIR 11
#define MC_FUNC_CHDIR 12
#define MC_FUNC_GET_DIR 13
#define MC_FUNC_DELETE 15
#define MC_FUNC_FORMAT 16

static int pending_function; /* 0: no call in flight */
static int pending_result;

static int start_call(int function, int result) {
    pending_function = function;
    pending_result = result;
    return 0;
}

int sceMcInit(void) {
    return 0;
}

int sceMcGetInfo(int port, int slot, int *type, int *free, int *format) {
    static int logged[2];

    if (port >= 0 && port < 2 && !logged[port]) {
        logged[port] = 1;
        port_event("memory card: no card in slot %d", port + 1);
    }
    (void)slot;
    if (type) {
        *type = 0;
    }
    if (free) {
        *free = 0;
    }
    if (format) {
        *format = 0;
    }
    return start_call(MC_FUNC_GET_INFO, MC_NO_CARD);
}

int sceMcOpen(int port, int slot, const char *name, int mode) {
    (void)port;
    (void)slot;
    (void)name;
    (void)mode;
    return start_call(MC_FUNC_OPEN, MC_NO_CARD);
}

int sceMcClose(int fd) {
    (void)fd;
    return start_call(MC_FUNC_CLOSE, MC_NO_CARD);
}

int sceMcRead(int fd, void *buffer, int size) {
    (void)fd;
    (void)buffer;
    (void)size;
    return start_call(MC_FUNC_READ, MC_NO_CARD);
}

int sceMcWrite(int fd, const void *buffer, int size) {
    (void)fd;
    (void)buffer;
    (void)size;
    return start_call(MC_FUNC_WRITE, MC_NO_CARD);
}

int sceMcSeek(int fd, int offset, int origin) {
    (void)fd;
    (void)offset;
    (void)origin;
    return start_call(MC_FUNC_SEEK, MC_NO_CARD);
}

int sceMcMkdir(int port, int slot, const char *name) {
    (void)port;
    (void)slot;
    (void)name;
    return start_call(MC_FUNC_MKDIR, MC_NO_CARD);
}

int sceMcChdir(int port, int slot, const char *new_dir, char *old_dir) {
    (void)port;
    (void)slot;
    (void)new_dir;
    (void)old_dir;
    return start_call(MC_FUNC_CHDIR, MC_NO_CARD);
}

int sceMcGetDir(int port, int slot, const char *name, unsigned int mode, int max_entries,
                sceMcTblGetDir *table) {
    (void)port;
    (void)slot;
    (void)name;
    (void)mode;
    (void)max_entries;
    (void)table;
    return start_call(MC_FUNC_GET_DIR, MC_NO_CARD);
}

int sceMcFormat(int port, int slot) {
    (void)port;
    (void)slot;
    return start_call(MC_FUNC_FORMAT, MC_NO_CARD);
}

int sceMcDelete(int port, int slot, const char *name) {
    (void)port;
    (void)slot;
    (void)name;
    return start_call(MC_FUNC_DELETE, MC_NO_CARD);
}

int sceMcSync(int mode, int *cmd, int *result) {
    (void)mode;
    if (pending_function == 0) {
        return -1;
    }
    if (cmd) {
        *cmd = pending_function;
    }
    if (result) {
        *result = pending_result;
    }
    pending_function = 0;
    return 1;
}
