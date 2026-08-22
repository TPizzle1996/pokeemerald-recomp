/* R13-G3: Emerald field-script shadow staging + typed resolver seam.
 * See emerald_script_compat.h for the contract. Implementation notes:
 *
 * - The shadow generation is the ONLY mutable state: arena, span
 *   index, source index, staged gStdScripts and the staged F rebind
 *   plan. Every query resolves against the current generation; a
 *   restage replaces it atomically after full validation.
 * - All indexes over the generated table are sorted at emission time
 *   (modules by id, exports/boundaries/relocs per module by payload
 *   offset, routing rows by GBA source, dynamic targets by
 *   (gba, class)); the seam binary-searches them - O(log n) on the
 *   hot paths (source operand, encoded target, reverse containment),
 *   per-module O(log m) on boundary/export queries.
 * - Live pointers for STAGED_ARENA targets are computed on demand
 *   from the generation's span bases; sibling-seam pointers come from
 *   the published text/leaf arenas; compiled-bridge and RAM pointers
 *   come from the generated table's extern symbols. No live pointer
 *   is ever stored inside the arena or any index row, and nothing is
 *   registered with the State-v5 range index.
 */

#include "emerald/resources/emerald_script_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/sha256.h"
#include "emerald/resources/emerald_leaf_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_script_state.h"
#include "emerald/resources/emerald_text_compat.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "emerald/resources/script_native.generated.h"
#include "emerald/resources/text_bundle_index.generated.h"

/* R13-G6: the movement bridges are no longer compiled-symbol externs;
 * the seam resolves them to the bytes embedded in each bridge row
 * (B-owned, plan sec 7.4 - the movement-isolation exception). */
extern uint8_t gStringVar4[1000];

/* ------------------------------------------------------------------ */
/* Shadow generation state                                             */

struct EmeraldScriptCompatModuleSpan
{
    uintptr_t base;
    uint32_t payloadSize;
    uint32_t routingBytes;
    uint32_t moduleIndex;
};

struct EmeraldScriptCompatSourceRow
{
    uintptr_t operandAddress;
    uint32_t relocIndex;     /* EMERALD_SCRIPT_NATIVE_OFFSET_NONE for routing rows */
    uint32_t routingRowIndex; /* index into kScriptRoutingRelocs when routing */
};

struct EmeraldScriptCompatGeneration
{
    uint64_t generationId;
    uint8_t *arena;
    size_t arenaSize;
    struct EmeraldScriptCompatModuleSpan *spans;  /* 523, sorted by base */
    struct EmeraldScriptCompatSourceRow *sources; /* 15,874, sorted by address */
    uint32_t sourceCount;
    struct EmeraldScriptCompatStagedStdScript std[EMERALD_SCRIPT_STD_SCRIPT_COUNT];
    struct EmeraldScriptCompatStagedFBinding *fBindings; /* 3,501 */
};

static struct EmeraldScriptCompatGeneration *sGeneration;
static uint64_t sGenerationCounter;
static uint32_t sParityChecked;
static uint32_t sParityMismatches;

#ifdef EMERALD_SCRIPT_COMPAT_TEST_HOOKS
/* Test-only allocation fault (plan sec 17 GENERATION faults): the
 * stage allocations draw from a call counter; the fault driver fails
 * the Nth allocation so a partial allocation failure (arena + span
 * index OK, the source-index malloc refused) is injectable
 * deterministically. Production builds never define the macro and
 * use malloc directly. */
static size_t sTestAllocFailAt = (size_t)-1;

void EmeraldScriptCompat_TestSetAllocFail(size_t nth)
{
    sTestAllocFailAt = nth;
}

static void *TestMalloc(size_t size)
{
    if (sTestAllocFailAt == 0u)
        return NULL;
    if (sTestAllocFailAt != (size_t)-1)
        sTestAllocFailAt--;
    return malloc(size);
}

#define SC_MALLOC TestMalloc
#else
#define SC_MALLOC malloc
#endif

static const char kScriptPrefix[] = "emerald:script/";

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */

static void NoteFailure(struct EmeraldScriptCompatDiagnostics *diag,
                        const char *stage, const char *name,
                        uint32_t expectedType, uint32_t actualType,
                        uint32_t expectedSchema, uint32_t actualSchema,
                        uint32_t expectedSize, uint32_t actualSize,
                        const struct Gen3ResourceView *view)
{
    if (diag == NULL)
        return;
    memset(diag, 0, sizeof(*diag));
    if (stage != NULL)
    {
        strncpy(diag->stage, stage, sizeof(diag->stage) - 1u);
        diag->stage[sizeof(diag->stage) - 1u] = '\0';
    }
    if (name != NULL)
    {
        strncpy(diag->canonicalName, name, sizeof(diag->canonicalName) - 1u);
        diag->canonicalName[sizeof(diag->canonicalName) - 1u] = '\0';
    }
    diag->expectedType[0] = '\0';
    diag->actualType[0] = '\0';
    diag->expectedSchema = expectedSchema;
    diag->actualSchema = actualSchema;
    diag->expectedSize = expectedSize;
    diag->actualSize = actualSize;
    if (view != NULL)
    {
        if (view->winningProviderId != NULL)
        {
            strncpy(diag->winningProviderId, view->winningProviderId,
                    sizeof(diag->winningProviderId) - 1u);
            diag->winningProviderId[sizeof(diag->winningProviderId) - 1u] = '\0';
        }
        if (view->winningProviderVersion != NULL)
        {
            strncpy(diag->winningProviderVersion, view->winningProviderVersion,
                    sizeof(diag->winningProviderVersion) - 1u);
            diag->winningProviderVersion[
                sizeof(diag->winningProviderVersion) - 1u] = '\0';
        }
        diag->winningProviderPrecedence = view->winningProviderPrecedence;
    }
    (void)expectedType;
    (void)actualType;
}

static void NoteFailureSimple(struct EmeraldScriptCompatDiagnostics *diag,
                              const char *stage, const char *name,
                              uint32_t expectedSize, uint32_t actualSize)
{
    NoteFailure(diag, stage, name, 0u, 0u, 0u, 0u, expectedSize, actualSize,
                NULL);
}

static void CopyName(char *dst, size_t dstSize, const char *src)
{
    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dstSize - 1u);
    dst[dstSize - 1u] = '\0';
}

static uint32_t ReadU32LE(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ModuleKeyCompare(const void *key, const void *element)
{
    return strcmp((const char *)key,
                  ((const struct EmeraldScriptNativeModule *)element)->id);
}

static const struct EmeraldScriptNativeModule *FindModule(const char *key)
{
    return bsearch(key, kEmeraldScriptCompatTable.modules,
                   kEmeraldScriptCompatTable.moduleCount,
                   sizeof(struct EmeraldScriptNativeModule), ModuleKeyCompare);
}

static const struct EmeraldScriptNativeExport *FindExport(
    uint32_t moduleIndex, uint32_t payloadOffset)
{
    const struct EmeraldScriptNativeModule *m =
        &kEmeraldScriptCompatTable.modules[moduleIndex];
    const struct EmeraldScriptNativeExport *exports =
        kEmeraldScriptCompatTable.exports;
    size_t low = m->exportFirst;
    size_t high = low + m->exportCount;

    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        if (exports[mid].payloadOffset < payloadOffset)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low < m->exportFirst + m->exportCount
     && exports[low].payloadOffset == payloadOffset
     && exports[low].moduleIndex == moduleIndex)
        return &exports[low];
    return NULL;
}

static const struct EmeraldScriptNativeBoundary *FindBoundary(
    uint32_t moduleIndex, uint32_t payloadOffset)
{
    const struct EmeraldScriptNativeModule *m =
        &kEmeraldScriptCompatTable.modules[moduleIndex];
    const struct EmeraldScriptNativeBoundary *rows =
        kEmeraldScriptCompatTable.boundaries;
    size_t low = m->boundaryFirst;
    size_t high = low + m->boundaryCount;

    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        if (rows[mid].payloadOffset < payloadOffset)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low < m->boundaryFirst + m->boundaryCount
     && rows[low].payloadOffset == payloadOffset)
        return &rows[low];
    return NULL;
}

/* The instruction whose span encloses `payloadOffset`, or NULL when the
 * offset is in an opaque zone. */
static const struct EmeraldScriptNativeBoundary *FindEnclosingInstruction(
    uint32_t moduleIndex, uint32_t payloadOffset)
{
    const struct EmeraldScriptNativeModule *m =
        &kEmeraldScriptCompatTable.modules[moduleIndex];
    const struct EmeraldScriptNativeBoundary *rows =
        kEmeraldScriptCompatTable.boundaries;
    size_t low = m->boundaryFirst;
    size_t high = low + m->boundaryCount;

    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        if (rows[mid].payloadOffset + rows[mid].length <= payloadOffset)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low < m->boundaryFirst + m->boundaryCount
     && rows[low].payloadOffset <= payloadOffset
     && payloadOffset < rows[low].payloadOffset + rows[low].length)
        return &rows[low];
    return NULL;
}

static const struct EmeraldScriptNativeSegment *FindSegment(
    uint32_t moduleIndex, uint32_t payloadOffset)
{
    const struct EmeraldScriptNativeModule *m =
        &kEmeraldScriptCompatTable.modules[moduleIndex];
    const struct EmeraldScriptNativeSegment *rows =
        kEmeraldScriptCompatTable.segments;
    size_t low = m->segmentFirst;
    size_t high = low + m->segmentCount;

    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        if (rows[mid].payloadOffset <= payloadOffset)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low > m->segmentFirst)
    {
        const struct EmeraldScriptNativeSegment *seg = &rows[low - 1u];
        if (payloadOffset < seg->payloadOffset + seg->byteCount)
            return seg;
    }
    return NULL;
}

struct DynamicKey
{
    uint32_t gba;
    uint8_t cls;
};

static int DynamicCompare(const void *a, const void *b)
{
    const struct DynamicKey *ka = (const struct DynamicKey *)a;
    const struct EmeraldScriptNativeDynamicTarget *db =
        (const struct EmeraldScriptNativeDynamicTarget *)b;
    if (ka->gba != db->gbaAddress)
        return ka->gba < db->gbaAddress ? -1 : 1;
    if (ka->cls != db->targetClass)
        return ka->cls < db->targetClass ? -1 : 1;
    return 0;
}

static const struct EmeraldScriptNativeDynamicTarget *FindDynamicTarget(
    uint32_t encodedGba, uint32_t targetClass)
{
    struct DynamicKey key;

    key.gba = encodedGba;
    key.cls = (uint8_t)targetClass;
    return bsearch(&key, kEmeraldScriptCompatTable.dynamicTargets,
                   kEmeraldScriptCompatTable.dynamicTargetCount,
                   sizeof(struct EmeraldScriptNativeDynamicTarget),
                   DynamicCompare);
}

static const struct EmeraldScriptNativeBridge *FindBridge(const char *key)
{
    uint32_t i;

    /* 3 rows: a linear scan is the honest lookup here. */
    for (i = 0u; i < kEmeraldScriptCompatTable.bridgeCount; i++)
    {
        if (strcmp(kEmeraldScriptCompatTable.bridges[i].key, key) == 0)
            return &kEmeraldScriptCompatTable.bridges[i];
    }
    return NULL;
}

static const struct EmeraldScriptNativeMart *FindMart(uint32_t moduleIndex,
                                                      uint32_t payloadOffset)
{
    uint32_t i;

    /* 38 rows. */
    for (i = 0u; i < kEmeraldScriptCompatTable.martCount; i++)
    {
        const struct EmeraldScriptNativeMart *mart =
            &kEmeraldScriptCompatTable.marts[i];
        if (mart->moduleIndex == moduleIndex
         && mart->payloadOffset == payloadOffset)
            return mart;
    }
    return NULL;
}

static const char *PoolString(uint32_t index)
{
    if (index == EMERALD_SCRIPT_NATIVE_OFFSET_NONE)
        return NULL;
    if (index >= kEmeraldScriptCompatTable.poolCount)
        return NULL;
    return kEmeraldScriptCompatTable.pool[index];
}

/* ------------------------------------------------------------------ */
/* Typed target resolution                                             */

