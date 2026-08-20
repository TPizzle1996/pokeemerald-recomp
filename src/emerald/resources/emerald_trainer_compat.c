/* R13-E1: trainer-data (gTrainers / party leaves / trainer class names)
 * publication seam. See include/emerald/resources/emerald_trainer_compat.h
 * for the contract.
 *
 * Publishes the trainer families (structured-data schema 16 metadata, 17
 * party, 18 class-name) into their native HOST_DATA fill targets from the
 * production pack through the NORMAL M0/M1 snapshot.
 *
 * REFUSE-CLASS: the compiled const definitions are NATIVE_LINUX-guarded out
 * of the link (src/data/trainers.h, src/data/text/trainer_class_names.h)
 * and the party leaves are guarded too (src/data/trainer_parties.h), so
 * there is no compiled fallback: a session whose trainer data cannot publish
 * is refused and the loader rolls the whole registration back.
 *
 * Transactional phases: 1 validates every trainer metadata row (schema 16,
 * size 40, ROM_BASE winner, M0/M1 resolution, pack byte equality), every
 * party leaf (schema 17; partySize x ROM stride == leaf size; generated
 * variant == partyFlags; the row's GBA party pointer resolves to the leaf's
 * ROM address), the 66 class-name rows, and the contiguous 18,088-byte party
 * tiling, before ANY allocation; 2 builds the packed native party arena and
 * the 855 native rows + 66 class rows (all infallible once phase 1 passed);
 * 3 publishes atomically (pure stores) and registers ONE COMPAT_OBJECT
 * range over the party arena. On any phase-1/2 failure nothing is written
 * and the diagnostics name the first failing resource.
 */

#include "emerald/resources/emerald_trainer_compat.h"
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

static bool RegisterArenaRanges(void);
static void UnregisterArenaRanges(void);

#define EMERALD_TRAINER_MAX_REG_RANGES 1u
#define EMERALD_GBA_ROM_BASE           ((uint32_t)0x08000000u)

