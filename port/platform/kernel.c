/* The EE kernel for the PC port: threads, semaphores, interrupt handlers, the timers and the vblank
   (port/include/eekernel.h, port/include/port/platform.h).

   Threads are fibers of the host's main thread and follow the EE kernel's rules: the ready thread of
   the best priority (the lowest number) runs, a thread of the same priority only gets the CPU when the
   running one waits or rotates the queue, and a switch happens only inside a kernel call. The main
   thread runs at priority 1, where libkernl's InitThread leaves it (decomp: src/sce/libkernl/thread.c).

   Time is the EE's bus clock, kept here, not the host's. It moves at a vblank wait (to the next
   vblank) and a little at the calls the game polls in loops; between two kernel calls it stands still.
   So the game sees the same timer counts, fields and interrupts on every run with the same input.
   --fast lets the host run ahead of 59.94 vblanks a second; otherwise each vblank waits for the
   host's clock to catch up.

   Interrupt handlers run in the kernel calls, on the stack of the thread that made the call, as the
   EE runs them on the interrupted thread's. What they make ready runs once they return. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eekernel.h"
#include "port/host.h"
#include "port/platform.h"

#define MAX_THREADS 64
#define MAX_SEMAS 64
#define MAX_HANDLERS 16
#define LOWEST_PRIORITY 127
/* the host stack each EE thread gets: the PC's code needs more than the EE's */
#define MIN_FIBER_STACK 0x40000

/* NTSC interlaced: 59.94 fields a second, 262.5 lines a field */
#define FIELD_CYCLES_X5 12300288u /* 5 x 147456000 / (60000 / 1001) */
#define LINE_CYCLES 9372u
#define VBLANK_LINES 22u

/* INTC causes and the EE kernel's error codes the game could see */
#define INTC_VBLANK_S 2
#define INTC_VBLANK_E 3
#define INTC_TIM0 9
#define KE_ILLEGAL_THID (-406)
#define KE_ILLEGAL_SEMID (-408)
#define KE_SEMA_ZERO (-419)

typedef enum { THREAD_FREE, THREAD_DORMANT, THREAD_READY, THREAD_RUN, THREAD_WAIT } ThreadStatus;
typedef enum { WAIT_NONE, WAIT_SLEEP, WAIT_SEMA, WAIT_VSYNC } WaitKind;

typedef struct {
    ThreadStatus status;
    int priority;
    int init_priority;
    void (*entry)(void *);
    void *arg;
    size_t stack_size;
    PortFiber *fiber;
    WaitKind wait;
    int wait_id;
    int wakeup_count;
    int64_t order; /* the place in its priority's ready queue: lower goes first */
} Thread;

typedef struct {
    int used;
    int count;
    int max_count;
    int init_count;
} Sema;

typedef struct {
    int (*handler)(int);
    int id;
} Handler;

typedef struct {
    Handler list[MAX_HANDLERS];
    int count;
    int enabled;
} HandlerChain;

static Thread threads[MAX_THREADS];
static Sema semas[MAX_SEMAS];
static int current = 1;
static int64_t order_tail;
static int64_t order_head;
static int need_reschedule;

static HandlerChain intc_chains[16];
static HandlerChain dmac_chains[16];
static int next_handler_id = 1;
static int in_interrupt;
static volatile uint32_t dmac_pending; /* channels whose end the DMA controller's thread posted */

static uint64_t ee_cycle;
static uint64_t vblank_count;
static uint64_t next_vblank_start;
static uint64_t next_vblank_end;

/* ---- The timers ------------------------------------------------------------------------------
   Each counts at the bus clock divided by its mode's CLKS (1, 16, 256; HBLANK is taken as a line),
   from the COUNT the game left in the register. */
#define TIMER_COUNT(n) EE_HW(volatile uint32_t *, 0x10000000 + (n) * 0x800)
#define TIMER_MODE(n) EE_HW(volatile uint32_t *, 0x10000010 + (n) * 0x800)
#define TIMER_COMP(n) EE_HW(volatile uint32_t *, 0x10000020 + (n) * 0x800)
#define T_MODE_ZRET 0x40
#define T_MODE_CUE 0x80
#define T_MODE_CMPE 0x100
#define T_MODE_OVFE 0x200
#define T_MODE_EQUF 0x400
#define T_MODE_OVFF 0x800

static uint64_t timer_synced[4]; /* the EE time each timer's COUNT was brought up to */

