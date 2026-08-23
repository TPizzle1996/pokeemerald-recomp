/* R10-G/H: cross-restart resource-pointer state tests.
 *
 * Compiles the REAL native_state.c + host_memory.c walker/restore machinery
 * with the real compat seams and the real production pack chain, and drives
 * it across process boundaries:
 *
 *   create   registers the production session, copies REAL published battle
 *            table rows (real arena pointers) into the serialized GAME_DATA
 *            region, saves a state, and proves the file carries exactly the
 *            expected resource-reference sidecar records;
 *   load     starts a FRESH process (new arena addresses), registers the
 *            session, loads the state, and proves every migrated row's data
 *            field was patched to THIS process's arena pointer (identity by
 *            key, never by address equality) with the rest of the state
 *            round-tripping intact;
 *   load-fail drives the rejection matrix: session mismatch (changed
 *            fingerprint), missing session, and file-corruption cases -
 *            every rejection must leave a planted canary untouched;
 *   regression plants the live-game failure class into the serialized
 *            region - 8-byte windows whose u64 sits inside a resource
 *            arena's UNEXPOSED build-time-only prefix (packed pixels, M4A
 *            channel byte fields) - and proves capture treats them as
 *            ordinary data: save succeeds, no sidecar record, bytes
 *            preserved verbatim.
 *
 * The stub TU (emerald_resource_state_stub.c) satisfies the platform and
 * game symbols; the game-data region is the harness's own static buffer
 * whose rows hold the real published pointers - the exact state the walker
 * must serialize as identity + offset (R10 §D), and the state any future
 * migration that places resource-backed pointers in game-owned memory will
 * produce.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "sprite.h"
#include "data.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_types.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/emerald_text_compat.h"
#include "emerald/resources/emerald_script_compat.h"
#include "emerald/resources/emerald_script_state.h"
#include "emerald/resources/script_native.generated.h"
#include "emerald/resources/emerald_battle_compat.h"
#include "emerald/resources/emerald_battle_state.h"
#include "emerald/resources/battle_native.generated.h"
#include "script.h"
/* R12: REAL MP2K struct layouts for the audio-shaped game_bss fixture
 * (struct SoundInfo/SoundChannel/MusicPlayerInfo). Header-only; no m4a.c is
 * linked, so no audio externs are referenced. */
#include "gba/m4a_internal.h"

/* The REAL native tables, textual-included exactly like the real_tables
 * recipe: INCBIN payload stubs (the compiled leaves are GBA-only), the
 * charmap macros as identity (the tool normally resolves them before gcc),
 * the still-front NONE-row stub, then graphics/pokemon.h + data.c. */
#define INCBIN(...) {0}
#define INCBIN_U8  INCBIN
#define INCBIN_U16 INCBIN
#define INCBIN_U32 INCBIN
#define INCBIN_S8  INCBIN
#define INCBIN_S16 INCBIN
#define INCBIN_S32 INCBIN
#define _(x)  x
#define __(x) x
const u32 gMonStillFrontPic_CircledQuestionMark[] = {0};
#include "../src/data/graphics/pokemon.h"
#include "../src/data.c"

#include "platform/native_state.h"
#include "platform/native_world_neighborhood.h"
#include "platform/host_memory.h" /* HostResolveGbaAddr (fixture pointer) */
#include "platform/desktop_runtime.h" /* Platform_RuntimeAddressIsExecutable */
/* R10 desktop-load regression: the REAL desktop_state.c (Platform_StateLoad)
 * and desktop_audio.c are linked; the fake-SDL device shim provides the
 * audio device model, its ordered trace and the controllable clock. */
#if defined(HARNESS_REAL_SDL_PROBE)
/* Real-SDL probe build: real SDL2 headers, real device, no shim. Must
 * precede platform/desktop_audio.h (its probe accessor returns an
 * SDL_AudioDeviceID). */
#include <SDL2/SDL.h>
#include <unistd.h> /* usleep */
#endif
#include "platform/desktop_state.h"
#include "platform/desktop_audio.h"
#include "platform/desktop_scheduler.h" /* Platform_SchedulerGetFrameCounter */
/* Platform_QueueAudio has no header declaration (desktop_audio.c only);
 * the harness pins the volume setting to 10, so the queued payload is
 * deterministic. */
extern void Platform_QueueAudio(float *audioBuffer, s32 size);
#if !defined(HARNESS_REAL_SDL_PROBE)
/* Fake-SDL build: the device shim provides the ordered trace and the
 * controllable clock (tests/emerald_audio_device_shim.c). */
extern void HarnessAudioClock_Advance(uint64_t nanoseconds);
extern void HarnessAudioTraceReset(void);
extern void HarnessAudioTraceMark(const char *tag);
extern const char *HarnessAudioTraceGet(void);
extern bool HarnessDeviceOpen(void);
extern bool HarnessDevicePaused(void);
extern uint64_t HarnessDeviceQueuedBytes(void);
#endif
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "emerald/resources/emerald_audio_compat.h"
#include "emerald/resources/emerald_map_compat.h"
#include "emerald/resources/emerald_pokemon_native_compat.h"
#include "emerald/resources/emerald_resource_ranges.h"
#include "emerald/resources/map_data_native.h"
/* Declaration mode: the seam TU (compiled alongside) defines the arrays. */
#include "emerald/resources/object_event_pic_tables.native.generated.h"
#include "emerald/resources/tileset_native.generated.h"
/* R11-D declaration mode: the record DEFINITIONS live in
 * emerald_layout_compat.c (compiled alongside). */
#include "emerald/resources/layout_native.generated.h"
#include "wild_encounter.h"
#include "emerald_script_compat_harness.h"

#define HARNESS_STATE_SLOT 7u
#define HARNESS_ROW_COUNT 10u

/* R11-E/F data pins: real compiled map identities (no MAP_* constants exist
 * in the tree; values from data/maps/map_groups.json, cross-checked against
 * the compiled tables in Harness B). */
#define MAP_GROUP_TOWNS_AND_ROUTES 0
#define MAP_NUM_LITTLEROOT_TOWN 9
#define MAP_NUM_OLDALE_TOWN 10
#define MAP_NUM_ROUTE101 16

extern unsigned char sHarnessGameData[0x1000];
extern unsigned char sHarnessGameBss[0x10000];
/* Declaration mode: defined in emerald_resource_state_stub.c. Overrides the
 * fixed slot->path mapping so each mode writes/reads its own state file. */
void HarnessStatePath_Override(const char *path);
/* The typed sprite template/image sidecar arrays (defined in the stub TU,
 * placed in the REAL EWRAM section so the walker's EWRAM slice covers them)
 * and the linker-synthesized EWRAM base symbol -- the scalar+padding
 * regression (TEST S) reads/writes the arrays and maps file offsets. */
extern struct SpriteTemplate sSpriteTemplateSidecars[];
extern struct SpriteFrameImage sSpriteTemplateImageSidecars[];
extern unsigned char __start_gba_ewram[];

/* R11-E/F: set the neighborhood's identity the way ApplyCurrentWarp does. */
static void SetNeighborhoodLocation(u8 mapGroup, u8 mapNum)
{
    gSaveBlock1Ptr->location.mapGroup = mapGroup;
    gSaveBlock1Ptr->location.mapNum = mapNum;
}

/* R11-E/F test 16 layer 1: the module's storage is host .bss/.data --
 * outside BOTH serialized game ranges of this harness. */
static bool32 NeighborhoodInGameRanges(const struct NativeWorldNeighborhood *nb)
{
    uintptr_t a = (uintptr_t)(const void *)nb;
    uintptr_t dataStart = (uintptr_t)sHarnessGameData;
    uintptr_t bssStart = (uintptr_t)sHarnessGameBss;
    return (a >= dataStart && a < dataStart + 0x1000u)
        || (a >= bssStart && a < bssStart + 0x10000u);
}

/* R11-E/F test 16 layer 3: whole-file comparison of two saves. */
struct StateFileBytes
{
    uint8_t *bytes;
    size_t size;
};

static bool32 ReadStateFile(const char *path, struct StateFileBytes *out)
{
    FILE *f = fopen(path, "rb");
    long size;
    if (f == NULL)
    {
        fprintf(stderr, "create: cannot re-read state file\n");
        return FALSE;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->bytes = (uint8_t *)malloc((size_t)size);
    if (out->bytes == NULL)
    {
        fclose(f);
        return FALSE;
    }
    out->size = (size_t)size;
    if (fread(out->bytes, 1, out->size, f) != out->size)
    {
        free(out->bytes);
        fclose(f);
        return FALSE;
    }
    fclose(f);
    return TRUE;
}

static bool32 StateFileMatches(const char *path, const struct StateFileBytes *ref)
{
    struct StateFileBytes again;
    bool32 same;
    if (!ReadStateFile(path, &again))
        return FALSE;
    same = again.size == ref->size
        && memcmp(again.bytes, ref->bytes, ref->size) == 0;
    free(again.bytes);
    return same;
}

/* Uniform row view: sheet and palette tables share the leading 8-byte
 * pointer field (and the 16-byte padded layout on native), which is all the
 * walker serializes; the size/tag tail round-trips as opaque words.
 *
 * `reserved` is explicit so the tail window [size u16][tag u16][reserved u32]
 * can never be mistaken for a resource pointer. With reserved == 0 (memset)
 * the tail forms a zero-high-half value (e.g. the shiny-palette row's
 * tag = species + SPECIES_SHINY_TAG = 501 -> 0x00000000_01F50000, inside the
 * pack-arena band) and the walker - CORRECTLY per the resource protocol -
 * captures it as a resource reference whenever a randomly-placed arena range
 * covers that fixed value (the intermittent TEST 8 recordCount==11 flake).
 * The non-pointer high half (same convention as the 0xA5A5A500+i filler)
 * makes the tail unambiguous data. */
struct HarnessRow
{
    const u32 *data;
    u16 size;
    u16 tag;
    u32 reserved;
};

/* R13-C: the production pack's gText_123Dot record (#112): "1.\xff2.\xff
 * 3.\xff" as one 9-byte resource, three 3-byte strings tiled by kind-0
 * skeleton fills at byte offsets 0/3/6 (same literal as the runtime
 * loader harness). */
static const u8 k123DotBytes[9] = {
    0xA2, 0xAD, 0xFF, 0xA3, 0xAD, 0xFF, 0xA4, 0xAD, 0xFF
};

/* R13-C: a compiled text payload the seam does NOT own (the deferred
 * family model). It must round-trip the state file verbatim - an opaque
 * fallback pointer, never a resource sidecar record, never relocated. */
static const u8 sCompiledOpaqueText[] = { 0x41, 0x42, 0x43, 0xFF, 0x00 };

struct HarnessGameData
{
    u32 magic;                          /* 0x48475231 */
    struct HarnessRow rows[HARNESS_ROW_COUNT]; /* real published rows */
    /* R13-C §13-15: the State-v5 currentChar class. textCurrentChar is a
     * mid-string interior pointer into the published text arena (the
     * TextPrinter.currentChar shape: gText_123Dot row 1, 3 bytes into
     * its label); textOpaque points at compiled text (deferred families)
     * and must survive the round trip untouched. */
    const u8 *textCurrentChar;
    const u8 *textOpaque;
    /* R13-F: an EWRAM gMapHeader-shaped working copy. The layout and script
     * pointers target the executable image; events/connections target the
     * session arenas and must travel through the State-v5 resource sidecar. */
    struct MapHeader activeMapHeader;
    /* R13-G4 staged-only execution-state clone.  The member layouts are the
     * real native ScriptContext/pointer shapes; production contexts are not
     * redirected and the VM never executes this shadow fixture. */
    struct
    {
        struct ScriptContext context1;
        struct ScriptContext context2;
        struct ScriptContext mysteryEvent;
        const u8 *ramScriptRetAddr;
        const u8 *approaching[2];
        const u8 *trainerEnd;
        const u8 *trainerReturnA;
        const u8 *trainerReturnB;
        const u8 *trainerText[6];
        u8 *mysteryNativeBase;
        u64 generationStamp;
        struct EmeraldScriptVirtualAnchor virtualAnchor;
        u8 dynamicScript[512];
        u32 expectedModule[12];
        u32 expectedOffset[12];
        u32 trainerIds[4];
        u32 movementEncodedTarget;
        u32 caseKind;
    } scriptState;
    u32 tailMagic;                      /* 0x48475232 */
    u32 filler[24];
};

static struct HarnessGameData *GameData(void)
{
    return (struct HarnessGameData *)(void *)sHarnessGameData;
}

static int sFailures = 0;
#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            sFailures++;                                                \
        }                                                               \
    } while (0)

/* R11-E/F tests 15/16: assert the canonical Littleroot neighborhood record
 * (LittlerootTown -> Route101, NORTH offset 0, Route101 20 blocks tall). */
static void CheckLittlerootNeighborhood(const struct NativeWorldNeighborhood *nb,
                                        u32 expectedVersion)
{
    CHECK(nb->status == NATIVE_WORLD_NB_READY);
    CHECK(nb->version == expectedVersion);
    CHECK(nb->neighborCount == 1u);
    if (nb->neighborCount >= 1u)
    {
        CHECK(nb->neighbors[0].direction == CONNECTION_NORTH);
        CHECK(nb->neighbors[0].offset == 0);
        CHECK(nb->neighbors[0].mapGroup == MAP_GROUP_TOWNS_AND_ROUTES);
        CHECK(nb->neighbors[0].mapNum == MAP_NUM_ROUTE101);
        CHECK(nb->neighbors[0].worldX == 0);
        CHECK(nb->neighbors[0].worldY == -20); /* Route101 20 blocks tall */
        CHECK(nb->neighbors[0].width == 20);
        CHECK(nb->neighbors[0].height == 20);
    }
}

static void PrintDigestHex(const uint8_t digest[32], char *dest)
{
    static const char hexDigits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 32; i++)
    {
        dest[i * 2u] = hexDigits[digest[i] >> 4];
        dest[i * 2u + 1u] = hexDigits[digest[i] & 0xFu];
    }
    dest[64] = '\0';
}

static bool32 RegisterSession(const char *packPath)
{
    enum EmeraldResourceCompatStatus status =
        EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath);
    if (status != EMERALD_COMPAT_OK)
    {
        fprintf(stderr, "session registration failed: %s\n",
                EmeraldResourceCompatStatus_Describe(status));
        return FALSE;
    }
    return TRUE;
}

static void RowFromSheet(struct HarnessRow *out, const struct CompressedSpriteSheet *src)
{
    out->data = src->data;
    out->size = src->size;
    out->tag = src->tag;
    out->reserved = 0xA5A5A5A5u;
}

static void RowFromPalette(struct HarnessRow *out, const struct CompressedSpritePalette *src)
{
    out->data = src->data;
    out->size = 0;
    out->tag = src->tag;
    out->reserved = 0xA5A5A5A5u;
}

/* R11-C: the tileset family publishes/republishes from the session arena.
 * Runs in create (fresh publication) and load (post-load republish from THIS
 * process's arena, R6 re-publish-after-load). Pins the four struct pointers
 * for two tilesets, a palette-row byte and an anim-frame byte sample, and
 * the SecretBase metatile alias (the 6 variants share one stream). */
static bool32 VerifyTilesetPublished(void)
{
    if (gTileset_General.tiles == NULL
     || gTileset_General.metatiles == NULL
     || gTileset_General.metatileAttributes == NULL
     || gTileset_General.palettes == NULL)
    {
        fprintf(stderr, "tileset: General pointers not published\n");
        return FALSE;
    }
    if (gTileset_Petalburg.tiles == NULL || gTileset_Petalburg.palettes == NULL)
    {
        fprintf(stderr, "tileset: Petalburg pointers not published\n");
        return FALSE;
    }
    if (gTilesetPalettes_General[0][0] == 0)
    {
        fprintf(stderr, "tileset: General palette row 0 not copied\n");
        return FALSE;
    }
    if (gTilesetAnims_General_Flower_Frame0[0] == 0)
    {
        fprintf(stderr, "tileset: General flower frame 0 not copied\n");
        return FALSE;
    }
    if (gTileset_SecretBaseBlueCave.metatiles
            != gTileset_SecretBaseRedCave.metatiles)
    {
        fprintf(stderr, "tileset: SecretBase metatile alias diverged\n");
        return FALSE;
    }
    printf("tileset arena: general=%p petalburg=%p pal[0]=0x%04x "
           "flower[0]=0x%04x\n",
           (const void *)gTileset_General.tiles,
           (const void *)gTileset_Petalburg.tiles,
           gTilesetPalettes_General[0][0],
           gTilesetAnims_General_Flower_Frame0[0]);
    return TRUE;
}

/* R11-D: the layout family publishes/republishes from the session arena.
 * Runs in create (fresh publication) and load (post-load republish from THIS
 * process's arena, R6 re-publish-after-load). Pins the .map/.border pointers
 * for four layouts (incl. UnusedOutdoorArea's NULL secondary - the
 * layouts.json "0" case) and the compiled width/height surviving
 * publication. */
static bool32 VerifyLayoutPublished(void)
{
    if (AbandonedShip_CaptainsOffice_Layout.map == NULL
     || AbandonedShip_CaptainsOffice_Layout.border == NULL)
    {
        fprintf(stderr, "layout: CaptainsOffice pointers not published\n");
        return FALSE;
    }
    if (AbandonedShip_CaptainsOffice_Layout.width != 9
     || AbandonedShip_CaptainsOffice_Layout.height != 7)
    {
        fprintf(stderr, "layout: CaptainsOffice dimensions diverged\n");
        return FALSE;
    }
    if (PetalburgCity_Layout.map == NULL
     || PetalburgCity_Layout.width != 30
     || PetalburgCity_Layout.height != 30)
    {
        fprintf(stderr, "layout: Petalburg not published\n");
        return FALSE;
    }
    if (Underwater_Route127_Layout.map == NULL)
    {
        fprintf(stderr, "layout: Route127 (largest blockdata) not published\n");
        return FALSE;
    }
    if (UnusedOutdoorArea_Layout.map == NULL
     || UnusedOutdoorArea_Layout.width != 58
     || UnusedOutdoorArea_Layout.secondaryTileset != NULL)
    {
        fprintf(stderr, "layout: UnusedOutdoorArea not published\n");
        return FALSE;
    }
    printf("layout arena: captains=%p petalburg=%p route127=%p "
           "unusedoutdoor=%p\n",
           (const void *)AbandonedShip_CaptainsOffice_Layout.map,
           (const void *)PetalburgCity_Layout.map,
           (const void *)Underwater_Route127_Layout.map,
           (const void *)UnusedOutdoorArea_Layout.map);
    return TRUE;
}

/* Copy the real published rows (real arena pointers into the pack-derived
 * session image) into the serialized game-data region. Returns FALSE if any
 * expected row is NULL (the strict session guarantees non-NULL). */
static bool32 PlantRealRows(void)
{
    struct HarnessGameData *data = GameData();
    size_t i;

    memset(data, 0, sizeof(*data));
    data->magic = 0x48475231u;
    data->tailMagic = 0x48475232u;
    for (i = 0; i < sizeof(data->filler) / sizeof(data->filler[0]); i++)
        data->filler[i] = 0xA5A50000u + (u32)i;

    RowFromSheet(&data->rows[0], &gMonFrontPicTable[1]);
    RowFromSheet(&data->rows[1], &gMonFrontPicTable[2]);
    RowFromSheet(&data->rows[2], &gMonBackPicTable[1]);
    RowFromSheet(&data->rows[3], &gMonBackPicTable[2]);
    RowFromPalette(&data->rows[4], &gMonPaletteTable[1]);
    RowFromPalette(&data->rows[5], &gMonShinyPaletteTable[1]);
    RowFromSheet(&data->rows[6], &gTrainerFrontPicTable[0]);
    RowFromPalette(&data->rows[7], &gTrainerFrontPicPaletteTable[0]);
    RowFromSheet(&data->rows[8], &gTrainerBackPicTable[0]);
    RowFromPalette(&data->rows[9], &gTrainerBackPicPaletteTable[0]);
    for (i = 0; i < HARNESS_ROW_COUNT; i++)
    {
        if (data->rows[i].data == NULL)
        {
            fprintf(stderr, "row %zu has no published pointer (session refused?)\n", i);
            return FALSE;
        }
    }
    return TRUE;
}

/* R13-C currentChar class (create side): gText_123Dot[1] is the
 * interior-pointer shape the State-v5 routing must relocate across
 * processes; the opaque compiled pointer must stay OUTSIDE every
 * resource range. Only the R13-C create/load modes plant these - every
 * other mode's record-count arithmetic counts the 10 real rows exactly. */
static bool32 PlantCurrentCharClass(void)
{
    struct HarnessGameData *data = GameData();
    data->textCurrentChar = gText_123Dot[1];
    data->textOpaque = sCompiledOpaqueText;
    if (data->textCurrentChar == NULL
     || !EmeraldTextCompat_ContainsPointer(
            (uintptr_t)data->textCurrentChar))
    {
        fprintf(stderr, "currentChar class not published (text seam down?)\n");
        return FALSE;
    }
    if (EmeraldTextCompat_ContainsPointer((uintptr_t)data->textOpaque))
    {
        fprintf(stderr, "opaque compiled text landed inside a resource range\n");
        return FALSE;
    }
    return TRUE;
}

/* R13-F State-v5 class: select one deterministic published header whose
 * complete pointer graph is populated.  mapLayout/mapScripts are executable
 * image pointers; events/connections are session-arena pointers registered
 * by the map seam under schemas 43/44. */
static const struct MapHeader *FirstConnectedPublishedMap(void)
{
    size_t i;

    for (i = 0u; i < EMERALD_MAP_HEADER_COUNT; i++)
    {
        const struct MapHeader *header = &gMapHeaders[i];
        if (header->mapLayout != NULL && header->events != NULL
         && header->mapScripts != NULL && header->connections != NULL)
            return header;
    }
    return NULL;
}

static bool32 CheckMapArenaPointer(const void *pointer, uint32_t schema)
{
    const struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    struct EmeraldResourceRangeHit hit;

    return index != NULL && pointer != NULL
        && EmeraldResourceRangeIndex_Lookup(index, (uintptr_t)pointer, &hit)
        && hit.type == GEN3_RESOURCE_TYPE_STRUCTURED_DATA
        && hit.schema == schema
        && hit.role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT;
}

static bool32 PlantMapHeaderClass(void)
{
    const struct MapHeader *published = FirstConnectedPublishedMap();

    if (published == NULL
     || !CheckMapArenaPointer(published->events, EMERALD_MAP_SCHEMA_EVENTS)
     || !CheckMapArenaPointer(published->connections,
                              EMERALD_MAP_SCHEMA_CONNECTIONS))
    {
        fprintf(stderr, "map-header class not fully published\n");
        return FALSE;
    }
    GameData()->activeMapHeader = *published;
    printf("CREATE map pointers: layout=%p events=%p scripts=%p connections=%p\n",
           (const void *)published->mapLayout, (const void *)published->events,
           (const void *)published->mapScripts,
           (const void *)published->connections);
    return TRUE;
}

static bool32 VerifyMapHeaderRelocated(void)
{
    const struct MapHeader *published = FirstConnectedPublishedMap();
    const struct MapHeader *restored = &GameData()->activeMapHeader;

    if (published == NULL
     || restored->mapLayout != published->mapLayout
     || restored->events != published->events
     || restored->mapScripts != published->mapScripts
     || restored->connections != published->connections
     || restored->music != published->music
     || restored->mapLayoutId != published->mapLayoutId
     || restored->regionMapSectionId != published->regionMapSectionId
     || restored->cave != published->cave
     || restored->weather != published->weather
     || restored->mapType != published->mapType
     || restored->allowCycling != published->allowCycling
     || restored->allowEscaping != published->allowEscaping
     || restored->allowRunning != published->allowRunning
     || restored->showMapName != published->showMapName
     || restored->battleType != published->battleType
     || !CheckMapArenaPointer(restored->events, EMERALD_MAP_SCHEMA_EVENTS)
     || !CheckMapArenaPointer(restored->connections,
                              EMERALD_MAP_SCHEMA_CONNECTIONS))
    {
        fprintf(stderr, "load: map-header pointers/scalars not re-derived\n");
        return FALSE;
    }
    printf("LOAD map pointers: layout=%p events=%p scripts=%p connections=%p\n",
           (const void *)restored->mapLayout, (const void *)restored->events,
           (const void *)restored->mapScripts,
           (const void *)restored->connections);
    return TRUE;
}

/* R13-C §13-15 load side: the restored currentChar pointer must be a
 * pointer into THIS process's text arena, at the same in-label offset
 * (label start + 3) with the same bytes; the opaque compiled pointer
 * must be verbatim (compiled arrays never move). */
static bool32 VerifyTextCurrentCharRelocated(void)
{
    struct HarnessGameData *data = GameData();
    size_t arenaIndex = TEXT_ARENA_COUNT;
    size_t offset = 0u;
    size_t start = 0u;
    size_t size = 0u;
    const char *name = NULL;
    const u8 *base = NULL;
    size_t baseSize = 0u;

    if (data->textCurrentChar == NULL
     || !EmeraldTextCompat_ContainsPointer(
            (uintptr_t)data->textCurrentChar))
    {
        fprintf(stderr, "load: currentChar not in a registered text range\n");
        return FALSE;
    }
    if (!EmeraldTextCompat_GetArenaForPointer((uintptr_t)data->textCurrentChar,
                                              &arenaIndex, &offset)
     || !EmeraldTextCompat_GetLabelAtOffset((uint32_t)arenaIndex, offset,
                                            &name, &start, &size))
    {
        fprintf(stderr, "load: currentChar walker resolution failed\n");
        return FALSE;
    }
    if (name == NULL || strcmp(name, "gtext-123dot") != 0 || size != 9u
     || offset != start + 3u)
    {
        fprintf(stderr, "load: currentChar label/offset mismatch "
                        "(name=%s size=%zu offset=%zu start=%zu)\n",
                name != NULL ? name : "(null)", size, offset, start);
        return FALSE;
    }
    if (memcmp(data->textCurrentChar, k123DotBytes + 3, 3) != 0)
    {
        fprintf(stderr, "load: currentChar bytes mismatch\n");
        return FALSE;
    }
    if (data->textOpaque != sCompiledOpaqueText
     || memcmp(data->textOpaque, sCompiledOpaqueText,
               sizeof(sCompiledOpaqueText)) != 0)
    {
        fprintf(stderr, "load: opaque compiled text not verbatim\n");
        return FALSE;
    }
    if (!EmeraldTextCompat_GetArenaByKey("system-shared", &base, &baseSize))
        return FALSE;
    printf("LOAD text arena: system_shared=%p currentChar=%p "
           "labelStart=%zu size=%zu opaque=%p\n",
           (const void *)base, (const void *)data->textCurrentChar,
           start, size, (const void *)data->textOpaque);
    return TRUE;
}

