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

#endif
