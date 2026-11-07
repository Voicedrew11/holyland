/* The function profiler (port/include/port/sampler.h).

   The sampler's thread ("kf4-sampler") keeps the process's threads (/proc/self/task, looked at again every
   100 ms, each thread's stat file kept open) and, every interval, reads each one's state: one that is running
   ('R') is sent SIGPROF with tgkill. Only threads caught running are interrupted, so a thread waiting in a
   system call is never woken early by the profiler.

   The signal handler runs on the interrupted thread: it walks that thread's frame-pointer chain from the
   interrupted registers, within the readable mapping that holds its stack pointer (the sampler's copy of
   /proc/self/maps), into its own stack, then hands the frames over through the one request slot: a compare and
   swap from REQUESTED to WRITING claims it, DONE gives it back. A request not answered in time is marked
   ABANDONED by the same compare and swap, so a late handler can never write into the next request; the slot
   also names the thread asked, so a late handler from another thread is turned away. The handler takes no lock,
   allocates nothing and keeps errno.

   The maps are read again each second into one of three buffers in turn, so that a handler that is late reads
   a buffer that is not being written (it would have to be two seconds late).

   Everything else (counting, naming, reports) is on the sampler's thread, under the lock that sampler_report
   and sampler_write take too. */
#if defined(__linux__) && defined(__x86_64__)
#define _GNU_SOURCE
#define SAMPLER_HERE 1
#endif

#include "port/sampler.h"

#if SAMPLER_HERE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "port/elf_symbols.h"
#include "port/sample_table.h"

#define MAX_TASKS 512
#define MAX_RANGES 8192
#define MAX_MODULES 512
#define ANSWER_WAIT_NS 2000000 /* a request unanswered this long is abandoned */
#define TASKS_EVERY_NS 100000000
#define MAPS_EVERY_NS 1000000000
#define REPORT_EVERY_NS 500000000
#define SILENT_LIMIT 4 /* unanswered requests in a row: the thread blocks SIGPROF, asked no more */

/* ---- What the handler reads and writes ---- */
typedef struct {
    uint64_t lo, hi;
} Range;

enum { SLOT_IDLE, SLOT_REQUESTED, SLOT_WRITING, SLOT_DONE, SLOT_ABANDONED };

static struct {
    _Atomic int state;
    _Atomic int tid; /* the thread asked */
    int frame_count;
    uint64_t frames[SAMPLE_FRAMES];
} slot;

static Range range_buffers[3][MAX_RANGES];
static _Atomic(const Range *) ranges;
static _Atomic int range_count;

/* ---- The sampler's own ---- */
typedef struct {
    int tid;
    int stat_fd;
    int gone;
    int silent;            /* unanswered requests in a row */
    int signals_blocked;
    uint64_t cpu_start_ns; /* its CPU clock at the start (or when first seen) */
    uint64_t cpu_ns;       /* since then, at the last look */
    char name[SAMPLER_THREAD_NAME];
} Task;

typedef struct {
    char path[256];
    char name[64];
    uint64_t bias;
    uint64_t lo, hi; /* its code, at run time */
    ElfSymbols *symbols;
    int symbols_tried;
} Module;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; /* the rest below */
static pthread_t thread;
static int thread_up;
static atomic_int stop_asked;
static atomic_int running;
static unsigned interval_us = 1000;
static SampleTable *table;
static Task tasks[MAX_TASKS];
static int task_count;
static Module modules[MAX_MODULES];
static int module_count;
static uint64_t started_ns, stopped_ns;
static uint64_t sample_count, missed_count;
static SamplerReport *report;
static int have_report;
static int handler_installed;

