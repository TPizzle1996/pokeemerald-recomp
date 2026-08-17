/* R12-B focused test: audio leaf ownership migration (tests A-G).
 *
 * Drives the R12-B seam (emerald_audio_compat.c) against the REAL production
 * pack (games/emerald/base/emerald-bpee01-v1.rpack, 5087 entries incl. the
 * 569 audio leaves) plus synthetic packs for the failure matrix:
 *
 *   A. counts        - pack composition: 105 root + 51 phoneme + 388 cry
 *                      samples + 25 programmable waves = 569 leaves, all
 *                      type AUDIO_SAMPLE / schema 1, names classified by the
 *                      R12-A taxonomy (pack total 5087);
 *   B. exact         - every leaf's ROM-relative arena offset fits the
 *                      verbatim zone [0x0867709C, 0x089A3DB4], no two leaves
 *                      overlap, and the published arena bytes are
 *                      byte-identical to the pack payloads (which the
 *                      R12-B manifest already proved == ROM slice == ELF
 *                      slice three-way);
 *   C. WaveData      - structural WaveData2 header validation for every
 *                      sample (16 B header: compression flags, u32 freq,
 *                      u32 loopStart, u32 size = sampleCount-1), exactly as
 *                      the bindings generator validates;
 *   D. waves 16 B    - every programmable wave is the exact 16-byte CGB
 *                      wave-table representation;
 *   E. determinism   - gen3-pack-build --check on the production pack (shell
 *                      runner);
 *   F. publication   - session -> TryInitialize -> arena published with
 *                      569 leaves, GetLeafSpan/GetLeafBytes agree with the
 *                      pack, holes zeroed, republish OK, relocated
 *                      session/arena replaces atomically, clear/shutdown
 *                      fail closed, invalid arguments fail closed;
 *   G. failure matrix- synthetic packs/sessions: missing leaf, renamed leaf
 *                      (unresolvable), wrong type, wrong schema, wrong size,
 *                      corrupt payload, out-of-span placement, and the pack
 *                      writer's duplicate-key guard.
 *
 * The seam is platform-neutral; this harness needs no native tables, so it
 * links the gen3 core + session + seam directly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_pack_writer.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/sha256.h"
#include "emerald/resources/emerald_audio_compat.h"
#include "emerald/resources/emerald_rom_profile.h"
#include "emerald/resources/emerald_resource_session.h"

/* ------------------------------------------------------------------ */
/* CHECK machinery (same shape as the other offline harnesses)         */
/* ------------------------------------------------------------------ */

static int sChecks = 0;
static int sFails = 0;

#define CHECK(label, cond)                                                   \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFails++;                                                        \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (label));         \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Leaf taxonomy (mirrors the seam's ClassifyLeaf)                     */
/* ------------------------------------------------------------------ */

enum AudioLeafKind
{
    LEAF_ROOT = 0,
    LEAF_PHONEME,
    LEAF_CRY,
    LEAF_WAVE,
    LEAF_UNKNOWN,
};

static enum AudioLeafKind ClassifyLeaf(const char *name)
{
    static const char rootPrefix[] = "emerald:audio/sample/";
    static const char cryPrefix[] = "emerald:audio/sample/cry/";
    static const char phonemePrefix[] = "emerald:audio/sample/phoneme/";
    static const char wavePrefix[] = "emerald:audio/wave/programmable/";

    if (name == NULL)
        return LEAF_UNKNOWN;
    if (strncmp(name, wavePrefix, sizeof(wavePrefix) - 1u) == 0)
        return LEAF_WAVE;
    if (strncmp(name, cryPrefix, sizeof(cryPrefix) - 1u) == 0)
        return LEAF_CRY;
    if (strncmp(name, phonemePrefix, sizeof(phonemePrefix) - 1u) == 0)
        return LEAF_PHONEME;
    if (strncmp(name, rootPrefix, sizeof(rootPrefix) - 1u) == 0
     && strchr(name + sizeof(rootPrefix) - 1u, '/') == NULL)
        return LEAF_ROOT;
    return LEAF_UNKNOWN;
}

/* ------------------------------------------------------------------ */
/* Fixtures: the 569 leaves of the REAL production pack, deep-copied   */
/* ------------------------------------------------------------------ */

struct AudioLeafFixture
{
    char canonicalName[96];
    enum AudioLeafKind kind;
    enum Gen3ResourceType type;
    uint32_t schema;
    uint8_t *payload;          /* test-owned copy */
    size_t payloadSize;
    uint64_t sourceRomOffset;
    Gen3ResourceKey key;       /* value copy from the pack entry */
    uint32_t arenaOffset;      /* romAddr - EMERALD_AUDIO_ROM_START */
};

