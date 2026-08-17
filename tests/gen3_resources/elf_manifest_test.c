/* Stage R1A standalone test suite for tools/gen3_resources/elf_manifest.
 *
 * Runs the generator against a deterministic synthetic ELF/ROM fixture (the
 * same fixture that backs the checked-in manifest) plus the real Brendan
 * source artifacts checked into the repo. No user-owned retail ROM or ELF is
 * used anywhere.
 *
 * Coverage (per the R1A directive):
 *   - LZ77 success / truncated / invalid-backref / trailing-data
 *   - decoded-size mismatch (manifest level)
 *   - canonical key derivation through the existing resource_id code
 *   - deterministic output + catalog/binding order independence
 *   - missing ELF symbol / zero-sized symbol / non-ROM symbol
 *   - ROM-range overflow / artifact-ELF-ROM mismatch / canonical mismatch
 *   - duplicate canonical id / duplicate ROM range (and allowSharedRange)
 *   - output changes when authoritative metadata changes
 *   - end-to-end from the shipped catalog.toml + bindings.toml via FromToml
 *
 * The runner (run_elf_manifest.sh) invokes this from the repository root so
 * artifact paths and the catalog/bindings TOML resolve against cwd.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "gen3/resources/lz77.h"
#include "manifest.h"
#include "gen3/resources/sha1.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"

#include "gen3/resources/resource_id.h"

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

/* ---- helpers ------------------------------------------------------------ */

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

static void Sha1HexOf(const uint8_t *bytes, size_t size, char *outHex)
{
    struct Gen3Sha1Context ctx;
    uint8_t digest[GEN3_SHA1_DIGEST_SIZE];
    Gen3Sha1_Init(&ctx);
    Gen3Sha1_Update(&ctx, bytes, size);
    Gen3Sha1_Final(&ctx, digest);
    Gen3Util_FormatHex(digest, sizeof(digest), outHex);
}

struct TestAssets
{
    struct Gen3Buffer frontLz;    /* encoded 4bpp.lz */
    struct Gen3Buffer frontRaw;   /* canonical decoded 4bpp */
    struct Gen3Buffer paletteLz;  /* encoded gbapal.lz */
    struct Gen3Buffer paletteRaw; /* canonical decoded gbapal */
    struct Gen3Buffer elf;        /* fixture ELF */
    struct Gen3Buffer rom;        /* fixture ROM */
    char romSha1Hex[GEN3_SHA1_HEX_SIZE];
};

static bool LoadAssets(struct TestAssets *a)
{
    memset(a, 0, sizeof(*a));
    if (!ReadFile("graphics/trainers/front_pics/brendan.4bpp.lz", &a->frontLz)
     || !ReadFile("graphics/trainers/front_pics/brendan.4bpp", &a->frontRaw)
     || !ReadFile("graphics/trainers/palettes/brendan.gbapal.lz", &a->paletteLz)
     || !ReadFile("graphics/trainers/palettes/brendan.gbapal", &a->paletteRaw))
        return false;
    if (!FixtureBuildElf((const uint8_t *)a->frontLz.data, a->frontLz.length,
                         (const uint8_t *)a->paletteLz.data, a->paletteLz.length,
                         &a->elf))
        return false;
    FixtureBuildRom((const uint8_t *)a->frontLz.data, a->frontLz.length,
                    (const uint8_t *)a->paletteLz.data, a->paletteLz.length,
                    &a->rom);
    if (a->rom.length != GEN3_ROM_SIZE)
        return false;
    Sha1HexOf((const uint8_t *)a->rom.data, a->rom.length, a->romSha1Hex);
    return true;
}

static void FreeAssets(struct TestAssets *a)
{
    Gen3Buffer_Destroy(&a->frontLz);
    Gen3Buffer_Destroy(&a->frontRaw);
    Gen3Buffer_Destroy(&a->paletteLz);
    Gen3Buffer_Destroy(&a->paletteRaw);
    Gen3Buffer_Destroy(&a->elf);
    Gen3Buffer_Destroy(&a->rom);
}

