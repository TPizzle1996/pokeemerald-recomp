/* Stage R4 production ROM_BASE snapshot proof (R9 Stage 4: multi-catalog).
 *
 * Drives the REAL ROM_BASE provider through the R4 session helper against the
 * REAL installed production pack and the REAL catalog contract(s):
 *
 *   installed pack (emerald-bpee01-v1.rpack)
 *     -> EmeraldResourceSession_BuildRomBaseCandidate
 *     -> Gen3ResourceCandidate_Build (snapshot)
 *     -> Gen3ResourceSnapshot_Resolve for every catalog resource (196 trainer
 *        + 1608 Pokémon battle = 1804 with the R9 pack)
 *
 * The ROM_BASE provider identity is the Emerald deployment contract:
 *   id         "emerald.rom-base.bpee01"
 *   version    "v1" (from base pack version 1)
 *   kind       GEN3_PROVIDER_ROM_BASE
 *   precedence 300
 *
 * For every resource the resolver view is checked byte-for-byte against the
 * pack entry (key, type, schema, payload bytes), and the winner provider
 * identity is pinned.
 *
 * Usage: emerald_rom_base_provider_production <pack> --catalog <catalog.toml>...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_import.h"
#include "emerald/resources/emerald_resource_session.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/toml.h"

static void PrintHex(const char *label, const uint8_t *p, size_t n)
{
    size_t i;
    printf("    %s: ", label);
    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static enum Gen3ResourceType CatalogTypeForName(const char *name)
{
    if (name == NULL)
        return GEN3_RESOURCE_TYPE_INVALID;
    if (strcmp(name, "bitmap") == 0)
        return GEN3_RESOURCE_TYPE_BITMAP;
    if (strcmp(name, "tile-graphics") == 0)
        return GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    if (strcmp(name, "palette") == 0)
        return GEN3_RESOURCE_TYPE_PALETTE;
    if (strcmp(name, "sprite-sheet") == 0)
        return GEN3_RESOURCE_TYPE_SPRITE_SHEET;
    return GEN3_RESOURCE_TYPE_BINARY;
}

/* Parse the real catalog.toml into a Gen3ResourceCatalog. */
/* Appends every resource of one catalog TOML file to `catalog` (R9 Stage 4:
 * the pack contract spans several family catalogs). Duplicate ids are rejected
 * by Gen3ResourceCatalog_Add, matching the importer's fail-closed merge. */
static bool AddCatalogFile(struct Gen3ResourceCatalog *catalog, const char *path)
{
    struct Gen3Buffer buf;
    struct Gen3TomlDocument doc;
    struct Gen3ResourceDiagnosticList diag;
    char errbuf[512];
    size_t i;
    bool ok = false;

    if (!Gen3Util_ReadFile(path, &buf, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "catalog read: %s\n", errbuf);
        return false;
    }
    if (!Gen3Toml_Parse(buf.data, buf.length, &doc, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "catalog parse: %s\n", errbuf);
        Gen3Buffer_Destroy(&buf);
        return false;
    }
    Gen3ResourceDiagnostics_Init(&diag);
    for (i = 0; i < Gen3Toml_GetArrayCount(&doc.root, "resources"); i++)
    {
        const struct Gen3TomlMap *item =
            Gen3Toml_GetArrayItem(&doc.root, "resources", i);
        const char *id, *type;
        long long schema;
        bool required = false;
        if (!Gen3Toml_GetString(item, "id", &id)
         || !Gen3Toml_GetString(item, "type", &type)
         || !Gen3Toml_GetInteger(item, "schema", &schema))
        {
            fprintf(stderr, "catalog record %zu missing id/type/schema\n", i);
            goto done;
        }
        Gen3Toml_GetBool(item, "required_for_base", &required);
        if (!Gen3ResourceCatalog_Add(catalog, id, CatalogTypeForName(type),
                                     (uint32_t)schema, required, &diag))
        {
            fprintf(stderr, "catalog add failed for %s\n", id);
            goto done;
        }
    }
    ok = true;
done:
    Gen3ResourceDiagnostics_Destroy(&diag);
    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&buf);
    return ok;
}

