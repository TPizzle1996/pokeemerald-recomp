/* R13-H3 battle-family shadow staging + State-v5 identity bridge.
 *
 * This seam builds complete candidate battle-family generations and
 * publishes them ONLY to private shadow state. Live battle/anim/AI/
 * contest/field-effect execution is never mutated, no range is
 * registered with the production State-v5 range index, and the seam is
 * harness-linked through H3 (production links only the weak state
 * adapter). See emerald_battle_compat.h for the contract.
 *
 * Generation internals: one combined buffer holding the five family
 * arenas per the requested physical layout (0 = GBA-preserving, 1 =
 * tight-packed perturbation), per-module spans, a host-order reverse
 * containment index, a key-order state-identity index, and the arena
 * range descriptors for the five-family dry-run transaction.
 *
 * Transaction order: validate the whole pack surface -> allocate the
 * candidate buffer -> copy every exact byte -> build and re-prove the
 * span/containment indexes -> only then swap the shadow generation.
 * A failed restage leaves the previous generation fully intact. */

#include "emerald/resources/emerald_battle_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"

struct EmeraldBattleCompatGeneration
{
    uint8_t *buffer;
    size_t bufferSize;
    uint32_t layout;
    uint64_t generationId;
    uint8_t *arenaBase[EMERALD_BATTLE_FAMILY_COUNT];
    /* Host-order span index: payload module indices sorted ascending by
     * host start (reverse containment bsearch). */
    uint32_t hostOrder[EMERALD_BATTLE_PAYLOAD_MODULE_COUNT];
    /* Key-order state identity index: ALL 2,089 module indices sorted by
     * their derived key bytes (bsearch for ResolveStateIdentity). */
    uint32_t keyOrder[EMERALD_BATTLE_MODULE_COUNT];
};

static struct EmeraldBattleCompatGeneration *sGeneration;
/* Candidate generation while staging (the comparators need its layout). */
static struct EmeraldBattleCompatGeneration *sStaging;
static uint64_t sNextGenerationId = 1u;

static void NoteFailure(struct EmeraldBattleCompatDiagnostics *diag,
                        const char *stage, const char *canonicalName,
                        uint32_t expectedSchema, uint32_t actualSchema,
                        uint32_t expectedSize, uint32_t actualSize)
{
    if (diag == NULL)
        return;
    memset(diag, 0, sizeof(*diag));
    snprintf(diag->canonicalName, sizeof(diag->canonicalName), "%s",
             canonicalName != NULL ? canonicalName : "");
    snprintf(diag->stage, sizeof(diag->stage), "%s", stage);
    diag->expectedSchema = expectedSchema;
    diag->actualSchema = actualSchema;
    diag->expectedSize = expectedSize;
    diag->actualSize = actualSize;
}

static void DiscardStaging(void)
{
    if (sStaging == NULL)
        return;
    free(sStaging->buffer);
    free(sStaging);
    sStaging = NULL;
}

/* ------------------------------------------------------------------ */
/* Table lookups.                                                      */

static const struct EmeraldBattleNativeModule *FindPayloadModule(
    const char *id)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    size_t lo = 0u;
    size_t hi = t->payloadModuleCount;

    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        int cmp = strcmp(t->modules[mid].id, id);
        if (cmp < 0)
            lo = mid + 1u;
        else if (cmp > 0)
            hi = mid;
        else
            return &t->modules[mid];
    }
    return NULL;
}

static const struct EmeraldBattleNativeAlias *FindAlias(const char *id)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    size_t lo = 0u;
    size_t hi = t->aliasCount;

    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        int cmp = strcmp(t->aliases[mid].id, id);
        if (cmp < 0)
            lo = mid + 1u;
        else if (cmp > 0)
            hi = mid;
        else
            return &t->aliases[mid];
    }
    return NULL;
}

