/* Native runtime loader for the R5/R6 trainer-table compatibility seam.
 *
 * The production harness proves the full chain off-line (pack -> catalog ->
 * ROM_BASE candidate -> snapshot -> EmeraldResourceCompat_InitializeFromSnapshot
 * -> live native trainer tables). This loader runs that same chain at native
 * startup from the production pack and registers the resulting snapshot with the
 * seam, so the existing content-hydration path
 * (EmeraldResourceCompat_TryInitialize) publishes the three migrated Brendan
 * slots for real gameplay (New Game / Trainer Card / battle).
 *
 * The pack is the production pipeline output
 * (games/emerald/base/emerald-bpee01-v1.rpack), read from the working tree at
 * runtime exactly as the production harness does
 * (tests/emerald_trainer_native_compat_production.c passes it via argv and
 * Gen3ResourcePack_OpenFile). The catalog is reconstructed from the pack's own
 * entries: the pack was built against (and validated with) the finalized R4
 * catalog, every record carries required_for_base = true, and each entry carries
 * its canonical name, type and schema - so the reconstructed catalog is
 * equivalent to the production harness's TOML parse for candidate + snapshot
 * construction, and the runtime depends on the single self-contained pack
 * (R4 section 9).
 *
 * The pack is deliberately NOT embedded in the binary. R7A's native
 * asset-isolation contract requires every ROM_BASE_ONLY payload's encoded and
 * decoded bytes to be absent from the native link (they are resolved from the
 * ROM at runtime, never compiled in); embedding the pack would put Brendan's
 * decoded sheet + palette into the executable and fail the isolation guardrail.
 * Disk loading keeps the native binary free of every ROM_BASE_ONLY payload.
 *
 * Fail-closed: if the pack is absent or does not validate, no snapshot is
 * registered, TryInitialize stays a no-op, and the migrated slots remain at
 * their NULL sentinel - exactly the behavior the R5 harness pins. R9 §8:
 * registration also PUBLISHES - the strict init runs at registration and a
 * pack that cannot serve the Pokémon battle family is a refused session (the
 * tables are rolled back, the snapshot dropped, the failure returned).
 * R12-E: the audio arena is part of that strict init - the native gSongTable
 * rows carry ROM logical addresses (sound/song_table_native.generated.inc),
 * so the arena is the live audio source and an unpublishable arena is a
 * session refusal with the same full rollback, never a silent fallback to
 * the compiled payloads.
 *
 * The file is platform-neutral in its includes (gen3 core + emerald resource
 * headers only, per the R3 guardrail-18 dependency-creep assertion); the native
 * target guards below are compile-time only.
 */

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "gen3/resources/resource_pack.h"
#include "emerald/resources/emerald_audio_compat.h"
#include "emerald/resources/emerald_encounter_compat.h"
#include "emerald/resources/emerald_frontier_compat.h"
#include "emerald/resources/emerald_pokedex_compat.h"
#include "emerald/resources/emerald_gameplay_compat.h"
#include "emerald/resources/emerald_map_compat.h"
#include "emerald/resources/emerald_script_compat.h"
#include "emerald/resources/emerald_battle_live.h"
#include "emerald/resources/emerald_leaf_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_text_compat.h"
#include "emerald/resources/emerald_trainer_compat.h"
#include "emerald/resources/emerald_trainer_native_compat.h"

/* R13-H4: the live battle-anim/FE seam is weak-probed exactly like the
 * state-adapter bridges - production always links emerald_battle_live.c,
 * so the cutover below is unconditional there; the harness-only H3 shadow
 * link (emerald_battle_compat.c, which shares the six bridge symbols and
 * therefore can never co-link) skips this step. The interpreters hard-
 * reference the seam, so a production build can never accidentally omit
 * it: omitting it is a link failure, not a silent fallback. */
extern enum EmeraldBattleLiveStatus EmeraldBattleLive_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack, uint32_t layout,
    struct EmeraldBattleCompatDiagnostics *diagnostics);
#pragma weak EmeraldBattleLive_TryInitialize
extern enum EmeraldBattleLiveStatus EmeraldBattleLive_RegisterRanges(void);
#pragma weak EmeraldBattleLive_RegisterRanges
extern size_t EmeraldBattleLive_GetRangeCount(void);
#pragma weak EmeraldBattleLive_GetRangeCount
extern enum EmeraldBattleLiveStatus EmeraldBattleLive_Publish(void);
#pragma weak EmeraldBattleLive_Publish
extern void EmeraldBattleLive_ClearMigratedEntries(void);
#pragma weak EmeraldBattleLive_ClearMigratedEntries

