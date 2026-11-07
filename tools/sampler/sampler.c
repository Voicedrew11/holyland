/* kf4-sampler: a sampling profiler for a running kf4.exe (Windows only; tools/sampler/profile.py runs the game with
   it, docs/port/PROFILING.md).

     kf4-sampler PID SECONDS INTERVAL_US OUT [DELAY_S]

   After DELAY_S seconds, for SECONDS: every INTERVAL_US microseconds each of the process's threads is suspended, its
   registers read, and resumed. A thread whose RIP is in ntdll or win32u is waiting, and only that frame is kept. Any
   other's stack is copied (up to its base, at most 64 KB) and, the thread running again, unwound from the .pdata of each module it
   goes through (the program, ucrtbase, the Vulkan driver...; each read from its file when a stack first needs it):
   each function's RUNTIME_FUNCTION and its UNWIND_INFO undone (the pushes, the stack allocated, a frame register);
   a function without one is a leaf. Not by frame pointers: Windows clang's RBP frames do not chain, even with
   -fno-omit-frame-pointer. A sample in an epilogue unwinds wrongly (rare), and the walk then stops early.

   OUT has a line "M name path" for each module, "T tid cpu_ms" for each thread (its CPU time while sampled), and
   "K tid count frame..." for each distinct stack, innermost first: the main module's frames as addresses at its
   preferred base (for llvm-symbolizer --obj=kf4.exe), others as "module+0xoffset". tools/sampler/stacks.py reads
   it. */
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 64
typedef struct {
    DWORD tid;
    unsigned n;
    unsigned long long f[MAXF];
    unsigned count;
} Stack;

static Stack *stacks;
static size_t nstacks, room;
#define TABLE (1u << 20)
static unsigned *table;

static void add(DWORD tid, const unsigned long long *f, unsigned n) {
    unsigned long long h = 0xcbf29ce484222325ull ^ tid;
    for (unsigned i = 0; i < n; i++) {
        h = (h ^ f[i]) * 0x100000001b3ull;
    }
    unsigned i = (unsigned)(h >> 20) & (TABLE - 1);
    for (;;) {
        unsigned k = table[i];
        if (k == 0) {
            if (nstacks == room) {
                room = room ? room * 2 : 65536;
                stacks = realloc(stacks, room * sizeof(*stacks));
            }
            Stack *s = &stacks[nstacks];
            s->tid = tid;
            s->n = n;
            memcpy(s->f, f, n * sizeof(*f));
            s->count = 1;
            table[i] = (unsigned)++nstacks;
            return;
        }
        Stack *s = &stacks[k - 1];
        if (s->tid == tid && s->n == n && memcmp(s->f, f, n * sizeof(*f)) == 0) {
            s->count++;
            return;
        }
        i = (i + 1) & (TABLE - 1);
    }
}

/* ---- x64 unwinding from a module's .pdata (read from its file) ---- */
typedef struct {
    DWORD begin, end, unwind;
} RtFunc;
typedef struct {
    int tried;
    unsigned char *image; /* the file */
    IMAGE_SECTION_HEADER *secs;
    int nsecs;
    const RtFunc *rtf; /* NULL: no unwind data, every function a leaf */
    size_t nrtf;
} Image;

static unsigned char *at_rva(const Image *im, DWORD rva) {
    for (int i = 0; i < im->nsecs; i++) {
        const IMAGE_SECTION_HEADER *s = &im->secs[i];
        DWORD va = s->VirtualAddress, size = s->Misc.VirtualSize > s->SizeOfRawData ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (rva >= va && rva < va + size) {
            return im->image + s->PointerToRawData + (rva - va);
        }
    }
    return NULL;
}

static int load_image(Image *im, const char *path) {
    im->tried = 1;
    FILE *fi = fopen(path, "rb");
    if (!fi) {
        return 0;
    }
    fseek(fi, 0, SEEK_END);
    long size = ftell(fi);
    fseek(fi, 0, SEEK_SET);
    im->image = malloc((size_t)size);
    size_t got = fread(im->image, 1, (size_t)size, fi);
    fclose(fi);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)im->image;
    if (got < sizeof(*dos) || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > got) {
        return 0;
    }
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(im->image + dos->e_lfanew);
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }
    im->secs = IMAGE_FIRST_SECTION(nt);
    im->nsecs = nt->FileHeader.NumberOfSections;
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    im->rtf = d.Size ? (const RtFunc *)at_rva(im, d.VirtualAddress) : NULL;
    im->nrtf = im->rtf ? d.Size / sizeof(RtFunc) : 0;
    return im->rtf != NULL;
}