static bool IsHFamilyEntry(const char *canonicalName)
{
    uint32_t i;

    for (i = 0u; i < EMERALD_BATTLE_FAMILY_COUNT; i++)
    {
        if (strncmp(canonicalName,
                    kEmeraldBattleCompatTable.families[i].prefix,
                    strlen(kEmeraldBattleCompatTable.families[i].prefix)) == 0)
            return true;
    }
    return false;
}

static uintptr_t CandidateSpanBase(uint32_t moduleIndex)
{
    const struct EmeraldBattleNativeModule *m =
        &kEmeraldBattleCompatTable.modules[moduleIndex];
    return (uintptr_t)sStaging->arenaBase[m->arena]
         + m->layoutOffset[sStaging->layout];
}

/* ------------------------------------------------------------------ */
/* Phase 1: pack surface + snapshot resolution.                        */

static enum EmeraldBattleCompatStatus ValidatePackSurface(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldBattleCompatDiagnostics *diag)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    size_t entryCount = Gen3ResourcePack_GetEntryCount(pack);
    uint8_t seen[EMERALD_BATTLE_PAYLOAD_MODULE_COUNT / 8u + 1u];
    uint32_t embeddedFound = 0u;
    size_t i;

    if (entryCount == 0u)
    {
        NoteFailure(diag, "validate", "pack-empty", 0u, 0u, 0u, 0u);
        return EMERALD_BATTLE_ERR_UNEXPECTED_COUNT;
    }
    memset(seen, 0, sizeof(seen));
    for (i = 0u; i < entryCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        const struct EmeraldBattleNativeModule *module;
        uint32_t moduleIndex;
        Gen3ResourceHandle handle;
        enum Gen3ResourceResult resolveResult;
        struct Gen3ResourceView view;

        if (entry == NULL || entry->canonicalName == NULL)
            return EMERALD_BATTLE_ERR_TABLE_MISMATCH;
        if (!IsHFamilyEntry(entry->canonicalName))
            continue;
        /* A zero-width alias identity must never carry a pack record. */
        if (FindAlias(entry->canonicalName) != NULL)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_ERR_TABLE_MISMATCH;
        }
        module = FindPayloadModule(entry->canonicalName);
        if (module == NULL)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_ERR_UNEXPECTED_COUNT;
        }
        moduleIndex = (uint32_t)(module - t->modules);
        if (seen[moduleIndex / 8u] & (1u << (moduleIndex % 8u)))
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_ERR_TABLE_MISMATCH;
        }
        seen[moduleIndex / 8u] |= (uint8_t)(1u << (moduleIndex % 8u));
        embeddedFound++;
        if (entry->type != GEN3_RESOURCE_TYPE_STRUCTURED_DATA
         || entry->schema != module->schema
         || entry->payloadSize != module->byteCount
         || entry->payload == NULL || entry->payloadSha256 == NULL
         || memcmp(entry->payloadSha256, module->digest, 32u) != 0)
        {
            NoteFailure(diag, "validate", entry->canonicalName,
                        module->schema, entry->schema,
                        module->byteCount, (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_ERR_TABLE_MISMATCH;
        }
        /* The snapshot must resolve the module with the ROM_BASE
         * provider winning (provenance gate). */
        resolveResult = Gen3ResourceSnapshot_FindHandle(
            snapshot, entry->canonicalName, &handle);
        if (resolveResult != GEN3_RESOURCE_OK)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_ERR_RESOLVE_FAILED;
        }
        resolveResult = Gen3ResourceSnapshot_Resolve(
            snapshot, handle, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            entry->schema, &view);
        if (resolveResult != GEN3_RESOURCE_OK || view.payload == NULL)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_ERR_RESOLVE_FAILED;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diag, "validate", entry->canonicalName,
                        module->schema, view.schema,
                        module->byteCount, (uint32_t)view.payloadSize);
            return EMERALD_BATTLE_ERR_UNEXPECTED_OWNERSHIP;
        }
        if (view.payloadSize != entry->payloadSize)
        {
            NoteFailure(diag, "validate", entry->canonicalName,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, 0u, 0u);
            return EMERALD_BATTLE_ERR_PAYLOAD_SIZE_MISMATCH;
        }
    }
    if (embeddedFound != EMERALD_BATTLE_PAYLOAD_MODULE_COUNT)
    {
        NoteFailure(diag, "validate", "battle-family-pack", 0u, 0u,
                    EMERALD_BATTLE_PAYLOAD_MODULE_COUNT, embeddedFound);
        return EMERALD_BATTLE_ERR_UNEXPECTED_COUNT;
    }
    return EMERALD_BATTLE_OK;
}

