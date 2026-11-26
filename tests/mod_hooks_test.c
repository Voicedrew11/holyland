/* Mods' hooks (port/platform/mod_hooks.c): only listed functions, every call caught, two mods chained, the same mod
   twice refused, and the shares the Mods page shows. Built with the game's hook pad (-fpatchable-function-entry=16)
   and the program's exports, as kf4 is. */
#include <stdio.h>
#include <string.h>

#include "port/mod_api.h"

static int failures;

static void check(const char *what, int ok) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* what the game's code would have: the probe the pad is compared with, the list, and functions to hook */
__attribute__((noinline)) void port_mod_pad_probe(void) {
    __asm__ volatile("");
}
const char *const port_mod_hookable_names[] = {"Test_Listed", 0};

static volatile int base;
__attribute__((noinline)) int Test_Listed(int x) {
    return x + base;
}
__attribute__((noinline)) int Test_Unlisted(int x) {
    return x * 2 + base;
}

/* two mods' replacements: each adds its own amount and calls its original */
static int (*original_a)(int);
static int (*original_b)(int);
static int replaced_a(int x) {
    return original_a(x) + 10;
}
static int replaced_b(int x) {
    return original_b(x) + 100;
}
static int skipping_c(int x) {
    return x + 1000; /* (does not call its original) */
}

int main(void) {
    int (*volatile call)(int) = Test_Listed; /* (through a pointer: as a caller in another file would) */
    void *original_c = NULL;
    PortModHookShare shares[4];

    check("unlisted refused", !port_mod_hook("a", "Test_Unlisted", (void *)replaced_a, (void **)&original_a));
    check("unknown refused", !port_mod_hook("a", "Not_There", (void *)replaced_a, (void **)&original_a));
    check("the game's own before", call(1) == 1 && Test_Listed(1) == 1);

    check("a hooks", port_mod_hook("a", "Test_Listed", (void *)replaced_a, (void **)&original_a));
    check("a's runs, direct and through a pointer", call(1) == 11 && Test_Listed(1) == 11);
    check("not shared yet", port_mod_hooks_shared("a", shares, 4) == 0);

    check("b hooks over a", port_mod_hook("b", "Test_Listed", (void *)replaced_b, (void **)&original_b));
    check("b then a then the game's", call(1) == 111);
    check("a twice refused", !port_mod_hook("a", "Test_Listed", (void *)replaced_a, (void **)&original_a));
    check("still chained after the refusal", call(1) == 111);

    check("a's shares: one", port_mod_hooks_shared("a", shares, 4) == 1);
    check("a shares Test_Listed with b", !strcmp(shares[0].function, "Test_Listed") && !strcmp(shares[0].others, "b"));
    check("b shares with a", port_mod_hooks_shared("b", shares, 4) == 1 && !strcmp(shares[0].others, "a"));
    check("a mod with no hooks shares none", port_mod_hooks_shared("z", shares, 4) == 0);

    check("c hooks and skips the original", port_mod_hook("c", "Test_Listed", (void *)skipping_c, &original_c));
    check("c's alone runs", call(1) == 1001);
    check("c's original is b's", original_c == (void *)replaced_b);
    check("c shares with a and b", port_mod_hooks_shared("c", shares, 4) == 1 && !strcmp(shares[0].others, "a\nb"));

    if (failures == 0) {
        printf("mod hooks: all passed\n");
    }
    return failures != 0;
}
