/* Stage R3 standalone importer tests.
 *
 * Exercises the full local import pipeline (emerald_resource_import.c) against
 * the deterministic synthetic bpee01 fixture: strict ROM validation, manifest
 * qualification + profile cross-checks, strict extraction/decoding, the
 * deterministic R2 .rpack build, atomic install under an import lock, temp-file
 * policy, installed-pack classification, and the failure matrix.
 *
 * The runner (run_emerald_resource_import.sh) invokes this from the repository
 * root so the repo source artifacts and the shipped manifest/catalog TOML
 * resolve against cwd. The fixture ROM is built in-process from the checked-in
 * Brendan source artifacts (no retail ROM, no ELF). The retail profile is used
 * ONLY to prove fail-closed rejection of fixture content; the retail ROM is
 * never read, written, or referenced by path.
 *
 * The fixture ROM path is never persisted anywhere; TestPrivacy proves the ROM
 * path is absent from reports and from the installed pack bytes (R3 §17).
 */

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "emerald/resources/emerald_rom_profile.h"
#include "emerald/resources/emerald_resource_import.h"
#include "fixture.h"
#include "gen3/resources/lz77.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_pack_writer.h"
#include "gen3/resources/sha1.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/util.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int gFailures = 0;
static int gChecks = 0;

static void Report(const char *test, bool ok, const char *detail)
{
    gChecks++;
    if (ok)
    {
        printf("  PASS %s\n", test);
    }
    else
    {
        gFailures++;
        printf("  FAIL %s%s%s\n", test, detail != NULL ? ": " : "",
               detail != NULL ? detail : "");
    }
}

#define CHECK(test, condition) \
    Report(test, (condition) ? true : false, NULL)

/* ---- resource identity constants (from the checked-in manifest) --------- */

#define kSheetName   "emerald:trainer/brendan/battle/front/sheet"
#define kPaletteName "emerald:trainer/brendan/battle/front/normal-palette"

#define SHEET_SOURCE_SHA_HEX \
    "c5fca4e037ab22c2c24bce1f0cf0c2b35679c3a1cf64b6e64dedbd26b3cebfde"
#define PALETTE_SOURCE_SHA_HEX \
    "7cb7654625eabaa37dbcc96f116ca3ac64b6a4424ec6f8bf1117e9ddcaabd18f"
#define SHEET_CANON_HEX \
    "17ce6ad427c987ebc70e85d28be731aa4db1600abc2c3122c6995a100a6ae265"
#define PALETTE_CANON_HEX \
    "ce6bb540c4209989adbe48b5801c566824453aa3162ffb47d6ca4f514fee3cba"

/* TOML lines used as replacement needles (uniquely present in the manifest). */
#define PALETTE_KEY_LINE \
    "key = \"6d7aec37ceb0faad6153f5c11380143c4fd6c033aa69599996e9ad0add670d56\""
#define PALETTE_KEY_LINE_FLIPPED \
    "key = \"6e7aec37ceb0faad6153f5c11380143c4fd6c033aa69599996e9ad0add670d56\""
#define PALETTE_SOURCE_LINE \
    "source_encoded_sha256 = \"7cb7654625eabaa37dbcc96f116ca3ac64b6a4424ec6f8bf1117e9ddcaabd18f\""
#define PALETTE_SOURCE_LINE_FLIPPED \
    "source_encoded_sha256 = \"8cb7654625eabaa37dbcc96f116ca3ac64b6a4424ec6f8bf1117e9ddcaabd18f\""
#define PALETTE_CANON_LINE \
    "canonical_decoded_sha256 = \"ce6bb540c4209989adbe48b5801c566824453aa3162ffb47d6ca4f514fee3cba\""
#define PALETTE_CANON_LINE_FLIPPED \
    "canonical_decoded_sha256 = \"de6bb540c4209989adbe48b5801c566824453aa3162ffb47d6ca4f514fee3cba\""
#define PALETTE_ID_LINE \
    "id = \"emerald:trainer/brendan/battle/front/normal-palette\""
#define PALETTE_ID_LINE_OTHER \
    "id = \"emerald:trainer/brendan/battle/front/nonexistent\""

/* ---- small helpers ------------------------------------------------------ */

static bool ReadFile(const char *path, struct Gen3Buffer *out)
{
    char errbuf[512];
    if (!Gen3Util_ReadFile(path, out, errbuf, sizeof(errbuf)))
    {
        fprintf(stderr, "  cannot load '%s': %s\n", path, errbuf);
        return false;
    }
    return true;
}

static unsigned char HexVal(char c)
{
    if (c >= '0' && c <= '9')
        return (unsigned char)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned char)(c - 'a' + 10u);
    if (c >= 'A' && c <= 'F')
        return (unsigned char)(c - 'A' + 10u);
    return 0u;
}

static void FromHexInto(uint8_t *out, const char *hex)
{
    size_t i;
    for (i = 0; hex[i] != '\0'; i += 2u)
        out[i / 2u] = (uint8_t)((HexVal(hex[i]) << 4u) | HexVal(hex[i + 1u]));
}

static bool MemCmpHex(const uint8_t *bytes, const char *hex)
{
    uint8_t decoded[64];
    size_t n = strlen(hex) / 2u;
    if (n > sizeof(decoded))
        return false;
    FromHexInto(decoded, hex);
    return memcmp(bytes, decoded, n) == 0;
}

static void Sha256BytesOf(const uint8_t *data, size_t size, uint8_t digest[32])
{
    struct Gen3Sha256Context context;
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, data, size);
    Gen3Sha256_Final(&context, digest);
}

static bool AllZero(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] != 0u)
            return false;
    return true;
}

static void PathJoin(const char *dir, const char *name, char *out)
{
    /* All call sites pass a char[4096] output buffer. */
    snprintf(out, 4096, "%s/%s", dir, name);
}

static bool MkdirIfMissing(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    if (errno == ENOENT)
        return mkdir(path, 0755) == 0 || errno == EEXIST;
    return false;
}

static bool WriteBytesTo(const char *path, const uint8_t *data, size_t size)
{
    FILE *file;
    size_t n;
    file = fopen(path, "wb");
    if (file == NULL)
        return false;
    n = fwrite(data, 1u, size, file);
    fclose(file);
    return n == size;
}

static bool ReadWholeFileInto(const char *path, uint8_t **outData, size_t *outSize)
{
    FILE *file;
    long length;
    uint8_t *data;
    size_t got;
    file = fopen(path, "rb");
    if (file == NULL)
        return false;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0
     || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }
    data = (uint8_t *)malloc((size_t)length);
    if (data == NULL)
    {
        fclose(file);
        return false;
    }
    got = fread(data, 1u, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length)
    {
        free(data);
        return false;
    }
    *outData = data;
    *outSize = (size_t)length;
    return true;
}

/* Builds a copy of the manifest with the first occurrence of `needle` replaced
 * by `replacement` (arbitrary length). Returns a NUL-terminated buffer whose
 * .length excludes the terminator, like Gen3Util_ReadFile. */
static struct Gen3Buffer ManifestReplace(const struct Gen3Buffer *src,
                                         const char *needle,
                                         const char *replacement)
{
    struct Gen3Buffer out;
    const char *hay = src->data;
    const char *pos = strstr(hay, needle);
    size_t needleLen = strlen(needle);
    size_t replLen = strlen(replacement);
    Gen3Buffer_Init(&out, src->length + replLen + 1u);
    if (pos != NULL)
    {
        size_t prefixLen = (size_t)(pos - hay);
        Gen3Buffer_Append(&out, hay, prefixLen);
        Gen3Buffer_Append(&out, replacement, replLen);
        Gen3Buffer_Append(&out, pos + needleLen, src->length - prefixLen - needleLen);
    }
    else
    {
        Gen3Buffer_Append(&out, hay, src->length);
    }
    Gen3Buffer_Append(&out, "\0", 1u);
    return out;
}

/* R9 Stage 4: splits a generated TOML file into two valid files at the second
 * `blockHead` block ("[[records]]" / "[[resources]]"): out1 = header + first
 * block, out2 = header + the remaining blocks. Both outputs are NUL-terminated
 * Gen3Buffers. Returns false if the file does not contain two blocks. */
static bool SplitToml(const struct Gen3Buffer *src, const char *blockHead,
                      struct Gen3Buffer *out1, struct Gen3Buffer *out2)
{
    char needle[64];
    const char *s = src->data;
    size_t len = src->length;
    size_t headLen;
    size_t first;
    size_t second;
    size_t i;
    snprintf(needle, sizeof(needle), "\n%s", blockHead);
    headLen = strlen(needle);
    first = SIZE_MAX;
    for (i = 0; i + headLen <= len; i++)
    {
        if (memcmp(s + i, needle, headLen) == 0)
        {
            first = i;
            break;
        }
    }
    if (first == SIZE_MAX)
        return false;
    second = SIZE_MAX;
    for (i = first + 1u; i + headLen <= len; i++)
    {
        if (memcmp(s + i, needle, headLen) == 0)
        {
            second = i;
            break;
        }
    }
    if (second == SIZE_MAX)
        return false;
    /* out1 = src[0..second): header + first block. */
    Gen3Buffer_Init(out1, second + 1u);
    Gen3Buffer_Append(out1, s, second);
    Gen3Buffer_Append(out1, "\0", 1u);
    /* out2 = header (src[0..first+1)) + src[second..end). */
    Gen3Buffer_Init(out2, first + 1u + (len - second) + 1u);
    Gen3Buffer_Append(out2, s, first + 1u);
    Gen3Buffer_Append(out2, s + second, len - second);
    Gen3Buffer_Append(out2, "\0", 1u);
    return true;
}

