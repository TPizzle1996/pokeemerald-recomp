/* Stage R3 local Emerald ROM import pipeline (see emerald_resource_import.h).
 *
 * The importer is a pure library: given a ROM image (bytes or path), the R1A
 * extraction manifest, the R2 catalog contract, a destination path, and one
 * ROM profile, it strictly validates everything and produces/installs the
 * deterministic v1 .rpack. Nothing is ever guessed; every manifest range is
 * bounded, hashed, strict-decoded and re-hashed before it is accepted.
 *
 * Privacy (R3 §17): the user's ROM path never appears in a pack, manifest,
 * config, or persisted diagnostic. This module's reports are transient and
 * deliberately echo no filesystem path.
 */

#if defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "emerald/resources/emerald_resource_import.h"
#include "gen3/resources/lz77.h"
#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/sha1.h"
#include "gen3/resources/sha256.h"
#include "gen3/resources/toml.h"
#include "gen3/resources/util.h"

/* One resource per family entry. The R7A trainer-front family ships 186
 * resources (93 sheets + 93 palettes); R8 adds the 10 trainer-back sheets
 * (196 total). R9 adds the 1608-record Pokémon battle family, so the merged
 * manifest/catalog views must hold 1804 resources; 2048 keeps headroom. The
 * view structs hold ~0.8 MiB at this size and are heap-allocated in
 * BuildPackCore (never on the stack). */
#define EMERALD_IMPORT_MAX_RECORDS 2048u
#define EMERALD_IMPORT_MAX_PAYLOAD_SIZE (16u * 1024u * 1024u) /* 16 MiB, matches the R2 writer cap */
#define EMERALD_IMPORT_LOCK_NAME ".emerald-import.lock"

/* ------------------------------------------------------------------ */
/* Reports                                                             */
/* ------------------------------------------------------------------ */

static enum EmeraldResourceImportError SetError(struct EmeraldImportReport *report,
                                                enum EmeraldResourceImportError error,
                                                const char *format, ...)
{
    if (report != NULL)
    {
        va_list ap;
        report->error = error;
        report->entryIndex = SIZE_MAX;
        report->hasName = false;
        report->canonicalName[0] = '\0';
        va_start(ap, format);
        vsnprintf(report->message, sizeof(report->message), format, ap);
        va_end(ap);
    }
    return error;
}

static enum EmeraldResourceImportError SetErrorForRecord(
    struct EmeraldImportReport *report, size_t index, const char *name,
    enum EmeraldResourceImportError error, const char *format, ...)
{
    if (report != NULL)
    {
        va_list ap;
        report->error = error;
        report->entryIndex = index;
        report->hasName = name != NULL && name[0] != '\0';
        if (report->hasName)
        {
            snprintf(report->canonicalName, sizeof(report->canonicalName), "%s", name);
        }
        else
        {
            report->canonicalName[0] = '\0';
        }
        va_start(ap, format);
        vsnprintf(report->message, sizeof(report->message), format, ap);
        va_end(ap);
    }
    return error;
}

/* ------------------------------------------------------------------ */
/* Small byte helpers                                                  */
/* ------------------------------------------------------------------ */

static bool BytesEqual(const uint8_t *a, const uint8_t *b, size_t n)
{
    return n == 0u || memcmp(a, b, n) == 0;
}

static void Sha256Bytes(const uint8_t *data, size_t size, uint8_t digest[32])
{
    struct Gen3Sha256Context context;
    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, data, size);
    Gen3Sha256_Final(&context, digest);
}

static void Sha1Bytes(const uint8_t *data, size_t size, uint8_t digest[20])
{
    struct Gen3Sha1Context context;
    Gen3Sha1_Init(&context);
    Gen3Sha1_Update(&context, data, size);
    Gen3Sha1_Final(&context, digest);
}

