#ifdef PLATFORM_SDL2

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "global.h"
#include "battle.h"
#include "battle_anim.h"
#include "battle_controllers.h"
#include "battle_message.h"
#include "malloc.h"
#include "main.h"
#include "sprite.h"
#include "task.h"
#include "text.h"
#include "window.h"
#include "gba/defines.h"
#include "gba/flash_internal.h"
#include "platform/desktop_clock.h"
#include "platform/desktop_audio.h"
#include "platform/desktop_runtime.h"
#include "platform/desktop_scheduler.h"
#include "platform/desktop_storage.h"
#include "platform/desktop_video.h"
#include "platform/host_memory.h"
#include "platform/native_state.h"
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "siirtc.h"

#define NATIVE_STATE_MAGIC 0x4E535431u /* NST1 */
#define NATIVE_STATE_FORMAT_VERSION 4u
#define NATIVE_STATE_MAX_FILE_SIZE (32u * 1024u * 1024u)
#ifdef _WIN32
#define NATIVE_STATE_ABI_ID "windows64-v1"
#else
#define NATIVE_STATE_ABI_ID "linux64-v5"
#endif
#define NATIVE_STATE_CONTENT_FINGERPRINT "f3ae088181bf583e55daf962a92bb46f4f1d07b7"

enum NativeStateSectionTag
{
    STATE_SECTION_GAME_BSS = 1,
    STATE_SECTION_REGISTERS,
    STATE_SECTION_VIDEO_MEMORY,
    STATE_SECTION_FLASH,
    STATE_SECTION_EWRAM,
    STATE_SECTION_IWRAM,
    STATE_SECTION_COMMON,
    STATE_SECTION_GAME_DATA,
    STATE_SECTION_FRAMEBUFFER,
    STATE_SECTION_TASK_SIDECAR,
    STATE_SECTION_SPRITE_SIDECAR,
    STATE_SECTION_RTC,
    STATE_SECTION_BATTLE_SIDECAR,
};

struct NativeStateHeader
{
    u32 magic;
    u32 formatVersion;
    u32 headerSize;
    u32 totalSize;
    u32 sectionCount;
    u32 payloadSize;
    u32 payloadCrc;
    u32 reserved;
    u64 frame;
    char buildId[64];
    char contentFingerprint[64];
} __attribute__((packed));

struct NativeStateSectionHeader
{
    u32 tag;
    u32 size;
    u32 crc;
} __attribute__((packed));

struct NativeStateSlice
{
    u32 tag;
    const void *source;
    u32 size;
    bool32 runtimePointers;
};

struct NativeStateBattleSidecar
{
    struct HostPersistentAddress commandBufferData[MAX_BATTLERS_COUNT];
};

extern unsigned char __start_gba_ewram[];
extern unsigned char __stop_gba_ewram[];
extern unsigned char __start_gba_iwram[];
extern unsigned char __stop_gba_iwram[];
extern unsigned char __start_gba_common[];
extern unsigned char __stop_gba_common[];
extern unsigned char __start_host_data[];
extern unsigned char __stop_host_data[];
extern u8 apuCycle;
extern const u8 *gAIScriptPtr;
#if defined(LINUX64) && LINUX64
extern struct SpriteTemplate sSpriteTemplateSidecars[MAX_SPRITES + 1];
extern struct SpriteFrameImage sSpriteTemplateImageSidecars[MAX_SPRITES + 1];
#endif

#if UINTPTR_MAX > UINT32_MAX
/* Runtime normalization walks at the GBA's four-byte word stride. That also
 * covers native pointers in vanilla task-data overlays whose host alignment is
 * only four bytes. Keep the explicitly modeled layouts honest as they change. */
STATIC_ASSERT(offsetof(struct SpriteTemplate, oam) % sizeof(uintptr_t) == 0, NativeStateSpriteTemplateOamAligned);
STATIC_ASSERT(offsetof(struct SpriteTemplate, anims) % sizeof(uintptr_t) == 0, NativeStateSpriteTemplateAnimsAligned);
STATIC_ASSERT(offsetof(struct SpriteTemplate, images) % sizeof(uintptr_t) == 0, NativeStateSpriteTemplateImagesAligned);
STATIC_ASSERT(offsetof(struct SpriteTemplate, affineAnims) % sizeof(uintptr_t) == 0, NativeStateSpriteTemplateAffineAnimsAligned);
STATIC_ASSERT(offsetof(struct SpriteTemplate, callback) % sizeof(uintptr_t) == 0, NativeStateSpriteTemplateCallbackAligned);
STATIC_ASSERT(offsetof(struct SpriteFrameImage, data) % sizeof(uintptr_t) == 0, NativeStateSpriteFrameImageDataAligned);
STATIC_ASSERT(offsetof(struct Sprite, anims) % sizeof(uintptr_t) == 0, NativeStateSpriteAnimsAligned);
STATIC_ASSERT(offsetof(struct Sprite, images) % sizeof(uintptr_t) == 0, NativeStateSpriteImagesAligned);
STATIC_ASSERT(offsetof(struct Sprite, affineAnims) % sizeof(uintptr_t) == 0, NativeStateSpriteAffineAnimsAligned);
STATIC_ASSERT(offsetof(struct Sprite, template) % sizeof(uintptr_t) == 0, NativeStateSpriteTemplateAligned);
STATIC_ASSERT(offsetof(struct Sprite, subspriteTables) % sizeof(uintptr_t) == 0, NativeStateSpriteSubspritesAligned);
STATIC_ASSERT(offsetof(struct Sprite, callback) % sizeof(uintptr_t) == 0, NativeStateSpriteCallbackAligned);
STATIC_ASSERT((offsetof(struct TextPrinter, printerTemplate)
             + offsetof(struct TextPrinterTemplate, currentChar)) % sizeof(uintptr_t) == 0,
             NativeStateTextPrinterCurrentCharAligned);
STATIC_ASSERT(offsetof(struct TextPrinter, callback) % sizeof(uintptr_t) == 0, NativeStateTextPrinterCallbackAligned);
STATIC_ASSERT(offsetof(struct Window, tileData) % sizeof(uintptr_t) == 0, NativeStateWindowTileDataAligned);
STATIC_ASSERT(offsetof(struct Task, func) % sizeof(uintptr_t) == 0, NativeStateTaskFuncAligned);
STATIC_ASSERT(offsetof(struct Main, callback1) % sizeof(uintptr_t) == 0, NativeStateMainCallback1Aligned);
STATIC_ASSERT(offsetof(struct Main, callback2) % sizeof(uintptr_t) == 0, NativeStateMainCallback2Aligned);
STATIC_ASSERT(offsetof(struct Main, savedCallback) % sizeof(uintptr_t) == 0, NativeStateMainSavedCallbackAligned);
STATIC_ASSERT(offsetof(struct Main, vblankCallback) % sizeof(uintptr_t) == 0, NativeStateMainVBlankCallbackAligned);
STATIC_ASSERT(offsetof(struct Main, hblankCallback) % sizeof(uintptr_t) == 0, NativeStateMainHBlankCallbackAligned);
STATIC_ASSERT(offsetof(struct Main, vcountCallback) % sizeof(uintptr_t) == 0, NativeStateMainVCountCallbackAligned);
STATIC_ASSERT(offsetof(struct Main, serialCallback) % sizeof(uintptr_t) == 0, NativeStateMainSerialCallbackAligned);
STATIC_ASSERT(offsetof(struct BattlePokemon, statStages) == 0x18, NativeStateBattlePokemonStatStagesOffset);
STATIC_ASSERT(offsetof(struct BattleStruct, savedCallback) % sizeof(uintptr_t) == 0, NativeStateBattleSavedCallbackAligned);
STATIC_ASSERT(offsetof(struct BattleScriptsStack, ptr) % sizeof(uintptr_t) == 0, NativeStateBattleScriptStackAligned);
STATIC_ASSERT(offsetof(struct BattleCallbacksStack, function) % sizeof(uintptr_t) == 0, NativeStateBattleCallbackStackAligned);
#endif

/* Keep every logical section present in small or feature-reduced builds. The
 * anchors are part of the section and are harmless state bytes. */
EWRAM_DATA static u8 sNativeStateEwramAnchor __attribute__((used));
IWRAM_DATA static u8 sNativeStateIwramAnchor __attribute__((used));
COMMON_DATA static u8 sNativeStateCommonAnchor __attribute__((used));

#define NATIVE_STATE_ERROR_LENGTH 1024
HOST_DATA static char sLastError[NATIVE_STATE_ERROR_LENGTH];

static void SetError(const char *message)
{
    snprintf(sLastError, sizeof(sLastError), "%s", message != NULL ? message : "unknown state error");
}

static bool32 GetNativeBuildId(char *dest, u32 destSize)
{
    return Platform_RuntimeGetBuildId(NATIVE_STATE_ABI_ID, dest, destSize);
}

const char *NativeState_GetLastError(void)
{
    return sLastError;
}

static u32 Crc32(const void *data, u32 size)
{
    const u8 *bytes = data;
    u32 crc = 0xFFFFFFFFu;
    u32 i;
    int bit;

    for (i = 0; i < size; i++)
    {
        crc ^= bytes[i];
        for (bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)-(s32)(crc & 1));
    }
    return ~crc;
}

static u32 SectionSize(const unsigned char *start, const unsigned char *stop)
{
    uintptr_t begin = (uintptr_t)start;
    uintptr_t end = (uintptr_t)stop;
    return end >= begin && end - begin <= UINT32_MAX ? (u32)(end - begin) : 0;
}

static u32 BuildSlices(struct NativeStateSlice *slices, u32 capacity,
                       u8 *framebuffer, struct SiiRtcInfo *rtc)
{
    u32 count = 0;
    uintptr_t gameBssStart;
    uintptr_t gameBssEnd;
    uintptr_t gameDataStart;
    uintptr_t gameDataEnd;

    if (!Platform_RuntimeGetGameBssRange(&gameBssStart, &gameBssEnd))
        gameBssStart = gameBssEnd = 0;
    if (!Platform_RuntimeGetGameDataRange(&gameDataStart, &gameDataEnd))
        gameDataStart = gameDataEnd = 0;

#define ADD_SLICE(sliceTag, sliceSource, sliceSize, pointerState) \
    do { \
        if (count < capacity) { \
            slices[count].tag = (sliceTag); \
            slices[count].source = (sliceSource); \
            slices[count].size = (sliceSize); \
            slices[count].runtimePointers = (pointerState); \
            count++; \
        } \
    } while (0)

    Platform_ClockCopyState(rtc, sizeof(*rtc));
    Platform_VideoCopyFramebuffer(framebuffer, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(u32));

    /* These ranges are emitted by ld_script_native.ld from the same game-owned
     * object inputs as the GBA linker. They deliberately exclude the native
     * executable .bss/.data, including libc COPY relocations and platform
     * state. */
    ADD_SLICE(STATE_SECTION_GAME_BSS, (const void *)gameBssStart,
              gameBssEnd >= gameBssStart && gameBssEnd - gameBssStart <= UINT32_MAX
              ? (u32)(gameBssEnd - gameBssStart) : 0, TRUE);
    ADD_SLICE(STATE_SECTION_REGISTERS, REG_BASE, 0x400, FALSE);
    ADD_SLICE(STATE_SECTION_VIDEO_MEMORY, VRAM_, sizeof(VRAM_) + sizeof(PLTT) + sizeof(OAM), FALSE);
    /* The three video arrays are not adjacent, so their combined slice is
     * written by CaptureSlice rather than treating this descriptor as a range. */
    slices[count - 1].source = NULL;
    slices[count - 1].size = sizeof(VRAM_) + sizeof(PLTT) + sizeof(OAM);
    ADD_SLICE(STATE_SECTION_FLASH, FLASH_BASE, sizeof(FLASH_BASE), FALSE);
    ADD_SLICE(STATE_SECTION_EWRAM, __start_gba_ewram,
              SectionSize(__start_gba_ewram, __stop_gba_ewram), TRUE);
    ADD_SLICE(STATE_SECTION_IWRAM, __start_gba_iwram,
              SectionSize(__start_gba_iwram, __stop_gba_iwram), TRUE);
    ADD_SLICE(STATE_SECTION_COMMON, __start_gba_common,
              SectionSize(__start_gba_common, __stop_gba_common), TRUE);
    ADD_SLICE(STATE_SECTION_GAME_DATA, (const void *)gameDataStart,
              gameDataEnd >= gameDataStart && gameDataEnd - gameDataStart <= UINT32_MAX
              ? (u32)(gameDataEnd - gameDataStart) : 0, TRUE);
    ADD_SLICE(STATE_SECTION_FRAMEBUFFER, framebuffer,
              DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(u32), FALSE);
    ADD_SLICE(STATE_SECTION_TASK_SIDECAR, NULL, Task_GetStateSize(), FALSE);
    ADD_SLICE(STATE_SECTION_SPRITE_SIDECAR, NULL, Sprite_GetStateSize(), FALSE);
    ADD_SLICE(STATE_SECTION_BATTLE_SIDECAR, NULL,
              sizeof(struct NativeStateBattleSidecar), FALSE);
    ADD_SLICE(STATE_SECTION_RTC, rtc, sizeof(*rtc), FALSE);

#undef ADD_SLICE
    return count;
}

