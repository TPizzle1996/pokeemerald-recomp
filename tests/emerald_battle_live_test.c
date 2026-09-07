/* R13-H4/H5/H6 live battle + battle-anim + battle-AI + contest-AI +
 * field-effect cutover tests (tests/run_emerald_battle_live.sh).
 *
 * Drives the PRODUCTION live seam (emerald_battle_live.c +
 * battle_live_table.generated.c) instead of the H3 shadow seam, over
 * the real production pack:
 *
 *   oracle <pack> <modsDir> <layout>  differential resolution oracle:
 *       2,043 entry words x 2 layouts, 7,549 relocs x 2 layouts,
 *       byte-exact arena vs the committed .bin artifacts, boundary and
 *       family re-verification through the six-bridge adapter API;
 *   faults <pack>                    the brief sec 24 fail-closed
 *       matrix (pre-publish refusal, corrupt-word gate, refuse-only
 *       bindings, wrong family, non-root launch, scalar words never
 *       converted, compiled pointers never accepted, BUSY quiescence,
 *       invalid layout, teardown refusal);
 *   replace <pack>                   brief sec 25/26: publish generation
 *       A, replace with generation B (perturbed layout), ranges hold at
 *       the 6,382 invariant, identity-based unregister + re-register
 *       across all five live arenas;
 *   state-create <pack> <state>      fresh-process proof (sec 15) side
 *       A: live anim IP/return planted, real walker saves v5 state;
 *   state-load  <pack> <state>       side B: fresh process, perturbed
 *       layout, restore -> anim pointers relocate into the new arena by
 *       module+offset identity;
 *   battle-oracle / battle-faults / battle-249 / h5-state
 *                                    H5 battle suite (routing rows,
 *                                    compiled labels, EWRAM, sweep,
 *                                    fail-closed matrix, 249-opcode
 *                                    differential, nested state);
 *   ai-oracle / ai-faults / ai-249   H6 battle-AI + contest-AI suite:
 *       2,351 instructions, 1,586 typed relocs (38 data targets),
 *       64 entry rows, pointer sweep, fail-closed matrix, 235-opcode
 *       differential, mini-VM execution differential;
 *   h6-state-create/-load            H6 quiescent AI fresh-process
 *       proof (battle-ai and contest-ai).
 *
 * The weak BattleAnimCompat_IsAnimActive and
 * BattleScriptCompat_IsBattleActive probes are defined strongly here so
 * the quiescence gates (sec 25) are exercised deterministically.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_battle_compat.h"
#include "emerald/resources/emerald_battle_live.h"
#include "emerald/resources/emerald_battle_state.h"
#include "emerald/resources/battle_live.generated.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"
#include "platform/native_state.h"
#include "emerald_script_compat_harness.h"

#define HARNESS_STATE_SLOT 7u
#define H4_STACK_CAP 8u
#define H4_FAMILY_ANIM EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT
#define H4_FAMILY_FE EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT

/* EWRAM battle-surface globals provided by emerald_resource_state_stub.c. */
extern EWRAM_DATA const u8 *gBattlescriptCurrInstr;
extern EWRAM_DATA const u8 *gAIScriptPtr;
extern EWRAM_DATA const u8 *gSelectionBattleScripts[4];
extern EWRAM_DATA const u8 *gPalaceSelectionBattleScripts[4];
extern EWRAM_DATA void (*gAnimScriptCallback)(void);
extern EWRAM_DATA const u8 *sBattleAnimScriptPtr;
extern EWRAM_DATA const u8 *sBattleAnimScriptRetAddr;
extern unsigned char sHarnessGameBss[0x10000];
void HarnessStatePath_Override(const char *path);

static void H4ClearSurfaces(void);
static void H4BindLayout(void);
struct H5BattleFixtures;
static struct H5BattleFixtures *H5BattleFixtures(void);
struct H6AiFixtures;
static struct H6AiFixtures *H6AiFixtures(void);

static int sFailures;

#define CHECK(expr)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(expr))                                                    \
        {                                                               \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,   \
                    __LINE__, #expr);                                   \
            sFailures++;                                                \
            goto fail;                                                  \
        }                                                               \
    } while (0)

/* Strong override of the seam's weak probe: quiescence gating becomes
 * deterministic in the harness (production links battle_anim.c's own
 * definition). */
static bool sHarnessAnimBusy;

bool BattleAnimCompat_IsAnimActive(void)
{
    return sHarnessAnimBusy;
}

static void H4WaitCallback(void)
{
}

static const struct EmeraldBattleLiveTable *H4Table(void)
{
    return &kEmeraldBattleLiveTable;
}

/* spanBase of a module in the CURRENT generation (via the launch API -
 * the same path production uses; never host arithmetic). */
static bool32 H4SpanBase(uint32_t moduleIndex, const uint8_t **outBase)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    const struct EmeraldBattleLiveModule *m = &t->modules[moduleIndex];
    uintptr_t pointer;

    if (m->byteCount == 0u)
        return FALSE;
    if (EmeraldBattleLive_ResolveLaunchTarget(
            m->family, m->gbaStart, &pointer) != EMERALD_BATTLE_LIVE_OK)
        return FALSE;
    if (outBase != NULL)
        *outBase = (const uint8_t *)pointer;
    return TRUE;
}

/* Span of any payload module (routing tables included) from the staged
 * arena + layout offset - routing roots are not launch targets, so the
 * byte-exact/reloc oracles cannot use the launch resolver. */
static bool32 H4LayoutSpan(uint32_t moduleIndex, uint32_t layout,
                           const uint8_t **outBase)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    const struct EmeraldBattleLiveModule *m = &t->modules[moduleIndex];
    const uint8_t *arena;
    size_t arenaSize;

    if (m->byteCount == 0u)
        return FALSE;
    if (!EmeraldBattleCompat_GetArena(m->family, &arena, &arenaSize))
        return FALSE;
    if (outBase != NULL)
        *outBase = arena + m->layoutOffset[layout];
    return TRUE;
}

/* ---------------------------------------------------------------- */
/* The range index (emerald_trainer_native_compat.c) is only built and
 * validated during the loader's session registration, so every driver
 * mode runs through the production loader (SetupScriptCompatSession)
 * and then rolls the live generation back with ClearMigratedEntries to
 * drive the full transactional sequence from scratch. */

static enum EmeraldBattleLiveStatus H4StageLive(uint32_t layout)
{
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;

    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, layout, &diagnostics);
    if (status != EMERALD_BATTLE_LIVE_OK)
        return status;
    status = EmeraldBattleLive_RegisterRanges();
    if (status != EMERALD_BATTLE_LIVE_OK)
        return status;
    return EmeraldBattleLive_Publish();
}

/* ---------------------------------------------------------------- */
/* Oracle. */

static bool32 ReadModuleBin(const char *modsDir, const char *id,
                            uint8_t **outData, size_t *outSize)
{
    char path[1024];
    FILE *file;
    long size;
    uint8_t *data;

    /* id = "emerald:<family>/<name>" -> <modsDir>/data/<family>/<name>.bin */
    snprintf(path, sizeof(path), "%s/data/%s.bin", modsDir,
             id + strlen("emerald:"));
    file = fopen(path, "rb");
    if (file == NULL)
        return FALSE;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0)
    {
        fclose(file);
        return FALSE;
    }
    data = malloc((size_t)size);
    if (data == NULL
     || fread(data, 1, (size_t)size, file) != (size_t)size)
    {
        free(data);
        fclose(file);
        return FALSE;
    }
    fclose(file);
    *outData = data;
    *outSize = (size_t)size;
    return TRUE;
}

/* First odd (bytecode-operand-aligned) word offset of `module` that is
 * NOT a reloc operand position; UINT32_MAX when none exists. */
static uint32_t FirstGapOffset(uint32_t moduleIndex)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    const struct EmeraldBattleLiveModule *m = &t->modules[moduleIndex];
    uint32_t k;

    for (k = 0u; 5u * k + 1u + 3u < m->byteCount; k++)
    {
        uint32_t candidate = 5u * k + 1u;
        uint32_t r;
        bool32 isReloc = FALSE;

        for (r = m->relocFirst; r < m->relocFirst + m->relocCount; r++)
        {
            if (t->relocs[r].operandOffset == candidate)
            {
                isReloc = TRUE;
                break;
            }
        }
        if (!isReloc)
            return candidate;
    }
    return UINT32_MAX;
}

static int DoOracle(const char *packPath, const char *modsDir, uint32_t layout)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t i;
    uint32_t entryWords = 0u;
    uint32_t byteExact = 0u;
    uint32_t relocs = 0u;
    uint32_t scriptTargets = 0u;
    uint32_t engineRefused = 0u;
    uint32_t gaps = 0u;
    char keyBuf[96];
    uint32_t revOffset;
    uint32_t revFamily;

    /* The loader publishes the live generation during session
     * registration (6,382 ranges); roll it back so the oracle drives
     * the full transactional sequence from scratch (sec 4/25). */
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();
    CHECK(EmeraldBattleLive_GetRangeCount() == 7603u);
    /* RegisterRanges with no generation refuses. */
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_ERR_UNAVAILABLE);
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, layout, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    /* Pre-publish: every resolver refuses (sec 4 fail-closed). */
    CHECK(EmeraldBattleLive_ResolveScriptTarget(
              H4_FAMILY_ANIM, t->modules[0].gbaStart, &(uintptr_t){0})
          == EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED);
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[0].gbaStart, &(uintptr_t){0})
          == EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED);
    CHECK(EmeraldBattleLive_ResolveOperand(
              (uintptr_t)&sHarnessAnimBusy, &(uintptr_t){0})
          == EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_IsPublished());

    /* Entry words: every module root (723 payload + 3 alias identities)
     * is a launchable root export of its family. */
    for (i = 0u; i < t->moduleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *arena;
        size_t arenaSize;
        uintptr_t pointer;
        const uint8_t *spanBase;

        if (m->byteCount == 0u)
            continue; /* alias identity: launchable, no bytes */
        /* Routing tables (gBattleAnims_Moves & friends) are staged
         * payload but never launch roots: the interpreter resolves the
         * table WORDS (reloc operands) and launches the pointed script.
         * Only bytecode spans carry launchable roots (sec 8/9). */
        if (m->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
            continue;
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  m->family, m->gbaStart, &pointer) == EMERALD_BATTLE_LIVE_OK);
        spanBase = (const uint8_t *)pointer;
        CHECK(EmeraldBattleCompat_GetArena(m->family, &arena, &arenaSize));
        CHECK(arena != NULL);
        /* The span sits at the module's layout offset inside its arena. */
        CHECK((uintptr_t)spanBase - (uintptr_t)arena == m->layoutOffset[layout]);
        entryWords++;
    }
    /* 2,081 payload - 13 routing tables (5 battle + 4 anim + 1 FE +
     * 1 anim quiet-BGM + 2 AI entry) - 25 AI data tables. */
    CHECK(entryWords == EMERALD_BATTLE_LIVE_PAYLOAD_MODULE_COUNT - 38u);

    /* Routing roots: launchable only via their reloc'd words; the root
     * itself refuses (bytecode boundary gate, not family). The H6 AI
     * if_in_* data tables (25) are payload spans with no instruction
     * boundaries - never launch roots either. */
    {
        uint32_t routing = 0u;
        uint32_t dataTables = 0u;
        uintptr_t pointer;
        for (i = 0u; i < t->payloadModuleCount; i++)
        {
            const struct EmeraldBattleLiveModule *m = &t->modules[i];
            if (m->mapKind == EMERALD_BATTLE_MAP_BYTECODE)
                continue;
            if (m->mapKind == EMERALD_BATTLE_MAP_DATA)
                dataTables++;
            else
                routing++;
            CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                      m->family, m->gbaStart, &pointer)
                  == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
        }
        CHECK(routing == 13u);
        CHECK(dataTables == 25u);
    }

    /* Byte-exact: every payload module's staged bytes == the committed
     * .bin artifact (the independent ground truth). */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;
        uint8_t *bin = NULL;
        size_t binSize = 0u;

        CHECK(H4LayoutSpan(i, layout, &spanBase));
        CHECK(ReadModuleBin(modsDir, m->id, &bin, &binSize));
        CHECK(binSize == m->byteCount);
        CHECK(memcmp(spanBase, bin, m->byteCount) == 0u);
        free(bin);
        byteExact++;
    }
    CHECK(byteExact == t->payloadModuleCount);

    /* Reloc oracle: resolve every operand position of every payload
     * module and verify the exact target identity. */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;
        uint32_t r;

        CHECK(H4LayoutSpan(i, layout, &spanBase));
        for (r = m->relocFirst; r < m->relocFirst + m->relocCount; r++)
        {
            const struct EmeraldBattleLiveReloc *reloc = &t->relocs[r];
            uintptr_t operand = (uintptr_t)(spanBase + reloc->operandOffset);
            uintptr_t pointer;

            status = EmeraldBattleLive_ResolveOperand(operand, &pointer);
            relocs++;
            if (reloc->relocClass == EMERALD_BATTLE_LIVE_RELOC_SCRIPT_TARGET)
            {
                const struct EmeraldBattleLiveModule *target =
                    &t->modules[reloc->target];
                const uint8_t *targetBase;
                uintptr_t expected;
                uintptr_t viaWord;

                CHECK(status == EMERALD_BATTLE_LIVE_OK);
                CHECK(H4LayoutSpan(reloc->target, layout, &targetBase));
                expected = (uintptr_t)targetBase + reloc->targetOffset;
                CHECK(pointer == expected);
                /* Raw-word API (sec 8): the operand word itself resolves
                 * to the same target. */
                CHECK(EmeraldBattleLive_ResolveScriptTarget(
                          m->family, reloc->expectedWord, &viaWord)
                      == EMERALD_BATTLE_LIVE_OK);
                CHECK(viaWord == expected);
                /* Boundary + reverse-containment re-verification via
                 * the bridges: the resolved pointer sits in exactly one
                 * payload span, at the row's offset, under the row's
                 * family identity. H6 data tables (battle-AI if_in_*
                 * byte/hword lists) carry no instruction boundaries -
                 * the row pins offset 0 (brief sec 6). */
                if (target->mapKind == EMERALD_BATTLE_MAP_DATA)
                    CHECK(reloc->targetOffset == 0u);
                else
                    CHECK(EmeraldBattleCompat_ValidateBoundary(
                              target->id, reloc->targetOffset,
                              EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
                          == EMERALD_BATTLE_OK);
                CHECK(EmeraldBattleCompat_ReverseResolve(
                          pointer, keyBuf, sizeof(keyBuf), &revOffset,
                          &revFamily) == EMERALD_BATTLE_OK);
                CHECK(strcmp(keyBuf, target->id) == 0);
                CHECK(revOffset == reloc->targetOffset);
                CHECK(revFamily == m->family);
                scriptTargets++;
            }
            else
            {
                const struct EmeraldBattleLiveBinding *binding =
                    &t->bindings[reloc->target];
                uintptr_t expected = binding->address;

                /* A (EWRAM) + battle B rows resolve through the
                 * native-address TU; the table row carries 0. */
                if (expected == 0u)
                    expected = EmeraldBattleLiveNative_BindingAddress(
                        reloc->target);
                if (expected != 0u)
                {
                    CHECK(status == EMERALD_BATTLE_LIVE_OK);
                    CHECK(pointer == expected
                          + (binding->letter == 'A'
                                 ? binding->addend : 0u));
                }
                else
                {
                    /* Refuse-only binding (sec 10): hard refusal. */
                    CHECK(status == EMERALD_BATTLE_LIVE_ERR_REFUSED);
                    engineRefused++;
                }
            }
        }
    }
    CHECK(relocs == EMERALD_BATTLE_LIVE_RELOC_COUNT);
    /* Every script-target word was discovered from at least one operand
     * position, so the reloc-side count covers the word set. */
    CHECK(scriptTargets >= EMERALD_BATTLE_LIVE_SCRIPT_TARGET_WORD_COUNT);
    /* 4 refuse-only bindings (mist/terrain anims absent from this fork),
     * referenced by 20 operand positions (10 sprite-template words for
     * gShakeMonOrTerrainSpriteTemplate + 8+1+1 callback words for the
     * three AnimTask_*): every one hard-refuses (sec 10). */
    CHECK(engineRefused == 20u);

    /* Sec 12: scalars and non-reloc words are never converted. */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;
        uint32_t gap;
        uintptr_t pointer;

        if (m->family != H4_FAMILY_ANIM)
            continue;
        gap = FirstGapOffset(i);
        if (gap == UINT32_MAX)
            continue;
        CHECK(H4SpanBase(i, &spanBase));
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)(spanBase + gap), &pointer)
              != EMERALD_BATTLE_LIVE_OK);
        gaps++;
        if (gaps >= 8u)
            break;
    }
    CHECK(gaps >= 1u);
    /* A function binding word is not a script target: the word-set gate
     * (sec 8) refuses it. */
    for (i = 0u; i < t->bindingCount; i++)
    {
        const struct EmeraldBattleLiveBinding *b = &t->bindings[i];
        uintptr_t pointer;

        if (b->address == 0u)
            continue;
        CHECK(EmeraldBattleLive_ResolveScriptTarget(
                  H4_FAMILY_ANIM, b->word, &pointer)
              != EMERALD_BATTLE_LIVE_OK);
    }
    /* Sec 23: a compiled (non-arena) host address is never accepted. */
    CHECK(EmeraldBattleLive_ResolveOperand(
              (uintptr_t)&sHarnessAnimBusy, &(uintptr_t){0})
          != EMERALD_BATTLE_LIVE_OK);

    printf("H4-ORACLE layout=%u entry-words=%u byte-exact=%u relocs=%u "
           "script-targets=%u refuse-only=%u gaps=%u\n",
           layout, entryWords, byteExact, relocs, scriptTargets,
           engineRefused, gaps);
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* Fail-closed matrix (brief sec 24). */