/* Party wire / native per-mon strides by variant (partyFlags & 3). */
static size_t RomStride(uint8_t variant)
{
    static const size_t t[4] = { 8u, 16u, 8u, 16u };
    return t[variant & 3u];
}
static size_t NativeStride(uint8_t variant)
{
    static const size_t t[4] = { 6u, 14u, 8u, 16u };
    return t[variant & 3u];
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

/* Lua charmap name row validation (same as the gameplay seam). */
static bool ValidNameRow(const uint8_t *b, size_t len)
{
    size_t i;
    for (i = 0u; i < len; i++)
    {
        if (b[i] == 0xFF)
        {
            size_t k;
            for (k = i + 1u; k < len; k++)
                if (b[k] != 0x00)
                    return false;
            return true;
        }
    }
    return false;
}

static uint8_t *sArenaBytes;
static size_t sArenaByteTotal;
static size_t sPublishedCount;

/* Native fill rows prebuilt in phase 2 (infallible once phase 1 passed). */
static struct Trainer sTrainerRows[EMERALD_TRAINER_COUNT];
static u8 sClassRows[EMERALD_TRAINER_CLASS_COUNT][13];

static struct { uintptr_t base; size_t length; } sRegisteredRanges[
    EMERALD_TRAINER_MAX_REG_RANGES];
static size_t sRegisteredRangeCount;

static void ClearDiagnostics(struct EmeraldTrainerCompatDiagnostics *d)
{
    if (d != NULL)
        memset(d, 0, sizeof(*d));
}

static void NoteFailure(struct EmeraldTrainerCompatDiagnostics *d,
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

enum EmeraldTrainerCompatStatus
EmeraldTrainerCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldTrainerCompatDiagnostics *diagnostics)
{
    enum EmeraldTrainerCompatStatus result = EMERALD_TRAINER_OK;
    const uint8_t *metaSrc[EMERALD_TRAINER_COUNT];
    const uint8_t *partySrc[EMERALD_TRAINER_COUNT];
    uint32_t partyRomOffset[EMERALD_TRAINER_COUNT];
    uint8_t partylistPartySize[EMERALD_TRAINER_COUNT];
    uint8_t partylistVariant[EMERALD_TRAINER_COUNT];
    uint8_t *arena = NULL;
    size_t i;
    uint32_t partyCount = 0u;
    size_t packCount;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_TRAINER_SCHEMA_METADATA,
                    0u, 0u, 0u, NULL);
        return EMERALD_TRAINER_ERR_INVALID_ARGUMENT;
    }

    memset(metaSrc, 0, sizeof(metaSrc));
    memset(partySrc, 0, sizeof(partySrc));
    memset(partyRomOffset, 0, sizeof(partyRomOffset));
    memset(partylistPartySize, 0, sizeof(partylistPartySize));
    memset(partylistVariant, 0, sizeof(partylistVariant));
    packCount = Gen3ResourcePack_GetEntryCount(pack);

    /* ---- Phase 1a: every trainer metadata row. ---- */
    for (i = 0u; i < EMERALD_TRAINER_COUNT; i++)
    {
        const char *name = kGameplayTrainerKeys[i];
        struct Gen3ResourceView view;
        const struct Gen3ResourcePackEntry *entry;
        const char *partyKey;
        const struct Gen3ResourcePackEntry *partyEntry = NULL;
        const uint8_t *wire;
        uint8_t flags;
        uint8_t wirePartySize;
        uint8_t variant;
        uint32_t wirePartyPtr;
        uint8_t canonVariant = kGameplayTrainerPartyMeta[i].variant;
        uint8_t canonPartySize = kGameplayTrainerPartyMeta[i].partySize;

        if (!ResolveSessionView(snapshot, name, EMERALD_TRAINER_SCHEMA_METADATA,
                                &view))
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_METADATA, 0u, 0u, 0u, NULL);
            result = EMERALD_TRAINER_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                        &view);
            result = EMERALD_TRAINER_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        entry = Gen3ResourcePack_FindByCanonicalName(pack, name);
        if (entry == NULL
         || entry->payload == NULL
         || entry->payloadSize != EMERALD_TRAINER_ROW_WIRE
         || view.payloadSize != EMERALD_TRAINER_ROW_WIRE
         || memcmp(view.payload, entry->payload, EMERALD_TRAINER_ROW_WIRE) != 0)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        EMERALD_TRAINER_ROW_WIRE,
                        (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
            result = EMERALD_TRAINER_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        wire = view.payload;
        flags = wire[0];
        variant = (uint8_t)(flags & 3u);
        wirePartySize = wire[0x20];
        wirePartyPtr = ReadLe32(wire + 0x24);

        if (variant != canonVariant)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        (uint32_t)canonVariant, (uint32_t)variant, NULL);
            result = EMERALD_TRAINER_ERR_VARIANT_MISMATCH;
            goto done;
        }
        if (wirePartySize != canonPartySize)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        (uint32_t)canonPartySize, (uint32_t)wirePartySize, NULL);
            result = EMERALD_TRAINER_ERR_BAD_PARTY_SIZE;
            goto done;
        }
        metaSrc[i] = wire;

        partyKey = kGameplayTrainerPartyKeys[i];
        if (partyKey[0] == '\0')
        {
            if (wirePartySize != 0u || variant != 0u)
            {
                NoteFailure(diagnostics, "build", name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_TRAINER_SCHEMA_PARTY,
                            EMERALD_TRAINER_SCHEMA_PARTY, 0u, 0u, NULL);
                result = EMERALD_TRAINER_ERR_TABLE_MISMATCH;
                goto done;
            }
            continue; /* TRAINER_NONE: no party leaf */
        }

        /* Resolve the party leaf. */
        if (!ResolveSessionView(snapshot, partyKey,
                                EMERALD_TRAINER_SCHEMA_PARTY, &view))
        {
            NoteFailure(diagnostics, "resolve", partyKey,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_PARTY, 0u, 0u, 0u, NULL);
            result = EMERALD_TRAINER_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", partyKey,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_PARTY,
                        EMERALD_TRAINER_SCHEMA_PARTY,
                        (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                        &view);
            result = EMERALD_TRAINER_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        partyEntry = Gen3ResourcePack_FindByCanonicalName(pack, partyKey);
        if (partyEntry == NULL || partyEntry->payload == NULL
         || view.payloadSize != (uint32_t)(wirePartySize * RomStride(variant))
         || view.payloadSize != partyEntry->payloadSize
         || memcmp(view.payload, partyEntry->payload, view.payloadSize) != 0)
        {
            NoteFailure(diagnostics, "build", partyKey,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_PARTY,
                        EMERALD_TRAINER_SCHEMA_PARTY,
                        (uint32_t)(wirePartySize * RomStride(variant)),
                        (uint32_t)view.payloadSize, NULL);
            result = EMERALD_TRAINER_ERR_TABLE_MISMATCH;
            goto done;
        }
        /* Linkage: the row's GBA party pointer must resolve to this leaf's
         * ROM address exactly (sourceRomOffset is the ROM-relative offset). */
        if (wirePartyPtr != (uint32_t)(partyEntry->sourceRomOffset
                                       + EMERALD_GBA_ROM_BASE))
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_PARTY,
                        EMERALD_TRAINER_SCHEMA_PARTY,
                        (uint32_t)(partyEntry->sourceRomOffset
                                   + EMERALD_GBA_ROM_BASE),
                        wirePartyPtr, NULL);
            result = EMERALD_TRAINER_ERR_PARTY_LINK;
            goto done;
        }

        partySrc[i] = view.payload;
        partyRomOffset[i] = (uint32_t)partyEntry->sourceRomOffset;
        partylistPartySize[i] = wirePartySize;
        partylistVariant[i] = variant;
        partyCount++;
    }
    if (partyCount != EMERALD_TRAINER_PARTY_COUNT)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_TRAINER_SCHEMA_PARTY,
                    EMERALD_TRAINER_SCHEMA_PARTY,
                    EMERALD_TRAINER_PARTY_COUNT, partyCount, NULL);
        result = EMERALD_TRAINER_ERR_UNEXPECTED_COUNT;
        goto done;
    }

    /* ---- Phase 1b: the 66 class-name rows. ---- */
    for (i = 0u; i < EMERALD_TRAINER_CLASS_COUNT; i++)
    {
        const char *name = kGameplayTrainerClassKeys[i];
        struct Gen3ResourceView view;
        const struct Gen3ResourcePackEntry *entry;

        if (!ResolveSessionView(snapshot, name, EMERALD_TRAINER_SCHEMA_CLASS,
                                &view))
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_CLASS, 0u, 0u, 0u, NULL);
            result = EMERALD_TRAINER_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_CLASS,
                        EMERALD_TRAINER_SCHEMA_CLASS,
                        (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                        &view);
            result = EMERALD_TRAINER_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        entry = Gen3ResourcePack_FindByCanonicalName(pack, name);
        if (entry == NULL || entry->payload == NULL
         || entry->payloadSize != EMERALD_TRAINER_CLASS_WIRE
         || view.payloadSize != EMERALD_TRAINER_CLASS_WIRE
         || memcmp(view.payload, entry->payload, EMERALD_TRAINER_CLASS_WIRE) != 0)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_CLASS,
                        EMERALD_TRAINER_SCHEMA_CLASS,
                        EMERALD_TRAINER_CLASS_WIRE,
                        (uint32_t)(entry ? entry->payloadSize : 0u), NULL);
            result = EMERALD_TRAINER_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (!ValidNameRow(view.payload, EMERALD_TRAINER_CLASS_WIRE))
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_CLASS,
                        EMERALD_TRAINER_SCHEMA_CLASS,
                        EMERALD_TRAINER_CLASS_WIRE, 0u, NULL);
            result = EMERALD_TRAINER_ERR_TABLE_MISMATCH;
            goto done;
        }
        memcpy(sClassRows[i], view.payload, EMERALD_TRAINER_CLASS_WIRE);
    }

    /* ---- Phase 1c: the party leaves tile the contiguous 18,088-byte block
     * in trainer-index order, no overlap/gap. ---- */
    {
        size_t cursor = EMERALD_TRAINER_PARTY_BLOCK_START;
        for (i = 0u; i < EMERALD_TRAINER_COUNT; i++)
        {
            size_t leafSize;
            if (partySrc[i] == NULL)
                continue;
            leafSize = (size_t)partylistPartySize[i]
                     * RomStride(partylistVariant[i]);
            if (partyRomOffset[i] != cursor)
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer),
                         "party tiling gap @ trainer %zu (leaf 0x%x, expected "
                         "0x%zx)", i, partyRomOffset[i], cursor);
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID,
                            EMERALD_TRAINER_SCHEMA_PARTY,
                            EMERALD_TRAINER_SCHEMA_PARTY,
                            (uint32_t)cursor, partyRomOffset[i], NULL);
                result = EMERALD_TRAINER_ERR_TILING;
                goto done;
            }
            cursor += leafSize;
        }
        if (cursor != EMERALD_TRAINER_PARTY_BLOCK_END)
        {
            NoteFailure(diagnostics, "build", NULL,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_PARTY,
                        EMERALD_TRAINER_SCHEMA_PARTY,
                        EMERALD_TRAINER_PARTY_BLOCK_END, (uint32_t)cursor, NULL);
            result = EMERALD_TRAINER_ERR_TILING;
            goto done;
        }
    }

    /* ---- Phase 1d: pack-level trainer-inventory set equality. The whole
     * pack must carry EXACTLY the 1775 trainer-family entries (the schema
     * 16/17/18 structured-data codes are reserved to these families), so a
     * stray/unexpected trainer record cannot appear unnoticed. ---- */
    {
        size_t metaCount = 0u, partyCount = 0u, classCount = 0u;
        for (i = 0u; i < packCount; i++)
        {
            const struct Gen3ResourcePackEntry *pe = Gen3ResourcePack_GetEntry(pack, i);
            if (pe == NULL || pe->type != GEN3_RESOURCE_TYPE_STRUCTURED_DATA)
                continue;
            if (pe->schema == EMERALD_TRAINER_SCHEMA_METADATA) metaCount++;
            else if (pe->schema == EMERALD_TRAINER_SCHEMA_PARTY) partyCount++;
            else if (pe->schema == EMERALD_TRAINER_SCHEMA_CLASS) classCount++;
        }
        if (metaCount != EMERALD_TRAINER_COUNT
         || partyCount != EMERALD_TRAINER_PARTY_COUNT
         || classCount != EMERALD_TRAINER_CLASS_COUNT)
        {
            NoteFailure(diagnostics, "build", NULL,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        EMERALD_TRAINER_SCHEMA_METADATA,
                        (uint32_t)(EMERALD_TRAINER_COUNT
                                    + EMERALD_TRAINER_PARTY_COUNT
                                    + EMERALD_TRAINER_CLASS_COUNT),
                        (uint32_t)(metaCount + partyCount + classCount), NULL);
            result = EMERALD_TRAINER_ERR_UNEXPECTED_COUNT;
            goto done;
        }
    }

    /* ---- Phase 2: build the packed native party arena + the native rows. */
    {
        size_t arenaTotal = 0u;
        for (i = 0u; i < EMERALD_TRAINER_COUNT; i++)
            if (partySrc[i] != NULL)
                arenaTotal += (size_t)partylistPartySize[i]
                            * NativeStride(partylistVariant[i]);
        arena = (uint8_t *)malloc(arenaTotal > 0u ? arenaTotal : 1u);
        if (arena == NULL)
        {
            NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, EMERALD_TRAINER_SCHEMA_PARTY,
                        EMERALD_TRAINER_SCHEMA_PARTY, 0u, 0u, NULL);
            result = EMERALD_TRAINER_ERR_OUT_OF_MEMORY;
            goto done;
        }
        {
            size_t cursor = 0u;
            for (i = 0u; i < EMERALD_TRAINER_COUNT; i++)
            {
                struct Trainer *row = &sTrainerRows[i];
                uint8_t variant = partylistVariant[i];
                uint8_t psz = partylistPartySize[i];
                size_t m;

                /* Copy the 36 canonical scalar bytes (partyFlags .. partySize
                 * and the fixed-width name/items/aiFields) untouched; the
                 * party pointer is rebuilt below. The wire's 4-byte party
                 * pointer at 0x24 is intentionally not part of the copy. */
                memcpy(row, metaSrc[i], 36u);
                memset(&row->party, 0, sizeof(row->party));

                if (partySrc[i] == NULL)
                {
                    row->partySize = 0u;
                    continue;
                }
                for (m = 0u; m < psz; m++)
                {
                    const uint8_t *w = partySrc[i] + m * RomStride(variant);
                    uint8_t *dst = arena + cursor + m * NativeStride(variant);
                    switch (variant)
                    {
                    case 0u:
                    {
                        struct TrainerMonNoItemDefaultMoves mon;
                        mon.iv = ReadLe16(w + 0u);
                        mon.lvl = w[2];
                        mon.species = ReadLe16(w + 4u);
                        memcpy(dst, &mon, sizeof(mon));
                        break;
                    }
                    case 1u:
                    {
                        struct TrainerMonNoItemCustomMoves mon;
                        uint32_t k;
                        mon.iv = ReadLe16(w + 0u);
                        mon.lvl = w[2];
                        mon.species = ReadLe16(w + 4u);
                        for (k = 0u; k < 4u; k++)
                            mon.moves[k] = ReadLe16(w + 6u + k * 2u);
                        memcpy(dst, &mon, sizeof(mon));
                        break;
                    }
                    case 2u:
                    {
                        struct TrainerMonItemDefaultMoves mon;
                        mon.iv = ReadLe16(w + 0u);
                        mon.lvl = w[2];
                        mon.species = ReadLe16(w + 4u);
                        mon.heldItem = ReadLe16(w + 6u);
                        memcpy(dst, &mon, sizeof(mon));
                        break;
                    }
                    default:
                    {
                        struct TrainerMonItemCustomMoves mon;
                        uint32_t k;
                        mon.iv = ReadLe16(w + 0u);
                        mon.lvl = w[2];
                        mon.species = ReadLe16(w + 4u);
                        mon.heldItem = ReadLe16(w + 6u);
                        for (k = 0u; k < 4u; k++)
                            mon.moves[k] = ReadLe16(w + 8u + k * 2u);
                        memcpy(dst, &mon, sizeof(mon));
                        break;
                    }
                    }
                }
                /* Rebuild the party pointer into the arena (host pointer). */
                switch (variant)
                {
                case 0u: row->party.NoItemDefaultMoves =
                    (const struct TrainerMonNoItemDefaultMoves *)
                        (arena + cursor); break;
                case 1u: row->party.NoItemCustomMoves =
                    (const struct TrainerMonNoItemCustomMoves *)
                        (arena + cursor); break;
                case 2u: row->party.ItemDefaultMoves =
                    (const struct TrainerMonItemDefaultMoves *)
                        (arena + cursor); break;
                default: row->party.ItemCustomMoves =
                    (const struct TrainerMonItemCustomMoves *)
                        (arena + cursor); break;
                }
                cursor += (size_t)psz * NativeStride(variant);
            }
        }
        sArenaBytes = arena;
        sArenaByteTotal = arenaTotal;
        arena = NULL; /* now owned by sArenaBytes */
    }

    /* ---- Phase 3: publish atomically. ---- */
    memcpy(gTrainers, sTrainerRows, sizeof(sTrainerRows));
    memcpy(gTrainerClassNames, sClassRows, sizeof(sClassRows));

    if (!RegisterArenaRanges())
    {
        NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_TRAINER_SCHEMA_PARTY,
                    EMERALD_TRAINER_SCHEMA_PARTY, 0u, 0u, NULL);
        result = EMERALD_TRAINER_ERR_RANGE_REGISTRATION;
        goto done;
    }

    sPublishedCount = EMERALD_TRAINER_COUNT + EMERALD_TRAINER_PARTY_COUNT
                    + EMERALD_TRAINER_CLASS_COUNT;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
    result = EMERALD_TRAINER_OK;

