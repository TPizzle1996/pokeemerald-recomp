/* R13-H4/H5 production live seam: battle + battle-anim + field-effect
 * arenas.
 *
 * H4 made the FIRST live H-family cutover (anim + field-effect, 2
 * ranges). H5 adds the battle-script family: the seam stages the
 * battle (14,413 B hull), anim (63,811 B) and field-effect (817 B)
 * arena payloads from the pack into one host buffer, registers exactly
 * three live ranges (6,377 -> 6,380), publishes live execution, and
 * then resolves every battle / animation / field-effect script pointer
 * the interpreters read through a typed, metadata-driven path - NEVER
 * through HostResolveGbaAddr identity arithmetic and NEVER back to
 * compiled payloads (brief sec 8/9/23).
 *
 * Transaction order (brief sec 4): validate pack surfaces -> stage the
 * generation -> register the 3 ranges -> publish. No partial cutover:
 * every failure before publish leaves the compiled runtime untouched;
 * every failure after ranges registered rolls the ranges back by exact
 * identity. The loader drives this and refuses the session on any
 * failure (see emerald_runtime_loader.c).
 *
 * The weak state-adapter bridge (emerald_battle_state.c) resolves
 * through the six EmeraldBattleCompat_* functions this seam provides -
 * the same symbols the harness-only H3 shadow seam provides, and the
 * two never link into the same binary. The adapter gates battle/anim/
 * FE surfaces live; AI/contest surfaces report NOT_BATTLE
 * (family-live policy).
 *
 * H5 battle resolution specifics:
 *  - the central pointer-operand reader (ReadPointerOperand) backs
 *    T1_READ_PTR/T2_READ_PTR on linux64: battle-arena operand words
 *    resolve through the relocation source index with the word check
 *    (brief sec 6/7); legal NULL literals (no reloc row, word 0)
 *    return NULL; anything else is a hard fail-closed refusal;
 *  - EWRAM operands resolve as semantic base + validated addend via
 *    the native-address TU (battle_live_native.generated.c); no
 *    GBA->host arithmetic anywhere (brief sec 15/16);
 *  - the 5 battle routing tables are arena-owned: rows are read from
 *    the canonical arena bytes and each row word resolves as a
 *    SCRIPT_TARGET root (brief sec 12);
 *  - direct C label references (BattleScript_Get) resolve through the
 *    compiled-label map (native symbol -> canonical GBA word -> arena
 *    root export; brief sec 11/14).
 *
 * Refuse-only bindings: four engine bindings whose native symbols do
 * not exist in this fork (upstream battle_anim_mist.c /
 * battle_anim_terrain.c are absent) carry address 0 and the resolver
 * hard-refuses them - the affected moves' animations end cleanly and
 * the battle continues (fail-closed, brief sec 10/24).
 */

#include "emerald/resources/emerald_battle_live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_trainer_native_compat.h" /* GetRangeIndex */
#include "emerald/resources/battle_live.generated.h"

/* host_memory.c (GbaAddr == uint32_t; the seam stays global.h-free). */
void *HostResolveGbaAddr(uint32_t addr);

/* Weak coupling to the production animation interpreter (battle_anim.c):
 * the seam's Publish calls RegisterStateLayout to hand the State-v5
 * adapter its live surface layout, and StageGeneration probes
 * IsAnimActive to refuse replacing a generation a running animation VM
 * is executing from. Harness builds without battle_anim.c leave both
 * NULL (Publish still works; tests bind their own fixtures). */
extern void BattleAnimCompat_RegisterStateLayout(void);
#pragma weak BattleAnimCompat_RegisterStateLayout
extern bool BattleAnimCompat_IsAnimActive(void);
#pragma weak BattleAnimCompat_IsAnimActive
/* H5: the battle interpreter's State-v5 surface layout + the battle
 * quiescence probe (an in-progress battle refuses generation
 * replacement). Weak like the anim hooks. */
extern void BattleScriptCompat_RegisterStateLayout(void);
#pragma weak BattleScriptCompat_RegisterStateLayout
extern bool BattleScriptCompat_IsBattleActive(void);
#pragma weak BattleScriptCompat_IsBattleActive

struct EmeraldBattleLiveGeneration
{
    uint8_t *buffer;
    size_t bufferSize;
    uint32_t layout;
    uint64_t generationId;
    uint8_t *arenaBase[EMERALD_BATTLE_LIVE_ARENA_COUNT];
    /* Host-order span index: payload module indices sorted ascending by
     * host start (reverse containment bsearch for ResolveOperand and
     * ReverseResolve). */
    uint32_t hostOrder[EMERALD_BATTLE_LIVE_PAYLOAD_MODULE_COUNT];
    /* GBA-order span index: payload module indices sorted ascending by
     * GBA start (SCRIPT_TARGET word containment bsearch). */
    uint32_t gbaOrder[EMERALD_BATTLE_LIVE_PAYLOAD_MODULE_COUNT];
    /* Key-order state identity index: ALL module indices (payload +
     * zero-width aliases) sorted by derived key bytes (ResolveStateIdentity
     * bsearch). */
    uint32_t keyOrder[EMERALD_BATTLE_LIVE_MODULE_COUNT];
};

static struct EmeraldBattleLiveGeneration *sGeneration;
/* Candidate generation while staging (the comparators need its layout). */
static struct EmeraldBattleLiveGeneration *sStaging;
static uint64_t sNextGenerationId = 1u;
static bool sPublished;
static bool sRangesRegistered;

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

