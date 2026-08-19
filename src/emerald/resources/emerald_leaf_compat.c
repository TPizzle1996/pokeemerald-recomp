/* R13-B: Emerald leaf payload ownership migration - additive publication
 * seam (movement scripts + multiboot programs).
 * See include/emerald/resources/emerald_leaf_compat.h for the contract.
 * Platform-neutral: no global.h, engine objects, or frontend.
 *
 * ADDITIVE: the 1,057 canonical leaf payloads (1,055 movement + 2
 * multiboot, 183,780 B) resolve through the normal M0/M1 snapshot
 * (winner must be the ROM_BASE provider) and their canonical bytes are
 * copied into ONE process-lifetime arena: a record table (canonical id,
 * legacy compiled symbol, ROM offset, size, schema) + a packed payload
 * zone. No consumer is redirected, so a failed publication degrades
 * (the compiled payloads still serve the game exactly as pre-R13-B), it
 * never refuses the session. There is no republish path (no hot reload),
 * no R10 range registration, no logical-address publication, and no host
 * pointer inside the arena: the payloads are pure bytes (movement
 * bytecode / multiboot programs) and the record table holds only names,
 * ROM offsets, sizes and schemas - nothing persistable as a raw native
 * pointer.
 *
 * Transactional: phase 1 validates every leaf (composition pins,
 * M0/M1 resolution, type/schema, ownership, size + byte equality
 * against the pack) AND the generated slot table (name/size/schema set
 * equality against kLeafNativeResources) before any allocation; phase 2
 * performs the single arena allocation and all copies. On any failure
 * the arena stays absent and the diagnostics name the first failing
 * resource.
 */

#include "emerald/resources/emerald_leaf_compat.h"
#include "emerald/resources/emerald_resource_session.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One leaf record: identity + placement in the packed payload zone. No
 * host pointers anywhere - payloadOffset is relative to bytes[]. The
 * canonical id buffer must hold the LONGEST leaf id in the family
 * ('emerald:movement/battlefrontier-battlefactoryprebattleroom-movement-
 * playerwalktobattleroomlvopen' is exactly 96 chars; a truncating copy
 * made the phase-1b sorted-name comparison fail against the untruncated
 * slot table), and the legacy symbol buffer the longest compiled symbol
 * (79 chars) - both 128. */
struct EmeraldLeafRecord
{
    char canonicalName[128];
    char legacySymbol[128]; /* compiled native symbol (from the slot table) */
    uint32_t romOffset;     /* manifest rom_offset (file-relative) */
    uint32_t size;
    uint8_t schema;         /* 1 = movement, 2 = multiboot */
    uint32_t payloadOffset; /* relative to bytes[] */
};

struct EmeraldLeafArena
{
    size_t spanSize;            /* EMERALD_LEAF_TOTAL_BYTES when published */
    size_t publishedCount;      /* EMERALD_LEAF_RESOURCE_COUNT when published */
    size_t recordTableOffset;   /* relative to bytes[] */
    size_t payloadZoneOffset;   /* relative to bytes[] */
    uint8_t bytes[];
};

static struct EmeraldLeafArena *sArena; /* NULL = not published */

