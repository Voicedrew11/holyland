/* The screen's vblanks, as the system knows them (port/include/port/screen_vblank.h) */
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#elif defined(KF4_HAVE_LIBDRM)
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#include "port/platform.h"
#include "port/screen_vblank.h"

#ifdef _WIN32
int port_screen_vblank_open(int width, int height, int refresh_hz) {
    double at;

    (void)width;
    (void)height;
    (void)refresh_hz;
    return port_screen_vblank_last(&at);
}

int port_screen_vblank_last(double *at_s) {
    DWM_TIMING_INFO info;
    LARGE_INTEGER now, frequency;
    double wall = port_wall_seconds();

    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (FAILED(DwmGetCompositionTimingInfo(NULL, &info)) || info.qpcVBlank == 0) {
        return 0;
    }
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    *at_s = wall - (double)(int64_t)((uint64_t)now.QuadPart - info.qpcVBlank) / (double)frequency.QuadPart;
    return 1;
}
#elif defined(KF4_HAVE_LIBDRM)
#define CARDS 16

/* The card's file and the CRTC's index on it (-1: none), under the lock */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int card = -1, pipe_index = -1;

/* The index of the CRTC on the card showing width x height at refresh_hz, else the one CRTC at that rate
   (-1: none, or more than one at the rate and none of the size) */
static int find_pipe(int fd, int width, int height, int refresh_hz) {
    drmModeRes *resources = drmModeGetResources(fd);
    int i, by_size = -1, by_rate = -1, at_rate = 0;

    if (resources == NULL) {
        return -1;
    }
    for (i = 0; i < resources->count_crtcs && by_size < 0; i++) {
        drmModeCrtc *crtc = drmModeGetCrtc(fd, resources->crtcs[i]);

        if (crtc == NULL) {
            continue;
        }
        if (crtc->mode_valid && abs((int)crtc->mode.vrefresh - refresh_hz) <= 1) {
            at_rate++;
            by_rate = i;
            if (crtc->mode.hdisplay == width && crtc->mode.vdisplay == height) {
                by_size = i;
            }
        }
        drmModeFreeCrtc(crtc);
    }
    drmModeFreeResources(resources);
    return by_size >= 0 ? by_size : at_rate == 1 ? by_rate : -1;
}

int port_screen_vblank_open(int width, int height, int refresh_hz) {
    int n;

    pthread_mutex_lock(&lock);
    if (card >= 0) {
        close(card);
    }
    card = pipe_index = -1;
    for (n = 0; n < CARDS && pipe_index < 0; n++) {
        char path[32];

        snprintf(path, sizeof(path), "/dev/dri/card%d", n);
        card = open(path, O_RDWR | O_CLOEXEC);
        if (card < 0) {
            continue;
        }
        pipe_index = find_pipe(card, width, height, refresh_hz);
        if (pipe_index < 0) {
            close(card);
            card = -1;
        }
    }
    n = pipe_index >= 0;
    pthread_mutex_unlock(&lock);
    return n;
}

int port_screen_vblank_last(double *at_s) {
    drmVBlank vblank;
    struct timespec now;
    double wall;
    int known;

    pthread_mutex_lock(&lock);
    known = pipe_index >= 0;
    if (known) {
        /* (no vblank more than the last: answered at once) */
        memset(&vblank, 0, sizeof(vblank));
        vblank.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE |
                                                 ((pipe_index << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK));
        known = drmWaitVBlank(card, &vblank) == 0;
    }
    pthread_mutex_unlock(&lock);
    if (!known) {
        return 0;
    }
    /* the kernel's time is CLOCK_MONOTONIC's, port_wall_seconds' own from a start of its own */
    wall = port_wall_seconds();
    clock_gettime(CLOCK_MONOTONIC, &now);
    *at_s = wall - ((double)now.tv_sec - (double)vblank.reply.tval_sec) -
            ((double)now.tv_nsec * 1e-9 - (double)vblank.reply.tval_usec * 1e-6);
    return 1;
}
#else
int port_screen_vblank_open(int width, int height, int refresh_hz) {
    (void)width;
    (void)height;
    (void)refresh_hz;
    return 0;
}

int port_screen_vblank_last(double *at_s) {
    (void)at_s;
    return 0;
}
#endif
