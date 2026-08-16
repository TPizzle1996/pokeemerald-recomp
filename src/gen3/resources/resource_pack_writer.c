#include "gen3/resources/resource_pack_writer.h"

#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_content_digest.h"

#include "resource_pack_internal.h"
#include "gen3/resources/sha256.h"

/* Fixed API/format constants for v1 output. Not configurable: the v1 contract
 * pins these so two writers in the same format always agree. */
#define GEN3_PACK_WRITER_FORMAT_VERSION 1u
#define GEN3_PACK_WRITER_API_MAJOR      1u
#define GEN3_PACK_WRITER_API_MINOR      0u
#define GEN3_PACK_WRITER_API_PATCH      0u

struct BuildEntry
{
    char *canonicalName;
    Gen3ResourceKey key;
    enum Gen3ResourceType type;
    uint32_t schema;
    uint32_t flags;
    enum Gen3ResourcePackRepresentationCode representation;
    enum Gen3ResourcePackSourceEncodingCode sourceEncoding;
    uint8_t *payload;
    size_t payloadSize;
    uint8_t payloadSha256[GEN3_PACK_SHA256_SIZE];
    uint64_t sourceRomOffset;
    uint64_t sourceEncodedSize;
    uint8_t sourceEncodedSha256[GEN3_PACK_SHA256_SIZE];
};

struct Gen3ResourcePackBuild
{
    bool profileSet;
    uint32_t basePackVersion;
    uint32_t catalogVersion;
    uint32_t extractionManifestVersion;
    uint32_t canonicalRepresentationVersion;
    uint64_t sourceRomSize;
    uint8_t sourceRomSha1[GEN3_PACK_SHA1_SIZE];
    uint8_t sourceRomSha256[GEN3_PACK_SHA256_SIZE];
    uint8_t gameCode[4];
    uint8_t makerCode[2];
    uint8_t softwareRevision;
    uint8_t gameId[GEN3_PACK_GAME_ID_SIZE];
    uint8_t catalogSha256[GEN3_PACK_SHA256_SIZE];
    uint8_t extractionManifestSha256[GEN3_PACK_SHA256_SIZE];

    struct BuildEntry *entries;
    size_t entryCount;
    size_t entryCapacity;
    uint64_t totalPayload; /* raw byte sum across entries */

    uint32_t maxEntries;
    size_t maxPayloadSize;
    size_t maxTotalPayload;
    size_t maxPackSize;
};

void Gen3ResourcePackBytes_Destroy(struct Gen3ResourcePackBytes *bytes)
{
    if (bytes == NULL)
        return;
    free(bytes->data);
    memset(bytes, 0, sizeof(*bytes));
}

struct Gen3ResourcePackBuild *Gen3ResourcePackBuild_Create(void)
{
    struct Gen3ResourcePackBuild *build = calloc(1, sizeof(*build));
    if (build == NULL)
        return NULL;
    build->maxEntries = GEN3_PACK_MAX_ENTRIES;
    build->maxPayloadSize = GEN3_PACK_MAX_PAYLOAD_SIZE;
    build->maxTotalPayload = GEN3_PACK_MAX_TOTAL_PAYLOAD;
    build->maxPackSize = GEN3_PACK_MAX_PACK_SIZE;
    return build;
}

void Gen3ResourcePackBuild_Destroy(struct Gen3ResourcePackBuild *build)
{
    size_t i;
    if (build == NULL)
        return;
    for (i = 0; i < build->entryCount; i++)
    {
        free(build->entries[i].canonicalName);
        free(build->entries[i].payload);
    }
    free(build->entries);
    memset(build, 0, sizeof(*build));
    free(build);
}

void Gen3ResourcePackBuild_SetLimits(struct Gen3ResourcePackBuild *build,
                                     uint32_t maxEntries,
                                     size_t maxPayloadSize,
                                     size_t maxTotalPayload,
                                     size_t maxPackSize)
{
    if (build == NULL)
        return;
    if (maxEntries != 0u)
        build->maxEntries = maxEntries;
    if (maxPayloadSize != 0u)
        build->maxPayloadSize = maxPayloadSize;
    if (maxTotalPayload != 0u)
        build->maxTotalPayload = maxTotalPayload;
    if (maxPackSize != 0u)
        build->maxPackSize = maxPackSize;
}