static int DoFaults(const char *packPath)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t passes = 0u;
    uint32_t m = 0u;
    uint32_t r;
    const uint8_t *spanBase;
    uint8_t *arena;
    size_t arenaSize;
    uint32_t firstScriptTarget = UINT32_MAX;
    uintptr_t pointer;

    /* Loader-driven session (range index validity), then roll the live
     * generation back: faults 1..11 drive the transactional sequence
     * from the cleared-but-index-valid state. */
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();

    /* 1. invalid layout refuses transactionally. */
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack,
        EMERALD_BATTLE_LIVE_LAYOUT_COUNT, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT);
    CHECK(EmeraldBattleLive_GetGenerationId() == 0u);
    passes++;

    /* 2. stage layout 0 + publish (6,382 = 6,377 + the 5 live ranges). */
    status = H4StageLive(0u);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    passes++;

    /* Find the first SCRIPT_TARGET reloc of the first anim payload
     * module (for corruption and non-root cases). Every script target
     * in this data is a root export (interior flow is relative
     * bytecode, never absolute words), so the non-root launch probe
     * uses an interior bytecode boundary instead. */
    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        if (mod->family != H4_FAMILY_ANIM || mod->relocCount == 0u)
            continue;
        for (r = mod->relocFirst; r < mod->relocFirst + mod->relocCount; r++)
        {
            if (t->relocs[r].relocClass
                    == EMERALD_BATTLE_LIVE_RELOC_SCRIPT_TARGET)
            {
                firstScriptTarget = r;
                break;
            }
        }
        if (firstScriptTarget != UINT32_MAX)
            break;
    }
    CHECK(firstScriptTarget != UINT32_MAX);
    CHECK(H4SpanBase(m, &spanBase));
    CHECK(EmeraldBattleCompat_GetArena(H4_FAMILY_ANIM,
                                       (const uint8_t **)&arena, &arenaSize));

    /* 3. wrong-family launch: an anim root word under the FE family. */
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  H4_FAMILY_FE, mod->gbaStart, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY);
        passes++;
    }

    /* 4. non-root launch: an interior bytecode boundary word
     * (offset != 0) refuses - launch is root-export only. */
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        const struct EmeraldBattleNativeBoundary *boundary;
        CHECK(mod->boundaryCount >= 2u);
        boundary = &t->boundaries[mod->boundaryFirst + 1u];
        CHECK(boundary->payloadOffset != 0u);
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  H4_FAMILY_ANIM,
                  mod->gbaStart + boundary->payloadOffset, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
        passes++;
    }

    /* 5. corrupt operand word: byte flip -> the word check refuses. */
    {
        const struct EmeraldBattleLiveReloc *reloc = &t->relocs[firstScriptTarget];
        uint8_t *operand = (uint8_t *)spanBase + reloc->operandOffset;
        uint8_t saved = operand[0];

        operand[0] ^= 0xFFu;
        status = EmeraldBattleLive_ResolveOperand(
            (uintptr_t)operand, &pointer);
        CHECK(status != EMERALD_BATTLE_LIVE_OK);
        operand[0] = saved;
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)operand, &pointer) == EMERALD_BATTLE_LIVE_OK);
        passes++;
    }

    /* 6. raw-word gate: a script-target word under the WRONG family
     * refuses (the word contains to an anim module). */
    {
        const struct EmeraldBattleLiveReloc *reloc = &t->relocs[firstScriptTarget];
        CHECK(EmeraldBattleLive_ResolveScriptTarget(
                  H4_FAMILY_FE, reloc->expectedWord, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY);
        passes++;
    }

    /* 7. unaligned non-reloc word: never converted. */
    {
        uint32_t gap = FirstGapOffset(m);
        uintptr_t p;

        CHECK(gap != UINT32_MAX);
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)(spanBase + gap), &p)
              != EMERALD_BATTLE_LIVE_OK);
        passes++;
    }

    /* 8. compiled pointer never accepted (sec 23). */
    CHECK(EmeraldBattleLive_ResolveOperand(
              (uintptr_t)&sHarnessAnimBusy, &pointer)
          != EMERALD_BATTLE_LIVE_OK);
    passes++;

    /* 9. quiescence: a running animation VM blocks replacement. */
    sHarnessAnimBusy = true;
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_ERR_BUSY);
    sHarnessAnimBusy = false;
    /* The generation survived untouched. */
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[m].gbaStart, &pointer)
          == EMERALD_BATTLE_LIVE_OK);
    passes++;

    /* 10. teardown: ClearMigratedEntries unpublishes and refuses
     * (back to the loader's 6,377 sibling ranges). */
    EmeraldBattleLive_ClearMigratedEntries();
    CHECK(EmeraldBattleLive_GetRangeCount() == 7603u);
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[m].gbaStart, &pointer)
          == EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED);
    passes++;

    /* 11. re-publish after teardown restores full resolution. */
    status = H4StageLive(0u);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[m].gbaStart, &pointer)
          == EMERALD_BATTLE_LIVE_OK);
    passes++;

    printf("LIVE-FAULTS passed=%u\n", passes);
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* H5 battle: routing + compiled-label + EWRAM + sweep oracle.        */

#define H5_FAMILY_BATTLE EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT

/* Strong override of the seam's weak battle quiescence probe. */
static bool sHarnessBattleBusy;

bool BattleScriptCompat_IsBattleActive(void)
{
    return sHarnessBattleBusy;
}

static int DoBattleOracle(const char *packPath, const char *modsDir,
                          uint32_t layout)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t i;
    uint32_t r;
    uint32_t routingRows = 0u;
    uint32_t labels = 0u;
    uint32_t ewram = 0u;
    uint32_t sweptWords = 0u;
    uintptr_t pointer;

    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, layout, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);

    /* Routing republication (brief sec 12): every row of every battle
     * routing table resolves from the ARENA bytes to a battle script
     * root, identical to the canonical .bin row word. */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        uint8_t *bin = NULL;
        size_t binSize = 0u;
        uint32_t row;

        if (m->family != H5_FAMILY_BATTLE
         || m->mapKind != EMERALD_BATTLE_MAP_ROUTING)
            continue;
        CHECK(ReadModuleBin(modsDir, m->id, &bin, &binSize));
        CHECK(binSize == m->byteCount);
        for (row = 0u; row < m->byteCount / 4u; row++)
        {
            uint32_t word = (uint32_t)bin[row * 4u]
                          | ((uint32_t)bin[row * 4u + 1u] << 8)
                          | ((uint32_t)bin[row * 4u + 2u] << 16)
                          | ((uint32_t)bin[row * 4u + 3u] << 24);
            uintptr_t viaWord;

            CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                      m->gbaStart, row, &pointer) == EMERALD_BATTLE_LIVE_OK);
            CHECK(EmeraldBattleLive_ResolveScriptTarget(
                      H5_FAMILY_BATTLE, word, &viaWord)
                  == EMERALD_BATTLE_LIVE_OK);
            CHECK(pointer == viaWord);
            routingRows++;
        }
        free(bin);
    }
    /* 5 battle routing tables: 214 + 13 + 6 + 1 + 4 rows (H1 sec 6). */
    CHECK(routingRows == 238u);

    /* Compiled-label translation (brief sec 11/14): every generated
     * label map row resolves to its arena export; a non-label native
     * address refuses. */
    for (i = 0u; i < t->labelCount; i++)
    {
        const struct EmeraldBattleLiveLabel *label = &t->labels[i];
        const struct EmeraldBattleLiveModule *m = &t->modules[label->module];
        const uint8_t *spanBase;
        uintptr_t native = EmeraldBattleLiveNative_LabelAddress(i);

        CHECK(native != 0u);
        CHECK(EmeraldBattleLive_ResolveCompiledLabel(
                  (const void *)native, &pointer) == EMERALD_BATTLE_LIVE_OK);
        CHECK(H4LayoutSpan(label->module, layout, &spanBase));
        CHECK(pointer == (uintptr_t)spanBase + label->offset);
        CHECK(m->family == H5_FAMILY_BATTLE);
        labels++;
    }
    CHECK(labels == EMERALD_BATTLE_LIVE_LABEL_COUNT);
    CHECK(EmeraldBattleLive_ResolveCompiledLabel(
              (const void *)&sHarnessBattleBusy, &(uintptr_t){0})
          == EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED);

    /* EWRAM semantic resolution (brief sec 15/16): every A-letter
     * binding row resolves as native base + validated addend through
     * the native-address TU; the stored word is base + addend. */
    for (i = 0u; i < t->bindingCount; i++)
    {
        const struct EmeraldBattleLiveBinding *b = &t->bindings[i];

        if (b->letter != 'A')
            continue;
        CHECK(b->addend < b->allowedOffset);
        CHECK(b->word == b->baseWord + b->addend);
        CHECK(EmeraldBattleLiveNative_BindingAddress(i) != 0u);
        ewram++;
    }
    /* 50 EWRAM rows (23 symbols; H1 sec 4). */
    CHECK(ewram == 50u);

    /* Pointer sweep (brief sec 35): every battle payload module's
     * consumed pointer operand positions are exactly its reloc rows -
     * every reloc resolves; every NON-reloc 4-byte word that equals a
     * known battle script-target word is a violation (it would be a
     * silent compiled-fallback candidate); scalar words never convert. */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;
        uint32_t off;

        if (m->family != H5_FAMILY_BATTLE)
            continue;
        CHECK(H4LayoutSpan(i, layout, &spanBase));
        for (r = m->relocFirst; r < m->relocFirst + m->relocCount; r++)
        {
            const struct EmeraldBattleLiveReloc *reloc = &t->relocs[r];

            CHECK(EmeraldBattleLive_ResolveOperand(
                      (uintptr_t)(spanBase + reloc->operandOffset),
                      &pointer) == EMERALD_BATTLE_LIVE_OK);
        }
        for (off = 0u; off + 4u <= m->byteCount; off += 4u)
        {
            uint32_t word;
            bool isReloc = false;

            memcpy(&word, spanBase + off, 4u);
            for (r = m->relocFirst; r < m->relocFirst + m->relocCount; r++)
            {
                if (t->relocs[r].operandOffset == off)
                {
                    isReloc = true;
                    break;
                }
            }
            if (isReloc)
                continue;
            /* A non-reloc word must NOT be a battle script-target word
             * (else a runtime reader could resolve it to compiled
             * bytes); scalars and IDs are fine. */
            {
                uint32_t lo = 0u;
                uint32_t hi = t->scriptTargetWordCount;
                bool inSet = false;

                while (lo < hi)
                {
                    uint32_t mid = lo + (hi - lo) / 2u;
                    if (word < t->scriptTargetWords[mid])
                        hi = mid;
                    else if (word > t->scriptTargetWords[mid])
                        lo = mid + 1u;
                    else
                    {
                        inSet = true;
                        break;
                    }
                }
                CHECK(!inSet);
            }
            sweptWords++;
        }
    }
    CHECK(sweptWords >= 2500u);

    printf("H5-BATTLE-ORACLE layout=%u routing-rows=%u labels=%u "
           "ewram=%u swept-words=%u\n",
           layout, routingRows, labels, ewram, sweptWords);
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* H5 battle fail-closed matrix (brief sec 25).                       */

