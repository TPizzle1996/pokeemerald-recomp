#include "gen3/resources/resource_pack.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_content_digest.h"

#include "resource_pack_internal.h"
#include "gen3/resources/sha256.h"

/* ------------------------------------------------------------------ */
/* Shared bytecode / arithmetic helpers (declared in the internal hdr) */
/* ------------------------------------------------------------------ */

void Gen3PackPutU16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)(value >> 8u);
}

void Gen3PackPutU32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8u) & 0xFFu);
    out[2] = (uint8_t)((value >> 16u) & 0xFFu);
    out[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

void Gen3PackPutU64(uint8_t *out, uint64_t value)
{
    size_t i;
    for (i = 0; i < 8u; i++)
        out[i] = (uint8_t)((value >> (8u * i)) & 0xFFu);
}

uint16_t Gen3PackGetU16(const uint8_t *in)
{
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8u));
}

uint32_t Gen3PackGetU32(const uint8_t *in)
{
    return (uint32_t)in[0]
         | ((uint32_t)in[1] << 8u)
         | ((uint32_t)in[2] << 16u)
         | ((uint32_t)in[3] << 24u);
}

uint64_t Gen3PackGetU64(const uint8_t *in)
{
    uint64_t value = 0;
    size_t i;
    for (i = 0; i < 8u; i++)
        value |= (uint64_t)in[i] << (8u * i);
    return value;
}

bool Gen3PackAddU64(uint64_t left, uint64_t right, uint64_t *out)
{
    if (out == NULL)
        return false;
    if (left > UINT64_MAX - right)
        return false;
    *out = left + right;
    return true;
}

bool Gen3PackMulU64(uint64_t left, uint64_t right, uint64_t *out)
{
    if (out == NULL)
        return false;
    if (left != 0u && right > UINT64_MAX / left)
        return false;
    *out = left * right;
    return true;
}

bool Gen3PackAlign16U64(uint64_t value, uint64_t *out)
{
    if (out == NULL)
        return false;
    if (value > UINT64_MAX - 15u)
        return false;
    *out = (value + 15u) & ~(uint64_t)15u;
    return true;
}

int Gen3PackCompareNameBytes(const uint8_t *left, size_t leftLength,
                             const uint8_t *right, size_t rightLength)
{
    size_t minimum = leftLength < rightLength ? leftLength : rightLength;
    int comparison = memcmp(left, right, minimum);
    if (comparison != 0)
        return comparison;
    if (leftLength < rightLength)
        return -1;
    if (leftLength > rightLength)
        return 1;
    return 0;
}

