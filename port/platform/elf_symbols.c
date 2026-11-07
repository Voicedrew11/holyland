/* A module's functions by address, from its ELF file (port/include/port/elf_symbols.h).

   Read: the section headers, the symbol table (.symtab, else .dynsym) and its strings, and .gnu_debugdata (an
   xz-compressed ELF holding a .symtab, read the same way) when built with liblzma (KF4_HAVE_LZMA). The functions
   (STT_FUNC, STT_GNU_IFUNC, defined, at an address) are kept sorted by address, one a start, with their names
   copied: nothing of the file is kept. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>

#ifdef KF4_HAVE_LZMA
#include <lzma.h>
#endif

#include "port/elf_symbols.h"

typedef struct {
    uint64_t start;
    uint64_t size;
    size_t name; /* in the name pool */
} Symbol;

struct ElfSymbols {
    Symbol *symbols;
    size_t count, room;
    char *names;
    size_t names_size, names_room;
};

/* ---- Reading the file's bytes, bounds-checked ---- */
typedef struct {
    const uint8_t *bytes;
    size_t size;
} Bytes;

static int in_bounds(const Bytes *b, uint64_t at, uint64_t length) {
    return at <= b->size && length <= b->size - at;
}

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read64(const uint8_t *p) {
    return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32;
}

typedef struct {
    uint32_t name;
    uint32_t type;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint64_t entry_size;
} Section;

enum { SHT_SYMTAB_ = 2, SHT_DYNSYM_ = 11, SHT_NOBITS_ = 8, STT_FUNC_ = 2, STT_GNU_IFUNC_ = 10 };

typedef struct {
    Bytes file;
    uint64_t section_at;
    uint32_t section_count;
    uint32_t names_section;
} Elf;

static int open_elf(Elf *elf, const uint8_t *bytes, size_t size) {
    static const uint8_t magic[4] = {0x7F, 'E', 'L', 'F'};
    const uint8_t *h = bytes;
    uint16_t entry_size;

    elf->file.bytes = bytes;
    elf->file.size = size;
    if (!in_bounds(&elf->file, 0, 64) || memcmp(h, magic, 4) != 0 || h[4] != 2 /* 64-bit */ ||
        h[5] != 1 /* little-endian */) {
        return 0;
    }
    elf->section_at = read64(h + 0x28);
    entry_size = read16(h + 0x3A);
    elf->section_count = read16(h + 0x3C);
    elf->names_section = read16(h + 0x3E);
    return entry_size == 64 && in_bounds(&elf->file, elf->section_at, (uint64_t)elf->section_count * 64);
}

static int section(const Elf *elf, uint32_t index, Section *s) {
    const uint8_t *p;

    if (index >= elf->section_count) {
        return 0;
    }
    p = elf->file.bytes + elf->section_at + (uint64_t)index * 64;
    s->name = read32(p);
    s->type = read32(p + 4);
    s->offset = read64(p + 0x18);
    s->size = read64(p + 0x20);
    s->link = read32(p + 0x28);
    s->entry_size = read64(p + 0x38);
    return s->type == SHT_NOBITS_ || in_bounds(&elf->file, s->offset, s->size);
}

/* The section named name; 0 when there is none */
static int section_named(const Elf *elf, const char *name, Section *out) {
    Section names;
    size_t length = strlen(name);
    uint32_t i;

    if (!section(elf, elf->names_section, &names) || names.type == SHT_NOBITS_) {
        return 0;
    }
    for (i = 1; i < elf->section_count; i++) {
        if (section(elf, i, out) && out->name < names.size && names.size - out->name > length &&
            memcmp(elf->file.bytes + names.offset + out->name, name, length + 1) == 0) {
            return out->type != SHT_NOBITS_;
        }
    }
    return 0;
}

/* The first section of type; 0 when there is none */
static int section_of_type(const Elf *elf, uint32_t type, Section *out) {
    uint32_t i;

    for (i = 1; i < elf->section_count; i++) {
        if (section(elf, i, out) && out->type == type) {
            return 1;
        }
    }
    return 0;
}

/* ---- The table ---- */
static int grow(void **array, size_t *room, size_t want, size_t item) {
    size_t room_now = *room ? *room : 1024;
    void *bigger;

    if (want <= *room) {
        return 1;
    }
    while (room_now < want) {
        room_now *= 2;
    }
    bigger = realloc(*array, room_now * item);
    if (bigger == NULL) {
        return 0;
    }
    *array = bigger;
    *room = room_now;
    return 1;
}

static int add_symbol(ElfSymbols *s, uint64_t start, uint64_t size, const char *name, size_t length) {
    Symbol *symbol;

    if (!grow((void **)&s->symbols, &s->room, s->count + 1, sizeof(Symbol)) ||
        !grow((void **)&s->names, &s->names_room, s->names_size + length + 1, 1)) {
        return 0;
    }
    symbol = &s->symbols[s->count++];
    symbol->start = start;
    symbol->size = size;
    symbol->name = s->names_size;
    memcpy(s->names + s->names_size, name, length);
    s->names[s->names_size + length] = '\0';
    s->names_size += length + 1;
    return 1;
}

