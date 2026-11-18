/* Save states (port/platform/savestate.c): the whole running game written to a file and read back,
   for testing and debugging. A request is taken at the next safe point (a vblank, with every EE
   thread parked in the kernel), never in the middle of the game's code. */
#ifndef PORT_SAVESTATE_H
#define PORT_SAVESTATE_H

#include <stddef.h>
#include <stdint.h>

/* Asks for the state to be saved to, or loaded from, `slot` (0 is the quick slot) at the next safe
   point. Safe from any thread (the window's events, the settings overlay). */
void port_state_request_save(int slot);
void port_state_request_load(int slot);
/* A save to a file (the flash recorder's folder: port/debug/debug.c); path is copied */
void port_state_request_save_file(const char *path);

/* Whether `slot` has a state, and when it was written (seconds since 1970, 0 if none). */
int port_state_exists(int slot);
long long port_state_time(int slot);

/* The last thing a request did, for the overlay and the log ("saved to slot 0", or why not). */
const char *port_state_message(void);

/* ---- The port's side ------------------------------------------------------------------------- */

/* Reads --state-save-at and --state-load (port/platform/main_host.c), once the host is set up. */
void port_state_init(void);
/* (the kernel, at each vblank) 1 when a request waits: the running EE thread then hands the host
   thread port_state_host, and goes on when it returns. */
int port_state_wanted(void);
/* (the host thread, with every EE thread parked) does what was asked: a save, a load (before the
   game's first instruction, too) or nothing. */
void port_state_host(void);

/* Each part of the PC's PS2 writes its state into a state, or reads it back, through one function
   that does both: port_state_bytes copies `size` bytes at `data` into the state when saving, and
   from it when loading (with `data` NULL: zeros, and skipped). A part lists its variables in the
   same order either way. */
typedef struct PortStateIo PortStateIo;
struct PortStateIo {
    void (*bytes)(PortStateIo *io, void *data, size_t size);
    int loading;
};

static inline void port_state_bytes(PortStateIo *io, void *data, size_t size) {
    io->bytes(io, data, size);
}
#define PORT_STATE(io, variable) port_state_bytes((io), &(variable), sizeof(variable))

/* The parts, in the order a state holds them. Each runs on the DMA controller's thread, with the EE
   threads parked and the sound locked. */
void port_memory_state(PortStateIo *io);  /* game RAM, the scratchpad, VU0, the register pages */
void port_fiber_state(PortStateIo *io);   /* the EE threads' fibers and their stacks */
void port_kernel_state(PortStateIo *io);  /* threads, semaphores, handlers, timers, EE time */
void port_dmac_state(PortStateIo *io);    /* the channels' counts and PATH3 */
void port_vif1_state(PortStateIo *io);
void port_vu1_state(PortStateIo *io);
void port_gs_state(PortStateIo *io);      /* the GIF's paths and the GS (VRAM, registers) */
void port_libgraph_state(PortStateIo *io);
void port_pad_state(PortStateIo *io);
void port_cdvd_state(PortStateIo *io);
void port_sif_state(PortStateIo *io);     /* open files, modules, the IOP heap */
void port_libc_state(PortStateIo *io);
void port_sd_state(PortStateIo *io);
void port_fsiopsnd_state(PortStateIo *io);
void port_modmidi_state(PortStateIo *io);
void port_modhsyn_state(PortStateIo *io);
void port_audio_state(PortStateIo *io);   /* the sound's clock: last, after the driver */

/* Why a state cannot be taken now (NULL: it can), from the parts that know (port/sdk/sif.c,
   port/platform/gs.c); the request waits for the next vblank. */
const char *port_sif_state_busy(void);
const char *port_gs_state_busy(void);
/* (port/sdk/libmpeg.c) a movie plays: its decoder is the host's, not in a state */
const char *port_mpeg_state_busy(void);

/* 1 when game RAM (memory.c) and the EE threads' stacks (fiber.c) are at their fixed addresses, which
   save states need */
int port_memory_fixed(void);
int port_fiber_fixed(void);

#endif