bool Gen3PackBytesAllZero(const uint8_t *bytes, size_t size)
{
    size_t i;
    if (bytes == NULL && size != 0u)
        return false;
    for (i = 0; i < size; i++)
    {
        if (bytes[i] != 0u)
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* On-disk type-code mapping                                           */
/* ------------------------------------------------------------------ */

enum Gen3ResourcePackTypeCode Gen3ResourcePack_TypeToCode(enum Gen3ResourceType type)
{
    if ((int)type <= (int)GEN3_RESOURCE_TYPE_INVALID
     || (int)type > (int)GEN3_RESOURCE_TYPE_BINARY)
        return GEN3_PACK_TYPE_INVALID;
    return (enum Gen3ResourcePackTypeCode)(uint32_t)type;
}

enum Gen3ResourceType Gen3ResourcePack_TypeFromCode(enum Gen3ResourcePackTypeCode code)
{
    if ((int)code <= (int)GEN3_PACK_TYPE_INVALID
     || (int)code > (int)GEN3_PACK_TYPE_BINARY)
        return GEN3_RESOURCE_TYPE_INVALID;
    return (enum Gen3ResourceType)(uint32_t)code;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

void Gen3ResourcePackDiagnostics_Init(struct Gen3ResourcePackDiagnosticList *list)
{
    if (list != NULL)
        memset(list, 0, sizeof(*list));
}

void Gen3ResourcePackDiagnostics_Destroy(struct Gen3ResourcePackDiagnosticList *list)
{
    if (list == NULL)
        return;
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool GrowList(struct Gen3ResourcePackDiagnosticList *list, size_t required)
{
    size_t next;
    struct Gen3ResourcePackDiagnostic *resized;
    if (list->capacity >= required)
        return true;
    next = list->capacity == 0u ? 8u : list->capacity;
    while (next < required)
    {
        if (next > SIZE_MAX / 2u)
            return false;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*list->items))
        return false;
    resized = realloc(list->items, next * sizeof(*list->items));
    if (resized == NULL)
        return false;
    list->items = resized;
    list->capacity = next;
    return true;
}

bool Gen3ResourcePackDiagnostics_Append(struct Gen3ResourcePackDiagnosticList *list,
                                        enum Gen3ResourcePackStage stage,
                                        enum Gen3ResourcePackError error,
                                        size_t entryIndex,
                                        const char *canonicalName)
{
    struct Gen3ResourcePackDiagnostic *item;
    if (list == NULL)
        return true;
    if (!GrowList(list, list->count + 1u))
        return false;
    item = &list->items[list->count++];
    memset(item, 0, sizeof(*item));
    item->stage = stage;
    item->error = error;
    item->entryIndex = entryIndex;
    if (canonicalName != NULL && canonicalName[0] != '\0')
    {
        size_t length = strlen(canonicalName);
        if (length >= sizeof(item->canonicalName))
            length = sizeof(item->canonicalName) - 1u;
        memcpy(item->canonicalName, canonicalName, length);
        item->canonicalName[length] = '\0';
        item->hasName = true;
    }
    return true;
}

const char *Gen3ResourcePackError_Describe(enum Gen3ResourcePackError error)
{
    static const char *const descriptions[] =
    {
        "no error",
        "invalid argument",
        "out of memory",
        "bad magic",
        "bad header size",
        "unsupported format version",
        "bad endian tag",
        "bad flags",
        "nonzero reserved field",
        "truncated header",
        "truncated TOC",
        "TOC size/count mismatch",
        "section overlap",
        "section outside file",
        "section order/layout violation",
        "arithmetic overflow",
        "file size mismatch or trailing garbage",
        "too many entries",
        "bad name offset",
        "invalid canonical name",
        "canonical name too long",
        "stable key does not match name",
        "duplicate canonical name",
        "duplicate stable key with different name",
        "TOC not sorted bytewise by canonical name",
        "invalid resource-type code",
        "invalid schema",
        "invalid representation code",
        "invalid source-encoding code",
        "payload offset before payload section",
        "payload range overflow",
        "misaligned payload offset",
        "bad payload layout",
        "overlapping payload ranges",
        "payload too large",
        "canonical payload hash mismatch",
        "TOC hash mismatch",
        "names hash mismatch",
        "payload-section hash mismatch",
        "logical pack digest mismatch",
        "header hash mismatch",
        "malformed required metadata",
        "bad game ID padding/encoding",
        "source range exceeds source ROM",
        "duplicate payload range",
        "total payload too large",
        "pack too large",
        "catalog not supplied",
        "resource unknown to catalog",
        "resource type mismatch with catalog",
        "resource schema mismatch with catalog",
        "file I/O error",
    };
    if ((int)error < 0 || (size_t)error >= sizeof(descriptions) / sizeof(descriptions[0]))
        return "unknown pack error";
    return descriptions[(size_t)error];
}

/* ------------------------------------------------------------------ */
/* Parsed pack internals                                               */
/* ------------------------------------------------------------------ */

struct Gen3ResourcePack
{
    uint8_t *ownedBytes;
    size_t size;
    struct Gen3ResourcePackEntry *entries;
    size_t entryCount;
    char **names;
    struct Gen3ResourcePackProfile profile;
    uint8_t logicalDigest[GEN3_PACK_SHA256_SIZE];
};

static void PackDestroy(struct Gen3ResourcePack *pack)
{
    size_t i;
    if (pack == NULL)
        return;
    for (i = 0; i < pack->entryCount; i++)
        free(pack->names[i]);
    free(pack->names);
    free(pack->entries);
    free(pack->ownedBytes);
    free(pack);
}

bool Gen3PackIsPrintableAscii(const uint8_t *bytes, size_t size)
{
    size_t i;
    if (bytes == NULL && size != 0u)
        return false;
    for (i = 0; i < size; i++)
    {
        if (bytes[i] < 0x20u || bytes[i] > 0x7Eu)
            return false;
    }
    return true;
}

bool Gen3PackGameIdValid(const uint8_t gameId[GEN3_PACK_GAME_ID_SIZE])
{
    size_t i;
    if (gameId == NULL)
        return false;
    if (gameId[0] == 0u)
        return false;
    for (i = 0; i < GEN3_PACK_GAME_ID_SIZE; i++)
    {
        if (gameId[i] == 0u)
            break;
        if (gameId[i] < 0x20u || gameId[i] > 0x7Eu)
            return false;
    }
    if (i == GEN3_PACK_GAME_ID_SIZE)
        return false; /* never NUL-terminated */
    for (; i < GEN3_PACK_GAME_ID_SIZE; i++)
    {
        if (gameId[i] != 0u)
            return false; /* trailing garbage after the NUL */
    }
    return true;
}

static bool ValidateGameFields(const uint8_t *header)
{
    const uint8_t *gameCode = header + GEN3_PACK_OFF_GAME_CODE;
    const uint8_t *makerCode = header + GEN3_PACK_OFF_MAKER_CODE;
    const uint8_t *gameId = header + GEN3_PACK_OFF_GAME_ID;
    return Gen3PackIsPrintableAscii(gameCode, 4u)
        && Gen3PackIsPrintableAscii(makerCode, 2u)
        && Gen3PackGameIdValid(gameId);
}

static bool HashMatches(const uint8_t *stored, const uint8_t *bytes, size_t size)
{
    struct Gen3Sha256Context context;
    uint8_t digest[GEN3_PACK_SHA256_SIZE];
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, bytes, size);
    Gen3Sha256_Final(&context, digest);
    return memcmp(digest, stored, GEN3_PACK_SHA256_SIZE) == 0;
}

static bool ValidateHeaderMetadata(const uint8_t *header)
{
    uint64_t sourceRomSize = Gen3PackGetU64(header + GEN3_PACK_OFF_ROM_SIZE);
    if (sourceRomSize == 0u || sourceRomSize > GEN3_PACK_MAX_ROM_SIZE)
        return false;
    if (Gen3PackGetU32(header + GEN3_PACK_OFF_BASE_PACK_VERSION) == 0u
     || Gen3PackGetU32(header + GEN3_PACK_OFF_CATALOG_VERSION) == 0u
     || Gen3PackGetU32(header + GEN3_PACK_OFF_EXTRACTION_MANIFEST_VER) == 0u
     || Gen3PackGetU32(header + GEN3_PACK_OFF_CANONICAL_REP_VERSION) == 0u)
        return false;
    if (!Gen3PackBytesAllZero(header + GEN3_PACK_OFF_ROM_SHA1, GEN3_PACK_SHA1_SIZE)
     && !Gen3PackBytesAllZero(header + GEN3_PACK_OFF_ROM_SHA256, GEN3_PACK_SHA256_SIZE)
     && !Gen3PackBytesAllZero(header + GEN3_PACK_OFF_CATALOG_SHA256, GEN3_PACK_SHA256_SIZE)
     && !Gen3PackBytesAllZero(header + GEN3_PACK_OFF_MANIFEST_SHA256, GEN3_PACK_SHA256_SIZE))
        return true;
    return false;
}

static bool RangeOverlaps(uint64_t aStart, uint64_t aEnd,
                          uint64_t bStart, uint64_t bEnd)
{
    return aStart < bEnd && bStart < aEnd;
}

static bool CheckAggregatePayloadHash(const struct Gen3ResourcePack *pack)
{
    struct Gen3Sha256Context context;
    uint8_t digest[GEN3_PACK_SHA256_SIZE];
    size_t i;
    Gen3Sha256_Init(&context);
    for (i = 0; i < pack->entryCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry = &pack->entries[i];
        Gen3Sha256_Update(&context, entry->payload, entry->payloadSize);
    }
    Gen3Sha256_Final(&context, digest);
    return memcmp(digest, pack->ownedBytes + GEN3_PACK_OFF_PAYLOAD_SHA256,
                  GEN3_PACK_SHA256_SIZE) == 0;
}

static bool CheckLogicalDigest(const struct Gen3ResourcePack *pack)
{
    const uint8_t *header = pack->ownedBytes;
    struct Gen3ResourcePackLogicalInput input;
    uint8_t digest[GEN3_PACK_SHA256_SIZE];
    memset(&input, 0, sizeof(input));
    input.formatVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_FORMAT_VERSION);
    input.apiMajor = Gen3PackGetU32(header + GEN3_PACK_OFF_API_MAJOR);
    input.apiMinor = Gen3PackGetU32(header + GEN3_PACK_OFF_API_MINOR);
    input.apiPatch = Gen3PackGetU32(header + GEN3_PACK_OFF_API_PATCH);
    input.basePackVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_BASE_PACK_VERSION);
    input.catalogVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_CATALOG_VERSION);
    input.extractionManifestVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_EXTRACTION_MANIFEST_VER);
    input.canonicalRepresentationVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_CANONICAL_REP_VERSION);
    input.sourceRomSize = Gen3PackGetU64(header + GEN3_PACK_OFF_ROM_SIZE);
    input.sourceRomSha1 = header + GEN3_PACK_OFF_ROM_SHA1;
    input.sourceRomSha256 = header + GEN3_PACK_OFF_ROM_SHA256;
    input.gameCode = header + GEN3_PACK_OFF_GAME_CODE;
    input.makerCode = header + GEN3_PACK_OFF_MAKER_CODE;
    input.softwareRevision = header[GEN3_PACK_OFF_SOFTWARE_REVISION];
    input.reserved = header[GEN3_PACK_OFF_RESERVED1];
    input.catalogSha256 = header + GEN3_PACK_OFF_CATALOG_SHA256;
    input.extractionManifestSha256 = header + GEN3_PACK_OFF_MANIFEST_SHA256;
    input.tocSha256 = header + GEN3_PACK_OFF_TOC_SHA256;
    input.namesSha256 = header + GEN3_PACK_OFF_NAMES_SHA256;
    input.payloadSha256 = header + GEN3_PACK_OFF_PAYLOAD_SHA256;
    if (!Gen3ResourceContentDigest_LogicalPack(&input, digest))
        return false;
    return memcmp(digest, header + GEN3_PACK_OFF_LOGICAL_SHA256,
                  GEN3_PACK_SHA256_SIZE) == 0;
}

