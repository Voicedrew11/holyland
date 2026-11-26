/* Hooks on the game's functions for mods (port/include/port/mod_api.h).

   The game's code is compiled with -fpatchable-function-entry=16 (CMakeLists.txt): every function of it starts with
   16 bytes of no-ops. A hook writes over them a jump to the mod's replacement (jmp [rip+0] and the 8-byte address:
   14 bytes); what calls the function as it was is the code just after them. A second mod's hook on the same function
   jumps to its own replacement, whose "original" is the first mod's. A function the compiler inlined into its
   callers, or one that is not the game's (no pad), cannot be hooked: the pad is checked against port_mod_pad_probe's,
   a function compiled the same way (game/mod_events.c). x86-64 only.

   Only the functions in modding/hookable.txt are hooked: those are built never to be inlined or copied
   (cmake/mod_hookable.cmake), so the hook catches every call. Another game function could be inlined into some of
   its callers and not others, and a hook on it would quietly miss those calls.

   Two mods may hook one function: they chain, the later mod's replacement first, its original the earlier mod's.
   Both work when each calls its original; a mod that does not cuts out the mods hooked before it. That is often
   meant (a mod replacing what the game does), so a shared hook is not refused: it is said in the log and on the Mods
   page (port_mod_hooks_shared), so a player whose mods misbehave together can see which ones touch the same thing.

   Threads: hooks are made on the game's thread (hl_mod_init, HL_EVENT_START), and the Mods page reads who shares
   them on the menus' thread: the list is behind `lock`. */
#include <pthread.h>
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
extern const char *const port_mod_hookable_names[]; /* modding/hookable.txt (mod_hookable_names.c, written at configure) */

static int hookable(const char *name) {
    int i;

    for (i = 0; port_mod_hookable_names[i] != NULL; i++) {
        if (strcmp(port_mod_hookable_names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct {
    unsigned char *pad;
    void *target;
    char name[96];
    int mod_count;
    char mods[8][64]; /* the mods that hooked it, in order (the last one's replacement runs first) */
} Hooked;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
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

static int hooked_by(const Hooked *h, const char *mod_id) {
    int i;

    for (i = 0; i < h->mod_count; i++) {
        if (strcmp(h->mods[i], mod_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_mod(Hooked *h, const char *mod_id) {
    if (h->mod_count < (int)(sizeof(h->mods) / sizeof(h->mods[0]))) {
        snprintf(h->mods[h->mod_count++], sizeof(h->mods[0]), "%s", mod_id);
    }
}

/* the function hooked before, by another mod: chained (under lock) */
static int hook_again(Hooked *h, const char *mod_id, void *replacement, void **original) {
    char others[256] = "";
    int i;

    if (hooked_by(h, mod_id)) { /* (its original would be its own replacement: a call to itself, for ever) */
        fprintf(stderr, "[mods] %s: hook %s: hooked by this mod already\n", mod_id, h->name);
        return 0;
    }
    if (!jump(h->pad, replacement)) {
        fprintf(stderr, "[mods] %s: hook %s: its code could not be written\n", mod_id, h->name);
        return 0;
    }
    *original = h->target;
    h->target = replacement;
    for (i = 0; i < h->mod_count; i++) {
        snprintf(others + strlen(others), sizeof(others) - strlen(others), "%s%s", i ? ", " : "", h->mods[i]);
    }
    add_mod(h, mod_id);
    fprintf(stderr, "[mods] %s: hooked %s, which %s hooked too: %s runs first, and the others run only if it calls "
                    "the original\n", mod_id, h->name, others, mod_id);
    return 1;
}

int port_mod_hook(const char *mod_id, const char *name, void *replacement, void **original) {
#if defined(__x86_64__) || defined(_M_X64)
    unsigned char *function, *pad;
    Hooked *h;
    int i, done = 0;

    if (replacement == NULL || original == NULL || name == NULL) {
        return 0;
    }
    if (!hookable(name)) {
        fprintf(stderr, "[mods] %s: hook %s: not in modding/hookable.txt, the game functions a hook catches every call "
                        "to; ask for it to be added\n", mod_id, name);
        return 0;
    }
    function = port_mod_symbol(name);
    if (function == NULL) {
        fprintf(stderr, "[mods] %s: hook %s: no such function (or not exported)\n", mod_id, name);
        return 0;
    }
    pad = pad_of(function);
    pthread_mutex_lock(&lock);
    for (i = 0; i < hooked_count; i++) {
        if (hooked[i].pad == pad) {
            done = hook_again(&hooked[i], mod_id, replacement, original);
            pthread_mutex_unlock(&lock);
            return done;
        }
    }
    if (hooked_count == (int)(sizeof(hooked) / sizeof(hooked[0])) ||
        memcmp(pad, pad_of((void *)port_mod_pad_probe), PAD) != 0) {
        fprintf(stderr, "[mods] %s: hook %s: not a game function that can be hooked\n", mod_id, name);
    } else if (!jump(pad, replacement)) {
        fprintf(stderr, "[mods] %s: hook %s: its code could not be written\n", mod_id, name);
    } else {
        h = &hooked[hooked_count++];
        memset(h, 0, sizeof(*h));
        h->pad = pad;
        h->target = replacement;
        snprintf(h->name, sizeof(h->name), "%s", name);
        add_mod(h, mod_id);
        *original = pad + PAD;
        fprintf(stderr, "[mods] %s: hooked %s\n", mod_id, name);
        done = 1;
    }
    pthread_mutex_unlock(&lock);
    return done;
#else
    (void)replacement, (void)original;
    fprintf(stderr, "[mods] %s: hook %s: hooks need an x86-64 build\n", mod_id, name);
    return 0;
#endif
}

int port_mod_hooks_shared(const char *mod_id, PortModHookShare *out, int max) {
    int i, j, count = 0;

    pthread_mutex_lock(&lock);
    for (i = 0; i < hooked_count && count < max; i++) {
        const Hooked *h = &hooked[i];
        size_t used = 0;

        if (h->mod_count < 2 || !hooked_by(h, mod_id)) {
            continue;
        }
        snprintf(out[count].function, sizeof(out[count].function), "%s", h->name);
        out[count].others[0] = '\0';
        for (j = 0; j < h->mod_count; j++) {
            if (strcmp(h->mods[j], mod_id) != 0 && used < sizeof(out[count].others)) {
                used += (size_t)snprintf(out[count].others + used, sizeof(out[count].others) - used, "%s%s",
                                         used ? "\n" : "", h->mods[j]);
            }
        }
        count++;
    }
    pthread_mutex_unlock(&lock);
    return count;
}
