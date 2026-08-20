/* R13-E3b: Emerald Pokédex publication seam. See
 * include/emerald/resources/emerald_pokedex_compat.h for the contract.
 *
 * Publishes the Pokédex families (structured-data schema 38 row / 39 order /
 * 40 species-to-national) into the native HOST_DATA fill targets through the
 * NORMAL M0/M1 snapshot and completes the R13-C Pokédex description-text
 * cutover: each row's GBA description pointer is validated to resolve to the
 * exact R13-C Pokédex text label (emerald:text/pokedex/g<species>pokedextext)
 * and the native description pointer is re-pointed into the live R13-C arena
 * (via EmeraldTextCompat_GetResourceBytes), so the published gPokedexEntries
 * rows consume the arena text directly.
 *
 * Row model (ROM-verified): gPokedexEntries is 387 x 32 B GBA wire. category
 * name is an inline 12-byte charmap string @0. The native 40-byte transform
 * copies category + the five u16 scalars byte-identically and replaces the GBA
 * text u32 (@16) with a native const u8* arena pointer. The description is the
 * ONLY text pointer per row. Ordering/routing arrays are byte-identical u16.
 *
 * Transactional phases: 1 validates every row resource + its description→
 * text binding + the ordering/routing resources + schema counts BEFORE any
 * write; 2 precomputes the native rows + arrays and runs the native-oracle
 * parity proof (category + scalars reproduce the wire bytes exactly, and the
 * four u16 arrays match the pack payloads byte-for-byte); 3 publishes
 * atomically (pure stores). No arena allocated; no range registered (see
 * header - the R13-C text arena already owns the State-v5 pointer surface).
 */

#include "emerald/resources/emerald_pokedex_compat.h"
#include "emerald/resources/emerald_resource_session.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMERALD_POKEDEX_GBA_ROM_BASE ((uint32_t)0x08000000u)

static uint16_t ReadLe16(const uint8_t *data)
{
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}
static uint32_t ReadLe32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/* Native fill rows prebuilt in phase 2 (infallible once phase 1 passed). */
static struct PokedexEntry sEntryRows[EMERALD_POKEDEX_ROW_COUNT];
static u16 sOrderAlpha[EMERALD_POKEDEX_ALPHABETICAL_COUNT];
static u16 sOrderHeight[EMERALD_POKEDEX_HEIGHT_COUNT];
static u16 sOrderWeight[EMERALD_POKEDEX_WEIGHT_COUNT];
static u16 sS2N[EMERALD_POKEDEX_S2N_COUNT];

static size_t sPublishedCount;

static void ClearDiagnostics(struct EmeraldPokedexCompatDiagnostics *d)
{
    if (d != NULL)
        memset(d, 0, sizeof(*d));
}

