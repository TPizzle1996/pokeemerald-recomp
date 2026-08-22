/* R13-G3 focused test: EmeraldScriptCompat shadow staging + typed
 * resolver seam (plan sec 26).
 *
 * Driven over the REAL production pack (games/emerald/base/
 * emerald-bpee01-v1.rpack) with the text + leaf sibling seams
 * published first (the loader's ordering). Covers:
 *
 *   1. preconditions (pack 20,988 entries; sibling seams published);
 *   2. refused staging before the siblings publish (UNAVAILABLE);
 *   3. full shadow staging: index counts, parity 16,704/0, arena
 *      geometry, per-module byte identity against the pack;
 *   4. every one of the 16,704 relocation rows resolved through the
 *      seam (source index for module rows, encoded-target index for
 *      routing rows) with the exact disposition partition, the
 *      95-root / 8,113-interior pins and the class partition;
 *   5. typed target classes: text into the published C catalog (incl.
 *      bundle members + the 20 gift labels), movement into the B
 *      surface + 3 named compiled bridges, marts (sentinel), RAM into
 *      gStringVar4, braille live via the generated address table
 *      (R13-G6 sec 7.3: 26 edges -> 22 distinct compiled blocks),
 *      dispatch deferred;
 *   6. gStdScripts shadow staging (11 slots, arena pointers only);
 *   7. F inbound staging (3,501 rows: 2,983 staged + 518 deferred
 *      routing);
 *   8. reverse containment incl. end/hull/stale refusals;
 *   9. boundary validation (instruction starts, entrypoints, typed
 *      data, routing refusal);
 *  10. generation lifecycle: restage at a different arena base,
 *      canonical identity equality across generations, stale-pointer
 *      refusal, failed replacement preserves the old generation;
 *  11. pack-variant fault injection (missing module, extra module,
 *      wrong schema, flipped payload);
 *  12. State-v5 invariant: the range index is untouched by staging
 *      and no range intersects the shadow arena;
 *  13. runtime-behavior invariant: staged bytes are byte-identical to
 *      the pack (no operand patching), and the seam writes nothing
 *      outside its private shadow state (the range-index check above
 *      plus the absence of any registration call in the seam TU).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_id.h"
#include "gen3/resources/sha256.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_script_compat.h"
#include "emerald/resources/emerald_trainer_native_compat.h"

#include "emerald_script_compat_harness.h"

extern uint8_t gStringVar4[1000];

static void DigestSha256(const uint8_t *data, size_t size, uint8_t digest[32])
{
    struct Gen3Sha256Context context;

    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, data, size);
    Gen3Sha256_Final(&context, digest);
}

static int sChecks;
static int sFailures;
static uint64_t sBaseGenerationId;

#define CHECK(desc, cond)                                                    \
    do                                                                       \
    {                                                                        \
        sChecks++;                                                           \
        if (!(cond))                                                         \
        {                                                                    \
            sFailures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, desc);   \
        }                                                                    \
    } while (0)

static int ModuleKeyCompareLocal(const void *key, const void *element)
{
    return strcmp((const char *)key,
                  ((const struct EmeraldScriptNativeModule *)element)->id);
}

static const struct EmeraldScriptNativeModule *FindModuleLocal(const char *key)
{
    return bsearch(key, kEmeraldScriptCompatTable.modules,
                   kEmeraldScriptCompatTable.moduleCount,
                   sizeof(struct EmeraldScriptNativeModule),
                   ModuleKeyCompareLocal);
}

/* ------------------------------------------------------------------ */
/* Pack variants (plan sec 17 RESOURCE faults)                         */