static bool32 IsExecutableOrGameAddress(uintptr_t value)
{
    uintptr_t start;
    uintptr_t end;
    uintptr_t bssStart;
    uintptr_t bssEnd;
    uintptr_t gameBssStart;
    uintptr_t gameBssEnd;
    uintptr_t hostStart = (uintptr_t)__start_host_data;
    uintptr_t hostEnd = (uintptr_t)__stop_host_data;

    if (!Platform_RuntimeGetImageRange(&start, &end)
     || value < start || value >= end
     || (value >= hostStart && value < hostEnd))
        return FALSE;
    /* The native ELF's ordinary BSS contains platform state and libc COPY
     * relocations. Only the linker-owned game_bss range is a persistent game
     * address range; this keeps host globals from becoming valid pointer roots. */
    if (Platform_RuntimeGetBssRange(&bssStart, &bssEnd)
     && value >= bssStart && value < bssEnd)
        return Platform_RuntimeGetGameBssRange(&gameBssStart, &gameBssEnd)
            && value >= gameBssStart && value < gameBssEnd;
    return TRUE;
}

static bool32 IsMappedHostAddress(uintptr_t value)
{
    return Platform_RuntimeAddressIsMapped(value);
}

static bool32 LooksLikeExternalHostPointer(uintptr_t value)
{
#if UINTPTR_MAX > UINT32_MAX
    /* The non-PIE game image and its logical buffers are below 4 GiB. Linux
     * host allocations use the canonical high user-address range. Values in
     * between are ordinary packed GBA fields (for example the heap header's
     * flag/magic/size bytes viewed as one eight-byte word), not pointers. */
    return value >= UINT64_C(0x0000500000000000)
        && value < UINT64_C(0x0000800000000000)
        && IsMappedHostAddress(value);
#else
    (void)value;
    return FALSE;
#endif
}

static const char *SectionName(u32 tag)
{
    switch (tag)
    {
    case STATE_SECTION_GAME_BSS:       return "GAME_BSS";
    case STATE_SECTION_REGISTERS:      return "REGISTERS";
    case STATE_SECTION_VIDEO_MEMORY:   return "VIDEO_MEMORY";
    case STATE_SECTION_FLASH:          return "FLASH";
    case STATE_SECTION_EWRAM:          return "EWRAM";
    case STATE_SECTION_IWRAM:          return "IWRAM";
    case STATE_SECTION_COMMON:         return "COMMON";
    case STATE_SECTION_GAME_DATA:      return "GAME_DATA";
    case STATE_SECTION_FRAMEBUFFER:    return "FRAMEBUFFER";
    case STATE_SECTION_TASK_SIDECAR:   return "TASK_SIDECAR";
    case STATE_SECTION_SPRITE_SIDECAR: return "SPRITE_SIDECAR";
    case STATE_SECTION_BATTLE_SIDECAR: return "BATTLE_SIDECAR";
    case STATE_SECTION_RTC:            return "RTC";
    default:                           return "UNKNOWN";
    }
}

#if defined(__linux__) && UINTPTR_MAX > UINT32_MAX
struct NativeStateSymbolInfo
{
    uintptr_t address;
    uintptr_t size;
    u8 type;
    char name[96];
};

/* Release builds retain their ordinary ELF symbol table. Consult it only on a
 * pointer-audit failure so even file-local EWRAM roots get a useful nearest
 * symbol without maintaining a hand-written list of hundreds of declarations. */
static bool32 FindExecutableObjectSymbol(uintptr_t address, struct NativeStateSymbolInfo *result)
{
    FILE *file = NULL;
    Elf64_Ehdr header;
    Elf64_Shdr *sections = NULL;
    char *strings = NULL;
    u32 targetSection = UINT32_MAX;
    uintptr_t bestAddress = 0;
    uintptr_t bestSize = 0;
    u32 bestName = 0;
    u8 bestType = STT_NOTYPE;
    bool32 bestContains = FALSE;
    bool32 found = FALSE;
    u32 i;

    if (result == NULL)
        return FALSE;
    file = fopen("/proc/self/exe", "rb");
    if (file == NULL || fread(&header, sizeof(header), 1, file) != 1
     || memcmp(header.e_ident, ELFMAG, SELFMAG) != 0
     || header.e_ident[EI_CLASS] != ELFCLASS64
     || header.e_type != ET_EXEC
     || header.e_shentsize != sizeof(Elf64_Shdr)
     || header.e_shnum == 0)
        goto finish;

    sections = malloc((size_t)header.e_shnum * sizeof(*sections));
    if (sections == NULL
     || fseek(file, (long)header.e_shoff, SEEK_SET) != 0
     || fread(sections, sizeof(*sections), header.e_shnum, file) != header.e_shnum)
        goto finish;

    for (i = 0; i < header.e_shnum; i++)
    {
        if ((sections[i].sh_flags & SHF_ALLOC) != 0
         && address >= sections[i].sh_addr
         && address - sections[i].sh_addr < sections[i].sh_size)
        {
            targetSection = i;
            break;
        }
    }
    if (targetSection == UINT32_MAX)
        goto finish;

    for (i = 0; i < header.e_shnum; i++)
    {
        Elf64_Shdr *symbols = &sections[i];
        Elf64_Shdr *stringSection;
        Elf64_Sym symbol;
        u64 count;
        u64 j;

        if (symbols->sh_type != SHT_SYMTAB || symbols->sh_entsize != sizeof(symbol)
         || symbols->sh_link >= header.e_shnum)
            continue;
        stringSection = &sections[symbols->sh_link];
        if (stringSection->sh_type != SHT_STRTAB || stringSection->sh_size == 0
         || stringSection->sh_size > 32u * 1024u * 1024u)
            continue;
        strings = malloc((size_t)stringSection->sh_size);
        if (strings == NULL
         || fseek(file, (long)stringSection->sh_offset, SEEK_SET) != 0
         || fread(strings, 1, (size_t)stringSection->sh_size, file) != stringSection->sh_size
         || fseek(file, (long)symbols->sh_offset, SEEK_SET) != 0)
            goto finish;

        count = symbols->sh_size / symbols->sh_entsize;
        for (j = 0; j < count; j++)
        {
            bool32 contains;
            u8 type;

            if (fread(&symbol, sizeof(symbol), 1, file) != 1)
                goto finish;
            type = ELF64_ST_TYPE(symbol.st_info);
            if (symbol.st_shndx != targetSection || symbol.st_name == 0
             || symbol.st_name >= stringSection->sh_size
             || (type != STT_OBJECT && type != STT_NOTYPE && type != STT_FUNC)
             || symbol.st_value > address
             || memchr(strings + symbol.st_name, '\0',
                       (size_t)stringSection->sh_size - symbol.st_name) == NULL)
                continue;
            contains = symbol.st_size != 0 && address - symbol.st_value < symbol.st_size;
            if (!found
             || (contains && !bestContains)
             || (contains == bestContains && symbol.st_value > bestAddress))
            {
                found = TRUE;
                bestContains = contains;
                bestAddress = symbol.st_value;
                bestSize = symbol.st_size;
                bestName = symbol.st_name;
                bestType = type;
            }
        }
        if (found)
        {
            result->address = bestAddress;
            result->size = bestSize;
            result->type = bestType;
            snprintf(result->name, sizeof(result->name), "%s", strings + bestName);
        }
        break;
    }

finish:
    free(strings);
    free(sections);
    if (file != NULL)
        fclose(file);
    return found;
}
#endif

static void DescribeNearestSymbol(uintptr_t address, char *symbol, u32 symbolSize)
{
    snprintf(symbol, symbolSize, "unknown");
#if defined(__linux__) && UINTPTR_MAX > UINT32_MAX
    {
        struct NativeStateSymbolInfo info;

        if (FindExecutableObjectSymbol(address, &info))
        {
            snprintf(symbol, symbolSize, "%s+0x%zx", info.name, address - info.address);
            return;
        }
    }
#else
    (void)address;
#endif
}

struct NativeBattleBufferLocation
{
    const char *name;
    const u8 *buffer;
    u32 battler;
    u32 offset;
    u8 command;
    bool32 toController;
};

static bool32 GetBattleBufferLocation(uintptr_t address,
                                      struct NativeBattleBufferLocation *location)
{
    uintptr_t relative;

    if (address >= (uintptr_t)gBattleBufferA
     && address < (uintptr_t)gBattleBufferA + sizeof(gBattleBufferA))
    {
        relative = address - (uintptr_t)gBattleBufferA;
        location->name = "gBattleBufferA";
        location->battler = (u32)(relative / sizeof(gBattleBufferA[0]));
        location->offset = (u32)(relative % sizeof(gBattleBufferA[0]));
        location->buffer = gBattleBufferA[location->battler];
        location->command = location->buffer[0];
        location->toController = TRUE;
        return TRUE;
    }
    if (address >= (uintptr_t)gBattleBufferB
     && address < (uintptr_t)gBattleBufferB + sizeof(gBattleBufferB))
    {
        relative = address - (uintptr_t)gBattleBufferB;
        location->name = "gBattleBufferB";
        location->battler = (u32)(relative / sizeof(gBattleBufferB[0]));
        location->offset = (u32)(relative % sizeof(gBattleBufferB[0]));
        location->buffer = gBattleBufferB[location->battler];
        location->command = location->buffer[0];
        location->toController = FALSE;
        return TRUE;
    }
    return FALSE;
}

static const char *BattleControllerCommandName(u8 command)
{
    switch (command)
    {
    case CONTROLLER_GETMONDATA:              return "CONTROLLER_GETMONDATA";
    case CONTROLLER_MOVEANIMATION:           return "CONTROLLER_MOVEANIMATION";
    case CONTROLLER_PRINTSTRING:             return "CONTROLLER_PRINTSTRING";
    case CONTROLLER_PRINTSTRINGPLAYERONLY:   return "CONTROLLER_PRINTSTRINGPLAYERONLY";
    case CONTROLLER_CHOOSEACTION:            return "CONTROLLER_CHOOSEACTION";
    case CONTROLLER_CHOOSEMOVE:              return "CONTROLLER_CHOOSEMOVE";
    case CONTROLLER_OPENBAG:                 return "CONTROLLER_OPENBAG";
    case CONTROLLER_CHOOSEPOKEMON:           return "CONTROLLER_CHOOSEPOKEMON";
    case CONTROLLER_DATATRANSFER:             return "CONTROLLER_DATATRANSFER";
    case CONTROLLER_DMA3TRANSFER:             return "CONTROLLER_DMA3TRANSFER";
    case CONTROLLER_TWORETURNVALUES:          return "CONTROLLER_TWORETURNVALUES";
    case CONTROLLER_CHOSENMONRETURNVALUE:     return "CONTROLLER_CHOSENMONRETURNVALUE";
    case CONTROLLER_ONERETURNVALUE:           return "CONTROLLER_ONERETURNVALUE";
    case CONTROLLER_ONERETURNVALUE_DUPLICATE: return "CONTROLLER_ONERETURNVALUE_DUPLICATE";
    case CONTROLLER_TERMINATOR_NOP:           return "CONTROLLER_TERMINATOR_NOP";
    default:                                  return "other controller command";
    }
}