done:
    if (arena != NULL)
        free(arena);
    return result;
}

/* ---- State-v5 arena-range registration (the party arena only). ---- */

static bool RegisterArenaRanges(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    bool allRanges = true;
    size_t r;
    (void)index;
    (void)r;
    sRegisteredRangeCount = 0u;
    if (index == NULL || sArenaBytes == NULL)
        return true;

    /* ONE COMPAT_OBJECT range over the packed party arena. The arena is
     * host memory (native-packed leaves) that gTrainers[i].party points
     * into; a State-v5 walk that follows a serialized trainer party pointer
     * must reconcile the addresses, so the span is registered just like the
     * gameplay levelup arena. No per-trainer ranges. */
    if (!EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)sArenaBytes, sArenaByteTotal,
            "emerald:data/arena/trainer-party",
            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            EMERALD_TRAINER_SCHEMA_PARTY,
            EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        allRanges = false;
    else
    {
        sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)sArenaBytes;
        sRegisteredRanges[sRegisteredRangeCount].length = sArenaByteTotal;
        sRegisteredRangeCount++;
    }
    return allRanges;
}

static void UnregisterArenaRanges(void)
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

void EmeraldTrainerCompat_ClearMigratedEntries(void)
{
    size_t i;
    UnregisterArenaRanges();
    for (i = 0u; i < EMERALD_TRAINER_COUNT; i++)
    {
        gTrainers[i].partySize = 0u;
        memset(&gTrainers[i].party, 0, sizeof(gTrainers[i].party));
    }
    memset(gTrainerClassNames, 0, sizeof(gTrainerClassNames));
    free(sArenaBytes);
    sArenaBytes = NULL;
    sArenaByteTotal = 0u;
    sPublishedCount = 0u;
}