static uint32_t timer_divider(uint32_t mode) {
    static const uint32_t dividers[4] = {1, 16, 256, LINE_CYCLES};

    return dividers[mode & 3];
}

static void raise_intc(int cause);

/* Brings a timer's COUNT up to the EE time `now`, raising its interrupts on the way. */
static void timer_sync(int n, uint64_t now) {
    uint32_t mode = *TIMER_MODE(n);
    uint32_t divider = timer_divider(mode);
    uint64_t ticks;
    uint32_t count, compare;

    if (now <= timer_synced[n]) {
        return;
    }
    ticks = now / divider - timer_synced[n] / divider;
    timer_synced[n] = now;
    if (!(mode & T_MODE_CUE) || ticks == 0) {
        return;
    }
    count = *TIMER_COUNT(n) & 0xFFFF;
    compare = *TIMER_COMP(n) & 0xFFFF;
    while (ticks > 0) {
        /* ticks to the next compare match or overflow */
        uint32_t to_compare = compare > count ? compare - count : 0x10000 - count + compare;
        uint32_t to_overflow = 0x10000 - count;
        uint32_t step = to_compare < to_overflow ? to_compare : to_overflow;

        if (ticks < step) {
            count += (uint32_t)ticks;
            break;
        }
        ticks -= step;
        count = (count + step) & 0xFFFF;
        if (count == compare) {
            if (mode & T_MODE_CMPE) {
                *TIMER_MODE(n) |= T_MODE_EQUF;
                raise_intc(INTC_TIM0 + n);
            }
            if (mode & T_MODE_ZRET) {
                count = 0;
            }
        } else if (count == 0) {
            if (mode & T_MODE_OVFE) {
                *TIMER_MODE(n) |= T_MODE_OVFF;
                raise_intc(INTC_TIM0 + n);
            }
        }
    }
    *TIMER_COUNT(n) = count;
}

/* The EE time of a timer's next interrupt, or UINT64_MAX. */
static uint64_t timer_next_event(int n) {
    uint32_t mode = *TIMER_MODE(n);
    uint32_t divider = timer_divider(mode);
    uint32_t count, compare, ticks = 0;

    if (!(mode & T_MODE_CUE) || !(mode & (T_MODE_CMPE | T_MODE_OVFE))) {
        return UINT64_MAX;
    }
    count = *TIMER_COUNT(n) & 0xFFFF;
    compare = *TIMER_COMP(n) & 0xFFFF;
    if (mode & T_MODE_CMPE) {
        ticks = compare > count ? compare - count : 0x10000 - count + compare;
    }
    if ((mode & T_MODE_OVFE) && (ticks == 0 || 0x10000 - count < ticks)) {
        ticks = 0x10000 - count;
    }
    return (timer_synced[n] / divider + ticks) * divider;
}

/* ---- Interrupts ------------------------------------------------------------------------------ */
static uint32_t intc_pending;

static void raise_intc(int cause) {
    intc_pending |= 1u << cause;
}

static void run_chain(HandlerChain *chain, int cause) {
    int i;

    if (!chain->enabled) {
        return;
    }
    for (i = 0; i < chain->count; i++) {
        chain->list[i].handler(cause);
    }
}

/* Runs the handlers of the interrupts that are pending; what they make ready runs afterwards. */
static void deliver_interrupts(void) {
    uint32_t dmac;
    int cause;

    if (in_interrupt) {
        return;
    }
    in_interrupt = 1;
    while (intc_pending != 0 || dmac_pending != 0) {
        for (cause = 0; cause < 16; cause++) {
            if (intc_pending & (1u << cause)) {
                intc_pending &= ~(1u << cause);
                run_chain(&intc_chains[cause], cause);
            }
        }
        dmac = __atomic_exchange_n(&dmac_pending, 0, __ATOMIC_ACQ_REL);
        for (cause = 0; cause < 16; cause++) {
            if (dmac & (1u << cause)) {
                run_chain(&dmac_chains[cause], cause);
            }
        }
    }
    in_interrupt = 0;
}

void port_kernel_post_dmac(int channel) {
    __atomic_fetch_or(&dmac_pending, 1u << channel, __ATOMIC_ACQ_REL);
}

static int add_handler(HandlerChain *chain, int (*handler)(int), int next) {
    int id = next_handler_id++;

    if (chain->count == MAX_HANDLERS) {
        PORT_FATAL("too many interrupt handlers on one cause");
    }
    if (next == 0) {
        memmove(&chain->list[1], &chain->list[0], (size_t)chain->count * sizeof(Handler));
        chain->list[0].handler = handler;
        chain->list[0].id = id;
    } else {
        chain->list[chain->count].handler = handler;
        chain->list[chain->count].id = id;
    }
    chain->count++;
    return id;
}