/* ---- assets ------------------------------------------------------------- */

struct TestAssets
{
    struct Gen3Buffer frontLz;   /* encoded 4bpp.lz (804) */
    struct Gen3Buffer frontRaw;  /* canonical decoded 4bpp (2048) */
    struct Gen3Buffer paletteLz; /* encoded gbapal.lz (40) */
    struct Gen3Buffer paletteRaw;/* canonical decoded gbapal (32) */
    struct Gen3Buffer rom;       /* 16 MiB fixture ROM */
    struct Gen3Buffer manifest;  /* manifest.generated.toml */
    struct Gen3Buffer catalog;   /* catalog.toml */
    const struct EmeraldRomProfile *profile;
    char scratch[4096];
};

static bool LoadAssets(struct TestAssets *a)
{
    memset(a, 0, sizeof(*a));
    a->profile = EmeraldRomProfile_SyntheticFixture();
    if (!ReadFile("graphics/trainers/front_pics/brendan.4bpp.lz", &a->frontLz)
     || !ReadFile("graphics/trainers/front_pics/brendan.4bpp", &a->frontRaw)
     || !ReadFile("graphics/trainers/palettes/brendan.gbapal.lz", &a->paletteLz)
     || !ReadFile("graphics/trainers/palettes/brendan.gbapal", &a->paletteRaw)
     || !ReadFile("resources/extraction/emerald/bpee01/manifest.generated.toml",
                  &a->manifest)
     || !ReadFile("resources/catalogs/emerald/catalog.toml", &a->catalog))
        return false;

    CHECK("fixture front decodes to 2048", a->frontRaw.length == 2048u);
    CHECK("fixture palette decodes to 32", a->paletteRaw.length == 32u);

    /* Strict decode of the encoded artifacts must equal the canonical raws. */
    {
        uint8_t decoded[8192];
        size_t decodedSize;
        enum Gen3Lz77Result r = Gen3Lz77_Decode(
            (const uint8_t *)a->frontLz.data, a->frontLz.length,
            decoded, sizeof(decoded), &decodedSize);
        CHECK("strict decode of front == canonical raw",
              r == GEN3_LZ77_OK && decodedSize == 2048u
              && memcmp(decoded, a->frontRaw.data, 2048u) == 0);
        r = Gen3Lz77_Decode(
            (const uint8_t *)a->paletteLz.data, a->paletteLz.length,
            decoded, sizeof(decoded), &decodedSize);
        CHECK("strict decode of palette == canonical raw",
              r == GEN3_LZ77_OK && decodedSize == 32u
              && memcmp(decoded, a->paletteRaw.data, 32u) == 0);
    }

    FixtureBuildRom((const uint8_t *)a->frontLz.data, a->frontLz.length,
                    (const uint8_t *)a->paletteLz.data, a->paletteLz.length,
                    &a->rom);
    if (a->rom.length != EMERALD_ROM_SIZE)
        return false;

    /* The built fixture ROM must match the locked synthetic profile and the
     * checked-in manifest's whole-ROM digests. */
    {
        uint8_t sha1Digest[20];
        uint8_t sha256Digest[32];
        struct Gen3Sha1Context sha1Context;
        struct Gen3Sha256Context sha256Context;
        Gen3Sha1_Init(&sha1Context);
        Gen3Sha1_Update(&sha1Context, (const uint8_t *)a->rom.data, a->rom.length);
        Gen3Sha1_Final(&sha1Context, sha1Digest);
        Gen3Sha256_Init(&sha256Context);
        Gen3Sha256_Update(&sha256Context, (const uint8_t *)a->rom.data, a->rom.length);
        Gen3Sha256_Final(&sha256Context, sha256Digest);
        CHECK("fixture ROM SHA-1 == synthetic profile",
              memcmp(sha1Digest, a->profile->romSha1, 20u) == 0);
        CHECK("fixture ROM SHA-256 == synthetic profile",
              memcmp(sha256Digest, a->profile->romSha256, 32u) == 0);
    }

    /* Fixture/artifact/manifest cross-consistency: the embedded slices and the
     * canonical decoded payloads hash to the manifest's declared digests. */
    {
        uint8_t digest[32];
        Sha256BytesOf((const uint8_t *)a->rom.data + FIXTURE_FRONT_ROM_OFFSET,
                      a->frontLz.length, digest);
        CHECK("fixture sheet slice hash == manifest",
              MemCmpHex(digest, SHEET_SOURCE_SHA_HEX));
        Sha256BytesOf((const uint8_t *)a->rom.data + FIXTURE_PALETTE_ROM_OFFSET,
                      a->paletteLz.length, digest);
        CHECK("fixture palette slice hash == manifest",
              MemCmpHex(digest, PALETTE_SOURCE_SHA_HEX));
        Sha256BytesOf((const uint8_t *)a->frontRaw.data, a->frontRaw.length, digest);
        CHECK("canonical sheet decode hash == manifest",
              MemCmpHex(digest, SHEET_CANON_HEX));
        Sha256BytesOf((const uint8_t *)a->paletteRaw.data, a->paletteRaw.length, digest);
        CHECK("canonical palette decode hash == manifest",
              MemCmpHex(digest, PALETTE_CANON_HEX));
    }
    return true;
}

static void FreeAssets(struct TestAssets *a)
{
    Gen3Buffer_Destroy(&a->frontLz);
    Gen3Buffer_Destroy(&a->frontRaw);
    Gen3Buffer_Destroy(&a->paletteLz);
    Gen3Buffer_Destroy(&a->paletteRaw);
    Gen3Buffer_Destroy(&a->rom);
    Gen3Buffer_Destroy(&a->manifest);
    Gen3Buffer_Destroy(&a->catalog);
}

/* Fills the importer input with the in-memory fixture ROM / manifest / catalog. */
static void FillInput(struct EmeraldImportInput *input, const struct TestAssets *a)
{
    memset(input, 0, sizeof(*input));
    input->profile = a->profile;
    input->romBytes = (const uint8_t *)a->rom.data;
    input->romSize = a->rom.length;
    input->manifestBytes = (const uint8_t *)a->manifest.data;
    input->manifestSize = a->manifest.length;
    input->catalogBytes = (const uint8_t *)a->catalog.data;
    input->catalogSize = a->catalog.length;
}

/* Hand-built two-entry pack at an arbitrary base pack version. With the same
 * inputs as the importer this is byte-identical to EmeraldImport_BuildPack at
 * basePackVersion == 1 (checked in TestBuildPackHappy), which makes the
 * version-999 pack a genuine "written by a newer engine" artifact. */
static void BuildPackFromArtifacts(const struct TestAssets *a, uint32_t baseVersion,
                                   struct Gen3ResourcePackBytes *out)
{
    uint8_t sheetSha[32];
    uint8_t paletteSha[32];
    uint8_t sheetSrc[32];
    uint8_t paletteSrc[32];
    uint8_t catalogSha[32];
    uint8_t manifestSha[32];
    Gen3ResourceKey sheetKey;
    Gen3ResourceKey paletteKey;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePackEntryInput entry;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackBuild *build;

    memset(out, 0, sizeof(*out));
    Sha256BytesOf((const uint8_t *)a->frontRaw.data, a->frontRaw.length, sheetSha);
    Sha256BytesOf((const uint8_t *)a->paletteRaw.data, a->paletteRaw.length, paletteSha);
    Sha256BytesOf((const uint8_t *)a->catalog.data, a->catalog.length, catalogSha);
    Sha256BytesOf((const uint8_t *)a->manifest.data, a->manifest.length, manifestSha);
    FromHexInto(sheetSrc, SHEET_SOURCE_SHA_HEX);
    FromHexInto(paletteSrc, PALETTE_SOURCE_SHA_HEX);
    Gen3ResourceId_DeriveKey(kSheetName, &sheetKey);
    Gen3ResourceId_DeriveKey(kPaletteName, &paletteKey);

    Gen3ResourcePackDiagnostics_Init(&diag);
    memset(&profile, 0, sizeof(profile));
    profile.basePackVersion = baseVersion;
    profile.catalogVersion = 1u;
    profile.extractionManifestVersion = 1u;
    profile.canonicalRepresentationVersion = 1u;
    profile.sourceRomSize = a->profile->romSize;
    profile.sourceRomSha1 = a->profile->romSha1;
    profile.sourceRomSha256 = a->profile->romSha256;
    memcpy(profile.gameCode, a->profile->gameCode, 4u);
    memcpy(profile.makerCode, a->profile->makerCode, 2u);
    profile.softwareRevision = a->profile->softwareRevision;
    memset(profile.gameId, 0, sizeof(profile.gameId));
    memcpy(profile.gameId, a->profile->game, strlen(a->profile->game) + 1u);
    profile.catalogSha256 = catalogSha;
    profile.extractionManifestSha256 = manifestSha;

