#ifndef GEN3_RESOURCES_RESOURCE_PACK_H
#define GEN3_RESOURCES_RESOURCE_PACK_H

/* Shared Gen3 .rpack codec - v1 on-disk format, reader/validator, and read API.
 *
 * This is platform-neutral infrastructure only. No Emerald, GBA engine, SDL,
 * desktop frontend, ROM_BASE, or runtime integration lives here.
 *
 * The canonical pack layout is fully deterministic:
 *
 *     header (448 bytes) | TOC | names | alignment padding | payload
 *
 * All serialized integers are fixed-width little endian. Never serialized:
 * C structs, enum binary values, pointers, runtime resource handles,
 * timestamps, random IDs, absolute paths, or host metadata. Reserved bytes are
 * always zero on write and validated zero on read.
 *
 * ROM offsets in TOC entries are provenance only. They are never exposed as
 * resource identity through the read API, and resource runtime handles never
 * appear in a pack.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_types.h"

struct Gen3ResourceCatalog;

/* ------------------------------------------------------------------ */
/* v1 physical format constants                                        */
/* ------------------------------------------------------------------ */

#define GEN3_PACK_HEADER_SIZE      448u
#define GEN3_PACK_ENTRY_SIZE       160u
#define GEN3_PACK_FORMAT_VERSION_1 1u
#define GEN3_PACK_ENDIAN_TAG       0x01020304u
#define GEN3_PACK_MAGIC_BYTES      "G3RPACK\0"
#define GEN3_PACK_GAME_ID_SIZE     16u
#define GEN3_PACK_SHA1_SIZE        20u
#define GEN3_PACK_SHA256_SIZE      32u

/* Header field offsets (see docs/EMERALD_ROM_BACKED_ASSET_MIGRATION_ARCHITECTURE.md). */
#define GEN3_PACK_OFF_MAGIC                    0u
#define GEN3_PACK_OFF_HEADER_SIZE              8u
#define GEN3_PACK_OFF_FORMAT_VERSION           12u
#define GEN3_PACK_OFF_ENDIAN_TAG               16u
#define GEN3_PACK_OFF_FLAGS                    20u
#define GEN3_PACK_OFF_BASE_PACK_VERSION        24u
#define GEN3_PACK_OFF_ENTRY_COUNT              28u
#define GEN3_PACK_OFF_ENTRY_SIZE               32u
#define GEN3_PACK_OFF_API_MAJOR                36u
#define GEN3_PACK_OFF_API_MINOR                40u
#define GEN3_PACK_OFF_API_PATCH                44u
#define GEN3_PACK_OFF_CATALOG_VERSION          48u
#define GEN3_PACK_OFF_EXTRACTION_MANIFEST_VER  52u
#define GEN3_PACK_OFF_CANONICAL_REP_VERSION    56u
#define GEN3_PACK_OFF_RESERVED0                60u
#define GEN3_PACK_OFF_FILE_SIZE                64u
#define GEN3_PACK_OFF_TOC_OFFSET               72u
#define GEN3_PACK_OFF_TOC_SIZE                 80u
#define GEN3_PACK_OFF_NAMES_OFFSET             88u
#define GEN3_PACK_OFF_NAMES_SIZE               96u
#define GEN3_PACK_OFF_PAYLOAD_OFFSET           104u
#define GEN3_PACK_OFF_PAYLOAD_SIZE             112u
#define GEN3_PACK_OFF_ROM_SIZE                 120u
#define GEN3_PACK_OFF_ROM_SHA1                 128u
#define GEN3_PACK_OFF_ROM_SHA256               148u
#define GEN3_PACK_OFF_GAME_CODE                180u
#define GEN3_PACK_OFF_MAKER_CODE               184u
#define GEN3_PACK_OFF_SOFTWARE_REVISION        186u
#define GEN3_PACK_OFF_RESERVED1                187u
#define GEN3_PACK_OFF_GAME_ID                  188u
#define GEN3_PACK_OFF_CATALOG_SHA256           204u
#define GEN3_PACK_OFF_MANIFEST_SHA256          236u
#define GEN3_PACK_OFF_TOC_SHA256               268u
#define GEN3_PACK_OFF_NAMES_SHA256             300u
#define GEN3_PACK_OFF_PAYLOAD_SHA256           332u
#define GEN3_PACK_OFF_LOGICAL_SHA256           364u
#define GEN3_PACK_OFF_HEADER_SHA256            396u
#define GEN3_PACK_OFF_RESERVED2                428u
#define GEN3_PACK_RESERVED2_SIZE               20u