static int remove_handler(HandlerChain *chain, int id) {
    int i;

    for (i = 0; i < chain->count; i++) {
        if (chain->list[i].id == id) {
            memmove(&chain->list[i], &chain->list[i + 1], (size_t)(chain->count - i - 1) * sizeof(Handler));
            chain->count--;
            return 0;
        }
    }
    return -1;
}

static int bad_cause(int cause) {
    return cause < 0 || cause >= 16;
}

/* ---- Time ------------------------------------------------------------------------------------ */
static double pace_start_wall;

static void wake_vsync_waiters(void);
static void frame_end(void);

static void vblank_start(void) {
    volatile uint64_t *csr = EE_HW(volatile uint64_t *, 0x12001000);

    vblank_count++;
    /* FIELD (bit 13) turns over each field; VSINT (bit 3) is set */
    *csr = (*csr ^ (1u << 13)) | (1u << 3);
    raise_intc(INTC_VBLANK_S);
    wake_vsync_waiters();
    if (!port_options.fast) {
        double due = pace_start_wall + (double)ee_cycle / (double)EE_BUS_HZ;
        double now = port_wall_seconds();

        if (due > now) {
            port_sleep(due - now);
        } else if (now - due > 0.25) {
            /* the host fell behind (a breakpoint, a slow load): start counting again from here */
            pace_start_wall = now - (double)ee_cycle / (double)EE_BUS_HZ;
        }
    }
    port_pad_poll();
}

/* Moves EE time on to `until`, raising the vblank and timer interrupts it passes, in their order. */
static void advance_to(uint64_t until) {
    for (;;) {
        uint64_t next = next_vblank_start < next_vblank_end ? next_vblank_start : next_vblank_end;
        int timer_due = -1;
        int n;

        for (n = 0; n < 2; n++) {
            uint64_t t = timer_next_event(n);

            if (t < next) {
                next = t;
                timer_due = n;
            }
        }
        if (next > until) {
            break;
        }
        ee_cycle = next;
        timer_sync(0, ee_cycle);
        timer_sync(1, ee_cycle);
        if (timer_due < 0) {
            if (next == next_vblank_start) {
                next_vblank_end = next_vblank_start + VBLANK_LINES * LINE_CYCLES;
                next_vblank_start = (uint64_t)(vblank_count + 2) * FIELD_CYCLES_X5 / 5;
                vblank_start();
            } else {
                next_vblank_end = UINT64_MAX;
                raise_intc(INTC_VBLANK_E);
            }
        }
        /* each event's handlers run before the next event, so that none is lost */
        deliver_interrupts();
    }
    ee_cycle = until;
    timer_sync(0, ee_cycle);
    timer_sync(1, ee_cycle);
}

uint64_t port_ee_cycles(void) {
    return ee_cycle;
}

uint64_t port_vblank_count(void) {
    return vblank_count;
}

/* ---- Threads --------------------------------------------------------------------------------- */
static Thread *thread_of(int id) {
    if (id <= 0 || id >= MAX_THREADS || threads[id].status == THREAD_FREE) {
        return NULL;
    }
    return &threads[id];
}

static void make_ready(Thread *thread, int at_front) {
    thread->status = THREAD_READY;
    thread->wait = WAIT_NONE;
    thread->order = at_front ? --order_head : ++order_tail;
    need_reschedule = 1;
}

/* The thread that should run: the best priority; the running one keeps the CPU against its equals. */
static int pick_thread(void) {
    int best = -1;
    int id;

    for (id = 1; id < MAX_THREADS; id++) {
        Thread *t = &threads[id];

        if (t->status != THREAD_READY && t->status != THREAD_RUN) {
            continue;
        }
        if (best < 0 || t->priority < threads[best].priority) {
            best = id;
        } else if (t->priority == threads[best].priority) {
            if (threads[best].status == THREAD_RUN) {
                continue;
            }
            if (t->status == THREAD_RUN || t->order < threads[best].order) {
                best = id;
            }
        }
    }
    return best;
}

static void wake_vsync_waiters(void) {
    int id;

    for (id = 1; id < MAX_THREADS; id++) {
        if (threads[id].status == THREAD_WAIT && threads[id].wait == WAIT_VSYNC) {
            make_ready(&threads[id], 0);
        }
    }
}

