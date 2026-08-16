/* Stage R5/R8 installed-pack validation.
 *
 * Deep-audits the installed production .rpack produced by the R3 importer
 * against the production manifest, the retail ROM identity, and the canonical
 * source artifacts:
 *
 *   - pack parses + is VALID for the production profile;
 *   - pack metadata digest fields equal the SHA-256 of the actual catalog and
 *     production manifest files on disk;
 *   - EVERY entry (R8: 196 = 186 front + 10 back) satisfies the manifest
 *     contract: payload size == decoded_length, payload SHA-256 ==
 *     canonical_decoded_sha256, provenance == rom_offset/encoded_length, and
 *     the retail ROM source slice reproduces the canonical payload under the
 *     record's source encoding (gba-lz77: strict decode; raw: the slice IS
 *     the payload, R8 trainer-back sheets);
 *   - the 10 trainer-back entries are byte-identical to the canonical source
 *     artifacts (8 raw .4bpp sheets + 2 strict-decoded gbapal palettes);
 *   - the two Brendan front entries stay byte-identical to their canonical
 *     artifacts (R5 regression).
 *
 * Usage: emerald_resource_import_validate <pack> <manifest> <catalog> <rom>
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_resource_import.h"
#include "gen3/resources/lz77.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"

#define MAX_RECORDS 256u

struct ManifestRecord
{
    char id[192];
    long long romOffset;
    long long encodedLength;
    long long decodedLength;
    enum Gen3ResourcePackSourceEncodingCode encoding;
    uint8_t canonicalSha[32];
    bool present;
};

static unsigned gFails;

static void Fail(const char *fmt, ...)
{
    va_list ap;
    gFails++;
    printf("      FAIL: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void PrintHex(const char *label, const uint8_t *p, size_t n)
{
    size_t i;
    printf("    %s: ", label);
    for (i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
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

static bool FromHexInto(uint8_t *out, const char *hex, size_t n)
{
    size_t i;
    if (hex == NULL || strlen(hex) != n * 2u)
        return false;
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)((HexVal(hex[2u * i]) << 4u) | HexVal(hex[2u * i + 1u]));
    return true;
}

static int HashFile(const char *path, uint8_t out[32], size_t *len)
{
    struct Gen3Buffer buf;
    char err[256];
    struct Gen3Sha256Context ctx;
    if (!Gen3Util_ReadFile(path, &buf, err, sizeof(err)))
    {
        fprintf(stderr, "read '%s': %s\n", path, err);
        return 0;
    }
    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, (const uint8_t *)buf.data, buf.length);
    Gen3Sha256_Final(&ctx, out);
    if (len)
        *len = buf.length;
    Gen3Buffer_Destroy(&buf);
    return 1;
}

static void Sha256Of(const uint8_t *data, size_t size, uint8_t out[32])
{
    struct Gen3Sha256Context ctx;
    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, data, size);
    Gen3Sha256_Final(&ctx, out);
}

/* Parse the production manifest into a per-record contract table. */
static size_t LoadManifestRecords(const char *manifestPath, struct ManifestRecord *recs)
{
    struct Gen3Buffer buf;
    struct Gen3TomlDocument doc;
    char err[512];
    size_t i, count, found = 0u;

    memset(recs, 0, sizeof(*recs) * MAX_RECORDS);
    if (!Gen3Util_ReadFile(manifestPath, &buf, err, sizeof(err)))
    {
        fprintf(stderr, "cannot read manifest '%s': %s\n", manifestPath, err);
        return 0u;
    }
    memset(&doc, 0, sizeof(doc));
    if (!Gen3Toml_Parse(buf.data, buf.length, &doc, err, sizeof(err)))
    {
        fprintf(stderr, "cannot parse manifest: %s\n", err);
        Gen3Buffer_Destroy(&buf);
        return 0u;
    }
    count = Gen3Toml_GetArrayCount(&doc.root, "records");
    if (count > MAX_RECORDS)
        count = MAX_RECORDS;
    for (i = 0u; i < count; i++)
    {
        const struct Gen3TomlMap *rec = Gen3Toml_GetArrayItem(&doc.root, "records", i);
        const char *id = NULL;
        const char *encoding = NULL;
        const char *canonHex = NULL;
        long long romOffset = 0, encodedLength = 0, decodedLength = 0;
        if (rec == NULL || !Gen3Toml_GetString(rec, "id", &id)
         || !Gen3Toml_GetInteger(rec, "rom_offset", &romOffset)
         || !Gen3Toml_GetInteger(rec, "encoded_length", &encodedLength)
         || !Gen3Toml_GetInteger(rec, "decoded_length", &decodedLength)
         || !Gen3Toml_GetString(rec, "source_encoding", &encoding)
         || !Gen3Toml_GetString(rec, "canonical_decoded_sha256", &canonHex))
            continue;
        if (strlen(id) >= sizeof(recs[found].id))
            continue;
        snprintf(recs[found].id, sizeof(recs[found].id), "%s", id);
        recs[found].romOffset = romOffset;
        recs[found].encodedLength = encodedLength;
        recs[found].decodedLength = decodedLength;
        if (strcmp(encoding, "raw") == 0)
            recs[found].encoding = GEN3_PACK_SOURCE_ENCODING_RAW;
        else
            recs[found].encoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
        if (!FromHexInto(recs[found].canonicalSha, canonHex, 32u))
            continue;
        recs[found].present = true;
        found++;
    }
    Gen3Toml_Destroy(&doc);
    Gen3Buffer_Destroy(&buf);
    return found;
}

