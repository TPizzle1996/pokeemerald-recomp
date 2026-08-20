/* R13-E2: wild-encounter (gWildMonHeaders / map-based WildPokemonInfo / slot
 * tables) publication seam. See
 * include/emerald/resources/emerald_encounter_compat.h for the contract.
 *
 * Publishes the encounter families (structured-data schema 19 headers block +
 * schema 20 per-(map,type) slot tables) from the production pack into the
 * native HOST_DATA fill targets through the NORMAL M0/M1 snapshot.
 *
 * REFUSE-CLASS: the compiled const map-based definitions (src/data/
 * wild_encounters.h) are NATIVE_LINUX-guarded out of the link (the Pier/Pyramid
 * facility tables stay compiled), so there is no compiled fallback: a session
 * whose wild-encounter data cannot publish is refused and the loader rolls the
 * whole registration back.
 *
 * Transactional phases: 1 validates the headers block (2,500 B, MAP_UNDEFINED
 * sentinel at row 124), every slot resource (schema 20, ROM_BASE winner, M0/M1
 * resolution, pack byte equality), the full header->info->slot pointer graph
 * (each header field's GBA info pointer matches its generated info record; each
 * info's slot pointer resolves to the slot table's ROM address), the Altering
 * Cave 9-header ordering (rows 114-122 = map (24,106), keys altering-cave-1..9
 * in header order) and the pack-level schema counts, before ANY allocation; 2
 * builds the slot arena + the 209 native info objects + the 125 native header
 * rows (all infallible once phase 1 passed); 3 publishes atomically (pure
 * stores) and registers ONE COMPAT_OBJECT range over the slot arena. On any
 * phase-1/2 failure nothing is written and the diagnostics name the first
 * failing resource.
 */

#include "emerald/resources/emerald_encounter_compat.h"
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

static bool RegisterSlotRange(void);
static void UnregisterSlotRange(void);

#define EMERALD_ENCOUNTER_MAX_REG_RANGES 1u

/* Slot-table payload must fill a whole number of 4-byte rows. */
static bool SlotRowsOk(uint32_t bytes)
{
    return bytes != 0u && (bytes % EMERALD_ENCOUNTER_SLOT_WIRE) == 0u;
}

static uint16_t ReadLe16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}
static uint32_t ReadLe32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* Header-row GBA-pointer field offsets within the 20-byte wire row (match the
 * native field order: land, water, rock-smash, fishing). */
static const uint32_t ksWireFieldOffset[EMERALD_ENCOUNTER_FIELD_COUNT] =
{
    4u, /* land        @4 */
    8u, /* water       @8 */
    12u, /* rock-smash @12 */
    16u, /* fishing    @16 */
};

static uint8_t *sSlotArena;
static size_t sSlotArenaBytes;
static size_t sPublishedCount;

/* Native fill rows prebuilt in phase 2 (infallible once phase 1 passed). */
static struct WildPokemonHeader sHeaderRows[EMERALD_ENCOUNTER_HEADER_COUNT];
static struct WildPokemonInfo sInfoRows[EMERALD_ENCOUNTER_INFO_COUNT];

static struct { uintptr_t base; size_t length; } sRegisteredRanges[
    EMERALD_ENCOUNTER_MAX_REG_RANGES];
static size_t sRegisteredRangeCount;

static void ClearDiagnostics(struct EmeraldEncounterCompatDiagnostics *d)
{
    if (d != NULL)
        memset(d, 0, sizeof(*d));
}

