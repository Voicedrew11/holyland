/* Fibers for the EE threads (port/include/port/platform.h). Every fiber lives on the host's main
   thread; port/platform/kernel.c decides when to switch.

   A switch is a few instructions of our own (port_fiber_swap below): it pushes the registers the
   host's ABI keeps across a call, leaves the stack pointer in the fiber, and pops the other fiber's.
   So a fiber that is not running is its stack and one pointer into it, which a save state can copy
   and put back (port/platform/savestate.c). For that the stacks are at a fixed address (FIBER_REGION),
   each fiber in its own slot, and the fibers themselves are in a fixed table: a pointer to a fiber or
   into its stack means the same in the next run of the same program. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port/host.h"
#include "port/platform.h"
#include "port/sanitize.h"
#include "port/savestate.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define MAX_FIBERS 64
/* each fiber's address space: its stack at the top, a gap that is never committed below it */
#define FIBER_SLOT_SIZE 0x800000u
#define FIBER_GAP 0x10000u
/* where the slots go: far above anything the host puts in memory by itself */
#define FIBER_REGION ((uintptr_t)0x100100000000ull)

struct PortFiber {
    void *sp; /* while it is not running: where port_fiber_swap left its registers */
    size_t stack_size;
    void (*entry)(void *);
    void *arg;
    int used;
#if PORT_ASAN
    /* the stack's bounds, for ASan's fiber switches (the host's are learned at its first switch) */
    const void *asan_bottom;
    size_t asan_size;
#endif
#if PORT_TSAN
    /* ThreadSanitizer's context for the fiber: a call stack of its own (without, every fiber's calls and returns
       were one stack's, and a report's stack ran from one EE thread into another). Not in a save state. */
    void *tsan;
#endif
};

int port_fpu_round_nearest;

/* The game's threads compute as the EE's FPU does (port/include/port/platform.h); with --fpu-round
   nearest, in the host's mode */
uint32_t port_ee_mxcsr(void) {
    return port_fpu_round_nearest ? PORT_MXCSR_HOST : PORT_MXCSR_EE;
}

static PortFiber host_fiber;
static PortFiber fibers[MAX_FIBERS];
static unsigned char *region;
static int region_fixed;

void port_fiber_swap(void **save_sp, void *load_sp);
void port_fiber_trampoline(void);
void port_fiber_start(PortFiber *fiber);

/* ---- The switch ------------------------------------------------------------------------------
   Windows x64 keeps rbx, rbp, rdi, rsi, r12..r15 and xmm6..xmm15 across a call, and a thread's
   stack bounds in its TEB (gs:0x08 StackBase, gs:0x10 StackLimit, gs:0x1478 DeallocationStack),
   which exception dispatch checks frames against: each fiber carries its own. System V keeps rbx,
   rbp, r12..r15. Both keep MXCSR's and the x87 control word's modes. A new fiber's stack is laid out
   as if it had switched away, with the trampoline as its return address and itself in r12. */
