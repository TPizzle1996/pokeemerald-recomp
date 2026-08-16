/* Stage R3 production import proof driver (R9 Stage 4: multi-family).
 *
 * Runs the REAL local import pipeline against the qualified retail ROM:
 *
 *   EmeraldImport_ValidateRom -> EmeraldImport_BuildPack -> EmeraldImport_Install
 *     -> EmeraldImport_ValidateInstalled
 *
 * Inputs are passed as paths (repeatable so a pack can merge several family
 * manifests and their catalogs, e.g. the R7/R8 trainer family plus the R9
 * Pokémon battle family):
 *
 *   usage: %s <rom> <dest.rpack> --manifest <manifest.toml>... --catalog <catalog.toml>...
 *
 * Manifests and catalogs are merged in input order by the importer with
 * cross-family duplicate rejection; every manifest must be production-qualified
 * against the BPEE01 Rev 0 profile (fixture manifests are rejected by the
 * importer itself, R3 §19), so this driver can never silently accept synthetic
 * content.
 *
 * Prints every phase result plus the deterministic pack metadata (entry count,
 * base pack version, retail ROM digests, logical digest) so the installed pack
 * can be audited byte-for-byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_import.h"

#define MAX_SOURCES 16u

static void PrintHex(const char *label, const uint8_t *p, size_t n)
{
    size_t i;
    printf("    %s: ", label);
    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static int PrintError(const char *phase, const struct EmeraldImportReport *report)
{
    printf("  %s FAILED: (%d) %s%s%s\n", phase, (int)report->error,
           report->message[0] != '\0' ? report->message : "(no detail)",
           report->hasName ? " [name=" : "", report->hasName ? report->canonicalName : "");
    return 1;
}

int main(int argc, char **argv)
{
    const struct EmeraldRomProfile *profile;
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    struct Gen3ResourcePackBytes pack;
    struct EmeraldInstalledInfo info;
    struct EmeraldImportSource manifests[MAX_SOURCES];
    struct EmeraldImportSource catalogs[MAX_SOURCES];
    enum EmeraldResourceImportError r;
    size_t manifestCount = 0u;
    size_t catalogCount = 0u;
    int i;

    for (i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc)
        {
            if (manifestCount >= MAX_SOURCES)
            {
                fprintf(stderr, "too many --manifest sources (max %u)\n", MAX_SOURCES);
                return 2;
            }
            memset(&manifests[manifestCount], 0, sizeof(manifests[manifestCount]));
            manifests[manifestCount].path = argv[++i];
            manifestCount++;
        }
        else if (strcmp(argv[i], "--catalog") == 0 && i + 1 < argc)
        {
            if (catalogCount >= MAX_SOURCES)
            {
                fprintf(stderr, "too many --catalog sources (max %u)\n", MAX_SOURCES);
                return 2;
            }
            memset(&catalogs[catalogCount], 0, sizeof(catalogs[catalogCount]));
            catalogs[catalogCount].path = argv[++i];
            catalogCount++;
        }
        else
        {
            fprintf(stderr, "usage: %s <rom> <dest.rpack> --manifest <manifest.toml>... "
                            "--catalog <catalog.toml>...\n", argv[0]);
            return 2;
        }
    }
    if (manifestCount == 0u || catalogCount == 0u)
    {
        fprintf(stderr, "usage: %s <rom> <dest.rpack> --manifest <manifest.toml>... "
                        "--catalog <catalog.toml>...\n", argv[0]);
        return 2;
    }

    profile = EmeraldRomProfile_Bpee01Rev0();
    printf("production profile: %s (%s/%s rev %u, %llu bytes)\n",
           profile->name, profile->gameCode, profile->makerCode,
           (unsigned)profile->softwareRevision, (unsigned long long)profile->romSize);
    PrintHex("  expected ROM SHA-1", profile->romSha1, 20u);
    PrintHex("  expected ROM SHA-256", profile->romSha256, 32u);

    memset(&input, 0, sizeof(input));
    input.profile = profile;
    input.romPath = argv[1];
    input.destinationPath = argv[2];
    input.manifests = manifests;
    input.manifestCount = manifestCount;
    input.catalogs = catalogs;
    input.catalogCount = catalogCount;

    /* Phase 1: strict ROM validation. */
    memset(&report, 0, sizeof(report));
    r = EmeraldImport_ValidateRom(&input, &report);
    printf("\n== Phase 1 ValidateRom ==\n");
    if (r != EMERALD_IMPORT_OK)
        return PrintError("ValidateRom", &report);
    printf("  PASS (ROM identity, size, header, SHA-1/SHA-256 match production profile)\n");

    /* Phase 2: build the deterministic pack in memory. */
    memset(&pack, 0, sizeof(pack));
    memset(&report, 0, sizeof(report));
    r = EmeraldImport_BuildPack(&input, &pack, &report);
    printf("\n== Phase 2 BuildPack ==\n");
    if (r != EMERALD_IMPORT_OK)
        return PrintError("BuildPack", &report);
    printf("  PASS - deterministic pack bytes = %zu\n", pack.size);
    {
        struct Gen3ResourcePack *opened = NULL;
        struct Gen3ResourcePackDiagnosticList diags;
        struct Gen3ResourcePackProfile pp;
        uint8_t logical[GEN3_PACK_SHA256_SIZE];
        Gen3ResourcePackDiagnostics_Init(&diags);
        if (Gen3ResourcePack_Parse(pack.data, pack.size, &opened, &diags) == GEN3_PACK_OK
            && opened != NULL)
        {
            printf("  entry count           : %zu\n", Gen3ResourcePack_GetEntryCount(opened));
            if (Gen3ResourcePack_GetProfile(opened, &pp))
            {
                printf("  basePackVersion       : %u\n", pp.basePackVersion);
                printf("  catalogVersion        : %u\n", pp.catalogVersion);
                printf("  manifestVersion       : %u\n", pp.extractionManifestVersion);
                printf("  representationVersion : %u\n", pp.canonicalRepresentationVersion);
                printf("  game id               : %.7s\n", pp.gameId);
                printf("  source ROM size       : %llu\n",
                       (unsigned long long)pp.sourceRomSize);
                PrintHex("  source ROM SHA-1", pp.sourceRomSha1, GEN3_PACK_SHA1_SIZE);
                PrintHex("  source ROM SHA-256", pp.sourceRomSha256, GEN3_PACK_SHA256_SIZE);
            }
            if (Gen3ResourcePack_GetLogicalDigest(opened, logical))
                PrintHex("  logical digest", logical, GEN3_PACK_SHA256_SIZE);
            Gen3ResourcePack_Destroy(opened);
        }
        else
        {
            printf("  WARNING: could not reopen the in-memory pack for metadata\n");
        }
        Gen3ResourcePackDiagnostics_Destroy(&diags);
    }
    Gen3ResourcePackBytes_Destroy(&pack);

    /* Phase 3: atomic install. */
    memset(&report, 0, sizeof(report));
    r = EmeraldImport_Install(&input, &report);
    printf("\n== Phase 3 Install ==\n");
    if (r != EMERALD_IMPORT_OK)
        return PrintError("Install", &report);
    printf("  PASS - atomically installed to %s\n", input.destinationPath);

    /* Phase 4: classify the installed pack. */
    memset(&info, 0, sizeof(info));
    memset(&report, 0, sizeof(report));
    r = EmeraldImport_ValidateInstalled(input.destinationPath, profile, &info, &report);
    printf("\n== Phase 4 ValidateInstalled ==\n");
    if (r != EMERALD_IMPORT_OK)
        return PrintError("ValidateInstalled", &report);
    printf("  status                : %d (0=VALID)\n", (int)info.status);
    printf("  basePackVersion       : %u\n", info.basePackVersion);
    printf("  entry count           : %u\n", info.entryCount);
    printf("  game id               : %.7s\n", info.gameId);
    PrintHex("  installed ROM SHA-1   ", info.romSha1, GEN3_PACK_SHA1_SIZE);
    PrintHex("  installed ROM SHA-256 ", info.romSha256, GEN3_PACK_SHA256_SIZE);
    if (info.hasLogicalDigest)
        PrintHex("  installed logical dgt ", info.logicalDigest, GEN3_PACK_SHA256_SIZE);

    printf("\nPRODUCTION IMPORT PROOF: PASS\n");
    return 0;
}