static bool CheckHeaderHash(const uint8_t *header)
{
    uint8_t copy[GEN3_PACK_HEADER_SIZE];
    uint8_t digest[GEN3_PACK_SHA256_SIZE];
    struct Gen3Sha256Context context;
    memcpy(copy, header, sizeof(copy));
    memset(copy + GEN3_PACK_OFF_HEADER_SHA256, 0, GEN3_PACK_SHA256_SIZE);
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, copy, sizeof(copy));
    Gen3Sha256_Final(&context, digest);
    return memcmp(digest, header + GEN3_PACK_OFF_HEADER_SHA256,
                  GEN3_PACK_SHA256_SIZE) == 0;
}

/* ------------------------------------------------------------------ */
/* Parse + validate                                                    */
/* ------------------------------------------------------------------ */

static enum Gen3ResourcePackError ParseHeaderAndSections(
    const uint8_t *data, size_t size,
    const uint8_t **outToc, const uint8_t **outNames, const uint8_t **outPayload,
    uint64_t *outPayloadOffset, uint64_t *outPayloadEnd,
    uint64_t *outSourceRomSize,
    struct Gen3ResourcePackDiagnosticList *diagnostics)
{
    const uint8_t *header = data;
    uint64_t tocOffset;
    uint64_t tocSize;
    uint64_t namesOffset;
    uint64_t namesSize;
    uint64_t payloadOffset;
    uint64_t payloadSize;
    uint64_t tocEnd;
    uint64_t namesEnd;
    uint64_t payloadEnd;
    uint64_t declaredFileSize;
    uint64_t calcTocSize;
    uint64_t expectedPayload;
    uint32_t entryCount;
    uint32_t entrySize;

