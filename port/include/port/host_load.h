/* The PC under the game, for the profiler (Shift+P): the CPU's speed now and the game's memory, so that a player's
   video shows a CPU slowing down (heat, power saving) or memory growing (2026-11-23: a player's stutter that grew
   worse over time, every thread several times slower). port/platform/host_load.c. */
#ifndef PORT_HOST_LOAD_H
#define PORT_HOST_LOAD_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double cpu_mhz;       /* the cores' speed now, their average (Windows: Task Manager's, the base speed times its
                             "% Processor Performance") */
    double cpu_most_mhz;  /* its most: the base speed on Windows, the highest boost on Linux */
    double cpu_limit_mhz; /* (Windows) the speed the system holds the CPU to now: under cpu_most_mhz when heat or power
                             holds it back */
    double memory_mb;     /* the game's own memory (Windows: committed, Linux: resident) */
} PortHostLoad;

/* Measured at most every half second (the last measure in between); 0 for what is unknown. From one thread only (the
   overlay's: the game's thread). */
PortHostLoad port_host_load(void);

/* What one read of the clock costs (port_ticks, Windows's QueryPerformanceCounter). The renderer and the pacing read it
   about 200 000 times a second: at the usual 20 to 40 ns that is nothing, but a Windows forced onto the HPET timer
   (bcdedit's useplatformclock, a "tweak" some guides give) takes 1 to 2 us a read, and every thread slows
   (2026-11-24: a player's Ryzen 5 3600 at 4.1 GHz ran every thread about 3.4 times slower than here). */
typedef struct {
    double read_ns;     /* one read, the least of a few tries */
    double counter_mhz; /* the counter's rate: 10 is Windows's TSC, 14.318 the HPET, 3.580 the ACPI timer */
} PortClockCost;

/* A clock this slow a read is named in the log and the profiler */
#define PORT_CLOCK_SLOW_NS 250.0

/* Measured at the first call (a millisecond or so); the main thread's first call comes before the others start */
PortClockCost port_clock_cost(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_HOST_LOAD_H */