static struct AudioLeafFixture sLeaves[EMERALD_AUDIO_LEAF_COUNT];
static size_t sLeafCount = 0;
static size_t sRootCount = 0, sPhonemeCount = 0, sCryCount = 0, sWaveCount = 0;

/* Load the 569 audio leaves from the real pack (owned copies). */
static bool LoadAudioLeaves(const struct Gen3ResourcePack *pack)
{
    size_t i, n = Gen3ResourcePack_GetEntryCount(pack);
    size_t count = 0;

    for (i = 0; i < n; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        struct AudioLeafFixture *fix;
        enum AudioLeafKind kind;

        if (entry->type != GEN3_RESOURCE_TYPE_AUDIO_SAMPLE || entry->schema != 1u)
            continue;
        if (count >= EMERALD_AUDIO_LEAF_COUNT)
            return false;
        kind = ClassifyLeaf(entry->canonicalName);
        if (kind == LEAF_UNKNOWN || entry->payload == NULL
         || entry->payloadSize == 0u)
            return false;
        fix = &sLeaves[count];
        memset(fix, 0, sizeof(*fix));
        snprintf(fix->canonicalName, sizeof(fix->canonicalName), "%s",
                 entry->canonicalName);
        fix->kind = kind;
        fix->type = entry->type;
        fix->schema = entry->schema;
        fix->payload = (uint8_t *)malloc(entry->payloadSize);
        if (fix->payload == NULL)
            return false;
        memcpy(fix->payload, entry->payload, entry->payloadSize);
        fix->payloadSize = entry->payloadSize;
        fix->sourceRomOffset = entry->sourceRomOffset;
        fix->key = entry->key;
        fix->arenaOffset =
            (uint32_t)((uint64_t)entry->sourceRomOffset + 0x08000000u
                       - EMERALD_AUDIO_ROM_START);
        count++;
    }
    sLeafCount = count;
    for (i = 0; i < count; i++)
    {
        switch (sLeaves[i].kind)
        {
        case LEAF_ROOT:    sRootCount++;    break;
        case LEAF_PHONEME: sPhonemeCount++; break;
        case LEAF_CRY:     sCryCount++;     break;
        case LEAF_WAVE:    sWaveCount++;    break;
        default:                             break;
        }
    }
    return count == EMERALD_AUDIO_LEAF_COUNT;
}

static void FreeAudioLeaves(void)
{
    size_t i;
    for (i = 0; i < sLeafCount; i++)
    {
        free(sLeaves[i].payload);
        sLeaves[i].payload = NULL;
    }
    sLeafCount = 0;
}

/* ------------------------------------------------------------------ */
/* Session construction (pack -> pack-derived catalog -> ROM_BASE      */
/* candidate -> snapshot), mirroring the runtime loader's path.        */
/* ------------------------------------------------------------------ */

struct TestSession
{
    struct Gen3ResourcePack *pack;
    struct Gen3ResourceSnapshot *snapshot;
};

static bool BuildCatalogFromPackEntries(const struct Gen3ResourcePack *pack,
                                        struct Gen3ResourceCatalog **out)
{
    struct Gen3ResourceCatalog *catalog;
    size_t i, n = Gen3ResourcePack_GetEntryCount(pack);