static const struct Gen3ManifestMeta kMeta = {
    .namespace = "emerald",
    .resourceApi = "1.0.0",
    .game = "emerald",
    .romProfile = "bpee01-rev0",
};

static void FillBindings(struct TestAssets *a, struct Gen3BindingInput *out)
{
    /* Zero-init so the R11-C slice/concat fields are deterministic; the
     * slice path is driven by hasSymbolOffset presence. */
    memset(out, 0, sizeof(out[0]) * 2u);
    out[0].id = "emerald:trainer/brendan/battle/front/sheet";
    out[0].symbol = "gTrainerFrontPic_Brendan";
    out[0].sourceEncoding = "gba-lz77";
    out[0].canonicalRepresentation = "gba-4bpp-tiles";
    out[0].expectedDecodedSize = (uint32_t)a->frontRaw.length;
    out[0].allowSharedRange = false;
    out[0].sourceArtifact = (const uint8_t *)a->frontLz.data;
    out[0].sourceArtifactSize = a->frontLz.length;
    out[0].canonicalDecoded = (const uint8_t *)a->frontRaw.data;
    out[0].canonicalDecodedSize = a->frontRaw.length;

    out[1].id = "emerald:trainer/brendan/battle/front/normal-palette";
    out[1].symbol = "gTrainerPalette_Brendan";
    out[1].sourceEncoding = "gba-lz77";
    out[1].canonicalRepresentation = "gba-bgr555-palette";
    out[1].expectedDecodedSize = (uint32_t)a->paletteRaw.length;
    out[1].allowSharedRange = false;
    out[1].sourceArtifact = (const uint8_t *)a->paletteLz.data;
    out[1].sourceArtifactSize = a->paletteLz.length;
    out[1].canonicalDecoded = (const uint8_t *)a->paletteRaw.data;
    out[1].canonicalDecodedSize = a->paletteRaw.length;
}

static const struct Gen3CatalogEntry kCatalog[2] =
{
    { "emerald:trainer/brendan/battle/front/sheet", "tile-graphics", 1 },
    { "emerald:trainer/brendan/battle/front/normal-palette", "palette", 1 },
};

/* Writes 32-bit LE value at offset. */
static void PatchWordLE(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset + 0u] = (uint8_t)(value & 0xFFu);
    data[offset + 1u] = (uint8_t)((value >> 8) & 0xFFu);
    data[offset + 2u] = (uint8_t)((value >> 16) & 0xFFu);
    data[offset + 3u] = (uint8_t)((value >> 24) & 0xFFu);
}

static size_t FrontSymbolValueOffset(void)
{
    return (size_t)FixtureSymtabOffset(804u, 40u)
         + (size_t)FIXTURE_FRONT_SYMBOL * FIXTURE_SYMBOL_TABLE_ENTRY_SIZE + 4u;
}

static size_t FrontSymbolSizeOffset(void)
{
    return (size_t)FixtureSymtabOffset(804u, 40u)
         + (size_t)FIXTURE_FRONT_SYMBOL * FIXTURE_SYMBOL_TABLE_ENTRY_SIZE + 8u;
}

static uint32_t ReadWordLE(const uint8_t *bytes)
{
    return (uint32_t)bytes[0]
         | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16)
         | ((uint32_t)bytes[3] << 24);
}

/* Byte offset of section 1's sh_addr field (the front section). */
static size_t FrontSectionAddrOffset(const struct Gen3Buffer *elf)
{
    uint32_t shoff = ReadWordLE((const uint8_t *)elf->data + 32u); /* e_shoff */
    return (size_t)shoff + 1u * 40u + 12u; /* section header 1, sh_addr at +12 */
}

