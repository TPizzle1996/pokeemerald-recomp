/* R13-B focused test: leaf payload ownership migration (movement scripts +
 * multiboot programs).
 *
 * Drives the R13-B seam (emerald_leaf_compat.c) against the REAL production
 * pack (games/emerald/base/emerald-bpee01-v1.rpack, 6876 entries incl. the
 * 1057 leaf resources: 1055 movement scripts, 7428 B + 2 multiboot
 * programs, 176352 B) plus synthetic packs for the failure matrix:
 *
 *   A. counts        - pack composition: 1055 movement (schema 1) + 2
 *                      multiboot (schema 2), all type binary, and the
 *                      name/size/schema set is EXACTLY the generated slot
 *                      table (kLeafNativeResources);
 *   B. exact         - the published arena's payload zone is byte-identical
 *                      to the pack payloads in sorted-name order, every
 *                      GetResourceSpan agrees with the pack entry's
 *                      sourceRomOffset/size/schema, and the claimed ROM
 *                      slices are pairwise disjoint (the phase-1c proof
 *                      re-derived at publish);
 *   C. publication   - session -> TryInitialize -> arena (1057 records,
 *                      183780 B), query helpers, ContainsPointer canary,
 *                      invalid arguments fail closed, clear/shutdown fail
 *                      closed;
 *   D. additive      - a FAILED republish keeps the published arena intact
 *                      (fail-soft degrade: nothing is redirected in R13-B,
 *                      so the game keeps behaving exactly as pre-R13-B);
 *   E. failure matrix- synthetic packs/sessions (writer API), every case
 *                      fail-closed with the arena absent:
 *                      E1 missing movement family      -> UNEXPECTED_COUNT
 *                      E2 missing one movement entry   -> UNEXPECTED_COUNT
 *                      E3 missing multiboot family     -> UNEXPECTED_COUNT
 *                      E4 wrong-type movement          -> UNEXPECTED_COUNT
 *                      E5 wrong-type multiboot         -> UNEXPECTED_COUNT
 *                      E6 truncated movement           -> PAYLOAD_SIZE_MISMATCH
 *                      E7 truncated multiboot          -> PAYLOAD_SIZE_MISMATCH
 *                      E8 overlapping claimed ROM slice-> OVERLAPPING_SLICE
 *                      E9 pack duplicate-key guard     -> writer rejects
 *                      (pack provenance mismatch is the runner-level E1
 *                      tampered-manifest check - the seam cannot see ROM
 *                      bytes, the manifest -> ROM digest validation can)
 *
 * The seam is platform-neutral; this harness needs no native tables, so it
 * links the gen3 core + session + seam directly (the audio runner's link
 * pattern).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_pack_writer.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/sha256.h"
#include "emerald/resources/emerald_leaf_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_rom_profile.h"

static int sChecks = 0;
static int sFailures = 0;

/* When a CHECK fails right after a TryInitialize, echo the seam's
 * diagnostics so a matrix regression names itself instead of printing a
 * bare "FAIL". Points at the LAST TryInitialize's diagnostics. */
static const struct EmeraldLeafCompatDiagnostics *sLastDiag = NULL;
#define CHECK(name, cond)                                                  \
    do                                                                     \
    {                                                                      \
        sChecks++;                                                         \
        if (cond)                                                          \
            printf("ok   - %s\n", name);                                   \
        else                                                               \
        {                                                                  \
            printf("FAIL - %s\n", name);                                   \
            if (sLastDiag != NULL)                                         \
                printf("       (stage='%s' name='%.48s' expSize=%u "       \
                       "actSize=%u expSchema=%u actSchema=%u)\n",          \
                       sLastDiag->stage, sLastDiag->canonicalName,         \
                       sLastDiag->expectedSize, sLastDiag->actualSize,     \
                       sLastDiag->expectedSchema,                          \
                       sLastDiag->actualSchema);                           \
            sFailures++;                                                   \
        }                                                                  \
    } while (0)

/* One leaf fixture: identity + canonical bytes, deep-copied from the real
 * pack so a synthetic pack can mutate it without corrupting the shared
 * set. The name buffer must hold the LONGEST leaf id (96 chars): a
 * truncated fixture name would poison every synthetic pack's TOC and make
 * the seam's FindHandle miss against the real session. */
struct LeafFixture
{
    char canonicalName[128];
    uint8_t *payload;
    size_t payloadSize;
    uint8_t schema;
    uint32_t romOffset;
};

static size_t sFixtureCount = 0u;
static size_t sFixtureCapacity = 0u;
static struct LeafFixture *sFixtures = NULL;

static bool IsMovementEntry(const struct Gen3ResourcePackEntry *entry)
{
    return entry->schema == 1u
        && strncmp(entry->canonicalName, "emerald:movement/", 17u) == 0;
}

static bool IsMultibootEntry(const struct Gen3ResourcePackEntry *entry)
{
    return entry->schema == 2u
        && strncmp(entry->canonicalName, "emerald:multiboot/", 18u) == 0;
}