static void NoteFailure(struct EmeraldEncounterCompatDiagnostics *d,
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

/* kEncounterInfos is sorted by canonical name; binary search. */
static size_t FindEncounterInfoIndex(const char *key)
{
    size_t lo = 0u;
    size_t hi = EMERALD_ENCOUNTER_INFO_COUNT;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = strcmp(key, kEncounterInfos[mid].name);
        if (cmp == 0)
            return mid;
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return (size_t)-1;
}

enum EmeraldEncounterCompatStatus
EmeraldEncounterCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldEncounterCompatDiagnostics *diagnostics)
{
    enum EmeraldEncounterCompatStatus result = EMERALD_ENCOUNTER_OK;
    static const char *kHeadersId = "emerald:data/encounter/headers";
    const uint8_t *headers = NULL;
    const uint8_t *slotSrc[EMERALD_ENCOUNTER_INFO_COUNT];
    int8_t infoByHeader[EMERALD_ENCOUNTER_HEADER_COUNT][EMERALD_ENCOUNTER_FIELD_COUNT];
    uint8_t *arena = NULL;
    size_t packCount;
    size_t h;
    size_t slotCount = 0u;
    uint32_t schema19Count = 0u;
    uint32_t schema20Count = 0u;
    size_t arenaBytes = 0u;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                    0u, 0u, 0u, NULL);
        return EMERALD_ENCOUNTER_ERR_INVALID_ARGUMENT;
    }

    memset(slotSrc, 0, sizeof(slotSrc));
    for (h = 0u; h < EMERALD_ENCOUNTER_HEADER_COUNT; h++)
        for (size_t t = 0u; t < EMERALD_ENCOUNTER_FIELD_COUNT; t++)
            infoByHeader[h][t] = -1;
    packCount = Gen3ResourcePack_GetEntryCount(pack);

    /* ---- Phase 1a: the headers block resource. ---- */
    {
        struct Gen3ResourceView view;
        const struct Gen3ResourcePackEntry *entry;
        if (!ResolveSessionView(snapshot, kHeadersId,
                                EMERALD_ENCOUNTER_SCHEMA_HEADERS, &view))
        {
            NoteFailure(diagnostics, "resolve", kHeadersId,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_ENCOUNTER_SCHEMA_HEADERS, 0u, 0u, 0u, NULL);
            result = EMERALD_ENCOUNTER_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", kHeadersId,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                        EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                        (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                        &view);
            result = EMERALD_ENCOUNTER_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        entry = Gen3ResourcePack_FindByCanonicalName(pack, kHeadersId);
        if (entry == NULL || entry->payload == NULL
         || entry->payloadSize != EMERALD_ENCOUNTER_HEADERS_BYTES
         || view.payloadSize != EMERALD_ENCOUNTER_HEADERS_BYTES
         || memcmp(view.payload, entry->payload, EMERALD_ENCOUNTER_HEADERS_BYTES) != 0)
        {
            NoteFailure(diagnostics, "build", kHeadersId,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                        EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                        EMERALD_ENCOUNTER_HEADERS_BYTES,
                        (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
            result = EMERALD_ENCOUNTER_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        /* Sentinel: row 124 is mapGroup 0xFF mapNum 0xFF with NULL info ptrs. */
        {
            const uint8_t *srow = view.payload
                + (size_t)EMERALD_ENCOUNTER_SENTINEL_ROW * EMERALD_ENCOUNTER_HEADER_WIRE;
            uint32_t fi;
            if (srow[0] != 0xFFu || srow[1] != 0xFFu)
            {
                NoteFailure(diagnostics, "build", kHeadersId,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                            EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                            (uint32_t)(0xFFu | (0xFFu << 8)),
                            (uint32_t)(srow[0] | (srow[1] << 8)), NULL);
                result = EMERALD_ENCOUNTER_ERR_BAD_HEADERS_BLOCK;
                goto done;
            }
            for (fi = 0u; fi < 4u; fi++)
                if (ReadLe32(srow + (4u + fi * 4u)) != 0u)
                {
                    NoteFailure(diagnostics, "build", kHeadersId,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                                EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                                0u, ReadLe32(srow + (4u + fi * 4u)), NULL);
                    result = EMERALD_ENCOUNTER_ERR_BAD_HEADERS_BLOCK;
                    goto done;
                }
        }
        headers = view.payload;
    }

    /* ---- Phase 1b: walk every header field, validate the pointer graph. ---- */
    for (h = 0u; h < EMERALD_ENCOUNTER_HEADER_COUNT; h++)
    {
        const uint8_t *row = headers + (size_t)h * EMERALD_ENCOUNTER_HEADER_WIRE;
        uint8_t mapGroup = row[0];
        uint8_t mapNum = row[1];
        uint32_t t;
        for (t = 0u; t < EMERALD_ENCOUNTER_FIELD_COUNT; t++)
        {
            const char *key = kEncounterHeaderKeys[h][t];
            uint32_t infoPtr = ReadLe32(row + ksWireFieldOffset[t]);
            size_t idx;

            if (infoPtr == 0u)
            {
                if (key[0] != '\0')
                {
                    NoteFailure(diagnostics, "build", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u, 0u, NULL);
                    result = EMERALD_ENCOUNTER_ERR_NULL_FIELD_KEY;
                    goto done;
                }
                continue;
            }
            if (key[0] == '\0')
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "header %zu field %u", h, t);
                NoteFailure(diagnostics, "build", buf,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u,
                            infoPtr, NULL);
                result = EMERALD_ENCOUNTER_ERR_MISSING_SLOT;
                goto done;
            }
            idx = FindEncounterInfoIndex(key);
            if (idx == (size_t)-1)
            {
                NoteFailure(diagnostics, "build", key,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u, 0u, NULL);
                result = EMERALD_ENCOUNTER_ERR_MISSING_SLOT;
                goto done;
            }
            /* header->info edge: the generated record's info ptr equals the
             * header field's GBA pointer, so a reordered/tampered headers
             * block (including an Altering Cave row shuffle) is caught here. */
            if (kEncounterInfos[idx].gbaInfoPtr != infoPtr)
            {
                NoteFailure(diagnostics, "build", key,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT,
                            kEncounterInfos[idx].gbaInfoPtr, infoPtr, NULL);
                result = EMERALD_ENCOUNTER_ERR_HEADER_INFO_PTR;
                goto done;
            }
            /* Resolve the slot resource (schema 20) + byte parity. */
            {
                struct Gen3ResourceView view;
                const struct Gen3ResourcePackEntry *entry;
                if (!ResolveSessionView(snapshot, key,
                                        EMERALD_ENCOUNTER_SCHEMA_SLOT, &view))
                {
                    NoteFailure(diagnostics, "resolve", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u, 0u, 0u, NULL);
                    result = EMERALD_ENCOUNTER_ERR_RESOLVE_FAILED;
                    goto done;
                }
                if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
                {
                    NoteFailure(diagnostics, "resolve", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT,
                                (uint32_t)view.payloadSize,
                                (uint32_t)view.payloadSize, &view);
                    result = EMERALD_ENCOUNTER_ERR_UNEXPECTED_OWNERSHIP;
                    goto done;
                }
                entry = Gen3ResourcePack_FindByCanonicalName(pack, key);
                if (entry == NULL || entry->payload == NULL
                 || view.payloadSize != entry->payloadSize
                 || memcmp(view.payload, entry->payload, view.payloadSize) != 0)
                {
                    NoteFailure(diagnostics, "build", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u, 0u, NULL);
                    result = EMERALD_ENCOUNTER_ERR_PAYLOAD_SIZE_MISMATCH;
                    goto done;
                }
                /* info->slot edge: the generated record's slot ptr must resolve
                 * to this slot table's ROM address exactly. */
                if (kEncounterInfos[idx].gbaSlotPtr
                        != (uint32_t)(entry->sourceRomOffset
                                      + EMERALD_GBA_ROM_BASE_COMPAT))
                {
                    NoteFailure(diagnostics, "build", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT,
                                kEncounterInfos[idx].gbaSlotPtr,
                                (uint32_t)(entry->sourceRomOffset
                                           + EMERALD_GBA_ROM_BASE_COMPAT),
                                NULL);
                    result = EMERALD_ENCOUNTER_ERR_INFO_SLOT_PTR;
                    goto done;
                }
                /* Slot resource size must match the generated row count. */
                if (view.payloadSize != kEncounterInfos[idx].slotRows * 4u
                 || !SlotRowsOk(view.payloadSize))
                {
                    NoteFailure(diagnostics, "build", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT,
                                EMERALD_ENCOUNTER_SCHEMA_SLOT,
                                (uint32_t)(kEncounterInfos[idx].slotRows * 4u),
                                (uint32_t)view.payloadSize, NULL);
                    result = EMERALD_ENCOUNTER_ERR_PAYLOAD_SIZE_MISMATCH;
                    goto done;
                }
                slotSrc[idx] = view.payload;
                infoByHeader[h][t] = (int8_t)idx;
                slotCount++;
            }
            /* Altering Cave row identity: rows 114..122 are map (24,106). */
            if (h == EMERALD_ENCOUNTER_SENTINEL_ROW - 1u)
                (void)0; /* no-op on the sentinel row */
        }
        /* Altering Cave variants: rows 114..122 must be map (24,106). */
        if (h >= EMERALD_ENCOUNTER_ALTERING_ROW0
         && h < EMERALD_ENCOUNTER_ALTERING_ROW0 + 9u)
        {
            size_t var = h - EMERALD_ENCOUNTER_ALTERING_ROW0 + 1u;
            char expect[64];
            const char *land = kEncounterHeaderKeys[h][EMERALD_ENCOUNTER_FIELD_LAND];
            if (mapGroup != EMERALD_ENCOUNTER_ALTERING_MAP_GROUP
             || mapNum != EMERALD_ENCOUNTER_ALTERING_MAP_NUM)
            {
                NoteFailure(diagnostics, "build", "altering-cave",
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT,
                            (uint32_t)(EMERALD_ENCOUNTER_ALTERING_MAP_GROUP << 8)
                                | EMERALD_ENCOUNTER_ALTERING_MAP_NUM,
                            (uint32_t)(mapGroup << 8) | mapNum, NULL);
                result = EMERALD_ENCOUNTER_ERR_ALTERING_CAVE;
                goto done;
            }
            snprintf(expect, sizeof(expect),
                     "emerald:data/encounter/altering-cave-%zu/land", var);
            if (land == NULL || strcmp(land, expect) != 0)
            {
                NoteFailure(diagnostics, "build", "altering-cave",
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT,
                            EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u, 0u, NULL);
                result = EMERALD_ENCOUNTER_ERR_ALTERING_CAVE;
                goto done;
            }
        }
    }

    if (slotCount != EMERALD_ENCOUNTER_INFO_COUNT)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_ENCOUNTER_SCHEMA_SLOT,
                    EMERALD_ENCOUNTER_SCHEMA_SLOT,
                    EMERALD_ENCOUNTER_INFO_COUNT, (uint32_t)slotCount, NULL);
        result = EMERALD_ENCOUNTER_ERR_UNEXPECTED_COUNT;
        goto done;
    }
    /* Every generated info record must be referenced by some header field. */
    for (h = 0u; h < EMERALD_ENCOUNTER_INFO_COUNT; h++)
    {
        if (slotSrc[h] == NULL)
        {
            NoteFailure(diagnostics, "build", kEncounterInfos[h].name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_ENCOUNTER_SCHEMA_SLOT,
                        EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u, 0u, NULL);
            result = EMERALD_ENCOUNTER_ERR_MISSING_SLOT;
            goto done;
        }
    }

    /* ---- Phase 1c: pack-level schema counts (exactly 1 headers + 209 slots). */
    for (size_t i = 0u; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *pe = Gen3ResourcePack_GetEntry(pack, i);
        if (pe == NULL || pe->type != GEN3_RESOURCE_TYPE_STRUCTURED_DATA)
            continue;
        if (pe->schema == EMERALD_ENCOUNTER_SCHEMA_HEADERS) schema19Count++;
        else if (pe->schema == EMERALD_ENCOUNTER_SCHEMA_SLOT) schema20Count++;
    }
    if (schema19Count != 1u || schema20Count != EMERALD_ENCOUNTER_INFO_COUNT)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                    EMERALD_ENCOUNTER_SCHEMA_HEADERS,
                    EMERALD_ENCOUNTER_INFO_COUNT + 1u,
                    schema19Count + schema20Count, NULL);
        result = EMERALD_ENCOUNTER_ERR_UNEXPECTED_COUNT;
        goto done;
    }

    /* ---- Phase 2: build the slot arena + native info rows + header rows. */
    for (h = 0u; h < EMERALD_ENCOUNTER_INFO_COUNT; h++)
        arenaBytes += kEncounterInfos[h].slotRows * 4u;
    arena = (uint8_t *)malloc(arenaBytes > 0u ? arenaBytes : 1u);
    if (arena == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_ENCOUNTER_SCHEMA_SLOT,
                    EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u, 0u, NULL);
        result = EMERALD_ENCOUNTER_ERR_OUT_OF_MEMORY;
        goto done;
    }
    {
        size_t cursor = 0u;
        for (h = 0u; h < EMERALD_ENCOUNTER_INFO_COUNT; h++)
        {
            size_t bytes = (size_t)kEncounterInfos[h].slotRows * 4u;
            memcpy(arena + cursor, slotSrc[h], bytes);
            sInfoRows[h].encounterRate = kEncounterInfos[h].rate;
            sInfoRows[h].wildPokemon =
                (const struct WildPokemon *)(arena + cursor);
            cursor += bytes;
        }
    }
    for (h = 0u; h < EMERALD_ENCOUNTER_HEADER_COUNT; h++)
    {
        const uint8_t *row = headers + (size_t)h * EMERALD_ENCOUNTER_HEADER_WIRE;
        struct WildPokemonHeader *hr = &sHeaderRows[h];
        const struct WildPokemonInfo *fields[EMERALD_ENCOUNTER_FIELD_COUNT];
        uint32_t t;
        hr->mapGroup = row[0];
        hr->mapNum = row[1];
        hr->pad = 0u;
        for (t = 0u; t < EMERALD_ENCOUNTER_FIELD_COUNT; t++)
        {
            /* The header info pointers reference the EXPORTED HOST_DATA
             * gWildEncounterInfos array directly (the single source the
             * seam publishes at phase 3; content committed by the trailing
             * memcpy). Pointers into a private buffer would hide the array
             * from consumers/tests. */
            int8_t idx = infoByHeader[h][t];
            fields[t] = (idx >= 0) ? &gWildEncounterInfos[idx] : NULL;
        }
        hr->landMonsInfo = fields[EMERALD_ENCOUNTER_FIELD_LAND];
        hr->waterMonsInfo = fields[EMERALD_ENCOUNTER_FIELD_WATER];
        hr->rockSmashMonsInfo = fields[EMERALD_ENCOUNTER_FIELD_ROCK_SMASH];
        hr->fishingMonsInfo = fields[EMERALD_ENCOUNTER_FIELD_FISHING];
    }

    /* ---- Phase 3: publish atomically + register the slot-arena range. */
    memcpy(gWildMonHeaders, sHeaderRows, sizeof(sHeaderRows));
    memcpy(gWildEncounterInfos, sInfoRows, sizeof(sInfoRows));

    sSlotArena = arena;
    sSlotArenaBytes = arenaBytes;
    arena = NULL; /* now owned by sSlotArena */

    if (!RegisterSlotRange())
    {
        NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_ENCOUNTER_SCHEMA_SLOT,
                    EMERALD_ENCOUNTER_SCHEMA_SLOT, 0u, 0u, NULL);
        result = EMERALD_ENCOUNTER_ERR_RANGE_REGISTRATION;
        goto done;
    }

    sPublishedCount = EMERALD_ENCOUNTER_INFO_COUNT + 1u;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
    result = EMERALD_ENCOUNTER_OK;