static const struct EmeraldBattleLiveModule *FindPayloadModule(
    const char *id)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
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

static const struct EmeraldBattleLiveAlias *FindAlias(const char *id)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
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

/* The five live families: pack records for battle + anim + battle-AI +
 * contest-AI + FE (R13-H6 cut over the AI families). */
static bool IsLiveFamilyEntry(const char *canonicalName)
{
    return strncmp(canonicalName, "emerald:battle-script/",
                   strlen("emerald:battle-script/")) == 0
        || strncmp(canonicalName, "emerald:battle-anim-script/",
                   strlen("emerald:battle-anim-script/")) == 0
        || strncmp(canonicalName, "emerald:battle-ai/",
                   strlen("emerald:battle-ai/")) == 0
        || strncmp(canonicalName, "emerald:contest-ai/",
                   strlen("emerald:contest-ai/")) == 0
        || strncmp(canonicalName, "emerald:field-effect-script/",
                   strlen("emerald:field-effect-script/")) == 0;
}

/* Family gate: a family is live when a live arena row carries it. */
static bool IsLiveFamily(uint32_t family)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t arena;

    for (arena = 0u; arena < t->arenaCount; arena++)
    {
        if (t->arenas[arena].family == family)
            return true;
    }
    return false;
}

static void ArenaCanonicalName(uint32_t arena, char *out, size_t cap)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    const char *name;

    if (arena >= t->arenaCount)
        name = "";
    else if (t->arenas[arena].family == EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT)
        name = "battle-script";
    else if (t->arenas[arena].family == EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT)
        name = "battle-anim-script";
    else if (t->arenas[arena].family == EMERALD_BATTLE_FAMILY_BATTLE_AI)
        name = "battle-ai";
    else if (t->arenas[arena].family == EMERALD_BATTLE_FAMILY_CONTEST_AI)
        name = "contest-ai";
    else
        name = "field-effect-script";
    snprintf(out, cap, "emerald:%s/@arena", name);
}

static uintptr_t CandidateSpanBase(uint32_t moduleIndex)
{
    const struct EmeraldBattleLiveModule *m =
        &kEmeraldBattleLiveTable.modules[moduleIndex];
    return (uintptr_t)sStaging->arenaBase[m->arena]
         + m->layoutOffset[sStaging->layout];
}

static const uint8_t *SpanBase(uint32_t moduleIndex)
{
    const struct EmeraldBattleLiveModule *m =
        &kEmeraldBattleLiveTable.modules[moduleIndex];
    if (sGeneration == NULL)
        return NULL;
    return sGeneration->arenaBase[m->arena]
         + m->layoutOffset[sGeneration->layout];
}

/* Unaligned-safe 32-bit read (jumpargeq operands sit at a +3 offset). */
static uint32_t ReadWord(const uint8_t *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

/* ------------------------------------------------------------------ */
/* Phase 1: pack surface + snapshot resolution.                        */

static enum EmeraldBattleLiveStatus ValidatePackSurface(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldBattleCompatDiagnostics *diag)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    size_t entryCount = Gen3ResourcePack_GetEntryCount(pack);
    uint8_t seen[EMERALD_BATTLE_LIVE_PAYLOAD_MODULE_COUNT / 8u + 1u];
    uint32_t embeddedFound = 0u;
    size_t i;

    if (entryCount == 0u)
    {
        NoteFailure(diag, "validate", "pack-empty", 0u, 0u, 0u, 0u);
        return EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_COUNT;
    }
    memset(seen, 0, sizeof(seen));
    for (i = 0u; i < entryCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        const struct EmeraldBattleLiveModule *module;
        uint32_t moduleIndex;
        Gen3ResourceHandle handle;
        enum Gen3ResourceResult resolveResult;
        struct Gen3ResourceView view;

        if (entry == NULL || entry->canonicalName == NULL)
            return EMERALD_BATTLE_LIVE_ERR_TABLE_MISMATCH;
        if (!IsLiveFamilyEntry(entry->canonicalName))
            continue;
        /* A zero-width alias identity must never carry a pack record. */
        if (FindAlias(entry->canonicalName) != NULL)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_LIVE_ERR_TABLE_MISMATCH;
        }
        module = FindPayloadModule(entry->canonicalName);
        if (module == NULL)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_COUNT;
        }
        moduleIndex = (uint32_t)(module - t->modules);
        if (seen[moduleIndex / 8u] & (1u << (moduleIndex % 8u)))
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_LIVE_ERR_TABLE_MISMATCH;
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
            return EMERALD_BATTLE_LIVE_ERR_TABLE_MISMATCH;
        }
        /* The snapshot must resolve the module with the ROM_BASE
         * provider winning (provenance gate). */
        resolveResult = Gen3ResourceSnapshot_FindHandle(
            snapshot, entry->canonicalName, &handle);
        if (resolveResult != GEN3_RESOURCE_OK)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_LIVE_ERR_RESOLVE_FAILED;
        }
        resolveResult = Gen3ResourceSnapshot_Resolve(
            snapshot, handle, GEN3_RESOURCE_TYPE_STRUCTURED_DATA,
            entry->schema, &view);
        if (resolveResult != GEN3_RESOURCE_OK || view.payload == NULL)
        {
            NoteFailure(diag, "validate", entry->canonicalName, 0u, 0u, 0u,
                        (uint32_t)entry->payloadSize);
            return EMERALD_BATTLE_LIVE_ERR_RESOLVE_FAILED;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diag, "validate", entry->canonicalName,
                        module->schema, view.schema,
                        module->byteCount, (uint32_t)view.payloadSize);
            return EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_OWNERSHIP;
        }
        if (view.payloadSize != entry->payloadSize)
        {
            NoteFailure(diag, "validate", entry->canonicalName,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, 0u, 0u);
            return EMERALD_BATTLE_LIVE_ERR_PAYLOAD_SIZE_MISMATCH;
        }
    }
    if (embeddedFound != EMERALD_BATTLE_LIVE_PAYLOAD_MODULE_COUNT)
    {
        NoteFailure(diag, "validate", "battle-live-pack", 0u, 0u,
                    EMERALD_BATTLE_LIVE_PAYLOAD_MODULE_COUNT, embeddedFound);
        return EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_COUNT;
    }
    return EMERALD_BATTLE_LIVE_OK;
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

