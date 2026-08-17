#ifndef EMERALD_RESOURCE_SESSION_H
#define EMERALD_RESOURCE_SESSION_H

/* Stage R4 Emerald ROM_BASE session helper.
 *
 * Minimal Emerald-side bridge between the shared pack->provider adapter and the
 * M0/M1 resolver. It does exactly one thing: take an ALREADY validated R2 pack
 * and its finalized catalog and build a transactional resource candidate that
 * carries the Emerald ROM_BASE provider.
 *
 * The provider identity is fixed by the emerald deployment contract:
 *   id         = EMERALD_ROM_BASE_PROVIDER_ID ("emerald.rom-base.bpee01")
 *   version    = "v<base-pack-version>" (derived from the pack profile)
 *   kind       = GEN3_PROVIDER_ROM_BASE
 *   precedence = EMERALD_ROM_BASE_PRECEDENCE (300)
 *
 * The candidate is deliberately NOT built into a snapshot here: the caller
 * decides when/where to build + publish, so tests can prove that an invalid
 * candidate leaves an already-published snapshot untouched. Building a snapshot
 * is one Gen3ResourceCandidate_Build call away for the caller.
 *
 * This helper owns no game state, touches no trainer tables, builds no
 * compatibility image, decodes no legacy LZ, and never launches gameplay. It is
 * a pure library call: pack and catalog are supplied by the caller (tests pass
 * in-memory synthetic data; no user ROM is ever required).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_provider.h"
#include "gen3/resources/resource_resolver.h"

#define EMERALD_ROM_BASE_PROVIDER_ID "emerald.rom-base.bpee01"
#define EMERALD_ROM_BASE_PRECEDENCE  300u

enum EmeraldResourceSessionError
{
    EMERALD_SESSION_OK = 0,
    EMERALD_SESSION_ERR_INVALID_ARGUMENT,
    EMERALD_SESSION_ERR_OUT_OF_MEMORY,
    EMERALD_SESSION_ERR_PACK_PROVIDER_FAILED, /* pack/catalog inconsistent, empty, or build failed */
    EMERALD_SESSION_ERR_CANDIDATE_FAILED,
};

/* What was produced: the provider identity plus pack-derived content metadata.
 * Everything here is deterministic and logical (no ROM offsets, pack paths,
 * pointers, or host data). Used for tests and for future session fingerprint
 * bookkeeping (R4: digests exposed without a save-state v5). */
struct EmeraldResourceSessionInfo
{
    char providerId[GEN3_PROVIDER_ID_MAX + 1u];
    char providerVersion[GEN3_PROVIDER_VERSION_MAX + 1u];
    enum Gen3ResourceProviderKind kind;
    uint32_t precedence;

    uint32_t basePackVersion;
    uint32_t catalogVersion;
    uint32_t extractionManifestVersion;
    uint32_t canonicalRepresentationVersion;
    char gameId[GEN3_PACK_GAME_ID_SIZE];
    uint8_t providerContentDigest[GEN3_PACK_SHA256_SIZE];
    uint8_t logicalContentDigest[GEN3_PACK_SHA256_SIZE];
    bool hasProviderContentDigest;
    bool hasLogicalContentDigest;
    size_t entryCount;
};

const char *EmeraldResourceSessionError_Describe(
    enum EmeraldResourceSessionError error);

/* Build a candidate carrying the ROM_BASE provider for a validated pack.
 *
 * `pack` must be a validated immutable pack (opaque: only producible by the R2
 * strict reader). `catalog` must be the finalized catalog the pack is
 * consistent with; every pack entry is validated against it before anything is
 * constructed, and failure leaves *outCandidate NULL with structured
 * diagnostics appended to `diagnostics` (may be NULL).
 *
 * On success *outCandidate is owned by the caller (release with
 * Gen3ResourceCandidate_Destroy); *outInfo (when non-NULL) is filled. The
 * provider is owned by the candidate, so the source pack may be destroyed as
 * soon as this call returns.
 */
enum EmeraldResourceSessionError
EmeraldResourceSession_BuildRomBaseCandidate(
    const struct Gen3ResourcePack *pack,
    const struct Gen3ResourceCatalog *catalog,
    struct Gen3ResourceCandidate **outCandidate,
    struct EmeraldResourceSessionInfo *outInfo,
    struct Gen3ResourceDiagnosticList *diagnostics);

/* R10-F: session content fingerprint.
 *
 * A raw 32-byte SHA-256 over deterministic LOGICAL provider content - never
 * paths, timestamps, pointers, allocation addresses, or process ids. Equal
 * logical content on a different machine or filesystem path hashes
 * identically; changed provider content or a different provider order cannot
 * match. The exact construction is
 *
 *   SHA-256(
 *       "gen3-session-content-v1\0"
 *       || LE32(len(gameId)) || gameId
 *       || LE32(EMERALD_RESOURCE_SESSION_ADAPTER_VERSION)
 *       || LE32(RESOURCE_API_VERSION)              // (MAJOR<<16)|(MINOR<<8)|PATCH
 *       || base provider logical content digest     // 32 bytes, zeros if absent
 *       || LE32(providerCount)
 *       || for each provider in ASCENDING precedence:
 *            LE32(kind)
 *            LE32(precedence)
 *            LE32(len(providerId)) || providerId
 *            LE32(len(providerVersion)) || providerVersion
 *            logical content digest                 // 32 bytes, zeros if absent
 *   )
 *
 * with every LE32 written explicitly little-endian. The base provider is the
 * lowest-precedence provider (the session's ROM_BASE provider). Executable
 * build identity is a separate compatibility check and is NOT part of this
 * digest. */

#define EMERALD_RESOURCE_SESSION_ADAPTER_VERSION 1u
#define EMERALD_RESOURCE_SESSION_MAX_FINGERPRINT_PROVIDERS 16u

struct EmeraldResourceFingerprintProvider
{
    const char *providerId;
    const char *providerVersion;
    uint32_t kind;
    uint32_t precedence;
    const uint8_t *logicalContentDigest; /* GEN3_PACK_SHA256_SIZE bytes, or NULL */
};

bool EmeraldResourceSession_ComputeContentFingerprint(
    const char *gameId,
    const struct EmeraldResourceFingerprintProvider *providers,
    size_t providerCount, /* ascending precedence, validated */
    uint8_t outDigest[GEN3_PACK_SHA256_SIZE]);

/* Convenience form for the single ROM_BASE provider carried by
 * EmeraldResourceSessionInfo. */
bool EmeraldResourceSession_ComputeBaseFingerprint(
    const struct EmeraldResourceSessionInfo *info,
    uint8_t outDigest[GEN3_PACK_SHA256_SIZE]);

#endif