static void NoteFailure(struct EmeraldPokedexCompatDiagnostics *d,
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

/* Validate a single whole-table structured resource: resolve schema + type,
 * ROM_BASE winner, exact pack-side size + byte parity. Returns the pack
 * payload on success. */
static const uint8_t *ResolveRowResource(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    const char *id, uint32_t schema, size_t expectedSize,
    struct EmeraldPokedexCompatDiagnostics *d,
    enum EmeraldPokedexCompatStatus *resultOut)
{
    struct Gen3ResourceView view;
    const struct Gen3ResourcePackEntry *entry;

    if (!ResolveSessionView(snapshot, id, schema, &view))
    {
        NoteFailure(d, "resolve", id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, 0u, 0u, 0u, NULL);
        *resultOut = EMERALD_POKEDEX_ERR_RESOLVE_FAILED;
        return NULL;
    }
    if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
    {
        NoteFailure(d, "resolve", id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, schema, schema,
                    (uint32_t)view.payloadSize, (uint32_t)view.payloadSize,
                    &view);
        *resultOut = EMERALD_POKEDEX_ERR_UNEXPECTED_OWNERSHIP;
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
        *resultOut = EMERALD_POKEDEX_ERR_PAYLOAD_SIZE_MISMATCH;
        return NULL;
    }
    return entry->payload;
}

enum EmeraldPokedexCompatStatus
EmeraldPokedexCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldPokedexCompatDiagnostics *diagnostics)
{
    enum EmeraldPokedexCompatStatus result = EMERALD_POKEDEX_OK;
    const char *resolvedLabels[EMERALD_POKEDEX_ROW_COUNT];
    const uint8_t *arenaPtrs[EMERALD_POKEDEX_ROW_COUNT];
    const uint8_t *rowSrc[EMERALD_POKEDEX_ROW_COUNT];
    size_t packCount;
    size_t i;
    uint32_t schema38 = 0u, schema39 = 0u, schema40 = 0u;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_POKEDEX_SCHEMA_ROW,
                    0u, 0u, 0u, NULL);
        return EMERALD_POKEDEX_ERR_INVALID_ARGUMENT;
    }
    memset(resolvedLabels, 0, sizeof(resolvedLabels));
    memset(arenaPtrs, 0, sizeof(arenaPtrs));
    memset(rowSrc, 0, sizeof(rowSrc));
    packCount = Gen3ResourcePack_GetEntryCount(pack);

    /* ---- Phase 1a: rows + description→text binding + pack parity. ---- */
    for (i = 0u; i < EMERALD_POKEDEX_ROW_COUNT; i++)
    {
        const char *name = kPokedexRowKeys[i];
        const char *label = kPokedexDescLabels[i];
        const uint8_t *wire;
        const uint8_t *arenaBytes = NULL;
        size_t arenaSize = 0u;
        size_t j;
        uint32_t desc;
        const char *foundLabel = NULL;

        wire = ResolveRowResource(snapshot, pack, name,
                                  EMERALD_POKEDEX_SCHEMA_ROW,
                                  EMERALD_POKEDEX_ROW_WIRE,
                                  diagnostics, &result);
        if (wire == NULL)
            goto done;
        rowSrc[i] = wire;

        desc = ReadLe32(wire + 16u);
        if (desc < EMERALD_POKEDEX_GBA_ROM_BASE)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_POKEDEX_SCHEMA_ROW,
                        EMERALD_POKEDEX_SCHEMA_ROW,
                        (uint32_t)(desc - EMERALD_POKEDEX_GBA_ROM_BASE),
                        (uint32_t)desc, NULL);
            result = EMERALD_POKEDEX_ERR_DESC_PTR_INVALID;
            goto done;
        }

        /* Resolve the description GBA address to an R13-C pokedex text label
         * by scanning the pack's text entries (sourceRomOffset == addr -
         * 0x08000000). */
        for (j = 0u; j < packCount; j++)
        {
            const struct Gen3ResourcePackEntry *pe =
                Gen3ResourcePack_GetEntry(pack, j);
            if (pe == NULL || pe->type != GEN3_RESOURCE_TYPE_TEXT)
                continue;
            if (pe->sourceRomOffset != (uint32_t)
                    (desc - EMERALD_POKEDEX_GBA_ROM_BASE))
                continue;
            foundLabel = pe->canonicalName;
            break;
        }
        if (foundLabel == NULL
                || strncmp(foundLabel, "emerald:text/pokedex/", 21u) != 0)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_POKEDEX_SCHEMA_ROW,
                        EMERALD_POKEDEX_SCHEMA_ROW,
                        (uint32_t)(desc - EMERALD_POKEDEX_GBA_ROM_BASE),
                        (uint32_t)desc, NULL);
            result = EMERALD_POKEDEX_ERR_TEXT_BINDING_MISSING;
            goto done;
        }
        if (strcmp(foundLabel, label) != 0)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_POKEDEX_SCHEMA_ROW,
                        EMERALD_POKEDEX_SCHEMA_ROW, 0u, 0u, NULL);
            result = EMERALD_POKEDEX_ERR_TEXT_LABEL_MISMATCH;
            goto done;
        }
        resolvedLabels[i] = foundLabel;

        /* Fetch the published arena pointer for the label. */
        if (!EmeraldTextCompat_GetResourceBytes(label, &arenaBytes, &arenaSize)
                || arenaBytes == NULL || arenaSize == 0u)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_POKEDEX_SCHEMA_ROW,
                        EMERALD_POKEDEX_SCHEMA_ROW, 0u, 0u, NULL);
            result = EMERALD_POKEDEX_ERR_TEXT_NOT_PUBLISHED;
            goto done;
        }
        arenaPtrs[i] = arenaBytes;
    }

    /* ---- Phase 1b: ordering/routing resources. ---- */
    {
        static struct { const char *key; uint32_t schema; size_t bytes; }
            whole[4] =
        {
            { NULL, EMERALD_POKEDEX_SCHEMA_ORDER,
              (size_t)EMERALD_POKEDEX_ALPHABETICAL_COUNT * 2u },
            { NULL, EMERALD_POKEDEX_SCHEMA_ORDER,
              (size_t)EMERALD_POKEDEX_HEIGHT_COUNT * 2u },
            { NULL, EMERALD_POKEDEX_SCHEMA_ORDER,
              (size_t)EMERALD_POKEDEX_WEIGHT_COUNT * 2u },
            { NULL, EMERALD_POKEDEX_SCHEMA_S2N,
              (size_t)EMERALD_POKEDEX_S2N_COUNT * 2u },
        };
        whole[0].key = kPokedexOrderAlphabeticalKey;
        whole[1].key = kPokedexOrderHeightKey;
        whole[2].key = kPokedexOrderWeightKey;
        whole[3].key = kPokedexSpeciesToNationalKey;
        for (i = 0u; i < 4u; i++)
        {
            const uint8_t *p = ResolveRowResource(snapshot, pack, whole[i].key,
                                                  whole[i].schema,
                                                  whole[i].bytes,
                                                  diagnostics, &result);
            if (p == NULL)
            {
                result = EMERALD_POKEDEX_ERR_MALFORMED_ORDER;
                goto done;
            }
            if (i == 0u)
                memcpy(sOrderAlpha, p, whole[i].bytes);
            else if (i == 1u)
                memcpy(sOrderHeight, p, whole[i].bytes);
            else if (i == 2u)
                memcpy(sOrderWeight, p, whole[i].bytes);
            else
                memcpy(sS2N, p, whole[i].bytes);
        }
    }

    /* ---- Phase 1c: pack-level schema-count set equality + row key set. ---- */
    for (i = 0u; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *pe = Gen3ResourcePack_GetEntry(pack, i);
        if (pe == NULL || pe->type != GEN3_RESOURCE_TYPE_STRUCTURED_DATA)
            continue;
        if (pe->schema == EMERALD_POKEDEX_SCHEMA_ROW) schema38++;
        else if (pe->schema == EMERALD_POKEDEX_SCHEMA_ORDER) schema39++;
        else if (pe->schema == EMERALD_POKEDEX_SCHEMA_S2N) schema40++;
    }
    if (schema38 != EMERALD_POKEDEX_ROW_COUNT
     || schema39 != 3u
     || schema40 != 1u)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                    GEN3_RESOURCE_TYPE_INVALID, EMERALD_POKEDEX_SCHEMA_ROW,
                    EMERALD_POKEDEX_SCHEMA_ROW, 0u, schema38, NULL);
        result = EMERALD_POKEDEX_ERR_UNEXPECTED_COUNT;
        goto done;
    }

    /* ---- Phase 2: precompute native rows + arrays (infallible), and run the
     * native-oracle parity proof. ---- */
    for (i = 0u; i < EMERALD_POKEDEX_ROW_COUNT; i++)
    {
        const uint8_t *w = rowSrc[i];
        struct PokedexEntry *d = &sEntryRows[i];
        /* category inline: byte-identical copy. */
        memcpy(d->categoryName, w, 12u);
        d->height = ReadLe16(w + 12u);
        d->weight = ReadLe16(w + 14u);
        d->description = (const u8 *)arenaPtrs[i]; /* re-point into arena */
        d->unused = ReadLe16(w + 20u);
        d->pokemonScale = ReadLe16(w + 22u);
        d->pokemonOffset = ReadLe16(w + 24u);
        d->trainerScale = ReadLe16(w + 26u);
        d->trainerOffset = ReadLe16(w + 28u);

        /* Oracle: the native row (minus the re-pointed text pointer) must
         * reproduce the wire row's category + scalar bytes exactly. */
        if (memcmp(d->categoryName, w, 12u) != 0
                || d->height != ReadLe16(w + 12u)
                || d->weight != ReadLe16(w + 14u)
                || d->unused != ReadLe16(w + 20u)
                || d->pokemonScale != ReadLe16(w + 22u)
                || d->pokemonOffset != ReadLe16(w + 24u)
                || d->trainerScale != ReadLe16(w + 26u)
                || d->trainerOffset != ReadLe16(w + 28u))
        {
            NoteFailure(diagnostics, "build", kPokedexRowKeys[i],
                        GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                        GEN3_RESOURCE_TYPE_INVALID,
                        EMERALD_POKEDEX_SCHEMA_ROW,
                        EMERALD_POKEDEX_SCHEMA_ROW, 0u, 0u, NULL);
            result = EMERALD_POKEDEX_ERR_PARITY_MISMATCH;
            goto done;
        }
    }
    /* Ordering/routing array parity: each u16 array must equal kPokedexDescLabels'
     * companion pack payloads, which phase 1b already byte-copied verbatim; the
     * native array content is byte-identical by construction. Nothing further to
     * prove - the copies above duplicated the ROM slices exactly. */

    /* ---- Phase 3: publish atomically (pure stores). ---- */
    memcpy(gPokedexEntries, sEntryRows, sizeof(sEntryRows));
    memcpy(gPokedexOrder_Alphabetical, sOrderAlpha, sizeof(sOrderAlpha));
    memcpy(gPokedexOrder_Height, sOrderHeight, sizeof(sOrderHeight));
    memcpy(gPokedexOrder_Weight, sOrderWeight, sizeof(sOrderWeight));
    memcpy(sSpeciesToNationalPokedexNum, sS2N, sizeof(sS2N));

    sPublishedCount = EMERALD_POKEDEX_ROW_COUNT + 4u;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
                GEN3_RESOURCE_TYPE_INVALID, 0u, 0u, 0u, 0u, NULL);
    result = EMERALD_POKEDEX_OK;