static uintptr_t ModuleSpanBase(uint32_t moduleIndex)
{
    if (sGeneration == NULL)
        return 0u;
    /* Spans are sorted by base, not by module index: find by index. */
    uint32_t i;
    for (i = 0u; i < kEmeraldScriptCompatTable.moduleCount; i++)
    {
        if (sGeneration->spans[i].moduleIndex == moduleIndex)
            return sGeneration->spans[i].base;
    }
    return 0u;
}

/* R13-G6 (plan sec 5.3): engine C-site entrypoint resolution. A
 * G-owned script export resolved by canonical symbol name through the
 * sorted export-name index (O(log n) bsearch). The resolved address
 * is the module span base + the export's payload offset - the same
 * formula the relocation index uses - so the result is generation-
 * exact with no cache and no stale-pointer window on republish.
 * Returns 0 on any failure (unknown name, no live generation); the
 * callers treat 0 as a terminal error, so a G resource that vanished
 * from the pack refuses instead of falling back to a compiled
 * symbol. */
uintptr_t EmeraldScriptCompat_GetScriptSymbol(const char *name)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    const struct EmeraldScriptNativeExportName *rows;
    const struct EmeraldScriptNativeExport *exports;
    const struct EmeraldScriptNativeExportName *hit;
    const struct EmeraldScriptNativeExport *exp;
    size_t low;
    size_t high;
    size_t mid;

    if (name == NULL)
        return 0u;
    if (sGeneration == NULL)
        return 0u;
    rows = t->exportNames;
    exports = t->exports;
    low = 0u;
    high = t->exportNameCount;
    while (low < high)
    {
        mid = low + (high - low) / 2u;
        if (strcmp(rows[mid].name, name) < 0)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low >= t->exportNameCount
     || strcmp(rows[low].name, name) != 0)
        return 0u;
    hit = &rows[low];
    if (hit->exportIndex >= t->exportCount)
        return 0u;
    exp = &exports[hit->exportIndex];
    return ModuleSpanBase(exp->moduleIndex) + exp->payloadOffset;
}

/* Fill `out` from a relocation-style target identity (class/kind/key/
 * label/offsets). Live addresses resolve against the current
 * generation and the sibling seams. Returns OK or a refusal status. */
/* R13-G5: the staged routing segment containing GBA address `gba`
 * (map dispatch / conditional table bytes), with the owning module
 * written back. O(log n) over the 581 sorted segments. */
static const struct EmeraldScriptNativeRoutingSegment *FindRoutingSegmentForGba(
    uint32_t gba, const struct EmeraldScriptNativeModule **outModule)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    size_t low = 0u;
    size_t high = t->routingSegmentCount;

    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        const struct EmeraldScriptNativeRoutingSegment *seg =
            &t->routingSegments[mid];
        if (seg->originalGbaStart + seg->byteCount <= gba)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low < t->routingSegmentCount)
    {
        const struct EmeraldScriptNativeRoutingSegment *seg =
            &t->routingSegments[low];
        if (seg->originalGbaStart <= gba
         && gba < seg->originalGbaStart + seg->byteCount)
        {
            if (outModule != NULL && seg->moduleIndex < t->moduleCount)
                *outModule = &t->modules[seg->moduleIndex];
            return seg;
        }
    }
    return NULL;
}

static enum EmeraldScriptCompatStatus ResolveTargetIdentity(
    uint32_t targetClass, uint32_t targetKind,
    const char *resourceKey, const char *label,
    uint32_t regionOffset, uint32_t payloadOffset, uint32_t boundaryKind,
    uint32_t encodedGba,
    struct EmeraldScriptCompatResolvedTarget *out)
{
    const struct EmeraldScriptNativeModule *module = NULL;
    const struct EmeraldScriptNativeExport *exportRow = NULL;

    memset(out, 0, sizeof(*out));
    out->targetClass = targetClass;
    out->targetKind = targetKind;
    out->targetOffset = regionOffset;
    out->targetPayloadOffset = payloadOffset;
    out->boundaryKind = boundaryKind;
    CopyName(out->resourceKey, sizeof(out->resourceKey), resourceKey);
    CopyName(out->label, sizeof(out->label), label);

    if (resourceKey != NULL)
        module = FindModule(resourceKey);
    if (module != NULL && payloadOffset != EMERALD_SCRIPT_NATIVE_OFFSET_NONE)
        exportRow = FindExport(
            (uint32_t)(module - kEmeraldScriptCompatTable.modules),
            payloadOffset);