    build = Gen3ResourcePackBuild_Create();
    CHECK("reference build object created", build != NULL);
    if (build == NULL)
    {
        Gen3ResourcePackDiagnostics_Destroy(&diag);
        return;
    }
    CHECK("reference build accepts profile",
          Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) == GEN3_PACK_OK);

    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = kPaletteName;
    entry.key = &paletteKey;
    entry.type = GEN3_RESOURCE_TYPE_PALETTE;
    entry.schema = 1u;
    entry.flags = GEN3_PACK_FLAG_REQUIRED_FOR_BASE;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
    entry.canonicalPayload = (const uint8_t *)a->paletteRaw.data;
    entry.canonicalPayloadSize = a->paletteRaw.length;
    entry.canonicalPayloadSha256 = paletteSha;
    entry.sourceRomOffset = FIXTURE_PALETTE_ROM_OFFSET;
    entry.sourceEncodedSize = a->paletteLz.length;
    entry.sourceEncodedSha256 = paletteSrc;
    CHECK("reference build accepts palette entry",
          Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_OK);

    entry.canonicalName = kSheetName;
    entry.key = &sheetKey;
    entry.type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entry.canonicalPayload = (const uint8_t *)a->frontRaw.data;
    entry.canonicalPayloadSize = a->frontRaw.length;
    entry.canonicalPayloadSha256 = sheetSha;
    entry.sourceRomOffset = FIXTURE_FRONT_ROM_OFFSET;
    entry.sourceEncodedSize = a->frontLz.length;
    entry.sourceEncodedSha256 = sheetSrc;
    CHECK("reference build accepts sheet entry",
          Gen3ResourcePackBuild_AddEntry(build, &entry, &diag) == GEN3_PACK_OK);

    CHECK("reference build writes pack",
          Gen3ResourcePackWriter_Write(build, out, &diag) == GEN3_PACK_OK);

    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
}

/* Lock helper for the contention test: holds the import lock like another
 * importer would. */
