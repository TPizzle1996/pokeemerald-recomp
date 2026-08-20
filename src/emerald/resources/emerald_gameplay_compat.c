/* R13-D1: Emerald gameplay-data (species / moves / shared tables / fonts)
 * publication seam.
 * See include/emerald/resources/emerald_gameplay_compat.h for the contract.
 *
 * Publishes the D1 structured-data families + fonts into their native
 * HOST_DATA fill targets (struct SpeciesInfo gSpeciesInfo, BattleMove,
 * u32 exp tables, name arrays, TM/HM/tutor/egg arrays, contest tables and
 * the level-up learnset pointer table over a freshly built leaf arena) from
 * the production pack through the NORMAL M0/M1 snapshot.
 *
 * REFUSE-CLASS: the D1 const definitions are NATIVE_LINUX-guarded out of
 * the link (src/data/pokemon/tmhm_learnsets.h, tutor_learnsets.h,
 * egg_moves.h, level_up_learnsets.h, level_up_learnset_pointers.h and
 * src/data/contest_moves.h), so there is no compiled fallback: a session
 * whose gameplay data cannot publish is refused and the loader rolls the
 * whole registration back.
 *
 * Transactional phases: 1 validates every entry (composition pins, M0/M1
 * resolution, family type/schema, ROM_BASE ownership, size + byte
 * equality vs the pack, family count pins, slice-disjointness, fixed-width
 * name rows, egg-stream byte total) before ANY publication; 2 builds the
 * levelup leaf arena and the assembled egg stream (all infallible once
 * phase 1 passed); 3 publishes atomically (pure stores). On any
 * phase-1/2 failure nothing is written and the diagnostics name the first
 * failing resource.
 */

#include "emerald/resources/emerald_gameplay_compat.h"
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

#define GAMEPLAY_DATA_PREFIX "emerald:data/"
#define GAMEPLAY_FONT_PREFIX "emerald:font/"
#define GAMEPLAY_FONT_PREFIX_LEN (sizeof(GAMEPLAY_FONT_PREFIX) - 1u)

/* D1 structured-data family schemas. */
#define SCHEMA_SPECIES_BASE     1u
#define SCHEMA_SPECIES_NAME     2u
#define SCHEMA_LEVELUP          4u
#define SCHEMA_TMHM             5u
#define SCHEMA_TUTOR            6u
#define SCHEMA_EGG              7u
#define SCHEMA_MOVE_BATTLE      8u
#define SCHEMA_MOVE_NAME        9u
#define SCHEMA_MOVE_CONTEST     10u
#define SCHEMA_GROWTH           12u
#define SCHEMA_TUTOR_MOVES      13u
#define SCHEMA_CONTEST_EFFECTS  14u
#define SCHEMA_COMBO            15u
#define SCHEMA_ITEM             11u

#define GAMEPLAY_SPECIES_WIRE_SIZE 28u
#define GAMEPLAY_MOVE_WIRE_SIZE   12u
#define GAMEPLAY_ITEM_WIRE_SIZE   44u
#define GAMEPLAY_EGG_TOTAL_BYTES  2278u  /* 1139 u16; the 165 blocks sum to
                                            exactly this (the last block
                                            carries the 0xFFFF terminator) */
#define GAMEPLAY_GROWTH_BYTES     404u
#define GAMEPLAY_MAX_REG_RANGES   (EMERALD_GAMEPLAY_FONT_COUNT + 2u)
#define GAMEPLAY_GBA_ROM_BASE     ((uint32_t)0x08000000u)

/* D2 item wire-row field layout (44 B, ROM gItems row):
 *   name[14] @0 | itemId u16 @14 | price u16 @16 | holdEffect u8 @18 |
 *   holdEffectParam u8 @19 | description u32(GBA) @20 | importance u8 @24 |
 *   registrability u8 @25 | pocket u8 @26 | type u8 @27 |
 *   fieldUseFunc u32(GBA) @28 | battleUsage u8 @32 |
 *   battleUseFunc u32(GBA) @36 | secondaryId u8 @40. */
#define ITEM_W_OFF_NAME        0u
#define ITEM_W_OFF_ITEMID      14u
#define ITEM_W_OFF_PRICE       16u
#define ITEM_W_OFF_HOLDEFFECT  18u
#define ITEM_W_OFF_HOLDPARAM   19u
#define ITEM_W_OFF_DESC        20u
#define ITEM_W_OFF_IMPORTANCE  24u
#define ITEM_W_OFF_REGISTRABLE 25u
#define ITEM_W_OFF_POCKET      26u
#define ITEM_W_OFF_TYPE        27u
#define ITEM_W_OFF_FIELDUSE    28u
#define ITEM_W_OFF_BATTLEUSAGE 32u
#define ITEM_W_OFF_BATTLEUSE   36u
#define ITEM_W_OFF_SECONDARYID 40u

/* One leaf record in the levelup arena: canonical resource name + payload
 * size (offsets are the running sum, by construction). */
struct GameplayLeafRecord
{
    const char *name;
    uint32_t size;
};

static struct GameplayLeafRecord *sLeaves; /* levelup leaf record table */
static size_t sLeafCount;
static uint8_t *sArenaBytes;   /* levelup leaf arena */
static size_t sArenaByteTotal;
static size_t sPublishedCount;
static struct { uintptr_t base; size_t length; } sRegisteredRanges[GAMEPLAY_MAX_REG_RANGES];
static size_t sRegisteredRangeCount;

/* ---- R13-D2 item publication state ---- */
/* One native fill-target row per item (built in phase 2, published to the
 * HOST_DATA gItems in phase 3 with a single memcpy). */
static struct Item sItemRows[EMERALD_GAMEPLAY_ITEM_COUNT];

/* R13-C item-description label -> ROM offset mapping, built in phase 2 by
 * scanning the pack's `emerald:text/item/s<item>desc` labels. It lets the
 * seam bind an item row's description GBA address (wire @20) to the exact
 * R13-C item text label the arena already publishes (ROM_BASE_ONLY after
 * the R13-D2 ownership flip), then re-point the native description to that
 * label's
 * arena bytes. Sorted by romOffset for binary search. */
