/* R13-C: Emerald text ownership migration - publication seam for the
 * sixteen per-family text arenas + live slot/skeleton cutover.
 * See include/emerald/resources/emerald_text_compat.h for the contract.
 * Platform-neutral: no global.h, engine objects, or frontend.
 *
 * REFUSE-CLASS (brief §17): the cut-over families (battle/move/ability/
 * nature/shared/system/match-call/ribbon) have NO compiled fallback once
 * the C-side guards land, so a session whose text cannot be published is
 * refused - the loader rolls the whole registration back. Unlike the
 * additive leaf seam there is no degrade path: this seam is
 * transactional and the loader treats any non-OK status as fatal for
 * the session.
 *
 * Layout (deterministic, same inputs -> same bytes):
 *   one allocation = header (16 sub-arena descriptors) + 16 sub-arenas
 *   (each = label records sorted by (name, isBundleLocal) + a
 *   16-aligned payload zone, byte-exact canonical copies incl. 0xFF
 *   terminators) + the applied-fills rollback list (6,453 (target,
 *   value) pairs staged for ClearMigratedEntries).
 *
 * Phases: 1 validates everything against the pack, the snapshot and the
 * generated inventory before ANY allocation (composition pins, M0/M1
 * resolution, ROM_BASE ownership, type/schema, size + byte equality,
 * native-table set equality, escape-grammar revalidation, C-label
 * ROM-slice disjointness, per-bundle blob tiling, per-arena summaries);
 * 2 performs the single allocation, copies every payload and resolves
 * every slot/skeleton fill (a failure frees the build, the old arena
 * stays live); 3 publishes atomically and applies the infallible
 * stores. Escape grammar and bundle tiling mirror the generator's
 * charmap decode byte for byte (tools/gen3_resources/text_family/
 * gen_text_family.py: make_decode).
 */

#include "emerald/resources/emerald_text_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* R13-C §13-15: the sixteen family-arena payload zones register into the
 * shared R10 resource-range index so a mid-string
 * TextPrinter.printerTemplate.currentChar (the one interior-pointer
 * class in state) is captured as a State-v5 sidecar record (identity +
 * rangeOffset) and loads re-derive it against the fresh arena base.
 * The index instance is owned by the trainer seam; this seam reaches it
 * through EmeraldResourceCompat_GetRangeIndex(). The reference is WEAK
 * so offline links without the trainer seam (the seam tests) resolve it
 * to NULL and skip registration - the arena stays published and the
 * state walker fails closed on in-arena pointers (no range, no hull). */
#if defined(__GNUC__)
__attribute__((weak))
#endif
struct EmeraldResourceRangeIndex *EmeraldResourceCompat_GetRangeIndex(void);

static bool RegisterArenaRanges(void);
static void UnregisterArenaRanges(void);

#if TEXT_SLOT_BINDING_COUNT + TEXT_SKELETON_FILL_COUNT != 6453u
#error "applied text fill count disagrees with the R13-C pins"
#endif

#define TEXT_PREFIX "emerald:text/"
#define TEXT_PREFIX_LEN (sizeof(TEXT_PREFIX) - 1u)

/* One arena record: identity + placement in the sub-arena payload zone.
 * The dash-form name is the sort key; EMERALD_TEXT_NAME_CAP holds the
 * longest canonical label (the longest resource id is 72 chars; the
 * bundle-label dash names are shorter - 128 is generous but pinned by
 * the header so a truncating copy is a build error, not a comparison
 * surprise). No host pointers anywhere: payloadOffset is relative to
 * the sub-arena payload zone. */
struct TextRecord
{
    char name[EMERALD_TEXT_NAME_CAP];
    uint32_t payloadOffset;
    uint32_t size;
    uint8_t isBundleLocal;   /* 0 = C-side per-label resource,
                                1 = bundle-local label (blob copy) */
    uint8_t pad[3];
};

struct TextSubArena
{
    uint32_t labelCount;
    uint32_t payloadSize;    /* byteTotal: sum of label sizes incl. 0xFF */
    size_t recordsOffset;    /* relative to bytes[] */
    size_t payloadOffset;    /* relative to bytes[] */
};

struct AppliedFill
{
    const void **target;
    const void *value;
};

struct EmeraldTextArena
{
    size_t publishedCount;     /* EMERALD_TEXT_LABEL_COUNT when published */
    size_t totalPayloadBytes;  /* EMERALD_TEXT_TOTAL_BYTES when published */
    size_t appliedFillCount;   /* 6,091 when published */
    size_t appliedFillsOffset; /* relative to bytes[] */
    struct TextSubArena subArenas[TEXT_ARENA_COUNT];
    uint8_t bytes[];
};

static struct EmeraldTextArena *sArena; /* NULL = not published */

