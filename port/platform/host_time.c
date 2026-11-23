/* The host's clock and sleep (port/include/port/platform.h). */
#ifdef _WIN32
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#endif

#include "port/platform.h"

#ifdef _WIN32
double port_wall_seconds(void) {
    static LARGE_INTEGER frequency, start;
    LARGE_INTEGER now;

    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

uint64_t port_ticks(void) {
    LARGE_INTEGER now;

    QueryPerformanceCounter(&now);
    return (uint64_t)now.QuadPart;
}

double port_ticks_per_second(void) {
    LARGE_INTEGER frequency;

    QueryPerformanceFrequency(&frequency);
    return (double)frequency.QuadPart;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void port_sleep(double seconds) {
    static int period_set;
    /* a high-resolution waitable timer (Windows 10 1803 on), a thread's own: it wakes within a tenth of
       a millisecond or so, where Sleep's whole milliseconds woke the vblank pacing 0 to 2.5 ms late */
    static __thread HANDLE timer;
    static __thread int timer_tried;
    DWORD ms;

    if (!period_set) {
        /* Sleep's granularity is otherwise 15.6 ms */
        timeBeginPeriod(1);
        period_set = 1;
    }
    if (seconds <= 0) {
        return;
    }
    if (!timer_tried) {
        timer_tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (timer != NULL) {
        LARGE_INTEGER due;

        due.QuadPart = -(LONGLONG)(seconds * 1e7); /* relative, in 100 ns */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
            return;
        }
    }
    ms = (DWORD)(seconds * 1000.0);
    Sleep(ms);
}
#else
double port_wall_seconds(void) {
    static struct timespec start;
    struct timespec now;

    if (start.tv_sec == 0 && start.tv_nsec == 0) {
        clock_gettime(CLOCK_MONOTONIC, &start);
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) * 1e-9;
}

uint64_t port_ticks(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

double port_ticks_per_second(void) {
    return 1e9;
}

void port_sleep(double seconds) {
    struct timespec delay;

    if (seconds <= 0) {
        return;
    }
    delay.tv_sec = (time_t)seconds;
    delay.tv_nsec = (long)((seconds - (double)delay.tv_sec) * 1e9);
    nanosleep(&delay, NULL);
}
#endif
