#include "manifest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf_reader.h"
#include "gen3/resources/lz77.h"
#include "gen3/resources/sha1.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/sha256.h"

#define GBA_HEADER_GAME_CODE 0xAC
#define GBA_HEADER_MAKER_CODE 0xB0
#define GBA_HEADER_REVISION 0xBC

/* ---- error plumbing ----------------------------------------------------- */

static void SetError(char *errbuf, size_t errbufSize, const char *format, ...)
{
    va_list args;
    int needed;
    if (errbuf == NULL || errbufSize == 0)
        return;
    va_start(args, format);
    needed = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (needed < 0)
    {
        errbuf[0] = '\0';
        return;
    }
    if ((size_t)needed >= errbufSize)
        needed = (int)errbufSize - 1;
    va_start(args, format);
    vsnprintf(errbuf, (size_t)needed + 1u, format, args);
    va_end(args);
}

/* ---- digest helpers ----------------------------------------------------- */

static void Sha1Of(const uint8_t *data, size_t size, uint8_t digest[20])
{
    struct Gen3Sha1Context context;
    Gen3Sha1_Init(&context);
    Gen3Sha1_Update(&context, data, size);
    Gen3Sha1_Final(&context, digest);
}

static void Sha256Of(const uint8_t *data, size_t size, uint8_t digest[32])
{
    struct Gen3Sha256Context context;
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, data, size);
    Gen3Sha256_Final(&context, digest);
}