static uint64_t now_ns(void) {
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        return 0;
    }
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* thread tid's CPU time (the kernel's per-thread CPU clock, MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)) */
static uint64_t thread_cpu_ns(int tid) {
    clockid_t clock = (clockid_t)((~(unsigned)tid << 3) | 6u);
    struct timespec t;

    if (clock_gettime(clock, &t) != 0) {
        return 0;
    }
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

/* ---- The handler (on the interrupted thread) ---- */
__attribute__((no_sanitize("address", "thread", "undefined"))) static int walk(const ucontext_t *uc,
                                                                                 uint64_t *frames) {
    uint64_t rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    uint64_t sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
    uint64_t fp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
    const Range *r = atomic_load_explicit(&ranges, memory_order_acquire);
    int count = atomic_load_explicit(&range_count, memory_order_acquire);
    int lo = 0, hi = count, n = 0;
    uint64_t stack_hi;

    frames[n++] = rip;
    if (r == NULL) {
        return n;
    }
    /* the range holding sp */
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;

        if (r[mid].hi <= sp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == count || r[lo].lo > sp) {
        return n;
    }
    stack_hi = r[lo].hi;
    while (n < SAMPLE_FRAMES && fp >= sp && (fp & 7) == 0 && fp < stack_hi && stack_hi - fp >= 16) {
        const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;
        uint64_t next = frame[0], ret = frame[1];

        if (ret == 0) {
            break;
        }
        frames[n++] = ret;
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    return n;
}

static void on_sigprof(int sig, siginfo_t *info, void *context) {
    int saved_errno = errno;
    uint64_t frames[SAMPLE_FRAMES];
    int expected = SLOT_REQUESTED, n;

    (void)sig;
    if (info->si_code == SI_TKILL && info->si_pid == getpid() &&
        atomic_load_explicit(&slot.tid, memory_order_acquire) == (int)syscall(SYS_gettid) &&
        atomic_load_explicit(&slot.state, memory_order_acquire) == SLOT_REQUESTED) {
        n = walk((const ucontext_t *)context, frames);
        if (atomic_compare_exchange_strong(&slot.state, &expected, SLOT_WRITING)) {
            memcpy(slot.frames, frames, (size_t)n * sizeof(*frames));
            slot.frame_count = n;
            atomic_store_explicit(&slot.state, SLOT_DONE, memory_order_release);
        }
    }
    errno = saved_errno;
}

/* ---- The maps ---- */
static int by_lo(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* One line of /proc/self/maps: a readable and writable mapping's bounds (a stack is one); 0 for others */
static int parse_maps_line(const char *line, Range *out) {
    unsigned long long lo, hi;
    char perms[5];

    if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) != 3 || perms[0] != 'r' || perms[1] != 'w' || hi <= lo) {
        return 0;
    }
    out->lo = lo;
    out->hi = hi;
    return 1;
}

static void read_maps(void) {
    static int next_buffer;
    Range *buffer = range_buffers[next_buffer];
    FILE *f = fopen("/proc/self/maps", "re");
    char *line = NULL;
    size_t room = 0;
    int n = 0;

    if (f == NULL) {
        return;
    }
    while (n < MAX_RANGES && getline(&line, &room, f) > 0) {
        if (parse_maps_line(line, &buffer[n])) {
            n++;
        }
    }
    free(line);
    fclose(f);
    qsort(buffer, (size_t)n, sizeof(*buffer), by_lo);
    /* (count first to the smaller of the two, so a handler between the stores reads within both) */
    if (n < atomic_load(&range_count)) {
        atomic_store_explicit(&range_count, n, memory_order_release);
        atomic_store_explicit(&ranges, buffer, memory_order_release);
    } else {
        atomic_store_explicit(&ranges, buffer, memory_order_release);
        atomic_store_explicit(&range_count, n, memory_order_release);
    }
    next_buffer = (next_buffer + 1) % 3;
}

/* ---- The threads ---- */
static Task *task_of(int tid) {
    int i;

    for (i = 0; i < task_count; i++) {
        if (tasks[i].tid == tid) {
            return &tasks[i];
        }
    }
    return NULL;
}

static void read_name(int tid, char *name) {
    char path[64];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    name[0] = '\0';
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    n = read(fd, name, SAMPLER_THREAD_NAME - 1);
    close(fd);
    if (n <= 0) {
        name[0] = '\0';
        return;
    }
    name[n] = '\0';
    if (name[n - 1] == '\n') {
        name[n - 1] = '\0';
    }
}

/* the threads seen again: new ones added, gone ones marked, CPU times and names brought up to date (lock held); the
   number of new ones */
static int look_at_tasks(int self_tid) {
    DIR *dir = opendir("/proc/self/task");
    struct dirent *e;
    int i, added = 0;

    if (dir == NULL) {
        return 0;
    }
    for (i = 0; i < task_count; i++) {
        tasks[i].gone = 1;
    }
    while ((e = readdir(dir)) != NULL) {
        int tid = atoi(e->d_name);
        Task *t;

        if (tid <= 0 || tid == self_tid) {
            continue;
        }
        t = task_of(tid);
        if (t == NULL) {
            char path[64];

            if (task_count == MAX_TASKS) {
                continue;
            }
            t = &tasks[task_count++];
            memset(t, 0, sizeof(*t));
            t->tid = tid;
            snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
            t->stat_fd = open(path, O_RDONLY | O_CLOEXEC);
            t->cpu_start_ns = thread_cpu_ns(tid);
            added++;
        }
        if (t->stat_fd < 0) {
            continue;
        }
        t->gone = 0;
        t->cpu_ns = thread_cpu_ns(tid) - t->cpu_start_ns;
        read_name(tid, t->name);
    }
    closedir(dir);
    for (i = 0; i < task_count; i++) {
        if (tasks[i].gone && tasks[i].stat_fd >= 0) {
            close(tasks[i].stat_fd);
            tasks[i].stat_fd = -1;
        }
    }
    return added;
}

/* the thread is running now ('R' in its stat, after the name's closing parenthesis) */
static int task_running(const Task *t) {
    char buffer[512];
    ssize_t n = pread(t->stat_fd, buffer, sizeof(buffer) - 1, 0);
    char *close_paren;

    if (n <= 0) {
        return 0;
    }
    buffer[n] = '\0';
    close_paren = strrchr(buffer, ')');
    return close_paren != NULL && close_paren[1] == ' ' && close_paren[2] == 'R';
}

/* ---- Sampling ---- */
static void sleep_ns(uint64_t ns) {
    struct timespec t = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};

    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/* one sample of t: asked, waited for, counted (lock held) */
static void sample_task(Task *t, pid_t pid) {
    uint64_t asked;
    int state;

    atomic_store_explicit(&slot.tid, t->tid, memory_order_relaxed);
    atomic_store_explicit(&slot.state, SLOT_REQUESTED, memory_order_release);
    if (syscall(SYS_tgkill, pid, t->tid, SIGPROF) != 0) {
        atomic_store(&slot.state, SLOT_IDLE);
        return;
    }
    asked = now_ns();
    for (;;) {
        int expected = SLOT_REQUESTED;

        state = atomic_load_explicit(&slot.state, memory_order_acquire);
        if (state == SLOT_DONE) {
            break;
        }
        if (state == SLOT_REQUESTED && now_ns() - asked > ANSWER_WAIT_NS &&
            atomic_compare_exchange_strong(&slot.state, &expected, SLOT_ABANDONED)) {
            missed_count++;
            if (++t->silent >= SILENT_LIMIT) {
                t->signals_blocked = 1;
            }
            atomic_store(&slot.state, SLOT_IDLE);
            return;
        }
        sched_yield();
    }
    t->silent = 0;
    if (sample_table_add(table, t->tid, slot.frames, slot.frame_count)) {
        sample_count++;
    }
    atomic_store(&slot.state, SLOT_IDLE);
}

static void make_report(void);

static void *sampler_main(void *unused) {
    pid_t pid = getpid();
    int self_tid = (int)syscall(SYS_gettid);
    uint64_t next = now_ns(), tasks_at = 0, maps_at = 0, report_at = next + REPORT_EVERY_NS;

    (void)unused;
    pthread_setname_np(pthread_self(), "kf4-sampler");
    while (!atomic_load(&stop_asked)) {
        uint64_t now = now_ns();
        int i;

        pthread_mutex_lock(&lock);
        if (now >= tasks_at) {
            /* (a new thread's stack is a new mapping: the maps read again at once, so its walks are bounded by it) */
            if (look_at_tasks(self_tid) > 0) {
                maps_at = now;
            }
            tasks_at = now + TASKS_EVERY_NS;
        }
        if (now >= maps_at) {
            read_maps();
            maps_at = now + MAPS_EVERY_NS;
        }
        for (i = 0; i < task_count; i++) {
            Task *t = &tasks[i];

            if (!t->gone && !t->signals_blocked && task_running(t)) {
                sample_task(t, pid);
            }
        }
        if (now >= report_at) {
            make_report();
            report_at = now + REPORT_EVERY_NS;
        }
        pthread_mutex_unlock(&lock);
        next += (uint64_t)interval_us * 1000u;
        now = now_ns();
        if (next > now) {
            sleep_ns(next - now);
        } else {
            next = now; /* (behind: the missed ticks let go) */
        }
    }
    return NULL;
}

/* ---- Naming ---- */
static int add_module(struct dl_phdr_info *info, size_t size, void *unused) {
    Module *m;
    const char *path = info->dlpi_name;
    char exe[256];
    int i;

    (void)size;
    (void)unused;
    if (path == NULL || path[0] == '\0') {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

        if (n <= 0 || module_count > 0) { /* (the program is the first object; others without a name: the vdso) */
            return 0;
        }
        exe[n] = '\0';
        path = exe;
    }
    for (i = 0; i < module_count; i++) {
        if (modules[i].bias == (uint64_t)info->dlpi_addr && strcmp(modules[i].path, path) == 0) {
            return 0; /* (known) */
        }
    }
    if (module_count == MAX_MODULES) {
        return 0;
    }
    m = &modules[module_count];
    memset(m, 0, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", path);
    snprintf(m->name, sizeof(m->name), "%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    m->bias = (uint64_t)info->dlpi_addr;
    m->lo = UINT64_MAX;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];

        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            uint64_t lo = m->bias + ph->p_vaddr, hi = lo + ph->p_memsz;

            m->lo = lo < m->lo ? lo : m->lo;
            m->hi = hi > m->hi ? hi : m->hi;
        }
    }
    if (m->lo < m->hi) {
        module_count++;
    }
    return 0;
}