static int HexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parses exactly 2*outSize lowercase/uppercase hex chars into `out`. */
static bool ParseHex(const char *hex, uint8_t *out, size_t outSize)
{
    size_t i;
    if (hex == NULL)
        return false;
    for (i = 0; i < outSize; i++)
    {
        int hi = HexNibble(hex[i * 2u]);
        int lo = HexNibble(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Buffers (bytes-borrowed or whole-file read; path never echoed)      */
/* ------------------------------------------------------------------ */

struct LoadedBuffer
{
    const uint8_t *data;
    size_t size;
    bool owned;
};

static void LoadedBuffer_Release(struct LoadedBuffer *buffer)
{
    if (buffer->owned)
    {
        free((void *)buffer->data);
        buffer->owned = false;
    }
    buffer->data = NULL;
    buffer->size = 0u;
}

static enum EmeraldResourceImportError ReadWholeFile(const char *path,
                                                     uint8_t **outData, size_t *outSize,
                                                     const char *label,
                                                     enum EmeraldResourceImportError ioError,
                                                     struct EmeraldImportReport *report)
{
    FILE *file;
    long length;
    uint8_t *data;
    size_t got;

    file = fopen(path, "rb");
    if (file == NULL)
    {
        return SetError(report, ioError, "cannot open %s (errno %d)", label, errno);
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return SetError(report, ioError, "cannot size %s (errno %d)", label, errno);
    }
    data = (uint8_t *)malloc((size_t)length > 0u ? (size_t)length : 1u);
    if (data == NULL)
    {
        fclose(file);
        return SetError(report, EMERALD_IMPORT_ERR_OUT_OF_MEMORY, "out of memory reading %s", label);
    }
    got = fread(data, 1u, (size_t)length, file);
    if (got != (size_t)length && ferror(file))
    {
        fclose(file);
        free(data);
        return SetError(report, ioError, "read error on %s (errno %d)", label, errno);
    }
    fclose(file);
    *outData = data;
    *outSize = (size_t)got;
    return EMERALD_IMPORT_OK;
}

/* Selects bytes-borrowed or path-read source for one input slot. */
static enum EmeraldResourceImportError LoadSlot(const uint8_t *bytes, size_t bytesSize,
                                                const char *path, const char *label,
                                                bool requireRegular,
                                                enum EmeraldResourceImportError ioError,
                                                struct LoadedBuffer *out,
                                                struct EmeraldImportReport *report)
{
    struct stat st;
    enum EmeraldResourceImportError err;

    out->data = NULL;
    out->size = 0u;
    out->owned = false;

    if (bytes != NULL)
    {
        if (bytesSize == 0u)
            return SetError(report, EMERALD_IMPORT_ERR_INVALID_ARGUMENT,
                            "%s bytes provided with zero size", label);
        out->data = bytes;
        out->size = bytesSize;
        return EMERALD_IMPORT_OK;
    }
    if (path == NULL || path[0] == '\0')
        return SetError(report, EMERALD_IMPORT_ERR_INVALID_ARGUMENT,
                        "no %s bytes or path supplied", label);
    if (requireRegular && stat(path, &st) == 0 && !S_ISREG(st.st_mode))
        return SetError(report, EMERALD_IMPORT_ERR_ROM_NOT_REGULAR_FILE,
                        "%s is not a regular file", label);
    err = ReadWholeFile(path, (uint8_t **)&out->data, &out->size, label, ioError, report);
    if (err == EMERALD_IMPORT_OK)
        out->owned = true; /* the buffer is heap-allocated by ReadWholeFile */
    return err;
}

/* ------------------------------------------------------------------ */
/* Parsed manifest / catalog views                                     */
/* ------------------------------------------------------------------ */

struct ImportManifestRecord
{
    char id[GEN3_RESOURCE_NAME_MAX + 1u];
    uint8_t key[GEN3_RESOURCE_KEY_SIZE];
    enum Gen3ResourceType type;
    uint32_t schema;
    uint64_t romOffset;
    uint64_t encodedLength;
    uint64_t decodedLength;
    enum Gen3ResourcePackSourceEncodingCode sourceEncoding;
    uint8_t sourceEncodedSha256[GEN3_PACK_SHA256_SIZE];
    uint8_t canonicalDecodedSha256[GEN3_PACK_SHA256_SIZE];
    bool requiredForBase; /* filled from the catalog contract */
};

struct ImportManifestView
{
    long long manifestVersion;
    char game[64];
    char romProfile[64];
    char qualification[64];
    uint64_t romSize;
    uint8_t romSha1[GEN3_PACK_SHA1_SIZE];
    uint8_t romSha256[GEN3_PACK_SHA256_SIZE];
    size_t recordCount;
    struct ImportManifestRecord records[EMERALD_IMPORT_MAX_RECORDS];
};

struct ImportCatalogResource
{
    char id[GEN3_RESOURCE_NAME_MAX + 1u];
    enum Gen3ResourceType type;
    uint32_t schema;
    bool requiredForBase;
};

struct ImportCatalogView
{
    long long catalogVersion;
    long long resourceApiMajor;
    char game[64];
    char romProfile[64];
    size_t resourceCount;
    struct ImportCatalogResource resources[EMERALD_IMPORT_MAX_RECORDS];
};

static enum Gen3ResourceType ParseTypeName(const char *name)
{
    if (name == NULL)
        return GEN3_RESOURCE_TYPE_INVALID;
    if (strcmp(name, "bitmap") == 0)
        return GEN3_RESOURCE_TYPE_BITMAP;
    if (strcmp(name, "tile-graphics") == 0)
        return GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    if (strcmp(name, "palette") == 0)
        return GEN3_RESOURCE_TYPE_PALETTE;
    if (strcmp(name, "sprite-sheet") == 0)
        return GEN3_RESOURCE_TYPE_SPRITE_SHEET;
    if (strcmp(name, "sprite-metadata") == 0)
        return GEN3_RESOURCE_TYPE_SPRITE_METADATA;
    if (strcmp(name, "tileset") == 0)
        return GEN3_RESOURCE_TYPE_TILESET;
    if (strcmp(name, "tilemap") == 0)
        return GEN3_RESOURCE_TYPE_TILEMAP;
    if (strcmp(name, "font") == 0)
        return GEN3_RESOURCE_TYPE_FONT;
    if (strcmp(name, "text") == 0)
        return GEN3_RESOURCE_TYPE_TEXT;
    if (strcmp(name, "audio-sample") == 0)
        return GEN3_RESOURCE_TYPE_AUDIO_SAMPLE;
    if (strcmp(name, "music-sequence") == 0)
        return GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE;
    if (strcmp(name, "sound-effect") == 0)
        return GEN3_RESOURCE_TYPE_SOUND_EFFECT;
    if (strcmp(name, "cry") == 0)
        return GEN3_RESOURCE_TYPE_CRY;
    if (strcmp(name, "binary") == 0)
        return GEN3_RESOURCE_TYPE_BINARY;
    return GEN3_RESOURCE_TYPE_INVALID;
}

static bool ParseApiMajor(const char *api, long long *outMajor)
{
    char *end;
    long long major;
    if (api == NULL || api[0] < '0' || api[0] > '9')
        return false;
    errno = 0;
    major = strtoll(api, &end, 10);
    if (errno != 0 || major <= 0 || (*end != '.' && *end != '\0'))
        return false;
    *outMajor = major;
    return true;
}

static bool CopyStr(const char *in, char *out, size_t outSize)
{
    if (in == NULL)
        return false;
    if (strlen(in) >= outSize)
        return false;
    memcpy(out, in, strlen(in) + 1u);
    return true;
}

static enum EmeraldResourceImportError ParseManifest(const uint8_t *data, size_t size,
                                                     struct ImportManifestView *view,
                                                     struct EmeraldImportReport *report)
{
    struct Gen3TomlDocument doc;
    char errbuf[256];
    const char *value;
    long long integer;
    size_t count;
    size_t i;

    memset(view, 0, sizeof(*view));
    if (!Gen3Toml_Parse((const char *)data, size, &doc, errbuf, sizeof(errbuf)))
    {
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_PARSE_FAILED,
                        "manifest parse failed: %s", errbuf);
    }

    if (!Gen3Toml_GetInteger(&doc.root, "manifest_version", &integer) || integer <= 0)
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
                        "manifest missing valid manifest_version");
    }
    view->manifestVersion = integer;

    if (!Gen3Toml_GetString(&doc.root, "game", &value) || !CopyStr(value, view->game, sizeof(view->game)))
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
                        "manifest missing valid game");
    }
    if (!Gen3Toml_GetString(&doc.root, "rom_profile", &value)
     || !CopyStr(value, view->romProfile, sizeof(view->romProfile)))
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
                        "manifest missing valid rom_profile");
    }
    if (!Gen3Toml_GetString(&doc.root, "qualification", &value)
     || !CopyStr(value, view->qualification, sizeof(view->qualification)))
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
                        "manifest missing valid qualification");
    }
    if (!Gen3Toml_GetInteger(&doc.root, "rom_size", &integer) || integer < 0)
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
                        "manifest missing valid rom_size");
    }
    view->romSize = (uint64_t)integer;
    if (!Gen3Toml_GetString(&doc.root, "rom_sha1", &value)
     || !ParseHex(value, view->romSha1, GEN3_PACK_SHA1_SIZE))
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
                        "manifest missing valid rom_sha1");
    }
    if (!Gen3Toml_GetString(&doc.root, "rom_sha256", &value)
     || !ParseHex(value, view->romSha256, GEN3_PACK_SHA256_SIZE))
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
                        "manifest missing valid rom_sha256");
    }

    count = Gen3Toml_GetArrayCount(&doc.root, "records");
    if (count == 0u)
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_NO_RECORDS,
                        "manifest declares no resource records");
    }
    if (count > EMERALD_IMPORT_MAX_RECORDS)
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_TOO_MANY_RECORDS,
                        "manifest declares %zu records (limit %u)", count,
                        (unsigned)EMERALD_IMPORT_MAX_RECORDS);
    }
    view->recordCount = count;

    for (i = 0; i < count; i++)
    {
        const struct Gen3TomlMap *record = Gen3Toml_GetArrayItem(&doc.root, "records", i);
        struct ImportManifestRecord *out = &view->records[i];
        const char *idValue;
        const char *typeName;
        const char *encoding;
        long long schema;
        long long offset;
        long long encodedLength;
        long long decodedLength;

        if (record == NULL)
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, NULL, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record %zu is unreadable", i);
        }
        if (!Gen3Toml_GetString(record, "id", &idValue) || !CopyStr(idValue, out->id, sizeof(out->id)))
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, NULL, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record %zu missing valid id", i);
        }
        if (Gen3ResourceId_ValidateCanonicalName(out->id) != GEN3_RESOURCE_NAME_VALID)
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record id fails the canonical name grammar");
        }
        if (!Gen3Toml_GetString(record, "key", &value)
         || !ParseHex(value, out->key, GEN3_RESOURCE_KEY_SIZE))
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record missing valid key");
        }
        if (!Gen3Toml_GetString(record, "type", &typeName)
         || (out->type = ParseTypeName(typeName)) == GEN3_RESOURCE_TYPE_INVALID)
        {
            char badType[64];
            /* typeName points into the doc arena: snapshot before destroy. */
            snprintf(badType, sizeof(badType), "%s",
                     typeName != NULL ? typeName : "(missing)");
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_TYPE,
                                     "record has unsupported type '%s'", badType);
        }
        if (!Gen3Toml_GetInteger(record, "schema", &schema) || schema < 1)
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_SCHEMA,
                                     "record has invalid schema");
        }
        out->schema = (uint32_t)schema;
        if (!Gen3Toml_GetInteger(record, "rom_offset", &offset) || offset < 0)
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record missing valid rom_offset");
        }
        if (!Gen3Toml_GetInteger(record, "encoded_length", &encodedLength) || encodedLength <= 0)
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record missing valid encoded_length");
        }
        if (!Gen3Toml_GetInteger(record, "decoded_length", &decodedLength) || decodedLength <= 0)
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record missing valid decoded_length");
        }
        out->romOffset = (uint64_t)offset;
        out->encodedLength = (uint64_t)encodedLength;
        out->decodedLength = (uint64_t)decodedLength;
        if (!Gen3Toml_GetString(record, "source_encoding", &encoding))
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_ENCODING,
                                     "record missing source_encoding");
        }
        if (strcmp(encoding, "gba-lz77") == 0)
            out->sourceEncoding = GEN3_PACK_SOURCE_ENCODING_GBA_LZ77;
        else if (strcmp(encoding, "raw") == 0)
            out->sourceEncoding = GEN3_PACK_SOURCE_ENCODING_RAW;
        else
        {
            char badEncoding[64];
            /* encoding points into the doc arena: snapshot before destroy. */
            snprintf(badEncoding, sizeof(badEncoding), "%s",
                     encoding != NULL ? encoding : "(missing)");
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_ENCODING,
                                     "record has unsupported source_encoding '%s'", badEncoding);
        }
        if (!Gen3Toml_GetString(record, "source_encoded_sha256", &value)
         || !ParseHex(value, out->sourceEncodedSha256, GEN3_PACK_SHA256_SIZE))
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record missing valid source_encoded_sha256");
        }
        if (!Gen3Toml_GetString(record, "canonical_decoded_sha256", &value)
         || !ParseHex(value, out->canonicalDecodedSha256, GEN3_PACK_SHA256_SIZE))
        {
            Gen3Toml_Destroy(&doc);
            return SetErrorForRecord(report, i, out->id, EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
                                     "record missing valid canonical_decoded_sha256");
        }
    }

    for (i = 0; i < view->recordCount; i++)
    {
        size_t j;
        for (j = i + 1u; j < view->recordCount; j++)
        {
            if (strcmp(view->records[i].id, view->records[j].id) == 0)
            {
                Gen3Toml_Destroy(&doc);
                return SetErrorForRecord(report, j, view->records[j].id,
                                         EMERALD_IMPORT_ERR_MANIFEST_DUPLICATE_RECORD,
                                         "duplicate record id");
            }
        }
    }

    Gen3Toml_Destroy(&doc);
    return EMERALD_IMPORT_OK;
}

