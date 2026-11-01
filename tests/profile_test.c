/* port/platform/profile.c: the profiler's counters, its hitches kept, and what its report names as
   bounding the frame rate from two readings.

     profile_test */
#include <stdio.h>
#include <string.h>

#include "port/profile.h"

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

static int near(double a, double b) {
    return a - b < 1e-6 && b - a < 1e-6;
}

/* Two readings a second apart: `ms` of each stage in it, and the pictures and vblanks */
static ProfileReport one_second(const double ms[PROFILE_STAGES], unsigned pictures, unsigned vblanks, double shown,
                                double target) {
    ProfileCounters before, after;
    int i;

    memset(&before, 0, sizeof(before));
    before.now_ns = 5000000000u;
    after = before;
    after.now_ns += 1000000000u;
    for (i = 0; i < PROFILE_STAGES; i++) {
        after.ns[i] = (uint64_t)(ms[i] * 1e6);
    }
    after.pictures = pictures;
    after.vblanks = vblanks;
    return port_profile_report(&before, &after, shown, target);
}

static void test_report(void) {
    double ms[PROFILE_STAGES] = {0};
    ProfileReport r;

    ms[PROFILE_GAME] = 300;
    ms[PROFILE_GPU] = 500;
    r = one_second(ms, 170, 60, 168, 170);
    check("keeping up: none", r.bound == PROFILE_BOUND_NONE);
    check("shares", near(r.share[PROFILE_GAME], 0.3) && near(r.share[PROFILE_GPU], 0.5));
    check("GPU ms a picture", near(r.gpu_ms_per_picture, 500.0 / 170.0));
    check("pictures and vblanks a second", near(r.pictures_per_second, 170) && near(r.vblanks_per_second, 60));

    ms[PROFILE_GPU] = 950;
    r = one_second(ms, 120, 60, 120, 170);
    check("GPU 95% busy: the GPU", r.bound == PROFILE_BOUND_GPU && near(r.bound_share, 0.95));

    /* the GS's thread is in its work 100% of the time, but 40% of it waits for the GPU: the GPU */
    ms[PROFILE_GS] = 1000;
    ms[PROFILE_GS_WAIT] = 400;
    r = one_second(ms, 120, 60, 120, 170);
    check("GS waiting is not GS work", r.bound == PROFILE_BOUND_GPU);

    ms[PROFILE_GPU] = 400;
    ms[PROFILE_GS_WAIT] = 50;
    r = one_second(ms, 100, 52, 100, 170);
    check("GS busy: the GS", r.bound == PROFILE_BOUND_GS && near(r.bound_share, 0.95));

    /* both over: the one furthest over its share wins (GPU 0.84/0.80 against the game's 0.88/0.85) */
    memset(ms, 0, sizeof(ms));
    ms[PROFILE_GPU] = 840;
    ms[PROFILE_GAME] = 880;
    r = one_second(ms, 100, 55, 100, 170);
    check("the furthest over wins", r.bound == PROFILE_BOUND_GPU);

    memset(ms, 0, sizeof(ms));
    ms[PROFILE_DMA] = 900;
    r = one_second(ms, 100, 55, 100, 170);
    check("DMA busy: the DMA", r.bound == PROFILE_BOUND_DMA);

    memset(ms, 0, sizeof(ms));
    ms[PROFILE_GPU] = 500;
    ms[PROFILE_GS] = 500;
    r = one_second(ms, 150, 60, 150, 170);
    check("nothing busy, short: bursts", r.bound == PROFILE_BOUND_BURSTS && r.bound_share == 0.0);

    r = one_second(ms, 0, 0, 0, 170);
    check("no pictures: no GPU ms", r.gpu_ms_per_picture == 0.0);

    {
        ProfileCounters same;

        memset(&same, 0, sizeof(same));
        r = port_profile_report(&same, &same, 0, 170);
        check("no time between: empty", r.seconds == 0.0 && r.bound == PROFILE_BOUND_NONE);
    }
    check("bound names", strcmp(port_profile_bound_name(PROFILE_BOUND_GPU), "the GPU") == 0 &&
                             strcmp(port_profile_bound_name((ProfileBound)99), "?") == 0);
}

static void test_counters(void) {
    ProfileCounters a, b;

    a = port_profile_read();
    port_profile_add(PROFILE_GS, 1500);
    port_profile_add(PROFILE_GS, 500);
    port_profile_add((ProfileStage)PROFILE_STAGES, 7); /* (out of range: left out) */
    port_profile_picture();
    port_profile_vblank();
    port_profile_vblank();
    b = port_profile_read();
    check("stage added", b.ns[PROFILE_GS] - a.ns[PROFILE_GS] == 2000);
    check("pictures and vblanks counted", b.pictures - a.pictures == 1 && b.vblanks - a.vblanks == 2);
    check("clock goes on", b.now_ns >= a.now_ns && b.now_ns > 0);
    check("off at the start", !port_profile_on());
    port_profile_set_on(5);
    check("on", port_profile_on() == 1);
}

static void test_hitches(void) {
    char texts[PROFILE_HITCHES][PROFILE_HITCH_TEXT];
    char line[400];
    int i, n;

    check("none at the start", port_profile_hitches(texts, PROFILE_HITCHES) == 0);
    for (i = 0; i < PROFILE_HITCHES + 3; i++) {
        snprintf(line, sizeof(line), "hitch %d", i);
        port_profile_hitch(line);
    }
    n = port_profile_hitches(texts, PROFILE_HITCHES);
    check("the last kept", n == PROFILE_HITCHES);
    check("newest first", strcmp(texts[0], "hitch 10") == 0 && strcmp(texts[PROFILE_HITCHES - 1], "hitch 3") == 0);
    check("fewer asked", port_profile_hitches(texts, 2) == 2 && strcmp(texts[1], "hitch 9") == 0);

    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    port_profile_hitch(line);
    port_profile_hitches(texts, 1);
    check("a long one cut", strlen(texts[0]) == PROFILE_HITCH_TEXT - 1);
}

static void test_compiles(void) {
    ProfileCompiles c = port_profile_compiles();
    uint64_t stage0;

    check("no compiles at the start", c.count == 0 && c.ns == 0 && c.longest_ns == 0);
    port_profile_set_on(0);
    stage0 = port_profile_read().ns[PROFILE_COMPILE];
    port_profile_compile(3000000);
    port_profile_compile(9000000);
    port_profile_compile(1000000);
    c = port_profile_compiles();
    check("counted with the panel closed", c.count == 3 && c.ns == 13000000);
    check("the longest", c.longest_ns == 9000000);
    check("no stage time with the panel closed", port_profile_read().ns[PROFILE_COMPILE] == stage0);
    port_profile_set_on(1);
    port_profile_compile(2000000);
    check("stage time with it open", port_profile_read().ns[PROFILE_COMPILE] == stage0 + 2000000);
    port_profile_set_on(0);
}

int main(void) {
    test_report();
    test_counters();
    test_hitches();
    test_compiles();
    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
