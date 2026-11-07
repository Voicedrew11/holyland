/* The function profiler's counts (port/include/port/sampler.h): each distinct call stack of each thread with the
   number of samples that caught it, and from them a thread's functions by self and inclusive samples.

   port/platform/sample_table.c. Plain data: one thread at a time (the sampler keeps it behind its lock). */
#ifndef PORT_SAMPLE_TABLE_H
#define PORT_SAMPLE_TABLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAMPLE_FRAMES 64 /* a stack's frames at most */

typedef struct SampleTable SampleTable;

/* NULL when out of memory */
SampleTable *sample_table_create(void);
void sample_table_destroy(SampleTable *table);
void sample_table_clear(SampleTable *table);

/* One sample of thread tid: its stack, innermost first (the interrupted address, then return addresses); a count
   over SAMPLE_FRAMES is cut there. 0 when out of memory (the sample is not counted). */
int sample_table_add(SampleTable *table, int tid, const uint64_t *frames, int count);
/* The samples of thread tid */
uint64_t sample_table_thread_samples(const SampleTable *table, int tid);

/* Each distinct stack */
typedef void (*SampleStackVisit)(void *context, int tid, uint64_t count, const uint64_t *frames, int frame_count);
void sample_table_each(const SampleTable *table, SampleStackVisit visit, void *context);

/* A function's samples */
typedef struct {
    uint64_t function; /* function_of's id */
    uint64_t self;     /* samples it was the innermost frame of */
    uint64_t inclusive; /* samples it was anywhere in the stack of, once a stack however often it recurs */
} SampleFunctionTotal;

/* Which function an address is in: an id the same for every address in the function. A return address is given
   less one (the call's own address). */
typedef uint64_t (*SampleFunctionOf)(void *context, uint64_t address);

/* Thread tid's functions, by inclusive samples, most first (ties by id): an array the caller frees with free(),
   its length in *count. NULL with *count 0 when the thread has no samples or memory ran out. */
SampleFunctionTotal *sample_table_functions(const SampleTable *table, int tid, SampleFunctionOf function_of,
                                            void *context, size_t *count);

#ifdef __cplusplus
}
#endif

#endif
