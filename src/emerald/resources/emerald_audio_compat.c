/* R12-B: Emerald audio leaf ownership migration - verbatim-zone arena.
 * See include/emerald/resources/emerald_audio_compat.h for the contract.
 * Platform-neutral: no global.h, MP2K, or frontend objects.
 *
 * Publication: the 569 audio-sample leaves of the session (105 root + 51
 * phoneme + 388 cry samples, 25 programmable waves) resolve through the
 * normal M0/M1 snapshot (winner must be the ROM_BASE provider) and their
 * canonical bytes are copied into ONE consolidated process-lifetime arena,
 * a verbatim zone mapped at ROM-relative offsets:
 *
 *     arenaOffset = romAddr - EMERALD_AUDIO_ROM_START   (0x0867709C)
 *
 * The pack supplies sourceRomOffset (build-time ROM provenance, proven
 * three-way against the retail ROM by gen3-elf-manifest/gen3-pack-build);
 * the snapshot supplies the canonical bytes through the resolver. The two
 * are cross-checked per leaf (size + byte equality) so the arena cannot
 * diverge from the session.
 *
 * Transactional: phase 1 validates every leaf (composition 105/51/388/25,
 * resolution, type/schema, ownership, bounds) before any allocation; phase 2
 * performs the single arena allocation and the verbatim copies. On any
 * failure the arena stays absent and the diagnostics name the first failing
 * leaf. R12-B consumers do not exist yet - a failed publication degrades
 * (the compiled audio objects still serve the game exactly as pre-R12-B),
 * it never refuses the session.
 */

#include "emerald/resources/emerald_audio_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_session.h"

/* The pack's sourceRomOffset values are file-relative (manifest rom_offset);
 * the verbatim zone is indexed by absolute GBA address. */
#define EMERALD_AUDIO_GBA_ROM_BASE 0x08000000u

enum EmeraldAudioLeafKind
{
    LEAF_KIND_ROOT = 0,
    LEAF_KIND_PHONEME,
    LEAF_KIND_CRY,
    LEAF_KIND_WAVE,
    LEAF_KIND_UNKNOWN,
};

struct EmeraldAudioLeafRecord
{
    char canonicalName[96];
    uint32_t kind;
    uint32_t arenaOffset; /* romAddr - EMERALD_AUDIO_ROM_START */
    uint32_t size;
};

/* One allocation: struct header + leaf table + the 3,329,304-byte verbatim
 * zone (holes zeroed, payloads at their ROM-relative offsets). */
struct EmeraldAudioArena
{
    size_t spanSize;        /* EMERALD_AUDIO_SPAN_SIZE */
    size_t publishedCount;  /* 569 when published */
    size_t payloadBytes;    /* sum of leaf payloads (diagnostics) */
    size_t leafTableOffset; /* relative to bytes[] */
    size_t zoneOffset;      /* verbatim zone start, relative to bytes[] */
    uint8_t bytes[];
};

static struct EmeraldAudioArena *sArena; /* NULL = not published */

