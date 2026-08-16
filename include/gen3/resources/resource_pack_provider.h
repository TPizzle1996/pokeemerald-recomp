#ifndef GEN3_RESOURCES_RESOURCE_PACK_PROVIDER_H
#define GEN3_RESOURCES_RESOURCE_PACK_PROVIDER_H

/* Shared pack->M0/M1 provider adapter (Stage R4).
 *
 * Turns an ALREADY validated immutable R2 .rpack into a normal
 * Gen3ResourceProvider. The adapter consumes:
 *
 *   - a `struct Gen3ResourcePack *` produced by Gen3ResourcePack_Parse /
 *     Gen3ResourcePack_OpenFile. The type is opaque: the only way to obtain one
 *     is the R2 strict reader, which fully validates structure, reserved bytes,
 *     canonical names, derived keys, type/schema codes, and every section and
 *     payload hash before returning. Passing raw bytes here is therefore
 *     impossible by API design - there is no constructor from unvalidated
 *     memory.
 *   - the finalized catalog the pack must be consistent with. Every pack entry
 *     is checked against the catalog (name existence, stable key, type,
 *     schema) before the provider is built; a single mismatch refuses
 *     construction.
 *   - explicit provider metadata (id/version/kind/precedence). The adapter
 *     never derives identity from the pack path, a pointer, registration order,
 *     or hash-map iteration.
 *
 * The resulting provider owns immutable copies of every canonical payload, so
 * the source pack and any construction temporaries may be destroyed after this
 * call; a later resolver snapshot built from the provider keeps valid payloads.
 *
 * This module is deliberately platform-neutral and reusable: it knows nothing
 * about Emerald, BPEE01, Brendan, FireRed, the renderer, or any game symbol.
 *
 * It does not duplicate R2 parsing. All structural/hash validation stays in
 * resource_pack.c; this adapter only checks catalog consistency and copies
 * validated entries into the provider API.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_provider.h"

enum Gen3ResourcePackProviderError
{
    GEN3_PACK_PROVIDER_OK = 0,
    GEN3_PACK_PROVIDER_ERR_INVALID_ARGUMENT,   /* NULL pack/catalog/metadata */
    GEN3_PACK_PROVIDER_ERR_OUT_OF_MEMORY,
    GEN3_PACK_PROVIDER_ERR_CATALOG_MISMATCH,   /* a pack entry fails catalog validation */
    GEN3_PACK_PROVIDER_ERR_NO_ENTRIES,         /* validated pack has no entries */
    GEN3_PACK_PROVIDER_ERR_PROVIDER_BUILD_FAILED,
};

/* Pack-derived metadata surfaced alongside the provider. Used to expose the
 * provider-content / logical-content digests (R4 §11) for future session
 * fingerprint use, and to report which pack versions a provider was built
 * from. All fields come from the validated pack, never from physical layout,
 * host pointers, or the pack path. */
struct Gen3ResourcePackProviderInfo
{
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

const char *Gen3ResourcePackProviderError_Describe(
    enum Gen3ResourcePackProviderError error);

/* Build a finalized provider from a validated pack.
 *
 * `pack` and `catalog` are required and must reference the same logical
 * contract: every pack entry is validated against `catalog` (name, key, type,
 * schema) and construction fails closed on the first mismatch. Every present
 * entry is added with requiredForProvider = true (each base-pack entry is
 * required for the provider it came from, R4 §7-A). The provider owns copies
 * of all payload bytes, so `pack` may be destroyed immediately afterwards.
 *
 * On success *outProvider is owned by the caller (release with
 * Gen3ResourceProvider_Destroy) and *outInfo (when non-NULL) is filled from
 * the pack profile and digests. M0/M1 structured diagnostics are appended to
 * `diagnostics` (may be NULL).
 */
enum Gen3ResourcePackProviderError
Gen3ResourcePackProvider_CreateFromPack(
    const struct Gen3ResourcePack *pack,
    const struct Gen3ResourceCatalog *catalog,
    const struct Gen3ResourceProviderMetadata *metadata,
    struct Gen3ResourceProvider **outProvider,
    struct Gen3ResourcePackProviderInfo *outInfo,
    struct Gen3ResourceDiagnosticList *diagnostics);

#endif