enum Gen3ResourcePackError Gen3ResourcePackBuild_SetProfile(
    struct Gen3ResourcePackBuild *build,
    const struct Gen3ResourcePackProfileInput *profile,
    struct Gen3ResourcePackDiagnosticList *diagnostics)
{
    if (build == NULL || profile == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }
    if (profile->sourceRomSha1 == NULL || profile->sourceRomSha256 == NULL
     || profile->catalogSha256 == NULL || profile->extractionManifestSha256 == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }
    if (profile->basePackVersion == 0u || profile->catalogVersion == 0u
     || profile->extractionManifestVersion == 0u
     || profile->canonicalRepresentationVersion == 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_METADATA, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_METADATA;
    }
    if (profile->sourceRomSize == 0u || profile->sourceRomSize > GEN3_PACK_MAX_ROM_SIZE)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_METADATA, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_METADATA;
    }
    if (Gen3PackBytesAllZero(profile->sourceRomSha1, GEN3_PACK_SHA1_SIZE)
     || Gen3PackBytesAllZero(profile->sourceRomSha256, GEN3_PACK_SHA256_SIZE)
     || Gen3PackBytesAllZero(profile->catalogSha256, GEN3_PACK_SHA256_SIZE)
     || Gen3PackBytesAllZero(profile->extractionManifestSha256, GEN3_PACK_SHA256_SIZE))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_METADATA, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_METADATA;
    }
    if (!Gen3PackIsPrintableAscii((const uint8_t *)profile->gameCode, 4u)
     || !Gen3PackIsPrintableAscii((const uint8_t *)profile->makerCode, 2u)
     || !Gen3PackGameIdValid((const uint8_t *)profile->gameId))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_GAME_ID, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_GAME_ID;
    }

    build->profileSet = true;
    build->basePackVersion = profile->basePackVersion;
    build->catalogVersion = profile->catalogVersion;
    build->extractionManifestVersion = profile->extractionManifestVersion;
    build->canonicalRepresentationVersion = profile->canonicalRepresentationVersion;
    build->sourceRomSize = profile->sourceRomSize;
    build->softwareRevision = profile->softwareRevision;
    memcpy(build->sourceRomSha1, profile->sourceRomSha1, GEN3_PACK_SHA1_SIZE);
    memcpy(build->sourceRomSha256, profile->sourceRomSha256, GEN3_PACK_SHA256_SIZE);
    memcpy(build->gameCode, profile->gameCode, 4u);
    memcpy(build->makerCode, profile->makerCode, 2u);
    memcpy(build->gameId, profile->gameId, GEN3_PACK_GAME_ID_SIZE);
    memcpy(build->catalogSha256, profile->catalogSha256, GEN3_PACK_SHA256_SIZE);
    memcpy(build->extractionManifestSha256, profile->extractionManifestSha256,
           GEN3_PACK_SHA256_SIZE);
    return GEN3_PACK_OK;
}

static bool GrowEntries(struct Gen3ResourcePackBuild *build)
{
    size_t next;
    struct BuildEntry *resized;
    if (build->entryCount < build->entryCapacity)
        return true;
    next = build->entryCapacity == 0u ? 8u : build->entryCapacity * 2u;
    if (next < build->entryCapacity || next > SIZE_MAX / sizeof(*build->entries))
        return false;
    resized = realloc(build->entries, next * sizeof(*build->entries));
    if (resized == NULL)
        return false;
    build->entries = resized;
    build->entryCapacity = next;
    return true;
}

enum Gen3ResourcePackError Gen3ResourcePackBuild_AddEntry(
    struct Gen3ResourcePackBuild *build,
    const struct Gen3ResourcePackEntryInput *input,
    struct Gen3ResourcePackDiagnosticList *diagnostics)
{
    struct BuildEntry *entry;
    Gen3ResourceKey derived;
    struct Gen3Sha256Context context;
    uint8_t digest[GEN3_PACK_SHA256_SIZE];
    uint64_t total;
    size_t i;
    size_t nameLength;