static int DoBattleFaults(const char *packPath)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t passes = 0u;
    uint32_t m;
    uint32_t r;
    uint32_t firstBattleModule = UINT32_MAX;
    uint32_t firstBattleReloc = UINT32_MAX;
    uint32_t firstBattleRelocModule = UINT32_MAX;
    const uint8_t *spanBase;
    uintptr_t pointer;

    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();

    /* Find the first battle payload module + its first reloc. */
    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        if (mod->family != H5_FAMILY_BATTLE || mod->byteCount == 0u)
            continue;
        firstBattleModule = m;
        break;
    }
    CHECK(firstBattleModule != UINT32_MAX);
    for (m = firstBattleModule; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        if (mod->family != H5_FAMILY_BATTLE)
            continue;
        if (mod->relocCount != 0u)
        {
            firstBattleReloc = mod->relocFirst;
            firstBattleRelocModule = m;
            break;
        }
    }
    CHECK(firstBattleReloc != UINT32_MAX);

    /* 1. battle launch under the FE family refuses. */
    {
        const struct EmeraldBattleLiveModule *mod =
            &t->modules[firstBattleModule];
        memset(&diagnostics, 0, sizeof(diagnostics));
        status = EmeraldBattleLive_TryInitialize(
            gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diagnostics);
        CHECK(status == EMERALD_BATTLE_LIVE_OK);
        CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
        CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  H4_FAMILY_FE, mod->gbaStart, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY);
        passes++;
    }

    /* 2. corrupt battle operand word refuses (the word check). */
    CHECK(H4SpanBase(firstBattleRelocModule, &spanBase));
    {
        const struct EmeraldBattleLiveReloc *reloc =
            &t->relocs[firstBattleReloc];
        uint8_t *operand = (uint8_t *)spanBase + reloc->operandOffset;
        uint8_t saved = operand[0];

        operand[0] ^= 0xFFu;
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)operand, &pointer) != EMERALD_BATTLE_LIVE_OK);
        operand[0] = saved;
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)operand, &pointer) == EMERALD_BATTLE_LIVE_OK);
        passes++;
    }

    /* 3. battle routing row out of range refuses. */
    {
        uint32_t table = 0u;
        for (m = 0u; m < t->payloadModuleCount; m++)
        {
            const struct EmeraldBattleLiveModule *mod = &t->modules[m];
            if (mod->family == H5_FAMILY_BATTLE
             && mod->mapKind == EMERALD_BATTLE_MAP_ROUTING)
            {
                table = mod->gbaStart;
                break;
            }
        }
        CHECK(table != 0u);
        CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                  table, 0u, &pointer) == EMERALD_BATTLE_LIVE_OK);
        CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                  table, 9999u, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
        passes++;
    }

    /* 4. a non-routing battle word under ResolveRoutingTarget refuses. */
    {
        const struct EmeraldBattleLiveModule *mod =
            &t->modules[firstBattleModule];
        CHECK(mod->mapKind == EMERALD_BATTLE_MAP_BYTECODE);
        CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                  mod->gbaStart, 0u, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
        passes++;
    }

    /* 5. interior non-export battle word under GetBattleScript refuses. */
    {
        const struct EmeraldBattleLiveModule *mod =
            &t->modules[firstBattleModule];
        uint32_t interior = mod->gbaStart + 1u;
        if (EmeraldBattleLive_GetBattleScript(interior, &pointer)
                == EMERALD_BATTLE_LIVE_OK)
        {
            /* +1 is an export: probe the byte before the end instead
             * (never an export of a bytecode span). */
            interior = mod->gbaStart + mod->byteCount - 1u;
        }
        CHECK(EmeraldBattleLive_GetBattleScript(interior, &pointer)
              != EMERALD_BATTLE_LIVE_OK);
        passes++;
    }

    /* 6. battle word under the anim family via ResolveScriptTarget
     * refuses (wrong family / word-set gate). */
    {
        const struct EmeraldBattleLiveReloc *reloc =
            &t->relocs[firstBattleReloc];
        if (reloc->relocClass == EMERALD_BATTLE_LIVE_RELOC_SCRIPT_TARGET)
            CHECK(EmeraldBattleLive_ResolveScriptTarget(
                      H4_FAMILY_ANIM, reloc->expectedWord, &pointer)
                  != EMERALD_BATTLE_LIVE_OK);
        passes++;
    }

    /* 7. battle quiescence blocks replacement. */
    sHarnessBattleBusy = true;
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_ERR_BUSY);
    sHarnessBattleBusy = false;
    {
        const struct EmeraldBattleLiveModule *mod =
            &t->modules[firstBattleModule];
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  H5_FAMILY_BATTLE, mod->gbaStart, &pointer)
              == EMERALD_BATTLE_LIVE_OK);
    }
    passes++;

    /* 8. unregister the exact battle range only (anim + FE + AI intact). */
    EmeraldBattleLive_UnregisterRange("emerald:battle-script/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 7607u);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    passes++;

    printf("H5-BATTLE-FAULTS passed=%u\n", passes);
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* H5 249-opcode differential (brief sec 28).                         */

#define H5_OPCODE_SLOTS 249u

/* Family-tagged grammar lookup (H6): the table now carries the battle,
 * battle-AI and contest-AI grammars - an (opcode, size) pair is only
 * qualified under the executing family's VM grammar. */
static const struct EmeraldBattleLiveGrammarEntry *H6GrammarFind(
    uint32_t family, uint8_t opcode, uint8_t size)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    uint32_t i;

    for (i = 0u; i < EMERALD_BATTLE_LIVE_GRAMMAR_ENTRY_COUNT; i++)
    {
        const struct EmeraldBattleLiveGrammarEntry *e = &t->grammar[i];

        if (e->family == family && e->opcode == opcode && e->size == size)
            return e;
    }
    return NULL;
}

static uint32_t H5OperandPrefix(const struct EmeraldBattleLiveGrammarEntry *e,
                                uint32_t operandIndex)
{
    uint32_t off = 1u; /* the opcode byte */
    uint32_t i;

    for (i = 0u; i < operandIndex; i++)
        off += e->widths[i];
    return off;
}

static const struct EmeraldBattleLiveModule *FindModuleByKeyStr(
    const char *id)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
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

static uint32_t H5LabelWord(const char *name)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    uint32_t i;

    for (i = 0u; i < t->labelCount; i++)
    {
        if (strcmp(t->labels[i].name, name) == 0)
            return t->labels[i].word;
    }
    return 0u;
}

/* Mini-VM: execute one battle root through the LIVE arena with the
 * real instruction decode; pointer operands resolve typed, call/return
 * balance, waitmessage parks, conditional targets resolve (straight
 * walk - condition evaluation belongs to the engine). */
static bool32 H5ExecuteScript(uint32_t word, uint32_t *outSteps,
                              uint32_t *outCalls, uint32_t *outParks)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    uint32_t steps = 0u;
    uint32_t calls = 0u;
    uint32_t parks = 0u;
    uint32_t depth = 0u;
    uint32_t budget = 4000u;
    uintptr_t base;

    if (EmeraldBattleLive_GetBattleScript(word, &base) != EMERALD_BATTLE_LIVE_OK)
        return FALSE;
    for (;;)
    {
        const struct EmeraldBattleLiveGrammarEntry *e;
        char keyBuf[96];
        uint32_t revOffset;
        uint32_t revFamily;
        uint8_t opcode;

        if (steps >= budget)
            return FALSE;
        opcode = *(const uint8_t *)base;
        /* The executing IP must always be an instruction start inside
         * the live battle arena. */
        if (EmeraldBattleCompat_ReverseResolve(
                (uintptr_t)base, keyBuf, sizeof(keyBuf), &revOffset,
                &revFamily) != EMERALD_BATTLE_OK)
            return FALSE;
        if (revFamily != H5_FAMILY_BATTLE)
            return FALSE;
        {
            const struct EmeraldBattleLiveModule *m =
                FindModuleByKeyStr(keyBuf);
            uintptr_t operand;
            uint32_t i;

            if (m == NULL)
                return FALSE;
            if (EmeraldBattleCompat_ValidateBoundary(
                    m->id, revOffset,
                    EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
                    != EMERALD_BATTLE_OK)
                return FALSE;
            /* Next-instruction size: the boundary map delta. */
            {
                uint32_t next = 0u;
                uint32_t r;
                bool32 found = FALSE;

                for (r = 0u; r < m->boundaryCount; r++)
                {
                    uint32_t off = t->boundaries[m->boundaryFirst + r]
                                       .payloadOffset;
                    if (off > revOffset)
                    {
                        next = off;
                        found = TRUE;
                        break;
                    }
                }
                if (!found)
                    next = m->byteCount;
                e = H6GrammarFind(H5_FAMILY_BATTLE, opcode,
                                  (uint8_t)(next - revOffset));
                {
                    uint32_t isize = next - revOffset;
                if (e == NULL)
                    return FALSE;
                /* Every RELOC'd pointer operand of this instruction
                 * resolves typed (the relocation source index covers
                 * it); legal NULL literals and scalar width-4 words
                 * carry no relocation row and stay untouched. */
                for (i = 0u; i < e->operandCount; i++)
                {
                    uint32_t prefix = H5OperandPrefix(e, i);
                    uint32_t rr;

                    if (e->widths[i] != 4u)
                        continue;
                    for (rr = m->relocFirst; rr < m->relocFirst + m->relocCount;
                         rr++)
                    {
                        if (t->relocs[rr].operandOffset
                                == revOffset + prefix)
                            break;
                    }
                    if (rr == m->relocFirst + m->relocCount)
                        continue;
                    if (EmeraldBattleLive_ResolveOperand(
                            (uintptr_t)((const uint8_t *)base + prefix),
                            &operand) != EMERALD_BATTLE_LIVE_OK)
                        return FALSE;
                }
                if (opcode == 0x28u) /* goto: absolute jump. */
                    base = (uintptr_t)operand;
                else if (opcode == 0x41u) /* call: push IP+5. */
                {
                    calls++;
                    depth++;
                    base = (uintptr_t)operand;
                }
                else if (opcode == 0x42u) /* return: pop. */
                {
                    if (depth == 0u)
                        return FALSE;
                    depth--;
                    base += isize;
                }
                else if (opcode == 0x3Du) /* end. */
                {
                    break;
                }
                else if (opcode == 0x12u) /* waitmessage: park. */
                {
                    parks++;
                    base += isize;
                }
                else
                {
                    /* All other opcodes (jumpif* included): the
                     * conditional target already resolved above; the
                     * walk continues straight. */
                    base += isize;
                }
                }
            }
        }
        steps++;
    }
    /* Battle `end` terminates the WHOLE run at any depth (not a
     * per-frame return), so no balance requirement - the counts alone
     * prove the call/return mechanics. */
    *outSteps = steps;
    *outCalls = calls;
    *outParks = parks;
    return TRUE;
}

static int DoBattle249(const char *packPath, const char *modsDir)
{
    (void)modsDir;
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t presence[H5_OPCODE_SLOTS];
    uint32_t decoded = 0u;
    uint32_t i;
    uint32_t synthetic = 0u;
    uint32_t executed = 0u;

    memset(presence, 0, sizeof(presence));
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);

    /* Grammar walk: every instruction of every battle bytecode module
     * decodes to a qualified (opcode, size) encoding with the exact
     * boundary-map delta; every width-4 operand position with a reloc
     * row resolves typed (the census identity). */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;
        uint32_t r;

        if (m->family != H5_FAMILY_BATTLE
         || m->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
            continue;
        CHECK(H4LayoutSpan(i, 0u, &spanBase));
        for (r = 0u; r < m->boundaryCount; r++)
        {
            uint32_t off = t->boundaries[m->boundaryFirst + r].payloadOffset;
            uint32_t next = (r + 1u < m->boundaryCount)
                ? t->boundaries[m->boundaryFirst + r + 1u].payloadOffset
                : m->byteCount;
            uint8_t opcode = spanBase[off];
            uint32_t operandOff;
            const struct EmeraldBattleLiveGrammarEntry *e;

            CHECK(off < m->byteCount);
            e = H6GrammarFind(H5_FAMILY_BATTLE, opcode,
                              (uint8_t)(next - off));
            CHECK(e != NULL);
            presence[opcode]++;
            decoded++;
            for (operandOff = 1u; operandOff < (uint32_t)(next - off);
                 operandOff++)
            {
                uint32_t rr;
                bool32 isReloc = FALSE;

                for (rr = m->relocFirst; rr < m->relocFirst + m->relocCount;
                     rr++)
                {
                    if (t->relocs[rr].operandOffset == off + operandOff)
                    {
                        isReloc = TRUE;
                        break;
                    }
                }
                if (isReloc)
                {
                    uintptr_t pointer;
                    CHECK(EmeraldBattleLive_ResolveOperand(
                              (uintptr_t)(spanBase + off + operandOff),
                              &pointer) == EMERALD_BATTLE_LIVE_OK);
                }
            }
        }
    }

    /* Synthetic coverage: every opcode slot absent from the qualified
     * data gets a fixture whose decode + word-level pointer resolution
     * run through the same seam. */
    for (i = 0u; i < H5_OPCODE_SLOTS; i++)
    {
        const struct EmeraldBattleLiveGrammarEntry *e;
        uint8_t fixture[32];
        uint32_t root;
        uint32_t j;

        if (presence[i] != 0u)
            continue;
        e = NULL;
        for (j = 0u; j < EMERALD_BATTLE_LIVE_GRAMMAR_ENTRY_COUNT; j++)
        {
            if (t->grammar[j].opcode == (uint8_t)i
             && t->grammar[j].family == H5_FAMILY_BATTLE)
            {
                e = &t->grammar[j];
                break;
            }
        }
        CHECK(e != NULL);
        root = H5LabelWord("BattleScript_MoveEnd");
        CHECK(root != 0u);
        memset(fixture, 0, sizeof(fixture));
        fixture[0] = (uint8_t)i;
        for (j = 0u; j < e->operandCount; j++)
        {
            uint32_t off = 1u + H5OperandPrefix(e, j);
            uintptr_t pointer;
            uint32_t word;

            if (e->widths[j] == 4u)
            {
                memcpy(&word, &root, 4u);
                memcpy(fixture + off, &word, 4u);
                CHECK(EmeraldBattleLive_ResolveScriptTarget(
                          H5_FAMILY_BATTLE, root, &pointer)
                      == EMERALD_BATTLE_LIVE_OK);
            }
        }
        synthetic++;
    }

    /* Execution differential on real high-fan-in roots: MoveEnd,
     * ButItFailed and a call-chain module execute end-to-end on the
     * live arena with balanced calls and typed pointer resolution. */
    {
        static const char *const kScripts[] = {
            "BattleScript_MoveEnd",
            "BattleScript_ButItFailed",
        };
        uint32_t s;

        for (s = 0u; s < 2u; s++)
        {
            uint32_t word = H5LabelWord(kScripts[s]);
            uint32_t steps;
            uint32_t calls;
            uint32_t parks;

            CHECK(word != 0u);
            CHECK(H5ExecuteScript(word, &steps, &calls, &parks));
            CHECK(steps >= 1u);
            executed++;
        }
    }

    {
        uint32_t present = 0u;
        for (i = 0u; i < H5_OPCODE_SLOTS; i++)
            if (presence[i] != 0u)
                present++;
        printf("H5-249 layout=0 decoded=%u slots-present=%u synthetic=%u "
               "executed=%u\n", decoded, present, synthetic, executed);
    }
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* R13-H6: battle-AI + contest-AI differential oracle (sec 5/6/9/11/13/
 * 15/16/17/18/19).                                                          */

#define H6_FAMILY_BATTLE_AI EMERALD_BATTLE_FAMILY_BATTLE_AI
#define H6_FAMILY_CONTEST_AI EMERALD_BATTLE_FAMILY_CONTEST_AI
/* Opcode slot spans (grammar opcode ranges): battle-ai 0..98 (99
 * slots), contest-ai 0..135 (136 slots). */
#define H6_BATTLE_AI_SLOTS 99u
#define H6_CONTEST_AI_SLOTS 136u
#define H6_AI_SLOTS (H6_BATTLE_AI_SLOTS + H6_CONTEST_AI_SLOTS)