    if (size < GEN3_PACK_HEADER_SIZE)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_TRUNCATED_HEADER, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_TRUNCATED_HEADER;
    }
    if (memcmp(header + GEN3_PACK_OFF_MAGIC, GEN3_PACK_MAGIC_BYTES, 8u) != 0)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_MAGIC, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_MAGIC;
    }
    if (Gen3PackGetU32(header + GEN3_PACK_OFF_HEADER_SIZE) != GEN3_PACK_HEADER_SIZE)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_HEADER_SIZE, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_HEADER_SIZE;
    }
    if (Gen3PackGetU32(header + GEN3_PACK_OFF_FORMAT_VERSION) != GEN3_PACK_FORMAT_VERSION_1)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_UNSUPPORTED_VERSION, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_UNSUPPORTED_VERSION;
    }
    if (Gen3PackGetU32(header + GEN3_PACK_OFF_ENDIAN_TAG) != GEN3_PACK_ENDIAN_TAG)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_ENDIAN_TAG, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_ENDIAN_TAG;
    }
    if (Gen3PackGetU32(header + GEN3_PACK_OFF_FLAGS) != 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_FLAGS, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_FLAGS;
    }
    if (Gen3PackGetU32(header + GEN3_PACK_OFF_RESERVED0) != 0u
     || header[GEN3_PACK_OFF_RESERVED1] != 0u
     || !Gen3PackBytesAllZero(header + GEN3_PACK_OFF_RESERVED2, GEN3_PACK_RESERVED2_SIZE))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_NONZERO_RESERVED, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_NONZERO_RESERVED;
    }
    if (!ValidateGameFields(header))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_GAME_ID, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_GAME_ID;
    }
    if (!ValidateHeaderMetadata(header))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_METADATA, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_METADATA;
    }

    entrySize = Gen3PackGetU32(header + GEN3_PACK_OFF_ENTRY_SIZE);
    entryCount = Gen3PackGetU32(header + GEN3_PACK_OFF_ENTRY_COUNT);
    if (entrySize != GEN3_PACK_ENTRY_SIZE)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_TOC_SIZE_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_TOC_SIZE_MISMATCH;
    }
    if (entryCount > GEN3_PACK_MAX_ENTRIES)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_TOO_MANY_ENTRIES, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_TOO_MANY_ENTRIES;
    }
    if (!Gen3PackMulU64((uint64_t)entryCount, GEN3_PACK_ENTRY_SIZE, &calcTocSize))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
    }
    tocSize = Gen3PackGetU64(header + GEN3_PACK_OFF_TOC_SIZE);
    if (tocSize != calcTocSize)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_TOC_SIZE_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_TOC_SIZE_MISMATCH;
    }
    declaredFileSize = Gen3PackGetU64(header + GEN3_PACK_OFF_FILE_SIZE);
    if (declaredFileSize != (uint64_t)size)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_SIZE_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_SIZE_MISMATCH;
    }

    tocOffset = Gen3PackGetU64(header + GEN3_PACK_OFF_TOC_OFFSET);
    namesOffset = Gen3PackGetU64(header + GEN3_PACK_OFF_NAMES_OFFSET);
    namesSize = Gen3PackGetU64(header + GEN3_PACK_OFF_NAMES_SIZE);
    payloadOffset = Gen3PackGetU64(header + GEN3_PACK_OFF_PAYLOAD_OFFSET);
    payloadSize = Gen3PackGetU64(header + GEN3_PACK_OFF_PAYLOAD_SIZE);

    if (!Gen3PackAddU64(tocOffset, tocSize, &tocEnd)
     || !Gen3PackAddU64(namesOffset, namesSize, &namesEnd)
     || !Gen3PackAddU64(payloadOffset, payloadSize, &payloadEnd))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
    }
    if (tocEnd > (uint64_t)size)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_TRUNCATED_TOC, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_TRUNCATED_TOC;
    }
    if (namesEnd > (uint64_t)size || payloadEnd > (uint64_t)size)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_SECTION_OUT_OF_FILE, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_SECTION_OUT_OF_FILE;
    }
    if (RangeOverlaps(0u, GEN3_PACK_HEADER_SIZE, tocOffset, tocEnd)
     || RangeOverlaps(0u, GEN3_PACK_HEADER_SIZE, namesOffset, namesEnd)
     || RangeOverlaps(0u, GEN3_PACK_HEADER_SIZE, payloadOffset, payloadEnd)
     || RangeOverlaps(tocOffset, tocEnd, namesOffset, namesEnd)
     || RangeOverlaps(tocOffset, tocEnd, payloadOffset, payloadEnd)
     || RangeOverlaps(namesOffset, namesEnd, payloadOffset, payloadEnd))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_SECTION_OVERLAP, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_SECTION_OVERLAP;
    }
    if (tocOffset != GEN3_PACK_HEADER_SIZE
     || namesOffset != tocEnd
     || !Gen3PackAlign16U64(namesEnd, &expectedPayload)
     || payloadOffset != expectedPayload)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_SECTION_ORDER, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_SECTION_ORDER;
    }
    if (declaredFileSize != payloadEnd)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_SIZE_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_SIZE_MISMATCH;
    }

    *outToc = data + (size_t)tocOffset;
    *outNames = data + (size_t)namesOffset;
    *outPayload = data + (size_t)payloadOffset;
    *outPayloadOffset = payloadOffset;
    *outPayloadEnd = payloadEnd;
    *outSourceRomSize = Gen3PackGetU64(header + GEN3_PACK_OFF_ROM_SIZE);
    return GEN3_PACK_OK;
}

/* Validates one TOC entry and fills pack->entries[i] / pack->names[i].
 * Returns GEN3_PACK_OK on success. On failure the caller releases the pack
 * (owned names are freed there). */
