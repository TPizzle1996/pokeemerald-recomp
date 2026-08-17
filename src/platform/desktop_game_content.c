#ifdef PLATFORM_SDL2

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "fonts.h"
#include "pokemon.h"
#include "constants/moves.h"
#include "constants/species.h"
#include "platform/desktop_assets.h"
#include "platform/desktop_filesystem.h"
#include "platform/desktop_game_content.h"
#include "platform/desktop_storage.h"
#if defined(NATIVE_LINUX)
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "platform/native_world_neighborhood.h"
#endif

#define CONTENT_PATH_MAX 1024
#define CONTENT_MANIFEST_MAX 2048
#define CONTENT_MAGIC "EMRLDATA"
#define CONTENT_MAGIC_SIZE 8
#define CONTENT_HEADER_SIZE 64
#define CONTENT_ENTRY_SIZE 16
#define CONTENT_ROM_SIZE 16777216u
#define CONTENT_SPECIES_WIRE_SIZE 28u
#define CONTENT_MOVE_WIRE_SIZE 12u
#define CONTENT_GROWTH_RATE_COUNT 8u

enum ContentEntryId
{
    CONTENT_SPECIES_NAMES = 1,
    CONTENT_MOVE_NAMES,
    CONTENT_BATTLE_MOVES,
    CONTENT_EXPERIENCE_TABLES,
    CONTENT_SPECIES_INFO,
    CONTENT_FONT_SMALL_NARROW_LATIN,
    CONTENT_FONT_SMALL_LATIN,
    CONTENT_FONT_NARROW_LATIN,
    CONTENT_FONT_SHORT_LATIN,
    CONTENT_FONT_NORMAL_LATIN,
    CONTENT_FONT_SMALL_JAPANESE,
    CONTENT_FONT_NORMAL_JAPANESE,
    CONTENT_FONT_FRLG_MALE_JAPANESE,
    CONTENT_FONT_FRLG_FEMALE_JAPANESE,
    CONTENT_FONT_SHORT_JAPANESE,
};

struct ContentEntry
{
    u32 id;
    u32 romOffset;
    u32 size;
    u32 payloadOffset;
};

static const struct ContentEntry sCanonicalEntries[] =
{
    {CONTENT_SPECIES_NAMES,             0x3185C8, NUM_SPECIES * (POKEMON_NAME_LENGTH + 1), 0},
    {CONTENT_MOVE_NAMES,                0x31977C, MOVES_COUNT * (MOVE_NAME_LENGTH + 1), 0},
    {CONTENT_BATTLE_MOVES,              0x31C898, MOVES_COUNT * CONTENT_MOVE_WIRE_SIZE, 0},
    {CONTENT_EXPERIENCE_TABLES,         0x31F72C, 8 * (MAX_LEVEL + 1) * sizeof(u32), 0},
    {CONTENT_SPECIES_INFO,              0x3203CC, NUM_SPECIES * CONTENT_SPECIES_WIRE_SIZE, 0},
    {CONTENT_FONT_SMALL_NARROW_LATIN,   0x62BAE4, 32768, 0},
    {CONTENT_FONT_SMALL_LATIN,          0x633CE4, 32768, 0},
    {CONTENT_FONT_NARROW_LATIN,         0x63BEE4, 32768, 0},
    {CONTENT_FONT_SHORT_LATIN,          0x6440E4, 32768, 0},
    {CONTENT_FONT_NORMAL_LATIN,         0x64C2E4, 32768, 0},
    {CONTENT_FONT_SMALL_JAPANESE,       0x6544E4, 16384, 0},
    {CONTENT_FONT_NORMAL_JAPANESE,      0x6584E4, 16384, 0},
    {CONTENT_FONT_FRLG_MALE_JAPANESE,   0x65C4E4, 32768, 0},
    {CONTENT_FONT_FRLG_FEMALE_JAPANESE, 0x6646E4, 32768, 0},
    {CONTENT_FONT_SHORT_JAPANESE,       0x66C8E4, 32768, 0},
};

struct Sha1Context
{
    u32 state[5];
    u64 bitCount;
    u8 buffer[64];
    u32 bufferSize;
};

HOST_DATA static char sLastError[192];
HOST_DATA static char sInstallPath[CONTENT_PATH_MAX];
HOST_DATA static char sInstalledPackageSha1[PLATFORM_GAME_CONTENT_SHA1_LENGTH + 1];

