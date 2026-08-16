/* fixture_build - builds the synthetic ELF32 (EM_ARM) and 16 MiB GBA ROM
 * fixture that backs the checked-in manifest and the standalone test suite.
 *
 * There is no user-owned retail ROM in this environment, so the deterministic
 * extraction manifest is generated against a deterministic fixture: a ROM that
 * carries the real retail header identity (BPEE / "01" / revision 0) and embeds
 * the real Brendan source artifacts at fixed offsets, plus a matching ELF whose
 * symbol table names them gTrainerFrontPic_Brendan / gTrainerPalette_Brendan.
 * Rebuilding the fixture always reproduces the same bytes, so --check passes.
 *
 * Prints the fixture ROM's SHA-1 and SHA-256 to stdout:
 *   rom_sha1 <hex>
 *   rom_sha256 <hex>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "gen3/resources/sha1.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/util.h"

#define ELF_IDENT_OFF 0
#define ELF_TYPE_OFF 16
#define ELF_MACHINE_OFF 18
#define ELF_VERSION_OFF 20
#define ELF_ENTRY_OFF 24
#define ELF_PHOFF_OFF 28
#define ELF_SHOFF_OFF 32
#define ELF_FLAGS_OFF 36
#define ELF_EHSIZE_OFF 40
#define ELF_PHENTSIZE_OFF 42
#define ELF_PHNUM_OFF 44
#define ELF_SHENTSIZE_OFF 46
#define ELF_SHNUM_OFF 48
#define ELF_SHSTRNDX_OFF 50
#define ELF_EHDR_SIZE 52
#define ELF_SHDR_SIZE 40
#define ELF_SYM_SIZE 16

#define SH_TYPE_OFF 4
#define SH_FLAGS_OFF 8
#define SH_ADDR_OFF 12
#define SH_OFFSET_OFF 16
#define SH_SIZE_OFF 20
#define SH_LINK_OFF 24
#define SH_INFO_OFF 28
#define SH_ALIGN_OFF 32
#define SH_ENTSIZE_OFF 36

#define SYM_VALUE_OFF 4
#define SYM_SIZE_OFF 8
#define SYM_INFO_OFF 12
#define SYM_SHNDX_OFF 14

#define SHT_PROGBITS 1u
#define SHT_SYMTAB 2u
#define SHT_STRTAB 3u
#define SHF_ALLOC 0x2u

static void WriteHalf(struct Gen3Buffer *buffer, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)(value >> 8);
    Gen3Buffer_Append(buffer, bytes, sizeof(bytes));
}

static void WriteWord(struct Gen3Buffer *buffer, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);
    Gen3Buffer_Append(buffer, bytes, sizeof(bytes));
}

static size_t Align4(size_t value)
{
    return (value + 3u) & ~(size_t)3u;
}

/* Appends a NUL-terminated string to a buffer, returning its starting offset. */
static size_t AppendString(struct Gen3Buffer *buffer, const char *text)
{
    size_t offset = buffer->length;
    Gen3Buffer_AppendCStr(buffer, text);
    Gen3Buffer_Append(buffer, "", 1);
    return offset;
}

static void WriteSectionHeader(struct Gen3Buffer *buffer, uint32_t name, uint32_t type,
                               uint32_t flags, uint32_t addr, uint32_t offset,
                               uint32_t size, uint32_t link, uint32_t info,
                               uint32_t align, uint32_t entsize)
{
    WriteWord(buffer, name);
    WriteWord(buffer, type);
    WriteWord(buffer, flags);
    WriteWord(buffer, addr);
    WriteWord(buffer, offset);
    WriteWord(buffer, size);
    WriteWord(buffer, link);
    WriteWord(buffer, info);
    WriteWord(buffer, align);
    WriteWord(buffer, entsize);
}

static void WriteSymbol(struct Gen3Buffer *buffer, uint32_t name, uint32_t value,
                        uint32_t size, uint8_t info, uint16_t shndx)
{
    WriteWord(buffer, name);
    WriteWord(buffer, value);
    WriteWord(buffer, size);
    Gen3Buffer_Append(buffer, &info, 1);
    Gen3Buffer_Append(buffer, "\0", 1); /* st_other */
    WriteHalf(buffer, shndx);
}

