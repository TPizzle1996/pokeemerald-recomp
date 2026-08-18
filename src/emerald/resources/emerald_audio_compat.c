/* R12-B/R12-C: Emerald audio ownership migration - verbatim-zone arena +
 * transformed-zone structural build.
 * See include/emerald/resources/emerald_audio_compat.h for the contract.
 * Platform-neutral: no global.h, MP2K, or frontend objects.
 *
 * R12-B publication: the 569 audio-sample leaves of the session (105 root +
 * 51 phoneme + 388 cry samples, 25 programmable waves) resolve through the
 * normal M0/M1 snapshot (winner must be the ROM_BASE provider) and their
 * canonical bytes are copied into ONE consolidated process-lifetime arena,
 * a verbatim zone mapped at ROM-relative offsets:
 *
 *     arenaOffset = romAddr - EMERALD_AUDIO_ROM_START   (0x0867709C)
 *
 * R12-C publication: the 202 structural resources (195 voicegroups, 2 cry
 * tables, 5 keysplit runs) resolve the same way; their GBA-form 12-byte
 * rows are transformed to the native 24-byte width into a TRANSFORMED ZONE
 * appended after the verbatim zone, with per-row pointers resolved at
 * publish (sample/wave -> verbatim leaf, subgroup -> transformed block,
 * keysplit -> verbatim run copy). The 10 drumset voicegroups are preceded
 * by their back-shift pads (N x 24 zero bytes) so the back-shifted group
 * label points at mapped zeroed memory, never another table's rows.
 *
 * Transactional: phase 1 validates every leaf AND every structural row
 * (composition pins, resolution, type/schema, ownership, bounds, pointer
 * targets) before any allocation; phase 2 performs the single arena
 * allocation and all copies. On any failure the arena stays absent and the
 * diagnostics name the first failing resource. R12-B/R12-C consumers do
 * not read the arena until the consumer redirect lands - a failed
 * publication degrades (the compiled audio objects still serve the game
 * exactly as pre-R12-B), it never refuses the session.
 */

#include "emerald/resources/emerald_audio_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_resource_session.h"

static void RegisterLogicalLabel(uint32_t gbaAddr, const void *hostBase,
                                 void *user);

/* R12-C §5 logical-address publication hooks (exact-start GBA label ->
 * arena native row). The strong definitions live in src/platform/host_memory.c,
 * which this seam must NOT include: the seam is platform-neutral (guardrail
 * 18 - no platform/ header may enter the gen3 core or seam include set). The
 * references are declared WEAK, mirroring GetRangeIndex above, so offline
 * links without host_memory.c (the seam tests, the parity gate, the loader
 * harnesses) resolve them to NULL and skip the runtime-only registration -
 * the table is a live-game resolution aid, never part of the pack or arena.
 * GbaAddr is u32 == uint32_t (include/gba/types.h), so the uint32_t
 * parameter is ABI-identical to the engine's declaration. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void HostMemoryRegisterLogicalAddress(uint32_t addr, void *hostBase);
#if defined(__GNUC__)
__attribute__((weak))
#endif
void HostMemoryClearLogicalAddresses(void);
/* R12-D §5: the interval hook for the contiguous song block. Same weak
 * pattern: strong definitions live in host_memory.c, absent in offline
 * links. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void HostMemoryRegisterLogicalRange(uint32_t romStart, uint32_t romEnd,
                                    void *hostBase);
#if defined(__GNUC__)
__attribute__((weak))
#endif
void HostMemoryClearLogicalRanges(void);

/* R12-C §8 (minimal State-v5 forward-pull): the arena's runtime tables and
 * zones register into the shared R10 resource-range index so mid-play
 * saves capture arena pointers as sidecar records (identity + offset) and
 * loads re-derive them, instead of refusing or silently persisting. The
 * index instance is owned by the trainer seam; this seam reaches it through
 * EmeraldResourceCompat_GetRangeIndex(). The reference is WEAK so offline
 * links without the trainer seam (the seam tests, the parity gate's
 * unrelated links) resolve it to NULL and simply skip registration - the
 * arena stays published and the state walker fails closed on arena
 * pointers (no range, no hull). The strong definition lives in
 * emerald_trainer_native_compat.c; the parity gate links its own stub
 * index and asserts the registration end-to-end. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
struct EmeraldResourceRangeIndex *EmeraldResourceCompat_GetRangeIndex(void);

/* The pack's sourceRomOffset values are file-relative (manifest rom_offset);
 * the verbatim zone is indexed by absolute GBA address. */
#define EMERALD_AUDIO_GBA_ROM_BASE 0x08000000u

/* 12-byte GBA ToneData row layout (GBA-form; the transform doubles it to
 * the 24-byte native width - plan §1.2). */
#define GBA_ROW_TYPE     0u
#define GBA_ROW_PTR      4u /* u32 LE; sample/wave/group depending on type */
#define GBA_ROW_ADSR     8u /* u32 LE; keysplit table ptr for type 0x40 */

/* GBA ToneData types (music_voice.inc). */
#define ROW_TYPE_DIRECTSOUND         0x00u
#define ROW_TYPE_SQUARE_1            0x01u
#define ROW_TYPE_SQUARE_2            0x02u
#define ROW_TYPE_PROGRAMMABLE_WAVE   0x03u
#define ROW_TYPE_NOISE               0x04u
#define ROW_TYPE_DIRECTSOUND_NORES   0x08u
#define ROW_TYPE_SQUARE_1_ALT        0x09u
#define ROW_TYPE_SQUARE_2_ALT        0x0Au
#define ROW_TYPE_PROGRAMMABLE_WAVE_A 0x0Bu
#define ROW_TYPE_NOISE_ALT           0x0Cu
#define ROW_TYPE_DIRECTSOUND_ALT     0x10u
#define ROW_TYPE_CRY                 0x20u
#define ROW_TYPE_CRY_REVERSE         0x30u
#define ROW_TYPE_KEYSPLIT            0x40u
#define ROW_TYPE_KEYSPLIT_ALL        0x80u

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

enum EmeraldAudioStructuralKind
{
    STRUCTURAL_VOICEGROUP = 0,
    STRUCTURAL_CRY_TABLE,
    STRUCTURAL_KEYSPLIT,
};

/* Structural record. labelRomAddr is the GBA address of the table LABEL
 * (first row minus the back-shift for drumsets/keysplits); transformOffset
 * is the transformed-block base in the transformed zone (for drumsets the
 * block base INCLUDES the zero pad - the label points there); verbatimOffset
 * is the keysplit run copy offset in the verbatim zone. */
struct EmeraldAudioStructuralRecord
{
    char canonicalName[96];
    uint32_t kind;          /* EmeraldAudioStructuralKind */
    uint32_t sub;           /* cry: 0=forward 1=reverse; else 0 */
    uint32_t labelRomAddr;  /* GBA label address (incl. back-shift) */
    uint32_t backshiftRows; /* drumset pad rows (keysplits: 0; see BackshiftBytes) */
    uint32_t backshiftBytes;/* keysplit back-shift bytes (drumsets: 0) */
    uint32_t rowCount;      /* schema-1 rows; keysplits: 0 */
    uint32_t payloadBytes;  /* keysplit run bytes; schema-1: rows*12 */
    uint32_t verbatimOffset;/* keysplits only */
    uint32_t transformOffset;/* voicegroups/cries only (block base incl. pad) */
};

/* R12-D song record: canonical identity + zone-relative placement. The song
 * payloads themselves stay canonical GBA-form bytes in the verbatim zone;
 * only the record table is seam-owned. */
struct EmeraldAudioSongRecord
{
    char canonicalName[96];
    uint32_t arenaOffset; /* romAddr - EMERALD_AUDIO_ROM_START */
    uint32_t size;
};

/* Phase-1c scratch: the song record under validation plus the resolver view
 * it proved byte-identical to. Sorted by arenaOffset before the tiling
 * proof; phase 2 copies record + payload straight from the sorted array. */
struct EmeraldAudioSongScratch
{
    struct EmeraldAudioSongRecord rec;
    const uint8_t *payload;
};

static int CompareSongScratch(const void *a, const void *b)
{
    const struct EmeraldAudioSongScratch *sa =
        (const struct EmeraldAudioSongScratch *)a;
    const struct EmeraldAudioSongScratch *sb =
        (const struct EmeraldAudioSongScratch *)b;
    if (sa->rec.arenaOffset != sb->rec.arenaOffset)
        return sa->rec.arenaOffset < sb->rec.arenaOffset ? -1 : 1;
    if (sa->rec.size != sb->rec.size)
        return sa->rec.size < sb->rec.size ? -1 : 1;
    return strcmp(sa->rec.canonicalName, sb->rec.canonicalName);
}

/* Phase-1c leaf-sub-zone tiling scratch (R12-F §3): one entry per leaf
 * payload or keysplit range (label-extended), sorted by start; the proof
 * verifies every entry is inside [0, SONG_BLOCK_OFFSET) and no two overlap. */
struct LeafTileEntry
{
    uint64_t start;
    uint64_t end;
    char name[96];
};

/* Phase-1c leaf-sub-zone tiling comparator (R12-F §3). */
static int CompareLeafTile(const void *a, const void *b)
{
    const struct LeafTileEntry *ta = (const struct LeafTileEntry *)a;
    const struct LeafTileEntry *tb = (const struct LeafTileEntry *)b;
    if (ta->start != tb->start)
        return ta->start < tb->start ? -1 : 1;
    if (ta->end != tb->end)
        return ta->end < tb->end ? -1 : 1;
    return strcmp(ta->name, tb->name);
}

/* One allocation: struct header + leaf table + structural table + song
 * table + the 3,329,304-byte verbatim zone (holes zeroed, payloads +
 * keysplit runs + the 530 song graphs at their ROM-relative offsets) + the
 * transformed zone (contiguous after the verbatim zone, 8-aligned). */
struct EmeraldAudioArena
{
    size_t spanSize;            /* EMERALD_AUDIO_SPAN_SIZE */
    size_t publishedCount;      /* 569 leaves when published */
    size_t structuralCount;     /* 202 when published */
    size_t publishedSongCount;  /* 530 when published */
    size_t payloadBytes;        /* sum of leaf payloads (diagnostics) */
    size_t leafTableOffset;     /* relative to bytes[] */
    size_t structuralTableOffset; /* relative to bytes[] */
    size_t songTableOffset;     /* relative to bytes[] */
    size_t zoneOffset;          /* verbatim zone start, relative to bytes[] */
    size_t transformOffset;     /* transformed zone start, relative to bytes[] */
    size_t transformSize;       /* EMERALD_AUDIO_TRANSFORM_SIZE */
    uint8_t bytes[];
};

