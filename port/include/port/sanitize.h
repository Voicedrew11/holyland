/* Sanitizer support for the port's own memory tricks (cmake -DKF4_SANITIZE=address,... or thread;
   docs/port/SANITIZERS.md). Without them, PORT_ASAN and PORT_TSAN are 0 and nothing here costs anything.

   - The fibers (port/platform/fiber.c) tell ASan about each stack switch.
   - A fiber's stack is cleared of the poison an abandoned or restored call chain left on it.
   - Save states (port/platform/savestate.c) copy whole regions (the program's globals with the
     redzones between them, the live part of each stack with its frames' redzones) with
     port_raw_copy, which ASan does not check.
   - ThreadSanitizer: each fiber has a TSan context of its own, so a report shows the EE thread's own calls. */
#ifndef PORT_SANITIZE_H
#define PORT_SANITIZE_H

#include <stddef.h>
#include <stdint.h>

#if defined(__SANITIZE_ADDRESS__)
#define PORT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PORT_ASAN 1
#endif
#endif
#ifndef PORT_ASAN
#define PORT_ASAN 0
#endif

#if defined(__SANITIZE_THREAD__)
#define PORT_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PORT_TSAN 1
#endif
#endif
#ifndef PORT_TSAN
#define PORT_TSAN 0
#endif
#if PORT_TSAN
#include <sanitizer/tsan_interface.h>
#endif

#if PORT_ASAN
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#define PORT_NO_ASAN __attribute__((no_sanitize_address))
#define port_unpoison(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define PORT_NO_ASAN
#define port_unpoison(p, n) ((void)(p), (void)(n))
#endif

/* memcpy that ASan does not check: volatile, so the compiler cannot turn it back into a call to
   the (intercepted) memcpy. Only the sanitizer build uses it. */
static inline PORT_NO_ASAN void port_raw_copy(void *dst, const void *src, size_t size) {
    volatile unsigned char *d = dst;
    const volatile unsigned char *s = src;
    size_t i = 0;

    if ((((uintptr_t)dst | (uintptr_t)src) & 7) == 0) {
        for (; i + 8 <= size; i += 8) {
            *(volatile uint64_t *)(d + i) = *(const volatile uint64_t *)(s + i);
        }
    }
    for (; i < size; i++) {
        d[i] = s[i];
    }
}

#endif
