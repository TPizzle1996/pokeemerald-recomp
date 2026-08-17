#include "elf_reader.h"

#include <stdlib.h>
#include <string.h>

#define ELF_NIDENT 16u
#define ELF_MAG0 0x7fu
#define ELFCLASS32 1u
#define ELFDATA2LSB 1u
#define EV_CURRENT 1u
#define ET_EXEC 2u
#define ET_DYN 3u
#define EM_ARM 40u
#define SHT_NULL 0u
#define SHT_SYMTAB 2u
#define SHT_STRTAB 3u
#define SHF_ALLOC (1u << 1)
#define SHN_UNDEF 0u
#define SHN_ABS 0xfff1u
#define SHN_COMMON 0xfff2u
#define ELF_SHDR_SIZE 40u /* sizeof(Elf32_Shdr) on the wire */
#define ELF_SYM_SIZE 16u  /* sizeof(Elf32_Sym) on the wire */
#define ELF32_ST_BIND(info) (((info) >> 4) & 0xfu)
#define STB_GLOBAL 1u

#define GBA_ROM_BASE 0x08000000u

static bool RangeFits(size_t base, size_t length, size_t limit)
{
    return base <= limit && length <= limit - base;
}

static uint16_t ReadHalf(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t ReadWord(const uint8_t *bytes)
{
    return (uint32_t)bytes[0]
         | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16)
         | ((uint32_t)bytes[3] << 24);
}

enum Gen3ElfResult Gen3Elf_Open(const uint8_t *data, size_t size, struct Gen3Elf *out)
{
    const uint8_t *ehdr;
    struct Gen3ElfSection *sections = NULL;
    size_t sectionCount = 0;
    size_t symtabIndex = 0;
    int symtabFound = 0;
    size_t i;
    enum Gen3ElfResult result = GEN3_ELF_BAD_FILE;