static Module *module_of(uint64_t address) {
    int i;

    for (i = 0; i < module_count; i++) {
        if (address >= modules[i].lo && address < modules[i].hi) {
            return &modules[i];
        }
    }
    return NULL;
}

static const ElfSymbols *symbols_of(Module *m) {
    if (!m->symbols_tried) {
        m->symbols_tried = 1;
        m->symbols = elf_symbols_load(m->path);
    }
    return m->symbols;
}

/* a function's id: its start's address; a module's code its symbols do not name: the module's index + 1; an
   address in no module: the address */
static uint64_t look_up_function(uint64_t address) {
    Module *m = module_of(address);
    uint64_t start;

    if (m == NULL) {
        return address;
    }
    if (elf_symbols_find(symbols_of(m), address - m->bias, &start) != NULL) {
        return m->bias + start;
    }
    return (uint64_t)(m - modules) + 1;
}

/* the ids found, by address: each report names the same frames again (cleared at each start) */
#define FUNCTION_CACHE (1u << 16)
static uint64_t cache_address[FUNCTION_CACHE], cache_id[FUNCTION_CACHE];

static uint64_t function_of(void *unused, uint64_t address) {
    unsigned at = (unsigned)((address * 0x9E3779B97F4A7C15ull) >> 48) & (FUNCTION_CACHE - 1);

    (void)unused;
    if (cache_address[at] != address || address == 0) {
        cache_address[at] = address;
        cache_id[at] = look_up_function(address);
    }
    return cache_id[at];
}