static void ClearDiagnostics(struct EmeraldTextCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static void NoteFailure(struct EmeraldTextCompatDiagnostics *diagnostics,
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

/* Escape-grammar revalidation, mirroring the generator's make_decode()
 * byte for byte: 0xFF is valid only as the final byte; F8/F9/FC/FD are
 * two-byte escapes (second byte consumed unchecked, but may not be the
 * terminator - the loop then ends without finding it); every other byte
 * is a charmap single (the extraction charmap maps all 253 non-escape
 * bytes; the four escape starts and 0xFF are the only special cases).
 * The R13-C gate requires 0 validation failures on the canonical
 * payloads, so this re-proves what the importer proved. */
static bool ValidEscapedString(const uint8_t *b, size_t len)
{
    size_t i = 0;
    while (i < len)
    {
        uint8_t c = b[i];
        if (c == 0xFF)
            return i == len - 1;
        if (c == 0xF8 || c == 0xF9 || c == 0xFC || c == 0xFD)
        {
            if (i + 1 >= len)
                return false;
            i += 2;
            continue;
        }
        i++;
    }
    return false; /* no terminator */
}

/* A payload that is not one canonical string may still be a row-major
 * concatenation of canonical strings (gText_123Dot: "1.\xff2.\xff3.\xff"
 * as a single 9-byte resource), but only when kind-0 skeleton fills
 * carry byte offsets that tile the payload exactly: at least two fills,
 * ascending offsets starting at 0, each slice running to its own 0xFF
 * terminator and passing the escape grammar, and the slices covering
 * the payload with no gaps and no remainder. Every other label has
 * either no kind-0 fills or a single row-0 fill and fails here (n < 2),
 * so the rule cannot loosen the whole-string check elsewhere. */
static bool TextPayloadTiledByFills(const char *name, const uint8_t *payload,
                                    size_t payloadSize)
{
    uint32_t offsets[TEXT_SKELETON_FILL_COUNT];
    size_t count = 0u;
    size_t t, f, i, pos;

    for (t = 0u; t < TEXT_SKELETON_TABLE_COUNT; t++)
    {
        const struct TextSkeletonTableInfo *table = &kTextSkeletonTables[t];
        for (f = 0u; f < table->fillCount; f++)
        {
            const struct TextSkeletonFill *fill = &table->fills[f];
            if (fill->kind == 0u && strcmp(fill->name, name) == 0)
                offsets[count++] = fill->row;
        }
    }
    if (count < 2u)
        return false;
    /* Insertion sort: fills are not emitted in offset order. */
    for (i = 1u; i < count; i++)
    {
        uint32_t v = offsets[i];
        size_t j = i;
        while (j > 0u && offsets[j - 1u] > v)
        {
            offsets[j] = offsets[j - 1u];
            j--;
        }
        offsets[j] = v;
    }
    pos = 0u;
    for (i = 0u; i < count; i++)
    {
        size_t j;
        if (offsets[i] != pos)
            return false;
        j = offsets[i];
        while (j < payloadSize && payload[j] != 0xFF)
            j++;
        if (j >= payloadSize)
            return false;
        if (!ValidEscapedString(payload + offsets[i], j + 1u - offsets[i]))
            return false;
        pos = j + 1u;
    }
    return pos == payloadSize;
}

static bool IsTextEntry(const struct Gen3ResourcePackEntry *entry)
{
    return entry->type == GEN3_RESOURCE_TYPE_TEXT
        && entry->schema == 1u
        && strncmp(entry->canonicalName, TEXT_PREFIX, TEXT_PREFIX_LEN) == 0;
}

/* --- generated inventory lookups (all tables are sorted) --- */

static const struct TextNativeResource *FindNativeResource(const char *id)
{
    size_t lo = 0u;
    size_t hi = TEXT_NATIVE_RESOURCE_COUNT;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = strcmp(id, kTextNativeResources[mid].name);
        if (cmp == 0)
            return &kTextNativeResources[mid];
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return NULL;
}

static const struct TextBundleEntry *FindBundleIndexEntry(const char *name)
{
    size_t lo = 0u;
    size_t hi = TEXT_BUNDLE_ENTRY_COUNT;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = strcmp(name, kTextBundleIndex[mid].name);
        if (cmp == 0)
            return &kTextBundleIndex[mid];
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return NULL;
}

/* Bundle resource id -> index into kTextBundleIds (the ids are sorted,
 * and every bundle resource id appears there; 363 candidates make the
 * linear scan free). */
static bool FindBundleIndexByResourceId(const char *id, uint32_t *outIndex)
{
    uint32_t i;
    for (i = 0u; i < TEXT_BUNDLE_COUNT; i++)
    {
        if (strcmp(id, kTextBundleIds[i]) == 0)
        {
            *outIndex = i;
            return true;
        }
    }
    return false;
}

/* Strip "emerald:text/<family>/" -> the dash-form label name. */
static const char *DashNameOf(const char *id)
{
    const char *p = id + TEXT_PREFIX_LEN;
    p = strchr(p, '/');
    if (p == NULL)
        return NULL;
    return p + 1;
}

/* Record comparator: (name, isBundleLocal) - the arena sort key. */
static int CompareRecords(const void *a, const void *b)
{
    const struct TextRecord *ra = (const struct TextRecord *)a;
    const struct TextRecord *rb = (const struct TextRecord *)b;
    int cmp = strcmp(ra->name, rb->name);
    if (cmp != 0)
        return cmp;
    return (int)ra->isBundleLocal - (int)rb->isBundleLocal;
}

/* Build-scratch comparator: sorts by the record key only (the phase-2
 * scratch pairs a record with its resolved payload source). */
struct TextBuildRecord
{
    struct TextRecord record;
    const uint8_t *source; /* resolved view payload / blob+offset */
};

static int CompareBuildRecords(const void *a, const void *b)
{
    const struct TextBuildRecord *ra = (const struct TextBuildRecord *)a;
    const struct TextBuildRecord *rb = (const struct TextBuildRecord *)b;
    return CompareRecords(&ra->record, &rb->record);
}

static const struct TextRecord *FindRecordInArena(
    const struct EmeraldTextArena *arena, uint32_t arenaIndex,
    const char *name, bool isBundleLocal)
{
    const struct TextSubArena *sub = &arena->subArenas[arenaIndex];
    const struct TextRecord *records = (const struct TextRecord *)(void *)
        (arena->bytes + sub->recordsOffset);
    struct TextRecord key;
    size_t lo = 0u;
    size_t hi = sub->labelCount;

    memset(&key, 0, sizeof(key));
    snprintf(key.name, sizeof(key.name), "%s", name);
    key.isBundleLocal = isBundleLocal ? 1u : 0u;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2u;
        int cmp = CompareRecords(&key, &records[mid]);
        if (cmp == 0)
            return &records[mid];
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1u;
    }
    return NULL;
}

/* C-side resolution: resource id -> arena pointer. The native table is
 * authoritative for arena assignment; the arena record re-verifies the
 * size. Bundle rows (isBundle) are not C labels - a slot/fill naming a
 * bundle is a generator-contract violation and fails closed. */
static bool ResolveId(const struct EmeraldTextArena *arena, const char *id,
                      const uint8_t **outBytes, size_t *outSize)
{
    const struct TextNativeResource *row = FindNativeResource(id);
    const struct TextRecord *record;
    const char *dash;

    if (row == NULL || row->isBundle)
        return false;
    dash = DashNameOf(id);
    if (dash == NULL)
        return false;
    record = FindRecordInArena(arena, row->arenaIndex, dash, false);
    if (record == NULL || record->size != row->size)
        return false;
    if (outBytes != NULL)
        *outBytes = arena->bytes + arena->subArenas[row->arenaIndex].payloadOffset
            + record->payloadOffset;
    if (outSize != NULL)
        *outSize = record->size;
    return true;
}

/* Bundle-local resolution: canonical label -> arena pointer via the
 * bundle index (arenaIndex + size are authoritative). */
static bool ResolveLabel(const struct EmeraldTextArena *arena,
                         const char *name,
                         const uint8_t **outBytes, size_t *outSize)
{
    const struct TextBundleEntry *entry = FindBundleIndexEntry(name);
    const struct TextRecord *record;

    if (entry == NULL)
        return false;
    record = FindRecordInArena(arena, entry->arenaIndex, name, true);
    if (record == NULL || record->size != entry->size)
        return false;
    if (outBytes != NULL)
        *outBytes = arena->bytes + arena->subArenas[entry->arenaIndex].payloadOffset
            + record->payloadOffset;
    if (outSize != NULL)
        *outSize = record->size;
    return true;
}

/* The whole blob must resolve once per session through the SAME
 * snapshot (fail-closed re-resolution mirroring the leaf seam; the
 * phase-1a validation made it provably reachable, but the copy source
 * must be the session bytes, never a pack-only artifact). */
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

enum EmeraldTextCompatStatus
EmeraldTextCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldTextCompatDiagnostics *diagnostics)
{
    enum EmeraldTextCompatStatus result = EMERALD_TEXT_OK;
    struct EmeraldTextArena *arena = NULL;
    struct AppliedFill *applied = NULL;
    struct TextBuildRecord *build = NULL;
    size_t appliedCount = 0u;
    uint8_t *seen = NULL;
    uint64_t *labelSpans = NULL;
    size_t packCount;
    size_t labelCount = 0u;
    size_t bundleCount = 0u;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_TEXT_ERR_INVALID_ARGUMENT;
    }

    seen = (uint8_t *)calloc(TEXT_NATIVE_RESOURCE_COUNT, sizeof(*seen));
    if (seen == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_TEXT_ERR_OUT_OF_MEMORY;
    }
    labelSpans = (uint64_t *)calloc(TEXT_NATIVE_LABEL_COUNT,
                                    sizeof(*labelSpans));
    if (labelSpans == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_TEXT_ERR_OUT_OF_MEMORY;
        goto done;
    }

    /* Phase 1a: validate the whole text family against the pack + the
     * snapshot before any allocation. The pack iteration is the
     * authoritative enumeration; the native table classifies each entry
     * (the pack file itself carries no bundle flag). */
    packCount = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        const struct TextNativeResource *row;
        struct Gen3ResourceView view;
        uint32_t rowIndex;

        if (!IsTextEntry(entry))
        {
            /* A text-typed entry outside the family contract is an
             * unexpected-composition failure, never a silent skip; other
             * families' types are simply not this seam's business. */
            if (entry->type == GEN3_RESOURCE_TYPE_TEXT)
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            GEN3_RESOURCE_TYPE_TEXT, entry->type,
                            1u, entry->schema,
                            (uint32_t)entry->payloadSize, 0u, NULL);
                result = EMERALD_TEXT_ERR_TABLE_MISMATCH;
                goto done;
            }
            continue;
        }
        row = FindNativeResource(entry->canonicalName);
        if (row == NULL)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_TEXT, entry->type,
                        1u, entry->schema, 0u,
                        (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_TEXT_ERR_TABLE_MISMATCH;
            goto done;
        }
        rowIndex = (uint32_t)(row - kTextNativeResources);
        if (seen[rowIndex] != 0u)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_TEXT, entry->type,
                        1u, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_TEXT_ERR_TABLE_MISMATCH;
            goto done;
        }
        seen[rowIndex] = 1u;

        if (!row->isBundle)
        {
            if (labelCount >= TEXT_NATIVE_LABEL_COUNT)
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            GEN3_RESOURCE_TYPE_TEXT, entry->type,
                            1u, entry->schema, TEXT_NATIVE_LABEL_COUNT,
                            (uint32_t)labelCount + 1u, NULL);
                result = EMERALD_TEXT_ERR_UNEXPECTED_COUNT;
                goto done;
            }
            labelSpans[labelCount++] =
                ((uint64_t)entry->sourceRomOffset << 32)
                | (uint64_t)entry->payloadSize;
        }
        else
        {
            if (bundleCount >= TEXT_NATIVE_BUNDLE_COUNT)
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            GEN3_RESOURCE_TYPE_TEXT, entry->type,
                            1u, entry->schema, TEXT_NATIVE_BUNDLE_COUNT,
                            (uint32_t)bundleCount + 1u, NULL);
                result = EMERALD_TEXT_ERR_UNEXPECTED_COUNT;
                goto done;
            }
            bundleCount++;
        }

        if (entry->payloadSize != row->size || entry->payload == NULL)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_TEXT, entry->type,
                        1u, entry->schema, row->size,
                        (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_TEXT_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (!ResolveSessionView(snapshot, entry->canonicalName,
                                GEN3_RESOURCE_TYPE_TEXT, 1u, &view))
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_TEXT, entry->type,
                        1u, entry->schema,
                        (uint32_t)entry->payloadSize, 0u, NULL);
            result = EMERALD_TEXT_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_TEXT, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_TEXT_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        if (view.payloadSize != entry->payloadSize)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_TEXT, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_TEXT_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (memcmp(view.payload, entry->payload, view.payloadSize) != 0)
        {
            /* The resolver view must be byte-identical to the pack entry:
             * the arenas copy session bytes, never pack-only bytes. */
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_TEXT, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_TEXT_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (!row->isBundle)
        {
            /* Per-label payloads are whole canonical strings; a payload
             * may instead be a row-major concatenation of strings, but
             * only when kind-0 skeleton fills tile it into valid whole
             * strings (gText_123Dot, R13-C #112). */
            if (!ValidEscapedString(view.payload, view.payloadSize)
             && !TextPayloadTiledByFills(entry->canonicalName,
                                         view.payload, view.payloadSize))
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            GEN3_RESOURCE_TYPE_TEXT, entry->type,
                            entry->schema, entry->schema,
                            (uint32_t)view.payloadSize, 0u, NULL);
                result = EMERALD_TEXT_ERR_ESCAPE_GRAMMAR;
                goto done;
            }
        }
    }

    /* Phase 1b: the generated inventory is the expected inventory - every
     * native row must have been seen with its pinned composition, no
     * more, no less. The per-row identity check runs first so a dropped
     * entry is refused with its canonical name (TABLE_MISMATCH); the
     * count pin remains as the composition backstop for any exotic count
     * drift that still passes identity (an extra family entry is refused
     * earlier, in phase 1a, at its own name). */
    for (i = 0; i < TEXT_NATIVE_RESOURCE_COUNT; i++)
    {
        if (seen[i] == 0u)
        {
            NoteFailure(diagnostics, "build", kTextNativeResources[i].name,
                        GEN3_RESOURCE_TYPE_TEXT, GEN3_RESOURCE_TYPE_INVALID,
                        1u, 0u, kTextNativeResources[i].size, 0u, NULL);
            result = EMERALD_TEXT_ERR_TABLE_MISMATCH;
            goto done;
        }
    }
    if (labelCount != TEXT_NATIVE_LABEL_COUNT
     || bundleCount != TEXT_NATIVE_BUNDLE_COUNT)
    {
        char buffer[160];
        snprintf(buffer, sizeof(buffer),
                 "%zu labels + %zu bundles", labelCount, bundleCount);
        NoteFailure(diagnostics, "build", buffer,
                    GEN3_RESOURCE_TYPE_TEXT, GEN3_RESOURCE_TYPE_INVALID,
                    1u, 0u, TEXT_NATIVE_RESOURCE_COUNT,
                    (uint32_t)(labelCount + bundleCount), NULL);
        result = EMERALD_TEXT_ERR_UNEXPECTED_COUNT;
        goto done;
    }

    /* Phase 1c: prove the claimed C-label ROM slices are pairwise
     * disjoint (the generator proved the extraction tiles the qualified
     * ROM; the seam re-derives the disjointness at publish time - a pack
     * whose slices overlap is provably inconsistent with the qualified
     * layout). Bundles are constructed blobs (writer-side sourceRomOffset
     * is 0) and never enter this proof. */
    {
        uint64_t *spans = labelSpans;
        size_t n = labelCount;
        size_t j;
        /* pack in place by (offset, size): offset in the top 32 bits. */
        for (j = 1; j < n; j++)
        {
            uint64_t key = spans[j];
            size_t k = j;
            while (k > 0u && spans[k - 1u] > key)
            {
                spans[k] = spans[k - 1u];
                k--;
            }
            spans[k] = key;
        }
        for (j = 0u; j + 1u < n; j++)
        {
            uint64_t aEnd = (spans[j] >> 32) + (uint32_t)spans[j];
            if (aEnd > (spans[j + 1u] >> 32))
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer),
                         "slice @0x%llx +%u overlaps @0x%llx",
                         (unsigned long long)(spans[j] >> 32),
                         (uint32_t)spans[j],
                         (unsigned long long)(spans[j + 1u] >> 32));
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            (uint32_t)aEnd,
                            (uint32_t)(spans[j + 1u] >> 32), NULL);
                result = EMERALD_TEXT_ERR_OVERLAPPING_SLICE;
                goto done;
            }
        }
    }

    /* Phase 1d: every bundle blob must be tiled by its index entries
     * (header count/dataOff, offsets array, back-to-back payloads, no
     * trailing bytes) and every label span must revalidate the escape
     * grammar. The bundle index is the blob's manifest: they must agree
     * exactly. */
    {
        uint32_t blobIndex;
        for (blobIndex = 0u; blobIndex < TEXT_BUNDLE_COUNT; blobIndex++)
        {
            const struct TextNativeResource *row =
                FindNativeResource(kTextBundleIds[blobIndex]);
            const struct Gen3ResourcePackEntry *entry;
            struct Gen3ResourceView view;
            const uint8_t *blob;
            uint32_t count;
            uint32_t dataOff;
            uint32_t prevEnd;
            size_t seenEntries = 0u;
            size_t offsetPos = 0u;
            size_t j;
            bool started = false;

            if (row == NULL || !row->isBundle)
            {
                NoteFailure(diagnostics, "build", kTextBundleIds[blobIndex],
                            GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
                result = EMERALD_TEXT_ERR_TABLE_MISMATCH;
                goto done;
            }
            entry = Gen3ResourcePack_FindByCanonicalName(pack,
                                                         kTextBundleIds[blobIndex]);
            if (entry == NULL || entry->payloadSize < 8u
             || !ResolveSessionView(snapshot, kTextBundleIds[blobIndex],
                                    GEN3_RESOURCE_TYPE_TEXT, 1u, &view)
             || view.payloadSize != entry->payloadSize)
            {
                NoteFailure(diagnostics, "build", kTextBundleIds[blobIndex],
                            GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            (uint32_t)(entry != NULL ? entry->payloadSize : 0u),
                            0u, NULL);
                result = EMERALD_TEXT_ERR_BUNDLE_LAYOUT;
                goto done;
            }
            blob = view.payload;
            count = 0u;
            dataOff = 0u;
            memcpy(&count, blob, sizeof(count));
            memcpy(&dataOff, blob + 4u, sizeof(dataOff));
            prevEnd = 0u;
            for (j = 0u; j < TEXT_BUNDLE_ENTRY_COUNT; j++)
            {
                const struct TextBundleEntry *labelEntry =
                    &kTextBundleIndex[j];
                uint32_t offset;
                if (labelEntry->bundleIndex != blobIndex)
                    continue;
                seenEntries++;
                if (!started)
                {
                    /* First row: the blob header must declare the same
                     * count/dataOff the index implies, and the first
                     * payload must sit right after the offsets array. */
                    if (count != 0u && dataOff == 8u + 4u * count
                     && labelEntry->blobOffset == 8u + 4u * count)
                    {
                        prevEnd = labelEntry->blobOffset;
                        started = true;
                    }
                }
                if (!started)
                {
                    NoteFailure(diagnostics, "build", labelEntry->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                (uint32_t)entry->payloadSize,
                                labelEntry->blobOffset, NULL);
                    result = EMERALD_TEXT_ERR_BUNDLE_LAYOUT;
                    goto done;
                }
                if (offsetPos >= count)
                {
                    /* More index rows than the blob header declares. */
                    NoteFailure(diagnostics, "build", labelEntry->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                count, (uint32_t)offsetPos, NULL);
                    result = EMERALD_TEXT_ERR_BUNDLE_LAYOUT;
                    goto done;
                }
                {
                    /* The blob's own offsets array must agree with the
                     * index row for row (both in canonical-name order):
                     * the blob is the byte truth, the index its map. */
                    uint32_t blobOffset = 0u;
                    memcpy(&blobOffset, blob + 8u + 4u * offsetPos,
                           sizeof(blobOffset));
                    if (blobOffset != labelEntry->blobOffset)
                    {
                        NoteFailure(diagnostics, "build", labelEntry->name,
                                    GEN3_RESOURCE_TYPE_TEXT,
                                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                    labelEntry->blobOffset, blobOffset, NULL);
                        result = EMERALD_TEXT_ERR_BUNDLE_LAYOUT;
                        goto done;
                    }
                    offsetPos++;
                }
                if (labelEntry->blobOffset != prevEnd
                 || labelEntry->size == 0u
                 || labelEntry->blobOffset > entry->payloadSize
                 || labelEntry->size > entry->payloadSize
                     - labelEntry->blobOffset)
                {
                    NoteFailure(diagnostics, "build", labelEntry->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                prevEnd, labelEntry->blobOffset, NULL);
                    result = EMERALD_TEXT_ERR_BUNDLE_LAYOUT;
                    goto done;
                }
                offset = labelEntry->blobOffset;
                if (!ValidEscapedString(blob + offset, labelEntry->size))
                {
                    NoteFailure(diagnostics, "build", labelEntry->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                labelEntry->size, 0u, NULL);
                    result = EMERALD_TEXT_ERR_ESCAPE_GRAMMAR;
                    goto done;
                }
                prevEnd = labelEntry->blobOffset + labelEntry->size;
            }
            if (!started || seenEntries != count || offsetPos != count
             || prevEnd != entry->payloadSize)
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer),
                         "%s: %zu index rows vs %u blob count, offsets %zu, "
                         "end %u/%zu",
                         kTextBundleIds[blobIndex], seenEntries, count,
                         offsetPos, prevEnd, entry->payloadSize);
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            (uint32_t)entry->payloadSize, prevEnd, NULL);
                result = EMERALD_TEXT_ERR_BUNDLE_LAYOUT;
                goto done;
            }
        }
    }

    /* Phase 1e: the 16 per-arena summaries must re-derive from the
     * inventory exactly (label counts + byte totals; the R13-C pins sum
     * to 12,777 labels / 903,151 B). */
    {
        uint32_t counts[TEXT_ARENA_COUNT];
        uint32_t bytes[TEXT_ARENA_COUNT];
        uint32_t a;
        size_t totalCount = 0u;
        size_t totalBytes = 0u;
        for (a = 0u; a < TEXT_ARENA_COUNT; a++)
        {
            counts[a] = 0u;
            bytes[a] = 0u;
        }
        for (i = 0u; i < TEXT_NATIVE_RESOURCE_COUNT; i++)
        {
            const struct TextNativeResource *row = &kTextNativeResources[i];
            if (!row->isBundle)
            {
                counts[row->arenaIndex]++;
                bytes[row->arenaIndex] += row->size;
            }
        }
        for (i = 0u; i < TEXT_BUNDLE_ENTRY_COUNT; i++)
        {
            const struct TextBundleEntry *row = &kTextBundleIndex[i];
            counts[row->arenaIndex]++;
            bytes[row->arenaIndex] += row->size;
        }
        for (a = 0u; a < TEXT_ARENA_COUNT; a++)
        {
            if (counts[a] != kTextArenaSummaries[a].labelCount
             || bytes[a] != kTextArenaSummaries[a].byteTotal)
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer),
                         "arena '%s': %u labels, %u bytes",
                         kTextArenaSummaries[a].key, counts[a], bytes[a]);
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            kTextArenaSummaries[a].byteTotal, bytes[a], NULL);
                result = EMERALD_TEXT_ERR_ARENA_MISMATCH;
                goto done;
            }
            totalCount += counts[a];
            totalBytes += bytes[a];
        }
        if (totalCount != EMERALD_TEXT_LABEL_COUNT
         || totalBytes != EMERALD_TEXT_TOTAL_BYTES)
        {
            NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                        EMERALD_TEXT_TOTAL_BYTES, (uint32_t)totalBytes, NULL);
            result = EMERALD_TEXT_ERR_ARENA_MISMATCH;
            goto done;
        }
    }

    /* ---- Phase 2: one allocation, all payload copies, all fills
     * resolved BEFORE anything is published. ---- */
    {
        size_t cursor = 0u;
        size_t recordsTotal = 0u;
        size_t a;
        for (a = 0u; a < TEXT_ARENA_COUNT; a++)
            recordsTotal += kTextArenaSummaries[a].labelCount;

        if (recordsTotal != EMERALD_TEXT_LABEL_COUNT)
        {
            NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                        EMERALD_TEXT_LABEL_COUNT, (uint32_t)recordsTotal, NULL);
            result = EMERALD_TEXT_ERR_UNEXPECTED_COUNT;
            goto done;
        }
        {
            size_t allocBytes;
            size_t fillsBytes =
                (size_t)(TEXT_SLOT_BINDING_COUNT + TEXT_SKELETON_FILL_COUNT)
                * sizeof(struct AppliedFill);
            size_t fillsOffset;
            size_t recordsOffset[TEXT_ARENA_COUNT];
            size_t payloadOffset[TEXT_ARENA_COUNT];

            /* Layout first, allocation second. The earlier estimate
             * (per-arena aligned records + payload) always undercounted
             * the true requirement: records/payload offsets are aligned
             * from a running cursor, the inter-arena padding accumulates,
             * and the fills zone is laid out bytes-relative but was
             * guarded against the block-relative allocation size - off
             * by exactly sizeof(struct EmeraldTextArena). The zone then
             * ran one arena header past the block end (observed: ASAN
             * heap-buffer-overflow at the first applied-fill store past
             * the zone, 0 bytes after the allocation). Sizing from the
             * exact cursor makes the zone end at the block end by
             * construction; the guards stay as the fail-closed check. */
            if (recordsTotal > SIZE_MAX / sizeof(struct TextRecord)
             || sizeof(struct EmeraldTextArena) > SIZE_MAX - fillsBytes)
            {
                NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
                result = EMERALD_TEXT_ERR_OUT_OF_MEMORY;
                goto done;
            }
            cursor = sizeof(struct EmeraldTextArena);
            for (a = 0u; a < TEXT_ARENA_COUNT; a++)
            {
                size_t recordsBytes =
                    kTextArenaSummaries[a].labelCount * sizeof(struct TextRecord);
                recordsOffset[a] = (cursor + 15u) & ~(size_t)15u;
                payloadOffset[a] = (recordsOffset[a] + recordsBytes + 15u)
                    & ~(size_t)15u;
                cursor = payloadOffset[a] + kTextArenaSummaries[a].byteTotal;
                if (cursor < payloadOffset[a])
                {
                    NoteFailure(diagnostics, "build", NULL,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
                    result = EMERALD_TEXT_ERR_OUT_OF_MEMORY;
                    goto done;
                }
            }
            fillsOffset = (cursor + 15u) & ~(size_t)15u;
            if (fillsOffset > SIZE_MAX - fillsBytes
             || sizeof(struct EmeraldTextArena) > SIZE_MAX - fillsOffset
             || sizeof(struct EmeraldTextArena) + fillsOffset
                    > SIZE_MAX - fillsBytes)
            {
                NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
                result = EMERALD_TEXT_ERR_OUT_OF_MEMORY;
                goto done;
            }
            allocBytes = sizeof(struct EmeraldTextArena)
                + fillsOffset + fillsBytes;
            arena = (struct EmeraldTextArena *)malloc(allocBytes);
            if (arena == NULL)
            {
                NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
                result = EMERALD_TEXT_ERR_OUT_OF_MEMORY;
                goto done;
            }
            memset(arena, 0, allocBytes);
            arena->appliedFillsOffset = fillsOffset;
            for (a = 0u; a < TEXT_ARENA_COUNT; a++)
            {
                struct TextSubArena *sub = &arena->subArenas[a];
                sub->labelCount = kTextArenaSummaries[a].labelCount;
                sub->payloadSize = kTextArenaSummaries[a].byteTotal;
                sub->recordsOffset = (uint32_t)recordsOffset[a];
                sub->payloadOffset = (uint32_t)payloadOffset[a];
            }
            applied = (struct AppliedFill *)(void *)(arena->bytes
                + arena->appliedFillsOffset);
        }
    }

    /* Fill the record tables + payload zones. C labels resolve through
     * the snapshot (fail-closed re-resolution); bundle labels copy from
     * their validated blob (re-resolved, never the pack-only bytes).
     * Records are built in inventory order, then sorted by (name,
     * isBundleLocal) and payload offsets are assigned IN THAT SORTED
     * ORDER: the deterministic sorted-name packing of the arena (plan
     * §7/§9) - the zone layout never depends on discovery order. */
    {
        struct TextBuildRecord *build = NULL;
        size_t a;

        build = (struct TextBuildRecord *)malloc(
            EMERALD_TEXT_LABEL_COUNT * sizeof(*build));
        if (build == NULL)
        {
            NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
            result = EMERALD_TEXT_ERR_OUT_OF_MEMORY;
            goto done;
        }
        for (a = 0u; a < TEXT_ARENA_COUNT; a++)
        {
            struct TextSubArena *sub = &arena->subArenas[a];
            struct TextRecord *records = (struct TextRecord *)(void *)
                (arena->bytes + sub->recordsOffset);
            size_t r = 0u;
            size_t pos = 0u;
            size_t i2;

            for (i2 = 0u; i2 < TEXT_NATIVE_RESOURCE_COUNT; i2++)
            {
                const struct TextNativeResource *row = &kTextNativeResources[i2];
                struct Gen3ResourceView view;
                const char *dash;

                if (row->isBundle || row->arenaIndex != a)
                    continue;
                dash = DashNameOf(row->name);
                if (dash == NULL
                 || !ResolveSessionView(snapshot, row->name,
                                        GEN3_RESOURCE_TYPE_TEXT, 1u, &view)
                 || view.payloadSize != row->size)
                {
                    NoteFailure(diagnostics, "build", row->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                row->size, 0u, NULL);
                    result = EMERALD_TEXT_ERR_RESOLVE_FAILED;
                    goto done;
                }
                if (r >= sub->labelCount)
                {
                    NoteFailure(diagnostics, "build", row->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                sub->labelCount, (uint32_t)r + 1u, NULL);
                    result = EMERALD_TEXT_ERR_ARENA_MISMATCH;
                    goto done;
                }
                snprintf(build[r].record.name,
                         sizeof(build[r].record.name), "%s", dash);
                build[r].record.size = row->size;
                build[r].record.isBundleLocal = 0u;
                build[r].source = view.payload;
                r++;
            }
            for (i2 = 0u; i2 < TEXT_BUNDLE_ENTRY_COUNT; i2++)
            {
                const struct TextBundleEntry *row = &kTextBundleIndex[i2];
                const struct TextNativeResource *bundleRow;
                struct Gen3ResourceView view;

                if (row->arenaIndex != a)
                    continue;
                bundleRow = FindNativeResource(kTextBundleIds[row->bundleIndex]);
                if (bundleRow == NULL || !bundleRow->isBundle
                 || !ResolveSessionView(snapshot, kTextBundleIds[row->bundleIndex],
                                        GEN3_RESOURCE_TYPE_TEXT, 1u, &view)
                 || view.payloadSize != bundleRow->size
                 || row->blobOffset + row->size > view.payloadSize)
                {
                    NoteFailure(diagnostics, "build", row->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                row->size, 0u, NULL);
                    result = EMERALD_TEXT_ERR_RESOLVE_FAILED;
                    goto done;
                }
                if (r >= sub->labelCount)
                {
                    NoteFailure(diagnostics, "build", row->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                sub->labelCount, (uint32_t)r + 1u, NULL);
                    result = EMERALD_TEXT_ERR_ARENA_MISMATCH;
                    goto done;
                }
                snprintf(build[r].record.name,
                         sizeof(build[r].record.name), "%s", row->name);
                build[r].record.size = row->size;
                build[r].record.isBundleLocal = 1u;
                build[r].source = view.payload + row->blobOffset;
                r++;
            }
            if (r != sub->labelCount)
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer),
                         "arena '%s': %zu records built, %u pinned",
                         kTextArenaSummaries[a].key, r, sub->labelCount);
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            sub->labelCount, (uint32_t)r, NULL);
                result = EMERALD_TEXT_ERR_ARENA_MISMATCH;
                goto done;
            }
            qsort(build, sub->labelCount, sizeof(*build),
                  CompareBuildRecords);
            for (r = 0u; r < sub->labelCount; r++)
            {
                const struct TextBuildRecord *src = &build[r];
                if (pos + src->record.size > sub->payloadSize)
                {
                    NoteFailure(diagnostics, "build", src->record.name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                sub->payloadSize,
                                (uint32_t)(pos + src->record.size), NULL);
                    result = EMERALD_TEXT_ERR_ARENA_MISMATCH;
                    goto done;
                }
                records[r] = src->record;
                records[r].payloadOffset = (uint32_t)pos;
                memcpy(arena->bytes + sub->payloadOffset + pos,
                       src->source, src->record.size);
                pos += src->record.size;
            }
            if (pos != sub->payloadSize)
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer),
                         "arena '%s': %zu payload bytes, %u pinned",
                         kTextArenaSummaries[a].key, pos, sub->payloadSize);
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_TEXT,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            sub->payloadSize, (uint32_t)pos, NULL);
                result = EMERALD_TEXT_ERR_ARENA_MISMATCH;
                goto done;
            }
        }
        free(build);
        build = NULL;
    }

    /* Resolve every slot + skeleton fill into the staged rollback list.
     * Everything resolves against the freshly built arena; any failure
     * frees the build and leaves the old arena live. */
    {
        size_t cap = (size_t)TEXT_SLOT_BINDING_COUNT + TEXT_SKELETON_FILL_COUNT;
        size_t t;

        for (t = 0u; t < TEXT_SLOT_BINDING_COUNT; t++)
        {
            const struct TextSlotBinding *binding = &kTextSlotBindings[t];
            const uint8_t *bytes;
            size_t size;
            if (binding->slot == NULL
             || !ResolveId(arena, binding->name, &bytes, &size))
            {
                NoteFailure(diagnostics, "build", binding->name,
                            GEN3_RESOURCE_TYPE_TEXT, GEN3_RESOURCE_TYPE_INVALID,
                            1u, 0u, 0u, 0u, NULL);
                result = EMERALD_TEXT_ERR_FILL_RESOLVE;
                goto done;
            }
            applied[appliedCount].target = (const void **)binding->slot;
            applied[appliedCount].value = bytes;
            appliedCount++;
        }
        for (t = 0u; t < TEXT_SKELETON_TABLE_COUNT; t++)
        {
            const struct TextSkeletonTableInfo *table = &kTextSkeletonTables[t];
            uint32_t f;
            for (f = 0u; f < table->fillCount; f++)
            {
                const struct TextSkeletonFill *fill = &table->fills[f];
                const uint8_t *bytes;
                size_t size;
                if (fill->target == NULL)
                {
                    NoteFailure(diagnostics, "build", table->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
                    result = EMERALD_TEXT_ERR_FILL_RESOLVE;
                    goto done;
                }
                if (fill->kind == 0u)
                {
                    if (!ResolveId(arena, fill->name, &bytes, &size))
                    {
                        NoteFailure(diagnostics, "build", fill->name,
                                    GEN3_RESOURCE_TYPE_TEXT,
                                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                    0u, 0u, NULL);
                        result = EMERALD_TEXT_ERR_FILL_RESOLVE;
                        goto done;
                    }
                    /* Kind-0 fills may carry a byte offset into the
                     * label (gText_123Dot rows: fill->row = 0/3/6).
                     * The offset must land inside the record, and any
                     * interior offset (row > 0) additionally requires
                     * the phase-1a tiling proof, so a fill can never
                     * point into a different label (fail-closed). */
                    if (fill->row >= size
                     || (fill->row != 0u
                         && !TextPayloadTiledByFills(fill->name, bytes,
                                                     size)))
                    {
                        NoteFailure(diagnostics, "build", fill->name,
                                    GEN3_RESOURCE_TYPE_TEXT,
                                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                    (uint32_t)size, fill->row, NULL);
                        result = EMERALD_TEXT_ERR_FILL_RESOLVE;
                        goto done;
                    }
                    bytes += fill->row;
                }
                else if (fill->kind == 1u)
                {
                    if (!ResolveLabel(arena, fill->name, &bytes, &size))
                    {
                        NoteFailure(diagnostics, "build", fill->name,
                                    GEN3_RESOURCE_TYPE_TEXT,
                                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                    0u, 0u, NULL);
                        result = EMERALD_TEXT_ERR_FILL_RESOLVE;
                        goto done;
                    }
                }
                else if (fill->kind == 2u)
                {
                    /* Sub-table row: the compiler-owned address of the
                     * target table's row (value == &Target[row]); the
                     * table must exist and the row must be in range. */
                    const struct TextSkeletonTableInfo *targetTable = NULL;
                    uint32_t u;
                    for (u = 0u; u < TEXT_SKELETON_TABLE_COUNT; u++)
                    {
                        if (strcmp(kTextSkeletonTables[u].name, fill->name) == 0)
                        {
                            targetTable = &kTextSkeletonTables[u];
                            break;
                        }
                    }
                    if (targetTable == NULL || fill->row >= targetTable->rowCount
                     || fill->value == NULL)
                    {
                        NoteFailure(diagnostics, "build", fill->name,
                                    GEN3_RESOURCE_TYPE_TEXT,
                                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                                    targetTable != NULL ? targetTable->rowCount
                                                        : 0u,
                                    fill->row, NULL);
                        result = EMERALD_TEXT_ERR_FILL_RESOLVE;
                        goto done;
                    }
                    bytes = (const uint8_t *)fill->value;
                }
                else
                {
                    NoteFailure(diagnostics, "build", fill->name,
                                GEN3_RESOURCE_TYPE_TEXT,
                                GEN3_RESOURCE_TYPE_INVALID, 1u, fill->kind,
                                0u, 0u, NULL);
                    result = EMERALD_TEXT_ERR_FILL_RESOLVE;
                    goto done;
                }
                applied[appliedCount].target = fill->target;
                applied[appliedCount].value = bytes;
                appliedCount++;
            }
        }
        if (appliedCount != cap)
        {
            NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_TEXT,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                        (uint32_t)cap, (uint32_t)appliedCount, NULL);
            result = EMERALD_TEXT_ERR_FILL_RESOLVE;
            goto done;
        }
    }

    /* ---- Phase 3: publish atomically, then apply the infallible
     * pointer stores. A previously published arena survives until the
     * new session fully validated AND built. ---- */
    UnregisterArenaRanges();   /* the old arena's spans leave the index */
    arena->publishedCount = EMERALD_TEXT_LABEL_COUNT;
    arena->totalPayloadBytes = EMERALD_TEXT_TOTAL_BYTES;
    arena->appliedFillCount = appliedCount;
    if (sArena != NULL)
    {
        free(sArena);
        sArena = NULL;
    }
    sArena = arena;
    arena = NULL;
    /* The new arena's spans enter the index. A registration conflict
     * with a live index is a publish failure: the session would
     * otherwise run with unsavable in-arena text pointers (currentChar
     * fails closed at save). Offline links (NULL index) skip. */
    if (!RegisterArenaRanges())
    {
        NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_TEXT,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_TEXT_ERR_RANGE_REGISTRATION;
        goto done;
    }
    for (i = 0u; i < appliedCount; i++)
        *applied[i].target = applied[i].value;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_TEXT,
                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                EMERALD_TEXT_TOTAL_BYTES, 0u, NULL);
    result = EMERALD_TEXT_OK;

