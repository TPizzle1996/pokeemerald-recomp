#ifndef EMERALD_RESOURCE_IMPORT_H
#define EMERALD_RESOURCE_IMPORT_H

/* Stage R3 local Emerald ROM import API.
 *
 * Four callable phases (no GUI, no runtime integration):
 *   ValidateRom        - strict ROM validation against one profile.
 *   BuildPack          - validate manifest + catalog, strictly extract and
 *                        decode the manifest ranges, build the deterministic
 *                        v1 .rpack in memory.
 *   Install            - BuildPack plus atomic installation to a destination
 *                        path under an import lock.
 *   ValidateInstalled  - classify an existing installed pack
 *                        (valid / corrupt / older / newer / wrong-profile).
 *
 * Everything is a pure library call: the caller supplies ROM bytes or a path,
 * the R1A extraction manifest (bytes or path), the R2 catalog contract (bytes
 * or path), the destination path, and the profile to validate against. No
 * ROM path is ever persisted by this module; reports are transient.
 *
 * Extraction is strict (R3 §7): for every manifest record the ROM slice is
 * read, hashed and matched to the manifest's encoded digest, LZ77-decoded
 * with the strict decoder, the decoded length matched to the manifest's
 * declared length, and the decoded bytes hashed and matched to the manifest's
 * canonical digest before the payload is accepted. Nothing is ever guessed;
 * a manifest whose offsets/hashes belong to the synthetic fixture is rejected
 * for the production profile (and vice versa).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "emerald/resources/emerald_rom_profile.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_pack.h"
#include "gen3/resources/resource_pack_writer.h"

/* ------------------------------------------------------------------ */
/* Structured error model (R3 §18: stable, >= 27 codes)                */
/* ------------------------------------------------------------------ */

enum EmeraldResourceImportError
{
    EMERALD_IMPORT_OK = 0,

    /* generic */
    EMERALD_IMPORT_ERR_INVALID_ARGUMENT,
    EMERALD_IMPORT_ERR_OUT_OF_MEMORY,

    /* ROM acquisition + identity */
    EMERALD_IMPORT_ERR_ROM_READ_FAILED,
    EMERALD_IMPORT_ERR_ROM_NOT_REGULAR_FILE,
    EMERALD_IMPORT_ERR_ROM_SIZE_MISMATCH,
    EMERALD_IMPORT_ERR_ROM_HEADER_IDENTITY,   /* game code / maker / revision */
    EMERALD_IMPORT_ERR_ROM_SHA1_MISMATCH,
    EMERALD_IMPORT_ERR_ROM_SHA256_MISMATCH,

    /* manifest parsing + profile qualification */
    EMERALD_IMPORT_ERR_MANIFEST_READ_FAILED,
    EMERALD_IMPORT_ERR_ARTIFACT_READ_FAILED,     /* R13-C bundle artifact file */
    EMERALD_IMPORT_ERR_MANIFEST_PARSE_FAILED,
    EMERALD_IMPORT_ERR_MANIFEST_MISSING_FIELD,
    EMERALD_IMPORT_ERR_MANIFEST_BAD_QUALIFICATION,
    EMERALD_IMPORT_ERR_MANIFEST_FIXTURE_NOT_ALLOWED,
    EMERALD_IMPORT_ERR_MANIFEST_PROFILE_MISMATCH,  /* game/rom_profile fields */
    EMERALD_IMPORT_ERR_MANIFEST_ROM_MISMATCH,      /* size/sha1/sha256 vs profile */

    /* catalog contract */
    EMERALD_IMPORT_ERR_CATALOG_READ_FAILED,
    EMERALD_IMPORT_ERR_CATALOG_PARSE_FAILED,
    EMERALD_IMPORT_ERR_CATALOG_PROFILE_MISMATCH,   /* game/rom_profile fields */
    EMERALD_IMPORT_ERR_CATALOG_MISSING_RESOURCE,   /* record not in catalog */

