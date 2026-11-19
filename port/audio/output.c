/* The SPU2's output (port/include/port/audio.h), on one of two clocks.

   The game's clock (headless, --audio-dump, --input, --fast, --no-audio, or no sound device): the SPU2
   is rendered up to the current EE time at each vblank, at each of the driver's timer ticks (a kernel
   event) and before each call to the driver, so what the driver reads back (a stream's state, a voice's
   envelope) is as up to date as on the PS2, and the same input gives the same sound to the byte. The
   frames go to the WAV file and, with a window, are queued for the sound device.

   The sound device's clock (a window and a device, none of the above): the IOP side runs on the clock
   it has on the PS2, its own. SDL's audio thread renders the SPU2 when the device asks for sound and
   runs the driver's 240 Hz tick between the frames it renders, so the music and the streams play on
   while the game's thread is held up (a load, a shader built, a host slower than the PS2) instead of
   stopping and starting. The game's calls to the driver take the same lock and happen at the device's
   current frame. What the game reads back then depends on how far the device has played, so a run is
   not repeatable to the frame: the tests use the game's clock. If the device stops asking for sound
   for half a second, the game's clock takes over for the rest of the run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/audio.h"
#include "port/debug.h"
#include "port/platform.h"
#include "port/savestate.h"
#include "sd.h"
#include "spu2.h"

#ifdef KF4_HAVE_SDL
#include <SDL3/SDL.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

void fsiopsnd_reset(void);
uint64_t fsiopsnd_next_tick(void);
void fsiopsnd_tick(uint64_t now);
void fsiopsnd_tick_device(void);
void fsiopsnd_resume_ticks(uint64_t now);

#define CHUNK_FRAMES 512
/* The driver's tick in SPU2 frames: 614448 EE cycles / 3072 = 200 + 1/64 */
#define TICK_FRAMES_X64 12801u

static int initialised;
static int device_clock;      /* the IOP side runs on the sound device's clock */
static int64_t frame_offset;  /* the SPU2 frame at EE time 0, after a change of clock */
static FILE *wav;
static uint32_t wav_frames;
/* the player's volume (port_audio_set_volume), for the sound device only: 0x10000 is the sound as made */
static volatile int32_t device_gain = 0x10000;

/* ---- The lock over the SPU2, libsd and the driver (the game's thread against SDL's audio thread) - */
#ifdef _WIN32
static SRWLOCK audio_lock = SRWLOCK_INIT;
void port_audio_lock(void) {
    AcquireSRWLockExclusive(&audio_lock);
}
void port_audio_unlock(void) {
    ReleaseSRWLockExclusive(&audio_lock);
}
#else
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
void port_audio_lock(void) {
    pthread_mutex_lock(&audio_lock);
}
void port_audio_unlock(void) {
    pthread_mutex_unlock(&audio_lock);
}
#endif

#ifdef KF4_HAVE_SDL
#define DEVICE_START_BYTES (SPU2_RATE * 4u / 10u)
#define DEVICE_MAX_BYTES (SPU2_RATE * 4u / 2u)
#define DEVICE_STALL_MS 500u

static SDL_AudioStream *device; /* the sound device's stream (SDL_OpenAudioDeviceStream), NULL with none */
static int device_playing;          /* the game's clock: the queue has started */
static uint64_t tick_due_x64;       /* the device's clock: the next tick, in 1/64 SPU2 frames */
static volatile uint32_t last_callback;
static int stats_on;
static uint32_t stats_time;
static uint32_t stats_min = 0xFFFFFFFFu;
static unsigned stats_dry;
static unsigned stats_drops;
static uint64_t stats_frames;
#endif