static const RtFunc *find_func(const Image *im, DWORD rva) {
    size_t lo = 0, hi = im->nrtf;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < im->rtf[mid].begin) {
            hi = mid;
        } else if (rva >= im->rtf[mid].end) {
            lo = mid + 1;
        } else {
            return &im->rtf[mid];
        }
    }
    return NULL;
}

static int stack_read(const unsigned char *stack, unsigned long long lo, unsigned long long hi, unsigned long long a,
                      unsigned long long *v) {
    if (a < lo || a + 8 > hi) {
        return 0;
    }
    memcpy(v, stack + (a - lo), 8);
    return 1;
}

/* One frame: from the function at rva of a module, the caller's registers and return address */
static int unwind_one(const Image *im, DWORD rva, unsigned long long *reg, const unsigned char *stack,
                      unsigned long long lo, unsigned long long hi, unsigned long long *ret) {
    const RtFunc *fn = find_func(im, rva);
    unsigned long long v;
    if (fn == NULL) {
        /* a leaf: the return address at the stack's top */
        if (!stack_read(stack, lo, hi, reg[4], ret)) {
            return 0;
        }
        reg[4] += 8;
        return 1;
    }
    int first = 1;
    while (fn) {
        unsigned char *ui = at_rva(im, fn->unwind);
        if (!ui) {
            return 0;
        }
        unsigned flags = ui[0] >> 3, prolog = ui[1], count = ui[2], freg = ui[3] & 15, foff = ui[3] >> 4;
        unsigned in_prolog = first ? rva - fn->begin : 0xFFFFFFFFu;
        const unsigned char *codes = ui + 4;
        for (unsigned i = 0; i < count;) {
            unsigned off = codes[i * 2], op = codes[i * 2 + 1] & 15, info = codes[i * 2 + 1] >> 4;
            unsigned slots = 1;
            int done = !first || in_prolog >= prolog || off <= in_prolog;
            switch (op) {
            case 0: /* PUSH_NONVOL */
                if (done) {
                    if (!stack_read(stack, lo, hi, reg[4], &v)) {
                        return 0;
                    }
                    reg[info] = v;
                    reg[4] += 8;
                }
                break;
            case 1: /* ALLOC_LARGE */
                if (info == 0) {
                    slots = 2;
                    if (done) {
                        reg[4] += (unsigned long long)(*(const unsigned short *)(codes + (i + 1) * 2)) * 8;
                    }
                } else {
                    slots = 3;
                    if (done) {
                        reg[4] += *(const unsigned *)(codes + (i + 1) * 2);
                    }
                }
                break;
            case 2: /* ALLOC_SMALL */
                if (done) {
                    reg[4] += info * 8 + 8;
                }
                break;
            case 3: /* SET_FPREG */
                if (done) {
                    reg[4] = reg[freg] - foff * 16;
                }
                break;
            case 4: /* SAVE_NONVOL */
                slots = 2;
                if (done) {
                    if (!stack_read(stack, lo, hi, reg[4] + (unsigned long long)(*(const unsigned short *)(codes + (i + 1) * 2)) * 8, &v)) {
                        return 0;
                    }
                    reg[info] = v;
                }
                break;
            case 5: /* SAVE_NONVOL_FAR */
                slots = 3;
                if (done) {
                    if (!stack_read(stack, lo, hi, reg[4] + *(const unsigned *)(codes + (i + 1) * 2), &v)) {
                        return 0;
                    }
                    reg[info] = v;
                }
                break;
            case 6: /* EPILOG (v2) */
            case 8: /* SAVE_XMM128 */
                slots = 2;
                break;
            case 7:
            case 9: /* SAVE_XMM128_FAR */
                slots = 3;
                break;
            case 10: /* PUSH_MACHFRAME */
                if (done) {
                    unsigned long long base = reg[4] + (info ? 8 : 0), r, s;
                    if (!stack_read(stack, lo, hi, base, &r) || !stack_read(stack, lo, hi, base + 24, &s)) {
                        return 0;
                    }
                    *ret = r;
                    reg[4] = s;
                    return 1;
                }
                break;
            default:
                return 0;
            }
            i += slots;
        }
        if (flags & 4) { /* CHAININFO */
            unsigned skip = (count + 1) & ~1u;
            fn = (const RtFunc *)(codes + skip * 2);
            first = 0;
        } else {
            fn = NULL;
        }
    }
    if (!stack_read(stack, lo, hi, reg[4], ret)) {
        return 0;
    }
    reg[4] += 8;
    return 1;
}

