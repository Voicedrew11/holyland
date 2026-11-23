/* The screen's vblanks, as the system knows them (port/include/port/screen_vblank.h) */
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <stdint.h>
#include <stdlib.h>
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
/* The kernel's vblanks (D3DKMTWaitForVerticalBlankEvent, gdi32's), for the screen showing the window: in exclusive
   fullscreen the desktop compositor does not compose the game's screen and its record of vblanks goes stale (measured
   2026-11-23 on an AMD laptop at 60 Hz: qpcVBlank moved on 12.9 times a second, 77.8 ms apart, against 60 a second,
   16.67 ms apart, on the desktop), so the refresh clock never locked there. A thread of its own waits for each vblank
   and keeps the time it woke; the compositor's record is used only while the thread has no recent vblank (no screen
   found, gdi32 without the calls, the waits failing). */

/* (d3dkmthk.h's, which not every MinGW has: looked up from gdi32 at the start) */
typedef UINT KmtHandle;
typedef struct {
    WCHAR DeviceName[32];
    KmtHandle hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} KmtOpenAdapterFromGdiDisplayName;
typedef struct {
    KmtHandle hAdapter;
    KmtHandle hDevice;
    UINT VidPnSourceId;
} KmtWaitForVerticalBlankEvent;
typedef struct {
    KmtHandle hAdapter;
} KmtCloseAdapter;
typedef LONG(APIENTRY *KmtOpenAdapterFn)(KmtOpenAdapterFromGdiDisplayName *);
typedef LONG(APIENTRY *KmtWaitFn)(const KmtWaitForVerticalBlankEvent *);
typedef LONG(APIENTRY *KmtCloseFn)(const KmtCloseAdapter *);

static KmtOpenAdapterFn kmt_open;
static KmtWaitFn kmt_wait;
static KmtCloseFn kmt_close;

/* Under the lock: the screen asked for (its GDI name, "" for none) and its generation; the last vblank the thread saw
   (its QPC count, 0 for none) */
static SRWLOCK lock = SRWLOCK_INIT;
static WCHAR wanted[32];
static unsigned wanted_generation;
static int64_t kernel_vblank_qpc;
static int64_t refresh_qpc; /* (a refresh's length in QPC counts, at the rate asked: how recent a vblank must be) */
static HANDLE thread;

#define WAIT_FAILURES 8 /* waits failing in a row before the screen is given up until it is asked for again */

/* The GDI name ("\\.\DISPLAY1") of the attached screen whose current mode is width x height at refresh_hz, else of the
   one attached screen at that rate; 0 when none or (by rate alone) more than one */
static int find_screen(int width, int height, int refresh_hz, WCHAR name[32]) {
    DISPLAY_DEVICEW device;
    DWORD i;
    int by_size = 0, by_rate = 0, at_rate = 0;
    WCHAR rate_name[32];

    for (i = 0;; i++) {
        DEVMODEW mode;

        ZeroMemory(&device, sizeof(device));
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(NULL, i, &device, 0)) {
            break;
        }
        if (!(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            continue;
        }
        ZeroMemory(&mode, sizeof(mode));
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode) ||
            abs((int)mode.dmDisplayFrequency - refresh_hz) > 1) {
            continue;
        }
        at_rate++;
        if (!by_rate) {
            lstrcpynW(rate_name, device.DeviceName, 32);
            by_rate = 1;
        }
        if ((int)mode.dmPelsWidth == width && (int)mode.dmPelsHeight == height) {
            lstrcpynW(name, device.DeviceName, 32);
            by_size = 1;
            break;
        }
    }
    if (by_size) {
        return 1;
    }
    if (by_rate && at_rate == 1) {
        lstrcpynW(name, rate_name, 32);
        return 1;
    }
    return 0;
}

/* The thread: the screen asked for opened, each of its vblanks waited for and its time kept, opened again when another
   is asked for */
static DWORD WINAPI vblank_thread(LPVOID unused) {
    unsigned generation = 0;
    KmtWaitForVerticalBlankEvent wait;
    int open = 0, failures = 0;

    (void)unused;
    /* (the time kept is the time it woke: woken late, the vblank's time is late too) */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ZeroMemory(&wait, sizeof(wait));
    for (;;) {
        WCHAR name[32];
        unsigned asked;
        LARGE_INTEGER now;

        AcquireSRWLockShared(&lock);
        asked = wanted_generation;
        lstrcpynW(name, wanted, 32);
        ReleaseSRWLockShared(&lock);
        if (asked != generation) {
            generation = asked;
            if (open) {
                KmtCloseAdapter close = {wait.hAdapter};
                kmt_close(&close);
                open = 0;
            }
            failures = 0;
            if (name[0] != 0) {
                KmtOpenAdapterFromGdiDisplayName adapter;

                ZeroMemory(&adapter, sizeof(adapter));
                lstrcpynW(adapter.DeviceName, name, 32);
                if (kmt_open(&adapter) == 0) {
                    wait.hAdapter = adapter.hAdapter;
                    wait.hDevice = 0;
                    wait.VidPnSourceId = adapter.VidPnSourceId;
                    open = 1;
                }
            }
        }
        if (!open) {
            Sleep(50); /* (nothing to wait for until another screen is asked for) */
            continue;
        }
        if (kmt_wait(&wait) != 0) {
            if (++failures >= WAIT_FAILURES) {
                KmtCloseAdapter close = {wait.hAdapter};
                kmt_close(&close);
                open = 0;
            } else {
                Sleep(5);
            }
            continue;
        }
        failures = 0;
        QueryPerformanceCounter(&now);
        AcquireSRWLockExclusive(&lock);
        kernel_vblank_qpc = now.QuadPart;
        ReleaseSRWLockExclusive(&lock);
    }
    return 0;
}

