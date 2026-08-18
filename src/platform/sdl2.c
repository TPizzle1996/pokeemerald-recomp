#ifdef PLATFORM_SDL2
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#endif

#ifdef __ANDROID__
#include <jni.h>
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#if defined(NATIVE_LINUX) || defined(_WIN32)
#include <SDL2/SDL_image.h>
#endif

#if defined(__GLIBC__)
#include <malloc.h> /* mallinfo2: allocation-free Republish probe (R7B Stage 9) */
#endif

#include "global.h"
#include "platform.h"
#include "rtc.h"
#include "item.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "cgb_audio.h"
#include "gba/flash_internal.h"
#include "gba/syscall.h"
#include "data.h"
#include "constants/trainers.h"
#include "platform/dma.h"
#include "platform/framedraw.h"
#include "platform/native_overworld_parity.h"
#include "platform/desktop_config.h"
#include "platform/desktop_clock.h"
#include "platform/desktop_audio.h"
#include "platform/desktop_input.h"
#include "platform/desktop_runtime.h"
#include "platform/desktop_scheduler.h"
#include "platform/desktop_video.h"
#include "platform/desktop_storage.h"
#include "platform/desktop_profiles.h"
#include "platform/desktop_frontend.h"
#include "platform/desktop_game_content.h"
#include "platform/desktop_state.h"
#include "platform/desktop_state_ui.h"
#include "platform/native_state.h"
#include "platform/desktop_assets.h"
#include "platform/host_memory.h"
#include "m4a.h"
#include "emerald/resources/emerald_trainer_native_compat.h"

HOST_DATA bool speedUp = false;
HOST_DATA bool isRunning = true;
HOST_DATA bool paused = false;
HOST_DATA double simTime = 0;
HOST_DATA double lastGameTime = 0;
HOST_DATA double curGameTime = 0;
HOST_DATA double fixedTimestep = 1.0 / 60.0; // 16.666667ms
HOST_DATA double timeScale = 1.0;

extern void DoSoftReset(void);

enum NativeStateRequest
{
    NATIVE_STATE_REQUEST_NONE,
    NATIVE_STATE_REQUEST_SAVE,
    NATIVE_STATE_REQUEST_LOAD,
};

static void PrepareHostFrame(enum NativeStateRequest request, u8 slot,
                             double *accumulator, u64 *lastPresentationCounter)
{
    enum NativeStateResult result;
    const char *reason;
    char status[256];

    if (request == NATIVE_STATE_REQUEST_LOAD)
    {
        result = Platform_StateLoad(slot) == PLATFORM_STATE_OPERATION_OK
            ? NATIVE_STATE_OK : NATIVE_STATE_UNAVAILABLE;
        if (result == NATIVE_STATE_OK)
        {
            char path[1024];
            Platform_VideoSetStatus("State loaded");
            DBGPRINTF("Native state loaded (slot %u)\n", slot);
            if (!Platform_ProfileGetStatePath(slot, path, sizeof(path)))
                snprintf(path, sizeof(path), "<unknown>");
            fprintf(stderr, "Native state loaded (slot %u): %s\n", slot, path);
            fflush(stderr);
            *accumulator = 0.0;
            *lastPresentationCounter = 0;
            return;
        }
        reason = Platform_StateGetLastError();
        if (reason == NULL || reason[0] == '\0')
            reason = "unknown serializer error";
        snprintf(status, sizeof(status), "State load failed: %s", reason);
        Platform_VideoSetStatus(status);
        fprintf(stderr, "Native state load failed (slot %u): %s\n", slot, reason);
        fflush(stderr);
        DBGPRINTF("Native state load failed (slot %u): %s\n", slot, reason);
    }

    Platform_VideoDrawFrame();
    if (request == NATIVE_STATE_REQUEST_SAVE)
    {
        {
            enum PlatformStateOperationResult operationResult = Platform_StateSave(slot);
            result = operationResult == PLATFORM_STATE_OPERATION_FAILED
                ? NATIVE_STATE_UNAVAILABLE : NATIVE_STATE_OK;
        }
        if (result == NATIVE_STATE_OK)
        {
            char path[1024];
            reason = Platform_StateGetLastError();
            Platform_VideoSetStatus(reason != NULL && reason[0] != '\0'
                                  ? reason : "State saved");
            DBGPRINTF("Native state saved (slot %u)\n", slot);
            if (!Platform_ProfileGetStatePath(slot, path, sizeof(path)))
                snprintf(path, sizeof(path), "<unknown>");
            fprintf(stderr, "Native state saved (slot %u): %s\n", slot, path);
            fflush(stderr);
        }
        else
        {
            reason = Platform_StateGetLastError();
            if (reason == NULL || reason[0] == '\0')
                reason = "unknown serializer error";
            snprintf(status, sizeof(status), "State save failed: %s", reason);
            Platform_VideoSetStatus(status);
            fprintf(stderr, "Native state save failed (slot %u): %s\n", slot, reason);
            fflush(stderr);
            DBGPRINTF("Native state save failed (slot %u): %s\n", slot, reason);
        }
    }
}

static enum PlatformStateUiResult RunStateUi(bool32 manager,
                                              double *accumulator,
                                              u64 *lastPresentationCounter)
{
    bool32 wasPaused = paused;
    enum PlatformStateUiResult result;

    if (!Platform_SchedulerWaitForFrame(1000))
    {
        Platform_VideoSetStatus("Unable to pause game for save-state menu");
        return PLATFORM_STATE_UI_CLOSED;
    }
    /* The worker is blocked at VBlank here. Draw and retain that exact logical
     * frame before the host UI replaces the renderer output. */
    Platform_VideoDrawFrame();
    paused = TRUE;
    Platform_AudioSetPaused(TRUE);
    result = manager ? Platform_StateUiRunManager() : Platform_StateUiRunSavePicker();
    Platform_VideoRenderFramebuffer();
    paused = wasPaused;
    if (!paused)
    {
        Platform_AudioClearQueue();
        Platform_AudioSetPaused(FALSE);
    }
    else
        Platform_AudioSetPaused(TRUE);
    *accumulator = 0.0;
    *lastPresentationCounter = 0;
    return result;
}

static bool32 EnvironmentEnabled(const char *name)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static u32 EnvironmentUnsigned(const char *name, u32 fallback)
{
    const char *value = getenv(name);
    char *end;
    unsigned long parsed;
    if (value == NULL || value[0] == '\0')
        return fallback;
    parsed = strtoul(value, &end, 10);
    if (*end != '\0' || parsed > UINT32_MAX)
        return fallback;
    return (u32)parsed;
}

static void PacePresentation(u64 *deadline)
{
    u64 frequency = SDL_GetPerformanceFrequency();
    u64 interval;
    u64 now;
    if (frequency == 0)
        return;
    interval = frequency / 60;
    now = SDL_GetPerformanceCounter();
    if (*deadline == 0 || now > *deadline + interval * 4)
        *deadline = now;
    *deadline += interval;
    while ((now = SDL_GetPerformanceCounter()) < *deadline)
    {
        u64 remaining = *deadline - now;
        u32 delay = (u32)(remaining * 1000 / frequency);
        SDL_Delay(delay > 1 ? delay - 1 : 0);
    }
}

#if defined(NATIVE_LINUX) || defined(WINDOWS64)
static void HandleNativeDebugActions(const struct PlatformInputActions *input)
{
    if (input->debugAddRareCandies && Platform_SchedulerWaitForFrame(1000))
    {
        /* Temporary native-only developer/testing shortcut. Keep the grant on
         * the normal item API and execute it while the game thread is parked
         * at its VBlank boundary. */
        if (AddBagItem(ITEM_RARE_CANDY, 99) == TRUE)
        {
            printf("Debug: added 99 Rare Candies\n");
            fflush(stdout);
        }
    }
}
#endif

#ifdef __ANDROID__
void Platform_HandleTouchEvent(const SDL_TouchFingerEvent *event);
static void DrawTouchControls(void);
#endif

#if defined(NATIVE_LINUX) || defined(_WIN32)
static bool32 NativeStateFileContainsPointer(u8 slot, const void *pointer)
{
    char path[1024];
    FILE *file;
    long fileSize;
    u8 *bytes;
    uintptr_t value = (uintptr_t)pointer;
    long i;
    bool32 found = FALSE;

    if (!Platform_ProfileGetStatePath(slot, path, sizeof(path)))
        return TRUE;
    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
    {
        if (file != NULL)
            fclose(file);
        return TRUE;
    }
    fileSize = ftell(file);
    if (fileSize < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return TRUE;
    }
    bytes = malloc((size_t)fileSize);
    if (bytes == NULL || fread(bytes, 1, (size_t)fileSize, file) != (size_t)fileSize)
        found = TRUE;
    else
    {
        for (i = 0; i + (long)sizeof(value) <= fileSize; i++)
        {
            uintptr_t candidate;
            memcpy(&candidate, bytes + i, sizeof(candidate));
            if (candidate == value)
            {
                found = TRUE;
                break;
            }
        }
    }
    free(bytes);
    fclose(file);
    return found;
}

static bool32 NativeStatePointerInGameRange(const void *pointer)
{
    uintptr_t address = (uintptr_t)pointer;
    uintptr_t start;
    uintptr_t end;

    return (Platform_RuntimeGetGameBssRange(&start, &end)
         && address >= start && address < end)
        || (Platform_RuntimeGetGameDataRange(&start, &end)
         && address >= start && address < end);
}
#endif

/* R7B/R8 Stage 9: trainer-family republish across a save/load round trip.
 *
 * The trainer tables themselves are plain native .data (no gba_data section
 * attribute - verified against the built binary: they are outside the
 * game_data slice), so a save state does not serialize them; the tables keep
 * whatever process-local values they hold across a save/load round trip, and
 * the load path re-publishes the current-session image unconditionally
 * (native_state.c) or fails closed to the NULL sentinel. Whatever the tables
 * contain after a load - restored game state, untouched corruption, or the
 * NULL sentinels of a fresh process - no stale or foreign process-local
 * compat-stream pointer can survive. This probe proves that end to end in
 * the real binary:
 *
 *   1. register the production ROM_BASE snapshot exactly like the live
 *      startup path (pack resolved via the asset-path helper, CWD fallback)
 *      and initialize the seam, publishing all 236 migrated slots (93 front
 *      sheets + 93 front palettes + 6 shared back-palette slots + R8: the 8
 *      back sheet slots, the 34 back SpriteFrameImage slots and the 2
 *      Red/Leaf back-palette slots);
 *   2. capture a baseline of every slot (data/size/tag);
 *   3. corrupt every migrated data pointer with a distinct garbage value and
 *      save + load, so the load path MUST repair tables that hold garbage;
 *   4. the load-path republish must repair every slot to its baseline stream
 *      and leave size/tag untouched;
 *   5. probe Republish directly under glibc heap accounting to pin the
 *      allocation-free contract, and decode sample streams through the real
 *      decompressor to prove the repaired pointers are live.
 *
 * Returns 1 on success, 0 on failure (message on stderr).
 */