    catalog = Gen3ResourceCatalog_Create();
    if (catalog == NULL)
        return false;
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

/* Deep-copy the fixture array, run `mutate` on each copy (may rename,
 * retype, re-schema, resize, corrupt, or move a leaf), then write a
 * deterministic pack. Returns the mutated deep copy for inspection. */
static bool BuildPackFromLeaves(const char *path,
                                const struct AudioLeafFixture *src, size_t count,
                                void (*mutate)(struct AudioLeafFixture *fix,
                                               size_t index))
{
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    struct AudioLeafFixture *fixes = NULL;
    uint8_t sha[32];
    uint8_t provenanceSha[32];
    FILE *f = NULL;
    size_t i;
    enum Gen3ResourcePackError packError;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;
    fixes = (struct AudioLeafFixture *)calloc(count, sizeof(*fixes));
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
        memset(&entry, 0, sizeof(entry));
        Gen3ResourceId_DeriveKey(fixes[i].canonicalName, &fixes[i].key);
        DigestSha256(fixes[i].payload, fixes[i].payloadSize, sha);
        entry.canonicalName = fixes[i].canonicalName;
        entry.key = &fixes[i].key;
        entry.type = fixes[i].type;
        entry.schema = fixes[i].schema;
        entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
        entry.representation = GEN3_PACK_REPRESENTATION_RAW;
        entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_RAW;
        entry.canonicalPayload = fixes[i].payload;
        entry.canonicalPayloadSize = fixes[i].payloadSize;
        entry.canonicalPayloadSha256 = sha;
        entry.sourceRomOffset = fixes[i].sourceRomOffset;
        entry.sourceEncodedSize = fixes[i].payloadSize;
        entry.sourceEncodedSha256 = provenanceSha;
        packError = Gen3ResourcePackBuild_AddEntry(build, &entry, &diag);
        if (packError != GEN3_PACK_OK)
            goto done;
    }

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

/* The mutate hooks for the failure matrix: each damages exactly ONE leaf
 * of the real 569-leaf fixture set - the cry sample
 * emerald:audio/sample/cry/abra - leaving the other 568 untouched. */
static bool IsAbraCry(const struct AudioLeafFixture *fix)
{
    return strcmp(fix->canonicalName, "emerald:audio/sample/cry/abra") == 0;
}
static void MutateRenameLeaf(struct AudioLeafFixture *fix, size_t index)
{
    (void)index;
    if (IsAbraCry(fix))
        strcpy(fix->canonicalName, "emerald:audio/sample/cry/abra-x");
}
static void MutateTruncate(struct AudioLeafFixture *fix, size_t index)
{
    (void)index;
    if (IsAbraCry(fix))
        fix->payloadSize = 16u;
}
static void MutateCorrupt(struct AudioLeafFixture *fix, size_t index)
{
    (void)index;
    if (IsAbraCry(fix))
        fix->payload[0] ^= 0xFFu;
}
static void MutateOffset(struct AudioLeafFixture *fix, size_t index)
{
    (void)index;
    if (IsAbraCry(fix))
        fix->sourceRomOffset = 0u;
}
/* The session layer validates payloads against their declared type
 * (TILE_GRAPHICS requires size % 32 == 0), so a mis-typed leaf must also
 * carry a payload valid for the WRONG type or the pack is refused at
 * session build (which is itself a guard, but then the seam never runs).
 * Pad the payload to a tile-valid multiple of 32. */
static void PadToTileMultiple(struct AudioLeafFixture *fix)
{
    size_t padded = (fix->payloadSize + 31u) & ~(size_t)31u;
    uint8_t *newPayload;
    if (padded == fix->payloadSize)
        return;
    newPayload = (uint8_t *)realloc(fix->payload, padded);
    if (newPayload == NULL)
        return; /* keep the original buffer; the session guard then trips */
    memset(newPayload + fix->payloadSize, 0, padded - fix->payloadSize);
    fix->payload = newPayload;
    fix->payloadSize = padded;
}

static void MutateType(struct AudioLeafFixture *fix, size_t index)
{
    (void)index;
    if (IsAbraCry(fix))
    {
        fix->type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
        PadToTileMultiple(fix);
    }
}
static void MutateSchema(struct AudioLeafFixture *fix, size_t index)
{
    (void)index;
    if (IsAbraCry(fix))
        fix->schema = 2u;
}

static void MutateAllToTiles(struct AudioLeafFixture *fix, size_t index)
{
    fix->type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    PadToTileMultiple(fix);
    (void)index;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* A. counts: pack composition pins 105/51/388/25 = 569. */
static void TestAudioCounts(const struct Gen3ResourcePack *pack)
{
    CHECK("pack total 5087 entries", Gen3ResourcePack_GetEntryCount(pack) == 5087u);
    CHECK("leaf total 569", sLeafCount == EMERALD_AUDIO_LEAF_COUNT);
    CHECK("root count 105", sRootCount == EMERALD_AUDIO_ROOT_COUNT);
    CHECK("phoneme count 51", sPhonemeCount == EMERALD_AUDIO_PHONEME_COUNT);
    CHECK("cry count 388", sCryCount == EMERALD_AUDIO_CRY_COUNT);
    CHECK("wave count 25", sWaveCount == EMERALD_AUDIO_WAVE_COUNT);
    CHECK("macro LEAF_COUNT == 569",
          EMERALD_AUDIO_LEAF_COUNT == 105u + 51u + 388u + 25u);
}

/* C + D: structural WaveData2 validation per leaf; waves are exactly 16 B. */
static void TestWaveDataValidation(void)
{
    size_t i;
    for (i = 0; i < sLeafCount; i++)
    {
        const struct AudioLeafFixture *fix = &sLeaves[i];
        uint8_t cf;
        uint32_t freq, loopStart, hdrSize;
        uint32_t sampleCount;

        if (fix->kind == LEAF_WAVE)
        {
            CHECK("wave payload exactly 16 B", fix->payloadSize == 16u);
            continue;
        }
        CHECK("sample payload >= 16 B header", fix->payloadSize >= 16u);
        cf = fix->payload[0] | fix->payload[1] | fix->payload[2];
        freq = (uint32_t)fix->payload[4] | ((uint32_t)fix->payload[5] << 8)
             | ((uint32_t)fix->payload[6] << 16) | ((uint32_t)fix->payload[7] << 24);
        loopStart = (uint32_t)fix->payload[8] | ((uint32_t)fix->payload[9] << 8)
                  | ((uint32_t)fix->payload[10] << 16) | ((uint32_t)fix->payload[11] << 24);
        hdrSize = (uint32_t)fix->payload[12] | ((uint32_t)fix->payload[13] << 8)
                | ((uint32_t)fix->payload[14] << 16) | ((uint32_t)fix->payload[15] << 24);
        sampleCount = hdrSize + 1u; /* size field stores sampleCount - 1 */
        CHECK("WaveData freq nonzero", freq != 0u);
        CHECK("WaveData loopStart within [0, sampleCount]",
              loopStart <= sampleCount);
        if (cf == 0u)
        {
            /* Uncompressed PCM: artifact == 16 + sampleCount bytes. */
            CHECK("uncompressed artifact == 16 + size-field + 1",
                  fix->payloadSize == 16u + sampleCount);
        }
        else
        {
            /* Compressed: only cries carry compression; the file is the
             * delta-compressed extent (must exceed the header). */
            CHECK("compressed leaf is a cry", fix->kind == LEAF_CRY);
            CHECK("compressed cry payload exceeds header", fix->payloadSize > 16u);
        }
        if (fix->kind != LEAF_CRY)
            CHECK("non-cry samples are uncompressed", cf == 0u);
    }
}

/* B (layout): every leaf fits the verbatim zone; no overlaps. */
static void TestArenaLayout(void)
{
    size_t i, j;
    size_t payloadBytes = 0;
    size_t overlaps = 0;

    for (i = 0; i < sLeafCount; i++)
    {
        const struct AudioLeafFixture *fix = &sLeaves[i];
        uint64_t romAddr = fix->sourceRomOffset + 0x08000000u;

        CHECK("leaf ROM address inside audio span",
              romAddr >= EMERALD_AUDIO_ROM_START);
        CHECK("leaf end inside audio span",
              romAddr + fix->payloadSize <= EMERALD_AUDIO_SPAN_END);
        CHECK("arenaOffset matches romAddr - start",
              fix->arenaOffset == (uint32_t)(romAddr - EMERALD_AUDIO_ROM_START));
        CHECK("leaf fits span size",
              (uint64_t)fix->arenaOffset + fix->payloadSize
                  <= EMERALD_AUDIO_SPAN_SIZE);
        payloadBytes += fix->payloadSize;
    }
    for (i = 0; i < sLeafCount; i++)
        for (j = i + 1; j < sLeafCount; j++)
        {
            const struct AudioLeafFixture *a = &sLeaves[i], *b = &sLeaves[j];
            uint64_t aEnd = (uint64_t)a->arenaOffset + a->payloadSize;
            uint64_t bEnd = (uint64_t)b->arenaOffset + b->payloadSize;
            if (a->arenaOffset < bEnd && b->arenaOffset < aEnd)
                overlaps++;
        }
    CHECK("no two leaves overlap in the zone", overlaps == 0u);
    /* The zone is sparse by design: 569 payloads never cover the whole
     * 3,329,304-byte span (the report pins the exact coverage). */
    CHECK("span sparse (payloads < span)", payloadBytes < EMERALD_AUDIO_SPAN_SIZE);
}

/* F: publication through a session built from the REAL production pack. */
static void TestPublication(const char *packPath)
{
    struct TestSession session;
    struct EmeraldAudioCompatDiagnostics diag;
    const uint8_t *arenaBase = NULL;
    size_t arenaSize = 0;
    size_t i;
    size_t holeOffset = SIZE_MAX;
    uint64_t firstRomAddr = UINT64_MAX;

    /* Relocated session/arena: build session A, publish, build session B
     * (a second, independent snapshot from the same pack), republish, and
     * verify the replacement is atomic and byte-identical. */
    CHECK("session A opens", OpenSession(packPath, &session));
    CHECK("initial state: no arena", EmeraldAudioCompat_GetPublishedCount() == 0u);
    CHECK("initial state: GetArena false",
          !EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize));
    CHECK("republish with no arena is UNAVAILABLE",
          EmeraldAudioCompat_Republish(&diag)
              == EMERALD_AUDIO_ERR_UNAVAILABLE);
    CHECK("invalid args are INVALID_ARGUMENT",
          EmeraldAudioCompat_TryInitialize(NULL, NULL, &diag)
              == EMERALD_AUDIO_ERR_INVALID_ARGUMENT);

    CHECK("TryInitialize OK",
          EmeraldAudioCompat_TryInitialize(session.snapshot, session.pack, &diag)
              == EMERALD_AUDIO_OK);
    CHECK("arena published (GetArena)", EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize));
    CHECK("arena size == span", arenaSize == EMERALD_AUDIO_SPAN_SIZE);
    CHECK("published count 569", EmeraldAudioCompat_GetPublishedCount() == EMERALD_AUDIO_LEAF_COUNT);