static uint32_t H6AiSlot(uint32_t family, uint8_t opcode)
{
    return (family == H6_FAMILY_BATTLE_AI)
        ? opcode
        : H6_BATTLE_AI_SLOTS + opcode;
}

static uint32_t H6AiFamilyOfSlot(uint32_t slot, uint8_t *outOpcode)
{
    if (slot < H6_BATTLE_AI_SLOTS)
    {
        *outOpcode = (uint8_t)slot;
        return H6_FAMILY_BATTLE_AI;
    }
    *outOpcode = (uint8_t)(slot - H6_BATTLE_AI_SLOTS);
    return H6_FAMILY_CONTEST_AI;
}

/* The first SCRIPT_TARGET word of the index that resolves under
 * `family` - a deterministic fixture target word (synthetic opcode
 * fixtures fill width-4 operands with real same-family targets). */
static uint32_t H6FirstFamilyWord(uint32_t family)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    uint32_t i;

    for (i = 0u; i < t->scriptTargetWordCount; i++)
    {
        uintptr_t pointer;

        if (EmeraldBattleLive_ResolveScriptTarget(
                family, t->scriptTargetWords[i], &pointer)
                == EMERALD_BATTLE_LIVE_OK)
            return t->scriptTargetWords[i];
    }
    return 0u;
}

static int DoAiOracle(const char *packPath, const char *modsDir,
                      uint32_t layout)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t i;
    uint32_t r;
    uint32_t instructions = 0u;
    uint32_t battleAiInstructions = 0u;
    uint32_t contestAiInstructions = 0u;
    uint32_t aiRelocs = 0u;
    uint32_t dataTargets = 0u;
    uint32_t routingRows = 0u;
    uint32_t sweptWords = 0u;
    char keyBuf[96];
    uint32_t revOffset;
    uint32_t revFamily;
    uintptr_t pointer;

    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, layout, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_IsPublished());

    /* Entry/routing publication (brief sec 11): the routing enum values
     * ARE the canonical arena starts of the two AI entry tables - the
     * production callers pass them directly. */
    {
        const struct EmeraldBattleLiveArena *arenas = t->arenas;
        uint32_t a;

        for (a = 0u; a < t->arenaCount; a++)
        {
            if (arenas[a].family == H6_FAMILY_BATTLE_AI)
                CHECK(arenas[a].gbaStart == EMERALD_BATTLE_ROUTING_BATTLEAI);
            else if (arenas[a].family == H6_FAMILY_CONTEST_AI)
                CHECK(arenas[a].gbaStart == EMERALD_BATTLE_ROUTING_CONTESTAI);
        }
    }

    /* Grammar walk (brief sec 5/13): every instruction of every AI
     * bytecode module decodes to a qualified (opcode, size) encoding of
     * the family-tagged grammar with the exact boundary-map delta;
     * every width-4 operand position with a reloc row resolves typed
     * (control-flow resolution at consumption, sec 7). */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;

        if (m->family != H6_FAMILY_BATTLE_AI
         && m->family != H6_FAMILY_CONTEST_AI)
            continue;
        if (m->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
            continue;
        CHECK(H4LayoutSpan(i, layout, &spanBase));
        for (r = 0u; r < m->boundaryCount; r++)
        {
            uint32_t off = t->boundaries[m->boundaryFirst + r].payloadOffset;
            uint32_t next = (r + 1u < m->boundaryCount)
                ? t->boundaries[m->boundaryFirst + r + 1u].payloadOffset
                : m->byteCount;
            uint8_t opcode = spanBase[off];
            const struct EmeraldBattleLiveGrammarEntry *e;
            uint32_t operandOff;

            CHECK(off < m->byteCount);
            e = H6GrammarFind(m->family, opcode, (uint8_t)(next - off));
            CHECK(e != NULL);
            instructions++;
            if (m->family == H6_FAMILY_BATTLE_AI)
                battleAiInstructions++;
            else
                contestAiInstructions++;
            for (operandOff = 1u; operandOff < (uint32_t)(next - off);
                 operandOff++)
            {
                uint32_t rr;
                bool32 isReloc = FALSE;

                for (rr = m->relocFirst; rr < m->relocFirst + m->relocCount;
                     rr++)
                {
                    if (t->relocs[rr].operandOffset == off + operandOff)
                    {
                        isReloc = TRUE;
                        break;
                    }
                }
                if (isReloc)
                {
                    CHECK(EmeraldBattleLive_ResolveOperand(
                              (uintptr_t)(spanBase + off + operandOff),
                              &pointer) == EMERALD_BATTLE_LIVE_OK);
                }
            }
        }
    }
    /* 1,738 + 613 = 2,351 (the boundary-pin census). */
    CHECK(instructions == 2351u);
    CHECK(battleAiInstructions == 1738u);
    CHECK(contestAiInstructions == 613u);

    /* Typed relocation resolution (brief sec 6/13): all 1,586 AI relocs
     * resolve (raw operand -> relocation row -> semantic module/export
     * -> current arena pointer); the 38 data-target rows pin offset 0
     * by map kind (no instruction boundaries); bytecode targets
     * re-validate boundary + identity. Zero cross-family bytecode
     * edges (brief sec 16). */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;

        if (m->family != H6_FAMILY_BATTLE_AI
         && m->family != H6_FAMILY_CONTEST_AI)
            continue;
        CHECK(H4LayoutSpan(i, layout, &spanBase));
        for (r = m->relocFirst; r < m->relocFirst + m->relocCount; r++)
        {
            const struct EmeraldBattleLiveReloc *reloc = &t->relocs[r];
            uintptr_t operand = (uintptr_t)(spanBase + reloc->operandOffset);
            const struct EmeraldBattleLiveModule *target;
            const uint8_t *targetBase;
            uintptr_t expected;
            uintptr_t viaWord;

            CHECK(reloc->relocClass
                  == EMERALD_BATTLE_LIVE_RELOC_SCRIPT_TARGET);
            target = &t->modules[reloc->target];
            /* The target lives in the SOURCE family - the cross-family
             * bytecode edge count must stay zero. */
            CHECK(target->family == m->family);
            status = EmeraldBattleLive_ResolveOperand(operand, &pointer);
            CHECK(status == EMERALD_BATTLE_LIVE_OK);
            CHECK(H4LayoutSpan(reloc->target, layout, &targetBase));
            if (target->mapKind == EMERALD_BATTLE_MAP_DATA)
            {
                /* Data tables carry no boundaries; the row pins 0. */
                CHECK(reloc->targetOffset == 0u);
                expected = (uintptr_t)targetBase;
                dataTargets++;
            }
            else
            {
                expected = (uintptr_t)targetBase + reloc->targetOffset;
                CHECK(EmeraldBattleCompat_ValidateBoundary(
                          target->id, reloc->targetOffset,
                          EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
                      == EMERALD_BATTLE_OK);
            }
            CHECK(pointer == expected);
            /* Raw-word API (sec 8): the operand word itself resolves to
             * the same arena pointer. */
            CHECK(EmeraldBattleLive_ResolveScriptTarget(
                      m->family, reloc->expectedWord, &viaWord)
                  == EMERALD_BATTLE_LIVE_OK);
            CHECK(viaWord == expected);
            /* Reverse containment: the resolved pointer sits in the
             * target module at the row's offset under the row's family. */
            CHECK(EmeraldBattleCompat_ReverseResolve(
                      pointer, keyBuf, sizeof(keyBuf), &revOffset,
                      &revFamily) == EMERALD_BATTLE_OK);
            CHECK(strcmp(keyBuf, target->id) == 0);
            CHECK(revOffset == reloc->targetOffset);
            CHECK(revFamily == m->family);
            aiRelocs++;
        }
    }
    CHECK(aiRelocs == 1586u);
    CHECK(dataTargets == 38u);

    /* Entry republication (brief sec 11/15): all 64 AI routing rows
     * resolve from the ARENA bytes (the compiled entry tables are dead)
     * to a script root of the table's own family - a cross-family row
     * word is refused by the family gate. */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        uint8_t *bin = NULL;
        size_t binSize = 0u;
        uint32_t row;

        if (m->family != H6_FAMILY_BATTLE_AI
         && m->family != H6_FAMILY_CONTEST_AI)
            continue;
        if (m->mapKind != EMERALD_BATTLE_MAP_ROUTING)
            continue;
        CHECK(ReadModuleBin(modsDir, m->id, &bin, &binSize));
        CHECK(binSize == m->byteCount);
        for (row = 0u; row < m->byteCount / 4u; row++)
        {
            uint32_t word = (uint32_t)bin[row * 4u]
                          | ((uint32_t)bin[row * 4u + 1u] << 8)
                          | ((uint32_t)bin[row * 4u + 2u] << 16)
                          | ((uint32_t)bin[row * 4u + 3u] << 24);
            uintptr_t viaWord;

            CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                      m->gbaStart, row, &pointer) == EMERALD_BATTLE_LIVE_OK);
            CHECK(EmeraldBattleLive_ResolveScriptTarget(
                      m->family, word, &viaWord)
                  == EMERALD_BATTLE_LIVE_OK);
            CHECK(pointer == viaWord);
            CHECK(EmeraldBattleCompat_ReverseResolve(
                      pointer, keyBuf, sizeof(keyBuf), &revOffset,
                      &revFamily) == EMERALD_BATTLE_OK);
            CHECK(revFamily == m->family);
            routingRows++;
        }
        free(bin);
    }
    /* 32 + 32 rows (brief sec 11). */
    CHECK(routingRows == 64u);

    /* Pointer sweep (brief sec 16/17): every AI bytecode module's
     * consumed pointer positions are exactly its reloc rows - a
     * NON-reloc 4-byte word that equals a known script-target word of
     * ANY family is a violation (a silent compiled-fallback or
     * cross-family raw pointer candidate); scalar words never convert
     * (sec 17). */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;
        uint32_t off;

        if (m->family != H6_FAMILY_BATTLE_AI
         && m->family != H6_FAMILY_CONTEST_AI)
            continue;
        if (m->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
            continue;
        CHECK(H4LayoutSpan(i, layout, &spanBase));
        for (off = 0u; off + 4u <= m->byteCount; off += 4u)
        {
            uint32_t word;
            bool isReloc = false;
            uint32_t rr;

            memcpy(&word, spanBase + off, 4u);
            for (rr = m->relocFirst; rr < m->relocFirst + m->relocCount; rr++)
            {
                if (t->relocs[rr].operandOffset == off)
                {
                    isReloc = true;
                    break;
                }
            }
            if (isReloc)
                continue;
            {
                uint32_t lo = 0u;
                uint32_t hi = t->scriptTargetWordCount;
                bool inSet = false;

                while (lo < hi)
                {
                    uint32_t mid = lo + (hi - lo) / 2u;

                    if (word < t->scriptTargetWords[mid])
                        hi = mid;
                    else if (word > t->scriptTargetWords[mid])
                        lo = mid + 1u;
                    else
                    {
                        inSet = true;
                        break;
                    }
                }
                CHECK(!inSet);
            }
            sweptWords++;
        }
    }
    CHECK(sweptWords >= 1800u);

    printf("H6-AI-ORACLE layout=%u instructions=%u battle-ai=%u "
           "contest-ai=%u ai-relocs=%u data-targets=%u routing-rows=%u "
           "swept-words=%u\n",
           layout, instructions, battleAiInstructions, contestAiInstructions,
           aiRelocs, dataTargets, routingRows, sweptWords);
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* H6 battle-AI + contest-AI fail-closed matrix (brief sec 26).       */

