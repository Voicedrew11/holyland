/* The function profiler: its counts (port/platform/sample_table.c), the ELF symbols that name them
   (port/platform/elf_symbols.c), and on Linux a live run that must find a busy thread's function on top
   (port/platform/sampler.c).

     sampler_test */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/elf_symbols.h"
#include "port/sample_table.h"
#include "port/sampler.h"

#if defined(__linux__)
#include <link.h>
#include <pthread.h>
#include <time.h>
#endif

static int failed, checked;

static void check(const char *what, int ok) {
    checked++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

/* ---- The counts ---- */

/* a function is an address's 0x100 bytes */
static uint64_t by_page(void *context, uint64_t address) {
    (void)context;
    return address & ~(uint64_t)0xFF;
}

static uint64_t visited_stacks, visited_samples;

static void count_visit(void *context, int tid, uint64_t count, const uint64_t *frames, int frame_count) {
    (void)context;
    (void)tid;
    (void)frames;
    (void)frame_count;
    visited_stacks++;
    visited_samples += count;
}

static const SampleFunctionTotal *find_total(const SampleFunctionTotal *t, size_t n, uint64_t id) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (t[i].function == id) {
            return &t[i];
        }
    }
    return NULL;
}

static void test_table(void) {
    SampleTable *table = sample_table_create();
    /* thread 1: main 0x1000 calls draw 0x2000 calls leaf 0x3000 (3 samples), main calls draw alone (1);
       a recursion: draw calls draw calls leaf (1). Return addresses are one past the call: 0x1001 is in 0x1000. */
    const uint64_t deep[] = {0x3010, 0x2011, 0x1021};
    const uint64_t shallow[] = {0x2040, 0x1021};
    const uint64_t recursive[] = {0x3010, 0x2031, 0x2011, 0x1021};
    const uint64_t other[] = {0x5000};
    const SampleFunctionTotal *f;
    SampleFunctionTotal *totals;
    size_t n = 0;
    uint64_t many[100];
    int i;

    check("table made", table != NULL);
    if (table == NULL) {
        return;
    }
    for (i = 0; i < 3; i++) {
        sample_table_add(table, 1, deep, 3);
    }
    sample_table_add(table, 1, shallow, 2);
    sample_table_add(table, 1, recursive, 4);
    sample_table_add(table, 2, other, 1);
    check("thread 1's samples", sample_table_thread_samples(table, 1) == 5);
    check("thread 2's samples", sample_table_thread_samples(table, 2) == 1);
    sample_table_each(table, count_visit, NULL);
    check("each distinct stack once", visited_stacks == 4 && visited_samples == 6);

    totals = sample_table_functions(table, 1, by_page, NULL, &n);
    check("thread 1's three functions", totals != NULL && n == 3);
    f = find_total(totals, n, 0x1000);
    check("main: in every stack, never innermost", f && f->inclusive == 5 && f->self == 0);
    f = find_total(totals, n, 0x2000);
    check("draw: inclusive once a stack though it recurs", f && f->inclusive == 5 && f->self == 1);
    f = find_total(totals, n, 0x3000);
    check("leaf: self", f && f->inclusive == 4 && f->self == 4);
    check("by inclusive, most first (ties by id)", totals && totals[0].function == 0x1000 &&
                                                       totals[1].function == 0x2000 && totals[2].function == 0x3000);
    free(totals);
    totals = sample_table_functions(table, 9, by_page, NULL, &n);
    check("a thread without samples", totals == NULL && n == 0);

    /* a stack longer than SAMPLE_FRAMES is cut there; many distinct stacks grow the table */
    for (i = 0; i < 100; i++) {
        many[i] = 0x10010 + (uint64_t)i * 0x100;
    }
    sample_table_add(table, 3, many, 100);
    totals = sample_table_functions(table, 3, by_page, NULL, &n);
    check("cut at SAMPLE_FRAMES", n == SAMPLE_FRAMES);
    free(totals);
    for (i = 0; i < 20000; i++) {
        uint64_t frame[2] = {(uint64_t)i * 0x100, 0x99910};

        sample_table_add(table, 4, frame, 2);
    }
    check("20000 distinct stacks", sample_table_thread_samples(table, 4) == 20000);
    totals = sample_table_functions(table, 4, by_page, NULL, &n);
    check("their caller in all of them", totals && totals[0].function == 0x99900 && totals[0].inclusive == 20000);
    free(totals);
    sample_table_clear(table);
    check("cleared", sample_table_thread_samples(table, 1) == 0);
    sample_table_destroy(table);
}

/* ---- The ELF symbols ---- */

/* the busy thread's work: the function the live run must find (not static, not inlined: in the symbol table) */
__attribute__((noinline)) double sampler_test_busy_spin(atomic_int *stop) {
    double x = 1.0;

    while (!atomic_load_explicit(stop, memory_order_relaxed)) {
        int i;

        for (i = 0; i < 1000; i++) {
            x = x * 1.0000001 + 0.5;
        }
    }
    return x;
}

#if defined(__linux__)
static uint64_t program_bias;

static int first_object(struct dl_phdr_info *info, size_t size, void *context) {
    (void)size;
    (void)context;
    program_bias = (uint64_t)info->dlpi_addr;
    return 1; /* (the program is the first) */
}
#endif