static void function_name(uint64_t id, char *name, size_t size) {
    Module *m;
    const char *symbol;

    if (id > 0 && id <= (uint64_t)module_count) {
        snprintf(name, size, "[%s]", modules[id - 1].name);
        return;
    }
    m = module_of(id);
    symbol = m ? elf_symbols_find(symbols_of(m), id - m->bias, NULL) : NULL;
    if (symbol != NULL) {
        snprintf(name, size, "%s", symbol);
    } else {
        snprintf(name, size, "0x%llx", (unsigned long long)id);
    }
}

/* ---- Reports ---- */
static int by_cpu(const void *a, const void *b) {
    const SamplerThread *x = a, *y = b;

    if (x->cpu_ms != y->cpu_ms) {
        return x->cpu_ms > y->cpu_ms ? -1 : 1;
    }
    return x->tid - y->tid;
}

static int by_self(const void *a, const void *b) {
    const SampleFunctionTotal *x = a, *y = b;

    if (x->self != y->self) {
        return x->self > y->self ? -1 : 1;
    }
    return x->function < y->function ? -1 : x->function > y->function;
}

static int by_inclusive_then_self(const void *a, const void *b) {
    const SamplerFunction *x = a, *y = b;

    if (x->inclusive != y->inclusive) {
        return x->inclusive > y->inclusive ? -1 : 1;
    }
    if (x->self != y->self) {
        return x->self > y->self ? -1 : 1;
    }
    return strcmp(x->name, y->name);
}