static void wav_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* The 44-byte header of 48 kHz 16-bit stereo PCM with `frames` frames. */
static void wav_header(uint8_t header[44], uint32_t frames) {
    uint32_t bytes = frames * 4u;

    memcpy(header, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\0\0\0\0\0\0\0\0\x04\0\x10\0data\0\0\0\0", 44);
    wav_put32(header + 4, 36u + bytes);
    wav_put32(header + 24, SPU2_RATE);
    wav_put32(header + 28, SPU2_RATE * 4u);
    wav_put32(header + 40, bytes);
}

/* ---- The player's volume -------------------------------------------------------------------------- */
void port_audio_set_volume(float gain) {
    device_gain = gain <= 0.0f ? 0 : gain >= 1.0f ? 0x10000 : (int32_t)(gain * 65536.0f + 0.5f);
}

#ifdef KF4_HAVE_SDL
/* Scales frames for the device (the WAV dump keeps the sound as made) */
static void apply_volume(int16_t *frames, int count) {
    int32_t gain = device_gain;
    int i;

    if (gain == 0x10000) {
        return;
    }
    for (i = 0; i < count * 2; i++) {
        frames[i] = (int16_t)(((int32_t)frames[i] * gain) >> 16);
    }
}
#endif

/* ---- The device's clock --------------------------------------------------------------------------- */
#ifdef KF4_HAVE_SDL
/* SDL's audio thread: the frames the device asks for (`wanted` bytes more), with the driver's ticks between
   them, put into the device's stream a chunk at a time */
static void device_callback(void *user, SDL_AudioStream *stream, int wanted, int total) {
    int16_t chunk[CHUNK_FRAMES * 2];
    int frames = (wanted + 3) / 4;

    (void)user;
    (void)total;
    port_audio_lock();
    last_callback = (uint32_t)SDL_GetTicks();
    if (!device_clock) {
        port_audio_unlock();
        memset(chunk, 0, sizeof(chunk));
        while (frames > 0) {
            int count = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;

            SDL_PutAudioStreamData(stream, chunk, count * 4);
            frames -= count;
        }
        return;
    }
    while (frames > 0) {
        uint64_t now_x64 = spu2_time() * 64u;
        int count = frames < CHUNK_FRAMES ? frames : CHUNK_FRAMES;

        if (tick_due_x64 <= now_x64) {
            sd_set_time_render();
            fsiopsnd_tick_device();
            tick_due_x64 += TICK_FRAMES_X64;
            continue;
        }
        if ((tick_due_x64 - now_x64 + 63u) / 64u < (uint64_t)count) {
            count = (int)((tick_due_x64 - now_x64 + 63u) / 64u);
        }
        spu2_render(chunk, count);
        port_debug_audio_frames(chunk, count); /* F12's last 20 seconds (port/debug/debug.c) */
        apply_volume(chunk, count);
        SDL_PutAudioStreamData(stream, chunk, count * 4);
        frames -= count;
    }
    port_audio_unlock();
}

/* The device stopped asking for sound: the game's clock from here, from the frame the SPU2 is at */
static void fall_back_to_game_clock(void) {
    port_audio_lock();
    device_clock = 0;
    sd_set_clock(0, frame_offset = (int64_t)spu2_time() - (int64_t)(port_ee_cycles() / AUDIO_EE_CYCLES_PER_FRAME));
    fsiopsnd_resume_ticks(port_ee_cycles());
    port_audio_unlock();
    SDL_DestroyAudioStream(device); /* (closes the device; waits for a callback still running) */
    device = NULL;
    fprintf(stderr, "[audio] the sound device stopped asking for sound; the sound now follows the game's clock, "
                    "without a device\n");
}
#endif

static void open_outputs(void) {
    if (port_options.audio_dump != NULL) {
        uint8_t header[44];

        wav = fopen(port_options.audio_dump, "wb");
        if (wav == NULL) {
            fprintf(stderr, "[audio] cannot write %s\n", port_options.audio_dump);
            exit(2);
        }
        wav_header(header, 0);
        fwrite(header, 1, sizeof(header), wav);
    }
#ifdef KF4_HAVE_SDL
    stats_on = getenv("KF4_AUDIO_STATS") != NULL;
    if (!port_options.headless && !port_options.no_audio) {
        SDL_AudioSpec spec;
        int pull = (port_options.audio_dump == NULL && port_options.input == NULL && !port_options.fast &&
                    port_options.record_input == NULL) ||
                   getenv("KF4_AUDIO_DEVICE_CLOCK") != NULL;

        /* the device's buffer, 1024 frames (21 ms): asked for before the audio starts */
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[audio] no sound: %s\n", SDL_GetError());
            return;
        }
        spec.freq = SPU2_RATE;
        spec.format = SDL_AUDIO_S16LE;
        spec.channels = 2;
        /* (the stream converts to the device's own format and rate, if they differ; it starts paused) */
        device = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull ? device_callback : NULL,
                                           NULL);
        if (device == NULL) {
            fprintf(stderr, "[audio] no sound device: %s\n", SDL_GetError());
            return;
        }
        if (pull) {
            device_clock = 1;
            sd_set_clock(1, 0);
            tick_due_x64 = TICK_FRAMES_X64;
            last_callback = (uint32_t)SDL_GetTicks();
            SDL_ResumeAudioStreamDevice(device);
        }
    }
#endif
}

/* The game's clock: frames to the WAV file and the device's queue */
static void output(const int16_t *frames, int count) {
    if (wav != NULL) {
        fwrite(frames, 4, (size_t)count, wav);
        wav_frames += (uint32_t)count;
    }
#ifdef KF4_HAVE_SDL
    if (device != NULL) {
        int queued_bytes = SDL_GetAudioStreamQueued(device);
        uint32_t queued = queued_bytes > 0 ? (uint32_t)queued_bytes : 0u;

        if (stats_on) {
            uint32_t now = (uint32_t)SDL_GetTicks();

            if (device_playing && queued < stats_min) {
                stats_min = queued;
            }
            stats_frames += (uint64_t)count;
            if (now - stats_time >= 1000) {
                fprintf(stderr, "[audio] game clock: %.1f%% of real time made, queue min %u ms, ran dry %u, dropped %u\n",
                        100.0 * (double)stats_frames / ((now - stats_time) * 48.0),
                        stats_min == 0xFFFFFFFFu ? 0u : stats_min / 192u, stats_dry, stats_drops);
                stats_time = now;
                stats_min = 0xFFFFFFFFu;
                stats_dry = stats_drops = 0;
                stats_frames = 0;
            }
        }
        if (device_playing && queued == 0) {
            /* ran dry: wait to have 100 ms again */
            stats_dry++;
            SDL_PauseAudioStreamDevice(device);
            device_playing = 0;
        } else if (queued > DEVICE_MAX_BYTES) {
            stats_drops++;
            SDL_ClearAudioStream(device);
        }
        if (device_gain != 0x10000) {
            int16_t scaled[CHUNK_FRAMES * 2];

            if (count > CHUNK_FRAMES) {
                count = CHUNK_FRAMES;
            }
            memcpy(scaled, frames, (size_t)count * 4u);
            apply_volume(scaled, count);
            SDL_PutAudioStreamData(device, scaled, count * 4);
        } else {
            SDL_PutAudioStreamData(device, frames, count * 4);
        }
        if (!device_playing && SDL_GetAudioStreamQueued(device) >= (int)DEVICE_START_BYTES) {
            SDL_ResumeAudioStreamDevice(device);
            device_playing = 1;
        }
    }
#endif
}