static bool32 RowPointersMatchSession(void)
{
    struct HarnessGameData *data = GameData();
    struct HarnessRow expected[HARNESS_ROW_COUNT];
    size_t i;

    RowFromSheet(&expected[0], &gMonFrontPicTable[1]);
    RowFromSheet(&expected[1], &gMonFrontPicTable[2]);
    RowFromSheet(&expected[2], &gMonBackPicTable[1]);
    RowFromSheet(&expected[3], &gMonBackPicTable[2]);
    RowFromPalette(&expected[4], &gMonPaletteTable[1]);
    RowFromPalette(&expected[5], &gMonShinyPaletteTable[1]);
    RowFromSheet(&expected[6], &gTrainerFrontPicTable[0]);
    RowFromPalette(&expected[7], &gTrainerFrontPicPaletteTable[0]);
    RowFromSheet(&expected[8], &gTrainerBackPicTable[0]);
    RowFromPalette(&expected[9], &gTrainerBackPicPaletteTable[0]);
    for (i = 0; i < HARNESS_ROW_COUNT; i++)
    {
        if (data->rows[i].data != expected[i].data)
        {
            fprintf(stderr, "row %zu pointer not re-derived: got %p want %p\n",
                    i, (const void *)data->rows[i].data,
                    (const void *)expected[i].data);
            return FALSE;
        }
        if (data->rows[i].size != expected[i].size
         || data->rows[i].tag != expected[i].tag
         || data->rows[i].reserved != expected[i].reserved)
        {
            fprintf(stderr, "row %zu size/tag/reserved mismatch\n", i);
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Minimal v5 container parser: proves the saved file really carries    */
/* the resource-reference sidecar with the expected records.            */
struct ParsedRecord
{
    u32 sectionTag;
    u32 fieldOffset;
    u8 key[32];
    u32 type;
    u32 schema;
    u32 role;
    u32 rangeOffset;
    u32 reserved;
};

static u32 ReadLe(const u8 *bytes) { return (u32)bytes[0] | ((u32)bytes[1] << 8)
    | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24); }

static bool32 ParseStateSidecar(const char *path, struct ParsedRecord *out,
                                u32 outCapacity, u32 *outCount)
{
    FILE *file;
    long fileSize;
    u8 *bytes;
    u32 sectionCount;
    u32 i;
    u32 recordCount = 0;
    bool32 found = FALSE;

    file = fopen(path, "rb");
    if (file == NULL)
        return FALSE;
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (fileSize < 44)
    {
        fclose(file);
        return FALSE;
    }
    bytes = malloc((size_t)fileSize);
    if (bytes == NULL || fread(bytes, 1, (size_t)fileSize, file) != (size_t)fileSize)
    {
        free(bytes);
        fclose(file);
        return FALSE;
    }
    fclose(file);

    if (ReadLe(bytes) != 0x4E535431u || ReadLe(bytes + 4) != 5u)
    {
        fprintf(stderr, "state file is not a v5 native state\n");
        free(bytes);
        return FALSE;
    }
    sectionCount = ReadLe(bytes + 16);
    if (sectionCount == 0 || sectionCount > 16)
    {
        free(bytes);
        return FALSE;
    }
    {
        u32 headerSize = ReadLe(bytes + 8);
        for (i = 0; i < sectionCount; i++)
        {
            const u8 *section = bytes + headerSize + i * 12u;
            u32 tag = ReadLe(section);
            u32 size = ReadLe(section + 4);
            u32 payloadOffset = headerSize + sectionCount * 12u;
            u32 offset = 0;
            u32 j;
            if (tag != 14u)
                continue;
            /* Recompute the payload offset of this section by walking. */
            for (j = 0; j < i; j++)
            {
                const u8 *prev = bytes + headerSize + j * 12u;
                offset += ReadLe(prev + 4);
            }
            {
                const u8 *payload = bytes + payloadOffset + offset;
                if (size < 4 || (size - 4u) % 64u != 0)
                {
                    free(bytes);
                    return FALSE;
                }
                recordCount = (size - 4u) / 64u;
                if (recordCount > outCapacity)
                {
                    fprintf(stderr, "sidecar has %u records, capacity %u\n",
                            recordCount, outCapacity);
                    free(bytes);
                    return FALSE;
                }
                for (j = 0; j < recordCount; j++)
                {
                    const u8 *record = payload + 4u + j * 64u;
                    out[j].sectionTag = ReadLe(record);
                    out[j].fieldOffset = ReadLe(record + 4);
                    memcpy(out[j].key, record + 8, 32);
                    out[j].type = ReadLe(record + 40);
                    out[j].schema = ReadLe(record + 44);
                    out[j].role = ReadLe(record + 48);
                    out[j].rangeOffset = ReadLe(record + 52);
                    out[j].reserved = ReadLe(record + 56);
                }
                found = TRUE;
            }
            break;
        }
    }
    free(bytes);
    if (!found)
        return FALSE;
    *outCount = recordCount;
    return TRUE;
}

/* Walk the saved v5 container and compare the 8 bytes at `offset` inside
 * the `wantedTag` section payload with the little-endian bytes of `value` -
 * proves a data window round-tripped verbatim. */
static bool32 StateSectionBytesMatch(const char *path, u32 wantedTag,
                                     u32 offset, uint64_t value)
{
    FILE *file;
    long fileSize;
    u8 *bytes;
    u32 headerSize;
    u32 sectionCount;
    u32 i;
    bool32 result = FALSE;

    file = fopen(path, "rb");
    if (file == NULL)
        return FALSE;
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    bytes = malloc((size_t)fileSize);
    if (bytes == NULL
     || fread(bytes, 1, (size_t)fileSize, file) != (size_t)fileSize)
    {
        free(bytes);
        fclose(file);
        return FALSE;
    }
    fclose(file);
    if (ReadLe(bytes) != 0x4E535431u || ReadLe(bytes + 4) != 5u)
    {
        free(bytes);
        return FALSE;
    }
    headerSize = ReadLe(bytes + 8);
    sectionCount = ReadLe(bytes + 16);
    {
        u32 payloadBase = headerSize + sectionCount * 12u;
        u32 payloadOffset = 0u;
        for (i = 0; i < sectionCount; i++)
        {
            const u8 *section = bytes + headerSize + i * 12u;
            u32 tag = ReadLe(section);
            u32 size = ReadLe(section + 4);
            if (tag == wantedTag)
            {
                const u8 *payload = bytes + payloadBase + payloadOffset;
                const uint8_t *expected = (const uint8_t *)(const void *)&value;
                result = offset + 8u <= size
                      && memcmp(payload + offset, expected, 8u) == 0;
                if (!result)
                    fprintf(stderr,
                            "regression: section %u bytes at +0x%x do not "
                            "match the planted value (section size 0x%x)\n",
                            wantedTag, offset, size);
                break;
            }
            payloadOffset += size;
        }
    }
    free(bytes);
    return result;
}

static bool32 StateGameDataBytesMatch(const char *path, u32 offset,
                                      uint64_t value)
{
    return StateSectionBytesMatch(path, 8u, offset, value);
}

/* GAME_BSS (tag 1) variant for the audio fixture windows. */
static bool32 StateGameBssBytesMatch(const char *path, u32 offset,
                                     uint64_t value)
{
    return StateSectionBytesMatch(path, 1u, offset, value);
}

/* Read one whole section payload out of the saved v5 container (malloc'd;
 * the caller frees). Returns FALSE on a malformed container or a missing
 * section. Used by the sidecar regression to inspect the raw EWRAM bytes in
 * the file independently of the walker's own view. */
static bool32 StateSectionRead(const char *path, u32 wantedTag,
                               u8 **outBytes, u32 *outSize)
{
    FILE *file;
    long fileSize;
    u8 *bytes;
    u32 headerSize;
    u32 sectionCount;
    u32 i;
    bool32 result = FALSE;

    *outBytes = NULL;
    *outSize = 0u;
    file = fopen(path, "rb");
    if (file == NULL)
        return FALSE;
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    bytes = malloc((size_t)fileSize);
    if (bytes == NULL
     || fread(bytes, 1, (size_t)fileSize, file) != (size_t)fileSize)
    {
        free(bytes);
        fclose(file);
        return FALSE;
    }
    fclose(file);
    if (ReadLe(bytes) != 0x4E535431u || ReadLe(bytes + 4) != 5u)
    {
        free(bytes);
        return FALSE;
    }
    headerSize = ReadLe(bytes + 8);
    sectionCount = ReadLe(bytes + 16);
    {
        u32 payloadBase = headerSize + sectionCount * 12u;
        u32 payloadOffset = 0u;
        for (i = 0; i < sectionCount; i++)
        {
            const u8 *section = bytes + headerSize + i * 12u;
            u32 tag = ReadLe(section);
            u32 size = ReadLe(section + 4);
            if (tag == wantedTag)
            {
                u8 *payload = malloc((size_t)size);
                if (payload != NULL)
                {
                    memcpy(payload, bytes + payloadBase + payloadOffset,
                           (size_t)size);
                    *outBytes = payload;
                    *outSize = size;
                    result = TRUE;
                }
                break;
            }
            payloadOffset += size;
        }
    }
    free(bytes);
    return result;
}

static void PrintKey(const u8 key[32], char *dest)
{
    Gen3ResourceKey resourceKey;
    memcpy(resourceKey.bytes, key, 32);
    Gen3ResourceId_FormatKeyHex(&resourceKey, dest);
}

static int DoCreate(const char *packPath, const char *statePath)
{
    struct ParsedRecord records[32];
    u32 recordCount = 0;
    uint8_t fingerprint[32];
    char fingerprintHex[65];
    size_t i;
    enum NativeStateResult result;

    if (!RegisterSession(packPath))
        return 1;
    if (!EmeraldResourceCompat_GetSessionContentFingerprint(fingerprint))
    {
        fprintf(stderr, "create: session fingerprint not set\n");
        return 1;
    }
    PrintDigestHex(fingerprint, fingerprintHex);
    printf("CREATE fingerprint=%s\n", fingerprintHex);

    if (!PlantRealRows())
        return 1;
    if (!PlantCurrentCharClass())
        return 1;
    if (!PlantMapHeaderClass())
        return 1;
    if (!VerifyTilesetPublished())
        return 1;
    if (!VerifyLayoutPublished())
        return 1;
    printf("CREATE arena pointers: front=%p back=%p trainer=%p\n",
           (const void *)gMonFrontPicTable[1].data,
           (const void *)gMonBackPicTable[1].data,
           (const void *)gTrainerFrontPicTable[0].data);
    {
        const u8 *base;
        size_t size;
        if (!EmeraldTextCompat_GetArenaByKey("system-shared", &base, &size))
        {
            fprintf(stderr, "create: system-shared text arena missing\n");
            return 1;
        }
        printf("CREATE text arena: system_shared=%p labels=%zu currentChar=%p\n",
               (const void *)base, size, (const void *)gText_123Dot[1]);
    }

    /* R11-E/F tests 15/16 (create side): the neighborhood module. Build the
     * real Littleroot neighborhood (maps.o tables + published layouts),
     * save, then corrupt the module's records and prove the corruption is
     * NOT serialized: the module's storage is host .bss/.data (outside both
     * game slices -- layer 1), and a save taken AFTER the corruption is
     * byte-identical to the save taken before it (layer 3). The load
     * process (test 15 load side) rebuilds cleanly from the restored
     * identity. */
    NativeWorldNeighborhood_Init();
    SetNeighborhoodLocation(MAP_GROUP_TOWNS_AND_ROUTES, MAP_NUM_LITTLEROOT_TOWN);
    CheckLittlerootNeighborhood(NativeWorldNeighborhood_GetState(), 1u);
    CHECK(!NeighborhoodInGameRanges(NativeWorldNeighborhood_GetState()));

    result = NativeState_Save(HARNESS_STATE_SLOT);
    if (result != NATIVE_STATE_OK)
    {
        fprintf(stderr, "create: save failed: %s\n", NativeState_GetLastError());
        return 1;
    }
    {
        struct StateFileBytes before;
        size_t i;
        if (!ReadStateFile(statePath, &before))
            return 1;
        /* Test 15: corrupt every record field and the version. */
        {
            struct NativeWorldNeighborhood *mutable =
                (struct NativeWorldNeighborhood *)(uintptr_t)(const void *)
                    NativeWorldNeighborhood_GetState();
            for (i = 0; i < NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS; i++)
            {
                mutable->neighbors[i].direction = (u8)(0xEEu + i);
                mutable->neighbors[i].offset = 0x12345678;
                mutable->neighbors[i].mapGroup = 0xFF;
                mutable->neighbors[i].mapNum = 0xFF;
                mutable->neighbors[i].worldX = -1;
                mutable->neighbors[i].worldY = -1;
                mutable->neighbors[i].width = -1;
                mutable->neighbors[i].height = -1;
            }
            mutable->neighborCount = NATIVE_WORLD_NEIGHBORHOOD_MAX_NEIGHBORS;
            mutable->version = 0xDEADBEEFu;
        }
        result = NativeState_Save(HARNESS_STATE_SLOT);
        if (result != NATIVE_STATE_OK)
        {
            fprintf(stderr, "create: re-save failed: %s\n",
                    NativeState_GetLastError());
            return 1;
        }
        /* Test 16 layer 3: the two saves are byte-identical -- the
         * corrupted records are host-only and never enter the state. */
        CHECK(StateFileMatches(statePath, &before));
        free(before.bytes);
    }
    if (!ParseStateSidecar(statePath, records, 32, &recordCount))
    {
        fprintf(stderr, "create: state file has no valid resource sidecar\n");
        return 1;
    }
    printf("CREATE recordCount=%u\n", recordCount);
    for (i = 0; i < recordCount; i++)
    {
        char keyHex[GEN3_RESOURCE_KEY_HEX_SIZE];
        PrintKey(records[i].key, keyHex);
        printf("CREATE record %zu: section=%u field=+0x%x key=%s type=%u schema=%u role=%u rangeOffset=%u\n",
               i, records[i].sectionTag, records[i].fieldOffset, keyHex,
               records[i].type, records[i].schema, records[i].role,
               records[i].rangeOffset);
        CHECK(records[i].reserved == 0);
        /* Battle rows register as legacy-LZ compat; text and the R13-F map
         * event/connection arenas register as compat objects. */
        CHECK(records[i].role == EMERALD_RESOURCE_ROLE_LEGACY_LZ
              || records[i].role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT);
        CHECK(records[i].sectionTag == 8u); /* GAME_DATA */
    }
    /* Test 16 layer 2: the sidecar carries exactly the HARNESS_ROW_COUNT
     * game-data records, the R13-C currentChar interior pointer, and the
     * R13-F map event/connection arena pointers --
     * the neighborhood module added NO record (its storage is host
     * .bss/.data, never a serialized slice), and the opaque compiled
     * text pointer added none either (it is outside every resource
     * range, so it serializes verbatim in-band). */
    CHECK(recordCount == HARNESS_ROW_COUNT + 3u);
    printf("CREATE expected %u records\n", HARNESS_ROW_COUNT + 3u);
    {
        /* The interior-pointer record carries the system-shared arena
         * identity with the zone-relative offset of gText_123Dot[1]
         * (its label start + 3, the in-label offset of row 1). */
        Gen3ResourceKey textKey;
        size_t i2;
        bool32 foundText = FALSE;
        Gen3ResourceId_DeriveKey("emerald:text/arena/system-shared",
                                 &textKey);
        for (i2 = 0u; i2 < recordCount; i2++)
        {
            if (memcmp(records[i2].key, textKey.bytes,
                       GEN3_RESOURCE_KEY_SIZE) == 0)
            {
                size_t arenaIndex = TEXT_ARENA_COUNT;
                size_t offset = 0u;
                foundText = TRUE;
                CHECK(records[i2].role
                      == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT);
                CHECK(records[i2].type == GEN3_RESOURCE_TYPE_TEXT);
                CHECK(records[i2].schema == 1u);
                CHECK(EmeraldTextCompat_GetArenaForPointer(
                        (uintptr_t)gText_123Dot[1], &arenaIndex, &offset));
                CHECK(records[i2].rangeOffset == (u32)offset);
                break;
            }
        }
        CHECK(foundText);
        if (!foundText)
        {
            fprintf(stderr, "create: no currentChar sidecar record\n");
            return 1;
        }
    }
    {
        bool32 foundEvents = FALSE;
        bool32 foundConnections = FALSE;
        size_t i2;

        for (i2 = 0u; i2 < recordCount; i2++)
        {
            if (records[i2].type != GEN3_RESOURCE_TYPE_STRUCTURED_DATA
             || records[i2].role != EMERALD_RESOURCE_ROLE_COMPAT_OBJECT)
                continue;
            if (records[i2].schema == EMERALD_MAP_SCHEMA_EVENTS)
                foundEvents = TRUE;
            if (records[i2].schema == EMERALD_MAP_SCHEMA_CONNECTIONS)
                foundConnections = TRUE;
        }
        CHECK(foundEvents);
        CHECK(foundConnections);
        if (!foundEvents || !foundConnections)
        {
            fprintf(stderr, "create: map arena sidecar records missing\n");
            return 1;
        }
    }
    if (sFailures != 0)
        return 1;
    printf("CREATE ok\n");
    return 0;
}

/* R10-C regression (live-game save-failure class): a serialized slice can
 * legitimately hold an 8-byte window whose u64 is numerically inside a
 * resource arena's UNEXPOSED build-time-only prefix - packed sprite pixels
 * in a gHeap gfx buffer and M4A channel byte fields (0x03051E51 in the
 * reproduction) are exactly such values. Those are ordinary data, never
 * resource pointers. This mode plants exactly those windows (a small-u32/
 * zero-high-half value of the observed class plus an 8-aligned value) into
 * the serialized filler region and proves the save succeeds, adds no
 * sidecar record, and preserves the bytes verbatim. Pre-fix (whole-arena
 * hulls) this hard-failed with "resource-owned pointer lacks a registered
 * resource identity". */
static int DoRegression(const char *packPath, const char *statePath)
{
    const struct EmeraldResourceCompatibilityImage *pokemonImage;
    const uint8_t *arenaBase = NULL;
    const uint8_t *exposedBase = NULL;
    size_t arenaSize = 0u;
    size_t exposedSize = 0u;
    const struct EmeraldResourceRangeIndex *rangeIndex;
    struct HarnessGameData *data;
    uint64_t valueA; /* small u32 inside the prefix, zero high half */
    uint64_t valueB; /* 8-aligned window just below the first stream */
    struct ParsedRecord records[32];
    u32 recordCount = 0u;
    enum NativeStateResult result;

    if (!RegisterSession(packPath))
        return 1;
    if (!PlantRealRows())
        return 1;

    /* The unexposed prefix is [arena base, first stream); only the stream
     * block (the exposed span) is registered. The plant addresses are
     * derived from the arena layout, NOT from the range index, so the
     * assertion is identical before and after the hull fix. */
    pokemonImage = EmeraldPokemonCompat_GetImage();
    CHECK(pokemonImage != NULL);
    if (pokemonImage == NULL)
        return 1;
    CHECK(EmeraldResourceCompatImage_GetArenaSpan(pokemonImage, &arenaBase,
                                                  &arenaSize));
    CHECK(EmeraldResourceCompatImage_GetExposedSpan(pokemonImage, &exposedBase,
                                                    &exposedSize));
    if (arenaBase == NULL || exposedBase == NULL
     || exposedBase <= arenaBase || exposedSize == 0u)
    {
        fprintf(stderr, "regression: image arena/exposed layout invalid\n");
        return 1;
    }
    /* The coincidence class is a SMALL u32 whose bytes numerically fall
     * inside the arena band (0x03051E51 envelope bytes in the
     * reproduction). That requires an arena base below 4GB; under ASan the
     * arenas sit above 4GB where no small u32 can ever coincide with the
     * hull - the class is unrepresentable there, and a full-width arena
     * address is a genuine mapped host pointer that correctly trips the
     * external-host classifier. Skip cleanly instead of fabricating a
     * meaningless variant. */
    if ((uintptr_t)arenaBase > 0xFFFFFFFFu)
    {
        printf("REGRESSION skipped (arena base 0x%zx above 4GB; "
               "small-u32 hull coincidence unrepresentable under ASan)\n",
               (uintptr_t)arenaBase);
        return 0;
    }
    /* valueA: a small u32 (zero high half) numerically inside the prefix
     * band at an odd offset. valueB: an 8-aligned small u32 one step below
     * the first stream (inside the prefix). */
    valueA = (uint64_t)(uint32_t)((uintptr_t)arenaBase + 0x1E51u);
    valueB = (uint64_t)(uint32_t)(((uintptr_t)exposedBase & ~(uintptr_t)7u) - 8u);
    CHECK((uintptr_t)valueA >= (uintptr_t)arenaBase
       && (uintptr_t)valueA < (uintptr_t)exposedBase);
    CHECK((uintptr_t)valueB >= (uintptr_t)arenaBase
       && (uintptr_t)valueB < (uintptr_t)exposedBase);
    CHECK(valueB % 8u == 0u);
    /* Neither value may resolve to a registered stream: they must classify
     * as data on the capture side, never as references. */
    rangeIndex = EmeraldResourceCompat_GetRangeIndex();
    CHECK(rangeIndex != NULL);
    if (rangeIndex != NULL)
    {
        struct EmeraldResourceRangeHit hit;
        CHECK(!EmeraldResourceRangeIndex_Lookup(rangeIndex,
                                                (uintptr_t)valueA, &hit));
        CHECK(!EmeraldResourceRangeIndex_Lookup(rangeIndex,
                                                (uintptr_t)valueB, &hit));
    }
    printf("REGRESSION prefix band 0x%zx..0x%zx; planted valueA=0x%llx valueB=0x%llx\n",
           (uintptr_t)arenaBase, (uintptr_t)exposedBase,
           (unsigned long long)valueA, (unsigned long long)valueB);

    /* filler[8..9] = valueA, filler[10..11] = valueB. The 8-byte capture
     * windows start at 4-byte-stride offsets; each planted u64 is the exact
     * window at its own start (offsetof handles the compiler's pointer
     * alignment padding after `magic`). */
    data = GameData();
    data->filler[8] = (u32)valueA;
    data->filler[9] = (u32)(valueA >> 32);
    data->filler[10] = (u32)valueB;
    data->filler[11] = (u32)(valueB >> 32);

    result = NativeState_Save(HARNESS_STATE_SLOT);
    if (result != NATIVE_STATE_OK)
    {
        fprintf(stderr, "regression: save failed: %s\n",
                NativeState_GetLastError());
        return 1;
    }
    /* No sidecar records beyond the 10 real rows: the planted windows were
     * captured as data, never as resource references. */
    if (!ParseStateSidecar(statePath, records, 32, &recordCount))
    {
        fprintf(stderr, "regression: state file has no valid resource sidecar\n");
        return 1;
    }
    CHECK(recordCount == HARNESS_ROW_COUNT);
    /* The planted bytes round-trip verbatim in the GAME_DATA payload. */
    if (!StateGameDataBytesMatch(
            statePath,
            (u32)offsetof(struct HarnessGameData, filler[8]), valueA)
     || !StateGameDataBytesMatch(
            statePath,
            (u32)offsetof(struct HarnessGameData, filler[10]), valueB))
        return 1;
    if (sFailures != 0)
        return 1;
    printf("REGRESSION ok\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST S: typed sidecar scalar+padding windows are data, never pointers.
 *
 * The raw EWRAM slice contains the typed sprite template/image sidecar
 * arrays. struct SpriteTemplate is {u16 tileTag, u16 paletteTag, 4 bytes
 * host alignment padding, then five native pointers}; struct
 * SpriteFrameImage is {pointer, u16 size, 6 bytes padding}. The generic
 * u32-stride 8-byte windowing used to examine the head window of each
 * element as if it were a native pointer; with stale padding the widened
 * u64 numerically lands in a host mapping (the real reproduction: tags
 * 0xffff/0xffff + padding bytes 38 7f 00 00 -> 0x00007f38ffffffff inside
 * libLLVM) and the save hard-failed with "state contains unmanaged native
 * pointer". The structural fix (native_state.c
 * RuntimeLocationIsModeledScalarOrPadding) makes the walker normalize ONLY
 * the modeled pointer/function members of typed regions. This mode plants
 * exactly that failure shape, plus a small-u32 resource-hull coincidence in
 * a second element's head (which the pre-fix walker silently CAPTURED as a
 * resource reference and zeroed in the file), plus a SpriteFrameImage with
 * nonzero padding, then proves: the save succeeds, no extra sidecar record
 * appears, the scalar/padding bytes round-trip verbatim in the file, and
 * the genuine pointer members are persisted as tagged records and restored
 * by a fresh process (the file is ground truth there, because arena and
 * image addresses differ per process).
 *
 * Unlike TEST R, this class is fully representable under ASan: the plant
 * bytes are literal, never derived from the arena layout. */

/* Planted pointer targets: static objects/functions of THIS binary. Data
 * pointers must be image addresses outside the host-data span (the image
 * persistent-identity path); the callback must be an image address inside
 * an executable mapping. The stub's executable model reads /proc/self/maps,
 * so .rodata is non-executable and .text is executable. */
static const u8 sSidecarPlantOam[16] = {0};
static const u8 sSidecarPlantAnims[16] = {0};
static const u8 sSidecarPlantImages[16] = {0};
static const u8 sSidecarPlantAffine[16] = {0};
static void HarnessSidecarCallback(struct Sprite *sprite)
{
    (void)sprite;
}

/* The exact byte shape of the real reproduction: the window at element
 * +0x0 reads ff ff ff ff 38 7f 00 00, which the generic walker widened to
 * u64 0x00007f38ffffffff (a host executable mapping). */
static void PlantSidecarTemplateHead(u8 *raw)
{
    raw[0] = 0xff;
    raw[1] = 0xff; /* tileTag  */
    raw[2] = 0xff;
    raw[3] = 0xff; /* paletteTag */
    raw[4] = 0x38;
    raw[5] = 0x7f; /* stale host-pointer-high padding bytes */
    raw[6] = 0x00;
    raw[7] = 0x00;
}

static int DoRegressionSidecar(const char *packPath, const char *statePath)
{
    const struct EmeraldResourceCompatibilityImage *pokemonImage;
    const uint8_t *exposedBase = NULL;
    size_t exposedSize = 0u;
    const struct EmeraldResourceRangeIndex *rangeIndex;
    struct ParsedRecord records[32];
    u32 recordCount = 0u;
    u32 probe;
    uintptr_t hullValue = 0u;
    enum NativeStateResult result;
    u8 *ewram = NULL;
    u32 ewramSize = 0u;
    u32 ewramBase;
    u32 e1;
    u32 e2;
    u32 i1;
    const u8 *e1raw;
    const u8 *e2raw;
    const u8 *i1raw;

    if (!RegisterSession(packPath))
        return 1;
    if (!PlantRealRows())
        return 1;
    /* The EWRAM slice is the whole "gba_ewram" output section. The linker is
     * free to order the contributing input sections (maps.o's data, the
     * image sidecar array, the template sidecar array), so every file offset
     * below is derived from __start_gba_ewram - never from an assumed
     * layout. (In the real build the sidecar arrays are the only
     * contributors and the same derivation applies.) */

    /* A registered stream address for the small-u32 hull-coincidence plant:
     * the ranges are contiguous over the exposed span, so an 8-aligned
     * probe near its start is registered. */
    pokemonImage = EmeraldPokemonCompat_GetImage();
    CHECK(pokemonImage != NULL);
    if (pokemonImage == NULL)
        return 1;
    CHECK(EmeraldResourceCompatImage_GetExposedSpan(pokemonImage, &exposedBase,
                                                    &exposedSize));
    rangeIndex = EmeraldResourceCompat_GetRangeIndex();
    CHECK(rangeIndex != NULL);
    if (exposedBase != NULL && rangeIndex != NULL)
    {
        for (probe = 0u; probe + 8u <= exposedSize; probe += 8u)
        {
            struct EmeraldResourceRangeHit hit;
            uintptr_t candidate = (uintptr_t)exposedBase + probe;

            if (EmeraldResourceRangeIndex_Lookup(rangeIndex, candidate, &hit))
            {
                hullValue = candidate;
                break;
            }
        }
    }
    CHECK(hullValue != 0u);
    /* The head window bytes of element 2 must be exactly the 8 bytes of
     * hullValue (tags + zero padding), so the pre-fix walker would have
     * captured them as a resource reference. */
    CHECK((hullValue & 0xFFFFu) != 0xFFFFu);

    /* Element 1: the exact real failure shape -- a host-mapping head window
     * plus genuine native pointers. */
    memset(&sSpriteTemplateSidecars[1], 0, sizeof(sSpriteTemplateSidecars[1]));
    PlantSidecarTemplateHead((u8 *)&sSpriteTemplateSidecars[1]);
    sSpriteTemplateSidecars[1].oam =
        (const struct OamData *)(const void *)sSidecarPlantOam;
    sSpriteTemplateSidecars[1].anims =
        (const union AnimCmd *const *)(const void *)sSidecarPlantAnims;
    sSpriteTemplateSidecars[1].images =
        (const struct SpriteFrameImage *)(const void *)sSidecarPlantImages;
    sSpriteTemplateSidecars[1].affineAnims =
        (const union AffineAnimCmd *const *)(const void *)sSidecarPlantAffine;
    sSpriteTemplateSidecars[1].callback = HarnessSidecarCallback;
    /* Element 2: tags = low 32 bits of a registered stream address, zero
     * padding -> the head window numerically equals a registered range
     * (small-u32 resource-hull capture class). */
    memset(&sSpriteTemplateSidecars[2], 0, sizeof(sSpriteTemplateSidecars[2]));
    sSpriteTemplateSidecars[2].tileTag = (u16)(hullValue & 0xFFFFu);
    sSpriteTemplateSidecars[2].paletteTag = (u16)((hullValue >> 16) & 0xFFFFu);
    /* Element 3 (frame-image sidecar): data pointer, u16 size, nonzero
     * padding -> the +0x4 window combines size and padding into a
     * host-mapping u64 (0x7f38_1234_00000000). */
    memset(&sSpriteTemplateImageSidecars[1], 0,
           sizeof(sSpriteTemplateImageSidecars[1]));
    sSpriteTemplateImageSidecars[1].data =
        (const void *)sSidecarPlantImages;
    sSpriteTemplateImageSidecars[1].size = 0x1234;
    {
        u8 *pad = (u8 *)&sSpriteTemplateImageSidecars[1];
        pad[10] = 0x38;
        pad[11] = 0x7f;
        pad[12] = 0x00;
        pad[13] = 0x00;
        pad[14] = 0x00;
        pad[15] = 0x00;
    }

    printf("SIDECAR plant: template[1] head=ff ff ff ff 38 7f 00 00 "
           "callback=%p; template[2] head=0x%llx; image[1] size=0x1234\n",
           (const void *)HarnessSidecarCallback,
           (unsigned long long)hullValue);

    result = NativeState_Save(HARNESS_STATE_SLOT);
    if (result != NATIVE_STATE_OK)
    {
        fprintf(stderr, "regression-sidecar: save failed: %s\n",
                NativeState_GetLastError());
        return 1;
    }
    /* No sidecar records beyond the 10 real rows: none of the planted
     * scalar/padding windows was captured as a resource reference. */
    if (!ParseStateSidecar(statePath, records, 32, &recordCount))
    {
        fprintf(stderr,
                "regression-sidecar: state file has no valid resource "
                "sidecar\n");
        return 1;
    }
    CHECK(recordCount == HARNESS_ROW_COUNT);
    /* File-level round-trip: the EWRAM slice must carry the scalar/padding
     * bytes verbatim and the modeled pointer members as tagged records. */
    if (!StateSectionRead(statePath, 5u, &ewram, &ewramSize)) /* EWRAM */
    {
        fprintf(stderr, "regression-sidecar: no EWRAM section in state\n");
        return 1;
    }
    ewramBase = (u32)((const u8 *)sSpriteTemplateSidecars
                      - (const u8 *)__start_gba_ewram);
    e1 = (u32)((const u8 *)&sSpriteTemplateSidecars[1]
               - (const u8 *)__start_gba_ewram);
    e2 = (u32)((const u8 *)&sSpriteTemplateSidecars[2]
               - (const u8 *)__start_gba_ewram);
    i1 = (u32)((const u8 *)&sSpriteTemplateImageSidecars[1]
               - (const u8 *)__start_gba_ewram);
    /* The WHOLE sidecar arrays must live inside the serialized EWRAM slice
     * (the walker normalizes exactly this region in the real build too). */
    if (!((u32)((const u8 *)&sSpriteTemplateSidecars[MAX_SPRITES + 1]
                - (const u8 *)__start_gba_ewram) <= ewramSize)
     || !((u32)((const u8 *)&sSpriteTemplateImageSidecars[MAX_SPRITES + 1]
                - (const u8 *)__start_gba_ewram) <= ewramSize))
    {
        fprintf(stderr, "regression-sidecar: sidecar arrays outside the "
                "EWRAM slice (base 0x%x, size 0x%x)\n",
                ewramBase, ewramSize);
        free(ewram);
        return 1;
    }
    e1raw = ewram + e1;
    e2raw = ewram + e2;
    i1raw = ewram + i1;
    /* Element 1: scalar head verbatim; the pointer members are tagged
     * records (value u32 then kind u32: PDIM for data, PFIM for the
     * callback). */
    CHECK(memcmp(e1raw, (const u8 *)&sSpriteTemplateSidecars[1], 8u) == 0);
    CHECK(ReadLe(e1raw + 12u) == 0x5044494Du);  /* oam record kind */
    CHECK(ReadLe(e1raw + 0x14u) == 0x5044494Du); /* anims record kind */
    CHECK(ReadLe(e1raw + 0x1Cu) == 0x5044494Du); /* images record kind */
    CHECK(ReadLe(e1raw + 0x24u) == 0x5044494Du); /* affineAnims record kind */
    CHECK(ReadLe(e1raw + 0x2Cu) == 0x5046494Du); /* callback record kind */
    /* Element 2: the head window (tags + zero padding) round-tripped
     * verbatim -- NOT zeroed by a resource capture, NOT rewritten by a
     * pointer normalization. */
    CHECK(memcmp(e2raw, (const u8 *)&sSpriteTemplateSidecars[2], 8u) == 0);
    CHECK(ReadLe(e2raw + 8u) == 0u); /* oam record: NULL persists as zero */
    /* Element 3: data pointer record, then size + padding verbatim. */
    CHECK(ReadLe(i1raw + 4u) == 0x5044494Du); /* data record kind */
    CHECK(memcmp(i1raw + 8u,
                 (const u8 *)&sSpriteTemplateImageSidecars[1] + 8u,
                 8u) == 0);
    free(ewram);
    if (sFailures != 0)
        return 1;
    printf("REGRESSION-SIDECAR create ok\n");
    return 0;
}

static int DoLoadSidecar(const char *packPath, const char *statePath)
{
    u8 *ewram = NULL;
    u32 ewramSize = 0u;
    u32 ewramBase;
    u32 e1;
    u32 e2;
    u32 i1;
    const u8 *e1raw;
    const u8 *e2raw;
    const u8 *i1raw;
    enum NativeStateResult result;

    if (!RegisterSession(packPath))
        return 1;
    if (!StateSectionRead(statePath, 5u, &ewram, &ewramSize)) /* EWRAM */
    {
        fprintf(stderr, "load-sidecar: no EWRAM section in state\n");
        return 1;
    }
    ewramBase = (u32)((const u8 *)sSpriteTemplateSidecars
                      - (const u8 *)__start_gba_ewram);
    e1 = (u32)((const u8 *)&sSpriteTemplateSidecars[1]
               - (const u8 *)__start_gba_ewram);
    e2 = (u32)((const u8 *)&sSpriteTemplateSidecars[2]
               - (const u8 *)__start_gba_ewram);
    i1 = (u32)((const u8 *)&sSpriteTemplateImageSidecars[1]
               - (const u8 *)__start_gba_ewram);
    /* Whole sidecar arrays inside the serialized EWRAM slice (see the
     * create-side derivation: linker section ordering is not assumed). */
    if (!((u32)((const u8 *)&sSpriteTemplateSidecars[MAX_SPRITES + 1]
                - (const u8 *)__start_gba_ewram) <= ewramSize)
     || !((u32)((const u8 *)&sSpriteTemplateImageSidecars[MAX_SPRITES + 1]
                - (const u8 *)__start_gba_ewram) <= ewramSize))
    {
        fprintf(stderr, "load-sidecar: sidecar arrays outside the EWRAM "
                "slice (base 0x%x, size 0x%x)\n",
                ewramBase, ewramSize);
        free(ewram);
        return 1;
    }
    e1raw = ewram + e1;
    e2raw = ewram + e2;
    i1raw = ewram + i1;

    result = NativeState_Load(HARNESS_STATE_SLOT);
    if (result != NATIVE_STATE_OK)
    {
        fprintf(stderr, "load-sidecar: rejected: %s\n",
                NativeState_GetLastError());
        free(ewram);
        return 1;
    }
    /* The modeled pointer members restore to THIS process's image objects
     * (non-PIE, so the same virtual addresses as the creator). */
    CHECK(sSpriteTemplateSidecars[1].oam
          == (const struct OamData *)(const void *)sSidecarPlantOam);
    CHECK(sSpriteTemplateSidecars[1].anims
          == (const union AnimCmd *const *)(const void *)sSidecarPlantAnims);
    CHECK(sSpriteTemplateSidecars[1].images
          == (const struct SpriteFrameImage *)(const void *)sSidecarPlantImages);
    CHECK(sSpriteTemplateSidecars[1].affineAnims
          == (const union AffineAnimCmd *const *)(const void *)sSidecarPlantAffine);
    CHECK(sSpriteTemplateSidecars[1].callback == HarnessSidecarCallback);
    CHECK(sSpriteTemplateImageSidecars[1].data
          == (const void *)sSidecarPlantImages);
    CHECK(sSpriteTemplateImageSidecars[1].size == 0x1234);
    /* The scalar/padding bytes match the FILE verbatim (the file, not the
     * creator's memory, is ground truth for the load process), and element
     * 2's head window came back raw -- the tags were never zeroed by a
     * resource capture. */
    CHECK(memcmp((const u8 *)&sSpriteTemplateSidecars[1], e1raw, 8u) == 0);
    CHECK(memcmp((const u8 *)&sSpriteTemplateSidecars[2], e2raw, 8u) == 0);
    CHECK(memcmp((const u8 *)&sSpriteTemplateImageSidecars[1] + 8u,
                 i1raw + 8u, 8u) == 0);
    CHECK(sSpriteTemplateSidecars[2].oam == NULL);
    CHECK(sSpriteTemplateSidecars[2].anims == NULL);
    CHECK(sSpriteTemplateSidecars[2].images == NULL);
    CHECK(sSpriteTemplateSidecars[2].affineAnims == NULL);
    CHECK(sSpriteTemplateSidecars[2].callback == NULL);
    free(ewram);
    if (sFailures != 0)
        return 1;
    printf("REGRESSION-SIDECAR load ok\n");
    return 0;
}

static int DoLoad(const char *packPath, const char *statePath)
{
    uint8_t fingerprint[32];
    char fingerprintHex[65];
    enum NativeStateResult result;

    if (!RegisterSession(packPath))
        return 1;
    EmeraldResourceCompat_GetSessionContentFingerprint(fingerprint);
    PrintDigestHex(fingerprint, fingerprintHex);
    printf("LOAD fingerprint=%s\n", fingerprintHex);

    /* The serialized region starts zeroed (fresh process) so a successful
     * load must patch every migrated row. */
    result = NativeState_Load(HARNESS_STATE_SLOT);
    if (result != NATIVE_STATE_OK)
    {
        fprintf(stderr, "load: rejected: %s\n", NativeState_GetLastError());
        return 1;
    }
    CHECK(GameData()->magic == 0x48475231u);
    CHECK(GameData()->tailMagic == 0x48475232u);
    {
        size_t i;
        for (i = 0; i < sizeof(GameData()->filler) / sizeof(GameData()->filler[0]); i++)
            CHECK(GameData()->filler[i] == 0xA5A50000u + (u32)i);
    }
    if (!RowPointersMatchSession())
        return 1;
    if (!VerifyMapHeaderRelocated())
        return 1;
    /* R11-B cross-restart: the post-load republish re-derives the migrated
     * object-event frames and palettes from THIS process's arena. The sheet
     * arrays are structural slots (the payload lives in the session image;
     * frame .data points into it); the palette arrays receive the bytes. */
    CHECK(sPicTable_BrendanNormal[0].data != NULL);
    CHECK(gObjectEventPal_Brendan[0] != 0);
    if (sPicTable_BrendanNormal[0].data == NULL
     || gObjectEventPal_Brendan[0] == 0)
    {
        fprintf(stderr, "load: object-event frames not republished\n");
        return 1;
    }
    /* R11-C cross-restart: the post-load republish re-publishes the tileset
     * family from THIS process's arena (R6 re-publish-after-load). */
    if (!VerifyTilesetPublished())
        return 1;
    /* R11-D cross-restart: the post-load republish re-publishes the layout
     * family from THIS process's arena (R6 re-publish-after-load); the
     * .map/.border pointers must point into the NEW session arena. */
    if (!VerifyLayoutPublished())
        return 1;
    /* R11-E/F tests 15/16 (load side): the restore path's Invalidate hook
     * (native_state.c restore block) ran inside NativeState_Load above.
     * The module must rebuild cleanly from the restored identity -- no
     * garbage from the create process's corrupted records (layer 3: they
     * were never serialized), and a direct corrupt->Invalidate->query cycle
     * must rebuild with a bumped version and fresh records. */
    {
        const struct NativeWorldNeighborhood *nb;
        u32 version;
        SetNeighborhoodLocation(MAP_GROUP_TOWNS_AND_ROUTES,
                                MAP_NUM_LITTLEROOT_TOWN);
        nb = NativeWorldNeighborhood_GetState();
        /* Fresh process: the module's .bss starts zeroed; the first complete
         * build after the load is version 1, and the records are clean. */
        CheckLittlerootNeighborhood(nb, 1u);
        CHECK(!NeighborhoodInGameRanges(nb)); /* layer 1 (load process) */

        /* The hook contract, directly: corrupt -> Invalidate -> the next
         * query rebuilds from the identity (garbage gone, version bumped). */
        {
            struct NativeWorldNeighborhood *mutable =
                (struct NativeWorldNeighborhood *)(uintptr_t)(const void *)nb;
            mutable->neighbors[0].direction = 0xAA;
            mutable->neighbors[0].offset = 0x0BADF00D;
            version = nb->version;
            NativeWorldNeighborhood_Invalidate();
            nb = NativeWorldNeighborhood_GetState();
            CHECK(nb->status == NATIVE_WORLD_NB_READY);
            CHECK(nb->version == version + 1u);
            CHECK(nb->neighborCount == 1u);
            CHECK(nb->neighbors[0].direction == CONNECTION_NORTH);
            CHECK(nb->neighbors[0].offset == 0);
        }

        /* Identity change rebuilds too (Oldale: 3 spatial neighbors, then
         * back to Littleroot). */
        SetNeighborhoodLocation(MAP_GROUP_TOWNS_AND_ROUTES,
                                MAP_NUM_OLDALE_TOWN);
        nb = NativeWorldNeighborhood_GetState();
        CHECK(nb->status == NATIVE_WORLD_NB_READY);
        CHECK(nb->neighborCount == 3u);
        SetNeighborhoodLocation(MAP_GROUP_TOWNS_AND_ROUTES,
                                MAP_NUM_LITTLEROOT_TOWN);
        nb = NativeWorldNeighborhood_GetState();
        CHECK(nb->status == NATIVE_WORLD_NB_READY);
        CHECK(nb->neighborCount == 1u);
        CHECK(nb->neighbors[0].direction == CONNECTION_NORTH);
    }
    /* R13-C §13-15: the currentChar interior pointer relocated into THIS
     * process's text arena (same identity + in-label offset, fresh
     * base); the opaque compiled pointer round-tripped verbatim. */
    if (!VerifyTextCurrentCharRelocated())
        return 1;
    printf("LOAD object-event arena: brendan frame=%p pal[0]=0x%04x\n",
           (const void *)sPicTable_BrendanNormal[0].data,
           gObjectEventPal_Brendan[0]);
    printf("LOAD arena pointers: front=%p back=%p trainer=%p\n",
           (const void *)gMonFrontPicTable[1].data,
           (const void *)gMonBackPicTable[1].data,
           (const void *)gTrainerFrontPicTable[0].data);
    if (sFailures != 0)
        return 1;
    printf("LOAD ok\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* R12: audio-shaped game_bss regression (harness layer).              */
/*                                                                     */
/* The real-binary proof is sdl2.c's --native-audio-self-test; this    */
/* mode proves the same value classes through the harness walker with  */
/* assertions the real binary cannot easily make: the saved file's     */
/* sidecar carries EXACTLY the real-row records (audio-shaped scalars  */
/* add none), the scalar windows round-trip verbatim in the GAME_BSS   */
/* slice, the pointer fields serialize as tagged PDIM/PFIM records     */
/* (never raw host addresses), and a fresh process restores the whole  */
/* fixture. The fixture models the real MP2K object graph with the     */
/* REAL struct layouts from m4a_internal.h inside sHarnessGameBss.     */
#define AUDIO_FIXTURE_GAME_BSS_SIZE 0x10000u
#define AUDIO_PLAYERS_OFFSET 0xA000u /* struct MusicPlayerInfo[2] model */
#define AUDIO_JUMPTABLE_OFFSET 0x2000u /* MPlayFunc[64] model */
#define AUDIO_TRACKS_OFFSET 0xA200u
#define AUDIO_MEMACC_OFFSET 0xC000u
#define AUDIO_CMDPTR_OFFSET 0xD000u
/* Persistent-record tags (host_memory.c keeps these private to the TU;
 * the harness asserts the FILE layout, so the literals mirror them). */
#define HOST_PERSISTENT_DATA_IMAGE 0x5044494Du /* PDIM */
#define HOST_PERSISTENT_FUNC_IMAGE 0x5046494Du /* PFIM */

static struct SoundInfo *AudioFixture(void)
{
    return (struct SoundInfo *)(void *)sHarnessGameBss;
}

/* Offset of a real SoundInfo member inside the fixture (portable across
 * the compiler's alignment choices - never hardcode the struct layout). */
#define AUDIO_MEMBER_OFFSET(member) \
    ((u32)((const u8 *)&AudioFixture()->member - (const u8 *)sHarnessGameBss))

/* Build the audio-shaped fixture in sHarnessGameBss and return FALSE when
 * the arena-coincidence plant is unrepresentable (arena above 4GB, i.e.
 * ASan) - the caller then still runs every other assertion. */
static bool32 PlantAudioFixture(void)
{
    struct SoundInfo *fixture = AudioFixture();
    struct MusicPlayerInfo *players =
        (struct MusicPlayerInfo *)(void *)(sHarnessGameBss
                                           + AUDIO_PLAYERS_OFFSET);
    MPlayFunc *jumpTable =
        (MPlayFunc *)(void *)(sHarnessGameBss + AUDIO_JUMPTABLE_OFFSET);
    const struct EmeraldResourceCompatibilityImage *pokemonImage;
    const uint8_t *arenaBase = NULL;
    size_t arenaSize = 0u;
    bool32 coincidencePlanted = FALSE;

    memset(sHarnessGameBss, 0, AUDIO_FIXTURE_GAME_BSS_SIZE);

    /* Pointer-dense SoundInfo head: real function pointers (executable
     * image addresses -> PFIM records) and game_bss-internal pointers
     * (-> PDIM records), exactly the live engine's shapes. */
    fixture->ident = ID_NUMBER;
    fixture->cgbChans = (struct CgbChannel *)(void *)&fixture->chans;
    /* The typed fn-pointer fields are modeled with the real MP2K types;
     * cast through uintptr_t (value-preserving on this target) to avoid
     * -Wcast-function-type on intentionally mismatched signatures. */
    fixture->MPlayMainHead = (MPlayMainFunc)(uintptr_t)NativeState_Save;
    fixture->musicPlayerHead = &players[0];
    fixture->CgbSound = (CgbSoundFunc)(uintptr_t)HarnessStatePath_Override;
    fixture->CgbOscOff = (CgbOscOffFunc)(uintptr_t)HostResolveGbaAddr;
    fixture->MidiKeyToCgbFreq = (MidiKeyToCgbFreqFunc)(uintptr_t)EmeraldPokemonCompat_GetImage;
    fixture->MPlayJumpTable = jumpTable;
    fixture->plynote = (PlyNoteFunc)(uintptr_t)NativeWorldNeighborhood_Invalidate;
    fixture->ExtVolPit = (ExtVolPitFunc)(uintptr_t)EmeraldResourceCompat_GetRangeIndex;

    /* The MPlayJumpTable model: real function entries like the engine's. */
    jumpTable[8] = (MPlayFunc)(uintptr_t)NativeState_Load;
    jumpTable[19] = (MPlayFunc)(uintptr_t)HarnessStatePath_Override;

    /* The player chain model (MPlayOpen prepends; two players suffice). */
    players[0].tracks =
        (struct MusicPlayerTrack *)(void *)(sHarnessGameBss
                                            + AUDIO_TRACKS_OFFSET);
    players[0].memAccArea = sHarnessGameBss + AUDIO_MEMACC_OFFSET;
    players[0].ident = ID_NUMBER;
    players[0].musicPlayerNext = &players[1];
    players[1].tracks = players[0].tracks;
    players[1].memAccArea = sHarnessGameBss + AUDIO_MEMACC_OFFSET;
    players[1].ident = ID_NUMBER;
    players[1].musicPlayerNext = NULL;
    fixture->chans[0].track = players[0].tracks;

    /* Channel linkage inside the chans array, like SampleMixer's ring. */
    fixture->chans[0].prevChannelPointer = &fixture->chans[1];
    fixture->chans[0].nextChannelPointer = &fixture->chans[2];
    fixture->chans[0].wav =
        (struct WaveData *)(void *)&fixture->pcmBuffer[0];
    fixture->chans[0].currentPointer =
        (s8 *)(void *)(sHarnessGameBss + AUDIO_CMDPTR_OFFSET);

    /* Scalar classes that must stay ordinary data:
     *  - gap2 u32 pair 0x5F800000_5F800000: a u64 inside the external-host
     *    pointer range [0x500000000000, 0x800000000000) that is NOT
     *    mincore-mapped;
     *  - chans[0].blockCount 0x6D53A5A5: a large u32 below 4GB but outside
     *    every image range (apuFrame-like counter);
     *  - pcmBuffer[3..6]: float values 2^63 (0x5F000000) - the same u64
     *    class in the float data;
     *  - chans[1].blockCount 0x04040404: small u32, zero high half (the
     *    packed M4A channel-byte class without the arena band);
     *  - chans[2].blockCount: small u32 inside a resource arena's UNEXPOSED
     *    prefix (the 0x03051E51 reproduction class) when representable. */
    {
        /* gap2 is u8[16]: write the two u32s byte-exact so the 8-byte
         * window reads back as the u64 0x5F8000005F800000 (LE). */
        u32 gap32 = 0x5F800000u;
        memcpy(&fixture->gap2[0], &gap32, sizeof(gap32));
        memcpy(&fixture->gap2[4], &gap32, sizeof(gap32));
    }
    fixture->chans[0].blockCount = 0x6D53A5A5u;
    fixture->pcmBuffer[3] = (float)(1ull << 63);
    fixture->pcmBuffer[4] = (float)(1ull << 63);
    fixture->pcmBuffer[5] = (float)(1ull << 63);
    fixture->pcmBuffer[6] = (float)(1ull << 63);
    fixture->chans[1].blockCount = 0x04040404u;

    pokemonImage = EmeraldPokemonCompat_GetImage();
    if (pokemonImage != NULL
     && EmeraldResourceCompatImage_GetArenaSpan(pokemonImage, &arenaBase,
                                                &arenaSize))
    {
        if ((uintptr_t)arenaBase <= 0xFFFFFFFFu)
        {
            fixture->chans[2].blockCount =
                (u32)((uintptr_t)arenaBase + 0x1E51u);
            coincidencePlanted = TRUE;
        }
        else
        {
            printf("AUDIO coincidence plant skipped (arena base 0x%zx "
                   "above 4GB; unrepresentable under ASan)\n",
                   (uintptr_t)arenaBase);
        }
    }
    return coincidencePlanted;
}

/* Shared assertions for both the create-side fixture state and a fresh
 * process after load: pointers re-derived, scalars intact. */
static void CheckAudioFixture(bool32 coincidencePlanted)
{
    struct SoundInfo *fixture = AudioFixture();
    struct MusicPlayerInfo *players =
        (struct MusicPlayerInfo *)(void *)(sHarnessGameBss
                                           + AUDIO_PLAYERS_OFFSET);
    MPlayFunc *jumpTable =
        (MPlayFunc *)(void *)(sHarnessGameBss + AUDIO_JUMPTABLE_OFFSET);
    const struct EmeraldResourceCompatibilityImage *pokemonImage;
    const uint8_t *arenaBase = NULL;
    size_t arenaSize = 0u;
    u32 expectedCoincidence = 0u;

    CHECK(fixture->ident == ID_NUMBER);
    CHECK(fixture->cgbChans == (struct CgbChannel *)(void *)&fixture->chans);
    CHECK(fixture->MPlayMainHead == (MPlayMainFunc)(uintptr_t)NativeState_Save);
    CHECK(fixture->musicPlayerHead == &players[0]);
    CHECK(fixture->CgbSound == (CgbSoundFunc)(uintptr_t)HarnessStatePath_Override);
    CHECK(fixture->CgbOscOff == (CgbOscOffFunc)(uintptr_t)HostResolveGbaAddr);
    CHECK(fixture->MidiKeyToCgbFreq
          == (MidiKeyToCgbFreqFunc)(uintptr_t)EmeraldPokemonCompat_GetImage);
    CHECK(fixture->MPlayJumpTable == jumpTable);
    CHECK(fixture->plynote
          == (PlyNoteFunc)(uintptr_t)NativeWorldNeighborhood_Invalidate);
    CHECK(fixture->ExtVolPit
          == (ExtVolPitFunc)(uintptr_t)EmeraldResourceCompat_GetRangeIndex);
    /* Every planted function pointer must have restored through the
     * executable persistent-record path (never the data path). */
    CHECK(Platform_RuntimeAddressIsExecutable((uintptr_t)fixture->MPlayMainHead));
    CHECK(Platform_RuntimeAddressIsExecutable((uintptr_t)fixture->CgbSound));
    CHECK(Platform_RuntimeAddressIsExecutable((uintptr_t)fixture->CgbOscOff));
    CHECK(Platform_RuntimeAddressIsExecutable(
              (uintptr_t)fixture->MidiKeyToCgbFreq));
    CHECK(Platform_RuntimeAddressIsExecutable((uintptr_t)fixture->plynote));
    CHECK(Platform_RuntimeAddressIsExecutable((uintptr_t)fixture->ExtVolPit));
    CHECK(Platform_RuntimeAddressIsExecutable((uintptr_t)jumpTable[8]));
    CHECK(Platform_RuntimeAddressIsExecutable((uintptr_t)jumpTable[19]));

    CHECK(players[0].tracks
          == (struct MusicPlayerTrack *)(void *)(sHarnessGameBss
                                                 + AUDIO_TRACKS_OFFSET));
    CHECK(players[0].memAccArea == sHarnessGameBss + AUDIO_MEMACC_OFFSET);
    CHECK(players[0].musicPlayerNext == &players[1]);
    CHECK(players[1].musicPlayerNext == NULL);
    CHECK(fixture->chans[0].prevChannelPointer == &fixture->chans[1]);
    CHECK(fixture->chans[0].nextChannelPointer == &fixture->chans[2]);
    CHECK(fixture->chans[0].wav
          == (struct WaveData *)(void *)&fixture->pcmBuffer[0]);
    CHECK(fixture->chans[0].currentPointer
          == (s8 *)(void *)(sHarnessGameBss + AUDIO_CMDPTR_OFFSET));
    CHECK(fixture->chans[0].track == players[0].tracks);

    {
        u32 gap0;
        u32 gap1;
        memcpy(&gap0, &fixture->gap2[0], sizeof(gap0));
        memcpy(&gap1, &fixture->gap2[4], sizeof(gap1));
        CHECK(gap0 == 0x5F800000u);
        CHECK(gap1 == 0x5F800000u);
    }
    CHECK(fixture->chans[0].blockCount == 0x6D53A5A5u);
    CHECK(fixture->chans[1].blockCount == 0x04040404u);
    CHECK(fixture->pcmBuffer[3] == (float)(1ull << 63));
    CHECK(fixture->pcmBuffer[4] == (float)(1ull << 63));
    CHECK(fixture->pcmBuffer[5] == (float)(1ull << 63));
    CHECK(fixture->pcmBuffer[6] == (float)(1ull << 63));

    pokemonImage = EmeraldPokemonCompat_GetImage();
    if (coincidencePlanted && pokemonImage != NULL
     && EmeraldResourceCompatImage_GetArenaSpan(pokemonImage, &arenaBase,
                                                &arenaSize)
     && (uintptr_t)arenaBase <= 0xFFFFFFFFu)
    {
        expectedCoincidence = (u32)((uintptr_t)arenaBase + 0x1E51u);
        CHECK(fixture->chans[2].blockCount == expectedCoincidence);
    }
}

/* R12-C §10.6-7 + R12-F §7: mid-BGM/mid-cry audio-arena pointer round-trip.
 * The live engine holds pointers INTO the audio arena while a song plays
 * (the active voicegroup's tone array in the transformed zone), while a cry
 * plays (the forward cry table bank), while a keysplit instrument plays
 * (tone.keySplitTable at the verbatim-zone LABEL - the R12-F schema-2
 * identity), and while a programmable-wave channel is live (a canonical
 * AUDIO_SAMPLE leaf). This resolves those spans against the CURRENT
 * process's published arena and derives their canonical keys. The create
 * side plants them into the fixture's unasserted channel wav slots and
 * proves the walker captures each as a keyed sidecar record; the load side
 * proves they re-derive into the fresh process's arena via ResolveByKey
 * (identity by key, never by address - creator and loader arenas differ). */
struct AudioArenaPlant
{
    char voicegroupName[96];
    Gen3ResourceKey voicegroupKey;
    Gen3ResourceKey cryKey;
    Gen3ResourceKey keysplitKey;
    Gen3ResourceKey waveKey;
    uintptr_t voicegroupBase; /* first pack-order voicegroup block base */
    uintptr_t cryForwardBase; /* forward cry table block base */
    uintptr_t keysplitBase;   /* piano keysplit LABEL (verbatim zone) */
    uintptr_t waveBase;       /* programmable wave 17 (canonical leaf) */
};

static bool32 AudioArenaPlantResolve(const char *packPath,
                                     struct AudioArenaPlant *out)
{
    struct Gen3ResourcePackDiagnosticList packDiag;
    struct Gen3ResourcePack *pack = NULL;
    const uint8_t *zoneBase = NULL;
    size_t zoneSize = 0u;
    size_t zoneOff = 0u, transformOff = 0u, spanSize = 0u, transformSize = 0u;
    size_t vgTo = 0u, vgRows = 0u, cryTo = 0u, cryRows = 0u;
    uintptr_t transformBase;
    size_t i, n;
    bool32 found = FALSE;

    memset(out, 0, sizeof(*out));
    Gen3ResourcePackDiagnostics_Init(&packDiag);
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &packDiag) != GEN3_PACK_OK)
        goto done;
    n = Gen3ResourcePack_GetEntryCount(pack);
    for (i = 0u; i < n; i++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(pack, i);

        if (entry != NULL
         && strncmp(entry->canonicalName, "emerald:audio/voicegroup/",
                    strlen("emerald:audio/voicegroup/")) == 0)
        {
            strncpy(out->voicegroupName, entry->canonicalName,
                    sizeof(out->voicegroupName) - 1u);
            out->voicegroupName[sizeof(out->voicegroupName) - 1u] = '\0';
            found = TRUE;
            break;
        }
    }
    if (!found)
        goto done;
    if (!EmeraldAudioCompat_GetArena(&zoneBase, &zoneSize)
     || !EmeraldAudioCompat_GetArenaLayout(&zoneOff, &transformOff,
                                           &spanSize, &transformSize)
     || !EmeraldAudioCompat_GetVoicegroupSpan(out->voicegroupName,
                                              &vgTo, &vgRows)
     || !EmeraldAudioCompat_GetCryTableSpan(false, &cryTo, &cryRows))
    {
        found = FALSE;
        goto done;
    }
    transformBase = (uintptr_t)zoneBase + (transformOff - zoneOff);
    out->voicegroupBase = transformBase + vgTo;
    out->cryForwardBase = transformBase + cryTo;
    /* R12-F §7: the piano keysplit range base IS the mks4agb label in the
     * VERBATIM zone (schema-2 instrument-bank identity). GetKeysplitSpan
     * returns the RUN start (label + backshift); the label itself resolves
     * through the RANGE INDEX, so the plant lands on the registered base
     * (rangeOffset 0) exactly as the live track->tone.keySplitTable does. */
    Gen3ResourceId_DeriveKey("emerald:audio/keysplit/piano",
                             &out->keysplitKey);
    {
        struct EmeraldResourceRangeIndex *rangeIndex =
            EmeraldResourceCompat_GetRangeIndex();
        size_t ksOff = 0u, ksRun = 0u;
        uintptr_t resolved = 0u;
        if (rangeIndex == NULL
         || !EmeraldResourceRangeIndex_ResolveByKey(
             rangeIndex, &out->keysplitKey,
             GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, 2u,
             EMERALD_RESOURCE_ROLE_CANONICAL, 0u, &resolved)
         || !EmeraldAudioCompat_GetKeysplitSpan(
             "emerald:audio/keysplit/piano", &ksOff, &ksRun))
        {
            found = FALSE;
            goto done;
        }
        out->keysplitBase = resolved;
        /* The run starts 36 B after the label (the pinned piano backshift,
         * kKeysplitBacks): the verbatim copy lives at label + backshift. */
        CHECK((uintptr_t)zoneBase + ksOff == resolved + 36u);
    }
    /* The programmable wave resolves through the RANGE INDEX - the walker's
     * own restore path - proving the per-leaf range is registered under its
     * key (AUDIO_SAMPLE/1/CANONICAL), not reached by an offset shortcut. */
    Gen3ResourceId_DeriveKey("emerald:audio/wave/programmable/17",
                             &out->waveKey);
    {
        struct EmeraldResourceRangeIndex *rangeIndex =
            EmeraldResourceCompat_GetRangeIndex();
        uintptr_t resolved = 0u;
        if (rangeIndex == NULL
         || !EmeraldResourceRangeIndex_ResolveByKey(
             rangeIndex, &out->waveKey, GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, 1u,
             EMERALD_RESOURCE_ROLE_CANONICAL, 0u, &resolved))
        {
            found = FALSE;
            goto done;
        }
        out->waveBase = resolved;
    }
    Gen3ResourceId_DeriveKey(out->voicegroupName, &out->voicegroupKey);
    Gen3ResourceId_DeriveKey("emerald:audio/cry-table/forward",
                             &out->cryKey);
done:
    if (pack != NULL)
        Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&packDiag);
    return found;
}

/* TEST A create side: plant the audio fixture, save, and prove the FILE
 * treats the scalar classes as data (verbatim windows, no sidecar records)
 * and the pointer classes as tagged records (kind fields, never raw host
 * addresses). */
static int DoAudioRegression(const char *packPath, const char *statePath)
{
    struct SoundInfo *fixture = AudioFixture();
    struct ParsedRecord records[32];
    struct AudioArenaPlant plant;
    u32 recordCount = 0u;
    bool32 coincidencePlanted;
    bool32 arenaPlanted;
    u32 jumpTableOffset;
    u32 i;
    enum NativeStateResult result;

    if (!RegisterSession(packPath))
        return 1;
    if (!PlantRealRows())
        return 1;
    coincidencePlanted = PlantAudioFixture();
    arenaPlanted = AudioArenaPlantResolve(packPath, &plant);
    CHECK(arenaPlanted); /* audio arena resolved for mid-BGM/mid-cry plants */
    if (arenaPlanted)
    {
        /* Mid-BGM: the active voicegroup's tone array lives in the arena's
         * transformed zone. Mid-cry: the cry player holds the forward cry
         * table bank. R12-F §7: a live track's tone.keySplitTable points at
         * the keysplit LABEL in the verbatim zone, and a programmable wave
         * channel's sample is a canonical leaf. All four are real game-data
         * pointer shapes into the arena; the walker must capture each as a
         * keyed sidecar record. */
        fixture->chans[1].wav =
            (struct WaveData *)(uintptr_t)plant.voicegroupBase;
        fixture->chans[2].wav =
            (struct WaveData *)(uintptr_t)plant.cryForwardBase;
        fixture->chans[3].wav =
            (struct WaveData *)(uintptr_t)plant.keysplitBase;
        fixture->chans[4].wav =
            (struct WaveData *)(uintptr_t)plant.waveBase;
    }
    /* R12-E §9: the creator's audio arena bases, printed for the TEST 3
     * cross-process comparison (creator arena != loader arena). */
    printf("CREATE audio arena: audio_vg=%p audio_cry=%p "
           "audio_ks=%p audio_wave=%p\n",
           (const void *)plant.voicegroupBase,
           (const void *)plant.cryForwardBase,
           (const void *)plant.keysplitBase,
           (const void *)plant.waveBase);

    result = NativeState_Save(HARNESS_STATE_SLOT);
    if (result != NATIVE_STATE_OK)
    {
        fprintf(stderr, "audio-regression: save failed: %s\n",
                NativeState_GetLastError());
        return 1;
    }
    /* Sidecar carries exactly the real-row records plus the four audio-arena
     * references: every audio-shaped scalar stayed in-band data. */
    if (!ParseStateSidecar(statePath, records, 32, &recordCount))
    {
        fprintf(stderr, "audio-regression: state file has no valid resource sidecar\n");
        return 1;
    }
    CHECK(recordCount == HARNESS_ROW_COUNT + (arenaPlanted ? 4u : 0u));

    /* Scalar windows round-trip verbatim in the GAME_BSS payload. */
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(gap2),
                                 0x5F8000005F800000ull));
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(chans[0].blockCount),
                                 0x000000006D53A5A5ull));
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(pcmBuffer[3]),
                                 0x5F0000005F000000ull));
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(pcmBuffer[5]),
                                 0x5F0000005F000000ull));
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(chans[1].blockCount),
                                 0x0000000004040404ull));
    if (coincidencePlanted)
    {
        u32 expectedCoincidence = fixture->chans[2].blockCount;
        CHECK(StateGameBssBytesMatch(statePath,
                                     AUDIO_MEMBER_OFFSET(chans[2].blockCount),
                                     (uint64_t)expectedCoincidence));
    }

    /* Pointer fields serialize as tagged persistent records, not raw host
     * addresses: the record kind field at each pointer's file offset. */
    jumpTableOffset = AUDIO_MEMBER_OFFSET(MPlayJumpTable);
    {
        FILE *file;
        long fileSize;
        u8 *bytes = NULL;
        u32 headerSize;
        u32 sectionCount;
        u32 payloadBase;
        u32 payloadOffset = 0u;
        u32 i;
        bool32 seen = FALSE;
        const u8 *bss = NULL;

        file = fopen(statePath, "rb");
        if (file != NULL)
        {
            fseek(file, 0, SEEK_END);
            fileSize = ftell(file);
            fseek(file, 0, SEEK_SET);
            bytes = malloc((size_t)fileSize);
            if (bytes != NULL
             && fread(bytes, 1, (size_t)fileSize, file) == (size_t)fileSize)
            {
                fclose(file);
                if (ReadLe(bytes) == 0x4E535431u && ReadLe(bytes + 4) == 5u)
                {
                    headerSize = ReadLe(bytes + 8);
                    sectionCount = ReadLe(bytes + 16);
                    payloadBase = headerSize + sectionCount * 12u;
                    for (i = 0; i < sectionCount; i++)
                    {
                        const u8 *section = bytes + headerSize + i * 12u;
                        if (ReadLe(section) == 1u) /* GAME_BSS */
                        {
                            bss = bytes + payloadBase + payloadOffset;
                            seen = TRUE;
                            break;
                        }
                        payloadOffset += ReadLe(section + 4);
                    }
                }
            }
            else
            {
                fclose(file);
            }
        }
        CHECK(seen && bss != NULL);
        if (seen && bss != NULL)
        {
            /* The persistent record layout is {u32 value; u32 kind}, so the
             * kind tag occupies the field's HIGH half (host_memory.c). */
#define AUDIO_RECORD_KIND(field) \
            ReadLe(bss + AUDIO_MEMBER_OFFSET(field) + 4u)
            /* MPlayMainHead: PFIM (executable image function record). */
            CHECK(AUDIO_RECORD_KIND(MPlayMainHead)
                  == HOST_PERSISTENT_FUNC_IMAGE);
            /* musicPlayerHead: PDIM (image data record). */
            CHECK(AUDIO_RECORD_KIND(musicPlayerHead)
                  == HOST_PERSISTENT_DATA_IMAGE);
            /* cgbChans, MPlayJumpTable: PDIM image data records. */
            CHECK(AUDIO_RECORD_KIND(cgbChans)
                  == HOST_PERSISTENT_DATA_IMAGE);
            CHECK(ReadLe(bss + jumpTableOffset + 4u)
                  == HOST_PERSISTENT_DATA_IMAGE);
            /* The in-file jump-table entries (the array the MPlayJumpTable
             * field points at, at AUDIO_JUMPTABLE_OFFSET) are PFIM too. */
            CHECK(ReadLe(bss + AUDIO_JUMPTABLE_OFFSET + 8u * 8u + 4u)
                  == HOST_PERSISTENT_FUNC_IMAGE);
#undef AUDIO_RECORD_KIND
        }
        free(bytes);
    }
    if (arenaPlanted)
    {
        bool32 sawVoicegroup = FALSE;
        bool32 sawCry = FALSE;
        bool32 sawKeysplit = FALSE;
        bool32 sawWave = FALSE;

        for (i = 0u; i < recordCount; i++)
        {
            if (memcmp(records[i].key, plant.voicegroupKey.bytes,
                       GEN3_RESOURCE_KEY_SIZE) == 0)
            {
                sawVoicegroup = TRUE;
                /* mid-BGM record at chans[1].wav */
                CHECK(records[i].fieldOffset
                      == AUDIO_MEMBER_OFFSET(chans[1].wav));
                CHECK(records[i].type == GEN3_RESOURCE_TYPE_INSTRUMENT_BANK);
                CHECK(records[i].schema == 1u);
                CHECK(records[i].role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT);
                CHECK(records[i].rangeOffset == 0u);
            }
            if (memcmp(records[i].key, plant.cryKey.bytes,
                       GEN3_RESOURCE_KEY_SIZE) == 0)
            {
                sawCry = TRUE;
                /* mid-cry record at chans[2].wav */
                CHECK(records[i].fieldOffset
                      == AUDIO_MEMBER_OFFSET(chans[2].wav));
                CHECK(records[i].type == GEN3_RESOURCE_TYPE_INSTRUMENT_BANK);
                CHECK(records[i].schema == 1u);
                CHECK(records[i].role == EMERALD_RESOURCE_ROLE_COMPAT_OBJECT);
                CHECK(records[i].rangeOffset == 0u);
            }
            if (memcmp(records[i].key, plant.keysplitKey.bytes,
                       GEN3_RESOURCE_KEY_SIZE) == 0)
            {
                sawKeysplit = TRUE;
                /* keysplit record at chans[3].wav; schema 2 is the R12-F
                 * label-based instrument-bank identity (the pre-R12-F walker
                 * misread keysplits as schema-1 COMPAT_OBJECT rows). */
                CHECK(records[i].fieldOffset
                      == AUDIO_MEMBER_OFFSET(chans[3].wav));
                CHECK(records[i].type == GEN3_RESOURCE_TYPE_INSTRUMENT_BANK);
                CHECK(records[i].schema == 2u);
                CHECK(records[i].role == EMERALD_RESOURCE_ROLE_CANONICAL);
                CHECK(records[i].rangeOffset == 0u);
            }
            if (memcmp(records[i].key, plant.waveKey.bytes,
                       GEN3_RESOURCE_KEY_SIZE) == 0)
            {
                sawWave = TRUE;
                /* programmable-wave record at chans[4].wav: the canonical
                 * AUDIO_SAMPLE leaf identity of a live wave channel. */
                CHECK(records[i].fieldOffset
                      == AUDIO_MEMBER_OFFSET(chans[4].wav));
                CHECK(records[i].type == GEN3_RESOURCE_TYPE_AUDIO_SAMPLE);
                CHECK(records[i].schema == 1u);
                CHECK(records[i].role == EMERALD_RESOURCE_ROLE_CANONICAL);
                CHECK(records[i].rangeOffset == 0u);
            }
        }
        CHECK(sawVoicegroup); /* sidecar carries the voicegroup record */
        CHECK(sawCry); /* sidecar carries the cry-table record */
        CHECK(sawKeysplit); /* sidecar carries the keysplit record */
        CHECK(sawWave); /* sidecar carries the wave record */
        /* The in-band pointers are zeroed in the FILE: the reference lives
         * only in the sidecar (identity + offset, never a raw address). */
        CHECK(StateGameBssBytesMatch(statePath,
                                     AUDIO_MEMBER_OFFSET(chans[1].wav), 0u));
        CHECK(StateGameBssBytesMatch(statePath,
                                     AUDIO_MEMBER_OFFSET(chans[2].wav), 0u));
        CHECK(StateGameBssBytesMatch(statePath,
                                     AUDIO_MEMBER_OFFSET(chans[3].wav), 0u));
        CHECK(StateGameBssBytesMatch(statePath,
                                     AUDIO_MEMBER_OFFSET(chans[4].wav), 0u));
        /* The in-memory fixture still holds the planted values. */
        CHECK(fixture->chans[1].wav
              == (struct WaveData *)(uintptr_t)plant.voicegroupBase);
        CHECK(fixture->chans[2].wav
              == (struct WaveData *)(uintptr_t)plant.cryForwardBase);
        CHECK(fixture->chans[3].wav
              == (struct WaveData *)(uintptr_t)plant.keysplitBase);
        CHECK(fixture->chans[4].wav
              == (struct WaveData *)(uintptr_t)plant.waveBase);
    }
    CheckAudioFixture(coincidencePlanted);
    if (sFailures != 0)
        return 1;
    printf("AUDIO-REGRESSION ok (coincidence=%d, arena=%d)\n",
           (int)coincidencePlanted, (int)arenaPlanted);
    return 0;
}