    if (build == NULL || input == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }
    if (input->canonicalName == NULL || input->key == NULL
     || input->canonicalPayload == NULL || input->canonicalPayloadSha256 == NULL
     || input->sourceEncodedSha256 == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }
    if (Gen3ResourceId_ValidateCanonicalName(input->canonicalName) != GEN3_RESOURCE_NAME_VALID)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_CANONICAL_NAME, build->entryCount,
            input->canonicalName);
        return GEN3_PACK_ERR_INVALID_CANONICAL_NAME;
    }
    nameLength = strlen(input->canonicalName);
    if (input->canonicalPayloadSize == 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_METADATA, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_BAD_METADATA;
    }
    if (input->canonicalPayloadSize > build->maxPayloadSize)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_PAYLOAD_TOO_LARGE, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_PAYLOAD_TOO_LARGE;
    }
    if (build->entryCount >= build->maxEntries)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_TOO_MANY_ENTRIES, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_TOO_MANY_ENTRIES;
    }
    if (!Gen3PackAddU64(build->totalPayload, input->canonicalPayloadSize, &total)
     || total > build->maxTotalPayload)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_TOTAL_PAYLOAD_TOO_LARGE, build->entryCount,
            input->canonicalName);
        return GEN3_PACK_ERR_TOTAL_PAYLOAD_TOO_LARGE;
    }

    /* Stable key must match the derived key. */
    Gen3ResourceId_DeriveKey(input->canonicalName, &derived);
    if (memcmp(derived.bytes, input->key->bytes, GEN3_RESOURCE_KEY_SIZE) != 0)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_KEY_MISMATCH, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_KEY_MISMATCH;
    }

    /* Canonical payload hash must match the payload bytes. */
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, input->canonicalPayload, input->canonicalPayloadSize);
    Gen3Sha256_Final(&context, digest);
    if (memcmp(digest, input->canonicalPayloadSha256, GEN3_PACK_SHA256_SIZE) != 0)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH, build->entryCount,
            input->canonicalName);
        return GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH;
    }

    /* Codes. */
    if (Gen3ResourcePack_TypeToCode(input->type) == GEN3_PACK_TYPE_INVALID)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_TYPE_CODE, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_INVALID_TYPE_CODE;
    }
    if (input->schema == 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_SCHEMA, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_INVALID_SCHEMA;
    }
    if ((input->flags & ~GEN3_PACK_FLAG_REQUIRED_FOR_BASE) != 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_FLAGS, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_BAD_FLAGS;
    }
    if (input->representation != GEN3_PACK_REPRESENTATION_DECODED
     && input->representation != GEN3_PACK_REPRESENTATION_RAW)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_REPRESENTATION_CODE, build->entryCount,
            input->canonicalName);
        return GEN3_PACK_ERR_INVALID_REPRESENTATION_CODE;
    }
    if (input->sourceEncoding != GEN3_PACK_SOURCE_ENCODING_GBA_LZ77
     && input->sourceEncoding != GEN3_PACK_SOURCE_ENCODING_RAW)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_ENCODING_CODE, build->entryCount,
            input->canonicalName);
        return GEN3_PACK_ERR_INVALID_ENCODING_CODE;
    }
    if (input->sourceEncodedSize == 0u
     || Gen3PackBytesAllZero(input->sourceEncodedSha256, GEN3_PACK_SHA256_SIZE))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_METADATA, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_BAD_METADATA;
    }

    /* Duplicate detection (name and derived key are 1:1 after key validation). */
    for (i = 0; i < build->entryCount; i++)
    {
        if (strcmp(build->entries[i].canonicalName, input->canonicalName) == 0)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_DUPLICATE_NAME, build->entryCount,
                input->canonicalName);
            return GEN3_PACK_ERR_DUPLICATE_NAME;
        }
        if (memcmp(build->entries[i].key.bytes, input->key->bytes,
                   GEN3_RESOURCE_KEY_SIZE) == 0)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_DUPLICATE_KEY, build->entryCount, input->canonicalName);
            return GEN3_PACK_ERR_DUPLICATE_KEY;
        }
    }

    if (!GrowEntries(build))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    entry = &build->entries[build->entryCount];
    memset(entry, 0, sizeof(*entry));
    entry->canonicalName = malloc(nameLength + 1u);
    entry->payload = malloc(input->canonicalPayloadSize);
    if (entry->canonicalName == NULL || entry->payload == NULL)
    {
        free(entry->canonicalName);
        free(entry->payload);
        entry->canonicalName = NULL;
        entry->payload = NULL;
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, build->entryCount, input->canonicalName);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    memcpy(entry->canonicalName, input->canonicalName, nameLength + 1u);
    memcpy(entry->payload, input->canonicalPayload, input->canonicalPayloadSize);
    entry->key = *input->key;
    entry->type = input->type;
    entry->schema = input->schema;
    entry->flags = input->flags;
    entry->representation = input->representation;
    entry->sourceEncoding = input->sourceEncoding;
    entry->payloadSize = input->canonicalPayloadSize;
    memcpy(entry->payloadSha256, input->canonicalPayloadSha256, GEN3_PACK_SHA256_SIZE);
    entry->sourceRomOffset = input->sourceRomOffset;
    entry->sourceEncodedSize = input->sourceEncodedSize;
    memcpy(entry->sourceEncodedSha256, input->sourceEncodedSha256, GEN3_PACK_SHA256_SIZE);
    build->totalPayload = total;
    build->entryCount++;
    return GEN3_PACK_OK;
}