static HMODULE mods[512];
static MODULEINFO mi[512];
static char names[512][64];
static char paths[512][MAX_PATH];
static Image images[512]; /* each module's file, read when a stack first needs it */
static int nmods;

static int module_of(unsigned long long a) {
    for (int j = 0; j < nmods; j++) {
        unsigned long long b = (unsigned long long)mi[j].lpBaseOfDll;
        if (a >= b && a < b + mi[j].SizeOfImage) {
            return j;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "kf4-sampler PID SECONDS INTERVAL_US OUT [DELAY_S]\n");
        return 2;
    }
    DWORD pid = (DWORD)atoi(argv[1]);
    double seconds = atof(argv[2]);
    int interval = atoi(argv[3]);
    const char *out = argv[4];
    double delay = argc > 5 ? atof(argv[5]) : 0;
    table = calloc(TABLE, sizeof(*table));
    timeBeginPeriod(1);
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        fprintf(stderr, "kf4-sampler: OpenProcess failed %lu\n", GetLastError());
        return 1;
    }
    if (delay > 0) {
        Sleep((DWORD)(delay * 1000));
    }
    DWORD needed = 0;
    if (EnumProcessModulesEx(proc, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
        nmods = (int)(needed / sizeof(HMODULE));
        for (int i = 0; i < nmods; i++) {
            GetModuleInformation(proc, mods[i], &mi[i], sizeof(mi[i]));
            GetModuleBaseNameA(proc, mods[i], names[i], sizeof(names[i]));
            GetModuleFileNameExA(proc, mods[i], paths[i], sizeof(paths[i]));
        }
    }
    /* (the first module is the program) */
    unsigned long long main_base = (unsigned long long)mi[0].lpBaseOfDll, main_end = main_base + mi[0].SizeOfImage;
    if (!load_image(&images[0], paths[0])) {
        fprintf(stderr, "kf4-sampler: no .pdata in %s: only the sampled frames\n", paths[0]);
    }
    unsigned long long pref = main_base;
    {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS64 nt;
        SIZE_T got;
        if (ReadProcessMemory(proc, (void *)main_base, &dos, sizeof(dos), &got) &&
            ReadProcessMemory(proc, (char *)main_base + dos.e_lfanew, &nt, sizeof(nt), &got)) {
            pref = nt.OptionalHeader.ImageBase;
        }
    }
    int idle_mod[512] = {0};
    for (int i = 0; i < nmods; i++) {
        idle_mod[i] = _stricmp(names[i], "ntdll.dll") == 0 || _stricmp(names[i], "win32u.dll") == 0;
    }
    struct {
        DWORD tid;
        HANDLE h;
        FILETIME k0, u0;
    } thr[256];
    int nthr = 0;
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    LONGLONG last_list = -1000000000;
    static unsigned char stack[65536];
    unsigned long long samples = 0, walked = 0;
    for (;;) {
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart >= seconds) {
            break;
        }
        if (now.QuadPart - last_list > freq.QuadPart / 2) {
            last_list = now.QuadPart;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID != pid) {
                        continue;
                    }
                    int found = 0;
                    for (int i = 0; i < nthr; i++) {
                        found |= thr[i].tid == te.th32ThreadID;
                    }
                    if (!found && nthr < 256) {
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                              te.th32ThreadID);
                        if (h) {
                            FILETIME c, e;
                            thr[nthr].tid = te.th32ThreadID;
                            thr[nthr].h = h;
                            GetThreadTimes(h, &c, &e, &thr[nthr].k0, &thr[nthr].u0);
                            nthr++;
                        }
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        for (int i = 0; i < nthr; i++) {
            CONTEXT ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(thr[i].h) == (DWORD)-1) {
                continue;
            }
            int ok = GetThreadContext(thr[i].h, &ctx), m = -1;
            SIZE_T got = 0;
            if (ok) {
                m = module_of(ctx.Rip);
                MEMORY_BASIC_INFORMATION mb;
                if (m >= 0 && !idle_mod[m] && VirtualQueryEx(proc, (void *)ctx.Rsp, &mb, sizeof(mb))) {
                    /* up to the stack's base (the end of its committed pages), or 64 KB */
                    unsigned long long room = (unsigned long long)mb.BaseAddress + mb.RegionSize - ctx.Rsp;
                    if (!ReadProcessMemory(proc, (void *)ctx.Rsp, stack, room < sizeof(stack) ? room : sizeof(stack), &got)) {
                        got = 0;
                    }
                }
            }
            ResumeThread(thr[i].h);
            /* (unwound from the stack's copy, the thread running again) */
            unsigned long long f[MAXF];
            unsigned n = 0;
            if (ok) {
                samples++;
                f[n++] = ctx.Rip;
            }
            if (got) {
                walked++;
                unsigned long long lo = ctx.Rsp, hi = ctx.Rsp + got;
                unsigned long long reg[16] = {ctx.Rax, ctx.Rcx, ctx.Rdx, ctx.Rbx, ctx.Rsp, ctx.Rbp, ctx.Rsi, ctx.Rdi,
                                              ctx.R8,  ctx.R9,  ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15};
                unsigned long long rip = ctx.Rip;
                while (n < MAXF) {
                    if (!images[m].tried) {
                        load_image(&images[m], paths[m]);
                    }
                    unsigned long long sp = reg[4];
                    if (!unwind_one(&images[m], (DWORD)(rip - (unsigned long long)mi[m].lpBaseOfDll), reg, stack, lo, hi,
                                    &rip) ||
                        reg[4] <= sp || (m = module_of(rip)) < 0) {
                        break;
                    }
                    f[n++] = rip;
                }
            }
            if (n) {
                add(thr[i].tid, f, n);
            }
        }
        LARGE_INTEGER until;
        QueryPerformanceCounter(&until);
        LONGLONG target = until.QuadPart + freq.QuadPart * interval / 1000000;
        if (interval >= 1500) {
            Sleep((DWORD)(interval / 1000 - 1));
        }
        do {
            QueryPerformanceCounter(&until);
        } while (until.QuadPart < target);
    }
    FILE *fo = fopen(out, "w");
    if (!fo) {
        fprintf(stderr, "kf4-sampler: cannot write %s\n", out);
        return 1;
    }
    for (int i = 0; i < nmods; i++) {
        fprintf(fo, "M %s %s\n", names[i], paths[i]);
    }
    for (int i = 0; i < nthr; i++) {
        FILETIME c, e, k, u;
        GetThreadTimes(thr[i].h, &c, &e, &k, &u);
        ULARGE_INTEGER a, b, a0, b0;
        a.LowPart = k.dwLowDateTime, a.HighPart = k.dwHighDateTime;
        b.LowPart = u.dwLowDateTime, b.HighPart = u.dwHighDateTime;
        a0.LowPart = thr[i].k0.dwLowDateTime, a0.HighPart = thr[i].k0.dwHighDateTime;
        b0.LowPart = thr[i].u0.dwLowDateTime, b0.HighPart = thr[i].u0.dwHighDateTime;
        fprintf(fo, "T %lu %.1f\n", thr[i].tid, (double)((a.QuadPart - a0.QuadPart) + (b.QuadPart - b0.QuadPart)) / 1e4);
    }
    for (size_t i = 0; i < nstacks; i++) {
        Stack *s = &stacks[i];
        fprintf(fo, "K %lu %u", s->tid, s->count);
        for (unsigned j = 0; j < s->n; j++) {
            unsigned long long a = s->f[j];
            if (a >= main_base && a < main_end) {
                fprintf(fo, " 0x%llx", a - main_base + pref);
            } else {
                int m = module_of(a);
                fprintf(fo, " %s+0x%llx", m >= 0 ? names[m] : "?", m >= 0 ? a - (unsigned long long)mi[m].lpBaseOfDll : a);
            }
        }
        fprintf(fo, "\n");
    }
    fclose(fo);
    fprintf(stderr, "kf4-sampler: %llu samples (%llu walked), %d threads, %zu stacks\n", samples, walked, nthr, nstacks);
    return 0;
}