/* ------------------------------------------------------------------ */
/* Phase 2: arena allocation + exact byte copy + per-generation index. */

static int CmpHostOrder(const void *a, const void *b)
{
    uintptr_t baseA = CandidateSpanBase(*(const uint32_t *)a);
    uintptr_t baseB = CandidateSpanBase(*(const uint32_t *)b);

    if (baseA < baseB)
        return -1;
    if (baseA > baseB)
        return 1;
    return 0;
}

static int CmpKeyOrder(const void *a, const void *b)
{
    return memcmp(kEmeraldBattleCompatTable.modules[*(const uint32_t *)a].key,
                  kEmeraldBattleCompatTable.modules[*(const uint32_t *)b].key,
                  32);
}

static enum EmeraldBattleCompatStatus StageGeneration(
    const struct Gen3ResourcePack *pack, uint32_t layout,
    struct EmeraldBattleCompatDiagnostics *diag)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    struct EmeraldBattleCompatGeneration *gen;
    size_t bufferSize = 0u;
    uint32_t arena;
    uint32_t i;
    size_t e;
    size_t entryCount;

    if (layout >= EMERALD_BATTLE_LAYOUT_COUNT)
        return EMERALD_BATTLE_ERR_INVALID_ARGUMENT;
    for (arena = 0u; arena < t->arenaCount; arena++)
        bufferSize += t->arenas[arena].layoutSize[layout];
    gen = calloc(1u, sizeof(*gen));
    if (gen == NULL)
        return EMERALD_BATTLE_ERR_OUT_OF_MEMORY;
    gen->layout = layout;
    gen->bufferSize = bufferSize;
    gen->buffer = bufferSize != 0u ? malloc(bufferSize) : NULL;
    if (bufferSize != 0u && gen->buffer == NULL)
    {
        free(gen);
        return EMERALD_BATTLE_ERR_OUT_OF_MEMORY;
    }
    for (arena = 0u; arena < t->arenaCount; arena++)
    {
        const struct EmeraldBattleNativeArena *a = &t->arenas[arena];
        gen->arenaBase[arena] = gen->buffer + a->layoutOffset[layout];
    }
    sStaging = gen;

    /* Copy the exact canonical bytes (one pass over the pack; phase 1
     * already proved every record's identity and digest). */
    entryCount = Gen3ResourcePack_GetEntryCount(pack);
    for (e = 0u; e < entryCount; e++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, e);
        const struct EmeraldBattleNativeModule *module;

        if (entry == NULL || entry->canonicalName == NULL
         || !IsHFamilyEntry(entry->canonicalName))
            continue;
        module = FindPayloadModule(entry->canonicalName);
        if (module == NULL || entry->payload == NULL
         || entry->payloadSize != module->byteCount)
        {
            NoteFailure(diag, "stage", entry->canonicalName, 0u, 0u,
                        module != NULL ? module->byteCount : 0u,
                        (uint32_t)entry->payloadSize);
            DiscardStaging();
            return EMERALD_BATTLE_ERR_TABLE_MISMATCH;
        }
        memcpy((void *)(CandidateSpanBase(
                            (uint32_t)(module - t->modules))),
               entry->payload, module->byteCount);
    }

    /* Host-order span index + strict non-overlap re-proof. */
    for (i = 0u; i < t->payloadModuleCount; i++)
        gen->hostOrder[i] = i;
    qsort(gen->hostOrder, t->payloadModuleCount,
          sizeof(gen->hostOrder[0]), CmpHostOrder);
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleNativeModule *m =
            &t->modules[gen->hostOrder[i]];
        uintptr_t start = CandidateSpanBase(gen->hostOrder[i]);

        if (m->byteCount == 0u
         || start < (uintptr_t)gen->arenaBase[m->arena]
         || m->byteCount
                > (uintptr_t)gen->arenaBase[m->arena]
                    + t->arenas[m->arena].layoutSize[layout] - start)
        {
            DiscardStaging();
            return EMERALD_BATTLE_ERR_STAGING_FAILED;
        }
        if (i != 0u)
        {
            const struct EmeraldBattleNativeModule *prev =
                &t->modules[gen->hostOrder[i - 1u]];
            uintptr_t prevStart = CandidateSpanBase(gen->hostOrder[i - 1u]);
            if (start < prevStart + prev->byteCount)
            {
                NoteFailure(diag, "stage", m->id, 0u, 0u, 0u,
                            (uint32_t)(start - prevStart));
                DiscardStaging();
                return EMERALD_BATTLE_ERR_STAGING_FAILED;
            }
        }
    }
    for (i = 0u; i < t->moduleCount; i++)
        gen->keyOrder[i] = i;
    qsort(gen->keyOrder, t->moduleCount,
          sizeof(gen->keyOrder[0]), CmpKeyOrder);
    gen->generationId = sNextGenerationId++;
    /* Commit: only now does the candidate replace the previous
     * generation, which survived any failure above untouched. */
    if (sGeneration != NULL)
    {
        free(sGeneration->buffer);
        free(sGeneration);
    }
    sGeneration = gen;
    sStaging = NULL;
    return EMERALD_BATTLE_OK;
}