/* Comparator for bytewise canonical-name sort of build entries. */
static int CompareBuildEntries(const void *left, const void *right)
{
    const struct BuildEntry *a = left;
    const struct BuildEntry *b = right;
    return Gen3PackCompareNameBytes((const uint8_t *)a->canonicalName,
                                    strlen(a->canonicalName),
                                    (const uint8_t *)b->canonicalName,
                                    strlen(b->canonicalName));
}

/* Writes the header into out (448 bytes). Returns false on arithmetic overflow
 * (caller then reports it). */
static bool WriteHeader(struct Gen3ResourcePackBuild *build,
                        uint64_t entryCount, uint64_t fileSize,
                        uint64_t tocOffset, uint64_t tocSize,
                        uint64_t namesOffset, uint64_t namesSize,
                        uint64_t payloadOffset, uint64_t payloadSize,
                        const uint8_t tocSha[GEN3_PACK_SHA256_SIZE],
                        const uint8_t namesSha[GEN3_PACK_SHA256_SIZE],
                        const uint8_t payloadSha[GEN3_PACK_SHA256_SIZE],
                        uint8_t out[GEN3_PACK_HEADER_SIZE])
{
    struct Gen3ResourcePackLogicalInput logicalInput;
    struct Gen3Sha256Context context;
    uint8_t logicalSha[GEN3_PACK_SHA256_SIZE];
    uint8_t headerDigest[GEN3_PACK_SHA256_SIZE];

    memset(out, 0, GEN3_PACK_HEADER_SIZE);
    memcpy(out + GEN3_PACK_OFF_MAGIC, GEN3_PACK_MAGIC_BYTES, 8u);
    Gen3PackPutU32(out + GEN3_PACK_OFF_HEADER_SIZE, GEN3_PACK_HEADER_SIZE);
    Gen3PackPutU32(out + GEN3_PACK_OFF_FORMAT_VERSION, GEN3_PACK_WRITER_FORMAT_VERSION);
    Gen3PackPutU32(out + GEN3_PACK_OFF_ENDIAN_TAG, GEN3_PACK_ENDIAN_TAG);
    Gen3PackPutU32(out + GEN3_PACK_OFF_FLAGS, 0u);
    Gen3PackPutU32(out + GEN3_PACK_OFF_BASE_PACK_VERSION, build->basePackVersion);
    Gen3PackPutU32(out + GEN3_PACK_OFF_ENTRY_COUNT, (uint32_t)entryCount);
    Gen3PackPutU32(out + GEN3_PACK_OFF_ENTRY_SIZE, GEN3_PACK_ENTRY_SIZE);
    Gen3PackPutU32(out + GEN3_PACK_OFF_API_MAJOR, GEN3_PACK_WRITER_API_MAJOR);
    Gen3PackPutU32(out + GEN3_PACK_OFF_API_MINOR, GEN3_PACK_WRITER_API_MINOR);
    Gen3PackPutU32(out + GEN3_PACK_OFF_API_PATCH, GEN3_PACK_WRITER_API_PATCH);
    Gen3PackPutU32(out + GEN3_PACK_OFF_CATALOG_VERSION, build->catalogVersion);
    Gen3PackPutU32(out + GEN3_PACK_OFF_EXTRACTION_MANIFEST_VER,
                   build->extractionManifestVersion);
    Gen3PackPutU32(out + GEN3_PACK_OFF_CANONICAL_REP_VERSION,
                   build->canonicalRepresentationVersion);
    Gen3PackPutU32(out + GEN3_PACK_OFF_RESERVED0, 0u);
    Gen3PackPutU64(out + GEN3_PACK_OFF_FILE_SIZE, fileSize);
    Gen3PackPutU64(out + GEN3_PACK_OFF_TOC_OFFSET, tocOffset);
    Gen3PackPutU64(out + GEN3_PACK_OFF_TOC_SIZE, tocSize);
    Gen3PackPutU64(out + GEN3_PACK_OFF_NAMES_OFFSET, namesOffset);
    Gen3PackPutU64(out + GEN3_PACK_OFF_NAMES_SIZE, namesSize);
    Gen3PackPutU64(out + GEN3_PACK_OFF_PAYLOAD_OFFSET, payloadOffset);
    Gen3PackPutU64(out + GEN3_PACK_OFF_PAYLOAD_SIZE, payloadSize);
    Gen3PackPutU64(out + GEN3_PACK_OFF_ROM_SIZE, build->sourceRomSize);
    memcpy(out + GEN3_PACK_OFF_ROM_SHA1, build->sourceRomSha1, GEN3_PACK_SHA1_SIZE);
    memcpy(out + GEN3_PACK_OFF_ROM_SHA256, build->sourceRomSha256, GEN3_PACK_SHA256_SIZE);
    memcpy(out + GEN3_PACK_OFF_GAME_CODE, build->gameCode, 4u);
    memcpy(out + GEN3_PACK_OFF_MAKER_CODE, build->makerCode, 2u);
    out[GEN3_PACK_OFF_SOFTWARE_REVISION] = build->softwareRevision;
    out[GEN3_PACK_OFF_RESERVED1] = 0u;
    memcpy(out + GEN3_PACK_OFF_GAME_ID, build->gameId, GEN3_PACK_GAME_ID_SIZE);
    memcpy(out + GEN3_PACK_OFF_CATALOG_SHA256, build->catalogSha256, GEN3_PACK_SHA256_SIZE);
    memcpy(out + GEN3_PACK_OFF_MANIFEST_SHA256, build->extractionManifestSha256,
           GEN3_PACK_SHA256_SIZE);
    memcpy(out + GEN3_PACK_OFF_TOC_SHA256, tocSha, GEN3_PACK_SHA256_SIZE);
    memcpy(out + GEN3_PACK_OFF_NAMES_SHA256, namesSha, GEN3_PACK_SHA256_SIZE);
    memcpy(out + GEN3_PACK_OFF_PAYLOAD_SHA256, payloadSha, GEN3_PACK_SHA256_SIZE);

    /* Logical digest from the packed header fields. */
    memset(&logicalInput, 0, sizeof(logicalInput));
    logicalInput.formatVersion = GEN3_PACK_WRITER_FORMAT_VERSION;
    logicalInput.apiMajor = GEN3_PACK_WRITER_API_MAJOR;
    logicalInput.apiMinor = GEN3_PACK_WRITER_API_MINOR;
    logicalInput.apiPatch = GEN3_PACK_WRITER_API_PATCH;
    logicalInput.basePackVersion = build->basePackVersion;
    logicalInput.catalogVersion = build->catalogVersion;
    logicalInput.extractionManifestVersion = build->extractionManifestVersion;
    logicalInput.canonicalRepresentationVersion = build->canonicalRepresentationVersion;
    logicalInput.sourceRomSize = build->sourceRomSize;
    logicalInput.sourceRomSha1 = build->sourceRomSha1;
    logicalInput.sourceRomSha256 = build->sourceRomSha256;
    logicalInput.gameCode = build->gameCode;
    logicalInput.makerCode = build->makerCode;
    logicalInput.softwareRevision = build->softwareRevision;
    logicalInput.reserved = 0u;
    logicalInput.catalogSha256 = build->catalogSha256;
    logicalInput.extractionManifestSha256 = build->extractionManifestSha256;
    logicalInput.tocSha256 = tocSha;
    logicalInput.namesSha256 = namesSha;
    logicalInput.payloadSha256 = payloadSha;
    if (!Gen3ResourceContentDigest_LogicalPack(&logicalInput, logicalSha))
        return false;
    memcpy(out + GEN3_PACK_OFF_LOGICAL_SHA256, logicalSha, GEN3_PACK_SHA256_SIZE);

    /* Header hash: over the header with its own hash field zeroed. The field is
     * still zero at this point (set by the initial memset), so a single hash of
     * the whole header is correct. */
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, out, GEN3_PACK_HEADER_SIZE);
    Gen3Sha256_Final(&context, headerDigest);
    memcpy(out + GEN3_PACK_OFF_HEADER_SHA256, headerDigest, GEN3_PACK_SHA256_SIZE);
    return true;
}