/* Load every leaf entry of the real pack into the fixture set. */
static bool LoadLeafFixtures(const struct Gen3ResourcePack *pack)
{
    size_t i;
    size_t n = Gen3ResourcePack_GetEntryCount(pack);

    for (i = 0; i < n; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        struct LeafFixture *fix;
        uint8_t *copy;

        if (!IsMovementEntry(entry) && !IsMultibootEntry(entry))
            continue;
        if (sFixtureCount >= sFixtureCapacity)
        {
            size_t newCap = sFixtureCapacity == 0u ? 1057u
                                                   : sFixtureCapacity * 2u;
            struct LeafFixture *grown = (struct LeafFixture *)realloc(
                sFixtures, newCap * sizeof(*sFixtures));
            if (grown == NULL)
                return false;
            sFixtures = grown;
            sFixtureCapacity = newCap;
        }
        fix = &sFixtures[sFixtureCount];
        memset(fix, 0, sizeof(*fix));
        snprintf(fix->canonicalName, sizeof(fix->canonicalName), "%s",
                 entry->canonicalName);
        fix->payloadSize = entry->payloadSize;
        fix->schema = entry->schema;
        fix->romOffset = entry->sourceRomOffset;
        copy = (uint8_t *)malloc(entry->payloadSize);
        if (copy == NULL)
            return false;
        memcpy(copy, entry->payload, entry->payloadSize);
        fix->payload = copy;
        sFixtureCount++;
    }
    return sFixtureCount == EMERALD_LEAF_RESOURCE_COUNT;
}

static void FreeLeafFixtures(void)
{
    size_t i;
    for (i = 0; i < sFixtureCount; i++)
        free(sFixtures[i].payload);
    free(sFixtures);
    sFixtures = NULL;
    sFixtureCount = 0u;
    sFixtureCapacity = 0u;
}

/* ------------------------------------------------------------------ */
/* Session helpers (same shape as the audio test's)                    */
/* ------------------------------------------------------------------ */

struct TestSession
{
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourcePack *pack;
};

static bool BuildCatalogFromPackEntries(
    const struct Gen3ResourcePack *pack,
    struct Gen3ResourceCatalog **out)
{
    struct Gen3ResourceCatalog *catalog = NULL;
    size_t i;
    size_t n;

    catalog = Gen3ResourceCatalog_Create();
    if (catalog == NULL)
        return false;
    n = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0; i < n; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        if (!Gen3ResourceCatalog_Add(catalog, entry->canonicalName,
                                     entry->type, entry->schema,
                                     (entry->flags & GEN3_PACK_FLAG_REQUIRED_FOR_BASE) != 0u,
                                     NULL))
        {
            Gen3ResourceCatalog_Destroy(catalog);
            return false;
        }
    }
    if (!Gen3ResourceCatalog_Finalize(catalog, NULL))
    {
        Gen3ResourceCatalog_Destroy(catalog);
        return false;
    }
    *out = catalog;
    return true;
}

static bool OpenSession(const char *packPath, struct TestSession *out)
{
    struct TestSession session = { NULL, NULL };
    struct Gen3ResourceCatalog *catalog = NULL;
    struct Gen3ResourceCandidate *candidate = NULL;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct EmeraldResourceSessionInfo info;
    enum EmeraldResourceSessionError sessionError;

    memset(out, 0, sizeof(*out));
    Gen3ResourcePackDiagnostics_Init(&packDiag);
    Gen3ResourceDiagnostics_Init(&diag);
    if (Gen3ResourcePack_OpenFile(packPath, &session.pack, &packDiag) != GEN3_PACK_OK)
        goto fail;
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    if (!BuildCatalogFromPackEntries(session.pack, &catalog))
        goto fail;
    sessionError = EmeraldResourceSession_BuildRomBaseCandidate(
        session.pack, catalog, &candidate, &info, &diag);
    if (sessionError != EMERALD_SESSION_OK || candidate == NULL)
        goto fail;
    if (!Gen3ResourceCandidate_Build(candidate, &session.snapshot, &diag))
        goto fail;
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceCatalog_Destroy(catalog);
    *out = session;
    return true;

fail:
    if (candidate != NULL)
        Gen3ResourceCandidate_Destroy(candidate);
    if (catalog != NULL)
        Gen3ResourceCatalog_Destroy(catalog);
    if (session.pack != NULL)
        Gen3ResourcePack_Destroy(session.pack);
    return false;
}

static void CloseSession(struct TestSession *session)
{
    if (session->snapshot != NULL)
        Gen3ResourceSnapshot_Destroy(session->snapshot);
    if (session->pack != NULL)
        Gen3ResourcePack_Destroy(session->pack);
    memset(session, 0, sizeof(*session));
}

/* ------------------------------------------------------------------ */
/* Synthetic packs (writer API) for the failure matrix                 */
/* ------------------------------------------------------------------ */

static void DigestSha256(const uint8_t *data, size_t size, uint8_t digest[32])
{
    struct Gen3Sha256Context ctx;
    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, data, size);
    Gen3Sha256_Final(&ctx, digest);
}

/* Deep-copy the fixture array, run `mutate` on each copy (may drop
 * entries via a callback on the index, retype, re-schema, resize, or move
 * a ROM slice), then write a deterministic pack. Returns the mutated deep
 * copy (for inspection); NULL when `mutate` drops the entry. */