/* The functions of a symbol table section into s; 0 when out of memory */
static int add_table(ElfSymbols *s, const Elf *elf, const Section *table) {
    Section strings;
    const uint8_t *strings_at;
    uint64_t i, count;

    if (table->entry_size != 24 || !section(elf, table->link, &strings) || strings.type == SHT_NOBITS_) {
        return 1;
    }
    strings_at = elf->file.bytes + strings.offset;
    count = table->size / 24;
    for (i = 1; i < count; i++) {
        const uint8_t *sym = elf->file.bytes + table->offset + i * 24;
        uint32_t name = read32(sym);
        unsigned type = sym[4] & 0xF;
        uint16_t section_index = read16(sym + 6);
        uint64_t value = read64(sym + 8), size = read64(sym + 16);
        const uint8_t *end;

        if ((type != STT_FUNC_ && type != STT_GNU_IFUNC_) || section_index == 0 || value == 0 ||
            name >= strings.size) {
            continue;
        }
        end = memchr(strings_at + name, '\0', (size_t)(strings.size - name));
        if (end == NULL || end == strings_at + name) {
            continue;
        }
        if (!add_symbol(s, value, size, (const char *)strings_at + name, (size_t)(end - (strings_at + name)))) {
            return 0;
        }
    }
    return 1;
}

#ifdef KF4_HAVE_LZMA
/* .gnu_debugdata's ELF, decompressed: malloc'd, its size in *size; NULL when there is none or it does not
   decompress */
static uint8_t *debugdata(const Elf *elf, size_t *size) {
    Section packed;
    lzma_stream stream = LZMA_STREAM_INIT;
    uint8_t *out = NULL;
    size_t room = 0;
    lzma_ret r;

    if (!section_named(elf, ".gnu_debugdata", &packed) ||
        lzma_stream_decoder(&stream, UINT64_MAX, 0) != LZMA_OK) {
        return NULL;
    }
    stream.next_in = elf->file.bytes + packed.offset;
    stream.avail_in = (size_t)packed.size;
    do {
        if (stream.avail_out == 0) {
            size_t used = room;

            if (room >= (size_t)256 << 20 || !grow((void **)&out, &room, room ? room * 2 : packed.size * 4, 1)) {
                r = LZMA_MEM_ERROR;
                break;
            }
            stream.next_out = out + used;
            stream.avail_out = room - used;
        }
        r = lzma_code(&stream, LZMA_FINISH);
    } while (r == LZMA_OK);
    *size = room - stream.avail_out;
    lzma_end(&stream);
    if (r != LZMA_STREAM_END) {
        free(out);
        return NULL;
    }
    return out;
}
#endif

static int by_start(const void *a, const void *b) {
    const Symbol *x = a, *y = b;

    if (x->start != y->start) {
        return x->start < y->start ? -1 : 1;
    }
    /* (of aliases, a sized one first, then the earliest named) */
    if ((x->size == 0) != (y->size == 0)) {
        return x->size == 0 ? 1 : -1;
    }
    return x->name < y->name ? -1 : x->name > y->name;
}

/* sorted, one a start */
static void finish(ElfSymbols *s) {
    size_t i, kept = 0;

    qsort(s->symbols, s->count, sizeof(Symbol), by_start);
    for (i = 0; i < s->count; i++) {
        if (kept == 0 || s->symbols[kept - 1].start != s->symbols[i].start) {
            s->symbols[kept++] = s->symbols[i];
        }
    }
    s->count = kept;
}

ElfSymbols *elf_symbols_parse(const uint8_t *bytes, size_t size) {
    ElfSymbols *s;
    Elf elf;
    Section table;
    int ok = 1;

    if (!open_elf(&elf, bytes, size)) {
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    if (section_of_type(&elf, SHT_SYMTAB_, &table) || section_of_type(&elf, SHT_DYNSYM_, &table)) {
        ok = add_table(s, &elf, &table);
    }
#ifdef KF4_HAVE_LZMA
    if (ok) {
        size_t inner_size = 0;
        uint8_t *inner = debugdata(&elf, &inner_size);
        Elf inner_elf;

        if (inner != NULL && open_elf(&inner_elf, inner, inner_size) &&
            section_of_type(&inner_elf, SHT_SYMTAB_, &table)) {
            ok = add_table(s, &inner_elf, &table);
        }
        free(inner);
    }
#endif
    if (!ok || s->count == 0) {
        elf_symbols_free(s);
        return NULL;
    }
    finish(s);
    return s;
}

ElfSymbols *elf_symbols_load(const char *path) {
#ifdef _WIN32
    (void)path;
    return NULL;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    void *bytes;
    ElfSymbols *s;

    if (fd < 0) {
        return NULL;
    }
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    bytes = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (bytes == MAP_FAILED) {
        return NULL;
    }
    s = elf_symbols_parse(bytes, (size_t)st.st_size);
    munmap(bytes, (size_t)st.st_size);
    return s;
#endif
}

void elf_symbols_free(ElfSymbols *symbols) {
    if (symbols == NULL) {
        return;
    }
    free(symbols->symbols);
    free(symbols->names);
    free(symbols);
}

const char *elf_symbols_find(const ElfSymbols *symbols, uint64_t vaddr, uint64_t *start) {
    size_t lo = 0, hi;
    const Symbol *found;

    if (symbols == NULL) {
        return NULL;
    }
    hi = symbols->count;
    /* the last symbol starting at or before vaddr */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (symbols->symbols[mid].start <= vaddr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return NULL;
    }
    found = &symbols->symbols[lo - 1];
    if (found->size != 0 && vaddr - found->start >= found->size) {
        return NULL;
    }
    if (start != NULL) {
        *start = found->start;
    }
    return symbols->names + found->name;
}

size_t elf_symbols_count(const ElfSymbols *symbols) {
    return symbols ? symbols->count : 0;
}