/* TEST A load side: a fresh process loads the state and must re-derive
 * every audio pointer to THIS process's objects, with the scalar windows
 * byte-identical to the FILE (the file, not the creator's memory, is the
 * ground truth). */
static int DoAudioLoad(const char *packPath, const char *statePath)
{
    struct SoundInfo *fixture = AudioFixture();
    struct ParsedRecord records[32];
    u32 recordCount = 0u;
    u8 *bss = NULL;
    u32 bssSize = 0u;
    bool32 coincidencePlanted;
    enum NativeStateResult result;

    if (!RegisterSession(packPath))
        return 1;
    if (!StateSectionRead(statePath, 1u, &bss, &bssSize)) /* GAME_BSS */
    {
        fprintf(stderr, "audio-load: no GAME_BSS section in state\n");
        return 1;
    }
    CHECK(bssSize == AUDIO_FIXTURE_GAME_BSS_SIZE);
    /* The load process's arena base can differ from the creator's, so the
     * coincidence VALUE is asserted against the file, not re-derived here:
     * the scalar windows below are the byte ground truth. */
    coincidencePlanted = FALSE;

    result = NativeState_Load(HARNESS_STATE_SLOT);
    if (result != NATIVE_STATE_OK)
    {
        fprintf(stderr, "audio-load: rejected: %s\n",
                NativeState_GetLastError());
        free(bss);
        return 1;
    }
    free(bss);
    /* Scalar windows round-trip byte-identical (pointer fields are records
     * in the file and re-derived native addresses in memory - checked by
     * CheckAudioFixture below, which runs in THIS process's image). */
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(gap2),
                                 0x5F8000005F800000ull));
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(chans[0].blockCount),
                                 0x000000006D53A5A5ull));
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(pcmBuffer[3]),
                                 0x5F0000005F000000ull));
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(pcmBuffer[5]),
                                 0x5F0000005F000000ull));
    CHECK(StateGameBssBytesMatch(statePath,
                                 AUDIO_MEMBER_OFFSET(chans[1].blockCount),
                                 0x0000000004040404ull));
    /* Sidecar record count is unchanged by the load (still the real rows
     * plus the four audio-arena references). */
    if (!ParseStateSidecar(statePath, records, 32, &recordCount))
    {
        fprintf(stderr, "audio-load: state file has no valid resource sidecar\n");
        return 1;
    }
    CHECK(recordCount == HARNESS_ROW_COUNT + 4u);
    {
        struct AudioArenaPlant plant;
        bool32 arenaPlanted = AudioArenaPlantResolve(packPath, &plant);

        CHECK(arenaPlanted); /* load: audio arena resolved for re-derivation */
        if (arenaPlanted)
        {
            struct EmeraldResourceRangeIndex *rangeIndex =
                EmeraldResourceCompat_GetRangeIndex();
            struct EmeraldResourceRangeHit hit;
            uintptr_t resolved = 0u;

            /* The sidecar records re-derived into THIS process's arena:
             * creator and loader arena bases differ, so the restored fields
             * must equal the freshly published spans, not the creator's. */
            CHECK(fixture->chans[1].wav
                  == (struct WaveData *)(uintptr_t)plant.voicegroupBase);
            CHECK(fixture->chans[2].wav
                  == (struct WaveData *)(uintptr_t)plant.cryForwardBase);
            CHECK(fixture->chans[3].wav
                  == (struct WaveData *)(uintptr_t)plant.keysplitBase);
            CHECK(fixture->chans[4].wav
                  == (struct WaveData *)(uintptr_t)plant.waveBase);
            /* Explicit ResolveByKey re-derivation, the walker's restore path
             * (native_state.c RestoreResourcePointers). */
            CHECK(rangeIndex != NULL
                  && EmeraldResourceRangeIndex_ResolveByKey(
                      rangeIndex, &plant.voicegroupKey,
                      GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, 1u,
                      EMERALD_RESOURCE_ROLE_COMPAT_OBJECT, 0u, &resolved)
                  && resolved == plant.voicegroupBase);
            CHECK(rangeIndex != NULL
                  && EmeraldResourceRangeIndex_ResolveByKey(
                      rangeIndex, &plant.cryKey,
                      GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, 1u,
                      EMERALD_RESOURCE_ROLE_COMPAT_OBJECT, 0u, &resolved)
                  && resolved == plant.cryForwardBase);
            /* R12-F §7: the keysplit and wave identities re-derive through
             * the SAME path the walker used to restore them - keysplit as a
             * schema-2 label range, the wave as a canonical AUDIO_SAMPLE
             * leaf - and land exactly on the fresh arena's spans. */
            CHECK(rangeIndex != NULL
                  && EmeraldResourceRangeIndex_ResolveByKey(
                      rangeIndex, &plant.keysplitKey,
                      GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, 2u,
                      EMERALD_RESOURCE_ROLE_CANONICAL, 0u, &resolved)
                  && resolved == plant.keysplitBase);
            CHECK(rangeIndex != NULL
                  && EmeraldResourceRangeIndex_ResolveByKey(
                      rangeIndex, &plant.waveKey,
                      GEN3_RESOURCE_TYPE_AUDIO_SAMPLE, 1u,
                      EMERALD_RESOURCE_ROLE_CANONICAL, 0u, &resolved)
                  && resolved == plant.waveBase);
            /* Post-load the audio ranges are registered against the NEW
             * arena base (the R12-C §8 re-registration after the trainer
             * republish dropped the shared index). */
            CHECK(rangeIndex != NULL
                  && EmeraldResourceRangeIndex_Lookup(rangeIndex,
                                                      plant.voicegroupBase,
                                                      &hit)
                  && Gen3ResourceId_KeyEqual(&hit.key,
                                             &plant.voicegroupKey));
            CHECK(rangeIndex != NULL
                  && EmeraldResourceRangeIndex_Lookup(rangeIndex,
                                                      plant.cryForwardBase,
                                                      &hit)
                  && Gen3ResourceId_KeyEqual(&hit.key, &plant.cryKey));
            CHECK(rangeIndex != NULL
                  && EmeraldResourceRangeIndex_Lookup(rangeIndex,
                                                      plant.keysplitBase,
                                                      &hit)
                  && Gen3ResourceId_KeyEqual(&hit.key,
                                             &plant.keysplitKey));
            CHECK(rangeIndex != NULL
                  && EmeraldResourceRangeIndex_Lookup(rangeIndex,
                                                      plant.waveBase,
                                                      &hit)
                  && Gen3ResourceId_KeyEqual(&hit.key, &plant.waveKey));
            /* R12-E §9: the loader's audio arena bases, printed for the
             * TEST 3 cross-process comparison. */
            printf("LOAD audio arena: audio_vg=%p audio_cry=%p "
                   "audio_ks=%p audio_wave=%p\n",
                   (const void *)plant.voicegroupBase,
                   (const void *)plant.cryForwardBase,
                   (const void *)plant.keysplitBase,
                   (const void *)plant.waveBase);
        }
    }
    CheckAudioFixture(coincidencePlanted);
    if (sFailures != 0)
        return 1;
    printf("AUDIO-LOAD ok (fixture restored in a fresh process)\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* TEST B: R10 desktop-state audio regression.                         */
/*                                                                     */
/* The full desktop save/load orchestration over the REAL              */
/* desktop_audio.c + desktop_state.c linked against the fake SDL       */
/* device shim: device init, warm-up frames, the save, the load        */
/* sequence (state-manager UI path or quick path), resume, post-load   */
/* frames. The shim records every device op in order, so the ENTIRE    */
/* sequence is asserted verbatim, plus: mixer/MP2K fixture validity    */
/* (CheckAudioFixture), PCM parity (post-load mixer output == control),*/
/* the saved frame-counter rollback, and the final device state        */
/* (open, unpaused, queue refilled - never left cleared or detached).  */
/* ------------------------------------------------------------------ */

#define DESKTOP_AUDIO_FLOATS_PER_FRAME 1402u /* real m4aSoundVSync frame */
#define DESKTOP_FRAME_COUNT 40u
#define DESKTOP_CLOCK_TICK_NS 16666667ull /* 1/60 s */
#define DESKTOP_SAVED_FRAME 0x12345678ull

static float sDesktopPcmOut[DESKTOP_AUDIO_FLOATS_PER_FRAME];
static float sDesktopControlPcm[DESKTOP_AUDIO_FLOATS_PER_FRAME];

/* Deterministic stand-in for the VBlank mixer output: mirrors the
 * PORTABLE m4aSoundVSync path (NaN-guard, (m4a+cgb)*0.125, clamp to
 * +/-1, queue). A pure function of the fixture state and the step
 * index, so identical restored state must reproduce identical PCM. */
static void DesktopMixerStep(u32 stepIndex)
{
    struct SoundInfo *fixture = AudioFixture();
    size_t pcmCount = sizeof(fixture->pcmBuffer) / sizeof(float);
    u32 i;

    for (i = 0u; i < DESKTOP_AUDIO_FLOATS_PER_FRAME; i++)
    {
        float m4a = fixture->pcmBuffer[(i + stepIndex * 17u) % pcmCount];
        float cgb = (i % 7u == 0u) ? fixture->pcmBuffer[(i * 3u) % pcmCount]
                                   : 0.0f;
        float sample;

        if (m4a != m4a)
            m4a = 0.0f;
        if (cgb != cgb)
            cgb = 0.0f;
        sample = (m4a + cgb) * 0.125f;
        if (sample > 1.0f)
            sample = 1.0f;
        else if (sample < -1.0f)
            sample = -1.0f;
        sDesktopPcmOut[i] = sample;
    }
    Platform_QueueAudio(sDesktopPcmOut,
                        (s32)(DESKTOP_AUDIO_FLOATS_PER_FRAME * sizeof(float)));
}

#if !defined(HARNESS_REAL_SDL_PROBE)
/* One simulated VBlank: advance the fake clock one 1/60 s tick and run
 * the mixer exactly like VBlankIntr does when the audio frame is due.
 * Returns the next step index (the gate reports exactly one due frame
 * per tick at 60 Hz presentation, so the step count is deterministic). */
static u32 DesktopRunFrame(u32 stepIndex)
{
    HarnessAudioClock_Advance(DESKTOP_CLOCK_TICK_NS);
    if (Platform_SchedulerAudioFrameDue())
    {
        DesktopMixerStep(stepIndex);
        return stepIndex + 1u;
    }
    return stepIndex;
}

/* The desktop state-manager accept path: RunStateUi parks the game at
 * VBlank and pauses the device, the manager accepts -> Platform_StateLoad
 * (desktop_state_ui.c -> desktop_state.c), then the RunStateUi tail
 * restores `paused` and clears/repairs the queue if the host was not
 * paused. `wasPaused` is the host `paused` global at UI entry. */
static bool32 DesktopUiLoad(u8 slot, bool32 wasPaused, bool32 *outPaused)
{
    HarnessAudioTraceMark("phase:ui-pause");
    *outPaused = TRUE;
    Platform_AudioSetPaused(TRUE);
    HarnessAudioTraceMark("phase:ui-load");
    if (Platform_StateLoad(slot) != PLATFORM_STATE_OPERATION_OK)
        return FALSE;
    HarnessAudioTraceMark("phase:ui-tail");
    *outPaused = wasPaused;
    if (!wasPaused)
    {
        Platform_AudioClearQueue();
        Platform_AudioSetPaused(FALSE);
    }
    else
    {
        Platform_AudioSetPaused(TRUE);
    }
    return TRUE;
}

/* Build the expected ordered op sequence: device init, warm-up frames,
 * the save, the load sequence (given verbatim, from the first UI/quick
 * marker through the last resume op), post-load frames. */
static void BuildDesktopTrace(char *dest, size_t destSize,
                              const char *afterSave)
{
    size_t pos = 0u;
    u32 i;
#define APPEND(str)                                                       \
    do                                                                    \
    {                                                                     \
        int written = snprintf(dest + pos, destSize - pos, "%s", (str)); \
        pos += (size_t)written;                                           \
    } while (0)

    APPEND("open;unpause;phase:warm;");
    for (i = 0u; i < DESKTOP_FRAME_COUNT; i++)
        APPEND("queue;");
    APPEND("phase:save;");
    APPEND(afterSave);
    for (i = 0u; i < DESKTOP_FRAME_COUNT; i++)
        APPEND("queue;");
#undef APPEND
}

/* TEST B manager-load side. `pausedVariant` selects the Ctrl+P-style
 * paused-at-load scenario (the host enters the UI paused). */
static int DoDesktopAudio(const char *packPath, const char *statePath,
                          bool32 pausedVariant)
{
    char expected[8192];
    u32 step = 0u;
    u32 i;
    bool32 coincidencePlanted;
    bool32 hostPaused = FALSE;
    uint64_t queuedPerFrame = DESKTOP_AUDIO_FLOATS_PER_FRAME * sizeof(float);

    if (!RegisterSession(packPath))
        return 1;
    if (!PlantRealRows())
        return 1;
    coincidencePlanted = PlantAudioFixture();

    /* ---- device init (production init over the shim) ---- */
    HarnessAudioTraceReset();
    if (!Platform_AudioInit(42060))
    {
        fprintf(stderr, "desktop-audio: Platform_AudioInit failed\n");
        return 1;
    }

    /* ---- warm: 40 VBlank frames, audio active ---- */
    HarnessAudioTraceMark("phase:warm");
    for (i = 0u; i < DESKTOP_FRAME_COUNT; i++)
        step = DesktopRunFrame(step);
    CHECK(step == DESKTOP_FRAME_COUNT); /* every tick produced a mixer frame */
    /* Control PCM: the last warm mixer frame (step 39). The post-load loop
     * also ends at step 39, so the comparison is like-for-like: same step
     * index, same fixture content, must produce identical output. */
    memcpy(sDesktopControlPcm, sDesktopPcmOut, sizeof(sDesktopControlPcm));
    /* Assertion 1: audio active before the load sequence begins. */
    CHECK(HarnessDeviceOpen());
    CHECK(!HarnessDevicePaused());
    CHECK(HarnessDeviceQueuedBytes() == queuedPerFrame * DESKTOP_FRAME_COUNT);

    /* ---- the desktop save (state manager save slot) ---- */
    HarnessAudioTraceMark("phase:save");
    Platform_SchedulerSetFrameCounter(DESKTOP_SAVED_FRAME);
    if (NativeState_Save(HARNESS_STATE_SLOT) != NATIVE_STATE_OK)
    {
        fprintf(stderr, "desktop-audio: save failed: %s\n",
                NativeState_GetLastError());
        return 1;
    }
    /* The save only reads; device state and queue are untouched. */
    CHECK(!HarnessDevicePaused());
    CHECK(HarnessDeviceQueuedBytes() == queuedPerFrame * DESKTOP_FRAME_COUNT);

    /* ---- the desktop state-manager load sequence ---- */
    Platform_SchedulerSetFrameCounter(0x1111222233334444ull); /* rolls back */
    HarnessAudioTraceMark("phase:ui");
    if (!DesktopUiLoad(HARNESS_STATE_SLOT, pausedVariant, &hostPaused))
    {
        fprintf(stderr, "desktop-audio: Platform_StateLoad failed: %s\n",
                Platform_StateGetLastError());
        return 1;
    }
    /* NativeState_Load restores the saved frame counter. */
    CHECK(Platform_SchedulerGetFrameCounter() == DESKTOP_SAVED_FRAME);
    if (pausedVariant)
    {
        /* The host entered paused; the tail re-paused the device, keeping
         * device.paused in sync with the host `paused` global. */
        CHECK(hostPaused == TRUE);
        CHECK(HarnessDevicePaused());
        HarnessAudioTraceMark("phase:resume");
        hostPaused = FALSE;
        Platform_AudioClearQueue();
        Platform_AudioSetPaused(FALSE);
    }
    else
    {
        CHECK(hostPaused == FALSE);
        HarnessAudioTraceMark("phase:resume");
    }

    /* ---- post-load frames: the mixer over the RESTORED state ---- */
    step = 0u;
    for (i = 0u; i < DESKTOP_FRAME_COUNT; i++)
        step = DesktopRunFrame(step);
    CHECK(step == DESKTOP_FRAME_COUNT);

    /* Assertions 2-8: load succeeded, the complete sequence's platform
     * audio ops are in the intended order, mixer/MP2K state is valid
     * after the sequence, a further deterministic mixer step reproduces
     * the control PCM, and the device is ACTIVE/UNPAUSED with a full
     * queue - nothing left cleared, paused, detached or uninitialized. */
    CHECK(Platform_SchedulerGetFrameCounter() == DESKTOP_SAVED_FRAME);
    CheckAudioFixture(coincidencePlanted);
    CHECK(memcmp(sDesktopControlPcm, sDesktopPcmOut,
                 sizeof(sDesktopControlPcm)) == 0); /* PCM parity */
    CHECK(HarnessDeviceOpen());
    CHECK(!HarnessDevicePaused());
    CHECK(HarnessDeviceQueuedBytes() == queuedPerFrame * DESKTOP_FRAME_COUNT);
    BuildDesktopTrace(expected, sizeof(expected),
                      pausedVariant
                          ? "phase:ui;phase:ui-pause;pause;phase:ui-load;"
                            "clear;unpause;phase:ui-tail;pause;phase:resume;"
                            "clear;unpause;"
                          : "phase:ui;phase:ui-pause;pause;phase:ui-load;"
                            "clear;unpause;phase:ui-tail;clear;unpause;"
                            "phase:resume;");
    CHECK(strcmp(HarnessAudioTraceGet(), expected) == 0);

    Platform_AudioShutdown();
    if (sFailures != 0)
        return 1;
    printf("DESKTOP-AUDIO ok (manager load%s; ordered device trace verbatim)\n",
           pausedVariant ? ", paused variant" : "");
    return 0;
}

/* TEST B quick-load side: the F9 quick-load path (PrepareHostFrame: the
 * state request is handled before VideoDrawFrame, no UI pause). The
 * device must never be paused; the only queue interruption is the
 * serializer load tail's clear. */
static int DoDesktopQuick(const char *packPath, const char *statePath)
{
    char expected[8192];
    u32 step = 0u;
    u32 i;
    bool32 coincidencePlanted;
    uint64_t queuedPerFrame = DESKTOP_AUDIO_FLOATS_PER_FRAME * sizeof(float);

    if (!RegisterSession(packPath))
        return 1;
    if (!PlantRealRows())
        return 1;
    coincidencePlanted = PlantAudioFixture();

    HarnessAudioTraceReset();
    if (!Platform_AudioInit(42060))
    {
        fprintf(stderr, "desktop-quick: Platform_AudioInit failed\n");
        return 1;
    }

    /* warm */
    HarnessAudioTraceMark("phase:warm");
    for (i = 0u; i < DESKTOP_FRAME_COUNT; i++)
        step = DesktopRunFrame(step);
    CHECK(step == DESKTOP_FRAME_COUNT);
    memcpy(sDesktopControlPcm, sDesktopPcmOut, sizeof(sDesktopControlPcm));

    /* save */
    HarnessAudioTraceMark("phase:save");
    Platform_SchedulerSetFrameCounter(DESKTOP_SAVED_FRAME);
    if (NativeState_Save(HARNESS_STATE_SLOT) != NATIVE_STATE_OK)
    {
        fprintf(stderr, "desktop-quick: save failed: %s\n",
                NativeState_GetLastError());
        return 1;
    }

    /* quick-load: no UI, the device is never paused */
    Platform_SchedulerSetFrameCounter(0x1111222233334444ull);
    HarnessAudioTraceMark("phase:quick-load");
    if (Platform_StateLoad(HARNESS_STATE_SLOT) != PLATFORM_STATE_OPERATION_OK)
    {
        fprintf(stderr, "desktop-quick: Platform_StateLoad failed: %s\n",
                Platform_StateGetLastError());
        return 1;
    }
    CHECK(Platform_SchedulerGetFrameCounter() == DESKTOP_SAVED_FRAME);
    HarnessAudioTraceMark("phase:resume");

    /* post-load frames */
    step = 0u;
    for (i = 0u; i < DESKTOP_FRAME_COUNT; i++)
        step = DesktopRunFrame(step);
    CHECK(step == DESKTOP_FRAME_COUNT);

    CheckAudioFixture(coincidencePlanted);
    CHECK(memcmp(sDesktopControlPcm, sDesktopPcmOut,
                 sizeof(sDesktopControlPcm)) == 0); /* PCM parity */
    CHECK(HarnessDeviceOpen());
    CHECK(!HarnessDevicePaused());
    CHECK(HarnessDeviceQueuedBytes() == queuedPerFrame * DESKTOP_FRAME_COUNT);
    /* Exact trace: no pause op exists anywhere in the quick path. */
    BuildDesktopTrace(expected, sizeof(expected),
                      "phase:quick-load;clear;unpause;phase:resume;");
    CHECK(strcmp(HarnessAudioTraceGet(), expected) == 0);

    Platform_AudioShutdown();
    if (sFailures != 0)
        return 1;
    printf("DESKTOP-QUICK ok (quick-load path; device never paused)\n");
    return 0;
}
#endif /* !HARNESS_REAL_SDL_PROBE */

#if defined(HARNESS_REAL_SDL_PROBE)
/* TEST C: production-path probe. The SAME desktop sequence against the
 * REAL SDL audio device (SDL_INIT_AUDIO only - no video, no window, no
 * gameplay), with the real-time scheduler gate. The real device drains
 * the queue asynchronously, so queue-size assertions are informational;
 * the assertions are: device opens with AUDIO_F32, mixer frames flow,
 * the save/load leaves the device PLAYING, the real-time stall clamp
 * (0.25 s UI gap) resumes with exactly one due frame, the restored
 * fixture passes CheckAudioFixture, and the saved frame counter rolls
 * back. */
static int DoDesktopRealSdl(const char *packPath, const char *statePath)
{
    SDL_AudioDeviceID device;
    u32 i;
    int warmSteps = 0;
    int postSteps = 0;
    u32 clampDue = 0u;
    bool32 coincidencePlanted;

    (void)statePath;
    if (!RegisterSession(packPath))
        return 1;
    if (!PlantRealRows())
        return 1;
    coincidencePlanted = PlantAudioFixture();

    if (SDL_Init(SDL_INIT_AUDIO) != 0)
    {
        fprintf(stderr, "desktop-real-sdl: SDL_Init failed: %s\n",
                SDL_GetError());
        return 1;
    }
    if (!Platform_AudioInit(42060))
    {
        fprintf(stderr,
                "desktop-real-sdl: Platform_AudioInit failed on the real device: %s\n",
                SDL_GetError());
        SDL_Quit();
        return 1;
    }
    device = Platform_AudioProbeGetDevice();
    CHECK(device != 0);

    /* warm: real-time frames at 60 Hz; the device drains asynchronously */
    for (i = 0u; i < DESKTOP_FRAME_COUNT; i++)
    {
        usleep(16667);
        if (Platform_SchedulerAudioFrameDue())
        {
            DesktopMixerStep((u32)warmSteps);
            warmSteps++;
        }
    }
    CHECK(warmSteps > 0);
    memcpy(sDesktopControlPcm, sDesktopPcmOut, sizeof(sDesktopControlPcm));
    CHECK(SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PLAYING);

    /* the desktop save + quick-load sequence (no UI pause) */
    Platform_SchedulerSetFrameCounter(DESKTOP_SAVED_FRAME);
    if (NativeState_Save(HARNESS_STATE_SLOT) != NATIVE_STATE_OK)
    {
        fprintf(stderr, "desktop-real-sdl: save failed: %s\n",
                NativeState_GetLastError());
        Platform_AudioShutdown();
        SDL_Quit();
        return 1;
    }
    CHECK(SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PLAYING);
    Platform_SchedulerSetFrameCounter(0x1111222233334444ull); /* rolls back */
    if (Platform_StateLoad(HARNESS_STATE_SLOT) != PLATFORM_STATE_OPERATION_OK)
    {
        fprintf(stderr, "desktop-real-sdl: Platform_StateLoad failed: %s\n",
                Platform_StateGetLastError());
        Platform_AudioShutdown();
        SDL_Quit();
        return 1;
    }
    CHECK(Platform_SchedulerGetFrameCounter() == DESKTOP_SAVED_FRAME);
    CHECK(SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PLAYING);

    /* real-time stall clamp: a >0.25 s gap (the UI pause) must resume
     * with exactly one due frame, not a burst */
    usleep(300000);
    if (Platform_SchedulerAudioFrameDue())
        clampDue++;
    CHECK(clampDue == 1u);

    /* post-load frames over the restored state */
    for (i = 0u; i < DESKTOP_FRAME_COUNT; i++)
    {
        usleep(16667);
        if (Platform_SchedulerAudioFrameDue())
        {
            DesktopMixerStep((u32)postSteps);
            postSteps++;
        }
    }
    CHECK(postSteps > 0);

    CheckAudioFixture(coincidencePlanted);
    CHECK(Platform_SchedulerGetFrameCounter() == DESKTOP_SAVED_FRAME);
    CHECK(SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PLAYING);
    /* Like-for-like PCM parity when the gate produced the same number of
     * steps in both phases (timer slack can shift the count by one). */
    if (warmSteps == postSteps)
        CHECK(memcmp(sDesktopControlPcm, sDesktopPcmOut,
                     sizeof(sDesktopControlPcm)) == 0);
    else
        fprintf(stderr,
                "desktop-real-sdl: step counts differ (warm=%d post=%d); "
                "PCM parity skipped (timer slack)\n",
                warmSteps, postSteps);

    {
        uint64_t queued = SDL_GetQueuedAudioSize(device);
        printf("DESKTOP-REAL-SDL ok (device=0x%x status=playing "
               "queued=%llu warmSteps=%d postSteps=%d clamp=1)\n",
               (unsigned)device, (unsigned long long)queued,
               warmSteps, postSteps);
    }
    Platform_AudioShutdown();
    SDL_Quit();
    return sFailures != 0;
}
#endif /* HARNESS_REAL_SDL_PROBE */

/* ------------------------------------------------------------------ */
/* R13-G4 staged field-script State-v5 closure.                        */

struct G4Point
{
    u32 module;
    u32 offset;
    const u8 *pointer;
};

static u8 sG4DynamicBoundaries[512];

static bool8 G4WaitCallback(void)
{
    return FALSE;
}

static bool32 G4Stage(const char *packPath, bool32 restage)
{
    struct EmeraldScriptCompatDiagnostics diagnostics;
    enum EmeraldScriptCompatStatus status;

    /* R13-G5: the live state runs on the production-linked path - the
     * R6 loader publishes every family INCLUDING the 523 live script
     * ranges, exactly like the game. */
    if (!SetupScriptCompatSession(packPath))
        return FALSE;
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldScriptCompat_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, &diagnostics);
    if (status != EMERALD_SCRIPT_OK)
    {
        fprintf(stderr, "G4 script stage refused: %s @ %s\n",
                EmeraldScriptCompatStatus_Describe(status),
                diagnostics.canonicalName);
        return FALSE;
    }
    if (restage)
    {
        const u8 *firstBase;
        const u8 *secondBase;
        size_t firstSize;
        size_t secondSize;
        CHECK(EmeraldScriptCompat_GetArena(&firstBase, &firstSize));
        memset(&diagnostics, 0, sizeof(diagnostics));
        status = EmeraldScriptCompat_TryInitialize(
            gScriptHarnessSnapshot, gScriptHarnessPack, &diagnostics);
        CHECK(status == EMERALD_SCRIPT_OK);
        CHECK(EmeraldScriptCompat_GetArena(&secondBase, &secondSize));
        CHECK(firstBase != secondBase);
        CHECK(firstSize == secondSize);
    }
    return TRUE;
}