static int CmpGbaOrder(const void *a, const void *b)
{
    uint32_t startA = kEmeraldBattleLiveTable
                          .modules[*(const uint32_t *)a].gbaStart;
    uint32_t startB = kEmeraldBattleLiveTable
                          .modules[*(const uint32_t *)b].gbaStart;

    if (startA < startB)
        return -1;
    if (startA > startB)
        return 1;
    return 0;
}

static int CmpKeyOrder(const void *a, const void *b)
{
    return memcmp(kEmeraldBattleLiveTable.modules[*(const uint32_t *)a].key,
                  kEmeraldBattleLiveTable.modules[*(const uint32_t *)b].key,
                  32);
}

static enum EmeraldBattleLiveStatus StageGeneration(
    const struct Gen3ResourcePack *pack, uint32_t layout,
    struct EmeraldBattleCompatDiagnostics *diag)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    struct EmeraldBattleLiveGeneration *gen;
    size_t bufferSize = 0u;
    uint32_t arena;
    uint32_t i;
    size_t e;
    size_t entryCount;

    if (layout >= EMERALD_BATTLE_LIVE_LAYOUT_COUNT)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    /* Brief sec 25/26 quiescence: a running animation VM or an
     * in-progress battle whose instruction pointers live in the current
     * buffer must never be orphaned by a replacement. The weak probes
     * are defined by the production interpreters; NULL (harness builds
     * without battle_anim.c / battle_main.c) skips. */
    if (sPublished && BattleAnimCompat_IsAnimActive != NULL
     && BattleAnimCompat_IsAnimActive())
        return EMERALD_BATTLE_LIVE_ERR_BUSY;
    if (sPublished && BattleScriptCompat_IsBattleActive != NULL
     && BattleScriptCompat_IsBattleActive())
        return EMERALD_BATTLE_LIVE_ERR_BUSY;
    for (arena = 0u; arena < t->arenaCount; arena++)
        bufferSize += t->arenas[arena].layoutSize[layout];
    gen = calloc(1u, sizeof(*gen));
    if (gen == NULL)
        return EMERALD_BATTLE_LIVE_ERR_OUT_OF_MEMORY;
    gen->layout = layout;
    gen->bufferSize = bufferSize;
    gen->buffer = bufferSize != 0u ? malloc(bufferSize) : NULL;
    if (bufferSize != 0u && gen->buffer == NULL)
    {
        free(gen);
        return EMERALD_BATTLE_LIVE_ERR_OUT_OF_MEMORY;
    }
    for (arena = 0u; arena < t->arenaCount; arena++)
    {
        const struct EmeraldBattleLiveArena *a = &t->arenas[arena];
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
        const struct EmeraldBattleLiveModule *module;

        if (entry == NULL || entry->canonicalName == NULL
         || !IsLiveFamilyEntry(entry->canonicalName))
            continue;
        module = FindPayloadModule(entry->canonicalName);
        if (module == NULL || entry->payload == NULL
         || entry->payloadSize != module->byteCount)
        {
            NoteFailure(diag, "stage", entry->canonicalName, 0u, 0u,
                        module != NULL ? module->byteCount : 0u,
                        (uint32_t)entry->payloadSize);
            DiscardStaging();
            return EMERALD_BATTLE_LIVE_ERR_TABLE_MISMATCH;
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
        const struct EmeraldBattleLiveModule *m =
            &t->modules[gen->hostOrder[i]];
        uintptr_t start = CandidateSpanBase(gen->hostOrder[i]);

        if (m->byteCount == 0u
         || start < (uintptr_t)gen->arenaBase[m->arena]
         || m->byteCount
                > (uintptr_t)gen->arenaBase[m->arena]
                    + t->arenas[m->arena].layoutSize[layout] - start)
        {
            DiscardStaging();
            return EMERALD_BATTLE_LIVE_ERR_STAGING_FAILED;
        }
        if (i != 0u)
        {
            const struct EmeraldBattleLiveModule *prev =
                &t->modules[gen->hostOrder[i - 1u]];
            uintptr_t prevStart = CandidateSpanBase(gen->hostOrder[i - 1u]);
            if (start < prevStart + prev->byteCount)
            {
                NoteFailure(diag, "stage", m->id, 0u, 0u, 0u,
                            (uint32_t)(start - prevStart));
                DiscardStaging();
                return EMERALD_BATTLE_LIVE_ERR_STAGING_FAILED;
            }
        }
    }
    for (i = 0u; i < t->payloadModuleCount; i++)
        gen->gbaOrder[i] = i;
    qsort(gen->gbaOrder, t->payloadModuleCount,
          sizeof(gen->gbaOrder[0]), CmpGbaOrder);
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
    return EMERALD_BATTLE_LIVE_OK;
}

