/* R12-B focused test: audio leaf ownership migration (tests A-G).
 *
 * Drives the R12-B seam (emerald_audio_compat.c) against the REAL production
 * pack (games/emerald/base/emerald-bpee01-v1.rpack, 20501 entries incl. the
 * 771 audio resources: 569 leaves + 202 structural + 530 song graphs, and
 * since R13-B the 1057 movement/multiboot leaf resources) plus synthetic
 * packs for the failure matrix:
 *
 *   A. counts        - pack composition: 105 root + 51 phoneme + 388 cry
 *                      samples + 25 programmable waves = 569 leaves + 530
 *                      MP2K song graphs (mus 210 / se 269 / ph 51), all
 *                      type AUDIO_SAMPLE / schema 1 (MUSIC_SEQUENCE for the
 *                      songs), names classified by the R12-A taxonomy (pack
 *                      total 20501 since R13-E2);
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
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_rom_profile.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "platform/host_memory.h"

/* R12-D: the song block's zone-relative placement (derived, matching the
 * seam's compile-time cross-check) and the last song's block offset
 * (ph_nurse_solo_1 at 0x089A3034, 28 B, ends exactly at the block end). */
#define TEST_SONG_OFF \
    (EMERALD_AUDIO_SONG_BLOCK_START - EMERALD_AUDIO_ROM_START)
#define TEST_SONG_LAST_OFF \
    (0x089A3034u - EMERALD_AUDIO_SONG_BLOCK_START)

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
/* R10 range index (R12-C §8 forward-pull)                              */
/* ------------------------------------------------------------------ */

/* The R12-B runner links NO trainer seam (run_audio_leaf.sh), so the audio
 * seam's weak EmeraldResourceCompat_GetRangeIndex reference binds to this
 * strong definition - the same stand-in the offline parity gate uses. Test F
 * asserts the publish/republish/relocate/clear registration lifecycle. */
static struct EmeraldResourceRangeIndex sTestRangeIndex;

struct EmeraldResourceRangeIndex *EmeraldResourceCompat_GetRangeIndex(void)
{
    return &sTestRangeIndex;
}

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
/* R12-C fixtures: the 202 structural resources of the REAL pack       */
/* ------------------------------------------------------------------ */

struct AudioStructuralFixture
{
    char canonicalName[96];
    enum Gen3ResourceType type;
    uint32_t schema;
    uint8_t *payload;          /* test-owned copy */
    size_t payloadSize;
    uint64_t sourceRomOffset;  /* first-row/run offset (label + backshift) */
    uint32_t rowCount;         /* schema 1: payloadSize / 12 */
    uint32_t kind;             /* 0 voicegroup, 1 cry-table, 2 keysplit */
    uint32_t cryReverse;       /* cry-table only */
};

static struct AudioStructuralFixture
    sStructural[EMERALD_AUDIO_STRUCTURAL_COUNT];
static size_t sStructuralCount = 0;
static size_t sVoicegroupCount = 0, sCryTableCount = 0, sKeysplitCount = 0;
static size_t sStreamRows = 0, sKeysplitRunBytes = 0;

static int ClassifyStructural(const char *name)
{
    if (strncmp(name, "emerald:audio/voicegroup/",
                sizeof("emerald:audio/voicegroup/") - 1u) == 0)
        return 0;
    if (strncmp(name, "emerald:audio/cry-table/",
                sizeof("emerald:audio/cry-table/") - 1u) == 0)
        return 1;
    if (strncmp(name, "emerald:audio/keysplit/",
                sizeof("emerald:audio/keysplit/") - 1u) == 0)
        return 2;
    return -1;
}

static bool LoadStructuralFixtures(const struct Gen3ResourcePack *pack)
{
    size_t i, n = Gen3ResourcePack_GetEntryCount(pack);
    size_t count = 0;

    for (i = 0; i < n; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        struct AudioStructuralFixture *fix;
        int kind;

        if (entry->type != GEN3_RESOURCE_TYPE_INSTRUMENT_BANK)
            continue;
        if (count >= EMERALD_AUDIO_STRUCTURAL_COUNT)
            return false;
        kind = ClassifyStructural(entry->canonicalName);
        if (kind < 0 || entry->payload == NULL || entry->payloadSize == 0u)
            return false;
        fix = &sStructural[count];
        memset(fix, 0, sizeof(*fix));
        snprintf(fix->canonicalName, sizeof(fix->canonicalName), "%s",
                 entry->canonicalName);
        fix->type = entry->type;
        fix->schema = entry->schema;
        fix->payload = (uint8_t *)malloc(entry->payloadSize);
        if (fix->payload == NULL)
            return false;
        memcpy(fix->payload, entry->payload, entry->payloadSize);
        fix->payloadSize = entry->payloadSize;
        fix->sourceRomOffset = entry->sourceRomOffset;
        fix->kind = (uint32_t)kind;
        if (kind == 1 && entry->schema == 1u)
            fix->cryReverse =
                strcmp(entry->canonicalName, "emerald:audio/cry-table/reverse") == 0;
        if (entry->schema == 1u)
        {
            fix->rowCount = (uint32_t)(entry->payloadSize / 12u);
            sStreamRows += fix->rowCount;
            if (kind == 0)
                sVoicegroupCount++;
            else
                sCryTableCount++;
        }
        else
        {
            sKeysplitRunBytes += entry->payloadSize;
            sKeysplitCount++;
        }
        count++;
    }
    sStructuralCount = count;
    return count == EMERALD_AUDIO_STRUCTURAL_COUNT;
}

static void FreeStructuralFixtures(void)
{
    size_t i;
    for (i = 0; i < sStructuralCount; i++)
    {
        free(sStructural[i].payload);
        sStructural[i].payload = NULL;
    }
    sStructuralCount = 0;
}

/* ------------------------------------------------------------------ */
/* R12-D fixtures: the 530 MP2K song graphs of the REAL pack           */
/* ------------------------------------------------------------------ */

struct AudioSongFixture
{
    char canonicalName[96];
    enum Gen3ResourceType type;
    uint32_t schema;
    uint8_t *payload;          /* test-owned copy */
    size_t payloadSize;
    uint64_t sourceRomOffset;
    Gen3ResourceKey key;       /* value copy from the pack entry */
};

static struct AudioSongFixture sSongs[EMERALD_AUDIO_SONG_COUNT];
static size_t sSongCount = 0;

static bool LoadAudioSongs(const struct Gen3ResourcePack *pack)
{
    size_t i, n = Gen3ResourcePack_GetEntryCount(pack);
    size_t count = 0;

    for (i = 0; i < n; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        struct AudioSongFixture *fix;

        if (entry->type != GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE
         || entry->schema != 1u)
            continue;
        if (count >= EMERALD_AUDIO_SONG_COUNT)
            return false;
        if (strncmp(entry->canonicalName, "emerald:audio/song/",
                    sizeof("emerald:audio/song/") - 1u) != 0
         || entry->payload == NULL || entry->payloadSize == 0u)
            return false;
        fix = &sSongs[count];
        memset(fix, 0, sizeof(*fix));
        snprintf(fix->canonicalName, sizeof(fix->canonicalName), "%s",
                 entry->canonicalName);
        fix->type = entry->type;
        fix->schema = entry->schema;
        fix->payload = (uint8_t *)malloc(entry->payloadSize);
        if (fix->payload == NULL)
            return false;
        memcpy(fix->payload, entry->payload, entry->payloadSize);
        fix->payloadSize = entry->payloadSize;
        fix->sourceRomOffset = entry->sourceRomOffset;
        fix->key = entry->key;
        count++;
    }
    sSongCount = count;
    return count == EMERALD_AUDIO_SONG_COUNT;
}

