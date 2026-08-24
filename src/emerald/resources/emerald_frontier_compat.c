/* R13-E3a-1: Emerald Battle Frontier + Battle Tent trainer/mon data
 * publication seam. See include/emerald/resources/emerald_frontier_compat.h
 * for the contract.
 *
 * Publishes the frontier families (structured-data schema 21 trainer metadata /
 * 22 mon-set leaves / 23 shared mons pool / 24 held-items / 25 banned-species)
 * plus the three Battle Tent families (same trainer schema, per-tent mons
 * pools) into their native HOST_DATA fill targets through the NORMAL M0/M1
 * snapshot.
 *
 * REFUSE-CLASS: the compiled const definitions are NATIVE_LINUX-guarded out of
 * the link (see the seam header), so there is no fallback: a session whose
 * frontier data cannot publish is refused and the loader rolls the whole
 * registration back.
 *
 * Mon index-stream model confirmed from the ROM: every frontier/tent trainer's
 * monSet GBA pointer resolves to a distinct 0xFFFF-terminated u16 leaf (indices
 * 0..881 into the shared gBattleFrontierMons pool, or 0..pool-1 for a tent's
 * own pool). The seam validates all 390 row->leaf edges, then rebuilds every
 * native trainer row's monSet into its own packed mon-set arena.
 *
 * Transactional phases: 1 validates every frontier/tent trainer metadata row
 * (52 B, ROM_BASE winner, M0/M1 resolution, pack byte equality), every mon-set
 * leaf (0xFFFF-terminated, in-range indices, GBA pointer == leaf source ROM
 * address), the shared 882-row mons pool (16 B rows), held items, banned
 * species and the pack-level schema counts - BEFORE any allocation; 2 builds
 * the packed mon-set arena + the native mons/trainer/tent rows (all infallible
 * once phase 1 passed); 3 publishes atomically (pure stores) and registers ONE
 * COMPAT_OBJECT range over the mon-set arena. On any phase-1/2 failure nothing
 * is written and the diagnostics name the first failing resource.
 */

#include "emerald/resources/emerald_frontier_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/frontier_aux_native.generated.h"
#include "wild_encounter.h"        /* struct WildPokemon/Info/Header for the E3a-2 wild handoff */
#include "apprentice.h"            /* struct ApprenticeTrainer gApprentices fill target */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
__attribute__((weak))
#endif
struct EmeraldResourceRangeIndex *EmeraldResourceCompat_GetRangeIndex(void);

static bool RegisterMonSetRange(void);
static void UnregisterMonSetRange(void);
static bool RegisterWildSlotRange(void);
static void UnregisterWildSlotRange(void);

#define EMERALD_FRONTIER_MAX_REG_RANGES 10u
#define EMERALD_FRONTIER_GBA_ROM_BASE   ((uint32_t)0x08000000u)

static uint16_t ReadLe16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}
static uint32_t ReadLe32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* Tent pool sizes, indexed by enum FrontierTentTag; tents index their leaves
 * into their OWN mons pool, not the shared 882-row pool. */
static const uint16_t kTentMonCount[3] =
{
    EMERALD_FRONTIER_TENT_SLATEPORT_MONS,
    EMERALD_FRONTIER_TENT_VERDANTURF_MONS,
    EMERALD_FRONTIER_TENT_FALLARBOR_MONS,
};

static uint8_t *sMonSetArena;
static size_t sMonSetArenaTotal;
static size_t sPublishedCount;

/* ---- R13-E3a-2 facility AUX + wild handoff state. ---- */
static uint8_t *sWildSlotArena;         /* packed 11 x 12-row WildPokemon slot arena */
static size_t sWildSlotArenaTotal;
static size_t sAuxPublishedCount;
static struct WildPokemonInfo sFacilityWildInfos[EMERALD_FRONTIER_AUX_WILD_INFO_COUNT];

/* Subfamily helper used by PublishFrontierAux. */
static enum EmeraldFrontierCompatStatus PublishFrontierAux(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldFrontierCompatDiagnostics *diagnostics);

/* Forward decls (defined later in this TU) so the early E3a-2 helpers can call
 * them before the E3a-1 helper definitions. */
static void NoteFailure(struct EmeraldFrontierCompatDiagnostics *d,
                        const char *stage, const char *canonicalName,
                        enum Gen3ResourceType expectedType,
                        enum Gen3ResourceType actualType,
                        uint32_t expectedSchema, uint32_t actualSchema,
                        uint32_t expectedSize, uint32_t actualSize,
                        const struct Gen3ResourceView *view);
static bool ResolveSessionView(const struct Gen3ResourceSnapshot *snapshot,
                               const char *id, uint32_t schema,
                               struct Gen3ResourceView *view);

/* Helper: resolve an aux resource with ROM_BASE ownership + pack parity.
 * Returns the pack entry payload on success, else sets `resultOut`. */