    /* Every leaf: span + bytes agree with the pack payloads. */
    for (i = 0; i < sLeafCount; i++)
    {
        const struct AudioLeafFixture *fix = &sLeaves[i];
        size_t spanOffset = 0, spanSize = 0;
        const uint8_t *leafBytes = NULL;
        size_t leafSize = 0;

        CHECK("GetLeafSpan resolves", EmeraldAudioCompat_GetLeafSpan(fix->canonicalName, &spanOffset, &spanSize));
        CHECK("GetLeafSpan offset matches pack-derived offset", spanOffset == fix->arenaOffset);
        CHECK("GetLeafSpan size matches payload", spanSize == fix->payloadSize);
        CHECK("GetLeafBytes resolves", EmeraldAudioCompat_GetLeafBytes(fix->canonicalName, &leafBytes, &leafSize));
        CHECK("GetLeafBytes size matches payload", leafSize == fix->payloadSize);
        CHECK("arena bytes == pack bytes",
              leafBytes != NULL
              && memcmp(leafBytes, fix->payload, fix->payloadSize) == 0);
        CHECK("arena offset == ROM-relative offset",
              leafBytes == arenaBase + fix->arenaOffset);
        if (fix->sourceRomOffset < firstRomAddr)
            firstRomAddr = fix->sourceRomOffset;
    }
    CHECK("unknown name: GetLeafSpan false",
          !EmeraldAudioCompat_GetLeafSpan("emerald:audio/sample/cry/not-a-real-mon", NULL, NULL));
    CHECK("unknown name: GetLeafBytes false",
          !EmeraldAudioCompat_GetLeafBytes("emerald:audio/sample/cry/not-a-real-mon", NULL, NULL));