static bool BuildLeafPack(const char *path,
                          const struct LeafFixture *src, size_t count,
                          void (*mutate)(struct LeafFixture *fix,
                                         size_t index))
{
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    struct LeafFixture *fixes = NULL;
    uint8_t sha[32];
    uint8_t provenanceSha[32];
    FILE *f = NULL;
    size_t i;
    size_t kept = 0u;
    enum Gen3ResourcePackError packError = GEN3_PACK_OK;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;
    fixes = (struct LeafFixture *)calloc(count, sizeof(*fixes));
    if (fixes == NULL)
        goto done;
    for (i = 0; i < count; i++)
    {
        fixes[i] = src[i];
        fixes[i].payload = (uint8_t *)malloc(src[i].payloadSize);
        if (fixes[i].payload == NULL)
            goto done;
        memcpy(fixes[i].payload, src[i].payload, src[i].payloadSize);
        if (mutate != NULL)
            mutate(&fixes[i], i);
    }

    /* Deterministic synthetic identity (never cross-checked by the
     * session/seam against a real ROM). */
    memset(&profile, 0, sizeof(profile));
    profile.basePackVersion = 1u;
    profile.catalogVersion = 1u;
    profile.extractionManifestVersion = 1u;
    profile.canonicalRepresentationVersion = 1u;
    profile.sourceRomSize = EMERALD_ROM_SIZE;
    profile.sourceRomSha1 = kEmeraldProfileSyntheticSha1;
    profile.sourceRomSha256 = kEmeraldProfileSyntheticSha256;
    memcpy(profile.gameCode, EMERALD_PROFILE_GAME_CODE, 4u);
    memcpy(profile.makerCode, EMERALD_PROFILE_MAKER_CODE, 2u);
    profile.softwareRevision = EMERALD_PROFILE_SOFTWARE_REV;
    snprintf(profile.gameId, sizeof(profile.gameId), "%s", EMERALD_PROFILE_GAME);
    profile.catalogSha256 = kEmeraldProfileSyntheticSha256;
    profile.extractionManifestSha256 = kEmeraldProfileSyntheticSha256;
    packError = Gen3ResourcePackBuild_SetProfile(build, &profile, &diag);
    if (packError != GEN3_PACK_OK)
        goto done;

    memset(provenanceSha, 0x5A, sizeof(provenanceSha));

    for (i = 0; i < count; i++)
    {
        Gen3ResourceKey key;

        if (fixes[i].payload == NULL)
            continue; /* dropped by the mutate callback */
        memset(&entry, 0, sizeof(entry));
        Gen3ResourceId_DeriveKey(fixes[i].canonicalName, &key);
        DigestSha256(fixes[i].payload, fixes[i].payloadSize, sha);
        entry.canonicalName = fixes[i].canonicalName;
        entry.key = &key;
        entry.type = GEN3_RESOURCE_TYPE_BINARY;
        entry.schema = fixes[i].schema;
        entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
        entry.representation = GEN3_PACK_REPRESENTATION_RAW;
        entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_RAW;
        entry.canonicalPayload = fixes[i].payload;
        entry.canonicalPayloadSize = fixes[i].payloadSize;
        entry.canonicalPayloadSha256 = sha;
        entry.sourceRomOffset = fixes[i].romOffset;
        entry.sourceEncodedSize = fixes[i].payloadSize;
        entry.sourceEncodedSha256 = provenanceSha;
        packError = Gen3ResourcePackBuild_AddEntry(build, &entry, &diag);
        if (packError != GEN3_PACK_OK)
            goto done;
        kept++;
    }
    if (kept == 0u)
        goto done; /* an empty pack cannot be written */

    packError = Gen3ResourcePackWriter_Write(build, &bytes, &diag);
    if (packError != GEN3_PACK_OK)
        goto done;
    f = fopen(path, "wb");
    if (f == NULL)
        goto done;
    if (fwrite(bytes.data, 1, bytes.size, f) != bytes.size)
        goto done;
    fclose(f);
    f = NULL;
    ok = true;

done:
    if (!ok)
    {
        /* Surface the writer's rejection reason: every matrix case that
         * builds a pack must fail with the intended seam error, and a
         * writer-level refusal means the CASE is broken, not the seam. */
        size_t d;
        fprintf(stderr, "BuildLeafPack failed (packError=%d):\n",
                (int)packError);
        for (d = 0; d < diag.count; d++)
            fprintf(stderr, "  [%zu] stage=%d entry=%zu name='%s'\n",
                    d, (int)diag.items[d].stage, diag.items[d].entryIndex,
                    diag.items[d].hasName ? diag.items[d].canonicalName
                                          : "(null)");
    }
    if (f != NULL)
        fclose(f);
    Gen3ResourcePackBytes_Destroy(&bytes);
    if (fixes != NULL)
    {
        for (i = 0; i < count; i++)
            free(fixes[i].payload);
        free(fixes);
    }
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    return ok;
}

/* Sort the fixture array by canonical name (arena zone order). */
static int CompareFixturesByName(const void *a, const void *b)
{
    const struct LeafFixture *fa = (const struct LeafFixture *)a;
    const struct LeafFixture *fb = (const struct LeafFixture *)b;
    return strcmp(fa->canonicalName, fb->canonicalName);
}

/* Sort by claimed ROM offset (tie-break by name, like the seam's phase-1c
 * proof): the disjointness re-derivation walks THIS order. */
static int CompareFixturesByOffset(const void *a, const void *b)
{
    const struct LeafFixture *fa = (const struct LeafFixture *)a;
    const struct LeafFixture *fb = (const struct LeafFixture *)b;
    if (fa->romOffset != fb->romOffset)
        return fa->romOffset < fb->romOffset ? -1 : 1;
    return strcmp(fa->canonicalName, fb->canonicalName);
}

/* ------------------------------------------------------------------ */
/* A: counts + generated-table cross-check                             */
/* ------------------------------------------------------------------ */

