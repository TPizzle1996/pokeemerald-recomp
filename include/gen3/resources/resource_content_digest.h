#ifndef GEN3_RESOURCES_RESOURCE_CONTENT_DIGEST_H
#define GEN3_RESOURCES_RESOURCE_CONTENT_DIGEST_H

/* Logical and provider-content digests for Gen3 resource packs.
 *
 * Both preimages are documented exactly below. The writer and reader share
 * these functions, so a pack's stored logical digest is recomputable by the
 * reader from the pack's own header fields, and the provider-content digest is
 * independent of physical layout (offsets, padding, header layout), which is
 * what makes it usable for save-state content identity.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_types.h"

/* ------------------------------------------------------------------ */
/* Logical pack-content digest                                          */
/* ------------------------------------------------------------------ */

/* Input values feed a fixed preimage:
 *
 *   "gen3-base-resource-pack-v1\0"        (25 bytes, NUL included)
 *   || LE32 format version
 *   || LE32 API major
 *   || LE32 API minor
 *   || LE32 API patch
 *   || LE32 base pack version
 *   || LE32 catalog version
 *   || LE32 extraction-manifest version
 *   || LE32 canonical representation version
 *   || LE64 source ROM size
 *   || game code           (4 raw bytes)
 *   || maker code          (2 raw bytes)
 *   || software revision   (1 byte)
 *   || reserved            (1 byte, always zero)
 *   || source ROM SHA-1    (20 bytes)
 *   || source ROM SHA-256  (32 bytes)
 *   || catalog SHA-256     (32 bytes)
 *   || extraction-manifest SHA-256 (32 bytes)
 *   || TOC SHA-256         (32 bytes)
 *   || names SHA-256       (32 bytes)
 *   || payload SHA-256     (32 bytes)
 *
 * digest = SHA-256 of the concatenation.
 *
 * This identifies the logical base pack content independent of process
 * addresses. It intentionally includes the section hashes per the v1 contract;
 * the layout-independent identity for save-state purposes is the
 * provider-content digest below.
 */
struct Gen3ResourcePackLogicalInput
{
    uint32_t formatVersion;
    uint32_t apiMajor;
    uint32_t apiMinor;
    uint32_t apiPatch;
    uint32_t basePackVersion;
    uint32_t catalogVersion;
    uint32_t extractionManifestVersion;
    uint32_t canonicalRepresentationVersion;
    uint64_t sourceRomSize;
    const uint8_t *sourceRomSha1;             /* 20 bytes */
    const uint8_t *sourceRomSha256;           /* 32 bytes */
    const uint8_t *gameCode;                  /* 4 bytes */
    const uint8_t *makerCode;                 /* 2 bytes */
    uint8_t softwareRevision;
    uint8_t reserved;                         /* must be zero */
    const uint8_t *catalogSha256;             /* 32 bytes */
    const uint8_t *extractionManifestSha256;  /* 32 bytes */
    const uint8_t *tocSha256;                 /* 32 bytes */
    const uint8_t *namesSha256;               /* 32 bytes */
    const uint8_t *payloadSha256;             /* 32 bytes */
};

bool Gen3ResourceContentDigest_LogicalPack(
    const struct Gen3ResourcePackLogicalInput *input,
    uint8_t outDigest[GEN3_PACK_SHA256_SIZE]);

/* ------------------------------------------------------------------ */
/* Provider-content digest                                              */
/* ------------------------------------------------------------------ */

/* One logical resource in provider-content terms. */
struct Gen3ResourceContentRecord
{
    Gen3ResourceKey key;
    enum Gen3ResourceType type;
    uint32_t schema;
    uint64_t payloadSize;
    const uint8_t *payloadSha256; /* 32 bytes */
};

/* Preimage:
 *
 *   "gen3-provider-content-v1\0"     (25 bytes, NUL included)
 *   || LE64 record count
 *   || for each record sorted bytewise by the 32-byte stable key:
 *        32-byte stable key
 *        || LE32 resource-type code
 *        || LE32 schema
 *        || LE64 canonical payload size
 *        || 32-byte canonical payload SHA-256
 *
 * digest = SHA-256 of the concatenation.
 *
 * Records are sorted bytewise by key internally, so input order is irrelevant.
 * Physical file offsets, padding, header layout, runtime handles, host
 * pointers, and timestamps are deliberately excluded: a physical pack-format
 * revision that preserves identical logical resources does not change this
 * digest, so it can identify save-state content.
 */
bool Gen3ResourceContentDigest_Provider(
    const struct Gen3ResourceContentRecord *records,
    size_t recordCount,
    uint8_t outDigest[GEN3_PACK_SHA256_SIZE]);

#endif