    /* The zone is a VERBATIM ZONE at ROM-relative offsets: the first leaf
     * does not sit at offset 0 (the span starts at the voicegroup_dummy
     * anchor 0x0867709C, before the first sample payload), so zone start is
     * an unbacked hole that must be zeroed. Find one byte covered by no
     * leaf and verify it is zero. */
    holeOffset = SIZE_MAX;
    for (i = 0; i < sLeafCount; i++)
    {
        const struct AudioLeafFixture *fix = &sLeaves[i];
        if (fix->arenaOffset == 0u)
            break; /* a leaf at zone start: no probe at 0; use a gap below */
    }
    if (i == sLeafCount)
        holeOffset = 0u; /* no leaf at zone start: byte 0 is a hole */
    if (holeOffset == SIZE_MAX)
    {
        /* Fall back: a byte just before a leaf that no other leaf covers. */
        for (i = 0; i < sLeafCount; i++)
        {
            size_t candidate = sLeaves[i].arenaOffset > 0u
                                 ? sLeaves[i].arenaOffset - 1u : 0u;
            size_t j;
            bool covered = false;
            for (j = 0; j < sLeafCount; j++)
            {
                const struct AudioLeafFixture *other = &sLeaves[j];
                if (other->arenaOffset <= candidate
                 && candidate < (size_t)other->arenaOffset + other->payloadSize)
                {
                    covered = true;
                    break;
                }
            }
            if (!covered)
            {
                holeOffset = candidate;
                break;
            }
        }
    }
    CHECK("hole probe offset found", holeOffset != SIZE_MAX);
    CHECK("hole probe offset within span",
          holeOffset < EMERALD_AUDIO_SPAN_SIZE);
    if (holeOffset != SIZE_MAX && holeOffset < EMERALD_AUDIO_SPAN_SIZE)
        CHECK("unbacked hole is zeroed", arenaBase[holeOffset] == 0u);