#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)
static int NativeStateTrainerFamilyTest(void)
{
    static const u8 sharedBackPalSlots[] = {
        TRAINER_BACK_PIC_BRENDAN,
        TRAINER_BACK_PIC_MAY,
        TRAINER_BACK_PIC_RUBY_SAPPHIRE_BRENDAN,
        TRAINER_BACK_PIC_RUBY_SAPPHIRE_MAY,
        TRAINER_BACK_PIC_WALLY,
        TRAINER_BACK_PIC_STEVEN,
    };
    /* R8 back-family mapping: the 8 sheet slots in TRAINER_BACK_PIC_* order,
     * the per-trainer frame counts (mirror the R8 family descriptor; the unit
     * harness pins these counts against the compiled frame arrays, so they
     * cannot drift), and the frame-table pointers the seam publishes into -
     * the live pixel surface of the back-pic sprite pipeline. */
    static const u8 backFrameCounts[TRAINER_BACK_PIC_STEVEN + 1] = {
        4, 4, 5, 5, 4, 4, 4, 4,
    };
    static struct SpriteFrameImage *const backFrameTables[TRAINER_BACK_PIC_STEVEN + 1] = {
        gTrainerBackPicTable_Brendan,
        gTrainerBackPicTable_May,
        gTrainerBackPicTable_Red,
        gTrainerBackPicTable_Leaf,
        gTrainerBackPicTable_RubySapphireBrendan,
        gTrainerBackPicTable_RubySapphireMay,
        gTrainerBackPicTable_Wally,
        gTrainerBackPicTable_Steven,
    };
    struct CompressedSpriteSheet sheetBase[EMERALD_TRAINER_FRONT_COUNT];
    struct CompressedSpritePalette palBase[EMERALD_TRAINER_FRONT_COUNT];
    struct CompressedSpritePalette backPalBase[TRAINER_BACK_PIC_STEVEN + 1];
    struct CompressedSpriteSheet backSheetBase[TRAINER_BACK_PIC_STEVEN + 1];
    struct SpriteFrameImage backFrameBase[TRAINER_BACK_PIC_STEVEN + 1][5]; /* max frames */
    struct EmeraldResourceCompatDiagnostics diagnostics;
    enum EmeraldResourceCompatStatus status;
    char packPath[1024];
    u32 corruption;
    u32 i;
    u32 f;

    /* 1. Live-session setup: same pack resolution and init order as the
     * content-hydration path (desktop_game_content.c). */
    if (!Platform_AssetGetPath("games/emerald/base/emerald-bpee01-v1.rpack",
                               packPath, sizeof(packPath)))
    {
        fprintf(stderr, "Native state self-test: production pack not resolvable "
                        "(run from the repo root)\n");
        return 0;
    }
    status = EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath);
    if (status != EMERALD_COMPAT_OK)
    {
        fprintf(stderr, "Native state self-test: runtime snapshot registration failed\n");
        return 0;
    }
    EmeraldResourceCompat_TryInitialize();
    if (EmeraldResourceCompat_Republish(&diagnostics) != EMERALD_COMPAT_OK)
    {
        fprintf(stderr, "Native state self-test: no valid session image after init\n");
        return 0;
    }

    /* 2. Baseline: everything the state-load republish may rewrite. */
    memcpy(sheetBase, gTrainerFrontPicTable, sizeof(sheetBase));
    memcpy(palBase, gTrainerFrontPicPaletteTable, sizeof(palBase));
    memcpy(backPalBase, gTrainerBackPicPaletteTable, sizeof(backPalBase));
    memcpy(backSheetBase, gTrainerBackPicTable, sizeof(backSheetBase));
    for (i = 0; i < ARRAY_COUNT(backFrameBase); i++)
    {
        for (f = 0; f < backFrameCounts[i]; f++)
            backFrameBase[i][f] = backFrameTables[i][f];
    }

    /* 3. Corrupt every migrated data pointer with a distinct garbage value
     * and save: the state file must carry the stale pointers verbatim. */
    corruption = 0xDEAD0000u;
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        gTrainerFrontPicTable[i].data = (const u32 *)(unsigned long)corruption++;
        gTrainerFrontPicPaletteTable[i].data = (const u32 *)(unsigned long)corruption++;
    }
    for (i = 0; i < ARRAY_COUNT(sharedBackPalSlots); i++)
    {
        gTrainerBackPicPaletteTable[sharedBackPalSlots[i]].data =
            (const u32 *)(unsigned long)corruption++;
    }
    for (i = 0; i < ARRAY_COUNT(backSheetBase); i++)
        gTrainerBackPicTable[i].data = (const u32 *)(unsigned long)corruption++;
    for (i = 0; i < ARRAY_COUNT(backFrameBase); i++)
    {
        for (f = 0; f < backFrameCounts[i]; f++)
            backFrameTables[i][f].data = (const void *)(unsigned long)corruption++;
    }
    /* R8: the Red/Leaf back-palette slots are migrated too - corrupt them
     * like the six shared slots. */
    gTrainerBackPicPaletteTable[TRAINER_BACK_PIC_RED].data =
        (const u32 *)(unsigned long)corruption++;
    gTrainerBackPicPaletteTable[TRAINER_BACK_PIC_LEAF].data =
        (const u32 *)(unsigned long)corruption++;
    if (Platform_StateSave(PLATFORM_STATE_QUICK_SLOT) == PLATFORM_STATE_OPERATION_FAILED)
    {
        fprintf(stderr, "Native state self-test: family-corrupt save failed: %s\n",
                Platform_StateGetLastError());
        return 0;
    }

    /* 4. Load back: the load-path republish must repair every migrated slot
     * to its baseline stream, and leave size/tag and the non-migrated
     * Red/Leaf back-palette slots untouched. */
    if (Platform_StateLoad(PLATFORM_STATE_QUICK_SLOT) != PLATFORM_STATE_OPERATION_OK)
    {
        fprintf(stderr, "Native state self-test: family state load failed: %s\n",
                Platform_StateGetLastError());
        return 0;
    }
    for (i = 0; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        if (gTrainerFrontPicTable[i].data != sheetBase[i].data
         || gTrainerFrontPicTable[i].size != sheetBase[i].size
         || gTrainerFrontPicTable[i].tag != sheetBase[i].tag)
        {
            fprintf(stderr, "Native state self-test: front sheet slot %u not repaired after load\n", i);
            return 0;
        }
        if (gTrainerFrontPicPaletteTable[i].data != palBase[i].data
         || gTrainerFrontPicPaletteTable[i].tag != palBase[i].tag)
        {
            fprintf(stderr, "Native state self-test: front palette slot %u not repaired after load\n", i);
            return 0;
        }
    }
    for (i = 0; i < ARRAY_COUNT(sharedBackPalSlots); i++)
    {
        u8 slot = sharedBackPalSlots[i];
        if (gTrainerBackPicPaletteTable[slot].data != backPalBase[slot].data
         || gTrainerBackPicPaletteTable[slot].tag != backPalBase[slot].tag)
        {
            fprintf(stderr, "Native state self-test: back palette slot %u not repaired after load\n", slot);
            return 0;
        }
    }
    if (gTrainerBackPicPaletteTable[TRAINER_BACK_PIC_RED].data != backPalBase[TRAINER_BACK_PIC_RED].data
     || gTrainerBackPicPaletteTable[TRAINER_BACK_PIC_RED].tag != backPalBase[TRAINER_BACK_PIC_RED].tag
     || gTrainerBackPicPaletteTable[TRAINER_BACK_PIC_LEAF].data != backPalBase[TRAINER_BACK_PIC_LEAF].data
     || gTrainerBackPicPaletteTable[TRAINER_BACK_PIC_LEAF].tag != backPalBase[TRAINER_BACK_PIC_LEAF].tag)
    {
        fprintf(stderr, "Native state self-test: Red/Leaf back palette slots not repaired after load\n");
        return 0;
    }
    for (i = 0; i < ARRAY_COUNT(backSheetBase); i++)
    {
        if (gTrainerBackPicTable[i].data != backSheetBase[i].data
         || gTrainerBackPicTable[i].size != backSheetBase[i].size
         || gTrainerBackPicTable[i].tag != backSheetBase[i].tag)
        {
            fprintf(stderr, "Native state self-test: back sheet slot %u not repaired after load\n", i);
            return 0;
        }
    }
    for (i = 0; i < ARRAY_COUNT(backFrameBase); i++)
    {
        for (f = 0; f < backFrameCounts[i]; f++)
        {
            if (backFrameTables[i][f].data != backFrameBase[i][f].data
             || backFrameTables[i][f].size != backFrameBase[i][f].size)
            {
                fprintf(stderr, "Native state self-test: back frame slot %u/%u not repaired after load\n",
                        i, f);
                return 0;
            }
        }
    }

    /* 5. Republish probe: idempotent, allocation-free, and the repaired
     * streams decode through the real decompressor (the LZ77 header's
     * declared length must match the table's decoded size; palettes are
     * 32 bytes). */
#if defined(__GLIBC__)
    {
        struct mallinfo2 before = mallinfo2();
        status = EmeraldResourceCompat_Republish(&diagnostics);
        if (status != EMERALD_COMPAT_OK)
        {
            fprintf(stderr, "Native state self-test: republish probe failed\n");
            return 0;
        }
        if (before.uordblks != mallinfo2().uordblks)
        {
            fprintf(stderr, "Native state self-test: republish allocated heap memory\n");
            return 0;
        }
    }
