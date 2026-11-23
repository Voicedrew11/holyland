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

#ifdef __cplusplus
}
#endif

#endif /* PORT_HOST_LOAD_H */
