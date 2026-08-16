#ifndef EMERALD_ROM_PROFILE_H
#define EMERALD_ROM_PROFILE_H

/* Emerald (BPEE) ROM identity profiles for the Stage R3 local import
 * pipeline.
 *
 * A profile is the canonical description of one supported source ROM image:
 * physical size, GBA header identity, and whole-ROM digests. The importer
 * validates a ROM against exactly one profile and never guesses offsets.
 * The ROM path is never part of a profile (it is transient import input only).
 *
 * Two profiles exist:
 *   - kEmeraldProfileBpee01Rev0   the retail Emerald (USA/Europe) revision 0
 *                                 image (size 16 MiB, BPEE / "01" / rev 0,
 *                                 SHA-1 f3ae08.., SHA-256 a9dec8..).
 *   - kEmeraldProfileSynthetic    a deterministic 16 MiB fixture image with
 *                                 the same BPEE header (SHA-1 092ee2..,
 *                                 SHA-256 e4c8f3..) used only by the test
 *                                 suite. `synthetic` is true.
 *
 * Both profiles describe the same ROM layout class ("bpee01-rev0"); they
 * differ in content digests. The importer selects a profile explicitly and
 * cross-checks the extraction manifest's qualification field against it, so a
 * fixture-qualified manifest is never accepted for a production profile and
 * vice versa (R3 §19).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EMERALD_ROM_SIZE (16u * 1024u * 1024u) /* 16,777,216 bytes */

#define EMERALD_PROFILE_GAME_CODE      "BPEE"
#define EMERALD_PROFILE_MAKER_CODE     "01"
#define EMERALD_PROFILE_SOFTWARE_REV   0u
#define EMERALD_PROFILE_NAME           "bpee01-rev0"
#define EMERALD_PROFILE_GAME           "emerald"

/* Retail Emerald (USA/Europe) rev 0 whole-ROM digests. */
extern const uint8_t kEmeraldProfileBpee01Rev0Sha1[20];
extern const uint8_t kEmeraldProfileBpee01Rev0Sha256[32];

/* Synthetic test fixture whole-ROM digests (deterministic fixture_build). */
extern const uint8_t kEmeraldProfileSyntheticSha1[20];
extern const uint8_t kEmeraldProfileSyntheticSha256[32];

struct EmeraldRomProfile
{
    const char *name;         /* "bpee01-rev0" */
    const char *game;         /* "emerald" */
    const char *gameCode;     /* "BPEE" */
    const char *makerCode;    /* "01" */
    uint8_t softwareRevision; /* 0 */
    uint64_t romSize;         /* 16,777,216 */
    const uint8_t *romSha1;   /* 20 bytes */
    const uint8_t *romSha256; /* 32 bytes */
    bool synthetic;           /* true only for the test fixture profile */
};

/* Returns the canonical (retail) bpee01-rev0 profile. */
const struct EmeraldRomProfile *EmeraldRomProfile_Bpee01Rev0(void);

/* Returns the synthetic test-fixture profile. This is TEST-ONLY: it exists so
 * the end-to-end synthetic suite exercises the full import path without a
 * retail ROM. Never used by production import. */
const struct EmeraldRomProfile *EmeraldRomProfile_SyntheticFixture(void);

/* Looks up a profile by name. Returns the production bpee01-rev0 profile for
 * "bpee01-rev0" (the canonical name). The synthetic profile is intentionally
 * NOT returned by name - callers that need it must ask for it explicitly so a
 * name lookup can never silently select the fixture. Returns NULL on unknown
 * name. */
const struct EmeraldRomProfile *EmeraldRomProfile_Find(const char *name);

#endif
