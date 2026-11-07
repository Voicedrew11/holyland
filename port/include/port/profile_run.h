/* A profile run (docs/port/PROFILING.md): the function profiler (port/include/port/sampler.h) and the GPU's
   time by what it does (port_profile_gpu_* in port/include/port/profile.h) started and stopped together, from
   Shift+P's Start/Stop or --profile-at, and written to one file for tools/sampler/stacks.py.

   port/platform/profile_run.c. Start, stop and save from any thread; port_profile_run_vblank from the game's. */
#ifndef PORT_PROFILE_RUN_H
#define PORT_PROFILE_RUN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The sampler (on systems that have it) and the GPU's table, both from zero. 1 if the sampler runs (0 where
   there is none, or it failed: the GPU's table is recorded all the same). */
int port_profile_run_start(void);
void port_profile_run_stop(void);
int port_profile_run_recording(void);
/* The run's samples and GPU rows into path ("G ns intervals level name" lines and "P pictures" after the
   sampler's). 1 if written. */
int port_profile_run_save(const char *path);
/* A file in the data folder named by the time, for the panel's Save: its path into path (size bytes) */
void port_profile_run_default_path(char *path, int size);

/* --profile-at VBLANK:SECONDS:FILE: kept, 0 if it does not parse */
int port_profile_run_at(const char *spec);
/* Each vblank (the game's thread): starts the --profile-at run at its vblank, stops and saves it SECONDS of the
   game's vblanks later */
void port_profile_run_vblank(uint64_t vblank);

#ifdef __cplusplus
}
#endif

#endif