uint32_t FixtureFrontSectionOffset(void)
{
    return ELF_EHDR_SIZE;
}

uint32_t FixturePaletteSectionOffset(size_t frontSize)
{
    return ELF_EHDR_SIZE + (uint32_t)frontSize;
}

uint32_t FixtureSymtabOffset(size_t frontSize, size_t paletteSize)
{
    return FixturePaletteSectionOffset(frontSize) + (uint32_t)paletteSize;
}

uint32_t FixtureStrtabOffset(size_t frontSize, size_t paletteSize)
{
    return FixtureSymtabOffset(frontSize, paletteSize)
         + FIXTURE_SYMBOL_COUNT * FIXTURE_SYMBOL_TABLE_ENTRY_SIZE;
}

uint32_t FixtureFrontNameOffset(void)
{
    return 1u; /* "\0" then "gTrainerFrontPic_Brendan" */
}

uint32_t FixturePaletteNameOffset(void)
{
    return FixtureFrontNameOffset() + 24u + 1u; /* front name + NUL */
}

bool FixtureBuildElf(const uint8_t *front, size_t frontSize,
                     const uint8_t *palette, size_t paletteSize,
                     struct Gen3Buffer *out)
{
    struct Gen3Buffer shstrtab;
    struct Gen3Buffer strtab;
    size_t frontName;
    size_t paletteName;
    size_t symtabName;
    size_t strtabName;
    size_t shstrtabName;
    size_t nameFront;
    size_t namePalette;
    size_t frontOffset;
    size_t paletteOffset;
    size_t symtabOffset;
    size_t strtabOffset;
    size_t shstrtabOffset;
    size_t sectionHeaderOffset;
    size_t strtabSize;
    size_t shstrtabSize;

    if (!Gen3Buffer_Init(out, 2048)
     || !Gen3Buffer_Init(&shstrtab, 128)
     || !Gen3Buffer_Init(&strtab, 128))
        return false;
    Gen3Buffer_Append(&shstrtab, "\0", 1);
    frontName = AppendString(&shstrtab, ".rodata.brendan_front");
    paletteName = AppendString(&shstrtab, ".rodata.brendan_palette");
    symtabName = AppendString(&shstrtab, ".symtab");
    strtabName = AppendString(&shstrtab, ".strtab");
    shstrtabName = AppendString(&shstrtab, ".shstrtab");

    Gen3Buffer_Append(&strtab, "\0", 1);
    nameFront = AppendString(&strtab, "gTrainerFrontPic_Brendan");
    namePalette = AppendString(&strtab, "gTrainerPalette_Brendan");

    frontOffset = FixtureFrontSectionOffset();
    paletteOffset = FixturePaletteSectionOffset(frontSize);
    symtabOffset = FixtureSymtabOffset(frontSize, paletteSize);
    strtabOffset = FixtureStrtabOffset(frontSize, paletteSize);
    shstrtabOffset = strtabOffset + strtab.length;
    shstrtabSize = shstrtab.length;
    strtabSize = strtab.length;
    sectionHeaderOffset = Align4(shstrtabOffset + shstrtabSize);

    /* ELF header. */
    {
        static const uint8_t ident[16] =
        {
            0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        };
        Gen3Buffer_Append(out, ident, sizeof(ident));
        WriteHalf(out, 2);   /* ET_EXEC */
        WriteHalf(out, 40);  /* EM_ARM */
        WriteWord(out, 1);   /* EV_CURRENT */
        WriteWord(out, 0x08000000u);
        WriteWord(out, 0);   /* e_phoff */
        WriteWord(out, (uint32_t)sectionHeaderOffset);
        WriteWord(out, 0);   /* e_flags */
        WriteHalf(out, ELF_EHDR_SIZE);
        WriteHalf(out, 0);   /* e_phentsize */
        WriteHalf(out, 0);   /* e_phnum */
        WriteHalf(out, ELF_SHDR_SIZE);
        WriteHalf(out, 6);   /* e_shnum */
        WriteHalf(out, 5);   /* e_shstrndx */
    }