#ifdef _WIN32
#define SWAP_FRAME_SIZE 264 /* the xmm block (168), the TEB's three, eight registers, the return */
#define SWAP_MXCSR_AT 160
__asm__(".pushsection .text\n" /* and back to the section the compiler was in, after */
        ".globl port_fiber_swap\n"
        ".def port_fiber_swap; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "port_fiber_swap:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %rdi\n"
        "    pushq %rsi\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    pushq %gs:0x08\n"
        "    pushq %gs:0x10\n"
        "    pushq %gs:0x1478\n"
        "    subq $168, %rsp\n"
        "    movdqu %xmm6, 0(%rsp)\n"
        "    movdqu %xmm7, 16(%rsp)\n"
        "    movdqu %xmm8, 32(%rsp)\n"
        "    movdqu %xmm9, 48(%rsp)\n"
        "    movdqu %xmm10, 64(%rsp)\n"
        "    movdqu %xmm11, 80(%rsp)\n"
        "    movdqu %xmm12, 96(%rsp)\n"
        "    movdqu %xmm13, 112(%rsp)\n"
        "    movdqu %xmm14, 128(%rsp)\n"
        "    movdqu %xmm15, 144(%rsp)\n"
        "    stmxcsr 160(%rsp)\n"
        "    fnstcw 164(%rsp)\n"
        "    movq %rsp, (%rcx)\n"
        "    movq %rdx, %rsp\n"
        "    ldmxcsr 160(%rsp)\n"
        "    fldcw 164(%rsp)\n"
        "    movdqu 0(%rsp), %xmm6\n"
        "    movdqu 16(%rsp), %xmm7\n"
        "    movdqu 32(%rsp), %xmm8\n"
        "    movdqu 48(%rsp), %xmm9\n"
        "    movdqu 64(%rsp), %xmm10\n"
        "    movdqu 80(%rsp), %xmm11\n"
        "    movdqu 96(%rsp), %xmm12\n"
        "    movdqu 112(%rsp), %xmm13\n"
        "    movdqu 128(%rsp), %xmm14\n"
        "    movdqu 144(%rsp), %xmm15\n"
        "    addq $168, %rsp\n"
        "    popq %gs:0x1478\n"
        "    popq %gs:0x10\n"
        "    popq %gs:0x08\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        ".globl port_fiber_trampoline\n"
        ".def port_fiber_trampoline; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "port_fiber_trampoline:\n"
        "    movq %r12, %rcx\n"
        "    andq $-16, %rsp\n"
        "    subq $32, %rsp\n"
        "    call port_fiber_start\n"
        "    ud2\n"
        ".popsection\n");
#else
#define SWAP_FRAME_SIZE 64 /* MXCSR and the control word (8), six registers, the return */
#define SWAP_MXCSR_AT 0
__asm__(".pushsection .text\n" /* and back to the section the compiler was in, after */
        ".globl port_fiber_swap\n"
        ".type port_fiber_swap, @function\n"
        ".p2align 4\n"
        "port_fiber_swap:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $8, %rsp\n"
        "    stmxcsr (%rsp)\n"
        "    fnstcw 4(%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    ldmxcsr (%rsp)\n"
        "    fldcw 4(%rsp)\n"
        "    addq $8, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        ".size port_fiber_swap, .-port_fiber_swap\n"
        ".globl port_fiber_trampoline\n"
        ".type port_fiber_trampoline, @function\n"
        ".p2align 4\n"
        "port_fiber_trampoline:\n"
        "    movq %r12, %rdi\n"
        "    andq $-16, %rsp\n"
        "    call port_fiber_start\n"
        "    ud2\n"
        ".size port_fiber_trampoline, .-port_fiber_trampoline\n"
        ".popsection\n");
#endif

/* ---- ASan (port/include/port/sanitize.h) ----------------------------------------------------
   Each switch tells ASan which stack it is moving to, so its reports and its checks of stack
   accesses follow the fiber that runs. */
#if PORT_ASAN
static PortFiber *asan_from; /* the fiber the last switch left */

/* on the fiber a switch arrived on: finish the switch ASan was told of, and keep the stack bounds
   of the fiber it came from (the host's, the first time) */
static void asan_arrived(void *fake_stack) {
    const void *bottom;
    size_t size;

    __sanitizer_finish_switch_fiber(fake_stack, &bottom, &size);
    if (asan_from != NULL && asan_from->asan_bottom == NULL) {
        asan_from->asan_bottom = bottom;
        asan_from->asan_size = size;
    }
}

/* a stack about to be used afresh: what a call chain left on it (one a thread abandoned, or one a
   save state replaced) is not ASan's to check any more */