/* a thread's top functions by inclusive and by self, merged, into out (lock held) */
static void thread_functions(int tid, SamplerThread *out) {
    size_t count = 0, i, j;
    SampleFunctionTotal *totals = sample_table_functions(table, tid, function_of, NULL, &count);
    uint64_t chosen[SAMPLER_FUNCTIONS];
    int n = 0;

    out->function_count = 0;
    if (totals == NULL) {
        return;
    }
    for (i = 0; i < count && i < SAMPLER_TOP; i++) { /* (by inclusive, as they come) */
        chosen[n++] = totals[i].function;
    }
    qsort(totals, count, sizeof(*totals), by_self);
    for (i = 0; i < count && i < SAMPLER_TOP && totals[i].self > 0; i++) {
        for (j = 0; j < (size_t)n && chosen[j] != totals[i].function; j++) {
        }
        if (j == (size_t)n) {
            chosen[n++] = totals[i].function;
        }
    }
    for (i = 0; i < count; i++) {
        for (j = 0; j < (size_t)n; j++) {
            if (chosen[j] == totals[i].function) {
                SamplerFunction *f = &out->functions[out->function_count++];

                function_name(totals[i].function, f->name, sizeof(f->name));
                f->self = totals[i].self;
                f->inclusive = totals[i].inclusive;
                break;
            }
        }
    }
    free(totals);
    qsort(out->functions, (size_t)out->function_count, sizeof(*out->functions), by_inclusive_then_self);
}