static bool sSnapshotRegistered;

/* Build a finalized catalog from the pack's own entries. The pack was validated
 * against the finalized R4 catalog at build time; each entry carries the
 * canonical name, type and schema it was validated with, and every production
 * catalog record is required-for-base. Reconstructing from the pack therefore
 * yields the same catalog the TOML parse would, without a second contract file
 * at runtime (R4 section 9: the pack is the single self-contained artifact). */
static bool BuildCatalogFromPack(const struct Gen3ResourcePack *pack,
                                 struct Gen3ResourceCatalog **outCatalog)
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceDiagnosticList diagnostics;
    size_t i;
    size_t count;

    if (outCatalog != NULL)
        *outCatalog = NULL;
    if (pack == NULL || outCatalog == NULL)
        return false;

    catalog = Gen3ResourceCatalog_Create();
    if (catalog == NULL)
        return false;

    Gen3ResourceDiagnostics_Init(&diagnostics);
    count = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0; i < count; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        if (!Gen3ResourceCatalog_Add(catalog, entry->canonicalName,
                                     entry->type, entry->schema, true,
                                     &diagnostics))
        {
            Gen3ResourceDiagnostics_Destroy(&diagnostics);
            Gen3ResourceCatalog_Destroy(catalog);
            return false;
        }
    }
    Gen3ResourceDiagnostics_Destroy(&diagnostics);

    if (!Gen3ResourceCatalog_Finalize(catalog, NULL))
    {
        Gen3ResourceCatalog_Destroy(catalog);
        return false;
    }
    *outCatalog = catalog;
    return true;
}