static void asan_fresh_stack(PortFiber *fiber, unsigned char *top) {
    fiber->asan_bottom = top - fiber->stack_size;
    fiber->asan_size = fiber->stack_size;
    port_unpoison(top - fiber->stack_size, fiber->stack_size);
}
#endif

void port_fiber_start(PortFiber *fiber) {
#if PORT_ASAN
    asan_arrived(NULL);
#endif
    fiber->entry(fiber->arg);
    PORT_FATAL("a fiber's entry returned");
}

/* ---- The slots ------------------------------------------------------------------------------- */
static void reserve_region(void) {
    size_t size = (size_t)MAX_FIBERS * FIBER_SLOT_SIZE;

#ifdef _WIN32
    region = VirtualAlloc((void *)FIBER_REGION, size, MEM_RESERVE, PAGE_READWRITE);
    if (region == NULL) {
        region = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
    }
#else
    region = mmap((void *)FIBER_REGION, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                  -1, 0);
    if (region == MAP_FAILED) {
        region = NULL;
    }
#endif
    if (region == NULL) {
        PORT_FATAL("cannot reserve %u MB of address space for the EE threads' stacks", (unsigned)(size >> 20));
    }
    region_fixed = (uintptr_t)region == FIBER_REGION;
    if (!region_fixed) {
        fprintf(stderr, "[port] the EE threads' stacks are not at their fixed address: no save states\n");
    }
}

static unsigned char *slot_top(int slot) {
    return region + (size_t)(slot + 1) * FIBER_SLOT_SIZE;
}