done:
    if (build != NULL)
        free(build);
    if (seen != NULL)
        free(seen);
    if (labelSpans != NULL)
        free(labelSpans);
    if (arena != NULL)
        free(arena);
    return result;
}

/* ---- State-v5 arena-range registration (R13-C §13-15). ---- */

static bool sTextRangesInIndex;   /* our 16 spans are registered now */

/* Identity-based unregister: each span is removed wherever it sits in
 * the sorted index. A block-mark scheme is unsafe here - another
 * seam's ranges can sort before our block (the D1 font ranges landed
 * below the text arena in the runtime loader harness) and shift it, and
 * the old fail-closed match then orphaned our spans: the arena freed,
 * the ranges stayed, and the next session's allocation collided with
 * its own freed arena. Bases are unique (InsertRange rejects overlap
 * and the arena allocation is exclusive to us), so removing by base
 * can only remove our own spans. */
static void UnregisterArenaRanges(void)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    size_t i;

    if (index == NULL || !sTextRangesInIndex)
        return;
    for (i = 0u; i < TEXT_ARENA_COUNT; i++)
    {
        uintptr_t base =
            (uintptr_t)(sArena->bytes + sArena->subArenas[i].payloadOffset);
        size_t j;
        for (j = 0u; j < index->rangeCount; j++)
        {
            if (index->ranges[j].base != base)
                continue;
            memmove(&index->ranges[j], &index->ranges[j + 1u],
                    (index->rangeCount - j - 1u) * sizeof(index->ranges[0]));
            index->rangeCount--;
            break;
        }
    }
    sTextRangesInIndex = false;
}