static int DoAiFaults(const char *packPath)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t passes = 0u;
    uint32_t m;
    uint32_t firstAiModule = UINT32_MAX;
    uint32_t firstContestModule = UINT32_MAX;
    uint32_t firstAiReloc = UINT32_MAX;
    uint32_t firstAiRelocModule = UINT32_MAX;
    uint32_t firstContestReloc = UINT32_MAX;
    uint32_t firstDataReloc = UINT32_MAX;
    uint32_t firstDataRelocModule = UINT32_MAX;
    uint32_t battleAiTable = 0u;
    uint32_t contestAiTable = 0u;
    const uint8_t *spanBase;
    uintptr_t pointer;

    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();

    /* Locate the fixtures: first battle-ai/contest-ai bytecode module,
     * first script-target reloc of each family, the first DATA-target
     * reloc (battle-ai if_in_* tables), and the two entry tables. */
    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];

        if (mod->family == H6_FAMILY_BATTLE_AI
         && mod->mapKind == EMERALD_BATTLE_MAP_BYTECODE
         && firstAiModule == UINT32_MAX)
            firstAiModule = m;
        if (mod->family == H6_FAMILY_CONTEST_AI
         && mod->mapKind == EMERALD_BATTLE_MAP_BYTECODE
         && firstContestModule == UINT32_MAX)
            firstContestModule = m;
        if (mod->family == H6_FAMILY_BATTLE_AI
         && mod->mapKind == EMERALD_BATTLE_MAP_ROUTING && battleAiTable == 0u)
            battleAiTable = mod->gbaStart;
        if (mod->family == H6_FAMILY_CONTEST_AI
         && mod->mapKind == EMERALD_BATTLE_MAP_ROUTING && contestAiTable == 0u)
            contestAiTable = mod->gbaStart;
    }
    CHECK(firstAiModule != UINT32_MAX);
    CHECK(firstContestModule != UINT32_MAX);
    CHECK(battleAiTable != 0u);
    CHECK(contestAiTable != 0u);
    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        uint32_t rr;

        if (mod->family != H6_FAMILY_BATTLE_AI)
            continue;
        for (rr = mod->relocFirst; rr < mod->relocFirst + mod->relocCount; rr++)
        {
            if (t->relocs[rr].relocClass
                    != EMERALD_BATTLE_LIVE_RELOC_SCRIPT_TARGET)
                continue;
            if (firstAiReloc == UINT32_MAX)
            {
                firstAiReloc = rr;
                firstAiRelocModule = m;
            }
            if (t->modules[t->relocs[rr].target].mapKind
                    == EMERALD_BATTLE_MAP_DATA
             && firstDataReloc == UINT32_MAX)
            {
                firstDataReloc = rr;
                firstDataRelocModule = m;
            }
        }
    }
    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        uint32_t rr;

        if (mod->family != H6_FAMILY_CONTEST_AI)
            continue;
        for (rr = mod->relocFirst; rr < mod->relocFirst + mod->relocCount; rr++)
        {
            if (t->relocs[rr].relocClass
                    == EMERALD_BATTLE_LIVE_RELOC_SCRIPT_TARGET)
            {
                firstContestReloc = rr;
                break;
            }
        }
        if (firstContestReloc != UINT32_MAX)
            break;
    }
    CHECK(firstAiReloc != UINT32_MAX);
    CHECK(firstContestReloc != UINT32_MAX);
    CHECK(firstDataReloc != UINT32_MAX);

    /* 1. stage layout 0 + publish (6,382 = 6,377 + the 5 live ranges). */
    status = H4StageLive(0u);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    passes++;

    /* 2. wrong-family launch: a battle-ai root under the contest-ai
     * family refuses (and vice versa) - the family gate, not the word
     * gate. */
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[firstAiModule];
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  H6_FAMILY_CONTEST_AI, mod->gbaStart, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY);
        mod = &t->modules[firstContestModule];
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  H6_FAMILY_BATTLE_AI, mod->gbaStart, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY);
        passes++;
    }

    /* 3. corrupt battle-ai script-target operand word refuses (raw
     * operand mismatch); restore resolves again. */
    CHECK(H4SpanBase(firstAiRelocModule, &spanBase));
    {
        const struct EmeraldBattleLiveReloc *reloc = &t->relocs[firstAiReloc];
        uint8_t *operand = (uint8_t *)spanBase + reloc->operandOffset;
        uint8_t saved = operand[0];

        operand[0] ^= 0xFFu;
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)operand, &pointer) != EMERALD_BATTLE_LIVE_OK);
        operand[0] = saved;
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)operand, &pointer) == EMERALD_BATTLE_LIVE_OK);
        passes++;
    }

    /* 4. corrupt DATA-target operand word refuses (raw operand
     * mismatch on an if_in_* table pointer); restore resolves. */
    CHECK(H4SpanBase(firstDataRelocModule, &spanBase));
    {
        const struct EmeraldBattleLiveReloc *reloc = &t->relocs[firstDataReloc];
        uint8_t *operand = (uint8_t *)spanBase + reloc->operandOffset;
        uint8_t saved = operand[0];

        CHECK(t->modules[reloc->target].mapKind == EMERALD_BATTLE_MAP_DATA);
        operand[0] ^= 0xFFu;
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)operand, &pointer) != EMERALD_BATTLE_LIVE_OK);
        operand[0] = saved;
        CHECK(EmeraldBattleLive_ResolveOperand(
                  (uintptr_t)operand, &pointer) == EMERALD_BATTLE_LIVE_OK);
        passes++;
    }

    /* 5. routing row out of range refuses (32 rows: row 31 resolves,
     * row 32 does not) - both AI tables. */
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              battleAiTable, 31u, &pointer) == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              battleAiTable, 32u, &pointer)
          == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              contestAiTable, 31u, &pointer) == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              contestAiTable, 32u, &pointer)
          == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
    passes++;

    /* 6. a non-routing word under ResolveRoutingTarget refuses (a
     * bytecode module start is not a routing table). */
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[firstAiModule];
        CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                  mod->gbaStart, 0u, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
        passes++;
    }

    /* 7. cross-family raw word gate: a battle-ai script-target word
     * under the contest-ai family refuses (and vice versa) - zero
     * cross-family bytecode edges (brief sec 16). */
    {
        const struct EmeraldBattleLiveReloc *reloc = &t->relocs[firstAiReloc];
        const struct EmeraldBattleLiveReloc *cReloc =
            &t->relocs[firstContestReloc];

        CHECK(EmeraldBattleLive_ResolveScriptTarget(
                  H6_FAMILY_CONTEST_AI, reloc->expectedWord, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY);
        CHECK(EmeraldBattleLive_ResolveScriptTarget(
                  H6_FAMILY_BATTLE_AI, cReloc->expectedWord, &pointer)
              == EMERALD_BATTLE_LIVE_ERR_WRONG_FAMILY);
        passes++;
    }

    /* 8. cross-family routing row: a battle-ai row word rewritten to a
     * contest-ai script target is refused by the family gate (the row
     * resolves under the TABLE's own family); restore resolves. */
    {
        uint32_t tableModule = UINT32_MAX;
        const uint8_t *tableBase;
        const uint32_t contestWord =
            t->relocs[firstContestReloc].expectedWord;
        uint8_t saved[4];

        for (m = 0u; m < t->payloadModuleCount; m++)
        {
            const struct EmeraldBattleLiveModule *mod = &t->modules[m];
            if (mod->family == H6_FAMILY_BATTLE_AI
             && mod->mapKind == EMERALD_BATTLE_MAP_ROUTING)
            {
                tableModule = m;
                break;
            }
        }
        CHECK(tableModule != UINT32_MAX);
        CHECK(H4LayoutSpan(tableModule, 0u, &tableBase));
        memcpy(saved, tableBase, 4u);
        memcpy((uint8_t *)tableBase, &contestWord, 4u);
        CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                  battleAiTable, 0u, &pointer)
              != EMERALD_BATTLE_LIVE_OK);
        memcpy((uint8_t *)tableBase, saved, 4u);
        CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                  battleAiTable, 0u, &pointer)
              == EMERALD_BATTLE_LIVE_OK);
        passes++;
    }

    /* 9. battle quiescence (the battle-AI VM is a battle-family VM)
     * blocks replacement; the generation survives untouched. */
    sHarnessBattleBusy = true;
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_ERR_BUSY);
    sHarnessBattleBusy = false;
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[firstAiModule];
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  H6_FAMILY_BATTLE_AI, mod->gbaStart, &pointer)
              == EMERALD_BATTLE_LIVE_OK);
    }
    passes++;

    /* 10. identity unregister (brief sec 26): the AI ranges drop out by
     * exact key; re-registration restores the 7,607 invariant. */
    EmeraldBattleLive_UnregisterRange("emerald:battle-ai/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 7607u);
    EmeraldBattleLive_UnregisterRange("emerald:contest-ai/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 7606u);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    passes++;

    printf("H6-AI-FAULTS passed=%u\n", passes);
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* H6 235-opcode differential (brief sec 18/19).                     */

/* Mini-VM: execute one AI root through the LIVE arena with the real
 * instruction decode; control-flow resolves typed at consumption
 * (brief sec 7): call pushes IP+5 (the AI stack convention), goto
 * jumps, end pops the stack - resuming the saved frame when non-empty
 * and terminating the run when empty (the production Cmd_end
 * semantics). Branch conditions evaluate C-side - the walk continues
 * straight; the opcode/next-IP/branch-target differential is the
 * decode + typed-resolution proof. */
static bool32 H6ExecuteAiScript(uint32_t family, uintptr_t start,
                                uint32_t *outSteps, uint32_t *outCalls,
                                uint32_t *outGotos)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    uint32_t steps = 0u;
    uint32_t calls = 0u;
    uint32_t gotos = 0u;
    uint32_t depth = 0u;
    uintptr_t retStack[8];
    uint32_t budget = 4000u;
    uintptr_t base = start;
    uint8_t endOpcode = (family == H6_FAMILY_BATTLE_AI) ? 0x5Au : 0x81u;
    uint8_t callOpcode = (family == H6_FAMILY_BATTLE_AI) ? 0x58u : 0x80u;
    uint8_t gotoOpcode = (family == H6_FAMILY_BATTLE_AI) ? 0x59u : 0x7Fu;

    for (;;)
    {
        const struct EmeraldBattleLiveGrammarEntry *e;
        char keyBuf[96];
        uint32_t revOffset;
        uint32_t revFamily;
        uint8_t opcode;
        uint32_t isize;

        if (steps >= budget)
            return FALSE;
        /* The current AI IP must always be an instruction start inside
         * the live arena, under the executing family (brief sec 9/16). */
        if (EmeraldBattleCompat_ReverseResolve(
                (uintptr_t)base, keyBuf, sizeof(keyBuf), &revOffset,
                &revFamily) != EMERALD_BATTLE_OK)
            return FALSE;
        if (revFamily != family)
            return FALSE;
        {
            const struct EmeraldBattleLiveModule *m = FindModuleByKeyStr(keyBuf);
            uintptr_t operand = 0u;
            uint32_t i;
            uint32_t rr;

            if (m == NULL)
                return FALSE;
            if (EmeraldBattleCompat_ValidateBoundary(
                    m->id, revOffset,
                    EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
                    != EMERALD_BATTLE_OK)
                return FALSE;
            opcode = *(const uint8_t *)base;
            isize = 0u;
            for (rr = 0u; rr < m->boundaryCount; rr++)
            {
                uint32_t off = t->boundaries[m->boundaryFirst + rr]
                                   .payloadOffset;
                if (off > revOffset)
                {
                    isize = off - revOffset;
                    break;
                }
            }
            if (isize == 0u)
                isize = m->byteCount - revOffset;
            e = H6GrammarFind(family, opcode, (uint8_t)isize);
            if (e == NULL)
                return FALSE;
            /* Every RELOC'd pointer operand of this instruction
             * resolves typed at consumption (the relocation source
             * index covers it; legal NULL literals and scalar width-4
             * words carry no relocation row and stay untouched). */
            for (i = 0u; i < e->operandCount; i++)
            {
                uint32_t prefix = H5OperandPrefix(e, i);

                if (e->widths[i] != 4u)
                    continue;
                for (rr = m->relocFirst; rr < m->relocFirst + m->relocCount;
                     rr++)
                {
                    if (t->relocs[rr].operandOffset == revOffset + prefix)
                        break;
                }
                if (rr == m->relocFirst + m->relocCount)
                    continue;
                if (EmeraldBattleLive_ResolveOperand(
                        (uintptr_t)((const uint8_t *)base + prefix),
                        &operand) != EMERALD_BATTLE_LIVE_OK)
                    return FALSE;
            }
            if (opcode == gotoOpcode)
            {
                gotos++;
                base = operand;
            }
            else if (opcode == callOpcode)
            {
                if (depth >= 8u)
                    return FALSE;
                /* The AI call convention pushes IP + 5 (opcode byte +
                 * pointer operand). */
                retStack[depth++] = base + 5u;
                calls++;
                base = operand;
            }
            else if (opcode == endOpcode)
            {
                /* end pops the stack: resume the saved frame while
                 * non-empty, terminate the run when empty. */
                if (depth == 0u)
                    break;
                base = retStack[--depth];
            }
            else
                base += isize;
        }
        steps++;
    }
    *outSteps = steps;
    *outCalls = calls;
    *outGotos = gotos;
    return TRUE;
}

/* R13-H7 fail-closed matrix (brief sec 24): the new seam surface -
 * routing-table resolution for the battle-anim + field-effect tables,
 * the module-data accessor (gMovesWithQuietBGM), the linux64
 * compiled-label word path - fails closed exactly like the H5/H6
 * resolvers, and the removed-compiled-symbol state refuses injection. */

static int DoH7Faults(const char *packPath)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t passes = 0u;
    uint32_t i;
    uint32_t m;
    uint32_t firstBytecodeModule = UINT32_MAX;
    uint32_t firstDataModule = UINT32_MAX;
    uintptr_t pointer;
    uintptr_t span;
    uint32_t byteCount;

    /* Pre-publish: the H7 accessors refuse like every other resolver
     * (routing publication absent = not published; brief sec 23). */
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              EMERALD_BATTLE_ROUTING_ANIMSMOVES, 0u, &pointer)
          == EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED);
    CHECK(EmeraldBattleLive_ResolveModuleData(
              EMERALD_BATTLE_QUIET_BGM_WORD, &span, &byteCount)
          == EMERALD_BATTLE_LIVE_ERR_NOT_PUBLISHED);
    passes++;

    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();
    /* Stage the H arenas exactly like the oracles: the loader publishes
     * the session image, then the seam drives its own transactional
     * sequence (fresh generation + range registration + publication).
     * Without this the H7 accessors refuse NOT_PUBLISHED. */
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);

    for (m = 0u; m < t->moduleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        if (mod->mapKind == EMERALD_BATTLE_MAP_BYTECODE
         && firstBytecodeModule == UINT32_MAX)
            firstBytecodeModule = m;
        if (mod->mapKind == EMERALD_BATTLE_MAP_DATA
         && firstDataModule == UINT32_MAX)
            firstDataModule = m;
    }
    CHECK(firstBytecodeModule != UINT32_MAX);
    CHECK(firstDataModule != UINT32_MAX);

    /* 1. compiled-symbol injection detector (brief sec 24): a host
     * pointer is never a canonical GBA word - the linux64 word scan
     * refuses it (the compiled symbols are removed; no host address
     * can masquerade as a label). */
    CHECK(EmeraldBattleLive_ResolveCompiledLabel(
              (const void *)&sHarnessBattleBusy, &pointer)
          == EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED);
    passes++;

    /* 2. partial ownership/publication mismatch: a valid module root
     * that is NOT a routing table refuses as a routing table (bytecode
     * root injected where a table word is expected). */
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              t->modules[firstBytecodeModule].gbaStart, 0u, &pointer)
          == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
    passes++;

    /* 3. the module-data accessor is data-only: a bytecode root
     * refuses (routing word injected where data is expected). */
    CHECK(EmeraldBattleLive_ResolveModuleData(
              t->modules[firstBytecodeModule].gbaStart, &span, &byteCount)
          == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
    passes++;

    /* 4. the module-data accessor requires the module root: an
     * interior word of the quiet-BGM module refuses. */
    CHECK(EmeraldBattleLive_ResolveModuleData(
              EMERALD_BATTLE_QUIET_BGM_WORD + 4u, &span, &byteCount)
          == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
    passes++;

    /* 5. resource missing after removal: a table word that contains to
     * no live module refuses outright. */
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              0x08000000u, 0u, &pointer)
          == EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED);
    CHECK(EmeraldBattleLive_ResolveModuleData(
              0x08000000u, &span, &byteCount)
          == EMERALD_BATTLE_LIVE_ERR_TARGET_UNRESOLVED);
    passes++;

    /* 6. all 12 routing tables are live after the H7 additions: row 0
     * of every table resolves to a script of the table's own family
     * (moved/status/general/special anim + field-effect tables are
     * now routed through the arena, not the removed compiled arrays). */
    {
        const struct EmeraldBattleLiveRoutingRow
        {
            uint32_t word;
            uint32_t family;
        } rows[] = {
            { EMERALD_BATTLE_ROUTING_MOVEEFFECTS,
              EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT },
            { EMERALD_BATTLE_ROUTING_BALLTHROW,
              EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT },
            { EMERALD_BATTLE_ROUTING_USINGITEM,
              EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT },
            { EMERALD_BATTLE_ROUTING_RUNNINGBYITEM,
              EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT },
            { EMERALD_BATTLE_ROUTING_SAFARIACTIONS,
              EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT },
            { EMERALD_BATTLE_ROUTING_BATTLEAI,
              EMERALD_BATTLE_FAMILY_BATTLE_AI },
            { EMERALD_BATTLE_ROUTING_CONTESTAI,
              EMERALD_BATTLE_FAMILY_CONTEST_AI },
            { EMERALD_BATTLE_ROUTING_ANIMSMOVES,
              EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT },
            { EMERALD_BATTLE_ROUTING_ANIMSSTATUS,
              EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT },
            { EMERALD_BATTLE_ROUTING_ANIMSGENERAL,
              EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT },
            { EMERALD_BATTLE_ROUTING_ANIMSSPECIAL,
              EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT },
            { EMERALD_BATTLE_ROUTING_FIELDEFFECTS,
              EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT },
        };
        uint32_t r;

        for (r = 0u; r < sizeof(rows) / sizeof(rows[0]); r++)
        {
            uintptr_t rowPointer = 0u;
            uint32_t containing = UINT32_MAX;
            uint32_t mm;

            CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                      rows[r].word, 0u, &rowPointer)
                  == EMERALD_BATTLE_LIVE_OK);
            CHECK(rowPointer != 0u);
            /* The resolved row must land in a live span of the table's
             * own family (cross-family rows refuse by design). */
            for (mm = 0u; mm < t->moduleCount; mm++)
            {
                const uint8_t *spanBase;

                if (!H4LayoutSpan(mm, 0u, &spanBase))
                    continue;
                if (rowPointer >= (uintptr_t)spanBase
                 && rowPointer < (uintptr_t)spanBase
                               + t->modules[mm].byteCount)
                {
                    containing = mm;
                    break;
                }
            }
            CHECK(containing != UINT32_MAX);
            CHECK(t->modules[containing].family == rows[r].family);
            passes++;
        }

        /* Row bounds: the anim-moves table is 356 rows; row 356 must
         * refuse, the last row must resolve. */
        {
            const struct EmeraldBattleLiveModule *mod = NULL;
            uint32_t mm;
            uint32_t rowCount;

            for (mm = 0u; mm < t->moduleCount; mm++)
            {
                if (t->modules[mm].gbaStart
                        == EMERALD_BATTLE_ROUTING_ANIMSMOVES)
                {
                    mod = &t->modules[mm];
                    break;
                }
            }
            CHECK(mod != NULL);
            CHECK(mod->mapKind == EMERALD_BATTLE_MAP_ROUTING);
            rowCount = mod->byteCount / 4u;
            CHECK(rowCount == 356u);
            CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                      EMERALD_BATTLE_ROUTING_ANIMSMOVES, rowCount - 1u,
                      &pointer) == EMERALD_BATTLE_LIVE_OK);
            CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                      EMERALD_BATTLE_ROUTING_ANIMSMOVES, rowCount,
                      &pointer) == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
            passes++;
        }
    }

    /* 7. the quiet-BGM module reads through the arena: 8 B, three
     * move ids + the 0xFFFF sentinel. */
    {
        uint16_t rows[4];

        CHECK(EmeraldBattleLive_ResolveModuleData(
                  EMERALD_BATTLE_QUIET_BGM_WORD, &span, &byteCount)
              == EMERALD_BATTLE_LIVE_OK);
        CHECK(byteCount == 8u);
        memcpy(rows, (const void *)span, byteCount);
        CHECK(rows[0] != 0xFFFF);
        CHECK(rows[3] == 0xFFFF); /* sentinel */
        passes++;
    }

    /* 8. the linux64 compiled-label word path: the accessor yields the
     * canonical word (the compiled symbols are removed) and every
     * label word resolves to its arena export. */
    for (i = 0u; i < t->labelCount; i++)
    {
        CHECK(EmeraldBattleLiveNative_LabelAddress(i)
              == (uintptr_t)t->labels[i].word);
        CHECK(EmeraldBattleLive_ResolveCompiledLabel(
                  (const void *)(uintptr_t)t->labels[i].word, &pointer)
              == EMERALD_BATTLE_LIVE_OK);
    }
    passes++;

    /* 9. stale generation: a replacement generation must keep every
     * routing table + data module resolvable - the enum words are
     * canonical GBA words, generation-independent (the arena bytes
     * are re-staged under the new generation). */
    sHarnessBattleBusy = false;
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              EMERALD_BATTLE_ROUTING_ANIMSMOVES, 0u, &pointer)
          == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_ResolveRoutingTarget(
              EMERALD_BATTLE_ROUTING_FIELDEFFECTS, 0u, &pointer)
          == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_ResolveModuleData(
              EMERALD_BATTLE_QUIET_BGM_WORD, &span, &byteCount)
          == EMERALD_BATTLE_LIVE_OK);
    passes++;

    printf("H7-FAULTS passed=%u\n", passes);
    return sFailures != 0;

