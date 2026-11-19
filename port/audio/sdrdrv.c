/* SDRDRV.IRX, the IOP end of libsdr: sceSdRemote runs libsd's calls on the IOP by number
   (port/include/port/audio.h). KF4 loads it for the movie player (gfx/movie.c), which plays a movie's
   sound through it. Supported disc movies preload PCM for core 0's sound data input. The original
   path feeds the same input through an emulated IOP ring. Core 1 mixes core 0's output in.

   The calls go to the same libsd as the sound driver's (port/audio/sd.c), at the EE time of the call,
   as port_fsiopsnd_call does. */
#include <stdlib.h>
#include <string.h>

#include "libmpeg.h"
#include "port/audio.h"
#include "port/host.h"
#include "port/platform.h"
#include "sd.h"
#include "spu2.h"

/* libsdr's command numbers (rSdXxx) */
#define SDR_INIT 0x8000
#define SDR_SET_PARAM 0x8010
#define SDR_GET_PARAM 0x8020
#define SDR_SET_SWITCH 0x8030
#define SDR_GET_SWITCH 0x8040
#define SDR_SET_ADDR 0x8050
#define SDR_GET_ADDR 0x8060
#define SDR_SET_CORE_ATTR 0x8070
#define SDR_GET_CORE_ATTR 0x8080
#define SDR_VOICE_TRANS 0x80D0
#define SDR_BLOCK_TRANS 0x80E0
#define SDR_VOICE_TRANS_STATUS 0x80F0
#define SDR_BLOCK_TRANS_STATUS 0x8100

/* SPU2 input halves have 256 little-endian samples of each channel. All playing state is under
   the audio lock, including the half-done callback from the SPU2 renderer. */
enum { MOVIE_PCM_BLOCK = 1024, MOVIE_PICTURES_PER_SECOND = 30 };
static uint8_t *movie_pcm;
static uint32_t movie_pcm_size;
static uint32_t movie_pcm_next;
static uint64_t movie_audio_start_frame;
static int movie_audio_playing;
static uint8_t movie_memin[2 * MOVIE_PCM_BLOCK];

int port_movie_audio_load(uint32_t lsn, uint32_t file_size) {
    movie_pcm = port_mpeg_load_pcm(lsn, file_size, &movie_pcm_size);
    if (movie_pcm == NULL) {
        port_event("movie: PCM preload unavailable, using the IOP stream");
        return 0;
    }
    port_event("movie: preloaded %u bytes of PCM", movie_pcm_size);
    return 1;
}

static void movie_audio_fill(int core, int half, void *user) {
    uint8_t *dest = movie_memin + half * MOVIE_PCM_BLOCK;

    (void)core;
    (void)user;
    if (movie_pcm_next < movie_pcm_size) {
        memcpy(dest, movie_pcm + movie_pcm_next, MOVIE_PCM_BLOCK);
        movie_pcm_next += MOVIE_PCM_BLOCK;
    } else {
        memset(dest, 0, MOVIE_PCM_BLOCK);
    }
}

static void movie_audio_start(void) {
    sd_block_trans(0, SD_TRANS_STOP, 0, 0);
    movie_pcm_next = 0;
    movie_audio_start_frame = spu2_time();
    movie_audio_fill(0, 0, NULL);
    movie_audio_fill(0, 1, NULL);
    spu2_memin_start(0, movie_memin, movie_audio_fill, NULL);
    movie_audio_playing = 1;
}

static void movie_audio_stop(void) {
    if (movie_audio_playing) {
        spu2_memin_stop(0);
        movie_audio_playing = 0;
    }
}

void port_movie_audio_unload(void) {
    port_audio_lock();
    movie_audio_stop();
    port_audio_unlock();
    free(movie_pcm);
    movie_pcm = NULL;
    movie_pcm_size = 0;
}

uint64_t port_movie_audio_picture(void) {
    uint64_t picture = 0;

    if (port_audio_device_clock()) {
        port_audio_lock();
        if (movie_audio_playing) {
            picture = (spu2_time() - movie_audio_start_frame) / (SPU2_RATE / MOVIE_PICTURES_PER_SECOND);
        }
        port_audio_unlock();
    }
    return picture;
}

int port_sdrdrv_arg_count(int command) {
    switch (command) {
    case SDR_INIT:
    case SDR_GET_PARAM:
    case SDR_GET_SWITCH:
    case SDR_GET_ADDR:
    case SDR_GET_CORE_ATTR:
        return 1;
    case SDR_SET_PARAM:
    case SDR_SET_SWITCH:
    case SDR_SET_ADDR:
    case SDR_SET_CORE_ATTR:
    case SDR_VOICE_TRANS_STATUS:
    case SDR_BLOCK_TRANS_STATUS:
        return 2;
    case SDR_BLOCK_TRANS:
        return 4; /* and a fifth, the start address, for a transfer that starts mid-ring */
    case SDR_VOICE_TRANS:
        return 5;
    default:
        return -1;
    }
}

int port_sdrdrv_call(int command, const uint32_t *args) {
    uint64_t now = port_ee_cycles();
    const char *module;
    int result = 0;

    port_audio_sync(now);
    port_audio_lock();
    sd_set_time(now);
    module = sd_trace_module("SDRDRV");
    switch (command) {
    case SDR_INIT:
        result = sd_init((int)args[0]);
        break;
    case SDR_SET_PARAM:
        sd_set_param((uint16_t)args[0], (uint16_t)args[1]);
        break;
    case SDR_GET_PARAM:
        result = sd_get_param((uint16_t)args[0]);
        break;
    case SDR_SET_SWITCH:
        sd_set_switch((uint16_t)args[0], args[1]);
        break;
    case SDR_GET_SWITCH:
        result = (int)sd_get_switch((uint16_t)args[0]);
        break;
    case SDR_SET_ADDR:
        sd_set_addr((uint16_t)args[0], args[1]);
        break;
    case SDR_GET_ADDR:
        result = (int)sd_get_addr((uint16_t)args[0]);
        break;
    case SDR_SET_CORE_ATTR:
        sd_set_core_attr((uint16_t)args[0], (uint16_t)args[1]);
        break;
    case SDR_GET_CORE_ATTR:
        result = sd_get_core_attr((uint16_t)args[0]);
        break;
    case SDR_VOICE_TRANS:
        result = sd_voice_trans((int16_t)args[0], (uint16_t)args[1], args[2], args[3], args[4]);
        break;
    case SDR_VOICE_TRANS_STATUS:
        result = sd_voice_trans_status((int16_t)args[0], (int16_t)args[1]);
        break;
    case SDR_BLOCK_TRANS:
        if (movie_pcm != NULL && args[0] == 0 && args[1] == SD_TRANS_STOP) {
            movie_audio_stop();
            break;
        }
        if (movie_pcm != NULL && args[0] == 0 && args[1] == (SD_TRANS_WRITE | SD_TRANS_LOOP)) {
            movie_audio_start();
            break;
        }
        if ((args[1] & 3) == 3) {
            PORT_TODO("sceSdRemote: a block transfer that starts mid-ring (the movie player's resume)");
        }
        result = sd_block_trans((int16_t)args[0], (uint16_t)args[1], args[2], args[3]);
        break;
    case SDR_BLOCK_TRANS_STATUS:
        result = (int)sd_block_trans_status((int16_t)args[0], (int16_t)args[1]);
        break;
    default:
        port_event("sceSdRemote: command 0x%X is not implemented", (unsigned)command);
        break;
    }
    sd_trace_module(module);
    port_audio_unlock();
    return result;
}