static enum Gen3ResourcePackError ParseEntry(
    struct Gen3ResourcePack *pack, const uint8_t *toc, const uint8_t *names,
    const uint8_t *payloadSection, uint64_t payloadSectionStart, uint64_t payloadSectionEnd,
    uint64_t sourceRomSize, size_t index,
    struct Gen3ResourcePackDiagnosticList *diagnostics)
{
    const uint8_t *entry = toc + index * GEN3_PACK_ENTRY_SIZE;
    struct Gen3ResourcePackEntry *parsed = &pack->entries[index];
    const Gen3ResourceKey *storedKey = (const Gen3ResourceKey *)(entry + GEN3_PACK_ENTRY_OFF_KEY);
    uint32_t nameOffset = Gen3PackGetU32(entry + GEN3_PACK_ENTRY_OFF_NAME_OFFSET);
    uint16_t nameLength = Gen3PackGetU16(entry + GEN3_PACK_ENTRY_OFF_NAME_LENGTH);
    uint16_t typeCode = Gen3PackGetU16(entry + GEN3_PACK_ENTRY_OFF_TYPE_CODE);
    uint32_t schema = Gen3PackGetU32(entry + GEN3_PACK_ENTRY_OFF_SCHEMA);
    uint32_t flags = Gen3PackGetU32(entry + GEN3_PACK_ENTRY_OFF_FLAGS);
    uint32_t representationCode = Gen3PackGetU32(entry + GEN3_PACK_ENTRY_OFF_REPRESENTATION);
    uint32_t sourceEncodingCode = Gen3PackGetU32(entry + GEN3_PACK_ENTRY_OFF_SOURCE_ENCODING);
    uint64_t payloadOffset = Gen3PackGetU64(entry + GEN3_PACK_ENTRY_OFF_PAYLOAD_OFFSET);
    uint64_t payloadSize = Gen3PackGetU64(entry + GEN3_PACK_ENTRY_OFF_PAYLOAD_SIZE);
    uint64_t romOffset = Gen3PackGetU64(entry + GEN3_PACK_ENTRY_OFF_ROM_OFFSET);
    uint64_t encodedSize = Gen3PackGetU64(entry + GEN3_PACK_ENTRY_OFF_ENCODED_SIZE);
    const uint8_t *payloadSha256 = entry + GEN3_PACK_ENTRY_OFF_PAYLOAD_SHA256;
    const uint8_t *sourceSha256 = entry + GEN3_PACK_ENTRY_OFF_SOURCE_SHA256;
    char *nameCopy;
    Gen3ResourceKey derivedKey;
    uint64_t runningEnd;
    uint64_t aligned;
    uint64_t sourceEnd;
    size_t nameIndex;