/* Runs the generator; returns the result code and (on success) fills outBuffer. */
static enum Gen3ManifestResult GenerateWith(
    const struct Gen3ManifestMeta *meta,
    const struct Gen3CatalogEntry *catalog, size_t catalogCount,
    const struct Gen3BindingInput *bindings, size_t bindingCount,
    const uint8_t *elfData, size_t elfSize,
    const uint8_t *romData, size_t romSize,
    const struct Gen3ManifestConfig *config,
    struct Gen3Buffer *outBuffer, char *errbuf, size_t errbufSize)
{
    struct Gen3ManifestConfig localConfig;
    if (config == NULL)
    {
        memset(&localConfig, 0, sizeof(localConfig));
        config = &localConfig;
    }
    /* Gen3Manifest_Generate treats outBuffer as fresh; release any scratch
     * content from a previous call so the same buffer can be reused. */
    Gen3Buffer_Destroy(outBuffer);
    return Gen3Manifest_Generate(meta, catalog, catalogCount,
                                 bindings, bindingCount,
                                 elfData, elfSize, romData, romSize,
                                 config, outBuffer, errbuf, errbufSize);
}

static enum Gen3ManifestResult GenerateDefault(
    const struct TestAssets *a,
    const struct Gen3BindingInput *bindings, size_t bindingCount,
    struct Gen3Buffer *outBuffer)
{
    struct Gen3ManifestConfig config;
    char errbuf[512];
    memset(&config, 0, sizeof(config));
    config.expectedRomSha1Hex = a->romSha1Hex;
    return GenerateWith(&kMeta, kCatalog, ARRAY_SIZE(kCatalog),
                        bindings, bindingCount,
                        (const uint8_t *)a->elf.data, a->elf.length,
                        (const uint8_t *)a->rom.data, a->rom.length,
                        &config, outBuffer, errbuf, sizeof(errbuf));
}

static bool Contains(const struct Gen3Buffer *haystack, const char *needle)
{
    return haystack->data != NULL && strstr(haystack->data, needle) != NULL;
}

/* ---- LZ77 decode -------------------------------------------------------- */

static void TestLz77(struct TestAssets *a)
{
    uint8_t decoded[8192];
    size_t decodedSize = 0;

    /* Success against the real artifacts. */
    {
        enum Gen3Lz77Result r = Gen3Lz77_Decode(
            (const uint8_t *)a->frontLz.data, a->frontLz.length,
            decoded, sizeof(decoded), &decodedSize);
        bool ok = (r == GEN3_LZ77_OK && decodedSize == 2048u
                && memcmp(decoded, a->frontRaw.data, 2048u) == 0);
        CHECK("lz77 front sheet decode -> 2048 == canonical", ok);
    }
    {
        enum Gen3Lz77Result r = Gen3Lz77_Decode(
            (const uint8_t *)a->paletteLz.data, a->paletteLz.length,
            decoded, sizeof(decoded), &decodedSize);
        bool ok = (r == GEN3_LZ77_OK && decodedSize == 32u
                && memcmp(decoded, a->paletteRaw.data, 32u) == 0);
        CHECK("lz77 palette decode -> 32 == canonical", ok);
    }

    /* Truncated stream: header + a couple of control bytes only. */
    {
        uint8_t truncated[8];
        size_t n = a->frontLz.length < sizeof(truncated)
                 ? a->frontLz.length : sizeof(truncated);
        memcpy(truncated, a->frontLz.data, n);
        n = 6; /* header + 2 bytes of stream */
        enum Gen3Lz77Result r = Gen3Lz77_Decode(
            truncated, n, decoded, sizeof(decoded), &decodedSize);
        CHECK("lz77 truncated stream rejected", r == GEN3_LZ77_TRUNCATED);
    }

    /* Invalid backreference: first block requests a distance of 2 at pos 0. */
    {
        uint8_t stream[7] = { 0x10, 0x04, 0x00, 0x00, 0x80, 0x30, 0x01 };
        enum Gen3Lz77Result r = Gen3Lz77_Decode(
            stream, sizeof(stream), decoded, sizeof(decoded), &decodedSize);
        CHECK("lz77 invalid backreference rejected",
              r == GEN3_LZ77_INVALID_BACKREF);
    }

    /* Trailing non-zero padding after the declared size. */
    {
        uint8_t stream[8] = { 0x10, 0x01, 0x00, 0x00, 0x00, 0xAA, 0xFF, 0xFF };
        enum Gen3Lz77Result r = Gen3Lz77_Decode(
            stream, sizeof(stream), decoded, sizeof(decoded), &decodedSize);
        CHECK("lz77 non-zero trailing padding rejected",
              r == GEN3_LZ77_TRAILING_DATA);
    }

    /* Overflow: declared size exceeds the decode buffer. */
    {
        uint8_t stream[4] = { 0x10, 0xFF, 0xFF, 0x1F };
        enum Gen3Lz77Result r = Gen3Lz77_Decode(
            stream, sizeof(stream), decoded, 128u, &decodedSize);
        CHECK("lz77 declared size exceeds capacity rejected",
              r == GEN3_LZ77_OVERFLOW);
    }
}