static enum EmeraldResourceImportError ParseCatalog(const uint8_t *data, size_t size,
                                                    struct ImportCatalogView *view,
                                                    struct EmeraldImportReport *report)
{
    struct Gen3TomlDocument doc;
    char errbuf[256];
    const char *value;
    long long integer;
    size_t count;
    size_t i;

    memset(view, 0, sizeof(*view));
    if (!Gen3Toml_Parse((const char *)data, size, &doc, errbuf, sizeof(errbuf)))
    {
        return SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                        "catalog parse failed: %s", errbuf);
    }
    if (!Gen3Toml_GetInteger(&doc.root, "catalog_version", &integer) || integer <= 0)
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                        "catalog missing valid catalog_version");
    }
    view->catalogVersion = integer;
    if (!Gen3Toml_GetString(&doc.root, "resource_api", &value) || !ParseApiMajor(value, &view->resourceApiMajor))
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                        "catalog missing valid resource_api");
    }
    if (!Gen3Toml_GetString(&doc.root, "game", &value) || !CopyStr(value, view->game, sizeof(view->game)))
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                        "catalog missing valid game");
    }
    if (!Gen3Toml_GetString(&doc.root, "rom_profile", &value)
     || !CopyStr(value, view->romProfile, sizeof(view->romProfile)))
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                        "catalog missing valid rom_profile");
    }

    count = Gen3Toml_GetArrayCount(&doc.root, "resources");
    if (count == 0u || count > EMERALD_IMPORT_MAX_RECORDS)
    {
        Gen3Toml_Destroy(&doc);
        return SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                        "catalog has %zu resources (expected 1..%u)", count,
                        (unsigned)EMERALD_IMPORT_MAX_RECORDS);
    }
    view->resourceCount = count;
    for (i = 0; i < count; i++)
    {
        const struct Gen3TomlMap *res = Gen3Toml_GetArrayItem(&doc.root, "resources", i);
        struct ImportCatalogResource *out = &view->resources[i];
        const char *idValue;
        const char *typeName;
        long long schema;
        bool required;

        if (res == NULL
         || !Gen3Toml_GetString(res, "id", &idValue) || !CopyStr(idValue, out->id, sizeof(out->id))
         || Gen3ResourceId_ValidateCanonicalName(out->id) != GEN3_RESOURCE_NAME_VALID
         || !Gen3Toml_GetString(res, "type", &typeName)
         || (out->type = ParseTypeName(typeName)) == GEN3_RESOURCE_TYPE_INVALID
         || !Gen3Toml_GetInteger(res, "schema", &schema) || schema < 1)
        {
            Gen3Toml_Destroy(&doc);
            return SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                            "catalog resource %zu is invalid", i);
        }
        out->schema = (uint32_t)schema;
        if (!Gen3Toml_GetBool(res, "required_for_base", &required))
            required = false;
        out->requiredForBase = required;
    }

    Gen3Toml_Destroy(&doc);
    return EMERALD_IMPORT_OK;
}

/* ------------------------------------------------------------------ */
/* ROM validation                                                      */
/* ------------------------------------------------------------------ */

static enum EmeraldResourceImportError ValidateRomBytes(const uint8_t *rom, size_t romSize,
                                                       const struct EmeraldRomProfile *profile,
                                                       struct EmeraldImportReport *report)
{
    uint8_t sha1Digest[20];
    uint8_t sha256Digest[32];

    if (rom == NULL)
        return SetError(report, EMERALD_IMPORT_ERR_INVALID_ARGUMENT, "ROM bytes missing");
    if (romSize != (size_t)profile->romSize)
    {
        return SetError(report, EMERALD_IMPORT_ERR_ROM_SIZE_MISMATCH,
                        "ROM size %zu does not match profile '%s' (expected %llu)",
                        romSize, profile->name, (unsigned long long)profile->romSize);
    }
    if (memcmp(rom + 0xAC, profile->gameCode, 4u) != 0
     || memcmp(rom + 0xB0, profile->makerCode, 2u) != 0
     || rom[0xBC] != profile->softwareRevision)
    {
        return SetError(report, EMERALD_IMPORT_ERR_ROM_HEADER_IDENTITY,
                        "ROM header identity does not match profile '%s'", profile->name);
    }
    Sha1Bytes(rom, romSize, sha1Digest);
    if (!BytesEqual(sha1Digest, profile->romSha1, 20u))
    {
        return SetError(report, EMERALD_IMPORT_ERR_ROM_SHA1_MISMATCH,
                        "ROM SHA-1 does not match profile '%s'", profile->name);
    }
    Sha256Bytes(rom, romSize, sha256Digest);
    if (!BytesEqual(sha256Digest, profile->romSha256, 32u))
    {
        return SetError(report, EMERALD_IMPORT_ERR_ROM_SHA256_MISMATCH,
                        "ROM SHA-256 does not match profile '%s'", profile->name);
    }
    return EMERALD_IMPORT_OK;
}

static enum EmeraldResourceImportError LoadRom(const struct EmeraldImportInput *input,
                                               struct LoadedBuffer *out,
                                               struct EmeraldImportReport *report)
{
    return LoadSlot(input->romBytes, input->romSize, input->romPath, "ROM file", true,
                    EMERALD_IMPORT_ERR_ROM_READ_FAILED, out, report);
}

/* ------------------------------------------------------------------ */
/* Manifest <-> profile cross-checks                                   */
/* ------------------------------------------------------------------ */

static enum EmeraldResourceImportError CheckQualification(const struct EmeraldRomProfile *profile,
                                                          const struct ImportManifestView *view,
                                                          struct EmeraldImportReport *report)
{
    if (strcmp(view->qualification, "production") == 0)
    {
        if (profile->synthetic)
        {
            return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_BAD_QUALIFICATION,
                            "production-qualified manifest cannot be imported against the synthetic profile");
        }
        return EMERALD_IMPORT_OK;
    }
    if (strcmp(view->qualification, "fixture") == 0)
    {
        if (!profile->synthetic)
        {
            return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_FIXTURE_NOT_ALLOWED,
                            "fixture-qualified manifest rejected for the production profile "
                            "(fixture offsets must never be used against the retail ROM)");
        }
        return EMERALD_IMPORT_OK;
    }
    return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_BAD_QUALIFICATION,
                    "manifest qualification '%s' is not 'fixture' or 'production'",
                    view->qualification);
}