struct ItemDescLabel
{
    uint64_t romOffset;
    const char *name;   /* canonical label id, e.g. emerald:text/item/spotiondesc */
};
static struct ItemDescLabel *sItemDescLabels;
static size_t sItemDescLabelCount;

static void ClearDiagnostics(struct EmeraldGameplayCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static void NoteFailure(struct EmeraldGameplayCompatDiagnostics *diagnostics,
                        const char *stage, const char *canonicalName,
                        enum Gen3ResourceType expectedType,
                        enum Gen3ResourceType actualType,
                        uint32_t expectedSchema, uint32_t actualSchema,
                        uint32_t expectedSize, uint32_t actualSize,
                        const struct Gen3ResourceView *view)
{
    if (diagnostics == NULL)
        return;
    ClearDiagnostics(diagnostics);
    if (stage != NULL)
        snprintf(diagnostics->stage, sizeof(diagnostics->stage), "%s", stage);
    if (canonicalName != NULL)
        snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                 "%s", canonicalName);
    snprintf(diagnostics->expectedType, sizeof(diagnostics->expectedType), "%s",
             Gen3ResourceType_Name(expectedType));
    if (actualType != GEN3_RESOURCE_TYPE_INVALID)
        snprintf(diagnostics->actualType, sizeof(diagnostics->actualType), "%s",
                 Gen3ResourceType_Name(actualType));
    diagnostics->expectedSchema = expectedSchema;
    diagnostics->actualSchema = actualSchema;
    diagnostics->expectedSize = expectedSize;
    diagnostics->actualSize = actualSize;
    if (view != NULL && view->winningProviderId != NULL)
    {
        snprintf(diagnostics->winningProviderId,
                 sizeof(diagnostics->winningProviderId), "%s",
                 view->winningProviderId);
        if (view->winningProviderVersion != NULL)
            snprintf(diagnostics->winningProviderVersion,
                     sizeof(diagnostics->winningProviderVersion), "%s",
                     view->winningProviderVersion);
        diagnostics->winningProviderPrecedence = view->winningProviderPrecedence;
    }
}

/* --- generated inventory lookups (the table is sorted by name) --- */

static const struct GameplayNativeResource *FindNativeResource(const char *id)
{
    size_t lo = 0u;
    size_t hi = GAMEPLAY_NATIVE_RESOURCE_COUNT;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = strcmp(id, kGameplayNativeResources[mid].name);
        if (cmp == 0)
            return &kGameplayNativeResources[mid];
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return NULL;
}

static size_t FindNativeIndex(const char *id)
{
    const struct GameplayNativeResource *row = FindNativeResource(id);
    return row != NULL ? (size_t)(row - kGameplayNativeResources) : (size_t)-1;
}

/* R13-D2: look up an item row's GBA use-function address in the 27-value
 * callback census (kGameplayItemCallbacks, sorted by gbaAddr). Returns the
 * semantic action, or GAMEPLAY_ITEM_USE_ACTION_COUNT when the address is not
 * in the census (the seam REFUSEs on any non-zero unmapped address). */
static enum GameplayItemUseAction ResolveItemCallback(uint32_t gbaAddr)
{
    size_t lo = 0u;
    size_t hi = GAMEPLAY_ITEM_CALLBACK_COUNT;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2u;
        uint32_t a = kGameplayItemCallbacks[mid].gbaAddr;
        if (a == gbaAddr)
            return (enum GameplayItemUseAction)kGameplayItemCallbacks[mid].action;
        if (a < gbaAddr)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return GAMEPLAY_ITEM_USE_ACTION_COUNT;
}

/* R13-D2: find an item-description R13-C label whose ROM offset equals `off`.
 * sItemDescLabels is sorted ascending by romOffset. */
static const struct ItemDescLabel *FindItemDescLabel(uint64_t off)
{
    size_t lo = 0u;
    size_t hi = sItemDescLabelCount;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2u;
        if (sItemDescLabels[mid].romOffset == off)
            return &sItemDescLabels[mid];
        if (sItemDescLabels[mid].romOffset < off)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return NULL;
}

/* D1 family type for a resource id (font prefix -> FONT, else structured). */
static enum Gen3ResourceType FamilyTypeOf(const char *name)
{
    if (strncmp(name, GAMEPLAY_FONT_PREFIX, GAMEPLAY_FONT_PREFIX_LEN) == 0)
        return GEN3_RESOURCE_TYPE_FONT;
    return GEN3_RESOURCE_TYPE_STRUCTURED_DATA;
}

/* Resolve through the NORMAL M0/M1 session snapshot (fail-closed). */
static bool ResolveSessionView(const struct Gen3ResourceSnapshot *snapshot,
                               const char *id, enum Gen3ResourceType type,
                               uint32_t schema, struct Gen3ResourceView *view)
{
    Gen3ResourceHandle handle;
    if (Gen3ResourceSnapshot_FindHandle(snapshot, id, &handle) != GEN3_RESOURCE_OK)
        return false;
    if (Gen3ResourceSnapshot_Resolve(snapshot, handle, type, schema, view)
            != GEN3_RESOURCE_OK || view->payload == NULL)
        return false;
    return true;
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

/* Fixed-width charmap names: one 0xFF terminator marking the end of the
 * name within the fixed width, then zero padding to the row end (e.g.
 * "ABSORB\xff\x00\x00\x00\x00\x00\x00" for a 13-byte move-name row). */
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
    return false; /* no terminator within the fixed width */
}

/* growth-rate resource name suffix -> native table row (GROWTH_*). */
static int GrowthRowIndex(const char *name)
{
    const char *p = strrchr(name, '/');
    if (p == NULL)
        return -1;
    p++;
    if (strcmp(p, "medium-fast") == 0) return 0;
    if (strcmp(p, "erratic") == 0)     return 1;
    if (strcmp(p, "fluctuating") == 0) return 2;
    if (strcmp(p, "medium-slow") == 0) return 3;
    if (strcmp(p, "fast") == 0)        return 4;
    if (strcmp(p, "slow") == 0)        return 5;
    if (strcmp(p, "unused-6") == 0)    return 6;
    if (strcmp(p, "unused-7") == 0)    return 7;
    return -1;
}