    /* Republish (immutable arena): OK. */
    CHECK("republish OK", EmeraldAudioCompat_Republish(&diag) == EMERALD_AUDIO_OK);

    /* Relocated session: a second snapshot from the same pack replaces the
     * arena atomically with identical content. */
    {
        struct TestSession sessionB;
        CHECK("session B opens", OpenSession(packPath, &sessionB));
        CHECK("TryInitialize (session B) OK",
              EmeraldAudioCompat_TryInitialize(sessionB.snapshot, sessionB.pack, &diag)
                  == EMERALD_AUDIO_OK);
        CHECK("count still 569 after replace",
              EmeraldAudioCompat_GetPublishedCount() == EMERALD_AUDIO_LEAF_COUNT);
        CHECK("bytes still == pack after replace",
              memcmp(arenaBase + sLeaves[0].arenaOffset, sLeaves[0].payload,
                     sLeaves[0].payloadSize) == 0);
        CloseSession(&sessionB);
    }

    /* Clear: fail closed. */
    EmeraldAudioCompat_ClearMigratedEntries();
    CHECK("after clear: count 0", EmeraldAudioCompat_GetPublishedCount() == 0u);
    CHECK("after clear: GetArena false",
          !EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize));
    CHECK("after clear: GetLeafSpan false",
          !EmeraldAudioCompat_GetLeafSpan(sLeaves[0].canonicalName, NULL, NULL));
    EmeraldAudioCompat_Shutdown();
    CHECK("shutdown idempotent", EmeraldAudioCompat_GetPublishedCount() == 0u);

    CloseSession(&session);
    CHECK("first payload above zone start (sparse, hole at zone start)",
          firstRomAddr + 0x08000000u > EMERALD_AUDIO_ROM_START);
}

/* G: failure matrix - every bad composition/pack/session combination must
 * fail closed with the arena absent. */
