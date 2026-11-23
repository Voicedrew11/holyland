/* A stand-in libxml2.so.2 for LLVM's release lld (packaging/linux/toolchain.sh).
 *
 * LLVM's release builds lld with libxml2, which only its Windows linker (lld-link) uses, to merge
 * manifests. The ELF linker the game builds with never calls it, but the loader still needs the
 * library to start lld at all, and libxml2 2.14 renamed it libxml2.so.16: Arch keeps the old name only
 * in the separate libxml2-legacy package, so without it the game could not be built ("clang: error:
 * unable to execute command"). lld finds this file first through its run path ($ORIGIN/../lib).
 *
 * Every function stops the linker if it is ever called. The names and versions are the ones lld asks
 * for (`nm -D --undefined-only lld | grep xml`); libxml2.ver gives them their versions. */

#define STOP(name) \
    void name(void) { __builtin_trap(); }

STOP(xmlAddChild)
STOP(xmlCopyNamespace)
STOP(xmlDocDumpFormatMemoryEnc)
STOP(xmlDocGetRootElement)
STOP(xmlDocSetRootElement)
STOP(xmlFreeDoc)
STOP(xmlFreeNode)
STOP(xmlFreeNs)
STOP(xmlNewDoc)
STOP(xmlNewNs)
STOP(xmlNewProp)
STOP(xmlReadMemory)
STOP(xmlSetGenericErrorFunc)
STOP(xmlStrdup)
STOP(xmlUnlinkNode)

/* In libxml2, xmlFree is a variable holding the function that frees, not a function. */
static void free_stop(void *pointer)
{
    (void)pointer;
    __builtin_trap();
}
void (*xmlFree)(void *) = free_stop;