static void dump_threads(void) {
    int id;

    for (id = 1; id < MAX_THREADS; id++) {
        Thread *t = &threads[id];

        if (t->status != THREAD_FREE) {
            fprintf(stderr, "[kernel]   thread %d: status %d, priority %d, wait %d (%d)\n", id, t->status,
                    t->priority, t->wait, t->wait_id);
        }
    }
}

/* Runs the thread that should run, after a kernel call changed who is ready. When none is, the EE
   idles: time moves on to the next interrupt. In an interrupt handler nothing switches: the
   handlers' caller reschedules once they are done. */
static void reschedule(void) {
    uint64_t idle_since = ee_cycle;

    if (in_interrupt) {
        need_reschedule = 1;
        return;
    }
    for (;;) {
        int next;

        deliver_interrupts();
        need_reschedule = 0;
        next = pick_thread();
        if (next < 0) {
            uint64_t until = next_vblank_start;
            int n;

            for (n = 0; n < 2; n++) {
                uint64_t t = timer_next_event(n);

                if (t < until) {
                    until = t;
                }
            }
            if (ee_cycle - idle_since > 10ull * EE_BUS_HZ) {
                fprintf(stderr, "[kernel] every thread has waited for ten seconds of EE time:\n");
                dump_threads();
                PORT_FATAL("the EE threads are deadlocked");
            }
            advance_to(until);
            continue;
        }
        if (next == current) {
            threads[current].status = THREAD_RUN;
            return;
        }
        {
            int previous = current;

            if (threads[previous].status == THREAD_RUN) {
                /* preempted: back to the front of its queue */
                threads[previous].status = THREAD_READY;
                threads[previous].order = --order_head;
            }
            current = next;
            threads[next].status = THREAD_RUN;
            port_fiber_switch(threads[previous].fiber, threads[next].fiber);
            /* back on `previous`, now running again */
        }
        if (!need_reschedule) {
            return;
        }
    }
}

/* Puts the running thread to wait and runs another until something makes it ready again. */
static void wait_current(WaitKind kind, int id) {
    threads[current].status = THREAD_WAIT;
    threads[current].wait = kind;
    threads[current].wait_id = id;
    reschedule();
}

static void thread_entry(void *param) {
    Thread *thread = param;

    thread->entry(thread->arg);
    /* returning from a thread's entry is ExitThread */
    ExitThread();
}

void port_kernel_init(void) {
    Thread *main_thread = &threads[1];

    main_thread->status = THREAD_RUN;
    main_thread->priority = 1;
    main_thread->init_priority = 1;
    main_thread->fiber = port_fiber_main();
    current = 1;
    next_vblank_start = FIELD_CYCLES_X5 / 5;
    next_vblank_end = UINT64_MAX;
    pace_start_wall = port_wall_seconds();
}

void port_kernel_poll(void) {
    if (in_interrupt) {
        return;
    }
    deliver_interrupts();
    if (need_reschedule) {
        reschedule();
    }
}

void port_kernel_spend(uint32_t cycles) {
    advance_to(ee_cycle + cycles);
    port_kernel_poll();
}

void port_vsync(void) {
    if (in_interrupt) {
        PORT_FATAL("VSync in an interrupt handler");
    }
    wait_current(WAIT_VSYNC, 0);
    frame_end();
}

int CreateThread(struct ThreadParam *param) {
    int id;

    for (id = 2; id < MAX_THREADS; id++) {
        if (threads[id].status == THREAD_FREE) {
            Thread *t = &threads[id];

            memset(t, 0, sizeof(*t));
            t->status = THREAD_DORMANT;
            t->entry = param->entry;
            t->init_priority = param->initPriority;
            t->priority = param->initPriority;
            t->stack_size = (size_t)param->stackSize * 16;
            if (t->stack_size < MIN_FIBER_STACK) {
                t->stack_size = MIN_FIBER_STACK;
            }
            return id;
        }
    }
    return -1;
}

int DeleteThread(int id) {
    Thread *t = thread_of(id);

    if (t == NULL || id == current || t->status != THREAD_DORMANT) {
        return KE_ILLEGAL_THID;
    }
    if (t->fiber != NULL) {
        port_fiber_delete(t->fiber);
    }
    memset(t, 0, sizeof(*t));
    return id;
}