    /* Entry reserved bytes must be zero. */
    if (!Gen3PackBytesAllZero(entry + GEN3_PACK_ENTRY_OFF_RESERVED,
                              GEN3_PACK_ENTRY_SIZE - GEN3_PACK_ENTRY_OFF_RESERVED))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_NONZERO_RESERVED, index, NULL);
        return GEN3_PACK_ERR_NONZERO_RESERVED;
    }

    /* Name length and bounds. name_offset is relative to the names section,
     * so nameOffset+nameLength must fall inside the declared names size. */
    if (nameLength == 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_INVALID_CANONICAL_NAME, index, NULL);
        return GEN3_PACK_ERR_INVALID_CANONICAL_NAME;
    }
    if (nameLength > GEN3_PACK_NAME_MAX)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_NAME_TOO_LONG, index, NULL);
        return GEN3_PACK_ERR_NAME_TOO_LONG;
    }
    {
        uint64_t namesSize = Gen3PackGetU64(pack->ownedBytes + GEN3_PACK_OFF_NAMES_SIZE);
        if ((uint64_t)nameOffset + (uint64_t)nameLength > namesSize)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_BAD_NAME_OFFSET, index, NULL);
            return GEN3_PACK_ERR_BAD_NAME_OFFSET;
        }
    }
    nameIndex = (size_t)nameOffset;

    /* Copy the canonical name into owned NUL-terminated memory. */
    nameCopy = malloc((size_t)nameLength + 1u);
    if (nameCopy == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, index, NULL);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    memcpy(nameCopy, names + nameIndex, nameLength);
    nameCopy[nameLength] = '\0';
    pack->names[index] = nameCopy;

    /* Canonical-name validity. */
    if (Gen3ResourceId_ValidateCanonicalName(nameCopy) != GEN3_RESOURCE_NAME_VALID)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_INVALID_CANONICAL_NAME, index, nameCopy);
        return GEN3_PACK_ERR_INVALID_CANONICAL_NAME;
    }

    /* Duplicate names / sort order (bytewise). */
    if (index > 0u)
    {
        size_t previousLength = strlen(pack->names[index - 1u]);
        int order = Gen3PackCompareNameBytes(
            (const uint8_t *)pack->names[index - 1u], previousLength,
            (const uint8_t *)nameCopy, (size_t)nameLength);
        if (order == 0)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_DUPLICATE_NAME, index, nameCopy);
            return GEN3_PACK_ERR_DUPLICATE_NAME;
        }
        if (order > 0)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_UNSORTED_TOC, index, nameCopy);
            return GEN3_PACK_ERR_UNSORTED_TOC;
        }
        if (memcmp(pack->entries[index - 1u].key.bytes, storedKey->bytes,
                   GEN3_RESOURCE_KEY_SIZE) == 0)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_DUPLICATE_KEY, index, nameCopy);
            return GEN3_PACK_ERR_DUPLICATE_KEY;
        }
    }

    /* Stable key recomputation. */
    Gen3ResourceId_DeriveKey(nameCopy, &derivedKey);
    if (memcmp(derivedKey.bytes, storedKey->bytes, GEN3_RESOURCE_KEY_SIZE) != 0)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_KEY_MISMATCH, index, nameCopy);
        return GEN3_PACK_ERR_KEY_MISMATCH;
    }

    /* Codes. */
    {
        enum Gen3ResourceType type = Gen3ResourcePack_TypeFromCode((enum Gen3ResourcePackTypeCode)typeCode);
        if (type == GEN3_RESOURCE_TYPE_INVALID)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_INVALID_TYPE_CODE, index, nameCopy);
            return GEN3_PACK_ERR_INVALID_TYPE_CODE;
        }
        parsed->type = type;
    }
    if (schema == 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_INVALID_SCHEMA, index, nameCopy);
        return GEN3_PACK_ERR_INVALID_SCHEMA;
    }
    if (representationCode != (uint32_t)GEN3_PACK_REPRESENTATION_DECODED
     && representationCode != (uint32_t)GEN3_PACK_REPRESENTATION_RAW)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_INVALID_REPRESENTATION_CODE, index, nameCopy);
        return GEN3_PACK_ERR_INVALID_REPRESENTATION_CODE;
    }
    if (sourceEncodingCode != (uint32_t)GEN3_PACK_SOURCE_ENCODING_GBA_LZ77
     && sourceEncodingCode != (uint32_t)GEN3_PACK_SOURCE_ENCODING_RAW)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_INVALID_ENCODING_CODE, index, nameCopy);
        return GEN3_PACK_ERR_INVALID_ENCODING_CODE;
    }
    if ((flags & ~(uint32_t)GEN3_PACK_FLAG_REQUIRED_FOR_BASE) != 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_FLAGS, index, nameCopy);
        return GEN3_PACK_ERR_BAD_FLAGS;
    }

    /* Provenance metadata consistency. */
    if (encodedSize == 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_METADATA, index, nameCopy);
        return GEN3_PACK_ERR_BAD_METADATA;
    }
    if (Gen3PackBytesAllZero(sourceSha256, GEN3_PACK_SHA256_SIZE))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_METADATA, index, nameCopy);
        return GEN3_PACK_ERR_BAD_METADATA;
    }
    if (!Gen3PackAddU64(romOffset, encodedSize, &sourceEnd)
     || sourceEnd > sourceRomSize)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_SOURCE_RANGE_OVERFLOW, index, nameCopy);
        return GEN3_PACK_ERR_SOURCE_RANGE_OVERFLOW;
    }

    /* Payload placement: dense 16-aligned slots beginning at the payload
     * section start, in TOC order. */
    if (payloadSize > GEN3_PACK_MAX_PAYLOAD_SIZE)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_PAYLOAD_TOO_LARGE, index, nameCopy);
        return GEN3_PACK_ERR_PAYLOAD_TOO_LARGE;
    }
    if (payloadOffset < payloadSectionStart)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_PAYLOAD_OFFSET_BEFORE_SECTION, index, nameCopy);
        return GEN3_PACK_ERR_PAYLOAD_OFFSET_BEFORE_SECTION;
    }
    {
        /* Reconstruct the running aligned offset across prior entries. */
        uint64_t running = 0;
        size_t prior;
        for (prior = 0; prior < index; prior++)
        {
            uint64_t priorSize = Gen3PackGetU64(
                toc + prior * GEN3_PACK_ENTRY_SIZE + GEN3_PACK_ENTRY_OFF_PAYLOAD_SIZE);
            if (!Gen3PackAlign16U64(priorSize, &aligned))
            {
                Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                    GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, index, nameCopy);
                return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
            }
            if (!Gen3PackAddU64(running, aligned, &running))
            {
                Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                    GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, index, nameCopy);
                return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
            }
        }
        if (!Gen3PackAddU64(payloadSectionStart, running, &runningEnd))
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, index, nameCopy);
            return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
        }
        if (payloadOffset != runningEnd)
        {
            if ((payloadOffset & 15u) != 0u)
            {
                Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                    GEN3_PACK_ERR_PAYLOAD_MISALIGNED, index, nameCopy);
                return GEN3_PACK_ERR_PAYLOAD_MISALIGNED;
            }
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_BAD_PAYLOAD_LAYOUT, index, nameCopy);
            return GEN3_PACK_ERR_BAD_PAYLOAD_LAYOUT;
        }
    }
    if (!Gen3PackAddU64(payloadOffset, payloadSize, &aligned)
     || aligned > payloadSectionEnd)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_PAYLOAD_RANGE_OVERFLOW, index, nameCopy);
        return GEN3_PACK_ERR_PAYLOAD_RANGE_OVERFLOW;
    }

    /* Per-entry canonical payload hash. */
    if (!HashMatches(payloadSha256, payloadSection + (size_t)(payloadOffset - payloadSectionStart),
                     (size_t)payloadSize))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH, index, nameCopy);
        return GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH;
    }

    /* Populate the immutable view (all pointers owned by the pack). */
    parsed->key = *storedKey;
    parsed->canonicalName = nameCopy;
    parsed->schema = schema;
    parsed->flags = flags;
    parsed->representation = (enum Gen3ResourcePackRepresentationCode)representationCode;
    parsed->sourceEncoding = (enum Gen3ResourcePackSourceEncodingCode)sourceEncodingCode;
    parsed->payload = payloadSection + (size_t)(payloadOffset - payloadSectionStart);
    parsed->payloadSize = (size_t)payloadSize;
    parsed->sourceRomOffset = romOffset;
    parsed->sourceEncodedSize = encodedSize;
    parsed->payloadSha256 = payloadSha256;
    parsed->sourceEncodedSha256 = sourceSha256;
    return GEN3_PACK_OK;
}

enum Gen3ResourcePackError Gen3ResourcePack_Parse(
    const void *bytes, size_t size,
    struct Gen3ResourcePack **outPack,
    struct Gen3ResourcePackDiagnosticList *diagnostics)
{
    const uint8_t *data = (const uint8_t *)bytes;
    const uint8_t *toc;
    const uint8_t *names;
    const uint8_t *payloadSection;
    uint64_t payloadSectionStart;
    uint64_t payloadSectionEnd;
    uint64_t sourceRomSize;
    uint64_t aggregateEnd;
    uint64_t aligned;
    struct Gen3ResourcePack *pack = NULL;
    enum Gen3ResourcePackError error;
    uint32_t entryCount;
    size_t i;

    if (outPack == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }
    *outPack = NULL;
    if (data == NULL || size == 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }

    error = ParseHeaderAndSections(data, size, &toc, &names, &payloadSection,
                                   &payloadSectionStart, &payloadSectionEnd,
                                   &sourceRomSize, diagnostics);
    if (error != GEN3_PACK_OK)
        return error;