/* ------------------------------------------------------------------ */
/* Boundary validation (the H3 identity model, live table).            */

static enum EmeraldBattleLiveStatus ValidateBoundaryInternal(
    uint32_t moduleIndex, uint32_t offset, uint32_t boundaryKind)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    const struct EmeraldBattleLiveModule *module = &t->modules[moduleIndex];
    size_t lo;
    size_t hi;

    if (boundaryKind > EMERALD_BATTLE_BOUNDARY_ENTRYPOINT)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (offset >= module->byteCount)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    if (boundaryKind == EMERALD_BATTLE_BOUNDARY_ENTRYPOINT)
    {
        /* An export identity at the exact offset. */
        const struct EmeraldBattleNativeExport *rows =
            &t->exports[module->exportFirst];
        uint32_t i;
        for (i = 0u; i < module->exportCount; i++)
        {
            if (rows[i].payloadOffset == offset)
                return EMERALD_BATTLE_LIVE_OK;
        }
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    }
    /* Instruction roles: only bytecode spans decode; an exact interval
     * start is required. */
    if (module->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    lo = module->boundaryFirst;
    hi = lo + module->boundaryCount;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        uint32_t bOff = t->boundaries[mid].payloadOffset;
        if (offset < bOff)
            hi = mid;
        else if (offset > bOff)
            lo = mid + 1u;
        else
            return EMERALD_BATTLE_LIVE_OK;
    }
    return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
}

/* ------------------------------------------------------------------ */
/* Runtime resolution.                                                 */

static bool ReverseContain(uintptr_t address, uint32_t *outModuleIndex,
                           uint32_t *outModuleOffset)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    size_t lo = 0u;
    size_t hi = t->payloadModuleCount;

    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        const struct EmeraldBattleLiveModule *m =
            &t->modules[sGeneration->hostOrder[mid]];
        uintptr_t start = (uintptr_t)SpanBase(sGeneration->hostOrder[mid]);
        if (address < start)
            hi = mid;
        else if (address >= start + m->byteCount)
            lo = mid + 1u;
        else
        {
            *outModuleIndex = sGeneration->hostOrder[mid];
            *outModuleOffset = (uint32_t)(address - start);
            return true;
        }
    }
    return false;
}

static bool GbaContain(uint32_t address, uint32_t *outModuleIndex,
                       uint32_t *outModuleOffset)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    size_t lo = 0u;
    size_t hi = t->payloadModuleCount;

    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        const struct EmeraldBattleLiveModule *m =
            &t->modules[sGeneration->gbaOrder[mid]];
        if (address < m->gbaStart)
            hi = mid;
        else if (address >= m->gbaStart + m->byteCount)
            lo = mid + 1u;
        else
        {
            *outModuleIndex = sGeneration->gbaOrder[mid];
            *outModuleOffset = address - m->gbaStart;
            return true;
        }
    }
    return false;
}

static const struct EmeraldBattleLiveReloc *FindReloc(
    uint32_t moduleIndex, uint32_t operandOffset)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    const struct EmeraldBattleLiveModule *m = &t->modules[moduleIndex];
    size_t lo = m->relocFirst;
    size_t hi = lo + m->relocCount;

    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        uint32_t rowOffset = t->relocs[mid].operandOffset;
        if (operandOffset < rowOffset)
            hi = mid;
        else if (operandOffset > rowOffset)
            lo = mid + 1u;
        else
            return &t->relocs[mid];
    }
    return NULL;
}

enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveScriptTarget(
    uint32_t family, uint32_t word, uintptr_t *outPointer)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    size_t lo;
    size_t hi;
    uint32_t moduleIndex;
    uint32_t moduleOffset;

    if (outPointer == NULL)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!IsLiveFamily(family))
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!sPublished)
        return EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE;
    /* Brief sec 8: the raw operand must be a known SCRIPT_TARGET word
     * (metadata gate) before any containment is attempted. */
    lo = 0u;
    hi = t->scriptTargetWordCount;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2u;
        if (word < t->scriptTargetWords[mid])
            hi = mid;
        else if (word > t->scriptTargetWords[mid])
            lo = mid + 1u;
        else
            goto known;
    }
    return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
known:
    if (!GbaContain(word, &moduleIndex, &moduleOffset))
        return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
    if (t->modules[moduleIndex].family != family)
        return EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY;
    /* Data targets (battle-AI if_in_* byte/hword list tables, 38 rows)
     * have NO instruction boundaries - the operand must denote the
     * module root (offset 0). Bytecode targets validate the boundary
     * (R13-H6 sec 6). */
    if (t->modules[moduleIndex].mapKind == EMERALD_BATTLE_MAP_DATA)
    {
        if (moduleOffset != 0u)
            return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    }
    else if (ValidateBoundaryInternal(moduleIndex, moduleOffset,
                                 EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
            != EMERALD_BATTLE_LIVE_OK)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    *outPointer = (uintptr_t)SpanBase(moduleIndex) + moduleOffset;
    return EMERALD_BATTLE_LIVE_OK;
}

enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveLaunchTarget(
    uint32_t family, uint32_t word, uintptr_t *outPointer)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t moduleIndex;
    uint32_t moduleOffset;

    if (outPointer == NULL)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!IsLiveFamily(family))
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!sPublished)
        return EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE;
    if (!GbaContain(word, &moduleIndex, &moduleOffset))
        return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
    if (t->modules[moduleIndex].family != family)
        return EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY;
    /* The launch word must denote the module's root export (offset 0). */
    if (moduleOffset != 0u)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    if (ValidateBoundaryInternal(moduleIndex, 0u,
                                 EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
            != EMERALD_BATTLE_LIVE_OK)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    *outPointer = (uintptr_t)SpanBase(moduleIndex);
    return EMERALD_BATTLE_LIVE_OK;
}

enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveOperand(
    uintptr_t operandHostAddress, uintptr_t *outPointer)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    const struct EmeraldBattleLiveReloc *reloc;
    uint32_t moduleIndex;
    uint32_t moduleOffset;

    if (outPointer == NULL)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!sPublished)
        return EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE;
    /* Reverse containment: the operand must live in a live payload span.
     * A compiled (non-arena) operand pointer is refused - never a
     * fallback to the compiled runtime (brief sec 23). */
    if (!ReverseContain(operandHostAddress, &moduleIndex, &moduleOffset))
        return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
    reloc = FindReloc(moduleIndex, moduleOffset);
    if (reloc == NULL)
        return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
    /* The staged bytes are digest-identical to the canonical .bin, but
     * the word at the operand offset must still equal the row's expected
     * word (corrupt-buffer / stale-buffer gate, brief sec 24). */
    if (ReadWord((const uint8_t *)operandHostAddress) != reloc->expectedWord)
        return EMERALD_BATTLE_LIVE_ERR_REFUSED;
    if (reloc->relocClass == EMERALD_BATTLE_LIVE_RELOC_SCRIPT_TARGET)
    {
        /* Data targets (battle-AI if_in_* list tables) carry no
         * instruction boundaries; the row pins targetOffset 0. */
        if (t->modules[reloc->target].mapKind == EMERALD_BATTLE_MAP_DATA)
        {
            if (reloc->targetOffset != 0u)
                return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
        }
        else if (ValidateBoundaryInternal(reloc->target, reloc->targetOffset,
                                     EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
                != EMERALD_BATTLE_LIVE_OK)
            return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
        *outPointer = (uintptr_t)SpanBase(reloc->target) + reloc->targetOffset;
        return EMERALD_BATTLE_LIVE_OK;
    }
    {
        const struct EmeraldBattleLiveBinding *binding =
            &t->bindings[reloc->target];
        uintptr_t address = binding->address;

        /* A (EWRAM) and battle B (string-ID table) rows resolve through
         * the native-address TU; the platform-neutral table carries 0. */
        if (address == 0u)
            address = EmeraldBattleLiveNative_BindingAddress(reloc->target);
        if (address == 0u)
            return EMERALD_BATTLE_LIVE_ERR_REFUSED;
        /* Brief sec 15/16: EWRAM resolves as semantic base + validated
         * addend - never GBA->host arithmetic. The stored word already
         * pinned (base + addend) in the generated table; the addend
         * must stay inside the symbol's ELF size bound. */
        if (binding->letter == 'A')
        {
            if (binding->addend >= binding->allowedOffset)
                return EMERALD_BATTLE_LIVE_ERR_REFUSED;
            *outPointer = address + binding->addend;
        }
        else
        {
            *outPointer = address;
        }
        return EMERALD_BATTLE_LIVE_OK;
    }
}

/* ------------------------------------------------------------------ */
/* H5/H6 routing + direct entry + compiled-label translation.          */

enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveRoutingTarget(
    uint32_t tableWord, uint32_t rowIndex, uintptr_t *outPointer)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t moduleIndex;
    uint32_t moduleOffset;
    const struct EmeraldBattleLiveModule *module;
    uint32_t rowCount;
    uint32_t word;

    if (outPointer == NULL)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!sPublished)
        return EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE;
    if (!GbaContain(tableWord, &moduleIndex, &moduleOffset))
        return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
    module = &t->modules[moduleIndex];
    /* The table word must denote the routing module's own start. Any
     * routing module family is accepted here (battle move-effects /
     * ball-throw / using-item / running-by-item / safari-actions +
     * battle-AI + contest-AI entry tables); the ROW word then resolves
     * as a SCRIPT_TARGET root of the table's OWN family - a cross-
     * family row word is refused by ResolveScriptTarget's family gate
     * (R13-H6 sec 15/16). */
    if (moduleOffset != 0u
     || module->mapKind != EMERALD_BATTLE_MAP_ROUTING)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    rowCount = module->byteCount / 4u;
    if (rowIndex >= rowCount)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    /* The canonical row is read from the ARENA - the compiled routing
     * tables are dead at runtime (brief sec 12/24). */
    word = ReadWord(SpanBase(moduleIndex) + (size_t)rowIndex * 4u);
    return EmeraldBattleLive_ResolveScriptTarget(module->family, word,
                                                 outPointer);
}

enum EmeraldBattleLiveStatus EmeraldBattleLive_GetBattleScript(
    uint32_t word, uintptr_t *outPointer)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t moduleIndex;
    uint32_t moduleOffset;

    if (outPointer == NULL)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!sPublished)
        return EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE;
    if (!GbaContain(word, &moduleIndex, &moduleOffset))
        return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
    if (t->modules[moduleIndex].family != EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT)
        return EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY;
    /* A direct entry is an export identity at the exact offset (root or
     * alias export; interior instructions never enter here). */
    if (ValidateBoundaryInternal(moduleIndex, moduleOffset,
                                 EMERALD_BATTLE_BOUNDARY_ENTRYPOINT)
            != EMERALD_BATTLE_LIVE_OK)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    *outPointer = (uintptr_t)SpanBase(moduleIndex) + moduleOffset;
    return EMERALD_BATTLE_LIVE_OK;
}

enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveCompiledLabel(
    const void *nativeSymbolAddress, uintptr_t *outPointer)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t i;

    if (outPointer == NULL || nativeSymbolAddress == NULL)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!sPublished)
        return EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED;
    /* Linear scan over the label map (199 rows); entry sites are not
     * hot loops and the map keeps the platform-neutral table free of
     * link-time addresses. R13-H7: on linux64 the compiled label
     * symbols are REMOVED from the link - every C reference is a
     * canonical-word macro, so the scan compares words (the argument
     * is the macro-expanded word constant); off-linux64 the argument
     * is the compiled symbol's host address. */
    for (i = 0u; i < t->labelCount; i++)
    {
#if defined(NATIVE_LINUX) && defined(LINUX64) && (LINUX64 == 1)
        if (t->labels[i].word == (uint32_t)(uintptr_t)nativeSymbolAddress)
#else
        if (EmeraldBattleLiveNative_LabelAddress(i)
                == (uintptr_t)nativeSymbolAddress)
#endif
            return EmeraldBattleLive_GetBattleScript(t->labels[i].word,
                                                     outPointer);
    }
    return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
}

/* R13-H7 (brief sec 2/§12): resolve a NON-pointer data module (e.g.
 * gMovesWithQuietBGM) by its canonical root word - the compiled
 * symbols are removed from the native link, so the read site fetches
 * the span from the arena. The root word must denote the module's own
 * start and the module must not be bytecode (a script module root is a
 * launch target, not raw data). */
enum EmeraldBattleLiveStatus EmeraldBattleLive_ResolveModuleData(
    uint32_t rootWord, uintptr_t *outSpan, uint32_t *outByteCount)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t moduleIndex;
    uint32_t moduleOffset;

    if (outSpan == NULL || outByteCount == NULL)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    if (!sPublished)
        return EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED;
    if (sGeneration == NULL)
        return EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE;
    if (!GbaContain(rootWord, &moduleIndex, &moduleOffset))
        return EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED;
    if (moduleOffset != 0u)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    if (t->modules[moduleIndex].mapKind == EMERALD_BATTLE_MAP_BYTECODE)
        return EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID;
    *outSpan = (uintptr_t)SpanBase(moduleIndex);
    *outByteCount = t->modules[moduleIndex].byteCount;
    return EMERALD_BATTLE_LIVE_OK;
}

/* ------------------------------------------------------------------ */
/* Fail-closed C-site accessors (BattleScript_Get / routing reads).    */

const uint8_t *EmeraldBattleLive_BattleScriptPtr(
    const void *nativeSymbolAddress)
{
    uintptr_t pointer = 0u;

    if (EmeraldBattleLive_ResolveCompiledLabel(nativeSymbolAddress,
                                               &pointer)
            != EMERALD_BATTLE_LIVE_OK)
    {
        fprintf(stderr,
                "emerald battle live: compiled battle label @ %p is not a "
                "mapped live root - refusing (no compiled fallback)\n",
                nativeSymbolAddress);
        abort();
    }
    return (const uint8_t *)pointer;
}

const uint8_t *EmeraldBattleLive_RoutingScriptPtr(uint32_t tableWord,
                                                  uint32_t rowIndex)
{
    uintptr_t pointer = 0u;

    if (EmeraldBattleLive_ResolveRoutingTarget(tableWord, rowIndex, &pointer)
            != EMERALD_BATTLE_LIVE_OK)
    {
        fprintf(stderr,
                "emerald battle live: routing row (table 0x%08x[%u]) "
                "refused - no compiled fallback\n", tableWord, rowIndex);
        abort();
    }
    return (const uint8_t *)pointer;
}

/* ------------------------------------------------------------------ */
/* Central pointer-operand reader (T1_READ_PTR/T2_READ_PTR body).      */

void *EmeraldBattleLive_ReadPointerOperand(const uint8_t *operandAddress)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t word;
    uint32_t moduleIndex;
    uint32_t moduleOffset;
    uintptr_t pointer;

    word = ReadWord(operandAddress);
    /* Operand words read OUTSIDE any live arena span keep the legacy
     * path: host-pointer words pass through unchanged. */
    if (!sPublished || sGeneration == NULL
     || !ReverseContain((uintptr_t)operandAddress, &moduleIndex,
                        &moduleOffset))
        return HostResolveGbaAddr(word);
    /* R13-H6: every family in the live table (battle, anim, battle-AI,
     * contest-AI, FE) resolves typed - the AI interpreters' control-
     * flow operands (branch/jump/call/tail-call) all funnel through
     * this central reader (brief sec 7). A module outside the live
     * family set keeps the legacy path. */
    if (!IsLiveFamily(t->modules[moduleIndex].family))
        return HostResolveGbaAddr(word);
    /* Inside a live arena span: typed resolution only. Legal
     * NULL literals (a pointer operand slot with a stored 0 and no
     * relocation row - the H1 NULL-literal census) return NULL; a
     * nonzero word with no relocation row is a hard refusal. */
    if (FindReloc(moduleIndex, moduleOffset) == NULL)
    {
        if (word == 0u)
            return NULL;
        fprintf(stderr,
                "emerald battle live: pointer operand @ %p (module %u"
                "+%u) has no relocation row (word 0x%08x) - refusing\n",
                (const void *)operandAddress, moduleIndex, moduleOffset,
                word);
        abort();
    }
    if (EmeraldBattleLive_ResolveOperand((uintptr_t)operandAddress,
                                         &pointer) != EMERALD_BATTLE_LIVE_OK)
    {
        fprintf(stderr,
                "emerald battle live: pointer operand @ %p (word 0x%08x) "
                "refused - no compiled fallback\n",
                (const void *)operandAddress, word);
        abort();
    }
    return (void *)pointer;
}