static int HexNibble(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static bool ParseHex(const char *hex, uint8_t *out, size_t size)
{
    size_t i;
    if (hex == NULL || out == NULL)
        return false;
    if (strlen(hex) != size * 2u)
        return false;
    for (i = 0; i < size; i++)
    {
        int high = HexNibble(hex[i * 2u]);
        int low = HexNibble(hex[i * 2u + 1u]);
        if (high < 0 || low < 0)
            return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

/* ---- ROM identity ------------------------------------------------------- */

static bool CheckRomIdentity(const uint8_t *rom, size_t romSize,
                             const struct Gen3ManifestConfig *config,
                             const char **badField)
{
    const char *gameCode = (config != NULL && config->romGameCode != NULL) ? config->romGameCode : "BPEE";
    const char *makerCode = (config != NULL && config->romMakerCode != NULL) ? config->romMakerCode : "01";
    unsigned revision = config != NULL ? config->romRevision : 0;
    if (romSize != GEN3_ROM_SIZE)
    {
        *badField = "size";
        return false;
    }
    if (strlen(gameCode) != 4 || memcmp(rom + GBA_HEADER_GAME_CODE, gameCode, 4) != 0)
    {
        *badField = "game code";
        return false;
    }
    if (strlen(makerCode) != 2 || memcmp(rom + GBA_HEADER_MAKER_CODE, makerCode, 2) != 0)
    {
        *badField = "maker code";
        return false;
    }
    if (rom[GBA_HEADER_REVISION] != (uint8_t)revision)
    {
        *badField = "software revision";
        return false;
    }
    return true;
}

/* ---- catalog helpers ---------------------------------------------------- */

static const struct Gen3CatalogEntry *FindCatalogEntry(
    const struct Gen3CatalogEntry *catalog, size_t catalogCount, const char *id)
{
    size_t i;
    for (i = 0; i < catalogCount; i++)
    {
        if (strcmp(catalog[i].id, id) == 0)
            return &catalog[i];
    }
    return NULL;
}

static bool TypeCompatibleWithRepresentation(const char *catalogType, int schema,
                                             const char *canonicalRepresentation)
{
    if (strcmp(catalogType, "tile-graphics") == 0
     || strcmp(catalogType, "sprite-sheet") == 0)
        return strcmp(canonicalRepresentation, "gba-4bpp-tiles") == 0
            || strcmp(canonicalRepresentation, "gba-1bpp-tiles") == 0;
    if (strcmp(catalogType, "palette") == 0)
        return strcmp(canonicalRepresentation, "gba-bgr555-palette") == 0;
    if (strcmp(catalogType, "tileset") == 0)
        return strcmp(canonicalRepresentation, "gba-metatile-defs") == 0
            || strcmp(canonicalRepresentation, "gba-metatile-attributes") == 0;
    /* R11-D: raw little-endian u16 tilemap entries (map blockdata and the
     * 2x2 border words) carry no compression on either target. */
    if (strcmp(catalogType, "tilemap") == 0)
        return strcmp(canonicalRepresentation, "gba-tilemap") == 0;
    /* R12-A: audio families (approved architecture, R12 §2). Each type has
     * exactly the approved (schema, representation) pairs; anything else --
     * wrong schema, wrong representation, cross-type mixes -- fails closed. */
    if (strcmp(catalogType, "audio-sample") == 0)
        return schema == 1
            && (strcmp(canonicalRepresentation, "gba-wave-data") == 0
             || strcmp(canonicalRepresentation, "gba-cgb-wave") == 0);
    if (strcmp(catalogType, "music-sequence") == 0)
        return schema == 1
            && strcmp(canonicalRepresentation, "gba-mp2k-song-graph") == 0;
    if (strcmp(catalogType, "instrument-bank") == 0)
        return schema == 1
            ? strcmp(canonicalRepresentation, "gba-tone-data-12") == 0
            : schema == 2
              ? strcmp(canonicalRepresentation, "gba-keysplit-run") == 0
              : false;
    /* R13-B: leaf byte payloads (movement scripts, multiboot programs) are
     * pure GBA bytes with no transform on either target - the pack stores the
     * ROM bytes verbatim. schema 1 = movement, schema 2 = multiboot (the
     * approved architecture §3 "BINARY with family schemas" pair). Anything
     * else - wrong schema, wrong representation, cross-type mixes - fails
     * closed. */
    if (strcmp(catalogType, "binary") == 0)
        return (schema == 1 || schema == 2)
            && strcmp(canonicalRepresentation, "gba-bytes") == 0;
    /* R13-C: text families (labels, bundles, skeletons, match-call tables).
     * schema 1 = gba-charmap character data; the pack stores the canonical
     * bytes verbatim (raw codec, one byte per character slot). */
    if (strcmp(catalogType, "text") == 0)
        return schema == 1 && strcmp(canonicalRepresentation, "gba-charmap") == 0;
    /* R13-D1: structured gameplay-data rows. Canonical payload is the exact
     * padded GBA wire slice; schema declares the family row contract
     * (species-base=1 .. contest-combo-starters=15; the approved plan §4 set,
     * incl. the D2 item=11). The native transform (padded->packed struct) is
     * owned entirely by the publication seam, never the pack. */
    if (strcmp(catalogType, "structured-data") == 0)
        return schema >= 1 && schema <= 15
            && strcmp(canonicalRepresentation, "gba-bytes") == 0;
    /* R13-D1: font glyph bitmaps (original GBA font wire, raw little-endian
     * u16 wordstreams, no tile-graphics interpretation). schema 1. This is
     * NOT a second graphics migration -- fonts keep their own type and never
     * enter the tile/palette validators. */
    if (strcmp(catalogType, "font") == 0)
        return schema == 1 && strcmp(canonicalRepresentation, "gba-bytes") == 0;
    return false;
}

/* ---- duplicate-range check ---------------------------------------------- */

struct RangeCheck
{
    const char *id;
    uint32_t offset;
    uint32_t end;
    bool allowShared;
};

static int CompareRangeByOffset(const void *left, const void *right)
{
    const struct RangeCheck *a = left;
    const struct RangeCheck *b = right;
    if (a->offset < b->offset)
        return -1;
    if (a->offset > b->offset)
        return 1;
    return strcmp(a->id, b->id);
}

/* ---- record ordering ---------------------------------------------------- */

static int CompareRecordById(const void *left, const void *right)
{
    const struct Gen3ManifestRecord *a = left;
    const struct Gen3ManifestRecord *b = right;
    return strcmp(a->id, b->id);
}

/* ---- generation core ---------------------------------------------------- */

enum Gen3ManifestResult Gen3Manifest_Generate(
    const struct Gen3ManifestMeta *meta,
    const struct Gen3CatalogEntry *catalog, size_t catalogCount,
    const struct Gen3BindingInput *bindings, size_t bindingCount,
    const uint8_t *elfData, size_t elfSize,
    const uint8_t *romData, size_t romSize,
    const struct Gen3ManifestConfig *config,
    struct Gen3Buffer *outManifest,
    char *errbuf, size_t errbufSize)
{
    const char *expectedSha1Hex = (config != NULL && config->expectedRomSha1Hex != NULL)
        ? config->expectedRomSha1Hex : GEN3_DEFAULT_ROM_SHA1_HEX;
    uint8_t expectedSha1[20];
    uint8_t romSha1[20];
    uint8_t romSha256[32];
    struct Gen3Elf elf;
    struct Gen3ManifestRecord *records = NULL;
    struct RangeCheck *ranges = NULL;
    size_t i;
    enum Gen3ManifestResult result = GEN3_MANIFEST_INVALID_INPUT;

    if (outManifest == NULL)
        return GEN3_MANIFEST_INVALID_INPUT;
    /* Leave the output in a safe empty state now; real capacity is allocated
     * only at serialization so early validation failures do not leak it. */
    Gen3Buffer_Init(outManifest, 0);
    if (catalog == NULL || bindings == NULL || elfData == NULL || romData == NULL)
    {
        SetError(errbuf, errbufSize, "internal: null generation argument");
        return GEN3_MANIFEST_INVALID_INPUT;
    }

    /* Guardrail: the qualification field is restricted to the two supported
     * values so a manifest can never silently claim a status it does not have.
     * NULL defaults to "fixture" (never "production"). */
    {
        const char *qualification = (config != NULL && config->qualification != NULL)
            ? config->qualification : "fixture";
        if (strcmp(qualification, "fixture") != 0
         && strcmp(qualification, "production") != 0)
        {
            SetError(errbuf, errbufSize,
                     "invalid manifest qualification '%s' (expected \"fixture\" or \"production\")",
                     qualification);
            return GEN3_MANIFEST_BAD_QUALIFICATION;
        }
    }

    /* Guardrail 1-4: ROM size, identity, SHA-1, SHA-256. */
    {
        const char *badField = NULL;
        if (!CheckRomIdentity(romData, romSize, config, &badField))
        {
            SetError(errbuf, errbufSize, "ROM identity check failed: %s", badField);
            return GEN3_MANIFEST_BAD_ROM_SIZE;
        }
    }
    Sha1Of(romData, romSize, romSha1);
    if (!ParseHex(expectedSha1Hex, expectedSha1, sizeof(expectedSha1)))
    {
        SetError(errbuf, errbufSize, "invalid expected ROM SHA-1 '%s'", expectedSha1Hex);
        return GEN3_MANIFEST_BAD_ROM_SHA1;
    }
    if (memcmp(romSha1, expectedSha1, sizeof(romSha1)) != 0)
    {
        char actual[GEN3_SHA1_HEX_SIZE];
        Gen3Util_FormatHex(romSha1, sizeof(romSha1), actual);
        SetError(errbuf, errbufSize, "ROM SHA-1 mismatch: expected %s, got %s",
                 expectedSha1Hex, actual);
        return GEN3_MANIFEST_BAD_ROM_SHA1;
    }
    Sha256Of(romData, romSize, romSha256);

    /* Guardrail 5-7: ELF symbol table. */
    memset(&elf, 0, sizeof(elf));
    {
        enum Gen3ElfResult elfResult = Gen3Elf_Open(elfData, elfSize, &elf);
        if (elfResult != GEN3_ELF_OK)
        {
            SetError(errbuf, errbufSize, "ELF parse failed (error %d)", (int)elfResult);
            return GEN3_MANIFEST_BAD_ELF;
        }
    }

    /* Duplicate canonical ids (catalog and bindings). */
    for (i = 0; i < catalogCount; i++)
    {
        size_t j;
        for (j = i + 1u; j < catalogCount; j++)
        {
            if (strcmp(catalog[i].id, catalog[j].id) == 0)
            {
                SetError(errbuf, errbufSize, "duplicate catalog resource id '%s'", catalog[i].id);
                result = GEN3_MANIFEST_DUPLICATE_ID;
                goto done;
            }
        }
    }
    for (i = 0; i < bindingCount; i++)
    {
        size_t j;
        for (j = i + 1u; j < bindingCount; j++)
        {
            if (strcmp(bindings[i].id, bindings[j].id) == 0)
            {
                SetError(errbuf, errbufSize, "duplicate binding id '%s'", bindings[i].id);
                result = GEN3_MANIFEST_DUPLICATE_ID;
                goto done;
            }
        }
    }

    records = calloc(bindingCount != 0 ? bindingCount : 1u, sizeof(*records));
    ranges = calloc(bindingCount != 0 ? bindingCount : 1u, sizeof(*ranges));
    if (records == NULL || ranges == NULL)
    {
        SetError(errbuf, errbufSize, "out of memory building manifest records");
        result = GEN3_MANIFEST_INVALID_INPUT;
        goto done;
    }

    /* R15 Phase 4 pre-pass: fork-renamed battle-anim gfx symbols are absent
     * from the pret reference ELF; their provenance is the ROM bytes. Build a
     * 4-byte-prefix index over those bindings, scan the ROM once collecting
     * full-match candidates (cap 16 per binding), then resolve: unique match
     * wins; ambiguous payloads resolve inside the family region anchored by
     * the unique matches; still-ambiguous payloads fail closed. */
    struct ByteProvenance
    {
        const struct Gen3BindingInput *binding;
        size_t matches[16];
        size_t matchCount;
    };
    struct ByteProvenance *byteProv = NULL;
    size_t byteProvCount = 0u;
    size_t byteRegionMin = SIZE_MAX;
    size_t byteRegionMax = 0u;
    for (i = 0; i < bindingCount; i++)
    {
        const struct Gen3BindingInput *b = &bindings[i];
        if (!b->bundle && Gen3Elf_FindSymbol(&elf, b->symbol) == NULL)
            byteProvCount++;
    }
    if (byteProvCount > 0u)
    {
        /* open-addressing prefix table: key = first 4 artifact bytes */
        size_t tableBits = 1u;
        while ((1u << tableBits) < byteProvCount * 4u)
            tableBits++;
        size_t tableSize = 1u << tableBits;
        struct PrefixEntry
        {
            uint32_t key;
            size_t provIndex;
            bool used;
        };
        struct PrefixEntry *prefixTable = calloc(tableSize, sizeof(*prefixTable));
        byteProv = calloc(byteProvCount, sizeof(*byteProv));
        if (prefixTable == NULL || byteProv == NULL)
        {
            free(prefixTable);
            free(byteProv);
            free(records);
            free(ranges);
            SetError(errbuf, errbufSize, "out of memory building byte provenance");
            goto done;
        }
        {
            size_t p = 0u;
            for (i = 0; i < bindingCount; i++)
            {
                const struct Gen3BindingInput *b = &bindings[i];
                uint32_t key;
                size_t slot;
                if (b->bundle || Gen3Elf_FindSymbol(&elf, b->symbol) != NULL
                    || b->sourceArtifactSize < 4u)
                    continue;
                memcpy(&key, b->sourceArtifact, sizeof(key));
                slot = (size_t)(key * 2654435761u) & (tableSize - 1u);
                while (prefixTable[slot].used)
                    slot = (slot + 1u) & (tableSize - 1u);
                prefixTable[slot].key = key;
                prefixTable[slot].provIndex = p;
                prefixTable[slot].used = true;
                byteProv[p].binding = b;
                p++;
            }
        }
        /* One ROM pass. */
        if (romSize >= 4u)
        {
            size_t o;
            for (o = 0u; o + 4u <= romSize; o++)
            {
                uint32_t key;
                size_t slot;
                memcpy(&key, romData + o, sizeof(key));
                slot = (size_t)(key * 2654435761u) & (tableSize - 1u);
                while (prefixTable[slot].used)
                {
                    if (prefixTable[slot].key == key)
                    {
                        struct ByteProvenance *prov =
                            &byteProv[prefixTable[slot].provIndex];
                        const struct Gen3BindingInput *b = prov->binding;
                        if (prov->matchCount < 16u
                         && o + b->sourceArtifactSize <= romSize
                         && memcmp(romData + o, b->sourceArtifact,
                                   b->sourceArtifactSize) == 0)
                        {
                            prov->matches[prov->matchCount++] = o;
                        }
                    }
                    slot = (slot + 1u) & (tableSize - 1u);
                }
            }
        }
        free(prefixTable);
        /* Resolve: unique matches anchor the region; ambiguous ones resolve
         * inside [min-0x1000, max+0x1000] when exactly one candidate sits
         * there; else they stay ambiguous and fail in the main loop. */
        for (i = 0; i < byteProvCount; i++)
        {
            if (byteProv[i].matchCount == 1u)
            {
                size_t o = byteProv[i].matches[0];
                if (o < byteRegionMin)
                    byteRegionMin = o;
                if (o > byteRegionMax)
                    byteRegionMax = o;
            }
        }
        for (i = 0; i < byteProvCount; i++)
        {
            struct ByteProvenance *prov = &byteProv[i];
            if (prov->matchCount <= 1u)
                continue;
            if (byteRegionMin != SIZE_MAX)
            {
                size_t lo = byteRegionMin > 0x1000u ? byteRegionMin - 0x1000u : 0u;
                size_t hi = byteRegionMax + 0x1000u;
                size_t m, inRegion = 0u, kept = SIZE_MAX;
                for (m = 0u; m < prov->matchCount; m++)
                {
                    if (prov->matches[m] >= lo && prov->matches[m] <= hi)
                    {
                        inRegion++;
                        if (prov->matches[m] < kept)
                            kept = prov->matches[m];
                    }
                }
                if (inRegion >= 1u)
                {
                    /* Identical-byte payloads (repeated palettes) can match
                     * at several ROM offsets; every match IS the canonical
                     * bytes, so the lowest in-region offset is the
                     * deterministic provenance. */
                    prov->matches[0] = kept;
                    prov->matchCount = 1u;
                }
            }
        }
    }

    for (i = 0; i < bindingCount; i++)
    {
        const struct Gen3BindingInput *binding = &bindings[i];
        const struct Gen3CatalogEntry *catalogEntry;
        const struct Gen3ElfSymbol *symbol;
        size_t elfOffset;
        size_t elfLength;
        uint32_t romOffset;
        uint32_t encodedLength;
        size_t romEnd;
        uint8_t *decoded = NULL;
        size_t decodedSize = 0;
        const uint8_t *canonicalBytes = NULL;
        Gen3ResourceKey key;

        if (Gen3ResourceId_ValidateCanonicalName(binding->id) != GEN3_RESOURCE_NAME_VALID)
        {
            SetError(errbuf, errbufSize, "binding id '%s' is not a valid canonical name",
                     binding->id);
            result = GEN3_MANIFEST_INVALID_NAME;
            goto done;
        }
        catalogEntry = FindCatalogEntry(catalog, catalogCount, binding->id);
        if (catalogEntry == NULL)
        {
            SetError(errbuf, errbufSize, "binding id '%s' is not declared in the catalog",
                     binding->id);
            result = GEN3_MANIFEST_UNKNOWN_RESOURCE;
            goto done;
        }
        if (!TypeCompatibleWithRepresentation(catalogEntry->type, catalogEntry->schema,
                                              binding->canonicalRepresentation))
        {
            SetError(errbuf, errbufSize,
                     "binding '%s': canonical representation '%s' is incompatible with catalog type '%s'",
                     binding->id, binding->canonicalRepresentation, catalogEntry->type);
            result = GEN3_MANIFEST_TYPE_MISMATCH;
            goto done;
        }

        if (binding->bundle)
        {
            /* R13-C text bundle: the artifact file is the entire payload --
             * no ELF symbol, no ROM slice, no range. Raw encoding only, so
             * encoded == decoded == canonical (the same three-way size and
             * byte agreement the raw path enforces below). */
            if (binding->hasSymbolOffset)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': 'symbol_offset' is invalid for a bundle",
                         binding->id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            if (strcmp(binding->sourceEncoding, "raw") != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': bundle bindings require a raw source encoding",
                         binding->id);
                result = GEN3_MANIFEST_UNSUPPORTED_ENCODING;
                goto done;
            }
            if (binding->sourceArtifactSize == 0u)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': bundle artifact is empty", binding->id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            if (binding->sourceArtifactSize != binding->expectedDecodedSize
             || binding->sourceArtifactSize != binding->canonicalDecodedSize)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': bundle size mismatch (artifact %zu, "
                         "binding expected %u, canonical %zu)",
                         binding->id, binding->sourceArtifactSize,
                         binding->expectedDecodedSize, binding->canonicalDecodedSize);
                result = GEN3_MANIFEST_DECODED_SIZE_MISMATCH;
                goto done;
            }
            if (memcmp(binding->sourceArtifact, binding->canonicalDecoded,
                       binding->sourceArtifactSize) != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': bundle artifact does not match the canonical "
                         "decoded artifact", binding->id);
                result = GEN3_MANIFEST_CANONICAL_MISMATCH;
                goto done;
            }
            romOffset = 0;
            encodedLength = (uint32_t)binding->sourceArtifactSize;
        }
        else
        {
        symbol = Gen3Elf_FindSymbol(&elf, binding->symbol);
        if (symbol == NULL)
        {
            /* R15 Phase 4 fallback: consult the indexed byte-provenance
             * pre-pass (fork-renamed battle-anim gfx symbols are absent from
             * the pret reference ELF; unique ROM byte matches - or matches
             * uniquely inside the family region - are the provenance). */
            const struct ByteProvenance *prov = NULL;
            size_t p;
            for (p = 0u; p < byteProvCount; p++)
            {
                if (byteProv[p].binding == binding)
                {
                    prov = &byteProv[p];
                    break;
                }
            }
            if (prov == NULL || prov->matchCount != 1u)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': symbol '%s' not found in the ELF "
                         "and the artifact bytes match the ROM %s",
                         binding->id, binding->symbol,
                         prov == NULL ? "nowhere"
                         : prov->matchCount == 0u ? "nowhere"
                         : "at more than one offset (ambiguous provenance)");
                result = GEN3_MANIFEST_MISSING_SYMBOL;
                goto done;
            }
            elfOffset = prov->matches[0];
            elfLength = SIZE_MAX;
            romOffset = (uint32_t)prov->matches[0];
            encodedLength = (uint32_t)binding->sourceArtifactSize;
            goto found_byte_provenance;
        }
        if (!Gen3Elf_SymbolFileRange(&elf, symbol, &elfOffset, &elfLength))
        {
            SetError(errbuf, errbufSize,
                     "binding '%s': symbol '%s' is not an allocated ROM object at or above 0x%08x",
                     binding->id, binding->symbol, GEN3_GBA_ROM_BASE);
            result = GEN3_MANIFEST_SYMBOL_NOT_ROM;
            goto done;
        }
        if (binding->hasSymbolOffset)
        {
            /* Palette-row slice (R11-C): the binding covers
             * [symbolOffset, symbolOffset + expectedDecodedSize) inside the
             * symbol, not the whole symbol (row 0 is offset 0; presence of
             * the key selects the slice path). Raw-encoding only, so the
             * encoded payload IS the decoded slice and the lengths agree.
             * elfLength is the symbol size, or the section bound for size-0
             * asm labels (R11-C: tileset palette blobs). */
            if (strcmp(binding->sourceEncoding, "raw") != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': 'symbol_offset' requires a raw source encoding",
                         binding->id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            if ((uint64_t)binding->symbolOffset + binding->expectedDecodedSize > elfLength)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': symbol '%s' slice [%u, %u) exceeds the available bound %zu",
                         binding->id, binding->symbol, binding->symbolOffset,
                         binding->symbolOffset + binding->expectedDecodedSize, elfLength);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            romOffset = symbol->value - GEN3_GBA_ROM_BASE + binding->symbolOffset;
            encodedLength = binding->expectedDecodedSize;
        }
        else
        {
            romOffset = symbol->value - GEN3_GBA_ROM_BASE;
            if (strcmp(catalogEntry->type, "text") == 0)
            {
                /* R13-C text labels: the ELF symbol bound for a .string
                 * label runs past the 0xFF terminator into the next label
                 * (GNU as emits no .size; the bound reaches the next
                 * symbol), so symbol->size is NOT the payload length. The
                 * canonical artifact (scan to the first 0xFF, charmap-
                 * validated by the generator) defines the payload; Guardrail
                 * 10 pins it as a prefix of the ELF/ROM bytes. */
                encodedLength = binding->sourceArtifactSize;
            }
            else
            {
                /* Size-0 asm labels declare no length; the artifact length
                 * is the source of truth and Guardrail 10 pins it against
                 * ELF+ROM. */
                encodedLength = symbol->size != 0 ? symbol->size : binding->sourceArtifactSize;
            }
        }
        found_byte_provenance:
        if (encodedLength > elfLength)
        {
            SetError(errbuf, errbufSize,
                     "binding '%s': symbol '%s' needs %u bytes but the ELF bound is %zu",
                     binding->id, binding->symbol, encodedLength, elfLength);
            result = GEN3_MANIFEST_SYMBOL_NOT_ROM;
            goto done;
        }
        romEnd = (size_t)romOffset + encodedLength;
        if (romEnd > GEN3_ROM_SIZE)
        {
            SetError(errbuf, errbufSize,
                     "binding '%s': symbol '%s' occupies ROM [0x%08x, 0x%08x), past the 16 MiB image",
                     binding->id, binding->symbol, romOffset, (uint32_t)romEnd);
            result = GEN3_MANIFEST_ROM_RANGE_OVERFLOW;
            goto done;
        }

        /* Guardrail 10: source artifact == ELF symbol bytes == ROM slice.
         * On the slice path the ELF side compares at the slice start and
         * romOffset already includes the slice, so the ROM side is unchanged.
         * symbolOffset is only meaningful on the slice path. */
        if (binding->sourceArtifactSize != encodedLength
         || (symbol != NULL
             && memcmp(binding->sourceArtifact,
                       elfData + elfOffset + (binding->hasSymbolOffset ? binding->symbolOffset : 0u),
                       encodedLength) != 0)
         || memcmp(binding->sourceArtifact, romData + romOffset, encodedLength) != 0)
        {
            SetError(errbuf, errbufSize,
                     "binding '%s': source artifact does not match the ELF/ROM bytes at symbol '%s'",
                     binding->id, binding->symbol);
            result = GEN3_MANIFEST_ARTIFACT_MISMATCH;
            goto done;
        }
        } /* else: symbol-backed binding */

        /* Guardrail 11: decode by source encoding.
         *   gba-lz77: strict LZ77 decode with a three-way size agreement
         *             (LZ header == binding expected == binding canonical).
         *   raw:     (R8 back sheets) the artifact IS the canonical decoded
         *             payload; encoded == decoded, no header, no decode. */
        if (strcmp(binding->sourceEncoding, "raw") == 0)
        {
            if (binding->sourceArtifactSize != binding->expectedDecodedSize
             || binding->sourceArtifactSize != binding->canonicalDecodedSize)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': raw size mismatch (artifact %zu, "
                         "binding expected %u, canonical %zu)",
                         binding->id, binding->sourceArtifactSize,
                         binding->expectedDecodedSize, binding->canonicalDecodedSize);
                result = GEN3_MANIFEST_DECODED_SIZE_MISMATCH;
                goto done;
            }
            /* Guardrails 12-13: artifact bytes == canonical decoded artifact. */
            if (memcmp(binding->sourceArtifact, binding->canonicalDecoded,
                       binding->sourceArtifactSize) != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': raw artifact does not match the canonical "
                         "decoded artifact", binding->id);
                result = GEN3_MANIFEST_CANONICAL_MISMATCH;
                goto done;
            }
            decodedSize = binding->sourceArtifactSize;
            canonicalBytes = binding->sourceArtifact;
        }
        else if (strcmp(binding->sourceEncoding, "gba-lz77") == 0)
        {
            uint32_t declaredSize;
            enum Gen3Lz77Result lzResult;
            if (binding->sourceArtifactSize < 4 || binding->sourceArtifact[0] != 0x10)
            {
                SetError(errbuf, errbufSize, "binding '%s': not a GBA LZ77 (.lz) stream",
                         binding->id);
                result = GEN3_MANIFEST_LZ_FAILED;
                goto done;
            }
            declaredSize = (uint32_t)binding->sourceArtifact[1]
                         | ((uint32_t)binding->sourceArtifact[2] << 8)
                         | ((uint32_t)binding->sourceArtifact[3] << 16);
            if ((size_t)declaredSize != binding->expectedDecodedSize
             || (size_t)declaredSize != binding->canonicalDecodedSize)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': decoded size mismatch (LZ header %u, binding %u, artifact %zu)",
                         binding->id, declaredSize, binding->expectedDecodedSize,
                         binding->canonicalDecodedSize);
                result = GEN3_MANIFEST_DECODED_SIZE_MISMATCH;
                goto done;
            }
            decoded = malloc(declaredSize != 0 ? declaredSize : 1u);
            if (decoded == NULL)
            {
                SetError(errbuf, errbufSize, "out of memory decoding '%s'", binding->id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            lzResult = Gen3Lz77_Decode(binding->sourceArtifact, binding->sourceArtifactSize,
                                       decoded, declaredSize, &decodedSize);
            if (lzResult != GEN3_LZ77_OK)
            {
                SetError(errbuf, errbufSize, "binding '%s': LZ77 decode rejected (error %d)",
                         binding->id, (int)lzResult);
                free(decoded);
                result = GEN3_MANIFEST_LZ_FAILED;
                goto done;
            }
            if (decodedSize != (size_t)declaredSize)
            {
                SetError(errbuf, errbufSize, "binding '%s': decoded %zu bytes, header declared %u",
                         binding->id, decodedSize, declaredSize);
                free(decoded);
                result = GEN3_MANIFEST_DECODED_SIZE_MISMATCH;
                goto done;
            }

            /* Guardrails 12-13: decoded bytes == canonical decoded artifact. */
            if (decodedSize != binding->canonicalDecodedSize
             || memcmp(decoded, binding->canonicalDecoded, decodedSize) != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': decoded bytes do not match the canonical decoded artifact",
                         binding->id);
                free(decoded);
                result = GEN3_MANIFEST_CANONICAL_MISMATCH;
                goto done;
            }
            canonicalBytes = decoded;
        }
        else
        {
            SetError(errbuf, errbufSize,
                     "binding '%s': unsupported source encoding '%s' (gba-lz77 or raw)",
                     binding->id, binding->sourceEncoding);
            result = GEN3_MANIFEST_UNSUPPORTED_ENCODING;
            goto done;
        }

        records[i].id = binding->id;
        records[i].type = catalogEntry->type;
        records[i].schema = catalogEntry->schema;
        records[i].symbol = binding->symbol;
        records[i].romOffset = romOffset;
        records[i].encodedLength = encodedLength;
        records[i].decodedLength = (uint32_t)decodedSize;
        records[i].sourceEncoding = binding->sourceEncoding;
        records[i].bundle = binding->bundle;
        records[i].bundleSourceArtifact = binding->bundleSourceArtifact;
        Sha256Of(binding->sourceArtifact, binding->sourceArtifactSize,
                 records[i].sourceEncodedSha256);
        Sha256Of(canonicalBytes, decodedSize, records[i].canonicalDecodedSha256);
        Gen3ResourceId_DeriveKey(binding->id, &key);
        memcpy(records[i].key, key.bytes, sizeof(key.bytes));

        ranges[i].id = binding->id;
        ranges[i].offset = binding->bundle ? 0u : romOffset;
        /* Bundle records have no ROM range; end == 0 marks them for the
         * duplicate-range sweep, which skips end == 0 entries. */
        ranges[i].end = binding->bundle ? 0u : (uint32_t)romEnd;
        ranges[i].allowShared = binding->allowSharedRange;

        free(decoded);
        decoded = NULL;
    }

    /* Records are emitted sorted bytewise by canonical id. */
    if (bindingCount > 1)
        qsort(records, bindingCount, sizeof(*records), CompareRecordById);

    /* Duplicate-range detection (overlapping allocations are rejected unless
     * the binding explicitly allows sharing). */
    {
        struct RangeCheck *sortedRanges;
        sortedRanges = malloc((bindingCount != 0 ? bindingCount : 1u) * sizeof(*sortedRanges));
        if (sortedRanges == NULL)
        {
            SetError(errbuf, errbufSize, "out of memory checking ROM ranges");
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        memcpy(sortedRanges, ranges, bindingCount * sizeof(*sortedRanges));
        if (bindingCount > 1)
            qsort(sortedRanges, bindingCount, sizeof(*sortedRanges), CompareRangeByOffset);
        for (i = 1; i < bindingCount; i++)
        {
            const struct RangeCheck *previous = &sortedRanges[i - 1u];
            const struct RangeCheck *current = &sortedRanges[i];
            /* Bundle records carry no ROM range (end == 0 sentinel). */
            if (previous->end == 0u || current->end == 0u)
                continue;
            if (previous->end > current->offset
             && !previous->allowShared && !current->allowShared)
            {
                SetError(errbuf, errbufSize,
                         "ROM range of '%s' overlaps '%s' (and neither allows a shared range)",
                         current->id, previous->id);
                free(sortedRanges);
                result = GEN3_MANIFEST_DUPLICATE_RANGE;
                goto done;
            }
        }
        free(sortedRanges);
    }

    /* Guardrails 14-16: key reuse + deterministic serialization. */
    if (!Gen3Buffer_Init(outManifest, 4096))
    {
        SetError(errbuf, errbufSize, "out of memory");
        result = GEN3_MANIFEST_INVALID_INPUT;
        goto done;
    }
    if (!Gen3Manifest_Serialize(meta, records, bindingCount, romSha1, romSha256,
                                GEN3_ROM_SIZE,
                                config != NULL ? config->provenance : NULL,
                                config != NULL ? config->qualification : NULL,
                                outManifest))
    {
        SetError(errbuf, errbufSize, "out of memory serializing the manifest");
        result = GEN3_MANIFEST_INVALID_INPUT;
        goto done;
    }
    result = GEN3_MANIFEST_OK;

done:
    free(byteProv);
    Gen3Elf_Destroy(&elf);
    free(records);
    free(ranges);
    return result;
}

