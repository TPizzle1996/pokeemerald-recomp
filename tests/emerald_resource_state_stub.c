/* R10-G/H: platform/game stubs for the cross-restart state harness.
 *
 * The harness compiles the REAL native_state.c + host_memory.c walker and
 * restore machinery, so it must satisfy every platform and game symbol the
 * walker touches. This TU provides:
 *
 *   - the platform layer (storage = real temp-file I/O, runtime ranges =
 *     the harness binary's own image/game regions, mapped = real mincore),
 *   - the task/sprite state sidecars as empty (size 0) implementations,
 *   - the game objects the walker models (gSprites, sprite template
 *     sidecars, battle buffers, task/window/main tables) as static zeroed
 *     instances,
 *   - the video/framebuffer/clock/audio platform calls as inert stubs.
 *
 * The harness binary is PIE by default, so image-relative persistent
 * addresses (HOST_PERSISTENT_DATA_IMAGE) are ASLR-invariant and resolve
 * correctly across the creator/loader processes - the walker's real
 * cross-process contract.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "global.h"
#include "sprite.h"
#include "task.h"
#include "window.h"
#include "main.h"
#include "battle.h"
#include "battle_message.h"
#include "battle_anim.h"
#include "gba/defines.h"
#include "gba/io_reg.h"
#include "platform.h"
#include "platform/desktop_profiles.h"
#include "platform/desktop_state_metadata.h"
#include "platform/desktop_state_thumbnail.h"
#include "platform/desktop_storage.h"

/* ------------------------------------------------------------------ */
/* Video/memory surfaces the walker copies.                           */
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char PLTT[0x400];
unsigned char OAM[0x400];
unsigned char REG_BASE[0x4000];
unsigned char FLASH_BASE[0x20000];
unsigned char __start_host_data[1];
unsigned char __stop_host_data[1];

/* ------------------------------------------------------------------ */
/* Game objects the walker models (static zeroed instances).          */
u8 apuCycle;
u8 gHeap[0x40000]; /* the game's portable allocator (data.c relocations) */
struct Sprite gSprites[MAX_SPRITES + 1];
/* The sprite template/image sidecar arrays live in the real build's EWRAM
 * section; the harness places them in .gba_ewram too so the walker's EWRAM
 * slice covers them and the typed-region scalar/padding rule is exercised
 * (maps.o contributes the rest of the section). */
EWRAM_DATA struct SpriteTemplate sSpriteTemplateSidecars[MAX_SPRITES + 1] = {0};
EWRAM_DATA struct SpriteFrameImage sSpriteTemplateImageSidecars[MAX_SPRITES + 1] = {0};
struct Task gTasks[16];
struct Window gWindows[20];
struct Main gMain;
struct MonSpritesGfx *gMonSpritesGfxPtr;
static struct BattleResources sBattleResources;
struct BattleResources *gBattleResources = &sBattleResources;
static struct BattleStruct sBattleStruct;
struct BattleStruct *gBattleStruct = &sBattleStruct;
u8 gBattleBufferA[MAX_BATTLERS_COUNT][0x200];
u8 gBattleBufferB[MAX_BATTLERS_COUNT][0x200];
u8 *gLinkBattleSendBuffer;
u8 *gLinkBattleRecvBuffer;
/* R13-H3: the battle-family execution surfaces match the production
 * EWRAM_DATA declarations exactly (battle_main.c:184-188,
 * battle_ai_script_commands.c:154, battle_anim.c:92-94), so the
 * walker's EWRAM slice serializes them with production layout parity.
 * The anim statics are file-local in production; the harness owns
 * same-shaped copies here. */