/* the report from the counts so far (lock held) */
static void make_report(void) {
    uint64_t end = stopped_ns ? stopped_ns : now_ns();
    int i, n = 0;

    if (report == NULL) {
        report = calloc(1, sizeof(*report));
        if (report == NULL) {
            return;
        }
    }
    dl_iterate_phdr(add_module, NULL);
    report->seconds = (double)(end - started_ns) / 1e9;
    report->interval_us = interval_us;
    report->samples = sample_count;
    report->missed = missed_count;
    for (i = 0; i < task_count; i++) {
        SamplerThread *t = &report->threads[n < SAMPLER_THREADS ? n : SAMPLER_THREADS - 1];

        if (tasks[i].cpu_ns == 0 && sample_table_thread_samples(table, tasks[i].tid) == 0) {
            continue;
        }
        if (n == SAMPLER_THREADS) {
            /* (full: the least busy kept so far gives way) */
            qsort(report->threads, SAMPLER_THREADS, sizeof(*t), by_cpu);
            if ((double)tasks[i].cpu_ns / 1e6 <= t->cpu_ms) {
                continue;
            }
        } else {
            n++;
        }
        t->tid = tasks[i].tid;
        memcpy(t->name, tasks[i].name, sizeof(t->name));
        t->cpu_ms = (double)tasks[i].cpu_ns / 1e6;
        t->samples = sample_table_thread_samples(table, tasks[i].tid);
        t->signals_blocked = tasks[i].signals_blocked;
        t->function_count = 0;
    }
    qsort(report->threads, (size_t)n, sizeof(SamplerThread), by_cpu);
    for (i = 0; i < n; i++) {
        if (report->threads[i].samples > 0) {
            thread_functions(report->threads[i].tid, &report->threads[i]);
        }
    }
    report->thread_count = n;
    have_report = 1;
}

/* ---- The interface ---- */
int sampler_available(void) {
    return 1;
}

int sampler_start(unsigned interval) {
    struct sigaction sa;
    int i;

    pthread_mutex_lock(&lock);
    if (thread_up) {
        pthread_mutex_unlock(&lock);
        return 1;
    }
    if (!handler_installed) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_sigprof;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGPROF, &sa, NULL) != 0) {
            pthread_mutex_unlock(&lock);
            fprintf(stderr, "sampler: cannot handle SIGPROF: %s\n", strerror(errno));
            return 0;
        }
        handler_installed = 1;
    }
    if (table == NULL) {
        table = sample_table_create();
    }
    if (table == NULL) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "sampler: out of memory\n");
        return 0;
    }
    sample_table_clear(table);
    memset(cache_address, 0, sizeof(cache_address));
    for (i = 0; i < task_count; i++) {
        if (tasks[i].stat_fd >= 0) {
            close(tasks[i].stat_fd);
        }
    }
    task_count = 0;
    sample_count = missed_count = 0;
    have_report = 0;
    interval_us = interval ? interval : 1000;
    started_ns = now_ns();
    stopped_ns = 0;
    atomic_store(&slot.state, SLOT_IDLE);
    atomic_store(&stop_asked, 0);
    if (pthread_create(&thread, NULL, sampler_main, NULL) != 0) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "sampler: cannot start its thread\n");
        return 0;
    }
    thread_up = 1;
    atomic_store(&running, 1);
    pthread_mutex_unlock(&lock);
    return 1;
}

void sampler_stop(void) {
    pthread_mutex_lock(&lock);
    if (!thread_up || atomic_load(&stop_asked)) { /* (not running, or another stop under way) */
        pthread_mutex_unlock(&lock);
        return;
    }
    atomic_store(&stop_asked, 1);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    pthread_mutex_lock(&lock);
    thread_up = 0;
    atomic_store(&running, 0);
    stopped_ns = now_ns();
    look_at_tasks(-1); /* (the CPU times to the stop) */
    make_report();
    pthread_mutex_unlock(&lock);
}

int sampler_running(void) {
    return atomic_load(&running);
}