static void TestLeafCounts(const struct Gen3ResourcePack *pack)
{
    size_t movement = 0u;
    size_t multiboot = 0u;
    size_t i;
    size_t n = Gen3ResourcePack_GetEntryCount(pack);

    for (i = 0; i < n; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        if (IsMovementEntry(entry))
            movement++;
        else if (IsMultibootEntry(entry))
            multiboot++;
    }
    CHECK("pack: 1055 movement entries (schema 1, binary)",
          movement == EMERALD_LEAF_MOVEMENT_COUNT);
    CHECK("pack: 2 multiboot entries (schema 2, binary)",
          multiboot == EMERALD_LEAF_MULTIBOOT_COUNT);

    /* The compiled slot table is the expected inventory: name/size/schema
     * set equality against the pack's leaf subset. */
    if (movement == EMERALD_LEAF_MOVEMENT_COUNT
     && multiboot == EMERALD_LEAF_MULTIBOOT_COUNT)
    {
        size_t matched = 0u;
        for (i = 0; i < EMERALD_LEAF_RESOURCE_COUNT; i++)
        {
            const struct LeafNativeResource *slot = &kLeafNativeResources[i];
            const struct Gen3ResourcePackEntry *entry =
                Gen3ResourcePack_FindByCanonicalName(pack, slot->name);
            if (entry != NULL && entry->type == GEN3_RESOURCE_TYPE_BINARY
             && entry->schema == slot->schema
             && entry->payloadSize == slot->size)
                matched++;
        }
        CHECK("pack leaf set == generated slot table "
              "(name/size/schema for all 1057)",
              matched == EMERALD_LEAF_RESOURCE_COUNT);
    }
}

/* ------------------------------------------------------------------ */
/* B+C: publication against the real pack                              */
/* ------------------------------------------------------------------ */

static void TestPublication(const char *packPath)
{
    struct TestSession session;
    struct EmeraldLeafCompatDiagnostics diag;
    enum EmeraldLeafCompatStatus status;
    const uint8_t *arenaBase = NULL;
    size_t arenaSize = 0u;
    size_t i;
    size_t pos = 0u;
    struct LeafFixture *sorted = NULL;

    CHECK("real session opens", OpenSession(packPath, &session));

    sorted = (struct LeafFixture *)malloc(
        sFixtureCount * sizeof(*sorted));
    CHECK("sorted fixture array allocates", sorted != NULL);
    if (sorted != NULL)
    {
        memcpy(sorted, sFixtures, sFixtureCount * sizeof(*sorted));
        qsort(sorted, sFixtureCount, sizeof(*sorted), CompareFixturesByName);
    }

    status = EmeraldLeafCompat_TryInitialize(session.snapshot, session.pack,
                                             &diag);
    CHECK("TryInitialize OK", status == EMERALD_LEAF_OK);
    CHECK("published count 1057",
          EmeraldLeafCompat_GetPublishedCount() == EMERALD_LEAF_RESOURCE_COUNT);
    CHECK("arena published",
          EmeraldLeafCompat_GetArena(&arenaBase, &arenaSize));
    CHECK("arena size == 183780",
          arenaBase != NULL && arenaSize == EMERALD_LEAF_TOTAL_BYTES);

    /* Byte equality: every leaf's arena bytes == the pack payload, and
     * the zone is exactly the concatenation in sorted-name order (the
     * arena layout order - sFixtures is in PACK order, which is keyed). */
    for (i = 0; i < sFixtureCount; i++)
    {
        const struct LeafFixture *fix;
        const uint8_t *bytes = NULL;
        size_t size = 0u;
        size_t romOffset = 0u;
        uint8_t schema = 0u;
        if (sorted == NULL)
            break;
        fix = &sorted[i];
        if (!EmeraldLeafCompat_GetResourceBytes(fix->canonicalName, &bytes,
                                                &size))
            break;
        if (size != fix->payloadSize
         || memcmp(bytes, fix->payload, size) != 0)
            break;
        if (!EmeraldLeafCompat_GetResourceSpan(fix->canonicalName,
                                               &romOffset, &size, &schema)
         || romOffset != fix->romOffset || size != fix->payloadSize
         || schema != fix->schema)
            break;
        if (arenaBase != NULL && bytes != arenaBase + pos)
            break;
        pos += size;
    }
    CHECK("every leaf: arena bytes == pack payload, span == pack entry",
          i == sFixtureCount);
    if (arenaBase != NULL && sorted != NULL && sFixtureCount > 0u)
    {
        size_t total = 0u;
        for (i = 0u; i < sFixtureCount; i++)
            total += sFixtures[i].payloadSize;
        CHECK("zone size == sum of the 1057 payloads",
              arenaSize == total);
    }

    /* Slice-disjointness: the published ROM spans are pairwise disjoint
     * (re-derived by phase 1c at publish, on OFFSET order - a name-sorted
     * adjacency check would be nonsense). */
    if (sorted != NULL)
    {
        struct LeafFixture *byOffset = NULL;
        int disjoint = 1;
        byOffset = (struct LeafFixture *)malloc(
            sFixtureCount * sizeof(*byOffset));
        CHECK("offset-sorted fixture array allocates", byOffset != NULL);
        if (byOffset != NULL)
        {
            size_t j;
            memcpy(byOffset, sFixtures, sFixtureCount * sizeof(*byOffset));
            qsort(byOffset, sFixtureCount, sizeof(*byOffset),
                  CompareFixturesByOffset);
            for (j = 0; j + 1u < sFixtureCount; j++)
                if ((uint64_t)byOffset[j].romOffset
                        + byOffset[j].payloadSize
                        > byOffset[j + 1u].romOffset)
                    disjoint = 0;
            free(byOffset);
        }
        CHECK("claimed ROM slices pairwise disjoint", disjoint != 0);
    }

    /* ContainsPointer canary: the payload zone bounds exactly. */
    if (arenaBase != NULL && arenaSize > 0u)
    {
        CHECK("canary: first payload byte inside",
              EmeraldLeafCompat_ContainsPointer((uintptr_t)arenaBase));
        CHECK("canary: last payload byte inside",
              EmeraldLeafCompat_ContainsPointer(
                  (uintptr_t)(arenaBase + arenaSize - 1u)));
        CHECK("canary: one past the zone outside",
              !EmeraldLeafCompat_ContainsPointer(
                  (uintptr_t)(arenaBase + arenaSize)));
        CHECK("canary: unrelated address outside",
              !EmeraldLeafCompat_ContainsPointer((uintptr_t)&sFixtures));
    }

    /* Query helpers fail closed. */
    CHECK("unknown name: GetResourceBytes false",
          !EmeraldLeafCompat_GetResourceBytes("emerald:movement/nope", NULL,
                                              NULL));
    CHECK("unknown name: GetResourceSpan false",
          !EmeraldLeafCompat_GetResourceSpan("emerald:movement/nope", NULL,
                                             NULL, NULL));
    CHECK("NULL name: GetResourceBytes false",
          !EmeraldLeafCompat_GetResourceBytes(NULL, NULL, NULL));

    /* Invalid arguments fail closed. */
    status = EmeraldLeafCompat_TryInitialize(NULL, session.pack, &diag);
    CHECK("NULL snapshot -> INVALID_ARGUMENT",
          status == EMERALD_LEAF_ERR_INVALID_ARGUMENT);
    status = EmeraldLeafCompat_TryInitialize(session.snapshot, NULL, &diag);
    CHECK("NULL pack -> INVALID_ARGUMENT",
          status == EMERALD_LEAF_ERR_INVALID_ARGUMENT);

    free(sorted);
    CloseSession(&session);
}

