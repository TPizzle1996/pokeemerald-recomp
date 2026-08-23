/* R13-H4 live battle-anim + field-effect cutover tests
 * (tests/run_emerald_battle_live.sh).
 *
 * Drives the PRODUCTION live seam (emerald_battle_live.c +
 * battle_live_table.generated.c) instead of the H3 shadow seam, over
 * the real production pack:
 *
 *   oracle <pack> <modsDir> <layout>  differential resolution oracle:
 *       726 entry words x 2 layouts, 4,401 relocs x 2 layouts,
 *       byte-exact arena vs the committed .bin artifacts, boundary and
 *       family re-verification through the six-bridge adapter API;
 *   faults <pack>                    the brief sec 24 fail-closed
 *       matrix (pre-publish refusal, corrupt-word gate, refuse-only
 *       bindings, wrong family, non-root launch, scalar words never
 *       converted, compiled pointers never accepted, BUSY quiescence,
 *       invalid layout, teardown refusal);
 *   replace <pack>                   brief sec 25/26: publish generation
 *       A, replace with generation B (perturbed layout), ranges hold at
 *       the 6,379 invariant, identity-based unregister + re-register;
 *   state-create <pack> <state>      fresh-process proof (sec 15) side
 *       A: live anim IP/return planted, real walker saves v5 state;
 *   state-load  <pack> <state>       side B: fresh process, perturbed
 *       layout, restore -> anim pointers relocate into the new arena by
 *       module+offset identity.
 *
 * The weak BattleAnimCompat_IsAnimActive probe is defined strongly here
 * so the quiescence gate (sec 25) is exercised deterministically.
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
extern EWRAM_DATA void (*gAnimScriptCallback)(void);
extern EWRAM_DATA const u8 *sBattleAnimScriptPtr;
extern EWRAM_DATA const u8 *sBattleAnimScriptRetAddr;
extern unsigned char sHarnessGameBss[0x10000];
void HarnessStatePath_Override(const char *path);

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
     * registration (6,379 ranges); roll it back so the oracle drives
     * the full transactional sequence from scratch (sec 4/25). */
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);
    EmeraldBattleLive_ClearMigratedEntries();
    CHECK(EmeraldBattleLive_GetRangeCount() == 6377u);
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
    CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);
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
    /* 723 payload - 6 routing tables (4 anim + 1 FE + 1 anim quiet-BGM). */
    CHECK(entryWords == EMERALD_BATTLE_LIVE_PAYLOAD_MODULE_COUNT - 6u);

    /* Routing roots: launchable only via their reloc'd words; the root
     * itself refuses (bytecode boundary gate, not family). */
    {
        uint32_t routing = 0u;
        uintptr_t pointer;
        for (i = 0u; i < t->payloadModuleCount; i++)
        {
            const struct EmeraldBattleLiveModule *m = &t->modules[i];
            if (m->mapKind == EMERALD_BATTLE_MAP_BYTECODE)
                continue;
            routing++;
            CHECK(EmeraldBattleLive_ResolveLaunchTarget(
                      m->family, m->gbaStart, &pointer)
                  == EMERALD_BATTLE_LIVE_ERR_BOUNDARY_INVALID);
        }
        CHECK(routing == 6u);
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
                 * family identity. */
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
                if (binding->address != 0u)
                {
                    CHECK(status == EMERALD_BATTLE_LIVE_OK);
                    CHECK(pointer == binding->address);
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
    CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);
    EmeraldBattleLive_ClearMigratedEntries();

    /* 1. invalid layout refuses transactionally. */
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack,
        EMERALD_BATTLE_LIVE_LAYOUT_COUNT, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT);
    CHECK(EmeraldBattleLive_GetGenerationId() == 0u);
    passes++;

    /* 2. stage layout 0 + publish (6,379 = 6,377 + the 2 live ranges). */
    status = H4StageLive(0u);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);
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
    CHECK(EmeraldBattleLive_GetRangeCount() == 6377u);
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
     * family and the H4 live step (6,377 G ranges + 2 live). */
    if (!SetupScriptCompatSession(packPath))
        return 1;
    CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);
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
    CHECK(EmeraldBattleLive_GetRangeCount() == 6377u);
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, 1u, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);
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

    /* Fail-closed restage: an invalid layout leaves the current
     * generation resolving. */
    status = EmeraldBattleLive_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack,
        EMERALD_BATTLE_LIVE_LAYOUT_COUNT, &diagnostics);
    CHECK(status == EMERALD_BATTLE_LIVE_ERR_INVALID_ARGUMENT);
    CHECK(EmeraldBattleLive_ResolveLaunchTarget(
              H4_FAMILY_ANIM, t->modules[m].gbaStart, &(uintptr_t){0})
          == EMERALD_BATTLE_LIVE_OK);

    /* Identity-specific unregister (sec 26): the FE range alone drops
     * out of the index by exact key, position-independent. */
    EmeraldBattleLive_UnregisterRange("emerald:field-effect-script/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 6378u);
    EmeraldBattleLive_UnregisterRange("emerald:battle-anim-script/@arena");
    CHECK(EmeraldBattleLive_GetRangeCount() == 6377u);
    /* Re-registration restores the invariant. */
    CHECK(EmeraldBattleLive_RegisterRanges() == EMERALD_BATTLE_LIVE_OK);
    CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);

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
    struct EmeraldBattleStateLayout layout;

    memset(&layout, 0, sizeof(layout));
    /* H4 production shape: only the anim surfaces are live; battle/AI/
     * contest slots stay NULL so those families fall through to the
     * generic image-relative v5 path (family-live gate). */
    layout.animScriptPtr = &sBattleAnimScriptPtr;
    layout.animScriptRetAddr = &sBattleAnimScriptRetAddr;
    layout.animScriptCallback = &gAnimScriptCallback;
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
    CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);
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
        CHECK(EmeraldBattleLive_GetRangeCount() == 6379u);
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
                "       %s state-create <pack> <state>\n"
                "       %s state-load <pack> <state>\n",
                argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "oracle") == 0)
        return DoOracle(argv[2], argv[3],
                        (uint32_t)strtoul(argv[4], NULL, 10));
    if (strcmp(argv[1], "faults") == 0)
        return DoFaults(argv[2]);
    if (strcmp(argv[1], "replace") == 0)
        return DoReplace(argv[2]);
    if (strcmp(argv[1], "state-create") == 0)
        return DoStateCreate(argv[2], argv[3]);
    if (strcmp(argv[1], "state-load") == 0)
        return DoStateLoad(argv[2], argv[3]);
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}