static enum EmeraldResourceImportError CheckManifestProfile(const struct EmeraldRomProfile *profile,
                                                            const struct ImportManifestView *view,
                                                            struct EmeraldImportReport *report)
{
    if (strcmp(view->game, profile->game) != 0 || strcmp(view->romProfile, profile->name) != 0)
    {
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_PROFILE_MISMATCH,
                        "manifest declares game='%s' rom_profile='%s'; profile requires game='%s' rom_profile='%s'",
                        view->game, view->romProfile, profile->game, profile->name);
    }
    if (view->romSize != profile->romSize)
    {
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_ROM_MISMATCH,
                        "manifest rom_size %llu != profile rom_size %llu",
                        (unsigned long long)view->romSize, (unsigned long long)profile->romSize);
    }
    if (!BytesEqual(view->romSha1, profile->romSha1, GEN3_PACK_SHA1_SIZE)
     || !BytesEqual(view->romSha256, profile->romSha256, GEN3_PACK_SHA256_SIZE))
    {
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_ROM_MISMATCH,
                        "manifest ROM digests do not match profile '%s'", profile->name);
    }
    return EMERALD_IMPORT_OK;
}

/* ------------------------------------------------------------------ */
/* R2 build helpers                                                    */
/* ------------------------------------------------------------------ */

static enum Gen3ResourceType CatalogTypeFor(const struct ImportCatalogView *catalog,
                                            const char *id, uint32_t *outSchema,
                                            bool *outRequired)
{
    size_t i;
    for (i = 0; i < catalog->resourceCount; i++)
    {
        if (strcmp(catalog->resources[i].id, id) == 0)
        {
            *outSchema = catalog->resources[i].schema;
            *outRequired = catalog->resources[i].requiredForBase;
            return catalog->resources[i].type;
        }
    }
    return GEN3_RESOURCE_TYPE_INVALID;
}

static bool CatalogAddAll(const struct ImportCatalogView *catalog,
                          struct Gen3ResourceCatalog *out,
                          struct Gen3ResourceDiagnosticList *diag)
{
    size_t i;
    for (i = 0; i < catalog->resourceCount; i++)
    {
        const struct ImportCatalogResource *resource = &catalog->resources[i];
        if (!Gen3ResourceCatalog_Add(out, resource->id, resource->type, resource->schema,
                                     resource->requiredForBase, diag))
        {
            return false;
        }
    }
    return Gen3ResourceCatalog_Finalize(out, diag);
}

/* Strictly extract one manifest record and append it to the R2 build. */
static enum EmeraldResourceImportError ExtractRecord(
    const uint8_t *romData, size_t romDataSize, const struct EmeraldRomProfile *profile,
    const struct ImportManifestView *manifest, size_t recordIndex,
    const struct ImportCatalogView *catalog,
    struct Gen3ResourcePackBuild *build,
    struct Gen3ResourcePackDiagnosticList *diag,
    struct EmeraldImportReport *report)
{
    const struct ImportManifestRecord *record = &manifest->records[recordIndex];
    enum Gen3ResourceType catalogType;
    uint32_t catalogSchema;
    bool catalogRequired;
    uint8_t sliceSha[32];
    uint8_t decodedSha[32];
    uint8_t *slice;
    uint8_t *decoded;
    size_t decodedSize;
    enum Gen3Lz77Result lz;
    struct Gen3ResourcePackEntryInput entry;
    enum Gen3ResourcePackError packError;
    Gen3ResourceKey derived;

    if ((uint64_t)romDataSize != profile->romSize
     || record->romOffset > (uint64_t)romDataSize
     || record->encodedLength > (uint64_t)romDataSize - record->romOffset)
    {
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_ROM_RANGE_OUT_OF_BOUNDS,
                                 "record range [0x%llx, +%llu) exceeds the ROM size 0x%llx",
                                 (unsigned long long)record->romOffset,
                                 (unsigned long long)record->encodedLength,
                                 (unsigned long long)romDataSize);
    }
    if (record->decodedLength > EMERALD_IMPORT_MAX_PAYLOAD_SIZE) /* bounded allocs (§31) */
    {
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_DECODED_SIZE_MISMATCH,
                                 "record decoded_length %llu exceeds the %u-byte cap",
                                 (unsigned long long)record->decodedLength,
                                 (unsigned)EMERALD_IMPORT_MAX_PAYLOAD_SIZE);
    }

    catalogType = CatalogTypeFor(catalog, record->id, &catalogSchema, &catalogRequired);
    if (catalogType == GEN3_RESOURCE_TYPE_INVALID)
    {
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_CATALOG_MISSING_RESOURCE,
                                 "resource is not present in the catalog contract");
    }
    if (catalogType != record->type || catalogSchema != record->schema)
    {
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_MANIFEST_TYPE_SCHEMA_MISMATCH,
                                 "manifest type/schema disagree with the catalog contract");
    }

    slice = (uint8_t *)malloc((size_t)record->encodedLength);
    decoded = (uint8_t *)malloc((size_t)record->decodedLength);
    if (slice == NULL || decoded == NULL)
    {
        free(slice);
        free(decoded);
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_OUT_OF_MEMORY,
                                 "out of memory extracting record");
    }

    memcpy(slice, romData + record->romOffset, (size_t)record->encodedLength);
    Sha256Bytes(slice, (size_t)record->encodedLength, sliceSha);
    if (!BytesEqual(sliceSha, record->sourceEncodedSha256, 32u))
    {
        free(slice);
        free(decoded);
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_ENCODED_HASH_MISMATCH,
                                 "ROM slice digest does not match the manifest's encoded digest");
    }

    if (record->sourceEncoding == GEN3_PACK_SOURCE_ENCODING_RAW)
    {
        /* raw (R8 trainer-back sheets): the ROM artifact IS the canonical
         * decoded payload — no LZ77 header, no decode step. */
        if (record->encodedLength != record->decodedLength)
        {
            free(slice);
            free(decoded);
            return SetErrorForRecord(report, recordIndex, record->id,
                                     EMERALD_IMPORT_ERR_DECODED_SIZE_MISMATCH,
                                     "raw record encoded_length %llu != decoded_length %llu",
                                     (unsigned long long)record->encodedLength,
                                     (unsigned long long)record->decodedLength);
        }
        memcpy(decoded, slice, (size_t)record->encodedLength);
        decodedSize = (size_t)record->encodedLength;
        free(slice);
    }
    else
    {
        lz = Gen3Lz77_Decode(slice, (size_t)record->encodedLength, decoded,
                             (size_t)record->decodedLength, &decodedSize);
        free(slice);
        if (lz != GEN3_LZ77_OK)
        {
            free(decoded);
            return SetErrorForRecord(report, recordIndex, record->id,
                                     EMERALD_IMPORT_ERR_LZ77_DECODE_FAILED,
                                     "strict LZ77 decode rejected the encoded payload (%d)", (int)lz);
        }
        if (decodedSize != (size_t)record->decodedLength)
        {
            free(decoded);
            return SetErrorForRecord(report, recordIndex, record->id,
                                     EMERALD_IMPORT_ERR_DECODED_SIZE_MISMATCH,
                                     "decoded %zu bytes but manifest declares %llu",
                                     decodedSize, (unsigned long long)record->decodedLength);
        }
    }

    /* §24: the manifest's declared decoded_length is the authoritative payload
     * contract for R3 (sheet 2048 / palette 32). Catalog-owned size constraints
     * are documented as a later extension; the tile-graphics schema is not
     * redefined here. */
    Sha256Bytes(decoded, decodedSize, decodedSha);
    if (!BytesEqual(decodedSha, record->canonicalDecodedSha256, 32u))
    {
        free(decoded);
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_CANONICAL_HASH_MISMATCH,
                                 "decoded payload digest does not match the manifest's canonical digest");
    }

    Gen3ResourceId_DeriveKey(record->id, &derived);
    if (!BytesEqual(derived.bytes, record->key, GEN3_RESOURCE_KEY_SIZE))
    {
        free(decoded);
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_KEY_MISMATCH,
                                 "manifest key does not match the canonical-name-derived key");
    }

    memset(&entry, 0, sizeof(entry));
    entry.canonicalName = record->id;
    entry.key = &derived;
    entry.type = record->type;
    entry.schema = record->schema;
    entry.flags = catalogRequired ? GEN3_PACK_FLAG_REQUIRED_FOR_BASE : 0u;
    entry.representation = GEN3_PACK_REPRESENTATION_DECODED;
    entry.sourceEncoding = record->sourceEncoding;
    entry.canonicalPayload = decoded;
    entry.canonicalPayloadSize = decodedSize;
    entry.canonicalPayloadSha256 = record->canonicalDecodedSha256;
    entry.sourceRomOffset = record->romOffset;
    entry.sourceEncodedSize = record->encodedLength;
    entry.sourceEncodedSha256 = record->sourceEncodedSha256;

    packError = Gen3ResourcePackBuild_AddEntry(build, &entry, diag);
    free(decoded);
    if (packError != GEN3_PACK_OK)
    {
        return SetErrorForRecord(report, recordIndex, record->id,
                                 EMERALD_IMPORT_ERR_PACK_BUILD_FAILED,
                                 "R2 writer rejected the entry (%s)",
                                 Gen3ResourcePackError_Describe(packError));
    }
    return EMERALD_IMPORT_OK;
}