EWRAM_DATA const u8 *gAIScriptPtr = NULL;
EWRAM_DATA void (*gAnimScriptCallback)(void) = NULL;
EWRAM_DATA const u8 *gBattlescriptCurrInstr = NULL;
EWRAM_DATA const u8 *gSelectionBattleScripts[MAX_BATTLERS_COUNT] = {NULL};
EWRAM_DATA const u8 *gPalaceSelectionBattleScripts[MAX_BATTLERS_COUNT] = {NULL};
EWRAM_DATA const u8 *sBattleAnimScriptPtr = NULL;
EWRAM_DATA const u8 *sBattleAnimScriptRetAddr = NULL;
struct DisableStruct *gAnimDisableStructPtr;
u8 *gBattleAnimBgTileBuffer;
u8 *gBattleAnimBgTilemapBuffer;
struct BattleMsgData *gBattleMsgDataPtr;
struct BattleSpriteData *gBattleSpritesDataPtr;
void (*gBattleMainFunc)(void);
void (*gPreBattleCallback1)(void);
void (*gBattlerControllerFuncs[MAX_BATTLERS_COUNT])(void);
struct BattleHealthboxInfo *gBattleControllerOpponentHealthboxData;
struct BattleHealthboxInfo *gBattleControllerOpponentFlankHealthboxData;

/* TextPrinter state hooks (text.c). The harness has no live printers,
 * but the State-v5 EWRAM failure forensics iterate the printer table
 * (native_state.c SetRuntimePointerError), so the registry must return
 * real all-inactive storage instead of NULL (R13-H3: the first EWRAM
 * capture failure ever hit this path and dereferenced the old NULL). */
static struct TextPrinter sHarnessPrinters[WINDOWS_MAX];
const struct TextPrinter *TextPrinter_GetStatePrinters(void)
{
    return sHarnessPrinters;
}
void TextPrinter_DumpStateEvents(void) { }

/* Native-overworld sprite snapshot sink (uncommitted native_sprite_snapshot
 * workstream); inert in the harness. */
void NativeSpriteCommandSink_Invalidate(void) {}