static void ClearDiagnostics(struct EmeraldLeafCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static void NoteFailure(struct EmeraldLeafCompatDiagnostics *diagnostics,
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

/* Classify a leaf pack entry into its family. The R13-B vocabulary is
 * deliberately narrow: type binary + schema 1|2 + canonical
 * representation gba-bytes, names under the two family prefixes. Any
 * other binary-typed entry (or a family-prefixed entry with the wrong
 * schema) is an unexpected-composition failure, never silently ignored. */
enum LeafKind
{
    LEAF_KIND_UNKNOWN = 0,
    LEAF_KIND_MOVEMENT,
    LEAF_KIND_MULTIBOOT,
};

static enum LeafKind ClassifyLeaf(const struct Gen3ResourcePackEntry *entry)
{
    static const char movementPrefix[] = "emerald:movement/";
    static const char multibootPrefix[] = "emerald:multiboot/";

    if (entry->type != GEN3_RESOURCE_TYPE_BINARY)
        return LEAF_KIND_UNKNOWN;
    if (entry->schema == 1u
     && strncmp(entry->canonicalName, movementPrefix,
                sizeof(movementPrefix) - 1u) == 0)
        return LEAF_KIND_MOVEMENT;
    if (entry->schema == 2u
     && strncmp(entry->canonicalName, multibootPrefix,
                sizeof(multibootPrefix) - 1u) == 0)
        return LEAF_KIND_MULTIBOOT;
    return LEAF_KIND_UNKNOWN;
}

/* Record comparators: by canonical name (table cross-check, phase-2
 * order) and by ROM offset (slice-disjointness proof, phase 1c). */
static int CompareRowsByName(const void *a, const void *b)
{
    const struct EmeraldLeafRecord *ra = (const struct EmeraldLeafRecord *)a;
    const struct EmeraldLeafRecord *rb = (const struct EmeraldLeafRecord *)b;
    return strcmp(ra->canonicalName, rb->canonicalName);
}

static int CompareRowsByOffset(const void *a, const void *b)
{
    const struct EmeraldLeafRecord *ra = (const struct EmeraldLeafRecord *)a;
    const struct EmeraldLeafRecord *rb = (const struct EmeraldLeafRecord *)b;
    if (ra->romOffset != rb->romOffset)
        return ra->romOffset < rb->romOffset ? -1 : 1;
    return strcmp(ra->canonicalName, rb->canonicalName);
}

enum EmeraldLeafCompatStatus
EmeraldLeafCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldLeafCompatDiagnostics *diagnostics)
{
    enum EmeraldLeafCompatStatus result = EMERALD_LEAF_OK;
    struct EmeraldLeafRecord *records = NULL;
    struct EmeraldLeafArena *arena = NULL;
    size_t packCount;
    size_t movementCount = 0u;
    size_t multibootCount = 0u;
    size_t payloadBytes = 0u;
    size_t recordTableBytes;
    size_t payloadZoneOffset;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_BINARY,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_LEAF_ERR_INVALID_ARGUMENT;
    }

    /* Phase 1a: validate the whole LEAF family set before any allocation.
     * The pack iteration is the authoritative leaf enumeration (the
     * session catalog is pack-derived); the snapshot provides the M0/M1
     * view for each leaf. */
    records = (struct EmeraldLeafRecord *)calloc(
        EMERALD_LEAF_RESOURCE_COUNT, sizeof(*records));
    if (records == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_BINARY,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_LEAF_ERR_OUT_OF_MEMORY;
    }

    packCount = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        struct Gen3ResourceView view;
        enum LeafKind kind;
        struct EmeraldLeafRecord *record;
        Gen3ResourceHandle handle;
        enum Gen3ResourceResult resolveResult;
        size_t count;

        kind = ClassifyLeaf(entry);
        if (kind == LEAF_KIND_UNKNOWN)
            continue;
        if (kind == LEAF_KIND_MOVEMENT)
        {
            if (movementCount >= EMERALD_LEAF_MOVEMENT_COUNT)
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            GEN3_RESOURCE_TYPE_BINARY, entry->type,
                            1u, entry->schema, EMERALD_LEAF_MOVEMENT_COUNT,
                            (uint32_t)movementCount + 1u, NULL);
                result = EMERALD_LEAF_ERR_UNEXPECTED_COUNT;
                goto done;
            }
            count = movementCount;
            movementCount++;
        }
        else
        {
            if (multibootCount >= EMERALD_LEAF_MULTIBOOT_COUNT)
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            GEN3_RESOURCE_TYPE_BINARY, entry->type,
                            2u, entry->schema, EMERALD_LEAF_MULTIBOOT_COUNT,
                            (uint32_t)multibootCount + 1u, NULL);
                result = EMERALD_LEAF_ERR_UNEXPECTED_COUNT;
                goto done;
            }
            count = EMERALD_LEAF_MOVEMENT_COUNT + multibootCount;
            multibootCount++;
        }

        resolveResult = Gen3ResourceSnapshot_FindHandle(
            snapshot, entry->canonicalName, &handle);
        if (resolveResult != GEN3_RESOURCE_OK)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_BINARY, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize, 0u, NULL);
            result = EMERALD_LEAF_ERR_RESOLVE_FAILED;
            goto done;
        }
        resolveResult = Gen3ResourceSnapshot_Resolve(
            snapshot, handle, GEN3_RESOURCE_TYPE_BINARY, entry->schema, &view);
        if (resolveResult != GEN3_RESOURCE_OK || view.payload == NULL)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_BINARY, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize, 0u, NULL);
            result = EMERALD_LEAF_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_BINARY, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_LEAF_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        if (view.payloadSize != entry->payloadSize || entry->payload == NULL)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_BINARY, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_LEAF_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (memcmp(view.payload, entry->payload, view.payloadSize) != 0)
        {
            /* The resolver view must be byte-identical to the pack entry:
             * the arena copies session bytes, never pack-only bytes. */
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_BINARY, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_LEAF_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }

        record = &records[count];
        snprintf(record->canonicalName, sizeof(record->canonicalName), "%s",
                 entry->canonicalName);
        record->romOffset = entry->sourceRomOffset;
        record->size = view.payloadSize;
        record->schema = (uint8_t)entry->schema;
        payloadBytes += view.payloadSize;
    }

    if (movementCount != EMERALD_LEAF_MOVEMENT_COUNT
     || multibootCount != EMERALD_LEAF_MULTIBOOT_COUNT)
    {
        char buffer[160];
        snprintf(buffer, sizeof(buffer),
                 "%zu leaves (movement %zu, multiboot %zu)",
                 movementCount + multibootCount, movementCount, multibootCount);
        NoteFailure(diagnostics, "build", buffer,
                    GEN3_RESOURCE_TYPE_BINARY, GEN3_RESOURCE_TYPE_INVALID,
                    1u, 0u, EMERALD_LEAF_RESOURCE_COUNT,
                    (uint32_t)(movementCount + multibootCount), NULL);
        result = EMERALD_LEAF_ERR_UNEXPECTED_COUNT;
        goto done;
    }
    if (payloadBytes != EMERALD_LEAF_TOTAL_BYTES)
    {
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "%zu payload bytes", payloadBytes);
        NoteFailure(diagnostics, "build", buffer,
                    GEN3_RESOURCE_TYPE_BINARY, GEN3_RESOURCE_TYPE_INVALID,
                    1u, 0u, EMERALD_LEAF_TOTAL_BYTES,
                    (uint32_t)payloadBytes, NULL);
        result = EMERALD_LEAF_ERR_PAYLOAD_SIZE_MISMATCH;
        goto done;
    }

    /* Phase 1b: cross-check the collected set against the generated slot
     * table (sorted canonical-name set equality + per-row size/schema).
     * The table is the expected inventory; a pack that disagrees with it
     * (missing entry, extra entry, or drift) fails before any allocation. */
    qsort(records, EMERALD_LEAF_RESOURCE_COUNT, sizeof(*records),
          CompareRowsByName);
    for (i = 0; i < EMERALD_LEAF_RESOURCE_COUNT; i++)
    {
        const struct LeafNativeResource *slot = &kLeafNativeResources[i];
        const struct EmeraldLeafRecord *record = &records[i];
        if (strcmp(slot->name, record->canonicalName) != 0
         || slot->size != record->size
         || slot->schema != record->schema)
        {
            char buffer[160];
            snprintf(buffer, sizeof(buffer),
                     "slot '%s' (size %u, schema %u) vs pack '%s' (size %u, "
                     "schema %u)", slot->name, slot->size, slot->schema,
                     record->canonicalName, record->size, record->schema);
            NoteFailure(diagnostics, "build", buffer,
                        GEN3_RESOURCE_TYPE_BINARY,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                        EMERALD_LEAF_RESOURCE_COUNT, (uint32_t)i, NULL);
            result = EMERALD_LEAF_ERR_TABLE_MISMATCH;
            goto done;
        }
    }
    /* Fill the legacy symbols from the table (the pack does not carry
     * them) so the arena records are self-contained. */
    for (i = 0; i < EMERALD_LEAF_RESOURCE_COUNT; i++)
        snprintf(records[i].legacySymbol, sizeof(records[i].legacySymbol),
                 "%s", kLeafNativeResources[i].legacySymbol);

    /* Phase 1c: prove the CLAIMED ROM SLICES are pairwise disjoint. The
     * generator proved the movement extents tile script_data and the
     * multiboot programs tile other_data, but the seam re-derives the
     * disjointness at publish time (mirrors the audio tiling proof): a
     * pack whose slices overlap is provably inconsistent with the
     * qualified ROM layout - fail closed before any allocation. */
    qsort(records, EMERALD_LEAF_RESOURCE_COUNT, sizeof(*records),
          CompareRowsByOffset);
    for (i = 0; i + 1u < EMERALD_LEAF_RESOURCE_COUNT; i++)
    {
        const struct EmeraldLeafRecord *ra = &records[i];
        const struct EmeraldLeafRecord *rb = &records[i + 1u];
        uint64_t aEnd = (uint64_t)ra->romOffset + ra->size;
        if (aEnd > rb->romOffset)
        {
            char buffer[160];
            snprintf(buffer, sizeof(buffer),
                     "'%s' @0x%x +%u overlaps '%s' @0x%x",
                     ra->canonicalName, ra->romOffset, ra->size,
                     rb->canonicalName, rb->romOffset);
            NoteFailure(diagnostics, "build", buffer,
                        GEN3_RESOURCE_TYPE_BINARY,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                        (uint32_t)aEnd, rb->romOffset, NULL);
            result = EMERALD_LEAF_ERR_OVERLAPPING_SLICE;
            goto done;
        }
    }
    /* Restore name order: phase 2 assigns payload offsets in the table's
     * sorted-key order, so the arena layout is deterministic. */
    qsort(records, EMERALD_LEAF_RESOURCE_COUNT, sizeof(*records),
          CompareRowsByName);

    /* Phase 2: one allocation for the header + record table + packed
     * payload zone. Payload offsets are assigned in sorted-name order and
     * 8-aligned (no alignment requirement for byte blobs, but keeping the
     * zone base aligned is cheap and future-proof). */
    recordTableBytes = EMERALD_LEAF_RESOURCE_COUNT * sizeof(*records);
    payloadZoneOffset = (recordTableBytes + 7u) & ~(size_t)7u;
    if (recordTableBytes / EMERALD_LEAF_RESOURCE_COUNT != sizeof(*records)
     || payloadZoneOffset < recordTableBytes
     || SIZE_MAX - sizeof(struct EmeraldLeafArena) - payloadZoneOffset
            < EMERALD_LEAF_TOTAL_BYTES)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_BINARY,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_LEAF_ERR_OUT_OF_MEMORY;
        goto done;
    }
    arena = (struct EmeraldLeafArena *)malloc(
        sizeof(struct EmeraldLeafArena) + payloadZoneOffset
        + EMERALD_LEAF_TOTAL_BYTES);
    if (arena == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_BINARY,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_LEAF_ERR_OUT_OF_MEMORY;
        goto done;
    }
    memset(arena, 0, sizeof(*arena) + payloadZoneOffset
           + EMERALD_LEAF_TOTAL_BYTES);
    arena->spanSize = EMERALD_LEAF_TOTAL_BYTES;
    arena->publishedCount = EMERALD_LEAF_RESOURCE_COUNT;
    arena->recordTableOffset = 0u;
    arena->payloadZoneOffset = payloadZoneOffset;
    memcpy(arena->bytes, records, recordTableBytes);

    {
        size_t pos = 0u;
        for (i = 0; i < EMERALD_LEAF_RESOURCE_COUNT; i++)
        {
            struct EmeraldLeafRecord *record =
                (struct EmeraldLeafRecord *)(void *)(arena->bytes
                    + arena->recordTableOffset + i * sizeof(*record));
            Gen3ResourceHandle handle;
            struct Gen3ResourceView view;

            if (Gen3ResourceSnapshot_FindHandle(
                    snapshot, record->canonicalName, &handle) != GEN3_RESOURCE_OK
             || Gen3ResourceSnapshot_Resolve(
                    snapshot, handle, GEN3_RESOURCE_TYPE_BINARY,
                    record->schema, &view) != GEN3_RESOURCE_OK
             || view.payload == NULL || view.payloadSize != record->size)
            {
                /* Provably unreachable (phase 1a validated every leaf
                 * through the same snapshot) but the fail-closed check is
                 * the seam's job, not re-resolution luck. */
                NoteFailure(diagnostics, "build", record->canonicalName,
                            GEN3_RESOURCE_TYPE_BINARY,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            record->size, 0u, NULL);
                result = EMERALD_LEAF_ERR_RESOLVE_FAILED;
                goto done;
            }
            record->payloadOffset = (uint32_t)pos;
            memcpy(arena->bytes + arena->payloadZoneOffset + pos,
                   view.payload, record->size);
            pos += record->size;
        }
    }

    /* Publish atomically: only a fully built arena replaces the old one. */
    if (sArena != NULL)
    {
        free(sArena);
        sArena = NULL;
    }
    sArena = arena;
    arena = NULL;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_BINARY,
                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                (uint32_t)EMERALD_LEAF_TOTAL_BYTES, 0u, NULL);
    result = EMERALD_LEAF_OK;