/* Register one span per family arena (payload zone: base + byteTotal,
 * exactly the range GetArenaByKey reports) under
 * "emerald:text/arena/<key>" - a seam-registered bundle identity that
 * round-trips through the range index without a resolver lookup (plan
 * §9). Returns true when the spans are registered OR the shared index
 * is absent (offline link: skipped, the walker fails closed on arena
 * pointers); returns false only on a real registration conflict. */
static bool RegisterArenaRanges(void)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    size_t mark, i;

    if (index == NULL || sArena == NULL)
        return true;
    if (sTextRangesInIndex)
        UnregisterArenaRanges();
    mark = 0u;
    while (mark < index->rangeCount
           && index->ranges[mark].base
              < (uintptr_t)(sArena->bytes
                            + sArena->subArenas[0].payloadOffset))
        mark++;
    for (i = 0u; i < TEXT_ARENA_COUNT; i++)
    {
        char name[96];
        snprintf(name, sizeof(name), "emerald:text/arena/%s",
                 kTextArenaSummaries[i].key);
        if (!EmeraldResourceRangeIndex_RegisterSpan(
                index,
                (uintptr_t)(sArena->bytes
                            + sArena->subArenas[i].payloadOffset),
                sArena->subArenas[i].payloadSize,
                name, GEN3_RESOURCE_TYPE_TEXT, 1u,
                EMERALD_RESOURCE_ROLE_COMPAT_OBJECT))
            goto fail;
    }
    sTextRangesInIndex = true;
    return true;