    /* record-level validation + strict extraction */
    EMERALD_IMPORT_ERR_MANIFEST_NO_RECORDS,
    EMERALD_IMPORT_ERR_MANIFEST_TOO_MANY_RECORDS,
    EMERALD_IMPORT_ERR_MANIFEST_DUPLICATE_RECORD,
    EMERALD_IMPORT_ERR_MANIFEST_BAD_RECORD_FIELD,
    EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_TYPE,
    EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_SCHEMA,
    EMERALD_IMPORT_ERR_MANIFEST_UNSUPPORTED_ENCODING,
    EMERALD_IMPORT_ERR_MANIFEST_TYPE_SCHEMA_MISMATCH, /* record vs catalog */
    EMERALD_IMPORT_ERR_ROM_RANGE_OUT_OF_BOUNDS,   /* offset/size outside ROM */
    EMERALD_IMPORT_ERR_ENCODED_HASH_MISMATCH,     /* slice != manifest encoded digest */
    EMERALD_IMPORT_ERR_LZ77_DECODE_FAILED,
    EMERALD_IMPORT_ERR_DECODED_SIZE_MISMATCH,     /* decoded length != declared */
    EMERALD_IMPORT_ERR_CANONICAL_HASH_MISMATCH,   /* decoded != canonical digest */
    EMERALD_IMPORT_ERR_KEY_MISMATCH,              /* derived key != manifest key */

    /* pack construction */
    EMERALD_IMPORT_ERR_PACK_BUILD_FAILED,
    EMERALD_IMPORT_ERR_PACK_WRITE_FAILED,

    /* atomic install */
    EMERALD_IMPORT_ERR_LOCK_FAILED,
    EMERALD_IMPORT_ERR_DEST_NOT_DIRECTORY,
    EMERALD_IMPORT_ERR_DEST_CREATE_DIR_FAILED,
    EMERALD_IMPORT_ERR_TEMP_OPEN_FAILED,
    EMERALD_IMPORT_ERR_TEMP_WRITE_FAILED,
    EMERALD_IMPORT_ERR_TEMP_REOPEN_FAILED,
    EMERALD_IMPORT_ERR_RENAME_FAILED,
    EMERALD_IMPORT_ERR_FINAL_VERIFY_FAILED,

    /* installed-pack inspection */
    EMERALD_IMPORT_ERR_OPEN_INSTALLED_FAILED,
    EMERALD_IMPORT_ERR_INSTALLED_CORRUPT,
    EMERALD_IMPORT_ERR_INSTALLED_NEWER,
    EMERALD_IMPORT_ERR_INSTALLED_OLDER,
    EMERALD_IMPORT_ERR_INSTALLED_WRONG_PROFILE,

    /* R9 Stage 4: multi-manifest/catalog import (appended so every earlier
     * code keeps its stable numeric value). */
    EMERALD_IMPORT_ERR_MANIFESTS_DISAGREE,  /* manifest_version differs across manifests */
    EMERALD_IMPORT_ERR_CATALOGS_DISAGREE,   /* catalog_version/resource_api differ across catalogs */
};

/* Transient, caller-owned diagnostics. `message` never persists anything and
 * deliberately avoids echoing the user's ROM path (R3 §17). */
struct EmeraldImportReport
{
    enum EmeraldResourceImportError error;
    char message[512];
    size_t entryIndex;                        /* SIZE_MAX when not entry-scoped */
    bool hasName;
    char canonicalName[GEN3_RESOURCE_NAME_MAX + 1u];
};

/* One manifest/catalog input slot for the R9 multi-family extension: exactly
 * one of {bytes+size, path} must be supplied, mirroring the legacy single-slot
 * fields below. */
struct EmeraldImportSource
{
    const uint8_t *bytes;
    size_t size;
    const char *path;
};

/* One import request. Pointers are borrowed for the duration of the call.
 * Exactly one of {bytes+size} or {path} must be supplied for the ROM,
 * the manifest, and the catalog. `profile` and (for Install) `destinationPath`
 * are required. */
struct EmeraldImportInput
{
    const struct EmeraldRomProfile *profile;  /* REQUIRED */

