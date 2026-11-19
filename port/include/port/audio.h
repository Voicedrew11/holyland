/* The sound side of the PC port (port/audio): the IOP's sound driver, FromSoftware's FSIOPSND, with
   the two Sony CSL modules it drives (modmidi, the MIDI sequencer, and modhsyn, the synthesizer), on
   the software SPU2 of third_party/ico-audio.

   Everything runs on EE time (port/platform/kernel.c), never on the host's: the driver's timer
   ticks, its replies and every SPU2 register write land at the same EE cycle on every run, so the
   sound the game hears back (stream states, transfer status) is deterministic, and so is the audio. */
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#include <stdint.h>

/* ---- IOP memory (port/audio/iop.c) ----------------------------------------------------------
   The IOP's 2 MB. The game sends data to it by SIF DMA and names it to the driver by IOP address. */
#define IOP_RAM_SIZE 0x200000u
uint8_t *port_iop_ram(void);
/* The host address of an IOP address (wrapped to the 2 MB, as the IOP's bus does). */
void *port_iop_ptr(uint32_t iop_address);

/* ---- The clock (port/audio/output.c) --------------------------------------------------------
   The SPU2 renders 48 kHz frames; one frame is 3072 EE bus cycles (147.456 MHz / 48 kHz). */
#define AUDIO_EE_CYCLES_PER_FRAME 3072u
void port_audio_init(void);
/* Reads libsd's reverb presets and idle voice block from the disc's LIBSD.IRX (port/audio/libsd_irx.c) */
void port_audio_load_libsd(void);
/* The EE cycle of the driver's next timer tick (UINT64_MAX: none). The kernel runs it at that time. */
uint64_t port_audio_next_event(void);
void port_audio_event(uint64_t now);
/* Renders the SPU2 up to the EE time `now` and hands the frames to the outputs. */
void port_audio_sync(uint64_t now);
/* At the end of the run: flushes the outputs. */
void port_audio_finish(void);
/* 1 while the IOP side runs on the sound device's clock (a window and a device; port/audio/output.c) */
int port_audio_device_clock(void);
/* The player's volume, 0 to 1 (the settings overlay): scales what the sound device plays, not the
   sound the game makes, nor --audio-dump */
void port_audio_set_volume(float gain);
/* The lock over the SPU2, libsd and the driver, taken by the game's calls and by the audio thread */
void port_audio_lock(void);
void port_audio_unlock(void);

/* ---- The sound driver's RPC server, 0x1010 (port/audio/fsiopsnd.c) --------------------------- */
int port_fsiopsnd_call(unsigned int function, void *send_buffer, int send_size, void *receive_buffer,
                       int receive_size);

/* ---- SDRDRV, libsdr's server (port/audio/sdrdrv.c) -------------------------------------------
   sceSdRemote's libsd calls (the movie player's sound). How many arguments a command takes (-1: not
   one SDRDRV knows), and the call. */
int port_sdrdrv_arg_count(int command);
int port_sdrdrv_call(int command, const uint32_t *args);

/* Disc movies can preload their PCM so SDRDRV plays it independently of video decoding. */
int port_movie_audio_load(uint32_t lsn, uint32_t file_size);
void port_movie_audio_unload(void);
/* The elapsed picture on the device clock, or 0 when there is no independent movie audio clock. */
uint64_t port_movie_audio_picture(void);

#endif