/* ------------------------------------------------------------------ */
/* Task/sprite sidecars: empty implementations (size 0 sections).      */
u32 Task_GetStateSize(void) { return 0; }
bool32 Task_SaveState(void *dest, u32 size) { (void)dest; (void)size; return TRUE; }
bool32 Task_ValidateState(const void *source, u32 size) { (void)source; (void)size; return TRUE; }
bool32 Task_LoadState(const void *source, u32 size) { (void)source; (void)size; return TRUE; }
bool32 Task_GetStateFailure(u32 *offset, uintptr_t *address)
{
    (void)offset; (void)address;
    return FALSE;
}
u32 Sprite_GetStateSize(void) { return 0; }
bool32 Sprite_SaveState(void *dest, u32 size) { (void)dest; (void)size; return TRUE; }
bool32 Sprite_ValidateState(const void *source, u32 size) { (void)source; (void)size; return TRUE; }
bool32 Sprite_LoadState(const void *source, u32 size) { (void)source; (void)size; return TRUE; }
bool32 Sprite_GetStateFailure(u32 *offset, uintptr_t *address)
{
    (void)offset; (void)address;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Platform layer.                                                     */
extern char __executable_start[];
extern char _end[];

/* The harness "game" ranges: two static regions serialized as GAME_DATA and
 * GAME_BSS slices. The game-data region carries copies of REAL published
 * battle-table rows (real arena pointers into the pack-derived session
 * image); the game-bss region carries scalar game state. */
#define HARNESS_GAME_DATA_SIZE 0x1000
unsigned char sHarnessGameData[HARNESS_GAME_DATA_SIZE] __attribute__((aligned(8)));
/* R12: grown to hold the audio regression fixture - a real struct SoundInfo
 * (PORTABLE: ~40 KB with pcmBuffer) plus modeled player/track/jump-table
 * regions, exactly like the real build's game_bss audio span. */
unsigned char sHarnessGameBss[0x10000] __attribute__((aligned(8)));

bool32 Platform_RuntimeGetImageRange(uintptr_t *outStart, uintptr_t *outEnd)
{
    *outStart = (uintptr_t)__executable_start;
    *outEnd = (uintptr_t)_end;
    return TRUE;
}

bool32 Platform_RuntimeGetBssRange(uintptr_t *outStart, uintptr_t *outEnd)
{
    /* The harness ELF's ordinary bss is treated as host state, exactly like
     * the real linker script treats the executable's bss. */
    *outStart = (uintptr_t)__start_host_data;
    *outEnd = (uintptr_t)__stop_host_data;
    return TRUE;
}

bool32 Platform_RuntimeGetGameBssRange(uintptr_t *outStart, uintptr_t *outEnd)
{
    *outStart = (uintptr_t)sHarnessGameBss;
    *outEnd = (uintptr_t)sHarnessGameBss + sizeof(sHarnessGameBss);
    return TRUE;
}

bool32 Platform_RuntimeGetGameDataRange(uintptr_t *outStart, uintptr_t *outEnd)
{
    *outStart = (uintptr_t)sHarnessGameData;
    *outEnd = (uintptr_t)sHarnessGameData + sizeof(sHarnessGameData);
    return TRUE;
}

bool32 Platform_RuntimeAddressIsMapped(uintptr_t address)
{
    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page;
    unsigned char resident;

    if (pageSize <= 0)
        return FALSE;
    page = address & ~((uintptr_t)pageSize - 1);
    return mincore((void *)page, (size_t)pageSize, &resident) == 0;
}

bool32 Platform_RuntimeAddressIsExecutable(uintptr_t address)
{
    /* Honest executable-region model from /proc/self/maps: host_memory's
     * persistent-identity helpers require data pointers to be NON-executable
     * and function pointers to be executable, so the whole-image shortcut
     * would make every image data pointer unpersistable (the typed-sidecar
     * regression plants exactly such pointers). */
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[256];
    bool32 executable = FALSE;

    if (maps == NULL)
        return FALSE;
    while (fgets(line, sizeof(line), maps) != NULL)
    {
        unsigned long begin;
        unsigned long end;
        char permissions[5];

        if (sscanf(line, "%lx-%lx %4s", &begin, &end, permissions) == 3
         && address >= (uintptr_t)begin && address < (uintptr_t)end)
        {
            executable = (permissions[2] == 'x');
            break;
        }
    }
    fclose(maps);
    return executable;
}

bool32 Platform_RuntimeGetBuildId(const char *abiId, char *dest, u32 destSize)
{
    (void)abiId;
    snprintf(dest, destSize, "harness-v5-00000000");
    return TRUE;
}

static HOST_DATA u64 sHarnessFrameCounter = 0x5A5Aull;
u64 Platform_SchedulerGetFrameCounter(void) { return sHarnessFrameCounter; }
void Platform_SchedulerSetFrameCounter(u64 frame) { sHarnessFrameCounter = frame; }

/* Shims for the fake SDL surface (tests/emerald_audio_device_shim.c): the
 * real desktop_scheduler.c reads the SDL performance counter; the port
 * below must see the same controllable clock. */
extern uint64_t SDL_GetPerformanceCounter(void);
extern uint64_t SDL_GetPerformanceFrequency(void);

/* R10 desktop-load regression: exact port of the real desktop_scheduler.c
 * audio gate (delta accumulation over a 1/60 s frame, 0.25 s stall clamp),
 * reading the shim's controllable performance clock instead of the SDL
 * wall clock so the desktop sequence runs deterministically. The harness
 * advances the clock one 1/60 s tick per simulated VBlank; the gate then
 * reports exactly one due audio frame per tick (the real main loop's
 * behavior at 60 Hz presentation). */
bool32 Platform_SchedulerAudioFrameDue(void)
{
    const double audioFrameDuration = 1.0 / 60.0;
    u64 now = SDL_GetPerformanceCounter();
    u64 frequency = SDL_GetPerformanceFrequency();
    static HOST_DATA u64 sAudioClockCounter;
    static HOST_DATA double sAudioAccumulator;
    double delta;

    if (frequency == 0)
        return FALSE;
    delta = (double)(now - sAudioClockCounter) / (double)frequency;
    sAudioClockCounter = now;
    if (delta > 0.25)
        delta = audioFrameDuration;
    sAudioAccumulator += delta;
    if (sAudioAccumulator < audioFrameDuration)
        return FALSE;
    sAudioAccumulator -= audioFrameDuration;
    if (sAudioAccumulator >= audioFrameDuration)
        sAudioAccumulator = 0.0;
    return TRUE;
}

/* Settings: the production desktop_audio.c scales queued samples by the
 * volume setting; the harness pins it to full volume so QueueAudio payload
 * bytes are deterministic. */
u8 Platform_GetSetting(enum PlatformSetting setting)
{
    (void)setting;
    return 10;
}

void Platform_ClockCopyState(void *dest, u32 size)
{
    memset(dest, 0xCC, size);
}
bool32 Platform_ClockRestoreState(const void *source, u32 size)
{
    (void)source;
    (void)size;
    return TRUE; /* the real implementation validates the RTC payload */
}

static u8 sHarnessFramebuffer[DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(u32)];
void Platform_VideoCopyFramebuffer(void *dest, u32 size)
{
    memset(dest, 0x11, size);
}
void Platform_VideoRestoreFramebuffer(const void *source, u32 size)
{
    memcpy(sHarnessFramebuffer, source, size);
}

/* R10 desktop-load regression: the REAL src/platform/desktop_audio.c is
 * linked (against the fake SDL shim), so Platform_AudioInit/QueueAudio/
 * SetPaused/ClearQueue/Shutdown here are supplied by production code. The
 * shim's device model replaces the no-op stubs that predate the desktop
 * sequence harness (their behavior is now asserted, not ignored). */

/* Storage: real temp-file I/O (the harness never touches real user state). */
static bool32 ReadFile(const char *path, void *dest, u32 capacity, u32 *outSize)
{
    FILE *file = fopen(path, "rb");
    long length;
    if (file == NULL)
        return FALSE;
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return FALSE;
    }
    length = ftell(file);
    if (length < 0 || (u64)length > capacity || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return FALSE;
    }
    if (fread(dest, 1, (size_t)length, file) != (size_t)length)
    {
        fclose(file);
        return FALSE;
    }
    fclose(file);
    *outSize = (u32)length;
    return TRUE;
}