enum Gen3ResourcePackError Gen3ResourcePackWriter_Write(
    const struct Gen3ResourcePackBuild *build,
    struct Gen3ResourcePackBytes *outBytes,
    struct Gen3ResourcePackDiagnosticList *diagnostics)
{
    struct BuildEntry *sorted;
    uint8_t *pack = NULL;
    uint8_t *tocBuffer = NULL;
    uint8_t header[GEN3_PACK_HEADER_SIZE];
    uint8_t tocSha[GEN3_PACK_SHA256_SIZE];
    uint8_t namesSha[GEN3_PACK_SHA256_SIZE];
    uint8_t payloadSha[GEN3_PACK_SHA256_SIZE];
    struct Gen3Sha256Context context;
    struct Gen3ResourcePackBytes result;
    uint64_t tocOffset;
    uint64_t tocSize;
    uint64_t namesOffset;
    uint64_t namesSize;
    uint64_t payloadOffset;
    uint64_t payloadSize;
    uint64_t fileSize;
    uint64_t run;
    uint64_t runName;
    size_t i;

    memset(&result, 0, sizeof(result));
    if (outBytes != NULL)
    {
        outBytes->data = NULL;
        outBytes->size = 0u;
    }
    if (build == NULL || outBytes == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_INVALID_ARGUMENT, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_INVALID_ARGUMENT;
    }
    if (!build->profileSet)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_METADATA, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_METADATA;
    }
    if (build->entryCount == 0u)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_BAD_METADATA, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_BAD_METADATA;
    }

    /* Final provenance + aligned payload-size math. */
    payloadSize = 0u;
    for (i = 0; i < build->entryCount; i++)
    {
        const struct BuildEntry *entry = &build->entries[i];
        uint64_t sourceEnd;
        uint64_t aligned;
        if (entry->sourceEncodedSize == 0u
         || Gen3PackBytesAllZero(entry->sourceEncodedSha256, GEN3_PACK_SHA256_SIZE))
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_BAD_METADATA, i, entry->canonicalName);
            return GEN3_PACK_ERR_BAD_METADATA;
        }
        if (!Gen3PackAddU64(entry->sourceRomOffset, entry->sourceEncodedSize, &sourceEnd)
         || sourceEnd > build->sourceRomSize)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_SOURCE_RANGE_OVERFLOW, i, entry->canonicalName);
            return GEN3_PACK_ERR_SOURCE_RANGE_OVERFLOW;
        }
        if (!Gen3PackAlign16U64(entry->payloadSize, &aligned))
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, i, entry->canonicalName);
            return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
        }
        if (!Gen3PackAddU64(payloadSize, aligned, &payloadSize))
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, i, entry->canonicalName);
            return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
        }
        if (payloadSize > build->maxTotalPayload)
        {
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_TOTAL_PAYLOAD_TOO_LARGE, i, entry->canonicalName);
            return GEN3_PACK_ERR_TOTAL_PAYLOAD_TOO_LARGE;
        }
    }

    tocOffset = GEN3_PACK_HEADER_SIZE;
    tocSize = (uint64_t)build->entryCount * GEN3_PACK_ENTRY_SIZE;
    namesOffset = tocOffset + tocSize;
    namesSize = 0u;
    for (i = 0; i < build->entryCount; i++)
        namesSize += (uint64_t)strlen(build->entries[i].canonicalName);
    if (!Gen3PackAddU64(namesOffset, namesSize, &run))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
    }
    if (!Gen3PackAlign16U64(run, &payloadOffset))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
    }
    if (!Gen3PackAddU64(payloadOffset, payloadSize, &fileSize))
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
    }
    if (fileSize > build->maxPackSize)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_PACK_TOO_LARGE, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_PACK_TOO_LARGE;
    }

    /* Sort a copy of the entries bytewise by canonical name. */
    sorted = malloc(build->entryCount * sizeof(*sorted));
    if (sorted == NULL)
    {
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < build->entryCount; i++)
        sorted[i] = build->entries[i];
    qsort(sorted, build->entryCount, sizeof(*sorted), CompareBuildEntries);

    /* Names section hash (concatenated length-delimited names). */
    Gen3Sha256_Init(&context);
    for (i = 0; i < build->entryCount; i++)
        Gen3Sha256_Update(&context, sorted[i].canonicalName,
                          strlen(sorted[i].canonicalName));
    Gen3Sha256_Final(&context, namesSha);

    /* Payload section hash: concatenation of canonical payloads, no padding. */
    Gen3Sha256_Init(&context);
    for (i = 0; i < build->entryCount; i++)
        Gen3Sha256_Update(&context, sorted[i].payload, sorted[i].payloadSize);
    Gen3Sha256_Final(&context, payloadSha);

    /* Build the TOC (sorted order) and derive its hash. */
    if (tocSize > (uint64_t)SIZE_MAX)
    {
        free(sorted);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
    }
    tocBuffer = calloc(1, (size_t)tocSize);
    if (tocBuffer == NULL)
    {
        free(sorted);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    run = payloadOffset;
    runName = 0u;
    for (i = 0; i < build->entryCount; i++)
    {
        struct BuildEntry *entry = &sorted[i];
        const char *entryName = entry->canonicalName;
        uint8_t *tocEntry = tocBuffer + i * GEN3_PACK_ENTRY_SIZE;
        uint64_t aligned;
        Gen3PackPutU32(tocEntry + GEN3_PACK_ENTRY_OFF_NAME_OFFSET, (uint32_t)runName);
        Gen3PackPutU16(tocEntry + GEN3_PACK_ENTRY_OFF_NAME_LENGTH,
                       (uint16_t)strlen(entry->canonicalName));
        Gen3PackPutU16(tocEntry + GEN3_PACK_ENTRY_OFF_TYPE_CODE,
                       (uint16_t)Gen3ResourcePack_TypeToCode(entry->type));
        Gen3PackPutU32(tocEntry + GEN3_PACK_ENTRY_OFF_SCHEMA, entry->schema);
        Gen3PackPutU32(tocEntry + GEN3_PACK_ENTRY_OFF_FLAGS, entry->flags);
        Gen3PackPutU32(tocEntry + GEN3_PACK_ENTRY_OFF_REPRESENTATION,
                       (uint32_t)entry->representation);
        Gen3PackPutU32(tocEntry + GEN3_PACK_ENTRY_OFF_SOURCE_ENCODING,
                       (uint32_t)entry->sourceEncoding);
        Gen3PackPutU64(tocEntry + GEN3_PACK_ENTRY_OFF_PAYLOAD_OFFSET, run);
        Gen3PackPutU64(tocEntry + GEN3_PACK_ENTRY_OFF_PAYLOAD_SIZE, entry->payloadSize);
        Gen3PackPutU64(tocEntry + GEN3_PACK_ENTRY_OFF_ROM_OFFSET, entry->sourceRomOffset);
        Gen3PackPutU64(tocEntry + GEN3_PACK_ENTRY_OFF_ENCODED_SIZE, entry->sourceEncodedSize);
        memcpy(tocEntry + GEN3_PACK_ENTRY_OFF_KEY, entry->key.bytes, GEN3_RESOURCE_KEY_SIZE);
        memcpy(tocEntry + GEN3_PACK_ENTRY_OFF_PAYLOAD_SHA256, entry->payloadSha256,
               GEN3_PACK_SHA256_SIZE);
        memcpy(tocEntry + GEN3_PACK_ENTRY_OFF_SOURCE_SHA256, entry->sourceEncodedSha256,
               GEN3_PACK_SHA256_SIZE);
        runName += (uint64_t)strlen(entry->canonicalName);
        if (!Gen3PackAlign16U64(entry->payloadSize, &aligned)
         || !Gen3PackAddU64(run, aligned, &run))
        {
            free(tocBuffer);
            free(sorted);
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, i, entryName);
            return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
        }
    }
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, tocBuffer, (size_t)tocSize);
    Gen3Sha256_Final(&context, tocSha);

    if (!WriteHeader((struct Gen3ResourcePackBuild *)build, build->entryCount,
                     fileSize, tocOffset, tocSize, namesOffset, namesSize,
                     payloadOffset, payloadSize, tocSha, namesSha, payloadSha, header))
    {
        free(tocBuffer);
        free(sorted);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
    }

    /* Serialize. calloc zero-fills all padding and reserved bytes. */
    pack = calloc(1, (size_t)fileSize);
    if (pack == NULL)
    {
        free(tocBuffer);
        free(sorted);
        Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
            GEN3_PACK_ERR_OUT_OF_MEMORY, SIZE_MAX, NULL);
        return GEN3_PACK_ERR_OUT_OF_MEMORY;
    }
    memcpy(pack, header, GEN3_PACK_HEADER_SIZE);
    memcpy(pack + (size_t)tocOffset, tocBuffer, (size_t)tocSize);
    runName = 0u;
    for (i = 0; i < build->entryCount; i++)
    {
        memcpy(pack + (size_t)namesOffset + (size_t)runName,
               sorted[i].canonicalName, strlen(sorted[i].canonicalName));
        runName += (uint64_t)strlen(sorted[i].canonicalName);
    }
    run = payloadOffset;
    for (i = 0; i < build->entryCount; i++)
    {
        uint64_t aligned;
        memcpy(pack + (size_t)run, sorted[i].payload, sorted[i].payloadSize);
        if (!Gen3PackAlign16U64(sorted[i].payloadSize, &aligned))
        {
            /* unreachable: every size was already aligned above */
            const char *name = sorted[i].canonicalName;
            free(pack);
            free(tocBuffer);
            free(sorted);
            Gen3ResourcePackDiagnostics_Append(diagnostics, GEN3_PACK_STAGE_WRITE,
                GEN3_PACK_ERR_ARITHMETIC_OVERFLOW, i, name);
            return GEN3_PACK_ERR_ARITHMETIC_OVERFLOW;
        }
        run += aligned;
    }

    free(tocBuffer);
    free(sorted);
    result.data = pack;
    result.size = (size_t)fileSize;
    *outBytes = result;
    return GEN3_PACK_OK;
}
