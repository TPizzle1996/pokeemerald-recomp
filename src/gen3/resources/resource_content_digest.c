#include "gen3/resources/resource_content_digest.h"

#include <stdlib.h>
#include <string.h>

#include "resource_pack_internal.h"
#include "gen3/resources/sha256.h"

/* Domain prefixes include the terminating NUL (sizeof includes it), matching
 * the canonical-key domain in resource_id.c. */
static const char kLogicalDomain[] = "gen3-base-resource-pack-v1";
static const char kProviderDomain[] = "gen3-provider-content-v1";

bool Gen3ResourceContentDigest_LogicalPack(
    const struct Gen3ResourcePackLogicalInput *input,
    uint8_t outDigest[GEN3_PACK_SHA256_SIZE])
{
    struct Gen3Sha256Context context;
    uint8_t scratch[40];
    uint8_t profile[8];

    if (input == NULL || outDigest == NULL)
        return false;
    if (input->sourceRomSha1 == NULL || input->sourceRomSha256 == NULL
     || input->gameCode == NULL || input->makerCode == NULL
     || input->catalogSha256 == NULL || input->extractionManifestSha256 == NULL
     || input->tocSha256 == NULL || input->namesSha256 == NULL
     || input->payloadSha256 == NULL)
        return false;

    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, kLogicalDomain, sizeof(kLogicalDomain));

    /* LE version fields (8 x u32) + LE64 source ROM size. */
    Gen3PackPutU32(scratch + 0u, input->formatVersion);
    Gen3PackPutU32(scratch + 4u, input->apiMajor);
    Gen3PackPutU32(scratch + 8u, input->apiMinor);
    Gen3PackPutU32(scratch + 12u, input->apiPatch);
    Gen3PackPutU32(scratch + 16u, input->basePackVersion);
    Gen3PackPutU32(scratch + 20u, input->catalogVersion);
    Gen3PackPutU32(scratch + 24u, input->extractionManifestVersion);
    Gen3PackPutU32(scratch + 28u, input->canonicalRepresentationVersion);
    Gen3PackPutU64(scratch + 32u, input->sourceRomSize);
    Gen3Sha256_Update(&context, scratch, sizeof(scratch));

    /* Fixed game/profile fields: game code (4) + maker code (2) + software
     * revision (1) + reserved (1). */
    memcpy(profile + 0u, input->gameCode, 4u);
    memcpy(profile + 4u, input->makerCode, 2u);
    profile[6] = input->softwareRevision;
    profile[7] = input->reserved;
    Gen3Sha256_Update(&context, profile, sizeof(profile));

    Gen3Sha256_Update(&context, input->sourceRomSha1, GEN3_PACK_SHA1_SIZE);
    Gen3Sha256_Update(&context, input->sourceRomSha256, GEN3_PACK_SHA256_SIZE);
    Gen3Sha256_Update(&context, input->catalogSha256, GEN3_PACK_SHA256_SIZE);
    Gen3Sha256_Update(&context, input->extractionManifestSha256, GEN3_PACK_SHA256_SIZE);
    Gen3Sha256_Update(&context, input->tocSha256, GEN3_PACK_SHA256_SIZE);
    Gen3Sha256_Update(&context, input->namesSha256, GEN3_PACK_SHA256_SIZE);
    Gen3Sha256_Update(&context, input->payloadSha256, GEN3_PACK_SHA256_SIZE);

    Gen3Sha256_Final(&context, outDigest);
    return true;
}

struct ProviderSortRecord
{
    struct Gen3ResourceContentRecord record;
};

static int CompareProviderRecords(const void *left, const void *right)
{
    const struct Gen3ResourceContentRecord *a = left;
    const struct Gen3ResourceContentRecord *b = right;
    int comparison = memcmp(a->key.bytes, b->key.bytes, GEN3_RESOURCE_KEY_SIZE);
    if (comparison != 0)
        return comparison;
    if (a->type != b->type)
        return a->type < b->type ? -1 : 1;
    if (a->schema != b->schema)
        return a->schema < b->schema ? -1 : 1;
    if (a->payloadSize != b->payloadSize)
        return a->payloadSize < b->payloadSize ? -1 : 1;
    comparison = memcmp(a->payloadSha256, b->payloadSha256, GEN3_PACK_SHA256_SIZE);
    if (comparison != 0)
        return comparison;
    return 0;
}

bool Gen3ResourceContentDigest_Provider(
    const struct Gen3ResourceContentRecord *records,
    size_t recordCount,
    uint8_t outDigest[GEN3_PACK_SHA256_SIZE])
{
    struct Gen3Sha256Context context;
    struct Gen3ResourceContentRecord *copy;
    uint8_t countBytes[8];
    uint8_t scratch[16];
    size_t i;

    if (outDigest == NULL)
        return false;
    if (records == NULL && recordCount != 0)
        return false;

    if (recordCount != 0)
    {
        copy = malloc(recordCount * sizeof(*copy));
        if (copy == NULL)
            return false;
        memcpy(copy, records, recordCount * sizeof(*copy));
    }
    else
    {
        copy = NULL;
    }
    qsort(copy, recordCount, sizeof(*copy), CompareProviderRecords);

    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, kProviderDomain, sizeof(kProviderDomain));
    Gen3PackPutU64(countBytes, (uint64_t)recordCount);
    Gen3Sha256_Update(&context, countBytes, sizeof(countBytes));

    for (i = 0; i < recordCount; i++)
    {
        enum Gen3ResourcePackTypeCode code;
        if (copy[i].payloadSha256 == NULL)
        {
            free(copy);
            return false;
        }
        code = Gen3ResourcePack_TypeToCode(copy[i].type);
        if (code == GEN3_PACK_TYPE_INVALID)
        {
            free(copy);
            return false;
        }
        Gen3Sha256_Update(&context, copy[i].key.bytes, GEN3_RESOURCE_KEY_SIZE);
        Gen3PackPutU32(scratch + 0u, (uint32_t)code);
        Gen3PackPutU32(scratch + 4u, copy[i].schema);
        Gen3PackPutU64(scratch + 8u, copy[i].payloadSize);
        Gen3Sha256_Update(&context, scratch, sizeof(scratch));
        Gen3Sha256_Update(&context, copy[i].payloadSha256, GEN3_PACK_SHA256_SIZE);
    }

    free(copy);
    Gen3Sha256_Final(&context, outDigest);
    return true;
}