static struct EmeraldAudioArena *sArena; /* NULL = not published */

/* The pinned drumset back-shifts (voice_group <name>, N; generator-pinned
 * PINNED_DRUMSET_BACKS, manifest rom_offset = first row - N*12). */
struct BackshiftPin
{
    const char *canonical;
    uint32_t rows;
};

static const struct BackshiftPin kDrumsetBacks[] =
{
    {"emerald:audio/voicegroup/rs-drumset", 36u},
    {"emerald:audio/voicegroup/frlg-drumset", 36u},
    {"emerald:audio/voicegroup/emerald-drumset-1", 36u},
    {"emerald:audio/voicegroup/emerald-drumset-2", 36u},
    {"emerald:audio/voicegroup/petalburg-drumset", 36u},
    {"emerald:audio/voicegroup/route101-drumset", 36u},
    {"emerald:audio/voicegroup/route110-drumset", 40u},
    {"emerald:audio/voicegroup/frlg-fanfare-drumset-1", 36u},
    {"emerald:audio/voicegroup/frlg-fanfare-drumset-2", 36u},
    {"emerald:audio/voicegroup/rg-credits-drumset", 36u},
};

static const struct BackshiftPin kKeysplitBacks[EMERALD_AUDIO_KEYSPLIT_COUNT] =
{
    {"emerald:audio/keysplit/piano", 36u},
    {"emerald:audio/keysplit/strings", 36u},
    {"emerald:audio/keysplit/trumpet", 36u},
    {"emerald:audio/keysplit/tuba", 24u},
    {"emerald:audio/keysplit/french-horn", 36u},
};

/* R12-F: the spans this seam registers into the shared range index are
 * EXACTLY the 1,301 audio resources - one span per resource, no synthetic
 * zone identity. The R10 index is NON-OVERLAPPING (InsertRange rejects any
 * overlap), so each leaf payload, each keysplit range (base at the LABEL
 * address the live keySplitTable pointers target; the five ranges tile
 * [firstLabel, lastRunEnd) exactly - KeysplitRangeLength), each of the
 * 530 per-song spans (tiling [SONG_BLOCK_START, SONG_BLOCK_END) exactly) and
 * each of the 197 transformed table blocks (195 voicegroups + 2 cry tables,
 * each base INCLUDING its drumset back-shift pad) gets its own identity:
 *   leaves         AUDIO_SAMPLE       / 1 / CANONICAL      (569)
 *   keysplits      INSTRUMENT_BANK    / 2 / CANONICAL      (5)
 *   songs          MUSIC_SEQUENCE     / 1 / CANONICAL      (530)
 *   voicegroups    INSTRUMENT_BANK    / 1 / COMPAT_OBJECT  (195)
 *   cry tables     INSTRUMENT_BANK    / 1 / COMPAT_OBJECT  (2)
 * The 3,428-byte zone tail [SONG_BLOCK_END, SPAN_END) is unbacked and hull-
 * EXCLUDED (fail-closed on capture), as is the whole arena prefix (header +
 * record tables) and the transform-align gap. Two hulls only: the verbatim
 * zone minus the tail [zoneBase, +SONG_BLOCK_OFFSET + SONG_BLOCK_SIZE) and
 * the transformed zone [transformBase, +transformSize). */
#define EMERALD_AUDIO_RANGE_SPAN_COUNT \
    (EMERALD_AUDIO_LEAF_COUNT + EMERALD_AUDIO_KEYSPLIT_COUNT \
     + EMERALD_AUDIO_SONG_COUNT \
     + EMERALD_AUDIO_VOICEGROUP_COUNT + EMERALD_AUDIO_CRY_TABLE_COUNT)
#if EMERALD_AUDIO_RANGE_SPAN_COUNT != 1301u
#error "audio range span count disagrees with the pinned 1,301"
#endif

/* Zone-relative song block placement: the plan §4 quoted 0x21BFA0, but
 * 0x088FC03C - 0x0867709C is 0x284FA0 (and the block end is 0x32BFB4, not
 * 0x32B614). The derived expression is the single source of truth - the
 * literal is reproduced here only as a compile-time cross-check. */
#define EMERALD_AUDIO_SONG_BLOCK_OFFSET \
    (EMERALD_AUDIO_SONG_BLOCK_START - EMERALD_AUDIO_ROM_START)
#if EMERALD_AUDIO_SONG_BLOCK_OFFSET != 0x284FA0u
#error "song block zone offset disagrees with the verified 0x284FA0"
#endif
#if (EMERALD_AUDIO_SONG_BLOCK_END - EMERALD_AUDIO_ROM_START) != 0x32BFB4u
#error "song block zone end disagrees with the verified 0x32BFB4"
#endif

/* The registered span list: kept so re-registration can verify that the
 * index still holds exactly our spans (it also holds the trainer seam's
 * ranges, which sort around ours by base) and remove only our own block
 * before re-registering - never the trainer's entries. When the trainer
 * seam rebuilds the index it RESETS it wholesale, so our spans are gone:
 * the verification mismatch is the signal to register fresh instead. */
struct EmeraldAudioRangeEntry
{
    uintptr_t base;
    size_t length;
    const char *canonicalName; /* points into the arena's record table */
    Gen3ResourceKey key;       /* derived at span build; verified in-index */
    uint32_t type;             /* per-resource identity (R12-F §4) */
    uint32_t schema;
    enum EmeraldResourceRangeRole role;
};

static struct EmeraldAudioRangeEntry sAudioRanges[EMERALD_AUDIO_RANGE_SPAN_COUNT];
static size_t sAudioRangeCount;  /* spans currently in the index */
static size_t sAudioRangeMark;   /* sorted position of our first span */
static size_t sAudioHullMark;    /* index->hullCount before our hulls */
static bool sAudioRangesInIndex; /* our spans + hulls are registered now */

static int CompareRangeByBase(const void *a, const void *b)
{
    const struct EmeraldAudioRangeEntry *ra =
        (const struct EmeraldAudioRangeEntry *)a;
    const struct EmeraldAudioRangeEntry *rb =
        (const struct EmeraldAudioRangeEntry *)b;
    if (ra->base != rb->base)
        return ra->base < rb->base ? -1 : 1;
    return strcmp(ra->canonicalName, rb->canonicalName);
}

/* The two audio hulls (R12-F §4, plan §5): hull A = the verbatim zone minus
 * its unbacked tail [zoneBase, +SONG_BLOCK_OFFSET + SONG_BLOCK_SIZE), hull B
 * = the transformed zone [transformBase, +transformSize). Everything else in
 * the allocation (header, record tables, tail, align gap) is UNHULLED: a
 * coincidental scalar there is ordinary data (the R10 apuCycle precedent),
 * while a real pointer inside a hull but outside every registered range
 * fails capture closed. */
static size_t VerbatimHullLength(void)
{
    return EMERALD_AUDIO_SONG_BLOCK_OFFSET + EMERALD_AUDIO_SONG_BLOCK_SIZE;
}

static bool ArenaHullMatches(const struct EmeraldResourceRangeIndex *index)
{
    const uint8_t *zoneBase;
    const uint8_t *transformBase;

    if (index == NULL || sArena == NULL
     || sAudioHullMark + 2u > index->hullCount)
        return false;
    zoneBase = sArena->bytes + sArena->zoneOffset;
    transformBase = sArena->bytes + sArena->transformOffset;
    return index->hulls[sAudioHullMark].base == (uintptr_t)zoneBase
        && index->hulls[sAudioHullMark].length == VerbatimHullLength()
        && index->hulls[sAudioHullMark + 1u].base == (uintptr_t)transformBase
        && index->hulls[sAudioHullMark + 1u].length == sArena->transformSize;
}

static struct EmeraldAudioRangeEntry sAudioRanges[EMERALD_AUDIO_RANGE_SPAN_COUNT];
static size_t sAudioRangeCount;  /* spans currently in the index */
static size_t sAudioRangeMark;   /* sorted position of our first span */
static size_t sAudioHullMark;    /* index->hullCount before our hull */
static bool sAudioRangesInIndex; /* our spans + hull are registered now */

/* True when the index still holds exactly our spans at [mark, mark+count):
 * our block is CONTIGUOUS in the sorted array (all spans lie inside one
 * allocation, and every trainer range lies in a different allocation, so
 * no trainer range sorts between our spans). */
static bool ArenaRangesMatch(const struct EmeraldResourceRangeIndex *index)
{
    size_t i;

    if (index == NULL || sAudioRangeCount == 0u)
        return false;
    /* Our block must fit at the recorded mark; entries AFTER it are allowed
     * (ranges from other seams may sort above our arena's spans - the block
     * is identified by its own base/length/key rows, never by being the
     * array tail). */
    if (sAudioRangeMark > index->rangeCount
     || sAudioRangeCount > index->rangeCount - sAudioRangeMark)
        return false;
    for (i = 0; i < sAudioRangeCount; i++)
    {
        const struct EmeraldResourceRange *range =
            &index->ranges[sAudioRangeMark + i];
        if (range->base != sAudioRanges[i].base
         || range->length != sAudioRanges[i].length
         || memcmp(range->key.bytes, sAudioRanges[i].key.bytes,
                   GEN3_RESOURCE_KEY_SIZE) != 0)
            return false;
    }
    return true;
}

/* Remove our spans + hull from the shared index. No-op when the trainer
 * seam rebuilt the index since our registration (reset - they are gone).
 * The index may be NULL in offline links (weak GetRangeIndex). Removal is
 * safe only when the entries are exactly ours (verified), and only in one
 * block: our spans sort CONTIGUOUSLY (one allocation; trainer ranges lie in
 * other allocations, so none sorts between ours) at [mark, mark+count). */
static void UnregisterArenaRanges(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();

    if (index == NULL || !sAudioRangesInIndex)
        return;
    if (ArenaRangesMatch(index))
    {
        memmove(&index->ranges[sAudioRangeMark],
                &index->ranges[sAudioRangeMark + sAudioRangeCount],
                (index->rangeCount - sAudioRangeMark - sAudioRangeCount)
                    * sizeof(index->ranges[0]));
        index->rangeCount -= sAudioRangeCount;
    }
    if (ArenaHullMatches(index))
        index->hullCount = sAudioHullMark;
    sAudioRangesInIndex = false;
}