void port_audio_finish(void) {
    if (wav != NULL) {
        uint8_t header[44];

        wav_header(header, wav_frames);
        fseek(wav, 0, SEEK_SET);
        fwrite(header, 1, sizeof(header), wav);
        fclose(wav);
        wav = NULL;
    }
#ifdef KF4_HAVE_SDL
    if (device != NULL) {
        SDL_DestroyAudioStream(device);
        device = NULL;
    }
#endif
}

void port_audio_init(void) {
    spu2_reset();
    port_audio_load_libsd();
    sd_reset();
    fsiopsnd_reset();
    open_outputs();
    initialised = 1;
    atexit(port_audio_finish);
}

int port_audio_device_clock(void) {
    return device_clock;
}

void port_audio_sync(uint64_t now) {
    int64_t target;
    int16_t chunk[CHUNK_FRAMES * 2];

    if (!initialised) {
        return;
    }
#ifdef KF4_HAVE_SDL
    if (device_clock) {
        uint32_t since = (uint32_t)SDL_GetTicks() - last_callback;

        if (since > DEVICE_STALL_MS) {
            fall_back_to_game_clock();
        } else {
            if (stats_on && (uint32_t)SDL_GetTicks() - stats_time >= 1000) {
                fprintf(stderr, "[audio] device clock: SPU2 at %.2f s, game at %.2f s\n", spu2_time() / 48000.0,
                        (double)now / 147456000.0);
                stats_time = (uint32_t)SDL_GetTicks();
            }
            return;
        }
    }
#endif
    target = (int64_t)(now / AUDIO_EE_CYCLES_PER_FRAME) + frame_offset;
    while ((int64_t)spu2_time() < target) {
        int64_t left = target - (int64_t)spu2_time();
        int count = left < CHUNK_FRAMES ? (int)left : CHUNK_FRAMES;

        spu2_render(chunk, count);
        port_debug_audio_frames(chunk, count);
        output(chunk, count);
    }
}

uint64_t port_audio_next_event(void) {
    return initialised && !device_clock ? fsiopsnd_next_tick() : UINT64_MAX;
}

void port_audio_event(uint64_t now) {
    port_audio_sync(now);
    fsiopsnd_tick(now);
}

/* ---- Save states -------------------------------------------------------------------------------
   The SPU2 (ico-audio's, saved whole with the program's image) and the driver come back as they were;
   this is the clock between them and the EE. The clock is this run's own: a state taken on the
   game's clock goes on on the device's, and the other way round, from where the SPU2 and the
   driver's next tick were. */
void port_audio_state(PortStateIo *io) {
    int state_device_clock = device_clock;
    int64_t state_frame_offset = frame_offset;
    uint64_t state_tick_due_x64 = 0;
    uint64_t now = port_ee_cycles();

#ifdef KF4_HAVE_SDL
    state_tick_due_x64 = tick_due_x64;
#endif
    PORT_STATE(io, state_device_clock);
    PORT_STATE(io, state_frame_offset);
    PORT_STATE(io, state_tick_due_x64);
    if (!io->loading) {
        return;
    }
    if (!device_clock) {
        frame_offset = state_device_clock ? (int64_t)spu2_time() - (int64_t)(now / AUDIO_EE_CYCLES_PER_FRAME)
                                          : state_frame_offset;
        sd_set_clock(0, frame_offset);
        if (state_device_clock) {
            fsiopsnd_resume_ticks(now);
        }
        return;
    }
#ifdef KF4_HAVE_SDL
    if (state_device_clock) {
        tick_due_x64 = state_tick_due_x64;
    } else {
        uint64_t next = fsiopsnd_next_tick();
        uint64_t ahead = next != UINT64_MAX && next > now ? next - now : 0;

        tick_due_x64 = spu2_time() * 64u + (ahead * 64u + AUDIO_EE_CYCLES_PER_FRAME - 1) / AUDIO_EE_CYCLES_PER_FRAME;
    }
    sd_set_clock(1, 0);
#endif
}
