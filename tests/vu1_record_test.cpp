// Checks port/render/vu1_record.cpp (a native draw's record for programs 0x000 and 0x280, worked out on the CPU)
// against VU1's own set-up: port/platform/vu1_hle.c's and vu1_hle_280.c's, the interpreter's bit for bit (vu1_test). For random draws, the
// fixed block VU1 would have (the camera's world_screen, the part's model, the light directions and colours, the
// point lights) is put in data memory and program 0x000 run on a small batch; the vertex matrix (0x14-0x17), the light
// directions by the model's rotation (vf05-vf07) and the point lights in the part's space (0x4A-0x4D) are compared
// with the record's, bit for bit. Program 0x280 likewise for both matrix sets (the part's model, its parent's):
// 0x14-0x17 and 0x1C-0x1F, 0x18-0x1A and 0x20-0x22, 0x4A-0x4D and 0x53-0x56.
//
//     out/<preset>/vu1_record_test     prints the failures, and exits 1 if any
#include <fenv.h>
#include <stdarg.h>
#include <xmmintrin.h>
#include <stdio.h>
#include <string.h>

#include "../port/render/vu1_record.hpp"

extern "C" {
#include "port/host.h"
#include "port/platform.h"
#include "../port/platform/vu1_internal.h"
}

// ---- What vu1.c and vu1_hle.c call -------------------------------------------------------------------------------
static uint8_t data_mem[16384];
static uint8_t micro_mem[16384];

extern "C" {
PortOptions port_options;
PortStats port_stats;

void *ee_hw_ptr(uint32_t ee_address) {
    return ee_address == 0x11008000 ? static_cast<void *>(micro_mem) : static_cast<void *>(data_mem);
}
void port_gif_path1_kick(uint32_t) {}
size_t port_gif_path1_read(uint32_t, uint8_t *) {
    return 0;
}
uint64_t port_ticks(void) {
    return 0;
}
void port_todo(const char *what, const char *file, int line) {
    printf("todo: %s (%s:%d)\n", what, file, line);
}
void port_fatal(const char *file, int line, const char *fmt, ...) {
    va_list args;
    printf("FATAL (%s:%d): ", file, line);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    exit(1);
}
}

// ---- Random draws ----------------------------------------------------------------------------------------------------
static uint32_t rng_state = 0x2545F491u;

static uint32_t rng() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

// a float of the size the game's matrices and lights have (an exponent about 2^-17 to 2^22), either sign; now and then 0
static float random_float() {
    if (rng() % 16 == 0) {
        return 0.0f;
    }
    uint32_t e = 110 + rng() % 40;
    uint32_t bits = (rng() & 0x807FFFFFu) | e << 23;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void random_matrix(float m[4][4]) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            m[c][r] = random_float();
        }
    }
}

static void put_qword(uint32_t address, const float *v) {
    memcpy(data_mem + address * 16, v, 16);
}

static int failures, checks;

static void compare(const char *what, int trial, const float *got, const float *want, int words) {
    checks++;
    if (memcmp(got, want, size_t(words) * 4) == 0) {
        return;
    }
    failures++;
    if (failures <= 10) {
        printf("FAIL trial %d: %s\n", trial, what);
        for (int i = 0; i < words; i++) {
            uint32_t g, w;
            memcpy(&g, &got[i], 4);
            memcpy(&w, &want[i], 4);
            printf("    [%d] record %08x  VU1 %08x%s\n", i, g, w, g == w ? "" : "  <-");
        }
    }
}

// vu1.c's MXCSR for its GIF kicks (run_csr) is taken when it starts a program: a program of one pair with its E bit
// is run first, in the FPU state the runs below have (the set-up kicks FOGCOL in the middle)
static void start_vu1() {
    const uint32_t unop = 0x2Fu >> 2 << 6 | 0x3C | (0x2F & 3), lnop = 0x80000000u | 0x30u >> 2 << 6 | 0x3C;
    const uint32_t pairs[4] = {lnop, unop | 0x40000000u, lnop, unop};
    memset(micro_mem, 0, sizeof(micro_mem));
    memcpy(micro_mem, pairs, sizeof(pairs));
    port_vu1_reset();
    port_vu1_micro_written();
    port_vu1_start(0, 0, 0);
}