#endif
    {
        static const u8 decodeSamples[] = {0, 1, 92}; /* first, second, last trainer */
        u8 decoded[4096 + 16]; /* max sheet decode = TRAINER_PIC_SIZE * 2 */
        for (i = 0; i < ARRAY_COUNT(decodeSamples); i++)
        {
            const u8 *stream =
                (const u8 *)(const void *)gTrainerFrontPicTable[decodeSamples[i]].data;
            u32 declared;
            if (stream == NULL)
            {
                fprintf(stderr, "Native state self-test: front sheet stream %u NULL after load\n",
                        decodeSamples[i]);
                return 0;
            }
            /* The literal-only codec stores the decoded size in bytes 1-3 of
             * the header (the real decompressor reads header >> 8). */
            declared = ((u32)stream[3] << 16) | ((u32)stream[2] << 8) | stream[1];
            if (declared != gTrainerFrontPicTable[decodeSamples[i]].size
             || declared > sizeof(decoded)
             || declared % TRAINER_PIC_SIZE != 0)
            {
                fprintf(stderr, "Native state self-test: front sheet stream %u invalid after load\n",
                        decodeSamples[i]);
                return 0;
            }
            LZ77UnCompWram((const u32 *)(const void *)stream, decoded);
        }
        for (i = 0; i < ARRAY_COUNT(sharedBackPalSlots); i++)
        {
            const u8 *stream =
                (const u8 *)(const void *)gTrainerBackPicPaletteTable[sharedBackPalSlots[i]].data;
            u32 declared;
            if (stream == NULL)
            {
                fprintf(stderr, "Native state self-test: back palette stream %u NULL after load\n",
                        sharedBackPalSlots[i]);
                return 0;
            }
            declared = ((u32)stream[3] << 16) | ((u32)stream[2] << 8) | stream[1];
            if (declared != 32 || declared > sizeof(decoded))
            {
                fprintf(stderr, "Native state self-test: back palette stream %u invalid after load\n",
                        sharedBackPalSlots[i]);
                return 0;
            }
            LZ77UnCompWram((const u32 *)(const void *)stream, decoded);
        }
        /* R8: the Red/Leaf back-only palette streams are LZ-encoded like the
         * shared slots, and the back SHEET streams are RAW (no LZ header) -
         * frame 0's pointer must be the sheet stream start (the sprite
         * pipeline's read path), proving the repaired raw publishing. */
        {
            static const u8 backOnlyPalSlots[] = {
                TRAINER_BACK_PIC_RED,
                TRAINER_BACK_PIC_LEAF,
            };
            for (i = 0; i < ARRAY_COUNT(backOnlyPalSlots); i++)
            {
                const u8 *stream =
                    (const u8 *)(const void *)gTrainerBackPicPaletteTable[backOnlyPalSlots[i]].data;
                u32 declared;
                if (stream == NULL)
                {
                    fprintf(stderr, "Native state self-test: back-only palette stream %u NULL after load\n",
                            backOnlyPalSlots[i]);
                    return 0;
                }
                declared = ((u32)stream[3] << 16) | ((u32)stream[2] << 8) | stream[1];
                if (declared != 32 || declared > sizeof(decoded))
                {
                    fprintf(stderr, "Native state self-test: back-only palette stream %u invalid after load\n",
                            backOnlyPalSlots[i]);
                    return 0;
                }
                LZ77UnCompWram((const u32 *)(const void *)stream, decoded);
            }
        }
        {
            const u8 *sheet =
                (const u8 *)(const void *)gTrainerBackPicTable[TRAINER_BACK_PIC_BRENDAN].data;
            if (sheet == NULL
             || (const u8 *)(const void *)gTrainerBackPicTable_Brendan[0].data != sheet
             || gTrainerBackPicTable_Brendan[0].size != TRAINER_PIC_SIZE)
            {
                fprintf(stderr, "Native state self-test: back sheet stream invalid after load\n");
                return 0;
            }
        }
    }
    return 1;
}

extern void RunMixerFrame(void);

/* R12 (headless audio/state): the real MP2K engine round-trip.
 *
 * Runs entirely without SDL video/audio (utility mode: SDL_Init(0)) and
 * without the game main loop. It initializes the REAL m4a engine, starts a
 * REAL song from the embedded ROM tables (mus_route101, 8 tracks), warms the
 * REAL mixer for a fixed number of frames, then proves that a State v5
 * save/perturb/load cycle restores the audio state byte-for-byte and that a
 * further run of mixer frames produces output identical to a control path
 * that never saved:
 *
 *   1. init invariants: ident == ID_NUMBER, player/track/channel chains live
 *   2. a started song populates channels and the PCM buffer (nonzero audio)
 *   3. every audio pointer field re-derives to its original target after load
 *   4. ordinary scalar audio bytes that numerically resemble host/resource
 *      pointers (u64 pairs in the external-host-pointer range, large u32
 *      counters) remain ordinary data
 *   5. post-restore mixer frames are byte-identical to the control
 *
 * The cgb emulator state (struct AudioCGB, host .bss) is not part of any
 * state slice, exactly like in the real game: cgb_audio_init() resets it, so
 * both the control and the test path re-run it before their mixer frames and
 * see identical emulator trajectories (a state load must NOT reset it - the
 * real engine never does).
 */
/* R12-C §8: post-load sample pointers may legitimately point into the audio
 * arena (above 4 GiB): the range index's registered spans cover the
 * verbatim + transformed zones and the hull covers the whole arena
 * allocation, so a resource-resident pointer is exactly as legitimate as an
 * image-resident one. Anything above 4 GiB that is neither is a defect -
 * the walker would have refused the save instead of persisting it. */
static bool32 NativeAudioPointerIsResourceResident(uintptr_t address)
{
    const struct EmeraldResourceRangeIndex *index =
        EmeraldResourceCompat_GetRangeIndex();
    struct EmeraldResourceRangeHit hit;

    if (index == NULL)
        return FALSE;
    return EmeraldResourceRangeIndex_Lookup(index, address, &hit)
        || EmeraldResourceRangeIndex_InHull(index, address);
}