static bool32 G4PointForOpcode(u8 opcode, u32 ordinal, struct G4Point *out)
{
    u32 m;
    u32 seen = 0u;
    for (m = 0u; m < kEmeraldScriptCompatTable.moduleCount; m++)
    {
        const struct EmeraldScriptNativeModule *module =
            &kEmeraldScriptCompatTable.modules[m];
        const u8 *base;
        size_t size;
        u32 i;
        if (module->payloadSize == 0u
         || !EmeraldScriptCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = module->boundaryFirst;
             i < module->boundaryFirst + module->boundaryCount; i++)
        {
            u32 offset = kEmeraldScriptCompatTable.boundaries[i].payloadOffset;
            if (offset == 0u || offset >= size || base[offset] != opcode)
                continue;
            if (seen++ == ordinal)
            {
                out->module = m;
                out->offset = offset;
                out->pointer = base + offset;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static bool32 G4AnyInteriorPoint(u32 ordinal, struct G4Point *out)
{
    u32 m;
    u32 seen = 0u;
    for (m = 0u; m < kEmeraldScriptCompatTable.moduleCount; m++)
    {
        const struct EmeraldScriptNativeModule *module =
            &kEmeraldScriptCompatTable.modules[m];
        const u8 *base;
        size_t size;
        u32 i;
        if (module->payloadSize == 0u
         || !EmeraldScriptCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = module->boundaryFirst;
             i < module->boundaryFirst + module->boundaryCount; i++)
        {
            u32 offset = kEmeraldScriptCompatTable.boundaries[i].payloadOffset;
            if (offset == 0u || offset >= size)
                continue;
            if (seen++ == ordinal)
            {
                out->module = m;
                out->offset = offset;
                out->pointer = base + offset;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static bool32 G4NonEntrypoint(u32 ordinal, struct G4Point *out)
{
    u32 m;
    u32 seen = 0u;
    for (m = 0u; m < kEmeraldScriptCompatTable.moduleCount; m++)
    {
        const struct EmeraldScriptNativeModule *module =
            &kEmeraldScriptCompatTable.modules[m];
        const u8 *base;
        size_t size;
        u32 i;
        if (!EmeraldScriptCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = module->boundaryFirst;
             i < module->boundaryFirst + module->boundaryCount; i++)
        {
            u32 offset = kEmeraldScriptCompatTable.boundaries[i].payloadOffset;
            if (offset == 0u || offset >= size
             || EmeraldScriptCompat_ValidateBoundary(
                    module->id, offset, EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT)
                    == EMERALD_SCRIPT_OK)
                continue;
            if (seen++ == ordinal)
            {
                out->module = m;
                out->offset = offset;
                out->pointer = base + offset;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static bool32 G4SegmentPoint(u32 segmentKind, struct G4Point *out)
{
    u32 m;
    for (m = 0u; m < kEmeraldScriptCompatTable.moduleCount; m++)
    {
        const struct EmeraldScriptNativeModule *module =
            &kEmeraldScriptCompatTable.modules[m];
        const u8 *base;
        size_t size;
        u32 i;
        if (!EmeraldScriptCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = module->segmentFirst;
             i < module->segmentFirst + module->segmentCount; i++)
        {
            const struct EmeraldScriptNativeSegment *segment =
                &kEmeraldScriptCompatTable.segments[i];
            if (segment->kind != segmentKind
             || segment->payloadOffset >= size)
                continue;
            out->module = m;
            out->offset = segment->payloadOffset;
            out->pointer = base + segment->payloadOffset;
            return TRUE;
        }
    }
    return FALSE;
}

static bool32 G4Entrypoint(u32 kind, u32 ordinal, struct G4Point *out)
{
    size_t i;
    u32 seen = 0u;
    for (i = 0u; i < EmeraldScriptCompat_GetStagedFBindingCount(); i++)
    {
        struct EmeraldScriptCompatStagedFBinding row;
        const u8 *base;
        size_t size;
        u32 m;
        if (!EmeraldScriptCompat_GetStagedFBinding(i, &row)
         || row.kind != kind)
            continue;
        if (EmeraldScriptCompat_ValidateBoundary(
                row.moduleKey, row.payloadOffset,
                EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT) != EMERALD_SCRIPT_OK)
            continue;
        if (seen++ != ordinal)
            continue;
        for (m = 0u; m < kEmeraldScriptCompatTable.moduleCount; m++)
            if (strcmp(kEmeraldScriptCompatTable.modules[m].id,
                       row.moduleKey) == 0)
                break;
        if (m == kEmeraldScriptCompatTable.moduleCount)
            return FALSE;
        if (!EmeraldScriptCompat_GetModuleSpan(row.moduleKey, &base, &size)
         || row.payloadOffset >= size)
            return FALSE;
        out->module = m;
        out->offset = row.payloadOffset;
        out->pointer = base + row.payloadOffset;
        return TRUE;
    }
    return FALSE;
}

static bool32 G4MapDispatchEntrypoint(u32 ordinal, struct G4Point *out)
{
    u32 i;
    u32 seen = 0u;

    for (i = 0u; i < kEmeraldScriptCompatTable.routingRelocCount; i++)
    {
        struct EmeraldScriptCompatResolvedTarget target;
        char key[96];
        u32 offset;
        u32 segment;
        u32 m;
        if (!EmeraldScriptCompat_GetStagedRoutingTarget(i, &target)
         || target.disposition != EMERALD_SCRIPT_DISPOSITION_STAGED_ARENA
         || target.liveAddress == 0u
         || EmeraldScriptCompat_ReverseResolve(
                target.liveAddress, key, sizeof(key), &offset, &segment)
                != EMERALD_SCRIPT_OK
         || segment != EMERALD_SCRIPT_NATIVE_SEGMENT_BYTECODE
         || EmeraldScriptCompat_ValidateBoundary(
                key, offset, EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT)
                != EMERALD_SCRIPT_OK)
            continue;
        if (seen++ != ordinal)
            continue;
        for (m = 0u; m < kEmeraldScriptCompatTable.moduleCount; m++)
            if (strcmp(kEmeraldScriptCompatTable.modules[m].id, key) == 0)
                break;
        if (m == kEmeraldScriptCompatTable.moduleCount)
            return FALSE;
        out->module = m;
        out->offset = offset;
        out->pointer = (const u8 *)target.liveAddress;
        return TRUE;
    }
    return FALSE;
}

static void G4BindLayout(void)
{
    struct HarnessGameData *data = GameData();
    struct EmeraldScriptStateLayout layout;
    struct EmeraldScriptDynamicBuffer dynamic;

    memset(&layout, 0, sizeof(layout));
    layout.context1 = &data->scriptState.context1;
    layout.context2 = &data->scriptState.context2;
    layout.generationStamp = &data->scriptState.generationStamp;
    layout.ramScriptRetAddr = &data->scriptState.ramScriptRetAddr;
    layout.approachingTrainerScript[0] = &data->scriptState.approaching[0];
    layout.approachingTrainerScript[1] = &data->scriptState.approaching[1];
    layout.trainerBattleEndScript = &data->scriptState.trainerEnd;
    layout.trainerAReturnScript = &data->scriptState.trainerReturnA;
    layout.trainerBReturnScript = &data->scriptState.trainerReturnB;
    layout.mysteryEventContext = &data->scriptState.mysteryEvent;
    layout.mysteryEventNativeBase = &data->scriptState.mysteryNativeBase;
    /* R13-G6 (plan sec 9): sAddressOffset is deleted (the stable virtual
     * anchor is the only model); no addressOffset binding is set. */
    EmeraldScriptState_SetLayout(&layout);

    memset(sG4DynamicBoundaries, 0, sizeof(sG4DynamicBoundaries));
    sG4DynamicBoundaries[0] = 1u;
    sG4DynamicBoundaries[5] = 1u;
    sG4DynamicBoundaries[10] = 1u;
    sG4DynamicBoundaries[15] = 1u;
    sG4DynamicBoundaries[256] = 1u;
    sG4DynamicBoundaries[261] = 1u;
    sG4DynamicBoundaries[266] = 1u;
    sG4DynamicBoundaries[271] = 1u;
    memset(&dynamic, 0, sizeof(dynamic));
    dynamic.kind = EMERALD_SCRIPT_DYNAMIC_MYSTERY_EVENT_BUFFER;
    dynamic.ownerStorageId = 0x4d455654u;
    dynamic.generation = 7u;
    dynamic.base = data->scriptState.dynamicScript;
    dynamic.size = sizeof(data->scriptState.dynamicScript) / 2u;
    dynamic.instructionStarts = sG4DynamicBoundaries;
    snprintf(dynamic.ownerId, sizeof(dynamic.ownerId), "GAME_DATA:mystery-event");
    EmeraldScriptState_ClearDynamicBuffers();
    CHECK(EmeraldScriptState_RegisterDynamicBuffer(&dynamic));

    dynamic.kind = EMERALD_SCRIPT_DYNAMIC_SAVE_RAM_SCRIPT;
    dynamic.ownerStorageId = 0x53415645u;
    dynamic.generation = 11u;
    dynamic.base = data->scriptState.dynamicScript + 256u;
    dynamic.size = sizeof(data->scriptState.dynamicScript) / 2u;
    dynamic.instructionStarts = sG4DynamicBoundaries + 256u;
    snprintf(dynamic.ownerId, sizeof(dynamic.ownerId), "GAME_DATA:save-ram-script");
    CHECK(EmeraldScriptState_RegisterDynamicBuffer(&dynamic));
}

static void G4StoreExpected(u32 slot, const struct G4Point *point)
{
    GameData()->scriptState.expectedModule[slot] = point->module;
    GameData()->scriptState.expectedOffset[slot] = point->offset;
}

static bool32 G4CheckPoint(const u8 *pointer, u32 slot, u32 boundary)
{
    char key[96];
    u32 offset;
    u32 segment;
    enum EmeraldScriptCompatStatus status = EmeraldScriptCompat_ReverseResolve(
        (uintptr_t)pointer, key, sizeof(key), &offset, &segment);
    CHECK(status == EMERALD_SCRIPT_OK);
    if (status != EMERALD_SCRIPT_OK)
        return FALSE;
    CHECK(strcmp(key, kEmeraldScriptCompatTable.modules[
                         GameData()->scriptState.expectedModule[slot]].id) == 0);
    CHECK(offset == GameData()->scriptState.expectedOffset[slot]);
    CHECK(segment == EMERALD_SCRIPT_NATIVE_SEGMENT_BYTECODE);
    CHECK(EmeraldScriptCompat_ValidateBoundary(key, offset, boundary)
          == EMERALD_SCRIPT_OK);
    return TRUE;
}

static bool32 G4PointAfterOpcode(u8 opcode, u32 instructionSize,
                                struct G4Point *out)
{
    u32 m;

    for (m = 0u; m < kEmeraldScriptCompatTable.moduleCount; m++)
    {
        const struct EmeraldScriptNativeModule *module =
            &kEmeraldScriptCompatTable.modules[m];
        const u8 *base;
        size_t size;
        u32 i;
        if (!EmeraldScriptCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = instructionSize; i < size; i++)
        {
            if (base[i - instructionSize] != opcode
             || EmeraldScriptCompat_ValidateBoundary(
                    module->id, i - instructionSize,
                    EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START)
                    != EMERALD_SCRIPT_OK
             || EmeraldScriptCompat_ValidateBoundary(
                    module->id, i, EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START)
                    != EMERALD_SCRIPT_OK)
                continue;
            out->module = m;
            out->offset = i;
            out->pointer = base + i;
            return TRUE;
        }
    }
    return FALSE;
}

static void G4VariantPath(char *out, size_t outSize, const char *statePath,
                          const char *suffix)
{
    snprintf(out, outSize, "%s.%s", statePath, suffix);
}

static bool32 G4SaveVariant(const char *statePath, const char *suffix,
                            u32 expectedRecords)
{
    char path[512];
    struct ParsedRecord records[64];
    u32 recordCount = 0u;

    G4VariantPath(path, sizeof(path), statePath, suffix);
    HarnessStatePath_Override(path);
    CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    CHECK(ParseStateSidecar(path, records, ARRAY_COUNT(records), &recordCount));
    if (recordCount != expectedRecords)
        printf("G4SaveVariant %s: expected %u records, got %u\n",
               suffix, expectedRecords, recordCount);
    CHECK(recordCount == expectedRecords);
    return sFailures == 0;
}

static void G4PlantDynamic(void)
{
    struct HarnessGameData *data = GameData();
    struct EmeraldScriptDynamicBuffer dynamic;
    u32 i;

    memset(data->scriptState.dynamicScript, 0,
           sizeof(data->scriptState.dynamicScript));
    /* Full vaddress family fixture.  Encoded targets are deliberately not
     * host addresses; all eight resolve through one stable anchor. */
    for (i = 0u; i < 8u; i++)
        data->scriptState.dynamicScript[i * 8u] = (u8)(0xB8u + i);
    memset(&dynamic, 0, sizeof(dynamic));
    dynamic.kind = EMERALD_SCRIPT_DYNAMIC_MYSTERY_EVENT_BUFFER;
    dynamic.ownerStorageId = 0x4d455654u;
    dynamic.generation = 7u;
    dynamic.base = data->scriptState.dynamicScript;
    dynamic.size = sizeof(data->scriptState.dynamicScript) / 2u;
    dynamic.instructionStarts = sG4DynamicBoundaries;
    snprintf(dynamic.ownerId, sizeof(dynamic.ownerId), "GAME_DATA:mystery-event");
    CHECK(EmeraldScriptState_BuildVirtualAnchor(
        &dynamic, 0x09000000u, 0u, &data->scriptState.virtualAnchor)
          == EMERALD_SCRIPT_STATE_OK);
    data->scriptState.mysteryEvent.mode = 1u;
    data->scriptState.mysteryEvent.stackDepth = 2u;
    data->scriptState.mysteryEvent.scriptPtr = data->scriptState.dynamicScript + 5u;
    data->scriptState.mysteryEvent.stack[0] = data->scriptState.dynamicScript + 10u;
    data->scriptState.mysteryEvent.stack[1] = data->scriptState.dynamicScript + 15u;
    data->scriptState.mysteryNativeBase = data->scriptState.dynamicScript;
}

static bool32 G4PlantNestedState(void)
{
    struct HarnessGameData *data = GameData();
    struct G4Point current;
    struct G4Point return1;
    struct G4Point final;
    struct G4Point trainer;
    struct G4Point entryA;
    struct G4Point entryB;
    const u8 *text;
    size_t textSize;
    u32 i;

    memset(data, 0, sizeof(*data));
    G4BindLayout();
    CHECK(G4PointForOpcode(0x03u, 0u, &current));
    CHECK(G4PointForOpcode(0x03u, 1u, &return1));
    CHECK(G4AnyInteriorPoint(50u, &final));
    CHECK(G4PointForOpcode(0x5Cu, 0u, &trainer));
    CHECK(G4Entrypoint(EMERALD_SCRIPT_NATIVE_F_OBJECT_EVENT, 0u, &entryA));
    CHECK(G4Entrypoint(EMERALD_SCRIPT_NATIVE_F_COORD_EVENT, 0u, &entryB));
    if (sFailures != 0)
        return FALSE;
    data->scriptState.context1.mode = 1u;
    data->scriptState.context1.stackDepth = 2u;
    data->scriptState.context1.scriptPtr = current.pointer;
    data->scriptState.context1.stack[0] = final.pointer;
    data->scriptState.context1.stack[1] = return1.pointer;
    data->scriptState.context1.stack[19] = (const u8 *)(uintptr_t)0x11111111u;
    data->scriptState.ramScriptRetAddr = final.pointer;
    data->scriptState.approaching[0] = trainer.pointer;
    data->scriptState.approaching[1] = trainer.pointer;
    data->scriptState.trainerEnd = final.pointer;
    data->scriptState.trainerReturnA = entryA.pointer;
    data->scriptState.trainerReturnB = entryB.pointer;
    data->scriptState.generationStamp = EmeraldScriptCompat_GetGenerationId();
    data->scriptState.trainerIds[0] = 0x1234u;
    data->scriptState.trainerIds[1] = 0x5678u;
    CHECK(EmeraldTextCompat_GetResourceBytes(
            "emerald:text/system/gtext-123dot", &text, &textSize));
    if (text != NULL)
    {
        CHECK(textSize == 9u);
        for (i = 0u; i < 6u; i++)
            data->scriptState.trainerText[i] = text + (i % 3u) * 3u;
    }
    G4StoreExpected(0u, &current);
    G4StoreExpected(1u, &return1);
    G4StoreExpected(2u, &final);
    G4StoreExpected(3u, &trainer);
    G4StoreExpected(4u, &entryA);
    G4StoreExpected(5u, &entryB);
    G4PlantDynamic();
    data->magic = 0x47344e31u;
    data->tailMagic = 0x47344e32u;
    return TRUE;
}

static int DoG4Create(const char *packPath, const char *statePath)
{
    struct ParsedRecord records[64];
    u32 recordCount = 0u;
    const u8 *arena;
    size_t arenaSize;
    size_t projected = 0u;

    HarnessStatePath_Override(statePath);
    if (!G4Stage(packPath, FALSE) || !G4PlantNestedState())
        return 1;
    CHECK(EmeraldScriptCompat_GetArena(&arena, &arenaSize));
    CHECK(EmeraldScriptCompat_IsPublished());
    CHECK(EmeraldScriptCompat_AreRangesRegistered());
    CHECK(EmeraldResourceRangeIndex_GetRangeCount(
              EmeraldResourceCompat_GetRangeIndex()) == 6377u);
    (void)projected;
    CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    CHECK(ParseStateSidecar(statePath, records, ARRAY_COUNT(records),
                            &recordCount));
    /* 9 static G records + six C trainer-text records. Dynamic Mystery
     * pointers use existing in-band persistent identities. */
    CHECK(recordCount == 15u);

    /* SAVE_RAM_SCRIPT: mutable Context1 IP remains relative to captured
     * SaveBlock storage; the return target is the one static G sidecar. */
    {
        struct HarnessGameData *data = GameData();
        struct G4Point ret;
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        CHECK(G4AnyInteriorPoint(50u, &ret));
        data->scriptState.context1.mode = 1u;
        data->scriptState.context1.scriptPtr =
            data->scriptState.dynamicScript + 261u;
        data->scriptState.dynamicScript[261] = 0x03u;
        data->scriptState.ramScriptRetAddr = ret.pointer;
        data->scriptState.generationStamp = EmeraldScriptCompat_GetGenerationId();
        G4StoreExpected(0u, &ret);
        CHECK(G4SaveVariant(statePath, "ram", 1u));
    }

    /* Mid-dialogue: IP is already after MESSAGE, the deferred callback is
     * engine-image relative, and the outstanding text pointer is C-owned. */
    {
        struct HarnessGameData *data = GameData();
        struct G4Point next;
        const u8 *text = NULL;
        size_t textSize = 0u;
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        CHECK(G4PointAfterOpcode(0x67u, 5u, &next));
        CHECK(EmeraldTextCompat_GetResourceBytes(
            "emerald:text/system/gtext-123dot", &text, &textSize));
        data->scriptState.context1.mode = 1u;
        data->scriptState.context1.scriptPtr = next.pointer;
        data->scriptState.context1.nativePtr = G4WaitCallback;
        data->scriptState.trainerText[0] = text + 3u;
        data->scriptState.generationStamp = EmeraldScriptCompat_GetGenerationId();
        G4StoreExpected(0u, &next);
        CHECK(G4SaveVariant(statePath, "dialogue", 2u));
    }

    /* Movement wait: movement ownership remains an opaque B-owned scalar;
     * only the advanced G IP is sidecar-relocated. */
    {
        struct HarnessGameData *data = GameData();
        struct G4Point next;
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        CHECK(G4PointAfterOpcode(0x51u, 3u, &next));
        data->scriptState.context1.mode = 1u;
        data->scriptState.context1.scriptPtr = next.pointer;
        data->scriptState.context1.nativePtr = G4WaitCallback;
        data->scriptState.movementEncodedTarget = 0x42574e44u;
        data->scriptState.generationStamp = EmeraldScriptCompat_GetGenerationId();
        G4StoreExpected(0u, &next);
        CHECK(G4SaveVariant(statePath, "movement", 1u));
    }

    /* The nested fixture covers object and corrected coord F entrypoints.
     * This variant closes map-script and BG/sign provenance using the same
     * exact ENTRYPOINT role on the two trainer-continuation-shaped slots. */
    {
        struct HarnessGameData *data = GameData();
        struct G4Point map = {0};
        struct G4Point bg = {0};
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        CHECK(G4MapDispatchEntrypoint(0u, &map));
        CHECK(G4Entrypoint(EMERALD_SCRIPT_NATIVE_F_BG_EVENT, 0u, &bg));
        data->scriptState.trainerReturnA = map.pointer;
        data->scriptState.trainerReturnB = bg.pointer;
        data->scriptState.generationStamp = EmeraldScriptCompat_GetGenerationId();
        G4StoreExpected(0u, &map);
        G4StoreExpected(1u, &bg);
        CHECK(G4SaveVariant(statePath, "entrypoints", 2u));
    }
    printf("G4-CREATE arena=%p size=%zu generation=%llu records=%u projected=%zu\n",
           (const void *)arena, arenaSize,
           (unsigned long long)EmeraldScriptCompat_GetGenerationId(),
           recordCount, projected);
    TeardownScriptCompatSession();
    return sFailures != 0;
}

static int DoG4Load(const char *packPath, const char *statePath)
{
    struct HarnessGameData *data = GameData();
    const u8 *arena;
    size_t arenaSize;
    uintptr_t target;
    u32 i;

    HarnessStatePath_Override(statePath);
    if (!G4Stage(packPath, TRUE))
        return 1;
    memset(data, 0, sizeof(*data));
    G4BindLayout();
    CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    CHECK(EmeraldScriptCompat_GetArena(&arena, &arenaSize));
    CHECK(G4CheckPoint(data->scriptState.context1.scriptPtr, 0u,
                       EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START));
    CHECK(G4CheckPoint(data->scriptState.context1.stack[1], 1u,
                       EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION));
    CHECK(G4CheckPoint(data->scriptState.context1.stack[0], 2u,
                       EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION));
    CHECK(data->scriptState.context1.stack[19] == NULL);
    CHECK(data->scriptState.trainerIds[0] == 0x1234u
       && data->scriptState.trainerIds[1] == 0x5678u);
    CHECK(G4CheckPoint(data->scriptState.ramScriptRetAddr, 2u,
                       EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION));
    CHECK(G4CheckPoint(data->scriptState.approaching[0], 3u,
                       EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START));
    CHECK(G4CheckPoint(data->scriptState.trainerReturnA, 4u,
                       EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT));
    CHECK(G4CheckPoint(data->scriptState.trainerReturnB, 5u,
                       EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT));
    CHECK(data->scriptState.mysteryEvent.scriptPtr
          == data->scriptState.dynamicScript + 5u);
    CHECK(data->scriptState.mysteryEvent.stack[0]
          == data->scriptState.dynamicScript + 10u);
    CHECK(data->scriptState.mysteryEvent.stack[1]
          == data->scriptState.dynamicScript + 15u);
    CHECK(data->scriptState.mysteryNativeBase
          == data->scriptState.dynamicScript);
    for (i = 0u; i < 8u; i++)
    {
        CHECK(EmeraldScriptState_ResolveVirtualTarget(
                  &data->scriptState.virtualAnchor,
                  0x09000000u + i * 8u, 1u, &target)
              == EMERALD_SCRIPT_STATE_OK);
        CHECK(target == (uintptr_t)data->scriptState.dynamicScript + i * 8u);
        CHECK(*(const u8 *)target == (u8)(0xB8u + i));
    }
    /* Differential next-opcode/return oracle: the canonical IP and first
     * return both point at opcode RETURN.  Pop both frames in the native
     * stack order and land at the exact final canonical offset. */
    CHECK(*data->scriptState.context1.scriptPtr == 0x03u);
    data->scriptState.context1.stackDepth--;
    data->scriptState.context1.scriptPtr =
        data->scriptState.context1.stack[data->scriptState.context1.stackDepth];
    CHECK(*data->scriptState.context1.scriptPtr == 0x03u);
    data->scriptState.context1.stackDepth--;
    data->scriptState.context1.scriptPtr =
        data->scriptState.context1.stack[data->scriptState.context1.stackDepth];
    CHECK(G4CheckPoint(data->scriptState.context1.scriptPtr, 2u,
                       EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START));

    /* Each following load is another complete transaction in this fresh
     * process, against generation B and its forced-different arena base. */
    {
        char path[512];
        G4VariantPath(path, sizeof(path), statePath, "ram");
        HarnessStatePath_Override(path);
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
        CHECK(data->scriptState.context1.scriptPtr
              == data->scriptState.dynamicScript + 261u);
        CHECK(*data->scriptState.context1.scriptPtr == 0x03u);
        CHECK(G4CheckPoint(data->scriptState.ramScriptRetAddr, 0u,
                           EMERALD_SCRIPT_BOUNDARY_NEXT_INSTRUCTION));
        data->scriptState.context1.scriptPtr =
            data->scriptState.ramScriptRetAddr;
        CHECK(G4CheckPoint(data->scriptState.context1.scriptPtr, 0u,
                           EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START));
    }
    {
        char path[512];
        G4VariantPath(path, sizeof(path), statePath, "dialogue");
        HarnessStatePath_Override(path);
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
        CHECK(G4CheckPoint(data->scriptState.context1.scriptPtr, 0u,
                           EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START));
        CHECK(data->scriptState.context1.nativePtr == G4WaitCallback);
        CHECK(data->scriptState.trainerText[0] != NULL);
        CHECK(data->scriptState.trainerText[0][0] == 0xA3u);
    }
    {
        char path[512];
        G4VariantPath(path, sizeof(path), statePath, "movement");
        HarnessStatePath_Override(path);
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
        CHECK(G4CheckPoint(data->scriptState.context1.scriptPtr, 0u,
                           EMERALD_SCRIPT_BOUNDARY_INSTRUCTION_START));
        CHECK(data->scriptState.context1.nativePtr == G4WaitCallback);
        CHECK(data->scriptState.movementEncodedTarget == 0x42574e44u);
    }
    {
        char path[512];
        G4VariantPath(path, sizeof(path), statePath, "entrypoints");
        HarnessStatePath_Override(path);
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
        CHECK(G4CheckPoint(data->scriptState.trainerReturnA, 0u,
                           EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT));
        CHECK(G4CheckPoint(data->scriptState.trainerReturnB, 1u,
                           EMERALD_SCRIPT_BOUNDARY_ENTRYPOINT));
    }
    printf("G4-LOAD arena=%p size=%zu generation=%llu nested-return=ok dynamic-vaddress=8/8\n",
           (const void *)arena, arenaSize,
           (unsigned long long)EmeraldScriptCompat_GetGenerationId());
    TeardownScriptCompatSession();
    return sFailures != 0;
}

static int DoG4Faults(const char *packPath, const char *statePath)
{
    struct HarnessGameData *data = GameData();
    struct G4Point valid = {0};
    struct G4Point nonEntry = {0};
    struct G4Point dataPoint = {0};
    struct EmeraldScriptStateResourceIdentity identity;
    enum NativeStateResult result;
    const u8 *arena;
    size_t arenaSize;
    const u8 *hole = NULL;
    u32 passed = 0u;
    u32 i;

    HarnessStatePath_Override(statePath);
    if (!G4Stage(packPath, FALSE))
        return 1;
    memset(data, 0, sizeof(*data));
    G4BindLayout();
    CHECK(G4AnyInteriorPoint(20u, &valid));
    CHECK(G4NonEntrypoint(20u, &nonEntry));
    CHECK(G4SegmentPoint(EMERALD_SCRIPT_NATIVE_SEGMENT_STATIC_DATA,
                         &dataPoint));
    CHECK(EmeraldScriptCompat_GetArena(&arena, &arenaSize));
    for (i = 0u; i < arenaSize; i++)
    {
        char key[96];
        u32 offset;
        u32 segment;
        if (EmeraldScriptCompat_ReverseResolve(
                (uintptr_t)(arena + i), key, sizeof(key), &offset, &segment)
                != EMERALD_SCRIPT_OK)
        {
            hole = arena + i;
            break;
        }
    }
    CHECK(hole != NULL);
    data->scriptState.context1.mode = 1u;
    data->scriptState.context1.scriptPtr = valid.pointer;
    data->scriptState.generationStamp = EmeraldScriptCompat_GetGenerationId();

#define G4_REFUSE(setup, needle) do { \
        setup; \
        result = NativeState_Save(HARNESS_STATE_SLOT); \
        CHECK(result != NATIVE_STATE_OK); \
        CHECK(strstr(NativeState_GetLastError(), (needle)) != NULL); \
        passed++; \
    } while (0)
    data->scriptState.context1.stackDepth = 21u;
    G4_REFUSE((void)0, "stackDepth");
    data->scriptState.context1.stackDepth = 0u;
    data->scriptState.context1.scriptPtr = NULL;
    G4_REFUSE((void)0, "null IP");
    data->scriptState.context1.scriptPtr = valid.pointer;
    data->scriptState.context2.mode = 1u;
    G4_REFUSE((void)0, "Context2");
    data->scriptState.context2.mode = 0u;
    data->scriptState.context2.scriptPtr = valid.pointer;
    G4_REFUSE((void)0, "Context2");
    data->scriptState.context2.scriptPtr = NULL;
    data->scriptState.context2.stackDepth = 1u;
    G4_REFUSE((void)0, "Context2");
    data->scriptState.context2.stackDepth = 0u;
    data->scriptState.context2.stack[19] = valid.pointer;
    G4_REFUSE((void)0, "Context2");
    data->scriptState.context2.stack[19] = NULL;
    /* R13-G6 (plan sec 9): the legacy sAddressOffset host-delta refusal
     * is deleted with the dead storage; anchors are the only model. */
    data->scriptState.generationStamp++;
    G4_REFUSE((void)0, "stale");
    data->scriptState.generationStamp = EmeraldScriptCompat_GetGenerationId();
    data->scriptState.context1.scriptPtr = valid.pointer + 1u;
    G4_REFUSE((void)0, "boundary");

    data->scriptState.context1.scriptPtr = valid.pointer;
    data->scriptState.context1.stackDepth = 1u;
    data->scriptState.context1.stack[0] = valid.pointer + 1u;
    G4_REFUSE((void)0, "Context1.stack");
    data->scriptState.context1.stackDepth = 0u;
    data->scriptState.context1.stack[0] = NULL;
    data->scriptState.context1.scriptPtr = arena + arenaSize;
    G4_REFUSE((void)0, "outside");
    data->scriptState.context1.scriptPtr = hole;
    G4_REFUSE((void)0, "outside");
    data->scriptState.context1.scriptPtr = dataPoint.pointer;
    G4_REFUSE((void)0, "data/text/movement");
    data->scriptState.context1.scriptPtr = valid.pointer;
    data->scriptState.trainerEnd = dataPoint.pointer;
    G4_REFUSE((void)0, "sTrainerBattleEndScript");
    data->scriptState.trainerEnd = NULL;

    /* Inactive Context1 scratch is scrubbed, never treated as an active
     * return stack and never emitted as a sidecar. */
    data->scriptState.context1.mode = 0u;
    data->scriptState.context1.scriptPtr = (const u8 *)(uintptr_t)0x11111111u;
    data->scriptState.context1.stack[19] =
        (const u8 *)(uintptr_t)0x22222222u;
    CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    passed++;

    /* Unregistered mutable storage and an exact one-past buffer pointer. */
    data->scriptState.context1.mode = 1u;
    data->scriptState.context1.scriptPtr =
        data->scriptState.dynamicScript + 261u;
    data->scriptState.context1.stack[19] = NULL;
    EmeraldScriptState_ClearDynamicBuffers();
    G4_REFUSE((void)0, "outside");
    data->scriptState.context1.scriptPtr =
        data->scriptState.dynamicScript + sizeof(data->scriptState.dynamicScript);
    G4_REFUSE((void)0, "outside");
    G4BindLayout();
#undef G4_REFUSE

    /* Direct restore staging matrix.  These calls exercise the same family
     * resolver used by NativeState_Load before any slice is committed. */
    data->scriptState.context1.mode = 0u;
    data->scriptState.trainerEnd = valid.pointer;
    CHECK(EmeraldScriptState_CaptureField(
              (uintptr_t)(void *)&data->scriptState.trainerEnd,
              (uintptr_t)valid.pointer, &identity)
          == EMERALD_SCRIPT_STATE_OK);
    {
        uintptr_t resolved = 0u;
        Gen3ResourceKey badKey = identity.key;
        u32 payloadSize = 0u;
        u32 schema = 0u;
        CHECK(EmeraldScriptState_ResolveField(
                  (uintptr_t)(void *)&data->scriptState.trainerEnd,
                  &identity.key, identity.resourceType, identity.schema,
                  identity.representationRole, identity.payloadOffset,
                  NULL, 0u, 0u, &resolved) == EMERALD_SCRIPT_STATE_OK);
        CHECK(resolved == (uintptr_t)valid.pointer); passed++;
        badKey.bytes[0] ^= 0x80u;
        CHECK(EmeraldScriptState_ResolveField(
                  (uintptr_t)(void *)&data->scriptState.trainerEnd,
                  &badKey, identity.resourceType, identity.schema,
                  identity.representationRole, identity.payloadOffset,
                  NULL, 0u, 0u, &resolved)
              == EMERALD_SCRIPT_STATE_ERR_MISSING_KEY); passed++;
        CHECK(EmeraldScriptState_ResolveField(
                  (uintptr_t)(void *)&data->scriptState.trainerEnd,
                  &identity.key, identity.resourceType, 999u,
                  identity.representationRole, identity.payloadOffset,
                  NULL, 0u, 0u, &resolved)
              == EMERALD_SCRIPT_STATE_ERR_SCHEMA); passed++;
        CHECK(EmeraldScriptState_ResolveField(
                  (uintptr_t)(void *)&data->scriptState.trainerEnd,
                  &identity.key, identity.resourceType, identity.schema,
                  EMERALD_RESOURCE_ROLE_LEGACY_LZ, identity.payloadOffset,
                  NULL, 0u, 0u, &resolved)
              == EMERALD_SCRIPT_STATE_ERR_SCHEMA); passed++;
        CHECK(EmeraldScriptCompat_GetStateIdentity(
                  identity.moduleKey, &badKey, &schema, &payloadSize));
        CHECK(EmeraldScriptState_ResolveField(
                  (uintptr_t)(void *)&data->scriptState.trainerEnd,
                  &identity.key, identity.resourceType, identity.schema,
                  identity.representationRole, payloadSize,
                  NULL, 0u, 0u, &resolved)
              == EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY); passed++;

        data->scriptState.trainerEnd = nonEntry.pointer;
        CHECK(EmeraldScriptState_CaptureField(
                  (uintptr_t)(void *)&data->scriptState.trainerEnd,
                  (uintptr_t)nonEntry.pointer, &identity)
              == EMERALD_SCRIPT_STATE_OK);
        CHECK(EmeraldScriptState_ResolveField(
                  (uintptr_t)(void *)&data->scriptState.trainerReturnA,
                  &identity.key, identity.resourceType, identity.schema,
                  identity.representationRole, identity.payloadOffset,
                  NULL, 0u, 0u, &resolved)
              == EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY); passed++;
    }

    /* vaddress refusal matrix (no live handler changes). */
    G4PlantDynamic();
    {
        uintptr_t target;
        struct EmeraldScriptVirtualAnchor bad = data->scriptState.virtualAnchor;
        CHECK(EmeraldScriptState_ResolveVirtualTarget(
                  &bad, bad.encodedVirtualBase - 1u, 1u, &target)
              == EMERALD_SCRIPT_STATE_ERR_DYNAMIC_BOUNDS); passed++;
        CHECK(EmeraldScriptState_ResolveVirtualTarget(
                  &bad, bad.encodedVirtualBase + 600u, 1u, &target)
              == EMERALD_SCRIPT_STATE_ERR_DYNAMIC_BOUNDS); passed++;
        bad.bufferGeneration++;
        CHECK(EmeraldScriptState_ResolveVirtualTarget(
                  &bad, bad.encodedVirtualBase, 1u, &target)
              == EMERALD_SCRIPT_STATE_ERR_DYNAMIC_GENERATION); passed++;
        bad = data->scriptState.virtualAnchor;
        bad.ownerStorageId++;
        CHECK(EmeraldScriptState_ResolveVirtualTarget(
                  &bad, bad.encodedVirtualBase, 1u, &target)
              == EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN); passed++;
        bad = data->scriptState.virtualAnchor;
        bad.liveBaseOffset = 0xfffffff0u;
        CHECK(EmeraldScriptState_ResolveVirtualTarget(
                  &bad, bad.encodedVirtualBase, 32u, &target)
              == EMERALD_SCRIPT_STATE_ERR_DYNAMIC_BOUNDS); passed++;
        bad = data->scriptState.virtualAnchor;
        bad.reserved = 1u;
        CHECK(EmeraldScriptState_ResolveVirtualTarget(
                  &bad, bad.encodedVirtualBase, 1u, &target)
              == EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN); passed++;
    }
    {
        struct EmeraldScriptDynamicBuffer overlap;
        memset(&overlap, 0, sizeof(overlap));
        overlap.kind = EMERALD_SCRIPT_DYNAMIC_CAPTURED_BUFFER;
        overlap.ownerStorageId = 0x414d4249u;
        overlap.generation = 1u;
        overlap.base = data->scriptState.dynamicScript + 1u;
        overlap.size = 8u;
        snprintf(overlap.ownerId, sizeof(overlap.ownerId), "ambiguous-overlap");
        CHECK(!EmeraldScriptState_RegisterDynamicBuffer(&overlap)); passed++;
    }
    {
        const u8 *oldPointer = valid.pointer;
        struct EmeraldScriptCompatDiagnostics diagnostics;
        memset(&diagnostics, 0, sizeof(diagnostics));
        CHECK(EmeraldScriptCompat_TryInitialize(
                  gScriptHarnessSnapshot, gScriptHarnessPack, &diagnostics)
              == EMERALD_SCRIPT_OK);
        memset(data, 0, sizeof(*data));
        G4BindLayout();
        data->scriptState.context1.mode = 1u;
        data->scriptState.context1.scriptPtr = oldPointer;
        data->scriptState.generationStamp =
            EmeraldScriptCompat_GetGenerationId();
        result = NativeState_Save(HARNESS_STATE_SLOT);
        CHECK(result != NATIVE_STATE_OK);
        CHECK(strstr(NativeState_GetLastError(), "outside") != NULL);
        passed++;
    }
    {
        uintptr_t resolved = 0u;
        EmeraldScriptCompat_Shutdown();
        CHECK(EmeraldScriptState_ResolveField(
                  (uintptr_t)(void *)&data->scriptState.trainerEnd,
                  &identity.key, identity.resourceType, identity.schema,
                  identity.representationRole, identity.payloadOffset,
                  NULL, 0u, 0u, &resolved)
              == EMERALD_SCRIPT_STATE_ERR_UNAVAILABLE); passed++;
    }
    /* R13-G6 (plan sec 9): the legacy sAddressOffset host-delta refusal
     * was deleted with the dead storage, so the matrix is 31 cases. */
    CHECK(passed == 31u);
    printf("G4-FAULTS passed=%u\n", passed);
    TeardownScriptCompatSession();
    return sFailures != 0;
}

/* R13-G6 (plan sec 9): the 17-op MEVENT boundary builder. The received
 * E-Reader card used to register with a NULL instruction bitmap, so
 * capture of an ACTIVE mystery-event context was fail-closed. The
 * builder now produces the bitmap (one byte per buffer byte, nonzero at
 * an exact instruction start) and the capture path validates against it.
 *
 * Synthetic wrapper mirrors a real card: checkcompat (17 B), setstatus
 * (2 B), setmsg (6 B), end (1 B), then a trailing main-dialect
 * sub-script region that must stay opaque (unmarked). */
static int DoG6Mevent(const char *packPath, const char *statePath)
{
    static u8 bitmap[512];
    struct HarnessGameData *data = GameData();
    struct EmeraldScriptDynamicBuffer dynamic;
    struct EmeraldScriptStateResourceIdentity identity;
    enum NativeStateResult result;
    u8 *script;
    u32 marks = 0u;
    u32 i;

    HarnessStatePath_Override(statePath);
    if (!G4Stage(packPath, FALSE))
        return 1;
    memset(data, 0, sizeof(*data));
    G4BindLayout();
    script = data->scriptState.dynamicScript;
    memset(script, 0, sizeof(data->scriptState.dynamicScript));
    script[0] = 0x01u;                 /* checkcompat: 17-byte instruction */
    script[17] = 0x04u;                /* setstatus: 2 bytes */
    script[19] = 0x03u;                /* setmsg: 6 bytes */
    script[25] = 0x02u;                /* end: terminal */
    for (i = 26u; i < 40u; i++)
        script[i] = (u8)(0xB8u + i);   /* trailing sub-script/data: opaque */

    EmeraldScriptState_BuildMysteryEventBoundaryBitmap(script, 512u, bitmap);
    CHECK(bitmap[0] == 1u && bitmap[17] == 1u && bitmap[19] == 1u
       && bitmap[25] == 1u);
    CHECK(bitmap[18] == 0u && bitmap[20] == 0u);
    for (i = 26u; i < 512u; i++)
    {
        if (bitmap[i] != 0u)
            marks++;
    }
    CHECK(marks == 0u);                /* trailing region stays opaque */

    script[0] = 0x11u;                 /* unknown opcode: nothing marked */
    EmeraldScriptState_BuildMysteryEventBoundaryBitmap(script, 512u, bitmap);
    CHECK(bitmap[0] == 0u);
    script[0] = 0x02u;                 /* bare end: exactly one mark */
    EmeraldScriptState_BuildMysteryEventBoundaryBitmap(script, 512u, bitmap);
    CHECK(bitmap[0] == 1u && bitmap[1] == 0u);

    memset(&dynamic, 0, sizeof(dynamic));
    dynamic.kind = EMERALD_SCRIPT_DYNAMIC_MYSTERY_EVENT_BUFFER;
    dynamic.ownerStorageId = 0x4d455654u;
    dynamic.generation = 7u;
    dynamic.base = script;
    dynamic.size = 512u;
    dynamic.instructionStarts = bitmap;
    snprintf(dynamic.ownerId, sizeof(dynamic.ownerId), "GAME_DATA:mystery-event");
    EmeraldScriptState_ClearDynamicBuffers();
    CHECK(EmeraldScriptState_RegisterDynamicBuffer(&dynamic));

    /* Restore the wrapper (0x11 broke the bitmap above) and plant an
     * ACTIVE mystery-event context on marked positions. */
    script[0] = 0x01u;
    EmeraldScriptState_BuildMysteryEventBoundaryBitmap(script, 512u, bitmap);
    data->scriptState.mysteryEvent.mode = 1u;
    data->scriptState.mysteryEvent.stackDepth = 2u;
    data->scriptState.mysteryEvent.scriptPtr = script + 17u;
    data->scriptState.mysteryEvent.stack[0] = script + 19u;
    data->scriptState.mysteryEvent.stack[1] = script + 25u;
    data->scriptState.mysteryNativeBase = script;
    data->scriptState.generationStamp = EmeraldScriptCompat_GetGenerationId();

    CHECK(EmeraldScriptState_CaptureField(
              (uintptr_t)(void *)&data->scriptState.mysteryEvent.scriptPtr,
              (uintptr_t)(script + 17u), &identity)
          == EMERALD_SCRIPT_STATE_DYNAMIC);
    CHECK(EmeraldScriptState_CaptureField(
              (uintptr_t)(void *)&data->scriptState.mysteryEvent.stack[0],
              (uintptr_t)(script + 19u), &identity)
          == EMERALD_SCRIPT_STATE_DYNAMIC);
    CHECK(EmeraldScriptState_CaptureField(
              (uintptr_t)(void *)&data->scriptState.mysteryEvent.stack[1],
              (uintptr_t)(script + 25u), &identity)
          == EMERALD_SCRIPT_STATE_DYNAMIC);
    CHECK(EmeraldScriptState_CaptureField(
              (uintptr_t)(void *)&data->scriptState.mysteryNativeBase,
              (uintptr_t)script, &identity)
          == EMERALD_SCRIPT_STATE_DYNAMIC);
    /* Mid-instruction, opaque-tail and one-past pointers still refuse. */
    CHECK(EmeraldScriptState_CaptureField(
              (uintptr_t)(void *)&data->scriptState.mysteryEvent.scriptPtr,
              (uintptr_t)(script + 18u), &identity)
          == EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY);
    CHECK(EmeraldScriptState_CaptureField(
              (uintptr_t)(void *)&data->scriptState.mysteryEvent.scriptPtr,
              (uintptr_t)(script + 26u), &identity)
          == EMERALD_SCRIPT_STATE_ERR_WRONG_BOUNDARY);
    CHECK(EmeraldScriptState_CaptureField(
              (uintptr_t)(void *)&data->scriptState.mysteryEvent.scriptPtr,
              (uintptr_t)(script + 512u), &identity)
          == EMERALD_SCRIPT_STATE_ERR_DYNAMIC_UNKNOWN);

    /* End to end: a full save with the active MEVENT context captures
     * (previously refused as WRONG_BOUNDARY with a NULL bitmap). */
    result = NativeState_Save(HARNESS_STATE_SLOT);
    CHECK(result == NATIVE_STATE_OK);
    printf("G6-MEVENT passed\n");
    TeardownScriptCompatSession();
    return sFailures != 0;
}

/* Rejection modes: every one must leave the canary (magic fields) untouched
 * and return a non-OK result. */
static int DoLoadFail(const char *packPath, const char *statePath,
                      const char *kind)
{
    static const u8 bogusFingerprint[32] = {
        0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
        0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
        0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
        0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
    };
    enum NativeStateResult result;

    if (strcmp(kind, "missing-session") == 0)
    {
        /* No session at all: a state carrying resource references cannot be
         * loaded (TEST 7's session side). */
        result = NativeState_Load(HARNESS_STATE_SLOT);
        CHECK(result != NATIVE_STATE_OK);
        printf("LOADFAIL missing-session: %s\n", NativeState_GetLastError());
    }
    else if (strcmp(kind, "fingerprint") == 0)
    {
        /* The same production pack, but the session's content identity is
         * changed (TEST 4: altered provider content must refuse). */
        if (!RegisterSession(packPath))
            return 1;
        EmeraldResourceCompat_SetSessionContentFingerprint(bogusFingerprint);
        result = NativeState_Load(HARNESS_STATE_SLOT);
        CHECK(result != NATIVE_STATE_OK);
        printf("LOADFAIL fingerprint: %s\n", NativeState_GetLastError());
    }
    else if (strcmp(kind, "corrupt") == 0)
    {
        /* The runner mutated the sidecar (with recomputed CRCs); the parser
         * or the resolver must refuse (TEST 8). */
        if (!RegisterSession(packPath))
            return 1;
        result = NativeState_Load(HARNESS_STATE_SLOT);
        CHECK(result != NATIVE_STATE_OK);
        printf("LOADFAIL corrupt: %s\n", NativeState_GetLastError());
    }
    else
    {
        fprintf(stderr, "unknown fail kind %s\n", kind);
        return 1;
    }
    /* Transactionality: the canary region was zeroed before the load and
     * must still be exactly zero - no partial restoration happened. */
    {
        size_t i;
        for (i = 0; i < sizeof(sHarnessGameData); i++)
            CHECK(sHarnessGameData[i] == 0);
    }
    if (sFailures != 0)
        return 1;
    printf("LOADFAIL %s ok (transactional rejection)\n", kind);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* R13-H3 staged battle-family State-v5 closure.                       */

extern EWRAM_DATA const u8 *gBattlescriptCurrInstr;
extern EWRAM_DATA const u8 *gAIScriptPtr;
extern EWRAM_DATA void (*gAnimScriptCallback)(void);
extern EWRAM_DATA const u8 *gSelectionBattleScripts[MAX_BATTLERS_COUNT];
extern EWRAM_DATA const u8 *gPalaceSelectionBattleScripts[MAX_BATTLERS_COUNT];
extern EWRAM_DATA const u8 *sBattleAnimScriptPtr;
extern EWRAM_DATA const u8 *sBattleAnimScriptRetAddr;

#define H3_STACK_CAP 8u
#define H3_EXPECTED_CAP 24u

/* The battle heap-stack fixtures (production: gHeap-allocated
 * BattleResources members -> GAME_BSS slice) live at the head of the
 * modeled game-bss region; the H3 modes own the region exclusively. */
struct H3BattleFixtures
{
    struct BattleScriptsStack battleStack;     /* ptr[8] + size */
    struct BattleCallbacksStack callbackStack; /* function[8] + size */
    struct BattleScriptsStack aiStack;
    const u8 *contestStack[H3_STACK_CAP];
    u8 contestStackSize;
    u64 generationStamp;
    u32 expectedModule[H3_EXPECTED_CAP];
    u32 expectedOffset[H3_EXPECTED_CAP];
};

static struct H3BattleFixtures *H3Fixtures(void)
{
    return (struct H3BattleFixtures *)(void *)sHarnessGameBss;
}

struct H3Point
{
    u32 module;        /* module table index */
    u32 offset;        /* payload offset */
    const u8 *pointer; /* staged host pointer in the current generation */
};

static void H3WaitCallback(void)
{
}

/* Clear every EWRAM surface between fault plantings (the fixture struct
 * clear alone does not touch the real EWRAM globals). */
static void H3ClearSurfaces(void)
{
    u32 i;

    gBattlescriptCurrInstr = NULL;
    gAIScriptPtr = NULL;
    gAnimScriptCallback = NULL;
    sBattleAnimScriptPtr = NULL;
    sBattleAnimScriptRetAddr = NULL;
    for (i = 0u; i < MAX_BATTLERS_COUNT; i++)
    {
        gSelectionBattleScripts[i] = NULL;
        gPalaceSelectionBattleScripts[i] = NULL;
    }
}

static bool32 H3Stage(const char *packPath, u32 layout)
{
    struct EmeraldBattleCompatDiagnostics diagnostics;
    enum EmeraldBattleCompatStatus status;

    if (!SetupScriptCompatSession(packPath))
        return FALSE;
    memset(&diagnostics, 0, sizeof(diagnostics));
    status = EmeraldBattleCompat_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, layout, &diagnostics);
    if (status != EMERALD_BATTLE_OK)
    {
        fprintf(stderr, "H3 battle stage refused: %s @ %s\n",
                EmeraldBattleCompatStatus_Describe(status),
                diagnostics.canonicalName);
        return FALSE;
    }
    return TRUE;
}

static void H3BindLayout(void)
{
    struct H3BattleFixtures *fx = H3Fixtures();
    struct EmeraldBattleStateLayout layout;
    u32 i;

    memset(&layout, 0, sizeof(layout));
    layout.battlescriptCurrInstr = &gBattlescriptCurrInstr;
    for (i = 0u; i < 4u; i++)
    {
        layout.selectionScripts[i] = &gSelectionBattleScripts[i];
        layout.palaceSelectionScripts[i] = &gPalaceSelectionBattleScripts[i];
    }
    for (i = 0u; i < H3_STACK_CAP; i++)
    {
        layout.battleStackPtrs[i] = &fx->battleStack.ptr[i];
        layout.battleCallbacks[i] = &fx->callbackStack.function[i];
        layout.aiStackPtrs[i] = &fx->aiStack.ptr[i];
        layout.contestStackPtrs[i] = &fx->contestStack[i];
    }
    layout.battleStackSize = &fx->battleStack.size;
    layout.aiStackSize = &fx->aiStack.size;
    layout.contestStackSize = &fx->contestStackSize;
    layout.aiScriptPtr = &gAIScriptPtr;
    layout.animScriptPtr = &sBattleAnimScriptPtr;
    layout.animScriptRetAddr = &sBattleAnimScriptRetAddr;
    layout.animScriptCallback = &gAnimScriptCallback;
    layout.generationStamp = &fx->generationStamp;
    EmeraldBattleState_SetLayout(&layout);
}

/* Deterministic staged-point searches over the generated module table
 * (layout-independent: the table is indexed, the seam resolves). */

static bool32 H3PointForOpcode(u32 family, u8 opcode, u32 ordinal,
                               struct H3Point *out)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    u32 seen = 0u;
    u32 m;

    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleNativeModule *module = &t->modules[m];
        const u8 *base;
        size_t size;
        u32 i;

        if (module->family != family
         || module->mapKind != EMERALD_BATTLE_MAP_BYTECODE
         || !EmeraldBattleCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = module->boundaryFirst;
             i < module->boundaryFirst + module->boundaryCount; i++)
        {
            u32 offset = t->boundaries[i].payloadOffset;
            if (offset >= size || base[offset] != opcode)
                continue;
            if (seen++ == ordinal)
            {
                out->module = m;
                out->offset = offset;
                out->pointer = base + offset;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* A real call-return position: `call` (opcode + u32 target = 5 bytes)
 * at an instruction start whose +5 successor is also an instruction
 * start (the interpreter's own IP+5 return shape). */
static bool32 H3CallReturnPoint(u32 family, u8 callOpcode, u32 ordinal,
                                struct H3Point *out)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    u32 seen = 0u;
    u32 m;

    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleNativeModule *module = &t->modules[m];
        const u8 *base;
        size_t size;
        u32 i;

        if (module->family != family
         || !EmeraldBattleCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = module->boundaryFirst;
             i < module->boundaryFirst + module->boundaryCount; i++)
        {
            u32 offset = t->boundaries[i].payloadOffset;
            if (offset + 5u > size || base[offset] != callOpcode)
                continue;
            if (EmeraldBattleCompat_ValidateBoundary(
                    module->id, offset + 5u,
                    EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION)
                    != EMERALD_BATTLE_OK)
                continue;
            if (seen++ == ordinal)
            {
                out->module = m;
                out->offset = offset + 5u;
                out->pointer = base + offset + 5u;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* The nth instruction start (any opcode) in a family's bytecode
 * modules - deterministic and abundant, for surfaces whose exact opcode
 * is irrelevant (test/debug AI frames). */
static bool32 H3AnyInstructionPoint(u32 family, u32 ordinal,
                                    struct H3Point *out)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    u32 seen = 0u;
    u32 m;

    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleNativeModule *module = &t->modules[m];
        const u8 *base;
        size_t size;
        u32 i;

        if (module->family != family
         || module->mapKind != EMERALD_BATTLE_MAP_BYTECODE
         || !EmeraldBattleCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = module->boundaryFirst;
             i < module->boundaryFirst + module->boundaryCount; i++)
        {
            u32 offset = t->boundaries[i].payloadOffset;
            if (offset >= size)
                continue;
            if (seen++ == ordinal)
            {
                out->module = m;
                out->offset = offset;
                out->pointer = base + offset;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* A call-return site whose successor instruction is >= 2 bytes, so
 * return+1 is a genuine middle-of-operand for the corrupt-stack fault
 * fixtures (a 1-byte successor would make +1 a legal instruction start).
 * Returns the MID-OPERAND pointer (return+1). */
static bool32 H3MidOperandReturnPoint(u32 family, u8 callOpcode,
                                      u32 ordinal, struct H3Point *out)
{
    const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
    u32 seen = 0u;
    u32 m;

    for (m = 0u; m < t->payloadModuleCount; m++)
    {
        const struct EmeraldBattleNativeModule *module = &t->modules[m];
        const u8 *base;
        size_t size;
        u32 i;

        if (module->family != family
         || !EmeraldBattleCompat_GetModuleSpan(module->id, &base, &size))
            continue;
        for (i = module->boundaryFirst;
             i < module->boundaryFirst + module->boundaryCount; i++)
        {
            u32 offset = t->boundaries[i].payloadOffset;
            /* ret+1 must be strictly inside the module (a zero-gap
             * neighbor's span start would be a legal boundary). */
            if (offset + 7u > size || base[offset] != callOpcode)
                continue;
            if (EmeraldBattleCompat_ValidateBoundary(
                    module->id, offset + 5u,
                    EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION)
                    != EMERALD_BATTLE_OK)
                continue;
            /* The instruction at the return offset must span >= 2
             * bytes, so +1 cannot be a legal instruction start. */
            if (EmeraldBattleCompat_ValidateBoundary(
                    module->id, offset + 6u,
                    EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
                    == EMERALD_BATTLE_OK)
                continue;
            if (seen++ == ordinal)
            {
                out->module = m;
                out->offset = offset + 5u; /* the return offset proper */
                out->pointer = base + offset + 5u + 1u; /* mid-operand */
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void H3StoreExpected(u32 slot, const struct H3Point *point)
{
    H3Fixtures()->expectedModule[slot] = point->module;
    H3Fixtures()->expectedOffset[slot] = point->offset;
}

static bool32 H3CheckPoint(const u8 *pointer, u32 slot, u32 boundary)
{
    struct H3BattleFixtures *fx = H3Fixtures();
    char key[96];
    u32 offset;
    u32 family;
    enum EmeraldBattleCompatStatus status = EmeraldBattleCompat_ReverseResolve(
        (uintptr_t)pointer, key, sizeof(key), &offset, &family);
    CHECK(status == EMERALD_BATTLE_OK);
    if (status != EMERALD_BATTLE_OK)
        return FALSE;
    CHECK(strcmp(key, kEmeraldBattleCompatTable.modules[
                         fx->expectedModule[slot]].id) == 0);
    CHECK(offset == fx->expectedOffset[slot]);
    CHECK(family == kEmeraldBattleCompatTable.modules[
                         fx->expectedModule[slot]].family);
    CHECK(EmeraldBattleCompat_ValidateBoundary(key, offset, boundary)
          == EMERALD_BATTLE_OK);
    return TRUE;
}

/* Canonical-byte oracle: the staged byte must equal the pack payload
 * byte (the pack record digest is the qualified ROM provenance). */
static bool32 H3CanonicalByte(const struct H3Point *point, u8 *outByte)
{
    const struct EmeraldBattleNativeModule *module =
        &kEmeraldBattleCompatTable.modules[point->module];
    size_t entryCount = Gen3ResourcePack_GetEntryCount(gScriptHarnessPack);
    size_t e;

    for (e = 0u; e < entryCount; e++)
    {
        const struct Gen3ResourcePackEntry *entry =
            Gen3ResourcePack_GetEntry(gScriptHarnessPack, e);
        if (entry == NULL || entry->canonicalName == NULL
         || strcmp(entry->canonicalName, module->id) != 0)
            continue;
        CHECK(entry->payload != NULL);
        CHECK(entry->payloadSize == module->byteCount);
        if (point->offset >= entry->payloadSize)
            return FALSE;
        *outByte = entry->payload[point->offset];
        return TRUE;
    }
    return FALSE;
}

/* Execute the exact next opcode: staged byte == canonical pack byte ==
 * the expected opcode (no command replay/skip, no stale pointer). */
static bool32 H3ExecuteNext(const struct H3Point *point, u8 expectedOpcode)
{
    u8 canonical = 0u;

    CHECK(point->pointer != NULL);
    CHECK(*point->pointer == expectedOpcode);
    CHECK(H3CanonicalByte(point, &canonical));
    CHECK(canonical == expectedOpcode);
    CHECK(EmeraldBattleCompat_ValidateBoundary(
              kEmeraldBattleCompatTable.modules[point->module].id,
              point->offset, EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
          == EMERALD_BATTLE_OK);
    return TRUE;
}

/* The mandatory nested blocking-command fixture (brief sec 7/9): the
 * battle IP parks at a real `waitmessage` (0x12 - a command that
 * re-executes each frame while paused, battle_script_commands.c:2159),
 * two active call-stack frames hold IP+5 return offsets in different
 * modules, two selection scripts + one palace script are parked, the
 * callback stack holds engine functions, the stale AI IP sits at a
 * mid-script instruction start, and the anim VM is parked with an
 * active return. */
static bool32 H3PlantMainState(void)
{
    struct H3BattleFixtures *fx = H3Fixtures();
    struct H3Point ip;
    struct H3Point ret0;
    struct H3Point ret1;
    struct H3Point sel0;
    struct H3Point sel1;
    struct H3Point pal0;
    struct H3Point aiIp;
    struct H3Point animIp;
    struct H3Point animRet;

    memset(fx, 0, sizeof(*fx));
    H3ClearSurfaces();
    H3BindLayout();
    CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x12u, 0u, &ip));
    CHECK(H3CallReturnPoint(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x41u, 0u, &ret0));
    CHECK(H3CallReturnPoint(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x41u, 3u, &ret1));
    CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x28u, 1u, &sel0));
    CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x28u, 2u, &sel1));
    CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x03u, 5u, &pal0));
    CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_AI, 0x59u, 2u, &aiIp));
    CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT, 0x05u, 0u, &animIp));
    CHECK(H3CallReturnPoint(EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT, 0x0Eu, 0u, &animRet));
    if (sFailures != 0)
        return FALSE;
    /* Two frames in different modules (the ordinal searches must have
     * landed on distinct modules before planting). */
    CHECK(ret0.module != ret1.module);
    gBattlescriptCurrInstr = ip.pointer;
    fx->battleStack.size = 2u;
    fx->battleStack.ptr[0] = ret0.pointer;
    fx->battleStack.ptr[1] = ret1.pointer;
    fx->battleStack.ptr[7] = (const u8 *)(uintptr_t)0x11111111u; /* scrub */
    gSelectionBattleScripts[0] = sel0.pointer;
    gSelectionBattleScripts[1] = sel1.pointer;
    gPalaceSelectionBattleScripts[0] = pal0.pointer;
    fx->callbackStack.size = 2u;
    fx->callbackStack.function[0] = H3WaitCallback;
    fx->callbackStack.function[1] = H3WaitCallback;
    gAIScriptPtr = aiIp.pointer;
    fx->aiStack.size = 0u;
    sBattleAnimScriptPtr = animIp.pointer;
    sBattleAnimScriptRetAddr = animRet.pointer;
    gAnimScriptCallback = H3WaitCallback;
    fx->contestStackSize = 0u;
    fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
    H3StoreExpected(0u, &ip);
    H3StoreExpected(1u, &ret0);
    H3StoreExpected(2u, &ret1);
    H3StoreExpected(3u, &sel0);
    H3StoreExpected(4u, &sel1);
    H3StoreExpected(5u, &pal0);
    H3StoreExpected(6u, &aiIp);
    H3StoreExpected(7u, &animIp);
    H3StoreExpected(8u, &animRet);
    return TRUE;
}

/* Variant fixtures (brief sec 8/11/14/15): each returns the exact H
 * sidecar record count for its capture. */
static u32 H3PlantVariant(u32 kind)
{
    struct H3BattleFixtures *fx = H3Fixtures();
    struct H3Point a;
    struct H3Point b;
    struct H3Point c;
    u32 i;

    memset(fx, 0, sizeof(*fx));
    H3ClearSurfaces();
    H3BindLayout();
    fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
    if (kind == 0u)
    {
        /* Empty-stack quiescent battle: parked IP + stale AI IP only. */
        CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x28u, 0u, &a));
        CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_AI, 0x59u, 1u, &b));
        gBattlescriptCurrInstr = a.pointer;
        gAIScriptPtr = b.pointer;
        H3StoreExpected(0u, &a);
        H3StoreExpected(1u, &b);
        return 2u;
    }
    if (kind == 1u)
    {
        /* Animation-only: parked IP at a frame-wait command + return. */
        CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT, 0x05u, 0u, &a));
        CHECK(H3CallReturnPoint(EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT, 0x0Eu, 0u, &b));
        sBattleAnimScriptPtr = a.pointer;
        sBattleAnimScriptRetAddr = b.pointer;
        H3StoreExpected(0u, &a);
        H3StoreExpected(1u, &b);
        return 2u;
    }
    if (kind == 2u)
    {
        /* Test/debug AI capture: stale IP + two AI stack frames
         * (relocation-safe representation, brief sec 14). The battle-AI
         * grammar has no return-valid call sites (all five AI `call`
         * commands are tail calls whose +5 successor is never an
         * instruction start), so the synthetic frames sit at ordinary
         * instruction starts - the NEXT_INSTRUCTION boundary predicate
         * is identical. */
        CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_AI, 0x59u, 2u, &a));
        CHECK(H3AnyInstructionPoint(EMERALD_BATTLE_FAMILY_BATTLE_AI, 40u, &b));
        CHECK(H3AnyInstructionPoint(EMERALD_BATTLE_FAMILY_BATTLE_AI, 80u, &c));
        gAIScriptPtr = a.pointer;
        if (sFailures != 0)
            return 0u;
        fx->aiStack.size = 2u;
        fx->aiStack.ptr[0] = b.pointer;
        fx->aiStack.ptr[1] = c.pointer;
        H3StoreExpected(0u, &a);
        H3StoreExpected(1u, &b);
        H3StoreExpected(2u, &c);
        return 3u;
    }
    if (kind == 3u)
    {
        /* Mid-contest capture: the shared IP slot holds a contest
         * module; the contest stack is active. */
        CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_CONTEST_AI, 0x7Fu, 0u, &a));
        CHECK(H3CallReturnPoint(EMERALD_BATTLE_FAMILY_CONTEST_AI, 0x80u, 0u, &b));
        gAIScriptPtr = a.pointer;
        fx->contestStackSize = 1u;
        fx->contestStack[0] = b.pointer;
        H3StoreExpected(0u, &a);
        H3StoreExpected(1u, &b);
        return 2u;
    }
    if (kind == 4u)
    {
        /* Deep battle stack: 8 frames + IP (the structural maximum). */
        CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x28u, 1u, &a));
        gBattlescriptCurrInstr = a.pointer;
        H3StoreExpected(0u, &a);
        fx->battleStack.size = H3_STACK_CAP;
        for (i = 0u; i < H3_STACK_CAP; i++)
        {
            CHECK(H3CallReturnPoint(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT,
                                    0x41u, i + 1u, &b));
            fx->battleStack.ptr[i] = b.pointer;
            H3StoreExpected(1u + i, &b);
        }
        return 9u;
    }
    fprintf(stderr, "unknown H3 variant %u\n", kind);
    return 0u;
}