    entryCount = Gen3PackGetU32(data + GEN3_PACK_OFF_ENTRY_COUNT);
    pack = calloc(1, sizeof(*pack));
    if (pack == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    pack->ownedBytes = malloc(size);
    pack->entries = calloc(entryCount, sizeof(*pack->entries));
    pack->names = calloc(entryCount, sizeof(*pack->names));
    if (pack->ownedBytes == NULL || pack->entries == NULL || pack->names == NULL)
    {
        PackDestroy(pack);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    memcpy(pack->ownedBytes, data, size);
    pack->size = size;
    pack->entryCount = entryCount;

    /* Entry views must point at memory owned by the pack: re-anchor every
     * section pointer to pack->ownedBytes (an identical byte-for-byte copy) so
     * the caller's input buffer may be freed immediately after Parse returns.
     * Gen3ResourcePack_OpenFile relies on this - it frees its file buffer as
     * soon as Parse returns, and without the re-anchor every entry's payload,
     * payloadSha256 and sourceEncodedSha256 would dangle (use-after-free). */
    toc = pack->ownedBytes + (size_t)(toc - data);
    names = pack->ownedBytes + (size_t)(names - data);
    payloadSection = pack->ownedBytes + (size_t)(payloadSection - data);

    for (i = 0; i < (size_t)entryCount; i++)
    {
        error = ParseEntry(pack, toc, names, payloadSection,
                           payloadSectionStart, payloadSectionEnd,
                           sourceRomSize, i, diagnostics);
        if (error != GEN3_PACK_OK)
        {
            PackDestroy(pack);
            return error;
        }
    }

    /* Payload section must be exactly tiled by the entries. */
    aggregateEnd = payloadSectionStart;
    for (i = 0; i < (size_t)entryCount; i++)
    {
        if (!Gen3PackAlign16U64(pack->entries[i].payloadSize, &aligned))
        {
            PackDestroy(pack);
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, i, pack->entries[i].canonicalName);
            return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
        }
        if (!Gen3PackAddU64(aggregateEnd, aligned, &aggregateEnd))
        {
            PackDestroy(pack);
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
                GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, i, pack->entries[i].canonicalName);
            return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
        }
    }
    if (aggregateEnd != payloadSectionEnd)
    {
        PackDestroy(pack);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_BAD_PAYLOAD_LAYOUT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_PAYLOAD_LAYOUT;
    }

    if (!CheckAggregatePayloadHash(pack))
    {
        PackDestroy(pack);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_PAYLOAD_SECTION_HASH_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_PAYLOAD_SECTION_HASH_MISMATCH;
    }
    if (!HashMatches(data + GEN3_PACK_OFF_TOC_SHA256, toc,
                     (size_t)Gen3PackGetU64(data + GEN3_PACK_OFF_TOC_SIZE)))
    {
        PackDestroy(pack);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_TOC_HASH_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_TOC_HASH_MISMATCH;
    }
    if (!HashMatches(data + GEN3_PACK_OFF_NAMES_SHA256, names,
                     (size_t)Gen3PackGetU64(data + GEN3_PACK_OFF_NAMES_SIZE)))
    {
        PackDestroy(pack);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_NAMES_HASH_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_NAMES_HASH_MISMATCH;
    }
    if (!CheckLogicalDigest(pack))
    {
        PackDestroy(pack);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_LOGICAL_DIGEST_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_LOGICAL_DIGEST_MISMATCH;
    }
    if (!CheckHeaderHash(data))
    {
        PackDestroy(pack);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_HEADER_HASH_MISMATCH, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_HEADER_HASH_MISMATCH;
    }

    memcpy(pack->logicalDigest, data + GEN3_PACK_OFF_LOGICAL_SHA256,
           GEN3_PACK_SHA256_SIZE);
    {
        const uint8_t *header = data;
        struct Gen3ResourcePackProfile *profile = &pack->profile;
        profile->basePackVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_BASE_PACK_VERSION);
        profile->catalogVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_CATALOG_VERSION);
        profile->extractionManifestVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_EXTRACTION_MANIFEST_VER);
        profile->canonicalRepresentationVersion = Gen3PackGetU32(header + GEN3_PACK_OFF_CANONICAL_REP_VERSION);
        profile->sourceRomSize = Gen3PackGetU64(header + GEN3_PACK_OFF_ROM_SIZE);
        memcpy(profile->sourceRomSha1, header + GEN3_PACK_OFF_ROM_SHA1, GEN3_PACK_SHA1_SIZE);
        memcpy(profile->sourceRomSha256, header + GEN3_PACK_OFF_ROM_SHA256, GEN3_PACK_SHA256_SIZE);
        memcpy(profile->gameCode, header + GEN3_PACK_OFF_GAME_CODE, 4u);
        memcpy(profile->makerCode, header + GEN3_PACK_OFF_MAKER_CODE, 2u);
        profile->softwareRevision = header[GEN3_PACK_OFF_SOFTWARE_REVISION];
        memcpy(profile->gameId, header + GEN3_PACK_OFF_GAME_ID, GEN3_PACK_GAME_ID_SIZE);
        memcpy(profile->catalogSha256, header + GEN3_PACK_OFF_CATALOG_SHA256, GEN3_PACK_SHA256_SIZE);
        memcpy(profile->extractionManifestSha256, header + GEN3_PACK_OFF_MANIFEST_SHA256, GEN3_PACK_SHA256_SIZE);
    }

    *outPack = pack;
    return GEN3_PACK_OK;
}

enum Gen3ResourcePackError Gen3ResourcePack_OpenFile(
    const char *path, struct Gen3ResourcePack **outPack,
    struct Gen3ResourcePackDiagnosticList *diagnostics)
{
    FILE *file;
    long length;
    uint8_t *bytes;
    enum Gen3ResourcePackError error;