/* Resolve the source slot for manifest/catalog `index`. When the legacy
 * single-slot fields are used (count == 0) they alias slot 0. */
static struct EmeraldImportSource ManifestSourceAt(const struct EmeraldImportInput *input,
                                                   size_t index)
{
    struct EmeraldImportSource slot;
    if (input->manifestCount > 0u)
        return input->manifests[index];
    slot.bytes = input->manifestBytes;
    slot.size = input->manifestSize;
    slot.path = input->manifestPath;
    return slot;
}

static struct EmeraldImportSource CatalogSourceAt(const struct EmeraldImportInput *input,
                                                  size_t index)
{
    struct EmeraldImportSource slot;
    if (input->catalogCount > 0u)
        return input->catalogs[index];
    slot.bytes = input->catalogBytes;
    slot.size = input->catalogSize;
    slot.path = input->catalogPath;
    return slot;
}

/* Append one parsed manifest's records to the merged view (R9 Stage 4). The
 * first manifest seeds the merged view wholesale (its header fields carry the
 * pack profile version); every later manifest must agree on manifest_version,
 * its records must be unique across the whole family (not just within one
 * file), and the merged total must stay within EMERALD_IMPORT_MAX_RECORDS. */
static enum EmeraldResourceImportError MergeManifest(struct ImportManifestView *merged,
                                                     const struct ImportManifestView *more,
                                                     struct EmeraldImportReport *report)
{
    size_t i;
    size_t j;

    if (merged->recordCount == 0u)
    {
        memcpy(merged, more, sizeof(*merged));
        return EMERALD_IMPORT_OK;
    }
    if (more->manifestVersion != merged->manifestVersion)
    {
        return SetError(report, EMERALD_IMPORT_ERR_MANIFESTS_DISAGREE,
                        "manifests disagree on manifest_version (%lld vs %lld)",
                        more->manifestVersion, merged->manifestVersion);
    }
    if (more->recordCount > EMERALD_IMPORT_MAX_RECORDS - merged->recordCount)
    {
        return SetError(report, EMERALD_IMPORT_ERR_MANIFEST_TOO_MANY_RECORDS,
                        "merged manifests declare %zu records (limit %u)",
                        more->recordCount + merged->recordCount,
                        (unsigned)EMERALD_IMPORT_MAX_RECORDS);
    }
    for (i = 0; i < more->recordCount; i++)
    {
        for (j = 0; j < merged->recordCount; j++)
        {
            if (strcmp(more->records[i].id, merged->records[j].id) == 0)
            {
                return SetErrorForRecord(report, merged->recordCount + i,
                                         more->records[i].id,
                                         EMERALD_IMPORT_ERR_MANIFEST_DUPLICATE_RECORD,
                                         "duplicate record id across merged manifests");
            }
        }
    }
    memcpy(&merged->records[merged->recordCount], more->records,
           more->recordCount * sizeof(merged->records[0]));
    merged->recordCount += more->recordCount;
    return EMERALD_IMPORT_OK;
}

/* Append one parsed catalog's resources to the merged view (R9 Stage 4).
 * Cross-catalog duplicate ids are rejected downstream by
 * Gen3ResourceCatalog_Add (DUPLICATE_CATALOG_NAME), the same fail-closed path
 * as in-catalog duplicates. */
static enum EmeraldResourceImportError MergeCatalog(struct ImportCatalogView *merged,
                                                    const struct ImportCatalogView *more,
                                                    struct EmeraldImportReport *report)
{
    if (merged->resourceCount == 0u)
    {
        memcpy(merged, more, sizeof(*merged));
        return EMERALD_IMPORT_OK;
    }
    if (more->catalogVersion != merged->catalogVersion
     || more->resourceApiMajor != merged->resourceApiMajor)
    {
        return SetError(report, EMERALD_IMPORT_ERR_CATALOGS_DISAGREE,
                        "catalogs disagree on catalog_version/resource_api (%lld/%lld vs %lld/%lld)",
                        more->catalogVersion, more->resourceApiMajor,
                        merged->catalogVersion, merged->resourceApiMajor);
    }
    if (more->resourceCount > EMERALD_IMPORT_MAX_RECORDS - merged->resourceCount)
    {
        return SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                        "merged catalogs declare %zu resources (limit %u)",
                        more->resourceCount + merged->resourceCount,
                        (unsigned)EMERALD_IMPORT_MAX_RECORDS);
    }
    memcpy(&merged->resources[merged->resourceCount], more->resources,
           more->resourceCount * sizeof(merged->resources[0]));
    merged->resourceCount += more->resourceCount;
    return EMERALD_IMPORT_OK;
}

/* Full BuildPack core shared by EmeraldImport_BuildPack and Install. */
static enum EmeraldResourceImportError BuildPackCore(const struct EmeraldImportInput *input,
                                                     struct Gen3ResourcePackBytes *outBytes,
                                                     struct EmeraldImportReport *report)
{
    struct LoadedBuffer rom;
    struct LoadedBuffer manifest;
    struct LoadedBuffer catalog;
    struct ImportManifestView *mergedManifest;
    struct ImportManifestView *parseManifest;
    struct ImportCatalogView *mergedCatalog;
    struct ImportCatalogView *parseCatalog;
    struct Gen3ResourceCatalog *catalogObject;
    struct Gen3ResourcePackBuild *build;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourceDiagnosticList catalogDiag;
    struct Gen3ResourcePackProfileInput profile;
    struct Gen3ResourcePack *parsedPack;
    struct EmeraldImportReport localReport;
    struct Gen3Sha256Context manifestHasher;
    struct Gen3Sha256Context catalogHasher;
    uint8_t catalogSha[32];
    uint8_t manifestSha[32];
    enum EmeraldResourceImportError err;
    size_t manifestCount;
    size_t catalogCount;
    size_t i;

    memset(&rom, 0, sizeof(rom));
    memset(&manifest, 0, sizeof(manifest));
    memset(&catalog, 0, sizeof(catalog));
    catalogObject = NULL;
    parsedPack = NULL;
    build = NULL;
    mergedManifest = NULL;
    parseManifest = NULL;
    mergedCatalog = NULL;
    parseCatalog = NULL;
    outBytes->data = NULL;
    outBytes->size = 0u;

    if (report == NULL)
        report = &localReport;

    /* The view structs hold EMERALD_IMPORT_MAX_RECORDS inline records each
     * (~0.8 MiB) — too large for the stack, so the merged and per-file parse
     * views are heap-allocated. */
    mergedManifest = (struct ImportManifestView *)calloc(1u, sizeof(*mergedManifest));
    parseManifest = (struct ImportManifestView *)calloc(1u, sizeof(*parseManifest));
    mergedCatalog = (struct ImportCatalogView *)calloc(1u, sizeof(*mergedCatalog));
    parseCatalog = (struct ImportCatalogView *)calloc(1u, sizeof(*parseCatalog));
    if (mergedManifest == NULL || parseManifest == NULL
     || mergedCatalog == NULL || parseCatalog == NULL)
    {
        err = SetError(report, EMERALD_IMPORT_ERR_OUT_OF_MEMORY, "out of memory");
        goto done;
    }

