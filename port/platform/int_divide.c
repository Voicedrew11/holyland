/* Integer division as the EE does it, where x86 traps (port/include/port/platform.h).

   The EE's DIV never traps: INT_MIN / -1 leaves LO = INT_MIN and HI = 0. x86's IDIV raises #DE for it,
   which would end the program. ee-gcc put no check before a division for that case (the retail program
   has no `break 6`), so on the PS2 it is quietly INT_MIN, remainder 0; this handler gives the same: it
   finds the IDIV that trapped, writes INT_MIN and 0 into its quotient and remainder registers and goes
   on after it (32 and 64 bits).

   A division by zero is different: ee-gcc checks every divisor that is not a constant and executes
   `break 7` for zero (808 such checks in SLUS_203.18), so the PS2 stops there too. That still ends the
   program here, after saying where. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* (glibc's REG_RIP and the rest) */
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port/platform.h"

#if defined(__x86_64__)

/* An IDIV or DIV (F7 /7, /6) at code: its length, whether it is IDIV, its operand size (4 or 8 bytes),
   and where its operand is: a register (reg >= 0) or memory (base and index registers, scale,
   displacement; base -2 for RIP-relative, -1 for none). 0 if it is not one. */
typedef struct {
    int length;
    int is_idiv;
    int size;
    int reg;
    int base, index, scale;
    int32_t disp;
} DivInsn;

static int decode_div(const uint8_t *code, DivInsn *d) {
    int at = 0, rex = 0, modrm, mod, rm, op;

    memset(d, 0, sizeof(*d));
    while (code[at] == 0x2E || code[at] == 0x3E || code[at] == 0x26 || code[at] == 0x36 ||
           code[at] == 0x64 || code[at] == 0x65) {
        at++; /* segment prefixes */
    }
    if ((code[at] & 0xF0) == 0x40) {
        rex = code[at++];
    }
    if (code[at] != 0xF7) {
        return 0;
    }
    at++;
    modrm = code[at++];
    mod = modrm >> 6;
    op = (modrm >> 3) & 7;
    rm = modrm & 7;
    if (op != 6 && op != 7) {
        return 0;
    }
    d->is_idiv = op == 7;
    d->size = (rex & 8) ? 8 : 4;
    d->reg = -1;
    d->base = d->index = -1;
    d->scale = 1;
    if (mod == 3) {
        d->reg = rm | ((rex & 1) << 3);
    } else {
        if (rm == 4) {
            int sib = code[at++];
            int base = sib & 7, index = ((sib >> 3) & 7) | ((rex & 2) << 2);

            d->scale = 1 << (sib >> 6);
            d->index = index == 4 ? -1 : index;
            if (base == 5 && mod == 0) {
                d->base = -1;
                memcpy(&d->disp, code + at, 4);
                at += 4;
            } else {
                d->base = base | ((rex & 1) << 3);
            }
        } else if (rm == 5 && mod == 0) {
            d->base = -2;
            memcpy(&d->disp, code + at, 4);
            at += 4;
        } else {
            d->base = rm | ((rex & 1) << 3);
        }
        if (mod == 1) {
            d->disp = (int8_t)code[at++];
        } else if (mod == 2) {
            memcpy(&d->disp, code + at, 4);
            at += 4;
        }
    }
    d->length = at;
    return 1;
}

/* The divisor, from the registers (in x86's order: rax rcx rdx rbx rsp rbp rsi rdi r8..r15) */
__attribute__((unused)) static int64_t divisor_of(const DivInsn *d, const uint64_t *regs, uint64_t rip) {
    uint64_t address;
    int64_t v = 0;

    if (d->reg >= 0) {
        v = (int64_t)regs[d->reg];
    } else {
        address = d->base == -2 ? rip + (uint64_t)d->length : d->base >= 0 ? regs[d->base] : 0;
        if (d->index >= 0) {
            address += regs[d->index] * (uint64_t)d->scale;
        }
        address += (uint64_t)(int64_t)d->disp;
        memcpy(&v, (const void *)(uintptr_t)address, d->size == 8 ? 8 : 4);
    }
    return d->size == 8 ? v : (int64_t)(int32_t)v;
}