/* Makes the top `stack_size` bytes of a slot usable (once; a slot keeps its stack when reused). */
static void commit_stack(int slot, size_t stack_size) {
#ifdef _WIN32
    if (VirtualAlloc(slot_top(slot) - stack_size, stack_size, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        PORT_FATAL("cannot commit a %u KB stack for an EE thread", (unsigned)(stack_size >> 10));
    }
#else
    (void)slot;
    (void)stack_size;
#endif
}

int port_fiber_fixed(void) {
    if (region == NULL) {
        reserve_region();
    }
    return region_fixed;
}

PortFiber *port_fiber_main(void) {
    return &host_fiber;
}

PortFiber *port_fiber_create(size_t stack_size, void (*entry)(void *), void *arg) {
    PortFiber *fiber;
    unsigned char *top;
    uint64_t *frame;
    uint32_t mxcsr;
    uint16_t control;
    int slot;

    if (region == NULL) {
        reserve_region();
    }
    stack_size = (stack_size + 0xFFFF) & ~(size_t)0xFFFF;
    if (stack_size > FIBER_SLOT_SIZE - FIBER_GAP) {
        PORT_FATAL("an EE thread wants a %u KB stack, more than the PC gives one", (unsigned)(stack_size >> 10));
    }
    for (slot = 0; slot < MAX_FIBERS && fibers[slot].used; slot++) {
    }
    if (slot == MAX_FIBERS) {
        PORT_FATAL("more than %d fibers", MAX_FIBERS);
    }
    commit_stack(slot, stack_size);
    fiber = &fibers[slot];
    fiber->used = 1;
    fiber->stack_size = stack_size;
    fiber->entry = entry;
    fiber->arg = arg;
#if PORT_ASAN
    asan_fresh_stack(fiber, slot_top(slot));
#endif
#if PORT_TSAN
    /* a fresh context: the one a reused slot had holds the call stack its thread abandoned */
    if (fiber->tsan != NULL) {
        __tsan_destroy_fiber(fiber->tsan);
    }
    fiber->tsan = __tsan_create_fiber(0);
#endif

    /* the frame port_fiber_swap pops: the EE's float mode and the host's x87 control word, the fiber
       in r12, the trampoline */
    mxcsr = port_ee_mxcsr();
    __asm__ volatile("fnstcw %0" : "=m"(control));
    top = slot_top(slot);
    /* A slot keeps its stack when reused (the movie's threads are made for each movie), so the stack
       starts zeroed: what a game function reads of a local it did not set is then the same on every
       run, whatever ran on the slot before. */
    memset(top - stack_size, 0, stack_size);
    frame = (uint64_t *)(top - 16 - SWAP_FRAME_SIZE);
    memset(frame, 0, SWAP_FRAME_SIZE);
#ifdef _WIN32
    frame[20] = (uint64_t)mxcsr | ((uint64_t)control << 32); /* at 160 and 164 */
    frame[21] = (uint64_t)(uintptr_t)(top - stack_size);      /* DeallocationStack */
    frame[22] = (uint64_t)(uintptr_t)(top - stack_size);      /* StackLimit */
    frame[23] = (uint64_t)(uintptr_t)top;                     /* StackBase */
    frame[27] = (uint64_t)(uintptr_t)fiber;                   /* r12 */
    frame[32] = (uint64_t)(uintptr_t)port_fiber_trampoline;
#else
    frame[0] = (uint64_t)mxcsr | ((uint64_t)control << 32);
    frame[4] = (uint64_t)(uintptr_t)fiber; /* r12 */
    frame[7] = (uint64_t)(uintptr_t)port_fiber_trampoline;
#endif
    fiber->sp = frame;
    return fiber;
}

void port_fiber_delete(PortFiber *fiber) {
    fiber->used = 0;
}

void port_fiber_switch(PortFiber *from, PortFiber *to) {
#if PORT_ASAN
    void *fake_stack = NULL;

    asan_from = from;
    __sanitizer_start_switch_fiber(&fake_stack, to->asan_bottom, to->asan_size);
    port_fiber_swap(&from->sp, to->sp);
    asan_arrived(fake_stack);
#else
#if PORT_TSAN
    /* (with a happens-before from this fiber to the next: they are one host thread taking turns) */
    if (from->tsan == NULL) {
        from->tsan = __tsan_get_current_fiber(); /* the host's, at its first switch */
    }
    __tsan_switch_to_fiber(to->tsan, 0);
#endif
    port_fiber_swap(&from->sp, to->sp);
#endif
}

/* ---- Save states -----------------------------------------------------------------------------
   The fibers as they are, and the live part of each stack, from where it switched away to its top.
   The host's own fiber is not in a state: a state is taken and loaded on it. */
void port_fiber_state(PortStateIo *io) {
    int slot;

    for (slot = 0; slot < MAX_FIBERS; slot++) {
        PortFiber *fiber = &fibers[slot];

        PORT_STATE(io, fiber->used);
        PORT_STATE(io, fiber->sp);
        PORT_STATE(io, fiber->stack_size);
        PORT_STATE(io, fiber->entry);
        PORT_STATE(io, fiber->arg);
        if (fiber->used) {
            unsigned char *top = slot_top(slot);
            unsigned char *sp = fiber->sp;

            if (fiber->stack_size > FIBER_SLOT_SIZE - FIBER_GAP || sp < top - fiber->stack_size || sp > top) {
                PORT_FATAL("a state's fiber %d has its stack pointer outside its stack", slot);
            }
            if (io->loading) {
                commit_stack(slot, fiber->stack_size);
#if PORT_ASAN
                asan_fresh_stack(fiber, top);
#endif
                /* below the live part, what a fresh stack has, whatever this run left there */
                memset(top - fiber->stack_size, 0, (size_t)(sp - (top - fiber->stack_size)));
            }
            port_state_bytes(io, sp, (size_t)(top - sp));
            if (io->loading) {
                /* the float mode this run asked for, whichever the state was saved with */
                uint32_t mxcsr;

                memcpy(&mxcsr, sp + SWAP_MXCSR_AT, 4);
                mxcsr = (mxcsr & PORT_MXCSR_FLAGS) | port_ee_mxcsr();
                memcpy(sp + SWAP_MXCSR_AT, &mxcsr, 4);
            }
        }
    }
}