    if (out == NULL)
        return GEN3_ELF_BAD_FILE;
    memset(out, 0, sizeof(*out));
    if (data == NULL || size < 52u)
        return GEN3_ELF_BAD_FILE;
    ehdr = data;
    if (ehdr[0] != ELF_MAG0 || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F')
        return GEN3_ELF_BAD_FILE;
    if (ehdr[4] != ELFCLASS32)
        return GEN3_ELF_BAD_CLASS;
    if (ehdr[5] != ELFDATA2LSB)
        return GEN3_ELF_BAD_ENDIAN;
    if (ehdr[6] != EV_CURRENT)
        return GEN3_ELF_BAD_FILE;
    if (ReadHalf(ehdr + 16) != ET_EXEC && ReadHalf(ehdr + 16) != ET_DYN)
        return GEN3_ELF_BAD_FILE;
    if (ReadHalf(ehdr + 18) != EM_ARM)
        return GEN3_ELF_BAD_MACHINE;
    if (ReadHalf(ehdr + 46) != 40u) /* e_shentsize (e_ehsize is at offset 40) */
        return GEN3_ELF_BAD_FILE;

    {
        uint32_t shoff = ReadWord(ehdr + 32);
        uint32_t shnum = ReadHalf(ehdr + 48);
        size_t tableBytes;
        if (shnum == 0 || shoff == 0)
            return GEN3_ELF_NO_SYMTAB;
        /* shnum is a uint16 from the ELF header; no overflow on any host. */
        tableBytes = (size_t)shnum * ELF_SHDR_SIZE;
        if (!RangeFits(shoff, tableBytes, size))
            return GEN3_ELF_OOB;
        sectionCount = (size_t)shnum;
        sections = calloc((size_t)shnum, sizeof(*sections));
        if (sections == NULL)
            return GEN3_ELF_BAD_FILE;
        for (i = 0; i < (size_t)shnum; i++)
        {
            const uint8_t *shdr = data + (size_t)shoff + i * ELF_SHDR_SIZE;
            sections[i].type = ReadWord(shdr + 4);
            sections[i].flags = ReadWord(shdr + 8);
            sections[i].addr = ReadWord(shdr + 12);
            sections[i].offset = ReadWord(shdr + 16);
            sections[i].size = ReadWord(shdr + 20);
            sections[i].link = ReadWord(shdr + 24);
            if (!RangeFits(sections[i].offset, sections[i].size, size))
            {
                result = GEN3_ELF_OOB;
                goto fail;
            }
            if (sections[i].type == SHT_SYMTAB && !symtabFound)
            {
                symtabIndex = i;
                symtabFound = 1;
            }
        }
    }

    if (!symtabFound)
    {
        result = GEN3_ELF_NO_SYMTAB;
        goto fail;
    }
    else
    {
        const struct Gen3ElfSection *symtab = &sections[symtabIndex];
        const struct Gen3ElfSection *strtab;
        size_t symbolCount;
        size_t strtabSize;
        char *strtabCopy;
        struct Gen3ElfSymbol *symbols;
        size_t j;
        uint32_t entsize = 16u; /* Elf32_Sym */
        if (symtab->size % entsize != 0)
        {
            result = GEN3_ELF_OOB;
            goto fail;
        }
        if (symtab->link == 0 || symtab->link >= sectionCount)
        {
            result = GEN3_ELF_NO_SYMTAB;
            goto fail;
        }
        strtab = &sections[symtab->link];
        if (strtab->type != SHT_STRTAB)
        {
            result = GEN3_ELF_NO_SYMTAB;
            goto fail;
        }
        symbolCount = (size_t)(symtab->size / entsize);
        strtabSize = strtab->size;
        if (symbolCount == 0 || strtabSize == 0)
        {
            result = GEN3_ELF_NO_SYMTAB;
            goto fail;
        }
        if (symbolCount > SIZE_MAX / sizeof(*symbols))
        {
            result = GEN3_ELF_OOB;
            goto fail;
        }
        strtabCopy = malloc(strtabSize);
        symbols = calloc(symbolCount, sizeof(*symbols));
        if (strtabCopy == NULL || symbols == NULL)
        {
            free(strtabCopy);
            free(symbols);
            result = GEN3_ELF_BAD_FILE;
            goto fail;
        }
        memcpy(strtabCopy, data + strtab->offset, strtabSize);
        for (j = 0; j < symbolCount; j++)
        {
            const uint8_t *sym = data + symtab->offset + j * entsize;
            uint32_t nameOffset = ReadWord(sym);
            uint32_t value = ReadWord(sym + 4);
            uint32_t symSize = ReadWord(sym + 8);
            uint8_t info = sym[12];
            uint16_t shndx = ReadHalf(sym + 14);
            if (nameOffset >= strtabSize)
            {
                free(strtabCopy);
                free(symbols);
                result = GEN3_ELF_OOB;
                goto fail;
            }
            symbols[j].name = strtabCopy + nameOffset;
            symbols[j].value = value;
            symbols[j].size = symSize;
            symbols[j].info = info;
            symbols[j].shndx = shndx;
            symbols[j].allocated = (shndx < (uint16_t)sectionCount)
                && (sections[shndx].flags & SHF_ALLOC) != 0
                && shndx != SHN_ABS && shndx != SHN_COMMON && shndx != 0;
        }
        out->sections = sections;
        out->sectionCount = sectionCount;
        out->symbols = symbols;
        out->symbolCount = symbolCount;
        out->strtab = strtabCopy;
        out->strtabSize = strtabSize;
    }

    out->owned = malloc(size);
    if (out->owned == NULL)
    {
        Gen3Elf_Destroy(out);
        return GEN3_ELF_BAD_FILE;
    }
    memcpy(out->owned, data, size);
    out->size = size;
    return GEN3_ELF_OK;

fail:
    free(sections);
    return result;
}

void Gen3Elf_Destroy(struct Gen3Elf *elf)
{
    if (elf == NULL)
        return;
    free(elf->owned);
    free(elf->sections);
    free(elf->symbols);
    free(elf->strtab);
    memset(elf, 0, sizeof(*elf));
}

const struct Gen3ElfSymbol *Gen3Elf_FindSymbol(const struct Gen3Elf *elf, const char *name)
{
    size_t i;
    if (elf == NULL || name == NULL)
        return NULL;
    for (i = 0; i < elf->symbolCount; i++)
    {
        const struct Gen3ElfSymbol *symbol = &elf->symbols[i];
        if (symbol->name != NULL && strcmp(symbol->name, name) == 0)
            return symbol;
    }
    return NULL;
}

bool Gen3Elf_SymbolFileRange(const struct Gen3Elf *elf, const struct Gen3ElfSymbol *symbol,
                             size_t *outOffset, size_t *outLength)
{
    size_t relative;
    size_t offset;
    size_t length;
    const struct Gen3ElfSection *section;
    if (elf == NULL || symbol == NULL || outOffset == NULL || outLength == NULL)
        return false;
    *outOffset = 0;
    *outLength = 0;
    if (!symbol->allocated)
        return false;
    if (symbol->value < GBA_ROM_BASE)
        return false;
    section = &elf->sections[symbol->shndx];
    if (symbol->value < section->addr)
        return false;
    relative = (size_t)(symbol->value - section->addr);
    if (relative >= section->size)
        return false;
    offset = (size_t)section->offset + relative;
    if (symbol->size != 0)
    {
        length = symbol->size;
        if (length > section->size - relative)
            return false;
    }
    else
    {
        /* Assembler .incbin data labels (tileset graphics, metatile, and
         * palette blobs) carry no size in the symtab (NOTYPE size-0). For
         * these, *outLength is the maximum contiguous bytes available in the
         * containing section; the caller supplies the real length and must
         * validate it against the bound. A size-0 OBJECT symbol is malformed
         * (a real C object always has a size) and stays rejected. */
        if ((symbol->info & 0x0Fu) != 0u) /* STT_NOTYPE = 0 */
            return false;
        length = section->size - relative;
    }
    if (!RangeFits(offset, length, elf->size))
        return false;
    *outOffset = offset;
    *outLength = length;
    return true;
}