    /* Section data. */
    Gen3Buffer_Append(out, front, frontSize);
    Gen3Buffer_Append(out, palette, paletteSize);
    {
        /* .symtab: null + two global object symbols. */
        uint8_t nullSymbol[ELF_SYM_SIZE];
        memset(nullSymbol, 0, sizeof(nullSymbol));
        Gen3Buffer_Append(out, nullSymbol, sizeof(nullSymbol));
        WriteSymbol(out, (uint32_t)nameFront, FIXTURE_FRONT_ADDR,
                    (uint32_t)frontSize, 0x11 /* GLOBAL|OBJECT */, 1);
        WriteSymbol(out, (uint32_t)namePalette, FIXTURE_PALETTE_ADDR,
                    (uint32_t)paletteSize, 0x11, 2);
    }
    Gen3Buffer_Append(out, strtab.data, strtabSize);
    Gen3Buffer_Append(out, shstrtab.data, shstrtabSize);

    /* Section header table. */
    while (out->length < sectionHeaderOffset)
        Gen3Buffer_Append(out, "\0", 1);
    WriteSectionHeader(out, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); /* NULL */
    WriteSectionHeader(out, (uint32_t)frontName, SHT_PROGBITS, SHF_ALLOC,
                       FIXTURE_FRONT_ADDR, (uint32_t)frontOffset,
                       (uint32_t)frontSize, 0, 0, 4, 0);
    WriteSectionHeader(out, (uint32_t)paletteName, SHT_PROGBITS, SHF_ALLOC,
                       FIXTURE_PALETTE_ADDR, (uint32_t)paletteOffset,
                       (uint32_t)paletteSize, 0, 0, 4, 0);
    WriteSectionHeader(out, (uint32_t)symtabName, SHT_SYMTAB, 0, 0,
                       (uint32_t)symtabOffset,
                       FIXTURE_SYMBOL_COUNT * FIXTURE_SYMBOL_TABLE_ENTRY_SIZE,
                       4 /* strtab */, 1 /* first global */, 4, ELF_SYM_SIZE);
    WriteSectionHeader(out, (uint32_t)strtabName, SHT_STRTAB, 0, 0,
                       (uint32_t)strtabOffset, (uint32_t)strtabSize, 0, 0, 1, 0);
    WriteSectionHeader(out, (uint32_t)shstrtabName, SHT_STRTAB, 0, 0,
                       (uint32_t)shstrtabOffset, (uint32_t)shstrtabSize, 0, 0, 1, 0);

    Gen3Buffer_Destroy(&strtab);
    Gen3Buffer_Destroy(&shstrtab);
    return true;
}

void FixtureBuildRom(const uint8_t *front, size_t frontSize,
                     const uint8_t *palette, size_t paletteSize,
                     struct Gen3Buffer *out)
{
    uint8_t zero = 0;
    size_t i;
    unsigned sum = 0;
    if (!Gen3Buffer_Init(out, 16u * 1024u * 1024u))
        return;
    /* zero-fill */
    while (out->length < 16u * 1024u * 1024u)
        Gen3Buffer_Append(out, &zero, 1);

    out->data[0x00] = 0x00;
    out->data[0x01] = 0xC0;
    out->data[0x02] = 0x1F;
    out->data[0x03] = 0xE5;
    memcpy(out->data + 0xA0, "POKEMON EMER", 12);
    memcpy(out->data + 0xAC, "BPEE", 4);
    memcpy(out->data + 0xB0, "01", 2);
    out->data[0xB2] = 0x96;
    out->data[0xB3] = 0x00;
    out->data[0xB4] = 0x00;
    out->data[0xBC] = 0x00; /* revision 0 */
    for (i = 0xA0; i <= 0xBC; i++)
        sum += out->data[i];
    out->data[0xBD] = (uint8_t)((0x100u - (sum & 0xFFu)) & 0xFFu);