/* ------------------------------------------------------------------ */
/* Range registration.                                                 */

enum EmeraldBattleLiveStatus EmeraldBattleLive_RegisterRanges(void)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t registered = 0u;
    uint32_t arena;

    if (sGeneration == NULL)
        return EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE;
    if (index == NULL)
        return EMERALD_BATTLE_LIVE_ERR_STAGING_FAILED;
    if (sRangesRegistered)
        EmeraldBattleLive_UnregisterRanges();
    if (t->arenaCount
            > EMERALD_RESOURCE_RANGE_INDEX_MAX_RANGES - index->rangeCount)
        return EMERALD_BATTLE_LIVE_ERR_RANGE_REGISTRATION;
    /* Sorted ascending spans register non-overlapping (anim, then FE);
     * any conflict is a hard refusal with the whole registration rolled
     * back by exact identity. */
    for (arena = 0u; arena < t->arenaCount; arena++)
    {
        const struct EmeraldBattleLiveArena *a = &t->arenas[arena];
        char canonicalName[96];

        ArenaCanonicalName(arena, canonicalName, sizeof(canonicalName));
        if (!EmeraldResourceRangeIndex_RegisterSpan(
                index, (uintptr_t)sGeneration->arenaBase[arena],
                a->layoutSize[sGeneration->layout], canonicalName,
                GEN3_RESOURCE_TYPE_STRUCTURED_DATA, a->schema,
                EMERALD_RESOURCE_ROLE_CANONICAL))
            goto fail;
        registered++;
    }
    sRangesRegistered = true;
    return EMERALD_BATTLE_LIVE_OK;

fail:
    /* Roll back the partial registration by exact resource key. */
    while (registered > 0u)
    {
        char canonicalName[96];

        registered--;
        ArenaCanonicalName(registered, canonicalName, sizeof(canonicalName));
        EmeraldBattleLive_UnregisterRange(canonicalName);
    }
    return EMERALD_BATTLE_LIVE_ERR_RANGE_REGISTRATION;
}

/* Identity-based single-range removal: find the exact key and remove it
 * wherever it sits (position-independent - the G2 text-seam lesson). */
void EmeraldBattleLive_UnregisterRange(const char *canonicalName)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    Gen3ResourceKey key;
    size_t i;

    if (index == NULL || canonicalName == NULL)
        return;
    Gen3ResourceId_DeriveKey(canonicalName, &key);
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

void EmeraldBattleLive_UnregisterRanges(void)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t arena;

    if (!sRangesRegistered)
        return;
    /* Remove in descending arena order so the memmove stays cheap. */
    for (arena = t->arenaCount; arena > 0u; arena--)
    {
        char canonicalName[96];

        ArenaCanonicalName(arena - 1u, canonicalName, sizeof(canonicalName));
        EmeraldBattleLive_UnregisterRange(canonicalName);
    }
    sRangesRegistered = false;
}

/* ------------------------------------------------------------------ */
/* Loader transaction.                                                 */

enum EmeraldBattleLiveStatus EmeraldBattleLive_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack, uint32_t layout,
    struct EmeraldBattleCompatDiagnostics *diagnostics)
{
    enum EmeraldBattleLiveStatus status;

    if (snapshot == NULL || pack == NULL || diagnostics == NULL)
        return EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT;
    memset(diagnostics, 0, sizeof(*diagnostics));
    status = ValidatePackSurface(snapshot, pack, diagnostics);
    if (status != EMERALD_BATTLE_LIVE_OK)
        return status;
    return StageGeneration(pack, layout, diagnostics);
}

enum EmeraldBattleLiveStatus EmeraldBattleLive_Publish(void)
{
    if (sGeneration == NULL)
        return EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE;
    if (!sRangesRegistered)
        return EMERALD_BATTLE_LIVE_ERR_RANGE_REGISTRATION;
    sPublished = true;
    /* Hand the State-v5 adapter its live surface layouts (anim IP /
     * return / callback field addresses + the battle VM surfaces; arena
     * bases are looked up live through GetArena on every capture, so a
     * later generation replacement needs no rebind). */
    if (BattleAnimCompat_RegisterStateLayout != NULL)
        BattleAnimCompat_RegisterStateLayout();
    if (BattleScriptCompat_RegisterStateLayout != NULL)
        BattleScriptCompat_RegisterStateLayout();
    return EMERALD_BATTLE_LIVE_OK;
}

void EmeraldBattleLive_ClearMigratedEntries(void)
{
    EmeraldBattleLive_UnregisterRanges();
    sPublished = false;
    if (sGeneration == NULL)
        return;
    free(sGeneration->buffer);
    free(sGeneration);
    sGeneration = NULL;
}

size_t EmeraldBattleLive_GetRangeCount(void)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    return index != NULL ? index->rangeCount : 0u;
}

bool EmeraldBattleLive_IsPublished(void)
{
    return sPublished;
}