static int NativeStateAudioSelfTest(void)
{
    /* R12-C: run against the redirected path. Same pack resolution and init
     * order as the content-hydration path (desktop_game_content.c): the
     * session publishes the audio arena, the logical-address table and the
     * R10 range index, so the song tone hydrates into the arena's
     * transformed rows, channels hold arena sample pointers, and the walker
     * captures all of them as sidecar records on the mid-play save below. */
    {
        char packPath[1024];
        if (!Platform_AssetGetPath("games/emerald/base/emerald-bpee01-v1.rpack",
                                   packPath, sizeof(packPath))
         || EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath)
                != EMERALD_COMPAT_OK)
        {
            fprintf(stderr, "Native audio self-test: production pack not "
                            "resolvable/registrable\n");
            return 1;
        }
    }

    uintptr_t bssStart;
    uintptr_t bssEnd;
    unsigned char *region;
    size_t regionSize;
    unsigned char *snapshot;
    unsigned char *expected;
    unsigned char *control;
    unsigned char *after;
    struct SoundInfo *soundInfo = &gSoundInfo;
    struct MusicPlayerInfo *player;
    struct SoundChannel *ch;
    u32 i;
    int frame;
    uintptr_t chansStart = (uintptr_t)(void *)soundInfo->chans;
    uintptr_t chansEnd = chansStart + sizeof(soundInfo->chans);
    uintptr_t playerAddrs[MAX_MUSIC_PLAYERS + MAX_POKEMON_CRIES];
    u32 expectedPlayers;
    u32 playersSeen = 0;
    bool32 playingPlayer = FALSE;

    if (!Platform_RuntimeGetGameBssRange(&bssStart, &bssEnd)
     || bssEnd <= bssStart || bssEnd - bssStart > 64u * 1024u * 1024u)
    {
        fprintf(stderr, "Native audio self-test: no game_bss range\n");
        return 1;
    }
    region = (unsigned char *)(uintptr_t)bssStart;
    regionSize = (size_t)(bssEnd - bssStart);
    if ((uintptr_t)soundInfo < bssStart
     || (uintptr_t)soundInfo + sizeof(*soundInfo) > bssEnd)
    {
        fprintf(stderr, "Native audio self-test: gSoundInfo outside game_bss\n");
        return 1;
    }
    snapshot = malloc(regionSize);
    expected = malloc(regionSize);
    control = malloc(regionSize);
    after = malloc(regionSize);
    if (snapshot == NULL || expected == NULL || control == NULL || after == NULL)
    {
        fprintf(stderr, "Native audio self-test: allocation failed\n");
        return 1;
    }

    if (!Platform_ProfileInit()
     || !Platform_ProfileLoadSelectedSave(FLASH_BASE, sizeof(FLASH_BASE)))
    {
        fprintf(stderr, "Native audio self-test: profile initialization failed\n");
        return 1;
    }

    /* The real main loop calls these once at startup; the mixer and the cgb
     * emulator need no SDL device (Platform_AudioInit is presentation-only). */
    cgb_audio_init(42060);
    m4aSoundInit();

    /* Start a real BGM (mus_route101: 8 tracks, sustained notes) and warm the
     * mixer far enough to populate channels, envelopes and the PCM buffers. */
    m4aSongNumStart(359);
    for (frame = 0; frame < 40; frame++)
        RunMixerFrame();

    /* 1. init invariants. */
    if (soundInfo->ident != ID_NUMBER)
    {
        fprintf(stderr, "Native audio self-test: ident 0x%08x != ID_NUMBER after init\n",
                soundInfo->ident);
        return 1;
    }
    if (soundInfo->musicPlayerHead == NULL
     || soundInfo->MPlayMainHead == NULL
     || soundInfo->cgbChans == NULL
     || soundInfo->MPlayJumpTable == NULL)
    {
        fprintf(stderr, "Native audio self-test: gSoundInfo pointer fields unset after init\n");
        return 1;
    }

    /* The chain head is the LAST-opened player: m4aSoundInit opens the four
     * main players then the two cry players, and MPlayOpen prepends. */
    playerAddrs[0] = (uintptr_t)HostResolveGbaAddr(gMPlayTable[0].info);
    playerAddrs[1] = (uintptr_t)HostResolveGbaAddr(gMPlayTable[1].info);
    playerAddrs[2] = (uintptr_t)HostResolveGbaAddr(gMPlayTable[2].info);
    playerAddrs[3] = (uintptr_t)HostResolveGbaAddr(gMPlayTable[3].info);
    playerAddrs[4] = (uintptr_t)gPokemonCryMusicPlayers;
    playerAddrs[5] = (uintptr_t)(gPokemonCryMusicPlayers + 1);
    expectedPlayers = MAX_MUSIC_PLAYERS + MAX_POKEMON_CRIES;
    if (soundInfo->musicPlayerHead != (struct MusicPlayerInfo *)playerAddrs[5])
    {
        fprintf(stderr, "Native audio self-test: musicPlayerHead chain root mismatch "
                        "(got %p want %p)\n",
                (void *)soundInfo->musicPlayerHead, (void *)playerAddrs[5]);
        return 1;
    }

    /* 2. a started song populated channels and PCM. */
    {
        bool32 anyChannelActive = FALSE;
        bool32 anyPcmNonzero = FALSE;

        for (i = 0; i < MAX_DIRECTSOUND_CHANNELS; i++)
        {
            ch = &soundInfo->chans[i];
            if ((ch->statusFlags & 0xC7) != 0 && ch->envelopeVolume != 0)
                anyChannelActive = TRUE;
        }
        for (i = 0; i < sizeof(soundInfo->pcmBuffer) / sizeof(float); i += 97)
        {
            if (soundInfo->pcmBuffer[i] != 0.0f)
            {
                anyPcmNonzero = TRUE;
                break;
            }
        }
        if (!anyChannelActive || !anyPcmNonzero)
        {
            fprintf(stderr, "Native audio self-test: song %u produced no audio "
                            "(channels active=%d pcm nonzero=%d)\n",
                    359u, anyChannelActive, anyPcmNonzero);
            return 1;
        }
    }
    fprintf(stdout, "Native audio self-test: engine initialized, song active, PCM nonzero\n");

    /* Snapshot the walked audio state before save. */
    memcpy(snapshot, region, regionSize);

    /* CONTROL: tick 40 more frames from the pristine state and record the
     * resulting bytes without ever saving. cgb_audio_init() resets the host
     * cgb emulator so the test path below sees the same trajectory. */
    memcpy(region, snapshot, regionSize);
    cgb_audio_init(42060);
    for (frame = 0; frame < 40; frame++)
        RunMixerFrame();
    memcpy(control, region, regionSize);

    /* PHASE A - scalar round-trip. Re-establish the pristine state, plant
     * scalar bytes that numerically resemble pointers, save, destroy, load,
     * and require the restored span to equal the pre-save snapshot. The
     * plants are exactly the walker's adversarial classes:
     *
     *   - two float values 2^64 (0x5F800000) as u64 pairs land inside the
     *     external-host-pointer range [0x500000000000, 0x800000000000) but
     *     are NOT mincore-mapped, so they must round-trip as data;
     *   - a large u32 (blockCount = 0x6D53A5A5) is below 4 GiB but outside
     *     every image range, so it must round-trip as data.
     *
     * No mixer frames run here, so the cgb emulator stays put. */
    memcpy(region, snapshot, regionSize);
    {
        float *pb = soundInfo->pcmBuffer;
        u32 *gap = (u32 *)soundInfo->gap2;      /* 16 untagged bytes */
        u32 *counter = &soundInfo->chans[0].blockCount;

        pb[3] = (float)(1ull << 63); pb[4] = (float)(1ull << 63);
        pb[5] = (float)(1ull << 63);  pb[6] = (float)(1ull << 63);
        gap[0] = 0x5F800000u;
        gap[1] = 0x5F800000u;
        *counter = 0x6D53A5A5u;
    }
    memcpy(expected, region, regionSize);
    if (Platform_StateSave(PLATFORM_STATE_QUICK_SLOT) == PLATFORM_STATE_OPERATION_FAILED)
    {
        fprintf(stderr, "Native audio self-test: save failed: %s\n",
                Platform_StateGetLastError());
        return 1;
    }

    /* Perturb: destroy the whole walked audio span (including every pointer
     * field) so any restore defect cannot hide behind the pre-save bytes. */
    memset(region, 0xA5, regionSize);
    soundInfo->MPlayMainHead = NULL;
    soundInfo->musicPlayerHead = NULL;
    soundInfo->MPlayJumpTable = NULL;
    soundInfo->cgbChans = NULL;
    for (i = 0; i < MAX_DIRECTSOUND_CHANNELS; i++)
    {
        ch = &soundInfo->chans[i];
        ch->wav = (struct WaveData *)(uintptr_t)0xDEADBEEF;
        ch->currentPointer = (s8 *)(uintptr_t)0xDEADBEEF;
        ch->prevChannelPointer = (void *)(uintptr_t)0xDEADBEEF;
        ch->nextChannelPointer = (void *)(uintptr_t)0xDEADBEEF;
    }

    if (Platform_StateLoad(PLATFORM_STATE_QUICK_SLOT) != PLATFORM_STATE_OPERATION_OK)
    {
        fprintf(stderr, "Native audio self-test: load failed: %s\n",
                Platform_StateGetLastError());
        return 1;
    }

    /* The restored span must equal the pre-save snapshot (plants included):
     * scalar audio bytes that resemble pointers round-tripped as data. */
    if (memcmp(region, expected, regionSize) != 0)
    {
        size_t first;
        for (first = 0; first < regionSize; first++)
        {
            if (region[first] != expected[first])
                break;
        }
        fprintf(stderr, "Native audio self-test: audio state diverges after load "
                        "(game_bss+0x%zx: got 0x%02x want 0x%02x)\n",
                first, region[first], expected[first]);
        return 1;
    }
    fprintf(stdout, "Native audio self-test: restored audio span byte-identical to snapshot\n");

    /* 3. pointer identities re-derived to their original targets. */
    if (soundInfo->ident != ID_NUMBER)
    {
        fprintf(stderr, "Native audio self-test: ident 0x%08x != ID_NUMBER after load\n",
                soundInfo->ident);
        return 1;
    }
    if (soundInfo->MPlayMainHead == NULL
     || soundInfo->musicPlayerHead == NULL
     || soundInfo->cgbChans == NULL
     || soundInfo->MPlayJumpTable == NULL)
    {
        fprintf(stderr, "Native audio self-test: gSoundInfo pointer fields NULL after load\n");
        return 1;
    }
    if (soundInfo->MPlayJumpTable != gMPlayJumpTable)
    {
        fprintf(stderr, "Native audio self-test: MPlayJumpTable not re-derived "
                        "(got %p want %p)\n",
                (void *)soundInfo->MPlayJumpTable, (const void *)gMPlayJumpTable);
        return 1;
    }
    if ((uintptr_t)soundInfo->cgbChans < bssStart
     || (uintptr_t)soundInfo->cgbChans >= bssEnd)
    {
        fprintf(stderr, "Native audio self-test: cgbChans outside game_bss after load\n");
        return 1;
    }
    /* Function pointers must have been restored through the executable
     * persistent-record path, not the data path. */
    {
        const uintptr_t funcs[8] = {
            (uintptr_t)soundInfo->MPlayMainHead,
            (uintptr_t)soundInfo->CgbSound,
            (uintptr_t)soundInfo->CgbOscOff,
            (uintptr_t)soundInfo->MidiKeyToCgbFreq,
            (uintptr_t)soundInfo->plynote,
            (uintptr_t)soundInfo->ExtVolPit,
            (uintptr_t)soundInfo->MPlayJumpTable[8],
            (uintptr_t)soundInfo->MPlayJumpTable[19],
        };
        u32 failed = 0;
        for (i = 0; i < 8; i++)
        {
            if (!Platform_RuntimeAddressIsExecutable(funcs[i]))
            {
                failed++;
                fprintf(stderr, "Native audio self-test: func[%u] = 0x%llx NOT executable after load\n",
                        i, (unsigned long long)funcs[i]);
            }
        }
        if (failed)
        {
            fprintf(stderr, "Native audio self-test: %u function pointers lost executability after load\n",
                    failed);
            return 1;
        }
    }

    /* Player chain: every node must be one of the six known players, its
     * tracks/memAccArea must match the table entry for that node, and the
     * chain must terminate after exactly six nodes. Exactly one player runs
     * the started song (songHeader non-NULL, pointing into game_bss). */
    player = soundInfo->musicPlayerHead;
    while (player != NULL && playersSeen < expectedPlayers)
    {
        uintptr_t address = (uintptr_t)player;
        u32 j;

        for (j = 0; j < expectedPlayers; j++)
        {
            if (address == playerAddrs[j])
                break;
        }
        if (j == expectedPlayers)
        {
            fprintf(stderr, "Native audio self-test: unknown player %p in chain after load\n",
                    (void *)player);
            return 1;
        }
        playerAddrs[j] = 0; /* visited */
        playersSeen++;
        if (j < MAX_MUSIC_PLAYERS)
        {
            if (player->tracks != (struct MusicPlayerTrack *)HostResolveGbaAddr(gMPlayTable[j].track)
             || player->memAccArea != gMPlayMemAccArea)
            {
                fprintf(stderr, "Native audio self-test: player %u linkage not re-derived\n", j);
                return 1;
            }
            if (player->songHeader != NULL)
            {
                if ((uintptr_t)player->songHeader < bssStart
                 || (uintptr_t)player->songHeader >= bssEnd)
                {
                    fprintf(stderr, "Native audio self-test: player %u songHeader outside game_bss\n", j);
                    return 1;
                }
                if (playingPlayer)
                {
                    fprintf(stderr, "Native audio self-test: more than one player has a song\n");
                    return 1;
                }
                playingPlayer = TRUE;
            }
        }
        else
        {
            u32 cry = j - MAX_MUSIC_PLAYERS;
            if (player->tracks != &gPokemonCryTracks[cry * 2]
             || player->memAccArea != NULL
             || player->songHeader != NULL)
            {
                fprintf(stderr, "Native audio self-test: cry player %u linkage not re-derived\n", cry);
                return 1;
            }
        }
        if ((uintptr_t)player->tracks < bssStart || (uintptr_t)player->tracks >= bssEnd)
        {
            fprintf(stderr, "Native audio self-test: player %u tracks outside game_bss\n", j);
            return 1;
        }
        player = player->musicPlayerNext;
    }
    if (player != NULL || playersSeen != expectedPlayers || !playingPlayer)
    {
        fprintf(stderr, "Native audio self-test: player chain malformed after load "
                        "(nodes=%u expected=%u song=%d)\n",
                playersSeen, expectedPlayers, (int)playingPlayer);
        return 1;
    }

    /* Channel linkage: prev/next must stay inside the chans array, sample
     * pointers must re-derive to image-resident (sub-4 GiB) or
     * arena-resident (R12-C) data. */
    for (i = 0; i < MAX_DIRECTSOUND_CHANNELS; i++)
    {
        ch = &soundInfo->chans[i];
        if (ch->prevChannelPointer != NULL
         && ((uintptr_t)ch->prevChannelPointer < chansStart
          || (uintptr_t)ch->prevChannelPointer >= chansEnd))
        {
            fprintf(stderr, "Native audio self-test: chans[%u].prevChannelPointer outside chans\n", i);
            return 1;
        }
        if (ch->nextChannelPointer != NULL
         && ((uintptr_t)ch->nextChannelPointer < chansStart
          || (uintptr_t)ch->nextChannelPointer >= chansEnd))
        {
            fprintf(stderr, "Native audio self-test: chans[%u].nextChannelPointer outside chans\n", i);
            return 1;
        }
        /* R12-C: image-resident (sub-4 GiB, compiled data) OR arena-resident
         * (the session's resource-owned spans - see the helper). */
        if (ch->wav != NULL
         && (uintptr_t)ch->wav >= 0x100000000ull
         && !NativeAudioPointerIsResourceResident((uintptr_t)ch->wav))
        {
            fprintf(stderr, "Native audio self-test: chans[%u].wav neither image- nor arena-resident after load\n", i);
            return 1;
        }
        if (ch->currentPointer != NULL
         && (uintptr_t)ch->currentPointer >= 0x100000000ull
         && !NativeAudioPointerIsResourceResident((uintptr_t)ch->currentPointer))
        {
            fprintf(stderr, "Native audio self-test: chans[%u].currentPointer neither image- nor arena-resident\n", i);
            return 1;
        }
    }
    fprintf(stdout, "Native audio self-test: all audio pointer fields re-derived correctly\n");

    /* PHASE B - post-restore mixer determinism. Re-establish the pristine
     * state, save, destroy, load, then tick 40 frames and require the result
     * to be byte-identical to the control. The save is made from the pristine
     * snapshot (no plants), so the restored engine state equals the control's
     * starting state, and cgb_audio_init() aligns the emulator trajectory. */
    memcpy(region, snapshot, regionSize);
    cgb_audio_init(42060);
    if (Platform_StateSave(PLATFORM_STATE_QUICK_SLOT) == PLATFORM_STATE_OPERATION_FAILED)
    {
        fprintf(stderr, "Native audio self-test: save failed: %s\n",
                Platform_StateGetLastError());
        return 1;
    }
    memset(region, 0xA5, regionSize);
    if (Platform_StateLoad(PLATFORM_STATE_QUICK_SLOT) != PLATFORM_STATE_OPERATION_OK)
    {
        fprintf(stderr, "Native audio self-test: load failed: %s\n",
                Platform_StateGetLastError());
        return 1;
    }
    for (frame = 0; frame < 40; frame++)
        RunMixerFrame();
    memcpy(after, region, regionSize);
    if (memcmp(after, control, regionSize) != 0)
    {
        size_t first;
        for (first = 0; first < regionSize; first++)
        {
            if (after[first] != control[first])
                break;
        }
        fprintf(stderr, "Native audio self-test: post-restore mixer output diverges "
                        "(game_bss+0x%zx: got 0x%02x want 0x%02x)\n",
                first, after[first], control[first]);
        return 1;
    }
    fprintf(stdout, "Native audio self-test passed "
                    "(control: 40 frames; save/load: 40 frames; PCM byte-identical)\n");
    return 0;
}
#endif /* PLATFORM_SDL2 && NATIVE_LINUX */