done:
    if (records != NULL)
        free(records);
    if (arena != NULL)
        free(arena);
    return result;
}

void EmeraldLeafCompat_ClearMigratedEntries(void)
{
    if (sArena != NULL)
    {
        free(sArena);
        sArena = NULL;
    }
}

void EmeraldLeafCompat_Shutdown(void)
{
    EmeraldLeafCompat_ClearMigratedEntries();
}

bool EmeraldLeafCompat_GetArena(const uint8_t **outBase, size_t *outSize)
{
    if (outBase != NULL)
        *outBase = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    if (sArena == NULL || outBase == NULL || outSize == NULL)
        return false;
    *outBase = sArena->bytes + sArena->payloadZoneOffset;
    *outSize = sArena->spanSize;
    return true;
}

size_t EmeraldLeafCompat_GetPublishedCount(void)
{
    return sArena != NULL ? sArena->publishedCount : 0u;
}

static const struct EmeraldLeafRecord *FindRecord(const char *canonicalName)
{
    size_t i;

    if (sArena == NULL || canonicalName == NULL)
        return NULL;
    for (i = 0; i < sArena->publishedCount; i++)
    {
        const struct EmeraldLeafRecord *record =
            (const struct EmeraldLeafRecord *)(const void *)
                (sArena->bytes + sArena->recordTableOffset
                 + i * sizeof(struct EmeraldLeafRecord));
        if (strcmp(record->canonicalName, canonicalName) == 0)
            return record;
    }
    return NULL;
}