/* ---- key derivation ----------------------------------------------------- */

static void TestKeyDerivation(void)
{
    struct Gen3ResourceKey key;
    char hex[GEN3_RESOURCE_KEY_HEX_SIZE];

    Gen3ResourceId_DeriveKey("emerald:trainer/brendan/battle/front/sheet", &key);
    Gen3ResourceId_FormatKeyHex(&key, hex);
    CHECK("key derivation matches M0/M1 for front sheet",
          strcmp(hex, "95821423a175034d3b872a3d3587200ceecc20ec81e258a83ee0bc477849b211") == 0);

    Gen3ResourceId_DeriveKey("emerald:trainer/brendan/battle/front/normal-palette", &key);
    Gen3ResourceId_FormatKeyHex(&key, hex);
    CHECK("key derivation matches M0/M1 for normal palette",
          strcmp(hex, "6d7aec37ceb0faad6153f5c11380143c4fd6c033aa69599996e9ad0add670d56") == 0);
}

/* ---- happy path + determinism + order independence ---------------------- */

static void TestHappyPath(struct TestAssets *a)
{
    struct Gen3BindingInput bindings[2];
    struct Gen3Buffer out1;
    struct Gen3Buffer out2;
    struct Gen3BindingInput reversed[2];
    struct Gen3CatalogEntry reversedCatalog[2];
    enum Gen3ManifestResult r;
    char errbuf[512];
    bool ok;

    FillBindings(a, bindings);
    Gen3Buffer_Init(&out1, 0);
    Gen3Buffer_Init(&out2, 0);
    r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &out1);
    CHECK("generate happy path succeeds", r == GEN3_MANIFEST_OK);
    if (r == GEN3_MANIFEST_OK)
    {
        CHECK("output has sheet key hex", Contains(&out1, "95821423a175034d3b872a3d3587200ceecc20ec81e258a83ee0bc477849b211"));
        CHECK("output has palette key hex", Contains(&out1, "6d7aec37ceb0faad6153f5c11380143c4fd6c033aa69599996e9ad0add670d56"));
        CHECK("output has sheet rom_offset 3145728", Contains(&out1, "rom_offset = 3145728"));
        CHECK("output has palette rom_offset 3211264", Contains(&out1, "rom_offset = 3211264"));
        CHECK("output records bytewise sorted (palette before sheet)",
              strstr(out1.data, "normal-palette") != NULL
              && strstr(out1.data, "normal-palette") < strstr(out1.data, "/sheet"));

        /* Determinism: a second identical generation is byte-identical. */
        r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &out2);
        ok = (r == GEN3_MANIFEST_OK
           && out1.length == out2.length
           && memcmp(out1.data, out2.data, out1.length) == 0);
        CHECK("deterministic regeneration is byte-identical", ok);
    }
    else
    {
        Gen3Util_FormatHex((const uint8_t *)&r, 0, errbuf);
        printf("  detail: %s\n", errbuf);
    }

    /* Order independence: reversed catalog + reversed bindings, same output. */
    reversed[0] = bindings[1];
    reversed[1] = bindings[0];
    reversedCatalog[0] = kCatalog[1];
    reversedCatalog[1] = kCatalog[0];
    {
        struct Gen3ManifestConfig config;
        struct Gen3Buffer out3;
        char errbuf2[512];
        memset(&config, 0, sizeof(config));
        config.expectedRomSha1Hex = a->romSha1Hex;
        Gen3Buffer_Init(&out3, 0);
        r = GenerateWith(&kMeta, reversedCatalog, ARRAY_SIZE(reversedCatalog),
                         reversed, ARRAY_SIZE(reversed),
                         (const uint8_t *)a->elf.data, a->elf.length,
                         (const uint8_t *)a->rom.data, a->rom.length,
                         &config, &out3, errbuf2, sizeof(errbuf2));
        ok = (r == GEN3_MANIFEST_OK
           && out1.length == out3.length
           && memcmp(out1.data, out3.data, out1.length) == 0);
        CHECK("catalog/binding order independence", ok);
        Gen3Buffer_Destroy(&out3);
    }

    Gen3Buffer_Destroy(&out1);
    Gen3Buffer_Destroy(&out2);
}