bool32 Platform_StorageReadFile(const char *path, void *dest, u32 capacity,
                                u32 *outSize)
{
    return ReadFile(path, dest, capacity, outSize);
}

bool32 Platform_StorageWriteAtomic(const char *path, const void *data, u32 size)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL)
        return FALSE;
    if (fwrite(data, 1, size, file) != size)
    {
        fclose(file);
        return FALSE;
    }
    fclose(file);
    return TRUE;
}

/* The slot-to-path mapping is a fixed harness name; the regression mode
 * (TEST R) runs alongside create/load in the same directory and must not
 * clobber the create state, so main() can override the path per process. */
static char sHarnessStatePathOverride[256];
static bool32 sHarnessStatePathOverridden = FALSE;

void HarnessStatePath_Override(const char *path)
{
    if (path != NULL)
    {
        snprintf(sHarnessStatePathOverride,
                 sizeof(sHarnessStatePathOverride), "%s", path);
        sHarnessStatePathOverridden = TRUE;
    }
}

bool32 Platform_ProfileGetStatePath(u8 slot, char *dest, u32 destSize)
{
    (void)slot;
    if (sHarnessStatePathOverridden)
        snprintf(dest, destSize, "%s", sHarnessStatePathOverride);
    else
        snprintf(dest, destSize, "harness-state-slot-%u.st", (unsigned)slot);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* R10 desktop-load regression: the REAL src/platform/desktop_state.c is
 * linked, so its platform surface is satisfied here. Every member is
 * audio-inert (profiles/metadata/thumbnails/storage); the harness needs
 * the real Platform_StateLoad wrapper (path -> existence -> metadata
 * guard -> NativeState_Load) to reproduce the desktop sequence verbatim,
 * with the guard hooks returning "no profile / no metadata" so the load
 * proceeds exactly like a normal in-session load. */

bool32 Platform_StorageFileExists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return FALSE;
    fclose(file);
    return TRUE;
}