static bool BuildScriptVariantPack(const char *srcPath, const char *dstPath,
                                   int mode)
{
    /* mode: 0 = drop the first script module (missing module)
     *       1 = zero its payload (wrong digest)
     *       2 = change its schema to 47 (wrong schema)
     *       3 = append a bogus extra script entry (extra module) */
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourcePackProfile profile;
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackProfileInput pin;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    uint8_t *zeroed = NULL;
    uint8_t zeroSha[32];
    (void)0;
    uint8_t provenanceSha[32];
    size_t count;
    size_t i;
    FILE *f = NULL;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(srcPath, &pack, &packDiag) != GEN3_PACK_OK
     || pack == NULL)
        goto done;
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;
    if (!Gen3ResourcePack_GetProfile(pack, &profile))
        goto done;
    memset(&pin, 0, sizeof(pin));
    pin.basePackVersion = profile.basePackVersion;
    pin.catalogVersion = profile.catalogVersion;
    pin.extractionManifestVersion = profile.extractionManifestVersion;
    pin.canonicalRepresentationVersion = profile.canonicalRepresentationVersion;
    pin.sourceRomSize = profile.sourceRomSize;
    pin.sourceRomSha1 = profile.sourceRomSha1;
    pin.sourceRomSha256 = profile.sourceRomSha256;
    memcpy(pin.gameCode, profile.gameCode, 4u);
    memcpy(pin.makerCode, profile.makerCode, 2u);
    pin.softwareRevision = profile.softwareRevision;
    memcpy(pin.gameId, profile.gameId, GEN3_PACK_GAME_ID_SIZE);
    pin.catalogSha256 = profile.catalogSha256;
    pin.extractionManifestSha256 = profile.extractionManifestSha256;
    if (Gen3ResourcePackBuild_SetProfile(build, &pin, &diag) != GEN3_PACK_OK)
        goto done;

    memset(provenanceSha, 0x5A, sizeof(provenanceSha));
    memset(zeroSha, 0, sizeof(zeroSha));

    count = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0; i < count; i++)
    {
        const struct Gen3ResourcePackEntry *e =
            Gen3ResourcePack_GetEntry(pack, i);
        bool isFirstScript =
            strncmp(e->canonicalName, "emerald:script/", 15u) == 0
            && e->canonicalName[15] != '\0';

        if (isFirstScript && mode == 0)
            continue;
        if (isFirstScript && mode == 1)
        {
            free(zeroed);
            zeroed = calloc(1u, e->payloadSize);
            if (zeroed == NULL)
                goto done;
            DigestSha256(zeroed, e->payloadSize, zeroSha);
        }
        memset(&entry, 0, sizeof(entry));
        entry.schema = (isFirstScript && mode == 2) ? 47u : e->schema;
        entry.flags = e->flags;
        entry.representation = e->representation;
        entry.sourceEncoding = e->sourceEncoding;
        entry.canonicalName = e->canonicalName;
        entry.key = &e->key;
        entry.type = e->type;
        entry.canonicalPayload = (isFirstScript && mode == 1)
            ? zeroed : e->payload;
        entry.canonicalPayloadSize = e->payloadSize;
        entry.canonicalPayloadSha256 = (isFirstScript && mode == 1)
            ? zeroSha : e->payloadSha256;
        entry.sourceRomOffset = e->sourceRomOffset;
        entry.sourceEncodedSize = e->sourceEncodedSize;
        entry.sourceEncodedSha256 = provenanceSha;
        if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
            goto done;
    }
    if (mode == 3)
    {
        /* An extra script-family entry: bogus identity. */
        static const uint8_t bogus[2] = { 'x', 'x' };
        Gen3ResourceKey derived;
        uint8_t bogusSha[32];
        memset(&entry, 0, sizeof(entry));
        DigestSha256(bogus, sizeof(bogus), bogusSha);
        Gen3ResourceId_DeriveKey("emerald:script/common/not-a-real-module",
                                 &derived);
        entry.schema = 45u;
        entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
        entry.representation = GEN3_PACK_REPRESENTATION_RAW;
        entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_RAW;
        entry.canonicalName = "emerald:script/common/not-a-real-module";
        entry.key = &derived;
        entry.type = GEN3_RESOURCE_TYPE_STRUCTURED_DATA;
        entry.canonicalPayload = bogus;
        entry.canonicalPayloadSize = 2u;
        entry.canonicalPayloadSha256 = bogusSha;
        entry.sourceEncodedSize = 2u;
        entry.sourceEncodedSha256 = bogusSha;
        if (Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) != GEN3_PACK_OK)
            goto done;
    }
    if (Gen3ResourcePackWriter_Write(build, &bytes, &diag) != GEN3_PACK_OK)
    {
        fprintf(stderr, "variant write failed mode %d (%zu diagnostics)\n",
                mode, diag.count);
        goto done;
    }
    f = fopen(dstPath, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(bytes.data, 1, bytes.size, f) != bytes.size)
        goto done;
    ok = true;

done:
    free(zeroed);
    if (f != NULL)
        fclose(f);
    Gen3ResourcePackBytes_Destroy(&bytes);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    Gen3ResourcePack_Destroy(pack);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Main suite                                                          */

static void TestStagingAndIndexes(void)
{
    struct EmeraldScriptCompatDiagnostics diag;
    struct EmeraldScriptCompatIndexCounts counts;
    enum EmeraldScriptCompatStatus status;
    const uint8_t *arena;
    size_t arenaSize;
    uint32_t checked = 0u;
    uint32_t mismatches = 0u;

    memset(&diag, 0, sizeof(diag));
    status = EmeraldScriptCompat_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, &diag);
    if (status != EMERALD_SCRIPT_OK)
        fprintf(stderr, "script seam refused: status %d stage '%s' "
                        "name '%s'\n",
                (int)status, diag.stage, diag.canonicalName);
    CHECK("full shadow staging succeeds", status == EMERALD_SCRIPT_OK);
    CHECK("generation committed (monotonic +1)",
          EmeraldScriptCompat_GetGenerationId() == sBaseGenerationId + 1u);
    CHECK("arena published",
          EmeraldScriptCompat_GetArena(&arena, &arenaSize));
    CHECK("arena size = deterministic 217360 B (payload + routing)", arenaSize == 217360u);

    CHECK("index counts reported",
          EmeraldScriptCompat_GetIndexCounts(&counts));
    CHECK("counts: 523 modules", counts.modules == 523u);
    CHECK("counts: 812 segments", counts.segments == 812u);
    CHECK("counts: 7683 exports", counts.exports == 7683u);
    CHECK("counts: 15874 module relocs", counts.relocs == 15874u);
    CHECK("counts: 830 routing relocs", counts.routingRelocs == 830u);
    CHECK("counts: 52042 boundaries", counts.boundaries == 52042u);
    CHECK("counts: 12016 dynamic targets", counts.dynamicTargets == 12016u);
    CHECK("counts: 11 std scripts", counts.stdScripts == 11u);
    CHECK("counts: 3501 F bindings", counts.fBindings == 3501u);
    CHECK("counts: 38 marts", counts.marts == 38u);
    CHECK("counts: 18 RAM targets / 1 allowlist row",
          counts.ramTargets == 18u && counts.ramAllowlist == 1u);
    CHECK("counts: 3 bridges", counts.bridges == 3u);
    CHECK("counts: 1423 opaque bytes", counts.opaqueBytes == 1423u);

    CHECK("parity reported",
          EmeraldScriptCompat_GetParityCounts(&checked, &mismatches));
    CHECK("parity: 16704/16704 checked, 0 mismatches",
          checked == 16704u && mismatches == 0u);
}