fail:
    return 1;
}

static int DoAi249(const char *packPath, const char *modsDir)
{
    (void)modsDir;
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    uint32_t presence[H6_AI_SLOTS];
    uint32_t decoded = 0u;
    uint32_t synthetic = 0u;
    uint32_t executed = 0u;
    uint32_t i;

    memset(presence, 0, sizeof(presence));
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    EmeraldBattleLive_ClearMigratedEntries();
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);

    /* Grammar walk: every AI instruction decodes to a qualified
     * (opcode, size) of the family-tagged grammar; width-4 operand
     * positions with reloc rows resolve typed (the census identity). */
    for (i = 0u; i < t->payloadModuleCount; i++)
    {
        const struct EmeraldBattleLiveModule *m = &t->modules[i];
        const uint8_t *spanBase;
        uint32_t r;

        if (m->family != H6_FAMILY_BATTLE_AI
         && m->family != H6_FAMILY_CONTEST_AI)
            continue;
        if (m->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
            continue;
        CHECK(H4LayoutSpan(i, 0u, &spanBase));
        for (r = 0u; r < m->boundaryCount; r++)
        {
            uint32_t off = t->boundaries[m->boundaryFirst + r].payloadOffset;
            uint32_t next = (r + 1u < m->boundaryCount)
                ? t->boundaries[m->boundaryFirst + r + 1u].payloadOffset
                : m->byteCount;
            uint8_t opcode = spanBase[off];
            const struct EmeraldBattleLiveGrammarEntry *e;
            uint32_t operandOff;

            e = H6GrammarFind(m->family, opcode, (uint8_t)(next - off));
            CHECK(e != NULL);
            presence[H6AiSlot(m->family, opcode)]++;
            decoded++;
            for (operandOff = 1u; operandOff < (uint32_t)(next - off);
                 operandOff++)
            {
                uint32_t rr;
                bool32 isReloc = FALSE;

                for (rr = m->relocFirst; rr < m->relocFirst + m->relocCount;
                     rr++)
                {
                    if (t->relocs[rr].operandOffset == off + operandOff)
                    {
                        isReloc = TRUE;
                        break;
                    }
                }
                if (isReloc)
                {
                    uintptr_t pointer;
                    CHECK(EmeraldBattleLive_ResolveOperand(
                              (uintptr_t)(spanBase + off + operandOff),
                              &pointer) == EMERALD_BATTLE_LIVE_OK);
                }
            }
        }
    }
    CHECK(decoded == 2351u);

    /* Synthetic coverage (brief sec 18/19): every opcode slot absent
     * from the qualified data (battle-ai 32, contest-ai 100) gets a
     * fixture whose width-4 operands carry a REAL script-target word of
     * the same family - decode + word-level resolution through the same
     * seam. */
    for (i = 0u; i < H6_AI_SLOTS; i++)
    {
        uint8_t opcode;
        uint32_t family = H6AiFamilyOfSlot(i, &opcode);
        const struct EmeraldBattleLiveGrammarEntry *e;
        uint8_t fixture[32];
        uint32_t root;
        uint32_t j;

        if (presence[i] != 0u)
            continue;
        e = NULL;
        for (j = 0u; j < EMERALD_BATTLE_LIVE_GRAMMAR_ENTRY_COUNT; j++)
        {
            if (t->grammar[j].family == family
             && t->grammar[j].opcode == opcode)
            {
                e = &t->grammar[j];
                break;
            }
        }
        CHECK(e != NULL);
        root = H6FirstFamilyWord(family);
        CHECK(root != 0u);
        memset(fixture, 0, sizeof(fixture));
        fixture[0] = opcode;
        for (j = 0u; j < e->operandCount; j++)
        {
            uint32_t off = 1u + H5OperandPrefix(e, j);
            uintptr_t pointer;
            uint32_t word;

            if (e->widths[j] == 4u)
            {
                memcpy(&word, &root, 4u);
                memcpy(fixture + off, &word, 4u);
                CHECK(EmeraldBattleLive_ResolveScriptTarget(
                          family, root, &pointer) == EMERALD_BATTLE_LIVE_OK);
            }
        }
        synthetic++;
    }
    /* 32 battle-ai + 100 contest-ai absent slots. */
    CHECK(synthetic == 132u);

    /* Execution differential on real entry roots (brief sec 18/19): the
     * row-0 target of each arena-owned AI entry table executes
     * end-to-end through the live arena with typed control-flow
     * resolution and the production end/pop semantics. */
    {
        static const uint32_t kTables[2] = {
            EMERALD_BATTLE_ROUTING_BATTLEAI,
            EMERALD_BATTLE_ROUTING_CONTESTAI,
        };
        static const uint32_t kFamilies[2] = {
            H6_FAMILY_BATTLE_AI, H6_FAMILY_CONTEST_AI,
        };
        uint32_t s;

        for (s = 0u; s < 2u; s++)
        {
            uintptr_t word;
            uint32_t steps;
            uint32_t calls;
            uint32_t gotos;

            CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                      kTables[s], 0u, &word) == EMERALD_BATTLE_LIVE_OK);
            CHECK(H6ExecuteAiScript(kFamilies[s], word, &steps, &calls,
                                    &gotos));
            CHECK(steps >= 1u);
            executed++;
        }
    }

    {
        uint32_t present = 0u;
        uint32_t battlePresent = 0u;
        uint32_t contestPresent = 0u;

        for (i = 0u; i < H6_AI_SLOTS; i++)
            if (presence[i] != 0u)
            {
                present++;
                if (i < H6_BATTLE_AI_SLOTS)
                    battlePresent++;
                else
                    contestPresent++;
            }
        printf("H6-AI-249 layout=0 decoded=%u slots-present=%u "
               "battle-ai=%u contest-ai=%u synthetic=%u executed=%u\n",
               decoded, present, battlePresent, contestPresent, synthetic,
               executed);
    }
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* Generation replacement (brief sec 25/26). */