/* ---- serialization ------------------------------------------------------ */

bool Gen3Manifest_Serialize(const struct Gen3ManifestMeta *meta,
                            const struct Gen3ManifestRecord *records, size_t recordCount,
                            const uint8_t romSha1[20], const uint8_t romSha256[32],
                            uint32_t romSize, const char *provenance,
                            const char *qualification,
                            struct Gen3Buffer *out)
{
    const char *namespace = (meta != NULL && meta->namespace != NULL) ? meta->namespace : "emerald";
    const char *resourceApi = (meta != NULL && meta->resourceApi != NULL) ? meta->resourceApi : "1.0.0";
    char sha1Hex[GEN3_SHA1_HEX_SIZE];
    char sha256Hex[GEN3_SHA256_HEX_SIZE];
    size_t i;
    if (out == NULL || records == NULL)
        return false;
    Gen3Util_FormatHex(romSha1, 20, sha1Hex);
    Gen3Util_FormatHex(romSha256, 32, sha256Hex);

    Gen3Buffer_AppendCStr(out, "# Deterministic extraction manifest - generated by tools/gen3_resources/elf_manifest.\n");
    Gen3Buffer_AppendCStr(out, "# Do not edit by hand; regenerate with `gen3-elf-manifest --check`.\n");
    Gen3Buffer_AppendCStr(out, "# Records are sorted bytewise by canonical resource id.\n");
    if (provenance != NULL && provenance[0] != '\0')
    {
        Gen3Buffer_AppendCStr(out, "# provenance: ");
        Gen3Buffer_AppendCStr(out, provenance);
        Gen3Buffer_AppendCStr(out, "\n");
    }
    Gen3TomlWrite_Integer(out, "manifest_version", GEN3_MANIFEST_VERSION);
    Gen3TomlWrite_String(out, "namespace", namespace);
    Gen3TomlWrite_String(out, "resource_api", resourceApi);
    {
        const char *game = (meta != NULL && meta->game != NULL) ? meta->game : NULL;
        const char *romProfile = (meta != NULL && meta->romProfile != NULL) ? meta->romProfile : NULL;
        const char *qualificationField = (qualification != NULL && qualification[0] != '\0')
            ? qualification : "fixture";
        if (game != NULL && game[0] != '\0')
            Gen3TomlWrite_String(out, "game", game);
        if (romProfile != NULL && romProfile[0] != '\0')
            Gen3TomlWrite_String(out, "rom_profile", romProfile);
        Gen3TomlWrite_String(out, "qualification", qualificationField);
    }
    Gen3TomlWrite_Integer(out, "rom_size", (long long)romSize);
    Gen3TomlWrite_String(out, "rom_sha1", sha1Hex);
    Gen3TomlWrite_String(out, "rom_sha256", sha256Hex);
    Gen3Buffer_AppendCStr(out, "\n");

