/*
 * gen3_pack_build — deterministic production .rpack builder.
 *
 * Thin driver over the EXISTING multi-manifest importer
 * (emerald_resource_import.c, the R9 Stage 4 path). It invents no import,
 * extraction, or pack-writing logic: it constructs the multi-source
 * EmeraldImportInput (qualified ROM + the per-family production manifests and
 * catalogs in input order) and calls EmeraldImport_BuildPack, then writes the
 * resulting bytes to the destination path.
 *
 * Usage (from the repository root):
 *   gen3_pack_build --rom <qualified-pokeemerald.gba> --output <out.rpack> \
 *       --manifest <path>... --catalog <path>...
 *   gen3_pack_build ... --check   (exit 1 when the destination differs)
 *
 * Manifests and catalogs are paired in input order (the importer merges each
 * list in the order given). Determinism: the importer validates every record
 * against the ROM/artifact digests and the writer emits bytewise-sorted
 * entries, so the same inputs always produce byte-identical output.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_import.h"
#include "emerald/resources/emerald_rom_profile.h"
#include "gen3/resources/resource_pack_writer.h"
#include "gen3/resources/util.h"

/* R13-C: the text family joins the eight existing manifests/catalogs. */
#define MAX_SOURCES 32u

static void Usage(void)
{
    fprintf(stderr,
            "usage: gen3_pack_build --rom PATH --output PATH "
            "--manifest PATH... --catalog PATH... [--check]\n");
}

int main(int argc, char **argv)
{
    const char *romPath = NULL;
    const char *outputPath = NULL;
    struct EmeraldImportSource manifests[MAX_SOURCES];
    struct EmeraldImportSource catalogs[MAX_SOURCES];
    size_t manifestCount = 0;
    size_t catalogCount = 0;
    bool check = false;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc)
            romPath = argv[++i];
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            outputPath = argv[++i];
        else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc)
        {
            if (manifestCount >= MAX_SOURCES)
            {
                fprintf(stderr, "too many manifests\n");
                return 2;
            }
            manifests[manifestCount].bytes = NULL;
            manifests[manifestCount].size = 0;
            manifests[manifestCount].path = argv[++i];
            manifestCount++;
        }
        else if (strcmp(argv[i], "--catalog") == 0 && i + 1 < argc)
        {
            if (catalogCount >= MAX_SOURCES)
            {
                fprintf(stderr, "too many catalogs\n");
                return 2;
            }
            catalogs[catalogCount].bytes = NULL;
            catalogs[catalogCount].size = 0;
            catalogs[catalogCount].path = argv[++i];
            catalogCount++;
        }
        else if (strcmp(argv[i], "--check") == 0)
            check = true;
        else
        {
            Usage();
            return 2;
        }
    }
    if (romPath == NULL || outputPath == NULL || manifestCount == 0
     || catalogCount == 0)
    {
        Usage();
        return 2;
    }

    {
        struct EmeraldImportInput input;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes bytes;
        char errbuf[256];

        memset(&input, 0, sizeof(input));
        memset(&report, 0, sizeof(report));
        input.profile = EmeraldRomProfile_Bpee01Rev0();
        input.romPath = romPath;
        input.manifests = manifests;
        input.manifestCount = manifestCount;
        input.catalogs = catalogs;
        input.catalogCount = catalogCount;

        if (EmeraldImport_BuildPack(&input, &bytes, &report) != EMERALD_IMPORT_OK)
        {
            fprintf(stderr, "pack build failed: %s%s%s\n", report.message,
                    report.hasName ? " @ " : "",
                    report.hasName ? report.canonicalName : "");
            return 1;
        }

        if (check)
        {
            struct Gen3Buffer existing;
            Gen3Buffer_Init(&existing, bytes.size + 1u);
            if (!Gen3Util_ReadFile(outputPath, &existing, errbuf,
                                   sizeof(errbuf))
             || existing.length != bytes.size
             || memcmp(existing.data, bytes.data, bytes.size) != 0)
            {
                fprintf(stderr, "check failed: %s differs from deterministic rebuild\n",
                        outputPath);
                Gen3Buffer_Destroy(&existing);
                Gen3ResourcePackBytes_Destroy(&bytes);
                return 1;
            }
            Gen3Buffer_Destroy(&existing);
            fprintf(stderr, "check passed: %s is byte-identical to the deterministic rebuild\n",
                    outputPath);
        }
        else
        {
            if (!Gen3Util_WriteFile(outputPath, (const char *)bytes.data,
                                    bytes.size, errbuf, sizeof(errbuf)))
            {
                fprintf(stderr, "write failed: %s\n", errbuf);
                Gen3ResourcePackBytes_Destroy(&bytes);
                return 1;
            }
            fprintf(stderr, "wrote %s (%zu bytes)\n", outputPath, bytes.size);
        }
        Gen3ResourcePackBytes_Destroy(&bytes);
    }
    return 0;
}