fail:
    /* Restore: our partial block sits at [mark, mark+i) (the spans
     * sort contiguously - one allocation); shift the entries that were
     * pushed right back over it. */
    if (i > 0u)
    {
        memmove(&index->ranges[mark], &index->ranges[mark + i],
                (index->rangeCount - mark - i) * sizeof(index->ranges[0]));
        index->rangeCount -= i;
    }
    sTextRangesInIndex = false;
    return false;
}

void EmeraldTextCompat_ClearMigratedEntries(void)
{
    /* The arena's spans leave the index before the arena is freed. */
    UnregisterArenaRanges();
    if (sArena != NULL)
    {
        struct AppliedFill *applied = (struct AppliedFill *)(void *)
            (sArena->bytes + sArena->appliedFillsOffset);
        size_t i;
        for (i = 0u; i < sArena->appliedFillCount; i++)
            *applied[i].target = NULL;
        free(sArena);
        sArena = NULL;
    }
}

void EmeraldTextCompat_Shutdown(void)
{
    EmeraldTextCompat_ClearMigratedEntries();
}

static bool GetArenaRange(const struct EmeraldTextArena *arena,
                          size_t arenaIndex,
                          const uint8_t **outBase, size_t *outSize)
{
    const struct TextSubArena *sub;
    if (arena == NULL || arenaIndex >= TEXT_ARENA_COUNT)
        return false;
    sub = &arena->subArenas[arenaIndex];
    if (outBase != NULL)
        *outBase = arena->bytes + sub->payloadOffset;
    if (outSize != NULL)
        *outSize = sub->payloadSize;
    return true;
}

