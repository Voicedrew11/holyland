/* port/platform/adaptive_sync.c: the cap under a VRR screen's rate at the common rates, and none for a rate
   not known.

     adaptive_sync_test */
#include <stdio.h>

#include "port/adaptive_sync.h"

static int failed, checked;

static void check_cap(int refresh_hz, int want_hz) {
    int got = adaptive_sync_cap_hz(refresh_hz);

    checked++;
    if (got != want_hz) {
        failed++;
        printf("FAIL %d Hz: cap %d, wanted %d\n", refresh_hz, got, want_hz);
    } else {
        printf("ok   %d Hz: cap %d\n", refresh_hz, got);
    }
}

int main(void) {
    int hz;

    check_cap(0, 0);
    check_cap(-1, 0);
    check_cap(60, 59);
    check_cap(75, 73);
    check_cap(120, 116);
    check_cap(144, 138);
    check_cap(165, 157);
    check_cap(170, 161);
    check_cap(240, 224);
    check_cap(360, 324);
    /* always under the screen's rate and never far under it */
    for (hz = 30; hz <= 360; hz++) {
        int cap = adaptive_sync_cap_hz(hz);

        if (cap >= hz || cap < hz - hz / 10 - 1) {
            failed++;
            printf("FAIL %d Hz: cap %d out of range\n", hz, cap);
        }
        checked++;
    }
    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
