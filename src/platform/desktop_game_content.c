#ifdef PLATFORM_SDL2

/* R13-D1: the legacy content.pak is RETIRED.
 *
 * The old flow built content.pak + manifest.json (EMRLDATA, 15 entries) from
 * the verified ROM and hydrated the five gameplay tables + ten fonts at boot.
 * R13-D1 absorbs all 15 into the production .rpack as STRUCTURED_DATA / FONT
 * resources and retires the second package entirely. The EMRLDATA writer/
 * reader, sCanonicalEntries, manifest.json handling, the SHA-1 machinery and
 * the five hydrated array definitions (now in
 * src/emerald/resources/gameplay_data_native.c) are removed; the gameplay
 * tables + fonts are published by the R13-D1 seam
 * (EmeraldGameplayCompat_TryInitialize) at boot.
 *
 * The public surface (Platform_GameContentVerifyInstalled /
 * Platform_GameContentImport + the Get* accessors) keeps its exact signatures
 * so src/platform/sdl2.c and src/platform/desktop_frontend.c need no edits.
 *
 * VerifyInstalled(hydrate): verifies the production pack
 * (games/emerald/base/emerald-bpee01-v1.rpack) is present + valid; when
 * hydrate is TRUE it ALSO runs the NATIVE_LINUX registration block
 * (RegisterRuntimeSnapshot -> TryInitialize -> NativeWorldNeighborhood_Init).
 *
 * Import(romPath): runs the proven ROM->.rpack importer
 * (EmeraldImport_ValidateRom / BuildPack / Install) against the merged
 * manifest+catalog set (same manifest/catalog set as
 * tools/gen3_resources/pack_build/build_d1_pack.sh), atomically installing
 * the single authoritative pack.
 *
 * No content.pak/manifest.json is read anywhere.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "platform/desktop_assets.h"
#include "platform/desktop_filesystem.h"
#include "platform/desktop_game_content.h"
#include "platform/desktop_storage.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/sha1.h"
#include "emerald/resources/emerald_resource_import.h"
#include "emerald/resources/emerald_rom_profile.h"
#if defined(NATIVE_LINUX)
#include "emerald/resources/emerald_trainer_native_compat.h"
#include "platform/native_world_neighborhood.h"
#endif

#define CONTENT_PATH_MAX 1024

#define EMERALD_PACK_ASSET "games/emerald/base/emerald-bpee01-v1.rpack"
#define EMERALD_PACK_FILE  "emerald-bpee01-v1.rpack"

/* The merged production manifest + catalog set (identical to
 * tools/gen3_resources/pack_build/build_d1_pack.sh). */
#define EMERALD_IMPORT_MANIFEST_COUNT 10u
#define EMERALD_IMPORT_CATALOG_COUNT  10u

static const char *const sImportManifests[EMERALD_IMPORT_MANIFEST_COUNT] =
{
    "resources/extraction/emerald/bpee01/manifest.production.toml",
    "resources/extraction/emerald/bpee01/pokemon_battle/manifest.production.toml",
    "resources/extraction/emerald/bpee01/object_event/manifest.production.toml",
    "resources/extraction/emerald/bpee01/tileset/manifest.production.toml",
    "resources/extraction/emerald/bpee01/layout/manifest.production.toml",
    "resources/extraction/emerald/bpee01/audio/manifest.production.toml",
    "resources/extraction/emerald/bpee01/movement/manifest.production.toml",
    "resources/extraction/emerald/bpee01/multiboot/manifest.production.toml",
    "resources/extraction/emerald/bpee01/text/manifest.production.toml",
    "resources/extraction/emerald/bpee01/gameplay/manifest.production.toml",
};

static const char *const sImportCatalogs[EMERALD_IMPORT_CATALOG_COUNT] =
{
    "resources/catalogs/emerald/catalog.toml",
    "resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml",
    "resources/extraction/emerald/bpee01/object_event/catalog.generated.toml",
    "resources/extraction/emerald/bpee01/tileset/catalog.generated.toml",
    "resources/extraction/emerald/bpee01/layout/catalog.generated.toml",
    "resources/extraction/emerald/bpee01/audio/catalog.generated.toml",
    "resources/extraction/emerald/bpee01/movement/catalog.generated.toml",
    "resources/extraction/emerald/bpee01/multiboot/catalog.generated.toml",
    "resources/extraction/emerald/bpee01/text/catalog.generated.toml",
    "resources/extraction/emerald/bpee01/gameplay/catalog.generated.toml",
};

HOST_DATA static char sLastError[192];
HOST_DATA static char sInstallPath[CONTENT_PATH_MAX];
HOST_DATA static char sInstalledPackageSha1[PLATFORM_GAME_CONTENT_SHA1_LENGTH + 1];

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

