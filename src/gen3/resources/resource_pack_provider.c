/* Shared pack->M0/M1 provider adapter (Stage R4).
 *
 * See include/gen3/resources/resource_pack_provider.h for the contract.
 *
 * This module consumes an ALREADY validated immutable pack and a finalized
 * catalog, runs the R2 catalog-consistency check, and copies every validated
 * entry into a normal Gen3ResourceProvider. It performs no structural or hash
 * validation of its own - the pack type is opaque and can only be produced by
 * the R2 strict reader, and Gen3ResourcePack_ValidateCatalog is the sole
 * authority on pack/catalog consistency. Nothing here knows about Emerald,
 * BPEE01, Brendan, FireRed, the renderer, or any game symbol.
 */

#include "resource_internal.h"

#include <string.h>

#include "gen3/resources/resource_pack_provider.h"

const char *Gen3ResourcePackProviderError_Describe(
    enum Gen3ResourcePackProviderError error)
{
    static const char *const descriptions[] =
    {
        "no error",
        "invalid argument",
        "out of memory",
        "pack entry is not consistent with the catalog",
        "validated pack contains no entries",
        "provider construction failed",
    };
    if ((size_t)error >= sizeof(descriptions) / sizeof(descriptions[0]))
        return "unknown pack provider error";
    return descriptions[error];
}

static enum Gen3ResourceReason MapCatalogReason(enum Gen3ResourcePackError error)
{
    switch (error)
    {
    case GEN3_PACK_ERR_UNKNOWN_RESOURCE:
        return GEN3_RESOURCE_REASON_UNKNOWN_RESOURCE;
    case GEN3_PACK_ERR_KEY_MISMATCH:
        return GEN3_RESOURCE_REASON_RESOURCE_KEY_COLLISION;
    case GEN3_PACK_ERR_TYPE_MISMATCH:
        return GEN3_RESOURCE_REASON_TYPE_MISMATCH;
    case GEN3_PACK_ERR_SCHEMA_MISMATCH:
        return GEN3_RESOURCE_REASON_SCHEMA_MISMATCH;
    default:
        /* Gen3ResourcePack_ValidateCatalog can only report catalog-consistency
         * failures; anything else reaching here is treated as a payload-level
         * contract violation rather than silently accepted. */
        return GEN3_RESOURCE_REASON_INVALID_PAYLOAD;
    }
}

enum Gen3ResourcePackProviderError
Gen3ResourcePackProvider_CreateFromPack(
    const struct Gen3ResourcePack *pack,
    const struct Gen3ResourceCatalog *catalog,
    const struct Gen3ResourceProviderMetadata *metadata,
    struct Gen3ResourceProvider **outProvider,
    struct Gen3ResourcePackProviderInfo *outInfo,
    struct Gen3ResourceDiagnosticList *diagnostics)
{
    struct Gen3ResourcePackDiagnosticList packDiagnostics;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourcePackProfile profile;
    struct Gen3ResourceProvider *provider;
    enum Gen3ResourcePackError packError;
    size_t entryCount;
    size_t i;

    if (outProvider != NULL)
        *outProvider = NULL;
    if (outInfo != NULL)
        memset(outInfo, 0, sizeof(*outInfo));
    if (pack == NULL || catalog == NULL || metadata == NULL || outProvider == NULL)
        return GEN3_PACK_PROVIDER_ERR_INVALID_ARGUMENT;