    if (path == NULL || outPack == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }
    *outPack = NULL;
    file = fopen(path, "rb");
    if (file == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_FILE_IO, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_FILE_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_FILE_IO, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_FILE_IO;
    }
    length = ftell(file);
    if (length < 0 || (unsigned long)length > SIZE_MAX)
    {
        fclose(file);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_FILE_IO, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_FILE_IO;
    }
    rewind(file);
    bytes = malloc((size_t)length);
    if (bytes == NULL)
    {
        fclose(file);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    if ((size_t)length != 0u
     && fread(bytes, 1u, (size_t)length, file) != (size_t)length)
    {
        free(bytes);
        fclose(file);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_PARSE,
            GEN3_PACK_ERR_FILE_IO, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_FILE_IO;
    }
    fclose(file);
    error = Gen3ResourcePack_Parse(bytes, (size_t)length, outPack, diagnostics);
    free(bytes);
    return error;
}

void Gen3ResourcePack_Destroy(struct Gen3ResourcePack *pack)
{
    PackDestroy(pack);
}

size_t Gen3ResourcePack_GetEntryCount(const struct Gen3ResourcePack *pack)
{
    return pack != NULL ? pack->entryCount : 0u;
}

const struct Gen3ResourcePackEntry *Gen3ResourcePack_GetEntry(
    const struct Gen3ResourcePack *pack, size_t index)
{
    if (pack == NULL || index >= pack->entryCount)
        return NULL;
    return &pack->entries[index];
}

const struct Gen3ResourcePackEntry *Gen3ResourcePack_FindByCanonicalName(
    const struct Gen3ResourcePack *pack, const char *canonicalName)
{
    size_t low;
    size_t high;
    if (pack == NULL || canonicalName == NULL)
        return NULL;
    low = 0u;
    high = pack->entryCount;
    while (low < high)
    {
        size_t middle = low + (high - low) / 2u;
        int comparison = strcmp(canonicalName, pack->names[middle]);
        if (comparison == 0)
            return &pack->entries[middle];
        if (comparison < 0)
            high = middle;
        else
            low = middle + 1u;
    }
    return NULL;
}

const struct Gen3ResourcePackEntry *Gen3ResourcePack_FindByKey(
    const struct Gen3ResourcePack *pack, const Gen3ResourceKey *key)
{
    size_t i;
    if (pack == NULL || key == NULL)
        return NULL;
    for (i = 0; i < pack->entryCount; i++)
    {
        if (memcmp(pack->entries[i].key.bytes, key->bytes, GEN3_RESOURCE_KEY_SIZE) == 0)
            return &pack->entries[i];
    }
    return NULL;
}

bool Gen3ResourcePack_GetProfile(const struct Gen3ResourcePack *pack,
                                 struct Gen3ResourcePackProfile *outProfile)
{
    if (pack == NULL || outProfile == NULL)
        return false;
    *outProfile = pack->profile;
    return true;
}

bool Gen3ResourcePack_GetLogicalDigest(const struct Gen3ResourcePack *pack,
                                       uint8_t outDigest[GEN3_PACK_SHA256_SIZE])
{
    if (pack == NULL || outDigest == NULL)
        return false;
    memcpy(outDigest, pack->logicalDigest, GEN3_PACK_SHA256_SIZE);
    return true;
}

bool Gen3ResourcePack_GetProviderDigest(const struct Gen3ResourcePack *pack,
                                        uint8_t outDigest[GEN3_PACK_SHA256_SIZE])
{
    struct Gen3ResourceContentRecord *records;
    bool ok;
    size_t i;
    if (pack == NULL || outDigest == NULL)
        return false;
    records = malloc(pack->entryCount * sizeof(*records));
    if (records == NULL && pack->entryCount != 0u)
        return false;
    for (i = 0; i < pack->entryCount; i++)
    {
        records[i].key = pack->entries[i].key;
        records[i].type = pack->entries[i].type;
        records[i].schema = pack->entries[i].schema;
        records[i].payloadSize = pack->entries[i].payloadSize;
        records[i].payloadSha256 = pack->entries[i].payloadSha256;
    }
    ok = Gen3ResourceContentDigest_Provider(records, pack->entryCount, outDigest);
    free(records);
    return ok;
}

enum Gen3ResourcePackError Gen3ResourcePack_ValidateCatalog(
    const struct Gen3ResourcePack *pack,
    const struct Gen3ResourceCatalog *catalog,
    struct Gen3ResourcePackDiagnosticList *diagnostics)
{
    size_t i;
    if (pack == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_VALIDATE_CATALOG,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }
    if (catalog == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_VALIDATE_CATALOG,
            GEN3_PACK_ERR_CATALOG_NOT_SUPPLIED, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_CATALOG_NOT_SUPPLIED;
    }
    for (i = 0; i < pack->entryCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry = &pack->entries[i];
        const struct Gen3ResourceContract *contract =
            Gen3ResourceCatalog_Find(catalog, entry->canonicalName);
        if (contract == NULL)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_VALIDATE_CATALOG,
                GEN3_PACK_ERR_UNKNOWN_RESOURCE, i, entry->canonicalName);
            return GEN3_PACK_ERR_UNKNOWN_RESOURCE;
        }
        if (memcmp(entry->key.bytes, contract->key.bytes, GEN3_RESOURCE_KEY_SIZE) != 0)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_VALIDATE_CATALOG,
                GEN3_PACK_ERR_KEY_MISMATCH, i, entry->canonicalName);
            return GEN3_PACK_ERR_KEY_MISMATCH;
        }
        if (entry->type != contract->type)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_VALIDATE_CATALOG,
                GEN3_PACK_ERR_TYPE_MISMATCH, i, entry->canonicalName);
            return GEN3_PACK_ERR_TYPE_MISMATCH;
        }
        if (entry->schema != contract->schema)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_VALIDATE_CATALOG,
                GEN3_PACK_ERR_SCHEMA_MISMATCH, i, entry->canonicalName);
            return GEN3_PACK_ERR_SCHEMA_MISMATCH;
        }
    }
    return GEN3_PACK_OK;
}
