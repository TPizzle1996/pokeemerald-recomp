#ifndef GEN3_RESOURCES_RESOURCE_PACK_WRITER_H
#define GEN3_RESOURCES_RESOURCE_PACK_WRITER_H

/* Deterministic .rpack writer.
 *
 * Consumes a structured in-memory representation (profile + resource records),
 * never Emerald runtime globals. Given logically identical inputs in any
 * insertion order, the writer emits byte-identical v1 output: records are
 * sorted bytewise by canonical name before physical layout is assigned.
 *
 * All writer inputs are validated for consistency before any bytes are
 * produced (see Gen3ResourcePackBuild_AddEntry and Gen3ResourcePackWriter_Write
 * error contracts in resource_pack.h).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_types.h"

/* One logical resource record. Pointers are borrowed for the duration of the
 * Gen3ResourcePackBuild_AddEntry call; the build copies what it needs. */
struct Gen3ResourcePackEntryInput
{
    const char *canonicalName;
    const Gen3ResourceKey *key;            /* must match the derived key */
    enum Gen3ResourceType type;
    uint32_t schema;                       /* >= 1 */
    uint32_t flags;                        /* only REQUIRED_FOR_BASE bit in v1 */
    enum Gen3ResourcePackRepresentationCode representation;
    enum Gen3ResourcePackSourceEncodingCode sourceEncoding;
    const uint8_t *canonicalPayload;
    size_t canonicalPayloadSize;
    const uint8_t *canonicalPayloadSha256; /* 32 bytes; must match the payload */
    uint64_t sourceRomOffset;              /* provenance only */
    uint64_t sourceEncodedSize;
    const uint8_t *sourceEncodedSha256;    /* 32 bytes; provenance only */
    /* R13-C text bundles: the "source" is a constructed artifact file, not a
     * ROM slice. sourceRomOffset/sourceEncodedSize are then provenance with
     * a placeholder 0 offset and the writer skips the ROM-range check. */
    bool bundle;
};

/* Pack-level fixed metadata. Pointers are borrowed for the duration of the
 * Gen3ResourcePackBuild_SetProfile call. */
struct Gen3ResourcePackProfileInput
{
    uint32_t basePackVersion;
    uint32_t catalogVersion;
    uint32_t extractionManifestVersion;
    uint32_t canonicalRepresentationVersion;
    uint64_t sourceRomSize;
    const uint8_t *sourceRomSha1;             /* 20 bytes */
    const uint8_t *sourceRomSha256;           /* 32 bytes */
    char gameCode[4];                         /* exact 4 bytes, e.g. "BPEE" */
    char makerCode[2];                        /* exact 2 bytes, e.g. "01" */
    uint8_t softwareRevision;
    char gameId[16];                          /* NUL-padded, e.g. "emerald" */
    const uint8_t *catalogSha256;             /* 32 bytes */
    const uint8_t *extractionManifestSha256;  /* 32 bytes */
};

struct Gen3ResourcePackBuild;

struct Gen3ResourcePackBytes
{
    uint8_t *data;
    size_t size;
};

void Gen3ResourcePackBytes_Destroy(struct Gen3ResourcePackBytes *bytes);

struct Gen3ResourcePackBuild *Gen3ResourcePackBuild_Create(void);
void Gen3ResourcePackBuild_Destroy(struct Gen3ResourcePackBuild *build);

/* Override the v1 caps. A zero value restores that cap's default. */
void Gen3ResourcePackBuild_SetLimits(struct Gen3ResourcePackBuild *build,
                                     uint32_t maxEntries,
                                     size_t maxPayloadSize,
                                     size_t maxTotalPayload,
                                     size_t maxPackSize);

enum Gen3ResourcePackError Gen3ResourcePackBuild_SetProfile(
    struct Gen3ResourcePackBuild *build,
    const struct Gen3ResourcePackProfileInput *profile,
    struct Gen3ResourcePackDiagnosticList *diagnostics);

enum Gen3ResourcePackError Gen3ResourcePackBuild_AddEntry(
    struct Gen3ResourcePackBuild *build,
    const struct Gen3ResourcePackEntryInput *input,
    struct Gen3ResourcePackDiagnosticList *diagnostics);

/* Serialize the build to a deterministic v1 pack. On success *outBytes owns a
 * heap buffer that must be released with Gen3ResourcePackBytes_Destroy. */
enum Gen3ResourcePackError Gen3ResourcePackWriter_Write(
    const struct Gen3ResourcePackBuild *build,
    struct Gen3ResourcePackBytes *outBytes,
    struct Gen3ResourcePackDiagnosticList *diagnostics);

#endif