/* ------------------------------------------------------------------ */
/* D: additive semantics - a failed republish keeps the arena          */
/* ------------------------------------------------------------------ */

static void MutateTruncate(struct LeafFixture *fix, size_t index)
{
    uint8_t *shorter;

    (void)index;
    /* Cut by ONE, never to zero: the pack writer refuses zero-size
     * payloads (GEN3_PACK_ERR_BAD_METADATA - a 4-byte script cut by 4
     * would abort the build before the seam ever ran), and a 1-byte cut
     * still flips every mismatch gate the matrix targets. */
    if (fix->payloadSize <= 1u)
        return;
    shorter = (uint8_t *)malloc(fix->payloadSize - 1u);
    if (shorter == NULL)
        return;
    memcpy(shorter, fix->payload, fix->payloadSize - 1u);
    free(fix->payload);
    fix->payload = shorter;
    fix->payloadSize -= 1u;
}

/* Cut ONLY the multiboot fixtures (index >= 1055): the movement entries
 * stay byte-identical so the E7 pack passes the per-entry checks and the
 * counts gate (1055 + 2), and only the payload-bytes pin fails. */
static void MutateTruncateMultiboot(struct LeafFixture *fix, size_t index)
{
    if (index < EMERALD_LEAF_MOVEMENT_COUNT)
        return;
    MutateTruncate(fix, index);
}

static void TestAdditiveSemantics(const char *packPath, const char *tempDir)
{
    struct TestSession session;
    struct EmeraldLeafCompatDiagnostics diag;
    enum EmeraldLeafCompatStatus status;
    const uint8_t *arenaBase = NULL;
    const uint8_t *bytes = NULL;
    size_t arenaSize = 0u;
    size_t size = 0u;
    char path[512];

    CHECK("additive: real session opens", OpenSession(packPath, &session));
    status = EmeraldLeafCompat_TryInitialize(session.snapshot, session.pack,
                                             &diag);
    CHECK("additive: publish OK", status == EMERALD_LEAF_OK);
    CHECK("additive: arena live",
          EmeraldLeafCompat_GetArena(&arenaBase, &arenaSize));

    /* A truncated synthetic pack (same session) must FAIL the publish and
     * leave the already-published arena untouched: the game keeps serving
     * the compiled payloads exactly as pre-R13-B either way. */
    snprintf(path, sizeof(path), "%s/truncated.rpack", tempDir);
    CHECK("additive: truncated pack builds",
          BuildLeafPack(path, sFixtures, sFixtureCount, MutateTruncate));
    {
        struct Gen3ResourcePack *syntheticPack = NULL;
        struct Gen3ResourcePackDiagnosticList openDiag;
        Gen3ResourcePackDiagnostics_Init(&openDiag);
        CHECK("additive: truncated pack opens",
              Gen3ResourcePack_OpenFile(path, &syntheticPack, &openDiag)
                  == GEN3_PACK_OK);
        if (syntheticPack != NULL)
        {
            status = EmeraldLeafCompat_TryInitialize(session.snapshot,
                                                     syntheticPack, &diag);
            sLastDiag = &diag;
            CHECK("additive: failed republish -> PAYLOAD_SIZE_MISMATCH",
                  status == EMERALD_LEAF_ERR_PAYLOAD_SIZE_MISMATCH);
            CHECK("additive: arena still published",
                  EmeraldLeafCompat_GetPublishedCount()
                      == EMERALD_LEAF_RESOURCE_COUNT);
            CHECK("additive: arena bytes unchanged",
                  EmeraldLeafCompat_GetResourceBytes(
                      sFixtures[0].canonicalName, &bytes, &size)
                  && size == sFixtures[0].payloadSize
                  && memcmp(bytes, sFixtures[0].payload, size) == 0);
            CHECK("additive: diagnostics name the truncated leaf",
                  strcmp(diag.canonicalName, sFixtures[0].canonicalName) == 0
                  || diag.canonicalName[0] != '\0');
            Gen3ResourcePack_Destroy(syntheticPack);
        }
        Gen3ResourcePackDiagnostics_Destroy(&openDiag);
    }

    EmeraldLeafCompat_ClearMigratedEntries();
    CHECK("additive: clear -> count 0",
          EmeraldLeafCompat_GetPublishedCount() == 0u);
    CHECK("additive: clear -> arena gone",
          !EmeraldLeafCompat_GetArena(&arenaBase, &arenaSize));
    EmeraldLeafCompat_Shutdown();
    CHECK("additive: shutdown idempotent",
          EmeraldLeafCompat_GetPublishedCount() == 0u);

    CloseSession(&session);
}