static const struct ManifestRecord *FindRecord(const struct ManifestRecord *recs,
                                               size_t count, const char *id)
{
    size_t i;
    for (i = 0u; i < count; i++)
        if (recs[i].present && strcmp(recs[i].id, id) == 0)
            return &recs[i];
    return NULL;
}

/* Verify one pack entry against its manifest contract and the ROM. */
static int VerifyEntry(const struct ManifestRecord *rec,
                       const struct Gen3ResourcePackEntry *e,
                       const uint8_t *rom, size_t romSize)
{
    uint8_t payloadSha[32];
    int ok = 1;

    printf("    entry: %s\n", e->canonicalName);
    printf("      encoding=%d payloadSize=%zu (manifest %lld)\n",
           (int)e->sourceEncoding, e->payloadSize, rec->decodedLength);

    if ((size_t)rec->decodedLength != e->payloadSize)
    {
        Fail("payload size %zu != manifest decoded_length %lld",
             e->payloadSize, rec->decodedLength);
        ok = 0;
    }
    if (memcmp(e->payloadSha256, rec->canonicalSha, 32u) != 0)
    {
        Fail("payload SHA-256 != manifest canonical_decoded_sha256");
        ok = 0;
    }
    Sha256Of(e->payload, e->payloadSize, payloadSha);
    if (memcmp(payloadSha, e->payloadSha256, 32u) != 0)
    {
        Fail("recomputed payload SHA-256 != pack payloadSha256");
        ok = 0;
    }
    if ((uint64_t)rec->romOffset != e->sourceRomOffset
     || (uint64_t)rec->encodedLength != e->sourceEncodedSize)
    {
        Fail("provenance (0x%llx/%llu) != manifest (0x%llx/%lld)",
             (unsigned long long)e->sourceRomOffset,
             (unsigned long long)e->sourceEncodedSize,
             (unsigned long long)rec->romOffset, rec->encodedLength);
        ok = 0;
    }
    if ((int)rec->encoding != (int)e->sourceEncoding)
    {
        Fail("source encoding %d != manifest %d", (int)e->sourceEncoding,
             (int)rec->encoding);
        ok = 0;
    }

    /* The retail ROM source slice must reproduce the canonical payload. */
    if (e->sourceRomOffset + e->sourceEncodedSize > romSize)
    {
        Fail("ROM slice out of bounds");
        ok = 0;
    }
    else
    {
        const uint8_t *slice = rom + e->sourceRomOffset;
        if (e->sourceEncoding == GEN3_PACK_SOURCE_ENCODING_RAW)
        {
            /* R8: the artifact IS the canonical decoded payload. */
            if (e->sourceEncodedSize != e->payloadSize
             || memcmp(slice, e->payload, e->payloadSize) != 0)
            {
                Fail("raw ROM slice != canonical payload");
                ok = 0;
            }
        }
        else
        {
            uint8_t dec[16384];
            size_t decLen = 0;
            enum Gen3Lz77Result r = Gen3Lz77_Decode(slice,
                                                    (size_t)e->sourceEncodedSize,
                                                    dec, sizeof(dec), &decLen);
            if (r != GEN3_LZ77_OK || decLen != e->payloadSize
             || memcmp(dec, e->payload, e->payloadSize) != 0)
            {
                Fail("strict decode of ROM slice != canonical payload (rc=%d)", (int)r);
                ok = 0;
            }
        }
    }
    if (ok)
        printf("      entry OK\n");
    return ok;
}