static void DescribeBattlePokemonField(u32 offset, char *field, u32 fieldSize)
{
#define BATTLE_MON_SCALAR(member) \
    if (offset == offsetof(struct BattlePokemon, member)) \
        snprintf(field, fieldSize, #member)
#define BATTLE_MON_ARRAY(member) \
    if (offset >= offsetof(struct BattlePokemon, member) \
     && offset < offsetof(struct BattlePokemon, member) \
               + sizeof(((struct BattlePokemon *)0)->member)) \
        snprintf(field, fieldSize, #member "[%u]", \
                 offset - (u32)offsetof(struct BattlePokemon, member))

    BATTLE_MON_SCALAR(species);
    else BATTLE_MON_SCALAR(attack);
    else BATTLE_MON_SCALAR(defense);
    else BATTLE_MON_SCALAR(speed);
    else BATTLE_MON_SCALAR(spAttack);
    else BATTLE_MON_SCALAR(spDefense);
    else BATTLE_MON_ARRAY(moves);
    else if (offset >= offsetof(struct BattlePokemon, moves)
                         + sizeof(((struct BattlePokemon *)0)->moves)
          && offset < offsetof(struct BattlePokemon, statStages))
        snprintf(field, fieldSize, "packed IV/egg/ability bits+0x%x",
                 offset - (u32)(offsetof(struct BattlePokemon, moves)
                              + sizeof(((struct BattlePokemon *)0)->moves)));
    else BATTLE_MON_ARRAY(statStages);
    else BATTLE_MON_SCALAR(ability);
    else BATTLE_MON_ARRAY(types);
    else BATTLE_MON_SCALAR(unknown);
    else BATTLE_MON_ARRAY(pp);
    else BATTLE_MON_SCALAR(hp);
    else BATTLE_MON_SCALAR(level);
    else BATTLE_MON_SCALAR(friendship);
    else BATTLE_MON_SCALAR(maxHP);
    else BATTLE_MON_SCALAR(item);
    else BATTLE_MON_ARRAY(nickname);
    else BATTLE_MON_SCALAR(ppBonuses);
    else BATTLE_MON_ARRAY(otName);
    else BATTLE_MON_SCALAR(experience);
    else BATTLE_MON_SCALAR(personality);
    else BATTLE_MON_SCALAR(status1);
    else BATTLE_MON_SCALAR(status2);
    else BATTLE_MON_SCALAR(otId);
    else
        snprintf(field, fieldSize, "+0x%x", offset);

#undef BATTLE_MON_ARRAY
#undef BATTLE_MON_SCALAR
}

static void DescribeBattleBufferLocation(const struct NativeBattleBufferLocation *location,
                                         char *owner, u32 ownerSize,
                                         char *field, u32 fieldSize)
{
    const char *direction = location->toController ? "controller command buffer" : "controller reply buffer";
    const char *command = BattleControllerCommandName(location->command);

    snprintf(owner, ownerSize, "%s[%u] (%s, %s)", location->name,
             location->battler, direction, command);
    if (location->offset == 0)
    {
        snprintf(field, fieldSize, "command");
        return;
    }
    if (!location->toController && location->command == CONTROLLER_DATATRANSFER)
    {
        u32 payloadSize = location->buffer[2] | (location->buffer[3] << 8);

        if (location->offset == 1)
            snprintf(field, fieldSize, "dataTransfer.reserved");
        else if (location->offset == 2)
            snprintf(field, fieldSize, "dataTransfer.size.lo");
        else if (location->offset == 3)
            snprintf(field, fieldSize, "dataTransfer.size.hi");
        else if (payloadSize != 0 && payloadSize % sizeof(struct BattlePokemon) == 0)
        {
            u32 payloadOffset = location->offset - 4;
            u32 element = payloadOffset / sizeof(struct BattlePokemon);
            u32 memberOffset = payloadOffset % sizeof(struct BattlePokemon);
            char member[64];

            DescribeBattlePokemonField(memberOffset, member, sizeof(member));
            snprintf(field, fieldSize, "dataTransfer.payload[%u].%s (struct BattlePokemon)",
                     element, member);
        }
        else
            snprintf(field, fieldSize, "dataTransfer.payload+0x%x", location->offset - 4);
        return;
    }
    if (location->toController && location->command == CONTROLLER_DMA3TRANSFER
     && location->offset >= 1 && location->offset < 1 + sizeof(GbaAddr))
    {
        snprintf(field, fieldSize, "dma3Transfer.destination (GbaAddr)+0x%x",
                 location->offset - 1);
        return;
    }
    snprintf(field, fieldSize, "protocol byte +0x%x", location->offset);
}

static bool32 AddressInObject(uintptr_t address, const void *object, size_t size)
{
    uintptr_t start = (uintptr_t)object;
    uintptr_t imageStart;
    uintptr_t imageEnd;

    return object != NULL && IsExecutableOrGameAddress(start)
        && Platform_RuntimeGetImageRange(&imageStart, &imageEnd)
        && start >= imageStart && size <= imageEnd - start
        && address >= start && address - start < size;
}

static bool32 DescribeBattleRuntimeLocation(uintptr_t address,
                                            char *owner, u32 ownerSize,
                                            char *field, u32 fieldSize)
{
    struct NativeBattleBufferLocation buffer;
    u32 index;

    if (GetBattleBufferLocation(address, &buffer))
    {
        DescribeBattleBufferLocation(&buffer, owner, ownerSize, field, fieldSize);
        return TRUE;
    }
#define BATTLE_ROOT(pointer, label) \
    if (address == (uintptr_t)&(pointer)) { \
        snprintf(owner, ownerSize, label); \
        snprintf(field, fieldSize, #pointer); \
        return TRUE; \
    }
    BATTLE_ROOT(gBattleAnimBgTileBuffer, "battle graphics root");
    BATTLE_ROOT(gBattleAnimBgTilemapBuffer, "battle graphics root");
    BATTLE_ROOT(gBattlescriptCurrInstr, "battle script interpreter state");
    BATTLE_ROOT(gBattleStruct, "battle allocation root");
    BATTLE_ROOT(gLinkBattleSendBuffer, "link battle command-buffer root");
    BATTLE_ROOT(gLinkBattleRecvBuffer, "link battle command-buffer root");
    BATTLE_ROOT(gBattleResources, "battle resource root");
    BATTLE_ROOT(gBattleSpritesDataPtr, "battle interface root");
    BATTLE_ROOT(gMonSpritesGfxPtr, "battle interface graphics root");
    BATTLE_ROOT(gBattleControllerOpponentHealthboxData, "battle interface root");
    BATTLE_ROOT(gBattleControllerOpponentFlankHealthboxData, "battle interface root");
    BATTLE_ROOT(gBattleMsgDataPtr, "derived battle-buffer pointer");
    BATTLE_ROOT(gAnimDisableStructPtr, "derived battle animation command pointer");
    BATTLE_ROOT(gAIScriptPtr, "battle AI script interpreter state");
    BATTLE_ROOT(gAnimScriptCallback, "battle animation callback state");
    BATTLE_ROOT(gPreBattleCallback1, "battle callback state");
    BATTLE_ROOT(gBattleMainFunc, "battle callback state");
#undef BATTLE_ROOT
    if (address >= (uintptr_t)gSelectionBattleScripts
     && address < (uintptr_t)gSelectionBattleScripts + sizeof(gSelectionBattleScripts))
    {
        index = (u32)((address - (uintptr_t)gSelectionBattleScripts) / sizeof(gSelectionBattleScripts[0]));
        snprintf(owner, ownerSize, "battle selection script state");
        snprintf(field, fieldSize, "gSelectionBattleScripts[%u]", index);
        return TRUE;
    }
    if (address >= (uintptr_t)gPalaceSelectionBattleScripts
     && address < (uintptr_t)gPalaceSelectionBattleScripts + sizeof(gPalaceSelectionBattleScripts))
    {
        index = (u32)((address - (uintptr_t)gPalaceSelectionBattleScripts) / sizeof(gPalaceSelectionBattleScripts[0]));
        snprintf(owner, ownerSize, "Battle Palace selection script state");
        snprintf(field, fieldSize, "gPalaceSelectionBattleScripts[%u]", index);
        return TRUE;
    }
    if (address >= (uintptr_t)gBattlerControllerFuncs
     && address < (uintptr_t)gBattlerControllerFuncs + sizeof(gBattlerControllerFuncs))
    {
        index = (u32)((address - (uintptr_t)gBattlerControllerFuncs) / sizeof(gBattlerControllerFuncs[0]));
        snprintf(owner, ownerSize, "battle controller callback state");
        snprintf(field, fieldSize, "gBattlerControllerFuncs[%u]", index);
        return TRUE;
    }
    if (AddressInObject(address, gBattleStruct, sizeof(*gBattleStruct)))
    {
        snprintf(owner, ownerSize, "*gBattleStruct (struct BattleStruct)");
        if (address - (uintptr_t)gBattleStruct == offsetof(struct BattleStruct, savedCallback))
            snprintf(field, fieldSize, "savedCallback");
        else
            snprintf(field, fieldSize, "+0x%zx", address - (uintptr_t)gBattleStruct);
        return TRUE;
    }
    if (AddressInObject(address, gBattleResources, sizeof(*gBattleResources)))
    {
        static const char *const fields[] =
        {
            "secretBase", "flags", "battleScriptsStack", "battleCallbackStack",
            "beforeLvlUp", "ai", "battleHistory", "AI_ScriptsStack"
        };
        index = (u32)((address - (uintptr_t)gBattleResources) / sizeof(void *));
        snprintf(owner, ownerSize, "*gBattleResources (struct BattleResources)");
        snprintf(field, fieldSize, "%s", index < ARRAY_COUNT(fields) ? fields[index] : "unknown");
        return TRUE;
    }
    if (AddressInObject((uintptr_t)gBattleResources, gBattleResources,
                        sizeof(*gBattleResources))
     && AddressInObject(address, gBattleResources->battleScriptsStack,
                        sizeof(*gBattleResources->battleScriptsStack)))
    {
        uintptr_t relative = address - (uintptr_t)gBattleResources->battleScriptsStack;

        snprintf(owner, ownerSize, "gBattleResources->battleScriptsStack (struct BattleScriptsStack)");
        if (relative < sizeof(gBattleResources->battleScriptsStack->ptr))
            snprintf(field, fieldSize, "ptr[%u]", (u32)(relative / sizeof(void *)));
        else
            snprintf(field, fieldSize, "+0x%zx", relative);
        return TRUE;
    }
    if (AddressInObject((uintptr_t)gBattleResources, gBattleResources,
                        sizeof(*gBattleResources))
     && AddressInObject(address, gBattleResources->AI_ScriptsStack,
                        sizeof(*gBattleResources->AI_ScriptsStack)))
    {
        uintptr_t relative = address - (uintptr_t)gBattleResources->AI_ScriptsStack;

        snprintf(owner, ownerSize, "gBattleResources->AI_ScriptsStack (struct BattleScriptsStack)");
        if (relative < sizeof(gBattleResources->AI_ScriptsStack->ptr))
            snprintf(field, fieldSize, "ptr[%u]", (u32)(relative / sizeof(void *)));
        else
            snprintf(field, fieldSize, "+0x%zx", relative);
        return TRUE;
    }
    if (AddressInObject((uintptr_t)gBattleResources, gBattleResources,
                        sizeof(*gBattleResources))
     && AddressInObject(address, gBattleResources->battleCallbackStack,
                        sizeof(*gBattleResources->battleCallbackStack)))
    {
        uintptr_t relative = address - (uintptr_t)gBattleResources->battleCallbackStack;

        snprintf(owner, ownerSize, "gBattleResources->battleCallbackStack (struct BattleCallbacksStack)");
        if (relative < sizeof(gBattleResources->battleCallbackStack->function))
            snprintf(field, fieldSize, "function[%u]", (u32)(relative / sizeof(void *)));
        else
            snprintf(field, fieldSize, "+0x%zx", relative);
        return TRUE;
    }
    if (AddressInObject(address, gBattleSpritesDataPtr, sizeof(*gBattleSpritesDataPtr)))
    {
        static const char *const fields[] =
        {
            "battlerData", "healthBoxesData", "animationData", "battleBars"
        };
        index = (u32)((address - (uintptr_t)gBattleSpritesDataPtr) / sizeof(void *));
        snprintf(owner, ownerSize, "*gBattleSpritesDataPtr (struct BattleSpriteData)");
        snprintf(field, fieldSize, "%s", index < ARRAY_COUNT(fields) ? fields[index] : "unknown");
        return TRUE;
    }
    return FALSE;
}

static bool32 BattleRuntimeLocationIsFunction(uintptr_t address)
{
    u32 i;

    if (address == (uintptr_t)&gPreBattleCallback1
     || address == (uintptr_t)&gBattleMainFunc
     || address == (uintptr_t)&gAnimScriptCallback)
        return TRUE;
    if (address >= (uintptr_t)gBattlerControllerFuncs
     && address < (uintptr_t)gBattlerControllerFuncs + sizeof(gBattlerControllerFuncs)
     && (address - (uintptr_t)gBattlerControllerFuncs) % sizeof(void *) == 0)
        return TRUE;
    if (AddressInObject(address, gBattleStruct, sizeof(*gBattleStruct))
     && address - (uintptr_t)gBattleStruct == offsetof(struct BattleStruct, savedCallback))
        return TRUE;
    if (AddressInObject((uintptr_t)gBattleResources, gBattleResources,
                        sizeof(*gBattleResources))
     && AddressInObject(address, gBattleResources->battleCallbackStack,
                        sizeof(*gBattleResources->battleCallbackStack)))
    {
        uintptr_t relative = address - (uintptr_t)gBattleResources->battleCallbackStack;

        return relative < sizeof(gBattleResources->battleCallbackStack->function)
            && relative % sizeof(void *) == 0;
    }
    if (AddressInObject(address, gMonSpritesGfxPtr, sizeof(*gMonSpritesGfxPtr)))
    {
        uintptr_t relative = address - (uintptr_t)gMonSpritesGfxPtr;

        for (i = 0; i < MAX_BATTLERS_COUNT; i++)
        {
            if (relative == offsetof(struct MonSpritesGfx, templates)
                         + i * sizeof(struct SpriteTemplate)
                         + offsetof(struct SpriteTemplate, callback))
                return TRUE;
        }
    }
    return FALSE;
}

static bool32 BattleRuntimeLocationIsKnownDataPointer(uintptr_t address)
{
    if (address == (uintptr_t)&gBattleAnimBgTileBuffer
     || address == (uintptr_t)&gBattleAnimBgTilemapBuffer
     || address == (uintptr_t)&gBattlescriptCurrInstr
     || address == (uintptr_t)&gBattleStruct
     || address == (uintptr_t)&gLinkBattleSendBuffer
     || address == (uintptr_t)&gLinkBattleRecvBuffer
     || address == (uintptr_t)&gBattleResources
     || address == (uintptr_t)&gBattleSpritesDataPtr
     || address == (uintptr_t)&gMonSpritesGfxPtr
     || address == (uintptr_t)&gBattleControllerOpponentHealthboxData
     || address == (uintptr_t)&gBattleControllerOpponentFlankHealthboxData
     || address == (uintptr_t)&gBattleMsgDataPtr
     || address == (uintptr_t)&gAnimDisableStructPtr
     || address == (uintptr_t)&gAIScriptPtr)
        return TRUE;
    if (address >= (uintptr_t)gSelectionBattleScripts
     && address < (uintptr_t)gSelectionBattleScripts + sizeof(gSelectionBattleScripts)
     && (address - (uintptr_t)gSelectionBattleScripts) % sizeof(void *) == 0)
        return TRUE;
    if (address >= (uintptr_t)gPalaceSelectionBattleScripts
     && address < (uintptr_t)gPalaceSelectionBattleScripts + sizeof(gPalaceSelectionBattleScripts)
     && (address - (uintptr_t)gPalaceSelectionBattleScripts) % sizeof(void *) == 0)
        return TRUE;
    if (AddressInObject(address, gBattleResources, sizeof(*gBattleResources))
     && (address - (uintptr_t)gBattleResources) % sizeof(void *) == 0)
        return TRUE;
    if (AddressInObject((uintptr_t)gBattleResources, gBattleResources,
                        sizeof(*gBattleResources))
     && AddressInObject(address, gBattleResources->battleScriptsStack,
                        sizeof(*gBattleResources->battleScriptsStack)))
    {
        uintptr_t relative = address - (uintptr_t)gBattleResources->battleScriptsStack;

        return relative < sizeof(gBattleResources->battleScriptsStack->ptr)
            && relative % sizeof(void *) == 0;
    }
    if (AddressInObject((uintptr_t)gBattleResources, gBattleResources,
                        sizeof(*gBattleResources))
     && AddressInObject(address, gBattleResources->AI_ScriptsStack,
                        sizeof(*gBattleResources->AI_ScriptsStack)))
    {
        uintptr_t relative = address - (uintptr_t)gBattleResources->AI_ScriptsStack;

        return relative < sizeof(gBattleResources->AI_ScriptsStack->ptr)
            && relative % sizeof(void *) == 0;
    }
    if (AddressInObject(address, gBattleSpritesDataPtr, sizeof(*gBattleSpritesDataPtr))
     && (address - (uintptr_t)gBattleSpritesDataPtr) % sizeof(void *) == 0)
        return TRUE;
    if (AddressInObject(address, gMonSpritesGfxPtr, sizeof(*gMonSpritesGfxPtr)))
    {
        uintptr_t relative = address - (uintptr_t)gMonSpritesGfxPtr;
        u32 i;
        u32 j;

        if (relative == offsetof(struct MonSpritesGfx, firstDecompressed)
         || relative == offsetof(struct MonSpritesGfx, barFontGfx)
         || relative == offsetof(struct MonSpritesGfx, unusedPtr)
         || relative == offsetof(struct MonSpritesGfx, buffer))
            return TRUE;
        for (i = 0; i < MAX_BATTLERS_COUNT; i++)
        {
            if (relative == offsetof(struct MonSpritesGfx, sprites) + i * sizeof(void *))
                return TRUE;
            if (relative == offsetof(struct MonSpritesGfx, templates)
                         + i * sizeof(struct SpriteTemplate) + offsetof(struct SpriteTemplate, oam)
             || relative == offsetof(struct MonSpritesGfx, templates)
                         + i * sizeof(struct SpriteTemplate) + offsetof(struct SpriteTemplate, anims)
             || relative == offsetof(struct MonSpritesGfx, templates)
                         + i * sizeof(struct SpriteTemplate) + offsetof(struct SpriteTemplate, images)
             || relative == offsetof(struct MonSpritesGfx, templates)
                         + i * sizeof(struct SpriteTemplate) + offsetof(struct SpriteTemplate, affineAnims))
                return TRUE;
            for (j = 0; j < MAX_MON_PIC_FRAMES; j++)
            {
                if (relative == offsetof(struct MonSpritesGfx, frameImages)
                             + (i * MAX_MON_PIC_FRAMES + j) * sizeof(struct SpriteFrameImage)
                             + offsetof(struct SpriteFrameImage, data))
                    return TRUE;
            }
        }
    }
    return FALSE;
}

static void DescribeRuntimeLocation(const struct NativeStateSlice *slice, u32 offset,
                                    char *symbol, u32 symbolSize,
                                    char *owner, u32 ownerSize,
                                    char *field, u32 fieldSize)
{
    uintptr_t address;

    snprintf(symbol, symbolSize, "unknown");
    snprintf(owner, ownerSize, "unknown");
    snprintf(field, fieldSize, "unknown");
    if (slice->tag == STATE_SECTION_TASK_SIDECAR
     && offset < slice->size
     && offset % sizeof(struct HostPersistentAddress) == 0)
    {
        u32 record = offset / (u32)sizeof(struct HostPersistentAddress);
        u32 taskId = record / TASK_STATE_FUNCTIONS_PER_TASK;
        u32 slot = record % TASK_STATE_FUNCTIONS_PER_TASK;

        snprintf(symbol, symbolSize, "%s+0x%x",
                 slot == 0 ? "sTaskFollowupFuncs" : "sTaskStoredFunctions", offset);
        snprintf(owner, ownerSize, "task sidecar task[%u]", taskId);
        snprintf(field, fieldSize, "%s", slot == 0 ? "followupFunc" : "storedFunction");
        return;
    }
    if (slice->tag == STATE_SECTION_SPRITE_SIDECAR
     && offset < slice->size
     && offset % sizeof(struct HostPersistentAddress) == 0)
    {
        snprintf(symbol, symbolSize, "sSpriteStoredCallbacks+0x%x", offset);
        snprintf(owner, ownerSize, "sprite sidecar entry[%u]",
                 offset / (u32)sizeof(struct HostPersistentAddress));
        snprintf(field, fieldSize, "storedCallback");
        return;
    }
    if (slice->tag == STATE_SECTION_BATTLE_SIDECAR
     && offset < slice->size
     && offset % sizeof(struct HostPersistentAddress) == 0)
    {
        u32 battler = offset / (u32)sizeof(struct HostPersistentAddress);

        snprintf(symbol, symbolSize, "battle command-buffer sidecar+0x%x", offset);
        snprintf(owner, ownerSize, "gBattleBufferA[%u] pointer sidecar", battler);
        snprintf(field, fieldSize, "dma3Transfer.destination");
        return;
    }
    if (slice->source == NULL || offset >= slice->size)
        return;

    address = (uintptr_t)slice->source + offset;
    DescribeNearestSymbol(address, symbol, symbolSize);
    if (DescribeBattleRuntimeLocation(address, owner, ownerSize, field, fieldSize))
        return;
#if defined(LINUX64) && LINUX64
    if (address >= (uintptr_t)sSpriteTemplateSidecars
     && address < (uintptr_t)sSpriteTemplateSidecars + sizeof(sSpriteTemplateSidecars))
    {
        uintptr_t relative = address - (uintptr_t)sSpriteTemplateSidecars;
        u32 spriteIndex = (u32)(relative / sizeof(struct SpriteTemplate));
        u32 fieldOffset = (u32)(relative % sizeof(struct SpriteTemplate));

        snprintf(owner, ownerSize, "sSpriteTemplateSidecars[%u] (struct SpriteTemplate)", spriteIndex);
        if (fieldOffset == offsetof(struct SpriteTemplate, oam))
            snprintf(field, fieldSize, "oam");
        else if (fieldOffset == offsetof(struct SpriteTemplate, anims))
            snprintf(field, fieldSize, "anims");
        else if (fieldOffset == offsetof(struct SpriteTemplate, images))
            snprintf(field, fieldSize, "images");
        else if (fieldOffset == offsetof(struct SpriteTemplate, affineAnims))
            snprintf(field, fieldSize, "affineAnims");
        else if (fieldOffset == offsetof(struct SpriteTemplate, callback))
            snprintf(field, fieldSize, "callback");
        else
            snprintf(field, fieldSize, "+0x%x", fieldOffset);
        return;
    }
    if (address >= (uintptr_t)sSpriteTemplateImageSidecars
     && address < (uintptr_t)sSpriteTemplateImageSidecars + sizeof(sSpriteTemplateImageSidecars))
    {
        uintptr_t relative = address - (uintptr_t)sSpriteTemplateImageSidecars;
        u32 spriteIndex = (u32)(relative / sizeof(struct SpriteFrameImage));
        u32 fieldOffset = (u32)(relative % sizeof(struct SpriteFrameImage));

        snprintf(owner, ownerSize, "sSpriteTemplateImageSidecars[%u] (struct SpriteFrameImage)", spriteIndex);
        if (fieldOffset == offsetof(struct SpriteFrameImage, data))
            snprintf(field, fieldSize, "data");
        else
            snprintf(field, fieldSize, "+0x%x", fieldOffset);
        return;
    }
#endif
    if (address >= (uintptr_t)gSprites
     && address < (uintptr_t)gSprites + sizeof(gSprites))
    {
        uintptr_t relative = address - (uintptr_t)gSprites;
        u32 spriteIndex = (u32)(relative / sizeof(struct Sprite));
        u32 fieldOffset = (u32)(relative % sizeof(struct Sprite));

        snprintf(owner, ownerSize, "gSprites[%u] (struct Sprite)", spriteIndex);
        if (fieldOffset == offsetof(struct Sprite, anims))
            snprintf(field, fieldSize, "anims");
        else if (fieldOffset == offsetof(struct Sprite, images))
            snprintf(field, fieldSize, "images");
        else if (fieldOffset == offsetof(struct Sprite, affineAnims))
            snprintf(field, fieldSize, "affineAnims");
        else if (fieldOffset == offsetof(struct Sprite, template))
            snprintf(field, fieldSize, "template");
        else if (fieldOffset == offsetof(struct Sprite, subspriteTables))
            snprintf(field, fieldSize, "subspriteTables");
        else if (fieldOffset == offsetof(struct Sprite, callback))
            snprintf(field, fieldSize, "callback");
        else
            snprintf(field, fieldSize, "+0x%x", fieldOffset);
        return;
    }
#if defined(LINUX64) && LINUX64
    {
        const struct TextPrinter *printers = TextPrinter_GetStatePrinters();

        if (address >= (uintptr_t)printers
         && address < (uintptr_t)printers + WINDOWS_MAX * sizeof(*printers))
        {
            uintptr_t relative = address - (uintptr_t)printers;
            u32 printerIndex = (u32)(relative / sizeof(struct TextPrinter));
            u32 fieldOffset = (u32)(relative % sizeof(struct TextPrinter));
            u32 currentCharOffset = offsetof(struct TextPrinter, printerTemplate)
                                  + offsetof(struct TextPrinterTemplate, currentChar);

            snprintf(owner, ownerSize, "sTextPrinters[%u] (struct TextPrinter)", printerIndex);
            if (fieldOffset == currentCharOffset)
                snprintf(field, fieldSize, "printerTemplate.currentChar");
            else if (fieldOffset == offsetof(struct TextPrinter, callback))
                snprintf(field, fieldSize, "callback");
            else
                snprintf(field, fieldSize, "+0x%x", fieldOffset);
            return;
        }
    }
#endif
    if (address >= (uintptr_t)gWindows
     && address < (uintptr_t)gWindows + WINDOWS_MAX * sizeof(*gWindows))
    {
        uintptr_t relative = address - (uintptr_t)gWindows;
        u32 windowIndex = (u32)(relative / sizeof(struct Window));
        u32 fieldOffset = (u32)(relative % sizeof(struct Window));

        snprintf(owner, ownerSize, "gWindows[%u] (struct Window)", windowIndex);
        if (fieldOffset == offsetof(struct Window, tileData))
            snprintf(field, fieldSize, "tileData");
        else
            snprintf(field, fieldSize, "+0x%x", fieldOffset);
        return;
    }
    if (address >= (uintptr_t)gTasks
     && address < (uintptr_t)gTasks + NUM_TASKS * sizeof(*gTasks))
    {
        uintptr_t relative = address - (uintptr_t)gTasks;
        u32 taskIndex = (u32)(relative / sizeof(struct Task));
        u32 fieldOffset = (u32)(relative % sizeof(struct Task));

        snprintf(owner, ownerSize, "gTasks[%u] (struct Task)", taskIndex);
        if (fieldOffset == offsetof(struct Task, func))
            snprintf(field, fieldSize, "func");
        else
            snprintf(field, fieldSize, "+0x%x", fieldOffset);
        return;
    }
    if (address >= (uintptr_t)&gMain && address < (uintptr_t)&gMain + sizeof(gMain))
    {
        u32 fieldOffset = (u32)(address - (uintptr_t)&gMain);

        snprintf(owner, ownerSize, "gMain (struct Main)");
        if (fieldOffset == offsetof(struct Main, callback1))
            snprintf(field, fieldSize, "callback1");
        else if (fieldOffset == offsetof(struct Main, callback2))
            snprintf(field, fieldSize, "callback2");
        else if (fieldOffset == offsetof(struct Main, savedCallback))
            snprintf(field, fieldSize, "savedCallback");
        else if (fieldOffset == offsetof(struct Main, vblankCallback))
            snprintf(field, fieldSize, "vblankCallback");
        else if (fieldOffset == offsetof(struct Main, hblankCallback))
            snprintf(field, fieldSize, "hblankCallback");
        else if (fieldOffset == offsetof(struct Main, vcountCallback))
            snprintf(field, fieldSize, "vcountCallback");
        else if (fieldOffset == offsetof(struct Main, serialCallback))
            snprintf(field, fieldSize, "serialCallback");
        else
            snprintf(field, fieldSize, "+0x%x", fieldOffset);
        return;
    }
#if defined(__linux__) && UINTPTR_MAX > UINT32_MAX
    {
        struct NativeStateSymbolInfo info;
        uintptr_t relative;

        if (FindExecutableObjectSymbol(address, &info))
        {
            relative = address - info.address;
            if (info.size != 0 && relative < info.size)
                snprintf(owner, ownerSize, "%s (ELF object, 0x%zx bytes)", info.name, info.size);
            else
                snprintf(owner, ownerSize, "linker padding after %s", info.name);
            snprintf(field, fieldSize, "+0x%zx", relative);
        }
    }
#endif
}

static bool32 RuntimeLocationIsFunction(const struct NativeStateSlice *slice, u32 offset)
{
    uintptr_t address;

    if (slice->source == NULL || offset >= slice->size)
        return FALSE;
    address = (uintptr_t)slice->source + offset;
    if (BattleRuntimeLocationIsFunction(address))
        return TRUE;
#if defined(LINUX64) && LINUX64
    if (address >= (uintptr_t)sSpriteTemplateSidecars
     && address < (uintptr_t)sSpriteTemplateSidecars + sizeof(sSpriteTemplateSidecars))
        return (address - (uintptr_t)sSpriteTemplateSidecars) % sizeof(struct SpriteTemplate)
            == offsetof(struct SpriteTemplate, callback);
#endif
    if (address >= (uintptr_t)gSprites
     && address < (uintptr_t)gSprites + sizeof(gSprites))
        return (address - (uintptr_t)gSprites) % sizeof(struct Sprite)
            == offsetof(struct Sprite, callback);
#if defined(LINUX64) && LINUX64
    {
        const struct TextPrinter *printers = TextPrinter_GetStatePrinters();

        if (address >= (uintptr_t)printers
         && address < (uintptr_t)printers + WINDOWS_MAX * sizeof(*printers))
            return (address - (uintptr_t)printers) % sizeof(struct TextPrinter)
                == offsetof(struct TextPrinter, callback);
    }
#endif
    if (address >= (uintptr_t)gTasks
     && address < (uintptr_t)gTasks + NUM_TASKS * sizeof(*gTasks))
        return (address - (uintptr_t)gTasks) % sizeof(struct Task)
            == offsetof(struct Task, func);
    if (address >= (uintptr_t)&gMain && address < (uintptr_t)&gMain + sizeof(gMain))
    {
        u32 fieldOffset = (u32)(address - (uintptr_t)&gMain);

        return fieldOffset == offsetof(struct Main, callback1)
            || fieldOffset == offsetof(struct Main, callback2)
            || fieldOffset == offsetof(struct Main, savedCallback)
            || fieldOffset == offsetof(struct Main, vblankCallback)
            || fieldOffset == offsetof(struct Main, hblankCallback)
            || fieldOffset == offsetof(struct Main, vcountCallback)
            || fieldOffset == offsetof(struct Main, serialCallback);
    }
    return FALSE;
}

static bool32 RuntimeLocationIsKnownDataPointer(const struct NativeStateSlice *slice, u32 offset)
{
    uintptr_t address;

    if (slice->source == NULL || offset >= slice->size)
        return FALSE;
    address = (uintptr_t)slice->source + offset;
    if (BattleRuntimeLocationIsKnownDataPointer(address))
        return TRUE;
#if defined(LINUX64) && LINUX64
    if (address >= (uintptr_t)sSpriteTemplateSidecars
     && address < (uintptr_t)sSpriteTemplateSidecars + sizeof(sSpriteTemplateSidecars))
    {
        u32 fieldOffset = (u32)((address - (uintptr_t)sSpriteTemplateSidecars)
                              % sizeof(struct SpriteTemplate));

        return fieldOffset == offsetof(struct SpriteTemplate, oam)
            || fieldOffset == offsetof(struct SpriteTemplate, anims)
            || fieldOffset == offsetof(struct SpriteTemplate, images)
            || fieldOffset == offsetof(struct SpriteTemplate, affineAnims);
    }
    if (address >= (uintptr_t)sSpriteTemplateImageSidecars
     && address < (uintptr_t)sSpriteTemplateImageSidecars + sizeof(sSpriteTemplateImageSidecars))
        return (address - (uintptr_t)sSpriteTemplateImageSidecars)
                   % sizeof(struct SpriteFrameImage)
            == offsetof(struct SpriteFrameImage, data);
#endif
    if (address >= (uintptr_t)gSprites
     && address < (uintptr_t)gSprites + sizeof(gSprites))
    {
        u32 fieldOffset = (u32)((address - (uintptr_t)gSprites) % sizeof(struct Sprite));

        return fieldOffset == offsetof(struct Sprite, anims)
            || fieldOffset == offsetof(struct Sprite, images)
            || fieldOffset == offsetof(struct Sprite, affineAnims)
            || fieldOffset == offsetof(struct Sprite, template)
            || fieldOffset == offsetof(struct Sprite, subspriteTables);
    }
#if defined(LINUX64) && LINUX64
    {
        const struct TextPrinter *printers = TextPrinter_GetStatePrinters();

        if (address >= (uintptr_t)printers
         && address < (uintptr_t)printers + WINDOWS_MAX * sizeof(*printers))
            return (address - (uintptr_t)printers) % sizeof(struct TextPrinter)
                == offsetof(struct TextPrinter, printerTemplate)
                 + offsetof(struct TextPrinterTemplate, currentChar);
    }
#endif
    if (address >= (uintptr_t)gWindows
     && address < (uintptr_t)gWindows + WINDOWS_MAX * sizeof(*gWindows))
        return (address - (uintptr_t)gWindows) % sizeof(struct Window)
            == offsetof(struct Window, tileData);
    return FALSE;
}

static bool32 RuntimeLocationIsInactiveTextPrinterPointer(const struct NativeStateSlice *slice,
                                                          u32 offset)
{
#if defined(LINUX64) && LINUX64
    uintptr_t address;
    const struct TextPrinter *printers = TextPrinter_GetStatePrinters();
    uintptr_t relative;
    u32 fieldOffset;
    u32 index;

    if (slice->source == NULL || offset >= slice->size)
        return FALSE;
    address = (uintptr_t)slice->source + offset;
    if (address < (uintptr_t)printers
     || address >= (uintptr_t)printers + WINDOWS_MAX * sizeof(*printers))
        return FALSE;
    relative = address - (uintptr_t)printers;
    index = (u32)(relative / sizeof(*printers));
    fieldOffset = (u32)(relative % sizeof(*printers));
    if (fieldOffset != offsetof(struct TextPrinter, callback)
     && fieldOffset != offsetof(struct TextPrinter, printerTemplate)
                     + offsetof(struct TextPrinterTemplate, currentChar))
        return FALSE;
    return !printers[index].active;
#else
    (void)slice;
    (void)offset;
    return FALSE;
#endif
}

static void DescribePointerTarget(const struct NativeStateSlice *slice, u32 offset,
                                  uintptr_t address, char *target, u32 targetSize)
{
    if (RuntimeLocationIsFunction(slice, offset))
        snprintf(target, targetSize, "native function/callback pointer");
    else
        snprintf(target, targetSize, "native data pointer");

    if (IsExecutableOrGameAddress(address))
    {
        char symbol[96];
#if defined(__linux__) && UINTPTR_MAX > UINT32_MAX
        struct NativeStateSymbolInfo info;
        bool32 hasSymbol = FindExecutableObjectSymbol(address, &info);
#endif

        DescribeNearestSymbol(address, symbol, sizeof(symbol));
#if defined(LINUX64) && LINUX64
        if ((address >= (uintptr_t)sSpriteTemplateSidecars
          && address < (uintptr_t)sSpriteTemplateSidecars + sizeof(sSpriteTemplateSidecars))
         || (address >= (uintptr_t)sSpriteTemplateImageSidecars
          && address < (uintptr_t)sSpriteTemplateImageSidecars + sizeof(sSpriteTemplateImageSidecars)))
            snprintf(target, targetSize, "logical sprite sidecar address (%s)", symbol);
        else
#endif
        if (RuntimeLocationIsFunction(slice, offset)
#if defined(__linux__) && UINTPTR_MAX > UINT32_MAX
         || (hasSymbol && info.type == STT_FUNC)
#endif
        )
            snprintf(target, targetSize, "logical game function/callback (%s)", symbol);
        else
        {
            uintptr_t bssStart;
            uintptr_t bssEnd;
            uintptr_t gameBssStart;
            uintptr_t gameBssEnd;
            bool32 inBss = Platform_RuntimeGetBssRange(&bssStart, &bssEnd)
                        && address >= bssStart && address < bssEnd;
            bool32 inGameBss = Platform_RuntimeGetGameBssRange(&gameBssStart, &gameBssEnd)
                            && address >= gameBssStart && address < gameBssEnd;

            if ((address >= (uintptr_t)__start_gba_ewram && address < (uintptr_t)__stop_gba_ewram)
             || (address >= (uintptr_t)__start_gba_iwram && address < (uintptr_t)__stop_gba_iwram)
             || (address >= (uintptr_t)__start_gba_common && address < (uintptr_t)__stop_gba_common)
             || inGameBss)
                snprintf(target, targetSize, "logical GBA runtime memory (%s)", symbol);
            else if (inBss)
                snprintf(target, targetSize, "host-only native BSS (%s)", symbol);
            else
                snprintf(target, targetSize, "generated/static game data (%s)", symbol);
        }
        return;
    }
    if (address >= (uintptr_t)__start_host_data && address < (uintptr_t)__stop_host_data)
    {
        char symbol[96];

        DescribeNearestSymbol(address, symbol, sizeof(symbol));
        snprintf(target, targetSize, "host-only host_data address (%s)", symbol);
        return;
    }
#if defined(__linux__)
    {
        FILE *maps;
        char line[256];

        maps = fopen("/proc/self/maps", "r");
        if (maps != NULL)
        {
            while (fgets(line, sizeof(line), maps) != NULL)
            {
                unsigned long begin;
                unsigned long end;
                char permissions[5];
                char pathname[128] = "";
                int fields;

                fields = sscanf(line, "%lx-%lx %4s %*s %*s %*s %127[^\n]",
                                &begin, &end, permissions, pathname);
                if (fields >= 3 && address >= (uintptr_t)begin && address < (uintptr_t)end)
                {
                    const char *name = pathname;

                    fclose(maps);
                    while (*name == ' ')
                        name++;
                    if (fields == 4 && strncmp(name, "[stack", 6) == 0)
                        snprintf(target, targetSize, "host stack/temporary storage (%s)", name);
                    else if (permissions[2] == 'x')
                        snprintf(target, targetSize, "host executable mapping (%s)",
                                 fields == 4 ? name : "anonymous");
                    else if (fields == 3 || name[0] == '[')
                        snprintf(target, targetSize, "host anonymous allocation (%s)",
                                 fields == 4 ? name : "anonymous");
                    else
                        snprintf(target, targetSize, "host mapped allocation (%s)", name);
                    return;
                }
            }
            fclose(maps);
        }
    }
#else
    (void)address;
#endif
    snprintf(target, targetSize, "unmapped native address (not logical GBA memory)");
}

static void DescribePersistentClassification(const struct NativeStateSlice *slice, u32 offset,
                                             uintptr_t address,
                                             char *classification, u32 classificationSize)
{
    struct NativeBattleBufferLocation buffer;
    uintptr_t location = slice->source == NULL ? 0 : (uintptr_t)slice->source + offset;

    if (GetBattleBufferLocation(location, &buffer))
    {
        if (buffer.toController && buffer.command == CONTROLLER_DMA3TRANSFER
         && buffer.offset >= 1 && buffer.offset < 1 + sizeof(GbaAddr))
            snprintf(classification, classificationSize,
                     "runtime GbaAddr command-buffer field; requires a stable tagged data identity");
        else
            snprintf(classification, classificationSize,
                     "battle protocol scalar/payload bytes; not a persistent pointer field");
        return;
    }
    if (address <= UINT32_MAX && HostAddressIsRuntimeHandle((GbaAddr)address))
    {
        snprintf(classification, classificationSize,
                 "runtime-only 0x%c handle; forbidden in persistent state",
                 ((GbaAddr)address & 0xF0000000u) == 0xE0000000u ? 'E' : 'F');
        return;
    }
    if (RuntimeLocationIsFunction(slice, offset))
    {
        snprintf(classification, classificationSize,
                 IsExecutableOrGameAddress(address)
                 ? "stable tagged persistent function record"
                 : "function field has no stable persistent identity");
        return;
    }
    if (IsExecutableOrGameAddress(address))
    {
        snprintf(classification, classificationSize,
                 "stable tagged persistent data record");
        return;
    }
    if (address >= (uintptr_t)__start_host_data && address < (uintptr_t)__stop_host_data)
    {
        snprintf(classification, classificationSize,
                 "host-only state; forbidden in persistent game state");
        return;
    }
    snprintf(classification, classificationSize,
             "no stable persistent identity");
}

static void SetRuntimePointerError(const struct NativeStateSlice *slice, u32 offset,
                                   uintptr_t address, const char *reason,
                                   const char *phase, const u8 *bytes, u32 size)
{
    char symbol[96];
    char owner[112];
    char field[64];
    char target[192];
    char persistentClassification[160];
    struct NativeBattleBufferLocation battleBuffer;
    bool32 hasBattleBuffer = slice->source != NULL
                          && GetBattleBufferLocation((uintptr_t)slice->source + offset,
                                                     &battleBuffer);

    DescribeRuntimeLocation(slice, offset, symbol, sizeof(symbol),
                            owner, sizeof(owner), field, sizeof(field));
    DescribePointerTarget(slice, offset, address, target, sizeof(target));
    DescribePersistentClassification(slice, offset, address,
                                     persistentClassification,
                                     sizeof(persistentClassification));
    /* Machine-readable forensics go to stderr even when the frontend status
     * line truncates the reason, so the next manual failure is self-describing. */
    {
        u32 dataHandles = 0;
        u32 functionHandles = 0;

        HostMemoryGetHandleCounters(&dataHandles, &functionHandles);
        fprintf(stderr, "native state pointer failure:\n");
        fprintf(stderr, "  phase=%s\n", phase);
        fprintf(stderr, "  section=%s\n", SectionName(slice->tag));
        fprintf(stderr, "  section_offset=0x%x\n", offset);
        fprintf(stderr, "  symbol=%s\n", symbol);
        fprintf(stderr, "  element=%s\n", owner);
        fprintf(stderr, "  field=%s\n", field);
        fprintf(stderr, "  raw_pointer=0x%zx\n", address);
        fprintf(stderr, "  classification=%s\n", target);
        if (hasBattleBuffer)
        {
            fprintf(stderr, "  battle_buffer=%s[%u]\n",
                    battleBuffer.name, battleBuffer.battler);
            fprintf(stderr, "  battle_offset=0x%x\n", battleBuffer.offset);
            fprintf(stderr, "  battle_command=%s (%u)\n",
                    BattleControllerCommandName(battleBuffer.command), battleBuffer.command);
            fprintf(stderr, "  exact_field=%s\n", field);
        }
        fprintf(stderr, "  raw_value=0x%zx\n", address);
        fprintf(stderr, "  pointer_target=%s\n", target);
        fprintf(stderr, "  persistent_classification=%s\n", persistentClassification);
        fprintf(stderr, "  reason=%s\n", reason);
        fprintf(stderr, "  handle_table=data:%u function:%u\n",
                dataHandles, functionHandles);
        if (bytes != NULL && size != 0)
        {
            u32 dumpStart = offset >= 16 ? offset - 16 : 0;
            u32 dumpEnd = offset + 32 <= size ? offset + 32 : size;
            u32 i;

            fprintf(stderr, "  bytes[0x%x..0x%x)=", dumpStart, dumpEnd);
            for (i = dumpStart; i < dumpEnd; i++)
                fprintf(stderr, "%02x", bytes[i]);
            fputc('\n', stderr);
        }
#if defined(LINUX64) && LINUX64
        if (slice->tag == STATE_SECTION_EWRAM)
        {
            const struct TextPrinter *printers = TextPrinter_GetStatePrinters();
            u32 printer;

            for (printer = 0; printer < WINDOWS_MAX; printer++)
            {
                fprintf(stderr,
                        "  printer[%02u] active=%u state=%u window=%u callback=%p currentChar=%p\n",
                        printer, printers[printer].active, printers[printer].state,
                        printers[printer].printerTemplate.windowId,
                        (const void *)printers[printer].callback,
                        (const void *)printers[printer].printerTemplate.currentChar);
            }
            TextPrinter_DumpStateEvents();
            {
                u32 sidecar;

                for (sidecar = 0; sidecar < MAX_SPRITES + 1; sidecar++)
                {
                    const struct SpriteTemplate *sidecarTemplate = &sSpriteTemplateSidecars[sidecar];
                    const struct SpriteFrameImage *sidecarImage = &sSpriteTemplateImageSidecars[sidecar];

                    fprintf(stderr,
                            "  sprite_sidecar[%02u] template oam=%p anims=%p images=%p"
                            " affineAnims=%p callback=%p image data=%p size=0x%x\n",
                            sidecar,
                            (const void *)sidecarTemplate->oam,
                            (const void *)sidecarTemplate->anims,
                            (const void *)sidecarTemplate->images,
                            (const void *)sidecarTemplate->affineAnims,
                            (const void *)sidecarTemplate->callback,
                            sidecarImage->data,
                            sidecarImage->size);
                }
            }
        }
#endif
        fflush(stderr);
    }
    snprintf(sLastError, sizeof(sLastError),
             "section %u (%s), section offset +0x%x, nearest symbol %s, "
             "containing structure %s, field %s, raw pointer 0x%zx, "
             "pointer target classification %s, persistent classification %s: %s",
             slice->tag, SectionName(slice->tag), offset, symbol, owner, field,
             address, target, persistentClassification, reason);
}

static bool32 ValidateNoRuntimeHandles(const struct NativeStateSlice *slice,
                                       const u8 *bytes, u32 size, bool32 serialized,
                                       const char *phase)
{
    u32 offset;

    for (offset = 0; offset + sizeof(GbaAddr) <= size; offset += sizeof(GbaAddr))
    {
        GbaAddr logical;
        bool32 durableRecord = FALSE;
        bool32 completeNativePointer = FALSE;

        memcpy(&logical, bytes + offset, sizeof(logical));
        /* E/F namespace-shaped words are also legal packed game data. Only a
         * value backed by this process's live handle table is demonstrably a
         * quarantined pointer and must be rejected. Newly produced states can
         * therefore never contain a process-local handle, without guessing at
         * arbitrary scalar words in EWRAM. */
        if (!HostAddressIsRegisteredRuntimeHandle(logical))
            continue;
        if (offset + sizeof(struct HostPersistentAddress) <= size)
        {
            struct HostPersistentAddress persistent;
            uintptr_t native;

            memcpy(&persistent, bytes + offset, sizeof(persistent));
            durableRecord = serialized && persistent.kind != 0
                         && (HostPersistentAddressIsFunction(&persistent)
                          || HostPersistentAddressIsData(&persistent));
            memcpy(&native, bytes + offset, sizeof(native));
            completeNativePointer = !serialized && IsExecutableOrGameAddress(native);
        }
        if (!durableRecord && !completeNativePointer)
        {
            SetRuntimePointerError(slice, offset, logical,
                                   serialized
                                   ? "state contains a process-local runtime handle"
                                   : "process-local runtime handle cannot be persisted",
                                   phase, bytes, size);
            return FALSE;
        }
    }
    return TRUE;
}

static void NormalizeBattleCommandBufferPointers(const struct NativeStateSlice *slice,
                                                 u8 *dest)
{
    u32 battler;
    uintptr_t start;
    uintptr_t end;

    if (slice->source == NULL)
        return;
    start = (uintptr_t)slice->source;
    end = start + slice->size;
    for (battler = 0; battler < MAX_BATTLERS_COUNT; battler++)
    {
        uintptr_t pointerField = (uintptr_t)&gBattleBufferA[battler][1];

        if (gBattleBufferA[battler][0] == CONTROLLER_DMA3TRANSFER
         && pointerField >= start && pointerField + sizeof(GbaAddr) <= end)
            memset(dest + pointerField - start, 0, sizeof(GbaAddr));
    }
}

static void NormalizeTextPrinterPadding(const struct NativeStateSlice *slice, u8 *dest)
{
#if defined(LINUX64) && LINUX64
    const struct TextPrinter *printers = TextPrinter_GetStatePrinters();
    uintptr_t sourceStart;
    uintptr_t sourceEnd;
    uintptr_t printerStart;
    uintptr_t printerEnd;
    u32 printer;
    u32 paddingOffset;
    u32 paddingSize;

    if (slice->source == NULL || slice->size == 0)
        return;

    sourceStart = (uintptr_t)slice->source;
    sourceEnd = sourceStart + slice->size;
    printerStart = (uintptr_t)printers;
    printerEnd = printerStart + WINDOWS_MAX * sizeof(*printers);
    if (printerStart < sourceStart || printerEnd > sourceEnd)
        return;

    /* TextPrinterTemplate has two color bytes followed by host alignment
     * padding before the callback. That padding is not game state, but on
     * Linux64 it can retain a stack/native pointer from the temporary printer. */
    paddingOffset = offsetof(struct TextPrinterTemplate, lineSpacing) + 3;
    paddingSize = sizeof(struct TextPrinterTemplate) - paddingOffset;
    for (printer = 0; printer < WINDOWS_MAX; printer++)
    {
        u32 offset = (u32)(printerStart - sourceStart)
                   + printer * sizeof(*printers)
                   + paddingOffset;
        memset(dest + offset, 0, paddingSize);
    }
#else
    (void)slice;
    (void)dest;
#endif
}

static bool32 RuntimeLocationIsTextPrinterPadding(const struct NativeStateSlice *slice,
                                                   u32 offset)
{
#if defined(LINUX64) && LINUX64
    const struct TextPrinter *printers = TextPrinter_GetStatePrinters();
    uintptr_t address;
    uintptr_t relative;
    u32 fieldOffset;
    u32 paddingOffset = offsetof(struct TextPrinterTemplate, lineSpacing) + 3;

    if (slice->source == NULL || offset + sizeof(uintptr_t) > slice->size)
        return FALSE;
    address = (uintptr_t)slice->source + offset;
    if (address < (uintptr_t)printers
     || address >= (uintptr_t)printers + WINDOWS_MAX * sizeof(*printers))
        return FALSE;
    relative = address - (uintptr_t)printers;
    fieldOffset = (u32)(relative % sizeof(*printers));
    /* A four-byte-stride scan can start before the padding and still read it.
     * Exclude only those overlapping windows; the real callback at +0x18 is
     * still handled by the normal persistent-function path. */
    return fieldOffset < sizeof(struct TextPrinterTemplate)
        && fieldOffset + sizeof(uintptr_t) > paddingOffset;
#else
    (void)slice;
    (void)offset;
    return FALSE;
#endif
}

static bool32 NormalizeRuntimeBytes(const struct NativeStateSlice *slice, u8 *dest, u32 size)
{
    const void *source = slice->source;
#if UINTPTR_MAX > UINT32_MAX
    u32 offset;
#endif

    memcpy(dest, source, size);
    NormalizeTextPrinterPadding(slice, dest);
    /* Four-byte GbaAddr operands cannot hold an eight-byte persistent tag.
     * Their stable identities live in STATE_SECTION_BATTLE_SIDECAR, while the
     * in-band command bytes are cleared in the serialized EWRAM image. */
    NormalizeBattleCommandBufferPointers(slice, dest);
#if UINTPTR_MAX > UINT32_MAX
    for (offset = 0; offset + sizeof(uintptr_t) <= size; offset += sizeof(u32))
    {
        uintptr_t address;
        struct HostPersistentAddress persistent;
        struct NativeBattleBufferLocation battleBuffer;
        bool32 functionPointer;
        bool32 knownDataPointer;

        memcpy(&address, (const u8 *)source + offset, sizeof(address));
        /* apuCycle is a one-byte COMMON_DATA scalar immediately followed by
         * the u32 apuFrame. Its linker address happens to be pointer-aligned,
         * so treating the combined eight bytes as a native pointer produces a
         * false high-address hit once the audio frame counter is large. */
        if ((const u8 *)source + offset == &apuCycle)
            continue;
        if (RuntimeLocationIsTextPrinterPadding(slice, offset))
            continue;
        /* Controller buffers are byte protocols, not native C object storage.
         * Never reinterpret ordinary packet bytes as an inferred pointer and
         * rewrite them. A complete mapped native pointer here is producer
         * leakage and must fail with the packet/field diagnostic above. */
        if (GetBattleBufferLocation((uintptr_t)source + offset, &battleBuffer))
        {
            /* Battle buffers are byte protocols. A 32-bit scalar, packed text
             * terminator, IV field, or logical game value can coincidentally
             * name an image symbol when widened to uintptr_t; that is not a
             * native pointer. Reject only a complete mapped host pointer or
             * an explicitly forbidden process-local handle. The one
             * pointer-bearing protocol field, DMA3 destination, is handled by
             * NormalizeBattleCommandBufferPointers/CaptureBattleSidecar. */
            bool32 runtimeHandle = address <= UINT32_MAX
                                && HostAddressIsRuntimeHandle((GbaAddr)address);
            if (LooksLikeExternalHostPointer(address) || runtimeHandle)
            {
                SetRuntimePointerError(slice, offset, address,
                                       "battle protocol contains leaked native pointer bytes",
                                       "save-normalize", (const u8 *)source, size);
                return FALSE;
            }
            continue;
        }
        functionPointer = RuntimeLocationIsFunction(slice, offset);
        knownDataPointer = RuntimeLocationIsKnownDataPointer(slice, offset);
        /* An inactive printer has no continuation. Its current character and
         * callback are stale scratch values and must not become state roots. */
        if (RuntimeLocationIsInactiveTextPrinterPointer(slice, offset))
        {
            memset(dest + offset, 0, sizeof(persistent));
            offset += sizeof(u32);
            continue;
        }
        if (address == 0)
            continue;
        if (functionPointer)
        {
            if (!HostFunctionToPersistentAddress((const u8 *)source + offset,
                                                 sizeof(address), &persistent))
            {
                SetRuntimePointerError(slice, offset, address,
                                       "callback has no persistent function identity",
                                       "save-normalize", (const u8 *)source, size);
                return FALSE;
            }
            memcpy(dest + offset, &persistent, sizeof(persistent));
            offset += sizeof(u32);
            continue;
        }
        if (!IsExecutableOrGameAddress(address))
        {
            /* The portable game allocator is gHeap. A mapped high value in a
             * native-pointer-aligned game slot is therefore an unmanaged host
             * pointer, even when the pointer target itself is byte-aligned. */
            if (LooksLikeExternalHostPointer(address))
            {
                SetRuntimePointerError(slice, offset, address,
                                       "state contains unmanaged native pointer",
                                       "save-normalize", (const u8 *)source, size);
                return FALSE;
            }
            if (knownDataPointer)
            {
                SetRuntimePointerError(slice, offset, address,
                                       "known pointer field does not target persistent game memory",
                                       "save-normalize", (const u8 *)source, size);
                return FALSE;
            }
            continue;
        }
        /* Callback fields outside the explicitly modeled core structures are
         * still recognizable by their executable target. Preserve the stronger
         * function validation and tag instead of treating code as generic data. */
        if (!knownDataPointer
         && HostFunctionToPersistentAddress((const u8 *)source + offset,
                                            sizeof(address), &persistent))
        {
            memcpy(dest + offset, &persistent, sizeof(persistent));
            offset += sizeof(u32);
            continue;
        }
        if (!HostPointerToPersistentAddress((const void *)address, &persistent))
        {
            SetRuntimePointerError(slice, offset, address,
                                   "pointer has no persistent logical identity",
                                   "save-normalize", (const u8 *)source, size);
            return FALSE;
        }
        memcpy(dest + offset, &persistent, sizeof(persistent));
        offset += sizeof(u32);
    }
    /* Validate the bytes that will actually be written to the file. Scanning the
     * live source before this walk rejected stale values in fields the walk
     * clears or converts first (for example inactive text-printer continuation
     * state), so the audit now sees exactly the serialized image. */
    return ValidateNoRuntimeHandles(slice, dest, size, TRUE, "save-final");
#else
    (void)source;
#endif
    return TRUE;
}

static bool32 RestoreRuntimeBytes(const struct NativeStateSlice *slice,
                                  void *dest, const u8 *source, u32 size)
{
#if UINTPTR_MAX > UINT32_MAX
    u32 offset;
#endif

#if UINTPTR_MAX > UINT32_MAX
    if (!ValidateNoRuntimeHandles(slice, source, size, TRUE, "load-verify"))
        return FALSE;
#endif
    memcpy(dest, source, size);
#if UINTPTR_MAX > UINT32_MAX
    for (offset = 0; offset + sizeof(u64) <= size; offset += sizeof(u32))
    {
        struct HostPersistentAddress persistent;
        struct NativeBattleBufferLocation battleBuffer;
        void *pointer;
        uintptr_t native;
        bool32 functionPointer;
        bool32 knownDataPointer;
        bool32 encodedFunction;

        /* Battle packets remain byte-for-byte protocol data. Their only
         * GbaAddr-bearing command is checked by the runtime-handle audit; no
         * eight-byte tagged record is ever decoded in-place over packet data. */
        if (slice->source != NULL
         && GetBattleBufferLocation((uintptr_t)slice->source + offset, &battleBuffer))
            continue;

        memcpy(&persistent, source + offset, sizeof(persistent));
        functionPointer = RuntimeLocationIsFunction(slice, offset);
        knownDataPointer = RuntimeLocationIsKnownDataPointer(slice, offset);
        encodedFunction = persistent.kind != 0
                       && HostPersistentAddressIsFunction(&persistent);
        if (functionPointer || encodedFunction)
        {
            if (knownDataPointer && encodedFunction)
            {
                memcpy(&native, &persistent, sizeof(persistent));
                SetRuntimePointerError(slice, offset, native,
                                       "data field contains a persistent callback identity",
                                       "load-restore", source, size);
                return FALSE;
            }
            if (!HostPersistentAddressIsFunction(&persistent)
             || !HostResolvePersistentFunction(&persistent, &native, sizeof(native)))
            {
                memcpy(&native, &persistent, sizeof(persistent));
                SetRuntimePointerError(slice, offset, native,
                                       "state contains an invalid persistent callback identity",
                                       "load-restore", source, size);
                return FALSE;
            }
            memcpy((u8 *)dest + offset, &native, sizeof(native));
            offset += sizeof(u32);
            continue;
        }
        if (!HostPersistentAddressIsData(&persistent))
        {
            if (knownDataPointer)
            {
                memcpy(&native, &persistent, sizeof(persistent));
                SetRuntimePointerError(slice, offset, native,
                                       "state contains an invalid persistent data identity",
                                       "load-restore", source, size);
                return FALSE;
            }
            continue;
        }
        if (!HostResolvePersistentAddress(&persistent, &pointer))
        {
            memcpy(&native, &persistent, sizeof(persistent));
            SetRuntimePointerError(slice, offset, native,
                                   "state contains an invalid rehydratable pointer",
                                   "load-restore", source, size);
            return FALSE;
        }
        native = (uintptr_t)pointer;
        memcpy((u8 *)dest + offset, &native, sizeof(native));
        offset += sizeof(u32);
    }
#else
    (void)source;
#endif
    return TRUE;
}

static void BuildEwramDiagnosticSlice(struct NativeStateSlice *slice)
{
    slice->tag = STATE_SECTION_EWRAM;
    slice->source = __start_gba_ewram;
    slice->size = SectionSize(__start_gba_ewram, __stop_gba_ewram);
    slice->runtimePointers = TRUE;
}

static bool32 ValidateBattleSidecar(const u8 *source, u32 size);

static bool32 CaptureBattleSidecar(void *dest, u32 size)
{
    struct NativeStateBattleSidecar *state = dest;
    struct NativeStateSlice ewram;
    u32 battler;

    if (size != sizeof(*state))
        return FALSE;
    memset(state, 0, sizeof(*state));
    BuildEwramDiagnosticSlice(&ewram);
    for (battler = 0; battler < MAX_BATTLERS_COUNT; battler++)
    {
        GbaAddr logical;
        void *pointer;
        u32 ewramOffset = (u32)((uintptr_t)&gBattleBufferA[battler][1]
                              - (uintptr_t)__start_gba_ewram);

        if (gBattleBufferA[battler][0] != CONTROLLER_DMA3TRANSFER)
            continue;
        memcpy(&logical, &gBattleBufferA[battler][1], sizeof(logical));
        if (logical == 0)
            continue;
        if ((logical & 0xFFFF0000u) == 0xE0000000u
         || (HostAddressIsRuntimeHandle(logical)
          && !HostAddressIsRegisteredRuntimeHandle(logical)))
        {
            SetRuntimePointerError(&ewram, ewramOffset, logical,
                                   "battle command buffer contains an invalid runtime handle",
                                   "save-battle-sidecar", (const u8 *)ewram.source, ewram.size);
            return FALSE;
        }
        pointer = HostResolveGbaAddr(logical);
        if (!HostPointerToPersistentAddress(pointer, &state->commandBufferData[battler]))
        {
            SetRuntimePointerError(&ewram, ewramOffset, (uintptr_t)pointer,
                                   "battle command-buffer pointer has no persistent data identity",
                                   "save-battle-sidecar", (const u8 *)ewram.source, ewram.size);
            return FALSE;
        }
    }
    /* Keep the battle-specific save path in the same order as normal runtime
     * sections: capture, encode, validate the encoded records, then let the
     * container writer commit the section. */
    return ValidateBattleSidecar(dest, size);
}

static bool32 ValidateBattleSidecar(const u8 *source, u32 size)
{
    const struct NativeStateBattleSidecar *state = (const void *)source;
    struct NativeStateSlice sidecar;
    u32 battler;

    if (size != sizeof(*state))
        return FALSE;
    sidecar.tag = STATE_SECTION_BATTLE_SIDECAR;
    sidecar.source = NULL;
    sidecar.size = size;
    sidecar.runtimePointers = FALSE;
    for (battler = 0; battler < MAX_BATTLERS_COUNT; battler++)
    {
        void *pointer;
        uintptr_t raw;

        if (state->commandBufferData[battler].kind == 0
         && state->commandBufferData[battler].value == 0)
            continue;
        if (HostPersistentAddressIsData(&state->commandBufferData[battler])
         && HostResolvePersistentAddress(&state->commandBufferData[battler], &pointer))
            continue;
        memcpy(&raw, &state->commandBufferData[battler], sizeof(raw));
        SetRuntimePointerError(&sidecar,
                               battler * sizeof(struct HostPersistentAddress), raw,
                               "battle sidecar contains an invalid persistent data identity",
                               "load-battle-sidecar", source, size);
        return FALSE;
    }
    return TRUE;
}

static bool32 RestoreBattleSidecar(const u8 *source, u32 size)
{
    const struct NativeStateBattleSidecar *state = (const void *)source;
    u32 battler;

    if (!ValidateBattleSidecar(source, size))
        return FALSE;
    for (battler = 0; battler < MAX_BATTLERS_COUNT; battler++)
    {
        void *pointer;
        GbaAddr logical;

        if (state->commandBufferData[battler].kind == 0
         && state->commandBufferData[battler].value == 0)
            continue;
        if (gBattleBufferA[battler][0] != CONTROLLER_DMA3TRANSFER)
        {
            SetError("battle pointer sidecar does not match its command buffer");
            return FALSE;
        }
        if (!HostResolvePersistentAddress(&state->commandBufferData[battler], &pointer))
            return FALSE;
        logical = HostPointerToGbaAddr(pointer);
        if (HostAddressIsRuntimeHandle(logical))
        {
            SetError("battle pointer sidecar rehydrated to a runtime-only handle");
            return FALSE;
        }
        memcpy(&gBattleBufferA[battler][1], &logical, sizeof(logical));
    }
    return TRUE;
}

static bool32 CaptureSlice(const struct NativeStateSlice *slice, u8 *dest)
{
    if (slice->tag == STATE_SECTION_VIDEO_MEMORY)
    {
        memcpy(dest, VRAM_, sizeof(VRAM_));
        memcpy(dest + sizeof(VRAM_), PLTT, sizeof(PLTT));
        memcpy(dest + sizeof(VRAM_) + sizeof(PLTT), OAM, sizeof(OAM));
        return TRUE;
    }
    if (slice->tag == STATE_SECTION_TASK_SIDECAR)
    {
        if (!Task_SaveState(dest, slice->size))
        {
            u32 failureOffset;
            uintptr_t failureAddress;
            if (Task_GetStateFailure(&failureOffset, &failureAddress))
            {
                SetRuntimePointerError(slice, failureOffset, failureAddress,
                                       "task follow-up callback could not be serialized",
                                       "save-sidecar", dest, slice->size);
            }
            else
            {
                SetError("task callback sidecar could not be serialized");
            }
            return FALSE;
        }
        return TRUE;
    }
    if (slice->tag == STATE_SECTION_SPRITE_SIDECAR)
    {
        if (!Sprite_SaveState(dest, slice->size))
        {
            u32 failureOffset;
            uintptr_t failureAddress;
            if (Sprite_GetStateFailure(&failureOffset, &failureAddress))
            {
                SetRuntimePointerError(slice, failureOffset, failureAddress,
                                       "sprite stored callback could not be serialized",
                                       "save-sidecar", dest, slice->size);
            }
            else
            {
                SetError("sprite callback sidecar could not be serialized");
            }
            return FALSE;
        }
        return TRUE;
    }
    if (slice->tag == STATE_SECTION_BATTLE_SIDECAR)
        return CaptureBattleSidecar(dest, slice->size);
    if (slice->runtimePointers && !NormalizeRuntimeBytes(slice, dest, slice->size))
        return FALSE;
    if (slice->runtimePointers)
        return TRUE;
    memcpy(dest, slice->source, slice->size);
    return TRUE;
}

static bool32 RestoreSlice(const struct NativeStateSlice *slice, const u8 *source)
{
    if (slice->tag == STATE_SECTION_VIDEO_MEMORY)
    {
        memcpy(VRAM_, source, sizeof(VRAM_));
        memcpy(PLTT, source + sizeof(VRAM_), sizeof(PLTT));
        memcpy(OAM, source + sizeof(VRAM_) + sizeof(PLTT), sizeof(OAM));
        return TRUE;
    }
    if (slice->tag == STATE_SECTION_TASK_SIDECAR)
    {
        if (!Task_LoadState(source, slice->size))
        {
            SetError("task callback sidecar could not be rehydrated");
            return FALSE;
        }
        return TRUE;
    }
    if (slice->tag == STATE_SECTION_SPRITE_SIDECAR)
    {
        if (!Sprite_LoadState(source, slice->size))
        {
            SetError("sprite callback sidecar could not be rehydrated");
            return FALSE;
        }
        return TRUE;
    }
    if (slice->tag == STATE_SECTION_BATTLE_SIDECAR)
        return RestoreBattleSidecar(source, slice->size);
    if (slice->tag == STATE_SECTION_RTC)
        return Platform_ClockRestoreState(source, slice->size);
    if (slice->runtimePointers)
        return RestoreRuntimeBytes(slice, (void *)slice->source, source, slice->size);
    memcpy((void *)slice->source, source, slice->size);
    return TRUE;
}

static bool32 ValidateRestoreSlice(const struct NativeStateSlice *slice, const u8 *source)
{
    if (slice->tag == STATE_SECTION_TASK_SIDECAR)
    {
        if (!Task_ValidateState(source, slice->size))
        {
            u32 failureOffset;
            uintptr_t failureAddress;
            if (Task_GetStateFailure(&failureOffset, &failureAddress))
            {
                SetRuntimePointerError(slice, failureOffset, failureAddress,
                                       "state contains an invalid persistent task callback identity",
                                       "load-sidecar", source, slice->size);
            }
            else
                SetError("task callback sidecar is invalid");
            return FALSE;
        }
        return TRUE;
    }
    if (slice->tag == STATE_SECTION_SPRITE_SIDECAR)
    {
        if (!Sprite_ValidateState(source, slice->size))
        {
            u32 failureOffset;
            uintptr_t failureAddress;
            if (Sprite_GetStateFailure(&failureOffset, &failureAddress))
            {
                SetRuntimePointerError(slice, failureOffset, failureAddress,
                                       "state contains an invalid persistent sprite callback identity",
                                       "load-sidecar", source, slice->size);
            }
            else
                SetError("sprite callback sidecar is invalid");
            return FALSE;
        }
        return TRUE;
    }
    if (slice->tag == STATE_SECTION_BATTLE_SIDECAR)
        return ValidateBattleSidecar(source, slice->size);
    if (slice->runtimePointers)
    {
        u8 *scratch = malloc(slice->size ? slice->size : 1);
        bool32 valid;
        if (scratch == NULL)
        {
            SetError("unable to validate state pointer fields");
            return FALSE;
        }
        valid = RestoreRuntimeBytes(slice, scratch, source, slice->size);
        free(scratch);
        return valid;
    }
    return TRUE;
}

static bool32 HeaderMatches(const struct NativeStateHeader *header, u32 fileSize)
{
    char buildId[sizeof(header->buildId)];

    if (header->magic != NATIVE_STATE_MAGIC || header->formatVersion != NATIVE_STATE_FORMAT_VERSION)
    {
        SetError("state format version is not supported");
        return FALSE;
    }
    if (header->headerSize != sizeof(*header) || header->totalSize != fileSize
     || header->sectionCount == 0 || header->sectionCount > 16
     || header->headerSize > fileSize
     || header->sectionCount > (fileSize - header->headerSize) / sizeof(struct NativeStateSectionHeader)
     || header->payloadSize != fileSize - header->headerSize)
    {
        SetError("state container is malformed");
        return FALSE;
    }
    if (!GetNativeBuildId(buildId, sizeof(buildId)))
    {
        SetError("native executable build identity is unavailable");
        return FALSE;
    }
    if (strncmp(header->buildId, buildId, sizeof(header->buildId)) != 0)
    {
        SetError("state was made by an incompatible native build");
        return FALSE;
    }
    if (strncmp(header->contentFingerprint, NATIVE_STATE_CONTENT_FINGERPRINT,
                sizeof(header->contentFingerprint)) != 0)
    {
        SetError("state content fingerprint does not match");
        return FALSE;
    }
    return TRUE;
}

static enum NativeStateResult SaveStateToPath(const char *path)
{
    struct NativeStateSlice slices[13];
    struct SiiRtcInfo rtc;
    u8 *framebuffer;
    u8 *file;
    u32 count;
    u32 totalSize;
    u32 offset;
    u32 i;
    struct NativeStateHeader *header;

    framebuffer = malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(u32));
    if (framebuffer == NULL)
    {
        SetError("unable to allocate state framebuffer");
        return NATIVE_STATE_UNAVAILABLE;
    }
    count = BuildSlices(slices, ARRAY_COUNT(slices), framebuffer, &rtc);
    totalSize = sizeof(struct NativeStateHeader) + count * sizeof(struct NativeStateSectionHeader);
    for (i = 0; i < count; i++)
    {
        if (slices[i].size > UINT32_MAX - totalSize)
        {
            free(framebuffer);
            SetError("state is too large");
            return NATIVE_STATE_UNAVAILABLE;
        }
        totalSize += slices[i].size;
    }
    if (totalSize > NATIVE_STATE_MAX_FILE_SIZE)
    {
        free(framebuffer);
        SetError("state is larger than the supported container limit");
        return NATIVE_STATE_UNAVAILABLE;
    }
    file = malloc(totalSize);
    if (file == NULL)
    {
        free(framebuffer);
        SetError("unable to allocate state container");
        return NATIVE_STATE_UNAVAILABLE;
    }
    memset(file, 0, totalSize);
    header = (struct NativeStateHeader *)file;
    header->magic = NATIVE_STATE_MAGIC;
    header->formatVersion = NATIVE_STATE_FORMAT_VERSION;
    header->headerSize = sizeof(*header);
    header->totalSize = totalSize;
    header->sectionCount = count;
    header->frame = Platform_SchedulerGetFrameCounter();
    if (!GetNativeBuildId(header->buildId, sizeof(header->buildId)))
    {
        free(file);
        free(framebuffer);
        SetError("native executable build identity is unavailable");
        return NATIVE_STATE_UNAVAILABLE;
    }
    snprintf(header->contentFingerprint, sizeof(header->contentFingerprint), "%s", NATIVE_STATE_CONTENT_FINGERPRINT);
    offset = sizeof(*header) + count * sizeof(struct NativeStateSectionHeader);
    for (i = 0; i < count; i++)
    {
        struct NativeStateSectionHeader *section =
            (struct NativeStateSectionHeader *)(file + sizeof(*header) + i * sizeof(*section));
        section->tag = slices[i].tag;
        section->size = slices[i].size;
        if (!CaptureSlice(&slices[i], file + offset))
        {
            free(file);
            free(framebuffer);
            return NATIVE_STATE_UNSUPPORTED;
        }
        section->crc = Crc32(file + offset, section->size);
        offset += section->size;
    }
    header->payloadSize = offset - sizeof(*header);
    header->payloadCrc = Crc32(file + sizeof(*header), header->payloadSize);
    if (!Platform_StorageWriteAtomic(path, file, totalSize))
    {
        free(file);
        free(framebuffer);
        SetError("unable to write state file");
        return NATIVE_STATE_IO_ERROR;
    }
    free(file);
    free(framebuffer);
    return NATIVE_STATE_OK;
}

static enum NativeStateResult LoadStateFromPath(const char *path)
{
    struct NativeStateSlice slices[13];
    struct SiiRtcInfo rtc;
    u8 *framebuffer;
    u8 *file;
    u32 fileSize;
    u32 count;
    u32 offset;
    u32 i;
    struct NativeStateHeader *header;

    file = malloc(NATIVE_STATE_MAX_FILE_SIZE);
    framebuffer = malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(u32));
    if (file == NULL || framebuffer == NULL)
    {
        free(file);
        free(framebuffer);
        SetError("unable to allocate state load buffer");
        return NATIVE_STATE_UNAVAILABLE;
    }
    if (!Platform_StorageReadFile(path, file, NATIVE_STATE_MAX_FILE_SIZE, &fileSize))
    {
        free(file);
        free(framebuffer);
        SetError("state file is missing, oversized, or unreadable");
        return NATIVE_STATE_IO_ERROR;
    }
    if (fileSize < sizeof(struct NativeStateHeader))
    {
        free(file);
        free(framebuffer);
        SetError("state file is truncated");
        return NATIVE_STATE_CORRUPT;
    }
    header = (struct NativeStateHeader *)file;
    if (!HeaderMatches(header, fileSize)
     || header->payloadCrc != Crc32(file + sizeof(*header), header->payloadSize))
    {
        free(file);
        free(framebuffer);
        if (sLastError[0] == '\0') SetError("state payload checksum failed");
        return NATIVE_STATE_INCOMPATIBLE;
    }
    count = BuildSlices(slices, ARRAY_COUNT(slices), framebuffer, &rtc);
    if (header->sectionCount != count)
    {
        free(file);
        free(framebuffer);
        SetError("state section layout does not match this build");
        return NATIVE_STATE_INCOMPATIBLE;
    }
    offset = sizeof(*header) + count * sizeof(struct NativeStateSectionHeader);
    for (i = 0; i < count; i++)
    {
        const struct NativeStateSectionHeader *section =
            (const struct NativeStateSectionHeader *)(file + sizeof(*header) + i * sizeof(*section));
        u32 expected = slices[i].tag;
        if (section->tag != expected || section->size != slices[i].size
         || section->size > fileSize - offset
         || section->crc != Crc32(file + offset, section->size))
        {
            free(file);
            free(framebuffer);
            SetError("state section is missing, incompatible, or corrupt");
            return NATIVE_STATE_CORRUPT;
        }
        if (!ValidateRestoreSlice(&slices[i], file + offset))
        {
            free(file);
            free(framebuffer);
            return NATIVE_STATE_UNSUPPORTED;
        }
        offset += section->size;
    }
    if (offset != fileSize)
    {
        free(file);
        free(framebuffer);
        SetError("state has trailing or missing data");
        return NATIVE_STATE_CORRUPT;
    }
    /* Validate every section before changing live game memory. A malformed
     * later section must not leave a partially restored runtime behind. */
    offset = sizeof(*header) + count * sizeof(struct NativeStateSectionHeader);
    for (i = 0; i < count; i++)
    {
        const struct NativeStateSectionHeader *section =
            (const struct NativeStateSectionHeader *)(file + sizeof(*header) + i * sizeof(*section));
        if (!RestoreSlice(&slices[i], file + offset))
        {
            free(file);
            free(framebuffer);
            if (sLastError[0] == '\0')
                SetError("state section could not be rehydrated");
            return NATIVE_STATE_UNSUPPORTED;
        }
        offset += section->size;
    }
    Platform_SchedulerSetFrameCounter(header->frame);
    Platform_AudioClearQueue();
    Platform_AudioSetPaused(FALSE);
    Platform_VideoRestoreFramebuffer(framebuffer, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(u32));
    free(file);
    free(framebuffer);

    /* R6 (§2/§3/§15): the trainer tables are plain native .data, outside the
     * serialized slices (verified against the built binary: no gba_data
     * attribute), so they keep whatever process-local values they hold across
     * a save/load round trip - possibly garbage, possibly the NULL sentinels
     * of a fresh process. Re-publish the current-session compatibility image
     * into the migrated slots unconditionally so no stale or foreign
     * process-local compat pointer can survive a load. The republish is
     * idempotent and allocation-free; if no valid session image exists, fail
     * closed by clearing the migrated entries (NULL sentinel) so consumers
     * cannot use them. The seam is guarded to the native SDL2 target
     * (emerald_trainer_native_compat.c is an empty TU elsewhere): on
     * non-native builds there are no migrated slots to re-publish, so the
     * calls must not be emitted (R7B, Windows release link). */
#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)
    {
        struct EmeraldResourceCompatDiagnostics diagnostics;
        enum EmeraldResourceCompatStatus status =
            EmeraldResourceCompat_Republish(&diagnostics);
        if (status != EMERALD_COMPAT_OK)
            EmeraldResourceCompat_ClearMigratedEntries();
    }
#endif
    return NATIVE_STATE_OK;
}

enum NativeStateResult NativeState_Save(u8 slot)
{
    char path[1024];
    sLastError[0] = '\0';
    if (slot > PLATFORM_STATE_SLOT_COUNT || !Platform_ProfileGetStatePath(slot, path, sizeof(path)))
    {
        SetError("no active profile is available for save state");
        return NATIVE_STATE_UNAVAILABLE;
    }
    return SaveStateToPath(path);
}

enum NativeStateResult NativeState_Load(u8 slot)
{
    char path[1024];
    sLastError[0] = '\0';
    if (slot > PLATFORM_STATE_SLOT_COUNT || !Platform_ProfileGetStatePath(slot, path, sizeof(path)))
    {
        SetError("no active profile is available for save state");
        return NATIVE_STATE_UNAVAILABLE;
    }
    return LoadStateFromPath(path);
}

u32 NativeState_GetFormatVersion(void)
{
    return NATIVE_STATE_FORMAT_VERSION;
}

#endif
