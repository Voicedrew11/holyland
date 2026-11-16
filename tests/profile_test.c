/* port/platform/profile.c: the profiler's clock, the pipelines a draw waited for, and the GPU's table by kind of
   work and shader state.

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

static void test_clock(void) {
    uint64_t a = port_profile_now_ns(), b = port_profile_now_ns();

    check("clock goes on", b >= a && a > 0);
}

static void test_compiles(void) {
    ProfileCompiles c = port_profile_compiles();

    check("no compiles at the start", c.count == 0 && c.ns == 0 && c.longest_ns == 0);
    port_profile_compile(3000000);
    port_profile_compile(9000000);
    port_profile_compile(1000000);
    c = port_profile_compiles();
    check("counted", c.count == 3 && c.ns == 13000000);
    check("the longest", c.longest_ns == 9000000);
}

/* The GPU's table: rows by key, most time first, reset, pictures from the reset, "(others)" when full */
static void test_gpu_rows(void) {
    static ProfileGpuRow rows[PROFILE_GPU_ROWS];
    int n, i, others = -1;
    char name[32];

    check("not recording at first", !port_profile_gpu_recording());
    port_profile_gpu_set_recording(1);
    check("recording", port_profile_gpu_recording());
    port_profile_gpu_reset();
    port_profile_picture();
    port_profile_picture();
    check("pictures from the reset", port_profile_gpu_pictures() == 2);
    port_profile_gpu_add(1, "draws", PROFILE_GPU_KIND, 1000, 1);
    port_profile_gpu_add(2, "copies", PROFILE_GPU_KIND, 5000, 1);
    port_profile_gpu_add(1, "draws (a later name is not kept)", PROFILE_GPU_KIND, 3000, 1);
    port_profile_gpu_add(3, "native 280 | blend", PROFILE_GPU_SHADER, 500, 1);
    n = port_profile_gpu_rows(rows, PROFILE_GPU_ROWS);
    check("three rows", n == 3);
    check("most time first", n == 3 && strcmp(rows[0].name, "copies") == 0 && rows[0].ns == 5000 &&
                                 strcmp(rows[1].name, "draws") == 0 && rows[1].ns == 4000 && rows[1].intervals == 2);
    check("a shader row's level", n == 3 && rows[2].level == PROFILE_GPU_SHADER);
    n = port_profile_gpu_rows(rows, 1);
    check("fewer wanted: the most time of all", n == 1 && strcmp(rows[0].name, "copies") == 0);
    port_profile_gpu_reset();
    check("reset: no rows", port_profile_gpu_rows(rows, PROFILE_GPU_ROWS) == 0);
    check("reset: no pictures", port_profile_gpu_pictures() == 0);
    for (i = 0; i < PROFILE_GPU_ROWS + 10; i++) {
        snprintf(name, sizeof(name), "row %d", i);
        port_profile_gpu_add(100 + (uint64_t)i, name, PROFILE_GPU_SHADER, 1, 1);
    }
    n = port_profile_gpu_rows(rows, PROFILE_GPU_ROWS);
    for (i = 0; i < n; i++) {
        if (strcmp(rows[i].name, "(others)") == 0) {
            others = i;
        }
    }
    check("full: every row kept", n == PROFILE_GPU_ROWS);
    check("full: the rest in (others)", others >= 0 && rows[others].ns == 11);
    port_profile_gpu_reset();
    port_profile_gpu_set_recording(0);
}

int main(void) {
    test_gpu_rows();
    test_clock();
    test_compiles();
    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