/* TOC entry field offsets (relative to the 160-byte entry). */
#define GEN3_PACK_ENTRY_OFF_KEY                0u
#define GEN3_PACK_ENTRY_OFF_NAME_OFFSET        32u
#define GEN3_PACK_ENTRY_OFF_NAME_LENGTH        36u
#define GEN3_PACK_ENTRY_OFF_TYPE_CODE          38u
#define GEN3_PACK_ENTRY_OFF_SCHEMA             40u
#define GEN3_PACK_ENTRY_OFF_FLAGS              44u
#define GEN3_PACK_ENTRY_OFF_REPRESENTATION     48u
#define GEN3_PACK_ENTRY_OFF_SOURCE_ENCODING    52u
#define GEN3_PACK_ENTRY_OFF_PAYLOAD_OFFSET     56u
#define GEN3_PACK_ENTRY_OFF_PAYLOAD_SIZE       64u
#define GEN3_PACK_ENTRY_OFF_ROM_OFFSET         72u
#define GEN3_PACK_ENTRY_OFF_ENCODED_SIZE       80u
#define GEN3_PACK_ENTRY_OFF_PAYLOAD_SHA256     88u
#define GEN3_PACK_ENTRY_OFF_SOURCE_SHA256      120u
#define GEN3_PACK_ENTRY_OFF_RESERVED           152u

/* Entry flags. Only bit 0 is defined in v1. */
#define GEN3_PACK_FLAG_REQUIRED_FOR_BASE 0x00000001u

/* ------------------------------------------------------------------ */
/* On-disk code tables (explicit stable integers, never host enums)    */
/* ------------------------------------------------------------------ */

/* Resource type codes. Values intentionally match the M0/M1 type enum for a
 * clean mapping, but are defined here as the authoritative stable on-disk
 * integers for the v1 format. */
enum Gen3ResourcePackTypeCode
{
    GEN3_PACK_TYPE_INVALID = 0,
    GEN3_PACK_TYPE_BITMAP = 1,
    GEN3_PACK_TYPE_TILE_GRAPHICS = 2,
    GEN3_PACK_TYPE_PALETTE = 3,
    GEN3_PACK_TYPE_SPRITE_SHEET = 4,
    GEN3_PACK_TYPE_SPRITE_METADATA = 5,
    GEN3_PACK_TYPE_TILESET = 6,
    GEN3_PACK_TYPE_TILEMAP = 7,
    GEN3_PACK_TYPE_FONT = 8,
    GEN3_PACK_TYPE_TEXT = 9,
    GEN3_PACK_TYPE_AUDIO_SAMPLE = 10,
    GEN3_PACK_TYPE_MUSIC_SEQUENCE = 11,
    GEN3_PACK_TYPE_SOUND_EFFECT = 12,
    GEN3_PACK_TYPE_CRY = 13,
    GEN3_PACK_TYPE_BINARY = 14,
    /* R12-A: audio instrument banks (voicegroups, cry tables, keysplit
     * runs). Append-only on-disk integer; existing codes never renumber. */
    GEN3_PACK_TYPE_INSTRUMENT_BANK = 15,
    /* R13-D1: structured gameplay data rows (species/move/item/evolution/
     * learnset/compatibility wire rows, growth curves). Canonical payload is
     * the exact GBA wire slice; schema declares the family row contract.
     * Append-only on-disk integer; existing codes never renumber. */
    GEN3_PACK_TYPE_STRUCTURED_DATA = 16,
    GEN3_PACK_TYPE_COUNT,
};

/* Canonical representation codes: what the canonical payload bytes are. */
enum Gen3ResourcePackRepresentationCode
{
    GEN3_PACK_REPRESENTATION_INVALID = 0,
    GEN3_PACK_REPRESENTATION_DECODED = 1, /* decoded GBA-native data */
    GEN3_PACK_REPRESENTATION_RAW = 2,     /* raw bytes as authored */
};

/* Source encoding codes: how the source artifact was compressed. */
enum Gen3ResourcePackSourceEncodingCode
{
    GEN3_PACK_SOURCE_ENCODING_INVALID = 0,
    GEN3_PACK_SOURCE_ENCODING_GBA_LZ77 = 1,
    GEN3_PACK_SOURCE_ENCODING_RAW = 2,
};

enum Gen3ResourcePackTypeCode Gen3ResourcePack_TypeToCode(enum Gen3ResourceType type);
enum Gen3ResourceType Gen3ResourcePack_TypeFromCode(enum Gen3ResourcePackTypeCode code);

/* ------------------------------------------------------------------ */
/* v1 limits / caps (bounded, but not tiny enough to block migration)  */
/* ------------------------------------------------------------------ */