done:
    if (arena != NULL)
        free(arena);
    return result;
}

/* ---- State-v5 arena-range registration (the slot arena only). ---- */

static bool RegisterSlotRange(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    bool allRanges = true;
    size_t r;
    (void)r;
    sRegisteredRangeCount = 0u;
    if (index == NULL || sSlotArena == NULL)
        return true;

    /* ONE COMPAT_OBJECT range over the packed slot arena. gWildMonHeaders[].*
     * info wildPokemon pointers resolve into this arena; a State-v5 walk that
     * follows a serialized wild slot pointer must reconcile the addresses, so
     * the span is registered just like the levelup/trainer-party arenas. No
     * per-resource ranges. */
    if (!EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)sSlotArena, sSlotArenaBytes,
            "emerald:data/arena/encounter-slots",
            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            EMERALD_ENCOUNTER_SCHEMA_SLOT,
            EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        allRanges = false;
    else
    {
        sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)sSlotArena;
        sRegisteredRanges[sRegisteredRangeCount].length = sSlotArenaBytes;
        sRegisteredRangeCount++;
    }
    return allRanges;
}

static void UnregisterSlotRange(void)
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

void EmeraldEncounterCompat_ClearMigratedEntries(void)
{
    size_t i;
    UnregisterSlotRange();
    for (i = 0u; i < EMERALD_ENCOUNTER_HEADER_COUNT; i++)
    {
        gWildMonHeaders[i].mapGroup = 0u;
        gWildMonHeaders[i].mapNum = 0u;
        gWildMonHeaders[i].pad = 0u;
        gWildMonHeaders[i].landMonsInfo = NULL;
        gWildMonHeaders[i].waterMonsInfo = NULL;
        gWildMonHeaders[i].rockSmashMonsInfo = NULL;
        gWildMonHeaders[i].fishingMonsInfo = NULL;
    }
    for (i = 0u; i < EMERALD_ENCOUNTER_INFO_COUNT; i++)
    {
        gWildEncounterInfos[i].encounterRate = 0u;
        gWildEncounterInfos[i].wildPokemon = NULL;
    }
    free(sSlotArena);
    sSlotArena = NULL;
    sSlotArenaBytes = 0u;
    sPublishedCount = 0u;
}