HOST_DATA struct SpeciesInfo gSpeciesInfo[NUM_SPECIES];
HOST_DATA struct BattleMove gBattleMoves[MOVES_COUNT];
HOST_DATA u32 gExperienceTables[CONTENT_GROWTH_RATE_COUNT][MAX_LEVEL + 1];
HOST_DATA u8 gSpeciesNames[NUM_SPECIES][POKEMON_NAME_LENGTH + 1];
HOST_DATA u8 gMoveNames[MOVES_COUNT][MOVE_NAME_LENGTH + 1];

STATIC_ASSERT(sizeof(struct SpeciesInfo) == 26, DesktopSpeciesInfoSize);
STATIC_ASSERT(sizeof(struct BattleMove) == 9, DesktopBattleMoveSize);
STATIC_ASSERT(ARRAY_COUNT(sCanonicalEntries) == 15, DesktopContentEntryCount);

static u32 RotateLeft(u32 value, u32 count)
{
    return (value << count) | (value >> (32 - count));
}

static u32 ReadBe32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16)
         | ((u32)data[2] << 8) | data[3];
}

static u32 ReadLe32(const u8 *data)
{
    return data[0] | ((u32)data[1] << 8) | ((u32)data[2] << 16) | ((u32)data[3] << 24);
}

static u16 ReadLe16(const u8 *data)
{
    return data[0] | ((u16)data[1] << 8);
}

static void WriteLe32(u8 *data, u32 value)
{
    data[0] = (u8)value;
    data[1] = (u8)(value >> 8);
    data[2] = (u8)(value >> 16);
    data[3] = (u8)(value >> 24);
}