/* Map a font resource name to its native glyph array + byte size. */
static u16 *FontTarget(const char *name, size_t *outBytes)
{
    const char *s = name + GAMEPLAY_FONT_PREFIX_LEN;
    if (strcmp(s, "small-narrow-latin") == 0)      { if (outBytes) *outBytes = 32768u; return gFontSmallNarrowLatinGlyphs; }
    if (strcmp(s, "small-latin") == 0)             { if (outBytes) *outBytes = 32768u; return gFontSmallLatinGlyphs; }
    if (strcmp(s, "narrow-latin") == 0)            { if (outBytes) *outBytes = 32768u; return gFontNarrowLatinGlyphs; }
    if (strcmp(s, "short-latin") == 0)             { if (outBytes) *outBytes = 32768u; return gFontShortLatinGlyphs; }
    if (strcmp(s, "normal-latin") == 0)            { if (outBytes) *outBytes = 32768u; return gFontNormalLatinGlyphs; }
    if (strcmp(s, "small-japanese") == 0)          { if (outBytes) *outBytes = 16384u; return gFontSmallJapaneseGlyphs; }
    if (strcmp(s, "normal-japanese") == 0)         { if (outBytes) *outBytes = 16384u; return gFontNormalJapaneseGlyphs; }
    if (strcmp(s, "frlg-male-japanese") == 0)      { if (outBytes) *outBytes = 32768u; return gFontFRLGMaleJapaneseGlyphs; }
    if (strcmp(s, "frlg-female-japanese") == 0)    { if (outBytes) *outBytes = 32768u; return gFontFRLGFemaleJapaneseGlyphs; }
    if (strcmp(s, "short-japanese") == 0)          { if (outBytes) *outBytes = 32768u; return gFontShortJapaneseGlyphs; }
    return NULL;
}