static const uint8_t *ModuleSpanBase(uint32_t moduleIndex)
{
    const struct EmeraldBattleNativeModule *m =
        &kEmeraldBattleCompatTable.modules[moduleIndex];
    if (sGeneration == NULL)
        return NULL;
    return sGeneration->arenaBase[m->arena]
         + m->layoutOffset[sGeneration->layout];
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */

enum EmeraldBattleCompatStatus
EmeraldBattleCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    uint32_t layout,
    struct EmeraldBattleCompatDiagnostics *diagnostics)
{
    enum EmeraldBattleCompatStatus status;

    if (snapshot == NULL || pack == NULL || diagnostics == NULL)
        return EMERALD_BATTLE_ERR_INVALID_ARGUMENT;
    memset(diagnostics, 0, sizeof(*diagnostics));
    status = ValidatePackSurface(snapshot, pack, diagnostics);
    if (status != EMERALD_BATTLE_OK)
        return status;
    return StageGeneration(pack, layout, diagnostics);
}

void EmeraldBattleCompat_ClearMigratedEntries(void)
{
    if (sGeneration == NULL)
        return;
    free(sGeneration->buffer);
    free(sGeneration);
    sGeneration = NULL;
}

void EmeraldBattleCompat_Shutdown(void)
{
    EmeraldBattleCompat_ClearMigratedEntries();
}

uint64_t EmeraldBattleCompat_GetGenerationId(void)
{
    return sGeneration != NULL ? sGeneration->generationId : 0u;
}

bool EmeraldBattleCompat_GetArena(uint32_t family,
                                  const uint8_t **outBase, size_t *outSize)
{
    if (sGeneration == NULL || family >= EMERALD_BATTLE_FAMILY_COUNT
     || outBase == NULL || outSize == NULL)
        return false;
    *outBase = sGeneration->arenaBase[family];
    *outSize = kEmeraldBattleCompatTable.arenas[family]
                   .layoutSize[sGeneration->layout];
    return true;
}