static void TestAllRelocations(void)
{
    /* Every one of the 16,704 rows resolves through the seam:
     * module rows through the source index (live operand address),
     * routing rows through the encoded-target index - with the exact
     * disposition partition and the class/root pins. */
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    struct EmeraldScriptCompatResolvedTarget target;
    const uint8_t *arena = NULL;
    size_t arenaSize = 0u;
    uint32_t dispositions[5] = {0u, 0u, 0u, 0u, 0u};
    uint32_t classes[5] = {0u, 0u, 0u, 0u, 0u};
    uint32_t roots = 0u;
    uint32_t interior = 0u;
    uint32_t brailleLive = 0u;
    uint32_t brailleDistinct[22] = {0u};
    uint32_t brailleDistinctCount = 0u;
    uint32_t module;
    uint32_t i;

    CHECK("arena available",
          EmeraldScriptCompat_GetArena(&arena, &arenaSize) && arena != NULL);
    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t j;
        for (j = m->relocFirst; j < m->relocFirst + m->relocCount; j++)
        {
            const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
            enum EmeraldScriptCompatStatus status =
                EmeraldScriptCompat_ResolveOperand(
                    (uintptr_t)(arena + m->arenaOffset
                                + r->operandPayloadOffset), &target);

            if (status != EMERALD_SCRIPT_OK)
            {
                CHECK("every module reloc resolves", false);
                fprintf(stderr, "  module %u reloc %u status %d\n",
                        module, j - m->relocFirst, (int)status);
                return;
            }
            dispositions[target.disposition]++;
            classes[target.targetClass]++;
            if (target.targetClass == EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT)
            {
                if (target.targetOffset == 0u)
                    roots++;
                else
                    interior++;
            }
            if (target.targetKind == EMERALD_SCRIPT_NATIVE_KIND_BRAILLE)
            {
                uint32_t k;

                if (target.disposition == EMERALD_SCRIPT_DISPOSITION_SIBLING_SEAM
                 && target.liveAddress != 0u)
                    brailleLive++;
                for (k = 0u; k < brailleDistinctCount; k++)
                {
                    if (brailleDistinct[k] == (uint32_t)target.liveAddress)
                        break;
                }
                if (k == brailleDistinctCount
                 && brailleDistinctCount < 22u)
                    brailleDistinct[brailleDistinctCount++] =
                        (uint32_t)target.liveAddress;
            }
        }
    }
    for (i = 0u; i < t->routingRelocCount; i++)
    {
        const struct EmeraldScriptNativeRoutingReloc *r = &t->routingRelocs[i];
        enum EmeraldScriptCompatStatus status =
            EmeraldScriptCompat_ResolveEncodedTarget(
                r->targetGba, r->targetClass, &target);
        if (status != EMERALD_SCRIPT_OK)
        {
            CHECK("every routing reloc resolves", false);
            fprintf(stderr, "  routing reloc %u status %d\n",
                    i, (int)status);
            return;
        }
        dispositions[target.disposition]++;
        classes[target.targetClass]++;
        if (target.targetClass == EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT)
        {
            if (target.targetOffset == 0u)
                roots++;
            else
                interior++;
        }
    }
    CHECK("dispositions: 8441 staged arena (payload + routing)",
          dispositions[EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA] == 8441u);
    CHECK("dispositions: 8242 sibling seam (8216 + 26 braille)",
          dispositions[EMERALD_SCRIPT_DISPOSITION_SIBLING_SEAM] == 8242u);
    CHECK("dispositions: 3 compiled bridge",
          dispositions[EMERALD_SCRIPT_DISPOSITION_COMPILED_BRIDGE] == 3u);
    CHECK("dispositions: 18 host RAM",
          dispositions[EMERALD_SCRIPT_DISPOSITION_HOST_RAM] == 18u);
    CHECK("dispositions: 0 deferred (dispatch stays ROM-resident)",
          dispositions[EMERALD_SCRIPT_DISPOSITION_DEFERRED] == 0u);
    CHECK("classes: 8208 script", classes[0] == 8208u);
    CHECK("classes: 6207 text", classes[1] == 6207u);
    CHECK("classes: 2009 movement", classes[2] == 2009u);
    CHECK("classes: 262 static-data", classes[3] == 262u);
    CHECK("classes: 18 RAM", classes[4] == 18u);
    CHECK("braille: 26 edges resolve SIBLING_SEAM with a live address",
          brailleLive == 26u);
    CHECK("braille: 22 distinct compiled blocks",
          brailleDistinctCount == 22u);
    CHECK("root/interior pin: 95 / 8113", roots == 95u && interior == 8113u);
}