    manifestCount = input->manifestCount > 0u ? input->manifestCount : 1u;
    catalogCount = input->catalogCount > 0u ? input->catalogCount : 1u;

    /* Phase A: every manifest, before the (expensive, 16 MiB) ROM hashing so a
     * disallowed manifest is rejected without touching the ROM. Qualification +
     * profile identity are cheap text checks per manifest (fail-closed: a
     * fixture manifest can never ride in beside a production one), then the
     * records are merged with cross-manifest duplicate rejection. The pack
     * profile's provenance digest covers all manifest files in input order. */
    Gen3Sha256_Init(&manifestHasher);
    for (i = 0; i < manifestCount; i++)
    {
        struct EmeraldImportSource slot = ManifestSourceAt(input, i);
        LoadedBuffer_Release(&manifest);
        err = LoadSlot(slot.bytes, slot.size, slot.path,
                       "extraction manifest", false, EMERALD_IMPORT_ERR_MANIFEST_READ_FAILED,
                       &manifest, report);
        if (err != EMERALD_IMPORT_OK)
            goto done;
        Gen3Sha256_Update(&manifestHasher, manifest.data, manifest.size);
        err = ParseManifest(manifest.data, manifest.size, parseManifest, report);
        if (err != EMERALD_IMPORT_OK)
            goto done;
        err = CheckQualification(input->profile, parseManifest, report);
        if (err != EMERALD_IMPORT_OK)
            goto done;
        err = CheckManifestProfile(input->profile, parseManifest, report);
        if (err != EMERALD_IMPORT_OK)
            goto done;
        err = MergeManifest(mergedManifest, parseManifest, report);
        if (err != EMERALD_IMPORT_OK)
            goto done;
    }
    Gen3Sha256_Final(&manifestHasher, manifestSha);
    LoadedBuffer_Release(&manifest);

    err = LoadRom(input, &rom, report);
    if (err != EMERALD_IMPORT_OK)
        goto done;
    err = ValidateRomBytes(rom.data, rom.size, input->profile, report);
    if (err != EMERALD_IMPORT_OK)
        goto done;

    /* Phase B: every catalog contract, each read, parsed, and profile-checked
     * independently, then merged. Duplicate ids across the merged catalogs are
     * rejected when the catalog object is built below. */
    Gen3Sha256_Init(&catalogHasher);
    for (i = 0; i < catalogCount; i++)
    {
        struct EmeraldImportSource slot = CatalogSourceAt(input, i);
        LoadedBuffer_Release(&catalog);
        err = LoadSlot(slot.bytes, slot.size, slot.path,
                       "catalog contract", false, EMERALD_IMPORT_ERR_CATALOG_READ_FAILED,
                       &catalog, report);
        if (err != EMERALD_IMPORT_OK)
            goto done;
        Gen3Sha256_Update(&catalogHasher, catalog.data, catalog.size);
        err = ParseCatalog(catalog.data, catalog.size, parseCatalog, report);
        if (err != EMERALD_IMPORT_OK)
            goto done;
        if (strcmp(parseCatalog->game, input->profile->game) != 0
         || strcmp(parseCatalog->romProfile, input->profile->name) != 0)
        {
            err = SetError(report, EMERALD_IMPORT_ERR_CATALOG_PROFILE_MISMATCH,
                           "catalog declares game='%s' rom_profile='%s'; profile requires game='%s' rom_profile='%s'",
                           parseCatalog->game, parseCatalog->romProfile,
                           input->profile->game, input->profile->name);
            goto done;
        }
        err = MergeCatalog(mergedCatalog, parseCatalog, report);
        if (err != EMERALD_IMPORT_OK)
            goto done;
    }
    Gen3Sha256_Final(&catalogHasher, catalogSha);
    LoadedBuffer_Release(&catalog);

    catalogObject = Gen3ResourceCatalog_Create();
    build = Gen3ResourcePackBuild_Create();
    if (catalogObject == NULL || build == NULL)
    {
        err = SetError(report, EMERALD_IMPORT_ERR_OUT_OF_MEMORY, "out of memory");
        goto done;
    }
    Gen3ResourceDiagnostics_Init(&catalogDiag);
    if (!CatalogAddAll(mergedCatalog, catalogObject, &catalogDiag))
    {
        err = SetError(report, EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
                       "catalog contract failed to build (%s)",
                       catalogDiag.count > 0u
                           ? Gen3ResourceReason_Describe(catalogDiag.items[catalogDiag.count - 1u].reason)
                           : "unknown");
        Gen3ResourceDiagnostics_Destroy(&catalogDiag);
        goto done;
    }
    Gen3ResourceDiagnostics_Destroy(&catalogDiag);

    Gen3ResourcePackDiagnostics_Init(&diag);

    memset(&profile, 0, sizeof(profile));
    profile.basePackVersion = GEN3_PACK_FORMAT_VERSION_1;
    profile.catalogVersion = (uint32_t)mergedCatalog->catalogVersion;
    profile.extractionManifestVersion = (uint32_t)mergedManifest->manifestVersion;
    profile.canonicalRepresentationVersion = (uint32_t)mergedCatalog->resourceApiMajor;
    profile.sourceRomSize = input->profile->romSize;
    profile.sourceRomSha1 = input->profile->romSha1;
    profile.sourceRomSha256 = input->profile->romSha256;
    memcpy(profile.gameCode, input->profile->gameCode, 4u);
    memcpy(profile.makerCode, input->profile->makerCode, 2u);
    profile.softwareRevision = input->profile->softwareRevision;
    memset(profile.gameId, 0, sizeof(profile.gameId));
    memcpy(profile.gameId, input->profile->game, strlen(input->profile->game) + 1u);
    /* With multiple manifests/catalogs the profile digests cover every input
     * file in input order; single-input callers hash the one file, exactly as
     * before R9. */
    profile.catalogSha256 = catalogSha;
    profile.extractionManifestSha256 = manifestSha;

    if (Gen3ResourcePackBuild_SetProfile(build, &profile, &diag) != GEN3_PACK_OK)
    {
        err = SetError(report, EMERALD_IMPORT_ERR_PACK_BUILD_FAILED,
                       "R2 writer rejected the pack profile (%s)",
                       diag.count > 0u ? Gen3ResourcePackError_Describe(diag.items[diag.count - 1u].error)
                                       : "unknown");
        goto diag_done;
    }

    for (i = 0; i < mergedManifest->recordCount; i++)
    {
        err = ExtractRecord(rom.data, rom.size, input->profile, mergedManifest, i,
                            mergedCatalog, build, &diag, report);
        if (err != EMERALD_IMPORT_OK)
            goto diag_done;
    }

    if (Gen3ResourcePackWriter_Write(build, outBytes, &diag) != GEN3_PACK_OK)
    {
        err = SetError(report, EMERALD_IMPORT_ERR_PACK_WRITE_FAILED,
                       "R2 writer failed to serialize the pack (%s)",
                       diag.count > 0u ? Gen3ResourcePackError_Describe(diag.items[diag.count - 1u].error)
                                       : "unknown");
        goto diag_done;
    }

    /* Reopen the freshly written pack and validate every entry against the
     * catalog contract end-to-end before it is trusted. */
    if (Gen3ResourcePack_Parse(outBytes->data, outBytes->size, &parsedPack, &diag) != GEN3_PACK_OK)
    {
        err = SetError(report, EMERALD_IMPORT_ERR_PACK_WRITE_FAILED,
                       "freshly written pack failed self-parse (%s)",
                       diag.count > 0u ? Gen3ResourcePackError_Describe(diag.items[diag.count - 1u].error)
                                       : "unknown");
        goto diag_done;
    }
    if (Gen3ResourcePack_ValidateCatalog(parsedPack, catalogObject, &diag) != GEN3_PACK_OK)
    {
        err = SetError(report, EMERALD_IMPORT_ERR_PACK_BUILD_FAILED,
                       "freshly written pack failed catalog validation (%s)",
                       diag.count > 0u ? Gen3ResourcePackError_Describe(diag.items[diag.count - 1u].error)
                                       : "unknown");
        goto diag_done;
    }