static void Sha1Transform(struct Sha1Context *context, const u8 block[64])
{
    u32 words[80];
    u32 a, b, c, d, e;
    u32 i;

    for (i = 0; i < 16; i++)
        words[i] = ReadBe32(block + i * 4);
    for (; i < 80; i++)
        words[i] = RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    for (i = 0; i < 80; i++)
    {
        u32 f;
        u32 k;
        u32 temp;
        if (i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        temp = RotateLeft(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = RotateLeft(b, 30);
        b = a;
        a = temp;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
}

static void Sha1Init(struct Sha1Context *context)
{
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->bitCount = 0;
    context->bufferSize = 0;
}

static void Sha1Update(struct Sha1Context *context, const void *bytes, u32 size)
{
    const u8 *data = bytes;
    context->bitCount += (u64)size * 8;
    while (size != 0)
    {
        u32 count = min(size, 64 - context->bufferSize);
        memcpy(context->buffer + context->bufferSize, data, count);
        context->bufferSize += count;
        data += count;
        size -= count;
        if (context->bufferSize == 64)
        {
            Sha1Transform(context, context->buffer);
            context->bufferSize = 0;
        }
    }
}

static void Sha1Final(struct Sha1Context *context, u8 digest[20])
{
    u64 bitCount = context->bitCount;
    u8 marker = 0x80;
    u8 zero = 0;
    u8 length[8];
    u32 i;

    Sha1Update(context, &marker, 1);
    while (context->bufferSize != 56)
        Sha1Update(context, &zero, 1);
    for (i = 0; i < 8; i++)
        length[7 - i] = (u8)(bitCount >> (i * 8));
    Sha1Update(context, length, sizeof(length));
    for (i = 0; i < 5; i++)
    {
        digest[i * 4] = (u8)(context->state[i] >> 24);
        digest[i * 4 + 1] = (u8)(context->state[i] >> 16);
        digest[i * 4 + 2] = (u8)(context->state[i] >> 8);
        digest[i * 4 + 3] = (u8)context->state[i];
    }
}

static void DigestToHex(const u8 digest[20], char hex[41])
{
    static const char digits[] = "0123456789abcdef";
    u32 i;
    for (i = 0; i < 20; i++)
    {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[40] = '\0';
}

static bool32 HexToDigest(const char *hex, u8 digest[20])
{
    u32 i;
    if (hex == NULL || strlen(hex) != 40)
        return FALSE;
    for (i = 0; i < 20; i++)
    {
        char high = hex[i * 2];
        char low = hex[i * 2 + 1];
        int a = high >= '0' && high <= '9' ? high - '0'
              : high >= 'a' && high <= 'f' ? high - 'a' + 10 : -1;
        int b = low >= '0' && low <= '9' ? low - '0'
              : low >= 'a' && low <= 'f' ? low - 'a' + 10 : -1;
        if (a < 0 || b < 0)
            return FALSE;
        digest[i] = (u8)((a << 4) | b);
    }
    return TRUE;
}

static bool32 HashOpenFile(FILE *file, u32 offset, u32 size, u8 digest[20])
{
    struct Sha1Context context;
    u8 buffer[8192];
    u32 remaining = size;

    if (file == NULL || fseek(file, (long)offset, SEEK_SET) != 0)
        return FALSE;
    Sha1Init(&context);
    while (remaining != 0)
    {
        u32 count = min(remaining, (u32)sizeof(buffer));
        if (fread(buffer, 1, count, file) != count)
            return FALSE;
        Sha1Update(&context, buffer, count);
        remaining -= count;
    }
    Sha1Final(&context, digest);
    return TRUE;
}

static bool32 HashFile(const char *path, char hex[41], u32 *fileSize)
{
    FILE *file = Platform_FileOpen(path, "rb");
    long size;
    u8 digest[20];
    bool32 result;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
    {
        if (file != NULL) fclose(file);
        return FALSE;
    }
    size = ftell(file);
    if (size < 0 || (unsigned long)size > UINT32_MAX)
    {
        fclose(file);
        return FALSE;
    }
    result = HashOpenFile(file, 0, (u32)size, digest);
    fclose(file);
    if (!result)
        return FALSE;
    DigestToHex(digest, hex);
    if (fileSize != NULL)
        *fileSize = (u32)size;
    return TRUE;
}

static void SetError(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(sLastError, sizeof(sLastError), format, args);
    va_end(args);
}

static bool32 JoinPath(char *dest, u32 destSize, const char *root, const char *suffix)
{
    int length = snprintf(dest, destSize, "%s/%s", root, suffix);
    return length >= 0 && (u32)length < destSize;
}

static bool32 BuildPaths(char *directory, char *manifest, char *package)
{
    char games[CONTENT_PATH_MAX];
    const char *root = Platform_StorageGetRootPath();

    if (!JoinPath(games, sizeof(games), root, "games")
     || !JoinPath(directory, CONTENT_PATH_MAX, games, "emerald")
     || !JoinPath(manifest, CONTENT_PATH_MAX, directory, "manifest.json")
     || !JoinPath(package, CONTENT_PATH_MAX, directory, "content.pak"))
        return FALSE;
    snprintf(sInstallPath, sizeof(sInstallPath), "%s", directory);
    return TRUE;
}

static u32 PayloadStart(void)
{
    return CONTENT_HEADER_SIZE + ARRAY_COUNT(sCanonicalEntries) * CONTENT_ENTRY_SIZE;
}

static u32 PayloadSize(void)
{
    u32 size = 0;
    u32 i;
    for (i = 0; i < ARRAY_COUNT(sCanonicalEntries); i++)
        size += sCanonicalEntries[i].size;
    return size;
}

static bool32 WritePackage(const char *romPath, const char *packagePath)
{
    FILE *rom = Platform_FileOpen(romPath, "rb");
    FILE *package = Platform_FileOpen(packagePath, "wb+");
    struct Sha1Context payloadContext;
    u8 header[CONTENT_HEADER_SIZE];
    u8 entryBytes[CONTENT_ENTRY_SIZE];
    u8 sourceDigest[20];
    u8 payloadDigest[20];
    u8 buffer[8192];
    u32 payloadOffset = 0;
    u32 i;
    bool32 ok = TRUE;

    if (rom == NULL || package == NULL || !HexToDigest(EMERALD_EXPECTED_SHA1, sourceDigest))
    {
        if (rom != NULL) fclose(rom);
        if (package != NULL) fclose(package);
        return FALSE;
    }
    memset(header, 0, sizeof(header));
    if (fwrite(header, 1, sizeof(header), package) != sizeof(header))
        ok = FALSE;
    for (i = 0; ok && i < ARRAY_COUNT(sCanonicalEntries); i++)
    {
        memset(entryBytes, 0, sizeof(entryBytes));
        if (fwrite(entryBytes, 1, sizeof(entryBytes), package) != sizeof(entryBytes))
            ok = FALSE;
    }
    Sha1Init(&payloadContext);
    for (i = 0; ok && i < ARRAY_COUNT(sCanonicalEntries); i++)
    {
        const struct ContentEntry *entry = &sCanonicalEntries[i];
        u32 remaining = entry->size;
        if (fseek(rom, (long)entry->romOffset, SEEK_SET) != 0)
        {
            ok = FALSE;
            break;
        }
        while (remaining != 0)
        {
            u32 count = min(remaining, (u32)sizeof(buffer));
            if (fread(buffer, 1, count, rom) != count
             || fwrite(buffer, 1, count, package) != count)
            {
                ok = FALSE;
                break;
            }
            Sha1Update(&payloadContext, buffer, count);
            remaining -= count;
        }
    }
    Sha1Final(&payloadContext, payloadDigest);
    if (ok)
    {
        memset(header, 0, sizeof(header));
        memcpy(header, CONTENT_MAGIC, CONTENT_MAGIC_SIZE);
        WriteLe32(header + 8, PLATFORM_GAME_CONTENT_FORMAT_VERSION);
        WriteLe32(header + 12, ARRAY_COUNT(sCanonicalEntries));
        memcpy(header + 16, sourceDigest, sizeof(sourceDigest));
        memcpy(header + 36, payloadDigest, sizeof(payloadDigest));
        WriteLe32(header + 56, PayloadSize());
        if (fseek(package, 0, SEEK_SET) != 0
         || fwrite(header, 1, sizeof(header), package) != sizeof(header))
            ok = FALSE;
    }
    for (i = 0; ok && i < ARRAY_COUNT(sCanonicalEntries); i++)
    {
        const struct ContentEntry *entry = &sCanonicalEntries[i];
        memset(entryBytes, 0, sizeof(entryBytes));
        WriteLe32(entryBytes, entry->id);
        WriteLe32(entryBytes + 4, entry->romOffset);
        WriteLe32(entryBytes + 8, entry->size);
        WriteLe32(entryBytes + 12, payloadOffset);
        if (fwrite(entryBytes, 1, sizeof(entryBytes), package) != sizeof(entryBytes))
            ok = FALSE;
        payloadOffset += entry->size;
    }
    if (fflush(package) != 0 || fclose(package) != 0)
        ok = FALSE;
    fclose(rom);
    if (!ok)
        Platform_StorageRemoveFile(packagePath);
    return ok;
}

static bool32 ExtractJsonString(const char *json, const char *key, char *dest, u32 destSize)
{
    char pattern[64];
    const char *start;
    const char *end;
    size_t length;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    start = strstr(json, pattern);
    if (start == NULL || (start = strchr(start + strlen(pattern), ':')) == NULL
     || (start = strchr(start, '"')) == NULL)
        return FALSE;
    start++;
    end = strchr(start, '"');
    if (end == NULL)
        return FALSE;
    length = (size_t)(end - start);
    if (length + 1 > destSize)
        return FALSE;
    memcpy(dest, start, length);
    dest[length] = '\0';
    return TRUE;
}

static bool32 ExtractJsonUnsigned(const char *json, const char *key, u32 *value)
{
    char pattern[64];
    const char *start;
    unsigned parsed;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    start = strstr(json, pattern);
    if (start == NULL || (start = strchr(start + strlen(pattern), ':')) == NULL
     || sscanf(start + 1, "%u", &parsed) != 1)
        return FALSE;
    *value = parsed;
    return TRUE;
}

static bool32 ReadEntryData(FILE *package, u32 payloadOffset, const struct ContentEntry *entry,
                            void *dest, u32 size)
{
    if (size != entry->size
     || fseek(package, (long)(PayloadStart() + payloadOffset), SEEK_SET) != 0)
        return FALSE;
    return fread(dest, 1, size, package) == size;
}

static bool32 LoadFont(FILE *package, u32 payloadOffset, const struct ContentEntry *entry,
                       u16 *dest, u32 destBytes)
{
    u8 bytes[8192];
    u32 remaining = destBytes;
    u32 index = 0;
    if (entry->size != destBytes
     || fseek(package, (long)(PayloadStart() + payloadOffset), SEEK_SET) != 0)
        return FALSE;
    while (remaining != 0)
    {
        u32 count = min(remaining, (u32)sizeof(bytes));
        u32 i;
        if (fread(bytes, 1, count, package) != count)
            return FALSE;
        for (i = 0; i < count; i += 2)
            dest[index++] = ReadLe16(bytes + i);
        remaining -= count;
    }
    return TRUE;
}

static bool32 HydrateEntry(FILE *package, const struct ContentEntry *entry)
{
    u32 payloadOffset = entry->payloadOffset;
    u32 i;
    switch (entry->id)
    {
    case CONTENT_SPECIES_NAMES:
        return ReadEntryData(package, payloadOffset, entry, gSpeciesNames, sizeof(gSpeciesNames));
    case CONTENT_MOVE_NAMES:
        return ReadEntryData(package, payloadOffset, entry, gMoveNames, sizeof(gMoveNames));
    case CONTENT_EXPERIENCE_TABLES:
    {
        u8 bytes[sizeof(gExperienceTables)];
        if (!ReadEntryData(package, payloadOffset, entry, bytes, sizeof(bytes)))
            return FALSE;
        for (i = 0; i < sizeof(gExperienceTables) / sizeof(u32); i++)
            ((u32 *)gExperienceTables)[i] = ReadLe32(bytes + i * 4);
        return TRUE;
    }
    case CONTENT_BATTLE_MOVES:
        for (i = 0; i < MOVES_COUNT; i++)
        {
            u8 wire[CONTENT_MOVE_WIRE_SIZE];
            struct BattleMove *move = &gBattleMoves[i];
            if (fseek(package, (long)(PayloadStart() + payloadOffset
                                      + i * CONTENT_MOVE_WIRE_SIZE), SEEK_SET) != 0
             || fread(wire, 1, sizeof(wire), package) != sizeof(wire))
                return FALSE;
            move->effect = wire[0];
            move->power = wire[1];
            move->type = wire[2];
            move->accuracy = wire[3];
            move->pp = wire[4];
            move->secondaryEffectChance = wire[5];
            move->target = wire[6];
            move->priority = (s8)wire[7];
            move->flags = wire[8];
        }
        return TRUE;
    case CONTENT_SPECIES_INFO:
        for (i = 0; i < NUM_SPECIES; i++)
        {
            u8 wire[CONTENT_SPECIES_WIRE_SIZE];
            u16 ev;
            struct SpeciesInfo *species = &gSpeciesInfo[i];
            if (fseek(package, (long)(PayloadStart() + payloadOffset
                                      + i * CONTENT_SPECIES_WIRE_SIZE), SEEK_SET) != 0
             || fread(wire, 1, sizeof(wire), package) != sizeof(wire))
                return FALSE;
            memset(species, 0, sizeof(*species));
            species->baseHP = wire[0];
            species->baseAttack = wire[1];
            species->baseDefense = wire[2];
            species->baseSpeed = wire[3];
            species->baseSpAttack = wire[4];
            species->baseSpDefense = wire[5];
            species->types[0] = wire[6];
            species->types[1] = wire[7];
            species->catchRate = wire[8];
            species->expYield = wire[9];
            ev = ReadLe16(wire + 10);
            species->evYield_HP = ev & 3;
            species->evYield_Attack = (ev >> 2) & 3;
            species->evYield_Defense = (ev >> 4) & 3;
            species->evYield_Speed = (ev >> 6) & 3;
            species->evYield_SpAttack = (ev >> 8) & 3;
            species->evYield_SpDefense = (ev >> 10) & 3;
            species->itemCommon = ReadLe16(wire + 12);
            species->itemRare = ReadLe16(wire + 14);
            species->genderRatio = wire[16];
            species->eggCycles = wire[17];
            species->friendship = wire[18];
            species->growthRate = wire[19];
            species->eggGroups[0] = wire[20];
            species->eggGroups[1] = wire[21];
            species->abilities[0] = wire[22];
            species->abilities[1] = wire[23];
            species->safariZoneFleeRate = wire[24];
            species->bodyColor = wire[25] & 0x7F;
            species->noFlip = wire[25] >> 7;
        }
        return TRUE;
    case CONTENT_FONT_SMALL_NARROW_LATIN:
        return LoadFont(package, payloadOffset, entry, gFontSmallNarrowLatinGlyphs, 32768);
    case CONTENT_FONT_SMALL_LATIN:
        return LoadFont(package, payloadOffset, entry, gFontSmallLatinGlyphs, 32768);
    case CONTENT_FONT_NARROW_LATIN:
        return LoadFont(package, payloadOffset, entry, gFontNarrowLatinGlyphs, 32768);
    case CONTENT_FONT_SHORT_LATIN:
        return LoadFont(package, payloadOffset, entry, gFontShortLatinGlyphs, 32768);
    case CONTENT_FONT_NORMAL_LATIN:
        return LoadFont(package, payloadOffset, entry, gFontNormalLatinGlyphs, 32768);
    case CONTENT_FONT_SMALL_JAPANESE:
        return LoadFont(package, payloadOffset, entry, gFontSmallJapaneseGlyphs, 16384);
    case CONTENT_FONT_NORMAL_JAPANESE:
        return LoadFont(package, payloadOffset, entry, gFontNormalJapaneseGlyphs, 16384);
    case CONTENT_FONT_FRLG_MALE_JAPANESE:
        return LoadFont(package, payloadOffset, entry, gFontFRLGMaleJapaneseGlyphs, 32768);
    case CONTENT_FONT_FRLG_FEMALE_JAPANESE:
        return LoadFont(package, payloadOffset, entry, gFontFRLGFemaleJapaneseGlyphs, 32768);
    case CONTENT_FONT_SHORT_JAPANESE:
        return LoadFont(package, payloadOffset, entry, gFontShortJapaneseGlyphs, 32768);
    default:
        return FALSE;
    }
}

static bool32 VerifyPaths(const char *manifestPath, const char *packagePath, bool32 hydrate)
{
    char manifest[CONTENT_MANIFEST_MAX];
    char game[32];
    char sourceSha1[41];
    char revision[96];
    char manifestPackageSha1[41];
    char actualPackageSha1[41];
    u32 manifestSize;
    u32 version;
    u32 manifestPackageSize;
    u32 packageSize;
    FILE *package;
    u8 header[CONTENT_HEADER_SIZE];
    u8 expectedSourceDigest[20];
    u8 payloadDigest[20];
    u8 actualPayloadDigest[20];
    u32 payloadOffset = 0;
    u32 i;

    if (!Platform_StorageReadFile(manifestPath, manifest, sizeof(manifest) - 1, &manifestSize))
    {
        SetError("Emerald content manifest is missing or unreadable");
        return FALSE;
    }
    manifest[manifestSize] = '\0';
    if (!ExtractJsonString(manifest, "game", game, sizeof(game)) || strcmp(game, "emerald") != 0
     || !ExtractJsonString(manifest, "source_sha1", sourceSha1, sizeof(sourceSha1))
     || strcmp(sourceSha1, EMERALD_EXPECTED_SHA1) != 0
     || !ExtractJsonString(manifest, "source_revision", revision, sizeof(revision))
     || strcmp(revision, EMERALD_SUPPORTED_REVISION) != 0
     || !ExtractJsonUnsigned(manifest, "content_format_version", &version)
     || version != PLATFORM_GAME_CONTENT_FORMAT_VERSION
     || !ExtractJsonString(manifest, "content_sha1", manifestPackageSha1,
                           sizeof(manifestPackageSha1))
     || !ExtractJsonUnsigned(manifest, "content_size", &manifestPackageSize))
    {
        SetError("Emerald content manifest is incompatible or corrupt");
        return FALSE;
    }
    if (!HashFile(packagePath, actualPackageSha1, &packageSize)
     || strcmp(actualPackageSha1, manifestPackageSha1) != 0)
    {
        SetError("Emerald content package checksum does not match its manifest");
        return FALSE;
    }
    if (packageSize != manifestPackageSize || packageSize != PayloadStart() + PayloadSize())
    {
        SetError("Emerald content package has the wrong size");
        return FALSE;
    }
    package = Platform_FileOpen(packagePath, "rb");
    if (package == NULL || fread(header, 1, sizeof(header), package) != sizeof(header))
    {
        if (package != NULL) fclose(package);
        SetError("Emerald content package is unreadable");
        return FALSE;
    }
    if (!HexToDigest(EMERALD_EXPECTED_SHA1, expectedSourceDigest)
     || memcmp(header, CONTENT_MAGIC, CONTENT_MAGIC_SIZE) != 0
     || ReadLe32(header + 8) != PLATFORM_GAME_CONTENT_FORMAT_VERSION
     || ReadLe32(header + 12) != ARRAY_COUNT(sCanonicalEntries)
     || memcmp(header + 16, expectedSourceDigest, sizeof(expectedSourceDigest)) != 0
     || ReadLe32(header + 56) != PayloadSize())
    {
        fclose(package);
        SetError("Emerald content package header is incompatible or corrupt");
        return FALSE;
    }
    memcpy(payloadDigest, header + 36, sizeof(payloadDigest));
    for (i = 0; i < ARRAY_COUNT(sCanonicalEntries); i++)
    {
        u8 bytes[CONTENT_ENTRY_SIZE];
        struct ContentEntry entry;
        const struct ContentEntry *expected = &sCanonicalEntries[i];
        if (fread(bytes, 1, sizeof(bytes), package) != sizeof(bytes))
        {
            fclose(package);
            SetError("Emerald content package entry table is truncated");
            return FALSE;
        }
        entry.id = ReadLe32(bytes);
        entry.romOffset = ReadLe32(bytes + 4);
        entry.size = ReadLe32(bytes + 8);
        entry.payloadOffset = ReadLe32(bytes + 12);
        if (entry.id != expected->id || entry.romOffset != expected->romOffset
         || entry.size != expected->size || entry.payloadOffset != payloadOffset)
        {
            fclose(package);
            SetError("Emerald content package entry table is incompatible");
            return FALSE;
        }
        payloadOffset += entry.size;
    }
    if (!HashOpenFile(package, PayloadStart(), PayloadSize(), actualPayloadDigest)
     || memcmp(payloadDigest, actualPayloadDigest, sizeof(payloadDigest)) != 0)
    {
        fclose(package);
        SetError("Emerald content package payload is corrupt");
        return FALSE;
    }
    if (hydrate)
    {
        payloadOffset = 0;
        for (i = 0; i < ARRAY_COUNT(sCanonicalEntries); i++)
        {
            struct ContentEntry entry = sCanonicalEntries[i];
            entry.payloadOffset = payloadOffset;
            if (!HydrateEntry(package, &entry))
            {
                fclose(package);
                SetError("Emerald content package could not hydrate runtime data");
                return FALSE;
            }
            payloadOffset += entry.size;
        }
    }
#if defined(NATIVE_LINUX)
    /* R5 explicit init point (§14): after the content package has fully
     * hydrated runtime data, run the ROM_BASE trainer-table compatibility
     * publication once. The R6 loader registers the production ROM_BASE
     * snapshot from the production pack read from disk first (idempotent,
     * fail-closed); TryInitialize then publishes the three migrated Brendan
     * slots. The pack is resolved via the asset-path helper (exe-relative then
     * CWD-relative) and is deliberately NOT embedded in the binary: R7A
     * requires every ROM_BASE_ONLY payload to be absent from the native link.
     * Without a valid snapshot it stays a no-op and never mutates the tables. */
    char packPath[CONTENT_PATH_MAX];
    if (Platform_AssetGetPath("games/emerald/base/emerald-bpee01-v1.rpack",
                              packPath, sizeof(packPath)))
        EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath);
    EmeraldResourceCompat_TryInitialize();
    /* R11-E/F: the neighborhood's one-time init point (plan §10): the map
     * tables, tilesets and layouts are all canonical by now (hydration +
     * publication), so seed the lazy cache with a zeroed state; the first
     * EnsureCurrent call after a map identity is set builds the
     * neighborhood. Allocation-free, idempotent. */
    NativeWorldNeighborhood_Init();
#endif
    fclose(package);
    snprintf(sInstalledPackageSha1, sizeof(sInstalledPackageSha1), "%s", actualPackageSha1);
    sLastError[0] = '\0';
    return TRUE;
}

bool32 Platform_GameContentVerifyInstalled(bool32 hydrate)
{
    char directory[CONTENT_PATH_MAX];
    char manifest[CONTENT_PATH_MAX];
    char package[CONTENT_PATH_MAX];
    if (!BuildPaths(directory, manifest, package))
    {
        SetError("Emerald content installation path is too long");
        return FALSE;
    }
    return VerifyPaths(manifest, package, hydrate);
}

static bool32 EnsureInstallDirectories(char *directory, char *manifest, char *package)
{
    char games[CONTENT_PATH_MAX];
    if (!BuildPaths(directory, manifest, package)
     || !JoinPath(games, sizeof(games), Platform_StorageGetRootPath(), "games")
     || !Platform_StorageEnsureDirectory(games)
     || !Platform_StorageEnsureDirectory(directory))
        return FALSE;
    return TRUE;
}

static bool32 ReplaceInstalledPackage(const char *tempManifest, const char *tempPackage,
                                      const char *manifest, const char *package)
{
    char backupManifest[CONTENT_PATH_MAX];
    char backupPackage[CONTENT_PATH_MAX];
    bool32 hadManifest = Platform_StorageFileExists(manifest);
    bool32 hadPackage = Platform_StorageFileExists(package);

    snprintf(backupManifest, sizeof(backupManifest), "%s.backup", manifest);
    snprintf(backupPackage, sizeof(backupPackage), "%s.backup", package);
    Platform_StorageRemoveFile(backupManifest);
    Platform_StorageRemoveFile(backupPackage);
    if ((hadManifest && !Platform_StorageCopyFileAtomic(manifest, backupManifest))
     || (hadPackage && !Platform_StorageCopyFileAtomic(package, backupPackage)))
        return FALSE;
    if (!Platform_FileReplace(tempPackage, package)
     || !Platform_FileReplace(tempManifest, manifest)
     || !Platform_GameContentVerifyInstalled(TRUE))
    {
        if (hadPackage)
            Platform_StorageCopyFileAtomic(backupPackage, package);
        else
            Platform_StorageRemoveFile(package);
        if (hadManifest)
            Platform_StorageCopyFileAtomic(backupManifest, manifest);
        else
            Platform_StorageRemoveFile(manifest);
        Platform_StorageRemoveFile(tempPackage);
        Platform_StorageRemoveFile(tempManifest);
        Platform_StorageRemoveFile(backupPackage);
        Platform_StorageRemoveFile(backupManifest);
        return FALSE;
    }
    Platform_StorageRemoveFile(backupPackage);
    Platform_StorageRemoveFile(backupManifest);
    return TRUE;
}

enum PlatformGameContentImportResult Platform_GameContentImport(
    const char *romPath, struct PlatformGameContentImportInfo *info)
{
    struct PlatformGameContentImportInfo localInfo;
    char directory[CONTENT_PATH_MAX];
    char manifest[CONTENT_PATH_MAX];
    char package[CONTENT_PATH_MAX];
    char tempManifest[CONTENT_PATH_MAX];
    char tempPackage[CONTENT_PATH_MAX];
    char packageSha1[41];
    char manifestJson[CONTENT_MANIFEST_MAX];
    u32 romSize = 0;
    u32 packageSize = 0;
    int manifestLength;

    if (info == NULL)
        info = &localInfo;
    memset(info, 0, sizeof(*info));
    info->result = PLATFORM_GAME_CONTENT_IMPORT_UNREADABLE;
    if (romPath == NULL || !HashFile(romPath, info->detectedSha1, &romSize))
    {
        snprintf(info->error, sizeof(info->error), "The selected ROM could not be read");
        SetError("%s", info->error);
        return info->result;
    }
    if (romSize != CONTENT_ROM_SIZE || strcmp(info->detectedSha1, EMERALD_EXPECTED_SHA1) != 0)
    {
        info->result = PLATFORM_GAME_CONTENT_IMPORT_UNSUPPORTED_ROM;
        snprintf(info->error, sizeof(info->error), "Unsupported Pokemon Emerald ROM");
        SetError("%s", info->error);
        return info->result;
    }
    info->result = PLATFORM_GAME_CONTENT_IMPORT_INSTALL_FAILED;
    if (!EnsureInstallDirectories(directory, manifest, package))
    {
        snprintf(info->error, sizeof(info->error), "The game-data directory could not be created");
        SetError("%s", info->error);
        return info->result;
    }
    snprintf(tempManifest, sizeof(tempManifest), "%s.import", manifest);
    snprintf(tempPackage, sizeof(tempPackage), "%s.import", package);
    Platform_StorageRemoveFile(tempManifest);
    Platform_StorageRemoveFile(tempPackage);
    if (!WritePackage(romPath, tempPackage)
     || !HashFile(tempPackage, packageSha1, &packageSize))
    {
        snprintf(info->error, sizeof(info->error), "The local content package could not be generated");
        SetError("%s", info->error);
        return info->result;
    }
    manifestLength = snprintf(manifestJson, sizeof(manifestJson),
        "{\n"
        "  \"game\": \"emerald\",\n"
        "  \"source_sha1\": \"%s\",\n"
        "  \"source_revision\": \"%s\",\n"
        "  \"content_format_version\": %u,\n"
        "  \"content_sha1\": \"%s\",\n"
        "  \"content_size\": %u\n"
        "}\n",
        EMERALD_EXPECTED_SHA1, EMERALD_SUPPORTED_REVISION,
        PLATFORM_GAME_CONTENT_FORMAT_VERSION, packageSha1, packageSize);
    if (manifestLength < 0 || (u32)manifestLength >= sizeof(manifestJson)
     || !Platform_StorageWriteAtomic(tempManifest, manifestJson, (u32)manifestLength)
     || !VerifyPaths(tempManifest, tempPackage, FALSE)
     || !ReplaceInstalledPackage(tempManifest, tempPackage, manifest, package))
    {
        Platform_StorageRemoveFile(tempManifest);
        Platform_StorageRemoveFile(tempPackage);
        snprintf(info->error, sizeof(info->error), "The local content package could not be installed safely");
        SetError("%s", info->error);
        return info->result;
    }
    info->result = PLATFORM_GAME_CONTENT_IMPORT_OK;
    info->error[0] = '\0';
    sLastError[0] = '\0';
    return info->result;
}

const char *Platform_GameContentGetExpectedSha1(void)
{
    return EMERALD_EXPECTED_SHA1;
}

const char *Platform_GameContentGetRevision(void)
{
    return EMERALD_SUPPORTED_REVISION;
}

const char *Platform_GameContentGetLastError(void)
{
    return sLastError;
}

const char *Platform_GameContentGetInstallPath(void)
{
    return sInstallPath;
}

const char *Platform_GameContentGetInstalledPackageSha1(void)
{
    return sInstalledPackageSha1;
}

#endif