bool EmeraldTextCompat_GetArenaByKey(const char *key,
                                     const uint8_t **outBase,
                                     size_t *outSize)
{
    size_t i;
    if (key == NULL)
        return false;
    if (outBase != NULL)
        *outBase = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    for (i = 0u; i < TEXT_ARENA_COUNT; i++)
    {
        if (strcmp(kTextArenaSummaries[i].key, key) == 0)
            return GetArenaRange(sArena, i, outBase, outSize);
    }
    return false;
}

size_t EmeraldTextCompat_GetArenaCount(void)
{
    return TEXT_ARENA_COUNT;
}

size_t EmeraldTextCompat_GetPublishedCount(void)
{
    return sArena != NULL ? sArena->publishedCount : 0u;
}

bool EmeraldTextCompat_GetResourceBytes(const char *resourceId,
                                        const uint8_t **outBytes,
                                        size_t *outSize)
{
    if (resourceId == NULL)
        return false;
    if (outBytes != NULL)
        *outBytes = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    if (sArena == NULL)
        return false;
    return ResolveId(sArena, resourceId, outBytes, outSize);
}

bool EmeraldTextCompat_GetLabelBytes(const char *canonicalName,
                                     const uint8_t **outBytes,
                                     size_t *outSize)
{
    if (canonicalName == NULL)
        return false;
    if (outBytes != NULL)
        *outBytes = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    if (sArena == NULL)
        return false;
    return ResolveLabel(sArena, canonicalName, outBytes, outSize);
}