static void test_elf(void) {
    static const uint8_t junk[200] = {0x7F, 'E', 'L', 'F', 2, 1};
    uint8_t *cut;
    FILE *f;
    size_t got;

    check("not an ELF: none", elf_symbols_parse((const uint8_t *)"hello", 5) == NULL);
    check("an ELF header pointing nowhere: none", elf_symbols_parse(junk, sizeof(junk)) == NULL);
    check("a missing file: none", elf_symbols_load("/nonexistent/file") == NULL);
#if defined(__linux__)
    {
        ElfSymbols *own = elf_symbols_load("/proc/self/exe");
        uint64_t start = 0, vaddr;
        const char *name;

        dl_iterate_phdr(first_object, NULL);
        vaddr = (uint64_t)(uintptr_t)&sampler_test_busy_spin - program_bias;
        check("the test's own symbols", own != NULL && elf_symbols_count(own) > 10);
        name = elf_symbols_find(own, vaddr + 5, &start);
        check("an address inside a function names it", name && strcmp(name, "sampler_test_busy_spin") == 0);
        check("and gives its start", start == vaddr);
        check("before the first function: none", elf_symbols_find(own, 0, NULL) == NULL);
        elf_symbols_free(own);
    }
    /* the file cut short at every few KB: never a crash */
    f = fopen("/proc/self/exe", "rb");
    cut = malloc(1 << 22);
    if (f != NULL && cut != NULL) {
        size_t size;
        int crashes_none = 1;

        got = fread(cut, 1, 1 << 22, f);
        for (size = 0; size < got; size += 4093) {
            ElfSymbols *s = elf_symbols_parse(cut, size);

            elf_symbols_free(s);
        }
        check("cut short anywhere: read without a crash", crashes_none);
    }
    if (f != NULL) {
        fclose(f);
    }
    free(cut);
#else
    (void)cut;
    (void)f;
    (void)got;
#endif
}

/* ---- A live run ---- */
#if defined(__linux__)
static atomic_int spin_stop;

/* ThreadSanitizer holds an asynchronous signal back until the thread enters one of its hooks, which a spin never does:
   under it the samples are not checked, only that the sampler races with nothing */
#if defined(__SANITIZE_THREAD__)
#define UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif

static void *spinner(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "spinner");
    sampler_test_busy_spin(&spin_stop);
    return NULL;
}

static void test_live(void) {
    SamplerReport *report = calloc(1, sizeof(*report));
    struct timespec wait = {0, 400000000};
    const SamplerThread *spin = NULL;
    pthread_t thread;
    int i, top_self = 0;
    FILE *out;

    check("available", sampler_available());
    check("started", sampler_start(500));
    check("running", sampler_running());
    pthread_create(&thread, NULL, spinner, NULL);
    nanosleep(&wait, NULL);
    atomic_store(&spin_stop, 1);
    pthread_join(thread, NULL);
    sampler_stop();
    check("stopped", !sampler_running());
    check("a report", report && sampler_report(report));
    if (report == NULL) {
        return;
    }
    for (i = 0; i < report->thread_count; i++) {
        if (strcmp(report->threads[i].name, "spinner") == 0) {
            spin = &report->threads[i];
        }
    }
    check("the busy thread in it, by name", spin != NULL);
#ifdef UNDER_TSAN
    printf("     (ThreadSanitizer: the samples not checked)\n");
    spin = NULL;
#endif
    if (spin != NULL) {
        printf("     spinner: %.0f ms CPU, %llu samples\n", spin->cpu_ms, (unsigned long long)spin->samples);
        /* (loose: a loaded machine, or a sanitizer build, gives the spinner and the sampler less of a core) */
        check("its CPU time", spin->cpu_ms > 50);
        check("sampled (of 400 ms at 0.5 ms)", spin->samples > 50);
        for (i = 0; i < spin->function_count; i++) {
            const SamplerFunction *fn = &spin->functions[i];

            printf("     %5.1f%% %5.1f%% %s\n", 100.0 * (double)fn->inclusive / (double)spin->samples,
                   100.0 * (double)fn->self / (double)spin->samples, fn->name);
            if (strcmp(fn->name, "sampler_test_busy_spin") == 0 && fn->self * 10 >= spin->samples * 9) {
                top_self = 1;
            }
        }
        check("its function on top: 90% self or more", top_self);
        check("the thread's start below it (the walk went up)", spin->function_count >= 2);
    }
    out = tmpfile();
    if (out != NULL) {
        char line[512];
        int stacks = 0, threads = 0, modules = 0;

        check("written", sampler_write(out));
        rewind(out);
        while (fgets(line, sizeof(line), out)) {
            stacks += line[0] == 'K';
            threads += line[0] == 'C';
            modules += line[0] == 'M';
        }
        fclose(out);
        check("the file has stacks, threads and modules", stacks > 0 && threads > 0 && modules > 0);
    }
    check("starts again", sampler_start(0));
    sampler_stop();
    free(report);
}
#endif

int main(void) {
    test_table();
    test_elf();
#if defined(__linux__)
    test_live();
#else
    check("not available here", !sampler_available());
#endif
    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