int sampler_report(SamplerReport *out) {
    int ok;

    pthread_mutex_lock(&lock);
    ok = have_report && report != NULL;
    if (ok) {
        memcpy(out, report, sizeof(*out));
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

/* ---- Writing the stacks ---- */
typedef struct {
    FILE *out;
    int ok;
    char *named;  /* modules with frames: one flag each */
    /* the names of other modules' frames, written once each: a small open-addressed set of addresses */
    uint64_t *seen;
    size_t seen_slots;
} Writer;

static int seen_before(Writer *w, uint64_t address) {
    size_t at = (size_t)((address * 0x9E3779B97F4A7C15ull) >> 20) & (w->seen_slots - 1);
    size_t tries;

    for (tries = 0; tries < w->seen_slots; tries++) {
        if (w->seen[at] == address) {
            return 1;
        }
        if (w->seen[at] == 0) {
            w->seen[at] = address;
            return 0;
        }
        at = (at + 1) & (w->seen_slots - 1);
    }
    return 1; /* (full: written no more) */
}

static void write_frame(Writer *w, uint64_t address) {
    Module *m = module_of(address);

    if (m == NULL) {
        w->ok &= fprintf(w->out, " ?+0x%llx", (unsigned long long)address) > 0;
    } else if (m == &modules[0]) {
        w->ok &= fprintf(w->out, " 0x%llx", (unsigned long long)(address - m->bias)) > 0;
    } else {
        w->ok &= fprintf(w->out, " %s+0x%llx", m->name, (unsigned long long)(address - m->bias)) > 0;
    }
    if (m != NULL) {
        w->named[m - modules] = 1;
    }
}

static void write_stack(void *context, int tid, uint64_t count, const uint64_t *frames, int frame_count) {
    Writer *w = context;
    int i;

    w->ok &= fprintf(w->out, "K %d %llu", tid, (unsigned long long)count) > 0;
    for (i = 0; i < frame_count; i++) {
        write_frame(w, frames[i]);
    }
    w->ok &= fputc('\n', w->out) != EOF;
}

static void write_names(void *context, int tid, uint64_t count, const uint64_t *frames, int frame_count) {
    Writer *w = context;
    int i;

    (void)tid;
    (void)count;
    for (i = 0; i < frame_count; i++) {
        Module *m = module_of(frames[i]);
        uint64_t vaddr;
        const char *name;

        if (m == NULL || m == &modules[0] || seen_before(w, frames[i])) {
            continue;
        }
        vaddr = frames[i] - m->bias;
        name = elf_symbols_find(symbols_of(m), i == 0 ? vaddr : vaddr - 1, NULL);
        if (name != NULL) {
            w->ok &= fprintf(w->out, "N %s+0x%llx %s\n", m->name, (unsigned long long)vaddr, name) > 0;
        }
    }
}

int sampler_write(FILE *out) {
    Writer w = {out, 1, NULL, NULL, (size_t)1 << 18};
    int i;

    w.named = calloc(MAX_MODULES, 1);
    w.seen = calloc(w.seen_slots, sizeof(*w.seen));
    if (w.named == NULL || w.seen == NULL) {
        free(w.named);
        free(w.seen);
        return 0;
    }
    pthread_mutex_lock(&lock);
    if (table != NULL) {
        dl_iterate_phdr(add_module, NULL);
        for (i = 0; i < task_count; i++) {
            if (tasks[i].cpu_ns || sample_table_thread_samples(table, tasks[i].tid)) {
                w.ok &= fprintf(out, "C %d %s\nT %d %.3f\n", tasks[i].tid, tasks[i].name[0] ? tasks[i].name : "?",
                                tasks[i].tid, (double)tasks[i].cpu_ns / 1e6) > 0;
            }
        }
        sample_table_each(table, write_stack, &w);
        sample_table_each(table, write_names, &w);
        for (i = 0; i < module_count; i++) {
            if (w.named[i]) {
                w.ok &= fprintf(out, "M %s %s\n", modules[i].name, modules[i].path) > 0;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    free(w.named);
    free(w.seen);
    return w.ok;
}

#else /* not Linux x86-64: no sampler */

int sampler_available(void) {
    return 0;
}

int sampler_start(unsigned interval) {
    (void)interval;
    return 0;
}

void sampler_stop(void) {
}

int sampler_running(void) {
    return 0;
}

int sampler_report(SamplerReport *report) {
    (void)report;
    return 0;
}

int sampler_write(FILE *out) {
    (void)out;
    return 0;
}

#endif