bool EmeraldBattleCompat_GetModuleSpan(const char *moduleKey,
                                       const uint8_t **outBase,
                                       size_t *outSize)
{
    const struct EmeraldBattleNativeModule *module;

    if (moduleKey == NULL || outBase == NULL || outSize == NULL
     || sGeneration == NULL)
        return false;
    module = FindPayloadModule(moduleKey);
    if (module == NULL)
        return false;
    *outBase = ModuleSpanBase((uint32_t)(module
                                 - kEmeraldBattleCompatTable.modules));
    *outSize = module->byteCount;
    return true;
}

enum EmeraldBattleCompatStatus
EmeraldBattleCompat_ReverseResolve(uintptr_t address,
                                   char *outModuleKey, size_t keyCap,
                                   uint32_t *outOffset, uint32_t *outFamily)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    size_t lo;
    size_t hi;

    if (outModuleKey == NULL || outOffset == NULL || outFamily == NULL)
        return EMERALD_BATTLE_ERR_INVALID_ARGUMENT;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_ERR_UNAVAILABLE;
    lo = 0u;
    hi = t->payloadModuleCount;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        const struct EmeraldBattleNativeModule *m =
            &t->modules[sGeneration->hostOrder[mid]];
        uintptr_t start = (uintptr_t)ModuleSpanBase(
            sGeneration->hostOrder[mid]);
        if (address < start)
            hi = mid;
        else if (address >= start + m->byteCount)
            lo = mid + 1u;
        else
        {
            snprintf(outModuleKey, keyCap, "%s", m->id);
            *outOffset = (uint32_t)(address - start);
            *outFamily = m->family;
            return EMERALD_BATTLE_OK;
        }
    }
    return EMERALD_BATTLE_ERR_TARGET_UNRESOLVED;
}

enum EmeraldBattleCompatStatus
EmeraldBattleCompat_ValidateBoundary(const char *moduleKey, uint32_t offset,
                                     uint32_t boundaryKind)
{
    const struct EmeraldBattleNativeModule *module;
    size_t lo;
    size_t hi;

    if (moduleKey == NULL || boundaryKind > EMERALD_BATTLE_BOUNDARY_ENTRYPOINT)
        return EMERALD_BATTLE_ERR_INVALID_ARGUMENT;
    if (FindAlias(moduleKey) != NULL)
        return EMERALD_BATTLE_ERR_ALIAS_IDENTITY;
    module = FindPayloadModule(moduleKey);
    if (module == NULL)
        return EMERALD_BATTLE_ERR_TARGET_UNRESOLVED;
    if (offset >= module->byteCount)
        return EMERALD_BATTLE_ERR_BOUNDARY_INVALID;
    if (boundaryKind == EMERALD_BATTLE_BOUNDARY_ENTRYPOINT)
    {
        /* An export identity at the exact offset. */
        const struct EmeraldBattleNativeExport *rows =
            &kEmeraldBattleCompatTable.exports[module->exportFirst];
        uint32_t i;
        for (i = 0u; i < module->exportCount; i++)
        {
            if (rows[i].payloadOffset == offset)
                return EMERALD_BATTLE_OK;
        }
        return EMERALD_BATTLE_ERR_BOUNDARY_INVALID;
    }
    /* Instruction roles: only bytecode spans decode; an exact interval
     * start is required. */
    if (module->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
        return EMERALD_BATTLE_ERR_BOUNDARY_INVALID;
    lo = module->boundaryFirst;
    hi = lo + module->boundaryCount;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        uint32_t bOff = kEmeraldBattleCompatTable.boundaries[mid].payloadOffset;
        if (offset < bOff)
            hi = mid;
        else if (offset > bOff)
            lo = mid + 1u;
        else
            return EMERALD_BATTLE_OK;
    }
    return EMERALD_BATTLE_ERR_BOUNDARY_INVALID;
}