/* R12-F keysplit geometry, audited against the reference tree's
 * sound/keysplit_tables.inc: each manifest run IS the table's content (the
 * .byte entries the game indexes - note range [36..107] for tables 1/2/3/5,
 * [24..107] for tuba), and the mks4agb label sits backshift bytes BEFORE the
 * run start ("key split table labels can appear before the actual start of
 * the key split table data", keysplit_tables.inc:1-11) so that the game's
 * absolute key numbers index the table with no offset arithmetic. Because
 * the runs tile contiguously, the LABELS tile the whole section
 * [firstLabel, lastRunEnd) exactly (verified against the ELF symbols:
 * KeySplitTable1..5 @ 0x086B4698/0x86B46E0/0x86B4728/0x86B477C/0x86B47C4,
 * deltas 72/72/84/72 and last run end 0x86B4830 - 108 after the last
 * label). The per-table range is therefore [label_N, label_{N+1}):
 * length = runLen_N + backshift_N - backshift_{N+1}, the LAST table (in
 * label order) getting runLen + backshift. This keeps the live
 * track->tone.keySplitTable pointer - which targets the LABEL - resolving at
 * rangeOffset 0, while the five ranges tile [firstLabel, lastRunEnd) with no
 * overlap and no hole (R12-F §3 tiling, §4 role table). The next label's
 * back-shift enters the formula, so lengths are assigned after sorting the
 * collected records by label address. */
struct KeysplitSpan
{
    uint64_t labelRomAddr;
    uint32_t backshiftBytes;
    uint32_t payloadBytes;
    const char *canonicalName;
};

static int CompareKeysplitByLabel(const void *a, const void *b)
{
    const struct KeysplitSpan *ka = (const struct KeysplitSpan *)a;
    const struct KeysplitSpan *kb = (const struct KeysplitSpan *)b;
    if (ka->labelRomAddr != kb->labelRomAddr)
        return ka->labelRomAddr < kb->labelRomAddr ? -1 : 1;
    return strcmp(ka->canonicalName, kb->canonicalName);
}

/* Range length of sorted[i] (sorted by labelRomAddr): runLen + backshift,
 * minus the NEXT label's back-shift (its label pad eats the tail of this
 * run - those bytes belong to the next range, keeping the tiling exact). */
static size_t KeysplitRangeLength(const struct KeysplitSpan *sorted,
                                  size_t count, size_t i)
{
    size_t length = (size_t)sorted[i].payloadBytes + sorted[i].backshiftBytes;
    if (i + 1u < count)
        length -= sorted[i + 1u].backshiftBytes;
    return length;
}

/* Build the span list for the published arena (sArena must be live): one
 * span per resource with its exact identity (R12-F §4). The list is sorted
 * by base at the end - the [mark, mark+count) block logic needs our spans
 * contiguous in the shared index, and the leaf/keysplit spans (pack order in
 * the tables) are not address-ordered until the qsort. */
static void BuildArenaSpanList(void)
{
    const uint8_t *zoneBase = sArena->bytes + sArena->zoneOffset;
    const uint8_t *transformBase = sArena->bytes + sArena->transformOffset;
    size_t i, n = 0u;

    /* The 569 leaves: each span is exactly its payload (the phase-1c tiling
     * proof pinned every leaf inside [zoneBase, +SONG_BLOCK_OFFSET), so no
     * span can reach the song block or another leaf). */
    for (i = 0; i < sArena->publishedCount; i++)
    {
        const struct EmeraldAudioLeafRecord *record =
            (const struct EmeraldAudioLeafRecord *)(const void *)
                (sArena->bytes + sArena->leafTableOffset
                 + i * sizeof(struct EmeraldAudioLeafRecord));
        sAudioRanges[n].base =
            (uintptr_t)(zoneBase + record->arenaOffset);
        sAudioRanges[n].length = record->size;
        sAudioRanges[n].canonicalName = record->canonicalName;
        Gen3ResourceId_DeriveKey(sAudioRanges[n].canonicalName,
                                 &sAudioRanges[n].key);
        sAudioRanges[n].type = GEN3_RESOURCE_TYPE_AUDIO_SAMPLE;
        sAudioRanges[n].schema = 1u;
        sAudioRanges[n].role = EMERALD_RESOURCE_ROLE_CANONICAL;
        n++;
    }
    /* The 5 keysplit ranges: base at the LABEL address (runStart minus the
     * back-shift) - the live track->tone.keySplitTable pointer targets the
     * label, so its sidecar record must resolve at rangeOffset 0 (R12-F §4
     * role table). Lengths come from the label tiling (KeysplitRangeLength):
     * the five ranges tile [firstLabel, lastRunEnd) exactly, so they never
     * overlap a leaf or each other and leave no keysplit-section byte
     * unowned. The phase-1c tiling proof re-verifies the same spans. */
    {
        struct KeysplitSpan ks[EMERALD_AUDIO_KEYSPLIT_COUNT];
        size_t ksCount = 0u;
        for (i = 0; i < sArena->structuralCount; i++)
        {
            const struct EmeraldAudioStructuralRecord *record =
                (const struct EmeraldAudioStructuralRecord *)(const void *)
                    (sArena->bytes + sArena->structuralTableOffset
                     + i * sizeof(struct EmeraldAudioStructuralRecord));
            if (record->kind != STRUCTURAL_KEYSPLIT)
                continue;
            ks[ksCount].labelRomAddr = record->labelRomAddr;
            ks[ksCount].backshiftBytes = record->backshiftBytes;
            ks[ksCount].payloadBytes = record->payloadBytes;
            ks[ksCount].canonicalName = record->canonicalName;
            ksCount++;
        }
        qsort(ks, ksCount, sizeof(ks[0]), CompareKeysplitByLabel);
        for (i = 0; i < ksCount; i++)
        {
            sAudioRanges[n].base =
                (uintptr_t)(zoneBase + (ks[i].labelRomAddr
                                        - EMERALD_AUDIO_ROM_START));
            sAudioRanges[n].length =
                KeysplitRangeLength(ks, ksCount, i);
            sAudioRanges[n].canonicalName = ks[i].canonicalName;
            Gen3ResourceId_DeriveKey(sAudioRanges[n].canonicalName,
                                     &sAudioRanges[n].key);
            sAudioRanges[n].type = GEN3_RESOURCE_TYPE_INSTRUMENT_BANK;
            sAudioRanges[n].schema = 2u;
            sAudioRanges[n].role = EMERALD_RESOURCE_ROLE_CANONICAL;
            n++;
        }
    }
    /* The 530 per-song spans (phase 1c proved they tile
     * [SONG_BLOCK_OFFSET, +SONG_BLOCK_SIZE) exactly - no gaps, no overlaps). */
    for (i = 0; i < sArena->publishedSongCount; i++)
    {
        const struct EmeraldAudioSongRecord *record =
            (const struct EmeraldAudioSongRecord *)(const void *)
                (sArena->bytes + sArena->songTableOffset
                 + i * sizeof(struct EmeraldAudioSongRecord));
        sAudioRanges[n].base =
            (uintptr_t)(zoneBase + record->arenaOffset);
        sAudioRanges[n].length = record->size;
        sAudioRanges[n].canonicalName = record->canonicalName;
        Gen3ResourceId_DeriveKey(sAudioRanges[n].canonicalName,
                                 &sAudioRanges[n].key);
        sAudioRanges[n].type = GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE;
        sAudioRanges[n].schema = 1u;
        sAudioRanges[n].role = EMERALD_RESOURCE_ROLE_CANONICAL;
        n++;
    }
    /* The 197 transformed blocks: whole-table COMPAT_OBJECT spans (plan §4:
     * no per-row identities - the 21,370 rows add zero identity value). The
     * block base INCLUDES the drumset back-shift pad; the pad must be inside
     * the range - the back-shifted label points at it. */
    for (i = 0; i < sArena->structuralCount; i++)
    {
        const struct EmeraldAudioStructuralRecord *record =
            (const struct EmeraldAudioStructuralRecord *)(const void *)
                (sArena->bytes + sArena->structuralTableOffset
                 + i * sizeof(struct EmeraldAudioStructuralRecord));
        if (record->kind == STRUCTURAL_KEYSPLIT)
            continue;
        sAudioRanges[n].base =
            (uintptr_t)(transformBase + record->transformOffset);
        sAudioRanges[n].length =
            ((size_t)record->backshiftRows + record->rowCount) * 24u;
        sAudioRanges[n].canonicalName = record->canonicalName;
        Gen3ResourceId_DeriveKey(sAudioRanges[n].canonicalName,
                                 &sAudioRanges[n].key);
        sAudioRanges[n].type = GEN3_RESOURCE_TYPE_INSTRUMENT_BANK;
        sAudioRanges[n].schema = 1u;
        sAudioRanges[n].role = EMERALD_RESOURCE_ROLE_COMPAT_OBJECT;
        n++;
    }
    qsort(sAudioRanges, n, sizeof(sAudioRanges[0]), CompareRangeByBase);
    sAudioRangeCount = n;
}

/* Register the spans + two hulls into the shared index (transactional: any
 * failure restores the index to its pre-registration state - the rollback
 * resets hullCount to the pre-hull mark, trimming BOTH hulls). Re-registration
 * first removes our previous spans (verified - only our own block) so it
 * can never overlap itself. The sorted-insertion position of our block is
 * computed up front: with the mark recorded, the block is always
 * [mark, mark+count) regardless of where it sorts. */