    memcpy(out->data + FIXTURE_FRONT_ROM_OFFSET, front, frontSize);
    memcpy(out->data + FIXTURE_PALETTE_ROM_OFFSET, palette, paletteSize);
}

/* ---- multi-resource fixture (R7A §21) -------------------------------- */

/* Address (ELF value) for a resource slot. Front slot f and palette slot p are
 * counted independently so the addresses are pure functions of index. */
static uint32_t FixtureMultiAddr(int kind, uint32_t frontIndex, uint32_t paletteIndex)
{
    if (kind == 1)
        return FIXTURE_MULTI_FRONT_ADDR + frontIndex * FIXTURE_MULTI_FRONT_SLOT;
    return FIXTURE_MULTI_PALETTE_ADDR + paletteIndex * FIXTURE_MULTI_PALETTE_SLOT;
}

bool FixtureBuildElfMulti(const struct FixtureResource *resources, size_t count,
                          struct Gen3Buffer *out)
{
    struct Gen3Buffer strtab;
    struct Gen3Buffer shstrtab;
    size_t *nameOffsets;
    uint32_t *slotAddr;
    size_t payloadSize = 0u;
    size_t symbolCount;
    size_t sectionCount;
    size_t symtabOffset;
    size_t strtabOffset;
    size_t shstrtabOffset;
    size_t sectionHeaderOffset;
    size_t i;
    size_t frontCount = 0u;
    size_t paletteCount = 0u;

    if (resources == NULL || count == 0u || count > FIXTURE_MULTI_MAX_RESOURCES)
        return false;

    nameOffsets = (size_t *)calloc(count, sizeof(nameOffsets[0]));
    slotAddr = (uint32_t *)calloc(count, sizeof(slotAddr[0]));
    if (nameOffsets == NULL || slotAddr == NULL)
    {
        free(nameOffsets);
        free(slotAddr);
        return false;
    }

    if (!Gen3Buffer_Init(out, 2048u)
     || !Gen3Buffer_Init(&strtab, 512u)
     || !Gen3Buffer_Init(&shstrtab, 512u))
    {
        free(nameOffsets);
        free(slotAddr);
        return false;
    }

    Gen3Buffer_Append(&strtab, "\0", 1);
    for (i = 0u; i < count; i++)
    {
        nameOffsets[i] = strtab.length;
        Gen3Buffer_AppendCStr(&strtab, resources[i].symbol);
        Gen3Buffer_Append(&strtab, "", 1);
    }

    Gen3Buffer_Append(&shstrtab, "\0", 1);
    for (i = 0u; i < count; i++)
    {
        char name[32];
        int n = snprintf(name, sizeof(name), ".rodata.res%zu", i);
        if (n < 0 || (size_t)n >= sizeof(name))
        {
            Gen3Buffer_Destroy(&strtab);
            Gen3Buffer_Destroy(&shstrtab);
            free(nameOffsets);
            free(slotAddr);
            return false;
        }
        Gen3Buffer_AppendCStr(&shstrtab, name);
        Gen3Buffer_Append(&shstrtab, "", 1);
    }
    Gen3Buffer_AppendCStr(&shstrtab, ".symtab");
    Gen3Buffer_Append(&shstrtab, "", 1);
    Gen3Buffer_AppendCStr(&shstrtab, ".strtab");
    Gen3Buffer_Append(&shstrtab, "", 1);
    Gen3Buffer_AppendCStr(&shstrtab, ".shstrtab");
    Gen3Buffer_Append(&shstrtab, "", 1);

    /* Slot addresses: front slots and palette slots counted independently. */
    for (i = 0u; i < count; i++)
    {
        if (resources[i].kind == 1)
        {
            slotAddr[i] = FixtureMultiAddr(1, (uint32_t)frontCount, 0u);
            frontCount++;
        }
        else
        {
            slotAddr[i] = FixtureMultiAddr(0, 0u, (uint32_t)paletteCount);
            paletteCount++;
        }
        payloadSize += resources[i].size;
    }