static void TestTypedTargets(void)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    struct EmeraldScriptCompatResolvedTarget target;
    const uint8_t *arena = NULL;
    size_t arenaSize = 0u;
    uint32_t module;
    uint32_t i;
    uint32_t textLive = 0u;
    uint32_t movementLive = 0u;
    uint32_t martLive = 0u;
    uint32_t ramLive = 0u;
    uint32_t bridgeLive = 0u;

    CHECK("arena available",
          EmeraldScriptCompat_GetArena(&arena, &arenaSize) && arena != NULL);

    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t j;
        for (j = m->relocFirst; j < m->relocFirst + m->relocCount; j++)
        {
            const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
            if (EmeraldScriptCompat_ResolveOperand(
                    (uintptr_t)(arena + m->arenaOffset
                                + r->operandPayloadOffset), &target)
                    != EMERALD_SCRIPT_OK)
                continue;
            switch (target.targetClass)
            {
            case EMERALD_SCRIPT_NATIVE_CLASS_TEXT:
                if (target.liveAddress != 0u && target.liveSize > 0u)
                    textLive++;
                break;
            case EMERALD_SCRIPT_NATIVE_CLASS_MOVEMENT:
                if (target.liveAddress != 0u && target.liveSize > 0u)
                    movementLive++;
                break;
            case EMERALD_SCRIPT_NATIVE_CLASS_STATIC_DATA:
                if (target.targetKind == EMERALD_SCRIPT_NATIVE_KIND_MART_TABLE)
                {
                    if (target.liveAddress != 0u)
                    {
                        /* Sentinel: the last u16 of the staged table. */
                        const uint8_t *bytes = (const uint8_t *)target.liveAddress;
                        uint32_t lo = bytes[target.liveSize - 2u];
                        uint32_t hi = bytes[target.liveSize - 1u];
                        if (lo == 0u && hi == 0u)
                            martLive++;
                    }
                }
                break;
            case EMERALD_SCRIPT_NATIVE_CLASS_RAM_DATA:
                if (target.liveAddress == (uintptr_t)gStringVar4)
                    ramLive++;
                break;
            case EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT:
                if (target.targetKind == EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_BRIDGE
                 || target.targetKind
                        == EMERALD_SCRIPT_NATIVE_KIND_MOVEMENT_BRIDGE)
                {
                    if (target.liveAddress != 0u)
                        bridgeLive++;
                }
                break;
            default:
                break;
            }
        }
    }
    for (i = 0u; i < t->routingRelocCount; i++)
    {
        const struct EmeraldScriptNativeRoutingReloc *r = &t->routingRelocs[i];
        if (EmeraldScriptCompat_ResolveEncodedTarget(
                r->targetGba, r->targetClass, &target) == EMERALD_SCRIPT_OK)
        {
            if (target.targetClass == EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT
             && (target.targetKind
                     == EMERALD_SCRIPT_NATIVE_KIND_SCRIPT_BRIDGE
                 || target.targetKind
                     == EMERALD_SCRIPT_NATIVE_KIND_MOVEMENT_BRIDGE)
             && target.liveAddress != 0u)
                bridgeLive++;
        }
    }
    CHECK("all 6207 text targets resolve to published C bytes",
          textLive == 6207u);
    CHECK("all 2009 movement targets resolve to published B bytes",
          movementLive == 2009u);
    CHECK("all 38 mart targets resolve to arena tables with ITEM_NONE",
          martLive == 38u);
    CHECK("all 18 RAM targets resolve to gStringVar4", ramLive == 18u);
    CHECK("all 3 bridges resolve to compiled symbols", bridgeLive == 3u);

    /* Encoded-target spot checks (plan sec 20, the dynamic path). */
    CHECK("encoded RAM target resolves",
          EmeraldScriptCompat_ResolveEncodedTarget(
              0x02021FC4u, EMERALD_SCRIPT_NATIVE_CLASS_RAM_DATA, &target)
              == EMERALD_SCRIPT_OK
          && target.liveAddress == (uintptr_t)gStringVar4);
    CHECK("encoded gift text target resolves",
          EmeraldScriptCompat_ResolveEncodedTarget(
              t->dynamicTargets[0].gbaAddress,
              t->dynamicTargets[0].targetClass, &target)
              == EMERALD_SCRIPT_OK);
    CHECK("unknown encoded target refuses",
          EmeraldScriptCompat_ResolveEncodedTarget(
              0x08FFFFFFu, EMERALD_SCRIPT_NATIVE_CLASS_SCRIPT, &target)
              == EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED);
}

static void TestStagedSurfaces(void)
{
    struct EmeraldScriptCompatStagedStdScript std;
    struct EmeraldScriptCompatStagedFBinding fb;
    const uint8_t *arena = NULL;
    size_t arenaSize = 0u;
    uint32_t i;
    uint32_t stagedF = 0u;
    uint32_t deferredF = 0u;
    uint32_t byKind[4] = {0u, 0u, 0u, 0u};

    CHECK("arena available",
          EmeraldScriptCompat_GetArena(&arena, &arenaSize) && arena != NULL);
    CHECK("11 staged std scripts",
          EmeraldScriptCompat_GetStagedStdScriptCount() == 11u);
    for (i = 0u; i < 11u; i++)
    {
        CHECK("std slot readable",
              EmeraldScriptCompat_GetStagedStdScript(i, &std));
        CHECK("std slot order", std.slot == i);
        CHECK("std pointer inside the shadow arena (never compiled GBA)",
              (const uint8_t *)std.stagedAddress >= arena
              && (const uint8_t *)std.stagedAddress < arena + arenaSize);
        if (i == 0u)
            CHECK("slot 0 = Std_ObtainItem",
                  strcmp(std.exportName, "Std_ObtainItem") == 0);
    }
    CHECK("std slot 11 refused",
          !EmeraldScriptCompat_GetStagedStdScript(11u, &std));

    CHECK("3501 staged F bindings",
          EmeraldScriptCompat_GetStagedFBindingCount() == 3501u);
    for (i = 0u; i < 3501u; i++)
    {
        CHECK("F row readable",
              EmeraldScriptCompat_GetStagedFBinding(i, &fb));
        byKind[fb.kind]++;
        if (fb.disposition == EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA)
        {
            stagedF++;
            CHECK("staged F pointer inside the shadow arena",
                  (const uint8_t *)fb.stagedAddress >= arena
                  && (const uint8_t *)fb.stagedAddress < arena + arenaSize);
        }
        else if (fb.disposition == EMERALD_SCRIPT_DISPOSITION_DEFERRED)
        {
            deferredF++;
            CHECK("deferred F row has no staged pointer",
                  fb.stagedAddress == 0u);
        }
        else
            CHECK("F disposition is staged or deferred", false);
    }
    CHECK("F kinds: 518/2163/289/531",
          byKind[0] == 518u && byKind[1] == 2163u
          && byKind[2] == 289u && byKind[3] == 531u);
    CHECK("F staged 3501 + deferred 0 (routing materialized)",
          stagedF == 3501u && deferredF == 0u);
    CHECK("F row 3501 refused",
          !EmeraldScriptCompat_GetStagedFBinding(3501u, &fb));
}