    err = EMERALD_IMPORT_OK;

diag_done:
    Gen3ResourcePackDiagnostics_Destroy(&diag);
done:
    Gen3ResourcePack_Destroy(parsedPack);
    Gen3ResourcePackBuild_Destroy(build);
    Gen3ResourceCatalog_Destroy(catalogObject);
    free(mergedManifest);
    free(parseManifest);
    free(mergedCatalog);
    free(parseCatalog);
    LoadedBuffer_Release(&rom);
    LoadedBuffer_Release(&manifest);
    LoadedBuffer_Release(&catalog);
    return err;
}

/* ------------------------------------------------------------------ */
/* Public phases                                                       */
/* ------------------------------------------------------------------ */

enum EmeraldResourceImportError EmeraldImport_ValidateRom(
    const struct EmeraldImportInput *input, struct EmeraldImportReport *report)
{
    struct LoadedBuffer rom;
    enum EmeraldResourceImportError err;

    if (input == NULL || input->profile == NULL)
        return SetError(report, EMERALD_IMPORT_ERR_INVALID_ARGUMENT,
                        "input and profile are required");
    memset(&rom, 0, sizeof(rom));
    err = LoadRom(input, &rom, report);
    if (err == EMERALD_IMPORT_OK)
        err = ValidateRomBytes(rom.data, rom.size, input->profile, report);
    LoadedBuffer_Release(&rom);
    return err;
}

enum EmeraldResourceImportError EmeraldImport_BuildPack(
    const struct EmeraldImportInput *input, struct Gen3ResourcePackBytes *outBytes,
    struct EmeraldImportReport *report)
{
    if (input == NULL || input->profile == NULL || outBytes == NULL)
        return SetError(report, EMERALD_IMPORT_ERR_INVALID_ARGUMENT,
                        "input, profile and outBytes are required");
    return BuildPackCore(input, outBytes, report);
}

/* ------------------------------------------------------------------ */
/* Portability seam for atomic install (R3 §12)                        */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

/* Windows seam: functional where cheap, stubbed where the Linux test env has
 * no counterpart. Not exercised by the Linux test suite. */

static bool PlatformFlush(int fd)
{
    return _commit(fd) == 0;
}

static bool PlatformRename(const char *from, const char *to)
{
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
}

/* Lock is a documented no-op seam on Windows for R3. */
static bool PlatformAcquireLock(const char *dir, int *outFd)
{
    (void)dir;
    *outFd = -1;
    return true;
}

static void PlatformReleaseLock(int fd)
{
    (void)fd;
}

static void PlatformCleanupTemps(const char *dir, const char *prefix)
{
    (void)dir;
    (void)prefix;
}

#else /* POSIX */

static bool PlatformFlush(int fd)
{
    return fsync(fd) == 0;
}

static bool PlatformRename(const char *from, const char *to)
{
    return rename(from, to) == 0;
}

static bool PlatformAcquireLock(const char *dir, int *outFd)
{
    char path[4096];
    int fd;
    if (snprintf(path, sizeof(path), "%s/%s", dir, EMERALD_IMPORT_LOCK_NAME) >= (int)sizeof(path))
        return false;
    fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        close(fd);
        return false;
    }
    *outFd = fd;
    return true;
}

static void PlatformReleaseLock(int fd)
{
    if (fd >= 0)
    {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

/* Removes leftover interrupted temp files "<prefix>*" in `dir`. */
static void PlatformCleanupTemps(const char *dir, const char *prefix)
{
    DIR *stream;
    struct dirent *entry;
    size_t prefixLength = strlen(prefix);

    stream = opendir(dir);
    if (stream == NULL)
        return;
    while ((entry = readdir(stream)) != NULL)
    {
        char path[4096];
        size_t nameLength = strlen(entry->d_name);
        /* Only temp siblings are removed: the name must start with the
         * destination base AND be strictly longer than it. The installed pack
         * itself has exactly the base name and must survive cleanup until the
         * atomic rename (R3 §16: a failed reimport preserves the valid pack). */
        if (nameLength <= prefixLength)
            continue;
        if (strncmp(entry->d_name, prefix, prefixLength) != 0)
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) < (int)sizeof(path))
            unlink(path);
    }
    closedir(stream);
}

#endif

/* Splits a destination path into directory and base name. Returns false when
 * the path is unusable. */
static bool SplitDestination(const char *destination, char *dir, size_t dirSize,
                             char *base, size_t baseSize)
{
    const char *slash;
    size_t dirLength;

    if (destination == NULL || destination[0] == '\0')
        return false;
    slash = strrchr(destination, '/');
#if defined(_WIN32)
    if (slash == NULL)
        slash = strrchr(destination, '\\');
#endif
    if (slash == NULL)
    {
        if (strlen(destination) >= baseSize)
            return false;
        memcpy(base, destination, strlen(destination) + 1u);
        if (dirSize > 0u)
        {
            dir[0] = '.';
            dir[1] = '\0';
        }
        return true;
    }
    dirLength = (size_t)(slash - destination);
    if (dirLength == 0u)
        dirLength = 1u; /* "/dest" -> "/" */
    if (dirLength >= dirSize)
        return false;
    memcpy(dir, destination, dirLength);
    dir[dirLength] = '\0';
    if (strlen(slash + 1) >= baseSize)
        return false;
    memcpy(base, slash + 1, strlen(slash + 1) + 1u);
    return true;
}