    /* One resource == one payload section and one symbol. The count*2 formula
     * would only hold for the two-leaf Brendan fixture. */
    symbolCount = count + 1u;               /* null + one per resource */
    sectionCount = count + 4u;              /* null + payloads + symtab + strtab + shstrtab */

    /* ELF header. */
    {
        static const uint8_t ident[16] =
        {
            0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        };
        symtabOffset = (size_t)ELF_EHDR_SIZE + payloadSize;
        strtabOffset = symtabOffset + symbolCount * ELF_SYM_SIZE;
        shstrtabOffset = strtabOffset + strtab.length;
        sectionHeaderOffset = Align4(shstrtabOffset + shstrtab.length);

        Gen3Buffer_Append(out, ident, sizeof(ident));
        WriteHalf(out, 2);   /* ET_EXEC */
        WriteHalf(out, 40);  /* EM_ARM */
        WriteWord(out, 1);   /* EV_CURRENT */
        WriteWord(out, 0x08000000u);
        WriteWord(out, 0);   /* e_phoff */
        WriteWord(out, (uint32_t)sectionHeaderOffset);
        WriteWord(out, 0);   /* e_flags */
        WriteHalf(out, ELF_EHDR_SIZE);
        WriteHalf(out, 0);   /* e_phentsize */
        WriteHalf(out, 0);   /* e_phnum */
        WriteHalf(out, ELF_SHDR_SIZE);
        WriteHalf(out, (uint16_t)sectionCount);
        WriteHalf(out, (uint16_t)(sectionCount - 1u)); /* e_shstrndx */
    }

    /* Payload sections (contiguous file offsets; slotted addresses). */
    for (i = 0u; i < count; i++)
        Gen3Buffer_Append(out, resources[i].data, resources[i].size);

    /* .symtab: null symbol then one global object per resource. */
    {
        uint8_t nullSymbol[ELF_SYM_SIZE];
        memset(nullSymbol, 0, sizeof(nullSymbol));
        Gen3Buffer_Append(out, nullSymbol, sizeof(nullSymbol));
        for (i = 0u; i < count; i++)
            WriteSymbol(out, (uint32_t)nameOffsets[i], slotAddr[i],
                        (uint32_t)resources[i].size, 0x11, (uint16_t)(i + 1u));
    }
    Gen3Buffer_Append(out, strtab.data, strtab.length);
    Gen3Buffer_Append(out, shstrtab.data, shstrtab.length);

    /* Section header table: NULL, one per payload, symtab, strtab, shstrtab.
     * shstrtab layout is "\0" + ".rodata.res<0..N-1>" + ".symtab/.strtab/
     * .shstrtab", each NUL-terminated; name offsets are walked sequentially.
     * Payload file offsets are the contiguous ELF_EHDR_SIZE + prefix sum. */
    while (out->length < sectionHeaderOffset)
        Gen3Buffer_Append(out, "\0", 1);
    WriteSectionHeader(out, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); /* NULL */
    {
        size_t nameCursor = 1u;
        for (i = 0u; i < count; i++)
        {
            char name[32];
            size_t payloadOffset = (size_t)ELF_EHDR_SIZE;
            size_t s;
            int n = snprintf(name, sizeof(name), ".rodata.res%zu", i);
            if (n < 0 || (size_t)n >= sizeof(name))
            {
                Gen3Buffer_Destroy(&strtab);
                Gen3Buffer_Destroy(&shstrtab);
                free(nameOffsets);
                free(slotAddr);
                return false;
            }
            for (s = 0u; s < i; s++)
                payloadOffset += resources[s].size;
            WriteSectionHeader(out, (uint32_t)nameCursor, SHT_PROGBITS, SHF_ALLOC,
                               slotAddr[i], (uint32_t)payloadOffset,
                               (uint32_t)resources[i].size, 0, 0, 4, 0);
            nameCursor += strlen(name) + 1u;
        }
        {
            /* Section indices: 0 = null, 1..count = payloads, count+1 = symtab,
             * count+2 = strtab, count+3 = shstrtab. The strtab name offsets walk
             * the fixed ".symtab"/".strtab" NUL-terminated entries. */
            size_t symtabNameCursor = nameCursor;
            size_t strtabNameCursor = symtabNameCursor + sizeof(".symtab");
            size_t shstrtabNameCursor = strtabNameCursor + sizeof(".strtab");
            WriteSectionHeader(out, (uint32_t)symtabNameCursor, SHT_SYMTAB, 0, 0,
                               (uint32_t)symtabOffset,
                               symbolCount * ELF_SYM_SIZE,
                               (uint32_t)(count + 2u) /* strtab index */,
                               1 /* first global */, 4, ELF_SYM_SIZE);
            WriteSectionHeader(out, (uint32_t)strtabNameCursor, SHT_STRTAB, 0, 0,
                               (uint32_t)strtabOffset, (uint32_t)strtab.length, 0, 0, 1, 0);
            WriteSectionHeader(out, (uint32_t)shstrtabNameCursor, SHT_STRTAB, 0, 0,
                               (uint32_t)shstrtabOffset, (uint32_t)shstrtab.length, 0, 0, 1, 0);
        }
    }

