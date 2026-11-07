/* A profile run: the sampler and the GPU's table together (port/include/port/profile_run.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/debug.h"
#include "port/platform.h"
#include "port/profile.h"
#include "port/profile_run.h"
#include "port/sampler.h"

static int recording;

/* --profile-at's: the vblank to start at, the vblanks to run, the file */
static uint64_t at_vblank, at_vblanks;
static char at_path[1024];
static int at_state; /* 0: none; 1: waiting; 2: running; 3: done */

int port_profile_run_start(void) {
    int sampling;

    port_profile_gpu_reset();
    port_profile_gpu_set_recording(1);
    sampling = sampler_start(0);
    __atomic_store_n(&recording, 1, __ATOMIC_RELAXED);
    return sampling;
}

void port_profile_run_stop(void) {
    sampler_stop();
    port_profile_gpu_set_recording(0);
    __atomic_store_n(&recording, 0, __ATOMIC_RELAXED);
}

int port_profile_run_recording(void) {
    return __atomic_load_n(&recording, __ATOMIC_RELAXED);
}

static int write_gpu(FILE *out) {
    static ProfileGpuRow rows[PROFILE_GPU_ROWS];
    int n = port_profile_gpu_rows(rows, PROFILE_GPU_ROWS), i, ok = 1;

    for (i = 0; i < n; i++) {
        ok &= fprintf(out, "G %llu %llu %s %s\n", (unsigned long long)rows[i].ns, (unsigned long long)rows[i].intervals,
                      rows[i].level == PROFILE_GPU_SHADER ? "shader" : "kind", rows[i].name) > 0;
    }
    ok &= fprintf(out, "P %llu\n", (unsigned long long)port_profile_gpu_pictures()) > 0;
    return ok;
}

int port_profile_run_save(const char *path) {
    FILE *out = fopen(path, "w");
    int ok;

    if (out == NULL) {
        fprintf(stderr, "profile: cannot write %s\n", path);
        return 0;
    }
    ok = (!sampler_available() || sampler_write(out)) && write_gpu(out);
    ok &= fclose(out) == 0;
    fprintf(stderr, ok ? "profile: written to %s\n" : "profile: could not write all of %s\n", path);
    return ok;
}

void port_profile_run_default_path(char *path, int size) {
    port_debug_capture_path("profile", port_vblank_count(), "txt", path, (size_t)(size > 0 ? size : 0));
}

int port_profile_run_at(const char *spec) {
    unsigned long long vblank, seconds;
    char *end;

    vblank = strtoull(spec, &end, 10);
    if (end == spec || *end != ':') {
        return 0;
    }
    spec = end + 1;
    seconds = strtoull(spec, &end, 10);
    if (end == spec || *end != ':' || seconds == 0 || seconds > 24u * 3600u || end[1] == '\0' ||
        strlen(end + 1) >= sizeof(at_path)) {
        return 0;
    }
    at_vblank = vblank;
    at_vblanks = seconds * 60u;
    snprintf(at_path, sizeof(at_path), "%s", end + 1);
    at_state = 1;
    return 1;
}

void port_profile_run_vblank(uint64_t vblank) {
    if (at_state == 1 && vblank >= at_vblank) {
        const char *detail = getenv("KF4_GPU_DETAIL");

        port_profile_gpu_set_detail(detail != NULL && atoi(detail) >= 1);
        if (!port_profile_run_start()) {
            fprintf(stderr, "profile: no function sampler here; the GPU's times only\n");
        }
        at_state = 2;
    } else if (at_state == 2 && vblank >= at_vblank + at_vblanks) {
        port_profile_run_stop();
        port_profile_run_save(at_path);
        at_state = 3;
    }
}