    switch (targetKind)
    {
    case EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_PAYLOAD:
    {
        const struct EmeraldScriptNativeBoundary *inst;

        if (module == NULL
         || payloadOffset == EMERALD_SCRIPT_NATIVE_OFFSET_NONE
         || payloadOffset >= module->payloadSize || exportRow == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA;
        out->boundaryKind =
            exportRow->boundaryKind == EMERALD_SCRIPT_NATIVE_BOUNDARY_OFFSET_ZERO
                ? EMERALD_SCRIPT_NATIVE_BOUNDARY_OFFSET_ZERO
                : EMERALD_SCRIPT_NATIVE_BOUNDARY_INTERIOR;
        if (sGeneration != NULL)
        {
            out->liveAddress = ModuleSpanBase(
                (uint32_t)(module - kEmeraldScriptCompatTable.modules))
                + payloadOffset;
            inst = FindEnclosingInstruction(
                (uint32_t)(module - kEmeraldScriptCompatTable.modules),
                payloadOffset);
            out->liveSize = inst != NULL ? inst->length : 0u;
        }
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_ROUTING:
    {
        /* R13-G5: a routing-class table start (conditional dispatch)
         * - staged in the owning module's routing suffix. */
        const struct EmeraldScriptNativeRoutingSegment *seg =
            FindRoutingSegmentForGba(encodedGba, &module);
        if (seg == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_ROUTING;
        if (sGeneration != NULL)
            out->liveAddress = ModuleSpanBase(
                (uint32_t)(module - kEmeraldScriptCompatTable.modules))
                + module->payloadSize + seg->spanOffset
                + (encodedGba - seg->originalGbaStart);
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_BRIDGE:
    case EMERALD_SCRIPT_NATIVE_KIND_MOVEMENT_BRIDGE:
    {
        const struct EmeraldScriptNativeBridge *bridge =
            resourceKey != NULL ? FindBridge(resourceKey) : NULL;

        if (bridge == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_COMPILED_BRIDGE;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_INTERIOR;
        /* R13-G6: the bridge rows embed the qualified-ROM movement
         * bytes (12 B total); no compiled bridge symbol exists on
         * native. B-owned until the R13-B movement cutover. */
        out->liveAddress = (uintptr_t)bridge->bytes;
        out->liveSize = bridge->byteCount;
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_TEXT_BUNDLE_MEMBER:
    {
        const struct TextBundleEntry *entry;
        const char *bundleId;
        const uint8_t *blob;
        size_t blobSize;
        size_t low;
        size_t high;

        if (label == NULL || resourceKey == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        /* kTextBundleIndex is sorted by canonical label name. */
        low = 0u;
        high = TEXT_BUNDLE_ENTRY_COUNT;
        while (low < high)
        {
            size_t mid = low + (high - low) / 2u;
            if (strcmp(kTextBundleIndex[mid].name, label) < 0)
                low = mid + 1u;
            else
                high = mid;
        }
        if (low >= TEXT_BUNDLE_ENTRY_COUNT
         || strcmp(kTextBundleIndex[low].name, label) != 0)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        entry = &kTextBundleIndex[low];
        if (entry->bundleIndex >= TEXT_BUNDLE_COUNT)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        bundleId = kTextBundleIds[entry->bundleIndex];
        if (bundleId == NULL || strcmp(bundleId, resourceKey) != 0)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        /* The bundle-index row (name, bundleIndex, blobOffset, size,
         * arenaIndex) is the identity proof; the live bytes come from
         * the label resolution (GetResourceBytes refuses bundle ids by
         * design - ResolveId rejects isBundle rows). */
        if (!EmeraldTextCompat_GetLabelBytes(label, &blob, &blobSize)
         || blob == NULL)
            return EMERALD_SCRIPT_ERR_UNAVAILABLE;
        if (blobSize != entry->size)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_SIBLING_SEAM;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE;
        out->targetPayloadOffset = entry->blobOffset;
        out->liveAddress = (uintptr_t)blob;
        out->liveSize = entry->size;
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_TEXT_LABEL:
    {
        const uint8_t *bytes;
        size_t size;

        if (resourceKey == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        if (!EmeraldTextCompat_GetResourceBytes(resourceKey, &bytes, &size)
         || bytes == NULL)
            return EMERALD_SCRIPT_ERR_UNAVAILABLE;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_SIBLING_SEAM;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE;
        out->liveAddress = (uintptr_t)bytes;
        out->liveSize = (uint32_t)size;
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_MOVEMENT_RESOURCE:
    {
        const uint8_t *bytes;
        size_t size;

        if (resourceKey == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        if (!EmeraldLeafCompat_GetResourceBytes(resourceKey, &bytes, &size)
         || bytes == NULL)
            return EMERALD_SCRIPT_ERR_UNAVAILABLE;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_SIBLING_SEAM;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE;
        out->liveAddress = (uintptr_t)bytes;
        out->liveSize = (uint32_t)size;
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_MART_TABLE:
    {
        const struct EmeraldScriptNativeMart *mart;

        if (module == NULL
         || payloadOffset == EMERALD_SCRIPT_NATIVE_OFFSET_NONE
         || payloadOffset >= module->payloadSize)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        mart = FindMart((uint32_t)(module - kEmeraldScriptCompatTable.modules),
                        payloadOffset);
        if (mart == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE;
        out->liveSize = mart->byteCount;
        if (sGeneration != NULL)
        {
            out->liveAddress = ModuleSpanBase(
                (uint32_t)(module - kEmeraldScriptCompatTable.modules))
                + payloadOffset;
        }
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_DISPATCH:
    {
        /* R13-G5: a map dispatch / conditional table start - staged in
         * the owning module's routing suffix. */
        const struct EmeraldScriptNativeRoutingSegment *seg =
            FindRoutingSegmentForGba(encodedGba, &module);
        if (seg == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_ROUTING;
        if (sGeneration != NULL)
            out->liveAddress = ModuleSpanBase(
                (uint32_t)(module - kEmeraldScriptCompatTable.modules))
                + module->payloadSize + seg->spanOffset
                + (encodedGba - seg->originalGbaStart);
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_BRAILLE:
    {
        /* R13-G6 (plan sec 7.3): the compiled braille text blocks stay
         * C-owned through R13-G (no C catalog record exists for them
         * yet - the identity handoff is R13-H's). The generated
         * kBrailleGbaAddrs (sorted ascending) pairs each encoded GBA
         * target with its live label address in the same assembly unit
         * as braille.inc, so the 26 BRAILLE edges resolve live instead
         * of deferring. */
        size_t low = 0u;
        size_t high = EMERALD_SCRIPT_BRAILLE_COUNT;

        while (low < high)
        {
            size_t mid = low + (high - low) / 2u;
            if (kBrailleGbaAddrs[mid] < encodedGba)
                low = mid + 1u;
            else
                high = mid;
        }
        if (low >= EMERALD_SCRIPT_BRAILLE_COUNT
         || kBrailleGbaAddrs[low] != encodedGba)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        out->disposition = EMERALD_SCRIPT_DISPOSITION_SIBLING_SEAM;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE;
        out->liveAddress = (uintptr_t)kBrailleTextAddresses[low];
        out->liveSize = 0u; /* extent is the next braille block; never needed */
        return EMERALD_SCRIPT_OK;
    }
    case EMERALD_SCRIPT_NATIVE_KIND_RAM_HOST:
        /* Allowlist shape is gated in ValidateTableStructure; here the
         * single approved writable target resolves to its host symbol
         * (permission comes from the allowlist, never from address
         * arithmetic). */
        out->disposition = EMERALD_SCRIPT_DISPOSITION_HOST_RAM;
        out->boundaryKind = EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE;
        out->liveAddress = (uintptr_t)gStringVar4;
        out->liveSize = 1000u;
        return EMERALD_SCRIPT_OK;
    default:
        return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
    }
}

/* Resolve a module relocation row's target (shared by the source-index
 * path and the phase-1/parity walk). */
static enum EmeraldScriptCompatStatus ResolveRelocRowTarget(
    const struct EmeraldScriptNativeReloc *row,
    struct EmeraldScriptCompatResolvedTarget *out)
{
    return ResolveTargetIdentity(
        row->targetClass, row->targetKind,
        PoolString(row->targetKey), PoolString(row->targetLabel),
        row->targetOffset, row->targetPayloadOffset,
        EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE, row->originalEncodedGba, out);
}

static enum EmeraldScriptCompatStatus ResolveRoutingRowTarget(
    const struct EmeraldScriptNativeRoutingReloc *row,
    struct EmeraldScriptCompatResolvedTarget *out)
{
    return ResolveTargetIdentity(
        row->targetClass, row->targetKind,
        PoolString(row->targetKey), PoolString(row->targetLabel),
        row->targetOffset, row->targetPayloadOffset,
        EMERALD_SCRIPT_NATIVE_BOUNDARY_NONE, row->targetGba, out);
}

/* ------------------------------------------------------------------ */
/* Phase 1: structural table gates                                     */

static enum EmeraldScriptCompatStatus ValidateTableStructure(
    struct EmeraldScriptCompatDiagnostics *diag)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    uint32_t i;
    uint32_t module;
    uint32_t arenaCursor = 0u;
    uint32_t payloadTotal = 0u;
    uint32_t roots = 0u;
    uint32_t interior = 0u;

    if (t->moduleCount != EMERALD_SCRIPT_MODULE_COUNT
     || t->segmentCount != EMERALD_SCRIPT_SEGMENT_COUNT
     || t->exportCount != EMERALD_SCRIPT_EXPORT_COUNT
     || t->relocCount != EMERALD_SCRIPT_RELOC_COUNT
     || t->routingRelocCount != EMERALD_SCRIPT_ROUTING_RELOC_COUNT
     || t->stdScriptCount != EMERALD_SCRIPT_STD_SCRIPT_COUNT
     || t->fBindingCount != EMERALD_SCRIPT_F_BINDING_COUNT
     || t->martCount != EMERALD_SCRIPT_MART_COUNT
     || t->ramTargetCount != EMERALD_SCRIPT_RAM_TARGET_COUNT
     || t->ramAllowlistCount != EMERALD_SCRIPT_RAM_ALLOWLIST_COUNT
     || t->bridgeCount != EMERALD_SCRIPT_BRIDGE_COUNT
     || t->pool == NULL || t->poolCount == 0u
     || t->pool[0][0] != '\0')
    {
        NoteFailureSimple(diag, "validate", "script-table", 0u, 0u);
        return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
    }
    if (t->relocCount + t->routingRelocCount
            != EMERALD_SCRIPT_TOTAL_RELOC_COUNT)
    {
        NoteFailureSimple(diag, "validate", "script-table",
                          EMERALD_SCRIPT_TOTAL_RELOC_COUNT,
                          t->relocCount + t->routingRelocCount);
        return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
    }

    /* Modules: sorted by id, unique; arena offsets = deterministic
     * prefix sums of 16-aligned payload sizes. */
    for (i = 0u; i < t->moduleCount; i++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[i];
        uint32_t j;

        if (i > 0u && strcmp(t->modules[i - 1u].id, m->id) >= 0)
        {
            NoteFailureSimple(diag, "validate", m->id, 0u, 0u);
            return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
        }
        if (m->arenaOffset != arenaCursor)
        {
            NoteFailureSimple(diag, "validate", m->id, arenaCursor,
                              m->arenaOffset);
            return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
        }
        {
            uint32_t routingBytes = 0u;
            uint32_t j;
            for (j = m->routingFirst; j < m->routingFirst + m->routingCount; j++)
                routingBytes += t->routingSegments[j].byteCount;
            arenaCursor += (m->payloadSize + routingBytes
                            + EMERALD_SCRIPT_ARENA_ALIGNMENT - 1u)
                         & ~(EMERALD_SCRIPT_ARENA_ALIGNMENT - 1u);
            payloadTotal += m->payloadSize;
        }
        if (m->schema != 45u && m->schema != 46u)
        {
            NoteFailureSimple(diag, "validate", m->id, 45u, m->schema);
            return EMERALD_SCRIPT_ERR_UNEXPECTED_SCHEMA;
        }
        if (m->kind > EMERALD_SCRIPT_NATIVE_MODULE_GIFT)
        {
            NoteFailureSimple(diag, "validate", m->id, 0u, 0u);
            return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
        }
        /* (cursor + payload total advanced in the block above) */

        /* Segments: packed, contiguous, in payload bounds. */
        {
            uint32_t expectedOff = 0u;
            for (j = m->segmentFirst; j < m->segmentFirst + m->segmentCount; j++)
            {
                const struct EmeraldScriptNativeSegment *s = &t->segments[j];
                if (s->payloadOffset != expectedOff
                 || s->kind > EMERALD_SCRIPT_NATIVE_SEGMENT_STATIC_DATA
                 || s->byteCount == 0u
                 || s->payloadOffset > m->payloadSize
                 || s->byteCount > m->payloadSize - s->payloadOffset)
                {
                    NoteFailureSimple(diag, "validate", m->id, 0u, 0u);
                    return EMERALD_SCRIPT_ERR_SEGMENT_INVALID;
                }
                expectedOff += s->byteCount;
            }
            if (expectedOff != m->payloadSize)
            {
                NoteFailureSimple(diag, "validate", m->id, m->payloadSize,
                                  expectedOff);
                return EMERALD_SCRIPT_ERR_SEGMENT_INVALID;
            }
        }

        /* Exports: sorted by payload offset, unique, in bounds. */
        for (j = m->exportFirst + 1u; j < m->exportFirst + m->exportCount; j++)
        {
            const struct EmeraldScriptNativeExport *e = &t->exports[j];
            const struct EmeraldScriptNativeExport *prev = &t->exports[j - 1u];
            if (e->moduleIndex != i || e->payloadOffset >= m->payloadSize
             || e->payloadOffset <= prev->payloadOffset
             || e->kind > EMERALD_SCRIPT_NATIVE_EXPORT_OPAQUE
             || e->boundaryKind > EMERALD_SCRIPT_NATIVE_BOUNDARY_ROUTING
             || e->name >= t->poolCount)
            {
                NoteFailureSimple(diag, "validate", m->id, 0u, 0u);
                return EMERALD_SCRIPT_ERR_EXPORT_INVALID;
            }
        }

        /* Relocs: sorted by operand offset, width 4, non-overlapping,
         * in bounds. */
        for (j = m->relocFirst + 1u; j < m->relocFirst + m->relocCount; j++)
        {
            const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
            const struct EmeraldScriptNativeReloc *prev = &t->relocs[j - 1u];
            if (r->moduleIndex != i || r->operandPayloadOffset >= m->payloadSize
             || r->operandPayloadOffset + 4u > m->payloadSize
             || r->operandPayloadOffset < prev->operandPayloadOffset + 4u
             || r->targetClass > EMERALD_SCRIPT_NATIVE_CLASS_RAM_DATA
             || r->targetKind > EMERALD_SCRIPT_NATIVE_KIND_RAM_HOST
             || r->targetKey >= t->poolCount
             || (r->targetLabel != EMERALD_SCRIPT_NATIVE_OFFSET_NONE
                 && r->targetLabel >= t->poolCount))
            {
                NoteFailureSimple(diag, "validate", m->id,
                                  r->operandPayloadOffset, 0u);
                return EMERALD_SCRIPT_ERR_RELOC_INVALID;
            }
        }

        /* Boundaries: sorted, non-overlapping, in bounds. */
        for (j = m->boundaryFirst + 1u; j < m->boundaryFirst + m->boundaryCount; j++)
        {
            const struct EmeraldScriptNativeBoundary *b = &t->boundaries[j];
            const struct EmeraldScriptNativeBoundary *prev =
                &t->boundaries[j - 1u];
            if (b->payloadOffset >= m->payloadSize
             || b->length == 0u
             || b->payloadOffset + b->length > m->payloadSize
             || b->payloadOffset < prev->payloadOffset + prev->length)
            {
                NoteFailureSimple(diag, "validate", m->id,
                                  b->payloadOffset, 0u);
                return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
            }
        }
    }
    if (payloadTotal != EMERALD_SCRIPT_ARENA_PAYLOAD_BYTES)
    {
        NoteFailureSimple(diag, "validate", "script-table",
                          EMERALD_SCRIPT_ARENA_PAYLOAD_BYTES, payloadTotal);
        return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
    }

    /* R13-G5: routing segments sorted globally by GBA start,
     * non-overlapping, per-module span offsets = payloadSize + prefix,
     * the blob covering exactly the 5,749 routing bytes. */
    {
        uint32_t routingBytes = 0u;
        for (i = 0u; i < t->routingSegmentCount; i++)
        {
            const struct EmeraldScriptNativeRoutingSegment *s =
                &t->routingSegments[i];
            if (i > 0u)
            {
                const struct EmeraldScriptNativeRoutingSegment *prev =
                    &t->routingSegments[i - 1u];
                if (s->originalGbaStart
                        < prev->originalGbaStart + prev->byteCount)
                {
                    NoteFailureSimple(diag, "validate", "routing-segments",
                                      s->originalGbaStart, 0u);
                    return EMERALD_SCRIPT_ERR_SEGMENT_INVALID;
                }
            }
            if (s->moduleIndex >= t->moduleCount || s->byteCount == 0u)
            {
                NoteFailureSimple(diag, "validate", "routing-segments", i, 0u);
                return EMERALD_SCRIPT_ERR_SEGMENT_INVALID;
            }
            routingBytes += s->byteCount;
        }
        if (routingBytes != 5749u || t->routingByteCount != 5749u
         || t->routingBytes == NULL)
        {
            NoteFailureSimple(diag, "validate", "routing-bytes", routingBytes,
                              t->routingByteCount);
            return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
        }
    }
    /* Per-module routing segment windows + span offsets. */
    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t expectedOff = m->payloadSize;
        uint32_t j;
        for (j = m->routingFirst; j < m->routingFirst + m->routingCount; j++)
        {
            const struct EmeraldScriptNativeRoutingSegment *s =
                &t->routingSegments[j];
            if (s->moduleIndex != module || s->spanOffset != expectedOff)
            {
                NoteFailureSimple(diag, "validate", m->id,
                                  expectedOff, s->spanOffset);
                return EMERALD_SCRIPT_ERR_SEGMENT_INVALID;
            }
            expectedOff += s->byteCount;
        }
    }

    /* Dynamic index: strictly ascending (gba, class). */
    for (i = 1u; i < t->dynamicTargetCount; i++)
    {
        const struct EmeraldScriptNativeDynamicTarget *d =
            &t->dynamicTargets[i];
        const struct EmeraldScriptNativeDynamicTarget *prev =
            &t->dynamicTargets[i - 1u];
        if (d->gbaAddress < prev->gbaAddress
         || (d->gbaAddress == prev->gbaAddress
             && d->targetClass <= prev->targetClass))
        {
            NoteFailureSimple(diag, "validate", "dynamic-index", 0u, 0u);
            return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
        }
        if (d->targetClass > EMERALD_SCRIPT_NATIVE_CLASS_RAM_DATA
         || d->targetKind > EMERALD_SCRIPT_NATIVE_KIND_RAM_HOST
         || d->targetKey >= t->poolCount
         || (d->targetLabel != EMERALD_SCRIPT_NATIVE_OFFSET_NONE
             && d->targetLabel >= t->poolCount))
        {
            NoteFailureSimple(diag, "validate", "dynamic-index", 0u, 0u);
            return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
        }
    }

    /* Routing rows: sorted by GBA source; well-formed fields. */
    for (i = 1u; i < t->routingRelocCount; i++)
    {
        if (t->routingRelocs[i].sourceGbaOffset
            <= t->routingRelocs[i - 1u].sourceGbaOffset)
        {
            NoteFailureSimple(diag, "validate", "routing-relocs", 0u, 0u);
            return EMERALD_SCRIPT_ERR_RELOC_INVALID;
        }
    }

    /* Root/interior pins over the whole 8,208 SCRIPT class. */
    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t j;
        for (j = m->relocFirst; j < m->relocFirst + m->relocCount; j++)
        {
            const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
            if (r->targetClass == EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT)
            {
                if (r->targetOffset == 0u)
                    roots++;
                else
                    interior++;
            }
        }
    }
    for (i = 0u; i < t->routingRelocCount; i++)
    {
        const struct EmeraldScriptNativeRoutingReloc *r = &t->routingRelocs[i];
        if (r->targetClass == EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT)
        {
            if (r->targetOffset == 0u)
                roots++;
            else
                interior++;
        }
    }
    if (roots != 95u || interior != 8113u)
    {
        NoteFailureSimple(diag, "validate", "root-interior-pins",
                          95u + 8113u, roots + interior);
        return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
    }

    /* Bridges: 7/2/3 bytes. */
    if (t->bridges[0].byteCount != 7u || t->bridges[1].byteCount != 2u
     || t->bridges[2].byteCount != 3u)
    {
        NoteFailureSimple(diag, "validate", "movement-bridges", 12u, 0u);
        return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
    }

    /* RAM allowlist: exactly the gStringVar4 row. */
    if (t->ramAllowlistCount != 1u
     || t->ramAllowlist[0].gbaAddress != 0x02021FC4u
     || t->ramAllowlist[0].size != 1000u)
    {
        NoteFailureSimple(diag, "validate", "ram-allowlist", 0u, 0u);
        return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
    }
    for (i = 0u; i < t->ramTargetCount; i++)
    {
        if (t->ramTargets[i].gbaAddress != 0x02021FC4u)
        {
            NoteFailureSimple(diag, "validate", "ram-targets",
                              t->ramTargets[i].gbaAddress, 0u);
            return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
        }
    }

    /* gStdScripts rows: slots 0..10 in order, resolvable exports. */
    for (i = 0u; i < t->stdScriptCount; i++)
    {
        const struct EmeraldScriptNativeStdScript *s = &t->stdScripts[i];
        const struct EmeraldScriptNativeExport *e;

        if (s->slot != i || s->moduleIndex >= t->moduleCount)
        {
            NoteFailureSimple(diag, "validate", "std-scripts", i, s->slot);
            return EMERALD_SCRIPT_ERR_EXPORT_INVALID;
        }
        e = FindExport(s->moduleIndex, s->payloadOffset);
        if (e == NULL || e->payloadOffset >= t->modules[s->moduleIndex].payloadSize)
        {
            NoteFailureSimple(diag, "validate", "std-scripts",
                              s->payloadOffset, 0u);
            return EMERALD_SCRIPT_ERR_EXPORT_INVALID;
        }
    }

    /* F rows: per-kind pins + resolvable exports (routing rows are
     * map-scripts only). */
    {
        uint32_t byKind[4] = {0u, 0u, 0u, 0u};
        for (i = 0u; i < t->fBindingCount; i++)
        {
            const struct EmeraldScriptNativeFBinding *f = &t->fBindings[i];
            if (f->kind > EMERALD_SCRIPT_NATIVE_F_BG_EVENT)
            {
                NoteFailureSimple(diag, "validate", "f-inbound", i, 0u);
                return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
            }
            byKind[f->kind]++;
            if (f->boundaryKind == EMERALD_SCRIPT_NATIVE_BOUNDARY_ROUTING)
            {
                if (f->kind != EMERALD_SCRIPT_NATIVE_F_MAP_SCRIPTS)
                {
                    NoteFailureSimple(diag, "validate", "f-inbound", i, 0u);
                    return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
                }
            }
            else
            {
                const struct EmeraldScriptNativeExport *e =
                    FindExport(f->moduleIndex, f->payloadOffset);
                if (e == NULL)
                {
                    NoteFailureSimple(diag, "validate", "f-inbound",
                                      f->payloadOffset, 0u);
                    return EMERALD_SCRIPT_ERR_EXPORT_INVALID;
                }
            }
        }
        if (byKind[0] != 518u || byKind[1] != 2163u
         || byKind[2] != 289u || byKind[3] != 531u)
        {
            NoteFailureSimple(diag, "validate", "f-inbound",
                              518u + 2163u + 289u + 531u,
                              byKind[0] + byKind[1] + byKind[2] + byKind[3]);
            return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
        }
    }

    /* Marts: static-data segments, item-count arithmetic. */
    for (i = 0u; i < t->martCount; i++)
    {
        const struct EmeraldScriptNativeMart *mart = &t->marts[i];
        const struct EmeraldScriptNativeSegment *seg =
            FindSegment(mart->moduleIndex, mart->payloadOffset);
        if (seg == NULL || seg->kind != EMERALD_SCRIPT_NATIVE_SEGMENT_STATIC_DATA
         || mart->payloadOffset != seg->payloadOffset
         || (uint32_t)mart->itemCount * 2u + 2u != mart->byteCount
         || mart->byteCount != seg->byteCount)
        {
            NoteFailureSimple(diag, "validate", "mart-tables",
                              mart->payloadOffset, 0u);
            return EMERALD_SCRIPT_ERR_SEGMENT_INVALID;
        }
    }

    return EMERALD_SCRIPT_OK;
}

/* ------------------------------------------------------------------ */
/* Phase 1: pack surface + snapshot resolution                         */

static enum EmeraldScriptCompatStatus ValidatePackSurface(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldScriptCompatDiagnostics *diag)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    size_t entryCount = Gen3ResourcePack_GetEntryCount(pack);
    size_t i;
    uint32_t embeddedFound = 0u;
    unsigned char *seen = NULL;
    enum EmeraldScriptCompatStatus result = EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;

    if (entryCount == 0u)
    {
        NoteFailureSimple(diag, "validate", "pack-empty", 0u, 0u);
        return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
    }
    seen = calloc(t->moduleCount, 1u);
    if (seen == NULL)
        return EMERALD_SCRIPT_ERR_OUT_OF_MEMORY;

    for (i = 0u; i < entryCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        const struct EmeraldScriptNativeModule *module;
        uint32_t moduleIndex;
        Gen3ResourceHandle handle;
        enum Gen3ResourceResult resolveResult;
        struct Gen3ResourceView view;

        if (entry == NULL || entry->canonicalName == NULL)
            goto done;
        if (strncmp(entry->canonicalName, kScriptPrefix,
                    sizeof(kScriptPrefix) - 1u) != 0)
            continue;
        module = FindModule(entry->canonicalName);
        if (module == NULL)
        {
            NoteFailureSimple(diag, "validate", entry->canonicalName, 0u, 0u);
            result = EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
            goto done;
        }
        moduleIndex = (uint32_t)(module - t->modules);
        if (module->embedded == 0u)
        {
            /* A routing-only identity must never carry a payload. */
            NoteFailureSimple(diag, "validate", entry->canonicalName, 0u, 0u);
            result = EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
            goto done;
        }
        if (seen[moduleIndex])
        {
            NoteFailureSimple(diag, "validate", entry->canonicalName, 0u, 0u);
            result = EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
            goto done;
        }
        seen[moduleIndex] = 1u;
        embeddedFound++;
        if (entry->type != GEN3_RESOURCE_TYPE_STRUCTURED_DATA
         || entry->schema != module->schema
         || entry->payloadSize != module->payloadSize
         || entry->payload == NULL || entry->payloadSha256 == NULL
         || memcmp(entry->payloadSha256, module->digest, 32u) != 0)
        {
            NoteFailureSimple(diag, "validate", entry->canonicalName,
                              module->payloadSize,
                              (uint32_t)entry->payloadSize);
            result = EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
            goto done;
        }

        /* The snapshot must resolve the module through the NORMAL M0/M1
         * path with the ROM_BASE provider winning. */
        resolveResult = Gen3ResourceSnapshot_FindHandle(
            snapshot, entry->canonicalName, &handle);
        if (resolveResult != GEN3_RESOURCE_OK)
        {
            NoteFailureSimple(diag, "validate", entry->canonicalName, 0u, 0u);
            result = EMERALD_SCRIPT_ERR_RESOLVE_FAILED;
            goto done;
        }
        resolveResult = Gen3ResourceSnapshot_Resolve(
            snapshot, handle, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            entry->schema, &view);
        if (resolveResult != GEN3_RESOURCE_OK || view.payload == NULL)
        {
            NoteFailureSimple(diag, "validate", entry->canonicalName, 0u, 0u);
            result = EMERALD_SCRIPT_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u,
                        module->schema, view.schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_SCRIPT_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        if (view.payloadSize != entry->payloadSize)
        {
            NoteFailureSimple(diag, "validate", entry->canonicalName,
                              (uint32_t)entry->payloadSize,
                              (uint32_t)view.payloadSize);
            result = EMERALD_SCRIPT_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
    }

    if (embeddedFound != EMERALD_SCRIPT_RELOC_COUNT - EMERALD_SCRIPT_RELOC_COUNT
            + 467u)
    {
        /* exactly 467 embedded modules */
        NoteFailureSimple(diag, "validate", "script-pack-surface", 467u,
                          embeddedFound);
        result = EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
        goto done;
    }
    for (i = 0u; i < t->moduleCount; i++)
    {
        if (t->modules[i].embedded != 0u && seen[i] == 0u)
        {
            NoteFailureSimple(diag, "validate", t->modules[i].id, 1u, 0u);
            result = EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
            goto done;
        }
    }
    result = EMERALD_SCRIPT_OK;

done:
    free(seen);
    return result;
}

/* ------------------------------------------------------------------ */
/* Phase 1: typed resolution + parity over all 16,704 rows             */

static enum EmeraldScriptCompatStatus ValidateTargetsAndParity(
    struct EmeraldScriptCompatDiagnostics *diag)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    uint32_t checked = 0u;
    uint32_t mismatches = 0u;
    uint32_t i;
    uint32_t module;

    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t j;
        for (j = m->relocFirst; j < m->relocFirst + m->relocCount; j++)
        {
            const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
            const struct EmeraldScriptNativeDynamicTarget *d;
            struct EmeraldScriptCompatResolvedTarget resolved;

            if (ResolveRelocRowTarget(r, &resolved)
                    != EMERALD_SCRIPT_OK)
            {
                const char *name = PoolString(r->targetKey);
                NoteFailureSimple(diag, "validate",
                                  name != NULL ? name : "reloc-target",
                                  r->targetKind, 0u);
                return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
            }
            d = FindDynamicTarget(r->originalEncodedGba, r->targetClass);
            checked++;
            if (d == NULL || d->targetKind != r->targetKind
             || d->targetKey != r->targetKey
             || d->targetLabel != r->targetLabel
             || d->targetOffset != r->targetOffset
             || d->targetPayloadOffset != r->targetPayloadOffset)
                mismatches++;
        }
    }
    for (i = 0u; i < t->routingRelocCount; i++)
    {
        const struct EmeraldScriptNativeRoutingReloc *r = &t->routingRelocs[i];
        const struct EmeraldScriptNativeDynamicTarget *d;
        struct EmeraldScriptCompatResolvedTarget resolved;

        if (ResolveRoutingRowTarget(r, &resolved) != EMERALD_SCRIPT_OK)
        {
            const char *name = PoolString(r->targetKey);
            NoteFailureSimple(diag, "validate",
                              name != NULL ? name : "routing-target",
                              r->targetKind, 0u);
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        }
        d = FindDynamicTarget(r->targetGba, r->targetClass);
        checked++;
        if (d == NULL || d->targetKind != r->targetKind
         || d->targetKey != r->targetKey
         || d->targetLabel != r->targetLabel
         || d->targetOffset != r->targetOffset
         || d->targetPayloadOffset != r->targetPayloadOffset)
            mismatches++;
    }
    sParityChecked = checked;
    sParityMismatches = mismatches;
    if (checked != EMERALD_SCRIPT_TOTAL_RELOC_COUNT || mismatches != 0u)
    {
        NoteFailureSimple(diag, "validate", "parity-oracle", checked,
                          mismatches);
        return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
    }
    return EMERALD_SCRIPT_OK;
}

/* ------------------------------------------------------------------ */
/* Phase 2: staging                                                    */

static void FreeGeneration(struct EmeraldScriptCompatGeneration *gen)
{
    if (gen == NULL)
        return;
    free(gen->arena);
    free(gen->spans);
    free(gen->sources);
    free(gen->fBindings);
    free(gen);
}

static int SpanCompare(const void *a, const void *b)
{
    const struct EmeraldScriptCompatModuleSpan *sa =
        (const struct EmeraldScriptCompatModuleSpan *)a;
    const struct EmeraldScriptCompatModuleSpan *sb =
        (const struct EmeraldScriptCompatModuleSpan *)b;
    if (sa->base != sb->base)
        return sa->base < sb->base ? -1 : 1;
    return 0;
}

static int SourceRowCompare(const void *a, const void *b)
{
    const struct EmeraldScriptCompatSourceRow *sa =
        (const struct EmeraldScriptCompatSourceRow *)a;
    const struct EmeraldScriptCompatSourceRow *sb =
        (const struct EmeraldScriptCompatSourceRow *)b;
    if (sa->operandAddress != sb->operandAddress)
        return sa->operandAddress < sb->operandAddress ? -1 : 1;
    return 0;
}

static enum EmeraldScriptCompatStatus StageGeneration(
    const struct Gen3ResourcePack *pack,
    struct EmeraldScriptCompatDiagnostics *diag)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    struct EmeraldScriptCompatGeneration *gen;
    size_t arenaSize = 0u;
    uint32_t i;
    uint32_t module;
    uint32_t sourceCount = 0u;
    size_t entryCount = Gen3ResourcePack_GetEntryCount(pack);
    size_t e;

    /* Arena size = the aligned end of the last span (the generator's
     * deterministic cursor); each span = payload + routing suffix. */
    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t routingBytes = 0u;
        uint32_t j;
        for (j = m->routingFirst; j < m->routingFirst + m->routingCount; j++)
            routingBytes += t->routingSegments[j].byteCount;
        {
            size_t end = (size_t)m->arenaOffset + m->payloadSize
                       + routingBytes;
            end = (end + EMERALD_SCRIPT_ARENA_ALIGNMENT - 1u)
                & ~(size_t)(EMERALD_SCRIPT_ARENA_ALIGNMENT - 1u);
            if (end > arenaSize)
                arenaSize = end;
        }
    }

    gen = calloc(1u, sizeof(*gen));
    if (gen == NULL)
        return EMERALD_SCRIPT_ERR_OUT_OF_MEMORY;
    gen->arena = SC_MALLOC(arenaSize);
    gen->spans = SC_MALLOC(sizeof(gen->spans[0]) * t->moduleCount);
    gen->sources = SC_MALLOC(sizeof(gen->sources[0])
                             * (t->relocCount + t->routingRelocCount));
    gen->fBindings = SC_MALLOC(sizeof(gen->fBindings[0]) * t->fBindingCount);
    if (gen->arena == NULL || gen->spans == NULL || gen->sources == NULL
     || gen->fBindings == NULL)
    {
        NoteFailureSimple(diag, "stage", "allocations", (uint32_t)arenaSize, 0u);
        FreeGeneration(gen);
        return EMERALD_SCRIPT_ERR_OUT_OF_MEMORY;
    }
    gen->arenaSize = arenaSize;

    /* Copy payloads: the arena bytes come from the pack entries
     * (already digest-validated in phase 1). */
    {
        for (e = 0u; e < entryCount; e++)
        {
            const struct Gen3ResourcePackEntry *entry =
                Gen3ResourcePack_GetEntry(pack, e);
            const struct EmeraldScriptNativeModule *m;

            if (entry == NULL || entry->canonicalName == NULL)
                continue;
            if (strncmp(entry->canonicalName, kScriptPrefix,
                        sizeof(kScriptPrefix) - 1u) != 0)
                continue;
            m = FindModule(entry->canonicalName);
            if (m == NULL)
                continue;
            memcpy(gen->arena + m->arenaOffset, entry->payload,
                   entry->payloadSize);
        }
    }

    /* R13-G5: copy each module's routing suffix from the generated
     * qualified-ROM blob (map dispatch + conditional tables). */
    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t j;
        for (j = m->routingFirst; j < m->routingFirst + m->routingCount; j++)
        {
            const struct EmeraldScriptNativeRoutingSegment *seg =
                &t->routingSegments[j];
            uint32_t blobOffset = 0u;
            uint32_t k;
            for (k = 0u; k < t->routingSegmentCount
                 && t->routingSegments[k].originalGbaStart
                        < seg->originalGbaStart; k++)
                blobOffset += t->routingSegments[k].byteCount;
            memcpy(gen->arena + m->arenaOffset + seg->spanOffset,
                   t->routingBytes + blobOffset, seg->byteCount);
        }
    }

    /* Spans (sorted by base) + raw-value proof + source index. */
    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t j;

        gen->spans[module].base = (uintptr_t)(gen->arena + m->arenaOffset);
        gen->spans[module].payloadSize = m->payloadSize;
        gen->spans[module].moduleIndex = module;
        gen->spans[module].routingBytes = 0u;
        for (j = m->routingFirst; j < m->routingFirst + m->routingCount; j++)
            gen->spans[module].routingBytes += t->routingSegments[j].byteCount;
        for (j = m->relocFirst; j < m->relocFirst + m->relocCount; j++)
        {
            const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
            const uint8_t *operand = gen->arena + m->arenaOffset
                                   + r->operandPayloadOffset;

            if (ReadU32LE(operand) != r->originalEncodedGba)
            {
                NoteFailureSimple(diag, "stage", m->id,
                                  r->originalEncodedGba, ReadU32LE(operand));
                FreeGeneration(gen);
                return EMERALD_SCRIPT_ERR_RELOC_INVALID;
            }
            gen->sources[sourceCount].operandAddress =
                (uintptr_t)operand;
            gen->sources[sourceCount].relocIndex = j;
            sourceCount++;
        }
    }
    /* R13-G5: the 830 routing sources become live operand rows (their
     * encoded u32 dispatch entries live in the staged routing tables). */
    for (i = 0u; i < t->routingRelocCount; i++)
    {
        const struct EmeraldScriptNativeRoutingReloc *r = &t->routingRelocs[i];
        const struct EmeraldScriptNativeModule *m = NULL;
        const struct EmeraldScriptNativeRoutingSegment *seg =
            FindRoutingSegmentForGba(r->sourceGbaOffset, &m);
        uintptr_t operand;
        if (seg == NULL || m == NULL)
        {
            NoteFailureSimple(diag, "stage", "routing-source",
                              r->sourceGbaOffset, 0u);
            FreeGeneration(gen);
            return EMERALD_SCRIPT_ERR_RELOC_INVALID;
        }
        operand = (uintptr_t)(gen->arena + m->arenaOffset + seg->spanOffset
                              + (r->sourceGbaOffset - seg->originalGbaStart));
        if (ReadU32LE((const uint8_t *)operand) != r->targetGba)
        {
            NoteFailureSimple(diag, "stage", "routing-source",
                              r->targetGba, ReadU32LE((const uint8_t *)operand));
            FreeGeneration(gen);
            return EMERALD_SCRIPT_ERR_RELOC_INVALID;
        }
        gen->sources[sourceCount].operandAddress = operand;
        gen->sources[sourceCount].relocIndex =
            EMERALD_SCRIPT_NATIVE_OFFSET_NONE;
        gen->sources[sourceCount].routingRowIndex = i;
        sourceCount++;
    }
    gen->sourceCount = sourceCount;
    qsort(gen->spans, t->moduleCount, sizeof(gen->spans[0]), SpanCompare);
    qsort(gen->sources, sourceCount, sizeof(gen->sources[0]),
          SourceRowCompare);

    /* Mart sentinels on the staged bytes. */
    for (i = 0u; i < t->martCount; i++)
    {
        const struct EmeraldScriptNativeMart *mart = &t->marts[i];
        const struct EmeraldScriptNativeModule *m = &t->modules[mart->moduleIndex];
        const uint8_t *bytes = gen->arena + m->arenaOffset + mart->payloadOffset;

        if (ReadU32LE(bytes + mart->byteCount - 2u) & 0xFFFFu)
        {
            NoteFailureSimple(diag, "stage", PoolString(mart->label),
                              mart->payloadOffset, 0u);
            FreeGeneration(gen);
            return EMERALD_SCRIPT_ERR_SEGMENT_INVALID;
        }
    }

    /* Staged gStdScripts candidate table (plan sec 12): native arena
     * pointers only - never a compiled GBA payload. */
    for (i = 0u; i < t->stdScriptCount; i++)
    {
        const struct EmeraldScriptNativeStdScript *s = &t->stdScripts[i];
        const struct EmeraldScriptNativeModule *m = &t->modules[s->moduleIndex];
        const struct EmeraldScriptNativeExport *e =
            FindExport(s->moduleIndex, s->payloadOffset);

        gen->std[i].slot = s->slot;
        gen->std[i].moduleIndex = s->moduleIndex;
        gen->std[i].payloadOffset = s->payloadOffset;
        gen->std[i].encodedGba = s->encodedGba;
        gen->std[i].boundaryKind = s->boundaryKind;
        CopyName(gen->std[i].moduleKey, sizeof(gen->std[i].moduleKey), m->id);
        CopyName(gen->std[i].exportName, sizeof(gen->std[i].exportName),
                 e != NULL ? PoolString(e->name) : NULL);
        gen->std[i].stagedAddress =
            (uintptr_t)(gen->arena + m->arenaOffset + s->payloadOffset);
    }

    /* Staged F rebind plan (plan sec 13): validation only - the live
     * R13-F structures are never mutated. Routing rows (518 map-script
     * dispatch pointers) carry their full identity but no staged
     * address: the routing bytes stay ROM-resident through G3. */
    for (i = 0u; i < t->fBindingCount; i++)
    {
        const struct EmeraldScriptNativeFBinding *f = &t->fBindings[i];
        const struct EmeraldScriptNativeModule *m = &t->modules[f->moduleIndex];
        const struct EmeraldScriptNativeExport *e;
        struct EmeraldScriptCompatStagedFBinding *out = &gen->fBindings[i];

        memset(out, 0, sizeof(*out));
        out->kind = f->kind;
        out->boundaryKind = f->boundaryKind;
        out->sameMap = f->sameMap;
        CopyName(out->mapSymbol, sizeof(out->mapSymbol),
                 PoolString(f->mapSymbol));
        CopyName(out->mapKey, sizeof(out->mapKey), PoolString(f->mapKey));
        CopyName(out->moduleKey, sizeof(out->moduleKey), m->id);
        out->gbaTarget = f->gbaTarget;
        out->moduleOffset = f->gbaTarget - m->regionGbaStart;
        out->payloadOffset = f->payloadOffset;
        e = FindExport(f->moduleIndex, f->payloadOffset);
        CopyName(out->exportName, sizeof(out->exportName),
                 e != NULL ? PoolString(e->name) : NULL);
        if (f->boundaryKind == EMERALD_SCRIPT_NATIVE_BOUNDARY_ROUTING)
        {
            /* R13-G5: the map dispatch table is staged in the module's
             * routing suffix - the F provenance now resolves live. */
            const struct EmeraldScriptNativeRoutingSegment *seg =
                FindRoutingSegmentForGba(f->gbaTarget, NULL);
            if (seg == NULL)
            {
                NoteFailureSimple(diag, "stage", "f-routing",
                                  f->gbaTarget, 0u);
                FreeGeneration(gen);
                return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
            }
            out->disposition = EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA;
            out->stagedAddress = (uintptr_t)(gen->arena + m->arenaOffset
                                           + seg->spanOffset
                                           + (f->gbaTarget
                                              - seg->originalGbaStart));
        }
        else
        {
            out->disposition = EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA;
            out->stagedAddress = (uintptr_t)(gen->arena + m->arenaOffset
                                           + f->payloadOffset);
        }
    }

    gen->generationId = ++sGenerationCounter;
    FreeGeneration(sGeneration);
    sGeneration = gen;
    return EMERALD_SCRIPT_OK;
}

/* ------------------------------------------------------------------ */
/* Public lifecycle                                                    */

enum EmeraldScriptCompatStatus
EmeraldScriptCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldScriptCompatDiagnostics *diagnostics)
{
    enum EmeraldScriptCompatStatus status;

    if (snapshot == NULL || pack == NULL)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    if (EmeraldTextCompat_GetPublishedCount() != EMERALD_TEXT_LABEL_COUNT)
    {
        NoteFailureSimple(diagnostics, "validate", "text-seam-unpublished",
                          EMERALD_TEXT_LABEL_COUNT,
                          (uint32_t)EmeraldTextCompat_GetPublishedCount());
        return EMERALD_SCRIPT_ERR_UNAVAILABLE;
    }
    if (EmeraldLeafCompat_GetPublishedCount() != EMERALD_LEAF_RESOURCE_COUNT)
    {
        NoteFailureSimple(diagnostics, "validate", "leaf-seam-unpublished",
                          EMERALD_LEAF_RESOURCE_COUNT,
                          (uint32_t)EmeraldLeafCompat_GetPublishedCount());
        return EMERALD_SCRIPT_ERR_UNAVAILABLE;
    }

    status = ValidateTableStructure(diagnostics);
    if (status != EMERALD_SCRIPT_OK)
        return status;
    status = ValidatePackSurface(snapshot, pack, diagnostics);
    if (status != EMERALD_SCRIPT_OK)
        return status;
    status = ValidateTargetsAndParity(diagnostics);
    if (status != EMERALD_SCRIPT_OK)
        return status;
    /* Phase 2/3: stage the candidate and shadow-commit it atomically.
     * A failed stage frees the candidate and preserves the previous
     * generation. */
    return StageGeneration(pack, diagnostics);
}

void EmeraldScriptCompat_ClearMigratedEntries(void)
{
    EmeraldScriptCompat_UnregisterRanges();
    sGenerationCounter++;
    FreeGeneration(sGeneration);
    sGeneration = NULL;
    sParityChecked = 0u;
    sParityMismatches = 0u;
}

void EmeraldScriptCompat_Shutdown(void)
{
    EmeraldScriptCompat_ClearMigratedEntries();
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */

uint64_t EmeraldScriptCompat_GetGenerationId(void)
{
    return sGeneration != NULL ? sGeneration->generationId : 0u;
}

bool EmeraldScriptCompat_GetArena(const uint8_t **outBase, size_t *outSize)
{
    if (sGeneration == NULL || outBase == NULL || outSize == NULL)
        return false;
    *outBase = sGeneration->arena;
    *outSize = sGeneration->arenaSize;
    return true;
}

bool EmeraldScriptCompat_GetModuleSpan(const char *moduleKey,
                                       const uint8_t **outBase,
                                       size_t *outSize)
{
    const struct EmeraldScriptNativeModule *m;
    const uint8_t *arena;
    size_t arenaSize;

    if (moduleKey == NULL || outBase == NULL || outSize == NULL)
        return false;
    if (sGeneration == NULL)
        return false;
    m = FindModule(moduleKey);
    if (m == NULL)
        return false;
    arena = sGeneration->arena;
    arenaSize = sGeneration->arenaSize;
    if (m->payloadSize == 0u)
    {
        *outBase = arena + arenaSize;
        *outSize = 0u;
        return true;
    }
    *outBase = arena + m->arenaOffset;
    *outSize = m->payloadSize;
    return true;
}

enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ResolveOperand(uintptr_t operandAddress,
                                   struct EmeraldScriptCompatResolvedTarget *outTarget)
{
    struct EmeraldScriptCompatSourceRow key;
    struct EmeraldScriptCompatSourceRow *hit;
    const struct EmeraldScriptNativeReloc *row;
    enum EmeraldScriptCompatStatus status;

    if (outTarget == NULL)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    if (sGeneration == NULL)
        return EMERALD_SCRIPT_ERR_UNAVAILABLE;
    key.operandAddress = operandAddress;
    key.relocIndex = 0u;
    key.routingRowIndex = 0u;
    hit = bsearch(&key, sGeneration->sources, sGeneration->sourceCount,
                  sizeof(sGeneration->sources[0]), SourceRowCompare);
    if (hit == NULL)
        return EMERALD_SCRIPT_ERR_RELOC_INVALID;
    if (hit->relocIndex == EMERALD_SCRIPT_NATIVE_OFFSET_NONE)
    {
        const struct EmeraldScriptNativeRoutingReloc *rrow =
            &kEmeraldScriptCompatTable.routingRelocs[hit->routingRowIndex];
        status = ResolveRoutingRowTarget(rrow, outTarget);
    }
    else
    {
        row = &kEmeraldScriptCompatTable.relocs[hit->relocIndex];
        status = ResolveRelocRowTarget(row, outTarget);
    }
    return status;
}

enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ResolveBinding(uint32_t targetClass,
                                   const char *resourceKey,
                                   const char *label,
                                   uint32_t targetOffset,
                                   struct EmeraldScriptCompatResolvedTarget *outTarget)
{
    if (outTarget == NULL)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    if (targetClass == EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT)
    {
        const struct EmeraldScriptNativeModule *module =
            resourceKey != NULL ? FindModule(resourceKey) : NULL;
        const struct EmeraldScriptNativeExport *e = NULL;
        uint32_t i;

        if (module == NULL || sGeneration == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        /* Bind by export name within the module (per-module scan; the
         * hot path is ResolveOperand, which is O(log n)). */
        for (i = module->exportFirst;
             i < module->exportFirst + module->exportCount; i++)
        {
            const struct EmeraldScriptNativeExport *candidate =
                &kEmeraldScriptCompatTable.exports[i];
            const char *name = PoolString(candidate->name);
            if (label != NULL && name != NULL && strcmp(name, label) == 0)
            {
                e = candidate;
                break;
            }
        }
        if (e == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        return ResolveTargetIdentity(
            targetClass,
            e->kind == EMERALD_SCRIPT_NATIVE_EXPORT_SCRIPT
                ? EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_PAYLOAD
                : EMERALD_SCRIPT_NATIVE_KIND_MART_TABLE,
            resourceKey, label, targetOffset, e->payloadOffset,
            e->boundaryKind, e->originalGbaAddress, outTarget);
    }
    /* Non-script classes resolve through the dynamic identity path:
     * the class selects the kind set; a label-level binding uses the
     * first matching dynamic row. */
    {
        const struct EmeraldScriptNativeDynamicTarget *d = NULL;
        uint32_t i;

        for (i = 0u; i < kEmeraldScriptCompatTable.dynamicTargetCount; i++)
        {
            const struct EmeraldScriptNativeDynamicTarget *candidate =
                &kEmeraldScriptCompatTable.dynamicTargets[i];
            const char *key = PoolString(candidate->targetKey);
            const char *lbl = PoolString(candidate->targetLabel);
            if (candidate->targetClass != targetClass)
                continue;
            if (resourceKey != NULL
                && (key == NULL || strcmp(key, resourceKey) != 0))
                continue;
            if (label != NULL
                && (lbl == NULL || strcmp(lbl, label) != 0))
                continue;
            d = candidate;
            break;
        }
        if (d == NULL)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        return ResolveTargetIdentity(
            d->targetClass, d->targetKind,
            PoolString(d->targetKey), PoolString(d->targetLabel),
            d->targetOffset, d->targetPayloadOffset, d->boundaryKind,
            d->gbaAddress, outTarget);
    }
}

enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ResolveEncodedTarget(
    uint32_t encodedGba, uint32_t expectedClass,
    struct EmeraldScriptCompatResolvedTarget *outTarget)
{
    const struct EmeraldScriptNativeDynamicTarget *d;

    if (outTarget == NULL)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    if (expectedClass > EMERALD_SCRIPT_NATIVE_CLASS_RAM_DATA)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    d = FindDynamicTarget(encodedGba, expectedClass);
    if (d == NULL)
        return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
    return ResolveTargetIdentity(
        d->targetClass, d->targetKind,
        PoolString(d->targetKey), PoolString(d->targetLabel),
        d->targetOffset, d->targetPayloadOffset, d->boundaryKind,
        encodedGba, outTarget);
}

enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ReverseResolve(uintptr_t address,
                                   char *outModuleKey, size_t keyCap,
                                   uint32_t *outOffset,
                                   uint32_t *outSegmentKind)
{
    size_t low;
    size_t high;
    size_t hit;
    const struct EmeraldScriptNativeModule *m;
    const struct EmeraldScriptNativeSegment *seg;

    if (outModuleKey == NULL || keyCap == 0u || outOffset == NULL
     || outSegmentKind == NULL)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    if (sGeneration == NULL)
        return EMERALD_SCRIPT_ERR_UNAVAILABLE;
    low = 0u;
    high = kEmeraldScriptCompatTable.moduleCount;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        if (sGeneration->spans[mid].base <= address)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low == 0u)
        return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID; /* before the first span */
    hit = low - 1u;
    {
        const struct EmeraldScriptCompatModuleSpan *span =
            &sGeneration->spans[hit];
        uintptr_t offset;
        size_t spanSize = (size_t)span->payloadSize + span->routingBytes;
        if (address < span->base
         || spanSize == 0u
         || address - span->base >= spanSize)
            return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID; /* hull/hole/end */
        offset = address - span->base;
        m = &kEmeraldScriptCompatTable.modules[span->moduleIndex];
        CopyName(outModuleKey, keyCap, m->id);
        if ((uint32_t)offset >= span->payloadSize)
        {
            /* R13-G5: the routing suffix (map dispatch / conditional
             * table bytes) - payload offsets stay the state identity;
             * the reported offset remains payload-relative for the
             * non-routing region only. */
            *outOffset = EMERALD_SCRIPT_NATIVE_OFFSET_NONE;
            *outSegmentKind = 2u; /* routing suffix */
            return EMERALD_SCRIPT_OK;
        }
        *outOffset = (uint32_t)offset;
        seg = FindSegment(span->moduleIndex, (uint32_t)offset);
        *outSegmentKind = seg != NULL ? seg->kind : 0xFFu;
        return EMERALD_SCRIPT_OK;
    }
}

enum EmeraldScriptCompatStatus
EmeraldScriptCompat_ValidateBoundary(const char *moduleKey, uint32_t offset,
                                     uint32_t boundaryKind)
{
    const struct EmeraldScriptNativeModule *m;
    const struct EmeraldScriptNativeExport *e;
    const struct EmeraldScriptNativeSegment *seg;
    const struct EmeraldScriptNativeBoundary *b;

    if (moduleKey == NULL)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    m = FindModule(moduleKey);
    if (m == NULL || offset >= m->payloadSize)
        return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    e = FindExport((uint32_t)(m - kEmeraldScriptCompatTable.modules), offset);
    seg = FindSegment((uint32_t)(m - kEmeraldScriptCompatTable.modules), offset);
    b = FindBoundary((uint32_t)(m - kEmeraldScriptCompatTable.modules), offset);

    switch (boundaryKind)
    {
    case EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START:
        return b != NULL ? EMERALD_SCRIPT_OK
                         : EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    case EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION:
        /* Returns resume at the next instruction, which is always an
         * instruction start in the decoded model. */
        return b != NULL ? EMERALD_SCRIPT_OK
                         : EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    case EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT:
        return e != NULL ? EMERALD_SCRIPT_OK
                         : EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    case EMERALD_SCRIPT_BOUNDARY_SCRIPT_INTERIOR:
        if (e != NULL
         && e->kind == EMERALD_SCRIPT_NATIVE_EXPORT_SCRIPT
         && e->boundaryKind == EMERALD_SCRIPT_NATIVE_BOUNDARY_INTERIOR)
            return EMERALD_SCRIPT_OK;
        return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    case EMERALD_SCRIPT_BOUNDARY_TYPED_DATA_START:
        if (seg != NULL
         && seg->kind == EMERALD_SCRIPT_NATIVE_SEGMENT_STATIC_DATA
         && seg->payloadOffset == offset)
            return EMERALD_SCRIPT_OK;
        return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    case EMERALD_SCRIPT_BOUNDARY_ROUTING:
        /* Routing bytes are not arena-resident in G3. */
        return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    default:
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    }
}

bool EmeraldScriptCompat_GetStateIdentity(
    const char *moduleKey, Gen3ResourceKey *outKey, uint32_t *outSchema,
    uint32_t *outPayloadSize)
{
    const struct EmeraldScriptNativeModule *m;

    if (moduleKey == NULL || outKey == NULL || outSchema == NULL
     || outPayloadSize == NULL)
        return false;
    m = FindModule(moduleKey);
    if (m == NULL)
        return false;
    Gen3ResourceId_DeriveKey(m->id, outKey);
    *outSchema = m->schema;
    *outPayloadSize = m->payloadSize;
    return true;
}

enum EmeraldScriptCompatStatus EmeraldScriptCompat_ResolveStateIdentity(
    const Gen3ResourceKey *key, uint32_t resourceType, uint32_t schema,
    uint32_t representationRole, uint32_t payloadOffset,
    uint32_t boundaryRole, uintptr_t *outAddress,
    char *outModuleKey, size_t keyCap)
{
    const struct EmeraldScriptNativeModule *m = NULL;
    uint32_t i;

    if (key == NULL || outAddress == NULL || outModuleKey == NULL
     || keyCap == 0u)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    if (sGeneration == NULL)
        return EMERALD_SCRIPT_ERR_UNAVAILABLE;
    if (resourceType != GEN3_RESOURCE_TYPE_STRUCTURED_DATA
     || representationRole != EMERALD_RESOURCE_ROLE_CANONICAL)
        return EMERALD_SCRIPT_ERR_UNEXPECTED_SCHEMA;
    for (i = 0u; i < kEmeraldScriptCompatTable.moduleCount; i++)
    {
        Gen3ResourceKey candidate;
        Gen3ResourceId_DeriveKey(kEmeraldScriptCompatTable.modules[i].id,
                                 &candidate);
        if (Gen3ResourceId_KeyEqual(&candidate, key))
        {
            m = &kEmeraldScriptCompatTable.modules[i];
            break;
        }
    }
    if (m == NULL)
        return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
    if (m->schema != schema)
        return EMERALD_SCRIPT_ERR_UNEXPECTED_SCHEMA;
    if (payloadOffset >= m->payloadSize)
        return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    if (EmeraldScriptCompat_ValidateBoundary(m->id, payloadOffset,
                                             boundaryRole)
            != EMERALD_SCRIPT_OK)
        return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    CopyName(outModuleKey, keyCap, m->id);
    *outAddress = (uintptr_t)(sGeneration->arena + m->arenaOffset
                              + payloadOffset);
    return EMERALD_SCRIPT_OK;
}

enum EmeraldScriptCompatStatus EmeraldScriptCompat_ValidateProjectedRanges(
    size_t currentRangeCount, size_t rangeCapacity,
    size_t *outProjectedRangeCount)
{
    size_t projected;
    uint32_t i;
    uintptr_t previousEnd = 0u;

    if (outProjectedRangeCount == NULL)
        return EMERALD_SCRIPT_ERR_INVALID_ARGUMENT;
    if (sGeneration == NULL)
        return EMERALD_SCRIPT_ERR_UNAVAILABLE;
    if (currentRangeCount > rangeCapacity
     || kEmeraldScriptCompatTable.moduleCount > rangeCapacity - currentRangeCount)
        return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
    projected = currentRangeCount + kEmeraldScriptCompatTable.moduleCount;
    for (i = 0u; i < kEmeraldScriptCompatTable.moduleCount; i++)
    {
        const struct EmeraldScriptNativeModule *m =
            &kEmeraldScriptCompatTable.modules[i];
        uintptr_t start;
        uintptr_t end;
        Gen3ResourceKey key;

        if (m->schema != 45u && m->schema != 46u)
            return EMERALD_SCRIPT_ERR_UNEXPECTED_SCHEMA;
        Gen3ResourceId_DeriveKey(m->id, &key);
        if (m->id == NULL || m->id[0] == '\0')
            return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
        if (m->arenaOffset > sGeneration->arenaSize
         || m->payloadSize > sGeneration->arenaSize - m->arenaOffset)
            return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
        start = (uintptr_t)sGeneration->arena + m->arenaOffset;
        end = start + m->payloadSize;
        /* Empty routing identities are valid half-open ranges for the G4
         * dry run and overlap no bytes.  G5 must materialize their routing
         * spans before the real range-index registration. */
        if (m->payloadSize != 0u)
        {
            if (previousEnd != 0u && start < previousEnd)
                return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
            previousEnd = end;
        }
        (void)key;
    }
    *outProjectedRangeCount = projected;
    return EMERALD_SCRIPT_OK;
}

size_t EmeraldScriptCompat_GetStagedStdScriptCount(void)
{
    return sGeneration != NULL ? EMERALD_SCRIPT_STD_SCRIPT_COUNT : 0u;
}

bool EmeraldScriptCompat_GetStagedStdScript(
    uint32_t slot, struct EmeraldScriptCompatStagedStdScript *outRow)
{
    if (sGeneration == NULL || outRow == NULL
     || slot >= EMERALD_SCRIPT_STD_SCRIPT_COUNT)
        return false;
    *outRow = sGeneration->std[slot];
    return true;
}

size_t EmeraldScriptCompat_GetStagedFBindingCount(void)
{
    return sGeneration != NULL ? EMERALD_SCRIPT_F_BINDING_COUNT : 0u;
}

bool EmeraldScriptCompat_GetStagedFBinding(
    size_t index, struct EmeraldScriptCompatStagedFBinding *outRow)
{
    if (sGeneration == NULL || outRow == NULL
     || index >= EMERALD_SCRIPT_F_BINDING_COUNT)
        return false;
    *outRow = sGeneration->fBindings[index];
    return true;
}

bool EmeraldScriptCompat_GetStagedRoutingTarget(
    size_t index, struct EmeraldScriptCompatResolvedTarget *outTarget)
{
    if (sGeneration == NULL || outTarget == NULL
     || index >= kEmeraldScriptCompatTable.routingRelocCount)
        return false;
    return ResolveRoutingRowTarget(
        &kEmeraldScriptCompatTable.routingRelocs[index], outTarget)
        == EMERALD_SCRIPT_OK;
}

bool EmeraldScriptCompat_GetIndexCounts(
    struct EmeraldScriptCompatIndexCounts *outCounts)
{
    uint32_t opaque = 0u;
    uint32_t module;

    if (outCounts == NULL)
        return false;
    memset(outCounts, 0, sizeof(*outCounts));
    outCounts->modules = kEmeraldScriptCompatTable.moduleCount;
    outCounts->segments = kEmeraldScriptCompatTable.segmentCount;
    outCounts->exports = kEmeraldScriptCompatTable.exportCount;
    outCounts->relocs = kEmeraldScriptCompatTable.relocCount;
    outCounts->routingRelocs = kEmeraldScriptCompatTable.routingRelocCount;
    outCounts->boundaries = kEmeraldScriptCompatTable.boundaryCount;
    outCounts->dynamicTargets = kEmeraldScriptCompatTable.dynamicTargetCount;
    outCounts->stdScripts = kEmeraldScriptCompatTable.stdScriptCount;
    outCounts->fBindings = kEmeraldScriptCompatTable.fBindingCount;
    outCounts->marts = kEmeraldScriptCompatTable.martCount;
    outCounts->ramTargets = kEmeraldScriptCompatTable.ramTargetCount;
    outCounts->ramAllowlist = kEmeraldScriptCompatTable.ramAllowlistCount;
    outCounts->bridges = kEmeraldScriptCompatTable.bridgeCount;
    for (module = 0u; module < kEmeraldScriptCompatTable.moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m =
            &kEmeraldScriptCompatTable.modules[module];
        uint32_t j;
        for (j = m->segmentFirst; j < m->segmentFirst + m->segmentCount; j++)
        {
            const struct EmeraldScriptNativeSegment *s =
                &kEmeraldScriptCompatTable.segments[j];
            uint32_t pos = s->payloadOffset;
            uint32_t end = pos + s->byteCount;
            if (s->kind != EMERALD_SCRIPT_NATIVE_SEGMENT_BYTECODE)
                continue;
            while (pos < end)
            {
                const struct EmeraldScriptNativeBoundary *inst =
                    FindEnclosingInstruction(module, pos);
                if (inst != NULL)
                {
                    uint32_t instEnd = inst->payloadOffset + inst->length;
                    pos = instEnd > end ? end : instEnd;
                }
                else
                {
                    const struct EmeraldScriptNativeBoundary *next =
                        NULL;
                    size_t low = m->boundaryFirst;
                    size_t high = low + m->boundaryCount;
                    while (low < high)
                    {
                        size_t mid = low + (high - low) / 2u;
                        if (kEmeraldScriptCompatTable.boundaries[mid].payloadOffset
                                <= pos)
                            low = mid + 1u;
                        else
                            high = mid;
                    }
                    if (low < m->boundaryFirst + m->boundaryCount)
                        next = &kEmeraldScriptCompatTable.boundaries[low];
                    {
                        uint32_t nxt = next != NULL
                            ? (next->payloadOffset < end ? next->payloadOffset
                                                         : end)
                            : end;
                        opaque += nxt - pos;
                        pos = nxt;
                    }
                }
            }
        }
    }
    outCounts->opaqueBytes = opaque;
    return true;
}

bool EmeraldScriptCompat_GetParityCounts(uint32_t *outChecked,
                                         uint32_t *outMismatches)
{
    if (outChecked == NULL || outMismatches == NULL)
        return false;
    *outChecked = sParityChecked;
    *outMismatches = sParityMismatches;
    return sParityChecked != 0u;
}

/* ------------------------------------------------------------------ */
/* R13-G5: live publication + engine-facing resolver APIs               */

extern const uint8_t *gStdScripts[];

static bool sRangesRegistered;

void EmeraldScriptCompat_UnregisterRanges(void);
void EmeraldScriptCompat_UnregisterModuleRange(uint32_t moduleIndex);

static Gen3ResourceKey RangeKeyForModule(uint32_t moduleIndex)
{
    Gen3ResourceKey key;
    Gen3ResourceId_DeriveKey(kEmeraldScriptCompatTable.modules[moduleIndex].id,
                             &key);
    return key;
}

enum EmeraldScriptCompatStatus EmeraldScriptCompat_RegisterRanges(void)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    uint32_t module;
    uint32_t registered = 0u;

    if (sGeneration == NULL)
        return EMERALD_SCRIPT_ERR_UNAVAILABLE;
    if (index == NULL)
        return EMERALD_SCRIPT_ERR_STAGING_FAILED;
    if (sRangesRegistered)
        EmeraldScriptCompat_UnregisterRanges();
    if (t->moduleCount
            > EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES
                  - index->rangeCount)
        return EMERALD_SCRIPT_ERR_STAGING_FAILED;
    /* Sorted ascending spans register non-overlapping; any conflict is
     * a hard refusal with the whole registration rolled back. */
    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        struct EmeraldScriptCompatModuleSpan *span = NULL;
        uint32_t i;
        for (i = 0u; i < t->moduleCount; i++)
        {
            if (sGeneration->spans[i].moduleIndex == module)
            {
                span = &sGeneration->spans[i];
                break;
            }
        }
        if (span == NULL)
            goto fail;
        if (!EmeraldResourceRangeIndex_RegisterSpan(
                index, span->base,
                (size_t)span->payloadSize + span->routingBytes,
                m->id, GEN3_RESOURCE_TYPE_STRUCTURED_DATA, m->schema,
                EMERALD_RESOURCE_ROLE_CANONICAL))
            goto fail;
        registered++;
    }
    sRangesRegistered = true;
    /* R13-G5 (plan sec 7): the live arena registers as one dynamic
     * buffer so the stable virtual anchor resolves static-script
     * vaddress targets uniformly (the adapter re-validates owner +
     * generation at every resolution). */
    {
        struct EmeraldScriptDynamicBuffer arenaBuffer;
        memset(&arenaBuffer, 0, sizeof(arenaBuffer));
        arenaBuffer.kind = EMERALD_SCRIPT_DYNAMIC_STATIC_G_ARENA;
        arenaBuffer.ownerStorageId = 0u;
        arenaBuffer.generation = sGeneration->generationId;
        arenaBuffer.base = sGeneration->arena;
        arenaBuffer.size = sGeneration->arenaSize;
        arenaBuffer.instructionStarts = NULL;
        snprintf(arenaBuffer.ownerId, sizeof(arenaBuffer.ownerId),
                 "%s", "script-arena");
        if (!EmeraldScriptState_RegisterDynamicBuffer(&arenaBuffer))
            goto fail;
    }
    return EMERALD_SCRIPT_OK;

fail:
    /* Roll back the partial registration by exact resource key. */
    while (registered > 0u)
    {
        registered--;
        EmeraldScriptCompat_UnregisterModuleRange(registered);
    }
    return EMERALD_SCRIPT_ERR_RANGE_REGISTRATION;
}

/* Identity-based single-range removal: find the exact key and remove
 * it wherever it sits (position-independent - the G2 text-seam lesson). */
void EmeraldScriptCompat_UnregisterModuleRange(uint32_t moduleIndex)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    Gen3ResourceKey key;
    size_t i;

    if (index == NULL)
        return;
    key = RangeKeyForModule(moduleIndex);
    for (i = 0u; i < index->rangeCount; i++)
    {
        if (Gen3ResourceId_KeyEqual(&index->ranges[i].key, &key)
         && index->ranges[i].type == GEN3_RESOURCE_TYPE_STRUCTURED_DATA)
        {
            memmove(&index->ranges[i], &index->ranges[i + 1u],
                    (index->rangeCount - i - 1u) * sizeof(index->ranges[0]));
            index->rangeCount--;
            return;
        }
    }
}

void EmeraldScriptCompat_UnregisterRanges(void)
{
    uint32_t module;

    if (!sRangesRegistered)
        return;
    /* Remove in descending module order so the memmove stays cheap. */
    for (module = kEmeraldScriptCompatTable.moduleCount; module > 0u; module--)
        EmeraldScriptCompat_UnregisterModuleRange(module - 1u);
    sRangesRegistered = false;
}

bool EmeraldScriptCompat_AreRangesRegistered(void)
{
    return sRangesRegistered;
}

/* The production ScriptReadPointer path (plan sec 5): the exact source
 * operand address -> its relocation row -> the typed live target. Hard
 * refusal on any mismatch; no HostResolveGbaAddr fallback for static G
 * operands. */
bool EmeraldScriptCompat_ResolveLiveOperand(uintptr_t operandAddress,
                                            uintptr_t *outPointer)
{
    struct EmeraldScriptCompatResolvedTarget target;

    if (outPointer == NULL)
        return false;
    if (EmeraldScriptCompat_ResolveOperand(operandAddress, &target)
            != EMERALD_SCRIPT_OK)
        return false;
    if (target.disposition != EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA
     || target.liveAddress == 0u)
        return false;
    *outPointer = target.liveAddress;
    return true;
}

/* The map-dispatch provenance read (plan sec 10): a MapHeader's stored
 * GBA mapScripts address -> the staged routing table's live base. */
bool EmeraldScriptCompat_ResolveRoutingTable(uint32_t gbaTarget,
                                             uintptr_t *outBase,
                                             size_t *outSize)
{
    const struct EmeraldScriptNativeModule *module = NULL;
    const struct EmeraldScriptNativeRoutingSegment *seg;

    if (outBase == NULL || outSize == NULL || sGeneration == NULL)
        return false;
    seg = FindRoutingSegmentForGba(gbaTarget, &module);
    if (seg == NULL)
        return false;
    *outBase = ModuleSpanBase(
        (uint32_t)(module - kEmeraldScriptCompatTable.modules))
        + module->payloadSize + seg->spanOffset
        + (gbaTarget - seg->originalGbaStart);
    *outSize = seg->byteCount
        - (gbaTarget - seg->originalGbaStart);
    return true;
}

/* Publish the staged 11-entry gStdScripts candidate table into the
 * live native publication table (plan sec 9). Pure stores; every slot
 * must be a pointer into the current generation. */
enum EmeraldScriptCompatStatus EmeraldScriptCompat_PublishStdScripts(void)
{
    uint32_t i;

    if (sGeneration == NULL)
        return EMERALD_SCRIPT_ERR_UNAVAILABLE;
    for (i = 0u; i < EMERALD_SCRIPT_STD_SCRIPT_COUNT; i++)
    {
        const struct EmeraldScriptCompatStagedStdScript *s =
            &sGeneration->std[i];
        if (s->stagedAddress < (uintptr_t)sGeneration->arena
         || s->stagedAddress >= (uintptr_t)sGeneration->arena
                                 + sGeneration->arenaSize)
            return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
        gStdScripts[i] = (const uint8_t *)s->stagedAddress;
    }
    return EMERALD_SCRIPT_OK;
}

/* The production vaddress family path (plan sec 7): the stable anchor
 * lives in the state adapter (already production-linked from G4); the
 * seam exposes the static-G side - a static module anchor resolves
 * buffer-relative targets through the staged span. */
bool EmeraldScriptCompat_ResolveVAddress(uint32_t encodedVirtualBase,
                                         uintptr_t liveBase,
                                         uint32_t encodedTarget,
                                         uintptr_t *outAddress)
{
    const struct EmeraldScriptCompatModuleSpan *span = NULL;
    uintptr_t base;
    size_t size;
    uint32_t i;
    int64_t delta;
    uint64_t target;

    if (outAddress == NULL || sGeneration == NULL)
        return false;
    /* The live base must lie inside one staged span (the static
     * anchor case; dynamic buffers resolve through the state
     * adapter's registered buffers). */
    for (i = 0u; i < kEmeraldScriptCompatTable.moduleCount; i++)
    {
        size_t spanSize = (size_t)sGeneration->spans[i].payloadSize
                        + sGeneration->spans[i].routingBytes;
        if (liveBase >= sGeneration->spans[i].base
         && liveBase - sGeneration->spans[i].base < spanSize)
        {
            span = &sGeneration->spans[i];
            break;
        }
    }
    if (span == NULL)
        return false;
    base = span->base;
    size = (size_t)span->payloadSize + span->routingBytes;
    delta = (int64_t)encodedTarget - (int64_t)encodedVirtualBase;
    if (delta < 0)
        return false;
    target = (uint64_t)(liveBase - base) + (uint64_t)delta;
    if (target >= size)
        return false;
    *outAddress = base + (uintptr_t)target;
    return true;
}

bool EmeraldScriptCompat_IsPublished(void)
{
    return sGeneration != NULL && sRangesRegistered;
}

/* R13-G5 (plan sec 11): the F-inbound entrypoint lookup - the staged
 * rebind plan's rows (sorted by kind + GBA provenance). F-entered
 * scripts are exactly the 3,501 bindings; some are never targeted by
 * any relocation, so the dynamic index is not their surface. */
bool EmeraldScriptCompat_ResolveFEntrypoint(uint32_t kind, uint32_t gbaTarget,
                                            uintptr_t *outAddress)
{
    size_t low;
    size_t high;
    size_t i;

    if (outAddress == NULL || sGeneration == NULL)
        return false;
    low = 0u;
    high = EMERALD_SCRIPT_F_BINDING_COUNT;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        const struct EmeraldScriptCompatStagedFBinding *row =
            &sGeneration->fBindings[mid];
        if (row->kind < kind
         || (row->kind == kind && row->gbaTarget < gbaTarget))
            low = mid + 1u;
        else
            high = mid;
    }
    for (i = low; i < EMERALD_SCRIPT_F_BINDING_COUNT; i++)
    {
        const struct EmeraldScriptCompatStagedFBinding *row =
            &sGeneration->fBindings[i];
        if (row->kind != kind || row->gbaTarget != gbaTarget)
            break;
        if (row->disposition == EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA
         && row->stagedAddress != 0u)
        {
            *outAddress = row->stagedAddress;
            return true;
        }
    }
    return false;
}

size_t EmeraldScriptCompat_GetRangeCount(void)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    return index != NULL ? index->rangeCount : 0u;
}

/* R13-G5 (plan sec 10): the ObjectEventTemplate getter path - a stored
 * GBA provenance resolves to the live module entrypoint. Falls back to
 * the legacy bridge only while no generation is published (the boot
 * path refuses publication failure before any script can run). */
const uint8_t *EmeraldScriptCompat_ResolveObjectScript(uint32_t gbaAddress)
{
    struct EmeraldScriptCompatResolvedTarget target;
    uintptr_t address;

    if (sGeneration == NULL)
        return NULL;
    if (EmeraldScriptCompat_ResolveFEntrypoint(
            EMERALD_SCRIPT_NATIVE_F_OBJECT_EVENT, gbaAddress, &address))
        return (const uint8_t *)address;
    if (EmeraldScriptCompat_ResolveEncodedTarget(
            gbaAddress, EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT, &target)
            == EMERALD_SCRIPT_OK
     && target.liveAddress != 0u)
        return (const uint8_t *)target.liveAddress;
    return NULL;
}

/* The setter path: a live arena pointer reverse-maps to its original
 * GBA export address (never a generation-local handle). */
bool EmeraldScriptCompat_ReverseResolveToGba(uintptr_t address,
                                             uint32_t *outGbaAddress)
{
    char moduleKey[EMERALD_SCRIPT_KEY_CAP];
    uint32_t payloadOffset = 0u;
    uint32_t segmentKind = 0u;
    const struct EmeraldScriptNativeModule *m;
    const struct EmeraldScriptNativeExport *e;

    if (outGbaAddress == NULL)
        return false;
    if (EmeraldScriptCompat_ReverseResolve(address, moduleKey,
                                           sizeof(moduleKey),
                                           &payloadOffset, &segmentKind)
            != EMERALD_SCRIPT_OK)
        return false;
    if (payloadOffset == EMERALD_SCRIPT_NATIVE_OFFSET_NONE)
        return false;
    m = FindModule(moduleKey);
    if (m == NULL)
        return false;
    e = FindExport((uint32_t)(m - kEmeraldScriptCompatTable.modules),
                   payloadOffset);
    if (e == NULL)
        return false;
    *outGbaAddress = e->originalGbaAddress;
    return true;
}

/* The production fast path (plan sec 5): O(log 523) membership over the
 * staged span set - no string copying. */
bool EmeraldScriptCompat_IsArenaAddress(uintptr_t address)
{
    size_t low;
    size_t high;

    if (sGeneration == NULL)
        return false;
    low = 0u;
    high = kEmeraldScriptCompatTable.moduleCount;
    while (low < high)
    {
        size_t mid = low + (high - low) / 2u;
        if (sGeneration->spans[mid].base <= address)
            low = mid + 1u;
        else
            high = mid;
    }
    if (low == 0u)
        return false;
    {
        const struct EmeraldScriptCompatModuleSpan *span =
            &sGeneration->spans[low - 1u];
        size_t spanSize = (size_t)span->payloadSize + span->routingBytes;
        return address - span->base < spanSize;
    }
}

const char *EmeraldScriptCompatStatus_Describe(
    enum EmeraldScriptCompatStatus status)
{
    switch (status)
    {
    case EMERALD_SCRIPT_OK:                 return "ok";
    case EMERALD_SCRIPT_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_SCRIPT_ERR_OUT_OF_MEMORY:  return "out of memory";
    case EMERALD_SCRIPT_ERR_RESOLVE_FAILED: return "snapshot resolve failed";
    case EMERALD_SCRIPT_ERR_UNEXPECTED_OWNERSHIP: return "unexpected ownership";
    case EMERALD_SCRIPT_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT: return "unexpected count";
    case EMERALD_SCRIPT_ERR_TABLE_MISMATCH: return "pack/table mismatch";
    case EMERALD_SCRIPT_ERR_SEGMENT_INVALID: return "invalid segment";
    case EMERALD_SCRIPT_ERR_EXPORT_INVALID: return "invalid export";
    case EMERALD_SCRIPT_ERR_RELOC_INVALID:  return "invalid relocation";
    case EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED: return "target unresolved";
    case EMERALD_SCRIPT_ERR_BOUNDARY_INVALID: return "invalid boundary";
    case EMERALD_SCRIPT_ERR_STAGING_FAILED: return "staging failed";
    case EMERALD_SCRIPT_ERR_UNAVAILABLE:    return "unavailable";
    default:                                return "unknown";
    }
}
