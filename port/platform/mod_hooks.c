/* Hooks on the game's functions for mods (port/include/port/mod_api.h).

   The game's code is compiled with -fpatchable-function-entry=16 (CMakeLists.txt): every function of it starts with
   16 bytes of no-ops. A hook writes over them a jump to the mod's replacement (jmp [rip+0] and the 8-byte address:
   14 bytes); what calls the function as it was is the code just after them. A second mod's hook on the same function
   jumps to its own replacement, whose "original" is the first mod's. A function the compiler inlined into its
   callers, or one that is not the game's (no pad), cannot be hooked: the pad is checked against port_mod_pad_probe's,
   a function compiled the same way (game/mod_events.c). x86-64 only. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "port/mod_api.h"

#define PAD 16

void port_mod_pad_probe(void); /* game/mod_events.c */

typedef struct {
    unsigned char *pad;
    void *target;
    char name[96];
} Hooked;

static Hooked hooked[256];
static int hooked_count;

void *port_mod_symbol(const char *name) {
    if (name == NULL || !*name) {
        return NULL;
    }
#ifdef _WIN32
    return (void *)GetProcAddress(GetModuleHandleW(NULL), name); /* (the program exports its symbols) */
#else
    return dlsym(RTLD_DEFAULT, name); /* (linked with -rdynamic) */
#endif
}

/* the pad's first byte: after an endbr64 when there is one */
static unsigned char *pad_of(void *function) {
    unsigned char *p = function;

    if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA) {
        p += 4;
    }
    return p;
}

static int write_code(unsigned char *at, const unsigned char *bytes, size_t size) {
#ifdef _WIN32
    DWORD old;

    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    memcpy(at, bytes, size);
    VirtualProtect(at, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return 1;
#else
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t first = (uintptr_t)at & ~(uintptr_t)(page - 1);
    uintptr_t last = ((uintptr_t)at + size - 1) & ~(uintptr_t)(page - 1);
    size_t length = last - first + (size_t)page;

    if (mprotect((void *)first, length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return 0;
    }
    memcpy(at, bytes, size);
    mprotect((void *)first, length, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)at, (char *)at + size);
    return 1;
#endif
}

static int jump(unsigned char *pad, void *target) {
    unsigned char code[14] = {0xFF, 0x25, 0, 0, 0, 0}; /* jmp [rip+0] */
    uint64_t address = (uint64_t)(uintptr_t)target;

    memcpy(code + 6, &address, 8);
    return write_code(pad, code, sizeof(code));
}

int port_mod_hook(const char *mod_id, const char *name, void *replacement, void **original) {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned char *function, *pad;
    int i;

    if (replacement == NULL || original == NULL) {
        return 0;
    }
    function = port_mod_symbol(name);
    if (function == NULL) {
        fprintf(stderr, "[mods] %s: hook %s: no such function (or not exported)\n", mod_id, name);
        return 0;
    }
    pad = pad_of(function);
    for (i = 0; i < hooked_count; i++) {
        if (hooked[i].pad == pad) { /* (hooked before: chained) */
            *original = hooked[i].target;
            if (!jump(pad, replacement)) {
                return 0;
            }
            hooked[i].target = replacement;
            fprintf(stderr, "[mods] %s: hooked %s (over another mod's)\n", mod_id, name);
            return 1;
        }
    }
    if (hooked_count == (int)(sizeof(hooked) / sizeof(hooked[0])) ||
        memcmp(pad, pad_of((void *)port_mod_pad_probe), PAD) != 0) {
        fprintf(stderr, "[mods] %s: hook %s: not a game function that can be hooked\n", mod_id, name);
        return 0;
    }
    *original = pad + PAD;
    if (!jump(pad, replacement)) {
        fprintf(stderr, "[mods] %s: hook %s: its code could not be written\n", mod_id, name);
        return 0;
    }
    hooked[hooked_count].pad = pad;
    hooked[hooked_count].target = replacement;
    snprintf(hooked[hooked_count].name, sizeof(hooked[0].name), "%s", name);
    hooked_count++;
    fprintf(stderr, "[mods] %s: hooked %s\n", mod_id, name);
    return 1;
#else
    (void)replacement, (void)original;
    fprintf(stderr, "[mods] %s: hook %s: hooks need an x86-64 build\n", mod_id, name);
    return 0;
#endif
}