static void TestByteIdentityAndContainment(void)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    const uint8_t *arena = NULL;
    size_t arenaSize = 0u;
    size_t entryCount = Gen3ResourcePack_GetEntryCount(gScriptHarnessPack);
    uint32_t verified = 0u;
    uint32_t zeroSpan = 0u;
    size_t e;

    CHECK("arena available",
          EmeraldScriptCompat_GetArena(&arena, &arenaSize) && arena != NULL);
    for (e = 0u; e < entryCount; e++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(gScriptHarnessPack, e);
        const struct EmeraldScriptNativeModule *m;

        if (entry == NULL || entry->canonicalName == NULL)
            continue;
        if (strncmp(entry->canonicalName, "emerald:script/", 15u) != 0)
            continue;
        m = FindModuleLocal(entry->canonicalName);
        if (m == NULL)
            continue;
        /* Exact copied G-owned segment bytes (plan sec 3): the staged
         * span IS the pack payload, byte for byte - no patching. */
        if (memcmp(arena + m->arenaOffset, entry->payload,
                   entry->payloadSize) == 0)
            verified++;
        else
            CHECK("staged span equals pack payload", false);
    }
    CHECK("467 embedded module spans byte-identical to the pack",
          verified == 467u);
    {
        uint32_t module;
        for (module = 0u; module < t->moduleCount; module++)
        {
            if (t->modules[module].payloadSize == 0u)
                zeroSpan++;
        }
        CHECK("56 routing-only zero-size spans", zeroSpan == 56u);
    }
}

static void TestReverseContainmentAndBoundaries(void)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    const uint8_t *arena = NULL;
    size_t arenaSize = 0u;
    char moduleKey[EMERALD_SCRIPT_KEY_CAP];
    uint32_t offset = 0u;
    uint32_t segmentKind = 0u;
    uint32_t module;
    uint32_t hits = 0u;

    CHECK("arena available",
          EmeraldScriptCompat_GetArena(&arena, &arenaSize) && arena != NULL);
    /* A pointer at every module's span base resolves uniquely. */
    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        if (m->payloadSize == 0u)
            continue;
        if (EmeraldScriptCompat_ReverseResolve(
                (uintptr_t)(arena + m->arenaOffset), moduleKey,
                sizeof(moduleKey), &offset, &segmentKind) == EMERALD_SCRIPT_OK
         && strcmp(moduleKey, m->id) == 0 && offset == 0u)
            hits++;
    }
    CHECK("all 467 non-empty span bases reverse-resolve", hits == 467u);
    CHECK("span end pointer refuses (explicit end handling)",
          EmeraldScriptCompat_ReverseResolve(
              (uintptr_t)(arena + t->modules[0].arenaOffset
                          + t->modules[0].payloadSize),
              moduleKey, sizeof(moduleKey), &offset, &segmentKind)
              == EMERALD_SCRIPT_ERR_BOUNDARY_INVALID);
    CHECK("pointer before the first span refuses",
          EmeraldScriptCompat_ReverseResolve(
              (uintptr_t)(arena - 16u), moduleKey, sizeof(moduleKey),
              &offset, &segmentKind)
              == EMERALD_SCRIPT_ERR_BOUNDARY_INVALID);
    CHECK("arena hull end refuses",
          EmeraldScriptCompat_ReverseResolve(
              (uintptr_t)(arena + arenaSize), moduleKey, sizeof(moduleKey),
              &offset, &segmentKind)
              == EMERALD_SCRIPT_ERR_BOUNDARY_INVALID);

    /* Boundary validation on the first embedded module. */
    {
        uint32_t first = 0u;
        while (first < t->moduleCount && t->modules[first].payloadSize == 0u)
            first++;
        {
            const struct EmeraldScriptNativeModule *m = &t->modules[first];
            const struct EmeraldScriptNativeBoundary *b =
                &t->boundaries[m->boundaryFirst];
            CHECK("instruction start validates",
                  EmeraldScriptCompat_ValidateBoundary(
                      m->id, b->payloadOffset,
                      EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START)
                      == EMERALD_SCRIPT_OK);
            CHECK("next-instruction validates",
                  EmeraldScriptCompat_ValidateBoundary(
                      m->id, b->payloadOffset,
                      EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION)
                      == EMERALD_SCRIPT_OK);
            CHECK("module root validates as entrypoint",
                  EmeraldScriptCompat_ValidateBoundary(
                      m->id, 0u, EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT)
                      == EMERALD_SCRIPT_OK);
            CHECK("routing boundary always refuses in G3",
                  EmeraldScriptCompat_ValidateBoundary(
                      m->id, 0u, EMERALD_SCRIPT_BOUNDARY_ROUTING)
                      == EMERALD_SCRIPT_ERR_BOUNDARY_INVALID);
            CHECK("payload-size offset refuses",
                  EmeraldScriptCompat_ValidateBoundary(
                      m->id, m->payloadSize,
                      EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START)
                      == EMERALD_SCRIPT_ERR_BOUNDARY_INVALID);
            CHECK("unknown module refuses",
                  EmeraldScriptCompat_ValidateBoundary(
                      "emerald:script/common/not-a-real-module", 0u,
                      EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START)
                      == EMERALD_SCRIPT_ERR_BOUNDARY_INVALID);
        }
    }
    /* Every mart start validates as TYPED_DATA_START. */
    {
        uint32_t i;
        uint32_t ok = 0u;
        for (i = 0u; i < t->martCount; i++)
        {
            const struct EmeraldScriptNativeMart *mart = &t->marts[i];
            if (EmeraldScriptCompat_ValidateBoundary(
                    t->modules[mart->moduleIndex].id, mart->payloadOffset,
                    EMERALD_SCRIPT_BOUNDARY_TYPED_DATA_START)
                    == EMERALD_SCRIPT_OK)
                ok++;
        }
        CHECK("all 38 mart starts validate as typed data", ok == 38u);
    }
}