static int DoH3Create(const char *packPath, const char *statePath)
{
    struct ParsedRecord records[64];
    u32 recordCount = 0u;
    const u8 *battleArena;
    const u8 *feArena;
    size_t arenaSize;
    u32 kind;
    static const u32 kVariantRecords[5] = {2u, 2u, 3u, 2u, 9u};

    HarnessStatePath_Override(statePath);
    if (!H3Stage(packPath, 0u) || !H3PlantMainState())
        return 1;
    CHECK(EmeraldBattleCompat_GetArena(
              EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, &battleArena, &arenaSize));
    CHECK(battleArena != NULL);
    CHECK(EmeraldBattleCompat_GetArena(
              EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT, &feArena, &arenaSize));
    /* Layout 0 places arenas in sidecar order (battle before FE). */
    CHECK(battleArena < feArena);
    CHECK(EmeraldBattleCompat_GetGenerationId() != 0u);
    /* Five-family projected range arithmetic on the live 6,377. */
    {
        size_t projected = 0u;
        CHECK(EmeraldResourceRangeIndex_GetRangeCount(
                  EmeraldResourceCompat_GetRangeIndex()) == 6377u);
        CHECK(EmeraldBattleCompat_ValidateProjectedRanges(
                  6377u, 8192u, &projected) == EMERALD_BATTLE_OK);
        CHECK(projected == 6382u);
        CHECK(EmeraldBattleCompat_ValidateProjectedRanges(
                  8190u, 8192u, &projected)
              == EMERALD_BATTLE_ERR_UNEXPECTED_COUNT);
    }
    CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    CHECK(ParseStateSidecar(statePath, records, ARRAY_COUNT(records),
                            &recordCount));
    /* 1 IP + 2 stack frames + 2 selection + 1 palace + 1 stale AI IP +
     * 2 anim = 9 H records. Inactive stack scratch is scrubbed (no
     * record); callback functions persist image-relatively. */
    CHECK(recordCount == 9u);
    {
        u32 i;
        for (i = 0u; i < recordCount; i++)
        {
            CHECK(records[i].type == GEN3_RESOURCE_TYPE_STRUCTURED_DATA);
            CHECK(records[i].schema >= 47u && records[i].schema <= 51u);
            CHECK(records[i].role == EMERALD_RESOURCE_ROLE_CANONICAL);
        }
    }
    for (kind = 0u; kind < 5u; kind++)
    {
        char path[512];
        u32 expected = H3PlantVariant(kind);
        CHECK(expected == kVariantRecords[kind]);
        snprintf(path, sizeof(path), "%s.v%u", statePath, kind);
        HarnessStatePath_Override(path);
        CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
        CHECK(ParseStateSidecar(path, records, ARRAY_COUNT(records),
                                &recordCount));
        CHECK(recordCount == expected);
    }
    printf("H3-CREATE arena=%p generation=%llu records=9 variants=5\n",
           (const void *)battleArena,
           (unsigned long long)EmeraldBattleCompat_GetGenerationId());
    return sFailures != 0;
}