#define GEN3_PACK_MAX_ENTRIES         16384u
#define GEN3_PACK_NAME_MAX            GEN3_RESOURCE_NAME_MAX /* 255 */
#define GEN3_PACK_MAX_PAYLOAD_SIZE    0x01000000u            /* 16 MiB per resource */
#define GEN3_PACK_MAX_TOTAL_PAYLOAD   0x40000000u            /* 1 GiB across the pack */
#define GEN3_PACK_MAX_PACK_SIZE       0x80000000u            /* 2 GiB total file */
#define GEN3_PACK_MAX_ROM_SIZE        0x100000000ull         /* 4 GiB source ROM */

/* ------------------------------------------------------------------ */
/* Error model (stable structured reason codes)                        */
/* ------------------------------------------------------------------ */

enum Gen3ResourcePackError
{
    GEN3_PACK_OK = 0,
    GEN3_PACK_ERR_INVALID_ARGUMENT,
    GEN3_PACK_ERR_OUT_OF_MEMORY,

    /* header / structural */
    GEN3_PACK_ERR_BAD_MAGIC,
    GEN3_PACK_ERR_BAD_HEADER_SIZE,
    GEN3_PACK_ERR_UNSUPPORTED_VERSION,
    GEN3_PACK_ERR_BAD_ENDIAN_TAG,
    GEN3_PACK_ERR_BAD_FLAGS,
    GEN3_PACK_ERR_NONZERO_RESERVED,
    GEN3_PACK_ERR_TRUNCATED_HEADER,
    GEN3_PACK_ERR_TRUNCATED_TOC,
    GEN3_PACK_ERR_TOC_SIZE_MISMATCH,
    GEN3_PACK_ERR_SECTION_OVERLAP,
    GEN3_PACK_ERR_SECTION_OUT_OF_FILE,
    GEN3_PACK_ERR_SECTION_ORDER,
    GEN3_PACK_ERR_ARITHMETIC_OVERFLOW,
    GEN3_PACK_ERR_SIZE_MISMATCH,
    GEN3_PACK_ERR_TOO_MANY_ENTRIES,

    /* TOC entry */
    GEN3_PACK_ERR_BAD_NAME_OFFSET,
    GEN3_PACK_ERR_INVALID_CANONICAL_NAME,
    GEN3_PACK_ERR_NAME_TOO_LONG,
    GEN3_PACK_ERR_KEY_MISMATCH,
    GEN3_PACK_ERR_DUPLICATE_NAME,
    GEN3_PACK_ERR_DUPLICATE_KEY,
    GEN3_PACK_ERR_UNSORTED_TOC,
    GEN3_PACK_ERR_INVALID_TYPE_CODE,
    GEN3_PACK_ERR_INVALID_SCHEMA,
    GEN3_PACK_ERR_INVALID_REPRESENTATION_CODE,
    GEN3_PACK_ERR_INVALID_ENCODING_CODE,

    /* payload placement / integrity */
    GEN3_PACK_ERR_PAYLOAD_OFFSET_BEFORE_SECTION,
    GEN3_PACK_ERR_PAYLOAD_RANGE_OVERFLOW,
    GEN3_PACK_ERR_PAYLOAD_MISALIGNED,
    GEN3_PACK_ERR_BAD_PAYLOAD_LAYOUT,
    GEN3_PACK_ERR_PAYLOAD_OVERLAP,
    GEN3_PACK_ERR_PAYLOAD_TOO_LARGE,
    GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH,
    GEN3_PACK_ERR_TOC_HASH_MISMATCH,
    GEN3_PACK_ERR_NAMES_HASH_MISMATCH,
    GEN3_PACK_ERR_PAYLOAD_SECTION_HASH_MISMATCH,
    GEN3_PACK_ERR_LOGICAL_DIGEST_MISMATCH,
    GEN3_PACK_ERR_HEADER_HASH_MISMATCH,

    /* metadata */
    GEN3_PACK_ERR_BAD_METADATA,
    GEN3_PACK_ERR_BAD_GAME_ID,
    GEN3_PACK_ERR_SOURCE_RANGE_OVERFLOW,

    /* writer build */
    GEN3_PACK_ERR_DUPLICATE_PAYLOAD_RANGE,
    GEN3_PACK_ERR_TOTAL_PAYLOAD_TOO_LARGE,
    GEN3_PACK_ERR_PACK_TOO_LARGE,

    /* catalog validation */
    GEN3_PACK_ERR_CATALOG_NOT_SUPPLIED,
    GEN3_PACK_ERR_UNKNOWN_RESOURCE,
    GEN3_PACK_ERR_TYPE_MISMATCH,
    GEN3_PACK_ERR_SCHEMA_MISMATCH,

    /* thin file helper */
    GEN3_PACK_ERR_FILE_IO,
};