/* Build the install path: <storage>/games/emerald/base/<pack>. */
static bool32 BuildInstallPaths(char *packPath)
{
    char games[CONTENT_PATH_MAX];
    char dir[CONTENT_PATH_MAX];
    char base[CONTENT_PATH_MAX];
    const char *root = Platform_StorageGetRootPath();

    if (!JoinPath(games, sizeof(games), root, "games")
     || !JoinPath(dir, sizeof(dir), games, "emerald")
     || !JoinPath(base, sizeof(base), dir, "base")
     || !JoinPath(packPath, CONTENT_PATH_MAX, base, EMERALD_PACK_FILE))
        return FALSE;
    snprintf(sInstallPath, sizeof(sInstallPath), "%s", base);
    return TRUE;
}

static bool32 EnsureInstallDirectories(const char *packPath)
{
    char games[CONTENT_PATH_MAX];
    char dir[CONTENT_PATH_MAX];
    char base[CONTENT_PATH_MAX];
    const char *root = Platform_StorageGetRootPath();

    if (!JoinPath(games, sizeof(games), root, "games")
     || !JoinPath(dir, sizeof(dir), games, "emerald")
     || !JoinPath(base, sizeof(base), dir, "base")
     || !Platform_StorageEnsureDirectory(games)
     || !Platform_StorageEnsureDirectory(dir)
     || !Platform_StorageEnsureDirectory(base))
        return FALSE;
    return packPath != NULL;
}

static void DigestSha1(const u8 *data, size_t size, u8 digest[GEN3_SHA1_DIGEST_SIZE])
{
    struct Gen3Sha1Context ctx;
    Gen3Sha1_Init(&ctx);
    Gen3Sha1_Update(&ctx, data, size);
    Gen3Sha1_Final(&ctx, digest);
}