static int HoldLock(const char *dir)
{
    char path[4096];
    int fd;
    snprintf(path, sizeof(path), "%s/%s", dir, ".emerald-import.lock");
    fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static void ReleaseLock(int fd)
{
    if (fd >= 0)
    {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

/* ---- Test 1: strict ROM validation -------------------------------------- */

static void TestValidateRom(struct TestAssets *a)
{
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    const struct EmeraldRomProfile *production = EmeraldRomProfile_Bpee01Rev0();

    memset(&input, 0, sizeof(input));
    memset(&report, 0, sizeof(report));
    input.profile = a->profile;
    input.romBytes = (const uint8_t *)a->rom.data;
    input.romSize = a->rom.length;
    CHECK("ValidateRom happy path", EmeraldImport_ValidateRom(&input, &report) == EMERALD_IMPORT_OK);

    input.romSize = 4096u;
    memset(&report, 0, sizeof(report));
    CHECK("ValidateRom size mismatch",
          EmeraldImport_ValidateRom(&input, &report) == EMERALD_IMPORT_ERR_ROM_SIZE_MISMATCH);
    input.romSize = a->rom.length;

    {
        uint8_t *copy = (uint8_t *)malloc(a->rom.length);
        memcpy(copy, a->rom.data, a->rom.length);
        copy[0xAC] = 'X';
        input.romBytes = copy;
        memset(&report, 0, sizeof(report));
        CHECK("ValidateRom header identity mismatch",
              EmeraldImport_ValidateRom(&input, &report) == EMERALD_IMPORT_ERR_ROM_HEADER_IDENTITY);
        free(copy);
    }

    {
        uint8_t *copy = (uint8_t *)malloc(a->rom.length);
        memcpy(copy, a->rom.data, a->rom.length);
        copy[FIXTURE_FRONT_ROM_OFFSET] ^= 1u;
        input.romBytes = copy;
        memset(&report, 0, sizeof(report));
        CHECK("ValidateRom SHA-1 mismatch",
              EmeraldImport_ValidateRom(&input, &report) == EMERALD_IMPORT_ERR_ROM_SHA1_MISMATCH);
        free(copy);
    }
    input.romBytes = (const uint8_t *)a->rom.data;

    memset(&report, 0, sizeof(report));
    CHECK("ValidateRom NULL input",
          EmeraldImport_ValidateRom(NULL, &report) == EMERALD_IMPORT_ERR_INVALID_ARGUMENT);

    {
        struct EmeraldImportInput none;
        memset(&none, 0, sizeof(none));
        none.profile = a->profile;
        memset(&report, 0, sizeof(report));
        CHECK("ValidateRom no bytes or path supplied",
              EmeraldImport_ValidateRom(&none, &report) == EMERALD_IMPORT_ERR_INVALID_ARGUMENT);
    }

    {
        struct EmeraldImportInput dirPath;
        memset(&dirPath, 0, sizeof(dirPath));
        dirPath.profile = a->profile;
        dirPath.romPath = a->scratch; /* a directory, not a regular file */
        memset(&report, 0, sizeof(report));
        CHECK("ValidateRom rejects non-regular file",
              EmeraldImport_ValidateRom(&dirPath, &report) == EMERALD_IMPORT_ERR_ROM_NOT_REGULAR_FILE);
    }

    {
        struct EmeraldImportInput prod;
        memset(&prod, 0, sizeof(prod));
        prod.profile = production;
        prod.romBytes = (const uint8_t *)a->rom.data;
        prod.romSize = a->rom.length;
        memset(&report, 0, sizeof(report));
        CHECK("retail profile rejects fixture content (fail closed)",
              EmeraldImport_ValidateRom(&prod, &report) == EMERALD_IMPORT_ERR_ROM_SHA1_MISMATCH);
    }
}

/* ---- Test 2: BuildPack happy path + determinism + entry verification ----- */

static void TestBuildPackHappy(struct TestAssets *a)
{
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    struct Gen3ResourcePackBytes pack1;
    struct Gen3ResourcePackBytes pack2;
    struct Gen3ResourcePack *parsed = NULL;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackProfile profile;
    const struct Gen3ResourcePackEntry *entry;
    uint8_t digest[GEN3_PACK_SHA256_SIZE];

    FillInput(&input, a);
    memset(&report, 0, sizeof(report));
    memset(&pack1, 0, sizeof(pack1));
    CHECK("BuildPack happy path", EmeraldImport_BuildPack(&input, &pack1, &report) == EMERALD_IMPORT_OK);
    CHECK("BuildPack produces bytes", pack1.data != NULL && pack1.size > 0u);

    /* Determinism: a second identical build is byte-identical. */
    memset(&pack2, 0, sizeof(pack2));
    memset(&report, 0, sizeof(report));
    CHECK("BuildPack determinism", EmeraldImport_BuildPack(&input, &pack2, &report) == EMERALD_IMPORT_OK);
    CHECK("BuildPack determinism byte-identical",
          pack2.size == pack1.size && memcmp(pack2.data, pack1.data, pack1.size) == 0);

    /* The hand-built reference at base version 1 matches the importer exactly. */
    {
        struct Gen3ResourcePackBytes hand;
        BuildPackFromArtifacts(a, 1u, &hand);
        CHECK("importer pack == hand-built reference",
              hand.size == pack1.size && memcmp(hand.data, pack1.data, pack1.size) == 0);
        Gen3ResourcePackBytes_Destroy(&hand);
    }

    /* Round-trip: parse and verify every entry against the canonical payloads. */
    Gen3ResourcePackDiagnostics_Init(&diag);
    CHECK("freshly built pack parses",
          Gen3ResourcePack_Parse(pack1.data, pack1.size, &parsed, &diag) == GEN3_PACK_OK);
    CHECK("pack has exactly two entries", Gen3ResourcePack_GetEntryCount(parsed) == 2u);

    entry = Gen3ResourcePack_FindByCanonicalName(parsed, kSheetName);
    CHECK("sheet entry present", entry != NULL);
    if (entry != NULL)
    {
        CHECK("sheet type tile-graphics", entry->type == GEN3_RESOURCE_TYPE_TILE_GRAPHICS);
        CHECK("sheet schema 1", entry->schema == 1u);
        CHECK("sheet decoded 2048", entry->payloadSize == 2048u);
        CHECK("sheet payload == strict decode",
              memcmp(entry->payload, a->frontRaw.data, 2048u) == 0);
        CHECK("sheet representation decoded",
              entry->representation == GEN3_PACK_REPRESENTATION_DECODED);
        CHECK("sheet source encoding gba-lz77",
              entry->sourceEncoding == GEN3_PACK_SOURCE_ENCODING_GBA_LZ77);
        CHECK("sheet required-for-base flag",
              (entry->flags & GEN3_PACK_FLAG_REQUIRED_FOR_BASE) != 0u);
        CHECK("sheet source offset 0x300000", entry->sourceRomOffset == FIXTURE_FRONT_ROM_OFFSET);
        CHECK("sheet source encoded size 804", entry->sourceEncodedSize == a->frontLz.length);
    }

    entry = Gen3ResourcePack_FindByCanonicalName(parsed, kPaletteName);
    CHECK("palette entry present", entry != NULL);
    if (entry != NULL)
    {
        CHECK("palette type palette", entry->type == GEN3_RESOURCE_TYPE_PALETTE);
        CHECK("palette schema 1", entry->schema == 1u);
        CHECK("palette decoded 32", entry->payloadSize == 32u);
        CHECK("palette payload == strict decode",
              memcmp(entry->payload, a->paletteRaw.data, 32u) == 0);
        CHECK("palette source offset 0x310000", entry->sourceRomOffset == FIXTURE_PALETTE_ROM_OFFSET);
        CHECK("palette source encoded size 40", entry->sourceEncodedSize == a->paletteLz.length);
    }

    CHECK("pack profile present", Gen3ResourcePack_GetProfile(parsed, &profile));
    CHECK("profile base pack version 1", profile.basePackVersion == 1u);
    CHECK("profile catalog version 1", profile.catalogVersion == 1u);
    CHECK("profile manifest version 1", profile.extractionManifestVersion == 1u);
    CHECK("profile canonical rep version 1", profile.canonicalRepresentationVersion == 1u);
    CHECK("profile rom size 16 MiB", profile.sourceRomSize == EMERALD_ROM_SIZE);
    CHECK("profile rom sha1 == synthetic",
          memcmp(profile.sourceRomSha1, a->profile->romSha1, GEN3_PACK_SHA1_SIZE) == 0);
    CHECK("profile rom sha256 == synthetic",
          memcmp(profile.sourceRomSha256, a->profile->romSha256, GEN3_PACK_SHA256_SIZE) == 0);
    CHECK("profile game code BPEE", memcmp(profile.gameCode, "BPEE", 4u) == 0);
    CHECK("profile maker code 01", memcmp(profile.makerCode, "01", 2u) == 0);
    CHECK("profile software revision 0", profile.softwareRevision == 0u);
    CHECK("profile game id emerald", memcmp(profile.gameId, "emerald", 7u) == 0);

    CHECK("logical digest present", Gen3ResourcePack_GetLogicalDigest(parsed, digest));
    CHECK("logical digest nonzero", !AllZero(digest, GEN3_PACK_SHA256_SIZE));

    Gen3ResourcePack_Destroy(parsed);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    Gen3ResourcePackBytes_Destroy(&pack1);
    Gen3ResourcePackBytes_Destroy(&pack2);
}

/* ---- Test 2b: raw source encoding (R8 trainer-back sheets) -------------- */

/* The committed fixture manifest carries only gba-lz77 records; the R8 raw
 * path (the ROM artifact IS the canonical decoded payload; no LZ77 header,
 * no decode) is exercised by appending a raw record that points at an
 * existing fixture ROM slice — the 804-byte front LZ blob treated as an
 * opaque raw payload. The importer validates sizes, digests, and encoding
 * but never interprets content, so the bytes need not be tile data. */
static void TestBuildPackRaw(struct TestAssets *a)
{
    static const char kBackSheetName[] = "emerald:trainer/brendan/battle/back/sheet";
    static const char kBackSheetKey[] =
        "31780bb05e12b1e65b54dc696e5ee66be1338ef95da9e26f81a2c9a8c9e7ab24"; /* M0 */
    struct Gen3Buffer extra;
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    struct Gen3ResourcePackBytes pack;
    struct Gen3ResourcePack *parsed = NULL;
    struct Gen3ResourcePackDiagnosticList diag;
    const struct Gen3ResourcePackEntry *entry;
    uint8_t sliceSha[32];
    char shaHex[65];
    unsigned k;

    Sha256BytesOf((const uint8_t *)a->rom.data + FIXTURE_FRONT_ROM_OFFSET,
                  a->frontLz.length, sliceSha);
    for (k = 0u; k < 32u; k++)
        snprintf(shaHex + 2u * k, 3u, "%02x", sliceSha[k]);

    Gen3Buffer_Init(&extra, a->manifest.length + 512u);
    Gen3Buffer_Append(&extra, a->manifest.data, a->manifest.length);
    Gen3Buffer_AppendFormat(&extra,
        "\n[[records]]\n"
        "id = \"%s\"\n"
        "key = \"%s\"\n"
        "type = \"tile-graphics\"\n"
        "schema = 1\n"
        "symbol = \"gTrainerBackPic_Brendan\"\n"
        "rom_offset = %u\n"
        "encoded_length = %zu\n"
        "decoded_length = %zu\n"
        "source_encoding = \"raw\"\n"
        "source_encoded_sha256 = \"%s\"\n"
        "canonical_decoded_sha256 = \"%s\"\n",
        kBackSheetName, kBackSheetKey, (unsigned)FIXTURE_FRONT_ROM_OFFSET,
        a->frontLz.length, a->frontLz.length, shaHex, shaHex);
    Gen3Buffer_Append(&extra, "\0", 1u);

    memset(&input, 0, sizeof(input));
    input.profile = a->profile;
    input.romBytes = (const uint8_t *)a->rom.data;
    input.romSize = a->rom.length;
    input.manifestBytes = (const uint8_t *)extra.data;
    input.manifestSize = extra.length;
    input.catalogBytes = (const uint8_t *)a->catalog.data;
    input.catalogSize = a->catalog.length;
    memset(&report, 0, sizeof(report));
    memset(&pack, 0, sizeof(pack));
    CHECK("raw-record build happy path",
          EmeraldImport_BuildPack(&input, &pack, &report) == EMERALD_IMPORT_OK);
    Gen3Buffer_Destroy(&extra);

    Gen3ResourcePackDiagnostics_Init(&diag);
    CHECK("raw-record pack parses",
          Gen3ResourcePack_Parse(pack.data, pack.size, &parsed, &diag) == GEN3_PACK_OK);
    CHECK("raw-record pack has three entries",
          parsed != NULL && Gen3ResourcePack_GetEntryCount(parsed) == 3u);
    entry = parsed != NULL ? Gen3ResourcePack_FindByCanonicalName(parsed, kBackSheetName) : NULL;
    CHECK("raw entry present", entry != NULL);
    if (entry != NULL)
    {
        CHECK("raw entry type tile-graphics", entry->type == GEN3_RESOURCE_TYPE_TILE_GRAPHICS);
        CHECK("raw entry schema 1", entry->schema == 1u);
        CHECK("raw entry source encoding raw",
              entry->sourceEncoding == GEN3_PACK_SOURCE_ENCODING_RAW);
        CHECK("raw entry representation decoded",
              entry->representation == GEN3_PACK_REPRESENTATION_DECODED);
        CHECK("raw entry payload is the artifact bytes",
              entry->payloadSize == a->frontLz.length
              && memcmp(entry->payload, a->frontLz.data, a->frontLz.length) == 0);
        CHECK("raw entry encoded == decoded size",
              entry->sourceEncodedSize == entry->payloadSize);
        CHECK("raw entry source offset 0x300000",
              entry->sourceRomOffset == FIXTURE_FRONT_ROM_OFFSET);
        CHECK("raw entry required-for-base flag",
              (entry->flags & GEN3_PACK_FLAG_REQUIRED_FOR_BASE) != 0u);
    }

    Gen3ResourcePack_Destroy(parsed);
    Gen3ResourcePackDiagnostics_Destroy(&diag);
    Gen3ResourcePackBytes_Destroy(&pack);
}

/* ---- Test 3: path-based input seams ------------------------------------- */

static void TestPathInput(struct TestAssets *a)
{
    char dir[4096];
    char romPath[4096];
    char manifestPath[4096];
    char catalogPath[4096];
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    struct Gen3ResourcePackBytes bytes;

    PathJoin(a->scratch, "paths", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "rom.gba", romPath);
    PathJoin(dir, "manifest.toml", manifestPath);
    PathJoin(dir, "catalog.toml", catalogPath);
    if (!WriteBytesTo(romPath, (const uint8_t *)a->rom.data, a->rom.length)
     || !WriteBytesTo(manifestPath, (const uint8_t *)a->manifest.data, a->manifest.length)
     || !WriteBytesTo(catalogPath, (const uint8_t *)a->catalog.data, a->catalog.length))
    {
        CHECK("path-based input writes", false);
        return;
    }

    memset(&input, 0, sizeof(input));
    memset(&report, 0, sizeof(report));
    memset(&bytes, 0, sizeof(bytes));
    input.profile = a->profile;
    input.romPath = romPath;
    input.manifestPath = manifestPath;
    input.catalogPath = catalogPath;
    CHECK("BuildPack from paths succeeds", EmeraldImport_BuildPack(&input, &bytes, &report) == EMERALD_IMPORT_OK);
    CHECK("path-based pack produces bytes", bytes.data != NULL && bytes.size > 0u);
    Gen3ResourcePackBytes_Destroy(&bytes);
}

/* ---- Test 4: BuildPack failure matrix ----------------------------------- */

static void ExpectBuildFailure(struct TestAssets *a,
                               const struct Gen3Buffer *manifestOverride,
                               const uint8_t *romOverride,
                               enum EmeraldResourceImportError expected,
                               const char *label)
{
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    struct Gen3ResourcePackBytes bytes;

    FillInput(&input, a);
    if (manifestOverride != NULL)
    {
        input.manifestBytes = (const uint8_t *)manifestOverride->data;
        input.manifestSize = manifestOverride->length;
    }
    if (romOverride != NULL)
    {
        input.romBytes = romOverride;
        input.romSize = a->rom.length;
    }
    memset(&report, 0, sizeof(report));
    memset(&bytes, 0, sizeof(bytes));
    Report(label, EmeraldImport_BuildPack(&input, &bytes, &report) == expected, NULL);
    Gen3ResourcePackBytes_Destroy(&bytes);
}

/* Same, but overrides the catalog slot (reached only after the ROM validates). */
static void ExpectCatalogBuildFailure(struct TestAssets *a,
                                      const struct Gen3Buffer *catalogOverride,
                                      enum EmeraldResourceImportError expected,
                                      const char *label)
{
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    struct Gen3ResourcePackBytes bytes;

    FillInput(&input, a);
    if (catalogOverride != NULL)
    {
        input.catalogBytes = (const uint8_t *)catalogOverride->data;
        input.catalogSize = catalogOverride->length;
    }
    memset(&report, 0, sizeof(report));
    memset(&bytes, 0, sizeof(bytes));
    Report(label, EmeraldImport_BuildPack(&input, &bytes, &report) == expected, NULL);
    Gen3ResourcePackBytes_Destroy(&bytes);
}

static void TestBuildPackFailureMatrix(struct TestAssets *a)
{
    struct Gen3Buffer tampered;

    /* Fixture-qualified manifest rejected for the production profile, before
     * any ROM is touched (R3 §19 fail closed). */
    {
        struct EmeraldImportInput prod;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes bytes;
        memset(&prod, 0, sizeof(prod));
        memset(&report, 0, sizeof(report));
        memset(&bytes, 0, sizeof(bytes));
        prod.profile = EmeraldRomProfile_Bpee01Rev0();
        prod.manifestBytes = (const uint8_t *)a->manifest.data;
        prod.manifestSize = a->manifest.length;
        Report("production profile + fixture manifest -> FIXTURE_NOT_ALLOWED",
               EmeraldImport_BuildPack(&prod, &bytes, &report) == EMERALD_IMPORT_ERR_MANIFEST_FIXTURE_NOT_ALLOWED,
               NULL);
        CHECK("fixture-not-allowed not entry-scoped",
              report.hasName == false && report.entryIndex == SIZE_MAX);
        Gen3ResourcePackBytes_Destroy(&bytes);
    }

    tampered = ManifestReplace(&a->manifest, "qualification = \"fixture\"", "qualification = \"bogus\"");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_BAD_QUALIFICATION,
                       "bogus qualification -> BAD_QUALIFICATION");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "qualification = \"fixture\"", "");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
                       "missing qualification -> MANIFEST_MISSING_FIELD");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "game = \"emerald\"", "game = \"ruby\"");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_PROFILE_MISMATCH,
                       "wrong game -> MANIFEST_PROFILE_MISMATCH");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "rom_size = 16777216", "rom_size = 16777215");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_ROM_MISMATCH,
                       "wrong rom_size -> MANIFEST_ROM_MISMATCH");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, PALETTE_CANON_LINE, PALETTE_CANON_LINE_FLIPPED);
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_CANONICAL_HASH_MISMATCH,
                       "tampered canonical digest -> CANONICAL_HASH_MISMATCH");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, PALETTE_SOURCE_LINE, PALETTE_SOURCE_LINE_FLIPPED);
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_ENCODED_HASH_MISMATCH,
                       "tampered encoded digest -> ENCODED_HASH_MISMATCH");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, PALETTE_KEY_LINE, PALETTE_KEY_LINE_FLIPPED);
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_KEY_MISMATCH,
                       "tampered key -> KEY_MISMATCH");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "decoded_length = 32", "decoded_length = 4");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_LZ77_DECODE_FAILED,
                       "declared decode below stream size -> LZ77_DECODE_FAILED");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "decoded_length = 32", "decoded_length = 64");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_DECODED_SIZE_MISMATCH,
                       "declared decode above stream size -> DECODED_SIZE_MISMATCH");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "rom_offset = 3211264", "rom_offset = 16777216");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_ROM_RANGE_OUT_OF_BOUNDS,
                       "out-of-ROM range -> ROM_RANGE_OUT_OF_BOUNDS");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "type = \"palette\"", "type = \"tile-graphics\"");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_TYPE_SCHEMA_MISMATCH,
                       "type/schema disagreement -> TYPE_SCHEMA_MISMATCH");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, PALETTE_ID_LINE, PALETTE_ID_LINE_OTHER);
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_CATALOG_MISSING_RESOURCE,
                       "record absent from catalog -> CATALOG_MISSING_RESOURCE");
    Gen3Buffer_Destroy(&tampered);

    /* Duplicate palette record inserted before the sheet record. */
    {
        static const char kDuplicatePalette[] =
            "[[records]]\n"
            "id = \"emerald:trainer/brendan/battle/front/normal-palette\"\n"
            "key = \"6d7aec37ceb0faad6153f5c11380143c4fd6c033aa69599996e9ad0add670d56\"\n"
            "type = \"palette\"\n"
            "schema = 1\n"
            "symbol = \"gTrainerPalette_Brendan\"\n"
            "rom_offset = 3211264\n"
            "encoded_length = 40\n"
            "decoded_length = 32\n"
            "source_encoding = \"gba-lz77\"\n"
            "source_encoded_sha256 = \"7cb7654625eabaa37dbcc96f116ca3ac64b6a4424ec6f8bf1117e9ddcaabd18f\"\n"
            "canonical_decoded_sha256 = \"ce6bb540c4209989adbe48b5801c566824453aa3162ffb47d6ca4f514fee3cba\"\n"
            "\n";
        char needle[512];
        char replacement[1024];
        snprintf(needle, sizeof(needle), "[[records]]\nid = \"%s\"", kSheetName);
        snprintf(replacement, sizeof(replacement), "%s[[records]]\nid = \"%s\"",
                 kDuplicatePalette, kSheetName);
        tampered = ManifestReplace(&a->manifest, needle, replacement);
        ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_DUPLICATE_RECORD,
                           "duplicate record -> MANIFEST_DUPLICATE_RECORD");
        Gen3Buffer_Destroy(&tampered);
    }

    /* Unparseable document. */
    {
        struct Gen3Buffer garbage;
        Gen3Buffer_Init(&garbage, 64);
        Gen3Buffer_AppendCStr(&garbage, "this is not [[[ valid toml @@@");
        Gen3Buffer_Append(&garbage, "\0", 1);
        ExpectBuildFailure(a, &garbage, NULL, EMERALD_IMPORT_ERR_MANIFEST_PARSE_FAILED,
                           "garbage manifest -> MANIFEST_PARSE_FAILED");
        Gen3Buffer_Destroy(&garbage);
    }

    /* Whole-ROM validation on the build path: a tampered fixture ROM fails
     * closed before extraction. */
    {
        uint8_t *copy = (uint8_t *)malloc(a->rom.length);
        memcpy(copy, a->rom.data, a->rom.length);
        copy[FIXTURE_FRONT_ROM_OFFSET] ^= 1u;
        ExpectBuildFailure(a, NULL, copy, EMERALD_IMPORT_ERR_ROM_SHA1_MISMATCH,
                           "tampered fixture ROM -> ROM_SHA1_MISMATCH");
        free(copy);
    }

    /* Path-slot read failure for the manifest: a bogus manifest path is
     * rejected before any ROM work. */
    {
        char badManifestPath[4096];
        struct EmeraldImportInput input;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes bytes;
        PathJoin(a->scratch, "no-such-manifest.toml", badManifestPath);
        FillInput(&input, a);
        input.manifestBytes = NULL;
        input.manifestSize = 0u;
        input.manifestPath = badManifestPath;
        memset(&report, 0, sizeof(report));
        memset(&bytes, 0, sizeof(bytes));
        Report("bogus manifest path -> MANIFEST_READ_FAILED",
               EmeraldImport_BuildPack(&input, &bytes, &report) == EMERALD_IMPORT_ERR_MANIFEST_READ_FAILED,
               NULL);
        Gen3ResourcePackBytes_Destroy(&bytes);
    }

    /* Path-slot read failure for the catalog: manifest + ROM validate, then
     * the bogus catalog path fails closed. */
    {
        char badCatalogPath[4096];
        struct EmeraldImportInput input;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes bytes;
        PathJoin(a->scratch, "no-such-catalog.toml", badCatalogPath);
        FillInput(&input, a);
        input.catalogBytes = NULL;
        input.catalogSize = 0u;
        input.catalogPath = badCatalogPath;
        memset(&report, 0, sizeof(report));
        memset(&bytes, 0, sizeof(bytes));
        Report("bogus catalog path -> CATALOG_READ_FAILED",
               EmeraldImport_BuildPack(&input, &bytes, &report) == EMERALD_IMPORT_ERR_CATALOG_READ_FAILED,
               NULL);
        Gen3ResourcePackBytes_Destroy(&bytes);
    }

    /* A manifest that parses to zero records is rejected. */
    {
        static const char kNoRecordsManifest[] =
            "manifest_version = 1\n"
            "namespace = \"emerald\"\n"
            "resource_api = \"1.0.0\"\n"
            "game = \"emerald\"\n"
            "rom_profile = \"bpee01-rev0\"\n"
            "qualification = \"fixture\"\n"
            "rom_size = 16777216\n"
            "rom_sha1 = \"092ee293c6e4381074682375249561bbc41bec44\"\n"
            "rom_sha256 = \"e4c8f3693d840fb233dfa31c1cd2b4d21aa2dde67359e4aad4b064a4b1b33bca\"\n"
            "\n";
        struct Gen3Buffer buf;
        Gen3Buffer_Init(&buf, sizeof(kNoRecordsManifest));
        Gen3Buffer_AppendCStr(&buf, kNoRecordsManifest);
        ExpectBuildFailure(a, &buf, NULL, EMERALD_IMPORT_ERR_MANIFEST_NO_RECORDS,
                           "zero-record manifest -> MANIFEST_NO_RECORDS");
        Gen3Buffer_Destroy(&buf);
    }

    /* A manifest exceeding the record limit is rejected before record parsing.
     * R9 Stage 4 raised the cap to 2048 (1804-entry merged family + headroom),
     * so the rejection now starts at 2049 records. */
    {
        static const char kManifestHead[] =
            "manifest_version = 1\n"
            "game = \"emerald\"\n"
            "rom_profile = \"bpee01-rev0\"\n"
            "qualification = \"fixture\"\n"
            "rom_size = 16777216\n"
            "rom_sha1 = \"092ee293c6e4381074682375249561bbc41bec44\"\n"
            "rom_sha256 = \"e4c8f3693d840fb233dfa31c1cd2b4d21aa2dde67359e4aad4b064a4b1b33bca\"\n";
        struct Gen3Buffer buf;
        unsigned i;
        Gen3Buffer_Init(&buf, 8192);
        Gen3Buffer_AppendCStr(&buf, kManifestHead);
        for (i = 0; i < 2049u; i++)
            Gen3Buffer_AppendFormat(&buf, "[[records]]\nid = \"emerald:x/x%u\"\n", i);
        Gen3Buffer_Append(&buf, "\0", 1u);
        ExpectBuildFailure(a, &buf, NULL, EMERALD_IMPORT_ERR_MANIFEST_TOO_MANY_RECORDS,
                           "2049-record manifest -> MANIFEST_TOO_MANY_RECORDS");
        Gen3Buffer_Destroy(&buf);
    }

    /* ---- R9 Stage 4: multi-manifest / multi-catalog import --------------- */

    /* Splitting the committed fixture manifest + catalog into two files each
     * (duplicate headers, disjoint record sets) must produce a pack with the
     * same entries as the single-input build: same entry count, same logical
     * digest. (The pack profile provenance digests necessarily differ because
     * they cover both input files in input order.) */
    {
        struct Gen3Buffer m1;
        struct Gen3Buffer m2;
        struct Gen3Buffer c1;
        struct Gen3Buffer c2;
        struct EmeraldImportSource sources[4];
        struct EmeraldImportInput input;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes single;
        struct Gen3ResourcePackBytes split;
        struct Gen3ResourcePack *opened;
        struct Gen3ResourcePackDiagnosticList diag;
        uint8_t singleDigest[GEN3_PACK_SHA256_SIZE];
        uint8_t splitDigest[GEN3_PACK_SHA256_SIZE];

        Gen3Buffer_Init(&m1, 0u);
        Gen3Buffer_Init(&m2, 0u);
        Gen3Buffer_Init(&c1, 0u);
        Gen3Buffer_Init(&c2, 0u);
        memset(&single, 0, sizeof(single));
        memset(&split, 0, sizeof(split));
        memset(&report, 0, sizeof(report));

        if (!SplitToml(&a->manifest, "[[records]]", &m1, &m2)
         || !SplitToml(&a->catalog, "[[resources]]", &c1, &c2))
        {
            Report("R9 split fixture manifest+catalog into two files each",
                   false, "split failed");
        }
        else
        {
            FillInput(&input, a);
            Report("R9 single-input build succeeds",
                   EmeraldImport_BuildPack(&input, &single, &report) == EMERALD_IMPORT_OK,
                   NULL);

            sources[0].bytes = (const uint8_t *)m1.data;
            sources[0].size = m1.length;
            sources[0].path = NULL;
            sources[1].bytes = (const uint8_t *)m2.data;
            sources[1].size = m2.length;
            sources[1].path = NULL;
            sources[2].bytes = (const uint8_t *)c1.data;
            sources[2].size = c1.length;
            sources[2].path = NULL;
            sources[3].bytes = (const uint8_t *)c2.data;
            sources[3].size = c2.length;
            sources[3].path = NULL;
            FillInput(&input, a);
            input.manifests = &sources[0];
            input.manifestCount = 2u;
            input.catalogs = &sources[2];
            input.catalogCount = 2u;
            memset(&report, 0, sizeof(report));
            Report("R9 split-input build succeeds",
                   EmeraldImport_BuildPack(&input, &split, &report) == EMERALD_IMPORT_OK,
                   NULL);

            opened = NULL;
            Gen3ResourcePackDiagnostics_Init(&diag);
            Report("R9 split pack reopens",
                   Gen3ResourcePack_Parse(split.data, split.size, &opened, &diag) == GEN3_PACK_OK
                       && opened != NULL,
                   NULL);
            Gen3ResourcePack_Destroy(opened);
            Gen3ResourcePackDiagnostics_Destroy(&diag);

            /* The provider digest covers exactly the entry content (key, type,
             * schema, payload), so it must match across both builds — the
             * logical digest may not, because it also mixes in the profile
             * provenance digests, which legitimately differ (the split build
             * hashes two input files). */
            memset(singleDigest, 0, sizeof(singleDigest));
            memset(splitDigest, 0, sizeof(splitDigest));
            opened = NULL;
            Gen3ResourcePackDiagnostics_Init(&diag);
            Gen3ResourcePack_Parse(single.data, single.size, &opened, &diag);
            if (opened != NULL)
                Gen3ResourcePack_GetProviderDigest(opened, singleDigest);
            Gen3ResourcePack_Destroy(opened);
            opened = NULL;
            Gen3ResourcePack_Parse(split.data, split.size, &opened, &diag);
            if (opened != NULL)
                Gen3ResourcePack_GetProviderDigest(opened, splitDigest);
            Gen3ResourcePack_Destroy(opened);
            Gen3ResourcePackDiagnostics_Destroy(&diag);
            Report("R9 split-input pack has the same entries (provider digest)",
                   memcmp(singleDigest, splitDigest, sizeof(singleDigest)) == 0,
                   NULL);
        }
        Gen3ResourcePackBytes_Destroy(&single);
        Gen3ResourcePackBytes_Destroy(&split);
        Gen3Buffer_Destroy(&m1);
        Gen3Buffer_Destroy(&m2);
        Gen3Buffer_Destroy(&c1);
        Gen3Buffer_Destroy(&c2);
    }

    /* Cross-manifest duplicate ids are rejected at the merge (fail-closed:
     * the same record in two family manifests can never slip in). */
    {
        struct Gen3Buffer dup1;
        struct Gen3Buffer dup2;
        struct EmeraldImportSource sources[2];
        struct EmeraldImportInput input;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes bytes;
        Gen3Buffer_Init(&dup1, 0u);
        Gen3Buffer_Init(&dup2, 0u);
        if (!SplitToml(&a->manifest, "[[records]]", &dup1, &dup2))
        {
            Report("R9 dup-manifest split", false, "split failed");
        }
        else
        {
            /* dup1 == header + first record block; feed it as BOTH manifests. */
            sources[0].bytes = (const uint8_t *)dup1.data;
            sources[0].size = dup1.length;
            sources[0].path = NULL;
            sources[1] = sources[0];
            FillInput(&input, a);
            input.manifests = sources;
            input.manifestCount = 2u;
            memset(&report, 0, sizeof(report));
            memset(&bytes, 0, sizeof(bytes));
            Report("R9 duplicate id across manifests -> MANIFEST_DUPLICATE_RECORD",
                   EmeraldImport_BuildPack(&input, &bytes, &report)
                       == EMERALD_IMPORT_ERR_MANIFEST_DUPLICATE_RECORD,
                   NULL);
            Gen3ResourcePackBytes_Destroy(&bytes);
        }
        Gen3Buffer_Destroy(&dup1);
        Gen3Buffer_Destroy(&dup2);
    }

    /* Two manifests whose merged total exceeds the 2048-record cap are
     * rejected even when each file alone is under the cap. */
    {
        static const char kBigRecordFmt[] =
            "[[records]]\n"
            "id = \"%s\"\n"
            "key = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
            "type = \"palette\"\n"
            "schema = 1\n"
            "rom_offset = 0\n"
            "encoded_length = 32\n"
            "decoded_length = 32\n"
            "source_encoding = \"raw\"\n"
            "source_encoded_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
            "canonical_decoded_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n";
        static const char kManifestHead[] =
            "manifest_version = 1\n"
            "game = \"emerald\"\n"
            "rom_profile = \"bpee01-rev0\"\n"
            "qualification = \"fixture\"\n"
            "rom_size = 16777216\n"
            "rom_sha1 = \"092ee293c6e4381074682375249561bbc41bec44\"\n"
            "rom_sha256 = \"e4c8f3693d840fb233dfa31c1cd2b4d21aa2dde67359e4aad4b064a4b1b33bca\"\n";
        struct Gen3Buffer big1;
        struct Gen3Buffer big2;
        struct EmeraldImportSource sources[2];
        struct EmeraldImportInput input;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes bytes;
        unsigned i;
        Gen3Buffer_Init(&big1, 128 * 1024u);
        Gen3Buffer_Init(&big2, 128 * 1024u);
        Gen3Buffer_AppendCStr(&big1, kManifestHead);
        Gen3Buffer_AppendCStr(&big2, kManifestHead);
        for (i = 0; i < 1025u; i++)
        {
            char id[64];
            snprintf(id, sizeof(id), "emerald:x/big1%u", i);
            Gen3Buffer_AppendFormat(&big1, kBigRecordFmt, id);
            snprintf(id, sizeof(id), "emerald:x/big2%u", i);
            Gen3Buffer_AppendFormat(&big2, kBigRecordFmt, id);
        }
        Gen3Buffer_Append(&big1, "\0", 1u);
        Gen3Buffer_Append(&big2, "\0", 1u);
        sources[0].bytes = (const uint8_t *)big1.data;
        sources[0].size = big1.length;
        sources[0].path = NULL;
        sources[1].bytes = (const uint8_t *)big2.data;
        sources[1].size = big2.length;
        sources[1].path = NULL;
        FillInput(&input, a);
        input.manifests = sources;
        input.manifestCount = 2u;
        memset(&report, 0, sizeof(report));
        memset(&bytes, 0, sizeof(bytes));
        Report("R9 merged manifests over the 2048 cap -> MANIFEST_TOO_MANY_RECORDS",
               EmeraldImport_BuildPack(&input, &bytes, &report)
                   == EMERALD_IMPORT_ERR_MANIFEST_TOO_MANY_RECORDS,
               NULL);
        Gen3ResourcePackBytes_Destroy(&bytes);
        Gen3Buffer_Destroy(&big1);
        Gen3Buffer_Destroy(&big2);
    }

    /* Manifests disagreeing on manifest_version are rejected at the merge. */
    {
        struct Gen3Buffer v1;
        struct Gen3Buffer v2;
        struct Gen3Buffer tampered;
        struct EmeraldImportSource sources[2];
        struct EmeraldImportInput input;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes bytes;
        Gen3Buffer_Init(&v1, 0u);
        Gen3Buffer_Init(&v2, 0u);
        if (!SplitToml(&a->manifest, "[[records]]", &v1, &v2))
        {
            Report("R9 version-disagree split", false, "split failed");
        }
        else
        {
            tampered = ManifestReplace(&v2, "manifest_version = 1",
                                       "manifest_version = 2");
            sources[0].bytes = (const uint8_t *)v1.data;
            sources[0].size = v1.length;
            sources[0].path = NULL;
            sources[1].bytes = (const uint8_t *)tampered.data;
            sources[1].size = tampered.length;
            sources[1].path = NULL;
            FillInput(&input, a);
            input.manifests = sources;
            input.manifestCount = 2u;
            memset(&report, 0, sizeof(report));
            memset(&bytes, 0, sizeof(bytes));
            Report("R9 manifest_version disagreement -> MANIFESTS_DISAGREE",
                   EmeraldImport_BuildPack(&input, &bytes, &report)
                       == EMERALD_IMPORT_ERR_MANIFESTS_DISAGREE,
                   NULL);
            Gen3ResourcePackBytes_Destroy(&bytes);
            Gen3Buffer_Destroy(&tampered);
        }
        Gen3Buffer_Destroy(&v1);
        Gen3Buffer_Destroy(&v2);
    }

    /* Every manifest is qualification-checked independently: a production
     * manifest beside the fixture manifest is rejected for the synthetic
     * profile before any ROM work. */
    {
        struct Gen3Buffer p1;
        struct Gen3Buffer p2;
        struct Gen3Buffer tampered;
        struct EmeraldImportSource sources[2];
        struct EmeraldImportInput input;
        struct EmeraldImportReport report;
        struct Gen3ResourcePackBytes bytes;
        Gen3Buffer_Init(&p1, 0u);
        Gen3Buffer_Init(&p2, 0u);
        if (!SplitToml(&a->manifest, "[[records]]", &p1, &p2))
        {
            Report("R9 qualification-mix split", false, "split failed");
        }
        else
        {
            tampered = ManifestReplace(&p2, "qualification = \"fixture\"",
                                       "qualification = \"production\"");
            sources[0].bytes = (const uint8_t *)p1.data;
            sources[0].size = p1.length;
            sources[0].path = NULL;
            sources[1].bytes = (const uint8_t *)tampered.data;
            sources[1].size = tampered.length;
            sources[1].path = NULL;
            FillInput(&input, a);
            input.manifests = sources;
            input.manifestCount = 2u;
            memset(&report, 0, sizeof(report));
            memset(&bytes, 0, sizeof(bytes));
            Report("R9 production manifest beside fixture -> MANIFEST_BAD_QUALIFICATION",
                   EmeraldImport_BuildPack(&input, &bytes, &report)
                       == EMERALD_IMPORT_ERR_MANIFEST_BAD_QUALIFICATION,
                   NULL);
            Gen3ResourcePackBytes_Destroy(&bytes);
            Gen3Buffer_Destroy(&tampered);
        }
        Gen3Buffer_Destroy(&p1);
        Gen3Buffer_Destroy(&p2);
    }

    /* Per-record field policy rejections in the parse loop (palette = index 0). */
    tampered = ManifestReplace(&a->manifest, "schema = 1", "schema = 0");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_SCHEMA,
                       "schema = 0 -> MANIFEST_UNSUPPORTED_SCHEMA");
    Gen3Buffer_Destroy(&tampered);

    /* R8: raw is now a supported encoding (trainer-back sheets). A raw record
     * over an LZ77 artifact fails the raw size contract instead: the first
     * gba-lz77 record here is the palette (40 encoded != 32 decoded). */
    tampered = ManifestReplace(&a->manifest, "source_encoding = \"gba-lz77\"", "source_encoding = \"raw\"");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_DECODED_SIZE_MISMATCH,
                       "raw on an LZ77 record -> DECODED_SIZE_MISMATCH");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "source_encoding = \"gba-lz77\"", "source_encoding = \"bogus\"");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_ENCODING,
                       "unknown source_encoding -> MANIFEST_UNSUPPORTED_ENCODING");
    Gen3Buffer_Destroy(&tampered);

    tampered = ManifestReplace(&a->manifest, "type = \"palette\"", "type = \"unknown\"");
    ExpectBuildFailure(a, &tampered, NULL, EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_TYPE,
                       "type unknown -> MANIFEST_UNSUPPORTED_TYPE");
    Gen3Buffer_Destroy(&tampered);

    /* Catalog contract errors are reached only after the ROM validates. */
    {
        static const char kGarbageCatalog[] = "nonsense [[[ @@@ not toml";
        struct Gen3Buffer garbage;
        Gen3Buffer_Init(&garbage, sizeof(kGarbageCatalog));
        Gen3Buffer_AppendCStr(&garbage, kGarbageCatalog);
        ExpectCatalogBuildFailure(a, &garbage, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                                  "garbage catalog -> CATALOG_PARSE_FAILED");
        Gen3Buffer_Destroy(&garbage);
    }

    tampered = ManifestReplace(&a->catalog, "game = \"emerald\"", "game = \"ruby\"");
    ExpectCatalogBuildFailure(a, &tampered, EMERALD_IMPORT_ERR_CATALOG_PROFILE_MISMATCH,
                              "catalog game ruby -> CATALOG_PROFILE_MISMATCH");
    Gen3Buffer_Destroy(&tampered);
}

