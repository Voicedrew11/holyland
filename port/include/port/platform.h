/* The host side of the PC port: what port/platform provides to the rest of port/ (not to the game). */
#ifndef PORT_PLATFORM_H
#define PORT_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* ---- Start-up -------------------------------------------------------------------------------- */

/* Allocates game RAM and resets the scratchpad, register pages and VU0 (port/platform/memory.c). */
void port_memory_init(void);

/* Gives the program's sections their EE addresses (port/platform/ee_address.c). */
void port_address_init(void);

/* The game's main (src/game/main.c, renamed by the build). */
int kf4_game_main(void);

/* What the command line asked for (port/platform/main_host.c). */
typedef struct {
    const char *disc;        /* --disc: the disc image (.iso) or the extracted disc's folder */
    const char *input;       /* --input: a pad script (port/sdk/libpad.c) instead of SDL */
    const char *log;         /* --log: the per-frame state log ("-" for stderr) */
    const char *host_dir;    /* --host-dir: where host0: files go (default: the current directory) */
    long long frames;        /* --frames: stop after this many vblanks (0: run on) */
    int fast;                /* --fast: do not hold the game to 59.94 vblanks a second */
    int headless;            /* --headless: no SDL at all */
    int clock_fixed;         /* --clock: the disc drive's clock is fixed (clock_fields), not the host's */
    int clock_fields[6];     /* year, month, day, hour, minute, second */
} PortOptions;
extern PortOptions port_options;

/* ---- Fibers (port/platform/fiber.c) ---------------------------------------------------------
   Each EE thread is a fiber of the host's main thread: one runs at a time, and a switch happens
   only where the kernel makes one. */
typedef struct PortFiber PortFiber;

/* The calling host thread as a fiber (the EE's main thread). */
PortFiber *port_fiber_main(void);
/* A fiber that will run entry(arg) when first switched to. entry must not return. */
PortFiber *port_fiber_create(size_t stack_size, void (*entry)(void *), void *arg);
/* Frees a fiber that is not running. */
void port_fiber_delete(PortFiber *fiber);
/* Saves the running fiber in from and runs to. */
void port_fiber_switch(PortFiber *from, PortFiber *to);

/* ---- The EE kernel and its clock (port/platform/kernel.c) -----------------------------------
   Time is the EE's bus clock (147.456 MHz), counted by the host: it moves only at vblank waits and
   at the calls a game polls in a loop, so the same input gives the same run. */
#define EE_BUS_HZ 147456000u
/* The kernel's first steps, before the game's main runs. */
void port_kernel_init(void);
/* The current EE time in bus cycles, and the vblanks so far. */
uint64_t port_ee_cycles(void);
uint64_t port_vblank_count(void);
/* Waits for the next vblank start (the EE's VSync): other threads run meanwhile. */
void port_vsync(void);
/* Lets time pass for a call the game polls in a loop (cycles of EE time). */
void port_kernel_spend(uint32_t cycles);
/* Runs the interrupt handlers that came due (the DMA controller's ends). Every kernel call does. */
void port_kernel_poll(void);
/* Posts a DMA channel's end for its handler (any host thread may call it). */
void port_kernel_post_dmac(int channel);

/* ---- The DMA controller (port/platform/dmac.c) ----------------------------------------------
   A host thread that plays the EE's DMA controller: a channel the game starts (STR set in its
   CHCR) is finished, and its STR cleared, as the hardware would. */
void port_dmac_start(void);
/* Wakes the controller: the game is about to start a channel. */
void port_dmac_wake(void);
/* Waits until a channel's STR is clear. */
void port_dmac_wait(int channel);
/* The channel's CHCR as the game sees it, and transfers finished so far. */
volatile int *port_dmac_chcr(int channel);
uint64_t port_dmac_transfers(int channel);

/* ---- The disc (port/platform/disc.c) --------------------------------------------------------
   The user's own disc, as an ISO 9660 image or as the folder it was extracted to, read by sector
   number as the drive reads it. */
#define DISC_SECTOR_SIZE 2048u
void port_disc_open(const char *path);
/* Finds a file by its ISO 9660 path ("\DATA\KF4.DAT;1", any case, the version optional):
   1 and its first sector and size, or 0. */
int port_disc_find(const char *path, uint32_t *lsn, uint32_t *size);
/* Reads sectors into buffer; 0 when they are past the end of the disc. */
int port_disc_read(uint32_t lsn, uint32_t sectors, void *buffer);
/* The name of the file a sector belongs to, for the log (NULL: none). */
const char *port_disc_name_at(uint32_t lsn, uint32_t *first_lsn);

/* ---- The log --------------------------------------------------------------------------------
   port_log writes one line to the state log (--log), port_event the same with the vblank count,
   for the things that happen between frames (files opened, movies, the memory card). */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int port_log_enabled(void);

/* The game's state in one line (game/state_log.c), written to the log once a frame. */
void kf4_game_state(char *line, size_t size);

/* ---- The pad (port/sdk/libpad.c) ------------------------------------------------------------ */
void port_pad_init(void);
/* Once a vblank: SDL's events. */
void port_pad_poll(void);

/* ---- Small host helpers (port/platform/host_time.c) --------------------------------------- */
/* Seconds since the program started, from the host's monotonic clock. */
double port_wall_seconds(void);
/* Sleeps for about the given seconds. */
void port_sleep(double seconds);

#endif /* PORT_PLATFORM_H */