int main() {
    start_vu1();
    for (int trial = 0; trial < 2000; trial++) {
        RdDraw d;
        RdCamera cam;
        memset(&d, 0, sizeof(d));
        memset(&cam, 0, sizeof(cam));
        random_matrix(cam.world_screen);
        random_matrix(d.model);
        random_matrix(d.light_dir);
        random_matrix(d.light_col);
        random_matrix(d.point_pos);
        random_matrix(d.point_col);
        d.point_count = int32_t(rng() % 6);
        Vu1Record k = vu1_record_000(d, cam);

        // VU1's memory as the PS2 path leaves it before the run (render.h: RdDraw's VU1 addresses), and a batch of
        // three vertices of zeros at TOP (only the set-up is looked at)
        memset(data_mem, 0, sizeof(data_mem));
        for (int i = 0; i < 4; i++) {
            put_qword(0x00 + i, cam.world_screen[i]);
            put_qword(0x04 + i, d.model[i]);
            put_qword(0x0C + i, d.light_dir[i]);
            put_qword(0x10 + i, d.light_col[i]);
            put_qword(0x41 + i, d.point_pos[i]);
            put_qword(0x45 + i, d.point_col[i]);
        }
        uint32_t count[4] = {0, 0, 0, uint32_t(d.point_count)};
        memcpy(data_mem + 0x40 * 16, count, 16);
        const uint32_t top = 0x100;
        uint32_t tag[4] = {3, 0, 0, 0};
        memcpy(data_mem + top * 16, tag, 16);
        Vu1State vu;
        memset(&vu, 0, sizeof(vu));
        vu.vf[0][3] = 0x3F800000u; // (vf00: 0, 0, 0, 1)
        // (as vu1.c runs a program: rounding toward zero, denormals read as zero, every exception masked)
        fesetround(FE_TOWARDZERO);
        _mm_setcsr(_mm_getcsr() | 0x1F80u | MXCSR_DAZ);
        uint64_t pairs = vu1_hle_run(&vu, data_mem, 0, top);
        fesetround(FE_TONEAREST);
        if (pairs == 0) {
            printf("FAIL trial %d: the HLE did not take the run\n", trial);
            failures++;
            continue;
        }
        float got[16];
        memcpy(got, data_mem + 0x14 * 16, 64);
        compare("the vertex matrix (0x14-0x17)", trial, k.vertex, got, 16);
        float dirs[12];
        for (int c = 0; c < 3; c++) {
            memcpy(&dirs[c * 4], vu.vf[5 + c], 16);
        }
        compare("the light directions (vf05-vf07)", trial, k.light_dir, dirs, 12);
        float lights[16];
        memcpy(lights, data_mem + 0x4A * 16, 64);
        compare("the point lights (0x4A-0x4D)", trial, k.point_pos, lights, 16);

        // program 0x280: the parent's matrix at 0x08 too, its batch's tag at TOP
        random_matrix(d.parent);
        for (int i = 0; i < 4; i++) {
            put_qword(0x08 + i, d.parent[i]);
        }
        Vu1Record280 k2 = vu1_record_280(d, cam);
        memset(&vu, 0, sizeof(vu));
        vu.vf[0][3] = 0x3F800000u;
        fesetround(FE_TOWARDZERO);
        _mm_setcsr(_mm_getcsr() | 0x1F80u | MXCSR_DAZ);
        pairs = vu1_hle_280_run(&vu, data_mem, 0x280, top);
        fesetround(FE_TONEAREST);
        if (pairs == 0) {
            printf("FAIL trial %d: the HLE did not take the run of program 0x280\n", trial);
            failures++;
            continue;
        }
        const uint32_t vertex_at[2] = {0x14, 0x1C}, dirs_at[2] = {0x18, 0x20}, lights_at[2] = {0x4A, 0x53};
        for (int set = 0; set < 2; set++) {
            memcpy(got, data_mem + vertex_at[set] * 16, 64);
            compare(set ? "0x280: set B's vertex matrix (0x1C-0x1F)" : "0x280: set A's vertex matrix (0x14-0x17)", trial,
                    k2.vertex[set], got, 16);
            memcpy(dirs, data_mem + dirs_at[set] * 16, 48);
            compare(set ? "0x280: set B's light directions (0x20-0x22)" : "0x280: set A's light directions (0x18-0x1A)",
                    trial, k2.light_dir[set], dirs, 12);
            memcpy(lights, data_mem + lights_at[set] * 16, 64);
            compare(set ? "0x280: set B's point lights (0x53-0x56)" : "0x280: set A's point lights (0x4A-0x4D)", trial,
                    &k2.point_pos[4 * set][0], lights, 16);
        }
    }
    printf("%d of %d checks failed\n", failures, checks);
    return failures ? 1 : 0;
}