static const uint8_t *AuxResolve(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    const char *id, uint32_t schema, size_t expectedSize,
    struct EmeraldFrontierCompatDiagnostics *d,
    enum EmeraldFrontierCompatStatus *resultOut)
{
    struct Gen3ResourceView view;
    const struct Gen3ResourcePackEntry *entry;
    if (!ResolveSessionView(snapshot, id, schema, &view))
    {
        NoteFailure(d, "build", id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, 0u, 0u, 0u, NULL);
        *resultOut = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
        return NULL;
    }
    if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
    {
        NoteFailure(d, "build", id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, schema,
                    (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                    &view);
        *resultOut = EMERALD_FRONTIER_ERR_UNEXPECTED_OWNERSHIP;
        return NULL;
    }
    entry = Gen3ResourcePack_FindByCanonicalName(pack, id);
    if (entry == NULL || entry->payload == NULL
     || entry->payloadSize != expectedSize
     || view.payloadSize != entry->payloadSize
     || memcmp(view.payload, entry->payload, entry->payloadSize) != 0)
    {
        NoteFailure(d, "build", id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, schema,
                    (uint32_t)expectedSize,
                    (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
        *resultOut = EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH;
        return NULL;
    }
    return entry->payload;
}

/* Native fill rows prebuilt in phase 2 (infallible once phase 1 passed). */
static struct BattleFrontierTrainer
    sTrainerRows[EMERALD_FRONTIER_TRAINER_COUNT];
static struct FacilityMon sMonsRows[EMERALD_FRONTIER_MONS_COUNT];
static u16 sHeldRows[EMERALD_FRONTIER_HELDITEMS_COUNT];
static u16 sBannedRows[EMERALD_FRONTIER_BANNED_COUNT];
static struct BattleFrontierTrainer
    sTentTrainerRows[3][EMERALD_FRONTIER_TENT_TRAINER_COUNT];
static struct FacilityMon sTentMonsRows[3][EMERALD_FRONTIER_TENT_SLATEPORT_MONS];

static struct { uintptr_t base; size_t length; } sRegisteredRanges[
    EMERALD_FRONTIER_MAX_REG_RANGES];
static size_t sRegisteredRangeCount;

static void ClearDiagnostics(struct EmeraldFrontierCompatDiagnostics *d)
{
    if (d != NULL)
        memset(d, 0, sizeof(*d));
}

static void NoteFailure(struct EmeraldFrontierCompatDiagnostics *d,
                        const char *stage, const char *canonicalName,
                        enum Gen3ResourceType expectedType,
                        enum Gen3ResourceType actualType,
                        uint32_t expectedSchema, uint32_t actualSchema,
                        uint32_t expectedSize, uint32_t actualSize,
                        const struct Gen3ResourceView *view)
{
    if (d == NULL)
        return;
    ClearDiagnostics(d);
    if (stage != NULL)
        snprintf(d->stage, sizeof(d->stage), "%s", stage);
    if (canonicalName != NULL)
        snprintf(d->canonicalName, sizeof(d->canonicalName), "%s",
                 canonicalName);
    snprintf(d->expectedType, sizeof(d->expectedType), "%s",
             Gen3ResourceType_Name(expectedType));
    if (actualType != GEN3_RESOURCE_TYPE_INVALID)
        snprintf(d->actualType, sizeof(d->actualType), "%s",
                 Gen3ResourceType_Name(actualType));
    d->expectedSchema = expectedSchema;
    d->actualSchema = actualSchema;
    d->expectedSize = expectedSize;
    d->actualSize = actualSize;
    if (view != NULL && view->winningProviderId != NULL)
    {
        snprintf(d->winningProviderId, sizeof(d->winningProviderId), "%s",
                 view->winningProviderId);
        if (view->winningProviderVersion != NULL)
            snprintf(d->winningProviderVersion,
                     sizeof(d->winningProviderVersion), "%s",
                     view->winningProviderVersion);
        d->winningProviderPrecedence = view->winningProviderPrecedence;
    }
}

static bool ResolveSessionView(const struct Gen3ResourceSnapshot *snapshot,
                               const char *id, uint32_t schema,
                               struct Gen3ResourceView *view)
{
    Gen3ResourceHandle handle;
    if (Gen3ResourceSnapshot_FindHandle(snapshot, id, &handle) != GEN3_RESOURCE_OK)
        return false;
    if (Gen3ResourceSnapshot_Resolve(snapshot, handle,
                                     GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                     schema, view) != GEN3_RESOURCE_OK
        || view->payload == NULL)
        return false;
    return true;
}

/* Validate + record a single mon-set leaf and hand back the leaf payload + the
 * pack entry. Checks 0xFFFF termination, in-range indices and (for the caller
 * below) that every index is < poolSize. */
static bool ValidateMonSetLeaf(const struct Gen3ResourcePack *pack,
                               struct Gen3ResourceView *view,
                               const char *key, uint32_t schema,
                               uint16_t poolSize,
                               const uint8_t **outLeaf,
                               struct Gen3ResourcePackEntry const **outEntry,
                               struct EmeraldFrontierCompatDiagnostics *d,
                               enum EmeraldFrontierCompatStatus *resultOut)
{
    const struct Gen3ResourcePackEntry *entry;
    const uint8_t *p;
    uint32_t n;

    /* The caller already resolved `view` (schema 22) and validated the ROM_BASE
     * winner; here we re-validate the pack payload and the stream shape. */
    entry = Gen3ResourcePack_FindByCanonicalName(pack, key);
    if (entry == NULL || entry->payload == NULL
     || entry->payloadSize < 2u || (entry->payloadSize & 1u) != 0u
     || entry->payloadSize != view->payloadSize
     || memcmp(view->payload, entry->payload, entry->payloadSize) != 0)
    {
        NoteFailure(d, "build", key, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, schema,
                    entry ? (uint32_t)entry->payloadSize : 0u,
                    (uint32_t)view->payloadSize, NULL);
        *resultOut = EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH;
        return false;
    }
    p = entry->payload;
    n = (uint32_t)(entry->payloadSize / 2u);
    /* must end with 0xFFFF and hold at least the terminator */
    if (n == 0u || ReadLe16(p + (n - 1u) * 2u) != 0xFFFFu)
    {
        NoteFailure(d, "build", key, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, schema, 0u, 0u, NULL);
        *resultOut = EMERALD_FRONTIER_ERR_UNTERMINATED_MONSET;
        return false;
    }
    for (uint32_t k = 0u; k < n - 1u; k++)
    {
        uint16_t idx = ReadLe16(p + k * 2u);
        if (idx >= poolSize)
        {
            NoteFailure(d, "build", key, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, schema, schema,
                        (uint32_t)poolSize, idx, NULL);
            *resultOut = EMERALD_FRONTIER_ERR_MON_INDEX;
            return false;
        }
    }
    *outLeaf = entry->payload;
    *outEntry = entry;
    return true;
}

enum EmeraldFrontierCompatStatus
EmeraldFrontierCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldFrontierCompatDiagnostics *diagnostics)
{
    enum EmeraldFrontierCompatStatus result = EMERALD_FRONTIER_OK;
    const uint8_t *metaSrc[EMERALD_FRONTIER_TRAINER_COUNT];
    const uint8_t *leafSrc[EMERALD_FRONTIER_TRAINER_COUNT];
    size_t leafSize[EMERALD_FRONTIER_TRAINER_COUNT];
    const uint8_t *tentMetaSrc[3][EMERALD_FRONTIER_TENT_TRAINER_COUNT];
    const uint8_t *tentLeafSrc[3][EMERALD_FRONTIER_TENT_TRAINER_COUNT];
    size_t tentLeafSize[3][EMERALD_FRONTIER_TENT_TRAINER_COUNT];
    const uint8_t *poolSrc = NULL;
    const uint8_t *heldSrc = NULL;
    const uint8_t *bannedSrc = NULL;
    const uint8_t *tentPoolSrc[3];
    uint8_t *arena = NULL;
    size_t packCount;
    size_t i;
    size_t arenaBytes = 0u;
    uint32_t schema21 = 0u, schema22 = 0u, schema23 = 0u;
    uint32_t schema24 = 0u, schema25 = 0u;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_TRAINER,
                    0u, 0u, 0u, NULL);
        return EMERALD_FRONTIER_ERR_INVALID_ARGUMENT;
    }

    memset(metaSrc, 0, sizeof(metaSrc));
    memset(leafSrc, 0, sizeof(leafSrc));
    memset(leafSize, 0, sizeof(leafSize));
    memset(tentMetaSrc, 0, sizeof(tentMetaSrc));
    memset(tentLeafSrc, 0, sizeof(tentLeafSrc));
    memset(tentLeafSize, 0, sizeof(tentLeafSize));
    for (i = 0u; i < 3u; i++)
        tentPoolSrc[i] = NULL;
    packCount = Gen3ResourcePack_GetEntryCount(pack);