int port_screen_vblank_open(int width, int height, int refresh_hz) {
    static int looked_up;
    WCHAR name[32];
    LARGE_INTEGER frequency;
    double at;
    int found;

    if (!looked_up) {
        HMODULE gdi = LoadLibraryW(L"gdi32.dll");

        looked_up = 1;
        if (gdi != NULL) {
            kmt_open = (KmtOpenAdapterFn)(void (*)(void))GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName");
            kmt_wait = (KmtWaitFn)(void (*)(void))GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent");
            kmt_close = (KmtCloseFn)(void (*)(void))GetProcAddress(gdi, "D3DKMTCloseAdapter");
        }
    }
    found = kmt_open && kmt_wait && kmt_close && refresh_hz > 0 && find_screen(width, height, refresh_hz, name);
    QueryPerformanceFrequency(&frequency);
    AcquireSRWLockExclusive(&lock);
    lstrcpynW(wanted, found ? name : L"", 32);
    wanted_generation++;
    kernel_vblank_qpc = 0;
    refresh_qpc = refresh_hz > 0 ? frequency.QuadPart / refresh_hz : 0;
    ReleaseSRWLockExclusive(&lock);
    if (found && thread == NULL) {
        thread = CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);
    }
    return found || port_screen_vblank_last(&at);
}

int port_screen_vblank_last(double *at_s) {
    DWM_TIMING_INFO info;
    LARGE_INTEGER now, frequency;
    double wall = port_wall_seconds();
    int64_t kernel, refresh;

    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    AcquireSRWLockShared(&lock);
    kernel = kernel_vblank_qpc;
    refresh = refresh_qpc;
    ReleaseSRWLockShared(&lock);
    /* (the kernel's, while the thread saw one within the last few refreshes) */
    if (kernel != 0 && refresh > 0 && now.QuadPart - kernel < 4 * refresh) {
        *at_s = wall - (double)(now.QuadPart - kernel) / (double)frequency.QuadPart;
        return 1;
    }
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    if (FAILED(DwmGetCompositionTimingInfo(NULL, &info)) || info.qpcVBlank == 0) {
        return 0;
    }
    *at_s = wall - (double)(int64_t)((uint64_t)now.QuadPart - info.qpcVBlank) / (double)frequency.QuadPart;
    return 1;
}

int port_screen_adaptive_sync_on(void) {
    return 0;
}
#elif defined(KF4_HAVE_LIBDRM)
#define CARDS 16

/* The card's file and the CRTC's index on it (-1: none), under the lock */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int card = -1, pipe_index = -1;
static uint32_t crtc_id; /* the pipe's CRTC's object id */

/* The index of the CRTC on the card showing width x height at refresh_hz, else the one CRTC at that rate
   (-1: none, or more than one at the rate and none of the size) */
static int find_pipe(int fd, int width, int height, int refresh_hz, uint32_t *id) {
    drmModeRes *resources = drmModeGetResources(fd);
    int i, by_size = -1, by_rate = -1, at_rate = 0, found;

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
    found = by_size >= 0 ? by_size : at_rate == 1 ? by_rate : -1;
    *id = found >= 0 ? resources->crtcs[found] : 0;
    drmModeFreeResources(resources);
    return found;
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
        pipe_index = find_pipe(card, width, height, refresh_hz, &crtc_id);
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

int port_screen_adaptive_sync_on(void) {
    drmModeObjectProperties *props;
    uint32_t i;
    int on = 0;

    pthread_mutex_lock(&lock);
    props = pipe_index >= 0 ? drmModeObjectGetProperties(card, crtc_id, DRM_MODE_OBJECT_CRTC) : NULL;
    for (i = 0; props != NULL && i < props->count_props; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(card, props->props[i]);

        if (prop != NULL && strcmp(prop->name, "VRR_ENABLED") == 0) {
            on = props->prop_values[i] != 0;
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    pthread_mutex_unlock(&lock);
    return on;
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

int port_screen_adaptive_sync_on(void) {
    return 0;
}
#endif