static bool DirectoryExists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* MinGW's io.h declares mkdir() with one argument; POSIX takes a mode. */
static int MkdirPath(const char *path)
{
#if defined(_WIN32)
    return mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static bool FileExists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

enum EmeraldResourceImportError EmeraldImport_Install(
    const struct EmeraldImportInput *input, struct EmeraldImportReport *report)
{
    struct Gen3ResourcePackBytes bytes;
    char destDir[4096];
    char destBase[256];
    char tempTemplate[4096];
    char tempPath[4096];
    struct EmeraldInstalledInfo existing;
    enum EmeraldResourceImportError err;
    int fd;
    int lockFd = -1;
    struct Gen3ResourcePack *pack;
    struct Gen3ResourcePackDiagnosticList diag;
    size_t written;

    if (input == NULL || input->profile == NULL || input->destinationPath == NULL
     || input->destinationPath[0] == '\0')
    {
        return SetError(report, EMERALD_IMPORT_ERR_INVALID_ARGUMENT,
                        "input, profile and destinationPath are required for Install");
    }
    if (!SplitDestination(input->destinationPath, destDir, sizeof(destDir),
                          destBase, sizeof(destBase)))
    {
        return SetError(report, EMERALD_IMPORT_ERR_INVALID_ARGUMENT,
                        "destination path is unusable");
    }

    /* Inspect an existing installed pack first (R3 §15/§16). A newer-format
     * or wrong-profile pack is refused; valid/older/corrupt packs are replaced
     * atomically, so a failed reimport always preserves the old valid pack. */
    if (FileExists(input->destinationPath))
    {
        err = EmeraldImport_ValidateInstalled(input->destinationPath, input->profile,
                                              &existing, report);
        if (err != EMERALD_IMPORT_OK)
            return err;
        if (existing.status == EMERALD_INSTALLED_NEWER)
            return SetError(report, EMERALD_IMPORT_ERR_INSTALLED_NEWER,
                            "refusing to replace an installed pack written by a newer engine");
        if (existing.status == EMERALD_INSTALLED_WRONG_PROFILE)
            return SetError(report, EMERALD_IMPORT_ERR_INSTALLED_WRONG_PROFILE,
                            "refusing to replace an installed pack for a different ROM profile");
    }

    bytes.data = NULL;
    bytes.size = 0u;
    err = BuildPackCore(input, &bytes, report);
    if (err != EMERALD_IMPORT_OK)
        return err;

    if (!DirectoryExists(destDir))
    {
        /* mkdir -p for the destination directory. */
        char dir[4096];
        char *cursor;
        if (strlen(destDir) >= sizeof(dir))
        {
            err = EMERALD_IMPORT_ERR_DEST_CREATE_DIR_FAILED;
            goto release_bytes;
        }
        memcpy(dir, destDir, strlen(destDir) + 1u);
        cursor = dir + (dir[0] == '/' ? 1 : 0);
        for (; *cursor != '\0'; cursor++)
        {
            if (*cursor == '/')
            {
                *cursor = '\0';
                if (dir[0] != '\0' && !DirectoryExists(dir) && MkdirPath(dir) != 0 && errno != EEXIST)
                {
                    err = SetError(report, EMERALD_IMPORT_ERR_DEST_CREATE_DIR_FAILED,
                                   "cannot create destination directory (errno %d)", errno);
                    goto release_bytes;
                }
                *cursor = '/';
            }
        }
        if (!DirectoryExists(dir) && MkdirPath(dir) != 0 && errno != EEXIST)
        {
            err = SetError(report, EMERALD_IMPORT_ERR_DEST_CREATE_DIR_FAILED,
                           "cannot create destination directory (errno %d)", errno);
            goto release_bytes;
        }
    }
    if (!DirectoryExists(destDir))
    {
        err = SetError(report, EMERALD_IMPORT_ERR_DEST_NOT_DIRECTORY,
                       "destination directory is not a directory");
        goto release_bytes;
    }

    if (!PlatformAcquireLock(destDir, &lockFd))
    {
        err = SetError(report, EMERALD_IMPORT_ERR_LOCK_FAILED,
                       "cannot acquire the import lock (another import in progress?)");
        goto release_bytes;
    }

    /* R3 §14: interrupted temp files from a prior import are removed first. */
    PlatformCleanupTemps(destDir, destBase);

    if (snprintf(tempTemplate, sizeof(tempTemplate), "%s.tmp.XXXXXX",
                 input->destinationPath) >= (int)sizeof(tempTemplate))
    {
        err = SetError(report, EMERALD_IMPORT_ERR_TEMP_OPEN_FAILED,
                       "destination path too long for a temp file");
        goto release_lock;
    }
    fd = mkstemp(tempTemplate);
    if (fd < 0)
    {
        err = SetError(report, EMERALD_IMPORT_ERR_TEMP_OPEN_FAILED,
                       "cannot create the temp pack (errno %d)", errno);
        goto release_lock;
    }
    memcpy(tempPath, tempTemplate, strlen(tempTemplate) + 1u);

    written = 0u;
    while (written < bytes.size)
    {
        size_t chunk = bytes.size - written;
        ssize_t n = write(fd, bytes.data + written, chunk);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tempPath);
            err = SetError(report, EMERALD_IMPORT_ERR_TEMP_WRITE_FAILED,
                           "temp pack write failed (errno %d)", errno);
            goto release_lock;
        }
        written += (size_t)n;
    }
    if (!PlatformFlush(fd))
    {
        close(fd);
        unlink(tempPath);
        err = SetError(report, EMERALD_IMPORT_ERR_TEMP_WRITE_FAILED,
                       "temp pack flush failed (errno %d)", errno);
        goto release_lock;
    }
    close(fd);

    /* Reopen the temp through the normal reader before it is trusted (step 6
     * of the atomic install flow). */
    Gen3ResourcePackDiagnostics_Init(&diag);
    pack = NULL;
    if (Gen3ResourcePack_OpenFile(tempPath, &pack, &diag) != GEN3_PACK_OK)
    {
        Gen3ResourcePackDiagnostics_Destroy(&diag);
        unlink(tempPath);
        err = SetError(report, EMERALD_IMPORT_ERR_TEMP_REOPEN_FAILED,
                       "temp pack failed reopen validation");
        goto release_lock;
    }
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&diag);

    if (!PlatformRename(tempPath, input->destinationPath))
    {
        unlink(tempPath);
        err = SetError(report, EMERALD_IMPORT_ERR_RENAME_FAILED,
                       "atomic rename into place failed (errno %d)", errno);
        goto release_lock;
    }

    /* Reopen and verify the installed file (step 9 of the atomic install flow). */
    Gen3ResourcePackDiagnostics_Init(&diag);
    pack = NULL;
    if (Gen3ResourcePack_OpenFile(input->destinationPath, &pack, &diag) != GEN3_PACK_OK)
    {
        Gen3ResourcePackDiagnostics_Destroy(&diag);
        err = SetError(report, EMERALD_IMPORT_ERR_FINAL_VERIFY_FAILED,
                       "installed pack failed reopen verification");
        goto release_lock;
    }
    Gen3ResourcePack_Destroy(pack);
    Gen3ResourcePackDiagnostics_Destroy(&diag);

    PlatformReleaseLock(lockFd);
    Gen3ResourcePackBytes_Destroy(&bytes);
    return EMERALD_IMPORT_OK;

release_lock:
    PlatformReleaseLock(lockFd);
release_bytes:
    Gen3ResourcePackBytes_Destroy(&bytes);
    return err;
}

static bool ProfileMatchesPack(const struct EmeraldRomProfile *profile,
                               const struct Gen3ResourcePackProfile *packProfile)
{
    if (profile->romSize != packProfile->sourceRomSize)
        return false;
    if (!BytesEqual(packProfile->sourceRomSha1, profile->romSha1, GEN3_PACK_SHA1_SIZE))
        return false;
    if (!BytesEqual(packProfile->sourceRomSha256, profile->romSha256, GEN3_PACK_SHA256_SIZE))
        return false;
    if (strncmp((const char *)packProfile->gameId, profile->game, strlen(profile->game)) != 0)
        return false;
    return true;
}

enum EmeraldResourceImportError EmeraldImport_ValidateInstalled(
    const char *packPath, const struct EmeraldRomProfile *profile,
    struct EmeraldInstalledInfo *info, struct EmeraldImportReport *report)
{
    struct Gen3ResourcePack *pack;
    struct Gen3ResourcePackDiagnosticList diag;
    struct Gen3ResourcePackProfile packProfile;
    uint8_t digest[GEN3_PACK_SHA256_SIZE];
    struct stat st;

    if (packPath == NULL || info == NULL)
        return SetError(report, EMERALD_IMPORT_ERR_INVALID_ARGUMENT,
                        "packPath and info are required");
    memset(info, 0, sizeof(*info));

    if (stat(packPath, &st) != 0)
    {
        info->status = EMERALD_INSTALLED_OPEN_FAILED;
        return EMERALD_IMPORT_OK;
    }

    Gen3ResourcePackDiagnostics_Init(&diag);
    pack = NULL;
    if (Gen3ResourcePack_OpenFile(packPath, &pack, &diag) != GEN3_PACK_OK)
    {
        Gen3ResourcePackDiagnostics_Destroy(&diag);
        info->status = EMERALD_INSTALLED_CORRUPT;
        return EMERALD_IMPORT_OK;
    }
    Gen3ResourcePackDiagnostics_Destroy(&diag);

    Gen3ResourcePack_GetProfile(pack, &packProfile);
    info->basePackVersion = packProfile.basePackVersion;
    info->entryCount = (uint32_t)Gen3ResourcePack_GetEntryCount(pack);
    memcpy(info->gameId, packProfile.gameId, GEN3_PACK_GAME_ID_SIZE);
    memcpy(info->romSha1, packProfile.sourceRomSha1, GEN3_PACK_SHA1_SIZE);
    memcpy(info->romSha256, packProfile.sourceRomSha256, GEN3_PACK_SHA256_SIZE);
    if (Gen3ResourcePack_GetLogicalDigest(pack, digest))
    {
        memcpy(info->logicalDigest, digest, GEN3_PACK_SHA256_SIZE);
        info->hasLogicalDigest = true;
    }

    if (packProfile.basePackVersion < GEN3_PACK_FORMAT_VERSION_1)
        info->status = EMERALD_INSTALLED_OLDER;
    else if (packProfile.basePackVersion > GEN3_PACK_FORMAT_VERSION_1)
        info->status = EMERALD_INSTALLED_NEWER;
    else if (profile != NULL && !ProfileMatchesPack(profile, &packProfile))
        info->status = EMERALD_INSTALLED_WRONG_PROFILE;
    else
        info->status = EMERALD_INSTALLED_VALID;

    Gen3ResourcePack_Destroy(pack);
    return EMERALD_IMPORT_OK;
}