    /* ---- Phase 1a: 300 frontier trainer rows + their mon-set leaves. ---- */
    for (i = 0u; i < EMERALD_FRONTIER_TRAINER_COUNT; i++)
    {
        const char *name = kFrontierTrainerKeys[i];
        const char *mkey = kFrontierMonSetKeys[i];
        struct Gen3ResourceView view;
        const struct Gen3ResourcePackEntry *entry;
        const struct Gen3ResourcePackEntry *leafEntry = NULL;
        const uint8_t *wire;
        uint32_t wireMonSet;

        if (!ResolveSessionView(snapshot, name, EMERALD_FRONTIER_SCHEMA_TRAINER,
                                &view))
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_TRAINER, 0u, 0u, 0u, NULL);
            result = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_TRAINER,
                        EMERALD_FRONTIER_SCHEMA_TRAINER,
                        (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                        &view);
            result = EMERALD_FRONTIER_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        entry = Gen3ResourcePack_FindByCanonicalName(pack, name);
        if (entry == NULL || entry->payload == NULL
         || entry->payloadSize != EMERALD_FRONTIER_TRAINER_WIRE
         || view.payloadSize != EMERALD_FRONTIER_TRAINER_WIRE
         || memcmp(view.payload, entry->payload, EMERALD_FRONTIER_TRAINER_WIRE) != 0)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_TRAINER,
                        EMERALD_FRONTIER_SCHEMA_TRAINER,
                        EMERALD_FRONTIER_TRAINER_WIRE,
                        (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
            result = EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        wire = entry->payload;
        wireMonSet = ReadLe32(wire + 48u);

        /* Resolve + validate the mon-set leaf. */
        if (!ResolveSessionView(snapshot, mkey, EMERALD_FRONTIER_SCHEMA_MON_SET,
                                &view))
        {
            NoteFailure(diagnostics, "resolve", mkey,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_MON_SET, 0u, 0u, 0u, NULL);
            result = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", mkey,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_MON_SET,
                        EMERALD_FRONTIER_SCHEMA_MON_SET,
                        (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                        &view);
            result = EMERALD_FRONTIER_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        if (!ValidateMonSetLeaf(pack, &view, mkey, EMERALD_FRONTIER_SCHEMA_MON_SET,
                                (uint16_t)EMERALD_FRONTIER_MONS_COUNT,
                                &leafSrc[i], &leafEntry, diagnostics, &result))
            goto done;
        leafSize[i] = leafEntry->payloadSize;
        /* Linkage: the row's GBA monSet pointer must resolve to this leaf's
         * ROM address exactly (sourceRomOffset is ROM-relative). */
        if (wireMonSet != (uint32_t)(leafEntry->sourceRomOffset
                                     + EMERALD_FRONTIER_GBA_ROM_BASE))
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_TRAINER,
                        EMERALD_FRONTIER_SCHEMA_MON_SET,
                        (uint32_t)(leafEntry->sourceRomOffset
                                   + EMERALD_FRONTIER_GBA_ROM_BASE),
                        wireMonSet, NULL);
            result = EMERALD_FRONTIER_ERR_MONSET_LINK;
            goto done;
        }
        metaSrc[i] = wire;
    }

    /* ---- Phase 1b: shared mons pool + held items + banned species. ---- */
    {
        struct Gen3ResourceView view;
        const struct Gen3ResourcePackEntry *entry;
        const char *id = kFrontierMonsKey;
        if (!ResolveSessionView(snapshot, id, EMERALD_FRONTIER_SCHEMA_MON,
                                &view))
        {
            NoteFailure(diagnostics, "resolve", id,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_MON,
                        0u, 0u, 0u, NULL);
            result = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", id,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_MON,
                        EMERALD_FRONTIER_SCHEMA_MON,
                        (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                        &view);
            result = EMERALD_FRONTIER_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        entry = Gen3ResourcePack_FindByCanonicalName(pack, id);
        if (entry == NULL || entry->payload == NULL
         || view.payloadSize != entry->payloadSize
         || memcmp(view.payload, entry->payload, view.payloadSize) != 0
         || entry->payloadSize != (size_t)EMERALD_FRONTIER_MONS_COUNT
                                     * EMERALD_FRONTIER_MON_WIRE)
        {
            NoteFailure(diagnostics, "build", id,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_MON,
                        EMERALD_FRONTIER_SCHEMA_MON,
                        (uint32_t)((size_t)EMERALD_FRONTIER_MONS_COUNT
                                   * EMERALD_FRONTIER_MON_WIRE),
                        (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
            result = EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        poolSrc = entry->payload;
    }
    {
        struct Gen3ResourceView view;
        const struct Gen3ResourcePackEntry *entry;
        const char *id = kFrontierHeldItemsKey;
        if (!ResolveSessionView(snapshot, id, EMERALD_FRONTIER_SCHEMA_HELD_ITEMS,
                                &view))
        {
            NoteFailure(diagnostics, "resolve", id,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_HELD_ITEMS, 0u, 0u, 0u, NULL);
            result = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
            goto done;
        }
        entry = Gen3ResourcePack_FindByCanonicalName(pack, id);
        if (entry == NULL || entry->payload == NULL
         || entry->payloadSize != (size_t)EMERALD_FRONTIER_HELDITEMS_COUNT * 2u
         || view.payloadSize != entry->payloadSize
         || memcmp(view.payload, entry->payload, entry->payloadSize) != 0)
        {
            NoteFailure(diagnostics, "build", id,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_HELD_ITEMS,
                        EMERALD_FRONTIER_SCHEMA_HELD_ITEMS,
                        (uint32_t)((size_t)EMERALD_FRONTIER_HELDITEMS_COUNT * 2u),
                        (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
            result = EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        heldSrc = entry->payload;
    }
    {
        struct Gen3ResourceView view;
        const struct Gen3ResourcePackEntry *entry;
        const char *id = kFrontierBannedSpeciesKey;
        if (!ResolveSessionView(snapshot, id, EMERALD_FRONTIER_SCHEMA_BANNED,
                                &view))
        {
            NoteFailure(diagnostics, "resolve", id,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_BANNED, 0u, 0u, 0u, NULL);
            result = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
            goto done;
        }
        entry = Gen3ResourcePack_FindByCanonicalName(pack, id);
        if (entry == NULL || entry->payload == NULL
         || entry->payloadSize != (size_t)EMERALD_FRONTIER_BANNED_COUNT * 2u
         || view.payloadSize != entry->payloadSize
         || memcmp(view.payload, entry->payload, entry->payloadSize) != 0)
        {
            NoteFailure(diagnostics, "build", id,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_BANNED,
                        EMERALD_FRONTIER_SCHEMA_BANNED,
                        (uint32_t)((size_t)EMERALD_FRONTIER_BANNED_COUNT * 2u),
                        (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
            result = EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        bannedSrc = entry->payload;
    }

    /* ---- Phase 1c: three tent trainer/mon families. ---- */
    for (size_t t = 0u; t < 3u; t++)
    {
        const char *poolKey = kFrontierTentMonsKeys[t];
        struct Gen3ResourceView view;
        const struct Gen3ResourcePackEntry *entry;
        uint16_t poolSize = kTentMonCount[t];

        if (!ResolveSessionView(snapshot, poolKey, EMERALD_FRONTIER_SCHEMA_MON,
                                &view))
        {
            NoteFailure(diagnostics, "resolve", poolKey,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_MON,
                        0u, 0u, 0u, NULL);
            result = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
            goto done;
        }
        entry = Gen3ResourcePack_FindByCanonicalName(pack, poolKey);
        if (entry == NULL || entry->payload == NULL
         || entry->payloadSize != (size_t)poolSize * EMERALD_FRONTIER_MON_WIRE
         || view.payloadSize != entry->payloadSize
         || memcmp(view.payload, entry->payload, entry->payloadSize) != 0)
        {
            NoteFailure(diagnostics, "build", poolKey,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_MON,
                        EMERALD_FRONTIER_SCHEMA_MON,
                        (uint32_t)((size_t)poolSize * EMERALD_FRONTIER_MON_WIRE),
                        (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
            result = EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        tentPoolSrc[t] = entry->payload;

        for (size_t tm = 0u; tm < EMERALD_FRONTIER_TENT_TRAINER_COUNT; tm++)
        {
            const char *name = kFrontierTentTrainerKeys[t][tm];
            const char *mkey = kFrontierTentMonSetKeys[t][tm];
            const struct Gen3ResourcePackEntry *leafEntry = NULL;
            uint32_t wireMonSet;

            if (!ResolveSessionView(snapshot, name,
                                    EMERALD_FRONTIER_SCHEMA_TRAINER, &view))
            {
                NoteFailure(diagnostics, "resolve", name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_FRONTIER_SCHEMA_TRAINER, 0u, 0u, 0u, NULL);
                result = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
                goto done;
            }
            entry = Gen3ResourcePack_FindByCanonicalName(pack, name);
            if (entry == NULL || entry->payload == NULL
             || entry->payloadSize != EMERALD_FRONTIER_TRAINER_WIRE
             || view.payloadSize != EMERALD_FRONTIER_TRAINER_WIRE
             || memcmp(view.payload, entry->payload,
                       EMERALD_FRONTIER_TRAINER_WIRE) != 0)
            {
                NoteFailure(diagnostics, "build", name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_FRONTIER_SCHEMA_TRAINER,
                            EMERALD_FRONTIER_SCHEMA_TRAINER,
                            EMERALD_FRONTIER_TRAINER_WIRE,
                            (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
                result = EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH;
                goto done;
            }
            wireMonSet = ReadLe32(entry->payload + 48u);

            if (!ResolveSessionView(snapshot, mkey,
                                    EMERALD_FRONTIER_SCHEMA_MON_SET, &view))
            {
                NoteFailure(diagnostics, "resolve", mkey,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_FRONTIER_SCHEMA_MON_SET, 0u, 0u, 0u, NULL);
                result = EMERALD_FRONTIER_ERR_RESOLVE_FAILED;
                goto done;
            }
            if (!ValidateMonSetLeaf(pack, &view, mkey,
                                    EMERALD_FRONTIER_SCHEMA_MON_SET, poolSize,
                                    &tentLeafSrc[t][tm], &leafEntry,
                                    diagnostics, &result))
                goto done;
            tentLeafSize[t][tm] = leafEntry->payloadSize;
            if (wireMonSet != (uint32_t)(leafEntry->sourceRomOffset
                                         + EMERALD_FRONTIER_GBA_ROM_BASE))
            {
                NoteFailure(diagnostics, "build", name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_FRONTIER_SCHEMA_TRAINER,
                            EMERALD_FRONTIER_SCHEMA_MON_SET,
                            (uint32_t)(leafEntry->sourceRomOffset
                                       + EMERALD_FRONTIER_GBA_ROM_BASE),
                            wireMonSet, NULL);
                result = EMERALD_FRONTIER_ERR_MONSET_LINK;
                goto done;
            }
            tentMetaSrc[t][tm] = entry->payload;
        }
    }

    /* ---- Phase 1d: pack-level schema-count set equality. ---- */
    for (i = 0u; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *pe = Gen3ResourcePack_GetEntry(pack, i);
        if (pe == NULL || pe->type != GEN3_RESOURCE_TYPE_STRUCTURED_DATA)
            continue;
        if (pe->schema == EMERALD_FRONTIER_SCHEMA_TRAINER) schema21++;
        else if (pe->schema == EMERALD_FRONTIER_SCHEMA_MON_SET) schema22++;
        else if (pe->schema == EMERALD_FRONTIER_SCHEMA_MON) schema23++;
        else if (pe->schema == EMERALD_FRONTIER_SCHEMA_HELD_ITEMS) schema24++;
        else if (pe->schema == EMERALD_FRONTIER_SCHEMA_BANNED) schema25++;
    }
    if (schema21 != EMERALD_FRONTIER_TRAINER_COUNT + 3u
                              * EMERALD_FRONTIER_TENT_TRAINER_COUNT
     || schema22 != EMERALD_FRONTIER_TRAINER_COUNT + 3u
                              * EMERALD_FRONTIER_TENT_TRAINER_COUNT
     || schema23 != 4u
     || schema24 != 1u
     || schema25 != 1u)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_TRAINER,
                    EMERALD_FRONTIER_SCHEMA_TRAINER, 0u, schema21 + schema22,
                    NULL);
        result = EMERALD_FRONTIER_ERR_UNEXPECTED_COUNT;
        goto done;
    }

    /* ---- Phase 2: build the packed mon-set arena + native rows. ---- */
    for (i = 0u; i < EMERALD_FRONTIER_TRAINER_COUNT; i++)
        arenaBytes += leafSize[i];
    for (size_t t = 0u; t < 3u; t++)
        for (size_t tm = 0u; tm < EMERALD_FRONTIER_TENT_TRAINER_COUNT; tm++)
            arenaBytes += tentLeafSize[t][tm];
    arena = (uint8_t *)malloc(arenaBytes > 0u ? arenaBytes : 1u);
    if (arena == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_MON_SET,
                    EMERALD_FRONTIER_SCHEMA_MON_SET, 0u, 0u, NULL);
        result = EMERALD_FRONTIER_ERR_OUT_OF_MEMORY;
        goto done;
    }
    {
        size_t cursor = 0u;
        for (i = 0u; i < EMERALD_FRONTIER_TRAINER_COUNT; i++)
        {
            memcpy(arena + cursor, leafSrc[i], leafSize[i]);
            memcpy(&sTrainerRows[i], metaSrc[i], 48u);
            sTrainerRows[i].monSet = (const u16 *)(arena + cursor);
            cursor += leafSize[i];
        }
        for (size_t t = 0u; t < 3u; t++)
            for (size_t tm = 0u; tm < EMERALD_FRONTIER_TENT_TRAINER_COUNT; tm++)
            {
                memcpy(arena + cursor, tentLeafSrc[t][tm], tentLeafSize[t][tm]);
                memcpy(&sTentTrainerRows[t][tm], tentMetaSrc[t][tm], 48u);
                sTentTrainerRows[t][tm].monSet = (const u16 *)(arena + cursor);
                cursor += tentLeafSize[t][tm];
            }
    }
    /* Shared mons pool: 16-byte GBA row -> 14-byte native row. */
    for (i = 0u; i < EMERALD_FRONTIER_MONS_COUNT; i++)
    {
        const uint8_t *w = poolSrc + i * EMERALD_FRONTIER_MON_WIRE;
        struct FacilityMon *d = &sMonsRows[i];
        d->species = ReadLe16(w + 0u);
        for (uint32_t k = 0u; k < 4u; k++)
            d->moves[k] = ReadLe16(w + 2u + k * 2u);
        d->itemTableId = w[10u];
        d->evSpread = w[11u];
        d->nature = w[12u];
    }
    /* Tent mons pools: same transform. */
    for (size_t t = 0u; t < 3u; t++)
        for (uint16_t m = 0u; m < kTentMonCount[t]; m++)
        {
            const uint8_t *w = tentPoolSrc[t] + m * EMERALD_FRONTIER_MON_WIRE;
            struct FacilityMon *d = &sTentMonsRows[t][m];
            d->species = ReadLe16(w + 0u);
            for (uint32_t k = 0u; k < 4u; k++)
                d->moves[k] = ReadLe16(w + 2u + k * 2u);
            d->itemTableId = w[10u];
            d->evSpread = w[11u];
            d->nature = w[12u];
        }
    memcpy(sHeldRows, heldSrc, sizeof(sHeldRows));
    memcpy(sBannedRows, bannedSrc, sizeof(sBannedRows));

    /* ---- Phase 3: publish atomically + register the mon-set arena range. */
    memcpy(gBattleFrontierTrainers, sTrainerRows, sizeof(sTrainerRows));
    memcpy(gBattleFrontierMons, sMonsRows, sizeof(sMonsRows));
    memcpy(gBattleFrontierHeldItems, sHeldRows, sizeof(sHeldRows));
    memcpy(gFrontierBannedSpecies, sBannedRows, sizeof(sBannedRows));
    memcpy(gSlateportBattleTentTrainers, sTentTrainerRows[FRONTIER_TENT_SLATEPORT],
           sizeof(sTentTrainerRows[0]));
    memcpy(gVerdanturfBattleTentTrainers, sTentTrainerRows[FRONTIER_TENT_VERDANTURF],
           sizeof(sTentTrainerRows[0]));
    memcpy(gFallarborBattleTentTrainers, sTentTrainerRows[FRONTIER_TENT_FALLARBOR],
           sizeof(sTentTrainerRows[0]));
    memcpy(gSlateportBattleTentMons, sTentMonsRows[FRONTIER_TENT_SLATEPORT],
           (size_t)EMERALD_FRONTIER_TENT_SLATEPORT_MONS * EMERALD_FRONTIER_MON_NATIVE);
    memcpy(gVerdanturfBattleTentMons, sTentMonsRows[FRONTIER_TENT_VERDANTURF],
           (size_t)EMERALD_FRONTIER_TENT_VERDANTURF_MONS * EMERALD_FRONTIER_MON_NATIVE);
    memcpy(gFallarborBattleTentMons, sTentMonsRows[FRONTIER_TENT_FALLARBOR],
           (size_t)EMERALD_FRONTIER_TENT_FALLARBOR_MONS * EMERALD_FRONTIER_MON_NATIVE);

    sMonSetArena = arena;
    sMonSetArenaTotal = arenaBytes;
    arena = NULL; /* now owned by sMonSetArena */

    if (!RegisterMonSetRange())
    {
        NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_FRONTIER_SCHEMA_MON_SET,
                    EMERALD_FRONTIER_SCHEMA_MON_SET, 0u, 0u, NULL);
        result = EMERALD_FRONTIER_ERR_RANGE_REGISTRATION;
        goto done;
    }

    /* ---- R13-E3a-2 facility AUX + pike/pyramid wild handoff. ---- */
    result = PublishFrontierAux(snapshot, pack, diagnostics);
    if (result != EMERALD_FRONTIER_OK)
        goto done;

    sPublishedCount = EMERALD_FRONTIER_TRAINER_COUNT * 2u
                    + 3u * EMERALD_FRONTIER_TENT_TRAINER_COUNT * 2u
                    + 4u + 1u + 1u
                    + sAuxPublishedCount;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
    result = EMERALD_FRONTIER_OK;

done:
    if (arena != NULL)
        free(arena);
    return result;
}

/* =========================================================================
 * R13-E3a-2: facility AUX publication (factory/palace/arena/pike/pyramid/
 * brain/apprentice + pike/pyramid wild-encounter handoff).
 *
 * Transactional like the E3a-1 body: phase 1 validates every resource
 * (schema 26..37; ROM_BASE winner; M0/M1 resolution; pack byte equality; the
 * exact wire sizes; the transformed-row shapes; and every wild
 * header->info->slot GBA-pointer edge + encounter rate) BEFORE any
 * allocation; phase 2 builds the packed native rows + the shared wild slot
 * arena (all infallible once phase 1 passed); phase 3 publishes atomically
 * (pure stores) and registers ONE COMPAT_OBJECT range over the wild arena.
 * On any phase-1/2 failure nothing is written to the fill targets and the
 * diagnostics name the exact facility/table via the EMERALD_FRONTIER_ERR_*
 * subfamily code.
 * ========================================================================= */
static enum EmeraldFrontierCompatStatus PublishFrontierAux(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldFrontierCompatDiagnostics *diagnostics)
{
    enum EmeraldFrontierCompatStatus result = EMERALD_FRONTIER_OK;
    const uint8_t *p;
    size_t i;
    uint8_t *slotArena = NULL;
    size_t slotArenaBytes = 0u;

    sAuxPublishedCount = 0u;

    /* ---- Phase 1a: factory 7 move lists (schema 26, LEAF verbatim). ---- */
    {
        static const size_t sizes[7] = { 56u, 30u, 40u, 54u, 56u, 66u, 12u };
        static u16 *const targets[7] =
        {
            gBattleFactoryMovesTotalPreparation, gBattleFactoryMovesImpossibleToPredict,
            gBattleFactoryMovesWeakeningTheFoe, gBattleFactoryMovesHighRiskHighReturn,
            gBattleFactoryMovesEndurance, gBattleFactoryMovesSlowAndSteady,
            gBattleFactoryMovesDependsOnTheBattlesFlow,
        };
        for (i = 0u; i < 7u; i++)
        {
            p = AuxResolve(snapshot, pack, kFrontierAuxFactoryMoves[i],
                           EMERALD_FRONTIER_SCHEMA_FACTORY, sizes[i], diagnostics, &result);
            if (p == NULL)
                return EMERALD_FRONTIER_ERR_FACTORY_MOVES;
            memcpy(targets[i], p, sizes[i]);
        }
    }

    /* ---- Phase 1b: palace + arena prize arrays (schemas 27/28). ---- */
    p = AuxResolve(snapshot, pack, kFrontierAuxPalaceEarlyKey,
                   EMERALD_FRONTIER_SCHEMA_PALACE, 12u, diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_PALACE_PRIZES;
    memcpy(gBattlePalaceEarlyPrizes, p, 12u);
    p = AuxResolve(snapshot, pack, kFrontierAuxPalaceLateKey,
                   EMERALD_FRONTIER_SCHEMA_PALACE, 18u, diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_PALACE_PRIZES;
    memcpy(gBattlePalaceLatePrizes, p, 18u);
    p = AuxResolve(snapshot, pack, kFrontierAuxArenaShortKey,
                   EMERALD_FRONTIER_SCHEMA_ARENA, 12u, diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_ARENA_PRIZES;
    memcpy(gBattleArenaShortStreakPrizeItems, p, 12u);
    p = AuxResolve(snapshot, pack, kFrontierAuxArenaLongKey,
                   EMERALD_FRONTIER_SCHEMA_ARENA, 18u, diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_ARENA_PRIZES;
    memcpy(gBattleArenaLongStreakPrizeItems, p, 18u);

    /* ---- Phase 1c: pike NPC (25 x 8 -> 6 transform), speeches/hints/heals,
     * wild mons. ---- */
    p = AuxResolve(snapshot, pack, kFrontierAuxPikeNpcKey,
                   EMERALD_FRONTIER_SCHEMA_PIKE_NPC,
                   (size_t)EMERALD_FRONTIER_AUX_PIKE_NPC_SLOTS
                       * EMERALD_FRONTIER_AUX_PIKE_NPC_WIRE,
                   diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_PIKE_NPC;
    result = EMERALD_FRONTIER_ERR_PIKE_NPC;
    for (i = 0u; i < EMERALD_FRONTIER_AUX_PIKE_NPC_SLOTS; i++)
        memcpy(&gBattlePikeNPC[i],
               p + i * EMERALD_FRONTIER_AUX_PIKE_NPC_WIRE,
               EMERALD_FRONTIER_AUX_PIKE_NPC_NATIVE);
    result = EMERALD_FRONTIER_OK;

    {
        size_t pikeSpeeches[3] = { 504u, 9u, 18u };
        void *speechTargets[3] = { gBattlePikeSpeeches, gBattlePikeRoomTypeHints,
                                   gBattlePikeHeals };
        for (i = 0u; i < 3u; i++)
        {
            p = AuxResolve(snapshot, pack, kFrontierAuxPikeSpeeches[i],
                           EMERALD_FRONTIER_SCHEMA_PIKE_SPEECH, pikeSpeeches[i],
                           diagnostics, &result);
            if (p == NULL) return EMERALD_FRONTIER_ERR_PIKE_SPEECH;
            memcpy(speechTargets[i], p, pikeSpeeches[i]);
        }
        static u8 const *const wildTargets[8] =
        {
            (const u8 *)gBattlePikeLvl50Mons1, (const u8 *)gBattlePikeLvl50Mons2,
            (const u8 *)gBattlePikeLvl50Mons3, (const u8 *)gBattlePikeLvl50Mons4,
            (const u8 *)gBattlePikeLvlOpenMons1, (const u8 *)gBattlePikeLvlOpenMons2,
            (const u8 *)gBattlePikeLvlOpenMons3, (const u8 *)gBattlePikeLvlOpenMons4,
        };
        for (i = 0u; i < 8u; i++)
        {
            p = AuxResolve(snapshot, pack, kFrontierAuxPikeWildMons[i],
                           EMERALD_FRONTIER_SCHEMA_PIKE_SPEECH, 36u,
                           diagnostics, &result);
            if (p == NULL)
                return EMERALD_FRONTIER_ERR_PIKE_WILDMON;
            memcpy((void *)wildTargets[i], p, 36u);
        }
    }

    /* ---- Phase 1d: pyramid floor templates (16 x 16 -> 13) + options. ---- */
    p = AuxResolve(snapshot, pack, kFrontierAuxPyramidFloor[0],
                   EMERALD_FRONTIER_SCHEMA_PYRAMID_FLOOR,
                   (size_t)EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_SLOTS
                       * EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_WIRE,
                   diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_PYRAMID_FLOOR;
    result = EMERALD_FRONTIER_ERR_PYRAMID_FLOOR;
    for (i = 0u; i < EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_SLOTS; i++)
        memcpy(&gBattlePyramidFloorTemplates[i],
               p + i * EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_WIRE,
               EMERALD_FRONTIER_AUX_PYRAMID_FLOOR_NATIVE);
    p = AuxResolve(snapshot, pack, kFrontierAuxPyramidFloor[1],
                   EMERALD_FRONTIER_SCHEMA_PYRAMID_FLOOR, 68u,
                   diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_PYRAMID_FLOOR;
    memcpy(gBattlePyramidFloorTemplateOptions, p, 68u);
    result = EMERALD_FRONTIER_OK;

    /* ---- Phase 1e: pyramid pickup items (deduped) + item slots. ---- */
    p = AuxResolve(snapshot, pack, kFrontierAuxPyramidItemKey,
                   EMERALD_FRONTIER_SCHEMA_PYRAMID_ITEM, 400u,
                   diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_PYRAMID_ITEM;
    memcpy(gBattlePyramidPickupItems, p, 400u);
    p = AuxResolve(snapshot, pack, kFrontierAuxPyramidSlotsKey,
                   EMERALD_FRONTIER_SCHEMA_PYRAMID_SLOTS, 126u,
                   diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_PYRAMID_SLOTS;
    memcpy(gBattlePyramidPickupItemSlots, p, 126u);

    /* ---- Phase 1f: brain ids / mons / shared streak (schema 34). ---- */
    p = AuxResolve(snapshot, pack, kFrontierAuxBrainIdsKey,
                   EMERALD_FRONTIER_SCHEMA_BRAIN, 14u, diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_BRAIN_IDS;
    memcpy(gFrontierBrainTrainerIds, p, 14u);
    p = AuxResolve(snapshot, pack, kFrontierAuxBrainMonsKey,
                   EMERALD_FRONTIER_SCHEMA_BRAIN,
                   (size_t)EMERALD_FRONTIER_AUX_BRAIN_MONS
                       * EMERALD_FRONTIER_AUX_BRAIN_MONS_WIRE,
                   diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_BRAIN_MONS;
    memcpy(gFrontierBrainsMons, p,
           (size_t)EMERALD_FRONTIER_AUX_BRAIN_MONS * EMERALD_FRONTIER_AUX_BRAIN_MONS_WIRE);
    p = AuxResolve(snapshot, pack, kFrontierAuxBrainStreakKey,
                   EMERALD_FRONTIER_SCHEMA_BRAIN, 28u, diagnostics, &result);
    if (p == NULL) return EMERALD_FRONTIER_ERR_BRAIN_STREAK;
    memcpy(gFrontierBrainStreakAppearances, p, 28u);

    /* ---- Phase 1g: apprentice 16 x (88 -> 86) transform. ---- */
    result = EMERALD_FRONTIER_ERR_APPRENTICE;
    for (i = 0u; i < EMERALD_FRONTIER_AUX_APPRENTICE_ROWS; i++)
    {
        p = AuxResolve(snapshot, pack, kFrontierAuxApprenticeKeys[i],
                       EMERALD_FRONTIER_SCHEMA_APPRENTICE,
                       EMERALD_FRONTIER_AUX_APPRENTICE_WIRE,
                       diagnostics, &result);
        if (p == NULL) return EMERALD_FRONTIER_ERR_APPRENTICE;
        memcpy(&gApprentices[i], p, EMERALD_FRONTIER_AUX_APPRENTICE_NATIVE);
    }
    result = EMERALD_FRONTIER_OK;

    /* ---- Phase 1h/2: pike/pyramid wild-encounter handoff. ----
     * Validate the gBattlePikeWildMonHeaders + gBattlePyramidWildMonHeaders
     * blocks (schema 36), the per-set slot resources (schema 37) and every
     * header->info->slot GBA-pointer edge against the generated metadata,
     * then build ONE packed 528-byte WildPokemon slot arena + 11 native
     * WildPokemonInfo rows. */
    {
        const uint8_t *hdrBlock[2];
        size_t hdrRows[2] = { 5u, 8u };
        const char *hdrKey[2] = { kFrontierAuxPikeWildHeadersKey,
                                  kFrontierAuxPyramidWildHeadersKey };
        const struct FrontierWildInfoMeta *mat[2] =
        { kFrontierAuxPikeWildInfos, kFrontierAuxPyramidWildInfos };
        size_t setCount[2] = { 4u, 7u };
        const uint8_t *slotSrc[EMERALD_FRONTIER_AUX_WILD_INFO_COUNT];
        uint16_t rates[EMERALD_FRONTIER_AUX_WILD_INFO_COUNT];
        size_t globalInfo = 0u;

        for (int f = 0; f < 2; f++)
        {
            hdrBlock[f] = AuxResolve(snapshot, pack, hdrKey[f],
                                     EMERALD_FRONTIER_SCHEMA_WILD_HEADERS,
                                     hdrRows[f] * 20u, diagnostics, &result);
            if (hdrBlock[f] == NULL) return result;
            /* sentinel row must be all NULL info pointers */
            {
                const uint8_t *last = hdrBlock[f] + (hdrRows[f] - 1u) * 20u;
                if (last[0] != 0xFFu || last[1] != 0xFFu
                 || ReadLe32(last + 4u) != 0u || ReadLe32(last + 8u) != 0u
                 || ReadLe32(last + 12u) != 0u || ReadLe32(last + 16u) != 0u)
                {
                    NoteFailure(diagnostics, "build", hdrKey[f],
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_FRONTIER_SCHEMA_WILD_HEADERS,
                                EMERALD_FRONTIER_SCHEMA_WILD_HEADERS, 0u, 0u, NULL);
                    return EMERALD_FRONTIER_ERR_WILD_HEADER;
                }
            }
            result = EMERALD_FRONTIER_ERR_WILD_INFO;
            for (size_t s = 0u; s < setCount[f]; s++)
            {
                const struct FrontierWildInfoMeta *m = &mat[f][s];
                const struct Gen3ResourcePackEntry *slotEntry;
                uint32_t wireInfo = ReadLe32(hdrBlock[f] + s * 20u + 4u);
                if (wireInfo != m->gbaInfoAddr)
                {
                    NoteFailure(diagnostics, "build", m->key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_FRONTIER_SCHEMA_WILD_HEADERS,
                                EMERALD_FRONTIER_SCHEMA_WILD_HEADERS,
                                m->gbaInfoAddr, wireInfo, NULL);
                    return EMERALD_FRONTIER_ERR_WILD_INFO;
                }
                slotSrc[globalInfo] = AuxResolve(snapshot, pack, m->key,
                                                 EMERALD_FRONTIER_SCHEMA_WILD,
                                                 (size_t)m->slotRows
                                                     * EMERALD_FRONTIER_AUX_WILD_SLOT_WIRE,
                                                 diagnostics, &result);
                if (slotSrc[globalInfo] == NULL)
                {
                    result = EMERALD_FRONTIER_ERR_WILD_SLOT;
                    return result;
                }
                /* info->slot edge: the slot resource's ROM base addresses MUST
                 * equal the generated metadata's dependency (the slotPtr), so a
                 * reordered/forged slot table is refused. */
                slotEntry = Gen3ResourcePack_FindByCanonicalName(pack, m->key);
                if (slotEntry == NULL
                 || m->gbaSlotAddr != (uint32_t)(slotEntry->sourceRomOffset
                                                 + EMERALD_FRONTIER_GBA_ROM_BASE))
                {
                    NoteFailure(diagnostics, "build", m->key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_FRONTIER_SCHEMA_WILD,
                                EMERALD_FRONTIER_SCHEMA_WILD,
                                m->gbaSlotAddr,
                                (uint32_t)(slotEntry ? slotEntry->sourceRomOffset
                                       + EMERALD_FRONTIER_GBA_ROM_BASE : 0u), NULL);
                    return EMERALD_FRONTIER_ERR_WILD_SLOT;
                }
                /* bad rate: pike sets must be rate 10, pyramid sets rate 4/8. */
                if ((f == 0 && m->rate != 10u)
                 || (f == 1 && m->rate != 4u && m->rate != 8u))
                {
                    NoteFailure(diagnostics, "build", m->key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_FRONTIER_SCHEMA_WILD,
                                EMERALD_FRONTIER_SCHEMA_WILD,
                                (uint32_t)m->rate, (uint32_t)m->rate, NULL);
                    return EMERALD_FRONTIER_ERR_WILD_RATE;
                }
                rates[globalInfo] = m->rate;
                globalInfo++;
            }
            result = EMERALD_FRONTIER_OK;
        }
        if (globalInfo != EMERALD_FRONTIER_AUX_WILD_INFO_COUNT)
            return EMERALD_FRONTIER_ERR_WILD_HEADER;

        /* Build the packed slot arena (verify slot bytes vs the pack entry's
         * source ROM address edge through the metadata we already validated)
         * and the native info rows. */
        slotArenaBytes = (size_t)EMERALD_FRONTIER_AUX_WILD_INFO_COUNT
                         * EMERALD_FRONTIER_AUX_WILD_SLOT_ROWS
                         * EMERALD_FRONTIER_AUX_WILD_SLOT_WIRE;
        slotArena = (uint8_t *)malloc(slotArenaBytes > 0u ? slotArenaBytes : 1u);
        if (slotArena == NULL)
        {
            NoteFailure(diagnostics, "build", NULL,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_FRONTIER_SCHEMA_WILD, 0u, 0u, 0u, NULL);
            return EMERALD_FRONTIER_ERR_OUT_OF_MEMORY;
        }
        globalInfo = 0u;
        for (int f = 0; f < 2; f++)
            for (size_t s = 0u; s < setCount[f]; s++)
            {
                size_t rows = EMERALD_FRONTIER_AUX_WILD_SLOT_ROWS;
                size_t bytes = rows * EMERALD_FRONTIER_AUX_WILD_SLOT_WIRE;
                memcpy(slotArena + globalInfo * bytes, slotSrc[globalInfo], bytes);
                sFacilityWildInfos[globalInfo].encounterRate = (uint8_t)rates[globalInfo];
                sFacilityWildInfos[globalInfo].wildPokemon =
                    (const struct WildPokemon *)(slotArena + globalInfo * bytes);
                globalInfo++;
            }
    }

    /* ---- Phase 3: publish atomically + register the wild arena range. ---- */
    {
        size_t gi = 0u;
        for (size_t h = 0u; h < 5u; h++)
        {
            gBattlePikeWildMonHeaders[h].mapGroup = (h < 4u) ? 0u : 0xFFu;
            gBattlePikeWildMonHeaders[h].mapNum = (h < 4u) ? (uint8_t)(h + 1u) : 0xFFu;
            gBattlePikeWildMonHeaders[h].waterMonsInfo = NULL;
            gBattlePikeWildMonHeaders[h].rockSmashMonsInfo = NULL;
            gBattlePikeWildMonHeaders[h].fishingMonsInfo = NULL;
            gBattlePikeWildMonHeaders[h].landMonsInfo =
                (h < 4u) ? &sFacilityWildInfos[gi++] : NULL;
        }
        for (size_t h = 0u; h < 8u; h++)
        {
            gBattlePyramidWildMonHeaders[h].mapGroup = (h < 7u) ? 0u : 0xFFu;
            gBattlePyramidWildMonHeaders[h].mapNum = (h < 7u) ? (uint8_t)(h + 1u) : 0xFFu;
            gBattlePyramidWildMonHeaders[h].waterMonsInfo = NULL;
            gBattlePyramidWildMonHeaders[h].rockSmashMonsInfo = NULL;
            gBattlePyramidWildMonHeaders[h].fishingMonsInfo = NULL;
            gBattlePyramidWildMonHeaders[h].landMonsInfo =
                (h < 7u) ? &sFacilityWildInfos[gi++] : NULL;
        }
    }

    sWildSlotArena = slotArena;
    sWildSlotArenaTotal = slotArenaBytes;
    slotArena = NULL;
    if (!RegisterWildSlotRange())
        return EMERALD_FRONTIER_ERR_RANGE_REGISTRATION;

    /* count of published E3a-2 fill targets (whole tables + apprentice rows) */
    sAuxPublishedCount = 7u + 2u + 2u + 1u + 3u + 8u + 2u + 1u + 1u + 3u
                       + EMERALD_FRONTIER_AUX_APPRENTICE_ROWS
                       + (EMERALD_FRONTIER_AUX_WILD_INFO_COUNT);
    return EMERALD_FRONTIER_OK;
}

/* ---- State-v5 arena-range registration (mon-set + E3a-2 wild arenas). ---- */

/* R13-I: remove every frontier-family span from the index by exact key
 * identity (position-independent), including an older refused generation
 * whose base is no longer tracked in sRegisteredRanges. Covers the two
 * arena spans AND the eight fixed-base HOST_DATA fill-target spans. */
static void RemoveFrontierRanges(struct EmeraldResourceRangeIndex *index)
{
    static const char *const kFrontierKeys[10] =
    {
        "emerald:data/arena/frontier-mon-set",
        "emerald:data/arena/facility-wild-slots",
        "emerald:data/frontier/tower-trainers",
        "emerald:data/frontier/tower-mons",
        "emerald:data/frontier/slateport-tent-trainers",
        "emerald:data/frontier/slateport-tent-mons",
        "emerald:data/frontier/verdanturf-tent-trainers",
        "emerald:data/frontier/verdanturf-tent-mons",
        "emerald:data/frontier/fallarbor-tent-trainers",
        "emerald:data/frontier/fallarbor-tent-mons",
    };
    Gen3ResourceKey keys[(sizeof(kFrontierKeys) / sizeof(kFrontierKeys[0]))];
    size_t k;
    size_t j;

    for (k = 0u; k < (sizeof(kFrontierKeys) / sizeof(kFrontierKeys[0])); k++)
        Gen3ResourceId_DeriveKey(kFrontierKeys[k], &keys[k]);
    j = 0u;
    while (j < index->rangeCount)
    {
        bool match = false;

        for (k = 0u; k < (sizeof(keys) / sizeof(keys[0])); k++)
        {
            if (memcmp(&index->ranges[j].key, &keys[k], sizeof(keys[k])) == 0)
            {
                match = true;
                break;
            }
        }
        if (match)
        {
            memmove(&index->ranges[j], &index->ranges[j + 1u],
                    (index->rangeCount - j - 1u) * sizeof(index->ranges[0]));
            index->rangeCount--;
        }
        else
            j++;
    }
}

static bool RegisterMonSetRange(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    /* R13-I: the published HOST_DATA trainer/mon/tent fill targets get the
     * same COMPAT_OBJECT span treatment as the map headers (the
     * emerald_map_compat.c precedent): the EWRAM globals gFacilityTrainers /
     * gFacilityTrainerMons hold raw pointers into these arrays inside
     * frontier facilities, and a State-v5 capture must reconcile them as
     * key+offset resource records instead of silently persisting raw
     * host_data addresses. The arrays never move (fixed link addresses),
     * so their spans have no generation lifecycle beyond publish/clear.
     * Registration is idempotent: any prior frontier spans (including an
     * older refused generation) are removed by key first, so repeated
     * publish cycles can never duplicate or overlap the fixed-base spans. */
    static const struct
    {
        const void *base;
        size_t length;
        const char *canonicalName;
        uint32_t schema;
    } kHostTableSpans[8] =
    {
        { gBattleFrontierTrainers,         sizeof(gBattleFrontierTrainers),
          "emerald:data/frontier/tower-trainers",      EMERALD_FRONTIER_SCHEMA_TRAINER },
        { gBattleFrontierMons,             sizeof(gBattleFrontierMons),
          "emerald:data/frontier/tower-mons",          EMERALD_FRONTIER_SCHEMA_MON },
        { gSlateportBattleTentTrainers,    sizeof(gSlateportBattleTentTrainers),
          "emerald:data/frontier/slateport-tent-trainers", EMERALD_FRONTIER_SCHEMA_TRAINER },
        { gSlateportBattleTentMons,        sizeof(gSlateportBattleTentMons),
          "emerald:data/frontier/slateport-tent-mons",  EMERALD_FRONTIER_SCHEMA_MON },
        { gVerdanturfBattleTentTrainers,   sizeof(gVerdanturfBattleTentTrainers),
          "emerald:data/frontier/verdanturf-tent-trainers", EMERALD_FRONTIER_SCHEMA_TRAINER },
        { gVerdanturfBattleTentMons,       sizeof(gVerdanturfBattleTentMons),
          "emerald:data/frontier/verdanturf-tent-mons",  EMERALD_FRONTIER_SCHEMA_MON },
        { gFallarborBattleTentTrainers,    sizeof(gFallarborBattleTentTrainers),
          "emerald:data/frontier/fallarbor-tent-trainers", EMERALD_FRONTIER_SCHEMA_TRAINER },
        { gFallarborBattleTentMons,        sizeof(gFallarborBattleTentMons),
          "emerald:data/frontier/fallarbor-tent-mons",    EMERALD_FRONTIER_SCHEMA_MON },
    };
    bool allRanges = true;
    size_t r;
    (void)r;
    sRegisteredRangeCount = 0u;
    if (index == NULL || sMonSetArena == NULL)
        return true;
    RemoveFrontierRanges(index);

    /* ONE COMPAT_OBJECT range over the packed mon-set arena. Every published
     * trainer row's monSet pointer (frontier + tents) resolves into this arena;
     * a State-v5 walk that follows a serialized monSet pointer must reconcile
     * the addresses, so the span is registered just like the levelup /
     * trainer-party arenas. No per-resource ranges. */
    if (!EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)sMonSetArena, sMonSetArenaTotal,
            "emerald:data/arena/frontier-mon-set",
            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            EMERALD_FRONTIER_SCHEMA_MON_SET,
            EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        allRanges = false;
    else
    {
        sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)sMonSetArena;
        sRegisteredRanges[sRegisteredRangeCount].length = sMonSetArenaTotal;
        sRegisteredRangeCount++;
    }
    /* R13-I: the eight HOST_DATA fill-target spans (see above). */
    for (r = 0u; r < (sizeof(kHostTableSpans) / sizeof(kHostTableSpans[0])); r++)
    {
        if (!EmeraldResourceRangeIndex_RegisterSpan(
                index, (uintptr_t)kHostTableSpans[r].base,
                kHostTableSpans[r].length, kHostTableSpans[r].canonicalName,
                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                kHostTableSpans[r].schema,
                EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
            allRanges = false;
        else
        {
            sRegisteredRanges[sRegisteredRangeCount].base =
                (uintptr_t)kHostTableSpans[r].base;
            sRegisteredRanges[sRegisteredRangeCount].length =
                kHostTableSpans[r].length;
            sRegisteredRangeCount++;
        }
    }
    return allRanges;
}

/* R13-E3a-2: ONE COMPAT_OBJECT range over the packed pike/pyramid wild slot
 * arena. Every published facility wild header's landMonsInfo points into the
 * sFacilityWildInfos[] rows, whose wildPokemon pointers resolve into this
 * arena - a State-v5 walk following a serialized wildPokemon must reconcile
 * the addresses. */
static bool RegisterWildSlotRange(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    if (index == NULL || sWildSlotArena == NULL)
        return true;
    if (!EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)sWildSlotArena, sWildSlotArenaTotal,
            "emerald:data/arena/facility-wild-slots",
            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            EMERALD_FRONTIER_SCHEMA_WILD,
            EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        return false;
    sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)sWildSlotArena;
    sRegisteredRanges[sRegisteredRangeCount].length = sWildSlotArenaTotal;
    sRegisteredRangeCount++;
    return true;
}

static void UnregisterMonSetRange(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    if (index == NULL)
        return;
    RemoveFrontierRanges(index);
    sRegisteredRangeCount = 0u;
}

void EmeraldFrontierCompat_ClearMigratedEntries(void)
{
    static struct BattleFrontierTrainer *const kTentTrainerTables[3] =
    {
        gSlateportBattleTentTrainers,
        gVerdanturfBattleTentTrainers,
        gFallarborBattleTentTrainers,
    };
    size_t i;
    UnregisterMonSetRange();
    for (i = 0u; i < EMERALD_FRONTIER_TRAINER_COUNT; i++)
        memset(&gBattleFrontierTrainers[i], 0, sizeof(gBattleFrontierTrainers[i]));
    for (size_t t = 0u; t < 3u; t++)
        for (size_t tm = 0u; tm < EMERALD_FRONTIER_TENT_TRAINER_COUNT; tm++)
            memset(&kTentTrainerTables[t][tm], 0,
                   sizeof(struct BattleFrontierTrainer));
    memset(gBattleFrontierMons, 0, sizeof(gBattleFrontierMons));
    memset(gBattleFrontierHeldItems, 0, sizeof(gBattleFrontierHeldItems));
    memset(gFrontierBannedSpecies, 0, sizeof(gFrontierBannedSpecies));
    memset(gSlateportBattleTentMons, 0,
           (size_t)EMERALD_FRONTIER_TENT_SLATEPORT_MONS * EMERALD_FRONTIER_MON_NATIVE);
    memset(gVerdanturfBattleTentMons, 0,
           (size_t)EMERALD_FRONTIER_TENT_VERDANTURF_MONS * EMERALD_FRONTIER_MON_NATIVE);
    memset(gFallarborBattleTentMons, 0,
           (size_t)EMERALD_FRONTIER_TENT_FALLARBOR_MONS * EMERALD_FRONTIER_MON_NATIVE);
    free(sMonSetArena);
    sMonSetArena = NULL;
    sMonSetArenaTotal = 0u;
    sPublishedCount = 0u;

    /* ---- R13-E3a-2 aux fill targets (zero; NULL the wild header pointers). */
    memset(gBattleFactoryMovesTotalPreparation, 0, sizeof(gBattleFactoryMovesTotalPreparation));
    memset(gBattleFactoryMovesImpossibleToPredict, 0, sizeof(gBattleFactoryMovesImpossibleToPredict));
    memset(gBattleFactoryMovesWeakeningTheFoe, 0, sizeof(gBattleFactoryMovesWeakeningTheFoe));
    memset(gBattleFactoryMovesHighRiskHighReturn, 0, sizeof(gBattleFactoryMovesHighRiskHighReturn));
    memset(gBattleFactoryMovesEndurance, 0, sizeof(gBattleFactoryMovesEndurance));
    memset(gBattleFactoryMovesSlowAndSteady, 0, sizeof(gBattleFactoryMovesSlowAndSteady));
    memset(gBattleFactoryMovesDependsOnTheBattlesFlow, 0, sizeof(gBattleFactoryMovesDependsOnTheBattlesFlow));
    memset(gBattlePalaceEarlyPrizes, 0, sizeof(gBattlePalaceEarlyPrizes));
    memset(gBattlePalaceLatePrizes, 0, sizeof(gBattlePalaceLatePrizes));
    memset(gBattleArenaShortStreakPrizeItems, 0, sizeof(gBattleArenaShortStreakPrizeItems));
    memset(gBattleArenaLongStreakPrizeItems, 0, sizeof(gBattleArenaLongStreakPrizeItems));
    memset(gBattlePikeNPC, 0, sizeof(gBattlePikeNPC));
    memset(gBattlePikeSpeeches, 0, sizeof(gBattlePikeSpeeches));
    memset(gBattlePikeRoomTypeHints, 0, sizeof(gBattlePikeRoomTypeHints));
    memset(gBattlePikeHeals, 0, sizeof(gBattlePikeHeals));
    memset(gBattlePikeLvl50Mons1, 0, sizeof(gBattlePikeLvl50Mons1));
    memset(gBattlePikeLvl50Mons2, 0, sizeof(gBattlePikeLvl50Mons2));
    memset(gBattlePikeLvl50Mons3, 0, sizeof(gBattlePikeLvl50Mons3));
    memset(gBattlePikeLvl50Mons4, 0, sizeof(gBattlePikeLvl50Mons4));
    memset(gBattlePikeLvlOpenMons1, 0, sizeof(gBattlePikeLvlOpenMons1));
    memset(gBattlePikeLvlOpenMons2, 0, sizeof(gBattlePikeLvlOpenMons2));
    memset(gBattlePikeLvlOpenMons3, 0, sizeof(gBattlePikeLvlOpenMons3));
    memset(gBattlePikeLvlOpenMons4, 0, sizeof(gBattlePikeLvlOpenMons4));
    memset(gBattlePyramidFloorTemplates, 0, sizeof(gBattlePyramidFloorTemplates));
    memset(gBattlePyramidFloorTemplateOptions, 0, sizeof(gBattlePyramidFloorTemplateOptions));
    memset(gBattlePyramidPickupItems, 0, sizeof(gBattlePyramidPickupItems));
    memset(gBattlePyramidPickupItemSlots, 0, sizeof(gBattlePyramidPickupItemSlots));
    memset(gFrontierBrainTrainerIds, 0, sizeof(gFrontierBrainTrainerIds));
    memset(gFrontierBrainsMons, 0, sizeof(gFrontierBrainsMons));
    memset(gFrontierBrainStreakAppearances, 0, sizeof(gFrontierBrainStreakAppearances));
    memset(&gApprentices[0], 0,
           (size_t)EMERALD_FRONTIER_AUX_APPRENTICE_ROWS * EMERALD_FRONTIER_AUX_APPRENTICE_NATIVE);
    for (i = 0u; i < EMERALD_FRONTIER_AUX_WILD_INFO_COUNT; i++)
    {
        sFacilityWildInfos[i].wildPokemon = NULL;
        sFacilityWildInfos[i].encounterRate = 0u;
    }
    for (i = 0u; i < EMERALD_FRONTIER_AUX_PIKE_WILD_SETS + 1u; i++)
        memset(&gBattlePikeWildMonHeaders[i], 0, sizeof(struct WildPokemonHeader));
    for (i = 0u; i < EMERALD_FRONTIER_AUX_PYRAMID_WILD_SETS + 1u; i++)
        memset(&gBattlePyramidWildMonHeaders[i], 0, sizeof(struct WildPokemonHeader));
    free(sWildSlotArena);
    sWildSlotArena = NULL;
    sWildSlotArenaTotal = 0u;
    sAuxPublishedCount = 0u;
}

void EmeraldFrontierCompat_Shutdown(void)
{
    EmeraldFrontierCompat_ClearMigratedEntries();
}

size_t EmeraldFrontierCompat_GetPublishedCount(void)
{
    return sPublishedCount;
}

size_t EmeraldFrontierCompat_GetMonSetArenaBytes(void)
{
    return sMonSetArenaTotal;
}

const char *EmeraldFrontierCompatStatus_Describe(
    enum EmeraldFrontierCompatStatus status)
{
    switch (status)
    {
    case EMERALD_FRONTIER_OK: return "ok";
    case EMERALD_FRONTIER_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_FRONTIER_ERR_OUT_OF_MEMORY: return "out of memory";
    case EMERALD_FRONTIER_ERR_RESOLVE_FAILED: return "resolve failed";
    case EMERALD_FRONTIER_ERR_UNEXPECTED_OWNERSHIP: return "unexpected ownership";
    case EMERALD_FRONTIER_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_FRONTIER_ERR_UNEXPECTED_COUNT: return "unexpected count";
    case EMERALD_FRONTIER_ERR_MONSET_LINK: return "mon-set pointer link";
    case EMERALD_FRONTIER_ERR_UNTERMINATED_MONSET: return "unterminated mon-set";
    case EMERALD_FRONTIER_ERR_MON_INDEX: return "mon index out of range";
    case EMERALD_FRONTIER_ERR_MISSING_RESOURCE: return "missing resource";
    case EMERALD_FRONTIER_ERR_UNEXPECTED_TYPE: return "unexpected type";
    case EMERALD_FRONTIER_ERR_UNEXPECTED_SCHEMA: return "unexpected schema";
    case EMERALD_FRONTIER_ERR_RANGE_REGISTRATION: return "range registration";
    case EMERALD_FRONTIER_ERR_UNAVAILABLE: return "unavailable";
    case EMERALD_FRONTIER_ERR_FACTORY_MOVES: return "factory move list";
    case EMERALD_FRONTIER_ERR_PALACE_PRIZES: return "battle palace prize";
    case EMERALD_FRONTIER_ERR_ARENA_PRIZES: return "battle arena prize";
    case EMERALD_FRONTIER_ERR_PIKE_NPC: return "pike npc table";
    case EMERALD_FRONTIER_ERR_PIKE_SPEECH: return "pike speech/hints/heals";
    case EMERALD_FRONTIER_ERR_PIKE_WILDMON: return "pike wild-mon table";
    case EMERALD_FRONTIER_ERR_PYRAMID_FLOOR: return "pyramid floor template/options";
    case EMERALD_FRONTIER_ERR_PYRAMID_ITEM: return "pyramid pickup item pool";
    case EMERALD_FRONTIER_ERR_PYRAMID_SLOTS: return "pyramid pickup item slots";
    case EMERALD_FRONTIER_ERR_BRAIN_IDS: return "frontier brain trainer ids";
    case EMERALD_FRONTIER_ERR_BRAIN_MONS: return "frontier brain mons";
    case EMERALD_FRONTIER_ERR_BRAIN_STREAK: return "brain streak appearances";
    case EMERALD_FRONTIER_ERR_APPRENTICE: return "apprentice transform";
    case EMERALD_FRONTIER_ERR_WILD_HEADER: return "pike/pyramid wild header";
    case EMERALD_FRONTIER_ERR_WILD_INFO: return "wild info pointer edge";
    case EMERALD_FRONTIER_ERR_WILD_SLOT: return "wild slot table";
    case EMERALD_FRONTIER_ERR_WILD_RATE: return "wild encounter rate";
    }
    return "unknown";
}
