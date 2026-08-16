/* Emerald runtime compatibility seam (Stage R5, generalized in R7B).
 * See emerald_resource_compat.h for the contract.
 * Platform-neutral: no global.h, sprite.h, or frontend objects. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_compat.h"

/* Single permanent allocation (R5 §5):
 *   struct header
 *   entry table   (entryCount * Entry, at entryTableOffset)
 *   names         (strlen+1 each, packed)
 *   canonicals    (payload copies, 4-aligned)
 *   streams       (literal-only LZ77, 4-aligned)
 * all relative to the flexible `bytes[]` array. Every offset is derived from
 * validated sizes, with checked arithmetic, and is stable for the image
 * lifetime; the image is immutable after construction. */
struct EmeraldResourceCompatImageEntry
{
    uint32_t type;               /* enum Gen3ResourceType */
    uint32_t decodedSize;
    size_t streamSize;
    size_t nameOffset;           /* relative to bytes[] */
    size_t canonicalOffset;      /* relative to bytes[] */
    size_t streamOffset;         /* relative to bytes[] */
};

struct EmeraldResourceCompatibilityImage
{
    size_t entryCount;
    size_t entryTableOffset;
    size_t nameArenaOffset;
    size_t canonicalArenaOffset;
    size_t streamArenaOffset;
    size_t arenaSize;
    uint8_t bytes[];
};

static const struct EmeraldResourceCompatImageEntry *EntryAt(
    const struct EmeraldResourceCompatibilityImage *image, size_t index)
{
    return (const struct EmeraldResourceCompatImageEntry *)
        (const void *)(image->bytes + image->entryTableOffset
                       + index * sizeof(struct EmeraldResourceCompatImageEntry));
}

static void ClearDiagnostics(struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static void NoteMismatch(struct EmeraldResourceCompatDiagnostics *diagnostics,
                         const char *canonicalName,
                         const char *expectedType,
                         uint32_t expectedSize, uint32_t actualSize)
{
    if (diagnostics == NULL)
        return;
    ClearDiagnostics(diagnostics);
    if (canonicalName != NULL)
        snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                 "%s", canonicalName);
    snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
    if (expectedType != NULL)
        snprintf(diagnostics->expectedType, sizeof(diagnostics->expectedType),
                 "%s", expectedType);
    diagnostics->expectedSize = expectedSize;
    diagnostics->actualSize = actualSize;
}

static uint32_t ExpectedSizeForType(enum Gen3ResourceType type)
{
    switch (type)
    {
    case GEN3_RESOURCE_TYPE_TILE_GRAPHICS:
        return EMERALD_TRAINER_SHEET_SIZE;
    case GEN3_RESOURCE_TYPE_PALETTE:
        return EMERALD_TRAINER_PALETTE_SIZE;
    default:
        return 0u;
    }
}

/* Encoded stream size for one source entry (R8/R9): the payload bytes
 * verbatim for RAW and GBA_LZ entries (R9: the payload IS the GBA LZ77
 * stream), the deterministic literal-only LZ77 size for LZ entries. Returns
 * SIZE_MAX on overflow. */
static size_t EntryStreamSize(const struct EmeraldResourceCompatSourceEntry *entry)
{
    if (entry->encoding == EMERALD_COMPAT_ENTRY_RAW
     || entry->encoding == EMERALD_COMPAT_ENTRY_GBA_LZ)
        return (size_t)entry->payloadSize;
    return Gen3LzLiteral_EncodedSize(entry->payloadSize);
}

const char *EmeraldResourceCompatStatus_Describe(
    enum EmeraldResourceCompatStatus status)
{
    switch (status)
    {
    case EMERALD_COMPAT_OK:                      return "ok";
    case EMERALD_COMPAT_ERR_INVALID_ARGUMENT:    return "invalid argument";
    case EMERALD_COMPAT_ERR_OUT_OF_MEMORY:       return "out of memory";
    case EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_COMPAT_ERR_ENCODE_FAILED:       return "literal LZ encode failed";
    case EMERALD_COMPAT_ERR_RESOLVE_FAILED:      return "M0/M1 resolve failed";
    case EMERALD_COMPAT_ERR_PUBLISH_FAILED:      return "native publication failed";
    case EMERALD_COMPAT_ERR_UNAVAILABLE:         return "no valid session image";
    default:                                     return "unknown";
    }
}

