/* The EE's DMA controller, played by a host thread (port/include/port/platform.h).

   The game starts a channel by storing STR into its CHCR, in about fifty places, and then waits in a
   loop for the hardware to clear it again, often with no call to the host in between (the menus draw
   each glyph that way). Nothing on the PC sees a store into the register page, so this thread looks
   at the ten CHCRs, finishes each channel it finds started and clears its STR, as the hardware does
   alongside the CPU. For step 1 finishing a transfer means nothing more: the packets are not read
   yet. Graphics (step 2) reads the VIF1 and GIF chains here, before STR is cleared, so the game cannot
   reuse a packet the GS has not taken.

   The game's EE time does not move while it spins on a register (port/platform/kernel.c), so how soon
   this thread clears STR never changes what the game does, only how long the host takes.

   The thread spins while the game is starting channels, and naps once it has been idle for a while.
   The calls that come before a game's own channel start (FlushCache, the libgraph and libdma calls)
   wake it, so the nap rarely costs the game a wait. */
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif
#include <stdint.h>

#include "port/host.h"
#include "port/platform.h"

#define CHCR_STR 0x100
/* polls with nothing started before the thread naps, and the nap */
#define SPINS_BEFORE_NAP 200000
#define NAP_SECONDS 0.0002

static const uint32_t chcr_address[10] = {
    0x10008000, /* 0 VIF0 */
    0x10009000, /* 1 VIF1 */
    0x1000A000, /* 2 GIF */
    0x1000B000, /* 3 from IPU */
    0x1000B400, /* 4 to IPU */
    0x1000C000, /* 5 SIF0 */
    0x1000C400, /* 6 SIF1 */
    0x1000C800, /* 7 SIF2 */
    0x1000D000, /* 8 from scratchpad */
    0x1000D400, /* 9 to scratchpad */
};

static volatile int *chcr[10];
static volatile uint64_t transfers[10];
static volatile int wake_requested;

#ifdef _WIN32
static CRITICAL_SECTION nap_lock;
static CONDITION_VARIABLE nap_signal;
#else
static pthread_mutex_t nap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t nap_signal = PTHREAD_COND_INITIALIZER;
#endif

static void cpu_relax(void) {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#endif
}

static void nap(void) {
#ifdef _WIN32
    EnterCriticalSection(&nap_lock);
    if (!__atomic_load_n(&wake_requested, __ATOMIC_ACQUIRE)) {
        SleepConditionVariableCS(&nap_signal, &nap_lock, 1);
    }
    LeaveCriticalSection(&nap_lock);
#else
    struct timespec until;

    pthread_mutex_lock(&nap_lock);
    if (!__atomic_load_n(&wake_requested, __ATOMIC_ACQUIRE)) {
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (long)(NAP_SECONDS * 1e9);
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&nap_signal, &nap_lock, &until);
    }
    pthread_mutex_unlock(&nap_lock);
#endif
}

/* Finishes the channels that are started; 1 when there was one. */
static int run_channels(void) {
    int found = 0;
    int channel;

    for (channel = 0; channel < 10; channel++) {
        int value = __atomic_load_n(chcr[channel], __ATOMIC_ACQUIRE);

        if (value & CHCR_STR) {
            /* step 2: the channel's chain is read here */
            __atomic_fetch_and(chcr[channel], ~CHCR_STR, __ATOMIC_RELEASE);
            __atomic_fetch_add(&transfers[channel], 1, __ATOMIC_RELAXED);
            port_kernel_post_dmac(channel);
            found = 1;
        }
    }
    return found;
}

#ifdef _WIN32
static DWORD WINAPI dmac_thread(void *unused)
#else
static void *dmac_thread(void *unused)
#endif
{
    int idle = 0;

    (void)unused;
    for (;;) {
        if (run_channels()) {
            idle = 0;
            continue;
        }
        if (__atomic_exchange_n(&wake_requested, 0, __ATOMIC_ACQ_REL)) {
            idle = 0;
        }
        if (++idle < SPINS_BEFORE_NAP) {
            cpu_relax();
            continue;
        }
        nap();
    }
    return 0;
}

void port_dmac_start(void) {
    int channel;

    for (channel = 0; channel < 10; channel++) {
        chcr[channel] = EE_HW(volatile int *, chcr_address[channel]);
    }
#ifdef _WIN32
    InitializeCriticalSection(&nap_lock);
    InitializeConditionVariable(&nap_signal);
    if (CreateThread(NULL, 0, dmac_thread, NULL, 0, NULL) == NULL) {
        PORT_FATAL("cannot start the DMA controller's thread");
    }
#else
    {
        pthread_t thread;

        if (pthread_create(&thread, NULL, dmac_thread, NULL) != 0) {
            PORT_FATAL("cannot start the DMA controller's thread");
        }
        pthread_detach(thread);
    }
#endif
}

void port_dmac_wake(void) {
    if (__atomic_exchange_n(&wake_requested, 1, __ATOMIC_ACQ_REL)) {
        return;
    }
#ifdef _WIN32
    EnterCriticalSection(&nap_lock);
    WakeConditionVariable(&nap_signal);
    LeaveCriticalSection(&nap_lock);
#else
    pthread_mutex_lock(&nap_lock);
    pthread_cond_signal(&nap_signal);
    pthread_mutex_unlock(&nap_lock);
#endif
}

void port_dmac_wait(int channel) {
    port_dmac_wake();
    while (__atomic_load_n(chcr[channel], __ATOMIC_ACQUIRE) & CHCR_STR) {
        cpu_relax();
    }
}

volatile int *port_dmac_chcr(int channel) {
    return chcr[channel];
}

uint64_t port_dmac_transfers(int channel) {
    return __atomic_load_n(&transfers[channel], __ATOMIC_RELAXED);
}
