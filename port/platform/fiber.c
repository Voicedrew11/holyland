/* Fibers for the EE threads (port/include/port/platform.h): Windows' own fibers, ucontext elsewhere.
   Every fiber lives on the host's main thread; port/platform/kernel.c decides when to switch. */
#include <stdlib.h>

#include "port/host.h"
#include "port/platform.h"

#ifdef _WIN32
#include <windows.h>

struct PortFiber {
    void *handle;
    void (*entry)(void *);
    void *arg;
};

static void CALLBACK fiber_start(void *param) {
    PortFiber *fiber = param;

    fiber->entry(fiber->arg);
    PORT_FATAL("a fiber's entry returned");
}

PortFiber *port_fiber_main(void) {
    static PortFiber main_fiber;

    if (main_fiber.handle == NULL) {
        main_fiber.handle = ConvertThreadToFiber(NULL);
        if (main_fiber.handle == NULL) {
            PORT_FATAL("ConvertThreadToFiber failed (%lu)", GetLastError());
        }
    }
    return &main_fiber;
}

PortFiber *port_fiber_create(size_t stack_size, void (*entry)(void *), void *arg) {
    PortFiber *fiber = calloc(1, sizeof(*fiber));

    if (fiber == NULL) {
        PORT_FATAL("out of memory for a fiber");
    }
    fiber->entry = entry;
    fiber->arg = arg;
    fiber->handle = CreateFiber(stack_size, fiber_start, fiber);
    if (fiber->handle == NULL) {
        PORT_FATAL("CreateFiber failed (%lu)", GetLastError());
    }
    return fiber;
}

void port_fiber_delete(PortFiber *fiber) {
    DeleteFiber(fiber->handle);
    free(fiber);
}

void port_fiber_switch(PortFiber *from, PortFiber *to) {
    (void)from;
    SwitchToFiber(to->handle);
}

#else
#include <ucontext.h>

struct PortFiber {
    ucontext_t context;
    void *stack;
    void (*entry)(void *);
    void *arg;
};

/* makecontext passes only ints: the fiber that starts is handed over here (one runs at a time) */
static PortFiber *starting_fiber;

static void fiber_start(void) {
    PortFiber *fiber = starting_fiber;

    fiber->entry(fiber->arg);
    PORT_FATAL("a fiber's entry returned");
}

PortFiber *port_fiber_main(void) {
    static PortFiber main_fiber;

    return &main_fiber;
}

PortFiber *port_fiber_create(size_t stack_size, void (*entry)(void *), void *arg) {
    PortFiber *fiber = calloc(1, sizeof(*fiber));

    if (fiber == NULL || (fiber->stack = malloc(stack_size)) == NULL) {
        PORT_FATAL("out of memory for a fiber");
    }
    fiber->entry = entry;
    fiber->arg = arg;
    if (getcontext(&fiber->context) != 0) {
        PORT_FATAL("getcontext failed");
    }
    fiber->context.uc_stack.ss_sp = fiber->stack;
    fiber->context.uc_stack.ss_size = stack_size;
    fiber->context.uc_link = NULL;
    makecontext(&fiber->context, fiber_start, 0);
    return fiber;
}

void port_fiber_delete(PortFiber *fiber) {
    free(fiber->stack);
    free(fiber);
}

void port_fiber_switch(PortFiber *from, PortFiber *to) {
    starting_fiber = to;
    if (swapcontext(&from->context, &to->context) != 0) {
        PORT_FATAL("swapcontext failed");
    }
}
#endif