    /* ROM image: either bytes+size or a path (read on demand). */
    const uint8_t *romBytes;
    size_t romSize;
    const char *romPath;

    /* R1A extraction manifest (TOML): bytes or path. */
    const uint8_t *manifestBytes;
    size_t manifestSize;
    const char *manifestPath;

    /* R2 catalog contract (TOML): bytes or path. */
    const uint8_t *catalogBytes;
    size_t catalogSize;
    const char *catalogPath;

    /* Final pack path (e.g. games/emerald/base/emerald-bpee01-v1.rpack).
     * Required by Install only. Never persisted. */
    const char *destinationPath;

    /* R9 Stage 4 multi-family extension. When manifestCount == 0 the single
     * manifest fields above are used (legacy callers, unchanged behavior);
     * otherwise `manifests` points at manifestCount slots, each supplying
     * exactly one of {bytes+size, path}. Same rule for catalogs/catalogCount.
     * Every manifest is validated independently (qualification, profile, ROM
     * digests) and its records are merged with cross-manifest duplicate
     * rejection; the pack profile's extractionManifestSha256/catalogSha256
     * cover all input files in input order. */
    const struct EmeraldImportSource *manifests;
    size_t manifestCount;
    const struct EmeraldImportSource *catalogs;
    size_t catalogCount;
};

/* Phase 1: strict ROM validation against input->profile. */
enum EmeraldResourceImportError
EmeraldImport_ValidateRom(const struct EmeraldImportInput *input,
                          struct EmeraldImportReport *report);

/* Phase 2: validate manifest + catalog contract, strictly extract and decode
 * every record, build the deterministic pack. On success *outBytes owns a
 * heap buffer released with Gen3ResourcePackBytes_Destroy. */
enum EmeraldResourceImportError
EmeraldImport_BuildPack(const struct EmeraldImportInput *input,
                        struct Gen3ResourcePackBytes *outBytes,
                        struct EmeraldImportReport *report);

/* Phase 3: full import. Revalidates everything, builds the pack in memory,
 * writes it to a unique temp file in the destination directory under an
 * import lock, reopens and validates the temp, atomically renames it over the
 * destination, then reopens and verifies the installed file. Existing packs
 * that are newer-format or wrong-profile are refused; valid/older/corrupt
 * packs are replaced atomically (the old pack survives any failure). */
enum EmeraldResourceImportError
EmeraldImport_Install(const struct EmeraldImportInput *input,
                      struct EmeraldImportReport *report);

/* ------------------------------------------------------------------ */
/* Installed-pack inspection (R3 §15/§26)                              */
/* ------------------------------------------------------------------ */

enum EmeraldResourceInstallStatus
{
    EMERALD_INSTALLED_VALID = 0,
    EMERALD_INSTALLED_CORRUPT,
    EMERALD_INSTALLED_OLDER,       /* physical format older than this engine */
    EMERALD_INSTALLED_NEWER,       /* written by a newer engine */
    EMERALD_INSTALLED_WRONG_PROFILE, /* parses but not for `profile`'s ROM */
    EMERALD_INSTALLED_OPEN_FAILED, /* absent / not a readable pack file */
};

struct EmeraldInstalledInfo
{
    enum EmeraldResourceInstallStatus status;
    uint32_t basePackVersion;
    uint32_t entryCount;
    char gameId[GEN3_PACK_GAME_ID_SIZE];
    uint8_t romSha1[GEN3_PACK_SHA1_SIZE];
    uint8_t romSha256[GEN3_PACK_SHA256_SIZE];
    uint8_t logicalDigest[GEN3_PACK_SHA256_SIZE];
    bool hasLogicalDigest;
};

/* Phase 4: classify an installed pack. Returns EMERALD_IMPORT_OK once the
 * classification itself succeeded (the result is in info->status). A NULL
 * `profile` skips the wrong-profile check. */
enum EmeraldResourceImportError
EmeraldImport_ValidateInstalled(const char *packPath,
                                const struct EmeraldRomProfile *profile,
                                struct EmeraldInstalledInfo *info,
                                struct EmeraldImportReport *report);

#endif