int StartThread(int id, void *arg) {
    Thread *t = thread_of(id);

    if (t == NULL || t->status != THREAD_DORMANT) {
        return KE_ILLEGAL_THID;
    }
    if (t->fiber != NULL) {
        /* it ran before, and has left its fiber (which is not running: this thread is) */
        port_fiber_delete(t->fiber);
    }
    t->arg = arg;
    t->priority = t->init_priority;
    t->wakeup_count = 0;
    t->fiber = port_fiber_create(t->stack_size, thread_entry, t);
    make_ready(t, 0);
    reschedule();
    return id;
}

void ExitThread(void) {
    threads[current].status = THREAD_DORMANT;
    threads[current].wait = WAIT_NONE;
    reschedule();
    PORT_FATAL("a thread that exited was run again");
}

int TerminateThread(int id) {
    Thread *t = thread_of(id);

    if (t == NULL || id == current || t->status == THREAD_DORMANT) {
        return KE_ILLEGAL_THID;
    }
    t->status = THREAD_DORMANT;
    t->wait = WAIT_NONE;
    return id;
}

void Exit(int status) {
    fprintf(stderr, "[port] the game called Exit(%d)\n", status);
    exit(status);
}

int ChangeThreadPriority(int id, int priority) {
    Thread *t = thread_of(id == 0 ? current : id);
    int old;

    if (t == NULL || priority < 0 || priority > LOWEST_PRIORITY) {
        return KE_ILLEGAL_THID;
    }
    old = t->priority;
    t->priority = priority;
    if (t->status == THREAD_READY) {
        t->order = ++order_tail;
    }
    need_reschedule = 1;
    reschedule();
    return old;
}

int RotateThreadReadyQueue(int priority) {
    int id;
    int first = -1;

    if (priority < 0 || priority > LOWEST_PRIORITY) {
        return -1;
    }
    /* a thread polling in a rotation loop lets EE time pass, so that its vblank comes */
    advance_to(ee_cycle + 256);
    for (id = 1; id < MAX_THREADS; id++) {
        Thread *t = &threads[id];

        if ((t->status == THREAD_READY || t->status == THREAD_RUN) && t->priority == priority) {
            if (t->status == THREAD_RUN) {
                first = id;
                break;
            }
            if (first < 0 || t->order < threads[first].order) {
                first = id;
            }
        }
    }
    if (first >= 0) {
        threads[first].status = THREAD_READY;
        threads[first].order = ++order_tail;
        need_reschedule = 1;
    }
    reschedule();
    return priority;
}

int GetThreadId(void) {
    return current;
}

int SleepThread(void) {
    if (threads[current].wakeup_count > 0) {
        threads[current].wakeup_count--;
        return current;
    }
    wait_current(WAIT_SLEEP, 0);
    return current;
}

static int wakeup(int id) {
    Thread *t = thread_of(id);

    if (t == NULL || t->status == THREAD_DORMANT) {
        return KE_ILLEGAL_THID;
    }
    if (t->status == THREAD_WAIT && t->wait == WAIT_SLEEP) {
        make_ready(t, 0);
    } else {
        t->wakeup_count++;
    }
    return id;
}

int WakeupThread(int id) {
    int result = wakeup(id);

    reschedule();
    return result;
}

int iWakeupThread(int id) {
    return wakeup(id);
}

/* ---- Semaphores ------------------------------------------------------------------------------- */
static Sema *sema_of(int id) {
    if (id <= 0 || id >= MAX_SEMAS || !semas[id].used) {
        return NULL;
    }
    return &semas[id];
}

int CreateSema(struct SemaParam *param) {
    int id;

    for (id = 1; id < MAX_SEMAS; id++) {
        if (!semas[id].used) {
            semas[id].used = 1;
            semas[id].count = param->initCount;
            semas[id].max_count = param->maxCount;
            semas[id].init_count = param->initCount;
            return id;
        }
    }
    return -1;
}

int DeleteSema(int id) {
    Sema *s = sema_of(id);
    int t;

    if (s == NULL) {
        return KE_ILLEGAL_SEMID;
    }
    /* the threads still waiting wake up (with an error on the EE) */
    for (t = 1; t < MAX_THREADS; t++) {
        if (threads[t].status == THREAD_WAIT && threads[t].wait == WAIT_SEMA && threads[t].wait_id == id) {
            make_ready(&threads[t], 0);
        }
    }
    memset(s, 0, sizeof(*s));
    reschedule();
    return id;
}

