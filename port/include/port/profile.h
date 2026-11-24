/* The profiler's counters (Shift+P, port/ui/profiler_overlay.hpp; docs/port/PROFILING.md): its clock, the pictures
   drawn, the pipelines a draw waited for, and the GPU's time by kind of work and shader state over a profile run.
   Where each picture's time went is the frame profiler's (port/include/port/zones.h).

   port/platform/profile.c, a library of its own (kf4_profile) that the renderer links as well as the
   port. Any thread may call any of these. */
#ifndef PORT_PROFILE_H
#define PORT_PROFILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A monotonic clock in nanoseconds (the frame profiler's too: port/include/port/zones.h) */
uint64_t port_profile_now_ns(void);
/* A picture drawn by the renderer (vblanks' own and pictures between): what the GPU table's time is divided by */
void port_profile_picture(void);

/* The pipelines made while a draw waited (a compile in play), since the start: counted whether the panel is
   open or not, so that one line says how many there were and how long they took */
typedef struct {
    uint64_t count;
    uint64_t ns;         /* all of them */
    uint64_t longest_ns; /* the longest one */
} ProfileCompiles;
/* A pipeline made while a draw waited, ns long: counted */
void port_profile_compile(uint64_t ns);
ProfileCompiles port_profile_compiles(void);

/* The GPU's time by what it was doing, while a profile run records (port/include/port/profile_run.h): rows by
   kind of work (the renderers' timestamp kinds) and, with detail on, by shader state (the PC renderer's: the
   GPU's "functions"). The renderers add an interval to a row as its timestamps come in. */
#define PROFILE_GPU_ROWS 512
#define PROFILE_GPU_NAME 112
typedef enum {
    PROFILE_GPU_KIND,   /* a kind of work: these rows add up to the GPU's time */
    PROFILE_GPU_SHADER, /* a shader state, within its kind: these too, of the renderers that time them */
    PROFILE_GPU_LEVELS
} ProfileGpuLevel;
typedef struct {
    char name[PROFILE_GPU_NAME];
    ProfileGpuLevel level;
    uint64_t ns;
    uint64_t intervals;
} ProfileGpuRow;
/* Recording: the renderers time their GPU work and add it here */
int port_profile_gpu_recording(void);
void port_profile_gpu_set_recording(int on);
/* Detail: 1, the PC renderer also times each change of shader state (a timestamp at each: a little GPU time); 2,
   each draw (a timestamp at every batch: enough to slow the GPU, for the frame profiler's timeline) */
int port_profile_gpu_detail(void);
void port_profile_gpu_set_detail(int on);
/* Every row let go, the pictures counted from now */
void port_profile_gpu_reset(void);
/* ns of the GPU's time in `intervals` timed intervals to the row of key (the caller's, one for each name: the first
   add names the row). A table full keeps the time in a row "(others)". */
void port_profile_gpu_add(uint64_t key, const char *name, ProfileGpuLevel level, uint64_t ns, uint64_t intervals);
/* The rows, most time first, into rows (at most most); their count */
int port_profile_gpu_rows(ProfileGpuRow *rows, int most);
/* The pictures drawn since the reset (port_profile_picture's) */
uint64_t port_profile_gpu_pictures(void);

#ifdef __cplusplus
}
#endif

#endif