/* ---- error paths -------------------------------------------------------- */

static void TestErrorPaths(struct TestAssets *a)
{
    struct Gen3BindingInput bindings[2];
    struct Gen3Buffer out;
    struct Gen3ManifestConfig config;
    char errbuf[512];
    enum Gen3ManifestResult r;
    struct Gen3Buffer elfCopy;
    struct Gen3Buffer romCopy;

    FillBindings(a, bindings);
    Gen3Buffer_Init(&out, 0);
    memset(&config, 0, sizeof(config));
    config.expectedRomSha1Hex = a->romSha1Hex;

    /* Missing ELF symbol. */
    bindings[0].symbol = "gTrainerFrontPic_DoesNotExist";
    r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &out);
    CHECK("missing ELF symbol -> MISSING_SYMBOL",
          r == GEN3_MANIFEST_MISSING_SYMBOL);
    bindings[0].symbol = "gTrainerFrontPic_Brendan";

    /* Zero-sized symbol. */
    Gen3Buffer_Init(&elfCopy, 0);
    Gen3Buffer_Append(&elfCopy, a->elf.data, a->elf.length);
    PatchWordLE((uint8_t *)elfCopy.data, FrontSymbolSizeOffset(), 0u);
    r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &out);
    (void)r;
    r = GenerateWith(&kMeta, kCatalog, ARRAY_SIZE(kCatalog),
                     bindings, ARRAY_SIZE(bindings),
                     (const uint8_t *)elfCopy.data, elfCopy.length,
                     (const uint8_t *)a->rom.data, a->rom.length,
                     &config, &out, errbuf, sizeof(errbuf));
    CHECK("zero-sized symbol -> SYMBOL_NOT_ROM",
          r == GEN3_MANIFEST_SYMBOL_NOT_ROM);
    Gen3Buffer_Destroy(&elfCopy);

    /* Non-ROM address (below 0x08000000). */
    Gen3Buffer_Init(&elfCopy, 0);
    Gen3Buffer_Append(&elfCopy, a->elf.data, a->elf.length);
    PatchWordLE((uint8_t *)elfCopy.data, FrontSymbolValueOffset(), 0x06000000u);
    r = GenerateWith(&kMeta, kCatalog, ARRAY_SIZE(kCatalog),
                     bindings, ARRAY_SIZE(bindings),
                     (const uint8_t *)elfCopy.data, elfCopy.length,
                     (const uint8_t *)a->rom.data, a->rom.length,
                     &config, &out, errbuf, sizeof(errbuf));
    CHECK("non-ROM symbol address -> SYMBOL_NOT_ROM",
          r == GEN3_MANIFEST_SYMBOL_NOT_ROM);
    Gen3Buffer_Destroy(&elfCopy);

    /* ROM-range overflow: st_value inside its section but the derived ROM
     * range [0x000FFF00, 0x00100224) runs past the 16 MiB image. */
    Gen3Buffer_Init(&elfCopy, 0);
    Gen3Buffer_Append(&elfCopy, a->elf.data, a->elf.length);
    PatchWordLE((uint8_t *)elfCopy.data, FrontSectionAddrOffset(&elfCopy),
                0x08FFFF00u);
    PatchWordLE((uint8_t *)elfCopy.data, FrontSymbolValueOffset(), 0x08FFFF00u);
    r = GenerateWith(&kMeta, kCatalog, ARRAY_SIZE(kCatalog),
                     bindings, ARRAY_SIZE(bindings),
                     (const uint8_t *)elfCopy.data, elfCopy.length,
                     (const uint8_t *)a->rom.data, a->rom.length,
                     &config, &out, errbuf, sizeof(errbuf));
    CHECK("ROM-range overflow -> ROM_RANGE_OVERFLOW",
          r == GEN3_MANIFEST_ROM_RANGE_OVERFLOW);
    Gen3Buffer_Destroy(&elfCopy);

    /* Artifact/ROM mismatch: flip one byte in the ROM front slice. */
    Gen3Buffer_Init(&romCopy, 0);
    Gen3Buffer_Append(&romCopy, a->rom.data, a->rom.length);
    romCopy.data[FIXTURE_FRONT_ROM_OFFSET + 10u] ^= 0xFF;
    {
        char romSha1[GEN3_SHA1_HEX_SIZE];
        Sha1HexOf((const uint8_t *)romCopy.data, romCopy.length, romSha1);
        config.expectedRomSha1Hex = romSha1;
        r = GenerateWith(&kMeta, kCatalog, ARRAY_SIZE(kCatalog),
                         bindings, ARRAY_SIZE(bindings),
                         (const uint8_t *)a->elf.data, a->elf.length,
                         (const uint8_t *)romCopy.data, romCopy.length,
                         &config, &out, errbuf, sizeof(errbuf));
        CHECK("artifact/ELF/ROM mismatch -> ARTIFACT_MISMATCH",
              r == GEN3_MANIFEST_ARTIFACT_MISMATCH);
        config.expectedRomSha1Hex = a->romSha1Hex;
    }
    Gen3Buffer_Destroy(&romCopy);

    /* Decoded-size mismatch: declare the wrong expected decoded size. */
    bindings[0].expectedDecodedSize = 999u;
    r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &out);
    CHECK("decoded-size mismatch -> DECODED_SIZE_MISMATCH",
          r == GEN3_MANIFEST_DECODED_SIZE_MISMATCH);
    bindings[0].expectedDecodedSize = (uint32_t)a->frontRaw.length;

    /* Canonical mismatch: corrupt the canonical decoded bytes. */
    {
        uint8_t corrupt[2048];
        memcpy(corrupt, a->frontRaw.data, a->frontRaw.length);
        corrupt[100u] ^= 0xFF;
        bindings[0].canonicalDecoded = corrupt;
        bindings[0].canonicalDecodedSize = sizeof(corrupt);
        r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &out);
        CHECK("canonical-decoded mismatch -> CANONICAL_MISMATCH",
              r == GEN3_MANIFEST_CANONICAL_MISMATCH);
    }
    FillBindings(a, bindings);

    /* Duplicate canonical id. */
    {
        struct Gen3BindingInput dup[3];
        dup[0] = bindings[0];
        dup[1] = bindings[1];
        dup[2] = bindings[0];
        r = GenerateDefault(a, dup, ARRAY_SIZE(dup), &out);
        CHECK("duplicate canonical id -> DUPLICATE_ID",
              r == GEN3_MANIFEST_DUPLICATE_ID);
    }

    /* Unknown resource: binding id not in the catalog. */
    {
        struct Gen3BindingInput unknown[1];
        unknown[0] = bindings[0];
        unknown[0].id = "emerald:some/other/resource";
        r = GenerateDefault(a, unknown, ARRAY_SIZE(unknown), &out);
        CHECK("binding id absent from catalog -> UNKNOWN_RESOURCE",
              r == GEN3_MANIFEST_UNKNOWN_RESOURCE);
    }

    /* Type mismatch: palette representation on a tile-graphics resource. */
    {
        struct Gen3BindingInput mismatch[1];
        mismatch[0] = bindings[0];
        mismatch[0].canonicalRepresentation = "gba-bgr555-palette";
        r = GenerateDefault(a, mismatch, ARRAY_SIZE(mismatch), &out);
        CHECK("incompatible canonical_representation -> TYPE_MISMATCH",
              r == GEN3_MANIFEST_TYPE_MISMATCH);
    }

    /* Duplicate ROM range rejected by default, allowed when shared. Two
     * DISTINCT canonical ids bound to the same GBA symbol so the overlap check
     * (not the duplicate-id check) is what fires. */
    {
        static const struct Gen3CatalogEntry sharedCatalog[2] =
        {
            { "emerald:test/shared/a", "tile-graphics", 1 },
            { "emerald:test/shared/b", "tile-graphics", 1 },
        };
        struct Gen3BindingInput shared[2];
        shared[0] = bindings[0];
        shared[0].id = "emerald:test/shared/a";
        shared[1] = bindings[0];
        shared[1].id = "emerald:test/shared/b";
        r = GenerateWith(&kMeta, sharedCatalog, ARRAY_SIZE(sharedCatalog),
                         shared, ARRAY_SIZE(shared),
                         (const uint8_t *)a->elf.data, a->elf.length,
                         (const uint8_t *)a->rom.data, a->rom.length,
                         &config, &out, errbuf, sizeof(errbuf));
        CHECK("overlapping ROM ranges rejected -> DUPLICATE_RANGE",
              r == GEN3_MANIFEST_DUPLICATE_RANGE);
        shared[0].allowSharedRange = true;
        shared[1].allowSharedRange = true;
        r = GenerateWith(&kMeta, sharedCatalog, ARRAY_SIZE(sharedCatalog),
                         shared, ARRAY_SIZE(shared),
                         (const uint8_t *)a->elf.data, a->elf.length,
                         (const uint8_t *)a->rom.data, a->rom.length,
                         &config, &out, errbuf, sizeof(errbuf));
        CHECK("shared ranges allowed -> OK", r == GEN3_MANIFEST_OK);
    }

    Gen3Buffer_Destroy(&out);
}