    for (i = 0; i < recordCount; i++)
    {
        const struct Gen3ManifestRecord *record = &records[i];
        char keyHex[GEN3_SHA256_HEX_SIZE];
        char sourceHex[GEN3_SHA256_HEX_SIZE];
        char decodedHex[GEN3_SHA256_HEX_SIZE];
        Gen3Util_FormatHex(record->key, sizeof(record->key), keyHex);
        Gen3Util_FormatHex(record->sourceEncodedSha256, sizeof(record->sourceEncodedSha256), sourceHex);
        Gen3Util_FormatHex(record->canonicalDecodedSha256, sizeof(record->canonicalDecodedSha256), decodedHex);

        Gen3TomlWrite_OpenArrayTable(out, "records");
        Gen3TomlWrite_String(out, "id", record->id);
        Gen3TomlWrite_String(out, "key", keyHex);
        Gen3TomlWrite_String(out, "type", record->type);
        Gen3TomlWrite_Integer(out, "schema", (long long)record->schema);
        if (record->bundle)
        {
            /* R13-C bundle record: the payload is a constructed artifact on
             * disk; the pack builder reads this path (repo-relative, resolved
             * against the pack-build working directory). rom_offset below is
             * 0 and carries no meaning for bundles. */
            Gen3Buffer_AppendCStr(out, "bundle = true\n");
            Gen3TomlWrite_String(out, "source_artifact", record->bundleSourceArtifact);
        }
        Gen3TomlWrite_String(out, "symbol", record->symbol);
        Gen3TomlWrite_Integer(out, "rom_offset", (long long)record->romOffset);
        Gen3TomlWrite_Integer(out, "encoded_length", (long long)record->encodedLength);
        Gen3TomlWrite_Integer(out, "decoded_length", (long long)record->decodedLength);
        Gen3TomlWrite_String(out, "source_encoding", record->sourceEncoding);
        Gen3TomlWrite_String(out, "source_encoded_sha256", sourceHex);
        Gen3TomlWrite_String(out, "canonical_decoded_sha256", decodedHex);
        Gen3Buffer_AppendCStr(out, "\n");
    }
    return true;
}

