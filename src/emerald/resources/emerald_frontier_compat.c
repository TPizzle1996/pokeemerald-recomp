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

#define EMERALD_FRONTIER_MAX_REG_RANGES 1u
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

    sPublishedCount = EMERALD_FRONTIER_TRAINER_COUNT * 2u
                    + 3u * EMERALD_FRONTIER_TENT_TRAINER_COUNT * 2u
                    + 4u + 1u + 1u;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
    result = EMERALD_FRONTIER_OK;

done:
    if (arena != NULL)
        free(arena);
    return result;
}

/* ---- State-v5 arena-range registration (the mon-set arena only). ---- */

static bool RegisterMonSetRange(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    bool allRanges = true;
    size_t r;
    (void)r;
    sRegisteredRangeCount = 0u;
    if (index == NULL || sMonSetArena == NULL)
        return true;

    /* ONE COMPAT_OBJECT range over the packed mon-set arena. Every published
     * trainer row's monSet pointer (frontier + tents) resolves into this arena;
     * a State-v5 walk that follows a serialized monSet pointer must reconcile
     * the addresses, so the span is registered just like the levelup /
     * trainer-party arenas. The HOST_DATA trainer/mon/tent tables live at
     * fixed host_data addresses (the same binary), so no ranges over them. No
     * per-resource ranges. */
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
    return allRanges;
}

static void UnregisterMonSetRange(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    size_t r;
    if (index == NULL)
        return;
    for (r = 0u; r < sRegisteredRangeCount; r++)
    {
        uintptr_t base = sRegisteredRanges[r].base;
        size_t j;
        for (j = 0u; j < index->rangeCount; j++)
        {
            if (index->ranges[j].base == base)
            {
                memmove(&index->ranges[j], &index->ranges[j + 1u],
                        (index->rangeCount - j - 1u) * sizeof(index->ranges[0]));
                index->rangeCount--;
                break;
            }
        }
    }
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
    }
    return "unknown";
}