enum Gen3ResourcePackStage
{
    GEN3_PACK_STAGE_NONE = 0,
    GEN3_PACK_STAGE_PARSE,
    GEN3_PACK_STAGE_WRITE,
    GEN3_PACK_STAGE_VALIDATE_CATALOG,
};

struct Gen3ResourcePackDiagnostic
{
    enum Gen3ResourcePackStage stage;
    enum Gen3ResourcePackError error;
    size_t entryIndex; /* SIZE_MAX when not entry-scoped */
    bool hasName;
    char canonicalName[GEN3_RESOURCE_NAME_MAX + 1u];
};

struct Gen3ResourcePackDiagnosticList
{
    struct Gen3ResourcePackDiagnostic *items;
    size_t count;
    size_t capacity;
};

void Gen3ResourcePackDiagnostics_Init(struct Gen3ResourcePackDiagnosticList *list);
void Gen3ResourcePackDiagnostics_Destroy(struct Gen3ResourcePackDiagnosticList *list);
bool Gen3ResourcePackDiagnostics_Append(struct Gen3ResourcePackDiagnosticList *list,
                                        enum Gen3ResourcePackStage stage,
                                        enum Gen3ResourcePackError error,
                                        size_t entryIndex,
                                        const char *canonicalName);
const char *Gen3ResourcePackError_Describe(enum Gen3ResourcePackError error);

/* ------------------------------------------------------------------ */
/* Parsed pack read API                                                */
/* ------------------------------------------------------------------ */

/* A parsed immutable entry. All pointers are owned by the pack and remain
 * valid until the pack is destroyed. */
struct Gen3ResourcePackEntry
{
    Gen3ResourceKey key;
    const char *canonicalName;
    enum Gen3ResourceType type;
    uint32_t schema;
    uint32_t flags;
    enum Gen3ResourcePackRepresentationCode representation;
    enum Gen3ResourcePackSourceEncodingCode sourceEncoding;
    const uint8_t *payload;            /* canonical payload bytes */
    size_t payloadSize;
    uint64_t sourceRomOffset;          /* provenance only */
    uint64_t sourceEncodedSize;
    const uint8_t *payloadSha256;      /* 32 bytes */
    const uint8_t *sourceEncodedSha256;/* 32 bytes */
};

struct Gen3ResourcePackProfile
{
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
};

struct Gen3ResourcePack;

/* Parse and strictly validate a whole pack image. The bytes are copied into
 * owned memory; the caller may free its buffer immediately after the call. */
enum Gen3ResourcePackError Gen3ResourcePack_Parse(
    const void *bytes,
    size_t size,
    struct Gen3ResourcePack **outPack,
    struct Gen3ResourcePackDiagnosticList *diagnostics);

/* Thin platform-neutral file helper (C stdio only). Reads the whole file,
 * parses it, and returns a validated pack. */
enum Gen3ResourcePackError Gen3ResourcePack_OpenFile(
    const char *path,
    struct Gen3ResourcePack **outPack,
    struct Gen3ResourcePackDiagnosticList *diagnostics);

void Gen3ResourcePack_Destroy(struct Gen3ResourcePack *pack);

size_t Gen3ResourcePack_GetEntryCount(const struct Gen3ResourcePack *pack);
const struct Gen3ResourcePackEntry *Gen3ResourcePack_GetEntry(
    const struct Gen3ResourcePack *pack, size_t index);
const struct Gen3ResourcePackEntry *Gen3ResourcePack_FindByCanonicalName(
    const struct Gen3ResourcePack *pack, const char *canonicalName);
const struct Gen3ResourcePackEntry *Gen3ResourcePack_FindByKey(
    const struct Gen3ResourcePack *pack, const Gen3ResourceKey *key);

bool Gen3ResourcePack_GetProfile(const struct Gen3ResourcePack *pack,
                                 struct Gen3ResourcePackProfile *outProfile);
bool Gen3ResourcePack_GetLogicalDigest(const struct Gen3ResourcePack *pack,
                                       uint8_t outDigest[GEN3_PACK_SHA256_SIZE]);
bool Gen3ResourcePack_GetProviderDigest(const struct Gen3ResourcePack *pack,
                                        uint8_t outDigest[GEN3_PACK_SHA256_SIZE]);

/* Optional catalog consistency check against an in-memory M0/M1 catalog.
 * Validates name existence, key, type, and schema for every entry. */
enum Gen3ResourcePackError Gen3ResourcePack_ValidateCatalog(
    const struct Gen3ResourcePack *pack,
    const struct Gen3ResourceCatalog *catalog,
    struct Gen3ResourcePackDiagnosticList *diagnostics);

#endif