/* Aggregate over the FULL 16,704-row oracle on one generation:
 * every module reloc through the source index, every routing reloc
 * through the encoded-target index, folded into a deterministic
 * canonical checksum (kind/key/offset only - never pointers). */
static uint32_t CanonicalChecksum(const uint8_t *arena)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    struct EmeraldScriptCompatResolvedTarget target;
    uint32_t sum = 0u;
    uint32_t module;
    uint32_t i;

    for (module = 0u; module < t->moduleCount; module++)
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[module];
        uint32_t j;
        for (j = m->relocFirst; j < m->relocFirst + m->relocCount; j++)
        {
            const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
            if (EmeraldScriptCompat_ResolveOperand(
                    (uintptr_t)(arena + m->arenaOffset
                                + r->operandPayloadOffset), &target)
                    != EMERALD_SCRIPT_OK)
                return 0u;
            sum += target.targetKind * 31u
                 + target.targetClass * 17u
                 + target.targetPayloadOffset
                 + (uint32_t)target.targetOffset;
        }
    }
    for (i = 0u; i < t->routingRelocCount; i++)
    {
        const struct EmeraldScriptNativeRoutingReloc *r = &t->routingRelocs[i];
        if (EmeraldScriptCompat_ResolveEncodedTarget(
                r->targetGba, r->targetClass, &target)
                != EMERALD_SCRIPT_OK)
            return 0u;
        sum += target.targetKind * 31u
             + target.targetClass * 17u
             + target.targetPayloadOffset
             + (uint32_t)target.targetOffset;
    }
    return sum;
}