static void TestFailureMatrix(const char *tempDir, const char *realPackPath)
{
    struct TestSession session;
    struct EmeraldAudioCompatDiagnostics diag;
    struct Gen3ResourcePackDiagnosticList openDiag;
    enum EmeraldAudioCompatStatus status;
    char path[512];

    Gen3ResourcePackDiagnostics_Init(&openDiag);

    /* Real session (real 569 names, real payloads) for the pack-side
     * mismatches; synthetic packs reuse the real names/sizes. */
    CHECK("real session opens", OpenSession(realPackPath, &session));

    /* G2: renamed leaf - pack entry the session cannot resolve. */
    {
        snprintf(path, sizeof(path), "%s/renamed.rpack", tempDir);
        CHECK("renamed pack builds",
              BuildPackFromLeaves(path, sLeaves, sLeafCount, MutateRenameLeaf));
        {
            struct Gen3ResourcePack *syntheticPack = NULL;
            CHECK("renamed pack opens",
                  Gen3ResourcePack_OpenFile(path, &syntheticPack, &openDiag) == GEN3_PACK_OK);
            status = EmeraldAudioCompat_TryInitialize(session.snapshot, syntheticPack, &diag);
            CHECK("renamed leaf -> RESOLVE_FAILED",
                  status == EMERALD_AUDIO_ERR_RESOLVE_FAILED);
            CHECK("diagnostics name the renamed leaf",
                  strcmp(diag.canonicalName, "emerald:audio/sample/cry/abra-x") == 0);
            CHECK("diagnostics stage resolve",
                  strcmp(diag.stage, "resolve") == 0);
            CHECK("arena absent after failure",
                  EmeraldAudioCompat_GetPublishedCount() == 0u);
            Gen3ResourcePack_Destroy(syntheticPack);
        }
    }

    /* G5: wrong size - one leaf's pack payload truncated. */
    {
        snprintf(path, sizeof(path), "%s/truncated.rpack", tempDir);
        CHECK("truncated pack builds",
              BuildPackFromLeaves(path, sLeaves, sLeafCount, MutateTruncate));
        {
            struct Gen3ResourcePack *syntheticPack = NULL;
            CHECK("truncated pack opens",
                  Gen3ResourcePack_OpenFile(path, &syntheticPack, &openDiag) == GEN3_PACK_OK);
            status = EmeraldAudioCompat_TryInitialize(session.snapshot, syntheticPack, &diag);
            CHECK("truncated leaf -> PAYLOAD_SIZE_MISMATCH",
                  status == EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH);
            CHECK("arena absent after failure",
                  EmeraldAudioCompat_GetPublishedCount() == 0u);
            Gen3ResourcePack_Destroy(syntheticPack);
        }
    }

    /* G6: corrupt payload - same size, byte-flipped. */
    {
        snprintf(path, sizeof(path), "%s/corrupt.rpack", tempDir);
        CHECK("corrupt pack builds",
              BuildPackFromLeaves(path, sLeaves, sLeafCount, MutateCorrupt));
        {
            struct Gen3ResourcePack *syntheticPack = NULL;
            CHECK("corrupt pack opens",
                  Gen3ResourcePack_OpenFile(path, &syntheticPack, &openDiag) == GEN3_PACK_OK);
            status = EmeraldAudioCompat_TryInitialize(session.snapshot, syntheticPack, &diag);
            CHECK("corrupt leaf -> PAYLOAD_SIZE_MISMATCH",
                  status == EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH);
            CHECK("arena absent after failure",
                  EmeraldAudioCompat_GetPublishedCount() == 0u);
            Gen3ResourcePack_Destroy(syntheticPack);
        }
    }

    /* G7: out-of-span placement - a leaf with a bogus (early) ROM offset. */
    {
        snprintf(path, sizeof(path), "%s/offset.rpack", tempDir);
        CHECK("offset pack builds",
              BuildPackFromLeaves(path, sLeaves, sLeafCount, MutateOffset));
        {
            struct Gen3ResourcePack *syntheticPack = NULL;
            CHECK("offset pack opens",
                  Gen3ResourcePack_OpenFile(path, &syntheticPack, &openDiag) == GEN3_PACK_OK);
            status = EmeraldAudioCompat_TryInitialize(session.snapshot, syntheticPack, &diag);
            CHECK("out-of-span leaf -> PAYLOAD_SIZE_MISMATCH",
                  status == EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH);
            CHECK("arena absent after failure",
                  EmeraldAudioCompat_GetPublishedCount() == 0u);
            Gen3ResourcePack_Destroy(syntheticPack);
        }
    }

    CloseSession(&session);

    /* G1: missing leaf - a 568-leaf pack fails the composition gate (its
     * own session, since a pack-derived session is consistent by
     * construction). */
    {
        struct TestSession smallSession;
        snprintf(path, sizeof(path), "%s/missing.rpack", tempDir);
        CHECK("missing-leaf pack builds",
              BuildPackFromLeaves(path, sLeaves, sLeafCount - 1u, NULL));
        CHECK("missing-leaf session opens", OpenSession(path, &smallSession));
        status = EmeraldAudioCompat_TryInitialize(smallSession.snapshot, smallSession.pack, &diag);
        CHECK("missing leaf -> UNEXPECTED_COUNT",
              status == EMERALD_AUDIO_ERR_UNEXPECTED_COUNT);
        CHECK("arena absent after failure",
              EmeraldAudioCompat_GetPublishedCount() == 0u);
        CloseSession(&smallSession);
    }

    /* G1b: no audio at all - a pack with only non-audio entries (two dummy
     * tile-graphics leaves) fails the gate (an empty pack cannot open). */
    {
        struct TestSession emptySession;
        snprintf(path, sizeof(path), "%s/noaudio.rpack", tempDir);
        CHECK("no-audio pack builds",
              BuildPackFromLeaves(path, sLeaves, 2u, MutateAllToTiles));
        CHECK("no-audio session opens", OpenSession(path, &emptySession));
        status = EmeraldAudioCompat_TryInitialize(emptySession.snapshot, emptySession.pack, &diag);
        CHECK("no audio -> UNEXPECTED_COUNT",
              status == EMERALD_AUDIO_ERR_UNEXPECTED_COUNT);
        CloseSession(&emptySession);
    }

    /* G3: wrong type - one leaf is a tile-graphics entry (its own session:
     * the pack-derived catalog declares it consistently, so the seam's
     * type filter is the guard). */
    {
        struct TestSession wrongTypeSession;
        snprintf(path, sizeof(path), "%s/wrongtype.rpack", tempDir);
        CHECK("wrong-type pack builds",
              BuildPackFromLeaves(path, sLeaves, sLeafCount, MutateType));
        CHECK("wrong-type session opens", OpenSession(path, &wrongTypeSession));
        status = EmeraldAudioCompat_TryInitialize(wrongTypeSession.snapshot, wrongTypeSession.pack, &diag);
        CHECK("wrong-type leaf -> UNEXPECTED_COUNT",
              status == EMERALD_AUDIO_ERR_UNEXPECTED_COUNT);
        CHECK("arena absent after failure",
              EmeraldAudioCompat_GetPublishedCount() == 0u);
        CloseSession(&wrongTypeSession);
    }

    /* G4: wrong schema - one leaf with schema 2 (valid pack, wrong for the
     * seam's schema-1 contract). */
    {
        struct TestSession wrongSchemaSession;
        snprintf(path, sizeof(path), "%s/wrongschema.rpack", tempDir);
        CHECK("wrong-schema pack builds",
              BuildPackFromLeaves(path, sLeaves, sLeafCount, MutateSchema));
        CHECK("wrong-schema session opens", OpenSession(path, &wrongSchemaSession));
        status = EmeraldAudioCompat_TryInitialize(wrongSchemaSession.snapshot, wrongSchemaSession.pack, &diag);
        CHECK("wrong-schema leaf -> UNEXPECTED_COUNT",
              status == EMERALD_AUDIO_ERR_UNEXPECTED_COUNT);
        CHECK("arena absent after failure",
              EmeraldAudioCompat_GetPublishedCount() == 0u);
        CloseSession(&wrongSchemaSession);
    }

    Gen3ResourcePackDiagnostics_Destroy(&openDiag);

    /* G8: duplicate key - the pack writer itself rejects a second entry
     * with the same canonical name (GEN3_PACK_ERR_DUPLICATE_NAME). */
    {
        struct Gen3ResourcePackBuild *build = NULL;
        struct Gen3ResourcePackDiagnosticList diag8;
        struct Gen3ResourcePackProfileInput profile;
        struct Gen3ResourcePackEntryInput entry;
        Gen3ResourceKey derived;
        uint8_t sha[32];
        uint8_t provenanceSha[32];
        enum Gen3ResourcePackError packError;

        Gen3ResourcePackDiagnostics_Init(&diag8);
        build = Gen3ResourcePackBuild_Create();
        CHECK("duplicate test: build created", build != NULL);
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
            packError = Gen3ResourcePackBuild_SetProfile(build, &profile, &diag8);
            CHECK("duplicate test: profile set", packError == GEN3_PACK_OK);
            memset(provenanceSha, 0x5A, sizeof(provenanceSha));
            memset(&entry, 0, sizeof(entry));
            entry.canonicalName = sLeaves[0].canonicalName;
            Gen3ResourceId_DeriveKey(entry.canonicalName, &derived);
            entry.key = &derived;
            entry.type = GEN3_RESOURCE_TYPE_AUDIO_SAMPLE;
            entry.schema = 1u;
            entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
            entry.representation = GEN3_PACK_REPRESENTATION_RAW;
            entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_RAW;
            entry.canonicalPayload = sLeaves[0].payload;
            entry.canonicalPayloadSize = sLeaves[0].payloadSize;
            DigestSha256(sLeaves[0].payload, sLeaves[0].payloadSize, sha);
            entry.canonicalPayloadSha256 = sha;
            entry.sourceRomOffset = sLeaves[0].sourceRomOffset;
            entry.sourceEncodedSize = sLeaves[0].payloadSize;
            entry.sourceEncodedSha256 = provenanceSha;
            packError = Gen3ResourcePackBuild_AddEntry(build, &entry, &diag8);
            CHECK("duplicate test: first add OK", packError == GEN3_PACK_OK);
            packError = Gen3ResourcePackBuild_AddEntry(build, &entry, &diag8);
            CHECK("duplicate name -> GEN3_PACK_ERR_DUPLICATE_NAME",
                  packError == GEN3_PACK_ERR_DUPLICATE_NAME);
            Gen3ResourcePackBuild_Destroy(build);
        }
        Gen3ResourcePackDiagnostics_Destroy(&diag8);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *packPath;
    const char *tempDir;
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct TestSession session;

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
    if (!LoadAudioLeaves(pack))
    {
        fprintf(stderr, "FAIL: cannot load the 569 audio leaves from %s\n",
                packPath);
        Gen3ResourcePack_Destroy(pack);
        return 1;
    }

    TestAudioCounts(pack);
    TestWaveDataValidation();
    TestArenaLayout();

    /* Publication runs against a fresh session each time. */
    CHECK("session opens", OpenSession(packPath, &session));
    TestPublication(packPath);
    CloseSession(&session);

    TestFailureMatrix(tempDir, packPath);

    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    FreeAudioLeaves();

    printf("emerald audio compat test: %d checks, %d failures\n",
           sChecks, sFails);
    return sFails == 0 ? 0 : 1;
}