int main(int argc, char **argv)
{
    const char *importRomPath = NULL;
    const char *dataRootOverride = NULL;
    bool32 verifyGameData = FALSE;
    bool32 printDataPath = FALSE;
    bool32 nativeStateSelfTest = FALSE;
    bool32 nativeAudioSelfTest = FALSE;
    bool32 utilityMode;
    char *prefPath = NULL;
    int argIndex;
#if defined(_WIN32) && defined(WINDOWS_DEBUG_CONSOLE)
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#endif

    for (argIndex = 1; argIndex < argc; argIndex++)
    {
        if (strcmp(argv[argIndex], "--import-rom") == 0 && argIndex + 1 < argc)
            importRomPath = argv[++argIndex];
        else if (strcmp(argv[argIndex], "--data-root") == 0 && argIndex + 1 < argc)
            dataRootOverride = argv[++argIndex];
        else if (strcmp(argv[argIndex], "--verify-game-data") == 0)
            verifyGameData = TRUE;
        else if (strcmp(argv[argIndex], "--print-data-path") == 0)
            printDataPath = TRUE;
        else if (strcmp(argv[argIndex], "--native-state-self-test") == 0)
            nativeStateSelfTest = TRUE;
        else if (strcmp(argv[argIndex], "--native-audio-self-test") == 0)
            nativeAudioSelfTest = TRUE;
        else
        {
            fprintf(stderr, "Usage: %s [--data-root PATH] [--import-rom FILE] "
                            "[--verify-game-data] [--print-data-path] "
                            "[--native-state-self-test] "
                            "[--native-audio-self-test]\n", argv[0]);
            return 2;
        }
    }
    if (dataRootOverride == NULL || dataRootOverride[0] == '\0')
        dataRootOverride = getenv("POKEEMERALD_DATA_ROOT");
    utilityMode = importRomPath != NULL || verifyGameData || printDataPath
                || nativeStateSelfTest || nativeAudioSelfTest;

#ifdef __ANDROID__
    SDL_setenv("SDL_AUDIODRIVER", "openslES", 1);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#endif
    if(SDL_Init(utilityMode ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) < 0)
    {
        DBGPRINTF("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    if (!utilityMode)
        Platform_InputInit();

    if (dataRootOverride == NULL || dataRootOverride[0] == '\0')
    {
#ifdef _WIN32
        prefPath = SDL_GetPrefPath(NULL, "PokemonEmeraldRecomp");
#else
        prefPath = SDL_GetPrefPath("pokeemerald", "pokeemerald");
#endif
    }
    Platform_StorageInit(dataRootOverride != NULL && dataRootOverride[0] != '\0'
                       ? dataRootOverride : prefPath);
    if (!utilityMode)
        Platform_ConfigLoad();
    if (!utilityMode || nativeStateSelfTest)
        Platform_ClockInit();
    if (prefPath != NULL)
        SDL_free(prefPath);

    if (printDataPath)
    {
        Platform_GameContentVerifyInstalled(FALSE);
        printf("%s\n", Platform_GameContentGetInstallPath());
        if (importRomPath == NULL && !verifyGameData)
        {
            Platform_StorageShutdown();
            SDL_Quit();
            return 0;
        }
    }
    if (importRomPath != NULL)
    {
        struct PlatformGameContentImportInfo info;
        enum PlatformGameContentImportResult result = Platform_GameContentImport(importRomPath, &info);
        if (result == PLATFORM_GAME_CONTENT_IMPORT_UNSUPPORTED_ROM)
        {
            fprintf(stderr,
                    "Unsupported Pokemon Emerald ROM\n\n"
                    "The selected ROM does not match the supported\n"
                    "Pokemon Emerald revision.\n\n"
                    "Detected SHA-1:\n%s\n\nExpected:\n%s\n",
                    info.detectedSha1, Platform_GameContentGetExpectedSha1());
        }
        else if (result != PLATFORM_GAME_CONTENT_IMPORT_OK)
            fprintf(stderr, "ROM import failed: %s\n", info.error);
        else
        {
            printf("Installed Pokemon Emerald content at %s\n",
                   Platform_GameContentGetInstallPath());
            printf("Source SHA-1: %s\n", info.detectedSha1);
            printf("Package SHA-1: %s\n", Platform_GameContentGetInstalledPackageSha1());
        }
        Platform_StorageShutdown();
        SDL_Quit();
        return result == PLATFORM_GAME_CONTENT_IMPORT_OK ? 0 : 2;
    }
    if (verifyGameData)
    {
        bool32 valid = Platform_GameContentVerifyInstalled(TRUE);
        if (valid)
            printf("Pokemon Emerald content verified: %s\n",
                   Platform_GameContentGetInstalledPackageSha1());
        else
            fprintf(stderr, "Pokemon Emerald content invalid: %s\n",
                    Platform_GameContentGetLastError());
        Platform_StorageShutdown();
        SDL_Quit();
        return valid ? 0 : 2;
    }

    if (nativeStateSelfTest)
    {
        static const u8 slots[] = {PLATFORM_STATE_QUICK_SLOT, 1};
        const void *filePointers[] = {stdin, stdout, stderr};
        char statePath[1024];
        u32 i;

        if (!Platform_ProfileInit()
         || !Platform_ProfileLoadSelectedSave(FLASH_BASE, sizeof(FLASH_BASE)))
        {
            fprintf(stderr, "Native state self-test could not initialize a profile\n");
            Platform_StorageShutdown();
            SDL_Quit();
            return 1;
        }
#ifdef _WIN32
        if (NativeStatePointerInGameRange(stdin)
         || NativeStatePointerInGameRange(stdout)
         || NativeStatePointerInGameRange(stderr))
#else
        if (NativeStatePointerInGameRange(&stdin)
         || NativeStatePointerInGameRange(&stdout)
         || NativeStatePointerInGameRange(&stderr))
#endif
        {
            fprintf(stderr, "Native state self-test found a libc FILE object in game state\n");
            Platform_StorageShutdown();
            SDL_Quit();
            return 1;
        }
        fprintf(stdout, "NATIVE_STATE_FILE_POINTERS stdin=%p stdout=%p stderr=%p\n",
                (void *)stdin, (void *)stdout, (void *)stderr);
        for (i = 0; i < ARRAY_COUNT(slots); i++)
        {
            if (Platform_StateSave(slots[i]) == PLATFORM_STATE_OPERATION_FAILED)
            {
                fprintf(stderr, "Native state self-test failed for slot %u: %s\n",
                        slots[i], Platform_StateGetLastError());
                Platform_StorageShutdown();
                SDL_Quit();
                return 1;
            }
            if (!Platform_ProfileGetStatePath(slots[i], statePath, sizeof(statePath))
             || !Platform_StorageFileExists(statePath)
             || NativeStateFileContainsPointer(slots[i], filePointers[0])
             || NativeStateFileContainsPointer(slots[i], filePointers[1])
             || NativeStateFileContainsPointer(slots[i], filePointers[2])
             || Platform_StateLoad(slots[i]) != PLATFORM_STATE_OPERATION_OK)
            {
                fprintf(stderr, "Native state self-test failed for slot %u: %s\n",
                        slots[i], Platform_StateGetLastError());
                Platform_StorageShutdown();
                SDL_Quit();
                return 1;
            }
        }
#if defined(NATIVE_LINUX)
        /* The trainer-family probe (sdl2.c above) corrupts and repairs the
         * migrated tables: its premise - NATIVE_LINUX NULL-sentinel tables
         * repaired by the compat republish seam - exists only on the linux
         * target. Windows keeps the GBA-style compiled tables (const, real
         * payload pointers, no republish), so the probe cannot run there;
         * the save/load slot tests above are target-independent. */
        if (!NativeStateTrainerFamilyTest())
        {
            fprintf(stderr, "Native state self-test failed (trainer family republish)\n");
            Platform_StorageShutdown();
            SDL_Quit();
            return 1;
        }
#endif /* NATIVE_LINUX */
        fprintf(stdout, "Native state self-test passed (quick and slot 1)\n");
#if defined(NATIVE_LINUX)
        fprintf(stdout, "Native state self-test passed (trainer family republish)\n");
#endif /* NATIVE_LINUX */
        Platform_StorageShutdown();
        SDL_Quit();
        return 0;
    }
#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)
    if (nativeAudioSelfTest)
    {
        /* Headless audio/state regression: real MP2K engine + real State v5
         * save/load, control vs restore mixer comparison. No SDL devices are
         * ever opened (utility mode: SDL_Init(0) only). */
        int rc = NativeStateAudioSelfTest();

        Platform_StorageShutdown();
        SDL_Quit();
        return rc;
    }
#endif /* PLATFORM_SDL2 && NATIVE_LINUX */

    if (!Platform_VideoInit())
        return 1;

    if (!Platform_GameContentVerifyInstalled(TRUE)
     && !Platform_FrontendRunGameDataSetup())
    {
        Platform_VideoShutdown();
        Platform_InputShutdown();
        Platform_StorageShutdown();
        SDL_Quit();
        return 0;
    }

    if (!Platform_ProfileInit())
        return 1;

    if (!Platform_FrontendRunStartup())
    {
        Platform_VideoShutdown();
        Platform_InputShutdown();
        Platform_StorageShutdown();
        SDL_Quit();
        return 0;
    }
    Platform_ProfileLoadSelectedSave(FLASH_BASE, sizeof(FLASH_BASE));
    {
        const struct PlatformProfileMetadata *profile = Platform_ProfileGetSelected();
        fprintf(stderr,
                "Platform startup: profile=%s save=%s profile_storage=%s legacy_fallback=%s\n",
                profile != NULL ? profile->id : "<none>",
                Platform_StorageGetActiveSavePath(),
                Platform_StorageActiveSaveIsProfile() ? "yes" : "no",
                Platform_StorageActiveSaveIsProfile() ? "disabled" : "enabled");
        fflush(stderr);
    }

#if 0
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#if defined(NATIVE_LINUX) || defined(_WIN32)
    sdlWindow = SDL_CreateWindow("Pokemon Emerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#else
    sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

#ifdef __ANDROID__
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED);
#else
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
#endif
    if (sdlRenderer == NULL)
    {
        DBGPRINTF("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    for (int i = 1; i < 15; i++)
    {
        char filename[16];
#ifdef _WIN32
        snprintf(filename, sizeof(filename), "BG%d.bmp", i);
#else
        snprintf(filename, sizeof(filename), "BG%d.png", i);
#endif
        SDL_RWops *backgroundFile = SDL_RWFromFile(filename, "rb");
        if (backgroundFile == NULL)
            break;
        SDL_RWclose(backgroundFile);
        sBorderBackgroundCount++;
    }
    if (Platform_ConfigGetBackgroundOrderVersion() < 2)
    {
        if (Platform_ConfigHasBorderBackground())
        {
            u8 selection = Platform_ConfigGetBorderBackground();
            if (selection == 1)
                selection = sBorderBackgroundCount;
            else if (selection >= 2)
                selection--;
            Platform_ConfigSetBorderBackground(selection);
        }
        Platform_ConfigSetBackgroundOrderVersion(2);
        Platform_ConfigStore();
    }
#ifdef NATIVE_LINUX
    SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0)
    {
        SDL_Log("SDL_image could not initialize: %s", IMG_GetError());
    }
    else
    {
        for (int i = 0; i < sBorderBackgroundCount; i++)
        {
            char filename[16];
            snprintf(filename, sizeof(filename), i == 0 ? "BG.png" : "BG%d.png", i);
            sdlBackgroundTextures[i] = IMG_LoadTexture(sdlRenderer, filename);
        }
        sdlBorderTexture = IMG_LoadTexture(sdlRenderer, "Border.png");
        if (sdlBackgroundTextures[0] == NULL)
            SDL_Log("Background image could not be loaded: %s", IMG_GetError());
        if (sdlBorderTexture == NULL)
            SDL_Log("Border image could not be loaded: %s", IMG_GetError());
    }
#elif defined(_WIN32)
    SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
    SDL_Surface *borderSurface = SDL_LoadBMP("Border.bmp");
    for (int i = 0; i < sBorderBackgroundCount; i++)
    {
        char filename[16];
        snprintf(filename, sizeof(filename), i == 0 ? "BG.bmp" : "BG%d.bmp", i);
        SDL_Surface *backgroundSurface = SDL_LoadBMP(filename);
        if (backgroundSurface == NULL)
            continue;
        sdlBackgroundTextures[i] = SDL_CreateTextureFromSurface(sdlRenderer, backgroundSurface);
        SDL_FreeSurface(backgroundSurface);
    }
    if (sdlBackgroundTextures[0] == NULL)
        SDL_Log("Background image could not be loaded: %s", SDL_GetError());
    if (borderSurface == NULL)
    {
        SDL_Log("Border image could not be loaded: %s", SDL_GetError());
    }
    else
    {
        sdlBorderTexture = SDL_CreateTextureFromSurface(sdlRenderer, borderSurface);
        SDL_FreeSurface(borderSurface);
    }
#else
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    SDL_RenderSetIntegerScale(sdlRenderer, SDL_TRUE);
#endif
    ApplyPlatformSettings();

    sdlTexture = SDL_CreateTexture(sdlRenderer,
                                   SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (sdlTexture == NULL)
    {
        DBGPRINTF("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureBlendMode(sdlTexture, SDL_BLENDMODE_NONE);
#endif

    simTime = curGameTime = lastGameTime = SDL_GetPerformanceCounter();

    Platform_AudioInit(42060);
    cgb_audio_init(42060);
#ifndef __ANDROID__
    Platform_VideoDrawFrame();
#endif
    if (!Platform_SchedulerInit())
    {
        DBGPRINTF("Unable to initialize scheduler\n");
        return 1;
    }

    double accumulator = 0.0;
    u64 lastPresentationCounter = 0;
    u64 presentationCount = 0;
    enum NativeStateRequest stateRequest = NATIVE_STATE_REQUEST_NONE;
    bool32 traceScheduler = EnvironmentEnabled("POKEEMERALD_SCHEDULER_TRACE");
    u32 probePresentations = EnvironmentUnsigned("POKEEMERALD_SCHEDULER_PROBE_PRESENTATIONS", 0);
    u32 probeSpeed = EnvironmentUnsigned("POKEEMERALD_SCHEDULER_PROBE_SPEED", UINT32_MAX);

    if (probeSpeed == 0 || (probeSpeed >= 1 && probeSpeed <= 5))
    {
        speedUp = probeSpeed != 1;
        timeScale = probeSpeed == 0 ? 0.0 : probeSpeed;
        Platform_SchedulerSetSpeed((u8)probeSpeed);
        Platform_VideoSetFastForward(speedUp);
    }

    while (isRunning)
    {
        struct PlatformInputActions input;
        Platform_InputPoll(&input);
#if defined(LINUX64) && LINUX64
        if (input.zoomReset)
            Platform_VideoZoomReset();
        else if (input.zoomOut)
            Platform_VideoZoomOut();
        else if (input.zoomIn)
            Platform_VideoZoomIn();
#endif
#if defined(NATIVE_LINUX) || defined(WINDOWS64)
        HandleNativeDebugActions(&input);
#endif
        if (input.quit)
            isRunning = false;
        if (input.quickSave)
            stateRequest = NATIVE_STATE_REQUEST_SAVE;
        if (input.quickLoad)
            stateRequest = NATIVE_STATE_REQUEST_LOAD;
        if (input.openStateManager)
        {
            enum PlatformStateUiResult uiResult = RunStateUi(TRUE, &accumulator,
                                                              &lastPresentationCounter);
            if (uiResult == PLATFORM_STATE_UI_LOADED)
                Platform_VideoSetStatus("State loaded");
            else if (uiResult == PLATFORM_STATE_UI_QUIT)
                isRunning = FALSE;
        }
        if (input.manualSave && isRunning)
        {
            enum PlatformStateUiResult uiResult = RunStateUi(FALSE, &accumulator,
                                                              &lastPresentationCounter);
            if (uiResult == PLATFORM_STATE_UI_SAVED)
            {
                const char *warning = Platform_StateGetLastError();
                Platform_VideoSetStatus(warning != NULL && warning[0] != '\0'
                                      ? warning : "State saved");
            }
            else if (uiResult == PLATFORM_STATE_UI_QUIT)
                isRunning = FALSE;
        }
        if (input.reset)
            DoSoftReset();
        if (input.openSettings)
        {
            bool32 wasPaused = paused;
            paused = TRUE;
            Platform_AudioSetPaused(TRUE);
            Platform_FrontendRunSettings();
            paused = wasPaused;
            if (!paused)
            {
                Platform_AudioClearQueue();
                Platform_AudioSetPaused(FALSE);
            }
            lastPresentationCounter = 0;
            accumulator = 0.0;
        }
        if (input.togglePause)
        {
            paused = !paused;
            if (paused)
                Platform_AudioSetPaused(TRUE);
            else
            {
                Platform_AudioClearQueue();
                Platform_AudioSetPaused(FALSE);
            }
        }
        if (input.speedUpChanged)
        {
            speedUp = input.speedUp;
            timeScale = speedUp ? (input.speed == 0 ? 0.0 : input.speed) : 1.0;
            Platform_SchedulerSetSpeed(speedUp ? input.speed : 1);
            Platform_VideoSetFastForward(speedUp);
            lastPresentationCounter = 0;
            if (!speedUp && paused)
                Platform_AudioSetPaused(TRUE);
        }

        /* A key request is not considered serviced until the worker has
         * published the next quiescent VBlank boundary. */
        if (stateRequest != NATIVE_STATE_REQUEST_NONE
         && Platform_SchedulerWaitForFrame(1000))
        {
            PrepareHostFrame(stateRequest, PLATFORM_STATE_QUICK_SLOT,
                             &accumulator, &lastPresentationCounter);
            stateRequest = NATIVE_STATE_REQUEST_NONE;
        }

        if (!paused)
        {
            u8 schedulerSpeed = Platform_SchedulerGetSpeed();
            u64 beforeFrame = Platform_SchedulerGetFrameCounter();
            u64 frequency = SDL_GetPerformanceFrequency();
            u64 batchDeadline;
            u32 targetFrames = schedulerSpeed == 0 ? 256 : schedulerSpeed;
            u32 completedFrames = 0;

            if (lastPresentationCounter == 0)
                lastPresentationCounter = SDL_GetPerformanceCounter();
            batchDeadline = lastPresentationCounter + frequency / 60;
            while (completedFrames < targetFrames && isRunning)
            {
                bool32 finalFrame;
                if (!Platform_SchedulerWaitForFrame(1000))
                {
                    fprintf(stderr, "Scheduler timed out waiting for simulation frame\n");
                    fflush(stderr);
                    isRunning = FALSE;
                    break;
                }
                finalFrame = schedulerSpeed == 0
                    ? ((completedFrames != 0 && SDL_GetPerformanceCounter() >= batchDeadline)
                       || completedFrames + 1 == targetFrames)
                    : completedFrames + 1 == targetFrames;
                if (finalFrame)
                    PrepareHostFrame(NATIVE_STATE_REQUEST_NONE, PLATFORM_STATE_QUICK_SLOT,
                                     &accumulator, &lastPresentationCounter);
                Platform_SchedulerCompleteFrame();
                completedFrames++;
                if (schedulerSpeed == 0 && finalFrame)
                    break;
            }

            if (traceScheduler)
                fprintf(stderr,
                        "Scheduler presentation=%llu speed=%u simulation_frames=%llu\n",
                        (unsigned long long)(presentationCount + 1), schedulerSpeed,
                        (unsigned long long)(Platform_SchedulerGetFrameCounter() - beforeFrame));
        }
        else
            SDL_Delay(1);

#ifdef __ANDROID__
        DrawTouchControls();
#endif
        if (speedUp || !Platform_GetSetting(PLATFORM_SETTING_VSYNC))
            PacePresentation(&lastPresentationCounter);
        Platform_VideoPresent();
        presentationCount++;
        if (probePresentations != 0 && presentationCount >= probePresentations)
            isRunning = FALSE;
    }

    //Platform_StoreSaveFile();
    NativeOverworldParity_Shutdown();
    Platform_SchedulerShutdown();
    Platform_StorageShutdown();

    Platform_VideoShutdown();
    Platform_InputShutdown();
    Platform_AudioShutdown();
    SDL_Quit();
    return 0;
}

#if 0
static void ApplyPlatformSettings(void)
{
    SDL_RenderSetVSync(sdlRenderer, Platform_GetSetting(PLATFORM_SETTING_VSYNC));
#if defined(NATIVE_LINUX) || defined(_WIN32)
    SDL_SetWindowFullscreen(sdlWindow, Platform_GetSetting(PLATFORM_SETTING_FULLSCREEN)
                                      ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!Platform_GetSetting(PLATFORM_SETTING_FULLSCREEN))
    {
        int scale = Platform_GetSetting(PLATFORM_SETTING_WINDOW_SCALE);
        SDL_SetWindowSize(sdlWindow, 320 * scale, 180 * scale);
        SDL_SetWindowPosition(sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#endif
}
#endif

void Platform_StoreSaveFile(void)
{
    if (!Platform_StorageWriteSave(FLASH_BASE, sizeof(FLASH_BASE)))
        DBGPRINTF("Unable to store save file\n");
}

void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    if (!Platform_StorageReadFlash(sectorNum, offset, dest, size))
        DBGPRINTF("ReadFlash out of bounds or unavailable\n");
}

/* The decorative border backgrounds/frame were removed from the presentation
 * layer. These stubs remain only because the in-game option menu still
 * queries them; a count of 1 leaves "OFF" as the sole background choice. */
u8 Platform_GetBorderBackgroundCount(void)
{
    return 1;
}

u8 Platform_GetBorderBackground(void)
{
    return 0;
}

void Platform_SetBorderBackground(u8 selection)
{
}

void Platform_SetSetting(enum PlatformSetting setting, u8 value)
{
    Platform_ConfigSetSetting(setting, value);
    Platform_VideoApplySetting(setting, value);
    Platform_ConfigStore();
}

#ifdef __ANDROID__
JNIEXPORT jint JNICALL Java_com_pokeemerald_experimental_GbaControlsView_getBorderBackground(JNIEnv *env, jclass clazz)
{
    return Platform_GetBorderBackground();
}

JNIEXPORT jint JNICALL Java_com_pokeemerald_experimental_GbaControlsView_getPlatformSetting(JNIEnv *env, jclass clazz, jint setting)
{
    if (setting < 0 || setting >= PLATFORM_SETTING_COUNT)
        return 0;
    return Platform_GetSetting(setting);
}
#endif


#ifdef __ANDROID__
#define MAX_TOUCH_FINGERS 10

struct TouchFinger
{
    SDL_FingerID id;
    float x;
    float y;
    bool active;
};

HOST_DATA static struct TouchFinger touchFingers[MAX_TOUCH_FINGERS];
HOST_DATA static u16 touchKeys;
static bool IsInsideRect(int x, int y, SDL_Rect rect)
{
    SDL_Point point = {x, y};
    return SDL_PointInRect(&point, &rect);
}

static int MinInt(int a, int b)
{
    return a < b ? a : b;
}

static int GetControlSideWidth(int windowWidth, int windowHeight)
{
    int sideWidth = (windowWidth - windowHeight * 3 / 2) / 2;
    int minimumWidth = windowWidth * 14 / 100;
    return sideWidth > minimumWidth ? sideWidth : minimumWidth;
}

static void UpdateTouchKeys(void)
{
    int windowWidth;
    int windowHeight;
    SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight);
    int sideWidth = GetControlSideWidth(windowWidth, windowHeight);
    int buttonSize = MinInt(sideWidth * 2 / 5, windowHeight / 6);
    int dpadUnit = MinInt(sideWidth / 3, windowHeight / 8);
    int dpadX = sideWidth * 2 / 3;
    int dpadY = windowHeight * 7 / 10;
    SDL_Rect dpadUp = {dpadX - dpadUnit / 2, dpadY - dpadUnit * 3 / 2,
                       dpadUnit, dpadUnit};
    SDL_Rect dpadDown = {dpadX - dpadUnit / 2, dpadY + dpadUnit / 2,
                         dpadUnit, dpadUnit};
    SDL_Rect dpadLeft = {dpadX - dpadUnit * 3 / 2, dpadY - dpadUnit / 2,
                         dpadUnit, dpadUnit};
    SDL_Rect dpadRight = {dpadX + dpadUnit / 2, dpadY - dpadUnit / 2,
                          dpadUnit, dpadUnit};
    SDL_Rect aButton = {windowWidth - sideWidth / 4 - buttonSize,
                        windowHeight * 58 / 100, buttonSize, buttonSize};
    SDL_Rect bButton = {windowWidth - sideWidth + sideWidth / 4,
                        windowHeight * 76 / 100, buttonSize, buttonSize};
    SDL_Rect selectButton = {sideWidth / 4, windowHeight / 4,
                             sideWidth / 2, windowHeight / 10};
    SDL_Rect startButton = {windowWidth - sideWidth * 3 / 4, windowHeight / 4,
                            sideWidth / 2, windowHeight / 10};
    SDL_Rect lButton = {sideWidth / 4, windowHeight / 20,
                        sideWidth / 2, windowHeight / 10};
    SDL_Rect rButton = {windowWidth - sideWidth * 3 / 4, windowHeight / 20,
                        sideWidth / 2, windowHeight / 10};

    touchKeys = 0;

    for (int i = 0; i < MAX_TOUCH_FINGERS; i++)
    {
        if (!touchFingers[i].active)
            continue;

        int x = touchFingers[i].x * windowWidth;
        int y = touchFingers[i].y * windowHeight;

        if (IsInsideRect(x, y, dpadUp)) touchKeys |= DPAD_UP;
        if (IsInsideRect(x, y, dpadDown)) touchKeys |= DPAD_DOWN;
        if (IsInsideRect(x, y, dpadLeft)) touchKeys |= DPAD_LEFT;
        if (IsInsideRect(x, y, dpadRight)) touchKeys |= DPAD_RIGHT;

        if (IsInsideRect(x, y, aButton)) touchKeys |= A_BUTTON;
        if (IsInsideRect(x, y, bButton)) touchKeys |= B_BUTTON;
        if (IsInsideRect(x, y, startButton)) touchKeys |= START_BUTTON;
        if (IsInsideRect(x, y, selectButton)) touchKeys |= SELECT_BUTTON;
        if (IsInsideRect(x, y, lButton)) touchKeys |= L_BUTTON;
        if (IsInsideRect(x, y, rButton)) touchKeys |= R_BUTTON;
    }
}

void Platform_HandleTouchEvent(const SDL_TouchFingerEvent *event)
{
    int slot = -1;
    for (int i = 0; i < MAX_TOUCH_FINGERS; i++)
    {
        if (touchFingers[i].active && touchFingers[i].id == event->fingerId)
        {
            slot = i;
            break;
        }
        if (slot < 0 && !touchFingers[i].active)
            slot = i;
    }

    if (slot < 0)
        return;

    if (event->type == SDL_FINGERUP)
    {
        touchFingers[slot].active = false;
    }
    else
    {
        touchFingers[slot].id = event->fingerId;
        touchFingers[slot].x = event->x;
        touchFingers[slot].y = event->y;
        touchFingers[slot].active = true;
    }

    UpdateTouchKeys();
}

static const Uint8 *GetGlyph(char character)
{
    static const Uint8 glyphA[7] = {14, 17, 17, 31, 17, 17, 17};
    static const Uint8 glyphB[7] = {30, 17, 17, 30, 17, 17, 30};
    static const Uint8 glyphC[7] = {15, 16, 16, 16, 16, 16, 15};
    static const Uint8 glyphE[7] = {31, 16, 16, 30, 16, 16, 31};
    static const Uint8 glyphL[7] = {16, 16, 16, 16, 16, 16, 31};
    static const Uint8 glyphR[7] = {30, 17, 17, 30, 20, 18, 17};
    static const Uint8 glyphS[7] = {15, 16, 16, 14, 1, 1, 30};
    static const Uint8 glyphT[7] = {31, 4, 4, 4, 4, 4, 4};

    switch (character)
    {
    case 'A': return glyphA;
    case 'B': return glyphB;
    case 'C': return glyphC;
    case 'E': return glyphE;
    case 'L': return glyphL;
    case 'R': return glyphR;
    case 'S': return glyphS;
    case 'T': return glyphT;
    default:  return NULL;
    }
}

static void DrawControlLabel(SDL_Rect rect, const char *label)
{
    int length = SDL_strlen(label);
    int scale = MinInt(rect.h / 9, rect.w / (length * 6));
    if (scale < 1)
        scale = 1;
    int startX = rect.x + (rect.w - (length * 6 - 1) * scale) / 2;
    int startY = rect.y + (rect.h - 7 * scale) / 2;

    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 230);
    for (int character = 0; character < length; character++)
    {
        const Uint8 *glyph = GetGlyph(label[character]);
        if (glyph == NULL)
            continue;
        for (int row = 0; row < 7; row++)
        {
            for (int column = 0; column < 5; column++)
            {
                if (glyph[row] & (1 << (4 - column)))
                {
                    SDL_Rect pixel = {startX + (character * 6 + column) * scale,
                                      startY + row * scale, scale, scale};
                    SDL_RenderFillRect(sdlRenderer, &pixel);
                }
            }
        }
    }
}

static void DrawControlRect(SDL_Rect rect, bool pressed, const char *label)
{
    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, pressed ? 150 : 65);
    SDL_RenderFillRect(sdlRenderer, &rect);
    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, pressed ? 230 : 130);
    SDL_RenderDrawRect(sdlRenderer, &rect);
    if (label != NULL)
        DrawControlLabel(rect, label);
}

static void DrawTouchControls(void)
{
    int windowWidth;
    int windowHeight;
    SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight);
    int sideWidth = GetControlSideWidth(windowWidth, windowHeight);
    int buttonSize = MinInt(sideWidth * 2 / 5, windowHeight / 6);
    int dpadUnit = MinInt(sideWidth / 3, windowHeight / 8);
    int dpadX = sideWidth * 2 / 3;
    int dpadY = windowHeight * 7 / 10;

    SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);

    DrawControlRect((SDL_Rect){dpadX - dpadUnit / 2, dpadY - dpadUnit * 3 / 2,
                               dpadUnit, dpadUnit}, touchKeys & DPAD_UP, NULL);
    DrawControlRect((SDL_Rect){dpadX - dpadUnit / 2, dpadY + dpadUnit / 2,
                               dpadUnit, dpadUnit}, touchKeys & DPAD_DOWN, NULL);
    DrawControlRect((SDL_Rect){dpadX - dpadUnit * 3 / 2, dpadY - dpadUnit / 2,
                               dpadUnit, dpadUnit}, touchKeys & DPAD_LEFT, NULL);
    DrawControlRect((SDL_Rect){dpadX + dpadUnit / 2, dpadY - dpadUnit / 2,
                               dpadUnit, dpadUnit}, touchKeys & DPAD_RIGHT, NULL);
    DrawControlRect((SDL_Rect){windowWidth - sideWidth / 4 - buttonSize,
                               windowHeight * 58 / 100, buttonSize, buttonSize}, touchKeys & A_BUTTON, "A");
    DrawControlRect((SDL_Rect){windowWidth - sideWidth + sideWidth / 4,
                               windowHeight * 76 / 100, buttonSize, buttonSize}, touchKeys & B_BUTTON, "B");
    DrawControlRect((SDL_Rect){windowWidth - sideWidth * 3 / 4, windowHeight / 4,
                               sideWidth / 2, windowHeight / 10}, touchKeys & START_BUTTON, "START");
    DrawControlRect((SDL_Rect){sideWidth / 4, windowHeight / 4,
                               sideWidth / 2, windowHeight / 10}, touchKeys & SELECT_BUTTON, "SELECT");
    DrawControlRect((SDL_Rect){sideWidth / 4, windowHeight / 20,
                               sideWidth / 2, windowHeight / 10}, touchKeys & L_BUTTON, "L");
    DrawControlRect((SDL_Rect){windowWidth - sideWidth * 3 / 4, windowHeight / 20,
                               sideWidth / 2, windowHeight / 10}, touchKeys & R_BUTTON, "R");

    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_NONE);
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    SDL_RenderSetIntegerScale(sdlRenderer, SDL_TRUE);
}