bool EmeraldBattleCompat_GetStateIdentity(
    const char *moduleKey, Gen3ResourceKey *outKey, uint32_t *outSchema,
    uint32_t *outPayloadSize)
{
    const struct EmeraldBattleNativeModule *module;
    const struct EmeraldBattleNativeAlias *alias;

    if (moduleKey == NULL || outKey == NULL || outSchema == NULL
     || outPayloadSize == NULL)
        return false;
    /* Zero-width aliases canonicalize to their payload owner. */
    alias = FindAlias(moduleKey);
    if (alias != NULL)
        module = &kEmeraldBattleCompatTable.modules[alias->ownerModule];
    else
    {
        module = FindPayloadModule(moduleKey);
        if (module == NULL)
            return false;
    }
    memcpy(outKey, module->key, sizeof(Gen3ResourceKey));
    *outSchema = module->schema;
    *outPayloadSize = module->byteCount;
    return true;
}

enum EmeraldBattleCompatStatus EmeraldBattleCompat_ResolveStateIdentity(
    const Gen3ResourceKey *key, uint32_t resourceType, uint32_t schema,
    uint32_t representationRole, uint32_t payloadOffset,
    uint32_t boundaryRole, uintptr_t *outAddress,
    char *outModuleKey, size_t keyCap)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    size_t lo;
    size_t hi;
    const struct EmeraldBattleNativeModule *module = NULL;

    if (key == NULL || outAddress == NULL || outModuleKey == NULL)
        return EMERALD_BATTLE_ERR_INVALID_ARGUMENT;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_ERR_UNAVAILABLE;
    if (resourceType != GEN3_RESOURCE_TYPE_STRUCTURED_DATA
     || representationRole != EMERALD_RESOURCE_ROLE_CANONICAL)
        return EMERALD_BATTLE_ERR_TABLE_MISMATCH;
    lo = 0u;
    hi = t->moduleCount;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        int cmp = memcmp(t->modules[sGeneration->keyOrder[mid]].key, key,
                         sizeof(Gen3ResourceKey));
        if (cmp < 0)
            lo = mid + 1u;
        else if (cmp > 0)
            hi = mid;
        else
        {
            module = &t->modules[sGeneration->keyOrder[mid]];
            break;
        }
    }
    if (module == NULL)
        return EMERALD_BATTLE_ERR_TARGET_UNRESOLVED;
    /* Zero-width alias keys are valid identities but must never resolve
     * to a pointer - the canonical owner was serialized instead. */
    if (module->byteCount == 0u)
        return EMERALD_BATTLE_ERR_ALIAS_IDENTITY;
    if (module->schema != schema)
        return EMERALD_BATTLE_ERR_UNEXPECTED_SCHEMA;
    if (payloadOffset >= module->byteCount)
        return EMERALD_BATTLE_ERR_BOUNDARY_INVALID;
    if (EmeraldBattleCompat_ValidateBoundary(module->id, payloadOffset,
                                             boundaryRole)
            != EMERALD_BATTLE_OK)
        return EMERALD_BATTLE_ERR_BOUNDARY_INVALID;
    *outAddress = (uintptr_t)ModuleSpanBase(
                      (uint32_t)(module - t->modules))
                + payloadOffset;
    snprintf(outModuleKey, keyCap, "%s", module->id);
    return EMERALD_BATTLE_OK;
}

enum EmeraldBattleCompatStatus EmeraldBattleCompat_ValidateProjectedRanges(
    size_t currentRangeCount, size_t rangeCapacity,
    size_t *outProjectedRangeCount)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    size_t projected;
    uint32_t arena;
    uintptr_t previousEnd = 0u;

    if (outProjectedRangeCount == NULL)
        return EMERALD_BATTLE_ERR_INVALID_ARGUMENT;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_ERR_UNAVAILABLE;
    if (currentRangeCount > rangeCapacity
     || t->arenaCount > rangeCapacity - currentRangeCount)
        return EMERALD_BATTLE_ERR_UNEXPECTED_COUNT;
    projected = currentRangeCount + t->arenaCount;
    for (arena = 0u; arena < t->arenaCount; arena++)
    {
        const struct EmeraldBattleNativeArena *a = &t->arenas[arena];
        uintptr_t start = (uintptr_t)sGeneration->arenaBase[arena];
        uintptr_t end = start + a->layoutSize[sGeneration->layout];
        if (end < start)
            return EMERALD_BATTLE_ERR_STAGING_FAILED;
        if (previousEnd != 0u && start < previousEnd)
            return EMERALD_BATTLE_ERR_STAGING_FAILED;
        previousEnd = end;
    }
    *outProjectedRangeCount = projected;
    return EMERALD_BATTLE_OK;
}