void EmeraldEncounterCompat_Shutdown(void)
{
    EmeraldEncounterCompat_ClearMigratedEntries();
}

size_t EmeraldEncounterCompat_GetPublishedCount(void)
{
    return sPublishedCount;
}

size_t EmeraldEncounterCompat_GetSlotArenaBytes(void)
{
    return sSlotArenaBytes;
}

const char *EmeraldEncounterCompatStatus_Describe(
    enum EmeraldEncounterCompatStatus status)
{
    switch (status)
    {
    case EMERALD_ENCOUNTER_OK: return "ok";
    case EMERALD_ENCOUNTER_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_ENCOUNTER_ERR_OUT_OF_MEMORY: return "out of memory";
    case EMERALD_ENCOUNTER_ERR_RESOLVE_FAILED: return "resolve failed";
    case EMERALD_ENCOUNTER_ERR_UNEXPECTED_OWNERSHIP: return "unexpected ownership";
    case EMERALD_ENCOUNTER_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_ENCOUNTER_ERR_UNEXPECTED_COUNT: return "unexpected count";
    case EMERALD_ENCOUNTER_ERR_BAD_HEADERS_BLOCK: return "bad headers block";
    case EMERALD_ENCOUNTER_ERR_HEADER_INFO_PTR: return "header info pointer";
    case EMERALD_ENCOUNTER_ERR_INFO_SLOT_PTR: return "info slot pointer";
    case EMERALD_ENCOUNTER_ERR_MISSING_SLOT: return "missing slot";
    case EMERALD_ENCOUNTER_ERR_NULL_FIELD_KEY: return "null field key";
    case EMERALD_ENCOUNTER_ERR_ALTERING_CAVE: return "altering cave order";
    case EMERALD_ENCOUNTER_ERR_UNEXPECTED_TYPE: return "unexpected type";
    case EMERALD_ENCOUNTER_ERR_UNEXPECTED_SCHEMA: return "unexpected schema";
    case EMERALD_ENCOUNTER_ERR_RANGE_REGISTRATION: return "range registration";
    case EMERALD_ENCOUNTER_ERR_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}