#endif

#if 0
void ProcessEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            isRunning = false;
            break;
#ifdef __ANDROID__
        case SDL_CONTROLLERDEVICEADDED:
            if (androidController == NULL && SDL_IsGameController(event.cdevice.which))
                androidController = SDL_GameControllerOpen(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (androidController != NULL
             && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(androidController)) == event.cdevice.which)
            {
                SDL_GameControllerClose(androidController);
                androidController = NULL;
                controllerKeys = 0;
                controllerAxisKeys = 0;
                controllerAxisX = 0;
                controllerAxisY = 0;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            controllerKeys |= ControllerButtonMask(event.cbutton.button);
            break;
        case SDL_CONTROLLERBUTTONUP:
            controllerKeys &= ~ControllerButtonMask(event.cbutton.button);
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
                controllerAxisX = event.caxis.value;
            else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                controllerAxisY = event.caxis.value;

            controllerAxisKeys = 0;
            if (controllerAxisX < -16000) controllerAxisKeys |= DPAD_LEFT;
            if (controllerAxisX >  16000) controllerAxisKeys |= DPAD_RIGHT;
            if (controllerAxisY < -16000) controllerAxisKeys |= DPAD_UP;
            if (controllerAxisY >  16000) controllerAxisKeys |= DPAD_DOWN;
            break;
#endif
        case SDL_KEYUP:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYUP(A_BUTTON)
            HANDLE_KEYUP(B_BUTTON)
            HANDLE_KEYUP(START_BUTTON)
            HANDLE_KEYUP(SELECT_BUTTON)
            HANDLE_KEYUP(L_BUTTON)
            HANDLE_KEYUP(R_BUTTON)
            HANDLE_KEYUP(DPAD_UP)
            HANDLE_KEYUP(DPAD_DOWN)
            HANDLE_KEYUP(DPAD_LEFT)
            HANDLE_KEYUP(DPAD_RIGHT)
            case SDLK_SPACE:
                if (speedUp)
                {
                    speedUp = false;
                    timeScale = 1.0;
                    Platform_AudioClearQueue();
                    Platform_AudioSetPaused(FALSE);
                }
                break;
            }
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYDOWN(A_BUTTON)
            HANDLE_KEYDOWN(B_BUTTON)
            HANDLE_KEYDOWN(START_BUTTON)
            HANDLE_KEYDOWN(SELECT_BUTTON)
            HANDLE_KEYDOWN(L_BUTTON)
            HANDLE_KEYDOWN(R_BUTTON)
            HANDLE_KEYDOWN(DPAD_UP)
            HANDLE_KEYDOWN(DPAD_DOWN)
            HANDLE_KEYDOWN(DPAD_LEFT)
            HANDLE_KEYDOWN(DPAD_RIGHT)
            case SDLK_r:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    DoSoftReset();
                }
                break;
            case SDLK_p:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    paused = !paused;
                }
                break;
            case SDLK_SPACE:
                if (!speedUp)
                {
                    speedUp = true;
                    timeScale = 5.0;
                    Platform_AudioSetPaused(TRUE);
                }
                break;
            }
            break;
        }
    }
}