uint64_t EmeraldBattleLive_GetGenerationId(void)
{
    return sGeneration != NULL ? sGeneration->generationId : 0u;
}

/* ------------------------------------------------------------------ */
/* Weak state-adapter bridge (same symbols as the H3 shadow seam).     */

uint64_t EmeraldBattleCompat_GetGenerationId(void)
{
    return sGeneration != NULL ? sGeneration->generationId : 0u;
}

bool EmeraldBattleCompat_GetArena(uint32_t family, const uint8_t **outBase,
                                  size_t *outSize)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    uint32_t arena;

    if (sGeneration == NULL || outBase == NULL || outSize == NULL)
        return false;
    /* Arena rows are family-attributed; only the live families can be
     * requested (AI/contest report false). */
    for (arena = 0u; arena < t->arenaCount; arena++)
    {
        if (t->arenas[arena].family == family)
        {
            *outBase = sGeneration->arenaBase[arena];
            *outSize = t->arenas[arena].layoutSize[sGeneration->layout];
            return true;
        }
    }
    return false;
}

enum EmeraldBattleCompatStatus EmeraldBattleCompat_ReverseResolve(
    uintptr_t address, char *outModuleKey, size_t keyCap,
    uint32_t *outOffset, uint32_t *outFamily)
{
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
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
        const struct EmeraldBattleLiveModule *m =
            &t->modules[sGeneration->hostOrder[mid]];
        uintptr_t start = (uintptr_t)SpanBase(sGeneration->hostOrder[mid]);
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

enum EmeraldBattleCompatStatus EmeraldBattleCompat_ValidateBoundary(
    const char *moduleKey, uint32_t offset, uint32_t boundaryKind)
{
    const struct EmeraldBattleLiveModule *module;
    enum EmeraldBattleLiveStatus status;

    if (moduleKey == NULL || boundaryKind > EMERALD_BATTLE_BOUNDARY_ENTRYPOINT)
        return EMERALD_BATTLE_ERR_INVALID_ARGUMENT;
    if (FindAlias(moduleKey) != NULL)
        return EMERALD_BATTLE_ERR_ALIAS_IDENTITY;
    module = FindPayloadModule(moduleKey);
    if (module == NULL)
        return EMERALD_BATTLE_ERR_TARGET_UNRESOLVED;
    status = ValidateBoundaryInternal((uint32_t)(module
                                          - kEmeraldBattleLiveTable.modules),
                                      offset, boundaryKind);
    return (enum EmeraldBattleCompatStatus)status;
}

bool EmeraldBattleCompat_GetStateIdentity(
    const char *moduleKey, Gen3ResourceKey *outKey, uint32_t *outSchema,
    uint32_t *outPayloadSize)
{
    const struct EmeraldBattleLiveModule *module;
    const struct EmeraldBattleLiveAlias *alias;

    if (moduleKey == NULL || outKey == NULL || outSchema == NULL
     || outPayloadSize == NULL)
        return false;
    /* Zero-width aliases canonicalize to their payload owner. */
    alias = FindAlias(moduleKey);
    if (alias != NULL)
        module = &kEmeraldBattleLiveTable.modules[alias->ownerModule];
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
    const struct EmeraldBattleLiveTable *t = &kEmeraldBattleLiveTable;
    size_t lo;
    size_t hi;
    const struct EmeraldBattleLiveModule *module = NULL;

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
    *outAddress = (uintptr_t)SpanBase((uint32_t)(module - t->modules))
                + payloadOffset;
    snprintf(outModuleKey, keyCap, "%s", module->id);
    return EMERALD_BATTLE_OK;
}

/* ------------------------------------------------------------------ */

const char *EmeraldBattleLiveStatus_Describe(enum EmeraldBattleLiveStatus status)
{
    switch (status)
    {
    case EMERALD_BATTLE_LIVE_OK: return "ok";
    case EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case EMERALD_BATTLE_LIVE_ERR_OUT_OF_MEMORY: return "out of memory";
    case EMERALD_BATTLE_LIVE_ERR_RESOLVE_FAILED: return "snapshot resolve failed";
    case EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_OWNERSHIP: return "module ownership is not ROM_BASE";
    case EMERALD_BATTLE_LIVE_ERR_PAYLOAD_SIZE_MISMATCH: return "snapshot payload size mismatch";
    case EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_COUNT: return "pack surface disagrees with the live inventory";
    case EMERALD_BATTLE_LIVE_ERR_UNEXPECTED_SCHEMA: return "module schema mismatch";
    case EMERALD_BATTLE_LIVE_ERR_TABLE_MISMATCH: return "pack record disagrees with the generated table";
    case EMERALD_BATTLE_LIVE_ERR_ALIAS_IDENTITY: return "zero-width alias must canonicalize to its payload owner";
    case EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID: return "boundary query refused";
    case EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED: return "target could not resolve";
    case EMERALD_BATTLE_LIVE_ERR_STAGING_FAILED: return "staging failed";
    case EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE: return "no live battle generation";
    case EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED: return "live execution is not published";
    case EMERALD_BATTLE_LIVE_ERR_REFUSED: return "binding refused (no native target)";
    case EMERALD_BATTLE_LIVE_ERR_BUSY: return "generation replacement refused while animation active";
    case EMERALD_BATTLE_LIVE_ERR_RANGE_REGISTRATION: return "range registration failed";
    case EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY: return "script word belongs to another family";
    default: return "unknown battle live error";
    }
}