enum EmeraldResourceCompatStatus
EmeraldResourceCompatImage_CreateFamily(
    const struct EmeraldResourceCompatSourceEntry *entries,
    size_t entryCount,
    struct EmeraldResourceCompatibilityImage **outImage,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    struct EmeraldResourceCompatibilityImage *image;
    struct EmeraldResourceCompatImageEntry *table;
    size_t total;
    size_t i;
    size_t offset;
    size_t entryTableSize;

    ClearDiagnostics(diagnostics);
    if (outImage != NULL)
        *outImage = NULL;
    if (entries == NULL || entryCount == 0u || outImage == NULL)
    {
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    }

    /* Phase 1: validate every entry (type + family size invariant) and
     * accumulate the deterministic encoded sizes before any allocation. */
    entryTableSize = entryCount * sizeof(struct EmeraldResourceCompatImageEntry);
    if (entryCount != 0u && entryTableSize / entryCount
            != sizeof(struct EmeraldResourceCompatImageEntry))
    {
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
        return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
    }
    offset = 0u;
    for (i = 0u; i < entryCount; i++)
    {
        const struct EmeraldResourceCompatSourceEntry *entry = &entries[i];
        uint32_t expected;
        size_t streamSize;

        if (entry->canonicalName == NULL || entry->payload == NULL)
        {
            NoteMismatch(diagnostics, entry != NULL ? entry->canonicalName : NULL,
                         NULL, 0u, 0u);
            return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
        }
        expected = (entry->expectedSize != 0u) ? entry->expectedSize
                                               : ExpectedSizeForType(entry->type);
        if (entry->encoding == EMERALD_COMPAT_ENTRY_GBA_LZ)
        {
            /* R9 §5: the payload is a GBA LZ77 stream. Its encoded length is
             * ROM-dependent (no invariant), but the stream header must be
             * well-formed and its declared decoded size must equal the
             * expected size - the mapping-level family invariant. */
            uint32_t declared = entry->payloadSize >= 3u
                ? (uint32_t)entry->payload[1]
                  | ((uint32_t)entry->payload[2] << 8)
                : 0u;
            if (expected == 0u || entry->payloadSize < 3u
             || entry->payload[0] != 0x10u || declared != expected)
            {
                NoteMismatch(diagnostics, entry->canonicalName,
                             Gen3ResourceType_Name(entry->type), expected,
                             declared);
                return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
            }
        }
        else if (expected == 0u || entry->payloadSize != expected)
        {
            NoteMismatch(diagnostics, entry->canonicalName,
                         Gen3ResourceType_Name(entry->type), expected,
                         entry->payloadSize);
            return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
        }
        streamSize = EntryStreamSize(entry);
        if (streamSize == SIZE_MAX)
        {
            NoteMismatch(diagnostics, entry->canonicalName,
                         Gen3ResourceType_Name(entry->type), expected,
                         entry->payloadSize);
            return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
        }
        (void)streamSize; /* deterministic for the invariants; computed per entry below */
    }

    /* Phase 2: arena layout. Layout (offsets relative to bytes[]):
     *   [entry table entryTableSize]
     *   [name 0] [name 1] ... (strlen+1 each)
     *   [canonical 0] ... (4-aligned)
     *   [stream 0] ... (4-aligned)
     * All arithmetic is checked; every offset derives from validated sizes. */
    offset = entryTableSize;
    for (i = 0u; i < entryCount; i++)
    {
        size_t nameLen = strlen(entries[i].canonicalName);
        if (SIZE_MAX - offset < nameLen + 1u)
        {
            NoteMismatch(diagnostics, entries[i].canonicalName,
                         Gen3ResourceType_Name(entries[i].type),
                         entries[i].payloadSize, entries[i].payloadSize);
            return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
        }
        offset += nameLen + 1u;
    }
    if (offset == entryTableSize && entryCount != 0u)
    {
        /* An empty-name entry would have produced offset == entryTableSize
         * only for entryCount == 0, which is rejected above. */
    }
    for (i = 0u; i < entryCount; i++)
    {
        size_t aligned = (offset + 3u) & ~(size_t)3u;
        if (aligned < offset)
        {
            NoteMismatch(diagnostics, entries[i].canonicalName,
                         Gen3ResourceType_Name(entries[i].type),
                         entries[i].payloadSize, entries[i].payloadSize);
            return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
        }
        offset = aligned;
        if (SIZE_MAX - offset < (size_t)entries[i].payloadSize)
        {
            NoteMismatch(diagnostics, entries[i].canonicalName,
                         Gen3ResourceType_Name(entries[i].type),
                         entries[i].payloadSize, entries[i].payloadSize);
            return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
        }
        offset += entries[i].payloadSize;
    }
    for (i = 0u; i < entryCount; i++)
    {
        size_t streamSize = EntryStreamSize(&entries[i]);
        size_t aligned = (offset + 3u) & ~(size_t)3u;
        if (aligned < offset)
        {
            NoteMismatch(diagnostics, entries[i].canonicalName,
                         Gen3ResourceType_Name(entries[i].type),
                         entries[i].payloadSize, entries[i].payloadSize);
            return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
        }
        offset = aligned;
        if (SIZE_MAX - offset < streamSize)
        {
            NoteMismatch(diagnostics, entries[i].canonicalName,
                         Gen3ResourceType_Name(entries[i].type),
                         entries[i].payloadSize, entries[i].payloadSize);
            return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
        }
        offset += streamSize;
    }
    total = sizeof(*image) + offset;
    if (total < sizeof(*image) || SIZE_MAX - sizeof(*image) < offset)
    {
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
        return EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH;
    }

    image = (struct EmeraldResourceCompatibilityImage *)malloc(total);
    if (image == NULL)
    {
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
        return EMERALD_COMPAT_ERR_OUT_OF_MEMORY;
    }
    memset(image, 0, total);
    image->entryCount = entryCount;
    image->entryTableOffset = 0u;
    image->arenaSize = offset;
    table = (struct EmeraldResourceCompatImageEntry *)(void *)image->bytes;

    /* Phase 3: fill. Names (packed), canonicals (4-aligned), streams
     * (4-aligned); per-entry offsets recorded in the table. The streams are
     * literal-only and byte-deterministic for the canonical payload (§5). */
    offset = entryTableSize;
    for (i = 0u; i < entryCount; i++)
    {
        size_t nameLen = strlen(entries[i].canonicalName);
        table[i].type = (uint32_t)entries[i].type;
        /* R9 §5: for GBA_LZ entries the payload is the ENCODED stream, so the
         * entry's decoded size is its expected size (already verified against
         * the stream's LZ77 header), not the payload length. For LZ/RAW the
         * canonical payload IS the decoded bytes. */
        table[i].decodedSize = (entries[i].encoding == EMERALD_COMPAT_ENTRY_GBA_LZ)
            ? ((entries[i].expectedSize != 0u)
                   ? entries[i].expectedSize
                   : ExpectedSizeForType(entries[i].type))
            : entries[i].payloadSize;
        table[i].nameOffset = offset;
        memcpy(image->bytes + offset, entries[i].canonicalName, nameLen + 1u);
        offset += nameLen + 1u;
    }
    image->nameArenaOffset = entryTableSize;
    image->canonicalArenaOffset = offset;
    for (i = 0u; i < entryCount; i++)
    {
        offset = (offset + 3u) & ~(size_t)3u;
        table[i].canonicalOffset = offset;
        memcpy(image->bytes + offset, entries[i].payload,
               (size_t)entries[i].payloadSize);
        offset += entries[i].payloadSize;
    }
    image->streamArenaOffset = offset;
    for (i = 0u; i < entryCount; i++)
    {
        enum Gen3LzResult lzResult;

        offset = (offset + 3u) & ~(size_t)3u;
        table[i].streamOffset = offset;
        table[i].streamSize = EntryStreamSize(&entries[i]);
        if (entries[i].encoding == EMERALD_COMPAT_ENTRY_RAW
         || entries[i].encoding == EMERALD_COMPAT_ENTRY_GBA_LZ)
        {
            /* R8 back sheets: the stream IS the payload bytes verbatim - the
             * consumers are the sprite pipeline (raw pixel copy into OBJ VRAM)
             * and DecompressTrainerBackPic (LZ77-decodes the same raw bytes it
             * decodes on the GBA build). R9 Pokémon battle: the payload IS the
             * GBA LZ77 stream (retail-ROM bytes), served verbatim so the
             * existing decompressors decode exactly what the GBA build decodes. */
            memcpy(image->bytes + offset, entries[i].payload,
                   (size_t)entries[i].payloadSize);
        }
        else
        {
            lzResult = Gen3LzLiteral_Encode(entries[i].payload,
                                            entries[i].payloadSize,
                                            image->bytes + offset,
                                            table[i].streamSize, NULL);
            if (lzResult != GEN3_LZ_OK)
            {
                free(image);
                NoteMismatch(diagnostics, entries[i].canonicalName,
                             Gen3ResourceType_Name(entries[i].type),
                             entries[i].payloadSize, entries[i].payloadSize);
                return EMERALD_COMPAT_ERR_ENCODE_FAILED;
            }
        }
        offset += table[i].streamSize;
    }

    if (diagnostics != NULL)
        snprintf(diagnostics->stage, sizeof(diagnostics->stage), "build");
    *outImage = image;
    return EMERALD_COMPAT_OK;
}