    Gen3Buffer_Destroy(&strtab);
    Gen3Buffer_Destroy(&shstrtab);
    free(nameOffsets);
    free(slotAddr);
    return true;
}

void FixtureBuildRomMulti(const struct FixtureResource *resources, size_t count,
                          struct Gen3Buffer *out)
{
    uint8_t zero = 0;
    size_t i;
    unsigned sum = 0;
    size_t frontCount = 0u;
    size_t paletteCount = 0u;
    if (!Gen3Buffer_Init(out, 16u * 1024u * 1024u))
        return;
    while (out->length < 16u * 1024u * 1024u)
        Gen3Buffer_Append(out, &zero, 1);

    out->data[0x00] = 0x00;
    out->data[0x01] = 0xC0;
    out->data[0x02] = 0x1F;
    out->data[0x03] = 0xE5;
    memcpy(out->data + 0xA0, "POKEMON EMER", 12);
    memcpy(out->data + 0xAC, "BPEE", 4);
    memcpy(out->data + 0xB0, "01", 2);
    out->data[0xB2] = 0x96;
    out->data[0xB3] = 0x00;
    out->data[0xB4] = 0x00;
    out->data[0xBC] = 0x00; /* revision 0 */
    for (i = 0xA0; i <= 0xBC; i++)
        sum += out->data[i];
    out->data[0xBD] = (uint8_t)((0x100u - (sum & 0xFFu)) & 0xFFu);

    for (i = 0u; i < count; i++)
    {
        uint32_t romOffset;
        if (resources[i].kind == 1)
        {
            romOffset = FIXTURE_MULTI_FRONT_ROM + (uint32_t)frontCount * FIXTURE_MULTI_FRONT_SLOT;
            frontCount++;
        }
        else
        {
            romOffset = FIXTURE_MULTI_PALETTE_ROM + (uint32_t)paletteCount * FIXTURE_MULTI_PALETTE_SLOT;
            paletteCount++;
        }
        memcpy(out->data + romOffset, resources[i].data, resources[i].size);
    }
}

#ifndef GEN3_FIXTURE_BUILD_NO_MAIN
static void Usage(FILE *stream)
{
    fprintf(stream,
        "usage: fixture_build --elf <out.elf> --rom <out.gba> [--front <lz>] [--palette <lz>]\n"
        "       fixture_build --elf <out.elf> --rom <out.gba> --multi <listfile>\n"
        "  --front/--palette override the two single-resource artifact paths.\n"
        "  --multi reads a list file of 'F <symbol> <path>' (front sheet) and\n"
        "          'P <symbol> <path>' (palette) lines, one resource per line.\n");
}
#endif /* !GEN3_FIXTURE_BUILD_NO_MAIN */