/* ---- output changes when authoritative metadata changes ------------------ */

static void TestMetadataSensitivity(struct TestAssets *a)
{
    struct Gen3BindingInput bindings[2];
    struct Gen3Buffer baseline;
    struct Gen3Buffer mutated;
    enum Gen3ManifestResult r;

    FillBindings(a, bindings);
    Gen3Buffer_Init(&baseline, 0);
    Gen3Buffer_Init(&mutated, 0);
    r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &baseline);
    CHECK("baseline generation succeeds", r == GEN3_MANIFEST_OK);

    /* Unknown encodings fail closed (R8 §18: only "gba-lz77" and "raw"
     * exist; previously any unrecognized string slipped through). */
    bindings[0].sourceEncoding = "some-other-encoding";
    r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &mutated);
    CHECK("unsupported encoding fails closed -> UNSUPPORTED_ENCODING",
          r == GEN3_MANIFEST_UNSUPPORTED_ENCODING);
    FillBindings(a, bindings);

    /* The R8 raw path is strict: repointing an LZ77 binding at raw fails the
     * encoded == decoded size agreement (the lz stream is smaller than the
     * decoded payload), proving raw bindings cannot smuggle lz77 artifacts. */
    bindings[0].sourceEncoding = "raw";
    r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &mutated);
    CHECK("raw encoding on an LZ77 artifact -> DECODED_SIZE_MISMATCH",
          r == GEN3_MANIFEST_DECODED_SIZE_MISMATCH);
    FillBindings(a, bindings);

    /* Repointing a binding at the other existing symbol changes the derived
     * ROM range, so the generator now rejects the front artifacts there. This
     * proves the derived range is keyed off the symbol metadata. */
    Gen3Buffer_Destroy(&mutated);
    Gen3Buffer_Init(&mutated, 0);
    bindings[0].symbol = "gTrainerPalette_Brendan";
    r = GenerateDefault(a, bindings, ARRAY_SIZE(bindings), &mutated);
    CHECK("symbol change propagates to the derived range",
          r == GEN3_MANIFEST_ARTIFACT_MISMATCH);

    Gen3Buffer_Destroy(&baseline);
    Gen3Buffer_Destroy(&mutated);
}