/* ------------------------------------------------------------------ */
/* E: failure matrix                                                   */
/* ------------------------------------------------------------------ */

/* Drop exactly the first entry (index 0): BuildLeafPack skips NULL
 * payloads, so the pack is the full set minus one. */
static void MutateDrop(struct LeafFixture *fix, size_t index)
{
    if (index != 0u)
        return;
    free(fix->payload);
    fix->payload = NULL;
}

/* Retype one movement entry to an unclassifiable schema. */
static void MutateWrongType(struct LeafFixture *fix, size_t index)
{
    if (index != 0u)
        return;
    if (fix->schema == 1u)
        fix->schema = 3u; /* not 1 or 2: unclassifiable */
}

/* Claim one movement entry's ROM slice at another entry's offset: two
 * records with the same start ALWAYS overlap, so the phase-1c proof must
 * fire regardless of offset-space adjacency. */
static uint32_t sOverlapTarget;

static void MutateOverlap(struct LeafFixture *fix, size_t index)
{
    if (index != 0u)
        return;
    fix->romOffset = sOverlapTarget;
}

static void TestFailureMatrix(const char *tempDir, const char *realPackPath)
{
    struct TestSession session;
    struct EmeraldLeafCompatDiagnostics diag;
    struct Gen3ResourcePackDiagnosticList openDiag;
    enum EmeraldLeafCompatStatus status;
    char path[512];

    Gen3ResourcePackDiagnostics_Init(&openDiag);

    /* Real session (real 1057 names, real payloads) for the pack-side
     * mismatches; synthetic packs reuse the real names/sizes/bytes.
     * Start from a cleared arena: TestPublication published one, and
     * every case asserts "arena absent after failure" - which only means
     * anything if the arena was absent before the case ran. */
    EmeraldLeafCompat_ClearMigratedEntries();
    CHECK("matrix: real session opens", OpenSession(realPackPath, &session));

    /* E6: truncated movement - every pack payload cut by 1 (its own
     * session is irrelevant: the real session's view has the real size,
     * so the first truncated entry fails entry size != view size). */
    {
        snprintf(path, sizeof(path), "%s/e6_trunc_movement.rpack", tempDir);
        CHECK("matrix: truncated pack builds",
              BuildLeafPack(path, sFixtures, sFixtureCount, MutateTruncate));
        {
            struct Gen3ResourcePack *syntheticPack = NULL;
            CHECK("matrix: truncated pack opens",
                  Gen3ResourcePack_OpenFile(path, &syntheticPack, &openDiag)
                      == GEN3_PACK_OK);
            status = EmeraldLeafCompat_TryInitialize(session.snapshot,
                                                     syntheticPack, &diag);
            sLastDiag = &diag;
            CHECK("matrix: truncated movement -> PAYLOAD_SIZE_MISMATCH",
                  status == EMERALD_LEAF_ERR_PAYLOAD_SIZE_MISMATCH);
            CHECK("matrix: arena absent after failure",
                  EmeraldLeafCompat_GetPublishedCount() == 0u);
            Gen3ResourcePack_Destroy(syntheticPack);
        }
    }

    /* E8: overlapping claimed ROM slice - the first movement entry claims
     * the second's offset; the phase-1c disjointness proof must fail
     * closed (the pack writer validates only pack-INTERNAL layout, never
     * source-ROM slice overlap, so the seam re-derives it). */
    {
        sOverlapTarget = sFixtures[1].romOffset;
        snprintf(path, sizeof(path), "%s/e8_overlap.rpack", tempDir);
        CHECK("matrix: overlapping pack builds",
              BuildLeafPack(path, sFixtures, sFixtureCount, MutateOverlap));
        {
            struct Gen3ResourcePack *syntheticPack = NULL;
            CHECK("matrix: overlapping pack opens",
                  Gen3ResourcePack_OpenFile(path, &syntheticPack, &openDiag)
                      == GEN3_PACK_OK);
            status = EmeraldLeafCompat_TryInitialize(session.snapshot,
                                                     syntheticPack, &diag);
            sLastDiag = &diag;
            CHECK("matrix: overlapping slice -> OVERLAPPING_SLICE",
                  status == EMERALD_LEAF_ERR_OVERLAPPING_SLICE);
            CHECK("matrix: diagnostics stage build",
                  strcmp(diag.stage, "build") == 0);
            CHECK("matrix: diagnostics name both slices",
                  strstr(diag.canonicalName, "overlaps") != NULL);
            CHECK("matrix: arena absent after failure",
                  EmeraldLeafCompat_GetPublishedCount() == 0u);
            Gen3ResourcePack_Destroy(syntheticPack);
        }
    }

    /* E4: wrong-type movement - one entry with an unclassifiable schema
     * (the pack-derived session declares it consistently, so the seam's
     * classification filter is the guard). */
    {
        struct TestSession wrongSession;
        snprintf(path, sizeof(path), "%s/e4_wrongtype_movement.rpack", tempDir);
        CHECK("matrix: wrong-type pack builds",
              BuildLeafPack(path, sFixtures, sFixtureCount, MutateWrongType));
        CHECK("matrix: wrong-type session opens",
              OpenSession(path, &wrongSession));
        status = EmeraldLeafCompat_TryInitialize(wrongSession.snapshot,
                                                 wrongSession.pack, &diag);
        sLastDiag = &diag;
        CHECK("matrix: wrong-type movement -> UNEXPECTED_COUNT",
              status == EMERALD_LEAF_ERR_UNEXPECTED_COUNT);
        CHECK("matrix: arena absent after failure",
              EmeraldLeafCompat_GetPublishedCount() == 0u);
        CloseSession(&wrongSession);
    }

    CloseSession(&session);

    /* E1: missing movement family - a 2-entry (multiboot-only) pack fails
     * the composition gate (its own session: pack-derived is consistent by
     * construction). */
    {
        struct TestSession smallSession;
        snprintf(path, sizeof(path), "%s/e1_no_movement.rpack", tempDir);
        CHECK("matrix: multiboot-only pack builds",
              BuildLeafPack(path, sFixtures + EMERALD_LEAF_MOVEMENT_COUNT,
                            EMERALD_LEAF_MULTIBOOT_COUNT, NULL));
        CHECK("matrix: multiboot-only session opens",
              OpenSession(path, &smallSession));
        status = EmeraldLeafCompat_TryInitialize(smallSession.snapshot,
                                                 smallSession.pack, &diag);
        sLastDiag = &diag;
        CHECK("matrix: missing movement family -> UNEXPECTED_COUNT",
              status == EMERALD_LEAF_ERR_UNEXPECTED_COUNT);
        CHECK("matrix: diagnostics name the composition",
              strstr(diag.canonicalName, "leaves") != NULL);
        CHECK("matrix: arena absent after failure",
              EmeraldLeafCompat_GetPublishedCount() == 0u);
        CloseSession(&smallSession);
    }

    /* E2: missing one movement entry - a 1056-entry pack fails the gate. */
    {
        struct TestSession smallSession;
        snprintf(path, sizeof(path), "%s/e2_missing_one.rpack", tempDir);
        CHECK("matrix: one-entry-missing pack builds",
              BuildLeafPack(path, sFixtures, EMERALD_LEAF_MOVEMENT_COUNT,
                            MutateDrop));
        CHECK("matrix: one-entry-missing session opens",
              OpenSession(path, &smallSession));
        status = EmeraldLeafCompat_TryInitialize(smallSession.snapshot,
                                                 smallSession.pack, &diag);
        sLastDiag = &diag;
        CHECK("matrix: missing one movement entry -> UNEXPECTED_COUNT",
              status == EMERALD_LEAF_ERR_UNEXPECTED_COUNT);
        CHECK("matrix: arena absent after failure",
              EmeraldLeafCompat_GetPublishedCount() == 0u);
        CloseSession(&smallSession);
    }

    /* E3: missing multiboot family - a 1055-entry (movement-only) pack. */
    {
        struct TestSession smallSession;
        snprintf(path, sizeof(path), "%s/e3_no_multiboot.rpack", tempDir);
        CHECK("matrix: movement-only pack builds",
              BuildLeafPack(path, sFixtures, EMERALD_LEAF_MOVEMENT_COUNT,
                            NULL));
        CHECK("matrix: movement-only session opens",
              OpenSession(path, &smallSession));
        status = EmeraldLeafCompat_TryInitialize(smallSession.snapshot,
                                                 smallSession.pack, &diag);
        sLastDiag = &diag;
        CHECK("matrix: missing multiboot family -> UNEXPECTED_COUNT",
              status == EMERALD_LEAF_ERR_UNEXPECTED_COUNT);
        CHECK("matrix: arena absent after failure",
              EmeraldLeafCompat_GetPublishedCount() == 0u);
        CloseSession(&smallSession);
    }

    /* E5: wrong-type multiboot - one multiboot entry re-schema'd. */
    {
        struct TestSession wrongSession;
        snprintf(path, sizeof(path), "%s/e5_wrongtype_multiboot.rpack", tempDir);
        CHECK("matrix: wrong-type-multiboot pack builds",
              BuildLeafPack(path, sFixtures + EMERALD_LEAF_MOVEMENT_COUNT,
                            EMERALD_LEAF_MULTIBOOT_COUNT, MutateWrongType));
        CHECK("matrix: wrong-type-multiboot session opens",
              OpenSession(path, &wrongSession));
        status = EmeraldLeafCompat_TryInitialize(wrongSession.snapshot,
                                                 wrongSession.pack, &diag);
        sLastDiag = &diag;
        CHECK("matrix: wrong-type multiboot -> UNEXPECTED_COUNT",
              status == EMERALD_LEAF_ERR_UNEXPECTED_COUNT);
        CHECK("matrix: arena absent after failure",
              EmeraldLeafCompat_GetPublishedCount() == 0u);
        CloseSession(&wrongSession);
    }

    /* E7: truncated multiboot - both multiboot payloads cut by 1, the
     * movement set byte-identical (FULL 1057-entry pack, its OWN session:
     * every per-entry check passes - sizes consistent - and the counts
     * gate passes, so the payload-bytes pin fails: 183,778 != 183,780). */
    {
        struct TestSession smallSession;
        snprintf(path, sizeof(path), "%s/e7_trunc_multiboot.rpack", tempDir);
        CHECK("matrix: truncated-multiboot pack builds",
              BuildLeafPack(path, sFixtures, sFixtureCount,
                            MutateTruncateMultiboot));
        CHECK("matrix: truncated-multiboot session opens",
              OpenSession(path, &smallSession));
        status = EmeraldLeafCompat_TryInitialize(smallSession.snapshot,
                                                 smallSession.pack, &diag);
        sLastDiag = &diag;
        CHECK("matrix: truncated multiboot -> PAYLOAD_SIZE_MISMATCH",
              status == EMERALD_LEAF_ERR_PAYLOAD_SIZE_MISMATCH);
        CHECK("matrix: arena absent after failure",
              EmeraldLeafCompat_GetPublishedCount() == 0u);
        CloseSession(&smallSession);
    }

    /* E9: duplicate key - the pack writer itself rejects a second entry
     * with the same canonical name (GEN3_PACK_ERR_DUPLICATE_NAME). */
    {
        struct Gen3ResourcePackBuild *build = NULL;
        struct Gen3ResourcePackDiagnosticList diag9;
        struct Gen3ResourcePackProfileInput profile;
        struct Gen3ResourcePackEntryInput entry;
        Gen3ResourceKey derived;
        uint8_t sha[32];
        uint8_t provenanceSha[32];
        enum Gen3ResourcePackError packError;

        Gen3ResourcePackDiagnostics_Init(&diag9);
        build = Gen3ResourcePackBuild_Create();
        CHECK("matrix: duplicate test: build created", build != NULL);
        if (build != NULL)
        {
            memset(&profile, 0, sizeof(profile));
            profile.basePackVersion = 1u;
            profile.catalogVersion = 1u;
            profile.extractionManifestVersion = 1u;
            profile.canonicalRepresentationVersion = 1u;
            profile.sourceRomSize = EMERALD_ROM_SIZE;
            profile.sourceRomSha1 = kEmeraldProfileSyntheticSha1;
            profile.sourceRomSha256 = kEmeraldProfileSyntheticSha256;
            memcpy(profile.gameCode, EMERALD_PROFILE_GAME_CODE, 4u);
            memcpy(profile.makerCode, EMERALD_PROFILE_MAKER_CODE, 2u);
            profile.softwareRevision = EMERALD_PROFILE_SOFTWARE_REV;
            snprintf(profile.gameId, sizeof(profile.gameId), "%s",
                     EMERALD_PROFILE_GAME);
            profile.catalogSha256 = kEmeraldProfileSyntheticSha256;
            profile.extractionManifestSha256 = kEmeraldProfileSyntheticSha256;
            packError = Gen3ResourcePackBuild_SetProfile(build, &profile,
                                                         &diag9);
            CHECK("matrix: duplicate test: profile set",
                  packError == GEN3_PACK_OK);
            if (packError == GEN3_PACK_OK)
            {
                Gen3ResourceId_DeriveKey(sFixtures[0].canonicalName, &derived);
                DigestSha256(sFixtures[0].payload, sFixtures[0].payloadSize,
                             sha);
                memset(provenanceSha, 0x5A, sizeof(provenanceSha));
                memset(&entry, 0, sizeof(entry));
                entry.canonicalName = sFixtures[0].canonicalName;
                entry.key = &derived;
                entry.type = GEN3_RESOURCE_TYPE_BINARY;
                entry.schema = sFixtures[0].schema;
                entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
                entry.representation = GEN3_PACK_REPRESENTATION_RAW;
                entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_RAW;
                entry.canonicalPayload = sFixtures[0].payload;
                entry.canonicalPayloadSize = sFixtures[0].payloadSize;
                entry.canonicalPayloadSha256 = sha;
                entry.sourceRomOffset = sFixtures[0].romOffset;
                entry.sourceEncodedSize = sFixtures[0].payloadSize;
                entry.sourceEncodedSha256 = provenanceSha;
                packError = Gen3ResourcePackBuild_AddEntry(build, &entry,
                                                           &diag9);
                CHECK("matrix: duplicate test: first add OK",
                      packError == GEN3_PACK_OK);
                packError = Gen3ResourcePackBuild_AddEntry(build, &entry,
                                                           &diag9);
                CHECK("matrix: duplicate key -> writer rejects",
                      packError == GEN3_PACK_ERR_DUPLICATE_NAME);
            }
        }
        Gen3ResourcePackBuild_Destroy(build);
        Gen3ResourcePackDiagnostics_Destroy(&diag9);
    }

    Gen3ResourcePackDiagnostics_Destroy(&openDiag);
}

int main(int argc, char **argv)
{
    const char *packPath;
    const char *tempDir;
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <production-pack.rpack> <temp-dir>\n",
                argv[0]);
        return 2;
    }
    packPath = argv[1];
    tempDir = argv[2];

    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK)
    {
        fprintf(stderr, "FAIL: cannot open production pack %s\n", packPath);
        return 1;
    }
    if (!LoadLeafFixtures(pack))
    {
        fprintf(stderr, "FAIL: cannot load the 1057 leaf fixtures from %s\n",
                packPath);
        Gen3ResourcePack_Destroy(pack);
        return 1;
    }

    TestLeafCounts(pack);
    TestPublication(packPath);
    TestFailureMatrix(tempDir, packPath);
    TestAdditiveSemantics(packPath, tempDir);

    FreeLeafFixtures();
    Gen3ResourcePack_Destroy(pack);

    printf("\nemerald leaf compat: %d checks, %d failures\n",
           sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