static void ClearDiagnostics(struct EmeraldAudioCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static void NoteFailure(struct EmeraldAudioCompatDiagnostics *diagnostics,
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

/* Classify an audio-sample canonical name into the R12-B leaf kinds. The
 * taxonomy (R12-A §4): sample/<canonical> (root, no further slash),
 * sample/phoneme/<n>, sample/cry/<canonical>, wave/programmable/<n>. */
static enum EmeraldAudioLeafKind ClassifyLeaf(const char *name)
{
    static const char rootPrefix[] = "emerald:audio/sample/";
    static const char cryPrefix[] = "emerald:audio/sample/cry/";
    static const char phonemePrefix[] = "emerald:audio/sample/phoneme/";
    static const char wavePrefix[] = "emerald:audio/wave/programmable/";

    if (name == NULL)
        return LEAF_KIND_UNKNOWN;
    if (strncmp(name, wavePrefix, sizeof(wavePrefix) - 1u) == 0)
        return LEAF_KIND_WAVE;
    if (strncmp(name, cryPrefix, sizeof(cryPrefix) - 1u) == 0)
        return LEAF_KIND_CRY;
    if (strncmp(name, phonemePrefix, sizeof(phonemePrefix) - 1u) == 0)
        return LEAF_KIND_PHONEME;
    if (strncmp(name, rootPrefix, sizeof(rootPrefix) - 1u) == 0
     && strchr(name + sizeof(rootPrefix) - 1u, '/') == NULL)
        return LEAF_KIND_ROOT;
    return LEAF_KIND_UNKNOWN;
}

const char *EmeraldAudioCompatStatus_Describe(enum EmeraldAudioCompatStatus status)
{
    switch (status)
    {
    case EMERALD_AUDIO_OK:                       return "ok";
    case EMERALD_AUDIO_ERR_INVALID_ARGUMENT:     return "invalid argument";
    case EMERALD_AUDIO_ERR_OUT_OF_MEMORY:        return "out of memory";
    case EMERALD_AUDIO_ERR_RESOLVE_FAILED:       return "M0/M1 resolve failed";
    case EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH: return "payload size mismatch";
    case EMERALD_AUDIO_ERR_UNEXPECTED_COUNT:     return "unexpected leaf composition";
    case EMERALD_AUDIO_ERR_UNEXPECTED_OWNERSHIP: return "unexpected winner provider";
    case EMERALD_AUDIO_ERR_UNAVAILABLE:          return "no published arena";
    default:                                     return "unknown";
    }
}

enum EmeraldAudioCompatStatus
EmeraldAudioCompat_TryInitialize(
    const struct Gen3ResourceSnapshot *snapshot,
    const struct Gen3ResourcePack *pack,
    struct EmeraldAudioCompatDiagnostics *diagnostics)
{
    enum EmeraldAudioCompatStatus result = EMERALD_AUDIO_OK;
    struct EmeraldAudioLeafRecord *records = NULL;
    const uint8_t *payloads[EMERALD_AUDIO_LEAF_COUNT];
    struct EmeraldAudioArena *arena = NULL;
    size_t packCount;
    size_t count = 0;
    size_t payloadBytes = 0;
    size_t rootCount = 0, phonemeCount = 0, cryCount = 0, waveCount = 0;
    size_t recordBytes;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_AUDIO_ERR_INVALID_ARGUMENT;
    }

    /* Phase 1: validate the whole family before any allocation. The pack
     * iteration is the authoritative leaf enumeration (the session catalog is
     * pack-derived); the snapshot provides the M0/M1 view for each leaf. */
    records = (struct EmeraldAudioLeafRecord *)calloc(
        EMERALD_AUDIO_LEAF_COUNT, sizeof(*records));
    if (records == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
    }

    packCount = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        struct Gen3ResourceView view;
        struct EmeraldAudioLeafRecord *record;
        enum EmeraldAudioLeafKind kind;
        Gen3ResourceHandle handle;
        enum Gen3ResourceResult resolveResult;
        uint64_t romAddr;
        uint64_t arenaOffset;
        uint64_t arenaEnd;
        uint32_t viewSize;

        if (entry->type != GEN3_RESOURCE_TYPE_AUDIO_SAMPLE || entry->schema != 1u)
            continue;
        kind = ClassifyLeaf(entry->canonicalName);
        if (kind == LEAF_KIND_UNKNOWN)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COUNT;
            goto done;
        }
        if (count >= EMERALD_AUDIO_LEAF_COUNT)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COUNT;
            goto done;
        }

        resolveResult = Gen3ResourceSnapshot_FindHandle(
            snapshot, entry->canonicalName, &handle);
        if (resolveResult != GEN3_RESOURCE_OK)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        NULL);
            result = EMERALD_AUDIO_ERR_RESOLVE_FAILED;
            goto done;
        }
        resolveResult = Gen3ResourceSnapshot_Resolve(
            snapshot, handle, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, 1u, &view);
        if (resolveResult != GEN3_RESOURCE_OK || view.payload == NULL)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        NULL);
            result = EMERALD_AUDIO_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        &view);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        if (view.payloadSize != entry->payloadSize || entry->payload == NULL)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (memcmp(view.payload, entry->payload, view.payloadSize) != 0)
        {
            /* The resolver view must be byte-identical to the pack entry:
             * the arena copies session bytes, never pack-only bytes. */
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }

        /* Verbatim-zone placement: absolute GBA address, relative to the
         * audio section start, fully inside the span. */
        romAddr = (uint64_t)entry->sourceRomOffset + EMERALD_AUDIO_GBA_ROM_BASE;
        if (romAddr < EMERALD_AUDIO_ROM_START)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        arenaOffset = romAddr - EMERALD_AUDIO_ROM_START;
        viewSize = (uint32_t)view.payloadSize;
        arenaEnd = arenaOffset + viewSize;
        if (arenaEnd > EMERALD_AUDIO_SPAN_SIZE || arenaEnd < arenaOffset)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }

        record = &records[count];
        snprintf(record->canonicalName, sizeof(record->canonicalName), "%s",
                 entry->canonicalName);
        record->kind = (uint32_t)kind;
        record->arenaOffset = (uint32_t)arenaOffset;
        record->size = viewSize;
        payloads[count] = view.payload;
        payloadBytes += viewSize;
        count++;
        switch (kind)
        {
        case LEAF_KIND_ROOT:    rootCount++;    break;
        case LEAF_KIND_PHONEME: phonemeCount++; break;
        case LEAF_KIND_CRY:     cryCount++;     break;
        case LEAF_KIND_WAVE:    waveCount++;    break;
        default:                                 break;
        }
    }

    if (count != EMERALD_AUDIO_LEAF_COUNT
     || rootCount != EMERALD_AUDIO_ROOT_COUNT
     || phonemeCount != EMERALD_AUDIO_PHONEME_COUNT
     || cryCount != EMERALD_AUDIO_CRY_COUNT
     || waveCount != EMERALD_AUDIO_WAVE_COUNT)
    {
        char buffer[160];
        snprintf(buffer, sizeof(buffer),
                 "%zu leaves (root %zu, phoneme %zu, cry %zu, wave %zu)",
                 count, rootCount, phonemeCount, cryCount, waveCount);
        NoteFailure(diagnostics, "build", buffer,
                    GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                    EMERALD_AUDIO_LEAF_COUNT, (uint32_t)count, NULL);
        result = EMERALD_AUDIO_ERR_UNEXPECTED_COUNT;
        goto done;
    }

    /* Phase 2: one allocation for the header + leaf table + verbatim zone;
     * holes between payloads stay zeroed (unbacked). */
    recordBytes = EMERALD_AUDIO_LEAF_COUNT * sizeof(*records);
    if (recordBytes / EMERALD_AUDIO_LEAF_COUNT != sizeof(*records)
     || SIZE_MAX - sizeof(struct EmeraldAudioArena) < recordBytes
     || SIZE_MAX - sizeof(struct EmeraldAudioArena) - recordBytes
            < EMERALD_AUDIO_SPAN_SIZE)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
        goto done;
    }
    arena = (struct EmeraldAudioArena *)malloc(
        sizeof(struct EmeraldAudioArena) + recordBytes + EMERALD_AUDIO_SPAN_SIZE);
    if (arena == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
        goto done;
    }
    memset(arena, 0, sizeof(*arena) + recordBytes + EMERALD_AUDIO_SPAN_SIZE);
    arena->spanSize = EMERALD_AUDIO_SPAN_SIZE;
    arena->publishedCount = count;
    arena->payloadBytes = payloadBytes;
    arena->leafTableOffset = 0u;
    arena->zoneOffset = recordBytes;
    memcpy(arena->bytes, records, recordBytes);
    for (i = 0; i < count; i++)
    {
        const struct EmeraldAudioLeafRecord *record = &records[i];
        memcpy(arena->bytes + arena->zoneOffset + record->arenaOffset,
               payloads[i], record->size);
    }

    /* Publish atomically: only a fully built arena replaces the old one. */
    if (sArena != NULL)
    {
        free(sArena);
        sArena = NULL;
    }
    sArena = arena;
    arena = NULL;
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, (uint32_t)payloadBytes,
                0u, NULL);
    result = EMERALD_AUDIO_OK;