bool EmeraldBattleCompat_GetArenaRanges(
    struct EmeraldBattleCompatArenaRange outRanges[EMERALD_BATTLE_FAMILY_COUNT])
{
    uint32_t arena;

    if (outRanges == NULL || sGeneration == NULL)
        return false;
    for (arena = 0u; arena < EMERALD_BATTLE_FAMILY_COUNT; arena++)
    {
        const struct EmeraldBattleNativeArena *a =
            &kEmeraldBattleCompatTable.arenas[arena];
        struct EmeraldBattleCompatArenaRange *out = &outRanges[arena];

        memset(out, 0, sizeof(*out));
        snprintf(out->canonicalName, sizeof(out->canonicalName),
                 "emerald:%s/@arena",
                 kEmeraldBattleCompatTable.families[arena].name);
        Gen3ResourceId_DeriveKey(out->canonicalName, &out->key);
        out->resourceType = GEN3_RESOURCE_TYPE_STRUCTURED_DATA;
        out->schema = kEmeraldBattleCompatTable.families[arena].schema;
        out->role = EMERALD_RESOURCE_ROLE_CANONICAL;
        out->base = (uintptr_t)sGeneration->arenaBase[arena];
        out->size = a->layoutSize[sGeneration->layout];
    }
    return true;
}

bool EmeraldBattleCompat_GetIndexCounts(struct EmeraldBattleCompatIndexCounts *outCounts)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;

    if (outCounts == NULL)
        return false;
    memset(outCounts, 0, sizeof(*outCounts));
    outCounts->modules = t->moduleCount;
    outCounts->payloadModules = t->payloadModuleCount;
    outCounts->aliases = t->aliasCount;
    outCounts->boundaries = t->boundaryCount;
    outCounts->exportRows = t->exportRowCount;
    outCounts->arenas = t->arenaCount;
    return true;
}

const char *EmeraldBattleCompatStatus_Describe(enum EmeraldBattleCompatStatus status)
{
    switch (status)
    {
    case EMERALD_BATTLE_OK: return "ok";
    case EMERALD_BATTLE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_BATTLE_ERR_OUT_OF_MEMORY: return "out of memory";
    case EMERALD_BATTLE_ERR_RESOLVE_FAILED: return "snapshot resolve failed";
    case EMERALD_BATTLE_ERR_UNEXPECTED_OWNERSHIP: return "module ownership is not ROM_BASE";
    case EMERALD_BATTLE_ERR_PAYLOAD_SIZE_MISMATCH: return "snapshot payload size mismatch";
    case EMERALD_BATTLE_ERR_UNEXPECTED_COUNT: return "pack surface disagrees with the battle inventory";
    case EMERALD_BATTLE_ERR_UNEXPECTED_SCHEMA: return "module schema mismatch";
    case EMERALD_BATTLE_ERR_TABLE_MISMATCH: return "pack record disagrees with the generated table";
    case EMERALD_BATTLE_ERR_ALIAS_IDENTITY: return "zero-width alias must canonicalize to its payload owner";
    case EMERALD_BATTLE_ERR_BOUNDARY_INVALID: return "boundary query refused";
    case EMERALD_BATTLE_ERR_TARGET_UNRESOLVED: return "target could not resolve";
    case EMERALD_BATTLE_ERR_STAGING_FAILED: return "staging failed";
    case EMERALD_BATTLE_ERR_UNAVAILABLE: return "no shadow battle generation";
    default: return "unknown battle compat error";
    }
}