static bool32 HashFile(const char *path, char hex[41])
{
    FILE *file = Platform_FileOpen(path, "rb");
    u8 buffer[8192];
    u8 digest[GEN3_SHA1_DIGEST_SIZE];
    static const char digits[] = "0123456789abcdef";
    struct Gen3Sha1Context ctx;
    size_t read;
    u32 i;

    if (file == NULL)
        return FALSE;
    Gen3Sha1_Init(&ctx);
    while ((read = fread(buffer, 1, sizeof(buffer), file)) != 0u)
        Gen3Sha1_Update(&ctx, buffer, read);
    Gen3Sha1_Final(&ctx, digest);
    fclose(file);
    for (i = 0; i < GEN3_SHA1_DIGEST_SIZE; i++)
    {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[GEN3_SHA1_DIGEST_SIZE * 2] = '\0';
    return TRUE;
}

static bool32 LoadProductionPack(const char *path, struct Gen3ResourcePack **outPack)
{
    struct Gen3ResourcePackDiagnosticList diagnostics;
    enum Gen3ResourcePackError error;

    if (outPack != NULL)
        *outPack = NULL;
    if (path == NULL || outPack == NULL)
        return FALSE;
    Gen3ResourcePackDiagnostics_Init(&diagnostics);
    error = Gen3ResourcePack_OpenFile(path, outPack, &diagnostics);
    Gen3ResourcePackDiagnostics_Destroy(&diagnostics);
    return error == GEN3_PACK_OK && *outPack != NULL;
}

static void SetInstalledPackageSha1(struct Gen3ResourcePack *pack)
{
    struct Gen3ResourcePackProfile profile;
    static const char digits[] = "0123456789abcdef";
    u32 i;

    if (pack == NULL || !Gen3ResourcePack_GetProfile(pack, &profile))
    {
        sInstalledPackageSha1[0] = '\0';
        return;
    }
    for (i = 0; i < GEN3_PACK_SHA1_SIZE; i++)
    {
        sInstalledPackageSha1[i * 2] = digits[profile.sourceRomSha1[i] >> 4];
        sInstalledPackageSha1[i * 2 + 1] = digits[profile.sourceRomSha1[i] & 15];
    }
    sInstalledPackageSha1[GEN3_PACK_SHA1_SIZE * 2] = '\0';
}

bool32 Platform_GameContentVerifyInstalled(bool32 hydrate)
{
    char packPath[CONTENT_PATH_MAX];
    struct Gen3ResourcePack *pack = NULL;

    if (!Platform_AssetGetPath(EMERALD_PACK_ASSET, packPath, sizeof(packPath)))
    {
        SetError("Emerald production pack is not installed");
        return FALSE;
    }
    if (!LoadProductionPack(packPath, &pack))
    {
        SetError("Emerald production pack is missing or invalid");
        return FALSE;
    }
    SetInstalledPackageSha1(pack);
    Gen3ResourcePack_Destroy(pack);
    pack = NULL;

#if defined(NATIVE_LINUX)
    if (hydrate)
    {
        /* R13-D1 boot registration (moved verbatim from the old content.pak
         * verify path): register the production pack snapshot, publish every
         * seam (the R13-B leaf / R13-C text / R13-D1 gameplay publication is
         * drill inside RegisterRuntimeSnapshot), then seed the neighborhood.
         * A refused registration is a REFUSED install, propagated so
         * --verify-game-data exits 2 and the startup surface falls into the
         * frontend data-setup/exit path. */
        if (EmeraldResourceCompat_RegisterRuntimeSnapshot(packPath)
                != EMERALD_COMPAT_OK)
        {
            SetError("Emerald runtime session refused: production pack could not "
                     "be registered (no gameplay/data session)");
            return FALSE;
        }
        EmeraldResourceCompat_TryInitialize();
        /* R11-E/F: one-time init of the neighborhood (map tables, tilesets,
         * layouts are all canonical by now; the first EnsureCurrent call
         * after a map identity is set builds it). Allocation-free, idempotent. */
        NativeWorldNeighborhood_Init();
    }
#endif

    sLastError[0] = '\0';
    return TRUE;
}

enum PlatformGameContentImportResult Platform_GameContentImport(
    const char *romPath, struct PlatformGameContentImportInfo *info)
{
    struct PlatformGameContentImportInfo localInfo;
    char packPath[CONTENT_PATH_MAX];
    struct EmeraldImportSource manifests[EMERALD_IMPORT_MANIFEST_COUNT];
    struct EmeraldImportSource catalogs[EMERALD_IMPORT_CATALOG_COUNT];
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    enum EmeraldResourceImportError importError;
    u32 i;

    if (info == NULL)
        info = &localInfo;
    memset(info, 0, sizeof(*info));
    info->result = PLATFORM_GAME_CONTENT_IMPORT_UNREADABLE;

    if (romPath == NULL || romPath[0] == '\0'
     || !HashFile(romPath, info->detectedSha1))
    {
        snprintf(info->error, sizeof(info->error), "The selected ROM could not be read");
        SetError("%s", info->error);
        return info->result;
    }
    if (!BuildInstallPaths(packPath) || !EnsureInstallDirectories(packPath))
    {
        info->result = PLATFORM_GAME_CONTENT_IMPORT_INSTALL_FAILED;
        snprintf(info->error, sizeof(info->error),
                 "The game-data directory could not be created");
        SetError("%s", info->error);
        return info->result;
    }

    memset(manifests, 0, sizeof(manifests));
    for (i = 0u; i < EMERALD_IMPORT_MANIFEST_COUNT; i++)
        manifests[i].path = sImportManifests[i];
    memset(catalogs, 0, sizeof(catalogs));
    for (i = 0u; i < EMERALD_IMPORT_CATALOG_COUNT; i++)
        catalogs[i].path = sImportCatalogs[i];

    memset(&input, 0, sizeof(input));
    input.profile = EmeraldRomProfile_Bpee01Rev0();
    input.romPath = romPath;
    input.manifests = manifests;
    input.manifestCount = EMERALD_IMPORT_MANIFEST_COUNT;
    input.catalogs = catalogs;
    input.catalogCount = EMERALD_IMPORT_CATALOG_COUNT;
    input.destinationPath = packPath;

    /* Classify a wrong ROM first (the sdl2 CLI prints the detected vs
     * expected SHA-1 on UNSUPPORTED_ROM). */
    importError = EmeraldImport_ValidateRom(&input, &report);
    if (importError != EMERALD_IMPORT_OK)
    {
        info->result = PLATFORM_GAME_CONTENT_IMPORT_UNSUPPORTED_ROM;
        snprintf(info->error, sizeof(info->error), "Unsupported Pokemon Emerald ROM");
        SetError("%s", info->error);
        return info->result;
    }

    info->result = PLATFORM_GAME_CONTENT_IMPORT_INSTALL_FAILED;
    importError = EmeraldImport_Install(&input, &report);
    if (importError != EMERALD_IMPORT_OK)
    {
        snprintf(info->error, sizeof(info->error), "%s",
                 report.message[0] != '\0' ? report.message
                                           : "The production pack could not be installed");
        SetError("%s", info->error);
        return info->result;
    }

    /* Populate the package SHA-1 accessor from the freshly installed pack. */
    Platform_GameContentVerifyInstalled(FALSE);

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

#endif /* PLATFORM_SDL2 */