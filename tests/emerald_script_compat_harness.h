/* R13-G3 script-compat focused-harness shared setup (test-only).
 *
 * The script seam requires the R13-C text seam and the R13-B leaf seam
 * to be published first (the loader's normal ordering). Both test
 * binaries drive the same setup: production pack -> catalog ->
 * ROM_BASE candidate -> snapshot -> text publish -> leaf publish.
 * Not compiled into the production binary.
 */
#ifndef EMERALD_SCRIPT_COMPAT_HARNESS_H
#define EMERALD_SCRIPT_COMPAT_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_pack_writer.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_audio_compat.h"
#include "emerald/resources/emerald_encounter_compat.h"
#include "emerald/resources/emerald_frontier_compat.h"
#include "emerald/resources/emerald_gameplay_compat.h"
#include "emerald/resources/emerald_leaf_compat.h"
#include "emerald/resources/emerald_map_compat.h"
#include "emerald/resources/emerald_pokedex_compat.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "emerald/resources/emerald_rom_profile.h"
#include "emerald/resources/emerald_text_compat.h"
#include "emerald/resources/emerald_trainer_compat.h"

static struct Gen3ResourceSnapshot *gScriptHarnessSnapshot;
static struct Gen3ResourcePack *gScriptHarnessPack;
static const char *gScriptHarnessTempDir;
static const char *gScriptHarnessPackPath;

static bool BuildCatalogFromPackHarness(struct Gen3ResourcePack *pack,
                                        struct Gen3ResourceCatalog **outCatalog)
{
    struct Gen3ResourceCatalog *catalog;
    size_t count = Gen3ResourcePack_GetEntryCount(pack);
    size_t i;

    catalog = Gen3ResourceCatalog_Create();
    if (catalog == NULL)
        return false;
    for (i = 0u; i < count; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        if (entry == NULL || entry->canonicalName == NULL)
            continue;
        if (!Gen3ResourceCatalog_Add(catalog, entry->canonicalName,
                                     entry->type, entry->schema,
                                     (entry->flags
                                      & GEN3_PACK_FLAG_REQUIRED_FOR_BASE) != 0u,
                                     NULL))
        {
            Gen3ResourceCatalog_Destroy(catalog);
            return false;
        }
    }
    if (!Gen3ResourceCatalog_Finalize(catalog, NULL))
    {
        Gen3ResourceCatalog_Destroy(catalog);
        return false;
    }
    *outCatalog = catalog;
    return true;
}

/* Open `path`, build the catalog + ROM_BASE candidate + snapshot, and
 * publish the two sibling seams. Returns true when the full setup
 * succeeds (script staging may then proceed). */
static bool SetupScriptCompatSession(const char *path)
{
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceDiagnosticList diagnostics;
    struct Gen3ResourcePackDiagnosticList packDiag;
    enum EmeraldResourceSessionError sessionError;
    struct EmeraldResourceSessionInfo info;
    struct EmeraldTextCompatDiagnostics textDiag;
    struct EmeraldLeafCompatDiagnostics leafDiag;
    enum EmeraldTextCompatStatus textStatus;
    enum EmeraldLeafCompatStatus leafStatus;
    bool ok = false;

    Gen3ResourceDiagnostics_Init(&diagnostics);
    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(path, &gScriptHarnessPack, &packDiag)
            != GEN3_PACK_OK
     || gScriptHarnessPack == NULL)
    {
        fprintf(stderr, "setup: pack open failed for %s\n", path);
        goto done;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    if (!BuildCatalogFromPackHarness(gScriptHarnessPack, &catalog)
     || catalog == NULL)
    {
        fprintf(stderr, "setup: catalog build failed\n");
        goto done;
    }
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        gScriptHarnessPack, catalog, &candidate, &info, &diagnostics);
    if (sessionError != EMERALD_SESSION_OK || candidate == NULL)
    {
        fprintf(stderr, "setup: ROM_BASE candidate failed (%d)\n",
                (int)sessionError);
        goto done;
    }
    if (!Gen3ResourceCandidate_Build(candidate, &gScriptHarnessSnapshot,
                                     &diagnostics)
     || gScriptHarnessSnapshot == NULL)
    {
        fprintf(stderr, "setup: snapshot build failed\n");
        goto done;
    }

    memset(&textDiag, 0, sizeof(textDiag));
    textStatus = EmeraldTextCompat_TryInitialize(gScriptHarnessSnapshot,
                                                 gScriptHarnessPack, &textDiag);
    if (textStatus != EMERALD_TEXT_OK)
    {
        fprintf(stderr, "text seam refused: status %d stage '%s' name '%s'\n",
                (int)textStatus, textDiag.stage, textDiag.canonicalName);
        goto done;
    }
    memset(&leafDiag, 0, sizeof(leafDiag));
    leafStatus = EmeraldLeafCompat_TryInitialize(gScriptHarnessSnapshot,
                                                 gScriptHarnessPack, &leafDiag);
    if (leafStatus != EMERALD_LEAF_OK)
    {
        fprintf(stderr, "leaf seam refused: status %d stage '%s' name '%s'\n",
                (int)leafStatus, leafDiag.stage, leafDiag.canonicalName);
        goto done;
    }
    /* The R6 runtime loader publishes every family (incl. the
     * gameplay/trainer fill targets the range index aggregates) and
     * registers the session image - exactly the production path. The
     * script seam stages AFTER it and must not change the index. */
    if (EmeraldResourceCompat_RegisterRuntimeSnapshot(path)
            != EMERALD_COMPAT_OK)
    {
        fprintf(stderr, "setup: runtime registration refused\n");
        goto done;
    }
    ok = true;

done:
    Gen3ResourceDiagnostics_Destroy(&diagnostics);
    Gen3ResourceCatalog_Destroy(catalog);
    Gen3ResourceCandidate_Destroy(candidate);
    return ok;
}

static void TeardownScriptCompatSession(void)
{
    /* Full shutdown so the sanitize variant's leak detector sees a
     * clean heap: the R6 loader's registered session (shared image +
     * snapshot) plus every seam's process-lifetime arenas. */
    EmeraldScriptCompat_Shutdown();
    EmeraldTextCompat_Shutdown();
    EmeraldLeafCompat_Shutdown();
    EmeraldGameplayCompat_Shutdown();
    EmeraldTrainerCompat_Shutdown();
    EmeraldAudioCompat_Shutdown();
    EmeraldEncounterCompat_Shutdown();
    EmeraldFrontierCompat_Shutdown();
    EmeraldPokedexCompat_Shutdown();
    EmeraldMapCompat_Shutdown();
    EmeraldResourceCompat_ClearSnapshot();
    EmeraldResourceCompat_Shutdown();
    if (gScriptHarnessSnapshot != NULL)
    {
        Gen3ResourceSnapshot_Destroy(gScriptHarnessSnapshot);
        gScriptHarnessSnapshot = NULL;
    }
    if (gScriptHarnessPack != NULL)
    {
        Gen3ResourcePack_Destroy(gScriptHarnessPack);
        gScriptHarnessPack = NULL;
    }
}

#endif /* EMERALD_SCRIPT_COMPAT_HARNESS_H */