const char *EmeraldGameplayCompatStatus_Describe(
    enum EmeraldGameplayCompatStatus status)
{
    switch (status)
    {
    case EMERALD_GAMEPLAY_OK: return "ok";
    case EMERALD_GAMEPLAY_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY: return "out of memory";
    case EMERALD_GAMEPLAY_ERR_RESOLVE_FAILED: return "resolve failed";
    case EMERALD_GAMEPLAY_ERR_UNEXPECTED_OWNERSHIP: return "unexpected ownership";
    case EMERALD_GAMEPLAY_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_GAMEPLAY_ERR_UNEXPECTED_COUNT: return "unexpected count";
    case EMERALD_GAMEPLAY_ERR_TABLE_MISMATCH: return "table mismatch";
    case EMERALD_GAMEPLAY_ERR_OVERLAPPING_SLICE: return "overlapping slice";
    case EMERALD_GAMEPLAY_ERR_UNEXPECTED_TYPE: return "unexpected type";
    case EMERALD_GAMEPLAY_ERR_UNEXPECTED_SCHEMA: return "unexpected schema";
    case EMERALD_GAMEPLAY_ERR_RANGE_REGISTRATION: return "range registration";
    case EMERALD_GAMEPLAY_ERR_ITEM_DESCRIPTION: return "item description unbound";
    case EMERALD_GAMEPLAY_ERR_ITEM_CALLBACK: return "item callback unresolvable";
    case EMERALD_GAMEPLAY_ERR_ITEM_OVERRIDE: return "item override guard failed";
    case EMERALD_GAMEPLAY_ERR_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}

enum EmeraldGameplayCompatStatus
EmeraldGameplayCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldGameplayCompatDiagnostics *diagnostics)
{
    enum EmeraldGameplayCompatStatus result = EMERALD_GAMEPLAY_OK;
    const uint8_t **src = NULL;  /* [GAMEPLAY_NATIVE_RESOURCE_COUNT] view payloads */
    uint8_t *seen = NULL;        /* [GAMEPLAY_NATIVE_RESOURCE_COUNT] */
    uint64_t *spans = NULL;
    uint8_t *arena = NULL;
    struct GameplayLeafRecord *leaves = NULL;
    size_t packCount;
    size_t spanCount = 0u;
    size_t arenaByteTotal = 0u;
    uint32_t familyCount[16];
    uint32_t fontCount = 0u;
    size_t i;
    uint32_t j;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        return EMERALD_GAMEPLAY_ERR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < 16u; i++)
        familyCount[i] = 0u;

    src = (const uint8_t **)calloc(GAMEPLAY_NATIVE_RESOURCE_COUNT, sizeof(*src));
    if (src == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        return EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY;
    }
    seen = (uint8_t *)calloc(GAMEPLAY_NATIVE_RESOURCE_COUNT, sizeof(*seen));
    if (seen == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        result = EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY;
        goto done;
    }

    /* ---- Phase 1: validate the whole D1 family against the pack + the
     * snapshot before any publication. A D1 entry is one whose canonical
     * name is in the generated inventory. ---- */
    packCount = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0u; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        const struct GameplayNativeResource *row =
            FindNativeResource(entry->canonicalName);
        size_t rowIndex;
        enum Gen3ResourceType familyType;
        struct Gen3ResourceView view;

        if (row == NULL)
            continue; /* not a D1-family entry */
        rowIndex = (size_t)(row - kGameplayNativeResources);
        if (seen[rowIndex] != 0u)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA, entry->type,
                        row->schema, entry->schema, row->size,
                        (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_GAMEPLAY_ERR_TABLE_MISMATCH;
            goto done;
        }
        seen[rowIndex] = 1u;

        familyType = FamilyTypeOf(entry->canonicalName);
        if (entry->type != familyType)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        familyType, entry->type, row->schema, entry->schema,
                        row->size, (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_GAMEPLAY_ERR_UNEXPECTED_TYPE;
            goto done;
        }
        if (entry->schema != row->schema)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        familyType, entry->type, row->schema, entry->schema,
                        row->size, (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_GAMEPLAY_ERR_UNEXPECTED_SCHEMA;
            goto done;
        }
        if (entry->payloadSize != row->size || entry->payload == NULL)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        familyType, entry->type, row->schema, entry->schema,
                        row->size, (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_GAMEPLAY_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (!ResolveSessionView(snapshot, entry->canonicalName, familyType,
                                row->schema, &view))
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        familyType, entry->type, row->schema, entry->schema,
                        (uint32_t)entry->payloadSize, 0u, NULL);
            result = EMERALD_GAMEPLAY_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        familyType, entry->type, entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_GAMEPLAY_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        if (view.payloadSize != entry->payloadSize
         || memcmp(view.payload, entry->payload, view.payloadSize) != 0)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        familyType, entry->type, entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_GAMEPLAY_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (row->schema == SCHEMA_SPECIES_NAME || row->schema == SCHEMA_MOVE_NAME)
        {
            if (!ValidNameRow(view.payload, view.payloadSize))
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            familyType, entry->type, row->schema, row->schema,
                            (uint32_t)view.payloadSize, 0u, NULL);
                result = EMERALD_GAMEPLAY_ERR_TABLE_MISMATCH;
                goto done;
            }
        }
        src[rowIndex] = view.payload;

        if (familyType == GEN3_RESOURCE_TYPE_FONT)
        {
            size_t fontBytes;
            if (FontTarget(entry->canonicalName, &fontBytes) == NULL
             || fontBytes != row->size)
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            familyType, entry->type, row->schema, row->schema,
                            fontBytes, (uint32_t)row->size, NULL);
                result = EMERALD_GAMEPLAY_ERR_TABLE_MISMATCH;
                goto done;
            }
            fontCount++;
        }
        else if (row->schema < 16u)
        {
            /* R13-E1: schema >= 16 = the trainer-owned families (16 metadata /
             * 17 party / 18 class-name). They are presence-validated above
             * (type/schema/size/ownership/bytes) and marked seen alongside
             * every other generated row, but their fill targets are owned and
             * published by EmeraldTrainerCompat (which runs after this seam in
             * the runtime loader chain), not here. */
            familyCount[row->schema]++;
        }
    }

    /* ---- Phase 1b: every generated row was seen; family pins. ---- */
    for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
    {
        if (seen[i] == 0u)
        {
            NoteFailure(diagnostics, "build", kGameplayNativeResources[i].name,
                        FamilyTypeOf(kGameplayNativeResources[i].name),
                        GEN3_RESOURCE_TYPE_INVALID,
                        kGameplayNativeResources[i].schema, 0u,
                        kGameplayNativeResources[i].size, 0u, NULL);
            result = EMERALD_GAMEPLAY_ERR_TABLE_MISMATCH;
            goto done;
        }
    }
    if (familyCount[SCHEMA_SPECIES_BASE] != EMERALD_GAMEPLAY_SPECIES_COUNT
     || familyCount[SCHEMA_SPECIES_NAME] != EMERALD_GAMEPLAY_SPECIES_COUNT
     || familyCount[SCHEMA_LEVELUP] != EMERALD_GAMEPLAY_LEVELUP_COUNT
     || familyCount[SCHEMA_TMHM] != EMERALD_GAMEPLAY_TMHM_COUNT
     || familyCount[SCHEMA_TUTOR] != EMERALD_GAMEPLAY_TUTOR_COUNT
     || familyCount[SCHEMA_EGG] != EMERALD_GAMEPLAY_EGG_COUNT
     || familyCount[SCHEMA_MOVE_BATTLE] != EMERALD_GAMEPLAY_MOVE_COUNT
     || familyCount[SCHEMA_MOVE_NAME] != EMERALD_GAMEPLAY_MOVE_COUNT
     || familyCount[SCHEMA_MOVE_CONTEST] != EMERALD_GAMEPLAY_MOVE_COUNT
     || familyCount[SCHEMA_GROWTH] != EMERALD_GAMEPLAY_GROWTH_COUNT
     || familyCount[SCHEMA_ITEM] != EMERALD_GAMEPLAY_ITEM_COUNT
     || familyCount[SCHEMA_TUTOR_MOVES] != 1u
     || familyCount[SCHEMA_CONTEST_EFFECTS] != 1u
     || familyCount[SCHEMA_COMBO] != 1u
     || fontCount != EMERALD_GAMEPLAY_FONT_COUNT)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        result = EMERALD_GAMEPLAY_ERR_UNEXPECTED_COUNT;
        goto done;
    }

    /* ---- Phase 1c: the assembled egg stream must be exactly 2,278 B
     * (165 blocks, no more, no less) before any publication. ---- */
    {
        size_t eggTotal = 0u;
        for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
            if (kGameplayNativeResources[i].schema == SCHEMA_EGG)
                eggTotal += kGameplayNativeResources[i].size;
        if (eggTotal != GAMEPLAY_EGG_TOTAL_BYTES)
        {
            NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, SCHEMA_EGG, SCHEMA_EGG,
                        GAMEPLAY_EGG_TOTAL_BYTES, (uint32_t)eggTotal, NULL);
            result = EMERALD_GAMEPLAY_ERR_UNEXPECTED_COUNT;
            goto done;
        }
    }

    /* ---- Phase 1d: prove the claimed ROM slices pairwise disjoint. ---- */
    spans = (uint64_t *)malloc(GAMEPLAY_NATIVE_RESOURCE_COUNT * sizeof(*spans));
    if (spans == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        result = EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY;
        goto done;
    }
    for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_FindByCanonicalName(pack,
                                                 kGameplayNativeResources[i].name);
        if (entry != NULL)
            spans[spanCount++] = (entry->sourceRomOffset << 32)
                               | (uint32_t)entry->payloadSize;
    }
    {
        size_t k;
        for (k = 1u; k < spanCount; k++)
        {
            uint64_t key = spans[k];
            size_t m = k;
            while (m > 0u && spans[m - 1u] > key)
            {
                spans[m] = spans[m - 1u];
                m--;
            }
            spans[m] = key;
        }
        for (k = 0u; k + 1u < spanCount; k++)
        {
            uint64_t aEnd = (spans[k] >> 32) + (uint32_t)spans[k];
            if (aEnd > (spans[k + 1u] >> 32))
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer), "slice @0x%llx +%u",
                         (unsigned long long)(spans[k] >> 32),
                         (uint32_t)spans[k]);
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
                result = EMERALD_GAMEPLAY_ERR_OVERLAPPING_SLICE;
                goto done;
            }
        }
    }

    /* ---- Phase 2 (D2): resolve the item rows against the pack + the R13-C
     * item text labels, and build the 377 native rows. Everything here is
     * validated before any publication; the phase-3 store into gItems is a
     * single infallible memcpy. ---- */
    {
        /* Build the R13-C item-description label -> ROM offset map by
         * scanning the pack (each `emerald:text/item/s<item>desc` label's
         * sourceRomOffset is its ROM offset: GBA addr - 0x08000000). */
        size_t descCap = 0u;
        sItemDescLabelCount = 0u;
        for (i = 0u; i < packCount && sItemDescLabelCount < packCount; i++)
        {
            const struct Gen3ResourcePackEntry *entry2 =
                Gen3ResourcePack_GetEntry(pack, i);
            if (entry2 == NULL || entry2->canonicalName == NULL)
                continue;
            if (strncmp(entry2->canonicalName, "emerald:text/item/", 18u) != 0)
                continue;
            if (entry2->type != GEN3_RESOURCE_TYPE_TEXT)
                continue;
            if (sItemDescLabelCount >= descCap)
            {
                size_t newCap = descCap ? descCap * 2u : 64u;
                struct ItemDescLabel *grown = (struct ItemDescLabel *)
                    realloc(sItemDescLabels, newCap * sizeof(*grown));
                if (grown == NULL)
                {
                    NoteFailure(diagnostics, "build", entry2->canonicalName,
                                GEN3_RESOURCE_TYPE_TEXT, entry2->type, 1u,
                                entry2->schema, 0u, 0u, NULL);
                    result = EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY;
                    goto done;
                }
                sItemDescLabels = grown;
                descCap = newCap;
            }
            sItemDescLabels[sItemDescLabelCount].romOffset =
                entry2->sourceRomOffset;
            sItemDescLabels[sItemDescLabelCount].name = entry2->canonicalName;
            sItemDescLabelCount++;
        }
        /* Sort the map ascending by romOffset for binary search. */
        {
            size_t a, b;
            for (a = 1u; a < sItemDescLabelCount; a++)
            {
                struct ItemDescLabel key = sItemDescLabels[a];
                b = a;
                while (b > 0u
                    && sItemDescLabels[b - 1u].romOffset > key.romOffset)
                {
                    sItemDescLabels[b] = sItemDescLabels[b - 1u];
                    b--;
                }
                sItemDescLabels[b] = key;
            }
        }

        /* Build each native row. */
        memset(sItemRows, 0, sizeof(sItemRows));
        for (i = 0u; i < EMERALD_GAMEPLAY_ITEM_COUNT; i++)
        {
            const char *key = kGameplayItemKeys[i];
            size_t idx;
            const uint8_t *p;
            struct Item *row;
            uint32_t descAddr;
            const struct ItemDescLabel *label;
            const uint8_t *descBytes;
            size_t descSize;
            enum GameplayItemUseAction fieldAction;
            enum GameplayItemUseAction battleAction;
            uint16_t overrideIdx;
            bool isOverride = false;

            idx = FindNativeIndex(key);
            if (idx == (size_t)-1 || src[idx] == NULL)
            {
                NoteFailure(diagnostics, "build", key,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID, SCHEMA_ITEM, SCHEMA_ITEM,
                            0u, 0u, NULL);
                result = EMERALD_GAMEPLAY_ERR_RESOLVE_FAILED;
                goto done;
            }
            p = src[idx];
            row = &sItemRows[i];

            /* Canonical scalar + name copy (identical to the compiled GBA
             * row's data fields). */
            memcpy(row->name, p + ITEM_W_OFF_NAME, ITEM_NAME_LENGTH);
            row->itemId = ReadLe16(p + ITEM_W_OFF_ITEMID);
            row->price = ReadLe16(p + ITEM_W_OFF_PRICE);
            row->holdEffect = p[ITEM_W_OFF_HOLDEFFECT];
            row->holdEffectParam = p[ITEM_W_OFF_HOLDPARAM];
            row->importance = p[ITEM_W_OFF_IMPORTANCE];
            row->registrability = p[ITEM_W_OFF_REGISTRABLE];
            row->pocket = p[ITEM_W_OFF_POCKET];
            row->type = p[ITEM_W_OFF_TYPE];
            row->battleUsage = p[ITEM_W_OFF_BATTLEUSAGE];
            row->secondaryId = p[ITEM_W_OFF_SECONDARYID];

            /* Description: the wire GBA address must be the STABLE match of
             * an R13-C item text label (ROM offset: addr - 0x08000000). Bind
             * the native description pointer to that label's arena bytes. A
             * description every item must resolve (0 NULL in the census;
             * the 68 dummy rows share emerald:text/item/sdummydesc). */
            descAddr = ReadLe32(p + ITEM_W_OFF_DESC);
            if (descAddr < GAMEPLAY_GBA_ROM_BASE)
            {
                NoteFailure(diagnostics, "build", key,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID, SCHEMA_ITEM, SCHEMA_ITEM,
                            descAddr, 0u, NULL);
                result = EMERALD_GAMEPLAY_ERR_ITEM_DESCRIPTION;
                goto done;
            }
            label = FindItemDescLabel((uint64_t)descAddr - GAMEPLAY_GBA_ROM_BASE);
            if (label == NULL
             || !EmeraldTextCompat_GetResourceBytes(label->name, &descBytes,
                                                    &descSize)
             || descBytes == NULL)
            {
                NoteFailure(diagnostics, "build", key,
                            GEN3_RESOURCE_TYPE_TEXT, GEN3_RESOURCE_TYPE_INVALID,
                            1u, 1u, 0u, 0u, NULL);
                result = EMERALD_GAMEPLAY_ERR_ITEM_DESCRIPTION;
                goto done;
            }
            row->description = descBytes;

            /* Callbacks: a ZERO GBA address means "no function" (a
             * non-battle item's battleUseFunc) -> NULL. Any NON-ZERO
             * address must be in the 27-value census; resolve to the
             * native ItemUseFunc. An unknown non-zero address is REFUSED. */
            if (ReadLe32(p + ITEM_W_OFF_FIELDUSE) != 0u)
            {
                fieldAction = ResolveItemCallback(
                    ReadLe32(p + ITEM_W_OFF_FIELDUSE));
                if (fieldAction == GAMEPLAY_ITEM_USE_ACTION_COUNT)
                {
                    NoteFailure(diagnostics, "build", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID, SCHEMA_ITEM, SCHEMA_ITEM,
                                0u, 0u, NULL);
                    result = EMERALD_GAMEPLAY_ERR_ITEM_CALLBACK;
                    goto done;
                }
                row->fieldUseFunc =
                    GameplayItemUseFunctionForAction(fieldAction);
            }
            else
            {
                fieldAction = GAMEPLAY_ITEM_USE_ACTION_COUNT;
                row->fieldUseFunc = NULL;
            }
            if (ReadLe32(p + ITEM_W_OFF_BATTLEUSE) != 0u)
            {
                battleAction = ResolveItemCallback(
                    ReadLe32(p + ITEM_W_OFF_BATTLEUSE));
                if (battleAction == GAMEPLAY_ITEM_USE_ACTION_COUNT)
                {
                    NoteFailure(diagnostics, "build", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID, SCHEMA_ITEM, SCHEMA_ITEM,
                                0u, 0u, NULL);
                    result = EMERALD_GAMEPLAY_ERR_ITEM_CALLBACK;
                    goto done;
                }
                row->battleUseFunc =
                    GameplayItemUseFunctionForAction(battleAction);
            }
            else
            {
                battleAction = GAMEPLAY_ITEM_USE_ACTION_COUNT;
                row->battleUseFunc = NULL;
            }

            /* Fork override: the six trade-evolution held items are directly
             * usable to trigger evolution. Before overriding each, assert the
             * canonical vanilla row is the expected 0x04 + CannotUse baseline
             * so the override stays grounded (never blind). */
            for (overrideIdx = 0u;
                 overrideIdx < EMERALD_GAMEPLAY_OVERRIDE_ITEM_COUNT;
                 overrideIdx++)
            {
                if (kGameplayItemUseOverrides[overrideIdx] == (uint16_t)i)
                {
                    isOverride = true;
                    break;
                }
            }
            if (isOverride)
            {
                if (row->type != ITEM_USE_BAG_MENU
                 || fieldAction != GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_CANNOTUSE)
                {
                    NoteFailure(diagnostics, "override", key,
                                GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                                GEN3_RESOURCE_TYPE_INVALID,
                                SCHEMA_ITEM, SCHEMA_ITEM,
                                row->type, (uint32_t)fieldAction, NULL);
                    result = EMERALD_GAMEPLAY_ERR_ITEM_OVERRIDE;
                    goto done;
                }
                row->type = ITEM_USE_PARTY_MENU;
                row->fieldUseFunc = GameplayItemUseFunctionForAction(
                    GAMEPLAY_ITEM_USE_ACTION_OUTOFBATTLE_EVOLUTIONSTONE);
            }
        }

        /* itemId/type/secondaryId sanity: at least the enum coverage is
         * index-ordered; the equal-payload parity for the other 371 rows is
         * proven field-by-field against the pack above (byte equality), so
         * nothing further is needed here. */
    }

    /* ---- Phase 2: build the levelup leaf arena + assembled egg stream
     * (all infallible now that phase 1 passed). ---- */
    for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
        if (kGameplayNativeResources[i].schema == SCHEMA_LEVELUP)
            arenaByteTotal += kGameplayNativeResources[i].size;
    arena = (uint8_t *)malloc(arenaByteTotal);
    if (arena == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        result = EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY;
        goto done;
    }
    leaves = (struct GameplayLeafRecord *)
        calloc(EMERALD_GAMEPLAY_LEVELUP_COUNT, sizeof(*leaves));
    if (leaves == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        result = EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY;
        goto done;
    }
    {
        size_t cursor = 0u;
        size_t leafCount = 0u;
        for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
        {
            const struct GameplayNativeResource *row = &kGameplayNativeResources[i];
            if (row->schema != SCHEMA_LEVELUP)
                continue;
            if (src[i] == NULL)
            {
                NoteFailure(diagnostics, "build", row->name,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID, row->schema, row->schema,
                            row->size, 0u, NULL);
                result = EMERALD_GAMEPLAY_ERR_RESOLVE_FAILED;
                goto done;
            }
            memcpy(arena + cursor, src[i], row->size);
            leaves[leafCount].name = row->name;
            leaves[leafCount].size = row->size;
            leafCount++;
            cursor += row->size;
        }
        if (leafCount != EMERALD_GAMEPLAY_LEVELUP_COUNT)
        {
            result = EMERALD_GAMEPLAY_ERR_UNEXPECTED_COUNT;
            goto done;
        }
        sArenaBytes = arena;
        sArenaByteTotal = arenaByteTotal;
        sLeaves = leaves;
        sLeafCount = leafCount;
        /* arena is now owned by sArenaBytes; the pointer table is published
         * as part of phase 3 (a pure store once phase 1 validated every
         * levelup key). */
        arena = NULL;
        leaves = NULL;
    }

    /* ---- Phase 3: publish atomically (infallible stores). ---- */

    /* Level-up pointer table: every species index resolves to the leaf
     * whose key is kGameplayLevelupSpeciesKeys[i]; the leaves live in
     * sArenaBytes in kGameplayNativeResources (schema-4) order. */
    {
        size_t *leafBase = (size_t *)calloc(sLeafCount, sizeof(*leafBase));
        size_t off = 0u;
        size_t k;
        if (leafBase == NULL)
        {
            NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
            result = EMERALD_GAMEPLAY_ERR_OUT_OF_MEMORY;
            goto done;
        }
        for (k = 0u; k < sLeafCount; k++)
        {
            leafBase[k] = off;
            off += sLeaves[k].size;
        }
        for (i = 0u; i < EMERALD_GAMEPLAY_SPECIES_COUNT; i++)
        {
            const char *key = kGameplayLevelupSpeciesKeys[i];
            size_t lo = 0u, hi = sLeafCount;
            size_t found = (size_t)-1;
            while (lo < hi)
            {
                size_t mid = lo + (hi - lo) / 2u;
                int cmp = strcmp(key, sLeaves[mid].name);
                if (cmp == 0) { found = mid; break; }
                if (cmp < 0) hi = mid; else lo = mid + 1u;
            }
            if (found == (size_t)-1)
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer), "levelup key %s unresolved", key);
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                            GEN3_RESOURCE_TYPE_INVALID, SCHEMA_LEVELUP,
                            SCHEMA_LEVELUP, 0u, 0u, NULL);
                free(leafBase);
                result = EMERALD_GAMEPLAY_ERR_RESOLVE_FAILED;
                goto done;
            }
            gLevelUpLearnsets[i] = (u16 *)(void *)(sArenaBytes + leafBase[found]);
        }
        free(leafBase);
    }
    for (i = 0u; i < EMERALD_GAMEPLAY_SPECIES_COUNT; i++)
    {
        const char *base = kGameplaySpeciesKeys[i];
        char id[160];
        size_t idx;
        const uint8_t *p;

        idx = FindNativeIndex(base);
        p = src[idx];
        {
            struct SpeciesInfo *species = &gSpeciesInfo[i];
            uint16_t ev;
            memset(species, 0, sizeof(*species));
            species->baseHP = p[0];
            species->baseAttack = p[1];
            species->baseDefense = p[2];
            species->baseSpeed = p[3];
            species->baseSpAttack = p[4];
            species->baseSpDefense = p[5];
            species->types[0] = p[6];
            species->types[1] = p[7];
            species->catchRate = p[8];
            species->expYield = p[9];
            ev = ReadLe16(p + 10);
            species->evYield_HP = ev & 3;
            species->evYield_Attack = (ev >> 2) & 3;
            species->evYield_Defense = (ev >> 4) & 3;
            species->evYield_Speed = (ev >> 6) & 3;
            species->evYield_SpAttack = (ev >> 8) & 3;
            species->evYield_SpDefense = (ev >> 10) & 3;
            species->itemCommon = ReadLe16(p + 12);
            species->itemRare = ReadLe16(p + 14);
            species->genderRatio = p[16];
            species->eggCycles = p[17];
            species->friendship = p[18];
            species->growthRate = p[19];
            species->eggGroups[0] = p[20];
            species->eggGroups[1] = p[21];
            species->abilities[0] = p[22];
            species->abilities[1] = p[23];
            species->safariZoneFleeRate = p[24];
            species->bodyColor = p[25] & 0x7F;
            species->noFlip = p[25] >> 7;
        }
        snprintf(id, sizeof(id), "%s/name", base);
        idx = FindNativeIndex(id);
        memcpy(gSpeciesNames[i], src[idx], 11u);
        snprintf(id, sizeof(id), "%s/tmhm", base);
        idx = FindNativeIndex(id);
        memcpy(&gTMHMLearnsets[i].as_u32s[0], src[idx], 8u);
        snprintf(id, sizeof(id), "%s/tutor", base);
        idx = FindNativeIndex(id);
        memcpy(&sTutorLearnsets[i], src[idx], 4u);
    }

    for (i = 0u; i < EMERALD_GAMEPLAY_MOVE_COUNT; i++)
    {
        const char *base = kGameplayMoveKeys[i];
        char id[160];
        size_t idx;
        const uint8_t *p;

        idx = FindNativeIndex(base);
        p = src[idx];
        {
            struct BattleMove *move = &gBattleMoves[i];
            move->effect = p[0];
            move->power = p[1];
            move->type = p[2];
            move->accuracy = p[3];
            move->pp = p[4];
            move->secondaryEffectChance = p[5];
            move->target = p[6];
            move->priority = (s8)p[7];
            move->flags = p[8];
        }
        snprintf(id, sizeof(id), "%s/name", base);
        idx = FindNativeIndex(id);
        memcpy(gMoveNames[i], src[idx], 13u);
        snprintf(id, sizeof(id), "%s/contest", base);
        idx = FindNativeIndex(id);
        p = src[idx];
        {
            struct ContestMove *cm = &gContestMoves[i];
            uint8_t m;
            cm->effect = p[0];
            cm->contestCategory = p[1] & 0x7u;
            cm->comboStarterId = p[2];
            for (m = 0u; m < 4u; m++)
                cm->comboMoves[m] = p[3u + m];
        }
    }

    /* growth curves (rate row per GROWTH_* enum). */
    for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
    {
        const struct GameplayNativeResource *row = &kGameplayNativeResources[i];
        int rate;
        if (row->schema != SCHEMA_GROWTH)
            continue;
        rate = GrowthRowIndex(row->name);
        if (rate < 0 || rate >= 8)
        {
            result = EMERALD_GAMEPLAY_ERR_TABLE_MISMATCH;
            goto done;
        }
        memcpy(gExperienceTables[rate], src[i], GAMEPLAY_GROWTH_BYTES);
    }

    /* tutor-moves / contest-effects / combo single-table fills. */
    for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
    {
        const struct GameplayNativeResource *row = &kGameplayNativeResources[i];
        const uint8_t *p = src[i];
        if (row->schema == SCHEMA_TUTOR_MOVES)
            memcpy(gTutorMoves, p, 60u);
        else if (row->schema == SCHEMA_CONTEST_EFFECTS)
        {
            for (j = 0u; j < 48u; j++)
            {
                struct ContestEffect *ce = &gContestEffects[j];
                const uint8_t *w = p + j * 4u;
                ce->effectType = w[0];
                ce->appeal = w[1];
                ce->jam = w[2];
            }
        }
        else if (row->schema == SCHEMA_COMBO)
            memcpy(gComboStarterLookupTable, p, 63u);
    }

    /* egg stream (assembly is a pure copy of the 165 blocks, the last of
     * which carries the 0xFFFF terminator): species-index order. */
    {
        size_t eggCursor = 0u;
        for (i = 0u; i < EMERALD_GAMEPLAY_SPECIES_COUNT; i++)
        {
            char id[160];
            size_t idx;
            const char *baseKey = kGameplaySpeciesKeys[i];
            if (strcmp(baseKey, "emerald:data/species/none") == 0)
                continue;
            snprintf(id, sizeof(id), "%s/egg-moves", baseKey);
            idx = FindNativeIndex(id);
            if (idx == (size_t)-1)
                continue;
            memcpy((uint8_t *)gEggMoves + eggCursor, src[idx],
                   kGameplayNativeResources[idx].size);
            eggCursor += kGameplayNativeResources[idx].size;
        }
        if (eggCursor != GAMEPLAY_EGG_TOTAL_BYTES)
        {
            NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID, SCHEMA_EGG, SCHEMA_EGG,
                        GAMEPLAY_EGG_TOTAL_BYTES, (uint32_t)eggCursor, NULL);
            result = EMERALD_GAMEPLAY_ERR_UNEXPECTED_COUNT;
            goto done;
        }
    }

    /* fonts: LE16 word loads. */
    for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
    {
        const struct GameplayNativeResource *row = &kGameplayNativeResources[i];
        u16 *font;
        size_t fontBytes;
        size_t f;
        if (strncmp(row->name, GAMEPLAY_FONT_PREFIX, GAMEPLAY_FONT_PREFIX_LEN) != 0)
            continue;
        font = FontTarget(row->name, &fontBytes);
        for (f = 0u; f < fontBytes; f += 2u)
            font[f / 2u] = ReadLe16(src[i] + f);
    }

    /* D2 publication: one infallible memcpy publishes all 377 native rows
     * into the HOST_DATA gItems. */
    memcpy(gItems, sItemRows, sizeof(sItemRows));

    /* Public ranges (levelup arena + egg + font arrays). D2 adds NO item
     * ranges: gItems is fixed HOST_DATA .data and descriptions live in the
     * R13-C item text arena (already range-covered). */
    if (!RegisterArenaRanges())
    {
        NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
        result = EMERALD_GAMEPLAY_ERR_RANGE_REGISTRATION;
        goto done;
    }

    sPublishedCount = GAMEPLAY_NATIVE_RESOURCE_COUNT;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
    result = EMERALD_GAMEPLAY_OK;