#ifdef _WIN32
#define STICK_THRESHOLD 0.5f
u16 GetXInputKeys()
{
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));

    DWORD dwResult = XInputGetState(0, &state);
    u16 xinputKeys = 0;

    if (dwResult == ERROR_SUCCESS)
    {
        /* A */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) >> 12;
        /* B */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) >> 13;
        /* Start */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) >> 1;
        /* Select */ xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) >> 3;
        /* L */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) << 1;
        /* R */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) >> 1;
        /* Up */     xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) << 6;
        /* Down */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) << 6;
        /* Left */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) << 3;
        /* Right */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) << 1;


        /* Control Stick */
        float xAxis = (float)state.Gamepad.sThumbLX / (float)SHRT_MAX;
        float yAxis = (float)state.Gamepad.sThumbLY / (float)SHRT_MAX;

        if (xAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_LEFT;
        if (xAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_RIGHT;
        if (yAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_DOWN;
        if (yAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_UP;


        /* Speedup */
        // Note: 'speedup' variable is only (un)set on keyboard input
        double oldTimeScale = timeScale;
        timeScale = (state.Gamepad.bRightTrigger > 0x80 || speedUp) ? 5.0 : 1.0;

        if (oldTimeScale != timeScale)
        {
            if (timeScale > 1.0)
            {
                Platform_AudioSetPaused(TRUE);
            }
            else
            {
                Platform_AudioClearQueue();
                Platform_AudioSetPaused(FALSE);
            }
        }
    }

    return xinputKeys;
}
#endif // _WIN32

#endif

u16 Platform_GetKeyInput(void)
{
#ifdef __ANDROID__
    return Platform_InputGetKeys() | touchKeys;
#else
    return Platform_InputGetKeys();
#endif
}

void SoftReset(u32 resetFlags)
{
    puts("Soft Reset called. Exiting.");
    exit(0);
}

#endif