/* ---- end-to-end via the shipped TOML documents -------------------------- */

static void TestFromToml(struct TestAssets *a)
{
    struct Gen3Buffer catalogBuf;
    struct Gen3Buffer bindingsBuf;
    struct Gen3TomlDocument catalogDoc;
    struct Gen3TomlDocument bindingsDoc;
    struct Gen3Buffer out;
    struct Gen3ManifestConfig config;
    char errbuf[512];
    enum Gen3ManifestResult r;
    bool parsed;

    Gen3Buffer_Init(&catalogBuf, 0);
    Gen3Buffer_Init(&bindingsBuf, 0);
    Gen3Buffer_Init(&out, 0);

    parsed = ReadFile("resources/catalogs/emerald/catalog.toml", &catalogBuf)
          && ReadFile("resources/extraction/emerald/bpee01/bindings.toml", &bindingsBuf);
    CHECK("shipped catalog.toml + bindings.toml load", parsed);
    if (!parsed)
    {
        Gen3Buffer_Destroy(&catalogBuf);
        Gen3Buffer_Destroy(&bindingsBuf);
        Gen3Buffer_Destroy(&out);
        return;
    }

    memset(&catalogDoc, 0, sizeof(catalogDoc));
    memset(&bindingsDoc, 0, sizeof(bindingsDoc));
    if (!Gen3Toml_Parse(catalogBuf.data, catalogBuf.length,
                        &catalogDoc, errbuf, sizeof(errbuf))
     || !Gen3Toml_Parse(bindingsBuf.data, bindingsBuf.length,
                        &bindingsDoc, errbuf, sizeof(errbuf)))
    {
        CHECK("shipped TOML documents parse", false);
        Gen3Buffer_Destroy(&catalogBuf);
        Gen3Buffer_Destroy(&bindingsBuf);
        Gen3Buffer_Destroy(&out);
        return;
    }

    memset(&config, 0, sizeof(config));
    config.expectedRomSha1Hex = a->romSha1Hex;
    config.provenance = "test provenance note";
    r = Gen3Manifest_FromToml(&catalogDoc, &bindingsDoc,
                              NULL /* resolve artifact paths against cwd */,
                              (const uint8_t *)a->elf.data, a->elf.length,
                              (const uint8_t *)a->rom.data, a->rom.length,
                              &config, &out, errbuf, sizeof(errbuf));
    CHECK("FromToml end-to-end succeeds", r == GEN3_MANIFEST_OK);
    CHECK("FromToml output carries the provenance note",
          r == GEN3_MANIFEST_OK && Contains(&out, "provenance: test provenance note"));
    CHECK("FromToml output records both resources",
          r == GEN3_MANIFEST_OK && Contains(&out, "gTrainerFrontPic_Brendan")
          && Contains(&out, "gTrainerPalette_Brendan"));

    Gen3Toml_Destroy(&catalogDoc);
    Gen3Toml_Destroy(&bindingsDoc);
    Gen3Buffer_Destroy(&catalogBuf);
    Gen3Buffer_Destroy(&bindingsBuf);
    Gen3Buffer_Destroy(&out);
}

int main(void)
{
    struct TestAssets assets;
    printf("== gen3-elf-manifest R1A standalone tests ==\n");

    if (!LoadAssets(&assets))
    {
        fprintf(stderr, "FAIL: cannot load repo artifacts / build fixture\n");
        return 1;
    }

    TestLz77(&assets);
    TestKeyDerivation();
    TestHappyPath(&assets);
    TestErrorPaths(&assets);
    TestMetadataSensitivity(&assets);
    TestFromToml(&assets);

    FreeAssets(&assets);

    printf("== %d checks, %d failures ==\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
