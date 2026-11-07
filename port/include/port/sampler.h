/* The function profiler (Shift+P's "Functions", docs/port/PROFILING.md): which functions the program's
   threads spend their CPU time in.

   A thread of its own ("kf4-sampler") wakes every interval and, for each of the process's threads that is
   running at that moment, has the thread interrupt itself (SIGPROF) and walk its own call stack by frame
   pointers into a buffer it hands back. The stacks are counted, each distinct stack once with its count;
   functions are named from the modules' ELF symbol tables (and Fedora's MiniDebugInfo for the system's
   libraries). A sample is a moment a thread was running, so a function's share of a thread's samples is its
   share of that thread's CPU time.

   Frames come out right where the code keeps frame pointers: ours with -DKF4_FRAME_POINTERS=ON (the default
   on Linux), and Fedora's own libraries (the Vulkan driver, SDL, libc). Elsewhere a stack may stop early.

   Linux only. Elsewhere sampler_available() is 0 and the rest do nothing (Windows has kf4-sampler,
   tools/sampler). Any thread may call any of these. */
#ifndef PORT_SAMPLER_H
#define PORT_SAMPLER_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAMPLER_TOP 25            /* a thread's functions kept in a report: the top by self, and by inclusive */
#define SAMPLER_FUNCTIONS (2 * SAMPLER_TOP)
#define SAMPLER_THREADS 64        /* threads in a report, the busiest */
#define SAMPLER_FUNCTION_NAME 512 /* a function's name as the symbol table has it (C++'s mangled: long) */
#define SAMPLER_THREAD_NAME 16    /* as the kernel keeps it (comm) */

typedef struct {
    char name[SAMPLER_FUNCTION_NAME]; /* the symbol, or "[module]" when the module's symbols do not cover it */
    uint64_t self;                    /* the samples it was the innermost frame of */
    uint64_t inclusive;               /* the samples it was anywhere in the stack of (a caller's cover its callees') */
} SamplerFunction;

typedef struct {
    int tid;
    char name[SAMPLER_THREAD_NAME];
    double cpu_ms;      /* its CPU time since the start */
    uint64_t samples;   /* the moments it was caught running */
    int signals_blocked; /* it never answered (it blocks SIGPROF): no samples, its CPU time only */
    int function_count;
    SamplerFunction functions[SAMPLER_FUNCTIONS]; /* the top by self and the top by inclusive, merged, by inclusive */
} SamplerThread;

typedef struct {
    double seconds;         /* since the start (to the stop, once stopped) */
    unsigned interval_us;
    uint64_t samples;       /* all threads' */
    uint64_t missed;        /* requests a thread did not answer in time */
    int thread_count;
    SamplerThread threads[SAMPLER_THREADS]; /* by CPU time, most first */
} SamplerReport;

/* 1 if this build can sample (Linux) */
int sampler_available(void);
/* Starts sampling every interval_us microseconds (0: 1000), the counts so far let go. 1 if it runs (also if it
   already did), 0 if it cannot (said on stderr). */
int sampler_start(unsigned interval_us);
/* Stops sampling; the counts are kept for sampler_report and sampler_write until the next start */
void sampler_stop(void);
int sampler_running(void);
/* The latest report: made by the sampler's thread twice a second while it runs, and once at the stop. 0 if
   there is none yet. report is large (a few hundred KB): not for the stack. */
int sampler_report(SamplerReport *report);
/* Every stack counted, for tools/sampler/stacks.py: lines "M name path" (each module), "C tid name" and
   "T tid cpu_ms" (each thread), "K tid count frame..." (each distinct stack, innermost first: the program's own
   frames as addresses at their ELF address, "0x...", others as "module+0x..."), "N module+0x... name" (the
   names known here of other modules' frames). 1 if it was all written. */
int sampler_write(FILE *out);

#ifdef __cplusplus
}
#endif

#endif
