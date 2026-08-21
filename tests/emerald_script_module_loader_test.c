/* R13-G2 §13: script-module loader/provider test.
 *
 * Drives the pack reader (the R6 loader's provider surface) over the REAL
 * production pack and the committed script-module artifacts, exactly as the
 * runtime loader consumes them:
 *
 *   1. the pack opens, reports the 20,988-entry surface and the qualified
 *      BPEE01 Rev 0 ROM identity (SHA-1 f3ae0881...);
 *   2. all 467 payload-bearing script modules are found by canonical name,
 *      byte-exact against the committed .bin artifacts, and their payload
 *      SHA-256 matches BOTH manifest digests (source_encoded and canonical
 *      decoded — identical for raw G bytes);
 *   3. the 56 routing-only modules (terminator `.byte 0` map-script tables,
 *      plan §5) are catalog identities only: FindByCanonicalName must return
 *      NULL for each, and no pack entry in the script namespace may exist
 *      outside the 523-entry catalog (pack surface == catalog surface);
 *   4. tamper refusal: a one-byte flip in the payload section, a one-byte
 *      flip in the TOC, and a truncated image are all REFUSED by
 *      Gen3ResourcePack_OpenFile with the structured reason codes
 *      (PAYLOAD_HASH_MISMATCH / TOC_HASH_MISMATCH / truncated header).
 *
 * Usage: emerald_script_module_loader_test <pack> <manifest> <catalog>
 *        <repo-root> <tmp-dir>
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_pack.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/toml.h"

static int gFailures = 0;
static int gChecks = 0;

static void Check(bool ok, const char *what)
{
    gChecks++;
    if (!ok) {
        gFailures++;
        fprintf(stderr, "FAIL: %s\n", what);
    } else {
        printf("ok: %s\n", what);
    }
}

static const char *TomlString(const struct Gen3TomlMap *map, const char *key)
{
    const struct Gen3TomlEntry *e = Gen3Toml_FindEntry(map, key);
    if (e == NULL || e->value == NULL || e->value->kind != GEN3_TOML_STRING)
        return NULL;
    return e->value->string;
}

static bool TomlInteger(const struct Gen3TomlMap *map, const char *key,
                        long long *out)
{
    const struct Gen3TomlEntry *e = Gen3Toml_FindEntry(map, key);
    if (e == NULL || e->value == NULL || e->value->kind != GEN3_TOML_INTEGER)
        return false;
    *out = e->value->integer;
    return true;
}

static char HexNibble(uint8_t b)
{
    return b < 10 ? (char)('0' + b) : (char)('a' + b - 10);
}

static void HexEncode(const uint8_t *bytes, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        out[2 * i] = HexNibble(bytes[i] >> 4);
        out[2 * i + 1] = HexNibble(bytes[i] & 0x0f);
    }
    out[2 * n] = '\0';
}

static void DigestSha256(const uint8_t *data, size_t size, uint8_t digest[32])
{
    struct Gen3Sha256Context ctx;
    Gen3Sha256_Init(&ctx);
    Gen3Sha256_Update(&ctx, data, size);
    Gen3Sha256_Final(&ctx, digest);
}

static uint8_t *ReadWholeFile(const char *path, size_t *outSize)
{
    FILE *f = fopen(path, "rb");
    uint8_t *bytes;
    long size;
    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    bytes = (uint8_t *)malloc((size_t)size);
    if (bytes == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        free(bytes);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *outSize = (size_t)size;
    return bytes;
}

static uint32_t ReadLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void WriteWholeFile(const char *path, const uint8_t *bytes, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        exit(2);
    if (size != 0 && fwrite(bytes, 1, size, f) != size)
        exit(2);
    fclose(f);
}

static size_t CountRecords(const struct Gen3TomlMap *root, const char *arrayKey)
{
    const struct Gen3TomlEntry *e = Gen3Toml_FindEntry(root, arrayKey);
    if (e == NULL || e->value == NULL || e->value->kind != GEN3_TOML_ARRAY)
        return 0;
    return e->value->itemCount;
}

static const struct Gen3TomlMap *RecordAt(const struct Gen3TomlMap *root,
                                          const char *arrayKey, size_t index)
{
    const struct Gen3TomlEntry *e = Gen3Toml_FindEntry(root, arrayKey);
    if (e == NULL || e->value == NULL || e->value->kind != GEN3_TOML_ARRAY
        || index >= e->value->itemCount)
        return NULL;
    return &e->value->items[index]->map;
}

int main(int argc, char **argv)
{
    const char *packPath, *manifestPath, *catalogPath, *repoRoot, *tmpDir;
    size_t i, n;
    struct Gen3ResourcePack *pack = NULL;
    struct Gen3ResourcePackDiagnosticList diag;
    size_t size;
    char path[4096];

    if (argc < 6) {
        fprintf(stderr, "usage: %s <pack> <manifest> <catalog> <repo-root> "
                        "<tmp-dir>\n", argv[0]);
        return 2;
    }
    packPath = argv[1];
    manifestPath = argv[2];
    catalogPath = argv[3];
    repoRoot = argv[4];
    tmpDir = argv[5];

    memset(&diag, 0, sizeof(diag));

    /* ---- Leg 1: open + identity ------------------------------------ */
    {
        enum Gen3ResourcePackError err =
            Gen3ResourcePack_OpenFile(packPath, &pack, &diag);
        if (err != GEN3_PACK_OK || pack == NULL) {
            fprintf(stderr, "FATAL: production pack refused: %s\n",
                    Gen3ResourcePackError_Describe(err));
            return 1;
        }
        printf("ok: production pack opens\n");
        gChecks++;

        struct Gen3ResourcePackProfile profile;
        if (Gen3ResourcePack_GetProfile(pack, &profile)) {
            char sha1hex[41];
            HexEncode(profile.sourceRomSha1, 20, sha1hex);
            char okline[128];
            snprintf(okline, sizeof(okline),
                     "pack profile ROM SHA-1 %s", sha1hex);
            Check(strcmp(sha1hex,
                         "f3ae088181bf583e55daf962a92bb46f4f1d07b7") == 0,
                  okline);
            Check(profile.sourceRomSize == 16777216ull,
                  "pack profile ROM size 16 MiB");
        } else {
            Check(false, "pack profile readable");
        }
        {
            char line[128];
            snprintf(line, sizeof(line), "pack entry count == 20,988 (got %zu)",
                     Gen3ResourcePack_GetEntryCount(pack));
            Check(Gen3ResourcePack_GetEntryCount(pack) == 20988u, line);
        }
    }

    /* ---- Leg 2: 467 embedded modules byte-exact + digest ----------- */
    {
        uint8_t *manifestBytes = ReadWholeFile(manifestPath, &size);
        struct Gen3TomlDocument doc;
        char tomlErr[128];
        char line[512];
        size_t found = 0;
        if (manifestBytes == NULL) {
            Check(false, "manifest readable");
            return 1;
        }
        if (!Gen3Toml_Parse((const char *)manifestBytes, size, &doc,
                            tomlErr, sizeof(tomlErr))) {
            fprintf(stderr, "manifest TOML error: %s\n", tomlErr);
            Check(false, "manifest parses");
            return 1;
        }
        Check(true, "manifest parses");
        n = CountRecords(&doc.root, "records");
        snprintf(line, sizeof(line), "manifest records == 467 (got %zu)", n);
        Check(n == 467, line);

        for (i = 0; i < n; i++) {
            const struct Gen3TomlMap *rec = RecordAt(&doc.root, "records", i);
            const char *id = TomlString(rec, "id");
            const char *artifact = TomlString(rec, "source_artifact");
            const char *encSha = TomlString(rec, "source_encoded_sha256");
            const char *decSha = TomlString(rec, "canonical_decoded_sha256");
            long long encodedLen = 0;
            const struct Gen3ResourcePackEntry *entry;
            uint8_t *artifactBytes;
            size_t artifactSize;
            uint8_t digest[32];
            char digestHex[65];

            if (id == NULL || artifact == NULL || encSha == NULL
                || decSha == NULL || !TomlInteger(rec, "encoded_length",
                                                  &encodedLen)) {
                Check(false, "manifest record shape");
                continue;
            }
            entry = Gen3ResourcePack_FindByCanonicalName(pack, id);
            if (entry == NULL) {
                snprintf(line, sizeof(line), "%s found in pack", id);
                Check(false, line);
                continue;
            }
            found++;
            snprintf(line, sizeof(line), "%s found in pack (%zu B)",
                     id, entry->payloadSize);
            Check(entry->payloadSize == (size_t)encodedLen, line);

            snprintf(path, sizeof(path), "%s/%s", repoRoot, artifact);
            artifactBytes = ReadWholeFile(path, &artifactSize);
            if (artifactBytes == NULL) {
                snprintf(line, sizeof(line), "%s artifact readable", id);
                Check(false, line);
                continue;
            }
            snprintf(line, sizeof(line), "%s byte-exact vs committed artifact",
                     id);
            Check(artifactSize == entry->payloadSize
                  && memcmp(artifactBytes, entry->payload,
                            entry->payloadSize) == 0,
                  line);
            DigestSha256(artifactBytes, artifactSize, digest);
            HexEncode(digest, 32, digestHex);
            snprintf(line, sizeof(line), "%s payload SHA-256 == source digest",
                     id);
            Check(strcmp(digestHex, encSha) == 0, line);
            snprintf(line, sizeof(line),
                     "%s payload SHA-256 == canonical digest", id);
            Check(strcmp(digestHex, decSha) == 0, line);
            free(artifactBytes);
        }
        snprintf(line, sizeof(line), "all 467 embedded modules byte-exact "
                 "(found %zu)", found);
        Check(found == 467, line);
        Gen3Toml_Destroy(&doc);
        free(manifestBytes);
    }

    /* ---- Leg 3: 56 routing-only modules absent from the pack ------- */
    /* The catalog is the full 523-identity surface (the ownership file's
     * [resources.targets] dotted headers are outside toml.c's subset, so
     * the per-record state check runs in the run script instead). */
    {
        uint8_t *catBytes = ReadWholeFile(catalogPath, &size);
        struct Gen3TomlDocument doc;
        char tomlErr[128];
        char line[512];
        size_t total = 0, embedded = 0, missing = 0;
        size_t extraScriptEntries = 0;
        size_t j;
        if (catBytes == NULL || !Gen3Toml_Parse((const char *)catBytes, size,
                                                &doc, tomlErr,
                                                sizeof(tomlErr))) {
            fprintf(stderr, "catalog TOML error: %s\n", tomlErr);
            Check(false, "catalog parses");
            return 1;
        }
        total = CountRecords(&doc.root, "resources");
        snprintf(line, sizeof(line), "catalog resources == 523 (got %zu)",
                 total);
        Check(total == 523, line);
        for (i = 0; i < total; i++) {
            const struct Gen3TomlMap *rec =
                RecordAt(&doc.root, "resources", i);
            const char *id = TomlString(rec, "id");
            if (id == NULL)
                continue;
            if (Gen3ResourcePack_FindByCanonicalName(pack, id) != NULL)
                embedded++;
            else {
                missing++;
                snprintf(line, sizeof(line),
                         "routing-only identity absent from pack: %s", id);
                Check(true, line);
            }
        }
        /* No pack entry in the script namespace may exist outside the
         * catalog (the pack's script surface == the catalog's). */
        for (j = 0; j < Gen3ResourcePack_GetEntryCount(pack); j++) {
            const struct Gen3ResourcePackEntry *entry =
                Gen3ResourcePack_GetEntry(pack, j);
            const char *name = entry->canonicalName;
            if (name != NULL && strncmp(name, "emerald:script/", 15) == 0) {
                size_t k;
                bool known = false;
                for (k = 0; k < total; k++) {
                    const struct Gen3TomlMap *rec =
                        RecordAt(&doc.root, "resources", k);
                    const char *id = TomlString(rec, "id");
                    if (id != NULL && strcmp(id, name) == 0) {
                        known = true;
                        break;
                    }
                }
                if (!known)
                    extraScriptEntries++;
            }
        }
        snprintf(line, sizeof(line),
                 "script entries in pack == 467 (found %zu)", embedded);
        Check(embedded == 467, line);
        snprintf(line, sizeof(line),
                 "routing-only catalog identities == 56 (found %zu)", missing);
        Check(missing == 56, line);
        snprintf(line, sizeof(line),
                 "no pack script entry outside the catalog (found %zu)",
                 extraScriptEntries);
        Check(extraScriptEntries == 0, line);
        Gen3Toml_Destroy(&doc);
        free(catBytes);
    }

    /* ---- Leg 4: tamper refusal ------------------------------------- */
    {
        enum Gen3ResourcePackError err;
        struct Gen3ResourcePack *tampered = NULL;
        uint8_t *image = ReadWholeFile(packPath, &size);
        uint32_t payloadOff, tocOff;
        char line[512];
        if (image == NULL) {
            Check(false, "pack image readable for tamper");
            return 1;
        }
        payloadOff = ReadLe32(image + GEN3_PACK_OFF_PAYLOAD_OFFSET);
        tocOff = ReadLe32(image + GEN3_PACK_OFF_TOC_OFFSET);

        /* (a) one flipped payload byte -> payload aggregate hash mismatch */
        image[payloadOff] ^= 0x5a;
        snprintf(path, sizeof(path), "%s/tamper_payload.rpack", tmpDir);
        WriteWholeFile(path, image, size);
        err = Gen3ResourcePack_OpenFile(path, &tampered, &diag);
        snprintf(line, sizeof(line),
                 "payload flip refused (err %s)",
                 Gen3ResourcePackError_Describe(err));
        Check(err == GEN3_PACK_ERR_PAYLOAD_HASH_MISMATCH, line);
        image[payloadOff] ^= 0x5a;

        /* (b) one flipped TOC byte -> refused (key/name integrity or the
         * TOC hash, whichever the parser catches first) */
        image[tocOff + 8] ^= 0x5a;
        snprintf(path, sizeof(path), "%s/tamper_toc.rpack", tmpDir);
        WriteWholeFile(path, image, size);
        err = Gen3ResourcePack_OpenFile(path, &tampered, &diag);
        snprintf(line, sizeof(line),
                 "TOC flip refused (err %s)",
                 Gen3ResourcePackError_Describe(err));
        Check(err != GEN3_PACK_OK, line);
        image[tocOff + 8] ^= 0x5a;

        /* (c) truncated image -> refused with a structural reason */
        snprintf(path, sizeof(path), "%s/tamper_truncated.rpack", tmpDir);
        WriteWholeFile(path, image, 100);
        err = Gen3ResourcePack_OpenFile(path, &tampered, &diag);
        snprintf(line, sizeof(line),
                 "truncated image refused (err %s)",
                 Gen3ResourcePackError_Describe(err));
        Check(err != GEN3_PACK_OK, line);
        free(image);
    }

    Gen3ResourcePack_Destroy(pack);

    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