/* Compare one pack entry against a canonical source artifact. */
static int CheckArtifact(const struct Gen3ResourcePack *pack, const char *canonical,
                         const char *artifactPath, bool lz77Encoded)
{
    const struct Gen3ResourcePackEntry *e =
        Gen3ResourcePack_FindByCanonicalName(pack, canonical);
    struct Gen3Buffer art;
    char err[512];
    int ok = 1;

    if (e == NULL)
    {
        printf("    MISSING ENTRY: %s\n", canonical);
        return 0;
    }
    printf("    artifact: %s <= %s\n", canonical, artifactPath);
    if (!Gen3Util_ReadFile(artifactPath, &art, err, sizeof(err)))
    {
        printf("      cannot read canonical artifact: %s\n", err);
        return 0;
    }
    if (lz77Encoded)
    {
        uint8_t dec[16384];
        size_t decLen = 0;
        enum Gen3Lz77Result r = Gen3Lz77_Decode((const uint8_t *)art.data,
                                                art.length, dec, sizeof(dec),
                                                &decLen);
        if (r != GEN3_LZ77_OK || decLen != e->payloadSize
         || memcmp(dec, e->payload, e->payloadSize) != 0)
        {
            printf("      payload != strict-decoded artifact (rc=%d)\n", (int)r);
            ok = 0;
        }
        else
            printf("      payload == strict-decoded artifact (%zu B)\n", decLen);
    }
    else if (e->payloadSize != art.length
          || memcmp(e->payload, art.data, art.length) != 0)
    {
        printf("      payload != artifact bytes\n");
        ok = 0;
    }
    else
        printf("      payload == artifact bytes (%zu B)\n", art.length);
    Gen3Buffer_Destroy(&art);
    return ok;
}