    /* Gate 1: the pack must be fully consistent with the catalog before a
     * single entry becomes a provider entry (R4: fail closed, no partial
     * publication). This is the R2 validator; it is not re-implemented here. */
    Gen3ResourcePackDiagnostics_Init(&packDiagnostics);
    packError = Gen3ResourcePack_ValidateCatalog(pack, catalog, &packDiagnostics);
    if (packError != GEN3_PACK_OK)
    {
        enum Gen3ResourceReason reason = MapCatalogReason(packError);
        const char *name = NULL;
        const struct Gen3ResourcePackEntry *entry = NULL;
        const struct Gen3ResourceContract *contract = NULL;
        enum Gen3ResourceType expectedType = GEN3_RESOURCE_TYPE_INVALID;
        enum Gen3ResourceType actualType = GEN3_RESOURCE_TYPE_INVALID;
        uint32_t expectedSchema = 0;
        uint32_t actualSchema = 0;
        bool catalogRequiredForBase = false;

        if (packDiagnostics.count != 0
         && packDiagnostics.items[packDiagnostics.count - 1u].hasName)
            name = packDiagnostics.items[packDiagnostics.count - 1u].canonicalName;
        if (name != NULL)
        {
            entry = Gen3ResourcePack_FindByCanonicalName(pack, name);
            contract = Gen3ResourceCatalog_Find(catalog, name);
            if (entry != NULL)
            {
                actualType = entry->type;
                actualSchema = entry->schema;
            }
            if (contract != NULL)
            {
                expectedType = contract->type;
                expectedSchema = contract->schema;
                catalogRequiredForBase = contract->requiredForBase;
            }
        }
        Gen3ResourceDiagnostics_Append(diagnostics, GEN3_DIAGNOSTIC_ERROR,
            reason, name, metadata->id, expectedType, actualType,
            expectedSchema, actualSchema, true, catalogRequiredForBase);
        Gen3ResourcePackDiagnostics_Destroy(&packDiagnostics);
        return GEN3_PACK_PROVIDER_ERR_CATALOG_MISMATCH;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiagnostics);

    /* Gate 2: a validated base pack must actually carry base content. An empty
     * pack parses fine at R2, but a ROM_BASE provider that provides nothing is
     * not a valid publication and is refused here. */
    entryCount = Gen3ResourcePack_GetEntryCount(pack);
    if (entryCount == 0)
        return GEN3_PACK_PROVIDER_ERR_NO_ENTRIES;

    provider = Gen3ResourceProvider_Create(metadata);
    if (provider == NULL)
        return GEN3_PACK_PROVIDER_ERR_OUT_OF_MEMORY;

    /* Every present base-pack entry is required for the provider it came from
     * (R4: requiredForProvider). The provider copies all payloads, so the
     * source pack may be destroyed as soon as this call returns. */
    for (i = 0; i < entryCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry = Gen3ResourcePack_GetEntry(pack, i);
        if (entry == NULL
         || !Gen3ResourceProvider_Add(provider, entry->canonicalName, entry->type,
              entry->schema, entry->payload, entry->payloadSize, true, diagnostics))
        {
            Gen3ResourceProvider_Destroy(provider);
            return GEN3_PACK_PROVIDER_ERR_PROVIDER_BUILD_FAILED;
        }
    }
    if (!Gen3ResourceProvider_Finalize(provider, diagnostics))
    {
        Gen3ResourceProvider_Destroy(provider);
        return GEN3_PACK_PROVIDER_ERR_PROVIDER_BUILD_FAILED;
    }

    /* Surface pack-derived metadata for session fingerprinting. All values come
     * from the validated pack (profile/digests); nothing derives from the pack
     * path, physical layout, or host pointers. */
    if (outInfo != NULL)
    {
        memset(&info, 0, sizeof(info));
        if (Gen3ResourcePack_GetProfile(pack, &profile))
        {
            info.basePackVersion = profile.basePackVersion;
            info.catalogVersion = profile.catalogVersion;
            info.extractionManifestVersion = profile.extractionManifestVersion;
            info.canonicalRepresentationVersion = profile.canonicalRepresentationVersion;
            memcpy(info.gameId, profile.gameId, GEN3_PACK_GAME_ID_SIZE);
        }
        info.entryCount = entryCount;
        info.hasProviderContentDigest =
            Gen3ResourcePack_GetProviderDigest(pack, info.providerContentDigest);
        info.hasLogicalContentDigest =
            Gen3ResourcePack_GetLogicalDigest(pack, info.logicalContentDigest);
        *outInfo = info;
    }

    *outProvider = provider;
    return GEN3_PACK_PROVIDER_OK;
}