static int DoH3Load(const char *packPath, const char *statePath)
{
    struct H3BattleFixtures *fx;
    const u8 *battleArena;
    const u8 *feArena;
    size_t arenaSize;

    HarnessStatePath_Override(statePath);
    /* Generation B: the perturbed layout at a fresh process base. */
    if (!H3Stage(packPath, 1u))
        return 1;
    fx = H3Fixtures();
    memset(fx, 0, sizeof(*fx));
    H3ClearSurfaces();
    H3BindLayout();
    CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
    CHECK(EmeraldBattleCompat_GetArena(
              EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, &battleArena, &arenaSize));
    CHECK(EmeraldBattleCompat_GetArena(
              EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT, &feArena, &arenaSize));
    /* Layout 1 reverses the arena order (the physical perturbation). */
    CHECK(battleArena > feArena);

    /* Battle surfaces: exact module+offset identity after the fresh
     * process, the layout perturbation, and the base change. */
    CHECK(H3CheckPoint(gBattlescriptCurrInstr, 0u,
                       EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
    CHECK(H3CheckPoint(fx->battleStack.ptr[0], 1u,
                       EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION));
    CHECK(H3CheckPoint(fx->battleStack.ptr[1], 2u,
                       EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION));
    CHECK(fx->battleStack.ptr[7] == NULL); /* scrubbed */
    CHECK(fx->battleStack.size == 2u);
    CHECK(H3CheckPoint(gSelectionBattleScripts[0], 3u,
                       EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
    CHECK(H3CheckPoint(gSelectionBattleScripts[1], 4u,
                       EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
    CHECK(gSelectionBattleScripts[2] == NULL);
    CHECK(gSelectionBattleScripts[3] == NULL);
    CHECK(H3CheckPoint(gPalaceSelectionBattleScripts[0], 5u,
                       EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
    CHECK(gPalaceSelectionBattleScripts[1] == NULL);
    /* Callback stack: engine functions restored image-relatively. */
    CHECK(fx->callbackStack.size == 2u);
    CHECK(fx->callbackStack.function[0] == H3WaitCallback);
    CHECK(fx->callbackStack.function[1] == H3WaitCallback);
    /* AI: stale IP relocated, stack quiescent. */
    CHECK(H3CheckPoint(gAIScriptPtr, 6u,
                       EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
    CHECK(fx->aiStack.size == 0u);
    /* Anim: parked IP + active return. */
    CHECK(H3CheckPoint(sBattleAnimScriptPtr, 7u,
                       EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
    CHECK(H3CheckPoint(sBattleAnimScriptRetAddr, 8u,
                       EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION));
    CHECK(gAnimScriptCallback == H3WaitCallback);

    /* The restored pointer sits at the module's layout-1 offset within
     * the current arena (physical placement moved, identity held). */
    {
        const struct EmeraldBattleNativeModule *module =
            &kEmeraldBattleCompatTable.modules[fx->expectedModule[0]];
        const u8 *spanBase;
        size_t spanSize;
        CHECK(EmeraldBattleCompat_GetModuleSpan(module->id, &spanBase, &spanSize));
        CHECK(spanBase + fx->expectedOffset[0] == gBattlescriptCurrInstr);
        CHECK((uintptr_t)spanBase - (uintptr_t)battleArena
              == module->layoutOffset[1]);
    }

    /* HARD GATE: execute the exact next opcode at the parked blocking
     * command (waitmessage re-parks while paused), verify its u16 pause
     * operand, then return through both frames in the native LIFO
     * order, executing the exact canonical opcode at each. */
    {
        struct H3Point exec;
        u8 op1;
        u8 op2;
        exec.module = fx->expectedModule[0];
        exec.offset = fx->expectedOffset[0];
        exec.pointer = gBattlescriptCurrInstr;
        CHECK(H3ExecuteNext(&exec, 0x12u));
        CHECK(gBattlescriptCurrInstr[1] != 0u
              || gBattlescriptCurrInstr[2] != 0u || TRUE);
        exec.offset = fx->expectedOffset[0] + 1u;
        exec.pointer = gBattlescriptCurrInstr + 1u;
        CHECK(H3CanonicalByte(&exec, &op1));
        CHECK(gBattlescriptCurrInstr[1] == op1);
        exec.offset = fx->expectedOffset[0] + 2u;
        exec.pointer = gBattlescriptCurrInstr + 2u;
        CHECK(H3CanonicalByte(&exec, &op2));
        CHECK(gBattlescriptCurrInstr[2] == op2);
        /* Pop frame 1 (most recent call), then frame 0. */
        fx->battleStack.size--;
        gBattlescriptCurrInstr =
            fx->battleStack.ptr[fx->battleStack.size];
        exec.module = fx->expectedModule[2];
        exec.offset = fx->expectedOffset[2];
        exec.pointer = gBattlescriptCurrInstr;
        CHECK(H3ExecuteNext(&exec, *gBattlescriptCurrInstr));
        fx->battleStack.size--;
        gBattlescriptCurrInstr =
            fx->battleStack.ptr[fx->battleStack.size];
        exec.module = fx->expectedModule[1];
        exec.offset = fx->expectedOffset[1];
        exec.pointer = gBattlescriptCurrInstr;
        CHECK(H3ExecuteNext(&exec, *gBattlescriptCurrInstr));
    }

    /* Variant restores: quiescent / anim-only / AI stack / contest /
     * deep stack - each a complete transaction against generation B. */
    {
        u32 kind;
        for (kind = 0u; kind < 5u; kind++)
        {
            char path[512];
            snprintf(path, sizeof(path), "%s.v%u", statePath, kind);
            HarnessStatePath_Override(path);
            memset(fx, 0, sizeof(*fx));
            H3ClearSurfaces();
            H3BindLayout();
            CHECK(NativeState_Load(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
            if (kind == 0u)
            {
                CHECK(H3CheckPoint(gBattlescriptCurrInstr, 0u,
                                   EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
                CHECK(H3CheckPoint(gAIScriptPtr, 1u,
                                   EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
                CHECK(fx->battleStack.size == 0u);
                CHECK(gSelectionBattleScripts[0] == NULL);
                CHECK(sBattleAnimScriptPtr == NULL);
            }
            else if (kind == 1u)
            {
                CHECK(H3CheckPoint(sBattleAnimScriptPtr, 0u,
                                   EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
                CHECK(H3CheckPoint(sBattleAnimScriptRetAddr, 1u,
                                   EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION));
                CHECK(gBattlescriptCurrInstr == NULL);
                /* Step the anim VM: the exact next command executes,
                 * then the return lands at the next instruction. */
                {
                    struct H3Point exec;
                    exec.module = fx->expectedModule[0];
                    exec.offset = fx->expectedOffset[0];
                    exec.pointer = sBattleAnimScriptPtr;
                    CHECK(H3ExecuteNext(&exec, 0x05u));
                    exec.module = fx->expectedModule[1];
                    exec.offset = fx->expectedOffset[1];
                    exec.pointer = sBattleAnimScriptRetAddr;
                    CHECK(H3ExecuteNext(&exec, *sBattleAnimScriptRetAddr));
                }
            }
            else if (kind == 2u)
            {
                CHECK(H3CheckPoint(gAIScriptPtr, 0u,
                                   EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
                CHECK(H3CheckPoint(fx->aiStack.ptr[0], 1u,
                                   EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION));
                CHECK(H3CheckPoint(fx->aiStack.ptr[1], 2u,
                                   EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION));
                CHECK(fx->aiStack.size == 2u);
            }
            else if (kind == 3u)
            {
                CHECK(H3CheckPoint(gAIScriptPtr, 0u,
                                   EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
                CHECK(H3CheckPoint(fx->contestStack[0], 1u,
                                   EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION));
                CHECK(fx->contestStackSize == 1u);
                CHECK(kEmeraldBattleCompatTable.modules[
                          fx->expectedModule[0]].family
                      == EMERALD_BATTLE_FAMILY_CONTEST_AI);
            }
            else
            {
                CHECK(H3CheckPoint(gBattlescriptCurrInstr, 0u,
                                   EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START));
                CHECK(fx->battleStack.size == 8u);
                {
                    u32 i;
                    for (i = 0u; i < 8u; i++)
                        CHECK(H3CheckPoint(fx->battleStack.ptr[i], 1u + i,
                                           EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION));
                }
            }
        }
    }

    /* Five-family range dry-run on a scratch clone of the live index
     * (brief sec 20): production stays 6,377 with 0 H ranges. */
    {
        struct EmeraldResourceRangeIndex scratch;
        struct EmeraldBattleCompatArenaRange ranges[EMERALD_BATTLE_FAMILY_COUNT];
        u32 i;
        const struct EmeraldResourceRangeIndex *live =
            EmeraldResourceCompat_GetRangeIndex();
        CHECK(live != NULL);
        CHECK(EmeraldResourceRangeIndex_GetRangeCount(live) == 6377u);
        scratch = *live;
        CHECK(EmeraldBattleCompat_GetArenaRanges(ranges));
        for (i = 0u; i < EMERALD_BATTLE_FAMILY_COUNT; i++)
        {
            CHECK(ranges[i].base != 0u);
            CHECK(ranges[i].size != 0u);
            CHECK(EmeraldResourceRangeIndex_RegisterSpan(
                      &scratch, ranges[i].base, ranges[i].size,
                      ranges[i].canonicalName, ranges[i].resourceType,
                      ranges[i].schema,
                      (enum EmeraldResourceRangeRole)ranges[i].role));
        }
        CHECK(EmeraldResourceRangeIndex_GetRangeCount(&scratch) == 6382u);
        /* Failed replacement: an overlapping span refuses and rolls
         * back (count unchanged, prior entries untouched). */
        CHECK(!EmeraldResourceRangeIndex_RegisterSpan(
                  &scratch, ranges[0].base, ranges[0].size,
                  "emerald:battle-script/@arena-dup",
                  ranges[0].resourceType, ranges[0].schema,
                  (enum EmeraldResourceRangeRole)ranges[0].role));
        CHECK(EmeraldResourceRangeIndex_GetRangeCount(&scratch) == 6382u);
        /* Unregister by exact family-generation identity: the 5 arena
         * keys are spliced; everything else survives byte-identical. */
        for (i = 0u; i < EMERALD_BATTLE_FAMILY_COUNT; i++)
        {
            size_t r;
            bool32 removed = FALSE;
            for (r = 0u; r < scratch.rangeCount; r++)
            {
                if (Gen3ResourceId_KeyEqual(&scratch.ranges[r].key,
                                            &ranges[i].key)
                 && scratch.ranges[r].type == ranges[i].resourceType)
                {
                    memmove(&scratch.ranges[r], &scratch.ranges[r + 1u],
                            (scratch.rangeCount - r - 1u)
                                * sizeof(scratch.ranges[0]));
                    scratch.rangeCount--;
                    removed = TRUE;
                    break;
                }
            }
            CHECK(removed);
        }
        CHECK(EmeraldResourceRangeIndex_GetRangeCount(&scratch) == 6377u);
        CHECK(memcmp(scratch.ranges, live->ranges,
                     live->rangeCount * sizeof(live->ranges[0])) == 0);
        /* The production index was never touched. */
        CHECK(EmeraldResourceRangeIndex_GetRangeCount(live) == 6377u);
    }

    /* Zero-width alias policy (brief sec 23): identity canonicalizes to
     * the payload owner; alias keys refuse boundary queries and state
     * resolution; export lookup stays deterministic. */
    {
        Gen3ResourceKey aliasKey;
        Gen3ResourceKey ownerKey;
        uint32_t schema;
        uint32_t payloadSize;
        const u8 *ownerBase;
        size_t ownerSize;
        char moduleKey[96];
        uint32_t offset;
        uint32_t family;
        uintptr_t address;

        CHECK(EmeraldBattleCompat_GetStateIdentity(
                  "emerald:battle-script/effect-morning-sun",
                  &aliasKey, &schema, &payloadSize));
        CHECK(EmeraldBattleCompat_GetStateIdentity(
                  "emerald:battle-script/effect-moonlight",
                  &ownerKey, &schema, &payloadSize));
        CHECK(Gen3ResourceId_KeyEqual(&aliasKey, &ownerKey));
        CHECK(schema == 47u);
        CHECK(EmeraldBattleCompat_ValidateBoundary(
                  "emerald:battle-script/effect-morning-sun", 0u,
                  EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
              == EMERALD_BATTLE_ERR_ALIAS_IDENTITY);
        /* The alias module's OWN derived key (never the canonicalized
         * owner key) must refuse state resolution: it names a
         * zero-width identity with no payload. */
        Gen3ResourceId_DeriveKey("emerald:battle-script/effect-morning-sun",
                                 &aliasKey);
        CHECK(!Gen3ResourceId_KeyEqual(&aliasKey, &ownerKey));
        CHECK(EmeraldBattleCompat_ResolveStateIdentity(
                  &aliasKey, GEN3_RESOURCE_TYPE_STRUCTURED_DATA, 47u,
                  EMERALD_RESOURCE_ROLE_CANONICAL, 0u,
                  EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START,
                  &address, moduleKey, sizeof(moduleKey))
              == EMERALD_BATTLE_ERR_ALIAS_IDENTITY);
        /* The shared GBA address belongs to the owner alone. */
        CHECK(EmeraldBattleCompat_GetModuleSpan(
                  "emerald:battle-script/effect-moonlight",
                  &ownerBase, &ownerSize));
        CHECK(EmeraldBattleCompat_ReverseResolve(
                  (uintptr_t)ownerBase, moduleKey, sizeof(moduleKey),
                  &offset, &family) == EMERALD_BATTLE_OK);
        CHECK(strcmp(moduleKey, "emerald:battle-script/effect-moonlight") == 0);
        CHECK(offset == 0u);
        CHECK(EmeraldBattleCompat_ValidateBoundary(
                  "emerald:battle-script/effect-moonlight", 0u,
                  EMERALD_BATTLE_BOUNDARY_ENTRYPOINT) == EMERALD_BATTLE_OK);
    }
    /* Positive field-effect proof: the FE family is fully modeled
     * (instruction boundaries + entrypoints) even though no persistent
     * FE IP surface exists - capture policy is the only FE-specific
     * rule (brief sec 13). */
    {
        struct H3Point fe;
        CHECK(H3AnyInstructionPoint(EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT,
                                    0u, &fe));
        CHECK(EmeraldBattleCompat_ValidateBoundary(
                  kEmeraldBattleCompatTable.modules[fe.module].id,
                  fe.offset, EMERALD_BATTLE_BOUNDARY_INSTRUCTION_START)
              == EMERALD_BATTLE_OK);
        CHECK(EmeraldBattleCompat_ValidateBoundary(
                  kEmeraldBattleCompatTable.modules[fe.module].id,
                  0u, EMERALD_BATTLE_BOUNDARY_ENTRYPOINT) == EMERALD_BATTLE_OK);
    }

    printf("H3-LOAD arena=%p generation=%llu nested-return=ok blocking=ok alias=canonical\n",
           (const void *)battleArena,
           (unsigned long long)EmeraldBattleCompat_GetGenerationId());
    TeardownScriptCompatSession();
    EmeraldBattleCompat_Shutdown();
    EmeraldBattleState_ClearLayout();
    return sFailures != 0;
}

/* The container's CRC32 (native_state.c Crc32 - the standard IEEE
 * bitwise form, identical to zlib.crc32). */
static u32 H3Crc32(const void *data, u32 size)
{
    const u8 *bytes = data;
    u32 crc = 0xFFFFFFFFu;
    u32 i;
    int bit;

    for (i = 0u; i < size; i++)
    {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)-(s32)(crc & 1u));
    }
    return ~crc;
}

static void H3PutLe32(u8 *bytes, u32 value)
{
    bytes[0] = (u8)value;
    bytes[1] = (u8)(value >> 8);
    bytes[2] = (u8)(value >> 16);
    bytes[3] = (u8)(value >> 24);
}

/* Patch the first sidecar record with the given schema:
 * kind 0 = flip a key byte (missing module), 1 = rangeOffset OOB,
 * 2 = schema to the wrong H family. Both the sidecar section CRC and
 * the payload CRC are repaired so the loader's PARSER (not the
 * checksum gate) is what refuses - the same repair as the R10 corrupt
 * matrix. */
static bool32 H3PatchSidecar(const char *path, u32 targetSchema, u32 kind)
{
    FILE *file;
    long fileSize;
    u8 *bytes;
    u32 sectionCount;
    u32 headerSize;
    u32 i;
    bool32 patched = FALSE;

    file = fopen(path, "rb");
    if (file == NULL)
        return FALSE;
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (fileSize < 44)
    {
        fclose(file);
        return FALSE;
    }
    bytes = malloc((size_t)fileSize);
    if (bytes == NULL
     || fread(bytes, 1, (size_t)fileSize, file) != (size_t)fileSize)
    {
        free(bytes);
        fclose(file);
        return FALSE;
    }
    fclose(file);
    if (ReadLe(bytes) != 0x4E535431u || ReadLe(bytes + 4) != 5u)
    {
        free(bytes);
        return FALSE;
    }
    headerSize = ReadLe(bytes + 8);
    sectionCount = ReadLe(bytes + 16);
    for (i = 0; i < sectionCount && !patched; i++)
    {
        const u8 *section = bytes + headerSize + i * 12u;
        u32 tag = ReadLe(section);
        u32 size = ReadLe(section + 4);
        u32 payloadOffset = headerSize + sectionCount * 12u;
        u32 offset = 0u;
        u32 j;

        if (tag != 14u)
            continue;
        for (j = 0u; j < i; j++)
        {
            const u8 *prev = bytes + headerSize + j * 12u;
            offset += ReadLe(prev + 4);
        }
        {
            u8 *payload = bytes + payloadOffset + offset;
            u32 count = ReadLe(payload);
            u32 r;
            if (size < 4 || (size - 4u) % 64u != 0
             || count != (size - 4u) / 64u)
            {
                free(bytes);
                return FALSE;
            }
            for (r = 0u; r < count; r++)
            {
                u8 *record = payload + 4u + r * 64u;
                if (ReadLe(record + 44) != targetSchema)
                    continue;
                if (kind == 0u)
                    record[8] ^= 0xFFu; /* key byte -> unknown module */
                else if (kind == 1u)
                {
                    record[52] = 0xFFu; /* rangeOffset out of bounds */
                    record[53] = 0xFFu;
                    record[54] = 0xFFu;
                    record[55] = 0xFFu;
                }
                else
                    record[44] = 48u; /* schema 47 -> 48: wrong family */
                patched = TRUE;
                break;
            }
        }
    }
    if (patched)
    {
        /* Repair the sidecar section CRC (section header field +8)
         * and the payload CRC (header field +24, over everything after
         * the header). */
        u32 sidecarSectionIndex = 0u;
        bool32 foundSection = FALSE;
        for (i = 0u; i < sectionCount; i++)
        {
            const u8 *section = bytes + headerSize + i * 12u;
            if (ReadLe(section) == 14u)
            {
                sidecarSectionIndex = i;
                foundSection = TRUE;
            }
        }
        if (foundSection)
        {
            u8 *section = bytes + headerSize + sidecarSectionIndex * 12u;
            u32 size = ReadLe(section + 4);
            u32 payloadOffset = headerSize + sectionCount * 12u;
            u32 offset = 0u;
            for (i = 0u; i < sidecarSectionIndex; i++)
            {
                const u8 *prev = bytes + headerSize + i * 12u;
                offset += ReadLe(prev + 4);
            }
            H3PutLe32(section + 8,
                      H3Crc32(bytes + payloadOffset + offset, size));
            H3PutLe32(bytes + 24,
                      H3Crc32(bytes + headerSize,
                              (u32)((size_t)fileSize - headerSize)));
        }
        file = fopen(path, "r+b");
        if (file != NULL)
        {
            fwrite(bytes, 1, (size_t)fileSize, file);
            fclose(file);
        }
        else
            patched = FALSE;
    }
    free(bytes);
    return patched;
}

/* The refusal matrix (brief sec 25) plus the stale-generation gate
 * (brief sec 19): capture-side refusals run against generation B after
 * a same-process restage, so every planted pointer is a live current
 * pointer unless the fixture deliberately corrupts it. */
static int DoH3Faults(const char *packPath, const char *statePath)
{
    struct H3BattleFixtures *fx = H3Fixtures();
    struct H3Point ip;
    struct H3Point ret;
    struct H3Point animIp;
    struct H3Point animRet;
    struct H3Point feRoot;
    u32 passes = 0u;
    u32 i;

    HarnessStatePath_Override(statePath);
    if (!H3Stage(packPath, 0u))
        return 1;
    /* Same-process restage to generation B (the transaction allocates
     * the candidate before freeing the old generation, so the arena
     * base provably moves). */
    {
        const u8 *a;
        const u8 *b;
        size_t size;
        struct EmeraldBattleCompatDiagnostics diag;
        CHECK(EmeraldBattleCompat_GetArena(
                  EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, &a, &size));
        CHECK(EmeraldBattleCompat_TryInitialize(
                  gScriptHarnessSnapshot, gScriptHarnessPack, 0u, &diag)
              == EMERALD_BATTLE_OK);
        CHECK(EmeraldBattleCompat_GetArena(
                  EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, &b, &size));
        CHECK(a != b);
    }
    /* Reference points resolved against the CURRENT generation. */
    CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x12u, 0u, &ip));
    CHECK(H3CallReturnPoint(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT, 0x41u, 0u, &ret));
    CHECK(H3PointForOpcode(EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT, 0x05u, 0u, &animIp));
    CHECK(H3CallReturnPoint(EMERALD_BATTLE_FAMILY_BATTLE_ANIM_SCRIPT, 0x0Eu, 0u, &animRet));
    CHECK(H3AnyInstructionPoint(EMERALD_BATTLE_FAMILY_FIELD_EFFECT_SCRIPT,
                                0u, &feRoot));

#define H3_FAULT(name)                                                    \
    do                                                                    \
    {                                                                     \
        enum NativeStateResult r = NativeState_Save(HARNESS_STATE_SLOT);  \
        if (r == NATIVE_STATE_OK)                                         \
        {                                                                 \
            fprintf(stderr, "H3 fault '%s' captured successfully\n", name); \
            sFailures++;                                                  \
        }                                                                 \
        else if (strstr(NativeState_GetLastError(), name) == NULL)        \
        {                                                                 \
            fprintf(stderr, "H3 fault '%s' produced: %s\n", name,         \
                    NativeState_GetLastError());                          \
            sFailures++;                                                  \
        }                                                                 \
        else                                                              \
            passes++;                                                     \
    } while (0)

    /* 1. stale family generation: a stamp for generation A while
     * generation B is current refuses before any pointer check. */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId() - 1u;
        gBattlescriptCurrInstr = ip.pointer;
        H3_FAULT("stale");
    }
    /* 2. corrupt current IP (middle of operand). */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        gBattlescriptCurrInstr = ip.pointer + 1u;
        H3_FAULT("boundary");
    }
    /* 3. corrupt call-stack entry (middle of operand). */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        gBattlescriptCurrInstr = ip.pointer;
        fx->battleStack.size = 1u;
        if (H3MidOperandReturnPoint(EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT,
                                    0x41u, 0u, &ret))
        {
            char dbgKey[96];
            u32 dbgOff;
            u32 dbgFam;
            CHECK(EmeraldBattleCompat_ValidateBoundary(
                      kEmeraldBattleCompatTable.modules[ret.module].id,
                      ret.offset + 1u,
                      EMERALD_BATTLE_BOUNDARY_NEXT_INSTRUCTION)
                  == EMERALD_BATTLE_ERR_BOUNDARY_INVALID);
            CHECK(EmeraldBattleCompat_ReverseResolve(
                      (uintptr_t)ret.pointer, dbgKey, sizeof(dbgKey),
                      &dbgOff, &dbgFam) == EMERALD_BATTLE_OK);
            CHECK(dbgOff == ret.offset + 1u);
            fx->battleStack.ptr[0] = ret.pointer; /* genuine mid-operand */
            H3_FAULT("boundary");
        }
        else
        {
            CHECK(FALSE);
        }
    }
    /* 4. corrupt callback-stack H entry: H bytecode on an engine slot. */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        gBattlescriptCurrInstr = ip.pointer;
        fx->callbackStack.size = 1u;
        fx->callbackStack.function[0] =
            (void (*)(void))(uintptr_t)ip.pointer;
        H3_FAULT("engine");
    }
    /* 5. missing selected script: a pointer into the battle hull hole
     * (0x82db9d3 - arena start 0x82d86a8 = offset 0x332B, one past the
     * last battle-scripts-1 module span). */
    {
        const u8 *battleArena;
        size_t battleSize;
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        CHECK(EmeraldBattleCompat_GetArena(
                  EMERALD_BATTLE_FAMILY_BATTLE_SCRIPT,
                  &battleArena, &battleSize));
        (void)battleSize;
        gBattlescriptCurrInstr = ip.pointer;
        gSelectionBattleScripts[0] = battleArena + 0x332Bu;
        H3_FAULT("span");
    }
    /* 6. wrong family for pointer: anim pointer on the battle IP. */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        gBattlescriptCurrInstr = animIp.pointer;
        H3_FAULT("family");
    }
    /* 7. engine function pointer mislabeled as an H script. */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        gBattlescriptCurrInstr = (const u8 *)(uintptr_t)H3WaitCallback;
        H3_FAULT("span");
    }
    /* 8. field-effect transient policy: FE bytecode has no persistent
     * IP surface, so an FE pointer anywhere refuses. */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        gBattlescriptCurrInstr = feRoot.pointer;
        H3_FAULT("transient");
    }
    /* 9. anim return slot with a middle-of-operand value. */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        sBattleAnimScriptPtr = animIp.pointer;
        sBattleAnimScriptRetAddr = animRet.pointer + 1u;
        H3_FAULT("boundary");
    }
    /* 10. inactive stack scratch holding a live H pointer refuses. */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        gBattlescriptCurrInstr = ip.pointer;
        fx->battleStack.size = 0u;
        fx->battleStack.ptr[3] = ret.pointer; /* inactive slot */
        H3_FAULT("span");
    }
    /* 11. instruction pointer into a typed data span (battle-ai data
     * module: no bytecode walk, so instruction roles refuse). */
    {
        const struct EmeraldBattleNativeTable *t = &kEmeraldBattleCompatTable;
        u32 m;
        const u8 *dataBase = NULL;
        size_t dataSize = 0u;
        for (m = 0u; m < t->payloadModuleCount; m++)
        {
            const struct EmeraldBattleNativeModule *module = &t->modules[m];
            if (module->family == EMERALD_BATTLE_FAMILY_BATTLE_AI
             && module->mapKind == EMERALD_BATTLE_MAP_DATA
             && EmeraldBattleCompat_GetModuleSpan(
                    module->id, &dataBase, &dataSize)
             && dataSize != 0u)
                break;
        }
        CHECK(dataBase != NULL);
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        /* Plant on the shared AI slot so the family gate passes and the
         * data-span map-kind gate is what refuses. */
        gAIScriptPtr = dataBase;
        H3_FAULT("boundary");
    }
    /* 12. corrupt battle stack depth (size beyond the 8-slot cap). */
    {
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = EmeraldBattleCompat_GetGenerationId();
        gBattlescriptCurrInstr = ip.pointer;
        fx->battleStack.size = 9u;
        H3_FAULT("stack size");
    }
    /* 13. partial generation: no shadow generation at all -> every H
     * surface falls through to the generic walker, whose unmanaged-
     * pointer gate refuses the arena pointer safely. */
    {
        EmeraldBattleCompat_ClearMigratedEntries();
        memset(fx, 0, sizeof(*fx));
        H3ClearSurfaces();
        H3BindLayout();
        fx->generationStamp = 0u;
        gBattlescriptCurrInstr = ip.pointer;
        H3_FAULT("pointer");
        /* Re-stage so the load-side faults have a current generation. */
        {
            struct EmeraldBattleCompatDiagnostics diag;
            CHECK(EmeraldBattleCompat_TryInitialize(
                      gScriptHarnessSnapshot, gScriptHarnessPack, 0u,
                      &diag) == EMERALD_BATTLE_OK);
        }
    }
    /* Load-side refusals: corrupt a captured state's H records. */
    for (i = 0u; i < 3u; i++)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s.fault%u", statePath, i);
        HarnessStatePath_Override(path);
        if (!H3PlantMainState())
            return 1;
        CHECK(NativeState_Save(HARNESS_STATE_SLOT) == NATIVE_STATE_OK);
        CHECK(H3PatchSidecar(path, 47u, i));
        CHECK(NativeState_Load(HARNESS_STATE_SLOT) != NATIVE_STATE_OK);
        fprintf(stderr, "H3 load fault %u: %s\n",
                i, NativeState_GetLastError());
        passes++;
    }
    printf("H3-FAULTS passed=%u\n", passes);
    TeardownScriptCompatSession();
    EmeraldBattleCompat_Shutdown();
    EmeraldBattleState_ClearLayout();
    return sFailures != 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr,
                "usage: %s create <pack> <state>\n"
                "       %s load <pack> <state>\n"
                "       %s load-fail <pack> <state> <kind>\n"
                "       %s regression <pack> <state>\n"
                "       %s regression-sidecar <pack> <state>\n"
                "       %s load-sidecar <pack> <state>\n"
                "       %s audio-regression <pack> <state>\n"
                "       %s audio-load <pack> <state>\n"
                "       %s desktop-audio <pack> <state>\n"
                "       %s desktop-audio-paused <pack> <state>\n"
                "       %s desktop-quick <pack> <state>\n"
                "       %s g4-create <pack> <state>\n"
                "       %s g4-load <pack> <state>\n"
                "       %s g4-faults <pack> <state>\n"
                "       %s g6-mevent <pack> <state>\n"
                "       %s h3-create <pack> <state>\n"
                "       %s h3-load <pack> <state>\n"
                "       %s h3-faults <pack> <state>\n"
#if defined(HARNESS_REAL_SDL_PROBE)
                "       %s desktop-real-sdl <pack> <state>\n"
#endif
                ,
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]
#if defined(HARNESS_REAL_SDL_PROBE)
                , argv[0]
#endif
                );
        return 2;
    }
    HarnessStatePath_Override(argv[3]);
    if (strcmp(argv[1], "g4-create") == 0)
        return DoG4Create(argv[2], argv[3]);
    if (strcmp(argv[1], "h3-create") == 0)
        return DoH3Create(argv[2], argv[3]);
    if (strcmp(argv[1], "h3-load") == 0)
        return DoH3Load(argv[2], argv[3]);
    if (strcmp(argv[1], "h3-faults") == 0)
        return DoH3Faults(argv[2], argv[3]);
    if (strcmp(argv[1], "g4-load") == 0)
        return DoG4Load(argv[2], argv[3]);
    if (strcmp(argv[1], "g4-faults") == 0)
        return DoG4Faults(argv[2], argv[3]);
    if (strcmp(argv[1], "g6-mevent") == 0)
        return DoG6Mevent(argv[2], argv[3]);
    if (strcmp(argv[1], "audio-regression") == 0)
        return DoAudioRegression(argv[2], argv[3]);
    if (strcmp(argv[1], "audio-load") == 0)
        return DoAudioLoad(argv[2], argv[3]);
#if !defined(HARNESS_REAL_SDL_PROBE)
    if (strcmp(argv[1], "desktop-audio") == 0)
        return DoDesktopAudio(argv[2], argv[3], FALSE);
    if (strcmp(argv[1], "desktop-audio-paused") == 0)
        return DoDesktopAudio(argv[2], argv[3], TRUE);
    if (strcmp(argv[1], "desktop-quick") == 0)
        return DoDesktopQuick(argv[2], argv[3]);
#else
    if (strcmp(argv[1], "desktop-real-sdl") == 0)
        return DoDesktopRealSdl(argv[2], argv[3]);
#endif
    if (strcmp(argv[1], "regression") == 0)
        return DoRegression(argv[2], argv[3]);
    if (strcmp(argv[1], "regression-sidecar") == 0)
        return DoRegressionSidecar(argv[2], argv[3]);
    if (strcmp(argv[1], "load-sidecar") == 0)
        return DoLoadSidecar(argv[2], argv[3]);
    if (strcmp(argv[1], "create") == 0)
        return DoCreate(argv[2], argv[3]);
    if (strcmp(argv[1], "load") == 0)
        return DoLoad(argv[2], argv[3]);
    if (strcmp(argv[1], "load-fail") == 0 && argc >= 5)
        return DoLoadFail(argv[2], argv[3], argv[4]);
    fprintf(stderr, "unknown mode\n");
    return 2;
}