int main(int argc, char **argv)
{
    struct EmeraldInstalledInfo info;
    struct EmeraldImportReport report;
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList diags;
    struct Gen3ResourcePackProfile pp;
    struct Gen3ResourcePackEntry *entries = NULL;
    struct ManifestRecord recs[MAX_RECORDS];
    const struct EmeraldRomProfile *profile;
    struct Gen3Buffer romBuf;
    char err[512];
    size_t recordCount, i, entryCount;
    int ok = 1;

    if (argc != 5)
    {
        fprintf(stderr, "usage: %s <pack> <manifest> <catalog> <rom>\n", argv[0]);
        return 2;
    }

    profile = EmeraldRomProfile_Bpee01Rev0();
    memset(&info, 0, sizeof(info));
    memset(&report, 0, sizeof(report));
    if (EmeraldImport_ValidateInstalled(argv[1], profile, &info, &report) != EMERALD_IMPORT_OK)
    {
        fprintf(stderr, "ValidateInstalled failed: (%d) %s\n", (int)report.error, report.message);
        return 1;
    }
    printf("== ValidateInstalled ==\n");
    printf("  status=%d (0=VALID) basePackVersion=%u entryCount=%u gameId=%.7s\n",
           (int)info.status, info.basePackVersion, info.entryCount, info.gameId);
    PrintHex("  romSha1   ", info.romSha1, 20u);
    PrintHex("  romSha256 ", info.romSha256, 32u);
    printf("  hasLogicalDigest=%d\n", info.hasLogicalDigest);

    Gen3ResourcePackDiagnostics_Init(&diags);
    if (Gen3ResourcePack_OpenFile(argv[1], &pack, &diags) != GEN3_PACK_OK || !pack)
    {
        fprintf(stderr, "cannot open pack\n");
        return 1;
    }
    Gen3ResourcePack_GetProfile(pack, &pp);

    printf("\n== pack metadata digest cross-checks ==\n");
    {
        uint8_t manifestHash[32], catalogHash[32];
        size_t manifestLen, catalogLen;
        if (HashFile(argv[2], manifestHash, &manifestLen))
        {
            PrintHex("  manifest file SHA-256", manifestHash, 32);
            printf("  == pack extractionManifestSha256: %s\n",
                   memcmp(manifestHash, pp.extractionManifestSha256, 32) == 0 ? "YES" : "NO");
            if (memcmp(manifestHash, pp.extractionManifestSha256, 32) != 0)
                ok = 0;
        }
        if (HashFile(argv[3], catalogHash, &catalogLen))
        {
            PrintHex("  catalog  file SHA-256", catalogHash, 32);
            printf("  == pack catalogSha256          : %s\n",
                   memcmp(catalogHash, pp.catalogSha256, 32) == 0 ? "YES" : "NO");
            if (memcmp(catalogHash, pp.catalogSha256, 32) != 0)
                ok = 0;
        }
    }
    printf("  game code=%.4s maker=%.2s revision=%u\n",
           pp.gameCode, pp.makerCode, pp.softwareRevision);

    if (!Gen3Util_ReadFile(argv[4], &romBuf, err, sizeof(err)))
    {
        fprintf(stderr, "cannot read ROM: %s\n", err);
        return 1;
    }

    recordCount = LoadManifestRecords(argv[2], recs);
    printf("\n== manifest contract: %zu records, %zu pack entries ==\n",
           recordCount, Gen3ResourcePack_GetEntryCount(pack));
    if (recordCount != Gen3ResourcePack_GetEntryCount(pack))
    {
        printf("  FAIL: record count != entry count\n");
        ok = 0;
    }

    printf("\n== every entry against its manifest contract and the ROM ==\n");
    entryCount = Gen3ResourcePack_GetEntryCount(pack);
    entries = (struct Gen3ResourcePackEntry *)calloc(entryCount ? entryCount : 1u,
                                                     sizeof(*entries));
    if (entries == NULL)
    {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    for (i = 0u; i < entryCount; i++)
    {
        const struct Gen3ResourcePackEntry *e = Gen3ResourcePack_GetEntry(pack, i);
        const struct ManifestRecord *rec;
        if (e == NULL)
        {
            Fail("pack entry %zu missing", i);
            ok = 0;
            continue;
        }
        rec = FindRecord(recs, recordCount, e->canonicalName);
        if (rec == NULL)
        {
            printf("    entry: %s\n", e->canonicalName);
            Fail("no manifest record for entry");
            ok = 0;
            continue;
        }
        if (!VerifyEntry(rec, e, (const uint8_t *)romBuf.data, romBuf.length))
            ok = 0;
    }
    for (i = 0u; i < recordCount; i++)
    {
        if (!recs[i].present)
            continue;
        if (Gen3ResourcePack_FindByCanonicalName(pack, recs[i].id) == NULL)
        {
            printf("    record: %s\n", recs[i].id);
            Fail("manifest record has no pack entry");
            ok = 0;
        }
    }
    free(entries);

    printf("\n== trainer-back canonical source artifacts (R8) ==\n");
    ok &= CheckArtifact(pack, "emerald:trainer/brendan/battle/back/sheet",
                        "graphics/trainers/back_pics/brendan.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/may/battle/back/sheet",
                        "graphics/trainers/back_pics/may.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/red/battle/back/sheet",
                        "graphics/trainers/back_pics/red.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/leaf/battle/back/sheet",
                        "graphics/trainers/back_pics/leaf.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/ruby-sapphire-brendan/battle/back/sheet",
                        "graphics/trainers/back_pics/brendan_rs.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/ruby-sapphire-may/battle/back/sheet",
                        "graphics/trainers/back_pics/may_rs.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/wally/battle/back/sheet",
                        "graphics/trainers/back_pics/wally.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/steven/battle/back/sheet",
                        "graphics/trainers/back_pics/steven.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/red/battle/back/palette",
                        "graphics/trainers/back_pics/red.gbapal.lz", true);
    ok &= CheckArtifact(pack, "emerald:trainer/leaf/battle/back/palette",
                        "graphics/trainers/back_pics/leaf.gbapal.lz", true);

    printf("\n== Brendan front canonical source artifacts (R5 regression) ==\n");
    ok &= CheckArtifact(pack, "emerald:trainer/brendan/battle/front/sheet",
                        "graphics/trainers/front_pics/brendan.4bpp", false);
    ok &= CheckArtifact(pack, "emerald:trainer/brendan/battle/front/normal-palette",
                        "graphics/trainers/palettes/brendan.gbapal", false);

    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&diags);
    Gen3Buffer_Destroy(&romBuf);
    printf("\nINSTALLED PACK VALIDATION: %s\n", ok && gFails == 0u ? "PASS" : "FAIL");
    return (ok && gFails == 0u) ? 0 : 1;
}
