/* Game RAM, the scratchpad, the hardware register pages and VU0's registers (port/host.h). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/host.h"
#include "port/platform.h"

unsigned char *ee_ram;
unsigned char ee_scratchpad[0x4000] __attribute__((aligned(64)));
EeVu0 ee_vu0;

/* The register blocks: EE I/O 0x10000000..0x1000FFFF, the VU memories 0x11000000..0x1100FFFF and the
   GS privileged registers 0x12000000..0x12001FFF. */
static unsigned char ee_io[0x10000] __attribute__((aligned(16)));
static unsigned char ee_vu_memory[0x10000] __attribute__((aligned(16)));
static unsigned char gs_privileged[0x2000] __attribute__((aligned(16)));

void port_memory_init(void) {
    /* 1 MB aligned, so EE addresses keep the alignment the game's 16-byte and 64-byte data expect */
    unsigned char *block = calloc(1, EE_RAM_SIZE + 0x100000);

    if (block == NULL) {
        PORT_FATAL("cannot allocate %u MB of game RAM", EE_RAM_SIZE >> 20);
    }
    ee_ram = (unsigned char *)(((uintptr_t)block + 0xFFFFF) & ~(uintptr_t)0xFFFFF);
    ee_vu0.vf[0][3] = 1.0f;
}

void *ee_hw_ptr(uint32_t ee_address) {
    uint32_t offset = ee_address & 0x00FFFFFF;

    switch (ee_address & 0xFF000000) {
    case 0x10000000:
        if (offset < sizeof(ee_io)) {
            return ee_io + offset;
        }
        break;
    case 0x11000000:
        if (offset < sizeof(ee_vu_memory)) {
            return ee_vu_memory + offset;
        }
        break;
    case 0x12000000:
        if (offset < sizeof(gs_privileged)) {
            return gs_privileged + offset;
        }
        break;
    }
    PORT_FATAL("no hardware register at 0x%08X on the PC", ee_address);
}

void port_todo(const char *what, const char *file, int line) {
    fprintf(stderr, "[todo] %s (%s:%d)\n", what, file, line);
}

void port_fatal(const char *file, int line, const char *fmt, ...) {
    va_list args;

    fprintf(stderr, "[fatal] %s:%d: ", file, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}