/* ---- Test 5: atomic install --------------------------------------------- */

static void TestInstall(struct TestAssets *a)
{
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    struct EmeraldInstalledInfo info;
    char dir[4096];
    char dest[4096];
    uint8_t *installed = NULL;
    size_t installedSize = 0u;

    FillInput(&input, a);

    /* (a) happy path install + byte-identical to a fresh BuildPack. */
    PathJoin(a->scratch, "inst1", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "emerald-bpee01-v1.rpack", dest);
    input.destinationPath = dest;
    memset(&report, 0, sizeof(report));
    CHECK("Install happy path", EmeraldImport_Install(&input, &report) == EMERALD_IMPORT_OK);

    {
        struct Gen3ResourcePackBytes pack;
        memset(&pack, 0, sizeof(pack));
        CHECK("fresh BuildPack for comparison",
              EmeraldImport_BuildPack(&input, &pack, &report) == EMERALD_IMPORT_OK);
        CHECK("installed file == fresh BuildPack bytes",
              ReadWholeFileInto(dest, &installed, &installedSize)
              && installedSize == pack.size
              && memcmp(installed, pack.data, pack.size) == 0);
        Gen3ResourcePackBytes_Destroy(&pack);
    }

    /* ValidateInstalled -> VALID (synthetic profile and NULL profile). */
    memset(&info, 0, sizeof(info));
    memset(&report, 0, sizeof(report));
    CHECK("ValidateInstalled returns OK",
          EmeraldImport_ValidateInstalled(dest, a->profile, &info, &report) == EMERALD_IMPORT_OK);
    CHECK("installed status VALID", info.status == EMERALD_INSTALLED_VALID);
    CHECK("installed base pack version 1", info.basePackVersion == 1u);
    CHECK("installed entry count 2", info.entryCount == 2u);
    CHECK("installed game id emerald", memcmp(info.gameId, "emerald", 7u) == 0);
    CHECK("installed rom sha1 == synthetic",
          memcmp(info.romSha1, a->profile->romSha1, GEN3_PACK_SHA1_SIZE) == 0);
    CHECK("installed logical digest present", info.hasLogicalDigest);

    memset(&info, 0, sizeof(info));
    CHECK("ValidateInstalled NULL profile skips profile check",
          EmeraldImport_ValidateInstalled(dest, NULL, &info, &report) == EMERALD_IMPORT_OK
          && info.status == EMERALD_INSTALLED_VALID);

    /* Idempotent reimport: the valid pack is replaced atomically and still
     * matches a fresh build afterwards. */
    memset(&report, 0, sizeof(report));
    CHECK("Install idempotent reimport", EmeraldImport_Install(&input, &report) == EMERALD_IMPORT_OK);
    {
        struct Gen3ResourcePackBytes pack;
        uint8_t *again;
        size_t againSize;
        memset(&pack, 0, sizeof(pack));
        CHECK("reimport fresh BuildPack for comparison",
              EmeraldImport_BuildPack(&input, &pack, &report) == EMERALD_IMPORT_OK);
        CHECK("reimported file == fresh BuildPack bytes",
              ReadWholeFileInto(dest, &again, &againSize)
              && againSize == pack.size && memcmp(again, pack.data, pack.size) == 0);
        free(again);
        Gen3ResourcePackBytes_Destroy(&pack);
    }

    /* Wrong-profile classification + Install refusal. */
    memset(&info, 0, sizeof(info));
    CHECK("installed synthetic pack vs retail profile -> WRONG_PROFILE",
          EmeraldImport_ValidateInstalled(dest, EmeraldRomProfile_Bpee01Rev0(),
                                          &info, &report) == EMERALD_IMPORT_OK
          && info.status == EMERALD_INSTALLED_WRONG_PROFILE);
    {
        struct EmeraldImportInput wrong = input;
        wrong.profile = EmeraldRomProfile_Bpee01Rev0();
        memset(&report, 0, sizeof(report));
        CHECK("Install refuses wrong-profile pack",
              EmeraldImport_Install(&wrong, &report) == EMERALD_IMPORT_ERR_INSTALLED_WRONG_PROFILE);
    }

    /* (b) NEWER refusal: a base-pack-version 999 pack is refused, never
     * replaced. */
    PathJoin(a->scratch, "inst_newer", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "emerald-bpee01-v1.rpack", dest);
    {
        struct Gen3ResourcePackBytes newer;
        BuildPackFromArtifacts(a, 999u, &newer);
        CHECK("newer-version pack writes", WriteBytesTo(dest, newer.data, newer.size));
        memset(&info, 0, sizeof(info));
        CHECK("installed newer-version pack classified NEWER",
              EmeraldImport_ValidateInstalled(dest, a->profile, &info, &report) == EMERALD_IMPORT_OK
              && info.status == EMERALD_INSTALLED_NEWER);
        input.destinationPath = dest;
        memset(&report, 0, sizeof(report));
        CHECK("Install refuses newer-version pack",
              EmeraldImport_Install(&input, &report) == EMERALD_IMPORT_ERR_INSTALLED_NEWER);
        Gen3ResourcePackBytes_Destroy(&newer);
    }

    /* (c) corrupt classification. */
    PathJoin(a->scratch, "inst_corrupt", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "emerald-bpee01-v1.rpack", dest);
    WriteBytesTo(dest, (const uint8_t *)"not a pack file", 15u);
    memset(&info, 0, sizeof(info));
    CHECK("corrupt installed pack classified CORRUPT",
          EmeraldImport_ValidateInstalled(dest, a->profile, &info, &report) == EMERALD_IMPORT_OK
          && info.status == EMERALD_INSTALLED_CORRUPT);

    /* (d) absent file -> OPEN_FAILED. */
    PathJoin(a->scratch, "inst_absent", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "missing.rpack", dest);
    memset(&info, 0, sizeof(info));
    CHECK("absent installed pack classified OPEN_FAILED",
          EmeraldImport_ValidateInstalled(dest, a->profile, &info, &report) == EMERALD_IMPORT_OK
          && info.status == EMERALD_INSTALLED_OPEN_FAILED);

    /* (e) lock contention -> LOCK_FAILED; a subsequent install succeeds once
     * the lock is released. */
    PathJoin(a->scratch, "inst_lock", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "emerald-bpee01-v1.rpack", dest);
    {
        int lockFd = HoldLock(dir);
        CHECK("test holds the import lock", lockFd >= 0);
        input.destinationPath = dest;
        memset(&report, 0, sizeof(report));
        CHECK("Install under contention -> LOCK_FAILED",
              EmeraldImport_Install(&input, &report) == EMERALD_IMPORT_ERR_LOCK_FAILED);
        ReleaseLock(lockFd);
        memset(&report, 0, sizeof(report));
        CHECK("Install after release succeeds",
              EmeraldImport_Install(&input, &report) == EMERALD_IMPORT_OK);
    }

    /* (f) leftover temp cleanup (R3 §14): an interrupted temp file is removed
     * on the next import while the destination itself is never deleted. */
    PathJoin(a->scratch, "inst_cleanup", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "emerald-bpee01-v1.rpack", dest);
    {
        char leftover[4096];
        struct stat st;
        PathJoin(dir, "emerald-bpee01-v1.rpack.tmp.ABCDEF", leftover);
        CHECK("leftover temp written", WriteBytesTo(leftover, (const uint8_t *)"junk", 4u));
        CHECK("leftover temp exists", stat(leftover, &st) == 0);
        input.destinationPath = dest;
        memset(&report, 0, sizeof(report));
        CHECK("Install cleans leftover temp",
              EmeraldImport_Install(&input, &report) == EMERALD_IMPORT_OK);
        CHECK("leftover temp removed", stat(leftover, &st) != 0);
        memset(&info, 0, sizeof(info));
        CHECK("installed pack after cleanup is VALID",
              EmeraldImport_ValidateInstalled(dest, a->profile, &info, &report) == EMERALD_IMPORT_OK
              && info.status == EMERALD_INSTALLED_VALID);
    }

    /* (g) deep destination directory is auto-created (mkdir -p). */
    PathJoin(a->scratch, "inst_deep", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "nested/a/b", dest);
    {
        char deepDir[4096];
        PathJoin(dir, "nested/a/b", deepDir);
        input.destinationPath = dest;
        memset(&report, 0, sizeof(report));
        CHECK("Install auto-creates deep directory",
              EmeraldImport_Install(&input, &report) == EMERALD_IMPORT_OK);
        memset(&info, 0, sizeof(info));
        CHECK("deep installed pack is VALID",
              EmeraldImport_ValidateInstalled(dest, a->profile, &info, &report) == EMERALD_IMPORT_OK
              && info.status == EMERALD_INSTALLED_VALID);
        (void)deepDir;
    }

    free(installed);
}