bool EmeraldTextCompat_ContainsPointer(uintptr_t address)
{
    size_t i;
    if (sArena == NULL)
        return false;
    for (i = 0u; i < TEXT_ARENA_COUNT; i++)
    {
        const uint8_t *base;
        size_t size;
        GetArenaRange(sArena, i, &base, &size);
        if ((uintptr_t)base <= address && address < (uintptr_t)base + size)
            return true;
    }
    return false;
}

bool EmeraldTextCompat_GetArenaForPointer(uintptr_t address,
                                          size_t *outArenaIndex,
                                          size_t *outOffset)
{
    size_t i;
    if (sArena == NULL)
        return false;
    if (outArenaIndex != NULL)
        *outArenaIndex = TEXT_ARENA_COUNT;
    if (outOffset != NULL)
        *outOffset = 0u;
    for (i = 0u; i < TEXT_ARENA_COUNT; i++)
    {
        const uint8_t *base;
        size_t size;
        GetArenaRange(sArena, i, &base, &size);
        if ((uintptr_t)base <= address && address < (uintptr_t)base + size)
        {
            if (outArenaIndex != NULL)
                *outArenaIndex = i;
            if (outOffset != NULL)
                *outOffset = (size_t)(address - (uintptr_t)base);
            return true;
        }
    }
    return false;
}

bool EmeraldTextCompat_GetLabelAtOffset(uint32_t arenaIndex, size_t offset,
                                        const char **outName,
                                        size_t *outStart, size_t *outSize)
{
    const struct TextSubArena *sub;
    const struct TextRecord *records;
    size_t i;
    if (sArena == NULL || arenaIndex >= TEXT_ARENA_COUNT)
        return false;
    sub = &sArena->subArenas[arenaIndex];
    if (offset >= sub->payloadSize)
        return false;
    records = (const struct TextRecord *)(void *)
        (sArena->bytes + sub->recordsOffset);
    for (i = 0u; i < sub->labelCount; i++)
    {
        const struct TextRecord *record = &records[i];
        if ((size_t)record->payloadOffset <= offset
         && offset < (size_t)record->payloadOffset + record->size)
        {
            if (outName != NULL)
                *outName = record->name;
            if (outStart != NULL)
                *outStart = record->payloadOffset;
            if (outSize != NULL)
                *outSize = record->size;
            return true;
        }
    }
    return false;
}