static bool RegisterArenaRanges(void)
{
    struct EmeraldResourceRangeIndex *index = EmeraldResourceCompat_GetRangeIndex();
    size_t mark, hullMark, i;

    if (index == NULL || sArena == NULL)
        return false;
    if (sAudioRangesInIndex)
        UnregisterArenaRanges();
    BuildArenaSpanList();
    hullMark = index->hullCount;
    sAudioHullMark = hullMark;
    /* Sorted insertion position of our first (lowest) span: no existing
     * range shares an allocation with us, so every range with a base below
     * ours sorts before our whole block. */
    mark = 0u;
    while (mark < index->rangeCount
           && index->ranges[mark].base < sAudioRanges[0].base)
        mark++;
    sAudioRangeMark = mark;
    for (i = 0; i < sAudioRangeCount; i++)
    {
        if (!EmeraldResourceRangeIndex_RegisterSpan(
                index, sAudioRanges[i].base, sAudioRanges[i].length,
                sAudioRanges[i].canonicalName, sAudioRanges[i].type,
                sAudioRanges[i].schema, sAudioRanges[i].role))
            goto fail;
    }
    if (!EmeraldResourceRangeIndex_AddHull(
            index, (uintptr_t)(sArena->bytes + sArena->zoneOffset),
            VerbatimHullLength())
     || !EmeraldResourceRangeIndex_AddHull(
            index, (uintptr_t)(sArena->bytes + sArena->transformOffset),
            sArena->transformSize))
        goto fail;
    sAudioRangesInIndex = true;
    return true;

fail:
    /* Restore: our partial block sits at [mark, mark+i) (each inserted
     * span is contiguous with the previous ones because the spans are
     * sorted among themselves); shift the entries that were pushed right
     * back over it. */
    if (i > 0u)
    {
        memmove(&index->ranges[mark], &index->ranges[mark + i],
                (index->rangeCount - mark - i) * sizeof(index->ranges[0]));
        index->rangeCount -= i;
    }
    index->hullCount = hullMark;
    sAudioRangesInIndex = false;
    return false;
}

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
    case EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION: return "unexpected structural composition";
    case EMERALD_AUDIO_ERR_UNRESOLVED_POINTER:   return "unresolved audio row pointer";
    case EMERALD_AUDIO_ERR_RANGE_REGISTRATION:   return "R10 range registration failed";
    default:                                     return "unknown";
    }
}

static uint32_t Load32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void Store64(uint8_t *p, uintptr_t v)
{
    size_t i;
    for (i = 0; i < sizeof(uintptr_t); i++)
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xFFu);
}

/* ---- Structural resolution helpers (publish-time, not runtime) ---- */

/* Resolve a sample/wave GBA pointer to its verbatim-zone host address. The
 * pointer must hit EXACTLY one leaf span (the generator proved every row
 * reference is a defined leaf; a miss here is a hard publication failure).
 * `zoneBase == NULL` is the phase-1 probe mode: only resolvability is
 * tested, and a hit returns a non-NULL sentinel (no address arithmetic). */
static const uint8_t *ResolveLeafPointer(
    const struct EmeraldAudioLeafRecord *leafRecords, size_t leafCount,
    const uint8_t *zoneBase, uint32_t gbaAddr)
{
    size_t hits = 0;
    size_t i;

    if (gbaAddr < EMERALD_AUDIO_ROM_START)
        return NULL;
    for (i = 0; i < leafCount; i++)
    {
        const struct EmeraldAudioLeafRecord *leaf = &leafRecords[i];
        uint64_t start = (uint64_t)EMERALD_AUDIO_ROM_START + leaf->arenaOffset;
        uint64_t end = start + leaf->size;
        if ((uint64_t)gbaAddr >= start && (uint64_t)gbaAddr < end)
        {
            hits++;
            if (zoneBase != NULL)
                return zoneBase + leaf->arenaOffset
                     + ((uint64_t)gbaAddr - start);
        }
    }
    return hits == 1u ? (const uint8_t *)(uintptr_t)1u : NULL;
}

/* Resolve a voicegroup label address (0x40/0x80 subgroup) to its
 * transformed-block host base. The address must equal exactly one label.
 * Probe mode (transformBase == NULL) tests resolvability only. */
static const uint8_t *ResolveGroupPointer(
    const struct EmeraldAudioStructuralRecord *records, size_t recordCount,
    const uint8_t *transformBase, uint32_t gbaAddr)
{
    size_t hits = 0;
    size_t i;

    for (i = 0; i < recordCount; i++)
    {
        const struct EmeraldAudioStructuralRecord *record = &records[i];
        if (record->kind != STRUCTURAL_VOICEGROUP)
            continue;
        if (record->labelRomAddr == gbaAddr)
        {
            hits++;
            if (transformBase != NULL)
                return transformBase + record->transformOffset;
        }
    }
    return hits == 1u ? (const uint8_t *)(uintptr_t)1u : NULL;
}

/* Resolve a keysplit label address (0x40 union2) to its verbatim-zone host
 * address. The label points `backshiftBytes` before the run, exactly where
 * the compiled label lands (m4a.inc back-shift); the bytes before the run
 * are in-span (zeroed or prior-table bytes - notes below the first split
 * are never played, same invariant as compiled). Probe mode (zoneBase ==
 * NULL) tests resolvability only. */
static const uint8_t *ResolveKeysplitPointer(
    const struct EmeraldAudioStructuralRecord *records, size_t recordCount,
    const uint8_t *zoneBase, uint32_t gbaAddr)
{
    size_t hits = 0;
    size_t i;

    if (gbaAddr < EMERALD_AUDIO_ROM_START)
        return NULL;
    for (i = 0; i < recordCount; i++)
    {
        const struct EmeraldAudioStructuralRecord *record = &records[i];
        if (record->kind != STRUCTURAL_KEYSPLIT)
            continue;
        if (record->labelRomAddr == gbaAddr)
        {
            hits++;
            if (zoneBase != NULL)
                return zoneBase + (gbaAddr - EMERALD_AUDIO_ROM_START);
        }
    }
    return hits == 1u ? (const uint8_t *)(uintptr_t)1u : NULL;
}

/* Transform one 12-byte GBA row into its 24-byte native form (plan §1.2):
 * four scalar bytes verbatim, 4 pad bytes, an 8-byte union (sample/wave
 * pointer, subgroup pointer, or 0 for squares/noise), then either the ADSR
 * bytes verbatim + pad (all rows) or the keysplit-table pointer (.quad) for
 * type 0x40 / zero for 0x80. Returns false on unknown type or an
 * unresolvable pointer, naming the row through `where`. */
