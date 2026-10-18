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

void port_sleep(double seconds) {
    static int period_set;
    DWORD ms;

    if (!period_set) {
        /* Sleep's granularity is otherwise 15.6 ms */
        timeBeginPeriod(1);
        period_set = 1;
    }
    if (seconds <= 0) {
        return;
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
