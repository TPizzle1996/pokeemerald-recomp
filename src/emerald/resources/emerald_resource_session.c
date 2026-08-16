/* Stage R4 Emerald ROM_BASE session helper.
 *
 * See include/emerald/resources/emerald_resource_session.h. This file is the
 * only place that carries Emerald ROM_BASE deployment constants
 * (provider id "emerald.rom-base.bpee01", precedence 300); the shared adapter
 * in src/gen3/resources/resource_pack_provider.c knows none of them.
 */

#include "emerald/resources/emerald_resource_session.h"

#include <stdio.h>
#include <string.h>

#include "gen3/resources/resource_pack_provider.h"

const char *EmeraldResourceSessionError_Describe(
    enum EmeraldResourceSessionError error)
{
    static const char *const descriptions[] =
    {
        "no error",
        "invalid argument",
        "out of memory",
        "pack/catalog provider construction failed",
        "candidate construction failed",
    };
    if ((size_t)error >= sizeof(descriptions) / sizeof(descriptions[0]))
        return "unknown emerald session error";
    return descriptions[error];
}

enum EmeraldResourceSessionError
EmeraldResourceSession_BuildRomBaseCandidate(
    const struct Gen3ResourcePack *pack,
    const struct Gen3ResourceCatalog *catalog,
    struct Gen3ResourceCandidate **outCandidate,
    struct EmeraldResourceSessionInfo *outInfo,
    struct Gen3ResourceDiagnosticList *diagnostics)
{
    struct Gen3ResourceProviderMetadata metadata;
    struct Gen3ResourcePackProviderInfo info;
    struct Gen3ResourcePackProfile profile;
    struct Gen3ResourceProvider *provider;
    struct Gen3ResourceCandidate *candidate;
    enum Gen3ResourcePackProviderError packError;
    char version[GEN3_PROVIDER_VERSION_MAX + 1u];
    uint32_t basePackVersion;

    if (outCandidate != NULL)
        *outCandidate = NULL;
    if (outInfo != NULL)
        memset(outInfo, 0, sizeof(*outInfo));
    if (pack == NULL || catalog == NULL || outCandidate == NULL)
        return EMERALD_SESSION_ERR_INVALID_ARGUMENT;

    /* Provider identity is derived from the validated pack's profile so the
     * version always matches the content (never from path/pointer/registration
     * order). */
    basePackVersion = 0;
    if (Gen3ResourcePack_GetProfile(pack, &profile))
        basePackVersion = profile.basePackVersion;
    snprintf(version, sizeof(version), "v%u", basePackVersion);

    metadata.id = EMERALD_ROM_BASE_PROVIDER_ID;
    metadata.version = version;
    metadata.kind = GEN3_PROVIDER_ROM_BASE;
    metadata.precedence = EMERALD_ROM_BASE_PRECEDENCE;

    packError = Gen3ResourcePackProvider_CreateFromPack(
        pack, catalog, &metadata, &provider,
        outInfo != NULL ? &info : NULL, diagnostics);
    if (packError != GEN3_PACK_PROVIDER_OK)
        return EMERALD_SESSION_ERR_PACK_PROVIDER_FAILED;

    candidate = Gen3ResourceCandidate_Create(catalog, diagnostics);
    if (candidate == NULL)
    {
        Gen3ResourceProvider_Destroy(provider);
        return EMERALD_SESSION_ERR_OUT_OF_MEMORY;
    }
    /* Gen3ResourceCandidate_AddProvider clones the provider; the local one can
     * be released immediately. */
    if (!Gen3ResourceCandidate_AddProvider(candidate, provider, diagnostics))
    {
        Gen3ResourceProvider_Destroy(provider);
        Gen3ResourceCandidate_Destroy(candidate);
        return EMERALD_SESSION_ERR_CANDIDATE_FAILED;
    }
    Gen3ResourceProvider_Destroy(provider);

    if (outInfo != NULL)
    {
        snprintf(outInfo->providerId, sizeof(outInfo->providerId), "%s",
                 metadata.id);
        snprintf(outInfo->providerVersion, sizeof(outInfo->providerVersion),
                 "%s", metadata.version);
        outInfo->kind = metadata.kind;
        outInfo->precedence = metadata.precedence;
        outInfo->basePackVersion = info.basePackVersion;
        outInfo->catalogVersion = info.catalogVersion;
        outInfo->extractionManifestVersion = info.extractionManifestVersion;
        outInfo->canonicalRepresentationVersion = info.canonicalRepresentationVersion;
        memcpy(outInfo->gameId, info.gameId, GEN3_PACK_GAME_ID_SIZE);
        memcpy(outInfo->providerContentDigest, info.providerContentDigest,
               GEN3_PACK_SHA256_SIZE);
        memcpy(outInfo->logicalContentDigest, info.logicalContentDigest,
               GEN3_PACK_SHA256_SIZE);
        outInfo->hasProviderContentDigest = info.hasProviderContentDigest;
        outInfo->hasLogicalContentDigest = info.hasLogicalContentDigest;
        outInfo->entryCount = info.entryCount;
    }

    *outCandidate = candidate;
    return EMERALD_SESSION_OK;
}