static void report_zero(uint64_t rip) {
    fprintf(stderr,
            "[port] integer division by zero at %p: the PS2 stops at this division too (ee-gcc's divide "
            "check, break 7)\n",
            (void *)(uintptr_t)rip);
}

#ifdef _WIN32
#include <windows.h>

static LONG CALLBACK int_divide_handler(EXCEPTION_POINTERS *info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    CONTEXT *c = info->ContextRecord;
    DivInsn d;

    if (code != EXCEPTION_INT_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!decode_div((const uint8_t *)(uintptr_t)c->Rip, &d)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_INT_OVERFLOW && d.is_idiv) {
        /* INT_MIN / -1: the EE's LO and HI */
        c->Rax = d.size == 8 ? 0x8000000000000000ull : 0x80000000ull;
        c->Rdx = 0;
        c->Rip += (DWORD64)d.length;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    report_zero(c->Rip);
    return EXCEPTION_CONTINUE_SEARCH;
}

void port_int_divide_init(void) {
    AddVectoredExceptionHandler(1, int_divide_handler);
}

#else
#include <signal.h>
#include <ucontext.h>

static void int_divide_handler(int sig, siginfo_t *si, void *context) {
    ucontext_t *uc = context;
    greg_t *g = uc->uc_mcontext.gregs;
    uint64_t regs[16];
    DivInsn d;

    (void)si;
    regs[0] = (uint64_t)g[REG_RAX];
    regs[1] = (uint64_t)g[REG_RCX];
    regs[2] = (uint64_t)g[REG_RDX];
    regs[3] = (uint64_t)g[REG_RBX];
    regs[4] = (uint64_t)g[REG_RSP];
    regs[5] = (uint64_t)g[REG_RBP];
    regs[6] = (uint64_t)g[REG_RSI];
    regs[7] = (uint64_t)g[REG_RDI];
    regs[8] = (uint64_t)g[REG_R8];
    regs[9] = (uint64_t)g[REG_R9];
    regs[10] = (uint64_t)g[REG_R10];
    regs[11] = (uint64_t)g[REG_R11];
    regs[12] = (uint64_t)g[REG_R12];
    regs[13] = (uint64_t)g[REG_R13];
    regs[14] = (uint64_t)g[REG_R14];
    regs[15] = (uint64_t)g[REG_R15];
    if (decode_div((const uint8_t *)(uintptr_t)g[REG_RIP], &d)) {
        if (d.is_idiv && divisor_of(&d, regs, (uint64_t)g[REG_RIP]) == -1) {
            g[REG_RAX] = (greg_t)(d.size == 8 ? 0x8000000000000000ull : 0x80000000ull);
            g[REG_RDX] = 0;
            g[REG_RIP] += d.length;
            return;
        }
        report_zero((uint64_t)g[REG_RIP]);
    }
    /* as if there were no handler: the default action when the instruction runs again */
    signal(sig, SIG_DFL);
}

void port_int_divide_init(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = int_divide_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGFPE, &sa, NULL);
}
#endif

#elif defined(__aarch64__) && defined(__APPLE__)
/* arm64 macOS: the division needs no help but by zero, which the compiler checks before each of the game's divisions
   (CMakeLists.txt: -fsanitize-trap=integer-divide-by-zero) with a BRK of UBSan's divide check, #0x5503. Its SIGTRAP
   says where, as x86's does, and the program ends there. */
#include <signal.h>
#include <sys/ucontext.h>

#define BRK_DIVIDE_CHECK (0xD4200000u | (0x5503u << 5)) /* brk #0x5503 */

static void int_divide_handler(int sig, siginfo_t *si, void *context) {
    ucontext_t *uc = context;
    uint64_t pc = (uint64_t)uc->uc_mcontext->__ss.__pc;
    uint32_t insn;

    (void)si;
    memcpy(&insn, (const void *)(uintptr_t)pc, sizeof(insn));
    if (insn == BRK_DIVIDE_CHECK) {
        fprintf(stderr,
                "[port] integer division by zero at %p: the PS2 stops at this division too (ee-gcc's divide "
                "check, break 7)\n",
                (void *)(uintptr_t)pc);
    }
    /* as if there were no handler: the default action when the instruction runs again */
    signal(sig, SIG_DFL);
}

void port_int_divide_init(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = int_divide_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, NULL);
}
#else
void port_int_divide_init(void) {
}
#endif
