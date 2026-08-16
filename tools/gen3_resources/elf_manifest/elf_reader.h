#ifndef GEN3_ELF_MANIFEST_ELF_READER_H
#define GEN3_ELF_MANIFEST_ELF_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal ELF32 reader for the GBA (ARM) linker output, modeled on the
 * structure definitions in tools/gbafix/elf.h. The extraction generator only
 * needs the symbol table plus each symbol's containing section so it can:
 *   1. reject symbols that are not allocated ROM objects (SHF_ALLOC), and
 *   2. recover the file bytes backing a symbol (sh_offset + st_value - sh_addr).
 */
enum Gen3ElfResult
{
    GEN3_ELF_OK = 0,
    GEN3_ELF_BAD_FILE,    /* too small to be an ELF header, or bad magic */
    GEN3_ELF_BAD_CLASS,   /* not ELFCLASS32 */
    GEN3_ELF_BAD_ENDIAN,  /* not little-endian (ELFDATA2LSB) */
    GEN3_ELF_BAD_MACHINE, /* not EM_ARM (40) */
    GEN3_ELF_NO_SYMTAB,   /* no SHT_SYMTAB section */
    GEN3_ELF_OOB,         /* a header/section/symbol/string read is out of bounds */
};

struct Gen3ElfSymbol
{
    const char *name;   /* points into the owned strtab copy */
    uint32_t value;
    uint32_t size;
    uint8_t info;
    uint16_t shndx;
    bool allocated;     /* containing section has SHF_ALLOC */
};

struct Gen3Elf
{
    uint8_t *owned;              /* owned copy of the file */
    size_t size;
    struct Gen3ElfSection *sections;
    size_t sectionCount;
    struct Gen3ElfSymbol *symbols;
    size_t symbolCount;
    char *strtab;                /* owned copy of the symtab string table */
    size_t strtabSize;
};

struct Gen3ElfSection
{
    uint32_t type;
    uint32_t flags;
    uint32_t addr;
    uint32_t offset;
    uint32_t size;
    uint32_t link;
};

/* Parses and validates the ELF; on success *out owns all buffers. Free with
 * Gen3Elf_Destroy. */
enum Gen3ElfResult Gen3Elf_Open(const uint8_t *data, size_t size, struct Gen3Elf *out);

void Gen3Elf_Destroy(struct Gen3Elf *elf);

/* Locates a symbol by exact name; returns NULL if absent. */
const struct Gen3ElfSymbol *Gen3Elf_FindSymbol(const struct Gen3Elf *elf, const char *name);

/* Recover the file byte range that backs a symbol. Returns false if the symbol
 * is not allocated, or its section placement is out of bounds. */
bool Gen3Elf_SymbolFileRange(const struct Gen3Elf *elf, const struct Gen3ElfSymbol *symbol,
                             size_t *outOffset, size_t *outLength);

#endif