void EmeraldResourceCompatImage_Destroy(struct EmeraldResourceCompatibilityImage *image)
{
    free(image);
}

size_t EmeraldResourceCompatImage_GetEntryCount(
    const struct EmeraldResourceCompatibilityImage *image)
{
    return image != NULL ? image->entryCount : 0u;
}

static const uint8_t *EntryBytes(const struct EmeraldResourceCompatibilityImage *image,
                                 size_t index)
{
    if (image == NULL || index >= image->entryCount)
        return NULL;
    return image->bytes;
}

const char *EmeraldResourceCompatImage_GetEntryName(
    const struct EmeraldResourceCompatibilityImage *image, size_t index)
{
    const struct EmeraldResourceCompatImageEntry *entry;
    if (EntryBytes(image, index) == NULL)
        return NULL;
    entry = EntryAt(image, index);
    return (const char *)(const void *)(image->bytes + entry->nameOffset);
}

enum Gen3ResourceType EmeraldResourceCompatImage_GetEntryType(
    const struct EmeraldResourceCompatibilityImage *image, size_t index)
{
    const struct EmeraldResourceCompatImageEntry *entry;
    if (EntryBytes(image, index) == NULL)
        return GEN3_RESOURCE_TYPE_INVALID;
    entry = EntryAt(image, index);
    return (enum Gen3ResourceType)entry->type;
}