static void FreeAudioSongs(void)
{
    size_t i;
    for (i = 0; i < sSongCount; i++)
    {
        free(sSongs[i].payload);
        sSongs[i].payload = NULL;
    }
    sSongCount = 0;
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

/* Shared entry writer for the R12-D full-audio pack builder (the same
 * deterministic synthetic identity + entry fill as BuildPackFromLeaves). */
static bool AddPackEntry(struct Gen3ResourcePackBuild *build,
                         const char *canonicalName,
                         enum Gen3ResourceType type, uint32_t schema,
                         const uint8_t *payload, size_t payloadSize,
                         uint64_t sourceRomOffset,
                         struct Gen3ResourcePackDiagnosticList *diag)
{
    struct Gen3ResourcePackEntryInput entry;
    Gen3ResourceKey key;
    uint8_t sha[32];
    uint8_t provenanceSha[32];

    memset(&entry, 0, sizeof(entry));
    Gen3ResourceId_DeriveKey(canonicalName, &key);
    DigestSha256(payload, payloadSize, sha);
    memset(provenanceSha, 0x5A, sizeof(provenanceSha));
    entry.canonicalName = canonicalName;
    entry.key = &key;
    entry.type = type;
    entry.schema = schema;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_RAW;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_RAW;
    entry.canonicalPayload = payload;
    entry.canonicalPayloadSize = payloadSize;
    entry.canonicalPayloadSha256 = sha;
    entry.sourceRomOffset = sourceRomOffset;
    entry.sourceEncodedSize = payloadSize;
    entry.sourceEncodedSha256 = provenanceSha;
    return Gen3ResourcePackBuild_AddEntry(build, &entry, diag) == GEN3_PACK_OK;
}

/* R12-D full-audio pack builder: the REAL 569 leaves + the REAL 202
 * structurals + the first `songCount` REAL songs (0..530). Fixture payloads
 * are COPIED before the mutate hooks run, so a damaged pack never corrupts
 * the shared fixture sets. Used by the song-graph failure cases, where the
 * seam must see a complete audio family with exactly ONE damaged song. */
static bool BuildFullAudioPack(const char *path, size_t songCount,
                               void (*mutateLeaf)(struct AudioLeafFixture *fix,
                                                  size_t index),
                               void (*mutateSong)(struct AudioSongFixture *fix,
                                                  size_t index),
                               size_t structuralCount,
                               size_t skipStructuralIndex,
                               void (*mutateStructural)(struct AudioStructuralFixture *fix,
                                                        size_t index))
{
    struct Gen3ResourcePackBuild *build = NULL;
    struct Gen3ResourcePackBytes bytes = { NULL, 0 };
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackProfileInput profile;
    struct AudioLeafFixture *leafFixes = NULL;
    struct AudioSongFixture *songFixes = NULL;
    struct AudioStructuralFixture *structuralFixes = NULL;
    uint8_t provenanceSha[32];
    FILE *f = NULL;
    size_t i;
    size_t structIndex = 0;
    enum Gen3ResourcePackError packError;
    bool ok = false;

    Gen3ResourcePackDiagnostics_Init(&diag);
    build = Gen3ResourcePackBuild_Create();
    if (build == NULL)
        goto done;
    if (songCount > sSongCount)
        goto done;
    leafFixes = (struct AudioLeafFixture *)calloc(sLeafCount,
                                                  sizeof(*leafFixes));
    if (leafFixes == NULL)
        goto done;
    for (i = 0; i < sLeafCount; i++)
    {
        leafFixes[i] = sLeaves[i];
        leafFixes[i].payload = (uint8_t *)malloc(sLeaves[i].payloadSize);
        if (leafFixes[i].payload == NULL)
            goto done;
        memcpy(leafFixes[i].payload, sLeaves[i].payload, sLeaves[i].payloadSize);
        if (mutateLeaf != NULL)
            mutateLeaf(&leafFixes[i], i);
    }
    songFixes = (struct AudioSongFixture *)calloc(songCount,
                                                  sizeof(*songFixes));
    if (songFixes == NULL)
        goto done;
    for (i = 0; i < songCount; i++)
    {
        songFixes[i] = sSongs[i];
        songFixes[i].payload = (uint8_t *)malloc(sSongs[i].payloadSize);
        if (songFixes[i].payload == NULL)
            goto done;
        memcpy(songFixes[i].payload, sSongs[i].payload, sSongs[i].payloadSize);
        if (mutateSong != NULL)
            mutateSong(&songFixes[i], i);
    }
    /* R12-E failure matrix: the structural family is now mutable too (S7/S8).
     * `skipStructuralIndex` drops one fixture (SIZE_MAX keeps all);
     * `mutateStructural` damages a copy in place. */
    if (structuralCount > sStructuralCount)
        goto done;
    structuralFixes = (struct AudioStructuralFixture *)calloc(
        structuralCount, sizeof(*structuralFixes));
    if (structuralFixes == NULL)
        goto done;
    for (i = 0; i < sStructuralCount && structIndex < structuralCount; i++)
    {
        if (i == skipStructuralIndex)
            continue;
        structuralFixes[structIndex] = sStructural[i];
        structuralFixes[structIndex].payload =
            (uint8_t *)malloc(sStructural[i].payloadSize);
        if (structuralFixes[structIndex].payload == NULL)
            goto done;
        memcpy(structuralFixes[structIndex].payload, sStructural[i].payload,
               sStructural[i].payloadSize);
        if (mutateStructural != NULL)
            mutateStructural(&structuralFixes[structIndex], structIndex);
        structIndex++;
    }
    if (structIndex != structuralCount)
        goto done;

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

    for (i = 0; i < sLeafCount; i++)
    {
        if (!AddPackEntry(build, leafFixes[i].canonicalName, leafFixes[i].type,
                          leafFixes[i].schema, leafFixes[i].payload,
                          leafFixes[i].payloadSize, leafFixes[i].sourceRomOffset,
                          &diag))
            goto done;
    }
    for (i = 0; i < structuralCount; i++)
    {
        if (!AddPackEntry(build, structuralFixes[i].canonicalName,
                          structuralFixes[i].type, structuralFixes[i].schema,
                          structuralFixes[i].payload,
                          structuralFixes[i].payloadSize,
                          structuralFixes[i].sourceRomOffset, &diag))
            goto done;
    }
    for (i = 0; i < songCount; i++)
    {
        if (!AddPackEntry(build, songFixes[i].canonicalName, songFixes[i].type,
                          songFixes[i].schema, songFixes[i].payload,
                          songFixes[i].payloadSize, songFixes[i].sourceRomOffset,
                          &diag))
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
    if (leafFixes != NULL)
    {
        for (i = 0; i < sLeafCount; i++)
            free(leafFixes[i].payload);
        free(leafFixes);
    }
    if (songFixes != NULL)
    {
        for (i = 0; i < songCount; i++)
            free(songFixes[i].payload);
        free(songFixes);
    }
    if (structuralFixes != NULL)
    {
        for (i = 0; i < structuralCount; i++)
            free(structuralFixes[i].payload);
        free(structuralFixes);
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

/* The R12-D song-graph mutate hooks: each damages exactly ONE song of the
 * real 530-song fixture set - emerald:audio/song/mus-route101 - except the
 * overlap case, which moves emerald:audio/wave/programmable/17 (provably
 * referenced by NO structural row; a referenced leaf would trip phase 1b's
 * row-pointer resolution first, not phase 1c's disjointness proof). */
static bool IsRoute101Song(const struct AudioSongFixture *fix)
{
    return strcmp(fix->canonicalName, "emerald:audio/song/mus-route101") == 0;
}
static bool IsWave17(const struct AudioLeafFixture *fix)
{
    return strcmp(fix->canonicalName, "emerald:audio/wave/programmable/17") == 0;
}
static void MutateSongRename(struct AudioSongFixture *fix, size_t index)
{
    (void)index;
    if (IsRoute101Song(fix))
        strcpy(fix->canonicalName, "emerald:audio/song/mus-route101-x");
}
static void MutateSongSchema(struct AudioSongFixture *fix, size_t index)
{
    (void)index;
    if (IsRoute101Song(fix))
        fix->schema = 2u;
}
static void MutateSongOffset(struct AudioSongFixture *fix, size_t index)
{
    (void)index;
    /* A 0x10 shift inside the block: sizes still match the session (the
     * size/bytes checks pass), the placement is still in-block (bounds
     * passes), but the tiling proof then fails closed. A truncated song
     * would be caught EARLIER by the view-vs-entry size check. */
    if (IsRoute101Song(fix))
        fix->sourceRomOffset += 0x10u;
}
static void MutateSongEarlyOffset(struct AudioSongFixture *fix, size_t index)
{
    (void)index;
    if (IsRoute101Song(fix))
        fix->sourceRomOffset = 0x00012345u; /* below the audio section */
}
static void MutateWaveIntoSongBlock(struct AudioLeafFixture *fix, size_t index)
{
    (void)index;
    if (IsWave17(fix))
        fix->sourceRomOffset = 0x008FC04Bu; /* 15 B into the song block */
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
    /* R13-D1: the production pack entry count grew to 15,373 (12,063 after
     * R13-C + 3,310 D1 gameplay/font resources; the diverged evolution
     * family is excluded). The audio-family pins below are unchanged. */
    CHECK("pack total 20501 entries (+786 R13-E3a-1 frontier/tent)",
          Gen3ResourcePack_GetEntryCount(pack) == 20501u);
    CHECK("leaf total 569", sLeafCount == EMERALD_AUDIO_LEAF_COUNT);
    CHECK("root count 105", sRootCount == EMERALD_AUDIO_ROOT_COUNT);
    CHECK("phoneme count 51", sPhonemeCount == EMERALD_AUDIO_PHONEME_COUNT);
    CHECK("cry count 388", sCryCount == EMERALD_AUDIO_CRY_COUNT);
    CHECK("wave count 25", sWaveCount == EMERALD_AUDIO_WAVE_COUNT);
    CHECK("macro LEAF_COUNT == 569",
          EMERALD_AUDIO_LEAF_COUNT == 105u + 51u + 388u + 25u);
    CHECK("song count 530", sSongCount == EMERALD_AUDIO_SONG_COUNT);
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

/* R12-C counts: pack composition pins for the structural family. */
static void TestStructuralCounts(void)
{
    size_t i;

    CHECK("structural total 202", sStructuralCount == EMERALD_AUDIO_STRUCTURAL_COUNT);
    CHECK("voicegroup count 195", sVoicegroupCount == EMERALD_AUDIO_VOICEGROUP_COUNT);
    CHECK("cry-table count 2", sCryTableCount == EMERALD_AUDIO_CRY_TABLE_COUNT);
    CHECK("keysplit count 5", sKeysplitCount == EMERALD_AUDIO_KEYSPLIT_COUNT);
    CHECK("stream rows 21370", sStreamRows == EMERALD_AUDIO_STREAM_ROWS);
    CHECK("keysplit run bytes 372", sKeysplitRunBytes == 372u);
    for (i = 0; i < sStructuralCount; i++)
    {
        const struct AudioStructuralFixture *fix = &sStructural[i];
        if (fix->schema == 1u)
            CHECK("schema-1 payload is whole 12-byte rows",
                  fix->payloadSize % 12u == 0u && fix->rowCount > 0u);
        else
            CHECK("schema-2 payload is a keysplit run",
                  fix->schema == 2u && fix->kind == 2u);
    }
    /* Known anchors: forward cry table = 388 rows; reverse too. */
    for (i = 0; i < sStructuralCount; i++)
    {
        const struct AudioStructuralFixture *fix = &sStructural[i];
        if (fix->kind == 1u)
            CHECK("cry table rows 388", fix->rowCount == 388u);
    }
}

struct LabelCollector
{
    uint32_t addrs[197];
    const uint8_t *bases[197];
    size_t count;
};

static void CollectLabel(uint32_t gbaAddr, const void *hostBase, void *user)
{
    struct LabelCollector *c = (struct LabelCollector *)user;
    if (c->count < 197u)
    {
        c->addrs[c->count] = gbaAddr;
        c->bases[c->count] = (const uint8_t *)hostBase;
        c->count++;
    }
}

/* R12-C structural publication: composition gate, transformed zone,
 * per-resource spans, cry accessor, logical-label enumeration. */
static void TestStructuralPublication(void)
{
    const uint8_t *transformBase = NULL;
    size_t transformSize = 0;
    size_t i;
    size_t cryFwd = 0, cryRev = 0;

    CHECK("structural count 202 (published)",
          EmeraldAudioCompat_GetStructuralCount() == EMERALD_AUDIO_STRUCTURAL_COUNT);
    CHECK("transformed rows published",
          EmeraldAudioCompat_GetTransformedRows(&transformBase, &transformSize));
    CHECK("transformed zone size pinned",
          transformSize == EMERALD_AUDIO_TRANSFORM_SIZE);

    /* Per-resource spans against the pack fixtures. */
    for (i = 0; i < sStructuralCount; i++)
    {
        const struct AudioStructuralFixture *fix = &sStructural[i];
        if (fix->kind == 0)
        {
            size_t off = 0, rows = 0;
            CHECK("voicegroup span resolves",
                  EmeraldAudioCompat_GetVoicegroupSpan(fix->canonicalName,
                                                       &off, &rows));
            CHECK("voicegroup rows match payload", rows == fix->rowCount);
            CHECK("voicegroup block inside transform zone",
                  off + rows * 24u <= transformSize);
        }
        else if (fix->kind == 1)
        {
            size_t off = 0, rows = 0;
            CHECK("cry span resolves",
                  EmeraldAudioCompat_GetCryTableSpan(fix->cryReverse != 0u,
                                                     &off, &rows));
            CHECK("cry rows 388", rows == fix->rowCount);
            if (fix->cryReverse)
                cryRev = rows;
            else
                cryFwd = rows;
        }
        else
        {
            size_t off = 0, bytes = 0;
            const uint8_t *zoneBase = NULL;
            size_t zoneSize = 0;
            uint64_t romAddr = fix->sourceRomOffset + 0x08000000u;
            CHECK("keysplit span resolves",
                  EmeraldAudioCompat_GetKeysplitSpan(fix->canonicalName,
                                                     &off, &bytes));
            CHECK("keysplit bytes match payload", bytes == fix->payloadSize);
            CHECK("keysplit copy at ROM-relative offset",
                  off == (size_t)(romAddr - EMERALD_AUDIO_ROM_START));
            CHECK("keysplit copy inside verbatim zone",
                  off + bytes <= EMERALD_AUDIO_SPAN_SIZE);
            CHECK("zone published", EmeraldAudioCompat_GetArena(&zoneBase, &zoneSize));
            CHECK("keysplit bytes == pack payload",
                  zoneBase != NULL
                  && memcmp(zoneBase + off, fix->payload, fix->payloadSize) == 0);
        }
    }
    CHECK("cry forward rows 388", cryFwd == 388u);
    CHECK("cry reverse rows 388", cryRev == 388u);

    /* Drumset back-shift pads: rs-drumset is 29 rows with a 36-row pad. */
    {
        size_t off = 0, rows = 0;
        const uint8_t *base = NULL;
        size_t size = 0;
        size_t k;
        bool padZero = true;
        CHECK("rs-drumset span resolves",
              EmeraldAudioCompat_GetVoicegroupSpan(
                  "emerald:audio/voicegroup/rs-drumset", &off, &rows));
        CHECK("rs-drumset rows 29", rows == 29u);
        CHECK("transformed rows published",
              EmeraldAudioCompat_GetTransformedRows(&base, &size));
        for (k = 0; k < 36u * 24u; k++)
        {
            if (base[off + k] != 0u)
                padZero = false;
        }
        CHECK("rs-drumset pad zeroed (36 rows x 24)", padZero);
        /* The label points at the pad start: a back-shifted group pointer
         * dereferences the pad (mapped zeroed memory), never another
         * table's rows. */
        CHECK("rs-drumset pad before rows",
              off + 36u * 24u + rows * 24u <= size);
    }

    /* Cry accessor: row indexing and bounds. */
    CHECK("cry row 0 forward non-NULL",
          EmeraldAudioCryTableRow(0u, false, 0u) != NULL);
    CHECK("cry row 0 reverse non-NULL",
          EmeraldAudioCryTableRow(0u, true, 0u) != NULL);
    CHECK("cry rows differ forward/reverse",
          EmeraldAudioCryTableRow(0u, false, 0u)
              != EmeraldAudioCryTableRow(0u, true, 0u));
    CHECK("cry row at bank 3 index 3 non-NULL",
          EmeraldAudioCryTableRow(3u, false, 3u) != NULL);
    CHECK("cry row beyond 388 is NULL",
          EmeraldAudioCryTableRow(3u, false, 4u) == NULL);
    CHECK("cry row bank 4 is NULL",
          EmeraldAudioCryTableRow(4u, false, 0u) == NULL);

    /* Logical-label enumeration: 195 voicegroups + 2 cry tables = 197
     * unique GBA addresses, host bases inside the transformed zone. */
    {
        struct LabelCollector collector;
        size_t a, b;
        bool unique = true;
        bool inZone = true;
        memset(&collector, 0, sizeof(collector));
        EmeraldAudioCompat_ForEachLogicalLabel(CollectLabel, &collector);
        CHECK("label count 197", collector.count == 197u);
        for (a = 0; a < collector.count; a++)
        {
            for (b = a + 1; b < collector.count; b++)
            {
                if (collector.addrs[a] == collector.addrs[b])
                    unique = false;
            }
            if (collector.bases[a] < transformBase
             || collector.bases[a] >= transformBase + transformSize)
                inZone = false;
        }
        CHECK("labels unique", unique);
        CHECK("labels inside transformed zone", inZone);
    }
}

/* R12-F §4/§7: assert the R10 registration state of a published arena.
 * `arenaBase` is the current GetArena base (verbatim zone start); the
 * checks cover the 1,301 per-resource ranges - a leaf payload (CANONICAL
 * AUDIO_SAMPLE/1, offset 0 and mid-leaf), the last leaf byte (the byte
 * before the song block), ALL 5 keysplit ranges (base = the mks4agb LABEL,
 * INSTRUMENT_BANK/schema 2/CANONICAL, offset 0 at the label; the run start
 * resolves at offset == backshift; the five ranges tile
 * [firstLabel, lastRunEnd) exactly), the 530 per-song canonical
 * MUSIC_SEQUENCE spans (block start, mid-song, last song, last block byte;
 * the unbacked zone tail must MISS), a transformed cry row (COMPAT_OBJECT
 * INSTRUMENT_BANK), the load-direction ResolveByKey round trips, hull A +
 * hull B coverage with the negative probes (zone tail, arena prefix,
 * transform-align gap - all unhulled), and the R12-D HostResolveGbaAddr
 * interval (identity-preserving inside the block, direct cast outside it).
 * The synthetic emerald:audio/verbatim-zone identity is GONE: no probe may
 * resolve under it. */
static void CheckRangeRegistration(const char *tag, const uint8_t *arenaBase,
                                   size_t expectRanges, size_t expectHulls)
{
    struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    struct EmeraldResourceRangeHit hit;
    Gen3ResourceKey cryKey;
    uintptr_t resolved = 0;
    size_t zoneOff = 0, transformOff = 0, spanSize = 0, transformSize = 0;
    size_t cryTo = 0, cryRows = 0;
    uintptr_t transformBase = 0, cryProbe = 0;
    bool layoutOk, cryOk, cryHit, resolveOk;

    CHECK("index present", index != NULL);
    if (index == NULL)
        return;
    CHECK("range count", EmeraldResourceRangeIndex_GetRangeCount(index)
                             == expectRanges);
    CHECK("hull count", index->hullCount == expectHulls);
    Gen3ResourceId_DeriveKey("emerald:audio/cry-table/forward", &cryKey);

    layoutOk = EmeraldAudioCompat_GetArenaLayout(&zoneOff, &transformOff,
                                                 &spanSize, &transformSize);
    cryOk = EmeraldAudioCompat_GetCryTableSpan(false, &cryTo, &cryRows);
    CHECK("layout accessor", layoutOk);
    CHECK("cry span accessor", cryOk);
    if (layoutOk && cryOk)
    {
        transformBase = (uintptr_t)arenaBase + (transformOff - zoneOff);
        cryProbe = transformBase + cryTo + 12u;
    }

    /* A leaf payload: CANONICAL AUDIO_SAMPLE/1, offset 0 at its run start
     * and a mid-payload probe. The synthetic verbatim-zone key is gone -
     * the leaf's own key resolves. */
    {
        Gen3ResourceKey leafKey;
        bool leafHit = sLeafCount > 0u;
        Gen3ResourceId_DeriveKey(sLeaves[0].canonicalName, &leafKey);
        leafHit = leafHit
               && EmeraldResourceRangeIndex_Lookup(
                      index, (uintptr_t)arenaBase + sLeaves[0].arenaOffset,
                      &hit);
        CHECK("leaf hit", leafHit);
        if (leafHit)
        {
            CHECK("leaf key",
                  Gen3ResourceId_KeyEqual(&hit.key, &leafKey));
            CHECK("leaf type", hit.type == GEN3_RESOURCE_TYPE_AUDIO_SAMPLE);
            CHECK("leaf schema", hit.schema == 1u);
            CHECK("leaf role", hit.role == EMERALD_RESOURCE_ROLE_CANONICAL);
            CHECK("leaf offset 0", hit.rangeOffset == 0u);
        }
        leafHit = sLeafCount > 0u
               && EmeraldResourceRangeIndex_Lookup(
                      index,
                      (uintptr_t)arenaBase + sLeaves[0].arenaOffset + 4u,
                      &hit);
        CHECK("mid-leaf hit", leafHit);
        if (leafHit)
        {
            CHECK("mid-leaf key",
                  Gen3ResourceId_KeyEqual(&hit.key, &leafKey));
            CHECK("mid-leaf offset", hit.rangeOffset == 4u);
        }
    }
    /* The sub-zone's last byte (the last leaf ends 0x088FC03B, one byte
     * before the song block) resolves under the LAST LEAF's key. */
    {
        const struct AudioLeafFixture *lastLeaf = NULL;
        Gen3ResourceKey lastKey;
        bool lastHit = false;
        size_t li;
        for (li = 0; li < sLeafCount; li++)
        {
            if ((size_t)sLeaves[li].arenaOffset + sLeaves[li].payloadSize
                == TEST_SONG_OFF)
            {
                lastLeaf = &sLeaves[li];
                break;
            }
        }
        if (lastLeaf != NULL)
        {
            Gen3ResourceId_DeriveKey(lastLeaf->canonicalName, &lastKey);
            lastHit = EmeraldResourceRangeIndex_Lookup(
                index, (uintptr_t)arenaBase + TEST_SONG_OFF - 1u, &hit);
            CHECK("last leaf byte hit", lastHit);
            if (lastHit)
            {
                CHECK("last leaf byte key",
                      Gen3ResourceId_KeyEqual(&hit.key, &lastKey));
                CHECK("last leaf byte type",
                      hit.type == GEN3_RESOURCE_TYPE_AUDIO_SAMPLE);
                CHECK("last leaf byte offset",
                      hit.rangeOffset == lastLeaf->payloadSize - 1u);
            }
        }
    }
    /* All 5 keysplit ranges (R12-F §4): base at the mks4agb LABEL (the live
     * track->tone.keySplitTable pointer target) - INSTRUMENT_BANK/schema
     * 2/CANONICAL, offset 0 at the label, offset == backshift at the run
     * start, and the five ranges TILE [firstLabel, lastRunEnd) exactly (the
     * next table's label is the previous range's end byte). The pinned
     * offsets come from the reference tree's sound/keysplit_tables.inc and
     * the ELF symbol addresses (KeySplitTable1..5); the run starts and
     * lengths are cross-checked against the pack fixtures below. */
    {
        static const struct
        {
            const char *canonicalName;
            uint32_t labelOff;   /* zone offset of the mks4agb label */
            uint32_t runOff;     /* zone offset of the run start */
            uint32_t runLen;     /* run length (= .inc entry count) */
            uint32_t rangeLen;   /* label-tiled range length */
        } pins[EMERALD_AUDIO_KEYSPLIT_COUNT] =
        {
            {"emerald:audio/keysplit/piano",       0x3D5FCu, 0x3D620u, 72u, 72u},
            {"emerald:audio/keysplit/strings",     0x3D644u, 0x3D668u, 72u, 72u},
            {"emerald:audio/keysplit/trumpet",     0x3D68Cu, 0x3D6B0u, 72u, 84u},
            {"emerald:audio/keysplit/tuba",        0x3D6E0u, 0x3D6F8u, 84u, 72u},
            {"emerald:audio/keysplit/french-horn", 0x3D728u, 0x3D74Cu, 72u, 108u},
        };
        size_t pi;
        for (pi = 0; pi < EMERALD_AUDIO_KEYSPLIT_COUNT; pi++)
        {
            const struct AudioStructuralFixture *fix = NULL;
            Gen3ResourceKey ksKey;
            bool ksHit;
            size_t fi;
            for (fi = 0; fi < sStructuralCount; fi++)
            {
                if (sStructural[fi].kind == 2u
                 && strcmp(sStructural[fi].canonicalName,
                           pins[pi].canonicalName) == 0)
                {
                    fix = &sStructural[fi];
                    break;
                }
            }
            CHECK("keysplit fixture present", fix != NULL);
            if (fix == NULL)
                continue;
            Gen3ResourceId_DeriveKey(fix->canonicalName, &ksKey);

            /* The label: offset 0, the pointer's natural resolution. */
            ksHit = EmeraldResourceRangeIndex_Lookup(
                index, (uintptr_t)arenaBase + pins[pi].labelOff, &hit);
            CHECK("keysplit label hit", ksHit);
            if (ksHit)
            {
                CHECK("keysplit label key",
                      Gen3ResourceId_KeyEqual(&hit.key, &ksKey));
                CHECK("keysplit label type",
                      hit.type == GEN3_RESOURCE_TYPE_INSTRUMENT_BANK);
                CHECK("keysplit label schema", hit.schema == 2u);
                CHECK("keysplit label role",
                      hit.role == EMERALD_RESOURCE_ROLE_CANONICAL);
                CHECK("keysplit label offset 0", hit.rangeOffset == 0u);
            }
            /* The run start (the first mapped note): offset == backshift. */
            ksHit = EmeraldResourceRangeIndex_Lookup(
                index, (uintptr_t)arenaBase + pins[pi].runOff, &hit);
            CHECK("keysplit run-start hit", ksHit);
            if (ksHit)
            {
                CHECK("keysplit run-start key",
                      Gen3ResourceId_KeyEqual(&hit.key, &ksKey));
                CHECK("keysplit run-start offset",
                      hit.rangeOffset == pins[pi].runOff - pins[pi].labelOff);
            }
            /* The range's last byte - the byte before the next label. */
            ksHit = EmeraldResourceRangeIndex_Lookup(
                index,
                (uintptr_t)arenaBase + pins[pi].labelOff
                    + pins[pi].rangeLen - 1u,
                &hit);
            CHECK("keysplit last byte hit", ksHit);
            if (ksHit)
            {
                CHECK("keysplit last byte key",
                      Gen3ResourceId_KeyEqual(&hit.key, &ksKey));
                CHECK("keysplit last byte offset",
                      hit.rangeOffset == pins[pi].rangeLen - 1u);
            }
            /* Pack cross-check: the fixture run offset and payload length
             * agree with the pinned geometry. */
            CHECK("keysplit fixture run offset",
                  fix->sourceRomOffset + 0x08000000u - EMERALD_AUDIO_ROM_START
                      == pins[pi].runOff);
            CHECK("keysplit fixture run length",
                  fix->payloadSize == pins[pi].runLen);
        }
        /* The tiling is exact: each range ends at the next label, and the
         * last ends at the last run end (0x3D794 = 0x3D74C + 72). */
        for (pi = 0; pi + 1u < EMERALD_AUDIO_KEYSPLIT_COUNT; pi++)
            CHECK("keysplit ranges tile (end == next label)",
                  pins[pi].labelOff + pins[pi].rangeLen
                      == pins[pi + 1u].labelOff);
        CHECK("keysplit last range ends at last run end",
              pins[4u].labelOff + pins[4u].rangeLen == 0x3D794u);
    }

    /* R12-D: the 530 per-song CANONICAL MUSIC_SEQUENCE spans tile the song
     * block. Probes: block start (mus_dummy, 8 B), mid-first-song, the
     * last song (ph_nurse_solo, 28 B), the block's final byte, and the
     * zone tail (unbacked - must MISS, fail-closed). */
    {
        Gen3ResourceKey songDummyKey, songSoloKey;
        bool firstSongHit, midSongHit, lastSongHit, lastByteHit;
        bool tailMiss, resolveOk2;

        Gen3ResourceId_DeriveKey("emerald:audio/song/mus-dummy",
                                 &songDummyKey);
        Gen3ResourceId_DeriveKey("emerald:audio/song/ph-nurse-solo",
                                 &songSoloKey);
        firstSongHit = EmeraldResourceRangeIndex_Lookup(
            index, (uintptr_t)arenaBase + TEST_SONG_OFF, &hit);
        CHECK("first song hit", firstSongHit);
        if (firstSongHit)
        {
            CHECK("first song key",
                  Gen3ResourceId_KeyEqual(&hit.key, &songDummyKey));
            CHECK("first song type",
                  hit.type == GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE);
            CHECK("first song role",
                  hit.role == EMERALD_RESOURCE_ROLE_CANONICAL);
            CHECK("first song offset", hit.rangeOffset == 0u);
        }
        midSongHit = EmeraldResourceRangeIndex_Lookup(
            index, (uintptr_t)arenaBase + TEST_SONG_OFF + 4u, &hit);
        CHECK("mid first song hit", midSongHit);
        if (midSongHit)
        {
            CHECK("mid first song key",
                  Gen3ResourceId_KeyEqual(&hit.key, &songDummyKey));
            CHECK("mid first song offset", hit.rangeOffset == 4u);
        }
        lastSongHit = EmeraldResourceRangeIndex_Lookup(
            index, (uintptr_t)arenaBase + TEST_SONG_OFF + TEST_SONG_LAST_OFF,
            &hit);
        CHECK("last song hit", lastSongHit);
        if (lastSongHit)
        {
            CHECK("last song key",
                  Gen3ResourceId_KeyEqual(&hit.key, &songSoloKey));
            CHECK("last song type",
                  hit.type == GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE);
            CHECK("last song offset", hit.rangeOffset == 0u);
        }
        lastByteHit = EmeraldResourceRangeIndex_Lookup(
            index,
            (uintptr_t)arenaBase + TEST_SONG_OFF + EMERALD_AUDIO_SONG_BLOCK_SIZE
                - 1u,
            &hit);
        CHECK("last block byte hit", lastByteHit);
        if (lastByteHit)
        {
            CHECK("last block byte type",
                  hit.type == GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE);
        }
        /* Zone tail [SONG_BLOCK_END, SPAN_END): 3,428 unbacked bytes. */
        tailMiss = !EmeraldResourceRangeIndex_Lookup(
            index,
            (uintptr_t)arenaBase + TEST_SONG_OFF + EMERALD_AUDIO_SONG_BLOCK_SIZE,
            &hit);
        CHECK("zone tail unbacked", tailMiss);

        /* The R12-D §5 HostResolveGbaAddr interval: identity-preserving
         * inside the block, exact direct cast outside it. */
        CHECK("interval block start",
              HostResolveGbaAddr(EMERALD_AUDIO_SONG_BLOCK_START)
                  == (void *)(uintptr_t)(arenaBase + TEST_SONG_OFF));
        CHECK("interval mid-song",
              HostResolveGbaAddr(0x089A3034u + 10u)
                  == (void *)(uintptr_t)(arenaBase + TEST_SONG_OFF
                                         + TEST_SONG_LAST_OFF + 10u));
        CHECK("interval past end falls through",
              HostResolveGbaAddr(EMERALD_AUDIO_SONG_BLOCK_END)
                  != (void *)(uintptr_t)(arenaBase + TEST_SONG_OFF
                                         + EMERALD_AUDIO_SONG_BLOCK_SIZE));
        CHECK("pre-block direct cast",
              HostResolveGbaAddr(EMERALD_AUDIO_SONG_BLOCK_START - 1u)
                  == (void *)(uintptr_t)(EMERALD_AUDIO_SONG_BLOCK_START - 1u));
        resolveOk2 = EmeraldResourceRangeIndex_ResolveByKey(
            index, &songDummyKey, GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, 1u,
            EMERALD_RESOURCE_ROLE_CANONICAL, 4u, &resolved);
        CHECK("resolve song by key", resolveOk2);
        CHECK("resolved song == mid probe",
              resolveOk2 && resolved == (uintptr_t)arenaBase + TEST_SONG_OFF + 4u);
        /* Boundary: the last song's final byte (ph_nurse_solo, 28 B) - the
         * range's last resolvable offset before the unbacked tail. */
        resolveOk2 = EmeraldResourceRangeIndex_ResolveByKey(
            index, &songSoloKey, GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, 1u,
            EMERALD_RESOURCE_ROLE_CANONICAL, 27u, &resolved);
        CHECK("resolve last song byte by key", resolveOk2);
        CHECK("resolved == last block byte",
              resolveOk2 && resolved
                  == (uintptr_t)arenaBase + TEST_SONG_OFF
                     + EMERALD_AUDIO_SONG_BLOCK_SIZE - 1u);
        resolveOk2 = EmeraldResourceRangeIndex_ResolveByKey(
            index, &songSoloKey, GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, 1u,
            EMERALD_RESOURCE_ROLE_CANONICAL, 28u, &resolved);
        CHECK("resolve past song end fails", !resolveOk2);
    }
    /* Transformed block: COMPAT_OBJECT INSTRUMENT_BANK with the cry key. */
    cryHit = cryProbe != 0u
          && EmeraldResourceRangeIndex_Lookup(index, cryProbe, &hit);
    CHECK("cry row hit", cryHit);
    if (cryHit)
    {
        CHECK("cry key", Gen3ResourceId_KeyEqual(&hit.key, &cryKey));
        CHECK("cry type", hit.type == GEN3_RESOURCE_TYPE_INSTRUMENT_BANK);
        CHECK("cry role", hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT);
        CHECK("cry offset 12", hit.rangeOffset == 12u);
        resolveOk = EmeraldResourceRangeIndex_ResolveByKey(
            index, &cryKey, GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, 1u,
            EMERALD_RESOURCE_ROLE_COMPAT_OBJECT, 12u, &resolved);
        CHECK("resolve by key", resolveOk);
        CHECK("resolved pointer == probe", resolveOk && resolved == cryProbe);
    }
    /* The two audio hulls (R12-F §4/§5): hull A = the verbatim zone minus
     * its unbacked tail, hull B = the transformed zone. The zone tail, the
     * arena prefix (record tables) and the transform-align gap are
     * UNHULLED - a coincidental scalar there is ordinary data, while a
     * real pointer inside a hull but outside every range fails capture
     * closed. */
    CHECK("hull A start",
          EmeraldResourceRangeIndex_InHull(index, (uintptr_t)arenaBase));
    CHECK("hull A end",
          EmeraldResourceRangeIndex_InHull(
              index, (uintptr_t)arenaBase + TEST_SONG_OFF
                         + EMERALD_AUDIO_SONG_BLOCK_SIZE - 1u));
    CHECK("zone tail unhulled",
          !EmeraldResourceRangeIndex_InHull(
              index, (uintptr_t)arenaBase + TEST_SONG_OFF
                         + EMERALD_AUDIO_SONG_BLOCK_SIZE));
    CHECK("arena prefix unhulled",
          !EmeraldResourceRangeIndex_InHull(index, (uintptr_t)arenaBase - 1u));
    if (layoutOk)
    {
        CHECK("hull B start",
              EmeraldResourceRangeIndex_InHull(index, transformBase));
        CHECK("hull B end",
              EmeraldResourceRangeIndex_InHull(
                  index, transformBase + transformSize - 1u));
        CHECK("transform-align gap unhulled",
              !EmeraldResourceRangeIndex_InHull(index, transformBase - 1u));
        CHECK("past transform unhulled",
              !EmeraldResourceRangeIndex_InHull(
                  index, transformBase + transformSize));
    }
    (void)tag;
}

/* F: publication through a session built from the REAL production pack. */
static void TestPublication(const char *packPath)
{
    struct TestSession session;
    struct EmeraldAudioCompatDiagnostics diag;
    const uint8_t *arenaBase = NULL;
    const uint8_t *oldBase = NULL;
    const uint8_t *newBase = NULL;
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

    /* The engine runs the trainer seam's InitializeFromSnapshot (which
     * rebuilds the R10 index) before audio TryInitialize; mirror that. */
    EmeraldResourceRangeIndex_Reset(EmeraldResourceCompat_GetRangeIndex());

    CHECK("TryInitialize OK",
          EmeraldAudioCompat_TryInitialize(session.snapshot, session.pack, &diag)
              == EMERALD_AUDIO_OK);
    /* The success path records the informational publish note (payload
     * bytes); a FAILURE would also set the actualType field. */
    CHECK("publish note stage", strcmp(diag.stage, "publish") == 0);
    CHECK("no failure detail on success", diag.actualType[0] == '\0');
    CHECK("arena published (GetArena)", EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize));
    CHECK("arena size == span + transform",
          arenaSize == EMERALD_AUDIO_SPAN_SIZE + EMERALD_AUDIO_TRANSFORM_SIZE);
    CHECK("published count 569", EmeraldAudioCompat_GetPublishedCount() == EMERALD_AUDIO_LEAF_COUNT);
    /* R12-C §8: publish registered 198 spans + the arena hull. */
    CheckRangeRegistration("publish", arenaBase, 1301u, 2u);

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

    /* R12-C structural publication: transformed zone, spans, pads, cry
     * accessor and logical-label table, all against the pack fixtures.
     * Runs while the arena is published. */
    TestStructuralPublication();

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

    /* Republish (immutable arena): OK, and the ranges survive the
     * verify/truncate/re-register cycle in identical form. */
    CHECK("republish OK", EmeraldAudioCompat_Republish(&diag) == EMERALD_AUDIO_OK);
    CheckRangeRegistration("republish", arenaBase, 1301u, 2u);

    /* Relocated session: a second snapshot from the same pack replaces the
     * arena atomically with identical content; the ranges must follow the
     * NEW arena (the old spans are unregistered before the old arena dies). */
    {
        struct TestSession sessionB;
        struct EmeraldResourceRangeIndex *index =
            EmeraldResourceCompat_GetRangeIndex();
        struct EmeraldResourceRangeHit hit;
        oldBase = arenaBase;
        CHECK("session B opens", OpenSession(packPath, &sessionB));
        CHECK("TryInitialize (session B) OK",
              EmeraldAudioCompat_TryInitialize(sessionB.snapshot, sessionB.pack, &diag)
                  == EMERALD_AUDIO_OK);
        CHECK("count still 569 after replace",
              EmeraldAudioCompat_GetPublishedCount() == EMERALD_AUDIO_LEAF_COUNT);
        CHECK("bytes still == pack after replace",
              memcmp(arenaBase + sLeaves[0].arenaOffset, sLeaves[0].payload,
                     sLeaves[0].payloadSize) == 0);
        CHECK("new arena resolves",
              EmeraldAudioCompat_GetArena(&newBase, &arenaSize));
        CheckRangeRegistration("relocated", newBase, 1301u, 2u);
        /* If the allocator reused the freed region, the old address is the
         * new base and the check is vacuous; otherwise the old spans must
         * be gone (unregister ran before the free). */
        if (newBase != oldBase)
            CHECK("old arena spans unregistered",
                  index != NULL
                  && !EmeraldResourceRangeIndex_Lookup(index,
                                                       (uintptr_t)oldBase,
                                                       &hit));
        CloseSession(&sessionB);
    }

    /* Clear: fail closed - the ranges must die BEFORE the arena they point
     * at is freed. */
    {
        struct EmeraldResourceRangeIndex *index =
            EmeraldResourceCompat_GetRangeIndex();
        struct EmeraldResourceRangeHit hit;
        EmeraldAudioCompat_ClearMigratedEntries();
        CHECK("after clear: count 0", EmeraldAudioCompat_GetPublishedCount() == 0u);
        CHECK("after clear: GetArena false",
              !EmeraldAudioCompat_GetArena(&arenaBase, &arenaSize));
        CHECK("after clear: ranges unregistered",
              index != NULL
              && EmeraldResourceRangeIndex_GetRangeCount(index) == 0u
              && index->hullCount == 0u);
        CHECK("after clear: no lookups resolve",
              index != NULL
              && !EmeraldResourceRangeIndex_Lookup(index, (uintptr_t)newBase,
                                                   &hit)
              && !EmeraldResourceRangeIndex_InHull(index, (uintptr_t)newBase));
    }
    CHECK("after clear: GetLeafSpan false",
          !EmeraldAudioCompat_GetLeafSpan(sLeaves[0].canonicalName, NULL, NULL));
    CHECK("after clear: structural count 0",
          EmeraldAudioCompat_GetStructuralCount() == 0u);
    CHECK("after clear: transformed rows false",
          !EmeraldAudioCompat_GetTransformedRows(NULL, NULL));
    CHECK("after clear: voicegroup span false",
          !EmeraldAudioCompat_GetVoicegroupSpan(
              "emerald:audio/voicegroup/dummy", NULL, NULL));
    CHECK("after clear: cry span false",
          !EmeraldAudioCompat_GetCryTableSpan(false, NULL, NULL));
    CHECK("after clear: keysplit span false",
          !EmeraldAudioCompat_GetKeysplitSpan(
              "emerald:audio/keysplit/piano", NULL, NULL));
    CHECK("after clear: cry accessor NULL",
          EmeraldAudioCryTableRow(0u, false, 0u) == NULL);
    EmeraldAudioCompat_Shutdown();
    CHECK("shutdown idempotent", EmeraldAudioCompat_GetPublishedCount() == 0u);

    CloseSession(&session);
    CHECK("first payload above zone start (sparse, hole at zone start)",
          firstRomAddr + 0x08000000u > EMERALD_AUDIO_ROM_START);
}

/* S7/S8 helpers: the structural family is otherwise immutable in the
 * full-pack builder. */
static size_t FirstVoicegroupStructuralIndex(void)
{
    size_t i;
    for (i = 0; i < sStructuralCount; i++)
        if (sStructural[i].kind == 0u)
            return i;
    return SIZE_MAX;
}

/* S8: corrupt one voicegroup row's GBA_ROW_PTR (bytes 4..7 of a 12-byte
 * row). The first pointer-probing row (every type except squares/noise,
 * whose pointer the transform drops) gets its high half flipped: real
 * pointers are section addresses, so the flipped address is guaranteed
 * out-of-section and unresolvable. */
static void MutateStructuralRowPointer(struct AudioStructuralFixture *fix,
                                       size_t index)
{
    size_t i;
    uint8_t type;

    (void)index;
    if (fix->kind != 0u || fix->schema != 1u || fix->payloadSize < 12u)
        return;
    for (i = 0; i + 12u <= fix->payloadSize; i += 12u)
    {
        type = fix->payload[i];
        if ((type & 0x07u) == 1u || (type & 0x07u) == 2u
         || (type & 0x07u) == 4u)
            continue; /* square/noise: pointer dropped by the transform */
        fix->payload[i + 6u] ^= 0xFFu;
        fix->payload[i + 7u] ^= 0xFFu;
        return;
    }
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

    /* R12-D song-graph failure cases: FULL-audio packs (real 569 leaves +
     * real 202 structurals + real 530 songs) with exactly ONE damaged
     * song, against the real session. The seam must fail closed with the
     * family's error before any allocation, arena absent. */
    {
        struct Gen3ResourcePackDiagnosticList sdiag;
        Gen3ResourcePackDiagnostics_Init(&sdiag);
        /* The shared real session was closed for G1/G3/G4: reopen it for
         * the pack-side song mismatches. */
        CHECK("real session reopens", OpenSession(realPackPath, &session));

        /* S1: renamed song - pack entry the session cannot resolve. */
        {
            snprintf(path, sizeof(path), "%s/songrenamed.rpack", tempDir);
            CHECK("song-rename pack builds",
                  BuildFullAudioPack(path, sSongCount, NULL, MutateSongRename,
                                     sStructuralCount, SIZE_MAX, NULL));
            {
                struct Gen3ResourcePack *syntheticPack = NULL;
                CHECK("song-rename pack opens",
                      Gen3ResourcePack_OpenFile(path, &syntheticPack, &sdiag) == GEN3_PACK_OK);
                status = EmeraldAudioCompat_TryInitialize(session.snapshot,
                                                          syntheticPack, &diag);
                CHECK("renamed song -> RESOLVE_FAILED",
                      status == EMERALD_AUDIO_ERR_RESOLVE_FAILED);
                CHECK("diagnostics name the renamed song",
                      strcmp(diag.canonicalName,
                             "emerald:audio/song/mus-route101-x") == 0);
                CHECK("diagnostics stage resolve",
                      strcmp(diag.stage, "resolve") == 0);
                CHECK("arena absent after failure",
                      EmeraldAudioCompat_GetPublishedCount() == 0u);
                Gen3ResourcePack_Destroy(syntheticPack);
            }
        }

        /* S2: wrong schema - one song with schema 2 fails the seam's
         * schema-1 contract. */
        {
            snprintf(path, sizeof(path), "%s/songschema.rpack", tempDir);
            CHECK("song-schema pack builds",
                  BuildFullAudioPack(path, sSongCount, NULL, MutateSongSchema,
                                     sStructuralCount, SIZE_MAX, NULL));
            {
                struct Gen3ResourcePack *syntheticPack = NULL;
                CHECK("song-schema pack opens",
                      Gen3ResourcePack_OpenFile(path, &syntheticPack, &sdiag) == GEN3_PACK_OK);
                status = EmeraldAudioCompat_TryInitialize(session.snapshot,
                                                          syntheticPack, &diag);
                CHECK("schema-2 song -> UNEXPECTED_COUNT",
                      status == EMERALD_AUDIO_ERR_UNEXPECTED_COUNT);
                CHECK("arena absent after failure",
                      EmeraldAudioCompat_GetPublishedCount() == 0u);
                Gen3ResourcePack_Destroy(syntheticPack);
            }
        }

        /* S3: misplaced song - route101 shifted 0x10 inside the block. All
         * per-song checks pass (session-consistent size/bytes, in-block
         * placement, no leaf overlap) but the tiling proof then fails
         * closed: the boundary is unprovable. */
        {
            snprintf(path, sizeof(path), "%s/songoffset.rpack", tempDir);
            CHECK("song-offset pack builds",
                  BuildFullAudioPack(path, sSongCount, NULL, MutateSongOffset,
                                     sStructuralCount, SIZE_MAX, NULL));
            {
                struct Gen3ResourcePack *syntheticPack = NULL;
                CHECK("song-offset pack opens",
                      Gen3ResourcePack_OpenFile(path, &syntheticPack, &sdiag) == GEN3_PACK_OK);
                status = EmeraldAudioCompat_TryInitialize(session.snapshot,
                                                          syntheticPack, &diag);
                CHECK("misplaced song -> UNEXPECTED_COMPOSITION",
                      status == EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION);
                CHECK("arena absent after failure",
                      EmeraldAudioCompat_GetPublishedCount() == 0u);
                Gen3ResourcePack_Destroy(syntheticPack);
            }
        }

        /* S4: out-of-block placement - a song with a bogus early ROM
         * offset fails the block bounds check. */
        {
            snprintf(path, sizeof(path), "%s/songearly.rpack", tempDir);
            CHECK("song-early pack builds",
                  BuildFullAudioPack(path, sSongCount, NULL, MutateSongEarlyOffset,
                                     sStructuralCount, SIZE_MAX, NULL));
            {
                struct Gen3ResourcePack *syntheticPack = NULL;
                CHECK("song-early pack opens",
                      Gen3ResourcePack_OpenFile(path, &syntheticPack, &sdiag) == GEN3_PACK_OK);
                status = EmeraldAudioCompat_TryInitialize(session.snapshot,
                                                          syntheticPack, &diag);
                CHECK("out-of-block song -> PAYLOAD_SIZE_MISMATCH",
                      status == EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH);
                CHECK("arena absent after failure",
                      EmeraldAudioCompat_GetPublishedCount() == 0u);
                Gen3ResourcePack_Destroy(syntheticPack);
            }
        }

        /* S5: song/leaf overlap - an UNREFERENCED leaf (wave 17) moved
         * into the song block. Phase 1a placement and phase 1b row
         * resolution both pass (no row points at waves 17-20), so the
         * phase-1c tiling proof's bounds check fires (leaf start >=
         * SONG_BLOCK_OFFSET) with the R12-F fail-closed error. */
        {
            snprintf(path, sizeof(path), "%s/waveinblock.rpack", tempDir);
            CHECK("wave-in-block pack builds",
                  BuildFullAudioPack(path, sSongCount, MutateWaveIntoSongBlock,
                                     NULL, sStructuralCount, SIZE_MAX, NULL));
            {
                struct Gen3ResourcePack *syntheticPack = NULL;
                CHECK("wave-in-block pack opens",
                      Gen3ResourcePack_OpenFile(path, &syntheticPack, &sdiag) == GEN3_PACK_OK);
                status = EmeraldAudioCompat_TryInitialize(session.snapshot,
                                                          syntheticPack, &diag);
                CHECK("leaf in song block -> UNRESOLVED_POINTER",
                      status == EMERALD_AUDIO_ERR_UNRESOLVED_POINTER);
                CHECK("arena absent after failure",
                      EmeraldAudioCompat_GetPublishedCount() == 0u);
                Gen3ResourcePack_Destroy(syntheticPack);
            }
        }

        CloseSession(&session);
        Gen3ResourcePackDiagnostics_Destroy(&sdiag);
    }

    /* S6: missing song - a 529-song full-audio pack fails the composition
     * gate (its own session, since a pack-derived session is consistent by
     * construction). */
    {
        struct TestSession smallSongSession;
        snprintf(path, sizeof(path), "%s/songmissing.rpack", tempDir);
        CHECK("missing-song pack builds",
              BuildFullAudioPack(path, sSongCount - 1u, NULL, NULL,
                                 sStructuralCount, SIZE_MAX, NULL));
        CHECK("missing-song session opens", OpenSession(path, &smallSongSession));
        status = EmeraldAudioCompat_TryInitialize(smallSongSession.snapshot,
                                                  smallSongSession.pack, &diag);
        CHECK("missing song -> UNEXPECTED_COUNT",
              status == EMERALD_AUDIO_ERR_UNEXPECTED_COUNT);
        CHECK("arena absent after failure",
              EmeraldAudioCompat_GetPublishedCount() == 0u);
        CloseSession(&smallSongSession);
    }

    /* S7: missing voicegroup - one instrument-bank short of the 195-group
     * composition (its own session, pack-derived). */
    {
        struct TestSession missingVgSession;
        size_t skip = FirstVoicegroupStructuralIndex();
        snprintf(path, sizeof(path), "%s/vgmissing.rpack", tempDir);
        CHECK("missing-voicegroup pack builds",
              skip != SIZE_MAX
              && BuildFullAudioPack(path, sSongCount, NULL, NULL,
                                    sStructuralCount - 1u, skip, NULL));
        CHECK("missing-voicegroup session opens",
              OpenSession(path, &missingVgSession));
        status = EmeraldAudioCompat_TryInitialize(missingVgSession.snapshot,
                                                  missingVgSession.pack, &diag);
        CHECK("missing voicegroup -> UNEXPECTED_COMPOSITION",
              status == EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION);
        CHECK("diagnostics name the composition",
              strstr(diag.canonicalName, "structural (voicegroup 194") != NULL);
        CHECK("diagnostics stage build",
              strcmp(diag.stage, "build") == 0);
        CHECK("arena absent after failure",
              EmeraldAudioCompat_GetPublishedCount() == 0u);
        CloseSession(&missingVgSession);
    }

    /* S8: corrupt structural row - the first voicegroup's first
     * pointer-probing row points out-of-section. Phase 1c's transform
     * probe fails closed naming the row. */
    {
        struct TestSession corruptVgSession;
        snprintf(path, sizeof(path), "%s/vgrowptr.rpack", tempDir);
        CHECK("corrupt-structural pack builds",
              BuildFullAudioPack(path, sSongCount, NULL, NULL,
                                 sStructuralCount, SIZE_MAX,
                                 MutateStructuralRowPointer));
        CHECK("corrupt-structural session opens",
              OpenSession(path, &corruptVgSession));
        status = EmeraldAudioCompat_TryInitialize(corruptVgSession.snapshot,
                                                  corruptVgSession.pack, &diag);
        CHECK("corrupt structural row -> UNRESOLVED_POINTER",
              status == EMERALD_AUDIO_ERR_UNRESOLVED_POINTER);
        CHECK("diagnostics name the voicegroup row",
              strncmp(diag.canonicalName, "emerald:audio/voicegroup/",
                      sizeof("emerald:audio/voicegroup/") - 1u) == 0
              && strstr(diag.canonicalName, " row ") != NULL);
        CHECK("diagnostics stage transform",
              strcmp(diag.stage, "transform") == 0);
        CHECK("arena absent after failure",
              EmeraldAudioCompat_GetPublishedCount() == 0u);
        CloseSession(&corruptVgSession);
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
    if (!LoadStructuralFixtures(pack))
    {
        fprintf(stderr, "FAIL: cannot load the 202 structural fixtures from %s\n",
                packPath);
        Gen3ResourcePack_Destroy(pack);
        FreeAudioLeaves();
        return 1;
    }
    if (!LoadAudioSongs(pack))
    {
        fprintf(stderr, "FAIL: cannot load the 530 song fixtures from %s\n",
                packPath);
        Gen3ResourcePack_Destroy(pack);
        FreeAudioLeaves();
        FreeStructuralFixtures();
        return 1;
    }

    TestAudioCounts(pack);
    TestWaveDataValidation();
    TestArenaLayout();
    TestStructuralCounts();

    /* Publication runs against a fresh session each time. */
    CHECK("session opens", OpenSession(packPath, &session));
    TestPublication(packPath);
    CloseSession(&session);

    TestFailureMatrix(tempDir, packPath);

    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    FreeAudioLeaves();
    FreeStructuralFixtures();
    FreeAudioSongs();

    printf("emerald audio compat test: %d checks, %d failures\n",
           sChecks, sFails);
    return sFails == 0 ? 0 : 1;
}
