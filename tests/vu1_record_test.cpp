// Checks port/render/vu1_record.cpp (a native draw's record for programs 0x000 and 0x280, worked out on the CPU)
// against VU1's own set-up: port/platform/vu1_hle.c's and vu1_hle_280.c's, the interpreter's bit for bit (vu1_test).
// Then the record's fast arithmetic (plain SSE floats, checked) against its exact one (vu1_record_fast false), for the
// records and the clipping of programs 0x280 and 0x700, with operands chosen to trouble it: zeros, denormals, exponent
// 255, sizes whose products overflow or underflow, and places on the guard band's and Z's edges. For random draws, the
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

#include <vector>

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
static void start_vu1();

// ---- The fast arithmetic against the exact ---------------------------------------------------------------------------
static float from_bits(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static uint32_t bits_of(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}

// an operand that troubles the fast way: 0 or -0, a denormal, exponent 255, large or small enough that products
// overflow or underflow, 1; else the game's sizes
static float trouble_float() {
    uint32_t sign = rng() & 0x80000000u;
    switch (rng() % 12) {
    case 0:
        return from_bits(sign);
    case 1:
        return from_bits(sign | (rng() & 0x007FFFFFu)); // (a denormal)
    case 2:
        return from_bits(sign | 0x7F800000u | (rng() & 0x007FFFFFu)); // (exponent 255)
    case 3:
        return from_bits(sign | (230 + rng() % 25) << 23 | (rng() & 0x007FFFFFu)); // (products overflow)
    case 4:
        return from_bits(sign | (1 + rng() % 30) << 23 | (rng() & 0x007FFFFFu)); // (products underflow)
    case 5:
        return from_bits(sign | 0x3F800000u);
    default:
        return random_float();
    }
}

// a coordinate on an edge the clipping tests (with the identity matrix, w 1): x or y about 1 or 4095 (2048 -+ 2047),
// z about 0 (either sign, now and then a denormal): a few units of the last place either side
static float edge_float(float at) {
    if (at == 0.0f) {
        return from_bits((rng() & 0x80000000u) | (rng() % 4 == 0 ? rng() & 0x00FFFFFFu : 0));
    }
    return from_bits(bits_of(at) + (rng() % 9) - 4);
}

static void check_clips(const char *what, int trial, const bool *fast, const bool *exact, uint32_t n) {
    checks++;
    for (uint32_t i = 0; i < n; i++) {
        if (fast[i] != exact[i]) {
            failures++;
            if (failures <= 10) {
                printf("FAIL trial %d: %s, kick %u: fast %d, exact %d\n", trial, what, i, fast[i], exact[i]);
            }
            return;
        }
    }
}

static void check_fast_against_exact() {
    int clipped_seen = 0, unclipped_seen = 0;
    for (int trial = 0; trial < 20000; trial++) {
        RdDraw d;
        RdCamera cam;
        memset(&d, 0, sizeof(d));
        memset(&cam, 0, sizeof(cam));
        bool troubled = trial % 2 == 1;
        for (int i = 0; i < 16; i++) {
            (&cam.world_screen[0][0])[i] = troubled && rng() % 8 == 0 ? trouble_float() : random_float();
            (&d.model[0][0])[i] = troubled && rng() % 8 == 0 ? trouble_float() : random_float();
            (&d.parent[0][0])[i] = troubled && rng() % 8 == 0 ? trouble_float() : random_float();
            (&d.light_dir[0][0])[i] = troubled && rng() % 8 == 0 ? trouble_float() : random_float();
            (&d.point_pos[0][0])[i] = troubled && rng() % 8 == 0 ? trouble_float() : random_float();
        }
        d.point_count = int32_t(rng() % 5);
        vu1_record_fast = true;
        Vu1Record fast = vu1_record_000(d, cam);
        Vu1Record280 fast2 = vu1_record_280(d, cam);
        vu1_record_fast = false;
        Vu1Record exact = vu1_record_000(d, cam);
        Vu1Record280 exact2 = vu1_record_280(d, cam);
        compare("fast against exact: the record", trial, (const float *)&fast, (const float *)&exact,
                int(sizeof(fast) / 4));
        compare("fast against exact: 0x280's record", trial, (const float *)&fast2, (const float *)&exact2,
                int(sizeof(fast2) / 4));

        // a batch of each program, by the record's own matrices or by the identity (w 1) with places on the clipping's
        // edges now and then
        uint32_t n = 3 + rng() % (VU1_BATCH_MOST - 2);
        bool identity = rng() % 2 == 0;
        if (identity) {
            for (int i = 0; i < 16; i++) {
                fast.vertex[i] = (i % 5 == 0) ? 1.0f : (troubled && rng() % 16 == 0 ? trouble_float() : 0.0f);
                fast2.vertex[0][i] = fast.vertex[i];
                fast2.vertex[1][i] = fast.vertex[i];
            }
        }
        static const float edges[] = {1.0f, 4095.0f, 2048.0f};
        std::vector<int32_t> q280(size_t(n) * 32), q700(size_t(n) * 16);
        for (uint32_t i = 0; i < n; i++) {
            for (int j = 0; j < 4; j++) {
                float f;
                if (identity && j < 3 && rng() % 2) {
                    f = edge_float(j == 2 ? 0.0f : edges[rng() % 3]);
                } else if (troubled && rng() % 8 == 0) {
                    f = trouble_float();
                } else if (identity && j == 3) {
                    f = 1.0f;
                } else {
                    f = random_float();
                }
                q280[i * 32 + j] = int32_t(bits_of(f));
                // (0x700's places are integers, about the guard band's edges by the identity; now and then past the 24
                // bits a float holds exactly)
                q700[i * 16 + j] = identity && j == 3 ? 1
                                   : rng() % 32 == 0  ? int32_t(rng())
                                                      : int32_t(rng() % 4200) - 50 + (j == 2 ? -2048 : 0);
            }
            q280[i * 32 + 11] = int32_t(rng() % 2); // (the matrix set: ST's w)
        }
        RdBatch b{};
        b.count = n;
        b.tag[1] = (rng() % 4 == 0) ? uint64_t(1) << 32 : 0;
        b.past_known = rng() % 2 == 0;
        for (int j = 0; j < 4; j++) {
            b.past[j] = q700[j];
        }
        Vu1Kicks280 k280_fast, k280_exact;
        Vu1Kicks700 k700_fast, k700_exact;
        vu1_record_fast = true;
        vu1_kicks_280(q280.data(), b, fast2, k280_fast);
        vu1_kicks_700(q700.data(), b, fast, k700_fast);
        vu1_record_fast = false;
        vu1_kicks_280(q280.data(), b, fast2, k280_exact);
        vu1_kicks_700(q700.data(), b, fast, k700_exact);
        check_clips("fast against exact: 0x280's ADC", trial, k280_fast.adc, k280_exact.adc, n);
        check_clips("fast against exact: 0x280's all clipped", trial, &k280_fast.all_clipped, &k280_exact.all_clipped, 1);
        check_clips("fast against exact: 0x700's ADC", trial, k700_fast.adc, k700_exact.adc, n);
        check_clips("fast against exact: 0x700's kick", trial, &k700_fast.kick, &k700_exact.kick, 1);
        for (uint32_t i = 0; i < n; i++) {
            (k280_exact.adc[i] ? clipped_seen : unclipped_seen)++;
        }
    }
    vu1_record_fast = true;
    // (the batches must have both, or the clipping was not tested)
    checks++;
    if (clipped_seen < 1000 || unclipped_seen < 1000) {
        failures++;
        printf("FAIL: fast against exact: %d kicks with ADC, %d without\n", clipped_seen, unclipped_seen);
    }
}

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
    check_fast_against_exact();
    printf("%d of %d checks failed\n", failures, checks);
    return failures ? 1 : 0;
}