int main(int argc, char **argv)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diag;
    struct EmeraldResourceSessionInfo info;
    enum EmeraldResourceSessionError sessionError;
    size_t count;
    size_t i;
    int fail = 0;

    {
        struct Gen3ResourceDiagnosticList finalDiag;
        const char *catalogPaths[16];
        size_t catalogCount = 0u;
        int arg;
        catalog = Gen3ResourceCatalog_Create();
        if (catalog == NULL)
        {
            fprintf(stderr, "out of memory creating catalog\n");
            return 1;
        }
        for (arg = 2; arg < argc; arg++)
        {
            if (strcmp(argv[arg], "--catalog") == 0 && arg + 1 < argc
             && catalogCount < sizeof(catalogPaths) / sizeof(catalogPaths[0]))
            {
                catalogPaths[catalogCount++] = argv[++arg];
            }
            else
            {
                fprintf(stderr, "usage: %s <pack> --catalog <catalog.toml>...\n", argv[0]);
                Gen3ResourceCatalog_Destroy(catalog);
                return 2;
            }
        }
        if (catalogCount == 0u)
        {
            fprintf(stderr, "usage: %s <pack> --catalog <catalog.toml>...\n", argv[0]);
            Gen3ResourceCatalog_Destroy(catalog);
            return 2;
        }
        for (arg = 0; (size_t)arg < catalogCount; arg++)
        {
            if (!AddCatalogFile(catalog, catalogPaths[arg]))
            {
                Gen3ResourceCatalog_Destroy(catalog);
                return 1;
            }
        }
        Gen3ResourceDiagnostics_Init(&finalDiag);
        if (!Gen3ResourceCatalog_Finalize(catalog, &finalDiag))
        {
            fprintf(stderr, "catalog finalize failed\n");
            Gen3ResourceDiagnostics_Destroy(&finalDiag);
            Gen3ResourceCatalog_Destroy(catalog);
            return 1;
        }
        Gen3ResourceDiagnostics_Destroy(&finalDiag);
    }

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(argv[1], &pack, &packDiag) != GEN3_PACK_OK || pack == NULL)
    {
        fprintf(stderr, "cannot open pack '%s'\n", argv[1]);
        return 1;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    count = Gen3ResourceCatalog_Count(catalog);
    printf("loaded pack entries : %zu\n", Gen3ResourcePack_GetEntryCount(pack));
    printf("loaded catalog      : %zu resources\n", count);

    /* Build the ROM_BASE candidate from the real pack + real catalog. */
    Gen3ResourceDiagnostics_Init(&diag);
    memset(&info, 0, sizeof(info));
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        pack, catalog, &candidate, &info, &diag);
    printf("BuildRomBaseCandidate: error=%d\n", (int)sessionError);
    if (sessionError != EMERALD_SESSION_OK || candidate == NULL)
    {
        printf("  FAILED to build ROM_BASE candidate\n");
        Gen3ResourceDiagnostics_Destroy(&diag);
        Gen3ResourceCatalog_Destroy(catalog);
        Gen3ResourcePack_Destroy(pack);
        return 1;
    }

    /* Session info (R4 provider identity + pack metadata). */
    printf("\n== EmeraldResourceSessionInfo ==\n");
    printf("  providerId          : %s\n", info.providerId);
    printf("  providerVersion     : %s\n", info.providerVersion);
    printf("  kind                : %d (ROM_BASE=%d)\n", (int)info.kind, (int)GEN3_PROVIDER_ROM_BASE);
    printf("  precedence          : %u\n", info.precedence);
    printf("  basePackVersion     : %u\n", info.basePackVersion);
    printf("  catalogVersion      : %u\n", info.catalogVersion);
    printf("  manifestVersion     : %u\n", info.extractionManifestVersion);
    printf("  representationVer   : %u\n", info.canonicalRepresentationVersion);
    printf("  gameId              : %.7s\n", info.gameId);
    printf("  entryCount          : %zu\n", info.entryCount);
    printf("  hasProviderDigest   : %d\n", (int)info.hasProviderContentDigest);
    printf("  hasLogicalDigest    : %d\n", (int)info.hasLogicalContentDigest);
    if (info.hasProviderContentDigest)
        PrintHex("  providerContentDgst  ", info.providerContentDigest, GEN3_PACK_SHA256_SIZE);
    if (info.hasLogicalContentDigest)
        PrintHex("  logicalContentDigest ", info.logicalContentDigest, GEN3_PACK_SHA256_SIZE);

    /* Build the snapshot. */
    if (!Gen3ResourceCandidate_Build(candidate, &snapshot, &diag) || snapshot == NULL)
    {
        printf("  FAILED to build snapshot\n");
        Gen3ResourceCandidate_Destroy(candidate);
        Gen3ResourceDiagnostics_Destroy(&diag);
        Gen3ResourceCatalog_Destroy(catalog);
        Gen3ResourcePack_Destroy(pack);
        return 1;
    }

    /* Resolve every catalog contract and verify against the pack entry. */
    printf("\n== resolve all %zu resources through ROM_BASE ==\n", count);
    for (i = 0; i < count; i++)
    {
        const struct Gen3ResourceContract *contract = Gen3ResourceCatalog_At(catalog, i);
        const struct Gen3ResourcePackEntry *entry = NULL;
        struct Gen3ResourceView view;
        Gen3ResourceHandle handle;
        Gen3ResourceKey derivedKey;
        int ok = 1;
        const char *name = contract->canonicalName;

        if (Gen3ResourceSnapshot_FindHandle(snapshot, name, &handle) != GEN3_RESOURCE_OK)
        {
            printf("  [%zu] %s: NO HANDLE\n", i, name);
            fail++;
            continue;
        }
        if (Gen3ResourceSnapshot_Resolve(snapshot, handle, contract->type,
                                         contract->schema, &view) != GEN3_RESOURCE_OK)
        {
            printf("  [%zu] %s: NO RESOLVE\n", i, name);
            fail++;
            continue;
        }
        Gen3ResourceId_DeriveKey(name, &derivedKey);
        entry = Gen3ResourcePack_FindByCanonicalName(pack, name);
        if (entry == NULL)
        {
            printf("  [%zu] %s: no pack entry\n", i, name);
            fail++;
            continue;
        }
        if (memcmp(view.key.bytes, derivedKey.bytes, GEN3_RESOURCE_KEY_SIZE) != 0)
        {
            printf("  [%zu] %s: KEY mismatch\n", i, name);
            ok = 0;
        }
        if (view.type != contract->type || view.schema != contract->schema)
        {
            printf("  [%zu] %s: type/schema mismatch\n", i, name);
            ok = 0;
        }
        if (view.payloadSize != entry->payloadSize
         || memcmp(view.payload, entry->payload, entry->payloadSize) != 0)
        {
            printf("  [%zu] %s: payload mismatch (view %zu vs pack %zu)\n",
                   i, name, view.payloadSize, entry->payloadSize);
            ok = 0;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0
         || view.winningProviderPrecedence != EMERALD_ROM_BASE_PRECEDENCE
         || strcmp(view.winningProviderVersion, info.providerVersion) != 0)
        {
            printf("  [%zu] %s: winner identity mismatch (id=%s prec=%u ver=%s)\n",
                   i, name, view.winningProviderId,
                   view.winningProviderPrecedence, view.winningProviderVersion);
            ok = 0;
        }
        if (!ok)
            fail++;
    }
    printf("  resources resolved+verified: %zu, failures: %d\n", count - (size_t)fail, fail);

    printf("\nROM_BASE PRODUCTION SNAPSHOT: %s\n", fail == 0 ? "PASS" : "FAIL");

    Gen3ResourceSnapshot_Destroy(snapshot);
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceDiagnostics_Destroy(&diag);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourcePack_Destroy(pack);
    return fail == 0 ? 0 : 1;
}
