/* A module's functions by address, from its ELF file (port/include/port/sampler.h names the frames with it):
   the symbol table (.symtab), else the dynamic one (.dynsym); and, built with liblzma, the symbol table a
   distribution's stripped library keeps compressed in .gnu_debugdata (MiniDebugInfo), which names its static
   functions too.

   port/platform/elf_symbols.c. Everything read from the file is bounds-checked: a file that is not a 64-bit
   little-endian ELF, or is cut short, gives no table, not a crash. */
#ifndef PORT_ELF_SYMBOLS_H
#define PORT_ELF_SYMBOLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ElfSymbols ElfSymbols;

/* The file's functions; NULL when it cannot be read or holds none */
ElfSymbols *elf_symbols_load(const char *path);
/* The same from the file's bytes (size of them), which are not kept */
ElfSymbols *elf_symbols_parse(const uint8_t *bytes, size_t size);
void elf_symbols_free(ElfSymbols *symbols);

/* The function holding ELF address vaddr (the address less the module's load bias): its name, its start in
   *start (when not NULL). NULL when no function holds it: past a sized symbol's end, or before the first. */
const char *elf_symbols_find(const ElfSymbols *symbols, uint64_t vaddr, uint64_t *start);
size_t elf_symbols_count(const ElfSymbols *symbols);

#ifdef __cplusplus
}
#endif

#endif