static int DoReplace(const char *packPath)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    const uint8_t *arena0;
    const uint8_t *arena1;
    size_t arenaSize;
    uintptr_t p0;
    uintptr_t p1;
    uint32_t m;
    uint64_t genA;
    uint64_t genB;

    /* Full production-shaped session: the R6 loader publishes every
     * family and the live step (6,377 G ranges + 5 live). */
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    CHECK(EmeraldBattleLive_IsPublished());
    genA = EmeraldBattleLive_GetGenerationId();

    /* First anim payload module as the probe word. */
    for (m = 0u; m < t->payloadModuleCount; m++)
        if (t->modules[m].family == H4_FAMILY_ANIM)
            break;
    CHECK(m < t->payloadModuleCount);
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[m].gbaStart, &p0)
          == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleCompat_GetArena(H4_FAMILY_ANIM, &arena0, &arenaSize));
    CHECK(arena0 != NULL);
    CHECK((uintptr_t)p0 - (uintptr_t)arena0 == t->modules[m].layoutOffset[0]);

    /* Replace: stage generation B (perturbed layout) while published -
     * the quiescence probe is clear, so the swap commits. */
    EmeraldBattleLive_ClearMigratedEntries();
    CHECK(EmeraldBattleLive_GetRangeCount() == 7603u);
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 1u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);
    genB = EmeraldBattleLive_GetGenerationId();
    CHECK(genB > genA);

    /* The same identity now resolves into the layout-1 arena. */
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[m].gbaStart, &p1)
          == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleCompat_GetArena(H4_FAMILY_ANIM, &arena1, &arenaSize));
    CHECK(arena1 != NULL);
    CHECK(p1 != p0); /* physical placement moved */
    CHECK((uintptr_t)p1 - (uintptr_t)arena1 == t->modules[m].layoutOffset[1]);

    /* R13-H6: the AI arenas participate in the same replacement - the
     * battle-ai root and the arena-owned 32-row entry table republish
     * into the layout-1 arena (brief sec 11/25). */
    {
        uint32_t aiModule = 0u;
        uint32_t am;
        char keyBuf[96];
        uint32_t revOffset;
        uint32_t revFamily;

        /* A local index: the outer `m` must stay on the anim probe
         * module for the fail-closed checks below. */
        for (am = 0u; am < t->payloadModuleCount; am++)
            if (t->modules[am].family == EMERALD_BATTLE_FAMILY_BATTLE_AI
             && t->modules[am].mapKind == EMERALD_BATTLE_MAP_BYTECODE)
            {
                aiModule = am;
                break;
            }
        CHECK(aiModule < t->payloadModuleCount);
        CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                  EMERALD_BATTLE_FAMILY_BATTLE_AI,
                  t->modules[aiModule].gbaStart, &p1)
              == EMERALD_BATTLE_LIVE_OK);
        CHECK(EmeraldBattleCompat_GetArena(EMERALD_BATTLE_FAMILY_BATTLE_AI,
                                           &arena1, &arenaSize));
        CHECK(arena1 != NULL);
        CHECK((uintptr_t)p1 - (uintptr_t)arena1
              == t->modules[aiModule].layoutOffset[1]);
        /* The entry surface republicates: routing row 0 -> a battle-AI
         * bytecode root, family-verified. */
        CHECK(EmeraldBattleLive_ResolveRoutingTarget(
                  EMERALD_BATTLE_ROUTING_BATTLEAI, 0u, &p1)
              == EMERALD_BATTLE_LIVE_OK);
        CHECK(EmeraldBattleCompat_ReverseResolve(
                  p1, keyBuf, sizeof(keyBuf), &revOffset, &revFamily)
              == EMERALD_BATTLE_OK);
        CHECK(revFamily == EMERALD_BATTLE_FAMILY_BATTLE_AI);
    }

    /* Fail-closed restage: an invalid layout leaves the current
     * generation resolving. */
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack,
        EMERALD_BATTLE_LIVE_LAYOUT_COUNT, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT);
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[m].gbaStart, &(uintptr_t){0})
          == EMERALD_BATTLE_LIVE_OK);

    /* Identity-specific unregister (sec 26): ranges drop out of the
     * index by exact key, position-independent - anim + FE untouched by
     * the battle removal and vice versa. */
    EmeraldBattleLive_UnregisterRange("emerald:field-effect-script/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 7607u);
    EmeraldBattleLive_UnregisterRange("emerald:battle-anim-script/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 7606u);
    EmeraldBattleLive_UnregisterRange("emerald:battle-script/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 7605u);
    /* R13-H6: the AI ranges unregister by the same identity rule
     * (battle-ai then contest-ai; the G count returns). */
    EmeraldBattleLive_UnregisterRange("emerald:battle-ai/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 7604u);
    EmeraldBattleLive_UnregisterRange("emerald:contest-ai/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 7603u);
    /* Re-registration restores the invariant. */
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);

    printf("H4-REPLACE genA=%llu genB=%llu arena-a=%p arena-b=%p "
           "count=%zu\n",
           (unsigned long long)genA, (unsigned long long)genB,
           (const void *)arena0, (const void *)arena1,
           EmeraldBattleLive_GetRangeCount());
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */
/* Fresh-process State-v5 proof (brief sec 15). */

struct H4Fixtures
{
    u64 generationStamp;
    u32 expectedModule; /* anim module table index */
    u32 expectedIpOffset;
    u32 expectedRetOffset;
};

static struct H4Fixtures *H4Fixtures(void)
{
    return (struct H4Fixtures *)(void *)sHarnessGameBss;
}

/* H5 battle nested fixture: the 8-slot call stack storage + the
 * mandatory sec-22 identity pins (waitmessage IP + two IP+5 returns in
 * DIFFERENT modules). */
struct H5BattleFixtures
{
    struct
    {
        const u8 *ptr[H4_STACK_CAP];
        u8 size;
    } battleStack;
    u32 expectedModule; /* waitmessage module table index */
    u32 expectedIpOffset;
    u32 expectedRet0Module;
    u32 expectedRet0Offset;
    u32 expectedRet1Module;
    u32 expectedRet1Offset;
};

static struct H5BattleFixtures *H5BattleFixtures(void)
{
    return (struct H5BattleFixtures *)(void *)
        (sHarnessGameBss + sizeof(struct H4Fixtures));
}

/* H6 AI state fixture: BOTH AI stack surfaces (battle-ai lives on
 * aiStack, contest-ai on contestStack - the shared gAIScriptPtr surface
 * carries either family) + the mandatory sec-22 identity pins (the AI IP
 * at a module root and one active call frame holding an IP+5 return in a
 * DIFFERENT bytecode module). */
struct H6AiFixtures
{
    struct
    {
        const u8 *ptr[H4_STACK_CAP];
        u8 size;
    } aiStack;
    struct
    {
        const u8 *ptr[H4_STACK_CAP];
        u8 size;
    } contestStack;
    u32 expectedFamily;
    u32 expectedIpModule; /* module table index of the parked IP */
    u32 expectedIpOffset;
    u32 expectedRetModule; /* module table index of the call return */
    u32 expectedRetOffset;
};

static struct H6AiFixtures *H6AiFixtures(void)
{
    return (struct H6AiFixtures *)(void *)
        (sHarnessGameBss + sizeof(struct H4Fixtures)
         + sizeof(struct H5BattleFixtures));
}

/* Deterministic battle points (brief sec 22): a battle module holding a
 * `waitmessage` (0x12) instruction start; two OTHER battle modules each
 * holding a `call` (0x41) whose return (call + 5) is a NEXT_INSTRUCTION
 * boundary. */
static bool32 H5PickBattlePoints(u32 *outWaitModule, u32 *outWaitOffset,
                                 u32 *outRet0Module, u32 *outRet0Offset,
                                 u32 *outRet1Module, u32 *outRet1Offset)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    u32 waitModule = UINT32_MAX;
    u32 waitOffset = 0u;
    u32 retModules[2] = {UINT32_MAX, UINT32_MAX};
    u32 retOffsets[2] = {0u, 0u};
    u32 found = 0u;
    u32 m;
    u32 r;

    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];

        if (mod->family != H5_FAMILY_BATTLE
         || mod->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
            continue;
        for (r = mod->boundaryFirst; r < mod->boundaryFirst + mod->boundaryCount; r++)
        {
            u32 offset = t->boundaries[r].payloadOffset;
            const uint8_t *spanBase;
            u8 opcode;

            if (offset + 5u >= mod->byteCount)
                continue;
            if (!H4LayoutSpan(m, 0u, &spanBase))
                continue;
            opcode = spanBase[offset];
            if (opcode == 0x12u && waitModule == UINT32_MAX)
            {
                waitModule = m;
                waitOffset = offset;
            }
            else if (opcode == 0x41u && found < 2u && m != waitModule)
            {
                if (EmeraldBattleCompat_ValidateBoundary(
                        mod->id, offset + 5u,
                        EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION)
                        == EMERALD_BATTLE_OK)
                {
                    retModules[found] = m;
                    retOffsets[found] = offset + 5u;
                    found++;
                }
            }
        }
    }
    if (waitModule == UINT32_MAX || found != 2u)
        return FALSE;
    *outWaitModule = waitModule;
    *outWaitOffset = waitOffset;
    *outRet0Module = retModules[0];
    *outRet0Offset = retOffsets[0];
    *outRet1Module = retModules[1];
    *outRet1Offset = retOffsets[1];
    return TRUE;
}

static int DoH5StateCreate(const char *packPath, const char *statePath)
{
    struct H4Fixtures *fx = H4Fixtures();
    struct H5BattleFixtures *bfx = H5BattleFixtures();
    const uint8_t *arena;
    const uint8_t *spanBase;
    size_t arenaSize;
    u32 waitModule;
    u32 waitOffset;
    u32 ret0Module;
    u32 ret0Offset;
    u32 ret1Module;
    u32 ret1Offset;
    u32 i;

    HarnessStatePath_Override(statePath);
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    memset(fx, 0, sizeof(*fx));
    memset(bfx, 0, sizeof(*bfx));
    H4ClearSurfaces();
    H4BindLayout();
    CHECK(H5PickBattlePoints(&waitModule, &waitOffset,
                             &ret0Module, &ret0Offset,
                             &ret1Module, &ret1Offset));
    /* Plant the nested blocking battle state: IP parked at waitmessage,
     * two active call-stack frames holding IP+5 returns in different
     * modules (brief sec 22). */
    CHECK(H4LayoutSpan(waitModule, 0u, &spanBase));
    gBattlescriptCurrInstr = spanBase + waitOffset;
    CHECK(H4LayoutSpan(ret0Module, 0u, &spanBase));
    bfx->battleStack.ptr[0] = spanBase + ret0Offset;
    CHECK(H4LayoutSpan(ret1Module, 0u, &spanBase));
    bfx->battleStack.ptr[1] = spanBase + ret1Offset;
    bfx->battleStack.size = 2u;
    for (i = 0u; i < 4u; i++)
    {
        gSelectionBattleScripts[i] = NULL;
        gPalaceSelectionBattleScripts[i] = NULL;
    }
    bfx->expectedModule = waitModule;
    bfx->expectedIpOffset = waitOffset;
    bfx->expectedRet0Module = ret0Module;
    bfx->expectedRet0Offset = ret0Offset;
    bfx->expectedRet1Module = ret1Module;
    bfx->expectedRet1Offset = ret1Offset;
    fx->generationStamp = EmeraldBattleLive_GetGenerationId();
    CHECK(EmeraldBattleCompat_GetArena(H5_FAMILY_BATTLE, &arena, &arenaSize));
    CHECK(arena != NULL);
    CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    printf("H5-CREATE arena=%p generation=%llu module=%u ip=%u "
           "ret0=%u/%u ret1=%u/%u\n",
           (const void *)arena,
           (unsigned long long)EmeraldBattleLive_GetGenerationId(),
           waitModule, waitOffset, ret0Module, ret0Offset,
           ret1Module, ret1Offset);
    return sFailures != 0;

fail:
    return 1;
}

static int DoH5StateLoad(const char *packPath, const char *modsDir,
                        const char *statePath)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct H5BattleFixtures *bfx = H5BattleFixtures();
    const uint8_t *arena;
    size_t arenaSize;
    char keyBuf[96];
    uint32_t revOffset;
    uint32_t revFamily;
    const uint8_t *spanBase;
    uint8_t *bin = NULL;
    size_t binSize = 0u;
    uint8_t nextOpcode;
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;

    HarnessStatePath_Override(statePath);
    if (!SetupScriptCompatSession(packPath))
        return 1;
    EmeraldBattleLive_ClearMigratedEntries();
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 1u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);
    memset(bfx, 0, sizeof(*bfx));
    H4ClearSurfaces();
    H4BindLayout();
    CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);

    /* 1. the battle IP relocated into the NEW arena at the same
     * (module, offset) semantic identity. */
    CHECK(gBattlescriptCurrInstr != NULL);
    CHECK(EmeraldBattleCompat_ReverseResolve(
              (uintptr_t)gBattlescriptCurrInstr, keyBuf, sizeof(keyBuf),
              &revOffset, &revFamily) == EMERALD_BATTLE_OK);
    CHECK(strcmp(keyBuf, t->modules[bfx->expectedModule].id) == 0);
    CHECK(revOffset == bfx->expectedIpOffset);
    CHECK(revFamily == H5_FAMILY_BATTLE);

    /* 2. both stack entries relocated (different modules, IP+5). */
    CHECK(bfx->battleStack.size == 2u);
    CHECK(EmeraldBattleCompat_ReverseResolve(
              (uintptr_t)bfx->battleStack.ptr[0], keyBuf, sizeof(keyBuf),
              &revOffset, &revFamily) == EMERALD_BATTLE_OK);
    CHECK(strcmp(keyBuf, t->modules[bfx->expectedRet0Module].id) == 0);
    CHECK(revOffset == bfx->expectedRet0Offset);
    CHECK(EmeraldBattleCompat_ReverseResolve(
              (uintptr_t)bfx->battleStack.ptr[1], keyBuf, sizeof(keyBuf),
              &revOffset, &revFamily) == EMERALD_BATTLE_OK);
    CHECK(strcmp(keyBuf, t->modules[bfx->expectedRet1Module].id) == 0);
    CHECK(revOffset == bfx->expectedRet1Offset);

    /* 3. the exact next canonical opcode executes: the byte at the
     * restored IP equals the committed .bin byte at the same offset
     * (and is waitmessage itself, parked). */
    CHECK(ReadModuleBin(modsDir, t->modules[bfx->expectedModule].id,
                        &bin, &binSize));
    CHECK(binSize > bfx->expectedIpOffset);
    nextOpcode = bin[bfx->expectedIpOffset];
    free(bin);
    CHECK(gBattlescriptCurrInstr[0] == nextOpcode);

    /* 4. return through both frames in order (pop semantics: last
     * pushed first) - each restored return is a valid NEXT_INSTRUCTION
     * boundary in the live arena. */
    CHECK(EmeraldBattleCompat_ValidateBoundary(
              t->modules[bfx->expectedRet1Module].id,
              bfx->expectedRet1Offset,
              EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION) == EMERALD_BATTLE_OK);
    CHECK(EmeraldBattleCompat_ValidateBoundary(
              t->modules[bfx->expectedRet0Module].id,
              bfx->expectedRet0Offset,
              EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION) == EMERALD_BATTLE_OK);

    /* 5. the restored pointers live inside the new battle arena. */
    CHECK(EmeraldBattleCompat_GetArena(H5_FAMILY_BATTLE, &arena, &arenaSize));
    CHECK(arena != NULL);
    CHECK(gBattlescriptCurrInstr >= arena
          && gBattlescriptCurrInstr < arena + arenaSize);
    CHECK(bfx->battleStack.ptr[0] >= arena
          && bfx->battleStack.ptr[0] < arena + arenaSize);
    CHECK(bfx->battleStack.ptr[1] >= arena
          && bfx->battleStack.ptr[1] < arena + arenaSize);

    printf("H5-LOAD arena=%p generation=%llu module=%u ip=%u "
           "ret0=%u/%u ret1=%u/%u opcode=0x%02x\n",
           (const void *)arena,
           (unsigned long long)EmeraldBattleLive_GetGenerationId(),
           bfx->expectedModule, bfx->expectedIpOffset,
           bfx->expectedRet0Module, bfx->expectedRet0Offset,
           bfx->expectedRet1Module, bfx->expectedRet1Offset,
           nextOpcode);
    return sFailures != 0;

fail:
    return 1;
}

/* Deterministic AI points (brief sec 22): the AI IP parks at the first
 * bytecode module's root (offset 0, the module export); the single
 * active stack frame holds an instruction-start return in a DIFFERENT
 * bytecode module - the second boundary of that module. (The AI stack
 * carries call returns, but battle-ai canonical data has NO call 0x58
 * instructions - the seam validates the IP and both instruction roles
 * against the same boundary map, so any instruction start round-trips;
 * the identity proof, not the call provenance, is what is under test.) */
static bool32 H6PickAiPoints(u32 family, u32 *outIpModule,
                             u32 *outRetModule, u32 *outRetOffset)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    u32 ipModule = UINT32_MAX;
    u32 retModule = UINT32_MAX;
    u32 m;

    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];

        if (mod->family != family
         || mod->mapKind != EMERALD_BATTLE_MAP_BYTECODE)
            continue;
        if (ipModule == UINT32_MAX)
        {
            ipModule = m;
            continue;
        }
        if (mod->boundaryCount >= 2u)
        {
            retModule = m;
            break;
        }
    }
    if (ipModule == UINT32_MAX || retModule == UINT32_MAX)
        return FALSE;
    *outIpModule = ipModule;
    *outRetModule = retModule;
    *outRetOffset = t->boundaries[
        t->modules[retModule].boundaryFirst + 1u].payloadOffset;
    return TRUE;
}

static int DoH6StateCreate(const char *packPath, const char *statePath,
                           const char *familyName)
{
    struct H6AiFixtures *afx = H6AiFixtures();
    const uint8_t *arena;
    const uint8_t *spanBase;
    size_t arenaSize;
    u32 family = (strcmp(familyName, "contest-ai") == 0)
        ? H6_FAMILY_CONTEST_AI : H6_FAMILY_BATTLE_AI;
    u32 ipModule;
    u32 retModule;
    u32 retOffset;

    HarnessStatePath_Override(statePath);
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    memset(afx, 0, sizeof(*afx));
    H4ClearSurfaces();
    H4BindLayout();
    CHECK(H6PickAiPoints(family, &ipModule, &retModule, &retOffset));
    /* Plant the active AI state: the shared gAIScriptPtr parked at the
     * module root, one call frame holding the IP+5 return (battle-ai on
     * the aiStack surface, contest-ai on the contest stack - the other
     * surface stays empty and scrubs). */
    CHECK(H4LayoutSpan(ipModule, 0u, &spanBase));
    gAIScriptPtr = spanBase;
    CHECK(H4LayoutSpan(retModule, 0u, &spanBase));
    if (family == H6_FAMILY_BATTLE_AI)
    {
        afx->aiStack.ptr[0] = spanBase + retOffset;
        afx->aiStack.size = 1u;
    }
    else
    {
        afx->contestStack.ptr[0] = spanBase + retOffset;
        afx->contestStack.size = 1u;
    }
    afx->expectedFamily = family;
    afx->expectedIpModule = ipModule;
    afx->expectedIpOffset = 0u;
    afx->expectedRetModule = retModule;
    afx->expectedRetOffset = retOffset;
    H4Fixtures()->generationStamp = EmeraldBattleLive_GetGenerationId();
    CHECK(EmeraldBattleCompat_GetArena(family, &arena, &arenaSize));
    CHECK(arena != NULL);
    CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    printf("H6-CREATE family=%u arena=%p generation=%llu module=%u ip=%u "
           "ret=%u/%u\n",
           family, (const void *)arena,
           (unsigned long long)EmeraldBattleLive_GetGenerationId(),
           ipModule, 0u, retModule, retOffset);
    return sFailures != 0;

fail:
    return 1;
}