done:
    if (records != NULL)
        free(records);
    if (arena != NULL)
        free(arena);
    return result;
}

enum EmeraldAudioCompatStatus
EmeraldAudioCompat_Republish(struct EmeraldAudioCompatDiagnostics *diagnostics)
{
    ClearDiagnostics(diagnostics);
    if (sArena == NULL)
    {
        NoteFailure(diagnostics, "republish", NULL,
                    GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_AUDIO_ERR_UNAVAILABLE;
    }
    NoteFailure(diagnostics, "republish", NULL,
                GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, GEN3_RESOURCE_TYPE_INVALID,
                1u, 0u, (uint32_t)sArena->publishedCount, 0u, NULL);
    return EMERALD_AUDIO_OK;
}

void EmeraldAudioCompat_ClearMigratedEntries(void)
{
    if (sArena != NULL)
    {
        free(sArena);
        sArena = NULL;
    }
}

void EmeraldAudioCompat_Shutdown(void)
{
    EmeraldAudioCompat_ClearMigratedEntries();
}

bool EmeraldAudioCompat_GetArena(const uint8_t **outBase, size_t *outSize)
{
    if (outBase != NULL)
        *outBase = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    if (sArena == NULL || outBase == NULL || outSize == NULL)
        return false;
    *outBase = sArena->bytes + sArena->zoneOffset;
    *outSize = sArena->spanSize;
    return true;
}

size_t EmeraldAudioCompat_GetPublishedCount(void)
{
    return sArena != NULL ? sArena->publishedCount : 0u;
}

static const struct EmeraldAudioLeafRecord *FindRecord(const char *canonicalName)
{
    size_t i;
    if (sArena == NULL || canonicalName == NULL)
        return NULL;
    for (i = 0; i < sArena->publishedCount; i++)
    {
        const struct EmeraldAudioLeafRecord *record =
            (const struct EmeraldAudioLeafRecord *)(const void *)
                (sArena->bytes + sArena->leafTableOffset
                 + i * sizeof(struct EmeraldAudioLeafRecord));
        if (strcmp(record->canonicalName, canonicalName) == 0)
            return record;
    }
    return NULL;
}

bool EmeraldAudioCompat_GetLeafSpan(const char *canonicalName,
                                    size_t *outArenaOffset, size_t *outSize)
{
    const struct EmeraldAudioLeafRecord *record = FindRecord(canonicalName);
    if (record == NULL)
        return false;
    if (outArenaOffset != NULL)
        *outArenaOffset = record->arenaOffset;
    if (outSize != NULL)
        *outSize = record->size;
    return true;
}

bool EmeraldAudioCompat_GetLeafBytes(const char *canonicalName,
                                     const uint8_t **outBytes, size_t *outSize)
{
    const struct EmeraldAudioLeafRecord *record = FindRecord(canonicalName);
    if (record == NULL || sArena == NULL)
        return false;
    if (outBytes != NULL)
        *outBytes = sArena->bytes + sArena->zoneOffset + record->arenaOffset;
    if (outSize != NULL)
        *outSize = record->size;
    return true;
}