done:
    if (arena != NULL)
        free(arena);
    if (leaves != NULL)
        free(leaves);
    free(spans);
    free(seen);
    free(src);
    return result;
}

/* ---- State-v5 arena-range registration (levelup arena + egg + fonts). ---- */

static bool RegisterArenaRanges(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    size_t i;

    sRegisteredRangeCount = 0u;
    if (index == NULL || sArenaBytes == NULL)
        return true;

    if (!EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)sArenaBytes, sArenaByteTotal,
            "emerald:data/arena/levelup", GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            SCHEMA_LEVELUP, EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        return false;
    sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)sArenaBytes;
    sRegisteredRanges[sRegisteredRangeCount].length = sArenaByteTotal;
    sRegisteredRangeCount++;

    if (!EmeraldResourceRangeIndex_RegisterSpan(
            index, (uintptr_t)gEggMoves, sizeof(gEggMoves),
            "emerald:data/arena/egg", GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            SCHEMA_EGG, EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
        return (UnregisterArenaRanges(), false);
    sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)gEggMoves;
    sRegisteredRanges[sRegisteredRangeCount].length = sizeof(gEggMoves);
    sRegisteredRangeCount++;

    for (i = 0u; i < GAMEPLAY_NATIVE_RESOURCE_COUNT; i++)
    {
        const struct GameplayNativeResource *row = &kGameplayNativeResources[i];
        u16 *font;
        size_t fontBytes;
        char key[96];
        if (strncmp(row->name, GAMEPLAY_FONT_PREFIX, GAMEPLAY_FONT_PREFIX_LEN) != 0)
            continue;
        font = FontTarget(row->name, &fontBytes);
        if (font == NULL)
            return (UnregisterArenaRanges(), false);
        snprintf(key, sizeof(key), "emerald:data/arena/font-%s",
                 row->name + GAMEPLAY_FONT_PREFIX_LEN);
        if (!EmeraldResourceRangeIndex_RegisterSpan(
                index, (uintptr_t)font, fontBytes, key,
                GEN3_RESOURCE_TYPE_FONT, 1u, EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
            return (UnregisterArenaRanges(), false);
        sRegisteredRanges[sRegisteredRangeCount].base = (uintptr_t)font;
        sRegisteredRanges[sRegisteredRangeCount].length = fontBytes;
        sRegisteredRangeCount++;
    }
    return true;
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

void EmeraldGameplayCompat_ClearMigratedEntries(void)
{
    size_t i;
    UnregisterArenaRanges();
    for (i = 0u; i < NUM_SPECIES; i++)
        gLevelUpLearnsets[i] = NULL;
    free(sArenaBytes);
    free(sLeaves);
    free(sItemDescLabels);
    sArenaBytes = NULL;
    sLeaves = NULL;
    sLeafCount = 0u;
    sArenaByteTotal = 0u;
    sItemDescLabels = NULL;
    sItemDescLabelCount = 0u;
    /* R13-D2: zero the item fill target so no native description pointer
     * dangles into the (sibling-seam, outer rollback) freed text arena and
     * no callback pointer survives a refused session. */
    memset(gItems, 0, sizeof(gItems));
    sPublishedCount = 0u;
}

void EmeraldGameplayCompat_Shutdown(void)
{
    EmeraldGameplayCompat_ClearMigratedEntries();
}

size_t EmeraldGameplayCompat_GetPublishedCount(void)
{
    return sPublishedCount;
}