#ifndef GEN3_FIXTURE_BUILD_NO_MAIN
/* Reads a --multi list file into an array of FixtureResource. Buffers are
 * filled in order; symbols are copied into `symbols` so every pointer in
 * `resources` stays valid for the caller's lifetime. Returns the count, or
 * -1 on error (errbuf filled). */
static int ReadMultiList(const char *path, struct FixtureResource *resources,
                         struct Gen3Buffer *buffers, char (*symbols)[256],
                         size_t capacity, char *errbuf, size_t errbufSize)
{
    FILE *file = NULL;
    char line[1024];
    size_t count = 0;

    file = fopen(path, "r");
    if (file == NULL)
    {
        snprintf(errbuf, errbufSize, "cannot open list file '%s'", path);
        return -1;
    }
    while (fgets(line, sizeof(line), file) != NULL)
    {
        char *end = line + strlen(line);
        char kind;
        char symbol[256];
        char artifactPath[1024];
        int consumed;
        while (end > line && (end[-1] == '\n' || end[-1] == '\r'))
            *--end = '\0';
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (count >= capacity)
        {
            snprintf(errbuf, errbufSize, "list file '%s' exceeds %zu resources",
                     path, capacity);
            fclose(file);
            return -1;
        }
        consumed = sscanf(line, " %c %255s %1023s", &kind, symbol, artifactPath);
        if (consumed != 3 || (kind != 'F' && kind != 'P'))
        {
            snprintf(errbuf, errbufSize, "malformed line in list file '%s': %s",
                     path, line);
            fclose(file);
            return -1;
        }
        if (!Gen3Util_ReadFile(artifactPath, &buffers[count], errbuf, errbufSize))
        {
            fclose(file);
            return -1;
        }
        memcpy(symbols[count], symbol, strlen(symbol) + 1u);
        resources[count].symbol = symbols[count];
        resources[count].data = (const uint8_t *)buffers[count].data;
        resources[count].size = buffers[count].length;
        resources[count].kind = (kind == 'F') ? 1 : 0;
        count++;
    }
    fclose(file);
    return (int)count;
}

/* Hash the ROM, write ELF+ROM, and print the fixture digests. Shared by the
 * single-resource and multi-resource paths. */
static int Finish(const char *elfPath, const char *romPath,
                  const struct Gen3Buffer *elf, const struct Gen3Buffer *rom)
{
    struct Gen3Sha1Context sha1Context;
    struct Gen3Sha256Context sha256Context;
    uint8_t sha1Digest[GEN3_SHA1_DIGEST_SIZE];
    uint8_t sha256Digest[32];
    char sha1Hex[GEN3_SHA1_HEX_SIZE];
    char sha256Hex[65];
    char errbuf[512];

    if (rom->length != 16u * 1024u * 1024u)
    {
        fprintf(stderr, "fixture_build: failed to build 16 MiB ROM\n");
        return 1;
    }
    Gen3Sha1_Init(&sha1Context);
    Gen3Sha1_Update(&sha1Context, (const uint8_t *)rom->data, rom->length);
    Gen3Sha1_Final(&sha1Context, sha1Digest);
    Gen3Sha256_Init(&sha256Context);
    Gen3Sha256_Update(&sha256Context, (const uint8_t *)rom->data, rom->length);
    Gen3Sha256_Final(&sha256Context, sha256Digest);
    Gen3Util_FormatHex(sha1Digest, GEN3_SHA1_DIGEST_SIZE, sha1Hex);
    Gen3Util_FormatHex(sha256Digest, 32, sha256Hex);

    if (!Gen3Util_WriteFile(elfPath, elf->data, elf->length, errbuf, sizeof(errbuf))
     || !Gen3Util_WriteFile(romPath, rom->data, rom->length, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "fixture_build: %s\n", errbuf);
        return 1;
    }
    printf("rom_sha1 %s\n", sha1Hex);
    printf("rom_sha256 %s\n", sha256Hex);
    return 0;
}
#endif /* !GEN3_FIXTURE_BUILD_NO_MAIN */