static bool TransformRow(const uint8_t *gba, uint8_t *out,
                         const struct EmeraldAudioLeafRecord *leafRecords,
                         size_t leafCount, const uint8_t *zoneBase,
                         const struct EmeraldAudioStructuralRecord *records,
                         size_t recordCount, const uint8_t *transformBase)
{
    uint32_t type = gba[GBA_ROW_TYPE];

    memcpy(out, gba, 4u);           /* type, key, length, pan_sweep */
    memset(out + 4u, 0, 4u);        /* .space 4 */
    memset(out + 20u, 0, 4u);       /* .space 4 (after ADSR; keysplit rows
                                       overwrite 16-23 with the .quad) */
    switch (type)
    {
    case ROW_TYPE_DIRECTSOUND:
    case ROW_TYPE_DIRECTSOUND_NORES:
    case ROW_TYPE_DIRECTSOUND_ALT:
    case ROW_TYPE_PROGRAMMABLE_WAVE:
    case ROW_TYPE_PROGRAMMABLE_WAVE_A:
    case ROW_TYPE_CRY:
    case ROW_TYPE_CRY_REVERSE:
    {
        /* union @8 = sample/wave pointer; ADSR @16 = GBA bytes 8-11 verbatim. */
        const uint8_t *wav = ResolveLeafPointer(
            leafRecords, leafCount, zoneBase, Load32(gba + GBA_ROW_PTR));
        if (wav == NULL)
            return false;
        Store64(out + 8u, (uintptr_t)wav);
        memcpy(out + 16u, gba + GBA_ROW_ADSR, 4u);
        return true;
    }
    case ROW_TYPE_SQUARE_1:
    case ROW_TYPE_SQUARE_1_ALT:
    case ROW_TYPE_SQUARE_2:
    case ROW_TYPE_SQUARE_2_ALT:
    case ROW_TYPE_NOISE:
    case ROW_TYPE_NOISE_ALT:
        /* Duty/period (GBA bytes 4-7) is dropped on native: union = 0. */
        Store64(out + 8u, 0u);
        memcpy(out + 16u, gba + GBA_ROW_ADSR, 4u);
        return true;
    case ROW_TYPE_KEYSPLIT:
    {
        /* union @8 = subgroup rows; .quad @16 = keysplit run. No ADSR. */
        const uint8_t *group = ResolveGroupPointer(
            records, recordCount, transformBase, Load32(gba + GBA_ROW_PTR));
        const uint8_t *keysplit = ResolveKeysplitPointer(
            records, recordCount, zoneBase, Load32(gba + GBA_ROW_ADSR));
        if (group == NULL || keysplit == NULL)
            return false;
        Store64(out + 8u, (uintptr_t)group);
        Store64(out + 16u, (uintptr_t)keysplit);
        return true;
    }
    case ROW_TYPE_KEYSPLIT_ALL:
    {
        /* union @8 = subgroup rows; .quad @16 = 0. No ADSR. */
        const uint8_t *group = ResolveGroupPointer(
            records, recordCount, transformBase, Load32(gba + GBA_ROW_PTR));
        if (group == NULL)
            return false;
        Store64(out + 8u, (uintptr_t)group);
        Store64(out + 16u, 0u);
        return true;
    }
    default:
        return false;
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
    struct EmeraldAudioStructuralRecord *structRecords = NULL;
    struct EmeraldAudioSongScratch *songScratch = NULL;
    const uint8_t *payloads[EMERALD_AUDIO_LEAF_COUNT];
    struct EmeraldAudioArena *arena = NULL;
    size_t packCount;
    size_t count = 0;
    size_t payloadBytes = 0;
    size_t songCount = 0;
    size_t songTableBytes = 0;
    size_t rootCount = 0, phonemeCount = 0, cryCount = 0, waveCount = 0;
    size_t recordBytes;
    size_t structRecordBytes;
    size_t structuralCount = 0;
    size_t voicegroupCount = 0, cryTableCount = 0, keysplitCount = 0;
    size_t streamRows = 0;
    size_t zoneOffset, transformOffset;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || pack == NULL)
    {
        NoteFailure(diagnostics, "build", NULL, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_AUDIO_ERR_INVALID_ARGUMENT;
    }

    /* Phase 1a: validate the whole LEAF family before any allocation. The
     * pack iteration is the authoritative leaf enumeration (the session
     * catalog is pack-derived); the snapshot provides the M0/M1 view for
     * each leaf. */
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

    /* Phase 1b: validate the STRUCTURAL family (202 instrument-bank
     * resources). Same resolution/ownership/size machinery as the leaves;
     * the payload must divide into whole 12-byte rows (schema 1) or be a
     * keysplit run (schema 2). */
    structRecords = (struct EmeraldAudioStructuralRecord *)calloc(
        EMERALD_AUDIO_STRUCTURAL_COUNT, sizeof(*structRecords));
    if (structRecords == NULL)
    {
        NoteFailure(diagnostics, "build", NULL,
                    GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
        goto done;
    }

    for (i = 0; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        struct Gen3ResourceView view;
        struct EmeraldAudioStructuralRecord *record;
        const char *name;
        uint32_t backshiftRows = 0u;
        uint32_t backshiftBytes = 0u;
        enum EmeraldAudioStructuralKind kind;
        bool isReverseCry = false;
        Gen3ResourceHandle handle;
        enum Gen3ResourceResult resolveResult;
        uint64_t romAddr;
        size_t j;

        if (entry->type != GEN3_RESOURCE_TYPE_INSTRUMENT_BANK)
            continue;
        name = entry->canonicalName;
        if (strncmp(name, "emerald:audio/voicegroup/",
                    sizeof("emerald:audio/voicegroup/") - 1u) == 0)
        {
            kind = STRUCTURAL_VOICEGROUP;
            for (j = 0; j < sizeof(kDrumsetBacks) / sizeof(kDrumsetBacks[0]);
                 j++)
            {
                if (strcmp(name, kDrumsetBacks[j].canonical) == 0)
                {
                    backshiftRows = kDrumsetBacks[j].rows;
                    break;
                }
            }
        }
        else if (strncmp(name, "emerald:audio/cry-table/",
                         sizeof("emerald:audio/cry-table/") - 1u) == 0)
        {
            kind = STRUCTURAL_CRY_TABLE;
            isReverseCry =
                strcmp(name, "emerald:audio/cry-table/reverse") == 0;
        }
        else if (strncmp(name, "emerald:audio/keysplit/",
                         sizeof("emerald:audio/keysplit/") - 1u) == 0)
        {
            kind = STRUCTURAL_KEYSPLIT;
            for (j = 0; j < sizeof(kKeysplitBacks) / sizeof(kKeysplitBacks[0]);
                 j++)
            {
                if (strcmp(name, kKeysplitBacks[j].canonical) == 0)
                {
                    backshiftBytes = kKeysplitBacks[j].rows;
                    break;
                }
            }
            if (backshiftBytes == 0u)
            {
                NoteFailure(diagnostics, "build", name,
                            GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                            entry->schema, entry->schema,
                            (uint32_t)entry->payloadSize, 0u, NULL);
                result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
                goto done;
            }
        }
        else
        {
            /* An instrument-bank outside the audio family (e.g. a future
             * R12-D song) is not an R12-C structural resource. */
            continue;
        }
        if (kind == STRUCTURAL_VOICEGROUP && entry->schema != 1u)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
            goto done;
        }
        if (kind == STRUCTURAL_CRY_TABLE && entry->schema != 1u)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
            goto done;
        }
        if (kind == STRUCTURAL_KEYSPLIT && entry->schema != 2u)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        2u, entry->schema, (uint32_t)entry->payloadSize,
                        (uint32_t)entry->payloadSize, NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
            goto done;
        }
        if (structuralCount >= EMERALD_AUDIO_STRUCTURAL_COUNT)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize, 0u, NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
            goto done;
        }

        resolveResult = Gen3ResourceSnapshot_FindHandle(
            snapshot, name, &handle);
        if (resolveResult != GEN3_RESOURCE_OK)
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize, 0u, NULL);
            result = EMERALD_AUDIO_ERR_RESOLVE_FAILED;
            goto done;
        }
        resolveResult = Gen3ResourceSnapshot_Resolve(
            snapshot, handle, GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
            entry->schema, &view);
        if (resolveResult != GEN3_RESOURCE_OK || view.payload == NULL)
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize, 0u, NULL);
            result = EMERALD_AUDIO_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize, 0u, &view);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        if (view.payloadSize != entry->payloadSize || entry->payload == NULL)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        if (memcmp(view.payload, entry->payload, view.payloadSize) != 0)
        {
            NoteFailure(diagnostics, "build", name,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                        entry->schema, entry->schema,
                        (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }

        record = &structRecords[structuralCount];
        snprintf(record->canonicalName, sizeof(record->canonicalName), "%s",
                 name);
        record->kind = (uint32_t)kind;
        record->sub = isReverseCry ? 1u : 0u;
        record->backshiftRows = backshiftRows;
        record->backshiftBytes = backshiftBytes;
        record->payloadBytes = (uint32_t)entry->payloadSize;

        romAddr = (uint64_t)entry->sourceRomOffset + EMERALD_AUDIO_GBA_ROM_BASE;
        if (kind == STRUCTURAL_KEYSPLIT)
        {
            /* Keysplit run: label points backshiftBytes before the run. */
            if (backshiftBytes >= entry->payloadSize
             || romAddr < (uint64_t)EMERALD_AUDIO_ROM_START + backshiftBytes)
            {
                NoteFailure(diagnostics, "build", name,
                            GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                            entry->schema, entry->schema,
                            (uint32_t)entry->payloadSize,
                            (uint32_t)entry->payloadSize, &view);
                result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
                goto done;
            }
            record->labelRomAddr =
                (uint32_t)(romAddr - backshiftBytes);
            keysplitCount++;
        }
        else
        {
            uint32_t rows;
            uint64_t labelAddr;
            if (entry->payloadSize == 0u || entry->payloadSize % 12u != 0u)
            {
                NoteFailure(diagnostics, "build", name,
                            GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                            entry->schema, entry->schema,
                            (uint32_t)entry->payloadSize,
                            (uint32_t)entry->payloadSize, &view);
                result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
                goto done;
            }
            rows = (uint32_t)(entry->payloadSize / 12u);
            /* The label is back-shifted by backshiftRows GBA rows (12 B). */
            labelAddr = romAddr - (uint64_t)backshiftRows * 12u;
            if (labelAddr < EMERALD_AUDIO_SECTION_START)
            {
                NoteFailure(diagnostics, "build", name,
                            GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, entry->type,
                            entry->schema, entry->schema,
                            (uint32_t)entry->payloadSize,
                            (uint32_t)entry->payloadSize, &view);
                result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
                goto done;
            }
            record->labelRomAddr = (uint32_t)labelAddr;
            record->rowCount = rows;
            streamRows += rows;
            if (kind == STRUCTURAL_VOICEGROUP)
                voicegroupCount++;
            else
                cryTableCount++;
        }
        structuralCount++;
    }

    if (structuralCount != EMERALD_AUDIO_STRUCTURAL_COUNT
     || voicegroupCount != EMERALD_AUDIO_VOICEGROUP_COUNT
     || cryTableCount != EMERALD_AUDIO_CRY_TABLE_COUNT
     || keysplitCount != EMERALD_AUDIO_KEYSPLIT_COUNT
     || streamRows != EMERALD_AUDIO_STREAM_ROWS)
    {
        char buffer[160];
        snprintf(buffer, sizeof(buffer),
                 "%zu structural (voicegroup %zu, cry-table %zu, keysplit %zu, "
                 "rows %zu)", structuralCount, voicegroupCount, cryTableCount,
                 keysplitCount, streamRows);
        NoteFailure(diagnostics, "build", buffer,
                    GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                    EMERALD_AUDIO_STRUCTURAL_COUNT, (uint32_t)structuralCount,
                    NULL);
        result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
        goto done;
    }

    /* Phase 1c: prove the LEAF SUB-ZONE tiling (R12-F §3): every leaf
     * payload AND every keysplit range (label-extended: the back-shift pad
     * plus the run) must lie strictly inside [0, SONG_BLOCK_OFFSET) and no
     * two may overlap. This subsumes the R12-C per-run hole check, extends
     * it to the keysplit LABEL region (the live keySplitTable pointers
     * target the label, so an overlap there would alias another leaf's
     * bytes), and adds the missing leaf-vs-leaf and keysplit-vs-keysplit
     * disjointness proofs. The song block is separately proven to tile
     * [SONG_BLOCK_OFFSET, +SONG_BLOCK_SIZE) exactly below, so the two
     * families together leave no leaf-sub-zone byte unproven. */
    {
        struct LeafTileEntry *tiles = (struct LeafTileEntry *)calloc(
            EMERALD_AUDIO_LEAF_COUNT + EMERALD_AUDIO_KEYSPLIT_COUNT,
            sizeof(*tiles));
        size_t tileCount = 0u;

        if (tiles == NULL)
        {
            NoteFailure(diagnostics, "build", NULL,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
            result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
            goto done;
        }
        for (i = 0; i < count; i++)
        {
            tiles[tileCount].start = records[i].arenaOffset;
            tiles[tileCount].end = tiles[tileCount].start + records[i].size;
            snprintf(tiles[tileCount].name, sizeof(tiles[tileCount].name),
                     "%s", records[i].canonicalName);
            tileCount++;
        }
        {
            struct KeysplitSpan ks[EMERALD_AUDIO_KEYSPLIT_COUNT];
            size_t ksCount = 0u;
            for (i = 0; i < structuralCount; i++)
            {
                const struct EmeraldAudioStructuralRecord *record =
                    &structRecords[i];
                if (record->kind != STRUCTURAL_KEYSPLIT)
                    continue;
                ks[ksCount].labelRomAddr = record->labelRomAddr;
                ks[ksCount].backshiftBytes = record->backshiftBytes;
                ks[ksCount].payloadBytes = record->payloadBytes;
                ks[ksCount].canonicalName = record->canonicalName;
                ksCount++;
            }
            qsort(ks, ksCount, sizeof(ks[0]), CompareKeysplitByLabel);
            for (i = 0; i < ksCount; i++)
            {
                uint64_t labelOff = ks[i].labelRomAddr
                                  - EMERALD_AUDIO_ROM_START;
                tiles[tileCount].start = labelOff;
                tiles[tileCount].end = labelOff
                                     + KeysplitRangeLength(ks, ksCount, i);
                snprintf(tiles[tileCount].name, sizeof(tiles[tileCount].name),
                         "%s", ks[i].canonicalName);
                tileCount++;
            }
        }
        qsort(tiles, tileCount, sizeof(*tiles), CompareLeafTile);
        for (i = 0; i < tileCount; i++)
        {
            uint64_t endOff = tiles[i].end;
            if (tiles[i].end <= tiles[i].start
             || tiles[i].start >= EMERALD_AUDIO_SONG_BLOCK_OFFSET
             || endOff > EMERALD_AUDIO_SONG_BLOCK_OFFSET)
            {
                NoteFailure(diagnostics, "transform", tiles[i].name,
                            GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            EMERALD_AUDIO_SONG_BLOCK_OFFSET,
                            (uint32_t)tiles[i].end, NULL);
                free(tiles);
                result = EMERALD_AUDIO_ERR_UNRESOLVED_POINTER;
                goto done;
            }
            if (i > 0u && tiles[i - 1u].end > tiles[i].start)
            {
                NoteFailure(diagnostics, "transform", tiles[i].name,
                            GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            (uint32_t)tiles[i - 1u].end,
                            (uint32_t)tiles[i].start, NULL);
                free(tiles);
                result = EMERALD_AUDIO_ERR_UNRESOLVED_POINTER;
                goto done;
            }
        }
        free(tiles);
    }

    /* Phase 1c: validate the transform arithmetically (row + pad sums) and
     * prove every row pointer resolves (sample/wave -> exactly one leaf
     * span, subgroup -> exactly one voicegroup label, keysplit -> exactly
     * one keysplit label) before any allocation. */
    recordBytes = EMERALD_AUDIO_LEAF_COUNT * sizeof(*records);
    structRecordBytes =
        EMERALD_AUDIO_STRUCTURAL_COUNT * sizeof(*structRecords);
    songTableBytes =
        EMERALD_AUDIO_SONG_COUNT * sizeof(struct EmeraldAudioSongRecord);
    zoneOffset = recordBytes + structRecordBytes + songTableBytes;
    if (recordBytes / EMERALD_AUDIO_LEAF_COUNT != sizeof(*records)
     || songTableBytes / EMERALD_AUDIO_SONG_COUNT
            != sizeof(struct EmeraldAudioSongRecord)
     || zoneOffset < recordBytes
     || zoneOffset < structRecordBytes
     || zoneOffset < songTableBytes)
    {
        NoteFailure(diagnostics, "build", NULL,
                    GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
        goto done;
    }
    transformOffset = zoneOffset + EMERALD_AUDIO_SPAN_SIZE;
    transformOffset = (transformOffset + 7u) & ~(size_t)7u; /* 8-align */
    if (transformOffset < zoneOffset + EMERALD_AUDIO_SPAN_SIZE)
    {
        NoteFailure(diagnostics, "build", NULL,
                    GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
        goto done;
    }

    {
        size_t transformPos = 0u;
        size_t padBytes = 0u;
        uint8_t scratch[24];
        for (i = 0; i < structuralCount; i++)
        {
            const struct EmeraldAudioStructuralRecord *record =
                &structRecords[i];
            const struct Gen3ResourcePackEntry *entry =
                Gen3ResourcePack_FindByCanonicalName(pack, record->canonicalName);

            if (record->kind == STRUCTURAL_KEYSPLIT)
            {
                /* Keysplit runs carry no transform rows; the leaf-sub-zone
                 * tiling proof above covers their bounds, label regions and
                 * hole placement. */
                continue;
            }

            /* Voicegroup / cry-table block: drumset pad rows first, then
             * the transformed rows. */
            if (record->backshiftRows != 0u)
            {
                if (transformPos + (size_t)record->backshiftRows * 24u
                    > EMERALD_AUDIO_TRANSFORM_SIZE)
                {
                    result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
                    goto done;
                }
                transformPos += (size_t)record->backshiftRows * 24u;
                padBytes += (size_t)record->backshiftRows * 24u;
            }
            if (transformPos + (size_t)record->rowCount * 24u
                > EMERALD_AUDIO_TRANSFORM_SIZE)
            {
                result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
                goto done;
            }
            {
                size_t row;
                const uint8_t *gbaRows = entry->payload;
                for (row = 0; row < record->rowCount; row++)
                {
                    char where[160];
                    /* Probe mode (NULL bases): proves the pointer targets
                     * resolve; the phase-2 transform re-derives the host
                     * addresses with the real bases. */
                    if (!TransformRow(gbaRows + row * 12u, scratch,
                                      records, count, NULL,
                                      structRecords, structuralCount, NULL))
                    {
                        snprintf(where, sizeof(where), "%s row %zu",
                                 record->canonicalName, row);
                        NoteFailure(diagnostics, "transform", where,
                                    GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                                    entry->type, entry->schema, entry->schema,
                                    record->rowCount, 0u, NULL);
                        result = EMERALD_AUDIO_ERR_UNRESOLVED_POINTER;
                        goto done;
                    }
                }
            }
            transformPos += (size_t)record->rowCount * 24u;
        }
        if (transformPos != EMERALD_AUDIO_TRANSFORM_SIZE
         || padBytes != EMERALD_AUDIO_DRUMSET_PAD_BYTES)
        {
            char buffer[160];
            snprintf(buffer, sizeof(buffer),
                     "transform zone %zu (pad %zu)", transformPos, padBytes);
            NoteFailure(diagnostics, "build", buffer,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                        EMERALD_AUDIO_TRANSFORM_SIZE, (uint32_t)transformPos,
                        NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
            goto done;
        }
    }

    /* Phase 1c: validate the SONG GRAPH family (R12-D: exactly 530
     * music-sequence resources, schema 1, canonical prefix
     * "emerald:audio/song/"). Same resolution/ownership/size machinery as
     * the leaves; then the records are sorted by arenaOffset and proven to
     * TILE the song block exactly - first byte at the block start, zero
     * inter-object gaps, last byte at the block end. The tile proof is the
     * publish-time re-derivation of the generator's packed-object rule, so
     * an unprovable boundary fails closed before any allocation. */
    songScratch = (struct EmeraldAudioSongScratch *)calloc(
        EMERALD_AUDIO_SONG_COUNT, sizeof(*songScratch));
    if (songScratch == NULL)
    {
        NoteFailure(diagnostics, "build", NULL,
                    GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
        goto done;
    }
    songCount = 0u;
    for (i = 0; i < packCount; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);
        struct Gen3ResourceView view;
        struct EmeraldAudioSongScratch *scratch;
        Gen3ResourceHandle handle;
        enum Gen3ResourceResult resolveResult;
        uint64_t romAddr;
        uint64_t arenaEnd;
        uint32_t viewSize;
        size_t j;

        if (entry->type != GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE)
            continue;
        if (entry->schema != 1u
         || strncmp(entry->canonicalName, "emerald:audio/song/",
                    sizeof("emerald:audio/song/") - 1u) != 0)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COUNT;
            goto done;
        }
        if (songCount >= EMERALD_AUDIO_SONG_COUNT)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
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
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        NULL);
            result = EMERALD_AUDIO_ERR_RESOLVE_FAILED;
            goto done;
        }
        resolveResult = Gen3ResourceSnapshot_Resolve(
            snapshot, handle, GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, 1u, &view);
        if (resolveResult != GEN3_RESOURCE_OK || view.payload == NULL)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        NULL);
            result = EMERALD_AUDIO_ERR_RESOLVE_FAILED;
            goto done;
        }
        if (strcmp(view.winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
        {
            NoteFailure(diagnostics, "resolve", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
                        1u, entry->schema, 0u, (uint32_t)entry->payloadSize,
                        &view);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_OWNERSHIP;
            goto done;
        }
        if (view.payloadSize != entry->payloadSize || entry->payload == NULL)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
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
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        (uint32_t)view.payloadSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }

        /* Placement: inside the song block, so the ordinary
         * romAddr - EMERALD_AUDIO_ROM_START lands in
         * [SONG_BLOCK_OFFSET, +SONG_BLOCK_SIZE) and the zone offset never
         * aliases a leaf. */
        romAddr = (uint64_t)entry->sourceRomOffset + EMERALD_AUDIO_GBA_ROM_BASE;
        viewSize = (uint32_t)view.payloadSize;
        if (romAddr < EMERALD_AUDIO_SONG_BLOCK_START
         || romAddr > EMERALD_AUDIO_SONG_BLOCK_END
         || viewSize > (uint64_t)EMERALD_AUDIO_SONG_BLOCK_END - romAddr)
        {
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        viewSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        arenaEnd = romAddr - EMERALD_AUDIO_ROM_START + viewSize;
        if (arenaEnd > EMERALD_AUDIO_SPAN_SIZE)
        {
            /* Provably unreachable (block inside the zone) but the
             * fail-closed check is the seam's job, not arithmetic luck. */
            NoteFailure(diagnostics, "build", entry->canonicalName,
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
                        1u, entry->schema, (uint32_t)entry->payloadSize,
                        viewSize, &view);
            result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
            goto done;
        }
        /* No song may overlap a leaf payload: the generator's packed-object
         * proof covers the block, and R12-B proved the leaves, but the seam
         * re-derives the DISJOINTNESS at publish time (mirrors the keysplit
         * hole check in phase 1b). */
        for (j = 0; j < count; j++)
        {
            uint64_t leafStart = (uint64_t)EMERALD_AUDIO_ROM_START
                               + records[j].arenaOffset;
            uint64_t leafEnd = leafStart + records[j].size;
            if (romAddr < leafEnd
             && romAddr + viewSize > leafStart)
            {
                NoteFailure(diagnostics, "build", entry->canonicalName,
                            GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE, entry->type,
                            1u, entry->schema, (uint32_t)entry->payloadSize,
                            0u, NULL);
                result = EMERALD_AUDIO_ERR_PAYLOAD_SIZE_MISMATCH;
                goto done;
            }
        }

        scratch = &songScratch[songCount];
        snprintf(scratch->rec.canonicalName,
                 sizeof(scratch->rec.canonicalName), "%s",
                 entry->canonicalName);
        scratch->rec.arenaOffset =
            (uint32_t)(romAddr - EMERALD_AUDIO_ROM_START);
        scratch->rec.size = viewSize;
        scratch->payload = view.payload;
        songCount++;
    }
    if (songCount != EMERALD_AUDIO_SONG_COUNT)
    {
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "%zu songs", songCount);
        NoteFailure(diagnostics, "build", buffer,
                    GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                    EMERALD_AUDIO_SONG_COUNT, (uint32_t)songCount, NULL);
        result = EMERALD_AUDIO_ERR_UNEXPECTED_COUNT;
        goto done;
    }
    /* Tiling proof: the sorted records must start exactly at the block
     * offset, continue back-to-back, and end exactly at the block end. */
    qsort(songScratch, songCount, sizeof(*songScratch), CompareSongScratch);
    {
        uint64_t expected = EMERALD_AUDIO_SONG_BLOCK_OFFSET;
        for (i = 0; i < songCount; i++)
        {
            if (songScratch[i].rec.arenaOffset != expected)
            {
                char buffer[160];
                snprintf(buffer, sizeof(buffer), "%s @ %u (expected %llx)",
                         songScratch[i].rec.canonicalName,
                         songScratch[i].rec.arenaOffset,
                         (unsigned long long)expected);
                NoteFailure(diagnostics, "build", buffer,
                            GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE,
                            GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                            EMERALD_AUDIO_SONG_BLOCK_SIZE,
                            (uint32_t)songScratch[i].rec.size, NULL);
                result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
                goto done;
            }
            expected += songScratch[i].rec.size;
        }
        if (expected
            != (uint64_t)EMERALD_AUDIO_SONG_BLOCK_OFFSET
               + EMERALD_AUDIO_SONG_BLOCK_SIZE)
        {
            NoteFailure(diagnostics, "build", "song block end",
                        GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                        EMERALD_AUDIO_SONG_BLOCK_SIZE,
                        (uint32_t)(expected - EMERALD_AUDIO_SONG_BLOCK_OFFSET),
                        NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
            goto done;
        }
    }

    /* Phase 2: one allocation for the header + leaf table + structural
     * table + song table + verbatim zone + transformed zone. */
    if (SIZE_MAX - sizeof(struct EmeraldAudioArena) < zoneOffset
     || SIZE_MAX - sizeof(struct EmeraldAudioArena) - zoneOffset
            < EMERALD_AUDIO_SPAN_SIZE
     || SIZE_MAX - sizeof(struct EmeraldAudioArena) - zoneOffset
            - EMERALD_AUDIO_SPAN_SIZE < EMERALD_AUDIO_TRANSFORM_SIZE)
    {
        NoteFailure(diagnostics, "build", NULL,
                    GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
        goto done;
    }
    arena = (struct EmeraldAudioArena *)malloc(
        sizeof(struct EmeraldAudioArena) + zoneOffset
        + EMERALD_AUDIO_SPAN_SIZE + EMERALD_AUDIO_TRANSFORM_SIZE);
    if (arena == NULL)
    {
        NoteFailure(diagnostics, "build", NULL,
                    GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        result = EMERALD_AUDIO_ERR_OUT_OF_MEMORY;
        goto done;
    }
    memset(arena, 0, sizeof(*arena) + zoneOffset + EMERALD_AUDIO_SPAN_SIZE
           + EMERALD_AUDIO_TRANSFORM_SIZE);
    arena->spanSize = EMERALD_AUDIO_SPAN_SIZE;
    arena->publishedCount = count;
    arena->structuralCount = structuralCount;
    arena->publishedSongCount = songCount;
    arena->payloadBytes = payloadBytes;
    arena->leafTableOffset = 0u;
    arena->structuralTableOffset = recordBytes;
    arena->songTableOffset = recordBytes + structRecordBytes;
    arena->zoneOffset = zoneOffset;
    arena->transformOffset = transformOffset;
    arena->transformSize = EMERALD_AUDIO_TRANSFORM_SIZE;
    memcpy(arena->bytes, records, recordBytes);
    for (i = 0; i < count; i++)
    {
        const struct EmeraldAudioLeafRecord *record = &records[i];
        memcpy(arena->bytes + arena->zoneOffset + record->arenaOffset,
               payloads[i], record->size);
    }
    /* R12-D: the song record table + the canonical GBA-form payloads, in
     * the phase-1c sorted order (tiled across the song block). The scratch
     * struct embeds a payload pointer after the record, so the table copy
     * is per-record, never a whole-scratch memcpy. */
    for (i = 0; i < songCount; i++)
    {
        memcpy(arena->bytes + arena->songTableOffset
                   + i * sizeof(struct EmeraldAudioSongRecord),
               &songScratch[i].rec, sizeof(struct EmeraldAudioSongRecord));
        memcpy(arena->bytes + arena->zoneOffset + songScratch[i].rec.arenaOffset,
               songScratch[i].payload, songScratch[i].rec.size);
    }
    {
        const uint8_t *zoneBase = arena->bytes + arena->zoneOffset;
        const uint8_t *transformBase = arena->bytes + arena->transformOffset;
        size_t transformPos = 0u;

        /* Pass 1: assign ALL structural offsets first - keysplit verbatim
         * copies + offsets, then every block's transform offset - so a row
         * whose group pointer targets a LATER block resolves correctly.
         * (Probe order never mattered; offsets are address-agnostic.) */
        for (i = 0; i < structuralCount; i++)
        {
            struct EmeraldAudioStructuralRecord *record = &structRecords[i];
            const struct Gen3ResourcePackEntry *entry;
            if (record->kind == STRUCTURAL_KEYSPLIT)
            {
                uint64_t romAddr;
                entry = Gen3ResourcePack_FindByCanonicalName(
                    pack, record->canonicalName);
                romAddr = (uint64_t)entry->sourceRomOffset
                        + EMERALD_AUDIO_GBA_ROM_BASE;
                record->verbatimOffset =
                    (uint32_t)(romAddr - EMERALD_AUDIO_ROM_START);
                memcpy((uint8_t *)zoneBase + record->verbatimOffset,
                       entry->payload, entry->payloadSize);
                continue;
            }
            entry = Gen3ResourcePack_FindByCanonicalName(
                pack, record->canonicalName);
            /* The label = rowsStart - N*24: the back-shift pad occupies
             * [transformOffset, transformOffset + N*24) so the label lands
             * on the pad start (docs/R12C §1.2, m4a.inc back-shift). */
            record->transformOffset = (uint32_t)transformPos;
            if (record->backshiftRows != 0u)
                transformPos += (size_t)record->backshiftRows * 24u;
            transformPos += (size_t)record->rowCount * 24u;
        }
        if (transformPos != EMERALD_AUDIO_TRANSFORM_SIZE)
        {
            NoteFailure(diagnostics, "build", NULL,
                        GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                        GEN3_RESOURCE_TYPE_INVALID, 1u, 0u,
                        EMERALD_AUDIO_TRANSFORM_SIZE,
                        (uint32_t)transformPos, NULL);
            result = EMERALD_AUDIO_ERR_UNEXPECTED_COMPOSITION;
            goto done;
        }

        /* Pass 2: transform every row against the real bases (all offsets
         * now valid). Phase 1c proved every pointer resolves; a failure
         * here frees the arena and leaves nothing published. */
        for (i = 0; i < structuralCount; i++)
        {
            struct EmeraldAudioStructuralRecord *record = &structRecords[i];
            const struct Gen3ResourcePackEntry *entry;
            size_t row;
            if (record->kind == STRUCTURAL_KEYSPLIT)
                continue;
            entry = Gen3ResourcePack_FindByCanonicalName(
                pack, record->canonicalName);
            for (row = 0; row < record->rowCount; row++)
            {
                if (!TransformRow(
                        entry->payload + row * 12u,
                        (uint8_t *)transformBase + record->transformOffset
                            + (size_t)record->backshiftRows * 24u
                            + row * 24u,
                        records, count, zoneBase,
                        structRecords, structuralCount, transformBase))
                {
                    char where[160];
                    snprintf(where, sizeof(where), "%s row %zu",
                             record->canonicalName, row);
                    NoteFailure(diagnostics, "transform", where,
                                GEN3_RESOURCE_TYPE_INSTRUMENT_BANK,
                                entry->type, entry->schema, entry->schema,
                                record->rowCount, 0u, NULL);
                    result = EMERALD_AUDIO_ERR_UNRESOLVED_POINTER;
                    goto done;
                }
            }
        }
        /* Copy the finalized structural table in (verbatim/transform
         * offsets were filled in place). */
        memcpy(arena->bytes + recordBytes, structRecords, structRecordBytes);
    }

    /* Publish atomically: only a fully built arena replaces the old one.
     * The old arena's ranges must leave the shared index before the old
     * arena is freed (they point at it); the new arena's ranges register
     * after the swap. */
    UnregisterArenaRanges();
    if (sArena != NULL)
    {
        free(sArena);
        sArena = NULL;
    }
    sArena = arena;
    arena = NULL;
    /* Register the logical table labels (R12-C §5): 195 voicegroup + 2 cry
     * table starts resolve to the arena's native rows from now on. The table
     * is cleared first - a re-publish swaps the arena, so hosts change. */
    if (HostMemoryClearLogicalAddresses != NULL)
        HostMemoryClearLogicalAddresses();
    EmeraldAudioCompat_ForEachLogicalLabel(RegisterLogicalLabel, NULL);
    /* R12-D §5: ONE interval for the contiguous song block - every song
     * address in [0x088FC03C, 0x089A3050) resolves identity-preservingly to
     * the arena's canonical copy. Cleared + re-registered with every
     * publish/republish (the arena base changes on a swap). */
    if (HostMemoryClearLogicalRanges != NULL)
        HostMemoryClearLogicalRanges();
    if (HostMemoryRegisterLogicalRange != NULL)
    {
        const uint8_t *zoneBase = sArena->bytes + sArena->zoneOffset;
        HostMemoryRegisterLogicalRange(
            EMERALD_AUDIO_SONG_BLOCK_START, EMERALD_AUDIO_SONG_BLOCK_END,
            (void *)(zoneBase + EMERALD_AUDIO_SONG_BLOCK_OFFSET));
    }
    /* R12-C §8: register the arena's spans + hull into the shared R10
     * range index (mid-play saves capture arena pointers as sidecar
     * records). Failure is a degrade, not a publish failure: the arena is
     * live and consumers are unaffected; the state walker then fails
     * closed on arena pointers (no range, no hull) instead of persisting
     * them. Offline links without the trainer seam (weak GetRangeIndex)
     * always take this path. */
    (void)RegisterArenaRanges();
    NoteFailure(diagnostics, "publish", NULL, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, (uint32_t)payloadBytes,
                0u, NULL);
    result = EMERALD_AUDIO_OK;

done:
    if (records != NULL)
        free(records);
    if (structRecords != NULL)
        free(structRecords);
    if (songScratch != NULL)
        free(songScratch);
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
    /* The arena (and therefore every label host) is unchanged by a
     * republish, but the table is cleared + repopulated anyway (R12-C §5.2)
     * so the two can never drift. Allocation-free, fixed capacity. */
    if (HostMemoryClearLogicalAddresses != NULL)
        HostMemoryClearLogicalAddresses();
    EmeraldAudioCompat_ForEachLogicalLabel(RegisterLogicalLabel, NULL);
    /* R12-D §5: re-register the song interval with the CURRENT arena base
     * (a post-load trainer republish may have swapped the arena). */
    if (HostMemoryClearLogicalRanges != NULL)
        HostMemoryClearLogicalRanges();
    if (HostMemoryRegisterLogicalRange != NULL)
    {
        const uint8_t *zoneBase = sArena->bytes + sArena->zoneOffset;
        HostMemoryRegisterLogicalRange(
            EMERALD_AUDIO_SONG_BLOCK_START, EMERALD_AUDIO_SONG_BLOCK_END,
            (void *)(zoneBase + EMERALD_AUDIO_SONG_BLOCK_OFFSET));
    }
    /* R12-C §8: re-register the spans + hull (a post-load trainer republish
     * may have reset the shared index, dropping our ranges - the
     * verify/re-register logic handles both that and the already-registered
     * case). Failure fails closed: the caller clears the arena, so no live
     * arena can ever coexist with missing ranges. */
    if (!RegisterArenaRanges())
    {
        NoteFailure(diagnostics, "republish", NULL,
                    GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
                    GEN3_RESOURCE_TYPE_INVALID, 1u, 0u, 0u, 0u, NULL);
        return EMERALD_AUDIO_ERR_RANGE_REGISTRATION;
    }
    return EMERALD_AUDIO_OK;
}

void EmeraldAudioCompat_ClearMigratedEntries(void)
{
    /* The ranges must die before the arena they point at. */
    UnregisterArenaRanges();
    if (HostMemoryClearLogicalAddresses != NULL)
        HostMemoryClearLogicalAddresses();
    if (HostMemoryClearLogicalRanges != NULL)
        HostMemoryClearLogicalRanges();
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
    *outSize = sArena->spanSize + sArena->transformSize;
    return true;
}

bool EmeraldAudioCompat_GetArenaLayout(size_t *outZoneOffset,
                                       size_t *outTransformOffset,
                                       size_t *outSpanSize,
                                       size_t *outTransformSize)
{
    if (outZoneOffset != NULL)
        *outZoneOffset = 0u;
    if (outTransformOffset != NULL)
        *outTransformOffset = 0u;
    if (outSpanSize != NULL)
        *outSpanSize = 0u;
    if (outTransformSize != NULL)
        *outTransformSize = 0u;
    if (sArena == NULL)
        return false;
    if (outZoneOffset != NULL)
        *outZoneOffset = sArena->zoneOffset;
    if (outTransformOffset != NULL)
        *outTransformOffset = sArena->transformOffset;
    if (outSpanSize != NULL)
        *outSpanSize = sArena->spanSize;
    if (outTransformSize != NULL)
        *outTransformSize = sArena->transformSize;
    return true;
}

/* R12-E canary predicates: arena-residency bounds checks against the
 * published arena. ContainsPointer spans the whole allocation view
 * (verbatim zone + transformed zone, contiguous from the zone base);
 * ContainsCanonicalPointer / ContainsTransformedPointer test the individual
 * zones (song headers/leaves/keysplits are verbatim; voicegroup/cry rows and
 * the transformed song rows are in the transformed zone). */
bool EmeraldAudioCompat_ContainsPointer(uintptr_t address)
{
    const uint8_t *base;
    size_t size;
    if (!EmeraldAudioCompat_GetArena(&base, &size))
        return false;
    return address >= (uintptr_t)base
        && address < (uintptr_t)base + size;
}

bool EmeraldAudioCompat_ContainsCanonicalPointer(uintptr_t address)
{
    const uint8_t *base;
    size_t size;
    if (!EmeraldAudioCompat_GetArena(&base, &size))
        return false;
    return address >= (uintptr_t)base
        && address < (uintptr_t)base + size
        && address - (uintptr_t)base < sArena->spanSize;
}

bool EmeraldAudioCompat_ContainsTransformedPointer(uintptr_t address)
{
    size_t zoneOffset;
    size_t transformOffset;
    size_t spanSize;
    size_t transformSize;
    if (sArena == NULL)
        return false;
    if (!EmeraldAudioCompat_GetArenaLayout(&zoneOffset, &transformOffset,
                                           &spanSize, &transformSize))
        return false;
    return address >= (uintptr_t)(sArena->bytes + transformOffset)
        && address < (uintptr_t)(sArena->bytes + transformOffset
                                 + transformSize);
}

size_t EmeraldAudioCompat_GetPublishedCount(void)
{
    return sArena != NULL ? sArena->publishedCount : 0u;
}

size_t EmeraldAudioCompat_GetStructuralCount(void)
{
    return sArena != NULL ? sArena->structuralCount : 0u;
}

bool EmeraldAudioCompat_GetTransformedRows(const uint8_t **outBase,
                                           size_t *outSize)
{
    if (outBase != NULL)
        *outBase = NULL;
    if (outSize != NULL)
        *outSize = 0u;
    if (sArena == NULL || outBase == NULL || outSize == NULL)
        return false;
    *outBase = sArena->bytes + sArena->transformOffset;
    *outSize = sArena->transformSize;
    return true;
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

static const struct EmeraldAudioStructuralRecord *
FindStructuralRecord(const char *canonicalName)
{
    size_t i;
    if (sArena == NULL || canonicalName == NULL)
        return NULL;
    for (i = 0; i < sArena->structuralCount; i++)
    {
        const struct EmeraldAudioStructuralRecord *record =
            (const struct EmeraldAudioStructuralRecord *)(const void *)
                (sArena->bytes + sArena->structuralTableOffset
                 + i * sizeof(struct EmeraldAudioStructuralRecord));
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

size_t EmeraldAudioCompat_GetSongCount(void)
{
    return sArena != NULL ? sArena->publishedSongCount : 0u;
}

bool EmeraldAudioCompat_GetSongSpan(const char *canonicalName,
                                    size_t *outArenaOffset, size_t *outSize)
{
    size_t i;

    if (sArena == NULL || canonicalName == NULL)
        return false;
    for (i = 0; i < sArena->publishedSongCount; i++)
    {
        const struct EmeraldAudioSongRecord *record =
            (const struct EmeraldAudioSongRecord *)(const void *)
                (sArena->bytes + sArena->songTableOffset
                 + i * sizeof(struct EmeraldAudioSongRecord));
        if (strcmp(record->canonicalName, canonicalName) == 0)
        {
            if (outArenaOffset != NULL)
                *outArenaOffset = record->arenaOffset;
            if (outSize != NULL)
                *outSize = record->size;
            return true;
        }
    }
    return false;
}

bool EmeraldAudioCompat_GetVoicegroupSpan(const char *canonicalName,
                                          size_t *outTransformOffset,
                                          size_t *outRowCount)
{
    const struct EmeraldAudioStructuralRecord *record =
        FindStructuralRecord(canonicalName);
    if (record == NULL || record->kind != STRUCTURAL_VOICEGROUP)
        return false;
    if (outTransformOffset != NULL)
        *outTransformOffset = record->transformOffset;
    if (outRowCount != NULL)
        *outRowCount = record->rowCount;
    return true;
}

bool EmeraldAudioCompat_GetCryTableSpan(bool reversed,
                                        size_t *outTransformOffset,
                                        size_t *outRowCount)
{
    size_t i;
    if (sArena == NULL)
        return false;
    for (i = 0; i < sArena->structuralCount; i++)
    {
        const struct EmeraldAudioStructuralRecord *record =
            (const struct EmeraldAudioStructuralRecord *)(const void *)
                (sArena->bytes + sArena->structuralTableOffset
                 + i * sizeof(struct EmeraldAudioStructuralRecord));
        if (record->kind == STRUCTURAL_CRY_TABLE
         && (record->sub == 1u) == reversed)
        {
            if (outTransformOffset != NULL)
                *outTransformOffset = record->transformOffset;
            if (outRowCount != NULL)
                *outRowCount = record->rowCount;
            return true;
        }
    }
    return false;
}

bool EmeraldAudioCompat_GetKeysplitSpan(const char *canonicalName,
                                        size_t *outVerbatimOffset,
                                        size_t *outRunBytes)
{
    const struct EmeraldAudioStructuralRecord *record =
        FindStructuralRecord(canonicalName);
    if (record == NULL || record->kind != STRUCTURAL_KEYSPLIT)
        return false;
    if (outVerbatimOffset != NULL)
        *outVerbatimOffset = record->verbatimOffset;
    if (outRunBytes != NULL)
        *outRunBytes = record->payloadBytes;
    return true;
}

const struct EmeraldAudioToneRow *
EmeraldAudioCryTableRow(uint8_t table, bool reversed, uint8_t index)
{
    size_t off;
    if (sArena == NULL)
        return NULL;
    off = (size_t)table * 128u + index;
    if (off >= EMERALD_AUDIO_CRY_ROWS / 2u)
        return NULL;
    {
        size_t i;
        for (i = 0; i < sArena->structuralCount; i++)
        {
            const struct EmeraldAudioStructuralRecord *record =
                (const struct EmeraldAudioStructuralRecord *)(const void *)
                    (sArena->bytes + sArena->structuralTableOffset
                     + i * sizeof(struct EmeraldAudioStructuralRecord));
            if (record->kind == STRUCTURAL_CRY_TABLE
             && (record->sub == 1u) == reversed)
                return (const struct EmeraldAudioToneRow *)(const void *)
                    (sArena->bytes + sArena->transformOffset
                     + record->transformOffset + off * 24u);
        }
    }
    return NULL;
}

void EmeraldAudioCompat_ForEachLogicalLabel(
    EmeraldAudioLogicalLabelCallback callback, void *user)
{
    size_t i;
    if (sArena == NULL || callback == NULL)
        return;
    for (i = 0; i < sArena->structuralCount; i++)
    {
        const struct EmeraldAudioStructuralRecord *record =
            (const struct EmeraldAudioStructuralRecord *)(const void *)
                (sArena->bytes + sArena->structuralTableOffset
                 + i * sizeof(struct EmeraldAudioStructuralRecord));
        if (record->kind == STRUCTURAL_VOICEGROUP
         || record->kind == STRUCTURAL_CRY_TABLE)
        {
            callback(record->labelRomAddr,
                     (const void *)(sArena->bytes + sArena->transformOffset
                                    + record->transformOffset),
                     user);
        }
    }
}

/* ForEachLogicalLabel callback: publish one exact-start entry (R12-C §5). */
static void RegisterLogicalLabel(uint32_t gbaAddr, const void *hostBase,
                                 void *user)
{
    (void)user;
    if (HostMemoryRegisterLogicalAddress != NULL)
        HostMemoryRegisterLogicalAddress(gbaAddr, (void *)hostBase);
}