/* ---- Test 6: ROM path privacy (R3 §17) ---------------------------------- */

static void TestPrivacy(struct TestAssets *a)
{
    char dir[4096];
    char romPath[4096];
    char dest[4096];
    struct EmeraldImportInput input;
    struct EmeraldImportReport report;
    uint8_t *installed = NULL;
    size_t installedSize = 0u;
    size_t i;
    bool found;

    PathJoin(a->scratch, "priv", dir);
    MkdirIfMissing(dir);
    PathJoin(dir, "user-pokemon-emerald.gba", romPath);
    PathJoin(dir, "emerald-bpee01-v1.rpack", dest);
    CHECK("fixture ROM written under a user-like path",
          WriteBytesTo(romPath, (const uint8_t *)a->rom.data, a->rom.length));

    memset(&input, 0, sizeof(input));
    memset(&report, 0, sizeof(report));
    input.profile = a->profile;
    input.romPath = romPath;
    input.manifestBytes = (const uint8_t *)a->manifest.data;
    input.manifestSize = a->manifest.length;
    input.catalogBytes = (const uint8_t *)a->catalog.data;
    input.catalogSize = a->catalog.length;
    input.destinationPath = dest;
    CHECK("Install via romPath succeeds", EmeraldImport_Install(&input, &report) == EMERALD_IMPORT_OK);
    CHECK("success report echoes no ROM path", strstr(report.message, romPath) == NULL);

    /* The installed pack bytes must never contain the ROM path (deterministic,
     * no host metadata). */
    CHECK("installed pack readable", ReadWholeFileInto(dest, &installed, &installedSize));
    found = false;
    for (i = 0; installed != NULL && i + strlen(romPath) <= installedSize; i++)
    {
        if (memcmp(installed + i, romPath, strlen(romPath)) == 0)
        {
            found = true;
            break;
        }
    }
    CHECK("installed pack bytes contain no ROM path", found == false);
    free(installed);
    installed = NULL;

    /* A failed ROM open reports the failure with errno but never the path. */
    {
        struct Gen3ResourcePackBytes bytes;
        struct EmeraldImportInput bad = input;
        char missing[4096];
        PathJoin(dir, "no-such-rom.gba", missing);
        bad.romPath = missing;
        memset(&report, 0, sizeof(report));
        memset(&bytes, 0, sizeof(bytes));
        CHECK("missing ROM -> ROM_READ_FAILED",
              EmeraldImport_BuildPack(&bad, &bytes, &report) == EMERALD_IMPORT_ERR_ROM_READ_FAILED);
        CHECK("failed-open report echoes no ROM path", strstr(report.message, missing) == NULL);
        CHECK("failed-open report echoes no base name",
              strstr(report.message, "no-such-rom.gba") == NULL);
        CHECK("failed-open report is informative", report.message[0] != '\0');
        Gen3ResourcePackBytes_Destroy(&bytes);
    }
}

int main(int argc, char **argv)
{
    struct TestAssets assets;

    printf("== emerald resource import R3 standalone tests ==\n");
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }

    memset(&assets, 0, sizeof(assets));
    if (strlen(argv[1]) >= sizeof(assets.scratch))
    {
        fprintf(stderr, "scratch-dir too long\n");
        return 1;
    }
    if (!LoadAssets(&assets))
    {
        fprintf(stderr, "FAIL: cannot load repo artifacts / build fixture\n");
        FreeAssets(&assets);
        return 1;
    }

    /* LoadAssets zeroes the whole struct (including scratch), so the scratch
     * path is stamped AFTER loading and creating the scratch directory. */
    snprintf(assets.scratch, sizeof(assets.scratch), "%s", argv[1]);
    MkdirIfMissing(assets.scratch);

    TestValidateRom(&assets);
    TestBuildPackHappy(&assets);
    TestBuildPackRaw(&assets);
    TestPathInput(&assets);
    TestBuildPackFailureMatrix(&assets);
    TestInstall(&assets);
    TestPrivacy(&assets);

    FreeAssets(&assets);

    printf("== %d checks, %d failures ==\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
