/* EE addresses for the PC: the 32-bit numbers EEWord holds (port/host.h).

   Below EE_RAM_SIZE an EE address is an offset into game RAM, as on the PS2. The game's own globals
   (its data, its read-only data and its code) live in the program's image, not in game RAM, so they
   get EE addresses too: each section of the program is given a window above EE_STATIC_BASE, in the
   order the host lists them. A pointer anywhere else (the stack, the C heap) has no EE address. */
#ifndef _WIN32
#define _GNU_SOURCE /* dl_iterate_phdr */
#endif
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#endif

#include "port/host.h"
#include "port/platform.h"

#define EE_STATIC_BASE 0x40000000u
#define MAX_REGIONS 16

typedef struct {
    uintptr_t start;
    uintptr_t end;
    uint32_t ee_base;
} Region;

static Region regions[MAX_REGIONS];
static int region_count;
static uint32_t next_ee_base = EE_STATIC_BASE;

static void add_region(uintptr_t start, uintptr_t end) {
    if (region_count == MAX_REGIONS || end <= start) {
        return;
    }
    if ((uint64_t)next_ee_base + (end - start) > 0xFFFFFFFFu) {
        PORT_FATAL("the program's image is too large to give it EE addresses");
    }
    regions[region_count].start = start;
    regions[region_count].end = end;
    regions[region_count].ee_base = next_ee_base;
    region_count++;
    next_ee_base = (uint32_t)((next_ee_base + (end - start) + 0xFFFF) & ~0xFFFFu);
}

#ifdef _WIN32
static void find_sections(void) {
    unsigned char *image = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)image;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(image + dos->e_lfanew);
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    int i;

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        uintptr_t start = (uintptr_t)image + section->VirtualAddress;
        uintptr_t size = section->Misc.VirtualSize;

        add_region(start, start + size);
    }
}
#elif defined(__APPLE__)
/* macOS: the program's own segments, from its Mach-O header (the slide is ASLR's), as Linux lists its PT_LOADs;
   not __PAGEZERO (4 GiB of nothing) nor __LINKEDIT (the loader's tables) */
static void find_sections(void) {
    extern const struct mach_header_64 _mh_execute_header;
    const struct mach_header_64 *mh = &_mh_execute_header;
    const intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    uint32_t i;

    for (i = 0; i < mh->ncmds; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;

        if (lc->cmd != LC_SEGMENT_64 || seg->vmsize == 0 || strcmp(seg->segname, "__PAGEZERO") == 0 ||
            strcmp(seg->segname, "__LINKEDIT") == 0) {
            continue;
        }
        add_region((uintptr_t)(seg->vmaddr + slide), (uintptr_t)(seg->vmaddr + slide + seg->vmsize));
    }
}
#else
static int add_program_segments(struct dl_phdr_info *info, size_t size, void *data) {
    int i;

    (void)size;
    (void)data;
    /* the first object listed is the program itself */
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];

        if (ph->p_type == PT_LOAD) {
            uintptr_t start = info->dlpi_addr + ph->p_vaddr;

            add_region(start, start + ph->p_memsz);
        }
    }
    return 1;
}

static void find_sections(void) {
    dl_iterate_phdr(add_program_segments, NULL);
}
#endif

void port_address_init(void) {
    region_count = 0;
    next_ee_base = EE_STATIC_BASE;
    find_sections();
}

int ee_word_try(const void *ptr, EEWord *word) {
    const unsigned char *p = ptr;
    uintptr_t address = (uintptr_t)ptr;
    int i;

    if (p == NULL) {
        *word = 0;
        return 1;
    }
    if (p >= ee_ram && p < ee_ram + EE_RAM_SIZE) {
        *word = (EEWord)(p - ee_ram);
        return 1;
    }
    if (p >= ee_scratchpad && p < ee_scratchpad + sizeof(ee_scratchpad)) {
        *word = EE_SCRATCHPAD_ADDRESS + (EEWord)(p - ee_scratchpad);
        return 1;
    }
    for (i = 0; i < region_count; i++) {
        if (address >= regions[i].start && address < regions[i].end) {
            *word = regions[i].ee_base + (uint32_t)(address - regions[i].start);
            return 1;
        }
    }
    return 0;
}

EEWord ee_word_of(const void *ptr, const char *file, int line) {
    EEWord word;

    if (ee_word_try(ptr, &word)) {
        return word;
    }
    port_fatal(file, line, "pointer %p is neither in game RAM nor in the program, so it has no EE address", ptr);
}

void *ee_static_ptr_of(EEWord word) {
    static int last;
    int i;

    /* the scratchpad keeps its PS2 address, so DMA tags and registers can name it as on the PS2 */
    if (word - EE_SCRATCHPAD_ADDRESS < sizeof(ee_scratchpad)) {
        return ee_scratchpad + (word - EE_SCRATCHPAD_ADDRESS);
    }

    /* collision and drawing look up the same section (the scratchpad's) again and again */
    if (word >= regions[last].ee_base && word - regions[last].ee_base < regions[last].end - regions[last].start) {
        return (void *)(regions[last].start + (word - regions[last].ee_base));
    }
    for (i = 0; i < region_count; i++) {
        if (word >= regions[i].ee_base && word - regions[i].ee_base < regions[i].end - regions[i].start) {
            last = i;
            return (void *)(regions[i].start + (word - regions[i].ee_base));
        }
    }
    PORT_FATAL("EE address 0x%08X is not one the PC handed out", word);
}