/* Signals a semaphore: hands it to its first waiter, or counts it. */
static int signal(int id) {
    Sema *s = sema_of(id);
    int best = -1;
    int t;

    if (s == NULL) {
        return KE_ILLEGAL_SEMID;
    }
    for (t = 1; t < MAX_THREADS; t++) {
        if (threads[t].status == THREAD_WAIT && threads[t].wait == WAIT_SEMA && threads[t].wait_id == id &&
            (best < 0 || threads[t].order < threads[best].order)) {
            best = t;
        }
    }
    if (best >= 0) {
        make_ready(&threads[best], 0);
    } else if (s->count < s->max_count || s->max_count <= 0) {
        s->count++;
    }
    return id;
}

int SignalSema(int id) {
    int result = signal(id);

    reschedule();
    return result;
}

int iSignalSema(int id) {
    return signal(id);
}

int WaitSema(int id) {
    Sema *s = sema_of(id);

    if (s == NULL) {
        return KE_ILLEGAL_SEMID;
    }
    port_kernel_poll();
    if (s->count > 0) {
        s->count--;
        return id;
    }
    /* order is the place in the semaphore's queue while the thread waits */
    threads[current].order = ++order_tail;
    wait_current(WAIT_SEMA, id);
    return id;
}

int PollSema(int id) {
    Sema *s = sema_of(id);

    if (s == NULL) {
        return KE_ILLEGAL_SEMID;
    }
    port_kernel_spend(256);
    if (s->count > 0) {
        s->count--;
        return id;
    }
    return KE_SEMA_ZERO;
}

/* ---- Interrupt handlers ----------------------------------------------------------------------- */
int AddIntcHandler(int cause, int (*handler)(int), int next) {
    return bad_cause(cause) ? -1 : add_handler(&intc_chains[cause], handler, next);
}

int RemoveIntcHandler(int cause, int id) {
    return bad_cause(cause) ? -1 : remove_handler(&intc_chains[cause], id);
}

int AddDmacHandler(int channel, int (*handler)(int), int next) {
    return bad_cause(channel) ? -1 : add_handler(&dmac_chains[channel], handler, next);
}

int RemoveDmacHandler(int channel, int id) {
    return bad_cause(channel) ? -1 : remove_handler(&dmac_chains[channel], id);
}

/* Enable/Disable return 1 when the cause was enabled before, 0 when it was not */
static int set_enabled(HandlerChain *chain, int enabled) {
    int was = chain->enabled;

    chain->enabled = enabled;
    return was;
}

int EnableIntc(int cause) {
    return bad_cause(cause) ? -1 : set_enabled(&intc_chains[cause], 1);
}

int DisableIntc(int cause) {
    return bad_cause(cause) ? -1 : set_enabled(&intc_chains[cause], 0);
}

int EnableDmac(int channel) {
    return bad_cause(channel) ? -1 : set_enabled(&dmac_chains[channel], 1);
}

int DisableDmac(int channel) {
    return bad_cause(channel) ? -1 : set_enabled(&dmac_chains[channel], 0);
}

int iEnableIntc(int cause) {
    return EnableIntc(cause);
}

int iDisableIntc(int cause) {
    return DisableIntc(cause);
}

int iEnableDmac(int channel) {
    return EnableDmac(channel);
}

int iDisableDmac(int channel) {
    return DisableDmac(channel);
}

/* Interrupts are only taken in kernel calls on the PC, so there is nothing to hold off. */
int DIntr(void) {
    return 1;
}

int EIntr(void) {
    return 1;
}

/* ---- Caches: nothing to do on the PC, but the game flushes right before it starts a channel ---- */
void FlushCache(int operation) {
    (void)operation;
    port_dmac_wake();
    port_kernel_poll();
}

void SyncDCache(void *start, void *end) {
    (void)start;
    (void)end;
    port_dmac_wake();
}

void iSyncDCache(void *start, void *end) {
    (void)start;
    (void)end;
}

void InvalidDCache(void *start, void *end) {
    (void)start;
    (void)end;
}

/* ---- Once a frame ----------------------------------------------------------------------------- */
static void frame_end(void) {
    if (port_log_enabled()) {
        char line[512];

        kf4_game_state(line, sizeof(line));
        port_log("%llu %s", (unsigned long long)vblank_count, line);
    }
    if (port_options.frames > 0 && vblank_count >= (uint64_t)port_options.frames) {
        port_event("stop: %lld vblanks run", port_options.frames);
        exit(0);
    }
}