enum EmeraldResourceCompatStatus
EmeraldResourceCompat_RegisterRuntimeSnapshot(const char *packPath)
{
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiagnostics;
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceDiagnosticList diagnostics;
    struct EmeraldResourceCompatDiagnostics diag;
    struct EmeraldResourceSessionInfo info;
    enum EmeraldResourceSessionError sessionError;
    enum EmeraldResourceCompatStatus status = EMERALD_COMPAT_ERR_UNAVAILABLE;

    /* Idempotent: the snapshot is built and registered at most once per
     * process session (VerifyPaths may run several times before gameplay). */
    if (sSnapshotRegistered)
        return EMERALD_COMPAT_OK;

    if (packPath == NULL || packPath[0] == '\0')
        return EMERALD_COMPAT_ERR_UNAVAILABLE;

    /* Open, parse and strictly validate the production pack from disk - the
     * same path the production harness uses (Gen3ResourcePack_OpenFile).
     * R7A: the ROM_BASE_ONLY payloads stay OUT of the native binary; they are
     * resolved from the pack file at runtime. */
    Gen3ResourcePackDiagnostics_Init(&packDiagnostics);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiagnostics) != GEN3_PACK_OK
     || pack == NULL)
    {
        Gen3ResourcePackDiagnostics_Destroy(&packDiagnostics);
        return EMERALD_COMPAT_ERR_UNAVAILABLE;
    }
    Gen3ResourcePackDiagnostics_Destroy(&packDiagnostics);

    if (!BuildCatalogFromPack(pack, &catalog) || catalog == NULL)
        goto done;

    /* Candidate + snapshot: identical sequence to the production harness. */
    Gen3ResourceDiagnostics_Init(&diagnostics);
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        pack, catalog, &candidate, &info, &diagnostics);
    if (sessionError == EMERALD_SESSION_OK && candidate != NULL
     && Gen3ResourceCandidate_Build(candidate, &snapshot, &diagnostics)
     && snapshot != NULL)
    {
        /* The snapshot owns clones of the catalog and provider payloads
         * (resource_resolver.c), so the intermediate pack/catalog/candidate
         * below are released here; the snapshot is retained for the process
         * session by the seam and released only by
         * EmeraldResourceCompat_Shutdown. */
        EmeraldResourceCompat_SetSnapshot(snapshot);

        /* R9 §8: publish at registration. The strict init is the game
         * contract - with the compiled Pokémon leaf payloads gone from the
         * native link (R9 §7), a pack that cannot serve the Pokémon battle
         * family is a refused session. Roll the published tables back and
         * drop the snapshot so nothing survives: sSnapshotRegistered stays
         * false and the content-hydration TryInitialize stays a no-op. */
        status = EmeraldResourceCompat_InitializeFromSnapshot(snapshot, &diag);
        if (status != EMERALD_COMPAT_OK)
        {
            fprintf(stderr,
                    "emerald runtime: session refused: Pokémon battle family "
                    "not published (status %d%s%s)\n",
                    (int)status,
                    diag.canonicalName[0] != '\0' ? " @ " : "",
                    diag.canonicalName);
            EmeraldResourceCompat_ClearMigratedEntries();
            EmeraldResourceCompat_ClearSnapshot();
            EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
            Gen3ResourceSnapshot_Destroy(snapshot);
            snapshot = NULL;
            sSnapshotRegistered = false;
        }
        else
        {
            /* R12-E: publish the audio arena (leaves + structural + songs).
             * The arena is now the LIVE source - the native gSongTable rows
             * carry ROM logical addresses, so every consumer resolves into
             * the arena - and the compiled audio objects are dead weight
             * (still linked until R12-G, never consumed). A pack that cannot
             * publish the audio arena is therefore a REFUSED session, with
             * the same full rollback as the Pokémon family above: the
             * trainer tables published at init are rolled back too, the
             * snapshot is dropped, and nothing survives (sSnapshotRegistered
             * stays false; content-hydration TryInitialize stays a no-op).
             * No silent compiled fallback in native production. */
            struct EmeraldAudioCompatDiagnostics audioDiag;
            enum EmeraldAudioCompatStatus audioStatus =
                EmeraldAudioCompat_TryInitialize(snapshot, pack, &audioDiag);
            if (audioStatus != EMERALD_AUDIO_OK)
            {
                fprintf(stderr,
                        "emerald runtime: session refused: audio arena not "
                        "published (status %d%s%s)\n",
                        (int)audioStatus,
                        audioDiag.canonicalName[0] != '\0' ? " @ " : "",
                        audioDiag.canonicalName);
                EmeraldResourceCompat_ClearMigratedEntries();
                EmeraldAudioCompat_ClearMigratedEntries();
                EmeraldTrainerCompat_ClearMigratedEntries();
                EmeraldResourceCompat_ClearSnapshot();
                EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                Gen3ResourceSnapshot_Destroy(snapshot);
                snapshot = NULL;
                sSnapshotRegistered = false;
                status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
            }
            else
            {
                /* R13-B: publish the leaf arena (movement + multiboot).
                 * ADDITIVE-ONLY: no consumer is redirected (the movement
                 * consumers are deferred to the R13-G pointer graph; the
                 * ereader program stays compiled and live), so a failed
                 * publication is a DEGRADE with a named diagnostic, never
                 * a session refusal - the game behaves exactly as
                 * pre-R13-B either way. The arena holds pure payload
                 * bytes + a name/offset/size record table; nothing in it
                 * is serialized by State-v5, so no range registration. */
                {
                    struct EmeraldLeafCompatDiagnostics leafDiag;
                    enum EmeraldLeafCompatStatus leafStatus =
                        EmeraldLeafCompat_TryInitialize(snapshot, pack,
                                                        &leafDiag);
                    if (leafStatus != EMERALD_LEAF_OK)
                    {
                        fprintf(stderr,
                                "emerald runtime: R13-B leaf arena not "
                                "published (additive degrade, no consumer "
                                "redirect): status %d%s%s\n",
                                (int)leafStatus,
                                leafDiag.canonicalName[0] != '\0' ? " @ " : "",
                                leafDiag.canonicalName);
                    }
                }
                /* R13-C: publish the sixteen text family arenas + apply
                 * the generated slot/skeleton fills. REFUSE-CLASS (brief
                 * §17): the cut-over families (battle/move/ability/nature/
                 * shared/system/match-call/ribbon) have NO compiled
                 * fallback once the C-side guards land, so a session whose
                 * text cannot be published is refused with the same full
                 * rollback as the Pokémon family and the audio arena: every
                 * published seam's migrated entries are cleared, the
                 * snapshot is dropped, and nothing survives
                 * (sSnapshotRegistered stays false). */
                {
                    struct EmeraldTextCompatDiagnostics textDiag;
                    enum EmeraldTextCompatStatus textStatus =
                        EmeraldTextCompat_TryInitialize(snapshot, pack,
                                                        &textDiag);
                    if (textStatus != EMERALD_TEXT_OK)
                    {
                        fprintf(stderr,
                                "emerald runtime: session refused: text "
                                "arenas not published (status %d%s%s)\n",
                                (int)textStatus,
                                textDiag.canonicalName[0] != '\0' ? " @ " : "",
                                textDiag.canonicalName);
                        EmeraldResourceCompat_ClearMigratedEntries();
                        EmeraldAudioCompat_ClearMigratedEntries();
                        EmeraldTextCompat_ClearMigratedEntries();
                        EmeraldLeafCompat_ClearMigratedEntries();
                        EmeraldTrainerCompat_ClearMigratedEntries();
                        EmeraldResourceCompat_ClearSnapshot();
                        EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                        Gen3ResourceSnapshot_Destroy(snapshot);
                        snapshot = NULL;
                        sSnapshotRegistered = false;
                        status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                    }
                    else
                    {
                        /* R13-D1: publish the gameplay-data families
                         * (species/moves/shared tables/fonts) into their
                         * native HOST_DATA fill targets. REFUSE-CLASS like
                         * the text seam: the D1 compiled const definitions
                         * are guarded out of the native link, so there is
                         * no fallback - a session whose gameplay data
                         * cannot publish is refused with the same full
                         * rollback as every earlier seam (trainer, audio,
                         * text), the snapshot is dropped, and nothing
                         * survives (sSnapshotRegistered stays false).
                         * Runs only after audio/text have succeeded (the
                         * loader's failure-rollback discipline). */
                        struct EmeraldGameplayCompatDiagnostics gameplayDiag;
                        enum EmeraldGameplayCompatStatus gameplayStatus =
                            EmeraldGameplayCompat_TryInitialize(snapshot, pack,
                                                                &gameplayDiag);
                        if (gameplayStatus != EMERALD_GAMEPLAY_OK)
                        {
                            fprintf(stderr,
                                    "emerald runtime: session refused: "
                                    "gameplay data not published (status %d%s%s)\n",
                                    (int)gameplayStatus,
                                    gameplayDiag.canonicalName[0] != '\0' ? " @ " : "",
                                    gameplayDiag.canonicalName);
                            EmeraldResourceCompat_ClearMigratedEntries();
                            EmeraldAudioCompat_ClearMigratedEntries();
                            EmeraldTextCompat_ClearMigratedEntries();
                            EmeraldLeafCompat_ClearMigratedEntries();
                            EmeraldGameplayCompat_ClearMigratedEntries();
                            EmeraldTrainerCompat_ClearMigratedEntries();
                            EmeraldResourceCompat_ClearSnapshot();
                            EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                            Gen3ResourceSnapshot_Destroy(snapshot);
                            snapshot = NULL;
                            sSnapshotRegistered = false;
                            status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                        }
                        else
                        {
                            /* R13-E1: publish the trainer-data families
                             * (gTrainers + party leaves + trainer class
                             * names) into their native HOST_DATA fill
                             * targets. REFUSE-CLASS like the gameplay seam
                             * (which runs just before this so gItems/gameplay
                             * are live first): the compiled const trainer
                             * definitions are NATIVE_LINUX-guarded out, so
                             * there is no fallback - a session whose trainer
                             * data cannot publish is refused with the same
                             * full rollback as every earlier seam (trainer
                             * graphics, audio, text, gameplay), the snapshot
                             * is dropped, and nothing survives
                             * (sSnapshotRegistered stays false). */
                            struct EmeraldTrainerCompatDiagnostics trainerDiag;
                            enum EmeraldTrainerCompatStatus trainerStatus =
                                EmeraldTrainerCompat_TryInitialize(
                                    snapshot, pack, &trainerDiag);
                            if (trainerStatus != EMERALD_TRAINER_OK)
                            {
                                fprintf(stderr,
                                        "emerald runtime: session refused: "
                                        "trainer data not published (status %d%s%s)\n",
                                        (int)trainerStatus,
                                        trainerDiag.canonicalName[0] != '\0' ? " @ " : "",
                                        trainerDiag.canonicalName);
                                EmeraldResourceCompat_ClearMigratedEntries();
                                EmeraldAudioCompat_ClearMigratedEntries();
                                EmeraldTextCompat_ClearMigratedEntries();
                                EmeraldLeafCompat_ClearMigratedEntries();
                                EmeraldGameplayCompat_ClearMigratedEntries();
                                EmeraldTrainerCompat_ClearMigratedEntries();
                                EmeraldResourceCompat_ClearSnapshot();
                                EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                                Gen3ResourceSnapshot_Destroy(snapshot);
                                snapshot = NULL;
                                sSnapshotRegistered = false;
                                status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                            }
                            else
                            {
                                /* R13-E2: publish the wild-encounter families
                                 * (gWildMonHeaders + map-based WildPokemonInfo +
                                 * slot tables) into their native HOST_DATA fill
                                 * targets. REFUSE-CLASS like the gameplay/trainer
                                 * seams: the compiled const map-based encounter
                                 * definitions are NATIVE_LINUX-guarded out, so
                                 * there is no fallback - a session whose wild-
                                 * encounter data cannot publish is refused with
                                 * the same full rollback as every earlier seam,
                                 * the snapshot is dropped, and nothing survives
                                 * (sSnapshotRegistered stays false). */
                                struct EmeraldEncounterCompatDiagnostics
                                    encounterDiag;
                                enum EmeraldEncounterCompatStatus encounterStatus =
                                    EmeraldEncounterCompat_TryInitialize(
                                        snapshot, pack, &encounterDiag);
                                if (encounterStatus != EMERALD_ENCOUNTER_OK)
                                {
                                    fprintf(stderr,
                                            "emerald runtime: session refused: "
                                            "wild-encounter data not published "
                                            "(status %d%s%s)\n",
                                            (int)encounterStatus,
                                            encounterDiag.canonicalName[0] != '\0'
                                                ? " @ " : "",
                                            encounterDiag.canonicalName);
                                    EmeraldResourceCompat_ClearMigratedEntries();
                                    EmeraldAudioCompat_ClearMigratedEntries();
                                    EmeraldTextCompat_ClearMigratedEntries();
                                    EmeraldLeafCompat_ClearMigratedEntries();
                                    EmeraldGameplayCompat_ClearMigratedEntries();
                                    EmeraldTrainerCompat_ClearMigratedEntries();
                                    EmeraldEncounterCompat_ClearMigratedEntries();
                                    EmeraldResourceCompat_ClearSnapshot();
                                    EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                                    Gen3ResourceSnapshot_Destroy(snapshot);
                                    snapshot = NULL;
                                    sSnapshotRegistered = false;
                                    status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                                }
                                else
                                {
                                    /* R13-E3a-1: publish the Battle Frontier
                                     * trainer/mon + Battle Tent families into
                                     * their native HOST_DATA fill targets.
                                     * REFUSE-CLASS like the enemy/encounter
                                     * seams: the compiled const frontier
                                     * definitions are NATIVE_LINUX-guarded
                                     * out, so there is no fallback - a
                                     * session whose frontier data cannot
                                     * publish is refused with the same full
                                     * rollback as every earlier seam, the
                                     * snapshot is dropped, and nothing
                                     * survives (sSnapshotRegistered stays
                                     * false). */
                                    struct EmeraldFrontierCompatDiagnostics
                                        frontierDiag;
                                    enum EmeraldFrontierCompatStatus frontierStatus =
                                        EmeraldFrontierCompat_TryInitialize(
                                            snapshot, pack, &frontierDiag);
                                    if (frontierStatus != EMERALD_FRONTIER_OK)
                                    {
                                        fprintf(stderr,
                                                "emerald runtime: session refused: "
                                                "frontier data not published "
                                                "(status %d%s%s)\n",
                                                (int)frontierStatus,
                                                frontierDiag.canonicalName[0] != '\0'
                                                    ? " @ " : "",
                                                frontierDiag.canonicalName);
                                        EmeraldResourceCompat_ClearMigratedEntries();
                                        EmeraldAudioCompat_ClearMigratedEntries();
                                        EmeraldTextCompat_ClearMigratedEntries();
                                        EmeraldLeafCompat_ClearMigratedEntries();
                                        EmeraldGameplayCompat_ClearMigratedEntries();
                                        EmeraldTrainerCompat_ClearMigratedEntries();
                                        EmeraldEncounterCompat_ClearMigratedEntries();
                                        EmeraldFrontierCompat_ClearMigratedEntries();
                                        EmeraldResourceCompat_ClearSnapshot();
                                        EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                                        Gen3ResourceSnapshot_Destroy(snapshot);
                                        snapshot = NULL;
                                        sSnapshotRegistered = false;
                                        status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                                    }
                                    else
                                    {
                                        /* R13-E3b: Pokédex structured-data +
                                         * R13-C description-text cutover. Runs
                                         * AFTER EmeraldFrontierCompat; the
                                         * R13-C text arena it re-points into is
                                         * already live. REFUSE-class: on failure
                                         * the whole registration rolls back. */
                                        struct EmeraldPokedexCompatDiagnostics
                                            pokedexDiag;
                                        enum EmeraldPokedexCompatStatus
                                            pokedexStatus =
                                            EmeraldPokedexCompat_TryInitialize(
                                                snapshot, pack, &pokedexDiag);
                                        if (pokedexStatus != EMERALD_POKEDEX_OK)
                                        {
                                            fprintf(stderr,
                                                    "emerald runtime: session refused: "
                                                    "pokedex data not published "
                                                    "(status %d%s%s)\n",
                                                    (int)pokedexStatus,
                                                    pokedexDiag.canonicalName[0] != '\0'
                                                        ? " @ " : "",
                                                    pokedexDiag.canonicalName);
                                            EmeraldResourceCompat_ClearMigratedEntries();
                                            EmeraldAudioCompat_ClearMigratedEntries();
                                            EmeraldTextCompat_ClearMigratedEntries();
                                            EmeraldLeafCompat_ClearMigratedEntries();
                                            EmeraldGameplayCompat_ClearMigratedEntries();
                                            EmeraldTrainerCompat_ClearMigratedEntries();
                                            EmeraldEncounterCompat_ClearMigratedEntries();
                                            EmeraldFrontierCompat_ClearMigratedEntries();
                                            EmeraldPokedexCompat_ClearMigratedEntries();
                                            EmeraldResourceCompat_ClearSnapshot();
                                            EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                                            Gen3ResourceSnapshot_Destroy(snapshot);
                                            snapshot = NULL;
                                            sSnapshotRegistered = false;
                                            status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                                        }
                                        else
                                        {
                                            /* R13-F: publish the map-metadata + event
                                             * families (headers/layout-meta/event-bundles/
                                             * connections) into the native HOST_DATA
                                             * gMapHeaders + the event/connection arenas.
                                             * REFUSE-CLASS like the pokedex seam: the
                                             * mapjson-generated map data is
                                             * NATIVE_LINUX-gated out of the link, so a
                                             * session whose map metadata cannot publish is
                                             * refused with the same full rollback, the
                                             * snapshot is dropped, and nothing survives
                                             * (sSnapshotRegistered stays false). */
                                            struct EmeraldMapCompatDiagnostics mapDiag;
                                            enum EmeraldMapCompatStatus mapStatus =
                                                EmeraldMapCompat_TryInitialize(
                                                    snapshot, pack, &mapDiag);
                                            if (mapStatus != EMERALD_MAP_OK)
                                            {
                                                fprintf(stderr,
                                                        "emerald runtime: session refused: "
                                                        "map metadata not published "
                                                        "(status %d%s%s)\n",
                                                        (int)mapStatus,
                                                        mapDiag.canonicalName[0] != '\0'
                                                            ? " @ " : "",
                                                        mapDiag.canonicalName);
                                                EmeraldResourceCompat_ClearMigratedEntries();
                                                EmeraldAudioCompat_ClearMigratedEntries();
                                                EmeraldTextCompat_ClearMigratedEntries();
                                                EmeraldLeafCompat_ClearMigratedEntries();
                                                EmeraldGameplayCompat_ClearMigratedEntries();
                                                EmeraldTrainerCompat_ClearMigratedEntries();
                                                EmeraldEncounterCompat_ClearMigratedEntries();
                                                EmeraldFrontierCompat_ClearMigratedEntries();
                                                EmeraldPokedexCompat_ClearMigratedEntries();
                                                EmeraldMapCompat_ClearMigratedEntries();
                                                EmeraldResourceCompat_ClearSnapshot();
                                                EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                                                Gen3ResourceSnapshot_Destroy(snapshot);
                                                snapshot = NULL;
                                                sSnapshotRegistered = false;
                                                status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                                            }
                                            else
                                            {
                                                /* R13-G5: the FIRST LIVE field-script
                                                 * cutover (plan sec 3). After every
                                                 * dependency family published: stage +
                                                 * validate the complete G generation,
                                                 * register the 523 live module ranges,
                                                 * publish the live gStdScripts table,
                                                 * and rebind the R13-F script
                                                 * pointers - all before any script
                                                 * entrypoint can execute. A failure at
                                                 * any step refuses the session with the
                                                 * same full rollback as every earlier
                                                 * seam; compiled field scripts are
                                                 * NEVER executed after this point. */
                                                struct EmeraldScriptCompatDiagnostics scriptDiag;
                                                enum EmeraldScriptCompatStatus scriptStatus =
                                                    EmeraldScriptCompat_TryInitialize(
                                                        snapshot, pack, &scriptDiag);
                                                enum EmeraldBattleLiveStatus liveStatus =
                                                    EMERALD_BATTLE_LIVE_OK;
                                                if (scriptStatus == EMERALD_SCRIPT_OK)
                                                    scriptStatus =
                                                        EmeraldScriptCompat_RegisterRanges();
                                                if (scriptStatus == EMERALD_SCRIPT_OK
                                                 && EmeraldScriptCompat_GetRangeCount() != 6377u)
                                                    scriptStatus =
                                                        EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
                                                if (scriptStatus == EMERALD_SCRIPT_OK)
                                                    scriptStatus =
                                                        EmeraldScriptCompat_PublishStdScripts();
                                                if (scriptStatus == EMERALD_SCRIPT_OK
                                                 && !EmeraldMapCompat_RebindScripts())
                                                    scriptStatus =
                                                        EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
                                                if (scriptStatus != EMERALD_SCRIPT_OK)
                                                {
                                                    fprintf(stderr,
                                                            "emerald runtime: session refused: "
                                                            "field-script generation not "
                                                            "published (status %d%s%s)\n",
                                                            (int)scriptStatus,
                                                            scriptDiag.canonicalName[0] != '\0'
                                                                ? " @ " : "",
                                                            scriptDiag.canonicalName);
                                                    EmeraldScriptCompat_ClearMigratedEntries();
                                                    EmeraldResourceCompat_ClearMigratedEntries();
                                                    EmeraldAudioCompat_ClearMigratedEntries();
                                                    EmeraldTextCompat_ClearMigratedEntries();
                                                    EmeraldLeafCompat_ClearMigratedEntries();
                                                    EmeraldGameplayCompat_ClearMigratedEntries();
                                                    EmeraldTrainerCompat_ClearMigratedEntries();
                                                    EmeraldEncounterCompat_ClearMigratedEntries();
                                                    EmeraldFrontierCompat_ClearMigratedEntries();
                                                    EmeraldPokedexCompat_ClearMigratedEntries();
                                                    EmeraldMapCompat_ClearMigratedEntries();
                                                    EmeraldResourceCompat_ClearSnapshot();
                                                    EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                                                    Gen3ResourceSnapshot_Destroy(snapshot);
                                                    snapshot = NULL;
                                                    sSnapshotRegistered = false;
                                                    status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                                                }
                                                else
                                                {
                                            /* R13-H4/H5: the LIVE battle-family
                                             * cutover (brief sec 17). After the G
                                             * script family published: validate the
                                             * battle + anim + field-effect pack
                                             * surfaces, stage the combined
                                             * generation, register the 3 live
                                             * arena ranges (6,377 -> 6,380),
                                             * publish live execution, and hand
                                             * the State-v5 adapter its surface
                                             * layouts - all before any battle /
                                             * animation / field-effect
                                             * instruction can execute through the
                                             * live seam. A failure at any step
                                             * refuses the session with the same
                                             * full rollback as every earlier
                                             * seam; compiled battle/anim/FE
                                             * payloads are NEVER executed after
                                             * this point. The weak probe above
                                             * is NULL only in the harness-only H3
                                             * shadow link (the two battle seams
                                             * can never co-link). */
                                            if (EmeraldBattleLive_TryInitialize != NULL)
                                            {
                                                struct EmeraldBattleCompatDiagnostics liveDiag;
                                                liveStatus = EmeraldBattleLive_TryInitialize(
                                                    snapshot, pack, 0u, &liveDiag);
                                                if (liveStatus == EMERALD_BATTLE_LIVE_OK)
                                                    liveStatus =
                                                        EmeraldBattleLive_RegisterRanges();
                                                if (liveStatus == EMERALD_BATTLE_LIVE_OK
                                                 && EmeraldBattleLive_GetRangeCount() != 6380u)
                                                    liveStatus =
                                                        EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_COUNT;
                                                if (liveStatus == EMERALD_BATTLE_LIVE_OK)
                                                    liveStatus =
                                                        EmeraldBattleLive_Publish();
                                                if (liveStatus != EMERALD_BATTLE_LIVE_OK)
                                                {
                                                    fprintf(stderr,
                                                            "emerald runtime: session refused: "
                                                            "battle live generation not "
                                                            "published (status %d%s%s)\n",
                                                            (int)liveStatus,
                                                            liveDiag.canonicalName[0] != '\0'
                                                                ? " @ " : "",
                                                            liveDiag.canonicalName);
                                                    EmeraldBattleLive_ClearMigratedEntries();
                                                    EmeraldScriptCompat_ClearMigratedEntries();
                                                    EmeraldResourceCompat_ClearMigratedEntries();
                                                    EmeraldAudioCompat_ClearMigratedEntries();
                                                    EmeraldTextCompat_ClearMigratedEntries();
                                                    EmeraldLeafCompat_ClearMigratedEntries();
                                                    EmeraldGameplayCompat_ClearMigratedEntries();
                                                    EmeraldTrainerCompat_ClearMigratedEntries();
                                                    EmeraldEncounterCompat_ClearMigratedEntries();
                                                    EmeraldFrontierCompat_ClearMigratedEntries();
                                                    EmeraldPokedexCompat_ClearMigratedEntries();
                                                    EmeraldMapCompat_ClearMigratedEntries();
                                                    EmeraldResourceCompat_ClearSnapshot();
                                                    EmeraldResourceCompat_SetSessionContentFingerprint(NULL);
                                                    Gen3ResourceSnapshot_Destroy(snapshot);
                                                    snapshot = NULL;
                                                    sSnapshotRegistered = false;
                                                    status = EMERALD_COMPAT_ERR_PUBLISH_FAILED;
                                                }
                                            }
                                            if (status == EMERALD_COMPAT_OK)
                                            {
                                            /* R10-F: the session content fingerprint
                                             * pins the exact logical provider content
                                             * this session was built from (the
                                             * construction lives in
                                             * emerald_resource_session.h). The
                                             * native save-state system stamps it
                                             * into every state it writes and
                                             * rejects states whose recorded
                                             * fingerprint differs from the active
                                             * session's - equivalent content at
                                             * another path still matches, changed
                                             * or reordered providers cannot. */
                                            uint8_t sessionFingerprint[GEN3_PACK_SHA256_SIZE];
                                            sSnapshotRegistered = true;
                                            status = EMERALD_COMPAT_OK;
                                            if (EmeraldResourceSession_ComputeBaseFingerprint(
                                                    &info, sessionFingerprint))
                                                EmeraldResourceCompat_SetSessionContentFingerprint(
                                                    sessionFingerprint);
                                            else
                                                EmeraldResourceCompat_SetSessionContentFingerprint(
                                                    NULL);
                                            }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Gen3ResourceDiagnostics_Destroy(&diagnostics);

done:
    if (candidate != NULL)
        Gen3ResourceCandidate_Destroy(candidate);
    if (catalog != NULL)
        Gen3ResourceCatalog_Destroy(catalog);
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    return status;
}

/* R12-E §12.2/§12.3: whether a runtime session is registered. The loader
 * owns the flag (it is set only by a fully successful registration; a
 * refused session leaves it false). Session-ful links (production) must
 * fail a load whose post-load resource republishes (trainer tables, audio
 * arena) cannot run; session-less links (test/offline builds that never
 * call the cutover) skip the republishes entirely (R12-F §5 gates both on
 * this flag) - the arena's absence alone cannot distinguish "never
 * registered" from "registered then cleared", and the cleared case must
 * refuse. */
bool EmeraldResourceCompat_IsSessionRegistered(void)
{
    return sSnapshotRegistered;
}

#endif /* PLATFORM_SDL2 && NATIVE_LINUX */