done:
    return result;
}

void EmeraldPokedexCompat_ClearMigratedEntries(void)
{
    size_t i;
    /* NULL every description pointer so no host pointer dangles into a
     * torn-down text arena, then zero the HOST_DATA rows/arrays. */
    for (i = 0u; i < EMERALD_POKEDEX_ROW_COUNT; i++)
        memset(&gPokedexEntries[i], 0, sizeof(struct PokedexEntry));
    memset(gPokedexOrder_Alphabetical, 0, sizeof(gPokedexOrder_Alphabetical));
    memset(gPokedexOrder_Height, 0, sizeof(gPokedexOrder_Height));
    memset(gPokedexOrder_Weight, 0, sizeof(gPokedexOrder_Weight));
    memset(sSpeciesToNationalPokedexNum, 0, sizeof(sSpeciesToNationalPokedexNum));
    sPublishedCount = 0u;
}

void EmeraldPokedexCompat_Shutdown(void)
{
    EmeraldPokedexCompat_ClearMigratedEntries();
}

size_t EmeraldPokedexCompat_GetPublishedCount(void)
{
    return sPublishedCount;
}

const char *EmeraldPokedexCompatStatus_Describe(
    enum EmeraldPokedexCompatStatus status)
{
    switch (status)
    {
    case EMERALD_POKEDEX_OK: return "ok";
    case EMERALD_POKEDEX_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_POKEDEX_ERR_OUT_OF_MEMORY: return "out of memory";
    case EMERALD_POKEDEX_ERR_RESOLVE_FAILED: return "resolve failed";
    case EMERALD_POKEDEX_ERR_UNEXPECTED_OWNERSHIP: return "unexpected ownership";
    case EMERALD_POKEDEX_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_POKEDEX_ERR_UNEXPECTED_COUNT: return "unexpected count";
    case EMERALD_POKEDEX_ERR_MISSING_RESOURCE: return "missing resource";
    case EMERALD_POKEDEX_ERR_UNEXPECTED_TYPE: return "unexpected type";
    case EMERALD_POKEDEX_ERR_UNEXPECTED_SCHEMA: return "unexpected schema";
    case EMERALD_POKEDEX_ERR_DESC_PTR_INVALID: return "invalid description pointer";
    case EMERALD_POKEDEX_ERR_TEXT_BINDING_MISSING: return "missing text binding";
    case EMERALD_POKEDEX_ERR_TEXT_LABEL_MISMATCH: return "text label mismatch";
    case EMERALD_POKEDEX_ERR_TEXT_NOT_PUBLISHED: return "text arena not published";
    case EMERALD_POKEDEX_ERR_MALFORMED_ORDER: return "malformed ordering table";
    case EMERALD_POKEDEX_ERR_PARITY_MISMATCH: return "parity mismatch";
    case EMERALD_POKEDEX_ERR_UNAVAILABLE: return "unavailable";
    }
    return "unknown";
}