bool EmeraldLeafCompat_GetResourceSpan(const char *canonicalName,
                                       size_t *outRomOffset, size_t *outSize,
                                       uint8_t *outSchema)
{
    const struct EmeraldLeafRecord *record = FindRecord(canonicalName);

    if (record == NULL)
        return false;
    if (outRomOffset != NULL)
        *outRomOffset = record->romOffset;
    if (outSize != NULL)
        *outSize = record->size;
    if (outSchema != NULL)
        *outSchema = record->schema;
    return true;
}

bool EmeraldLeafCompat_GetResourceBytes(const char *canonicalName,
                                        const uint8_t **outBytes,
                                        size_t *outSize)
{
    const struct EmeraldLeafRecord *record = FindRecord(canonicalName);

    if (record == NULL || sArena == NULL)
        return false;
    if (outBytes != NULL)
        *outBytes = sArena->bytes + sArena->payloadZoneOffset
                  + record->payloadOffset;
    if (outSize != NULL)
        *outSize = record->size;
    return true;
}

bool EmeraldLeafCompat_ContainsPointer(uintptr_t address)
{
    const uint8_t *base;
    size_t size;

    if (!EmeraldLeafCompat_GetArena(&base, &size))
        return false;
    return address >= (uintptr_t)base && address < (uintptr_t)base + size;
}