/* ---- TOML-driven entry point ------------------------------------------- */

/* Resolves `relative` against `baseDir` and stores the result in `outPath`. */
static bool JoinPath(const char *baseDir, const char *relative, struct Gen3Buffer *outPath)
{
    if (relative == NULL || outPath == NULL)
        return false;
    if (relative[0] == '/')
        return Gen3Buffer_AppendCStr(outPath, relative);
    if (baseDir != NULL && baseDir[0] != '\0')
    {
        if (!Gen3Buffer_AppendCStr(outPath, baseDir))
            return false;
        if (baseDir[strlen(baseDir) - 1u] != '/')
            Gen3Buffer_AppendCStr(outPath, "/");
    }
    return Gen3Buffer_AppendCStr(outPath, relative);
}

enum Gen3ManifestResult Gen3Manifest_FromToml(
    const struct Gen3TomlDocument *catalogDoc,
    const struct Gen3TomlDocument *bindingsDoc,
    const char *baseDir,
    const uint8_t *elfData, size_t elfSize,
    const uint8_t *romData, size_t romSize,
    const struct Gen3ManifestConfig *config,
    struct Gen3Buffer *outManifest,
    char *errbuf, size_t errbufSize)
{
    struct Gen3ManifestMeta meta;
    struct Gen3CatalogEntry *catalog = NULL;
    struct Gen3BindingInput *bindings = NULL;
    struct Gen3Buffer *fileBuffers = NULL;
    size_t catalogCount = 0;
    size_t bindingCount = 0;
    size_t i;
    enum Gen3ManifestResult result;

    if (catalogDoc == NULL || bindingsDoc == NULL)
    {
        SetError(errbuf, errbufSize, "internal: null TOML document");
        return GEN3_MANIFEST_INVALID_INPUT;
    }
    memset(&meta, 0, sizeof(meta));
    Gen3Toml_GetString(&catalogDoc->root, "namespace", &meta.namespace);
    Gen3Toml_GetString(&catalogDoc->root, "resource_api", &meta.resourceApi);
    Gen3Toml_GetString(&catalogDoc->root, "game", &meta.game);
    Gen3Toml_GetString(&catalogDoc->root, "rom_profile", &meta.romProfile);

    catalogCount = Gen3Toml_GetArrayCount(&catalogDoc->root, "resources");
    bindingCount = Gen3Toml_GetArrayCount(&bindingsDoc->root, "bindings");
    catalog = calloc(catalogCount != 0 ? catalogCount : 1u, sizeof(*catalog));
    bindings = calloc(bindingCount != 0 ? bindingCount : 1u, sizeof(*bindings));
    /* Three buffers per binding: [i*3] encoded artifact, [i*3+1] lz-decoded
     * canonical (or the raw concatenation when source_artifact_2 is set),
     * [i*3+2] the optional concatenation tail. */
    fileBuffers = calloc(bindingCount != 0 ? bindingCount * 3u : 3u, sizeof(*fileBuffers));
    if (catalog == NULL || bindings == NULL || fileBuffers == NULL)
    {
        SetError(errbuf, errbufSize, "out of memory loading inputs");
        result = GEN3_MANIFEST_INVALID_INPUT;
        goto done;
    }

    for (i = 0; i < catalogCount; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&catalogDoc->root, "resources", i);
        long long schema;
        bool requiredForBase;
        if (item == NULL)
        {
            SetError(errbuf, errbufSize, "catalog resource %zu is not a table", i);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetString(item, "id", &catalog[i].id))
        {
            SetError(errbuf, errbufSize, "catalog resource %zu is missing 'id'", i);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetString(item, "type", &catalog[i].type))
        {
            SetError(errbuf, errbufSize, "catalog resource '%s' is missing 'type'", catalog[i].id);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetInteger(item, "schema", &schema) || schema < 0 || schema > (long long)UINT32_MAX)
        {
            SetError(errbuf, errbufSize, "catalog resource '%s' has an invalid 'schema'", catalog[i].id);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetBool(item, "required_for_base", &requiredForBase))
        {
            SetError(errbuf, errbufSize, "catalog resource '%s' is missing 'required_for_base'", catalog[i].id);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        catalog[i].schema = (uint32_t)schema;
    }

    for (i = 0; i < bindingCount; i++)
    {
        const struct Gen3TomlMap *item = Gen3Toml_GetArrayItem(&bindingsDoc->root, "bindings", i);
        const char *sourceArtifact = NULL;
        const char *concatTail = NULL;
        long long expectedDecodedSize;
        long long symbolOffset = 0;
        bool hasSymbolOffset = false;
        struct Gen3Buffer path;
        const char *artifact;
        size_t artifactLength;
        size_t bufferIndex = i * 3u;
        if (item == NULL)
        {
            SetError(errbuf, errbufSize, "binding %zu is not a table", i);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetString(item, "id", &bindings[i].id))
        {
            SetError(errbuf, errbufSize, "binding %zu is missing 'id'", i);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetString(item, "symbol", &bindings[i].symbol))
        {
            SetError(errbuf, errbufSize, "binding '%s' is missing 'symbol'", bindings[i].id);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetString(item, "source_artifact", &sourceArtifact))
        {
            SetError(errbuf, errbufSize, "binding '%s' is missing 'source_artifact'", bindings[i].id);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetString(item, "source_encoding", &bindings[i].sourceEncoding))
        {
            SetError(errbuf, errbufSize, "binding '%s' is missing 'source_encoding'", bindings[i].id);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetString(item, "canonical_representation", &bindings[i].canonicalRepresentation))
        {
            SetError(errbuf, errbufSize, "binding '%s' is missing 'canonical_representation'", bindings[i].id);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!Gen3Toml_GetInteger(item, "expected_decoded_size", &expectedDecodedSize)
         || expectedDecodedSize < 0 || expectedDecodedSize > (long long)UINT32_MAX)
        {
            SetError(errbuf, errbufSize, "binding '%s' has an invalid 'expected_decoded_size'", bindings[i].id);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        bindings[i].expectedDecodedSize = (uint32_t)expectedDecodedSize;
        if (!Gen3Toml_GetBool(item, "allow_shared_range", &bindings[i].allowSharedRange))
            bindings[i].allowSharedRange = false;
        /* R13-C text bundles: bundle = true marks a constructed artifact
         * with no ELF symbol / ROM slice. The artifact path is carried into
         * the manifest record so the pack builder can re-read the file. */
        if (!Gen3Toml_GetBool(item, "bundle", &bindings[i].bundle))
            bindings[i].bundle = false;
        if (bindings[i].bundle)
        {
            const char *bundlePath;
            if (!Gen3Toml_GetString(item, "source_artifact", &bundlePath))
            {
                SetError(errbuf, errbufSize, "binding '%s' is a bundle but is missing 'source_artifact'",
                         bindings[i].id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            if (strcmp(bindings[i].sourceEncoding, "raw") != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': bundle bindings require a raw source encoding",
                         bindings[i].id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            bindings[i].bundleSourceArtifact = bundlePath;
        }
        hasSymbolOffset = Gen3Toml_GetInteger(item, "symbol_offset", &symbolOffset);
        if (hasSymbolOffset)
        {
            if (symbolOffset < 0 || symbolOffset > (long long)UINT32_MAX)
            {
                SetError(errbuf, errbufSize, "binding '%s' has an invalid 'symbol_offset'",
                         bindings[i].id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            bindings[i].hasSymbolOffset = true;
            bindings[i].symbolOffset = (uint32_t)symbolOffset;
        }

        /* Load the encoded artifact. Gen3Util_ReadFile initializes the buffer
         * itself, so do not pre-init here (that would leak the pre-allocation). */
        if (!Gen3Buffer_Init(&path, 256))
        {
            SetError(errbuf, errbufSize, "out of memory building artifact path");
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        if (!JoinPath(baseDir, sourceArtifact, &path)
         || !Gen3Util_ReadFile(path.data, &fileBuffers[bufferIndex], errbuf, errbufSize))
        {
            Gen3Buffer_Destroy(&path);
            result = GEN3_MANIFEST_INVALID_INPUT;
            goto done;
        }
        Gen3Buffer_Destroy(&path);
        bindings[i].sourceArtifact = (const uint8_t *)fileBuffers[bufferIndex].data;
        bindings[i].sourceArtifactSize = fileBuffers[bufferIndex].length;

        /* The canonical decoded artifact: the artifact path minus ".lz" for
         * gba-lz77; for "raw" (R8 back sheets) the artifact IS the canonical
         * decoded payload (encoded == decoded), so the same file serves both. */
        artifact = sourceArtifact;
        artifactLength = strlen(artifact);
        if (strcmp(bindings[i].sourceEncoding, "raw") == 0)
        {
            bindings[i].canonicalDecoded = bindings[i].sourceArtifact;
            bindings[i].canonicalDecodedSize = bindings[i].sourceArtifactSize;
        }
        else
        {
            if (strcmp(bindings[i].sourceEncoding, "gba-lz77") != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': unsupported source encoding '%s' (gba-lz77 or raw)",
                         bindings[i].id, bindings[i].sourceEncoding);
                result = GEN3_MANIFEST_UNSUPPORTED_ENCODING;
                goto done;
            }
            if (artifactLength < 3 || strcmp(artifact + artifactLength - 3u, ".lz") != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': canonical artifact path requires a '.lz' suffix",
                         bindings[i].id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            if (!Gen3Buffer_Init(&path, 256))
            {
                SetError(errbuf, errbufSize, "out of memory building canonical path");
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            {
                struct Gen3Buffer canonicalRelative;
                Gen3Buffer_Init(&canonicalRelative, artifactLength);
                Gen3Buffer_Append(&canonicalRelative, artifact, artifactLength - 3u);
                Gen3Buffer_AppendCStr(&canonicalRelative, "");
                if (!JoinPath(baseDir, canonicalRelative.data, &path))
                {
                    Gen3Buffer_Destroy(&canonicalRelative);
                    Gen3Buffer_Destroy(&path);
                    result = GEN3_MANIFEST_INVALID_INPUT;
                    goto done;
                }
                Gen3Buffer_Destroy(&canonicalRelative);
            }
            if (!Gen3Util_ReadFile(path.data, &fileBuffers[bufferIndex + 1u], errbuf, errbufSize))
            {
                Gen3Buffer_Destroy(&path);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            Gen3Buffer_Destroy(&path);
            bindings[i].canonicalDecoded = (const uint8_t *)fileBuffers[bufferIndex + 1u].data;
            bindings[i].canonicalDecodedSize = fileBuffers[bufferIndex + 1u].length;
        }

        /* Optional raw concatenation tail (R11-C, StormyWater anim frames):
         * the preproc emits every INCBIN argument, so the symbol bytes are
         * artifact || artifact_2. The concatenation replaces the encoded
         * payload (and, raw rule, the canonical payload) in slot [i*3+1];
         * the tail half stays in slot [i*3+2] for the cleanup loop. */
        if (Gen3Toml_GetString(item, "source_artifact_2", &concatTail))
        {
            if (strcmp(bindings[i].sourceEncoding, "raw") != 0)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': 'source_artifact_2' requires a raw source encoding",
                         bindings[i].id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            if (bindings[i].hasSymbolOffset)
            {
                SetError(errbuf, errbufSize,
                         "binding '%s': 'source_artifact_2' and 'symbol_offset' are mutually exclusive",
                         bindings[i].id);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            if (!Gen3Buffer_Init(&path, 256))
            {
                SetError(errbuf, errbufSize, "out of memory building concat path");
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            if (!JoinPath(baseDir, concatTail, &path)
             || !Gen3Util_ReadFile(path.data, &fileBuffers[bufferIndex + 2u], errbuf, errbufSize))
            {
                Gen3Buffer_Destroy(&path);
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            Gen3Buffer_Destroy(&path);
            bindings[i].concatTail = (const uint8_t *)fileBuffers[bufferIndex + 2u].data;
            bindings[i].concatTailSize = fileBuffers[bufferIndex + 2u].length;
            if (!Gen3Buffer_Init(&fileBuffers[bufferIndex + 1u], 256)
             || !Gen3Buffer_Append(&fileBuffers[bufferIndex + 1u],
                                   fileBuffers[bufferIndex].data,
                                   fileBuffers[bufferIndex].length)
             || !Gen3Buffer_Append(&fileBuffers[bufferIndex + 1u],
                                   bindings[i].concatTail, bindings[i].concatTailSize))
            {
                SetError(errbuf, errbufSize, "out of memory concatenating artifact");
                result = GEN3_MANIFEST_INVALID_INPUT;
                goto done;
            }
            bindings[i].sourceArtifact = (const uint8_t *)fileBuffers[bufferIndex + 1u].data;
            bindings[i].sourceArtifactSize = fileBuffers[bufferIndex + 1u].length;
            /* Raw rule: the concatenation IS the canonical decoded payload. */
            bindings[i].canonicalDecoded = bindings[i].sourceArtifact;
            bindings[i].canonicalDecodedSize = bindings[i].sourceArtifactSize;
        }
    }

    result = Gen3Manifest_Generate(&meta, catalog, catalogCount,
                                   bindings, bindingCount,
                                   elfData, elfSize, romData, romSize,
                                   config, outManifest, errbuf, errbufSize);

done:
    free(catalog);
    free(bindings);
    if (fileBuffers != NULL)
    {
        for (i = 0; i < bindingCount * 3u; i++)
            Gen3Buffer_Destroy(&fileBuffers[i]);
        free(fileBuffers);
    }
    return result;
}