#ifndef GEN3_FIXTURE_BUILD_NO_MAIN
int main(int argc, char **argv)
{
    const char *frontPath = "graphics/trainers/front_pics/brendan.4bpp.lz";
    const char *palettePath = "graphics/trainers/palettes/brendan.gbapal.lz";
    const char *multiPath = NULL;
    const char *elfPath = NULL;
    const char *romPath = NULL;
    struct Gen3Buffer front;
    struct Gen3Buffer palette;
    struct Gen3Buffer elf;
    struct Gen3Buffer rom;
    char errbuf[512];
    int exitCode;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--elf") == 0 && i + 1 < argc)
            elfPath = argv[++i];
        else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc)
            romPath = argv[++i];
        else if (strcmp(argv[i], "--front") == 0 && i + 1 < argc)
            frontPath = argv[++i];
        else if (strcmp(argv[i], "--palette") == 0 && i + 1 < argc)
            palettePath = argv[++i];
        else if (strcmp(argv[i], "--multi") == 0 && i + 1 < argc)
            multiPath = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            Usage(stdout);
            return 0;
        }
        else
        {
            fprintf(stderr, "fixture_build: unknown option '%s'\n", argv[i]);
            Usage(stderr);
            return 2;
        }
    }
    if (elfPath == NULL || romPath == NULL)
    {
        fprintf(stderr, "fixture_build: --elf and --rom are required\n");
        Usage(stderr);
        return 2;
    }

    if (multiPath != NULL)
    {
        struct FixtureResource resources[FIXTURE_MULTI_MAX_RESOURCES];
        struct Gen3Buffer buffers[FIXTURE_MULTI_MAX_RESOURCES];
        char symbolStorage[FIXTURE_MULTI_MAX_RESOURCES][256];
        size_t j;
        int count;
        for (j = 0u; j < FIXTURE_MULTI_MAX_RESOURCES; j++)
            buffers[j].data = NULL;
        count = ReadMultiList(multiPath, resources, buffers, symbolStorage,
                              FIXTURE_MULTI_MAX_RESOURCES,
                              errbuf, sizeof(errbuf));
        if (count < 0)
        {
            fprintf(stderr, "fixture_build: %s\n", errbuf);
            return 1;
        }
        if (!FixtureBuildElfMulti(resources, (size_t)count, &elf))
        {
            fprintf(stderr, "fixture_build: out of memory building ELF\n");
            exitCode = 1;
        }
        else
        {
            FixtureBuildRomMulti(resources, (size_t)count, &rom);
            exitCode = Finish(elfPath, romPath, &elf, &rom);
        }
        for (j = 0u; j < FIXTURE_MULTI_MAX_RESOURCES; j++)
            Gen3Buffer_Destroy(&buffers[j]);
        Gen3Buffer_Destroy(&elf);
        Gen3Buffer_Destroy(&rom);
        return exitCode;
    }

    if (!Gen3Util_ReadFile(frontPath, &front, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "fixture_build: %s\n", errbuf);
        return 1;
    }
    if (!Gen3Util_ReadFile(palettePath, &palette, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "fixture_build: %s\n", errbuf);
        Gen3Buffer_Destroy(&front);
        return 1;
    }

    if (!FixtureBuildElf((const uint8_t *)front.data, front.length,
                         (const uint8_t *)palette.data, palette.length, &elf))
    {
        fprintf(stderr, "fixture_build: out of memory building ELF\n");
        Gen3Buffer_Destroy(&front);
        Gen3Buffer_Destroy(&palette);
        return 1;
    }
    FixtureBuildRom((const uint8_t *)front.data, front.length,
                    (const uint8_t *)palette.data, palette.length, &rom);
    exitCode = Finish(elfPath, romPath, &elf, &rom);

    Gen3Buffer_Destroy(&front);
    Gen3Buffer_Destroy(&palette);
    Gen3Buffer_Destroy(&elf);
    Gen3Buffer_Destroy(&rom);
    return exitCode;
}
#endif /* !GEN3_FIXTURE_BUILD_NO_MAIN */
