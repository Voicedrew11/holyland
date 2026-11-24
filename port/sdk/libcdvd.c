/* libcdvd for the PC port (port/include/libcdvd.h): the disc drive, reading the user's disc through
   port/platform/disc.c. A read is done by the time sceCdRead returns, so the drive is never busy and
   never reports an error. */
#include <string.h>
#include <time.h>

#include "libcdvd.h"
#include "port/host.h"
#include "port/libc_ps2.h"
#include "port/platform.h"
#include "port/savestate.h"

#define SCECD_COMPLETE 2

static uint32_t stream_lsn;

/* Names the file a read starts, in the log, when it starts one. */
static void log_read(uint32_t lsn, uint32_t sectors) {
    uint32_t first;
    const char *name;

    if (!port_log_enabled()) {
        return;
    }
    name = port_disc_name_at(lsn, &first);
    if (name != NULL && first == lsn) {
        port_event("read %s (%u sectors)", name, sectors);
    }
}

int sceCdInit(int mode) {
    (void)mode;
    return 1;
}

int sceCdMmode(int media) {
    (void)media;
    return 1;
}

int sceCdDiskReady(int mode) {
    (void)mode;
    return SCECD_COMPLETE;
}

int sceCdSync(int mode) {
    (void)mode;
    return 0;
}

int sceCdGetError(void) {
    return 0;
}

int sceCdBreak(void) {
    return 1;
}

int sceCdSearchFile(sceCdlFILE *file, const char *name) {
    uint32_t lsn, size;
    const char *base;
    size_t i;

    if (!port_disc_find(name, &lsn, &size)) {
        port_event("sceCdSearchFile: no %s on the disc", name);
        return 0;
    }
    file->lsn = lsn;
    file->size = size;
    /* the name as the directory has it: the last part of the path, with its version */
    base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    memset(file->name, 0, sizeof(file->name));
    for (i = 0; base[i] != 0 && i < sizeof(file->name) - 1; i++) {
        file->name[i] = base[i];
    }
    memset(file->date, 0, sizeof(file->date));
    return 1;
}

int sceCdRead(unsigned int lsn, unsigned int sectors, void *buf, sceCdRMode *mode) {
    (void)mode;
    log_read(lsn, sectors);
    if (!port_disc_read(lsn, sectors, buf)) {
        PORT_FATAL("sceCdRead: sectors %u..%u are not on the disc", lsn, lsn + sectors - 1);
    }
    return 1;
}

int port_cd_read_bytes(unsigned int lsn, unsigned int sectors, void *buf, int size, sceCdRMode *mode) {
    unsigned int whole = (unsigned int)size / 0x800; /* the sectors that fit in buf */
    unsigned char tail[0x800];
    unsigned char *p = buf;

    /* Game RAM is laid out as on the PS2, so there it gets the whole sectors, as on the PS2 (the
       disc's bytes after a file's end are zeros, which a parser may stop at). The program's globals
       are laid out by the PC's compiler: what follows a buffer there is not what followed it on the
       PS2, so it gets only size bytes. */
    if (size < 0 || whole >= sectors || sectors != whole + 1 ||
        (p >= ee_ram && p + (size_t)sectors * 0x800 <= ee_ram + EE_RAM_SIZE)) {
        return sceCdRead(lsn, sectors, buf, mode);
    }
    log_read(lsn, sectors);
    if ((whole != 0 && !port_disc_read(lsn, whole, buf)) || !port_disc_read(lsn + whole, 1, tail)) {
        PORT_FATAL("sceCdRead: sectors %u..%u are not on the disc", lsn, lsn + sectors - 1);
    }
    memcpy((unsigned char *)buf + (size_t)whole * 0x800, tail, (size_t)size % 0x800);
    return 1;
}

int sceCdSeek(unsigned int lsn) {
    (void)lsn;
    return 1;
}

static unsigned char bcd(int value) {
    return (unsigned char)(((value / 10) << 4) | (value % 10));
}

/* The drive's clock, in BCD: the host's time, or the one --clock gives at boot. A fixed clock runs on
   with the EE's time (port_ee_cycles), so a run is repeatable and the play time, which the game works
   out from the clock (core/play_time.c), still counts. A PS2 keeps its clock in Japan's time (UTC+9)
   whatever the console's region, and the game uses it as it is (no libscf time zone), so the PC gives
   UTC+9 too. */
int sceCdReadClock(sceCdCLOCK *clock) {
    int year, month, day, hour, minute, second;

    if (port_options.clock_fixed) {
        /* newlib's mktime normalises the fields (port/sdk/libc_ps2.c) */
        int fields[9] = {0};

        fields[0] = port_options.clock_fields[5] + (int)(port_ee_cycles() / EE_BUS_HZ);
        fields[1] = port_options.clock_fields[4];
        fields[2] = port_options.clock_fields[3];
        fields[3] = port_options.clock_fields[2];
        fields[4] = port_options.clock_fields[1] - 1;
        fields[5] = port_options.clock_fields[0] - 1900;
        ps2_mktime(fields);
        year = fields[5] + 1900;
        month = fields[4] + 1;
        day = fields[3];
        hour = fields[2];
        minute = fields[1];
        second = fields[0];
    } else {
        time_t now = time(NULL) + 9 * 60 * 60;
        struct tm *local = gmtime(&now);

        year = local->tm_year + 1900;
        month = local->tm_mon + 1;
        day = local->tm_mday;
        hour = local->tm_hour;
        minute = local->tm_min;
        second = local->tm_sec;
    }
    clock->stat = 0;
    clock->second = bcd(second);
    clock->minute = bcd(minute);
    clock->hour = bcd(hour);
    clock->pad = 0;
    clock->day = bcd(day);
    clock->month = bcd(month);
    clock->year = bcd(year % 100);
    return 1;
}

/* ---- Streaming (the movie player) ------------------------------------------------------------- */
int sceCdStInit(unsigned int bufmax, unsigned int bankmax, void *buffer) {
    (void)bufmax;
    (void)bankmax;
    (void)buffer;
    return 1;
}

int sceCdStStart(unsigned int lsn, sceCdRMode *mode) {
    (void)mode;
    log_read(lsn, 0);
    stream_lsn = lsn;
    return 1;
}

int sceCdStRead(unsigned int sectors, void *buffer, unsigned int mode, unsigned int *error) {
    (void)mode;
    if (error) {
        *error = 0;
    }
    /* Sectors that cannot be read (past the disc's end, an image cut short: logged by port_disc_read) are zeros
       and the stream goes on. On the PS2 a failed read is retried; here a retry reads the same file the same way,
       and a stream that gave nothing and no error left the movie's reader (gfx/movie.c) waiting for ever. */
    if (!port_disc_read(stream_lsn, sectors, buffer)) {
        memset(buffer, 0, (size_t)sectors * 0x800);
    }
    stream_lsn += sectors;
    return (int)sectors;
}

int sceCdStSeek(unsigned int lsn) {
    stream_lsn = lsn;
    return 1;
}

int sceCdStStop(void) {
    return 1;
}

/* ---- Save states: where the stream reads next ------------------------------------------------- */
void port_cdvd_state(PortStateIo *io) {
    PORT_STATE(io, stream_lsn);
}
