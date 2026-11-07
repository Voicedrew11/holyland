/* The debugging tools for playthrough testing (port/debug/, docs/port/README.md "Debugging"). Each
   writes a file into the captures folder of the data folder (port_data_dir()/captures), readable by
   any build:
   - F9: a GS frame capture (.kf4gs) of the next whole frame, for tools/gsreplay (kf4-gsreplay);
   - F10: the game's state as JSON (.json);
   - F12: the last 20 seconds of the sound as a WAV (.wav);
   - F8: the flash recorder's folder (port_debug_flash_record).
   Unused, none of them changes what the game does or costs anything but a test of a flag. */
#ifndef PORT_DEBUG_H
#define PORT_DEBUG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "port/gs_capture_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Requests (any thread: the keys, the overlay's buttons) ----------------------------------- */
void port_debug_request_gs_capture(void);
void port_debug_request_gs_capture_into(const char *folder); /* the capture written into that folder */
/* F8, the flash recorder: a folder captures/flash_<vblank>_<time> with the pictures the presenter kept
   (port_vk_flash_write: NNNN.png, pictures.txt), a GS capture of the next vblanks, a quick save and a save
   state. The game's thread (the keys, the overlay). */
void port_debug_flash_record(void);
void port_debug_set_message(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void port_debug_write_state_dump(void); /* the game's thread, at a vblank */
void port_debug_write_audio_tail(void);
/* The captures folder, and what the last request did (for the overlay and stderr) */
const char *port_debug_folder(void);
const char *port_debug_message(void);
/* <captures>/<stem>_<vblank>_<local time>.<extension> into out (size bytes), the folders made if missing: out */
const char *port_debug_capture_path(const char *stem, uint64_t vblank, const char *extension, char *out,
                                    size_t size);
/* 1 while a GS capture is asked for or being recorded */
int port_debug_gs_capture_busy(void);

/* ---- Set-up (port/platform/main_host.c) ------------------------------------------------------- */
/* --gs-capture-at VBLANK[:N],...; called before port_gs_init, which asks port_debug_wants_gs */
void port_debug_init(const char *gs_capture_at);
/* 1 if a GS capture is asked for at a vblank (--gs-capture-at, KF4_DEBUG_AT's gs@): a --headless
   run then draws, as for --dump-frames, or there would be nothing to capture */
int port_debug_wants_gs(void);
/* each port_event line (files read, movies, the memory card), kept for F10's dump */
void port_debug_note_event(uint64_t vblank, const char *fmt, va_list args);

/* ---- The GS capture's hooks (port/platform/gs.c, vif1.c), on the DMA controller's thread --------
   port_gs_capture_recording is 1 only between a capture's start and end; port_gs_capture_armed is
   non-zero while one is asked for or recorded. */
extern int port_gs_capture_recording;
extern volatile int port_gs_capture_armed;
void port_gs_capture_gif(int path, const void *data, size_t bytes);
void port_gs_capture_vif1(const uint32_t *words, size_t count);
void port_gs_capture_vu1_run(uint32_t address, uint32_t top, uint32_t itop);
void port_gs_capture_read_fifo(uint32_t qwords);
/* after each scanout: privileged = the 0xF0 bytes of PMODE..BGCOLOR */
void port_gs_capture_vsync(const uint64_t *privileged, int field, uint64_t vblank);
/* the game's thread, at each vblank while armed: notes the game's state for the capture's file */
void port_gs_capture_note_vblank(uint64_t vblank);

/* ---- What the capture reads (port/platform/vu1.c, vif1.c, port/gs/gs_parallel.cpp) ------------- */
void port_vu1_debug_regs(Kf4GsVu1Regs *regs, int set);
int port_vu1_debug_idle(void);
extern int port_vu1_round_nearest;
int port_vif1_debug_regs(Kf4GsVif1Regs *regs, int set);
/* The backend's state as capture records (VRAM, GS_REGS, GS_PRIV, GS_HISTORY) through emit; and one
   record put back (kf4-gsreplay), VRAM first: 1 when it was taken */
void port_gs_backend_capture(void (*emit)(uint32_t type, const void *data, size_t size));
int port_gs_backend_restore(uint32_t type, const void *data, size_t size);
/* Waits for the GS and returns its VRAM as the GS lays it out (4 MB) */
const void *port_gs_backend_vram(void);
/* A rectangle of a buffer read back through the GS itself, as RGBA: for comparing the renderers */
void port_gs_backend_read_rect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w,
                               uint32_t h, uint32_t *rgba);
/* The renderer's state as capture records: paraLLEl-GS's, or the native GS's (port/platform/gs.c) */
void port_gs_state_records(void (*emit)(uint32_t type, const void *data, size_t size));

/* ---- The sound (port/audio/output.c): frames as the SPU2 made them, before the volume ----------- */
void port_debug_audio_frames(const int16_t *frames, int count);

/* ---- The game's state as JSON (port/debug/game_dump.c, compiled with the game code) ------------ */
void kf4_game_dump(FILE *file);

#ifdef __cplusplus
}
#endif

#endif
