/* The Sony library that is still a stand-in: libdev. Each call does nothing, or the least that keeps
   the game going. The rest of the SDK is in port/sdk: libcdvd.c, libdma.c, libgraph.c,
   libmpeg.c, libpad.c, libsdr.c and sif.c, and the kernel in port/platform/kernel.c. There is no libmc:
   the saves are files (game/saves.c). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eeregs.h"
#include "libdev.h"
#include "libgraph.h"
#include "port/host.h"
#include "port/platform.h"

#define STUB(name) PORT_TODO(name " (stub)")

/* ---- libdev --------------------------------------------------------------------------------- */
int sceDevVu0Pause(void) { return 0; }
int sceDevVu0SetTBit(void) { return 0; }
int sceDevVu0IsRunning(void) { return 0; }
int sceDevVif0Reset(void) { return 0; }
int sceDevVif1Reset(void) { return 0; }
int sceDevVu0Reset(void) { return 0; }
int sceDevVu1Reset(void) { return 0; }