void EmeraldTrainerCompat_Shutdown(void)
{
    EmeraldTrainerCompat_ClearMigratedEntries();
}

size_t EmeraldTrainerCompat_GetPublishedCount(void)
{
    return sPublishedCount;
}

size_t EmeraldTrainerCompat_GetPartyArenaBytes(void)
{
    return sArenaByteTotal;
}

const char *EmeraldTrainerCompatStatus_Describe(
    enum EmeraldTrainerCompatStatus status)
{
    switch (status)
    {
    case EMERALD_TRAINER_OK: return "ok";
    case EMERALD_TRAINER_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_TRAINER_ERR_OUT_OF_MEMORY: return "out of memory";
    case EMERALD_TRAINER_ERR_RESOLVE_FAILED: return "resolve failed";
    case EMERALD_TRAINER_ERR_UNEXPECTED_OWNERSHIP: return "unexpected ownership";
    case EMERALD_TRAINER_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_TRAINER_ERR_UNEXPECTED_COUNT: return "unexpected count";
    case EMERALD_TRAINER_ERR_TABLE_MISMATCH: return "table mismatch";
    case EMERALD_TRAINER_ERR_PARTY_LINK: return "party pointer link";
    case EMERALD_TRAINER_ERR_VARIANT_MISMATCH: return "variant/partyFlags mismatch";
    case EMERALD_TRAINER_ERR_BAD_PARTY_SIZE: return "bad party size";
    case EMERALD_TRAINER_ERR_TILING: return "party tiling";
    case EMERALD_TRAINER_ERR_UNEXPECTED_TYPE: return "unexpected type";
    case EMERALD_TRAINER_ERR_UNEXPECTED_SCHEMA: return "unexpected schema";
    case EMERALD_TRAINER_ERR_RANGE_REGISTRATION: return "range registration";
    case EMERALD_TRAINER_ERR_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}