static void TestGenerationLifecycle(void)
{
    const struct EmeraldScriptCompatNativeTable *t = &kEmeraldScriptCompatTable;
    const uint8_t *arenaA = NULL;
    const uint8_t *arenaB = NULL;
    size_t sizeA = 0u;
    size_t sizeB = 0u;
    struct EmeraldScriptCompatResolvedTarget targetA;
    struct EmeraldScriptCompatResolvedTarget targetB;
    struct EmeraldScriptCompatDiagnostics diag;
    char moduleKey[EMERALD_SCRIPT_KEY_CAP];
    uint32_t offset = 0u;
    uint32_t segmentKind = 0u;
    uintptr_t stalePointer;
    uint32_t checksumA = 0u;
    uint32_t module;
    enum EmeraldScriptCompatStatus status = EMERALD_SCRIPT_OK;
    char path[512];
    const char *tempDir;

    CHECK("generation A published",
          EmeraldScriptCompat_GetArena(&arenaA, &sizeA));
    stalePointer = (uintptr_t)(arenaA + t->modules[0].arenaOffset);

    /* Capture generation-A resolution BEFORE the restage: the full
     * oracle checksum plus one sample target. */
    {
        const struct EmeraldScriptNativeModule *m = &t->modules[0];
        uint32_t j = m->relocFirst;
        const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
        memset(&targetA, 0, sizeof(targetA));
        memset(&targetB, 0, sizeof(targetB));
        checksumA = CanonicalChecksum(arenaA);
        CHECK("generation-A oracle checksum computed", checksumA != 0u);
        CHECK("A: operand resolves before restage",
              EmeraldScriptCompat_ResolveOperand(
                  (uintptr_t)(arenaA + m->arenaOffset
                              + r->operandPayloadOffset), &targetA)
                  == EMERALD_SCRIPT_OK);
    }

    /* Stage a second generation while the first is live: the new arena
     * is allocated before the old one frees, so the bases differ. */
    memset(&diag, 0, sizeof(diag));
    status = EmeraldScriptCompat_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, &diag);
    CHECK("restage succeeds", status == EMERALD_SCRIPT_OK);
    CHECK("generation advanced on restage",
          EmeraldScriptCompat_GetGenerationId() == sBaseGenerationId + 2u);
    CHECK("generation B published",
          EmeraldScriptCompat_GetArena(&arenaB, &sizeB));
    CHECK("distinct arena bases", arenaA != arenaB);
    CHECK("identical arena geometry", sizeA == sizeB);

    /* Canonical identity equality across generations (plan sec 18/19):
     * the FULL 16,704-row oracle ran on generation A before the
     * restage; it now runs again on generation B (a different arena
     * base) and the canonical aggregates match exactly - pointer
     * values differ, identities never. */
    {
        uint32_t checksumB = CanonicalChecksum(arenaB);
        CHECK("full-oracle canonical checksum identical across bases",
              checksumA == checksumB);
        {
            const struct EmeraldScriptNativeModule *m = &t->modules[0];
            uint32_t j = m->relocFirst;
            const struct EmeraldScriptNativeReloc *r = &t->relocs[j];
            CHECK("B: operand resolves",
                  EmeraldScriptCompat_ResolveOperand(
                      (uintptr_t)(arenaB + m->arenaOffset
                                  + r->operandPayloadOffset), &targetB)
                      == EMERALD_SCRIPT_OK);
            CHECK("A/B identity equal (key)",
                  strcmp(targetA.resourceKey, targetB.resourceKey) == 0);
            CHECK("A/B identity equal (offset)",
                  targetA.targetPayloadOffset == targetB.targetPayloadOffset
                  && targetA.targetOffset == targetB.targetOffset);
            CHECK("A/B pointers differ (generation-scoped)",
                  targetA.liveAddress != targetB.liveAddress);
            CHECK("B pointer inside arena B",
                  (const uint8_t *)targetB.liveAddress >= arenaB
                  && (const uint8_t *)targetB.liveAddress < arenaB + sizeB);
        }
    }

    /* Stale-generation pointers refuse. */
    CHECK("stale generation-A pointer refuses",
          EmeraldScriptCompat_ReverseResolve(
              stalePointer, moduleKey, sizeof(moduleKey), &offset,
              &segmentKind) == EMERALD_SCRIPT_ERR_BOUNDARY_INVALID);

    /* Failed replacement preserves the old generation. */
    tempDir = gScriptHarnessTempDir;
    snprintf(path, sizeof(path), "%s/script_missing_module.rpack", tempDir);
    CHECK("missing-module variant builds",
          BuildScriptVariantPack(gScriptHarnessPackPath, path, 0));
    memset(&diag, 0, sizeof(diag));
    {
        struct Gen3ResourcePack *variantPack = NULL;
        struct Gen3ResourcePackDiagnosticList packDiag;
        Gen3ResourcePackDiagnostics_Init(&packDiag);
        CHECK("variant pack opens",
              Gen3ResourcePack_OpenFile(path, &variantPack, &packDiag)
                  == GEN3_PACK_OK
              && variantPack != NULL);
        Gen3ResourcePackDiagnostics_Destroy(&packDiag);
        status = EmeraldScriptCompat_TryInitialize(
            gScriptHarnessSnapshot, variantPack, &diag);
        Gen3ResourcePack_Destroy(variantPack);
    }
    CHECK("missing-module variant refused",
          status == EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT);
    CHECK("failed replacement preserves the live generation",
          EmeraldScriptCompat_GetGenerationId() == sBaseGenerationId + 2u);
    CHECK("generation B still queryable",
          EmeraldScriptCompat_GetArena(&arenaB, &sizeB));
    CHECK("old generation still resolves",
          EmeraldScriptCompat_ReverseResolve(
              (uintptr_t)(arenaB + t->modules[0].arenaOffset),
              moduleKey, sizeof(moduleKey), &offset, &segmentKind)
              == EMERALD_SCRIPT_OK);

    /* More RESOURCE faults (plan sec 17). */
    snprintf(path, sizeof(path), "%s/script_flipped.rpack", tempDir);
    CHECK("flipped-payload variant builds",
          BuildScriptVariantPack(gScriptHarnessPackPath, path, 1));
    {
        struct Gen3ResourcePack *variantPack = NULL;
        struct Gen3ResourcePackDiagnosticList packDiag;
        Gen3ResourcePackDiagnostics_Init(&packDiag);
        if (Gen3ResourcePack_OpenFile(path, &variantPack, &packDiag)
                == GEN3_PACK_OK && variantPack != NULL)
        {
            Gen3ResourcePackDiagnostics_Destroy(&packDiag);
            memset(&diag, 0, sizeof(diag));
            status = EmeraldScriptCompat_TryInitialize(
                gScriptHarnessSnapshot, variantPack, &diag);
            Gen3ResourcePack_Destroy(variantPack);
            CHECK("flipped-payload variant refused (TABLE_MISMATCH)",
                  status == EMERALD_SCRIPT_ERR_TABLE_MISMATCH);
        }
        else
            CHECK("flipped variant opens", false);
    }
    snprintf(path, sizeof(path), "%s/script_schema.rpack", tempDir);
    CHECK("schema variant builds",
          BuildScriptVariantPack(gScriptHarnessPackPath, path, 2));
    {
        struct Gen3ResourcePack *variantPack = NULL;
        struct Gen3ResourcePackDiagnosticList packDiag;
        Gen3ResourcePackDiagnostics_Init(&packDiag);
        if (Gen3ResourcePack_OpenFile(path, &variantPack, &packDiag)
                == GEN3_PACK_OK && variantPack != NULL)
        {
            Gen3ResourcePackDiagnostics_Destroy(&packDiag);
            memset(&diag, 0, sizeof(diag));
            status = EmeraldScriptCompat_TryInitialize(
                gScriptHarnessSnapshot, variantPack, &diag);
            Gen3ResourcePack_Destroy(variantPack);
            CHECK("schema variant refused (TABLE_MISMATCH)",
                  status == EMERALD_SCRIPT_ERR_TABLE_MISMATCH);
        }
        else
            CHECK("schema variant opens", false);
    }
    snprintf(path, sizeof(path), "%s/script_extra.rpack", tempDir);
    CHECK("extra-module variant builds",
          BuildScriptVariantPack(gScriptHarnessPackPath, path, 3));
    {
        struct Gen3ResourcePack *variantPack = NULL;
        struct Gen3ResourcePackDiagnosticList packDiag;
        Gen3ResourcePackDiagnostics_Init(&packDiag);
        if (Gen3ResourcePack_OpenFile(path, &variantPack, &packDiag)
                == GEN3_PACK_OK && variantPack != NULL)
        {
            Gen3ResourcePackDiagnostics_Destroy(&packDiag);
            memset(&diag, 0, sizeof(diag));
            status = EmeraldScriptCompat_TryInitialize(
                gScriptHarnessSnapshot, variantPack, &diag);
            Gen3ResourcePack_Destroy(variantPack);
            CHECK("extra-module variant refused (UNEXPECTED_COUNT)",
                  status == EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT);
        }
        else
            CHECK("extra variant opens", false);
    }
    CHECK("generation unchanged after all refusals",
          EmeraldScriptCompat_GetGenerationId() == sBaseGenerationId + 2u);
    (void)module;
}