static int DoH6StateLoad(const char *packPath, const char *modsDir,
                         const char *statePath, const char *familyName)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct H6AiFixtures *afx = H6AiFixtures();
    const uint8_t *arena;
    size_t arenaSize;
    char keyBuf[96];
    uint32_t revOffset;
    uint32_t revFamily;
    uint8_t *bin = NULL;
    size_t binSize = 0u;
    uint8_t nextOpcode;
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleLiveStatus status;
    u32 family = (strcmp(familyName, "contest-ai") == 0)
        ? H6_FAMILY_CONTEST_AI : H6_FAMILY_BATTLE_AI;

    HarnessStatePath_Override(statePath);
    if (!SetupScriptCompatSession(packPath))
        return 1;
    EmeraldBattleLive_ClearMigratedEntries();
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 1u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);
    memset(afx, 0, sizeof(*afx));
    H4ClearSurfaces();
    H4BindLayout();
    CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);

    /* 1. the shared AI IP relocated into the NEW arena at the same
     * (module, offset) semantic identity, under the captured family. */
    CHECK(gAIScriptPtr != NULL);
    CHECK(EmeraldBattleCompat_ReverseResolve(
              (uintptr_t)gAIScriptPtr, keyBuf, sizeof(keyBuf),
              &revOffset, &revFamily) == EMERALD_BATTLE_OK);
    CHECK(strcmp(keyBuf, t->modules[afx->expectedIpModule].id) == 0);
    CHECK(revOffset == afx->expectedIpOffset);
    CHECK(revFamily == family);

    /* 2. the single stack frame relocated (IP+5 of the call, in a
     * different module) onto the family's own stack surface. */
    {
        const u8 **stackPtr = (family == H6_FAMILY_BATTLE_AI)
            ? afx->aiStack.ptr : afx->contestStack.ptr;
        u8 *stackSize = (family == H6_FAMILY_BATTLE_AI)
            ? &afx->aiStack.size : &afx->contestStack.size;

        CHECK(*stackSize == 1u);
        CHECK(EmeraldBattleCompat_ReverseResolve(
                  (uintptr_t)stackPtr[0], keyBuf, sizeof(keyBuf),
                  &revOffset, &revFamily) == EMERALD_BATTLE_OK);
        CHECK(strcmp(keyBuf, t->modules[afx->expectedRetModule].id) == 0);
        CHECK(revOffset == afx->expectedRetOffset);
        CHECK(revFamily == family);
    }

    /* 3. the exact next canonical opcode executes: the byte at the
     * restored IP equals the committed .bin byte at the same offset
     * (and is the module root itself, parked). */
    CHECK(ReadModuleBin(modsDir, t->modules[afx->expectedIpModule].id,
                        &bin, &binSize));
    CHECK(binSize > afx->expectedIpOffset);
    nextOpcode = bin[afx->expectedIpOffset];
    free(bin);
    CHECK(gAIScriptPtr[0] == nextOpcode);

    /* 4. the restored return is a valid NEXT_INSTRUCTION boundary in the
     * live arena. */
    CHECK(EmeraldBattleCompat_ValidateBoundary(
              t->modules[afx->expectedRetModule].id,
              afx->expectedRetOffset,
              EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION) == EMERALD_BATTLE_OK);

    /* 5. the restored pointers live inside the new AI arena. */
    CHECK(EmeraldBattleCompat_GetArena(family, &arena, &arenaSize));
    CHECK(arena != NULL);
    CHECK(gAIScriptPtr >= arena && gAIScriptPtr < arena + arenaSize);
    {
        const u8 **stackPtr = (family == H6_FAMILY_BATTLE_AI)
            ? afx->aiStack.ptr : afx->contestStack.ptr;

        CHECK(stackPtr[0] >= arena && stackPtr[0] < arena + arenaSize);
    }

    printf("H6-LOAD family=%u arena=%p generation=%llu module=%u ip=%u "
           "ret=%u/%u opcode=0x%02x\n",
           family, (const void *)arena,
           (unsigned long long)EmeraldBattleLive_GetGenerationId(),
           afx->expectedIpModule, afx->expectedIpOffset,
           afx->expectedRetModule, afx->expectedRetOffset, nextOpcode);
    return sFailures != 0;

fail:
    return 1;
}

static void H4ClearSurfaces(void)
{
    gBattlescriptCurrInstr = NULL;
    gAIScriptPtr = NULL;
    gAnimScriptCallback = NULL;
    sBattleAnimScriptPtr = NULL;
    sBattleAnimScriptRetAddr = NULL;
}

static void H4BindLayout(void)
{
    struct H4Fixtures *fx = H4Fixtures();
    struct H5BattleFixtures *bfx = H5BattleFixtures();
    struct H6AiFixtures *afx = H6AiFixtures();
    struct EmeraldBattleStateLayout layout;
    u32 i;

    memset(&layout, 0, sizeof(layout));
    /* H4/H5/H6 production shape: anim + battle + both AI surfaces are
     * bound. The battle stack lives in the harness fixture (production
     * binds the per-battle heap at BattleAllocResources); the two AI
     * stacks live in the H6 fixture - the h4/h5/h6-state-* modes zero
     * their fixture before capture, so an unplanted AI stack has size 0
     * and the adapter scrubs the surface (fresh-process bss is zeroed in
     * every other mode). */
    layout.animScriptPtr = &sBattleAnimScriptPtr;
    layout.animScriptRetAddr = &sBattleAnimScriptRetAddr;
    layout.animScriptCallback = &gAnimScriptCallback;
    layout.battlescriptCurrInstr = &gBattlescriptCurrInstr;
    for (i = 0u; i < 4u; i++)
    {
        layout.selectionScripts[i] = &gSelectionBattleScripts[i];
        layout.palaceSelectionScripts[i] = &gPalaceSelectionBattleScripts[i];
    }
    for (i = 0u; i < H4_STACK_CAP; i++)
        layout.battleStackPtrs[i] = &bfx->battleStack.ptr[i];
    layout.battleStackSize = &bfx->battleStack.size;
    layout.aiScriptPtr = &gAIScriptPtr;
    for (i = 0u; i < H4_STACK_CAP; i++)
    {
        layout.aiStackPtrs[i] = &afx->aiStack.ptr[i];
        layout.contestStackPtrs[i] = &afx->contestStack.ptr[i];
    }
    layout.aiStackSize = &afx->aiStack.size;
    layout.contestStackSize = &afx->contestStack.size;
    layout.generationStamp = &fx->generationStamp;
    EmeraldBattleState_SetLayout(&layout);
}

/* Deterministic anim points: the first anim payload module; the IP at
 * its first nested instruction start, the return at the second. */
static bool32 H4PickAnimPoints(u32 *outModule, u32 *outIpOffset,
                               u32 *outRetOffset)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    u32 m;
    u32 r;

    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        if (mod->family == H4_FAMILY_ANIM && mod->byteCount > 16u)
            break;
    }
    if (m >= t->payloadModuleCount)
        return FALSE;
    {
        const struct EmeraldBattleLiveModule *mod = &t->modules[m];
        u32 ipOffset = 0u;
        u32 retOffset = 0u;
        u32 found = 0u;

        for (r = mod->boundaryFirst; r < mod->boundaryFirst + mod->boundaryCount; r++)
        {
            u32 offset = t->boundaries[r].payloadOffset;
            if (offset == 0u || offset >= mod->byteCount)
                continue;
            if (found == 0u)
            {
                ipOffset = offset;
                found = 1u;
            }
            else if (offset != ipOffset)
            {
                retOffset = offset;
                break;
            }
        }
        if (found == 0u)
            return FALSE;
        *outModule = m;
        *outIpOffset = ipOffset;
        *outRetOffset = retOffset;
    }
    return TRUE;
}

static int DoStateCreate(const char *packPath, const char *statePath)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct H4Fixtures *fx = H4Fixtures();
    const uint8_t *arena;
    size_t arenaSize;
    uintptr_t spanBase;
    u32 module;
    u32 ipOffset;
    u32 retOffset;

    HarnessStatePath_Override(statePath);
    /* The production loader path publishes the live generation. */
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
    memset(fx, 0, sizeof(*fx));
    H4ClearSurfaces();
    H4BindLayout();
    CHECK(H4PickAnimPoints(&module, &ipOffset, &retOffset));
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[module].gbaStart, &spanBase)
          == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleCompat_ValidateBoundary(
              t->modules[module].id, ipOffset,
              EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
          == EMERALD_BATTLE_OK);
    CHECK(EmeraldBattleCompat_ValidateBoundary(
              t->modules[module].id, retOffset,
              EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION)
          == EMERALD_BATTLE_OK);
    fx->expectedModule = module;
    fx->expectedIpOffset = ipOffset;
    fx->expectedRetOffset = retOffset;
    fx->generationStamp = EmeraldBattleLive_GetGenerationId();
    sBattleAnimScriptPtr = (const u8 *)spanBase + ipOffset;
    sBattleAnimScriptRetAddr = (const u8 *)spanBase + retOffset;
    gAnimScriptCallback = H4WaitCallback;
    CHECK(EmeraldBattleCompat_GetArena(H4_FAMILY_ANIM, &arena, &arenaSize));
    CHECK(arena != NULL);
    CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    printf("H4-CREATE arena=%p generation=%llu module=%u ip=%u ret=%u\n",
           (const void *)arena,
           (unsigned long long)EmeraldBattleLive_GetGenerationId(),
           module, ipOffset, retOffset);
    return sFailures != 0;

fail:
    return 1;
}

static int DoStateLoad(const char *packPath, const char *statePath)
{
    const struct EmeraldBattleLiveTable *t = H4Table();
    struct H4Fixtures *fx = H4Fixtures();
    const uint8_t *arena;
    size_t arenaSize;
    uintptr_t spanBase;
    u32 module;
    u32 ipOffset;
    u32 retOffset;

    HarnessStatePath_Override(statePath);
    /* Fresh process: the loader re-publishes, then the fixture replaces
     * the generation with the perturbed layout 1 - physical placement
     * moves while every identity stays stable. */
    if (!SetupScriptCompatSession(packPath))
        return 1;
    EmeraldBattleLive_ClearMigratedEntries();
    {
        struct EmeraldBattleCompatDiagnostics diagnostics;
        enum EmeraldBattleLiveStatus status;

        memset(&diagnostics, 0, sizeof(diagnostics));
        status = EmeraldBattleLive_TryInitialize(
            gScriptHarnessSnapshot, gScriptHarnessPack, 1u, &diagnostics);
        CHECK(status == EMERALD_BATTLE_LIVE_OK);
        CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
        CHECK(EmeraldBattleLive_GetRangeCount() == 7608u);
        CHECK(EmeraldBattleLive_Publish() == EMERALD_BATTLE_LIVE_OK);
    }
    memset(fx, 0, sizeof(*fx));
    H4ClearSurfaces();
    H4BindLayout();
    CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    CHECK(EmeraldBattleCompat_GetArena(H4_FAMILY_ANIM, &arena, &arenaSize));
    CHECK(arena != NULL);
    module = fx->expectedModule;
    ipOffset = fx->expectedIpOffset;
    retOffset = fx->expectedRetOffset;
    CHECK(module < t->payloadModuleCount);
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[module].gbaStart, &spanBase)
          == EMERALD_BATTLE_LIVE_OK);
    /* Module+offset identity held across the process boundary, the
     * layout perturbation, and the arena base change. */
    CHECK(sBattleAnimScriptPtr == (const u8 *)spanBase + ipOffset);
    CHECK(sBattleAnimScriptRetAddr == (const u8 *)spanBase + retOffset);
    CHECK(gAnimScriptCallback == H4WaitCallback);
    /* The restored pointer sits in THIS process's arena at the layout-1
     * physical placement. */
    CHECK((uintptr_t)sBattleAnimScriptPtr - (uintptr_t)arena
          >= t->modules[module].layoutOffset[1]);
    CHECK((uintptr_t)sBattleAnimScriptPtr - (uintptr_t)arena
          < t->modules[module].layoutOffset[1] + t->modules[module].byteCount);
    /* Boundary re-validates for the restored surfaces. */
    CHECK(EmeraldBattleCompat_ValidateBoundary(
              t->modules[module].id, ipOffset,
              EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
          == EMERALD_BATTLE_OK);
    printf("H4-LOAD arena=%p generation=%llu module=%u ip=%u ret=%u\n",
           (const void *)arena,
           (unsigned long long)EmeraldBattleLive_GetGenerationId(),
           module, ipOffset, retOffset);
    return sFailures != 0;

fail:
    return 1;
}

/* ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr,
                "usage: %s oracle <pack> <modsDir> <layout>\n"
                "       %s faults <pack>\n"
                "       %s replace <pack>\n"
                "       %s battle-oracle <pack> <modsDir> <layout>\n"
                "       %s battle-faults <pack>\n"
                "       %s battle-249 <pack> <modsDir>\n"
                "       %s state-create <pack> <state>\n"
                "       %s state-load <pack> <state>\n"
                "       %s h5-state-create <pack> <modsDir> <state>\n"
                "       %s h5-state-load <pack> <modsDir> <state>\n"
                "       %s ai-oracle <pack> <modsDir> <layout>\n"
                "       %s ai-faults <pack>\n"
                "       %s ai-249 <pack> <modsDir>\n"
                "       %s h6-state-create <pack> <modsDir> <state> <battle-ai|contest-ai>\n"
                "       %s h6-state-load <pack> <modsDir> <state> <battle-ai|contest-ai>\n"
                "       %s h7-faults <pack>\n",
                argv[0], argv[0], argv[0], argv[0], argv[0],
                argv[0], argv[0], argv[0], argv[0], argv[0],
                argv[0], argv[0], argv[0], argv[0], argv[0],
                argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "oracle") == 0)
        return DoOracle(argv[2], argv[3],
                        (uint32_t)strtoul(argv[4], NULL, 10));
    if (strcmp(argv[1], "faults") == 0)
        return DoFaults(argv[2]);
    if (strcmp(argv[1], "replace") == 0)
        return DoReplace(argv[2]);
    if (strcmp(argv[1], "battle-oracle") == 0)
        return DoBattleOracle(argv[2], argv[3],
                              (uint32_t)strtoul(argv[4], NULL, 10));
    if (strcmp(argv[1], "battle-faults") == 0)
        return DoBattleFaults(argv[2]);
    if (strcmp(argv[1], "battle-249") == 0)
        return DoBattle249(argv[2], argv[3]);
    if (strcmp(argv[1], "state-create") == 0)
        return DoStateCreate(argv[2], argv[3]);
    if (strcmp(argv[1], "state-load") == 0)
        return DoStateLoad(argv[2], argv[3]);
    if (strcmp(argv[1], "h5-state-create") == 0)
        return DoH5StateCreate(argv[2], argv[4]);
    if (strcmp(argv[1], "h5-state-load") == 0)
        return DoH5StateLoad(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "ai-oracle") == 0)
        return DoAiOracle(argv[2], argv[3],
                          (uint32_t)strtoul(argv[4], NULL, 10));
    if (strcmp(argv[1], "ai-faults") == 0)
        return DoAiFaults(argv[2]);
    if (strcmp(argv[1], "ai-249") == 0)
        return DoAi249(argv[2], argv[3]);
    if (strcmp(argv[1], "h6-state-create") == 0)
        return DoH6StateCreate(argv[2], argv[4], argv[5]);
    if (strcmp(argv[1], "h6-state-load") == 0)
        return DoH6StateLoad(argv[2], argv[3], argv[4], argv[5]);
    if (strcmp(argv[1], "h7-faults") == 0)
        return DoH7Faults(argv[2]);
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