bool32 Platform_StorageRemoveFile(const char *path)
{
    return remove(path) == 0;
}

const struct PlatformProfileMetadata *Platform_ProfileGetSelected(void)
{
    return NULL; /* no profile: the metadata profile guard is skipped */
}

bool32 Platform_ProfileGetStateFilePath(u8 slot, enum PlatformStateFileKind kind,
                                        char *dest, u32 destSize)
{
    (void)kind;
    return Platform_ProfileGetStatePath(slot, dest, destSize);
}

bool32 Platform_StateMetadataRead(u8 slot, struct PlatformStateMetadata *metadata)
{
    (void)slot;
    (void)metadata;
    return FALSE; /* no metadata companion: the profile guard is skipped */
}

void Platform_StateMetadataCapture(u8 slot, struct PlatformStateMetadata *metadata)
{
    (void)slot;
    memset(metadata, 0, sizeof(*metadata));
}

bool32 Platform_StateMetadataWrite(u8 slot,
                                   const struct PlatformStateMetadata *metadata)
{
    (void)slot;
    (void)metadata;
    return FALSE;
}

bool32 Platform_StateThumbnailCapture(u8 slot)
{
    (void)slot;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* R11-C: inert tileset-animation callbacks. The compat seam's generated
 * tileset structs reference the compiled InitTilesetAnim_* entry points
 * (which stay compiled in the real native build); the harness only links
 * the seam, so the callbacks are satisfied here as no-ops - the tests
 * assert pointer equality (.callback), never execution. */
void InitTilesetAnim_General(void) {}
void InitTilesetAnim_Petalburg(void) {}
void InitTilesetAnim_Rustboro(void) {}
void InitTilesetAnim_Dewford(void) {}
void InitTilesetAnim_Slateport(void) {}
void InitTilesetAnim_Mauville(void) {}
void InitTilesetAnim_MauvilleGym(void) {}
void InitTilesetAnim_Lavaridge(void) {}
void InitTilesetAnim_Fallarbor(void) {}
void InitTilesetAnim_Fortree(void) {}
void InitTilesetAnim_Lilycove(void) {}
void InitTilesetAnim_Mossdeep(void) {}
void InitTilesetAnim_Sootopolis(void) {}
void InitTilesetAnim_SootopolisGym(void) {}
void InitTilesetAnim_Pacifidlog(void) {}
void InitTilesetAnim_EverGrande(void) {}
void InitTilesetAnim_EliteFour(void) {}
void InitTilesetAnim_Building(void) {}
void InitTilesetAnim_BikeShop(void) {}
void InitTilesetAnim_Cave(void) {}
void InitTilesetAnim_BattlePyramid(void) {}
void InitTilesetAnim_BattleFrontierOutsideWest(void) {}
void InitTilesetAnim_BattleFrontierOutsideEast(void) {}
void InitTilesetAnim_BattleDome(void) {}
void InitTilesetAnim_Underwater(void) {}

/* NOTE (R11-E/F): the neighborhood's game fixtures (gSaveBlock1Ptr and the
 * Overworld accessor) are provided by emerald_native_world_overworld_stub.c
 * (the real PORTABLE-branch accessor over the compiled maps.o tables) --
 * see run_emerald_resource_state.sh. The module is linked because the real
 * native_state.c restore path calls NativeWorldNeighborhood_Invalidate; the
 * Harness C proofs (tests 15/16) live in emerald_resource_state_test.c. */