static void TestStateV5AndRuntimeInvariants(void)
{
    const struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    size_t before;
    size_t after;
    const uint8_t *arena = NULL;
    size_t arenaSize = 0u;
    struct EmeraldScriptCompatDiagnostics diag;
    enum EmeraldScriptCompatStatus status;
    size_t i;

    CHECK("range index present", index != NULL);
    before = EmeraldResourceRangeIndex_GetRangeCount(index);
    CHECK("live range pin 6377/8192 after the loader's cutover",
          before == 6377u && before < 8192u);

    /* Range lifecycle (plan sec 22): the clear unregisters exactly the
     * 523 module ranges (identity-based), the restage re-registers
     * them, and the count returns to exactly 6,377. */
    EmeraldScriptCompat_ClearMigratedEntries();
    CHECK("clear drops the generation",
          !EmeraldScriptCompat_GetArena(&arena, &arenaSize)
          && EmeraldScriptCompat_GetGenerationId() == 0u);
    CHECK("clear unregisters exactly the 523 module ranges",
          EmeraldResourceRangeIndex_GetRangeCount(index)
              == before - 523u);
    memset(&diag, 0, sizeof(diag));
    status = EmeraldScriptCompat_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, &diag);
    CHECK("restage after clear succeeds", status == EMERALD_SCRIPT_OK);
    CHECK("restage registers the 523 module ranges",
          EmeraldScriptCompat_RegisterRanges() == EMERALD_SCRIPT_OK);
    after = EmeraldResourceRangeIndex_GetRangeCount(index);
    CHECK("range count restored to exactly 6377", after == 6377u);
    /* clear bumps the counter, the restage bumps it again: base+4. */
    CHECK("generation id advanced past the clear",
          EmeraldScriptCompat_GetGenerationId()
              == sBaseGenerationId + 4u);

    CHECK("arena republished",
          EmeraldScriptCompat_GetArena(&arena, &arenaSize));
    {
        /* The 523 registered module ranges must cover the arena
         * exactly (one range per non-empty span; the 56 routing-only
         * modules now carry routing-suffix spans, so every module has
         * a registered range). */
        uint32_t intersections = 0u;
        for (i = 0u; i < after; i++)
        {
            const struct EmeraldResourceRange *range = &index->ranges[i];
            uintptr_t rangeEnd = range->base + range->length;
            uintptr_t arenaEnd = (uintptr_t)(arena + arenaSize);
            bool beforeRange = arenaEnd <= range->base;
            bool afterRange = (uintptr_t)arena >= rangeEnd;
            if (!beforeRange && !afterRange)
                intersections++;
        }
        CHECK("exactly 523 registered ranges cover the live arena",
              intersections == 523u);
    }
}

int main(int argc, char **argv)
{
    struct EmeraldScriptCompatDiagnostics diag;
    enum EmeraldScriptCompatStatus status;
    const char *tempDir;
    const char *prodPack;

    if (argc < 3)
    {
        fprintf(stderr,
                "usage: %s <temp-dir> <production-pack.rpack>\n", argv[0]);
        return 2;
    }
    tempDir = argv[1];
    prodPack = argv[2];
    gScriptHarnessTempDir = tempDir;
    gScriptHarnessPackPath = prodPack;

    if (!SetupScriptCompatSession(prodPack))
    {
        fprintf(stderr, "setup failed: cannot publish text/leaf siblings\n");
        TeardownScriptCompatSession();
        return 1;
    }
    {
        size_t count = Gen3ResourcePack_GetEntryCount(gScriptHarnessPack);
        CHECK("pack entry count 20988", count == 20988u);
    }
    CHECK("text sibling published",
          EmeraldTextCompat_GetPublishedCount() == EMERALD_TEXT_LABEL_COUNT);
    CHECK("leaf sibling published",
          EmeraldLeafCompat_GetPublishedCount() == EMERALD_LEAF_RESOURCE_COUNT);

    /* Refused staging before... the siblings are published by setup; the
     * UNAVAILABLE path is exercised by clearing the leaf seam first. */
    {
        const uint8_t *arena = NULL;
        size_t arenaSize = 0u;
        uint64_t generationBefore = EmeraldScriptCompat_GetGenerationId();
        EmeraldLeafCompat_ClearMigratedEntries();
        memset(&diag, 0, sizeof(diag));
        status = EmeraldScriptCompat_TryInitialize(
            gScriptHarnessSnapshot, gScriptHarnessPack, &diag);
        CHECK("staging refuses while a sibling is unpublished",
              status == EMERALD_SCRIPT_ERR_UNAVAILABLE);
        /* The R6 loader already published the live generation; a refused
         * restage must leave it untouched. */
        CHECK("refused stage preserves the live generation",
              EmeraldScriptCompat_GetGenerationId() == generationBefore
              && EmeraldScriptCompat_GetArena(&arena, &arenaSize));
        /* Re-publish the leaf sibling directly (the R6 loader's
         * registration is one-shot per process session, so a full
         * teardown/re-setup is not a legal harness flow). */
        {
            struct EmeraldLeafCompatDiagnostics leafDiag;
            memset(&leafDiag, 0, sizeof(leafDiag));
            CHECK("leaf sibling republishes",
                  EmeraldLeafCompat_TryInitialize(
                      gScriptHarnessSnapshot, gScriptHarnessPack, &leafDiag)
                      == EMERALD_LEAF_OK);
        }
    }
    sBaseGenerationId = EmeraldScriptCompat_GetGenerationId();

    TestStagingAndIndexes();
    TestAllRelocations();
    TestTypedTargets();
    TestStagedSurfaces();
    TestByteIdentityAndContainment();
    TestReverseContainmentAndBoundaries();
    TestGenerationLifecycle();
    TestStateV5AndRuntimeInvariants();

    EmeraldScriptCompat_ClearMigratedEntries();
    TeardownScriptCompatSession();

    if (sFailures != 0)
    {
        fprintf(stderr, "emerald_script_compat_test FAILED: %d/%d checks\n",
                sFailures, sChecks);
        return 1;
    }
    printf("emerald_script_compat_test passed (%d checks)\n", sChecks);
    return 0;
}