const uint8_t *EmeraldResourceCompatImage_GetStream(
    const struct EmeraldResourceCompatibilityImage *image, size_t index)
{
    const struct EmeraldResourceCompatImageEntry *entry;
    if (EntryBytes(image, index) == NULL)
        return NULL;
    entry = EntryAt(image, index);
    return image->bytes + entry->streamOffset;
}

size_t EmeraldResourceCompatImage_GetStreamSize(
    const struct EmeraldResourceCompatibilityImage *image, size_t index)
{
    const struct EmeraldResourceCompatImageEntry *entry;
    if (EntryBytes(image, index) == NULL)
        return 0u;
    entry = EntryAt(image, index);
    return entry->streamSize;
}

uint32_t EmeraldResourceCompatImage_GetDecodedSize(
    const struct EmeraldResourceCompatibilityImage *image, size_t index)
{
    const struct EmeraldResourceCompatImageEntry *entry;
    if (EntryBytes(image, index) == NULL)
        return 0u;
    entry = EntryAt(image, index);
    return entry->decodedSize;
}

const uint8_t *EmeraldResourceCompatImage_GetCanonical(
    const struct EmeraldResourceCompatibilityImage *image, size_t index)
{
    const struct EmeraldResourceCompatImageEntry *entry;
    if (EntryBytes(image, index) == NULL)
        return NULL;
    entry = EntryAt(image, index);
    return image->bytes + entry->canonicalOffset;
}

uint32_t EmeraldResourceCompatImage_GetCanonicalSize(
    const struct EmeraldResourceCompatibilityImage *image, size_t index)
{
    const struct EmeraldResourceCompatImageEntry *entry;
    if (EntryBytes(image, index) == NULL)
        return 0u;
    entry = EntryAt(image, index);
    return entry->decodedSize;
}
